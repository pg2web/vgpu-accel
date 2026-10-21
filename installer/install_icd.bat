@echo off
setlocal
cd /d "%~dp0"
echo === vgpu-accel: register virgl OpenGL driver system-wide (64-bit and 32-bit apps) ===
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator.
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)
if not exist "%~dp0vio_ogl_x64.dll" (
    echo [ERROR] vio_ogl_x64.dll not found next to this script.
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)
if not exist "%~dp0vio_ogl_x86.dll" (
    echo [ERROR] vio_ogl_x86.dll not found next to this script.
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)

set KEY64=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL
set KEY32=HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL
set BACKUP=%ProgramData%\vgpu-accel
if not exist "%BACKUP%" mkdir "%BACKUP%"

echo [1/3] Backing up existing MSOGL entries to %BACKUP%
call :backup "%KEY64%" MSOGL
call :backup "%KEY32%" MSOGL32

echo [2/3] Copying the DLLs to System32 (64-bit) and SysWOW64 (32-bit)
copy /y "%~dp0vio_ogl_x64.dll" "%SystemRoot%\System32\vio_ogl_x64.dll" >nul || goto :copyfail
copy /y "%~dp0vio_ogl_x86.dll" "%SystemRoot%\SysWOW64\vio_ogl_x86.dll" >nul || goto :copyfail

echo [3/3] Registering as the default OpenGL ICD (MSOGL)
call :register "%KEY64%" vio_ogl_x64.dll
call :register "%KEY32%" vio_ogl_x86.dll

echo.
echo Done. OpenGL apps without their own GPU driver (RDP, VGA, QXL, Basic Display)
echo now use virgl; without the vgpu device they fall back to softpipe.
echo Undo with uninstall_icd.bat.
if not defined VGPU_NOPAUSE pause
exit /b 0

:copyfail
echo [ERROR] Copy failed - close all OpenGL applications and retry.
if not defined VGPU_NOPAUSE pause
exit /b 1

:backup
if exist "%BACKUP%\%~2-backup.reg" exit /b 0
if exist "%BACKUP%\%~2-none.txt" exit /b 0
reg query %1 >nul 2>&1 && (reg export %1 "%BACKUP%\%~2-backup.reg" /y >nul) || (echo none> "%BACKUP%\%~2-none.txt")
exit /b 0

:register
reg add %1 /v DLL /t REG_SZ /d %2 /f >nul
reg add %1 /v DriverVersion /t REG_DWORD /d 1 /f >nul
reg add %1 /v Flags /t REG_DWORD /d 1 /f >nul
reg add %1 /v Version /t REG_DWORD /d 2 /f >nul
exit /b 0
