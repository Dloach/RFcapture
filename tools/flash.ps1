# Flash the Cardputer over USB when it is enumerated.  If no 303A board is
# present, stop and tell the operator to open SETTINGS -> OTA UPDATE instead of
# waiting for a port that will never appear.
param(
    [string]$Env = "cardputer-adv",
    [string]$Port
)

$ErrorActionPreference = "Stop"
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
if (-not (Test-Path $pio)) {
    Write-Host "PlatformIO not found: $pio" -ForegroundColor Red
    exit 2
}

if (-not $Port) {
    $lines = & $pio device list 2>&1
    $candidate = $null
    foreach ($line in $lines) {
        if ($line -match '^(COM\d+)') {
            $candidate = $Matches[1]
        }
        if ($line -match 'VID:PID=303A:' -and $candidate) {
            $Port = $candidate
            break
        }
        if ($line -match '^COM\d+$') {
            $candidate = $line.Trim()
        }
    }
}

if (-not $Port) {
    Write-Host ""
    Write-Host "No wired Cardputer (USB VID 303A) found." -ForegroundColor Yellow
    Write-Host "Do NOT keep retrying the upload."
    Write-Host ""
    Write-Host "Next step: on the Cardputer open SETTINGS -> OTA UPDATE, keep that"
    Write-Host "page visible, then run:"
    Write-Host "  pio run -e cardputer-ota -t upload --upload-port <device-ip>"
    Write-Host ""
    exit 3
}

Write-Host "Flashing $Env on $Port ..." -ForegroundColor Cyan
& $pio run -e $Env -t upload --upload-port $Port
exit $LASTEXITCODE
