param(
    [string]$PdsRoot = 'D:\App\PDS_2022.2-SP6.1',
    [string]$Firmware = 'build\smoke.hex',
    [switch]$PlaceRoute
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$outputDirectory = Join-Path $projectRoot 'build\pds'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$firmwarePath = Join-Path $projectRoot $Firmware
if (!(Test-Path -LiteralPath $firmwarePath)) { throw "Build firmware first: $firmwarePath" }
Copy-Item -LiteralPath $firmwarePath -Destination (Join-Path $outputDirectory 'firmware.hex')
$previousPnr = $env:QL_PNR
try {
    $env:QL_PNR = if ($PlaceRoute) { '1' } else { '0' }
    & (Join-Path $PdsRoot 'bin\pds_shell.exe') -file (Join-Path $projectRoot 'fpga\build.tcl') -work_dir $outputDirectory -project_name qinglong
    if ($LASTEXITCODE -ne 0) { throw "PDS failed with exit code $LASTEXITCODE" }
} finally { $env:QL_PNR = $previousPnr }
