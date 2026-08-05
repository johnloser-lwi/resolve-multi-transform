# Install MultiTransform.ofx.bundle to a user-writable folder for development.
#
# Avoids needing an elevated shell on every rebuild. Resolve reads the
# OFX_PLUGIN_PATH environment variable at launch (semicolon-separated on
# Windows) in addition to the system plugin directory.
#
# One-time setup, then just re-run this script after each build:
#   [Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', "$env:LOCALAPPDATA\OFX\Plugins", 'User')
# ...and restart Resolve (a full restart, not just a project reload).

param(
    [string]$Destination = "$env:LOCALAPPDATA\OFX\Plugins",

    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bundle = Join-Path $repo "build\$Config\MultiTransform.ofx.bundle"

if (-not (Test-Path $bundle)) {
    throw "Bundle not found at $bundle. Run scripts\build.ps1 -Config $Config first."
}

if (Get-Process -Name 'Resolve' -ErrorAction SilentlyContinue) {
    Write-Warning "DaVinci Resolve is running - it holds a lock on loaded plugins. Close it first, or the copy below may fail."
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$target = Join-Path $Destination 'MultiTransform.ofx.bundle'
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
Copy-Item $bundle -Destination $Destination -Recurse -Force

Write-Host "Installed to $target"

$current = [Environment]::GetEnvironmentVariable('OFX_PLUGIN_PATH', 'User')
if (-not $current -or $current -notlike "*$Destination*") {
    Write-Host ""
    Write-Warning "OFX_PLUGIN_PATH does not include $Destination - Resolve will not find the plugin."
    Write-Host "Run this once, then restart Resolve:" -ForegroundColor Yellow
    Write-Host "  [Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', '$Destination', 'User')" -ForegroundColor Yellow
} else {
    Write-Host "OFX_PLUGIN_PATH already includes the destination. Restart Resolve to pick up the new build."
}

Write-Host ""
Write-Host "Probe log: $env:LOCALAPPDATA\MultiTransform\probe.log"
