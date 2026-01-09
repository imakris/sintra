@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%A in ("%SCRIPT_DIR%..\\..") do set "ROOT=%%~fA"
set "BUILD_DIR=%ROOT%\\build_qt"
set "CONFIG=Release"

if /i "%~1"=="Debug" set "CONFIG=Debug"
if /i "%~1"=="RelWithDebInfo" set "CONFIG=RelWithDebInfo"
if /i "%~1"=="MinSizeRel" set "CONFIG=MinSizeRel"

set "VcpkgXUseBuiltInApplocalDeps=true"

cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target ^
    sintra_example_qt_multi_cursor_coordinator ^
    sintra_example_qt_multi_cursor_window
