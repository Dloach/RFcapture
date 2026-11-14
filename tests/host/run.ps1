# Build and run the host side decoder tests.
#
# Works with any C++17 compiler.  The script looks, in this order, for:
#   1. -Compiler <path> argument
#   2. zig on PATH, or tools/zig/zig.exe inside the project
#   3. g++ / clang++ on PATH
#
# Usage:
#   .\tests\host\run.ps1
#   .\tests\host\run.ps1 -Compiler "C:\tools\zig\zig.exe"

param(
    [string]$Compiler = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root

$out = Join-Path $root "tests\host\test_decoder.exe"
$sources = @("tests\host\test_decoder.cpp", "src\ook_decoder.cpp")
$termOut = Join-Path $root "tests\host\test_ssh_terminal.exe"
$termSources = @("tests\host\test_ssh_terminal.cpp", "src\ssh_terminal.cpp")

$exe = ""
$isZig = $false

if ($Compiler) {
    if (-not (Test-Path $Compiler)) { Pop-Location; throw "compiler not found: $Compiler" }
    $exe = (Resolve-Path $Compiler).Path
    if ((Split-Path -Leaf $exe) -match '^zig') { $isZig = $true }
} else {
    if ($env:ZIG_PATH -and (Test-Path $env:ZIG_PATH)) {
        $exe = (Resolve-Path $env:ZIG_PATH).Path
        $isZig = $true
    }

    $zigCmd = Get-Command zig -ErrorAction SilentlyContinue
    if (-not $exe -and $zigCmd) {
        $exe = $zigCmd.Source
        $isZig = $true
    }

    if (-not $exe) {
        $localZig = Get-ChildItem -Path (Join-Path $root "tools\zig") -Recurse -Filter "zig.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($localZig) {
            $exe = $localZig.FullName
            $isZig = $true
        }
    }

    if (-not $exe) {
        foreach ($name in @("g++", "clang++")) {
            $cmd = Get-Command $name -ErrorAction SilentlyContinue
            if ($cmd) { $exe = $cmd.Source; break }
        }
    }
}

if (-not $exe) {
    Pop-Location
    throw "No C++ compiler found. Install g++/clang++, put zig on PATH, or pass -Compiler <path>."
}

$buildArgs = @("-std=c++17", "-I", "tests/host", "-I", "src", "-o", $out) + $sources

if ($isZig) {
    Write-Host "Building with zig: $exe"
    & $exe c++ @buildArgs
} else {
    Write-Host "Building with: $exe"
    & $exe @buildArgs
}

if ($LASTEXITCODE -ne 0) {
    Pop-Location
    throw "build failed"
}

& $out
$code = $LASTEXITCODE

$termBuildArgs = @("-std=c++17", "-I", "tests/host/stubs", "-I", "src", "-o", $termOut) + $termSources
if ($isZig) {
    Write-Host "Building terminal tests with zig: $exe"
    & $exe c++ @termBuildArgs
} else {
    Write-Host "Building terminal tests with: $exe"
    & $exe @termBuildArgs
}
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    throw "terminal test build failed"
}
& $termOut
if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE }

Pop-Location
exit $code
