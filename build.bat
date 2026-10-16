@echo off
setlocal EnableDelayedExpansion

REM Always run from the script directory.
cd /d "%~dp0"

REM Ensure Visual Studio environment is initialized if running from normal shell
if "%INCLUDE%"=="" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_DIR=%%i"
    )
    if exist "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat" (
        echo [INFO] Initializing Visual Studio environment from !VS_DIR!...
        call "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"
    )
    if exist "!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin" (
        set "PATH=!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
    )
)

REM ---------------------------------------------------------------------------
REM Configurable build options
REM   GENERATOR  - CMake generator (default: auto-detect)
REM   ARCH       - Architecture for multi-config generators (default: x64)
REM   CONFIGS    - Configurations to build, space-separated (default: Release)
REM ---------------------------------------------------------------------------
if "%GENERATOR%"=="" (
    where ninja >nul 2>nul
    if not errorlevel 1 (
        set "GENERATOR=Ninja Multi-Config"
    ) else (
        set "GENERATOR=Visual Studio 17 2022"
    )
)
if "%ARCH%"=="" set "ARCH=x64"
if "%CONFIGS%"=="" set "CONFIGS=Release"

echo [INFO] Configuring with generator: %GENERATOR%
echo "%GENERATOR%" | findstr /I /C:"Visual Studio" >nul
if not errorlevel 1 (
    cmake -S src -B build -G "%GENERATOR%" -A %ARCH% -DOST_ENABLE_UPDATER=OFF
) else (
    cmake -S src -B build -G "%GENERATOR%" -DOST_ENABLE_UPDATER=OFF
)
if errorlevel 1 goto :fail

for %%C in (%CONFIGS%) do (
    echo [INFO] Building core OpenSteamTool for %%C
    cmake --build build --config %%C
    if errorlevel 1 goto :fail

    echo [INFO] Building tool ost-manager for %%C
    cmake --build build --config %%C --target ost-manager
    if errorlevel 1 goto :fail

    echo [INFO] Building tool extract_tickets for %%C
    cmake --build build --config %%C --target extract_tickets
    if errorlevel 1 goto :fail
)

echo [OK] Build completed successfully.
exit /b 0

:fail
echo [ERROR] Build failed.
exit /b 1
