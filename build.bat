@echo off
rem ===========================================================================
rem StirHex - common build script
rem   usage: build.bat [Configuration] [Platform]
rem     Configuration : Debug | Release   (default Debug)
rem     Platform      : Win32 | x64        (default x64)
rem   Called by build_debug.bat / build_release.bat. Can also be run directly.
rem   MSBuild is auto-detected via vswhere (VS path is not hard-coded).
rem   NOTE: keep this file ASCII-only. cmd.exe mis-parses UTF-8 non-ASCII bytes.
rem ===========================================================================
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found: "%VSWHERE%"
    echo         Visual Studio 2017 or later is required.
    exit /b 1
)

set "MSBUILD="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo [ERROR] MSBuild.exe not found via vswhere.
    exit /b 1
)

set "PROJECT=%~dp0StirHex\StirHex.vcxproj"
if not exist "%PROJECT%" (
    echo [ERROR] Project not found: "%PROJECT%"
    exit /b 1
)

echo === Building StirHex [%CONFIG% ^| %PLATFORM%] ===
echo MSBuild: "%MSBUILD%"
"%MSBUILD%" "%PROJECT%" /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /t:Build /m /nologo /v:minimal
set "RC=%ERRORLEVEL%"

if "%RC%"=="0" (
    echo === Build succeeded [%CONFIG% ^| %PLATFORM%] ===
) else (
    echo === Build FAILED [%CONFIG% ^| %PLATFORM%] exit code %RC% ===
)
exit /b %RC%
