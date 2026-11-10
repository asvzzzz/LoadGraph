@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo === LoadGraph GUI — build (MSVC x64, Release) ===

if not exist "build" mkdir build
cmake -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo CMake configuration failed.
    exit /b 1
)

cmake --build build --config Release --target LoadGraphGUI
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo === Done. Binary in build\bin\Release\LoadGraphGUI.exe ===
