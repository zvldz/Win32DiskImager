@echo off
setlocal
set "MSYS_MINGW_BIN=C:\msys64\mingw64\bin"
if exist "%MSYS_MINGW_BIN%\qmake.exe" set "PATH=%MSYS_MINGW_BIN%;%PATH%"
set "ROOT_DIR=%~dp0"
set "BIN_DIR=%ROOT_DIR%bin"
set "BUILD_DIR=%BIN_DIR%\gui-build"
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"
if exist "%MSYS_MINGW_BIN%\qmake6.exe" (
  qmake6.exe ..\..\src\DiskImager.pro "DESTDIR=.."
) else if exist "%MSYS_MINGW_BIN%\qmake.exe" (
  qmake.exe ..\..\src\DiskImager.pro "DESTDIR=.."
) else if exist "%MSYS_MINGW_BIN%\qmake-qt5.exe" (
  qmake-qt5.exe ..\..\src\DiskImager.pro "DESTDIR=.."
) else (
  qmake.exe ..\..\src\DiskImager.pro "DESTDIR=.."
)
mingw32-make.exe
echo.
echo GUI build output: "%BIN_DIR%\Win32DiskImager.exe"
pause
@echo on
