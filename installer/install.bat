@echo off
setlocal
cd /d "%~dp0"
echo === vgpu-accel: VirtIO GPU 3D Accelerator (virgl) installer ===

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator.
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)

bcdedit /enum {current} | findstr /i "testsigning.*Yes" >nul
if %errorlevel% neq 0 (
    echo [!] Test signing is OFF. Enabling it - REBOOT and run install.bat again.
    bcdedit /set testsigning on
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)

echo [1/3] Trusting test certificate...
certutil -addstore -f Root "%~dp0vgpu_test_signing.cer" >nul
certutil -addstore -f TrustedPublisher "%~dp0vgpu_test_signing.cer" >nul

echo [2/3] Installing driver package...
pnputil /add-driver "%~dp0vgpu.inf" /install
if %errorlevel% neq 0 (
    echo [!] pnputil returned %errorlevel%. If viogpudo owns the device, remove it first
    echo     or use Device Manager ^> Update driver ^> Have disk on the VirtIO GPU device.
)

echo [3/3] Device status:
pnputil /enum-devices /deviceid "PCI\VEN_1AF4&DEV_1050" /drivers
echo.
echo Now run vgputest.exe (DebugView with "Capture Kernel" shows [vgpu] driver logs).
if not defined VGPU_NOPAUSE pause
