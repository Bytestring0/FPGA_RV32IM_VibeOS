param(
    [string]$Profile = 'build\ddr-soc102-mulpipe',
    [string]$PdsRoot = 'D:\App\PDS_2022.2-SP6.1'
)
# Finalize an existing routed snapshot without rebuilding its source/firmware.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out = if ([IO.Path]::IsPathRooted($Profile)) { $Profile } else { Join-Path $root $Profile }
$manifestPath = Join-Path $out 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$checks = @(
    @{path=(Join-Path $out 'ag_board.sv'); sha256=$manifest.source_sha256},
    @{path=(Join-Path $out 'firmware.hex'); sha256=$manifest.firmware_sha256},
    @{path=(Join-Path $out 'board.fdc'); sha256=$manifest.constraint_sha256}
) + @($manifest.additional_sources)
foreach ($inputFile in $checks) {
    if ((Get-FileHash -LiteralPath $inputFile.path).Hash -ne $inputFile.sha256) {
        throw "Input changed since this snapshot was built: $($inputFile.path)"
    }
}
$reportPath = Join-Path $out 'report_timing/ag_board_top.rtr'
$routePath = Join-Path $out 'place_route/ag_board_top_pnr.adf'
$report = Get-Item -LiteralPath $reportPath
$route = Get-Item -LiteralPath $routePath
if ($report.LastWriteTime -lt $route.LastWriteTime -or
    $route.LastWriteTime -lt (Get-Item (Join-Path $out 'ag_board.sv')).LastWriteTime -or
    [IO.File]::ReadAllText($reportPath) -notmatch 'Design Summary : All Constraints Met\.') {
    throw 'A fresh passing timing report for this routed database is required.'
}
$setup = @(Get-Content (Join-Path $out 'compile.tcl') | Where-Object { $_ -notmatch '^compile ' })
$setup += 'gen_bit_stream -force_to_run -input_db_file {place_route/ag_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED'
$setup | Set-Content -Encoding ascii (Join-Path $out 'finish-bitstream.tcl')
$projectPath = Join-Path $out 'ag_board.pds'
$project = [IO.File]::ReadAllText($projectPath)
$started = Get-Date
Push-Location $out
try {
    & (Join-Path $PdsRoot 'bin/pds_shell.exe') -file finish-bitstream.tcl -work_dir $out -project_name ag_board
    if ($LASTEXITCODE -ne 0) { throw 'Bitstream generation failed.' }
} finally {
    [IO.File]::WriteAllText($projectPath, $project, [Text.UTF8Encoding]::new($false))
    Pop-Location
}
$bit = Get-Item -LiteralPath (Join-Path $out 'generate_bitstream/ag_board_top.sbit')
if ($bit.LastWriteTime -lt $started) { throw 'No fresh bitstream was produced.' }
$manifest | Add-Member -NotePropertyName routed_timing_passed -NotePropertyValue $true -Force
$manifest | Add-Member -NotePropertyName timing_report_sha256 -NotePropertyValue (Get-FileHash $reportPath).Hash -Force
$manifest | Add-Member -NotePropertyName bitstreams -NotePropertyValue @(@{path=$bit.FullName; sha256=(Get-FileHash $bit.FullName).Hash}) -Force
$manifest.board_tested = $false
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $manifestPath
Write-Output "Verified snapshot bitstream: $($bit.FullName)"
