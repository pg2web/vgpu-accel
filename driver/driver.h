/* SPDX-License-Identifier: MIT */
/*
 * vgpu-accel: non-display KMDF driver that gives Windows guests OpenGL through
 * QEMU virtio-gpu-gl (virgl). Internal definitions.
 */
#pragma once

#include <ntifs.h>
#include <wdf.h>

#include "osdep.h"
#include "kdebugprint.h"
#include "virtio_pci.h"
#include "VirtIO.h"
#include "virtio_ring.h"
#include "VirtIOWdf.h"
#include "virtio_gpu.h"
#include "vgpu_ioctl.h"

#define VGPU_TAG            'cagV'
#define VGPU_DRIVER_VERSION 0x00020001     /* 2.1: host-visible blob resources */

#define VGPU_MAX_CAPSETS        8
#define VGPU_MAX_RESOURCE_SIZE  (512u << 20)
#define VGPU_MAX_SUBMIT_SIZE    (768u << 10)  /* Mesa sends at most ~260 KB; fits one indirect table */
#define VGPU_MAX_BO_HANDLES     65536
#define VGPU_MAX_CTX_PARAMS     16
#define VGPU_MAX_SG             256         /* one indirect descriptor page */
#define VGPU_DIRECT_SG          8           /* larger requests use an indirect table */
#define VGPU_SYNC_TIMEOUT_MS    30000
#define VGPU_MAX_BLOB_CMD       4096
#define VGPU_MAX_HOST_VISIBLE   (64ull << 30)  /* bitmap of 4 KB pages: 2 MB at most */

/* ERROR level is always printed; INFO needs the IHVDRIVER debug filter (e.g. 0xF) */
#define VGPU_LOG(fmt, ...)   DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[vgpu] " fmt "\n", __VA_ARGS__)
#define VGPU_TRACE(fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[vgpu] %s: " fmt "\n", __FUNCTION__, __VA_ARGS__)

typedef struct _VGPU_CAPSET {
    UINT32 Id;
    UINT32 MaxVersion;
    UINT32 MaxSize;
    PVOID Cached;           /* capset data for MaxVersion, fetched on first use */
    UINT32 CachedVersion;
} VGPU_CAPSET;

typedef struct _VGPU_DEVICE {
    WDFDEVICE Device;
    VIRTIO_WDF_DRIVER Virtio;
    WDFINTERRUPT Interrupt;
    BOOLEAN MessageSignaled;
    struct virtqueue *ControlQueue;

    /* QueueLock protects the control queue, the fence timeline and the fence waiter list */
    KSPIN_LOCK QueueLock;
    BOOLEAN Ready;                  /* queue usable: between D0Entry and D0Exit */
    ULONG InFlight;                 /* requests owned by the device */
    UINT64 NextFence;               /* last fence id handed out */
    UINT64 CompletedFence;          /* highest fence id the host reported done */
    LIST_ENTRY FenceWaiters;
    KEVENT RingSpace;               /* set whenever the device returns buffers */

    BOOLEAN HasVirgl;
    BOOLEAN HasContextInit;
    BOOLEAN HasIndirect;
    BOOLEAN HasBlob;

    /* host visible shared memory region (hostmem=): blobs are mapped into it by the host */
    PHYSICAL_ADDRESS HostVisibleBase;
    UINT64 HostVisibleSize;
    FAST_MUTEX HostVisibleLock;
    RTL_BITMAP HostVisibleMap;      /* one bit per page in use */
    PULONG HostVisibleBits;
    ULONG Generation;               /* bumped on every D0Entry: host state from before is gone */

    FAST_MUTEX CapsetLock;
    UINT32 NumCapsets;
    UINT64 CapsetMask;
    VGPU_CAPSET Capsets[VGPU_MAX_CAPSETS];

    volatile LONG NextContextId;
    volatile LONG NextResourceId;
} VGPU_DEVICE, *PVGPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VGPU_DEVICE, VgpuGetDevice);

typedef enum _VGPU_CONTEXT_STATE { ContextNone, ContextActive, ContextDestroyed } VGPU_CONTEXT_STATE;

typedef struct _VGPU_RESOURCE {
    UINT32 Id;                      /* host resource id; also the handle user mode sees */
    SIZE_T Size;
    PMDL Mdl;                       /* guest backing pages */
    PVOID UserAddress;              /* mapping in MapProcess, if mapped */
    PEPROCESS MapProcess;
    UINT64 BusyFence;               /* last fence that uses the resource */

    /* host blob (no guest backing): mapped through the host visible region on first MAP */
    BOOLEAN Blob;
    BOOLEAN HostMapped;
    UINT64 ShmOffset;
    PVOID IoAddress;                /* kernel mapping of the region slice; Mdl describes it */
    MEMORY_CACHING_TYPE CacheType;  /* as the host asked for in MAP_BLOB */
} VGPU_RESOURCE, *PVGPU_RESOURCE;

/* per open handle: one virgl context and the resources created through it */
typedef struct _VGPU_FILE {
    PVGPU_DEVICE Dev;
    KGUARDED_MUTEX Lock;            /* protects everything below */
    VGPU_CONTEXT_STATE State;
    UINT32 CtxId;
    ULONG Generation;
    RTL_AVL_TABLE Resources;        /* VGPU_RESOURCE_ENTRY keyed by id */
} VGPU_FILE, *PVGPU_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VGPU_FILE, VgpuGetFile);

typedef struct _VGPU_RESOURCE_ENTRY {
    UINT32 Id;
    PVGPU_RESOURCE Resource;
} VGPU_RESOURCE_ENTRY;

/*
 * One control queue command. Allocated with its command and response buffers inline;
 * Data is an optional second out buffer (command stream, backing entries). Reference counted:
 * one reference for the submitter, one while the device owns it.
 */
typedef struct _VGPU_REQUEST {
    volatile LONG Refs;
    KEVENT Done;
    UINT64 Fence;                   /* 0 if not fenced */
    PVOID Cmd;
    ULONG CmdSize;
    PVOID Data;                     /* owned: freed with the request */
    ULONG DataSize;
    BOOLEAN DataContiguous;         /* Data came from MmAllocateContiguousMemorySpecifyCache */
    VIRTIO_GPU_HDR *Resp;
    ULONG RespSize;
    PVOID Indirect;                 /* indirect descriptor page, if used */
} VGPU_REQUEST, *PVGPU_REQUEST;

typedef struct _VGPU_FENCE_WAITER {
    LIST_ENTRY Link;
    UINT64 Fence;
    PKEVENT Event;                  /* signalled when CompletedFence >= Fence */
    BOOLEAN Owned;                  /* holds a reference on a user Event; freed by the signaller */
    KEVENT LocalEvent;              /* used when a thread waits itself (never on the stack: */
                                    /* UserMode waits allow the kernel stack to be paged out) */
} VGPU_FENCE_WAITER, *PVGPU_FENCE_WAITER;

/* device.c */
EVT_WDF_DRIVER_DEVICE_ADD VgpuEvtDeviceAdd;

/* queue.c */
PVGPU_REQUEST VgpuAllocRequest(ULONG CmdSize, ULONG RespSize, UINT32 Type, UINT32 CtxId);
void VgpuReleaseRequest(PVGPU_REQUEST Req);
NTSTATUS VgpuSubmit(PVGPU_DEVICE Dev, PVGPU_REQUEST Req, BOOLEAN Fenced);
NTSTATUS VgpuWaitRequest(PVGPU_REQUEST Req, UINT32 ExpectedResp);
NTSTATUS VgpuExecute(PVGPU_DEVICE Dev, PVGPU_REQUEST Req, UINT32 ExpectedResp);
void VgpuProcessCompletions(PVGPU_DEVICE Dev);
void VgpuFailOutstanding(PVGPU_DEVICE Dev);
BOOLEAN VgpuFenceDone(PVGPU_DEVICE Dev, UINT64 Fence);
NTSTATUS VgpuWaitFence(PVGPU_DEVICE Dev, UINT64 Fence);
void VgpuSignalOnFence(PVGPU_DEVICE Dev, UINT64 Fence, PKEVENT Event);

/* resource.c */
void VgpuInitResourceTable(PVGPU_FILE File);
NTSTATUS VgpuCreateResource(PVGPU_FILE File, const struct vgpu_resource_create *Args, UINT32 *Id);
NTSTATUS VgpuCloseResource(PVGPU_FILE File, UINT32 Id);
NTSTATUS VgpuCreateBlobResource(PVGPU_FILE File, const struct vgpu_resource_create_blob *A, PVOID Cmd, UINT32 *Id);
NTSTATUS VgpuMapResource(PVGPU_FILE File, UINT32 Id, UINT64 *Address);
void VgpuDestroyAllResources(PVGPU_FILE File);
PVGPU_RESOURCE VgpuLookupResource(PVGPU_FILE File, UINT32 Id);

/* ioctl.c */
EVT_WDF_IO_IN_CALLER_CONTEXT VgpuEvtIoInCallerContext;
NTSTATUS VgpuQueryCapsets(PVGPU_DEVICE Dev);
void VgpuFreeCapsets(PVGPU_DEVICE Dev);
NTSTATUS VgpuEnsureContext(PVGPU_FILE File, UINT32 CapsetId, BOOLEAN Explicit);
void VgpuDestroyContext(PVGPU_FILE File);
