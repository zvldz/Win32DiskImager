@echo off
setlocal
set "MSYS_MINGW_BIN=C:\msys64\mingw64\bin"
if exist "%MSYS_MINGW_BIN%\qmake.exe" set "PATH=%MSYS_MINGW_BIN%;%PATH%"
set "ROOT_DIR=%~dp0"
set "BIN_DIR=%ROOT_DIR%bin"
set "BUILD_DIR=%BIN_DIR%\cli-build"
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"
if exist "%MSYS_MINGW_BIN%\qmake6.exe" (
  qmake6.exe ..\..\src\DiskImagerCli.pro "DESTDIR=.."
) else if exist "%MSYS_MINGW_BIN%\qmake.exe" (
  qmake.exe ..\..\src\DiskImagerCli.pro "DESTDIR=.."
) else if exist "%MSYS_MINGW_BIN%\qmake-qt5.exe" (
  qmake-qt5.exe ..\..\src\DiskImagerCli.pro "DESTDIR=.."
) else (
  qmake.exe ..\..\src\DiskImagerCli.pro "DESTDIR=.."
)
mingw32-make.exe
echo.
echo CLI build output: "%BIN_DIR%\Win32DiskImager-cli.exe"
pause
@echo on
