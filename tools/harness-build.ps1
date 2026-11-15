# Build (and optionally flash) the firmware from a LOCAL mirror of src/.
#
# Why a mirror: this project lives on an SMB share that reports zero-length
# files and stale directory metadata. PlatformIO's Library Dependency Finder
# stats every header it walks, and against that share it stalls in
# "Scanning dependencies..." indefinitely. Copying src/ + boards/ +
# platformio.ini to a local directory and building there takes ~45 s.
#
# The Y: tree stays the source of truth; this script always mirrors it first.
#
#   tools\harness-build.ps1                 build only
#   tools\harness-build.ps1 -Upload         build, then flash COM13
#   tools\harness-build.ps1 -Upload -Port COM7
param(
    [switch]$Upload,
    [string]$Port = 'COM13',
    [string]$Source = 'Y:\2026\cardputer',
    [string]$Mirror = 'C:\Users\dloach\build\cardputer',
    [string]$PlatformIO = 'C:\Users\dloach\.platformio\penv\Scripts\platformio.exe'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Source)) { throw "source not found: $Source" }
if (-not (Test-Path $PlatformIO)) { throw "platformio not found: $PlatformIO" }

New-Item -ItemType Directory -Force -Path $Mirror | Out-Null
robocopy "$Source\src" "$Mirror\src" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy "$Source\boards" "$Mirror\boards" /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
Copy-Item "$Source\platformio.ini" "$Mirror\platformio.ini" -Force

Push-Location $Mirror
try {
    if ($Upload) {
        & $PlatformIO run -e cardputer-adv -t upload --upload-port $Port
    } else {
        & $PlatformIO run -e cardputer-adv
    }
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
