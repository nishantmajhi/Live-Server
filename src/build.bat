@echo off
REM Builds liveserver.exe using MinGW-w64 (gcc/windres must be on PATH).
REM If you use MSVC instead, see build_msvc.bat.

windres liveserver.rc -O coff -o liveserver_res.o
if %ERRORLEVEL% NEQ 0 (
    echo Resource compile failed.
    exit /b 1
)

gcc -O2 -o liveserver.exe liveserver.c http.c liveserver_res.o -lws2_32 -lshell32
if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b 1
)
echo Build succeeded: liveserver.exe
