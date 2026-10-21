@echo off
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator.
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)
echo Removing vgpu-accel driver packages...
powershell -NoProfile -Command "Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\vgpu.inf' } | ForEach-Object { Write-Host 'Removing' $_.Driver; pnputil /delete-driver $_.Driver /uninstall /force }"
if not defined VGPU_NOPAUSE pause
