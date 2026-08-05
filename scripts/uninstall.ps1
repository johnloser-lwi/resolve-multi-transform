# Remove MultiTransform.ofx.bundle from the system OFX plugin directory.
# Self-elevating, same as install.ps1.

[CmdletBinding()]
param(
    [string]$Destination = 'C:\Program Files\Common Files\OFX\Plugins',
    [switch]$Force,
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$target = Join-Path $Destination 'MultiTransform.ofx.bundle'

# Only elevate if we actually cannot write there, and never for a no-op removal.
$needsElevation = $false
if (Test-Path $target) {
    try {
        $probe = Join-Path $Destination ".mtx-write-probe-$PID"
        New-Item -ItemType File -Path $probe -ErrorAction Stop | Out-Null
        Remove-Item $probe -Force -ErrorAction SilentlyContinue
    } catch { $needsElevation = $true }
}

if ($needsElevation -and -not $isAdmin) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass',
                 '-File', "`"$PSCommandPath`"",
                 '-Destination', "`"$Destination`"", '-Elevated')
    if ($Force) { $argList += '-Force' }
    try {
        $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -PassThru -Wait
    } catch {
        Write-Host "Elevation declined. Nothing was removed." -ForegroundColor Yellow
        exit 1
    }
    exit $proc.ExitCode
}

try {
    if (-not (Test-Path $target)) {
        Write-Host "Not installed - nothing to remove at $target" -ForegroundColor Yellow
        $exitCode = 0
    }
    else {
        $resolve = Get-Process -Name 'Resolve' -ErrorAction SilentlyContinue
        if ($resolve -and $Force) {
            $resolve.CloseMainWindow() | Out-Null
            if (-not $resolve.WaitForExit(30000)) { throw "Resolve did not close within 30s." }
            $resolve = $null
        }

        try {
            Remove-Item -Recurse -Force $target -ErrorAction Stop
        }
        catch {
            if ($resolve) {
                throw "Could not remove $target because DaVinci Resolve has the plugin locked.`nClose Resolve and re-run, or pass -Force.`n`nUnderlying error: $($_.Exception.Message)"
            }
            throw
        }

        Write-Host "Removed $target" -ForegroundColor Green
        $exitCode = 0
    }
}
catch {
    Write-Host "UNINSTALL FAILED" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    $exitCode = 1
}

if ($Elevated) { Write-Host ""; Read-Host "Press Enter to close" }
exit $exitCode
