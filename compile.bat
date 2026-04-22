@echo off
setlocal

call "%~dp0_detect-toolchain.bat"
if errorlevel 1 exit /b 1

set "ROOT_DIR=%~dp0"
set "BIN_DIR=%ROOT_DIR%bin"
set "BUILD_DIR=%BIN_DIR%\gui-build"
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

%QMAKE_EXE% ..\..\src\DiskImager.pro "DESTDIR=.."
if errorlevel 1 exit /b 1

mingw32-make.exe
if errorlevel 1 exit /b 1

echo.
echo GUI build output: "%BIN_DIR%\Win32DiskImager.exe"
pause
