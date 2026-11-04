param(
    [ValidateSet('Verify','Program')][string]$Action = 'Verify',
    [string]$Port = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$run = Join-Path $root 'npu_system_pds/npu_pipeline81_20261103'
if ($Action -eq 'Verify') {
    $timing = Join-Path $run 'prj_tasks/pnr_1/report_timing/ag_npu_board_top.rtr'
    $database = Join-Path $run 'prj_tasks/pnr_1/place_route/ag_npu_board_top_pnr.adf'
    $bit = Join-Path $run 'prj_tasks/pnr_1/generate_bitstream/ag_npu_board_top.sbit'
    foreach ($path in @($timing,$database,$bit)) {
        if (!(Test-Path -LiteralPath $path)) { throw ('Implementation artifact missing: ' + $path) }
    }
    if ((Get-Content -LiteralPath $timing -Raw) -notmatch 'Design Summary : All Constraints Met[.]') {
        throw 'Routed setup and hold constraints must all pass.'
    }
    if ((Get-Item -LiteralPath $timing).LastWriteTimeUtc -lt (Get-Item -LiteralPath $database).LastWriteTimeUtc -or
        (Get-Item -LiteralPath $bit).LastWriteTimeUtc -lt (Get-Item -LiteralPath $database).LastWriteTimeUtc) {
        throw 'Timing and bitstream do not cover the final optimized routed database.'
    }
    Write-Host '81 MHz internal setup/hold passed and matching bitstream is available.'
    exit 0
}
if (!$Port) { throw 'Specify the actual USB-UART port with -Port COMx.' }
& (Join-Path $root 'scripts/run_npu_model.ps1') -Port $Port -Run $run
