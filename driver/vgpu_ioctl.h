/* SPDX-License-Identifier: MIT */
/*
 * vgpu-accel user/kernel interface (ABI version 1).
 *
 * IOCTL_VGPU_RESOURCE_CREATE_BLOB was added in driver 2.1 (GETPARAM RESOURCE_BLOB/HOST_VISIBLE report it).
 *
 * Shared by vgpu.sys, the Mesa winsys (src/gallium/winsys/virgl/lib/vgpu_api.c) and the tools.
 * Every structure uses fixed-width fields only, so 32-bit (WOW64) and 64-bit callers share one
 * layout. Pointers are passed as UINT64 user addresses. The IOCTL codes and layouts of ABI 1 are
 * frozen; new functionality gets new codes, and IOCTL_VGPU_GET_VERSION reports what is there.
 */
#pragma once

#ifdef _KERNEL_MODE
#include <ntdef.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

/* {31c22912-7210-11ed-bf22-bce92fa2e22d} */
#define VGPU_INTERFACE_GUID_INIT { 0x31c22912, 0x7210, 0x11ed, { 0xbf, 0x22, 0xbc, 0xe9, 0x2f, 0xa2, 0xe2, 0x2d } }

#define VGPU_ABI_VERSION 1

#define VGPU_CTL(fn, method) CTL_CODE(FILE_DEVICE_UNKNOWN, fn, method, FILE_ANY_ACCESS)
#define IOCTL_VGPU_CONTEXT_INIT       VGPU_CTL(0x800, METHOD_OUT_DIRECT)  /* in: vgpu_context_init */
#define IOCTL_VGPU_DESTROY_CONTEXT    VGPU_CTL(0x801, METHOD_IN_DIRECT)   /* no buffers */
#define IOCTL_VGPU_MAP                VGPU_CTL(0x802, METHOD_OUT_DIRECT)  /* in: vgpu_map, out: UINT64 address */
#define IOCTL_VGPU_EXECBUFFER         VGPU_CTL(0x803, METHOD_IN_DIRECT)   /* in: vgpu_execbuffer */
#define IOCTL_VGPU_GETPARAM           VGPU_CTL(0x804, METHOD_OUT_DIRECT)  /* in: UINT64 param, out: UINT64 value */
#define IOCTL_VGPU_GET_CAPS           VGPU_CTL(0x805, METHOD_OUT_DIRECT)  /* in: vgpu_get_caps, out: capset data */
#define IOCTL_VGPU_RESOURCE_CREATE    VGPU_CTL(0x806, METHOD_OUT_DIRECT)  /* in: vgpu_resource_create, out: vgpu_resource_create_resp */
#define IOCTL_VGPU_RESOURCE_CLOSE     VGPU_CTL(0x807, METHOD_IN_DIRECT)   /* in: vgpu_handle */
#define IOCTL_VGPU_TRANSFER_FROM_HOST VGPU_CTL(0x808, METHOD_IN_DIRECT)   /* in: vgpu_transfer */
#define IOCTL_VGPU_TRANSFER_TO_HOST   VGPU_CTL(0x809, METHOD_IN_DIRECT)   /* in: vgpu_transfer */
#define IOCTL_VGPU_WAIT               VGPU_CTL(0x810, METHOD_OUT_DIRECT)  /* in: vgpu_wait, out: UINT32 busy */
#define IOCTL_VGPU_RESOURCE_CREATE_BLOB VGPU_CTL(0x811, METHOD_OUT_DIRECT) /* in: vgpu_resource_create_blob, out: vgpu_resource_create_resp */
#define IOCTL_VGPU_GET_VERSION        VGPU_CTL(0x812, METHOD_BUFFERED)    /* out: vgpu_version */

/* IOCTL_VGPU_GETPARAM parameters (same numbering as Linux virtgpu_drm.h) */
#define VGPU_PARAM_3D_FEATURES        1
#define VGPU_PARAM_CAPSET_QUERY_FIX   2
#define VGPU_PARAM_RESOURCE_BLOB      3
#define VGPU_PARAM_HOST_VISIBLE       4
#define VGPU_PARAM_CROSS_DEVICE       5
#define VGPU_PARAM_CONTEXT_INIT       6
#define VGPU_PARAM_SUPPORTED_CAPSET_IDS 7

/* vgpu_context_init parameters */
#define VGPU_CONTEXT_PARAM_CAPSET_ID       1
#define VGPU_CONTEXT_PARAM_NUM_RINGS       2
#define VGPU_CONTEXT_PARAM_POLL_RINGS_MASK 3

#define VGPU_EXECBUF_FENCE_IN   0x01   /* wait for in_fence (event handle) before submitting */
#define VGPU_EXECBUF_FENCE_OUT  0x02   /* signal out_fence (event handle) when the host completes */
#define VGPU_WAIT_NOWAIT        0x01

/* vgpu_resource_create_blob (same values as Linux virtgpu_drm.h) */
#define VGPU_BLOB_MEM_HOST3D          0x0002
#define VGPU_BLOB_FLAG_USE_MAPPABLE   0x0001

#include <pshpack8.h>

struct vgpu_context_set_param { UINT64 param, value; };
struct vgpu_context_init { UINT32 num_params, pad; UINT64 params; /* -> vgpu_context_set_param[] */ };
struct vgpu_get_caps { UINT32 capset_id, capset_version; };

struct vgpu_resource_create {
    UINT32 target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
    UINT32 bo_handle, res_handle;   /* ignored on input */
    UINT32 size;                    /* guest backing size in bytes; at least one page is attached */
    UINT32 stride;
};
struct vgpu_resource_create_resp { UINT32 bo_handle, res_handle; };

/* host memory blob (GL persistent/coherent buffers); same layout as Linux drm_virtgpu_resource_create_blob */
struct vgpu_resource_create_blob {
    UINT32 blob_mem, blob_flags;
    UINT32 bo_handle, res_handle;   /* ignored on input */
    UINT64 size;
    UINT32 pad, cmd_size;
    UINT64 cmd;                     /* -> virgl command creating the host resource, cmd_size bytes */
    UINT64 blob_id;
};

struct vgpu_handle { UINT32 handle, pad; };
struct vgpu_map { UINT64 offset /* unused */; UINT32 handle, pad; };
struct vgpu_box { UINT32 x, y, z, w, h, d; };
struct vgpu_transfer {
    UINT32 bo_handle;
    struct vgpu_box box;
    UINT32 level, offset, stride, layer_stride, pad;
};
struct vgpu_wait { UINT32 handle, flags; };

struct vgpu_execbuffer {
    UINT32 flags, size;
    UINT64 command;         /* -> virgl command stream, size bytes */
    UINT64 bo_handles;      /* -> UINT32[num_bo_handles]: resources the commands use */
    UINT32 num_bo_handles, ring_idx;
    UINT64 in_fence, out_fence;   /* event HANDLEs */
};

struct vgpu_version { UINT32 abi_version, driver_version; };

#include <poppack.h>

C_ASSERT(sizeof(struct vgpu_context_init) == 16);
C_ASSERT(sizeof(struct vgpu_resource_create) == 56);
C_ASSERT(sizeof(struct vgpu_transfer) == 48);
C_ASSERT(sizeof(struct vgpu_execbuffer) == 48);
C_ASSERT(sizeof(struct vgpu_map) == 16);
C_ASSERT(sizeof(struct vgpu_resource_create_blob) == 48);
