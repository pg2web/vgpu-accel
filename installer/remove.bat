@echo off
setlocal
cd /d "%~dp0"
echo === vgpu-accel: remove driver, OpenGL registration ===
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator.
    pause
    exit /b 1
)
set VGPU_NOPAUSE=1
call "%~dp0system-icd\uninstall_icd.bat"
call "%~dp0uninstall.bat"
echo.
echo Done. Reboot to finish removing the driver.
pause
