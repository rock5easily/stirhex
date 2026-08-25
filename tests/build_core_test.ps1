# Build and run the core unit tests using cl.exe from the VS developer environment.
# Default arch is x64 (the port targets 64-bit). Pass x86 for a 32-bit build.
# The 2GB+ tests only run on x64 and need STIRLING_CORE_TEST_LARGE=1.
param([ValidateSet("x86","x64")][string]$Arch = "x64")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "bin"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# Locate Visual Studio via vswhere
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found: $vswhere" }
$vsPath = (& $vswhere -latest -property installationPath | Select-Object -First 1)
if (-not $vsPath) { throw "Visual Studio not found" }

# Enter the VS developer shell (sets INCLUDE/LIB/PATH for cl.exe)
$devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $devShell)) { throw "DevShell module not found: $devShell" }
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -no_logo" | Out-Null

$exe = Join-Path $outDir "core_test.exe"
$srcTest = Join-Path $root "core_test.cpp"
$srcList = Join-Path $root "..\StirHex\src\core\BlockList.cpp"
$srcCur  = Join-Path $root "..\StirHex\src\core\BlockCursor.cpp"
$srcIO   = Join-Path $root "..\StirHex\src\core\BlockFileIO.cpp"
# MFC/Win32 non-dependent settings codec (64-bit setting value format, Issue #22)
$srcCodec = Join-Path $root "..\StirHex\src\app\SettingsCodec.cpp"
# MFC non-dependent encoding migration for MBCS-era settings (Issue #43)
$srcMigrate = Join-Path $root "..\StirHex\src\app\SettingsMigration.cpp"
# CP932 <-> wide conversion helpers at the byte-layer boundary (Issue #41)
$srcCp932 = Join-Path $root "..\StirHex\src\core\Cp932Text.cpp"
# Charset-specific byte formatting for the struct bar (byte layer, Issue #42)
$srcCharConv = Join-Path $root "..\StirHex\src\core\CharConv.cpp"
# struct.def parser (array element count validation, Issue #46)
$srcStructDef = Join-Path $root "..\StirHex\src\core\StructDef.cpp"
$srcInc  = Join-Path $root "..\StirHex\src"
# ClipboardUtil.h (header only, Issue #47) needs the clipboard APIs from user32.lib

Write-Host "== build ($Arch) =="
& cl /nologo /utf-8 /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I "$srcInc" /Fe:"$exe" /Fo:"$outDir\" "$srcTest" "$srcList" "$srcCur" "$srcIO" "$srcCodec" "$srcMigrate" "$srcCp932" "$srcCharConv" "$srcStructDef" /link user32.lib
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

Write-Host "== run =="
& $exe
$runExit = $LASTEXITCODE
Write-Host "== exit code: $runExit =="
exit $runExit
