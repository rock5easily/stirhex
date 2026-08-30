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
# Temp-file-then-replace streaming writer for range/dump save (Issue #155)
$srcStream = Join-Path $root "..\StirHex\src\core\StreamFileWriter.cpp"
# MFC/Win32 non-dependent settings codec (64-bit setting value format, Issue #22)
$srcCodec = Join-Path $root "..\StirHex\src\app\SettingsCodec.cpp"
# MFC non-dependent encoding migration for MBCS-era settings (Issue #43)
$srcMigrate = Join-Path $root "..\StirHex\src\app\SettingsMigration.cpp"
# Settings store: INI serialization and UTF-8 codec for the settings file (Issue #96)
$srcStore = Join-Path $root "..\StirHex\src\app\SettingsStore.cpp"
# Settings file I/O: cross-process merged save of the settings file (Issue #130)
$srcSettingsFile = Join-Path $root "..\StirHex\src\app\SettingsFile.cpp"
$srcMarkFile = Join-Path $root "..\StirHex\src\app\MarkFile.cpp"
# CP932 <-> wide conversion helpers at the byte-layer boundary (Issue #41)
$srcCp932 = Join-Path $root "..\StirHex\src\core\Cp932Text.cpp"
# Charset-specific byte formatting for the struct bar (byte layer, Issue #42)
$srcCharConv = Join-Path $root "..\StirHex\src\core\CharConv.cpp"
# struct.def parser (array element count validation, Issue #46)
$srcStructDef = Join-Path $root "..\StirHex\src\core\StructDef.cpp"
# Lenient hex-text parser for the paste-as-hex command (Issue #97)
$srcHexText = Join-Path $root "..\StirHex\src\core\HexText.cpp"
# UTF-8 decode/encode and byte-to-cell mapping for the UTF-8 charset (Issue #98)
$srcUtf8Text = Join-Path $root "..\StirHex\src\core\Utf8Text.cpp"
$srcInc  = Join-Path $root "..\StirHex\src"
# ClipboardUtil.h (header only, Issue #47) needs the clipboard APIs from user32.lib

Write-Host "== build ($Arch) =="
& cl /nologo /utf-8 /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /DSTIRLING_TEST_ALLOC_HOOK /I "$srcInc" /Fe:"$exe" /Fo:"$outDir\" "$srcTest" "$srcList" "$srcCur" "$srcIO" "$srcStream" "$srcCodec" "$srcMigrate" "$srcStore" "$srcSettingsFile" "$srcMarkFile" "$srcCp932" "$srcCharConv" "$srcStructDef" "$srcHexText" "$srcUtf8Text" /link user32.lib shell32.lib ole32.lib advapi32.lib
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

Write-Host "== run =="
& $exe
$runExit = $LASTEXITCODE
Write-Host "== exit code: $runExit =="
exit $runExit

