/* SPDX-License-Identifier: MIT */
/*
 * Resources: host 3D resources with guest backing, owned by one open handle.
 *
 * Every resource gets backing pages, even when user mode asks for less than a page:
 * virglrenderer requires guest backing for transfers of textures (COPY_TRANSFER3D reads
 * from host fail without it and poison the whole context).
 */
#include "driver.h"

static RTL_GENERIC_COMPARE_RESULTS NTAPI CompareEntries(PRTL_AVL_TABLE Table, PVOID A, PVOID B)
{
    UINT32 a = ((VGPU_RESOURCE_ENTRY *)A)->Id, b = ((VGPU_RESOURCE_ENTRY *)B)->Id;

    UNREFERENCED_PARAMETER(Table);
    return a < b ? GenericLessThan : a > b ? GenericGreaterThan : GenericEqual;
}

static PVOID NTAPI AllocEntry(PRTL_AVL_TABLE Table, CLONG Size)
{
    UNREFERENCED_PARAMETER(Table);
    return ExAllocatePoolWithTag(NonPagedPoolNx, Size, VGPU_TAG);
}

static VOID NTAPI FreeEntry(PRTL_AVL_TABLE Table, PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Table);
    ExFreePoolWithTag(Buffer, VGPU_TAG);
}

void VgpuInitResourceTable(PVGPU_FILE File)
{
    RtlInitializeGenericTableAvl(&File->Resources, CompareEntries, AllocEntry, FreeEntry, NULL);
}

/* File->Lock held */
PVGPU_RESOURCE VgpuLookupResource(PVGPU_FILE File, UINT32 Id)
{
    VGPU_RESOURCE_ENTRY key = { Id, NULL }, *e;

    e = RtlLookupElementGenericTableAvl(&File->Resources, &key);
    return e ? e->Resource : NULL;
}

static BOOLEAN HostAlive(PVGPU_FILE File)
{
    return File->Generation == File->Dev->Generation && File->Dev->Ready;
}

static void UnrefHost(PVGPU_DEVICE Dev, UINT32 Id)
{
    PVGPU_REQUEST req = VgpuAllocRequest(sizeof(VIRTIO_GPU_RESOURCE_CMD), sizeof(VIRTIO_GPU_HDR),
                                         VIRTIO_GPU_CMD_RESOURCE_UNREF, 0);
    if (req) {
        ((VIRTIO_GPU_RESOURCE_CMD *)req->Cmd)->ResourceId = Id;
        VgpuExecute(Dev, req, VIRTIO_GPU_RESP_OK_NODATA);
    }
}

/* Res is no longer in any table; PASSIVE_LEVEL, any process */
static void DestroyResource(PVGPU_FILE File, PVGPU_RESOURCE Res, BOOLEAN Alive)
{
    if (Res->UserAddress) {
        KAPC_STATE apc;
        BOOLEAN attach = Res->MapProcess != PsGetCurrentProcess();

        if (attach)
            KeStackAttachProcess(Res->MapProcess, &apc);
        MmUnmapLockedPages(Res->UserAddress, Res->Mdl);
        if (attach)
            KeUnstackDetachProcess(&apc);
        ObDereferenceObject(Res->MapProcess);
    }
    if (Res->Blob) {
        PVGPU_DEVICE dev = File->Dev;

        if (Res->HostMapped) {
            if (Alive) {
                PVGPU_REQUEST req = VgpuAllocRequest(sizeof(VIRTIO_GPU_RESOURCE_CMD), sizeof(VIRTIO_GPU_HDR),
                                                     VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB, 0);
                if (req) {
                    ((VIRTIO_GPU_RESOURCE_CMD *)req->Cmd)->ResourceId = Res->Id;
                    VgpuExecute(dev, req, VIRTIO_GPU_RESP_OK_NODATA);
                }
            }
            MmUnmapIoSpace(Res->IoAddress, Res->Size);
            IoFreeMdl(Res->Mdl);
            ExAcquireFastMutex(&dev->HostVisibleLock);
            RtlClearBits(&dev->HostVisibleMap, (ULONG)(Res->ShmOffset >> PAGE_SHIFT), (ULONG)(Res->Size >> PAGE_SHIFT));
            ExReleaseFastMutex(&dev->HostVisibleLock);
        }
        if (Alive)
            UnrefHost(dev, Res->Id);
        ExFreePoolWithTag(Res, VGPU_TAG);
        return;
    }

    /* the host processes the queue in order: once it answered the unref, it is done
       with the backing pages, including transfers queued before */
    if (Alive)
        UnrefHost(File->Dev, Res->Id);
    MmFreePagesFromMdl(Res->Mdl);
    ExFreePool(Res->Mdl);
    ExFreePoolWithTag(Res, VGPU_TAG);
}

/* make a new resource visible to user mode; destroys it if the context went away meanwhile */
static NTSTATUS Publish(PVGPU_FILE File, PVGPU_RESOURCE Res, ULONG Generation, UINT32 *Id)
{
    VGPU_RESOURCE_ENTRY entry;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN alive;

    entry.Id = Res->Id;
    entry.Resource = Res;
    KeAcquireGuardedMutex(&File->Lock);
    if (File->State != ContextActive || File->Generation != Generation)
        status = STATUS_INVALID_DEVICE_STATE;   /* destroyed or device reset meanwhile */
    else if (!RtlInsertElementGenericTableAvl(&File->Resources, &entry, sizeof(entry), NULL))
        status = STATUS_INSUFFICIENT_RESOURCES;
    alive = HostAlive(File);
    KeReleaseGuardedMutex(&File->Lock);

    if (!NT_SUCCESS(status)) {
        DestroyResource(File, Res, alive);
        return status;
    }
    *Id = Res->Id;
    return STATUS_SUCCESS;
}

/* build the ATTACH_BACKING entry list from the MDL's pages, one entry per contiguous run */
/*
 * The entry list goes to the device as one buffer. Physically contiguous memory keeps it a
 * single descriptor however fragmented the backing is (a pool buffer needs one descriptor
 * per page, and long lists of a fragmented resource would not fit one request).
 */
static VIRTIO_GPU_MEM_ENTRY *BuildEntries(PMDL Mdl, ULONG *Count, BOOLEAN *Contiguous)
{
    PPFN_NUMBER pfn = MmGetMdlPfnArray(Mdl);
    ULONG pages = (ULONG)(MmGetMdlByteCount(Mdl) >> PAGE_SHIFT), runs = 0, i;
    VIRTIO_GPU_MEM_ENTRY *e;

    for (i = 0; i < pages; i++)
        if (i == 0 || pfn[i] != pfn[i - 1] + 1)
            runs++;

    *Contiguous = FALSE;
    if (runs * sizeof(*e) > PAGE_SIZE) {
        PHYSICAL_ADDRESS lowest, highest, boundary;

        lowest.QuadPart = 0;
        highest.QuadPart = -1;
        boundary.QuadPart = 0;
        e = MmAllocateContiguousMemorySpecifyCache(runs * sizeof(*e), lowest, highest, boundary, MmCached);
        *Contiguous = e != NULL;
    } else {
        e = NULL;
    }
    if (!e)
        e = ExAllocatePoolWithTag(NonPagedPoolNx, runs * sizeof(*e), VGPU_TAG);
    if (!e)
        return NULL;
    RtlZeroMemory(e, runs * sizeof(*e));
    runs = 0;
    for (i = 0; i < pages; i++) {
        if (i == 0 || pfn[i] != pfn[i - 1] + 1) {
            e[runs].Address = (UINT64)pfn[i] << PAGE_SHIFT;
            runs++;
        }
        e[runs - 1].Length += PAGE_SIZE;
    }
    *Count = runs;
    return e;
}

NTSTATUS VgpuCreateResource(PVGPU_FILE File, const struct vgpu_resource_create *A, UINT32 *Id)
{
    PVGPU_DEVICE dev = File->Dev;
    PVGPU_REQUEST create = NULL, attach = NULL, ctxAttach = NULL;
    VIRTIO_GPU_RESOURCE_CREATE_3D *c;
    VIRTIO_GPU_ATTACH_BACKING *b;
    PHYSICAL_ADDRESS low, high, skip;
    PVGPU_RESOURCE res;
    ULONG entries = 0;
    UINT32 ctx;
    ULONG gen;
    SIZE_T size;
    NTSTATUS status, s2, s3;

    KeAcquireGuardedMutex(&File->Lock);
    ctx = File->CtxId;
    gen = File->Generation;
    status = File->State == ContextActive ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
    KeReleaseGuardedMutex(&File->Lock);
    if (!NT_SUCCESS(status))
        return status;

    if (A->size > VGPU_MAX_RESOURCE_SIZE)
        return STATUS_INVALID_PARAMETER;
    size = ROUND_TO_PAGES(max(A->size, 1));

    res = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*res), VGPU_TAG);
    if (!res)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(res, sizeof(*res));
    res->Size = size;
    do {
        res->Id = (UINT32)InterlockedIncrement(&dev->NextResourceId);
    } while (res->Id == 0);

    low.QuadPart = 0;
    high.QuadPart = -1;
    skip.QuadPart = 0;
    /* pages come back zeroed */
    res->Mdl = MmAllocatePagesForMdlEx(low, high, skip, size, MmCached,
                                       MM_ALLOCATE_FULLY_REQUIRED | MM_ALLOCATE_PREFER_CONTIGUOUS);
    if (!res->Mdl) {
        ExFreePoolWithTag(res, VGPU_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = STATUS_INSUFFICIENT_RESOURCES;
    create = VgpuAllocRequest(sizeof(*c), sizeof(VIRTIO_GPU_HDR), VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, ctx);
    attach = VgpuAllocRequest(sizeof(*b), sizeof(VIRTIO_GPU_HDR), VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, ctx);
    ctxAttach = VgpuAllocRequest(sizeof(VIRTIO_GPU_RESOURCE_CMD), sizeof(VIRTIO_GPU_HDR),
                                 VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx);
    if (!create || !attach || !ctxAttach)
        goto fail;
    attach->Data = BuildEntries(res->Mdl, &entries, &attach->DataContiguous);
    if (!attach->Data)
        goto fail;
    attach->DataSize = entries * sizeof(VIRTIO_GPU_MEM_ENTRY);

    c = create->Cmd;
    c->ResourceId = res->Id;
    c->Target = A->target;
    c->Format = A->format;
    c->Bind = A->bind;
    c->Width = A->width;
    c->Height = A->height;
    c->Depth = A->depth;
    c->ArraySize = A->array_size;
    c->LastLevel = A->last_level;
    c->NrSamples = A->nr_samples;
    c->Flags = A->flags;
    b = attach->Cmd;
    b->ResourceId = res->Id;
    b->NrEntries = entries;
    ((VIRTIO_GPU_RESOURCE_CMD *)ctxAttach->Cmd)->ResourceId = res->Id;

    /* queue all three, then collect the answers: one round trip instead of three */
    status = VgpuSubmit(dev, create, FALSE);
    if (!NT_SUCCESS(status))
        goto fail;
    s2 = VgpuSubmit(dev, attach, FALSE);
    s3 = NT_SUCCESS(s2) ? VgpuSubmit(dev, ctxAttach, FALSE) : s2;
    status = VgpuWaitRequest(create, VIRTIO_GPU_RESP_OK_NODATA);
    if (NT_SUCCESS(s2))
        s2 = VgpuWaitRequest(attach, VIRTIO_GPU_RESP_OK_NODATA);
    if (NT_SUCCESS(s3))
        s3 = VgpuWaitRequest(ctxAttach, VIRTIO_GPU_RESP_OK_NODATA);
    if (status == STATUS_IO_TIMEOUT || s2 == STATUS_IO_TIMEOUT || s3 == STATUS_IO_TIMEOUT) {
        /* the host may still use the pages later: leak them rather than hand them out again */
        VGPU_LOG("res %u: host did not answer, leaking %Iu bytes of backing", res->Id, size);
        res->Mdl = NULL;
        status = STATUS_IO_TIMEOUT;
        goto fail;
    }
    if (NT_SUCCESS(status) && (!NT_SUCCESS(s2) || !NT_SUCCESS(s3))) {
        UnrefHost(dev, res->Id);
        status = !NT_SUCCESS(s2) ? s2 : s3;
    }
    if (!NT_SUCCESS(status))
        goto fail;

    VgpuReleaseRequest(create);
    VgpuReleaseRequest(attach);
    VgpuReleaseRequest(ctxAttach);

    status = Publish(File, res, gen, Id);
    if (NT_SUCCESS(status))
        VGPU_TRACE("ctx %u res %u target %u format %u %ux%ux%u size %Iu", ctx, *Id, A->target, A->format,
                   A->width, A->height, A->depth, size);
    return status;

fail:
    if (create)
        VgpuReleaseRequest(create);
    if (attach)
        VgpuReleaseRequest(attach);
    if (ctxAttach)
        VgpuReleaseRequest(ctxAttach);
    if (res->Mdl) {
        MmFreePagesFromMdl(res->Mdl);
        ExFreePool(res->Mdl);
    }
    ExFreePoolWithTag(res, VGPU_TAG);
    return status;
}

/*
 * Host memory blob (GL persistent/coherent buffers): the host creates the resource from the
 * virgl command in Cmd (pool memory, ownership taken), the blob command then binds it to a
 * resource id. There is no guest backing; MAP maps it through the host visible region.
 */
NTSTATUS VgpuCreateBlobResource(PVGPU_FILE File, const struct vgpu_resource_create_blob *A, PVOID Cmd, UINT32 *Id)
{
    PVGPU_DEVICE dev = File->Dev;
    PVGPU_REQUEST submit = NULL, create = NULL, ctxAttach = NULL;
    VIRTIO_GPU_RESOURCE_CREATE_BLOB *c;
    PVGPU_RESOURCE res = NULL;
    NTSTATUS status, s2;
    UINT32 ctx;
    ULONG gen;

    KeAcquireGuardedMutex(&File->Lock);
    ctx = File->CtxId;
    gen = File->Generation;
    status = File->State == ContextActive ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
    KeReleaseGuardedMutex(&File->Lock);
    if (!NT_SUCCESS(status))
        goto out;

    status = STATUS_INVALID_PARAMETER;
    if (!dev->HasBlob || !dev->HostVisibleSize)
        status = STATUS_NOT_SUPPORTED;
    else if (A->blob_mem == VGPU_BLOB_MEM_HOST3D && !(A->blob_flags & ~VGPU_BLOB_FLAG_USE_MAPPABLE) &&
             A->size && A->size <= VGPU_MAX_RESOURCE_SIZE && A->size <= dev->HostVisibleSize)
        status = STATUS_SUCCESS;
    if (!NT_SUCCESS(status))
        goto out;

    status = STATUS_INSUFFICIENT_RESOURCES;
    res = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*res), VGPU_TAG);
    if (!res)
        goto out;
    RtlZeroMemory(res, sizeof(*res));
    res->Blob = TRUE;
    res->Size = ROUND_TO_PAGES((SIZE_T)A->size);
    do {
        res->Id = (UINT32)InterlockedIncrement(&dev->NextResourceId);
    } while (res->Id == 0);

    if (A->cmd_size) {
        submit = VgpuAllocRequest(sizeof(VIRTIO_GPU_SUBMIT_3D), sizeof(VIRTIO_GPU_HDR), VIRTIO_GPU_CMD_SUBMIT_3D, ctx);
        if (!submit)
            goto out;
        ((VIRTIO_GPU_SUBMIT_3D *)submit->Cmd)->Size = A->cmd_size;
        submit->Data = Cmd;
        submit->DataSize = A->cmd_size;
        Cmd = NULL;
    }
    create = VgpuAllocRequest(sizeof(*c), sizeof(VIRTIO_GPU_HDR), VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, ctx);
    ctxAttach = VgpuAllocRequest(sizeof(VIRTIO_GPU_RESOURCE_CMD), sizeof(VIRTIO_GPU_HDR),
                                 VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx);
    if (!create || !ctxAttach)
        goto out;
    c = create->Cmd;
    c->ResourceId = res->Id;
    c->BlobMem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    c->BlobFlags = A->blob_flags;
    c->BlobId = A->blob_id;
    c->Size = res->Size;
    ((VIRTIO_GPU_RESOURCE_CMD *)ctxAttach->Cmd)->ResourceId = res->Id;

    /* in queue order: the virgl create, the blob, the attach; wait for the last two */
    if (submit) {
        status = VgpuSubmit(dev, submit, FALSE);
        if (!NT_SUCCESS(status))
            goto out;
    }
    status = VgpuSubmit(dev, create, FALSE);
    if (!NT_SUCCESS(status))
        goto out;
    s2 = VgpuSubmit(dev, ctxAttach, FALSE);
    status = VgpuWaitRequest(create, VIRTIO_GPU_RESP_OK_NODATA);
    if (NT_SUCCESS(s2))
        s2 = VgpuWaitRequest(ctxAttach, VIRTIO_GPU_RESP_OK_NODATA);
    if (NT_SUCCESS(status) && !NT_SUCCESS(s2)) {
        UnrefHost(dev, res->Id);
        status = s2;
    }
    if (!NT_SUCCESS(status))
        goto out;

    status = Publish(File, res, gen, Id);
    res = NULL;                             /* published or destroyed */
    if (NT_SUCCESS(status))
        VGPU_TRACE("ctx %u blob res %u id %I64u size %I64u", ctx, *Id, A->blob_id, A->size);

out:
    if (submit)
        VgpuReleaseRequest(submit);
    if (create)
        VgpuReleaseRequest(create);
    if (ctxAttach)
        VgpuReleaseRequest(ctxAttach);
    if (res)
        ExFreePoolWithTag(res, VGPU_TAG);
    if (Cmd)
        ExFreePoolWithTag(Cmd, VGPU_TAG);
    return status;
}

/* File->Lock held: have the host map the blob into a free slice of the host visible region */
static NTSTATUS HostMapBlob(PVGPU_FILE File, PVGPU_RESOURCE Res)
{
    PVGPU_DEVICE dev = File->Dev;
    ULONG pages = (ULONG)(Res->Size >> PAGE_SHIFT), first, protect;
    PHYSICAL_ADDRESS pa;
    PVGPU_REQUEST req;
    NTSTATUS status;
    UINT32 info = 0;

    ExAcquireFastMutex(&dev->HostVisibleLock);
    first = RtlFindClearBitsAndSet(&dev->HostVisibleMap, pages, 0);
    ExReleaseFastMutex(&dev->HostVisibleLock);
    if (first == 0xFFFFFFFF) {
        VGPU_LOG("host visible region full (%I64u MB): raise hostmem= on the host", dev->HostVisibleSize >> 20);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Res->ShmOffset = (UINT64)first << PAGE_SHIFT;

    req = VgpuAllocRequest(sizeof(VIRTIO_GPU_RESOURCE_MAP_BLOB), sizeof(VIRTIO_GPU_RESP_MAP_INFO),
                           VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, 0);
    status = STATUS_INSUFFICIENT_RESOURCES;
    if (req) {
        ((VIRTIO_GPU_RESOURCE_MAP_BLOB *)req->Cmd)->ResourceId = Res->Id;
        ((VIRTIO_GPU_RESOURCE_MAP_BLOB *)req->Cmd)->Offset = Res->ShmOffset;
        status = VgpuSubmit(dev, req, FALSE);
        if (NT_SUCCESS(status))
            status = VgpuWaitRequest(req, VIRTIO_GPU_RESP_OK_MAP_INFO);
        if (NT_SUCCESS(status))
            info = ((VIRTIO_GPU_RESP_MAP_INFO *)req->Resp)->MapInfo & VIRTIO_GPU_MAP_CACHE_MASK;
        VgpuReleaseRequest(req);
    }
    if (status == STATUS_IO_TIMEOUT)
        return status;                      /* the host may still map it later: keep the slice */
    if (!NT_SUCCESS(status))
        goto fail;

    switch (info) {
    case VIRTIO_GPU_MAP_CACHE_CACHED:
        Res->CacheType = MmCached;
        protect = PAGE_READWRITE;
        break;
    case VIRTIO_GPU_MAP_CACHE_WC:
        Res->CacheType = MmWriteCombined;
        protect = PAGE_READWRITE | PAGE_WRITECOMBINE;
        break;
    default:
        Res->CacheType = MmNonCached;
        protect = PAGE_READWRITE | PAGE_NOCACHE;
        break;
    }
    pa.QuadPart = dev->HostVisibleBase.QuadPart + (LONGLONG)Res->ShmOffset;
    Res->IoAddress = MmMapIoSpaceEx(pa, Res->Size, protect);
    if (Res->IoAddress) {
        Res->Mdl = IoAllocateMdl(Res->IoAddress, (ULONG)Res->Size, FALSE, FALSE, NULL);
        if (Res->Mdl) {
            MmBuildMdlForNonPagedPool(Res->Mdl);
            Res->HostMapped = TRUE;
            VGPU_TRACE("res %u at +0x%I64x, cache %u", Res->Id, Res->ShmOffset, info);
            return STATUS_SUCCESS;
        }
        MmUnmapIoSpace(Res->IoAddress, Res->Size);
        Res->IoAddress = NULL;
    }
    status = STATUS_INSUFFICIENT_RESOURCES;
    req = VgpuAllocRequest(sizeof(VIRTIO_GPU_RESOURCE_CMD), sizeof(VIRTIO_GPU_HDR), VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB, 0);
    if (req) {
        ((VIRTIO_GPU_RESOURCE_CMD *)req->Cmd)->ResourceId = Res->Id;
        VgpuExecute(dev, req, VIRTIO_GPU_RESP_OK_NODATA);
    }
fail:
    ExAcquireFastMutex(&dev->HostVisibleLock);
    RtlClearBits(&dev->HostVisibleMap, first, pages);
    ExReleaseFastMutex(&dev->HostVisibleLock);
    return status;
}

NTSTATUS VgpuCloseResource(PVGPU_FILE File, UINT32 Id)
{
    VGPU_RESOURCE_ENTRY key = { Id, NULL };
    PVGPU_RESOURCE res;
    BOOLEAN alive;

    KeAcquireGuardedMutex(&File->Lock);
    res = VgpuLookupResource(File, Id);
    if (res)
        RtlDeleteElementGenericTableAvl(&File->Resources, &key);
    alive = HostAlive(File);
    KeReleaseGuardedMutex(&File->Lock);

    if (!res)
        return STATUS_INVALID_PARAMETER;
    DestroyResource(File, res, alive);
    VGPU_TRACE("res %u", Id);
    return STATUS_SUCCESS;
}

/* map the backing into the calling process; repeated calls return the same address */
NTSTATUS VgpuMapResource(PVGPU_FILE File, UINT32 Id, UINT64 *Address)
{
    PVGPU_RESOURCE res;
    NTSTATUS status = STATUS_SUCCESS;
    PVOID va = NULL;

    KeAcquireGuardedMutex(&File->Lock);
    res = VgpuLookupResource(File, Id);
    if (!res) {
        status = STATUS_INVALID_PARAMETER;
    } else if (res->UserAddress) {
        if (res->MapProcess == PsGetCurrentProcess())
            va = res->UserAddress;
        else
            status = STATUS_ACCESS_DENIED;
    } else {
        if (res->Blob && !res->HostMapped)
            status = HostMapBlob(File, res);
        __try {
            va = NT_SUCCESS(status) ? MmMapLockedPagesSpecifyCache(res->Mdl, UserMode,
                                                                   res->Blob ? res->CacheType : MmCached, NULL,
                                                                   FALSE, NormalPagePriority | MdlMappingNoExecute)
                                    : NULL;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            va = NULL;
        }
        if (va) {
            res->UserAddress = va;
            res->MapProcess = PsGetCurrentProcess();
            ObReferenceObject(res->MapProcess);
        } else if (NT_SUCCESS(status)) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    KeReleaseGuardedMutex(&File->Lock);

    *Address = (UINT64)(ULONG_PTR)va;
    return status;
}

void VgpuDestroyAllResources(PVGPU_FILE File)
{
    for (;;) {
        VGPU_RESOURCE_ENTRY *e;
        PVGPU_RESOURCE res = NULL;
        BOOLEAN alive;

        KeAcquireGuardedMutex(&File->Lock);
        e = RtlGetElementGenericTableAvl(&File->Resources, 0);
        if (e) {
            VGPU_RESOURCE_ENTRY key = *e;
            res = key.Resource;
            RtlDeleteElementGenericTableAvl(&File->Resources, &key);
        }
        alive = HostAlive(File);
        KeReleaseGuardedMutex(&File->Lock);

        if (!res)
            break;
        DestroyResource(File, res, alive);
    }
}
