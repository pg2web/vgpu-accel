/* SPDX-License-Identifier: MIT */
/*
 * virtio-gpu device protocol, the subset vgpu-accel uses (virtio specification 1.2+,
 * section "GPU Device"). All fields are little endian.
 */
#pragma once

#define VIRTIO_GPU_F_VIRGL          0
#define VIRTIO_GPU_F_RESOURCE_BLOB  3
#define VIRTIO_GPU_F_CONTEXT_INIT   4

/* virtio PCI capability describing a shared memory region (virtio 1.2, 4.1.4.7) */
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8
#define VIRTIO_GPU_SHM_ID_HOST_VISIBLE   1

#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB    0x010c
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB       0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB     0x0209

#define VIRTIO_GPU_RESP_OK_NODATA       0x1100
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO  0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET       0x1103
#define VIRTIO_GPU_RESP_OK_MAP_INFO     0x1106
#define VIRTIO_GPU_RESP_ERR_UNSPEC      0x1200

#define VIRTIO_GPU_FLAG_FENCE  0x01

#define VIRTIO_GPU_BLOB_MEM_HOST3D        0x0002
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE 0x0001

#define VIRTIO_GPU_MAP_CACHE_MASK     0x0f
#define VIRTIO_GPU_MAP_CACHE_CACHED   0x01
#define VIRTIO_GPU_MAP_CACHE_UNCACHED 0x02
#define VIRTIO_GPU_MAP_CACHE_WC       0x03

#include <pshpack1.h>

typedef struct _VIRTIO_GPU_HDR {
    UINT32 Type;
    UINT32 Flags;
    UINT64 FenceId;
    UINT32 CtxId;
    UINT8 RingIdx;
    UINT8 Padding[3];
} VIRTIO_GPU_HDR;

typedef struct _VIRTIO_GPU_CONFIG {
    UINT32 EventsRead;
    UINT32 EventsClear;
    UINT32 NumScanouts;
    UINT32 NumCapsets;
} VIRTIO_GPU_CONFIG;

typedef struct _VIRTIO_GPU_CTX_CREATE {
    VIRTIO_GPU_HDR Hdr;
    UINT32 NameLength;
    UINT32 ContextInit;     /* bits 0-7: capset id (VIRTIO_GPU_F_CONTEXT_INIT) */
    char DebugName[64];
} VIRTIO_GPU_CTX_CREATE;

typedef struct _VIRTIO_GPU_RESOURCE_CMD {    /* RESOURCE_UNREF, CTX_ATTACH_RESOURCE */
    VIRTIO_GPU_HDR Hdr;
    UINT32 ResourceId;
    UINT32 Padding;
} VIRTIO_GPU_RESOURCE_CMD;

typedef struct _VIRTIO_GPU_RESOURCE_CREATE_3D {
    VIRTIO_GPU_HDR Hdr;
    UINT32 ResourceId;
    UINT32 Target, Format, Bind, Width, Height, Depth, ArraySize, LastLevel, NrSamples, Flags;
    UINT32 Padding;
} VIRTIO_GPU_RESOURCE_CREATE_3D;

typedef struct _VIRTIO_GPU_ATTACH_BACKING {
    VIRTIO_GPU_HDR Hdr;
    UINT32 ResourceId;
    UINT32 NrEntries;       /* followed by NrEntries VIRTIO_GPU_MEM_ENTRY */
} VIRTIO_GPU_ATTACH_BACKING;

typedef struct _VIRTIO_GPU_MEM_ENTRY {
    UINT64 Address;
    UINT32 Length;
    UINT32 Padding;
} VIRTIO_GPU_MEM_ENTRY;

typedef struct _VIRTIO_GPU_BOX {
    UINT32 X, Y, Z, W, H, D;
} VIRTIO_GPU_BOX;

typedef struct _VIRTIO_GPU_TRANSFER_3D {
    VIRTIO_GPU_HDR Hdr;
    VIRTIO_GPU_BOX Box;
    UINT64 Offset;
    UINT32 ResourceId;
    UINT32 Level;
    UINT32 Stride;
    UINT32 LayerStride;
} VIRTIO_GPU_TRANSFER_3D;

typedef struct _VIRTIO_GPU_SUBMIT_3D {
    VIRTIO_GPU_HDR Hdr;
    UINT32 Size;            /* followed by Size bytes of virgl commands */
    UINT32 Padding;
} VIRTIO_GPU_SUBMIT_3D;

typedef struct _VIRTIO_GPU_GET_CAPSET_INFO {
    VIRTIO_GPU_HDR Hdr;
    UINT32 CapsetIndex;
    UINT32 Padding;
} VIRTIO_GPU_GET_CAPSET_INFO;

typedef struct _VIRTIO_GPU_RESP_CAPSET_INFO {
    VIRTIO_GPU_HDR Hdr;
    UINT32 CapsetId;
    UINT32 CapsetMaxVersion;
    UINT32 CapsetMaxSize;
    UINT32 Padding;
} VIRTIO_GPU_RESP_CAPSET_INFO;

typedef struct _VIRTIO_GPU_GET_CAPSET {
    VIRTIO_GPU_HDR Hdr;
    UINT32 CapsetId;
    UINT32 CapsetVersion;
} VIRTIO_GPU_GET_CAPSET;

typedef struct _VIRTIO_GPU_RESOURCE_CREATE_BLOB {
    VIRTIO_GPU_HDR Hdr;
    UINT32 ResourceId;
    UINT32 BlobMem;
    UINT32 BlobFlags;
    UINT32 NrEntries;       /* guest backing entries follow; 0 for host blobs */
    UINT64 BlobId;
    UINT64 Size;
} VIRTIO_GPU_RESOURCE_CREATE_BLOB;

typedef struct _VIRTIO_GPU_RESOURCE_MAP_BLOB {
    VIRTIO_GPU_HDR Hdr;
    UINT32 ResourceId;
    UINT32 Padding;
    UINT64 Offset;          /* in the host visible shared memory region */
} VIRTIO_GPU_RESOURCE_MAP_BLOB;

typedef struct _VIRTIO_GPU_RESP_MAP_INFO {
    VIRTIO_GPU_HDR Hdr;
    UINT32 MapInfo;         /* VIRTIO_GPU_MAP_CACHE_* */
    UINT32 Padding;
} VIRTIO_GPU_RESP_MAP_INFO;

/* virtio PCI shared memory capability (struct virtio_pci_cap64) */
typedef struct _VIRTIO_PCI_CAP64 {
    UINT8 CapVndr;          /* PCI_CAPABILITY_ID_VENDOR_SPECIFIC */
    UINT8 CapNext;
    UINT8 CapLen;
    UINT8 CfgType;
    UINT8 Bar;
    UINT8 Id;
    UINT8 Padding[2];
    UINT32 Offset;
    UINT32 Length;
    UINT32 OffsetHi;
    UINT32 LengthHi;
} VIRTIO_PCI_CAP64;

#include <poppack.h>

C_ASSERT(sizeof(VIRTIO_GPU_HDR) == 24);
C_ASSERT(sizeof(VIRTIO_GPU_CTX_CREATE) == 96);
C_ASSERT(sizeof(VIRTIO_GPU_RESOURCE_CREATE_3D) == 72);
C_ASSERT(sizeof(VIRTIO_GPU_TRANSFER_3D) == 72);
C_ASSERT(sizeof(VIRTIO_GPU_MEM_ENTRY) == 16);
C_ASSERT(sizeof(VIRTIO_GPU_RESOURCE_CREATE_BLOB) == 56);
C_ASSERT(sizeof(VIRTIO_GPU_RESOURCE_MAP_BLOB) == 40);
C_ASSERT(sizeof(VIRTIO_GPU_RESP_MAP_INFO) == 32);
C_ASSERT(sizeof(VIRTIO_PCI_CAP64) == 24);
