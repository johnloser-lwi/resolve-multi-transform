# Build MultiTransform.ofx.bundle
#
# Uses the CMake bundled with Visual Studio: the standalone CMake on PATH may be
# older than the installed VS toolset and will not know its generator.

param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',

    # Leave CUDA undeclared so every host renders on the CPU. Passed on every
    # run (ON or OFF), because CMake caches options: a later plain build must
    # switch the GPU path back on rather than silently stay CPU-only.
    [switch]$CpuOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }

$vsPath = & $vswhere -latest -property installationPath
$vsMajor = (& $vswhere -latest -property installationVersion).Split('.')[0]

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

$yearMap = @{ '17' = '2022'; '18' = '2026' }
$year = $yearMap[$vsMajor]
if (-not $year) { throw "Unrecognised Visual Studio major version '$vsMajor'." }
$generator = "Visual Studio $vsMajor $year"

Write-Host "CMake:     $cmake"
Write-Host "Generator: $generator"

$cpuFlag = if ($CpuOnly) { 'ON' } else { 'OFF' }
Write-Host "CPU only:  $cpuFlag"

& $cmake -S $repo -B (Join-Path $repo 'build') -G $generator -A x64 "-DMTX_CPU_ONLY=$cpuFlag"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

& $cmake --build (Join-Path $repo 'build') --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host ""
Write-Host "Bundle: $(Join-Path $repo "build\$Config\MultiTransform.ofx.bundle")"
