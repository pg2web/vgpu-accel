# vgpu-accel: design and technical details

This is the technical companion to the [README](../README.md): how the driver works, what was
changed in Mesa, the OpenGL feature gaps, the build internals and debugging notes.

## Overview
```
app -> opengl32.dll -> vio_ogl_x64.dll (Mesa virgl + WGL)
        -> DeviceIoControl -> vgpu.sys -> virtio-gpu (3D commands, resources, fences)
        -> QEMU virtio-gpu-gl -> virglrenderer -> host GPU
SwapBuffers: TRANSFER_FROM_HOST readback -> GDI blit into the window
```
- **`vgpu.sys`** is a small non-display KMDF 1.27 driver (x64, MIT). It binds to PCI
  `VEN_1AF4&DEV_1050` (virtio-gpu), speaks the virtio-gpu control queue, and exposes IOCTLs to user mode.
  There is no WDDM miniport, so the guest's display adapter (VGA, QXL, Remote Desktop) is untouched.
- **The OpenGL driver** (`vio_ogl_x64.dll`, `vio_ogl_x86.dll`) is Mesa's virgl Gallium driver with a
  Windows winsys that talks to `vgpu.sys`. It is registered either app-local (next to an app's exe)
  or system-wide as an OpenGL ICD (`OpenGLDrivers\MSOGL`).
- **Presentation** is a GDI readback: the frame is copied from the host (`TRANSFER_FROM_HOST`) and blitted
  into the window. That is why it works on any primary display and over RDP, and also why it is not
  as fast as a real display driver.

## The driver (`driver/`)
| File | Role |
|---|---|
| `device.c` | DriverEntry, device add, hardware prepare/release, power, interrupt (ISR/DPC), file create/cleanup, host-visible region discovery |
| `queue.c` | control queue: request allocation, submit, fence timeline, completions, waits |
| `resource.c` | resource create/close, mapping, blob and host-visible mapping, destroy |
| `ioctl.c` | IOCTL handlers, capsets, contexts, failure logging |
| `portio.c` | port I/O helpers (inline asm for clang-cl) |
| `vgpu_ioctl.h` | user/kernel ABI (version 1; IOCTL codes 0x800 to 0x811, `GET_VERSION` 0x812; fence fields are 64-bit so 32-bit and 64-bit callers share one layout) |
| `virtio_gpu.h` | virtio-gpu protocol definitions |
| `vgpu.inf` | install file; version and date are stamped by the build |

Design decisions worth knowing:
- All IOCTLs are handled in the caller's context (`EvtIoInCallerContext`).
- A fence timeline is kept under one queue lock; requests are reference counted.
- Resources live in a per-file table (AVL), so each handle sees only its own resources and a closed
  handle cleans up everything it owned.
- User-mode waits use pool-allocated events, never stack events.
- Indirect descriptors and one MSI vector; guest backing is allocated with
  `MmAllocatePagesForMdlEx` and is always at least one page (virglrenderer rejects copy transfers on
  textures without backing). Long backing lists use a contiguous entry buffer.
- Host-visible (blob) resources: the host-visible memory region is found through the virtio PCI vendor
  capability (shared memory, id 1; BAR 4 with `hostmem=1G`) and mapped into the process with
  `MmMapIoSpaceEx`, an MDL and `MmMapLockedPagesSpecifyCache(UserMode)`.
- The device security descriptor comes from INF `HKR,,Security` and applies after a disable/enable.

### virtio library
`third_party/virtio-win` is the BSD-3 virtio library from virtio-win (Red Hat, IBM, Google), taken from
max8rr8's viogpu3d fork. See its `VENDORED.txt` and `LICENSE`.

## The Mesa side
Mesa is not vendored. `mesa/prepare.sh` fetches tag `vgpu-accel-26.2.5-1` from
https://github.com/pg2web/mesa (commit-checked): official Mesa 26.2.5 (a root commit with exactly the
official file tree) plus one commit. That commit:
- adds `src/gallium/winsys/virgl/lib/vgpu_api.c/.h`, a small libdrm-like layer that finds the device and
  turns the virtio-gpu ioctls Mesa already uses into `vgpu.sys` IOCTLs;
- gives `virgl_drm_winsys` a Windows variant (a `HANDLE` instead of an fd, buffer mapping through the
  map IOCTL, fences as Win32 events);
- makes the WGL target create the virgl screen and present through GDI;
- makes small virgl driver changes (staging decision uses bind flags, a flush when the winsys has no
  coherent memory, `flush_frontbuffer` without a context);
- adds meson and header glue, and passes the blob-resource IOCTL through (0x811).

Compare the tag with the official one to see exactly what changed. For Mesa development, keep a checkout
next to this repo as `../mesa` (`build.sh` uses it when present), or set `MESA_SRC` / `MESA_BUILD`.
`MESA_URL`, `MESA_TAG` and `MESA_COMMIT` select another fork.

## OpenGL support
OpenGL **4.6** (core and compatibility) and **OpenGL ES 3.2**, as reported by `wglinfo` with blob
memory enabled (QEMU 10.2, virglrenderer 1.3, AMD radeonsi host). Compared with the same GPU on
the host, these optional extensions are not available in the guest, because virgl does not
forward them (upstream virglrenderer/Mesa limits, not this driver):

| Missing | Typical users |
|---|---|
| `ARB_bindless_texture` | some newer engines and renderers (most likely to matter) |
| `ARB_sparse_buffer`, `ARB_sparse_texture*` | virtual texturing; rare |
| `ARB_gpu_shader_int64`, `AMD_gpu_shader_int64`, `NV_shader_atomic_int64` | compute-heavy and scientific apps |
| `ARB_shader_ballot`, `KHR_shader_subgroup`, `EXT_demote_to_helper_invocation`, `NV_compute_shader_derivatives` | subgroup/wave operations, usually with fallbacks |
| `ARB_compute_variable_group_size`, `ARB_shader_viewport_layer_array`, `*_texture_filter_minmax`, `EXT_depth_bounds_test`, `EXT_window_rectangles`, `EXT_shader_image_load_formatted` | rare |
| `EXT_memory_object*`, `EXT_semaphore*`, `NV_timeline_semaphore` | GL/Vulkan interop (no Vulkan in the guest) |
| `AMD_performance_monitor`, `AMD_pinned_memory`, `INTEL_blackhole_render`, `ATI_texture_compression_3dc`, `EXT_texture_compression_latc` | profilers and old formats |

Apps that check for extensions are fine. An app that calls one of these anyway aborts, usually
with a "no provider of gl..." style message; that points to a virgl limitation.
Reported GPU memory (512 MB) is virgl's fixed value, not the host GPU's.

Without host-visible memory (`blob=true,hostmem=...`) the guest also lacks `GL_ARB_buffer_storage`, and apps
that call `glBufferStorage` anyway abort (for example Blender's box select).

## Why it replaces Red Hat's virtio-gpu driver
Red Hat's display driver (viogpudo, from virtio-win) and vgpu-accel both match the same hardware IDs
(`PCI\VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01` and the generic `PCI\VEN_1AF4&DEV_1050`). A PCI function has one
function driver, and both need to own the control queue, so they cannot share a device.

How Windows chooses between two packages that match a device (see Microsoft's
[ranking](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/how-windows-ranks-driver-packages) and
[selection](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/overview-of-the-driver-selection-process) pages):
lowest rank wins; on equal rank the **most recent INF date** wins, then the highest version. With the default
`AllSignersEqual` policy a WHQL signature does not outrank a trusted third-party one, so the date usually decides.
Our INF date is the build date, which is normally newer than Red Hat's package, so ours is chosen when both are
present. This is not a stable guarantee (a newer virtio-win release can win when Windows selects again), which is
why `setup.bat` removes the Red Hat GPU driver first.

Other notes:
- QEMU 10.2 has no property to change the subsystem or device ID of `virtio-gpu-gl-pci`, so the host cannot
  give the accelerator a different identity without a QEMU change.
- If a VM has two virtio-gpu devices, our INF matches both. On a plain (non-GL) device the driver fails to
  start (no virgl feature), so keep one virtio-gpu device, the GL one.

## Building

### Environment
Two ways, both start with the EWDK:

```sh
./fetch-ewdk.sh [cache-dir]     # once: EWDK subset into ./ewdk (about 1 GB)
```
Microsoft ships the EWDK (26100.6584: Windows SDK/WDK 10.0.26100.0, MSVC 14.44.35207) only as an
18.6 GB ISO, free to download but not redistributable. The script downloads it into `cache-dir`
(default `~/.cache/vgpu-accel`; it checks for about 20 GB free and says so if there is not), verifies
the pinned SHA-256, extracts only the headers and kernel libraries `vgpu.sys` needs, applies a
one-line clang 14 fix to `km/wdm.h`, and fetches `generate_vfs.py` (tmp64/clang-cl-linux, pinned by
commit and checksum). The ISO stays cached for later runs; `--delete-iso` removes it. Do not publish
the ISO or the extracted files.

**Docker (or Podman)**: everything else is in the image.
```sh
docker build -t vgpu-accel-env .
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" vgpu-accel-env ./build.sh
```
(With rootless Podman use `--userns=keep-id` and `-v "$PWD":/src:Z`.) The image contains EWDK files: build it
locally, do not publish it.

**Natively** (Debian 12 / similar): clang + lld (14 works), mingw-w64 gcc for x86_64 and i686 (posix
threads), meson 1.7 + ninja, bison, flex, glslangValidator, Python with mako/PyYAML, osslsigncode,
openssl and [makecat](https://github.com/anonymix007/makecat); then `./build.sh`.

### Commands
```sh
./build.sh      # Mesa (fetched and built on first run), vgpu.sys, tests -> target/vgpu_accel.zip
```
On the first run `build.sh` creates a test-signing certificate in `certs/` (git-ignored) and signs
every later build with it, so VMs that trust it keep working. `CERT_DIR=...` uses another one,
`EWDK=...` another EWDK subset. Every build gets a new driver version (`2.1.<yyddd>.<HHMM>`, UTC)
so Windows replaces an installed older build. The zip also carries the license texts and `NOTICE.txt`.

The driver is compiled with clang-cl and linked with lld-link. Link flags that matter:
`/MERGE:.retplne=.rdata` (otherwise Code 39 / 0xC0000018), `/TSAWARE:NO`, `/SECTION:.data,RW`;
never `/SECTION:INIT,d`.

### Building the driver on Windows (optional)
With the WDK (10.0.26100, includes KMDF 1.27), Visual Studio 2022 Build Tools (C++ x64) and LLVM
installed, `build-win.ps1` builds `vgpu.sys` natively with the same flags and sources as `build.sh`,
stamps the INF with a rising UTC version, makes the catalog with Inf2Cat, and signs `vgpu.sys` and
`vgpu.cat` when you pass a code-signing certificate (`-CertThumbprint <sha1>`). Mesa and the tests are
still built on Linux. Step by step: [BUILD-WINDOWS.md](../BUILD-WINDOWS.md).

## Debugging
- Driver: DebugView with *Capture Kernel* shows `[vgpu]` lines. To see the per-call
  `VGPU_TRACE` lines, enable INFO for IHVDRIVER
  (`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter`, `IHVDRIVER` = 0xF).
- 32-bit apps go through WOW64; the IOCTL structs use fixed-width fields so both share one layout.
- Mesa: `set VIRGL_PRESENT_DEBUG=1`, then run the app from a console (output goes to stderr).
- Host: the QEMU/virglrenderer log. `Illegal command buffer` means the host poisoned the
  context, and every later command in it is ignored (black window).
- The device DACL comes from INF `HKR,,Security` in the .HW section and only applies after a
  disable/enable of the device.
- A device with a stale driver after an upgrade usually means the INF version did not change: each build
  stamps a new version, so rebuild instead of re-using an old package.

## Repository layout
```
Dockerfile         build environment          fetch-ewdk.sh   EWDK subset into ./ewdk
build.sh           one-command build          cross/          Mesa mingw cross files
build-win.ps1      native Windows build of vgpu.sys (optional)
BUILD-WINDOWS.md   step-by-step Windows build
driver/            vgpu.sys (KMDF): device.c, queue.c, resource.c, ioctl.c, portio.c,
                   vgpu_ioctl.h (ABI), virtio_gpu.h (protocol), vgpu.inf
mesa/              prepare.sh: fetches and configures Mesa
installer/         setup.bat, remove.bat, remove_viogpu.bat and the individual install scripts
tests/             vgputest, gltest, glspin
third_party/       virtio-win library (BSD-3, unmodified apart from its location)
docs/              this file and README images
```

## References
- https://github.com/tenclass/mvisor-win-vgpu-driver (the original driver the 1.x line was based on)
- https://gitlab.freedesktop.org/mesa/mesa, https://gitlab.freedesktop.org/virgl/virglrenderer
