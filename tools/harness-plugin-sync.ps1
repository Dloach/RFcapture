# Deploy the Cardputer Host plugin from the project tree to its installed path.
#
# Why two copies: the Desktop profile installs the bundle with a `link:` to a
# LOCAL path (C:\Users\dloach\cardputer-ws). A junction to a mapped network
# drive (Y:) cannot be resolved by another process - that is what broke the
# previous plugin install - so the installed copy must live on local disk.
#
#   Y:\2026\cardputer\harness-ws\plugin   source of truth (versioned with the repo)
#   C:\Users\dloach\cardputer-ws          installed copy the Host actually runs
#
# Run this after editing the plugin, then reload it: Settings > Plugins, toggle
# "Cardputer live sessions" off and on (a full app restart also works).
#
#   tools\harness-plugin-sync.ps1
param(
    [string]$Source = 'Y:\2026\cardputer\harness-ws\plugin',
    [string]$Installed = 'C:\Users\dloach\cardputer-ws'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Source)) { throw "source not found: $Source" }

New-Item -ItemType Directory -Force -Path $Installed | Out-Null
robocopy $Source $Installed /MIR /XD node_modules /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with $LASTEXITCODE" }

Write-Host "synced $Source -> $Installed"
Get-ChildItem $Installed -File | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "reload: Settings > Plugins > Cardputer live sessions (toggle off/on)"
