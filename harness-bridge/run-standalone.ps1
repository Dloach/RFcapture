# Start the Cardputer <-> Harness bridge as an independent process.
# It does NOT install anything into the DSH desktop profile.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $PSScriptRoot ".venv\Scripts\python.exe"
$script = Join-Path $PSScriptRoot "standalone.py"
if (-not (Test-Path $python)) {
    Write-Host "Vosk venv missing: $python" -ForegroundColor Red
    exit 2
}

# A forced bridge exit can leave the ACP child holding a Session; remove only
# our own standalone cardputer children before starting a fresh one.
Get-CimInstance Win32_Process -Filter "Name='node.exe'" |
    Where-Object { $_.CommandLine -match 'dsh-standalone.*bin\.js' -and $_.CommandLine -match 'cardputer' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Start-Process -FilePath $python -ArgumentList @($script) -WorkingDirectory $root -NoNewWindow
Write-Host "Bridge starting on http://192.168.0.88:9100/" -ForegroundColor Cyan
Write-Host "Health: http://192.168.0.88:9100/cardputer/health"
