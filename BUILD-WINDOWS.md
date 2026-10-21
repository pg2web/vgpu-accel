# Building vgpu.sys on Windows

`build-win.ps1` builds the kernel driver natively with the same sources and flags as the Linux
`build.sh` (clang-cl and lld-link against the real WDK). Mesa (the OpenGL DLLs) and the test programs
are still built on Linux, see the main [README](README.md); this guide covers the driver only.

Tested on: Windows 11 24H2 (build 26100), WDK 10.0.26100.0 (KMDF 1.27), Visual Studio 2022 Build Tools
17.14 (MSVC 14.44), LLVM 23.1.4. The native `vgpu.sys` is about 55 KB.

## 1. Requirements
Install, in an elevated PowerShell, then open a **new** shell (Git is added to PATH; the LLVM installer does
not add `clang-cl`, which is fine because the script calls it by its full path):

```powershell
winget install --id Git.Git
winget install --id LLVM.LLVM
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```
- **WDK 10.0.26100** from Microsoft (https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk),
  matching the Windows SDK of the same version. Check that these folders exist:
  `C:\Program Files (x86)\Windows Kits\10\Include\wdf\kmdf\1.27` and
  `C:\Program Files (x86)\Windows Kits\10\Lib\wdf\kmdf\x64\1.27`.
- The script also uses `Inf2Cat.exe` (catalog) and `signtool.exe` (signing) from
  `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0`. `Inf2Cat` is part of the WDK, so a missing one means
  the WDK is not installed.
- The WDK and Build Tools are Microsoft software under their own licenses. Do not copy them into the repo.

## 2. Build
```powershell
git clone <repo-url> vgpu-accel
cd vgpu-accel
.\build-win.ps1
```
Output: `target\build\vgpu.sys` (plus `.map`) and an unsigned package in `target\pkg`
(`vgpu.sys`, `vgpu.inf`, `vgpu.cat`). The INF gets a rising UTC version, `2.1.<yy><ddd>.<HHmm>`, so
every build replaces the previous one. Windows keeps an installed driver with the same or a higher version.

Parameters if your layout differs: `-Kits`, `-KitVer`, `-Msvc` (the script looks under
`...\2022\BuildTools`; pass the MSVC folder for Community or Professional editions), `-Llvm`.

## 3. Sign with your own test certificate
Test signing needs **Secure Boot off**. The certificate below is only for your own machines.

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=vgpu-accel local test signing" `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable
Export-Certificate -Cert $cert -FilePath vgpu-test.cer
.\build-win.ps1 -CertThumbprint $cert.Thumbprint     # signs vgpu.sys and vgpu.cat in target\pkg
```
Trust the public certificate on the machine that installs the driver (elevated):
```powershell
Import-Certificate -FilePath vgpu-test.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath vgpu-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```
Keep the private key where it is. Never copy it or commit it. Remove the two trust entries when you no
longer need them.

## 4. Install and test
```powershell
bcdedit /set testsigning on        # then reboot (once)
installer\remove_viogpu.bat        # removes Red Hat's viogpudo, which claims the same PCI ID
pnputil /add-driver target\pkg\vgpu.inf /install
pnputil /enum-devices /deviceid "PCI\VEN_1AF4&DEV_1050" /drivers   # device Started, new version
```
`remove_viogpu.bat` does nothing if no Red Hat driver is installed. The installer scripts wait for a key at the
end; set `VGPU_NOPAUSE=1` (`$env:VGPU_NOPAUSE=1`) to skip the wait when running them from a script.

Then run `vgputest.exe` from the Linux-built package (`vgpu_accel.zip`): it should end with
`ALL PASSED`, and with QEMU `blob=true,hostmem=...` also `RESOURCE_BLOB = 1, HOST_VISIBLE = 1`.
`gltest.exe` and `gltest32.exe` from the same package check OpenGL (the renderer must be virgl).
The VM needs a `virtio-gpu-gl` device, see the README for the QEMU setup.

## Troubleshooting
- **`clang-cl` not found:** the script looks in `C:\Program Files\LLVM\bin` (the LLVM installer does not put it on
  PATH). If LLVM is installed elsewhere, pass `-Llvm "<folder with clang-cl.exe>"`.
- **`Inf2Cat` or `signtool` not found:** install the WDK (see Requirements) and check `-Kits` and `-KitVer`.
- **MSVC include folder not found:** the Build Tools are not under `...\2022\BuildTools`; pass `-Msvc`.
- **Header or `.lib` not found:** the WDK and Windows SDK versions must both be 10.0.26100.0, with the
  KMDF 1.27 folders listed above.
- **Driver install says it is not signed or the publisher is untrusted:** test signing is off (reboot
  needed), Secure Boot is on, or the certificate is not in both Root and TrustedPublisher.
- **The old driver stays active:** another driver (viogpudo, or a higher version) wins; run
  `remove_viogpu.bat` and check the version in the `pnputil` output.
- **Where is the loaded file?** KMDF runs the driver from the DriverStore, not from `System32\drivers`.
  To check it is your build, compare the SHA-256 of the DriverStore `vgpu.sys` with `target\pkg\vgpu.sys`.
