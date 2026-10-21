# Native Windows build of vgpu.sys (clang-cl + lld-link), same flags as build.sh.
param(
    [string]$Kits = "${env:ProgramFiles(x86)}\Windows Kits\10",
    [string]$KitVer = "10.0.26100.0",
    [string]$Msvc = "",
    [string]$Llvm = "$env:ProgramFiles\LLVM\bin",
    [string]$CertThumbprint = ""
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$drv = "$root\driver"
$vio = "$root\third_party\virtio-win"
$out = "$root\target\build"

if (-not $Msvc) {
    $Msvc = (Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" |
        Sort-Object Name | Select-Object -Last 1).FullName
}
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null

$inc = "$Kits\Include\$KitVer"
$cflags = @(
    '--target=x86_64-pc-windows-msvc', '-fms-compatibility', '-fms-extensions',
    '/c', '/O2', '/GS-', '/Gy', '/Zl', '/W3', '-Wno-everything',
    "-I$Msvc\include", "-I$inc\ucrt", "-I$inc\km\crt", "-I$inc\km", "-I$inc\shared",
    "-I$Kits\Include\wdf\kmdf\1.27",
    "-I$drv", "-I$vio", "-I$vio\WDF",
    '-D_AMD64_', '-DAMD64', '-D_WIN64', '-D_KERNEL_MODE', '-DNDEBUG',
    '-DNTDDI_VERSION=0x0A000008', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00',
    '-DKMDF_VERSION_MAJOR=1', '-DKMDF_VERSION_MINOR=27', '-DPOOL_NX_OPTIN=1'
)
$sources = @(
    "$drv\device.c", "$drv\queue.c", "$drv\resource.c", "$drv\ioctl.c", "$drv\portio.c",
    "$vio\VirtIOPCICommon.c", "$vio\VirtIOPCILegacy.c", "$vio\VirtIOPCIModern.c",
    "$vio\VirtIORing.c", "$vio\VirtIORing-Packed.c",
    "$vio\WDF\VirtIOWdf.c", "$vio\WDF\PCI.c", "$vio\WDF\MemPortIO.c", "$vio\WDF\Callbacks.c", "$vio\WDF\Dma.c"
)

Write-Host "=== Compiling vgpu.sys"
$objs = @()
foreach ($s in $sources) {
    $o = "$out\" + [IO.Path]::GetFileNameWithoutExtension($s) + ".obj"
    & "$Llvm\clang-cl.exe" @cflags $s "/Fo$o"
    if ($LASTEXITCODE) { throw "compile failed: $s" }
    $objs += $o
}

Write-Host "=== Linking vgpu.sys"
& "$Llvm\lld-link.exe" /NOLOGO "/OUT:$out\vgpu.sys" /DRIVER /SUBSYSTEM:NATIVE,10.00 /OSVERSION:10.00 `
    /ENTRY:FxDriverEntry /NODEFAULTLIB /DYNAMICBASE /NXCOMPAT /OPT:REF /OPT:ICF `
    /TSAWARE:NO /MERGE:.retplne=.rdata /SECTION:.data,RW "/MAP:$out\vgpu.map" `
    "/LIBPATH:$Kits\Lib\$KitVer\km\x64" "/LIBPATH:$Kits\Lib\wdf\kmdf\x64\1.27" `
    wdfdriverentry.lib wdfldr.lib ntoskrnl.lib hal.lib wdmguid.lib libcntpr.lib bufferoverflowfastfailk.lib `
    @objs
if ($LASTEXITCODE) { throw "link failed" }
Write-Host ("built {0} ({1} bytes)" -f "$out\vgpu.sys", (Get-Item "$out\vgpu.sys").Length)

# Package: stamp the INF (UTC, like build.sh), make the catalog, sign when a certificate thumbprint is given.
$pkg = "$root\target\pkg"
New-Item -ItemType Directory -Force $pkg | Out-Null
foreach ($f in 'vgpu.sys', 'vgpu.inf', 'vgpu.cat') { Remove-Item "$pkg\$f" -Force -ErrorAction SilentlyContinue }
Copy-Item "$out\vgpu.sys" $pkg
$now = (Get-Date).ToUniversalTime()
$ver = '2.1.' + $now.ToString('yy') + $now.DayOfYear.ToString('000') + '.' + $now.ToString('HHmm')
(Get-Content "$root\driver\vgpu.inf" -Raw) -replace '%%BUILDDATE%%', $now.ToString('MM/dd/yyyy') -replace '%%BUILDVERSION%%', $ver |
    Set-Content "$pkg\vgpu.inf" -Encoding ascii
$kitBin = "$Kits\bin\$KitVer"
& "$kitBin\x86\Inf2Cat.exe" "/driver:$pkg" /os:10_X64 | Out-Null
if ($LASTEXITCODE) { throw "Inf2Cat failed" }
if ($CertThumbprint) {
    & "$kitBin\x64\signtool.exe" sign /sha1 $CertThumbprint /fd sha256 "$pkg\vgpu.sys" "$pkg\vgpu.cat" | Out-Null
    if ($LASTEXITCODE) { throw "signtool failed" }
}
Write-Host "package $pkg version $ver"
