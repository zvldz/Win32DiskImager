@echo off
setlocal
set "ROOT_DIR=%~dp0"
set "BIN_DIR=%ROOT_DIR%bin"
set "MSYS_MINGW_BIN=C:\msys64\mingw64\bin"
if exist "%MSYS_MINGW_BIN%\mingw32-make.exe" set "PATH=%MSYS_MINGW_BIN%;%PATH%"

for %%D in (gui-build cli-build gui-static-build) do (
  if exist "%BIN_DIR%\%%D\Makefile" (
    cd /d "%BIN_DIR%\%%D"
    mingw32-make.exe distclean
  )
)

for %%D in (gui-build cli-build gui-static-build generic imageformats networkinformation platforms styles tls translations) do (
  if exist "%BIN_DIR%\%%D" rmdir /s /q "%BIN_DIR%\%%D"
)

if exist "%BIN_DIR%" (
  del /f /q "%BIN_DIR%\*.exe" >nul 2>nul
  del /f /q "%BIN_DIR%\*.dll" >nul 2>nul
  del /f /q "%BIN_DIR%\*.qm" >nul 2>nul
)

del /f /q "%ROOT_DIR%src\lang\*.qm" >nul 2>nul
del /f /q "%ROOT_DIR%src\.qmake.stash" >nul 2>nul
pause
@echo on
