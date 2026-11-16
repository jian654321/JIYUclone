@echo off
rem ClassCtrl build script - compile only, never run the produced exes
cd /d "%~dp0"
if not exist dist mkdir dist

echo [1/2] building dist\Teacher.exe ...
gcc -O2 -std=gnu99 -Wall -Wno-unused-parameter -mwindows -static ^
    -o dist\Teacher.exe src\teacher.c ^
    -lws2_32 -lgdi32 -lcomdlg32 -luser32 -lshell32 -ldwmapi -liphlpapi -ladvapi32
if errorlevel 1 goto :err

echo [2/2] building dist\Student.exe ...
gcc -O2 -std=gnu99 -Wall -Wno-unused-parameter -mwindows -static ^
    -o dist\Student.exe src\student.c ^
    -lws2_32 -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32
if errorlevel 1 goto :err

echo.
echo BUILD OK - dist\Teacher.exe  dist\Student.exe
exit /b 0

:err
echo.
echo BUILD FAILED
exit /b 1
