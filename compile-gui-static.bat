@echo off
setlocal

rem Build the GUI with a static Qt toolchain.
rem Usage:
rem   set QT_STATIC_BIN=C:\Qt\6.x.x\mingw_64_static\bin
rem   compile-gui-static.bat

if "%QT_STATIC_BIN%"=="" (
  echo ERROR: QT_STATIC_BIN is not set.
  echo Example: set QT_STATIC_BIN=C:\Qt\6.8.0\mingw_64_static\bin
  exit /b 1
)

if not exist "%QT_STATIC_BIN%\qmake6.exe" (
  echo ERROR: qmake6.exe not found in QT_STATIC_BIN: "%QT_STATIC_BIN%"
  exit /b 1
)

rem Pin Qt to the explicitly-requested static toolchain so the helper only
rem needs to resolve MinGW; auto-detection of a non-static Qt is suppressed.
set "QT_BIN=%QT_STATIC_BIN%"

call "%~dp0_detect-toolchain.bat"
if errorlevel 1 exit /b 1

set "ROOT_DIR=%~dp0"
set "BIN_DIR=%ROOT_DIR%bin"
set "BUILD_DIR=%BIN_DIR%\gui-static-build"

if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cd /d "%BUILD_DIR%"

set "QT_LIB_DIR="
for /f "usebackq delims=" %%L in (`qmake6.exe -query QT_INSTALL_LIBS`) do set "QT_LIB_DIR=%%L"

if "%QT_LIB_DIR%"=="" (
  echo ERROR: Could not query QT_INSTALL_LIBS from qmake.
  exit /b 1
)

if not exist "%QT_LIB_DIR%\libQt6Core.a" (
  echo ERROR: The selected Qt is not static.
  echo Missing static core lib: "%QT_LIB_DIR%\libQt6Core.a"
  exit /b 1
)

qmake6.exe ..\..\src\DiskImager.pro "DESTDIR=.."
if errorlevel 1 exit /b 1

mingw32-make.exe
if errorlevel 1 exit /b 1

echo.
echo Static GUI build output: "%BIN_DIR%\Win32DiskImager.exe"
pause
