@echo off
REM Builds and runs the request-handling tests with MinGW-w64 (gcc on PATH).
REM Run from the repository root:  tests\build.bat

gcc -std=c11 -Wall -Wextra -o tests\http_test.exe tests\http_test.c src\http.c
if %ERRORLEVEL% NEQ 0 (
    echo Test build failed.
    exit /b 1
)

tests\http_test.exe
exit /b %ERRORLEVEL%
