/*
 * vgputest - smoke test for vgpu.sys without Mesa.
 *
 * Exercises every ioctl against the host: params, capsets, context, resource
 * create/map, a data round trip through the host (to-host then from-host
 * transfer of a buffer), a fenced virgl NOP submit, a second handle, cleanup.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -static -o vgputest.exe vgputest.c -lsetupapi
 */
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

DEFINE_GUID(GUID_DEVINTERFACE_VGPU, 0x31c22912, 0x7210, 0x11ed, 0xbf, 0x22, 0xbc, 0xe9, 0x2f, 0xa2, 0xe2, 0x2d);

#define VGPU_IOCTL(fn, method) CTL_CODE(FILE_DEVICE_UNKNOWN, fn, method, FILE_ANY_ACCESS)
#define IOCTL_CONTEXT_INIT       VGPU_IOCTL(0x800, METHOD_OUT_DIRECT)
#define IOCTL_DESTROY_CONTEXT    VGPU_IOCTL(0x801, METHOD_IN_DIRECT)
#define IOCTL_MAP                VGPU_IOCTL(0x802, METHOD_OUT_DIRECT)
#define IOCTL_EXECBUFFER         VGPU_IOCTL(0x803, METHOD_IN_DIRECT)
#define IOCTL_GETPARAM           VGPU_IOCTL(0x804, METHOD_OUT_DIRECT)
#define IOCTL_GET_CAPS           VGPU_IOCTL(0x805, METHOD_OUT_DIRECT)
#define IOCTL_RESOURCE_CREATE    VGPU_IOCTL(0x806, METHOD_OUT_DIRECT)
#define IOCTL_RESOURCE_CLOSE     VGPU_IOCTL(0x807, METHOD_IN_DIRECT)
#define IOCTL_TRANSFER_FROM_HOST VGPU_IOCTL(0x808, METHOD_IN_DIRECT)
#define IOCTL_TRANSFER_TO_HOST   VGPU_IOCTL(0x809, METHOD_IN_DIRECT)
#define IOCTL_WAIT               VGPU_IOCTL(0x810, METHOD_OUT_DIRECT)
#define IOCTL_RESOURCE_CREATE_BLOB VGPU_IOCTL(0x811, METHOD_OUT_DIRECT)
#define IOCTL_GET_VERSION        VGPU_IOCTL(0x812, METHOD_BUFFERED)

/* must match driver/vgpu_ioctl.h */
struct context_init { uint32_t num_params, pad; uint64_t ctx_set_params; };
struct context_set_param { uint64_t param, value; };
struct get_caps { uint32_t cap_set_id, cap_set_ver; };
struct resource_create {
    uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
    uint32_t bo_handle, res_handle, size, stride;
};
struct resource_create_resp { uint32_t bo_handle, res_handle; };
struct resource_create_blob {
    uint32_t blob_mem, blob_flags, bo_handle, res_handle;
    uint64_t size;
    uint32_t pad, cmd_size;
    uint64_t cmd, blob_id;
};
struct box3d { uint32_t x, y, z, w, h, d; };
struct transfer { uint32_t bo_handle; struct box3d box; uint32_t level, offset, stride, layer_stride, pad; };
struct wait3d { uint32_t handle, flags; };
struct map { uint64_t offset; uint32_t handle, pad; };
struct gem_close { uint32_t handle, pad; };
struct execbuffer {
    uint32_t flags, size;
    uint64_t command, bo_handles;
    uint32_t num_bo_handles, ring_idx;
    uint64_t in_fence_fd, out_fence_fd;
};

#define PARAM_3D_FEATURES      1
#define PARAM_CAPSET_QUERY_FIX 2
#define PARAM_RESOURCE_BLOB    3
#define PARAM_HOST_VISIBLE     4
#define PARAM_CROSS_DEVICE     5
#define PARAM_CONTEXT_INIT     6
#define PARAM_CAPSET_IDS       7
#define CONTEXT_PARAM_CAPSET_ID 1
#define EXECBUF_FENCE_FD_OUT   0x02

#define PIPE_BUFFER            0
#define VIRGL_FORMAT_R8_UNORM  64
#define VIRGL_BIND_VERTEX_BUFFER (1 << 4)
#define BUF_SIZE               65536
#define BLOB_MEM_HOST3D        2
#define BLOB_FLAG_MAPPABLE     1
#define VIRGL_CCMD_PIPE_RESOURCE_CREATE 48
#define VIRGL_RESOURCE_FLAG_MAP_PERSISTENT (1 << 1)
#define VIRGL_RESOURCE_FLAG_MAP_COHERENT   (1 << 2)

static int failures;

#define CHECK(cond, ...) do { \
        if (cond) { printf("  PASS  "); printf(__VA_ARGS__); printf("\n"); } \
        else { failures++; printf("  FAIL  "); printf(__VA_ARGS__); printf("  (err=%lu)\n", GetLastError()); } \
    } while (0)

static BOOL ioctl(HANDLE h, DWORD code, void *in, DWORD inlen, void *out, DWORD outlen)
{
    DWORD ret = 0;
    return DeviceIoControl(h, code, in, inlen, out, outlen, &ret, NULL);
}

static HANDLE open_device(void)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_VGPU, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifdata = { sizeof(ifdata) };
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD size = 0;

    if (set == INVALID_HANDLE_VALUE) {
        printf("  info  SetupDiGetClassDevs failed (err=%lu)\n", GetLastError());
        return h;
    }
    if (!SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_VGPU, 0, &ifdata)) {
        printf("  info  no vgpu device interface present (err=%lu): driver not started or interface not enabled\n", GetLastError());
        SetupDiDestroyDeviceInfoList(set);
        return h;
    }
    SetupDiGetDeviceInterfaceDetailW(set, &ifdata, NULL, 0, &size, NULL);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = malloc(size);
    detail->cbSize = sizeof(*detail);
    if (SetupDiGetDeviceInterfaceDetailW(set, &ifdata, detail, size, NULL, NULL)) {
        printf("  info  interface path: %ls\n", detail->DevicePath);
        h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            printf("  info  CreateFile(GENERIC_READ|GENERIC_WRITE) failed (err=%lu)\n", err);
            /* all ioctls are FILE_ANY_ACCESS, so a handle with no access rights still works;
               if this succeeds, the device DACL is what denies read/write */
            h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE)
                printf("  info  opened with no access rights instead: the device DACL denies read/write for this user\n");
            else
                printf("  info  CreateFile(0) also failed (err=%lu): the driver or I/O manager rejects the open itself\n", GetLastError());
        }
    } else {
        printf("  info  SetupDiGetDeviceInterfaceDetail failed (err=%lu)\n", GetLastError());
    }
    free(detail);
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

static uint64_t getparam(HANDLE h, uint64_t param, BOOL *ok)
{
    uint64_t value = 0;
    *ok = ioctl(h, IOCTL_GETPARAM, &param, sizeof(param), &value, sizeof(value));
    return value;
}

static BOOL context_init(HANDLE h, BOOL use_capset)
{
    struct context_set_param p = { CONTEXT_PARAM_CAPSET_ID, 2 /* VIRGL2 */ };
    struct context_init init = { use_capset ? 1 : 0, 0, use_capset ? (uint64_t)(uintptr_t)&p : 0 };
    return ioctl(h, IOCTL_CONTEXT_INIT, &init, sizeof(init), NULL, 0);
}

static uint32_t create_buffer(HANDLE h, uint32_t size)
{
    struct resource_create c = { 0 };
    struct resource_create_resp r = { 0 };
    c.target = PIPE_BUFFER;
    c.format = VIRGL_FORMAT_R8_UNORM;
    c.bind = VIRGL_BIND_VERTEX_BUFFER;
    c.width = size;
    c.height = c.depth = c.array_size = 1;
    c.size = size;
    if (!ioctl(h, IOCTL_RESOURCE_CREATE, &c, sizeof(c), &r, sizeof(r)))
        return 0;
    return r.bo_handle;
}

static BOOL transfer(HANDLE h, BOOL to_host, uint32_t bo, uint32_t size)
{
    struct transfer t = { 0 };
    t.bo_handle = bo;
    t.box.w = size;
    t.box.h = t.box.d = 1;
    return ioctl(h, to_host ? IOCTL_TRANSFER_TO_HOST : IOCTL_TRANSFER_FROM_HOST, &t, sizeof(t), NULL, 0);
}

static BOOL wait_bo(HANDLE h, uint32_t bo)
{
    struct wait3d w = { bo, 0 };
    uint32_t busy = 1;
    return ioctl(h, IOCTL_WAIT, &w, sizeof(w), &busy, sizeof(busy)) && busy == 0;
}

static BOOL close_bo(HANDLE h, uint32_t bo)
{
    struct gem_close c = { bo, 0 };
    return ioctl(h, IOCTL_RESOURCE_CLOSE, &c, sizeof(c), NULL, 0);
}

int main(void)
{
    BOOL ok;
    HANDLE h, h2;

    printf("vgputest: vgpu.sys smoke test\n\n[1] open device\n");
    h = open_device();
    CHECK(h != INVALID_HANDLE_VALUE, "open device interface");
    if (h == INVALID_HANDLE_VALUE) {
        printf("\nDriver not loaded? Check Device Manager for 'VirtIO GPU 3D Accelerator'.\n");
        return 1;
    }

    printf("\n[2] params\n");
    uint64_t has3d = getparam(h, PARAM_3D_FEATURES, &ok);
    CHECK(ok && has3d == 1, "3D_FEATURES = %llu (virgl negotiated)", (unsigned long long)has3d);
    uint64_t ctxinit = getparam(h, PARAM_CONTEXT_INIT, &ok);
    CHECK(ok, "CONTEXT_INIT = %llu", (unsigned long long)ctxinit);
    uint64_t capsets = getparam(h, PARAM_CAPSET_IDS, &ok);
    CHECK(ok && (capsets & (1u << 2 | 1u << 1)), "CAPSET_IDS mask = 0x%llx (bit1 virgl, bit2 virgl2)", (unsigned long long)capsets);
    uint64_t blob = getparam(h, PARAM_RESOURCE_BLOB, &ok);
    uint64_t hostvis = ok ? getparam(h, PARAM_HOST_VISIBLE, &ok) : 0;
    CHECK(ok && blob == hostvis, "RESOURCE_BLOB = %llu, HOST_VISIBLE = %llu%s", (unsigned long long)blob,
          (unsigned long long)hostvis, hostvis ? "" : " (no host visible memory: add blob=true,hostmem=... on the host)");
    {
        struct { uint32_t abi, driver; } v = { 0, 0 };
        ok = ioctl(h, IOCTL_GET_VERSION, NULL, 0, &v, sizeof(v));
        if (ok)
            CHECK(v.abi == 1, "GET_VERSION: ABI %u, driver %u.%u", v.abi, v.driver >> 16, v.driver & 0xffff);
        else
            printf("  info  GET_VERSION not supported (driver older than 2.0)\n");
    }

    printf("\n[3] capsets\n");
    {
        static uint8_t caps[4096];
        uint32_t id = (capsets & (1u << 2)) ? 2 : 1;
        struct get_caps gc = { id, id == 2 ? 2 : 1 };
        memset(caps, 0, sizeof(caps));
        ok = ioctl(h, IOCTL_GET_CAPS, &gc, sizeof(gc), caps, sizeof(caps));
        uint32_t max_version = *(uint32_t *)caps;
        CHECK(ok && max_version > 0, "GET_CAPS id=%u: caps.max_version=%u", id, max_version);
    }

    printf("\n[4] context\n");
    CHECK(context_init(h, ctxinit == 1), "CONTEXT_INIT (%s)", ctxinit == 1 ? "capset virgl2" : "no params");
    CHECK(!context_init(h, ctxinit == 1), "second CONTEXT_INIT on same handle is rejected");

    printf("\n[5] resource + host round trip\n");
    uint32_t bo = create_buffer(h, BUF_SIZE);
    CHECK(bo != 0, "RESOURCE_CREATE buffer %u bytes -> handle %u", BUF_SIZE, bo);
    if (bo) {
        struct map m = { 0, bo, 0 };
        uint64_t ptr = 0;
        ok = ioctl(h, IOCTL_MAP, &m, sizeof(m), &ptr, sizeof(ptr));
        CHECK(ok && ptr, "MAP -> %p", (void *)(uintptr_t)ptr);
        if (ok && ptr) {
            uint8_t *p = (uint8_t *)(uintptr_t)ptr;
            for (int i = 0; i < BUF_SIZE; i++)
                p[i] = (uint8_t)(i * 7 + 3);
            CHECK(transfer(h, TRUE, bo, BUF_SIZE), "TRANSFER_TO_HOST");
            CHECK(wait_bo(h, bo), "WAIT after to-host");
            memset(p, 0, BUF_SIZE);
            CHECK(transfer(h, FALSE, bo, BUF_SIZE), "TRANSFER_FROM_HOST");
            CHECK(wait_bo(h, bo), "WAIT after from-host");
            int bad = 0;
            for (int i = 0; i < BUF_SIZE; i++)
                bad += p[i] != (uint8_t)(i * 7 + 3);
            CHECK(bad == 0, "data came back from the host intact (%d bad bytes)", bad);
        }
    }

    printf("\n[6] fenced submit\n");
    {
        uint32_t nop[1] = { 0 }; /* VIRGL_CCMD_NOP, length 0 */
        uint32_t handles[1] = { bo };
        HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        struct execbuffer eb = { 0 };
        eb.flags = EXECBUF_FENCE_FD_OUT;
        eb.size = sizeof(nop);
        eb.command = (uint64_t)(uintptr_t)nop;
        eb.bo_handles = (uint64_t)(uintptr_t)handles;
        eb.num_bo_handles = bo ? 1 : 0;
        eb.out_fence_fd = (uint64_t)(uintptr_t)ev;
        ok = ioctl(h, IOCTL_EXECBUFFER, &eb, sizeof(eb), NULL, 0);
        CHECK(ok, "EXECBUFFER (virgl NOP, fenced)");
        DWORD w = ok ? WaitForSingleObject(ev, 5000) : WAIT_FAILED;
        CHECK(w == WAIT_OBJECT_0, "fence signaled within 5s");
        if (bo)
            CHECK(wait_bo(h, bo), "resource idle after fence");
        CloseHandle(ev);
    }

    printf("\n[7] second handle (independent context)\n");
    h2 = open_device();
    CHECK(h2 != INVALID_HANDLE_VALUE, "open second handle");
    if (h2 != INVALID_HANDLE_VALUE) {
        CHECK(context_init(h2, ctxinit == 1), "CONTEXT_INIT on second handle");
        uint32_t bo2 = create_buffer(h2, 4096);
        CHECK(bo2 != 0, "RESOURCE_CREATE on second handle -> %u", bo2);
        CHECK(!close_bo(h2, bo), "first handle's resource is not visible on second handle");
        CloseHandle(h2); /* cleanup must free bo2 and the context */
        printf("  info  closed second handle with a live resource (driver cleans up)\n");
    }

    printf("\n[8] host visible blob (GL buffer storage)\n");
    if (!hostvis) {
        printf("  info  skipped: no host visible memory\n");
    } else {
        /* the virgl command that creates the host resource, as Mesa sends it */
        uint32_t cmd[12] = { 0 };
        struct resource_create_blob b = { 0 };
        struct resource_create_resp r = { 0 };
        cmd[0] = VIRGL_CCMD_PIPE_RESOURCE_CREATE | (11u << 16);
        cmd[1] = PIPE_BUFFER;
        cmd[2] = VIRGL_FORMAT_R8_UNORM;
        cmd[3] = VIRGL_BIND_VERTEX_BUFFER;
        cmd[4] = BUF_SIZE;
        cmd[5] = cmd[6] = cmd[7] = 1;
        cmd[10] = VIRGL_RESOURCE_FLAG_MAP_PERSISTENT | VIRGL_RESOURCE_FLAG_MAP_COHERENT;
        cmd[11] = 0x7e57;                   /* blob id */
        b.blob_mem = BLOB_MEM_HOST3D;
        b.blob_flags = BLOB_FLAG_MAPPABLE;
        b.size = BUF_SIZE;
        b.cmd_size = sizeof(cmd);
        b.cmd = (uint64_t)(uintptr_t)cmd;
        b.blob_id = 0x7e57;
        ok = ioctl(h, IOCTL_RESOURCE_CREATE_BLOB, &b, sizeof(b), &r, sizeof(r));
        CHECK(ok && r.bo_handle, "RESOURCE_CREATE_BLOB %u bytes -> handle %u", BUF_SIZE, r.bo_handle);
        if (ok && r.bo_handle) {
            struct map m = { 0, r.bo_handle, 0 };
            uint64_t ptr = 0;
            ok = ioctl(h, IOCTL_MAP, &m, sizeof(m), &ptr, sizeof(ptr));
            CHECK(ok && ptr, "MAP blob -> %p (host memory)", (void *)(uintptr_t)ptr);
            if (ok && ptr) {
                volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)ptr;
                int bad = 0;
                for (int i = 0; i < BUF_SIZE / 4; i++)
                    p[i] = 0x9e3779b9u * (uint32_t)i;
                for (int i = 0; i < BUF_SIZE / 4; i++)
                    bad += p[i] != 0x9e3779b9u * (uint32_t)i;
                CHECK(bad == 0, "write/read through the blob mapping (%d bad words)", bad);
            }
            CHECK(close_bo(h, r.bo_handle), "close blob");
        }
    }

    printf("\n[9] cleanup\n");
    if (bo) {
        CHECK(close_bo(h, bo), "RESOURCE_CLOSE");
        CHECK(!close_bo(h, bo), "double close is rejected");
    }
    CHECK(ioctl(h, IOCTL_DESTROY_CONTEXT, NULL, 0, NULL, 0), "DESTROY_CONTEXT");
    CHECK(create_buffer(h, 4096) == 0, "resource create without context is rejected");
    CloseHandle(h);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
