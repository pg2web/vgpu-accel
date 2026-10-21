@echo off
setlocal
echo === vgpu-accel: unregister virgl OpenGL driver ===
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Run as Administrator.
    if not defined VGPU_NOPAUSE pause
    exit /b 1
)
set BACKUP=%ProgramData%\vgpu-accel
if not exist "%BACKUP%" if exist "%ProgramData%\vgpu-rdp" set BACKUP=%ProgramData%\vgpu-rdp

call :restore "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL" MSOGL 64-bit
call :restore "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\MSOGL" MSOGL32 32-bit
call :remove "%SystemRoot%\System32\vio_ogl_x64.dll"
call :remove "%SystemRoot%\SysWOW64\vio_ogl_x86.dll"
if not defined VGPU_NOPAUSE pause
exit /b 0

:restore
reg delete %1 /f >nul 2>&1
if exist "%BACKUP%\%~2-backup.reg" (
    reg import "%BACKUP%\%~2-backup.reg" >nul && echo Restored the original %3 MSOGL entry.
    del "%BACKUP%\%~2-backup.reg"
) else (
    echo %3: MSOGL entry removed, Windows uses its built-in software OpenGL.
)
if exist "%BACKUP%\%~2-none.txt" del "%BACKUP%\%~2-none.txt"
exit /b 0

:remove
if not exist %1 exit /b 0
del /f %1 >nul 2>&1
if exist %1 (
    echo %~nx1 is in use; it will stay until the apps using it exit. Run this again later to delete it.
) else (
    echo Removed %~1
)
exit /b 0
