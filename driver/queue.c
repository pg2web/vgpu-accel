/* SPDX-License-Identifier: MIT */
/*
 * Control queue: request submission, completion and the fence timeline.
 *
 * Fences form one device-wide timeline. Every fenced command gets the next id under
 * QueueLock in the same step that puts it on the ring, so ids reach the host in order and
 * "completed >= n" means everything up to n is done.
 */
#include "driver.h"

PVGPU_REQUEST VgpuAllocRequest(ULONG CmdSize, ULONG RespSize, UINT32 Type, UINT32 CtxId)
{
    SIZE_T total = sizeof(VGPU_REQUEST) + CmdSize + RespSize;
    PVGPU_REQUEST req = ExAllocatePoolWithTag(NonPagedPoolNx, total, VGPU_TAG);
    VIRTIO_GPU_HDR *hdr;

    if (!req)
        return NULL;
    RtlZeroMemory(req, total);
    req->Refs = 1;
    KeInitializeEvent(&req->Done, NotificationEvent, FALSE);
    req->Cmd = req + 1;
    req->CmdSize = CmdSize;
    req->Resp = (VIRTIO_GPU_HDR *)((PUCHAR)req->Cmd + CmdSize);
    req->RespSize = RespSize;

    hdr = req->Cmd;
    hdr->Type = Type;
    hdr->CtxId = CtxId;
    return req;
}

void VgpuReleaseRequest(PVGPU_REQUEST Req)
{
    if (InterlockedDecrement(&Req->Refs) != 0)
        return;
    if (Req->Data && Req->DataContiguous)
        MmFreeContiguousMemory(Req->Data);
    else if (Req->Data)
        ExFreePoolWithTag(Req->Data, VGPU_TAG);
    if (Req->Indirect)
        ExFreePoolWithTag(Req->Indirect, VGPU_TAG);
    ExFreePoolWithTag(Req, VGPU_TAG);
}

/* append the physical pages behind [Va, Va+Length) to Sg, merging contiguous pieces */
static BOOLEAN AddBuffer(struct VirtIOBufferDescriptor *Sg, ULONG *Count, ULONG First, PVOID Va, ULONG Length)
{
    PUCHAR p = Va;

    while (Length) {
        ULONG chunk = min(Length, (ULONG)(PAGE_SIZE - BYTE_OFFSET(p)));
        PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(p);
        struct VirtIOBufferDescriptor *prev = *Count > First ? &Sg[*Count - 1] : NULL;

        if (prev && prev->physAddr.QuadPart + prev->length == pa.QuadPart) {
            prev->length += chunk;
        } else {
            if (*Count >= VGPU_MAX_SG)
                return FALSE;
            Sg[*Count].physAddr = pa;
            Sg[*Count].length = chunk;
            (*Count)++;
        }
        p += chunk;
        Length -= chunk;
    }
    return TRUE;
}

NTSTATUS VgpuSubmit(PVGPU_DEVICE Dev, PVGPU_REQUEST Req, BOOLEAN Fenced)
{
    struct VirtIOBufferDescriptor *sg;
    VIRTIO_GPU_HDR *hdr = Req->Cmd;
    ULONG out = 0, total;
    ULONGLONG indirectPa = 0;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    KIRQL irql;

    sg = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*sg) * VGPU_MAX_SG, VGPU_TAG);
    if (!sg)
        return STATUS_INSUFFICIENT_RESOURCES;

    total = 0;
    if (!AddBuffer(sg, &total, 0, Req->Cmd, Req->CmdSize) ||
        (Req->DataSize && !AddBuffer(sg, &total, 0, Req->Data, Req->DataSize)) ||
        !AddBuffer(sg, &total, (out = total), Req->Resp, Req->RespSize)) {
        VGPU_LOG("command 0x%x: more than %u physical fragments", hdr->Type, VGPU_MAX_SG);
        ExFreePoolWithTag(sg, VGPU_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (total > VGPU_DIRECT_SG && Dev->HasIndirect) {
        /* allocations of PAGE_SIZE are page aligned, so the table is physically contiguous */
        Req->Indirect = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, VGPU_TAG);
        if (!Req->Indirect) {
            ExFreePoolWithTag(sg, VGPU_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        indirectPa = MmGetPhysicalAddress(Req->Indirect).QuadPart;
    }

    InterlockedIncrement(&Req->Refs);       /* the device's reference */
    timeout.QuadPart = -10 * 1000 * 1000;   /* 1 s, then re-check */

    for (;;) {
        ULONG inFlight;
        int r;

        KeAcquireSpinLock(&Dev->QueueLock, &irql);
        if (!Dev->Ready) {
            KeReleaseSpinLock(&Dev->QueueLock, irql);
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        if (Fenced) {
            hdr->Flags |= VIRTIO_GPU_FLAG_FENCE;
            hdr->FenceId = Dev->NextFence + 1;
        }
        r = virtqueue_add_buf(Dev->ControlQueue, sg, out, total - out, Req, Req->Indirect, indirectPa);
        if (r >= 0) {
            if (Fenced)
                Req->Fence = ++Dev->NextFence;
            Dev->InFlight++;
            virtqueue_kick(Dev->ControlQueue);
            KeReleaseSpinLock(&Dev->QueueLock, irql);
            status = STATUS_SUCCESS;
            break;
        }
        inFlight = Dev->InFlight;
        KeClearEvent(&Dev->RingSpace);
        KeReleaseSpinLock(&Dev->QueueLock, irql);

        if (inFlight == 0) {
            /* the ring is empty and it still does not fit: it never will */
            VGPU_LOG("request with %lu descriptors does not fit the control queue", total);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        KeWaitForSingleObject(&Dev->RingSpace, Executive, KernelMode, FALSE, &timeout);
    }

    if (!NT_SUCCESS(status))
        VgpuReleaseRequest(Req);            /* drop the device's reference again */
    ExFreePoolWithTag(sg, VGPU_TAG);
    return status;
}

NTSTATUS VgpuWaitRequest(PVGPU_REQUEST Req, UINT32 ExpectedResp)
{
    LARGE_INTEGER timeout;
    NTSTATUS status;

    timeout.QuadPart = -10000LL * VGPU_SYNC_TIMEOUT_MS;
    status = KeWaitForSingleObject(&Req->Done, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        VGPU_LOG("command 0x%x: no response from the host in %u ms", ((VIRTIO_GPU_HDR *)Req->Cmd)->Type,
                 VGPU_SYNC_TIMEOUT_MS);
        return STATUS_IO_TIMEOUT;
    }
    if (Req->Resp->Type != ExpectedResp) {
        VGPU_LOG("command 0x%x failed: response 0x%x", ((VIRTIO_GPU_HDR *)Req->Cmd)->Type, Req->Resp->Type);
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VgpuExecute(PVGPU_DEVICE Dev, PVGPU_REQUEST Req, UINT32 ExpectedResp)
{
    NTSTATUS status = VgpuSubmit(Dev, Req, FALSE);

    if (NT_SUCCESS(status))
        status = VgpuWaitRequest(Req, ExpectedResp);
    VgpuReleaseRequest(Req);
    return status;
}

/* QueueLock held */
static void SignalWaiters(PVGPU_DEVICE Dev, BOOLEAN All)
{
    PLIST_ENTRY e = Dev->FenceWaiters.Flink;

    while (e != &Dev->FenceWaiters) {
        PVGPU_FENCE_WAITER w = CONTAINING_RECORD(e, VGPU_FENCE_WAITER, Link);
        e = e->Flink;
        if (All || w->Fence <= Dev->CompletedFence) {
            PKEVENT event = w->Event;
            BOOLEAN owned = w->Owned;

            RemoveEntryList(&w->Link);
            w->Link.Flink = NULL;           /* tells VgpuWaitFence it was signalled */
            KeSetEvent(event, IO_NO_INCREMENT, FALSE);
            if (owned) {
                ObDereferenceObject(event);
                ExFreePoolWithTag(w, VGPU_TAG);
            }
        }
    }
}

/* DPC level */
void VgpuProcessCompletions(PVGPU_DEVICE Dev)
{
    BOOLEAN any = FALSE, fenceMoved = FALSE;
    PVGPU_REQUEST req;
    unsigned int len;

    KeAcquireSpinLockAtDpcLevel(&Dev->QueueLock);
    if (Dev->ControlQueue) {
        do {
            virtqueue_disable_cb(Dev->ControlQueue);
            while ((req = virtqueue_get_buf(Dev->ControlQueue, &len)) != NULL) {
                Dev->InFlight--;
                if (req->Fence > Dev->CompletedFence) {
                    Dev->CompletedFence = req->Fence;
                    fenceMoved = TRUE;
                }
                KeSetEvent(&req->Done, IO_NO_INCREMENT, FALSE);
                VgpuReleaseRequest(req);
                any = TRUE;
            }
        } while (!virtqueue_enable_cb(Dev->ControlQueue));
    }
    if (fenceMoved)
        SignalWaiters(Dev, FALSE);
    KeReleaseSpinLockFromDpcLevel(&Dev->QueueLock);

    if (any)
        KeSetEvent(&Dev->RingSpace, IO_NO_INCREMENT, FALSE);
}

/* after the device was reset: nothing will complete any more, so fail what is left */
void VgpuFailOutstanding(PVGPU_DEVICE Dev)
{
    PVGPU_REQUEST req;
    unsigned int len;
    KIRQL irql;

    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    Dev->Ready = FALSE;
    if (Dev->ControlQueue) {
        while ((req = virtqueue_get_buf(Dev->ControlQueue, &len)) != NULL ||
               (req = virtqueue_detach_unused_buf(Dev->ControlQueue)) != NULL) {
            Dev->InFlight--;
            req->Resp->Type = 0;
            KeSetEvent(&req->Done, IO_NO_INCREMENT, FALSE);
            VgpuReleaseRequest(req);
        }
    }
    Dev->CompletedFence = Dev->NextFence;
    SignalWaiters(Dev, TRUE);
    KeReleaseSpinLock(&Dev->QueueLock, irql);
    KeSetEvent(&Dev->RingSpace, IO_NO_INCREMENT, FALSE);
}

BOOLEAN VgpuFenceDone(PVGPU_DEVICE Dev, UINT64 Fence)
{
    BOOLEAN done;
    KIRQL irql;

    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    done = Fence <= Dev->CompletedFence || !Dev->Ready;
    KeReleaseSpinLock(&Dev->QueueLock, irql);
    return done;
}

/* wait until the host completed Fence; the wait is interruptible (thread exit, APCs) */
NTSTATUS VgpuWaitFence(PVGPU_DEVICE Dev, UINT64 Fence)
{
    PVGPU_FENCE_WAITER w;
    NTSTATUS status;
    KIRQL irql;

    if (VgpuFenceDone(Dev, Fence))
        return STATUS_SUCCESS;

    w = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*w), VGPU_TAG);
    if (!w)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(w, sizeof(*w));
    KeInitializeEvent(&w->LocalEvent, NotificationEvent, FALSE);
    w->Event = &w->LocalEvent;
    w->Fence = Fence;

    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    if (Fence <= Dev->CompletedFence || !Dev->Ready) {
        KeReleaseSpinLock(&Dev->QueueLock, irql);
        ExFreePoolWithTag(w, VGPU_TAG);
        return STATUS_SUCCESS;
    }
    InsertTailList(&Dev->FenceWaiters, &w->Link);
    KeReleaseSpinLock(&Dev->QueueLock, irql);

    status = KeWaitForSingleObject(&w->LocalEvent, UserRequest, UserMode, TRUE, NULL);

    /* taking the lock also guarantees the DPC is done touching w */
    KeAcquireSpinLock(&Dev->QueueLock, &irql);
    if (w->Link.Flink) {
        RemoveEntryList(&w->Link);
        status = STATUS_CANCELLED;
    } else {
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&Dev->QueueLock, irql);
    ExFreePoolWithTag(w, VGPU_TAG);
    return status;
}

/* set Event (a referenced user event, reference consumed) once the host completed Fence */
void VgpuSignalOnFence(PVGPU_DEVICE Dev, UINT64 Fence, PKEVENT Event)
{
    PVGPU_FENCE_WAITER w;
    KIRQL irql;

    w = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*w), VGPU_TAG);
    if (w) {
        RtlZeroMemory(w, sizeof(*w));
        w->Event = Event;
        w->Owned = TRUE;
        w->Fence = Fence;
        KeAcquireSpinLock(&Dev->QueueLock, &irql);
        if (Fence > Dev->CompletedFence && Dev->Ready) {
            InsertTailList(&Dev->FenceWaiters, &w->Link);
            KeReleaseSpinLock(&Dev->QueueLock, irql);
            return;
        }
        KeReleaseSpinLock(&Dev->QueueLock, irql);
        ExFreePoolWithTag(w, VGPU_TAG);
    } else {
        /* no memory to track it: wait right here rather than lose the signal */
        VgpuWaitFence(Dev, Fence);
    }
    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
    ObDereferenceObject(Event);
}
