@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%A in ("%SCRIPT_DIR%..\\..") do set "ROOT=%%~fA"
set "BUILD_DIR=%ROOT%\\build_qt"
set "CONFIG=Release"

if /i "%~1"=="Debug" set "CONFIG=Debug"
if /i "%~1"=="RelWithDebInfo" set "CONFIG=RelWithDebInfo"
if /i "%~1"=="MinSizeRel" set "CONFIG=MinSizeRel"

set "QT_ROOT="
if exist "%BUILD_DIR%\\CMakeCache.txt" (
    for /f "tokens=2 delims==" %%A in ('findstr /b /c:"CMAKE_PREFIX_PATH:UNINITIALIZED=" "%BUILD_DIR%\\CMakeCache.txt"') do set "QT_ROOT=%%A"
)
if not defined QT_ROOT set "QT_ROOT=C:\\Qt\\6.10.1\\msvc2022_64"

set "QT_BIN=%QT_ROOT%\\bin"
set "QT_PLUGIN_PATH=%QT_ROOT%\\plugins"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%QT_ROOT%\\plugins\\platforms"
set "VCPKG_BIN=C:\\vcpkg\\installed\\x64-windows\\bin"
set "TARGET_DIR=%BUILD_DIR%\\example\\qt_multi_cursor\\%CONFIG%"
set "COORD=%TARGET_DIR%\\sintra_example_qt_multi_cursor_coordinator.exe"

if not exist "%COORD%" (
    echo Coordinator exe not found: "%COORD%"
    exit /b 1
)

set "PATH=%QT_BIN%;%VCPKG_BIN%;%PATH%"
if /i "%CONFIG%"=="Debug" (
    set "PATH=C:\\vcpkg\\installed\\x64-windows\\debug\\bin;%PATH%"
)

start "" "%COORD%"
