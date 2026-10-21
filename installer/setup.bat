@echo off
setlocal
cd /d "%~dp0"
echo === vgpu-accel setup: driver + system-wide OpenGL ===
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator.
    pause
    exit /b 1
)
set VGPU_NOPAUSE=1

call "%~dp0remove_viogpu.bat"

call "%~dp0install.bat"
if %errorlevel% neq 0 (
    echo [!] Driver step did not finish ^(reboot needed for test signing?^). Run setup.bat again afterwards.
    pause
    exit /b 1
)

call "%~dp0system-icd\install_icd.bat"


echo.
echo === Smoke test
"%~dp0system-icd\test\gltest.exe"
"%~dp0system-icd\test\gltest32.exe"
echo.
echo Setup finished.
echo Undo everything with remove.bat.
pause
