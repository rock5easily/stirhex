# Build the standalone binary-patch CLI used by the external interop harness.
# This is a test adapter only; it is not linked into StirHex.exe.
param([ValidateSet("x86", "x64")][string]$Arch = "x64")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path (Join-Path $root "bin") $Arch
if (-not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere not found: $vswhere" }
$vsPath = (& $vswhere -latest -property installationPath | Select-Object -First 1)
if (-not $vsPath) { throw "Visual Studio not found" }

$devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path -LiteralPath $devShell)) { throw "DevShell module not found: $devShell" }
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=$Arch -no_logo" | Out-Null

$exe = Join-Path $outDir "binary_patch_cli.exe"
$srcCli = Join-Path $root "binary_patch_cli.cpp"
$srcBinaryPatch = Join-Path $root "..\..\StirHex\src\core\BinaryPatch.cpp"
$srcStream = Join-Path $root "..\..\StirHex\src\core\StreamFileWriter.cpp"
$srcInc = Join-Path $root "..\..\StirHex\src"
$manifest = Join-Path $root "binary_patch_cli.manifest"
if (-not (Test-Path -LiteralPath $manifest)) { throw "manifest not found: $manifest" }

Write-Host "== build binary_patch_cli ($Arch) =="
& cl /nologo /utf-8 /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I "$srcInc" /Fe:"$exe" /Fo:"$outDir\" "$srcCli" "$srcBinaryPatch" "$srcStream" /link /MANIFEST:EMBED /MANIFESTINPUT:"$manifest" bcrypt.lib user32.lib shell32.lib ole32.lib advapi32.lib
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
Write-Host "CLI: $exe"
exit 0
