# Build, flash and read the serial log in one go.
#
# "pio run -t upload" leaves this board in the ROM downloader, so the script
# finishes with a watchdog reset (esptool --after watchdog_reset) which boots
# the freshly flashed application.
#
#   .\tools\run-device.ps1                 # env cardputer-adv, 6 s of log
#   .\tools\run-device.ps1 -Env probe -Seconds 10
#   .\tools\run-device.ps1 -NoBuild        # just flash + read

param(
    [string]$Env = "cardputer-adv",
    [string]$Port = "COM10",
    [int]$Seconds = 6,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
if ($PSVersionTable.PSVersion.Major -ge 7) { $PSNativeCommandUseErrorActionPreference = $false }
$root = Split-Path -Parent $PSScriptRoot
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$py = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"

if (-not $NoBuild) {
    Write-Host "== build $Env =="
    & $pio run -d $root -e $Env 2>&1 | Select-String -Pattern "error|RAM:|Flash:|SUCCESS|FAILED" | Select-Object -Last 4
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

Write-Host "== upload to $Port =="
& $pio run -d $root -e $Env -t upload --upload-port $Port 2>&1 | Select-String -Pattern "SUCCESS|FAILED|error" | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "upload failed" }

Start-Sleep -Milliseconds 1500

Write-Host "== boot application =="
for ($i = 0; $i -lt 3; $i++) {
    & $py $esptool --port $Port --before no_reset --after watchdog_reset chip_id *> $null
    if ($LASTEXITCODE -eq 0) { break }
    Start-Sleep -Milliseconds 800
}

Start-Sleep -Milliseconds 700

Write-Host "== serial ($Seconds s) =="
$serial = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$serial.ReadTimeout = 300
try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) {
        try {
            $line = $serial.ReadLine()
            if ($line) { $line.TrimEnd() }
        } catch { }
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
}
