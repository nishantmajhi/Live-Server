@echo off
REM Run this from a "Developer Command Prompt for VS" so cl.exe/rc.exe are on PATH.

rc /nologo liveserver.rc
if %ERRORLEVEL% NEQ 0 (
    echo Resource compile failed.
    exit /b 1
)

cl /O2 liveserver.c http.c liveserver.res ws2_32.lib shell32.lib /Fe:liveserver.exe
if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b 1
)
echo Build succeeded: liveserver.exe
