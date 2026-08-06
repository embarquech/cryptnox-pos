@echo off
REM ponytail: thin wrapper so idf.py runs with its env (won't run in Git Bash).
REM Usage: idf-build-flash.bat [PORT]   (default COM3). Flash only, no monitor.
setlocal
set "MSYSTEM="
set PORT=%1
if "%PORT%"=="" set PORT=COM3
call C:\esp\v5.5.4\esp-idf\export.bat || exit /b 1
cd /d %~dp0..
idf.py -p %PORT% build flash
