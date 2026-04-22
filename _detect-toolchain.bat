@echo off
rem Detect Qt qmake and MinGW toolchain; set PATH and QMAKE_EXE for the caller.
rem
rem Callers must `call` this file after their own `setlocal`, so env changes
rem are scoped to the caller's script. Exits /b 1 with a message on failure.
rem
rem Overrides (take priority over auto-detection):
rem   QT_BIN      directory containing qmake.exe or qmake6.exe
rem   MINGW_BIN   directory containing mingw32-make.exe / g++.exe
rem
rem Auto-detection scan order (when overrides are not set):
rem   1. MSYS2:               C:\msys64\mingw64\bin
rem   2. Qt online installer: C:\Qt\<ver>\mingw*_64\bin  (qmake)
rem                           C:\Qt\Tools\mingw*_64\bin (mingw32-make)
rem   3. Whatever is already on PATH

rem -- MSYS2 (single dir carries both toolchains) --
if not defined QT_BIN    if exist "C:\msys64\mingw64\bin\qmake.exe"         set "QT_BIN=C:\msys64\mingw64\bin"
if not defined QT_BIN    if exist "C:\msys64\mingw64\bin\qmake6.exe"        set "QT_BIN=C:\msys64\mingw64\bin"
if not defined MINGW_BIN if exist "C:\msys64\mingw64\bin\mingw32-make.exe"  set "MINGW_BIN=C:\msys64\mingw64\bin"

rem -- Qt online installer: C:\Qt\<ver>\mingw*_64\bin --
if not defined QT_BIN (
  for /d %%V in ("C:\Qt\*") do (
    for /d %%M in ("%%~V\mingw*_64") do (
      if not defined QT_BIN if exist "%%~M\bin\qmake.exe"  set "QT_BIN=%%~M\bin"
      if not defined QT_BIN if exist "%%~M\bin\qmake6.exe" set "QT_BIN=%%~M\bin"
    )
  )
)

rem -- Qt Tools MinGW: C:\Qt\Tools\mingw*_64\bin --
if not defined MINGW_BIN (
  for /d %%M in ("C:\Qt\Tools\mingw*_64") do (
    if not defined MINGW_BIN if exist "%%~M\bin\mingw32-make.exe" set "MINGW_BIN=%%~M\bin"
  )
)

rem -- Prepend to PATH (dedup if both point to the same dir) --
if defined QT_BIN set "PATH=%QT_BIN%;%PATH%"
if defined MINGW_BIN if /i not "%MINGW_BIN%"=="%QT_BIN%" set "PATH=%MINGW_BIN%;%PATH%"

rem -- Pick qmake flavor (prefer Qt 6) --
set "QMAKE_EXE="
where qmake6     >nul 2>nul && set "QMAKE_EXE=qmake6.exe"
if not defined QMAKE_EXE ( where qmake     >nul 2>nul && set "QMAKE_EXE=qmake.exe" )
if not defined QMAKE_EXE ( where qmake-qt5 >nul 2>nul && set "QMAKE_EXE=qmake-qt5.exe" )

rem -- Verify --
if not defined QMAKE_EXE (
  echo [error] qmake not found on PATH.
  echo   Install Qt ^(msys2: mingw-w64-x86_64-qt6-base, or Qt online installer^),
  echo   or set QT_BIN to your Qt "bin" directory, for example:
  echo     set QT_BIN=C:\Qt\6.8.0\mingw_64\bin
  exit /b 1
)

where mingw32-make >nul 2>nul
if errorlevel 1 (
  echo [error] mingw32-make not found on PATH.
  echo   Install MinGW-w64 ^(msys2 or Qt Tools^),
  echo   or set MINGW_BIN to your MinGW "bin" directory, for example:
  echo     set MINGW_BIN=C:\Qt\Tools\mingw1310_64\bin
  exit /b 1
)

exit /b 0
