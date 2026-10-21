@echo off
setlocal
rem Remove Red Hat's virtio-gpu display driver (virtio-win viogpudo / viogpu*) from the driver store.
rem It matches the same PCI ID (VEN_1AF4 DEV_1050) and would otherwise claim the device instead
rem of vgpu-accel, or take it back after a reboot or Windows Update.
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator.
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)
echo === Checking for the virtio-win GPU display driver (viogpudo)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$pkgs = @(Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match '\\viogpu[^\\]*\.inf$' });" ^
  "if ($pkgs.Count -eq 0) { Write-Host 'Not installed, nothing to do.'; exit 0 }" ^
  "$dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' -and $_.Class -eq 'Display' };" ^
  "if ($dev) { Write-Host '[!] It currently drives a display adapter:' ($dev.FriendlyName -join ', '); Write-Host '    That display falls back to the Microsoft Basic Display Adapter. vgpu-accel expects the VM display on -vga std or QXL.' }" ^
  "foreach ($p in $pkgs) { Write-Host 'Removing' $p.Driver '(' (Split-Path $p.OriginalFileName -Leaf) $p.Version ')'; pnputil /delete-driver $p.Driver /uninstall /force | Out-Null }" ^
  "pnputil /scan-devices | Out-Null; Write-Host 'Done.'"
if not defined VGPU_NOPAUSE pause
