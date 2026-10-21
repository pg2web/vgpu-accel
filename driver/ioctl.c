/* SPDX-License-Identifier: MIT */
/*
 * IOCTL handling. Every request is handled and completed in the caller's context
 * (EvtIoInCallerContext): user pointers can be locked directly, mappings land in the right
 * process and waits can be interrupted when the thread exits.
 */
#include "driver.h"

/* ---- capsets -------------------------------------------------------------------------- */

NTSTATUS VgpuQueryCapsets(PVGPU_DEVICE Dev)
{
    UINT32 count = 0, i;

    VirtIOWdfDeviceGet(&Dev->Virtio, FIELD_OFFSET(VIRTIO_GPU_CONFIG, NumCapsets), &count, sizeof(count));
    count = min(count, VGPU_MAX_CAPSETS);

    ExAcquireFastMutex(&Dev->CapsetLock);
    Dev->NumCapsets = 0;
    Dev->CapsetMask = 0;
    for (i = 0; i < count; i++) {
        PVGPU_REQUEST req = VgpuAllocRequest(sizeof(VIRTIO_GPU_GET_CAPSET_INFO), sizeof(VIRTIO_GPU_RESP_CAPSET_INFO),
                                             VIRTIO_GPU_CMD_GET_CAPSET_INFO, 0);
        NTSTATUS status;

        if (!req)
            break;
        ((VIRTIO_GPU_GET_CAPSET_INFO *)req->Cmd)->CapsetIndex = i;
        status = VgpuSubmit(Dev, req, FALSE);
        if (NT_SUCCESS(status))
            status = VgpuWaitRequest(req, VIRTIO_GPU_RESP_OK_CAPSET_INFO);
        if (NT_SUCCESS(status)) {
            VIRTIO_GPU_RESP_CAPSET_INFO *info = (VIRTIO_GPU_RESP_CAPSET_INFO *)req->Resp;
            VGPU_CAPSET *cs = &Dev->Capsets[Dev->NumCapsets];

            if (cs->Cached && (cs->Id != info->CapsetId || cs->MaxSize != info->CapsetMaxSize)) {
                ExFreePoolWithTag(cs->Cached, VGPU_TAG);
                cs->Cached = NULL;
            }
            cs->Id = info->CapsetId;
            cs->MaxVersion = info->CapsetMaxVersion;
            cs->MaxSize = info->CapsetMaxSize;
            if (cs->Id < 64)
                Dev->CapsetMask |= 1ull << cs->Id;
            Dev->NumCapsets++;
            VGPU_LOG("capset %u: id %u version %u size %u", i, cs->Id, cs->MaxVersion, cs->MaxSize);
        }
        VgpuReleaseRequest(req);
    }
    ExReleaseFastMutex(&Dev->CapsetLock);
    return STATUS_SUCCESS;
}

void VgpuFreeCapsets(PVGPU_DEVICE Dev)
{
    UINT32 i;

    for (i = 0; i < VGPU_MAX_CAPSETS; i++) {
        if (Dev->Capsets[i].Cached)
            ExFreePoolWithTag(Dev->Capsets[i].Cached, VGPU_TAG);
        Dev->Capsets[i].Cached = NULL;
    }
    Dev->NumCapsets = 0;
}

static NTSTATUS GetCaps(PVGPU_DEVICE Dev, const struct vgpu_get_caps *In, PVOID Out, SIZE_T OutLen, SIZE_T *Written)
{
    VGPU_CAPSET *cs = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    UINT32 i;

    ExAcquireFastMutex(&Dev->CapsetLock);
    for (i = 0; i < Dev->NumCapsets; i++)
        if (Dev->Capsets[i].Id == In->capset_id)
            cs = &Dev->Capsets[i];
    if (!cs || In->capset_version > cs->MaxVersion || cs->MaxSize == 0) {
        ExReleaseFastMutex(&Dev->CapsetLock);
        return STATUS_INVALID_PARAMETER;
    }

    if (!cs->Cached || cs->CachedVersion != In->capset_version) {
        PVGPU_REQUEST req = VgpuAllocRequest(sizeof(VIRTIO_GPU_GET_CAPSET), sizeof(VIRTIO_GPU_HDR) + cs->MaxSize,
                                             VIRTIO_GPU_CMD_GET_CAPSET, 0);
        PVOID data = ExAllocatePoolWithTag(NonPagedPoolNx, cs->MaxSize, VGPU_TAG);

        status = STATUS_INSUFFICIENT_RESOURCES;
        if (req && data) {
            ((VIRTIO_GPU_GET_CAPSET *)req->Cmd)->CapsetId = cs->Id;
            ((VIRTIO_GPU_GET_CAPSET *)req->Cmd)->CapsetVersion = In->capset_version;
            status = VgpuSubmit(Dev, req, FALSE);
            if (NT_SUCCESS(status))
                status = VgpuWaitRequest(req, VIRTIO_GPU_RESP_OK_CAPSET);
        }
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(data, req->Resp + 1, cs->MaxSize);
            if (cs->Cached)
                ExFreePoolWithTag(cs->Cached, VGPU_TAG);
            cs->Cached = data;
            cs->CachedVersion = In->capset_version;
            data = NULL;
        }
        if (data)
            ExFreePoolWithTag(data, VGPU_TAG);
        if (req)
            VgpuReleaseRequest(req);
    }
    if (NT_SUCCESS(status)) {
        *Written = min(OutLen, cs->MaxSize);
        RtlCopyMemory(Out, cs->Cached, *Written);
    }
    ExReleaseFastMutex(&Dev->CapsetLock);
    return status;
}

/* ---- contexts ------------------------------------------------------------------------- */

/* create the handle's virgl context: explicitly (CONTEXT_INIT) or on first use */
NTSTATUS VgpuEnsureContext(PVGPU_FILE File, UINT32 CapsetId, BOOLEAN Explicit)
{
    PVGPU_DEVICE dev = File->Dev;
    VIRTIO_GPU_CTX_CREATE *c;
    PVGPU_REQUEST req;
    NTSTATUS status;
    UINT32 ctx;
    ULONG gen;

    KeAcquireGuardedMutex(&File->Lock);
    if (File->State != ContextNone) {
        status = File->State == ContextDestroyed ? STATUS_INVALID_DEVICE_STATE :
                 Explicit ? STATUS_INVALID_DEVICE_REQUEST : STATUS_SUCCESS;
        KeReleaseGuardedMutex(&File->Lock);
        return status;
    }

    gen = dev->Generation;
    ctx = (UINT32)InterlockedIncrement(&dev->NextContextId);
    req = VgpuAllocRequest(sizeof(*c), sizeof(VIRTIO_GPU_HDR), VIRTIO_GPU_CMD_CTX_CREATE, ctx);
    if (!req) {
        KeReleaseGuardedMutex(&File->Lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    c = req->Cmd;
    c->ContextInit = dev->HasContextInit ? (CapsetId & 0xff) : 0;
    {
        static const char prefix[] = "vgpu-accel pid ";
        char digits[24];
        ULONG_PTR pid = (ULONG_PTR)PsGetCurrentProcessId();
        UINT32 n = 0, len = sizeof(prefix) - 1;

        do {
            digits[n++] = (char)('0' + pid % 10);
            pid /= 10;
        } while (pid);
        RtlCopyMemory(c->DebugName, prefix, len);
        while (n)
            c->DebugName[len++] = digits[--n];
        c->NameLength = len;
    }

    status = VgpuExecute(dev, req, VIRTIO_GPU_RESP_OK_NODATA);
    if (NT_SUCCESS(status)) {
        File->State = ContextActive;
        File->CtxId = ctx;
        File->Generation = gen;
        VGPU_TRACE("ctx %u capset %u", ctx, CapsetId);
    }
    KeReleaseGuardedMutex(&File->Lock);
    return status;
}

void VgpuDestroyContext(PVGPU_FILE File)
{
    PVGPU_DEVICE dev = File->Dev;
    BOOLEAN destroyHost;
    UINT32 ctx;

    KeAcquireGuardedMutex(&File->Lock);
    destroyHost = File->State == ContextActive && File->Generation == dev->Generation;
    ctx = File->CtxId;
    File->State = ContextDestroyed;     /* no new resources from here on */
    KeReleaseGuardedMutex(&File->Lock);

    VgpuDestroyAllResources(File);
    if (destroyHost && dev->Ready) {
        PVGPU_REQUEST req = VgpuAllocRequest(sizeof(VIRTIO_GPU_HDR), sizeof(VIRTIO_GPU_HDR),
                                             VIRTIO_GPU_CMD_CTX_DESTROY, ctx);
        if (req)
            VgpuExecute(dev, req, VIRTIO_GPU_RESP_OK_NODATA);
        VGPU_TRACE("ctx %u", ctx);
    }
}

/* the handle's context id, creating a default context if user mode never asked for one */
static NTSTATUS ActiveContext(PVGPU_FILE File, UINT32 *Ctx)
{
    NTSTATUS status = VgpuEnsureContext(File, 0, FALSE);

    if (!NT_SUCCESS(status))
        return status;
    KeAcquireGuardedMutex(&File->Lock);
    if (File->State != ContextActive || File->Generation != File->Dev->Generation)
        status = STATUS_DEVICE_NOT_READY;
    *Ctx = File->CtxId;
    KeReleaseGuardedMutex(&File->Lock);
    return status;
}

/* ---- handlers ------------------------------------------------------------------------- */

static NTSTATUS GetParam(PVGPU_DEVICE Dev, UINT64 Param, UINT64 *Value)
{
    switch (Param) {
    case VGPU_PARAM_3D_FEATURES:        *Value = Dev->HasVirgl; break;
    case VGPU_PARAM_CAPSET_QUERY_FIX:   *Value = 1; break;
    case VGPU_PARAM_RESOURCE_BLOB:      /* only host blobs, so both need the host visible region */
    case VGPU_PARAM_HOST_VISIBLE:       *Value = Dev->HasBlob && Dev->HostVisibleSize != 0; break;
    case VGPU_PARAM_CROSS_DEVICE:       *Value = 0; break;
    case VGPU_PARAM_CONTEXT_INIT:       *Value = Dev->HasContextInit; break;
    case VGPU_PARAM_SUPPORTED_CAPSET_IDS: *Value = Dev->CapsetMask; break;
    default:                            return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ContextInit(WDFREQUEST Request, PVGPU_FILE File, const struct vgpu_context_init *In)
{
    struct vgpu_context_set_param params[VGPU_MAX_CTX_PARAMS];
    UINT32 capset = 0, i;
    WDFMEMORY mem;
    NTSTATUS status;

    if (In->num_params > VGPU_MAX_CTX_PARAMS)
        return STATUS_INVALID_PARAMETER;
    if (In->num_params) {
        status = WdfRequestProbeAndLockUserBufferForRead(Request, (PVOID)(ULONG_PTR)In->params,
                                                         In->num_params * sizeof(params[0]), &mem);
        if (!NT_SUCCESS(status))
            return status;
        status = WdfMemoryCopyToBuffer(mem, 0, params, In->num_params * sizeof(params[0]));
        if (!NT_SUCCESS(status))
            return status;
    }
    for (i = 0; i < In->num_params; i++) {
        switch (params[i].param) {
        case VGPU_CONTEXT_PARAM_CAPSET_ID:
            if (params[i].value >= 64 || !(File->Dev->CapsetMask & (1ull << params[i].value)))
                return STATUS_INVALID_PARAMETER;
            capset = (UINT32)params[i].value;
            break;
        case VGPU_CONTEXT_PARAM_NUM_RINGS:
        case VGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
            break;                          /* one ring (ring 0) is all virgl needs */
        default:
            return STATUS_INVALID_PARAMETER;
        }
    }
    return VgpuEnsureContext(File, capset, TRUE);
}

static NTSTATUS Transfer(PVGPU_FILE File, BOOLEAN ToHost, const struct vgpu_transfer *In)
{
    PVGPU_DEVICE dev = File->Dev;
    VIRTIO_GPU_TRANSFER_3D *t;
    PVGPU_RESOURCE res;
    PVGPU_REQUEST req;
    NTSTATUS status;
    UINT64 fence;
    UINT32 ctx;

    status = ActiveContext(File, &ctx);
    if (!NT_SUCCESS(status))
        return status;
    KeAcquireGuardedMutex(&File->Lock);
    res = VgpuLookupResource(File, In->bo_handle);
    KeReleaseGuardedMutex(&File->Lock);
    if (!res)
        return STATUS_INVALID_PARAMETER;

    req = VgpuAllocRequest(sizeof(*t), sizeof(VIRTIO_GPU_HDR),
                           ToHost ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, ctx);
    if (!req)
        return STATUS_INSUFFICIENT_RESOURCES;
    t = req->Cmd;
    t->Box.X = In->box.x;
    t->Box.Y = In->box.y;
    t->Box.Z = In->box.z;
    t->Box.W = In->box.w;
    t->Box.H = In->box.h;
    t->Box.D = In->box.d;
    t->Offset = In->offset;
    t->ResourceId = In->bo_handle;
    t->Level = In->level;
    t->Stride = In->stride;
    t->LayerStride = In->layer_stride;

    status = VgpuSubmit(dev, req, TRUE);
    fence = req->Fence;
    VgpuReleaseRequest(req);
    if (!NT_SUCCESS(status))
        return status;

    KeAcquireGuardedMutex(&File->Lock);
    res = VgpuLookupResource(File, In->bo_handle);
    if (res && fence > res->BusyFence)
        res->BusyFence = fence;
    KeReleaseGuardedMutex(&File->Lock);
    return STATUS_SUCCESS;
}

static NTSTATUS Wait(PVGPU_FILE File, const struct vgpu_wait *In, UINT32 *Busy)
{
    PVGPU_RESOURCE res;
    UINT64 fence = 0;
    NTSTATUS status;

    KeAcquireGuardedMutex(&File->Lock);
    res = VgpuLookupResource(File, In->handle);
    if (res)
        fence = res->BusyFence;
    KeReleaseGuardedMutex(&File->Lock);
    if (!res)
        return STATUS_INVALID_PARAMETER;

    if (In->flags & VGPU_WAIT_NOWAIT) {
        *Busy = !VgpuFenceDone(File->Dev, fence);
        return STATUS_SUCCESS;
    }
    status = VgpuWaitFence(File->Dev, fence);
    *Busy = 0;
    return status;
}

static NTSTATUS ExecBuffer(WDFREQUEST Request, PVGPU_FILE File, const struct vgpu_execbuffer *In)
{
    PVGPU_DEVICE dev = File->Dev;
    PKEVENT outEvent = NULL;
    UINT32 *handles = NULL;
    PVGPU_REQUEST req = NULL;
    WDFMEMORY mem;
    NTSTATUS status;
    UINT64 fence;
    UINT32 ctx, i;

    if (In->flags & ~(VGPU_EXECBUF_FENCE_IN | VGPU_EXECBUF_FENCE_OUT) ||
        In->size > VGPU_MAX_SUBMIT_SIZE || In->num_bo_handles > VGPU_MAX_BO_HANDLES)
        return STATUS_INVALID_PARAMETER;
    if (In->size == 0)
        return STATUS_SUCCESS;
    status = ActiveContext(File, &ctx);
    if (!NT_SUCCESS(status))
        return status;

    req = VgpuAllocRequest(sizeof(VIRTIO_GPU_SUBMIT_3D), sizeof(VIRTIO_GPU_HDR), VIRTIO_GPU_CMD_SUBMIT_3D, ctx);
    if (!req)
        return STATUS_INSUFFICIENT_RESOURCES;
    ((VIRTIO_GPU_SUBMIT_3D *)req->Cmd)->Size = In->size;

    /* copy the command stream: user mode may reuse its buffer as soon as we return */
    req->Data = ExAllocatePoolWithTag(NonPagedPoolNx, In->size, VGPU_TAG);
    req->DataSize = In->size;
    status = req->Data ? WdfRequestProbeAndLockUserBufferForRead(Request, (PVOID)(ULONG_PTR)In->command,
                                                                 In->size, &mem)
                       : STATUS_INSUFFICIENT_RESOURCES;
    if (NT_SUCCESS(status))
        status = WdfMemoryCopyToBuffer(mem, 0, req->Data, In->size);
    if (!NT_SUCCESS(status))
        goto out;

    if (In->num_bo_handles) {
        handles = ExAllocatePoolWithTag(NonPagedPoolNx, In->num_bo_handles * sizeof(UINT32), VGPU_TAG);
        status = handles ? WdfRequestProbeAndLockUserBufferForRead(Request, (PVOID)(ULONG_PTR)In->bo_handles,
                                                                   In->num_bo_handles * sizeof(UINT32), &mem)
                         : STATUS_INSUFFICIENT_RESOURCES;
        if (NT_SUCCESS(status))
            status = WdfMemoryCopyToBuffer(mem, 0, handles, In->num_bo_handles * sizeof(UINT32));
        if (!NT_SUCCESS(status))
            goto out;
        KeAcquireGuardedMutex(&File->Lock);
        for (i = 0; i < In->num_bo_handles; i++)
            if (!VgpuLookupResource(File, handles[i]))
                status = STATUS_INVALID_PARAMETER;
        KeReleaseGuardedMutex(&File->Lock);
        if (!NT_SUCCESS(status))
            goto out;
    }

    if (In->flags & VGPU_EXECBUF_FENCE_OUT) {
        status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)In->out_fence, EVENT_MODIFY_STATE,
                                           *ExEventObjectType, UserMode, (PVOID *)&outEvent, NULL);
        if (!NT_SUCCESS(status))
            goto out;
    }
    if (In->flags & VGPU_EXECBUF_FENCE_IN) {
        PKEVENT inEvent;

        status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)In->in_fence, SYNCHRONIZE,
                                           *ExEventObjectType, UserMode, (PVOID *)&inEvent, NULL);
        if (!NT_SUCCESS(status))
            goto out;
        status = KeWaitForSingleObject(inEvent, UserRequest, UserMode, TRUE, NULL);
        ObDereferenceObject(inEvent);
        if (status != STATUS_SUCCESS) {
            status = STATUS_CANCELLED;
            goto out;
        }
    }

    status = VgpuSubmit(dev, req, TRUE);
    if (!NT_SUCCESS(status))
        goto out;
    fence = req->Fence;

    if (handles) {
        KeAcquireGuardedMutex(&File->Lock);
        for (i = 0; i < In->num_bo_handles; i++) {
            PVGPU_RESOURCE res = VgpuLookupResource(File, handles[i]);
            if (res && fence > res->BusyFence)
                res->BusyFence = fence;
        }
        KeReleaseGuardedMutex(&File->Lock);
    }
    if (outEvent) {
        VgpuSignalOnFence(dev, fence, outEvent);    /* takes over the reference */
        outEvent = NULL;
    }

out:
    if (outEvent)
        ObDereferenceObject(outEvent);
    if (handles)
        ExFreePoolWithTag(handles, VGPU_TAG);
    VgpuReleaseRequest(req);
    return status;
}

static NTSTATUS CreateBlob(WDFREQUEST Request, PVGPU_FILE File, const struct vgpu_resource_create_blob *In, UINT32 *Id)
{
    PVOID cmd = NULL;
    WDFMEMORY mem;
    NTSTATUS status;
    UINT32 ctx;

    if (In->cmd_size > VGPU_MAX_BLOB_CMD)
        return STATUS_INVALID_PARAMETER;
    status = ActiveContext(File, &ctx);
    if (!NT_SUCCESS(status))
        return status;
    if (In->cmd_size) {
        cmd = ExAllocatePoolWithTag(NonPagedPoolNx, In->cmd_size, VGPU_TAG);
        if (!cmd)
            return STATUS_INSUFFICIENT_RESOURCES;
        status = WdfRequestProbeAndLockUserBufferForRead(Request, (PVOID)(ULONG_PTR)In->cmd, In->cmd_size, &mem);
        if (NT_SUCCESS(status))
            status = WdfMemoryCopyToBuffer(mem, 0, cmd, In->cmd_size);
        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(cmd, VGPU_TAG);
            return status;
        }
    }
    return VgpuCreateBlobResource(File, In, cmd, Id);   /* takes over cmd */
}

/* ---- dispatch ------------------------------------------------------------------------- */

#define IN_ARGS(type) \
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(type), &in, &inLen); \
    if (NT_SUCCESS(status) && inLen != sizeof(type)) status = STATUS_INVALID_PARAMETER; \
    if (!NT_SUCCESS(status)) break
#define OUT_ARGS(size) \
    status = WdfRequestRetrieveOutputBuffer(Request, (size), &out, &outLen); \
    if (!NT_SUCCESS(status)) break

VOID VgpuEvtIoInCallerContext(WDFDEVICE Device, WDFREQUEST Request)
{
    PVGPU_DEVICE dev = VgpuGetDevice(Device);
    WDF_REQUEST_PARAMETERS p;
    PVGPU_FILE file;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    SIZE_T inLen = 0, outLen = 0, info = 0;
    PVOID in = NULL, out = NULL;

    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    if (p.Type != WdfRequestTypeDeviceControl || !WdfRequestGetFileObject(Request)) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    file = VgpuGetFile(WdfRequestGetFileObject(Request));

    switch (p.Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_VGPU_GET_VERSION: {
        struct vgpu_version *v;
        OUT_ARGS(sizeof(*v));
        v = out;
        v->abi_version = VGPU_ABI_VERSION;
        v->driver_version = VGPU_DRIVER_VERSION;
        info = sizeof(*v);
        break;
    }
    case IOCTL_VGPU_GETPARAM:
        IN_ARGS(UINT64);
        OUT_ARGS(sizeof(UINT64));
        status = GetParam(dev, *(UINT64 *)in, (UINT64 *)out);
        info = NT_SUCCESS(status) ? sizeof(UINT64) : 0;
        break;
    case IOCTL_VGPU_GET_CAPS:
        IN_ARGS(struct vgpu_get_caps);
        OUT_ARGS(1);
        status = GetCaps(dev, in, out, outLen, &info);
        break;
    case IOCTL_VGPU_CONTEXT_INIT:
        IN_ARGS(struct vgpu_context_init);
        status = ContextInit(Request, file, in);
        break;
    case IOCTL_VGPU_DESTROY_CONTEXT:
        VgpuDestroyContext(file);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_VGPU_RESOURCE_CREATE: {
        struct vgpu_resource_create_resp *r;
        UINT32 id = 0, ctx;
        IN_ARGS(struct vgpu_resource_create);
        OUT_ARGS(sizeof(*r));
        status = ActiveContext(file, &ctx);
        if (NT_SUCCESS(status))
            status = VgpuCreateResource(file, in, &id);
        if (NT_SUCCESS(status)) {
            r = out;
            r->bo_handle = r->res_handle = id;
            info = sizeof(*r);
        }
        break;
    }
    case IOCTL_VGPU_RESOURCE_CREATE_BLOB: {
        struct vgpu_resource_create_resp *r;
        UINT32 id = 0;
        IN_ARGS(struct vgpu_resource_create_blob);
        OUT_ARGS(sizeof(*r));
        status = CreateBlob(Request, file, in, &id);
        if (NT_SUCCESS(status)) {
            r = out;
            r->bo_handle = r->res_handle = id;
            info = sizeof(*r);
        }
        break;
    }
    case IOCTL_VGPU_RESOURCE_CLOSE:
        IN_ARGS(struct vgpu_handle);
        status = VgpuCloseResource(file, ((struct vgpu_handle *)in)->handle);
        break;
    case IOCTL_VGPU_MAP:
        IN_ARGS(struct vgpu_map);
        OUT_ARGS(sizeof(UINT64));
        status = VgpuMapResource(file, ((struct vgpu_map *)in)->handle, (UINT64 *)out);
        info = NT_SUCCESS(status) ? sizeof(UINT64) : 0;
        break;
    case IOCTL_VGPU_TRANSFER_TO_HOST:
    case IOCTL_VGPU_TRANSFER_FROM_HOST:
        IN_ARGS(struct vgpu_transfer);
        status = Transfer(file, p.Parameters.DeviceIoControl.IoControlCode == IOCTL_VGPU_TRANSFER_TO_HOST, in);
        break;
    case IOCTL_VGPU_WAIT:
        IN_ARGS(struct vgpu_wait);
        OUT_ARGS(sizeof(UINT32));
        status = Wait(file, in, (UINT32 *)out);
        info = NT_SUCCESS(status) ? sizeof(UINT32) : 0;
        break;
    case IOCTL_VGPU_EXECBUFFER:
        IN_ARGS(struct vgpu_execbuffer);
        status = ExecBuffer(Request, file, in);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    if (!NT_SUCCESS(status)) {
        static volatile LONG reported;
        if (InterlockedIncrement(&reported) <= 100)
            VGPU_LOG("ioctl 0x%x failed: 0x%08x (pid %Iu)", p.Parameters.DeviceIoControl.IoControlCode, status,
                     (ULONG_PTR)PsGetCurrentProcessId());
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}
