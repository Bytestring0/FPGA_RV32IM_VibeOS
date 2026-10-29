param(
    [string]$PdsRoot = 'D:\App\PDS_2022.2-SP6.1',
    [string]$IpDirectory = 'build\ag-ddr-ip',
    [string]$OutputDirectory = 'build\ddr-soc102',
    [string]$Firmware = 'build\ddr_smoke.hex',
    [ValidateRange(1,5)][int]$RoutingPasses = 1,
    [ValidateRange(0,120)][int]$RoutingTimeoutMinutes = 0,
    [ValidateRange(0,500)][int]$CongestionIterations = 0,
    [switch]$DumpCongestion,
    [switch]$DisableControlChain,
    [switch]$OptimizeMultiCorner,
    [ValidateRange(0,1024)][int[]]$CpuLogicRegion = @(),
    [ValidateRange(0,65535)][int]$PlacementSeed = 0,
    [ValidateRange(0,200)][int]$HoldMarginPs = 50,
    [ValidateRange(2,9)][int]$CacheSetBits = 8,
    [switch]$NoCache, [switch]$WordLoadBypassOnly,
    [switch]$Build, [switch]$MapOnly, [switch]$Bitstream, [switch]$MulhPipeline
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
function Absolute([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p } }
$out = Absolute $OutputDirectory
$ip = Absolute $IpDirectory
$fw = Absolute $Firmware
if ($Bitstream -and !$Build) { throw '-Bitstream requires -Build.' }
if ($MapOnly -and (!$Build -or $Bitstream)) { throw '-MapOnly requires -Build and excludes -Bitstream.' }
if ($CpuLogicRegion.Count -ne 0 -and ($CpuLogicRegion.Count -ne 4 -or
    $CpuLogicRegion[0] -gt $CpuLogicRegion[1] -or $CpuLogicRegion[2] -gt $CpuLogicRegion[3])) {
    throw '-CpuLogicRegion requires xmin,xmax,ymin,ymax in PDS floorplan coordinates.'
}
if (!(Test-Path -LiteralPath $fw)) { throw 'Build the firmware first: WSL make ddr-firmware' }
[xml]$idf = Get-Content -LiteralPath (Join-Path $ip 'ag_ddr3.idf') -Raw
if ($idf.ip_inst.header.version -ne '1.10' -or $idf.ip_inst.header.device -ne 'PG2L100H' -or
    $idf.ip_inst.header.package -ne 'FBG676' -or $idf.ip_inst.header.speedgrade -ne '-6') {
    throw 'Expected HMIC_S 1.10 for PG2L100H-6IFBG676.'
}
$frozen = Get-Content -LiteralPath (Join-Path $root 'fpga/ip/ag_ddr3/ag_ddr3.idf') -Raw
[xml]$reference = $frozen
foreach ($p in $reference.ip_inst.param_list.param) {
    $actual = @($idf.ip_inst.param_list.param | Where-Object { $_.name -ceq $p.name })
    if ($actual.Count -ne 1 -or $actual[0].value -ne $p.value) { throw "IP configuration mismatch: $($p.name). Run prepare_ddr_ip.ps1." }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$names = @(Get-Content (Join-Path $root 'rtl/files.f') | Where-Object { $_.Trim() -and !$_.StartsWith('#') })
$names += 'rtl/ag_hmic_bridge.sv', 'fpga/board/ag_board_clock.sv', 'fpga/board/ag_ddr_board_top.sv'
$source = ($names | ForEach-Object { "// Source: $_`n" + [IO.File]::ReadAllText((Join-Path $root $_)) }) -join "`n"
$source = $source.Replace('module ag_ddr_board_top', 'module ag_board_top')
if ($WordLoadBypassOnly) { $source = $source.Replace('parameter bit BOARD_LOAD_WORD_ONLY = 0', 'parameter bit BOARD_LOAD_WORD_ONLY = 1') }
if ($NoCache) { $source = $source.Replace('parameter bit BOARD_CACHE_ENABLE = 1', 'parameter bit BOARD_CACHE_ENABLE = 0') }
$source = $source.Replace('parameter integer BOARD_CACHE_SET_BITS = 8', "parameter integer BOARD_CACHE_SET_BITS = $CacheSetBits")
if ($MulhPipeline) { $source = $source.Replace('parameter bit BOARD_MULH_PIPELINE = 0', 'parameter bit BOARD_MULH_PIPELINE = 1') }
[IO.File]::WriteAllText((Join-Path $out 'ag_board.sv'), $source, $utf8)
Copy-Item -LiteralPath $fw -Destination (Join-Path $out 'firmware.hex')
$fdc = @'
create_clock -name board_27m -period 37.037037 [get_ports clk_27m]
create_generated_clock -name cpu_clk -source [get_ports clk_27m] -multiply_by 34 -divide_by 9 [get_pins {clocking_unit/pll_mode.pll/CLKOUT0}]
set_false_path -from [get_ports reset_n]
set_false_path -to [get_ports {uart_txd led[*]}]
'@
$fdc += "`n" + [IO.File]::ReadAllText((Join-Path $root 'fpga/ip/ag_ddr3/board_ddr.fdc'))
$fdc += "`n" + [IO.File]::ReadAllText((Join-Path $root 'fpga/board/board_pins.fdc'))
$fdc += "`n" + [IO.File]::ReadAllText((Join-Path $root 'fpga/board/ddr_cdc.fdc'))
if ($CpuLogicRegion.Count -eq 4) {
    # Keep APM/DRM macros free. Region coordinates differ from resource names in timing reports.
    $region = 'region1(' + ($CpuLogicRegion -join ',') + ')'
    $fdc += "`n" + 'define_global_attribute {PAP_REGION} {' + $region + '[CLMA,CLMS];}'
    $fdc += "`n" + 'define_attribute {soc.cpu} {PAP_REGION} {' + $region + '}' + "`n"
}
[IO.File]::WriteAllText((Join-Path $out 'board.fdc'), $fdc, $utf8)
# Generate explicit add_design commands. The shell does not inherit GUI sources.
$vendorFiles = @($idf.ip_inst.file_list.source.file | ForEach-Object { (Join-Path $ip $_.pathname).Replace('\','/') })
$setup = @('set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676', 'add_design ag_board.sv')
$setup += $vendorFiles | ForEach-Object { "add_design {$_}" }
$setup += 'add_constraint board.fdc'
$compile = $setup + 'compile -top_module ag_board_top -system_verilog -multi_file_compilation_unit TRUE'
$routeCommand = "pnr -gplace_seed $PlacementSeed -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree $HoldMarginPs"
if ($RoutingTimeoutMinutes -gt 0) { $routeCommand += " -action_timeout $RoutingTimeoutMinutes" }
if ($CongestionIterations -gt 0) { $routeCommand += " -congest_overlap_limit $CongestionIterations" }
if ($DumpCongestion) { $routeCommand += ' -dump_router_congestion_plot TRUE' }
if ($DisableControlChain) { $routeCommand += ' -share_router_control_signal FALSE' }
if ($OptimizeMultiCorner) { $routeCommand += ' -optimize_multi_corner_timing' }
if ($RoutingPasses -gt 1) {
    $routeCommand += " -enable_route_constraint_iterate -route_constraint_iterate_times $RoutingPasses -route_constraint_slack 0.1"
}
$flow = $compile + @('synthesize -ads -resource_sharing FALSE', 'dev_map')
if (!$MapOnly) { $flow += @($routeCommand, 'report_timing') }
[IO.File]::WriteAllLines((Join-Path $out 'build.tcl'), $flow, $utf8)
[IO.File]::WriteAllLines((Join-Path $out 'compile.tcl'), $compile, $utf8)
$manifest = [ordered]@{
    board='MES2L676-100HP'; device='PG2L100H-6IFBG676'; clock_hz=102000000
    ddr_ui_hz=125000000; ddr_mt_s=1000; ddr_bytes=1073741824; board_tested=$false; mulh_pipeline=[bool]$MulhPipeline
    hold_optimization_margin_ps=$HoldMarginPs; routing_passes=$RoutingPasses; word_load_bypass_only=[bool]$WordLoadBypassOnly; cache_enabled=(!$NoCache); cache_bytes_each=(64*(1 -shl $CacheSetBits))
    routing_timeout_minutes=$RoutingTimeoutMinutes; congestion_iterations=$CongestionIterations; dump_congestion=[bool]$DumpCongestion
    shared_control_chain=(!$DisableControlChain)
    optimize_multi_corner_timing=[bool]$OptimizeMultiCorner
    cpu_logic_region=@($CpuLogicRegion)
    placement_seed=$PlacementSeed
    source_sha256=(Get-FileHash (Join-Path $out 'ag_board.sv')).Hash
    constraint_sha256=(Get-FileHash (Join-Path $out 'board.fdc')).Hash
    firmware_sha256=(Get-FileHash (Join-Path $out 'firmware.hex')).Hash
    additional_sources=@($vendorFiles | ForEach-Object { @{path=$_; sha256=(Get-FileHash $_).Hash} })
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $out 'manifest.json')
Push-Location $out
try {
    # Compile also creates a complete GUI project with all source files registered.
    $script = if ($Build) { 'build.tcl' } else { 'compile.tcl' }
    & (Join-Path $PdsRoot 'bin/pds_shell.exe') -file $script -work_dir $out -project_name ag_board
    if ($LASTEXITCODE -ne 0) { throw "PDS failed: $LASTEXITCODE" }
    $compiled = Join-Path $out 'compile/ag_board_top_comp.adf'
    if (!(Test-Path $compiled) -or (Get-Item $compiled).LastWriteTime -lt (Get-Item 'ag_board.sv').LastWriteTime) {
        throw 'Compile did not produce a fresh design database; inspect compile/run.log.'
    }
    if (!$Build -or $MapOnly) { return }
    $report = Join-Path $out 'report_timing/ag_board_top.rtr'
    if (!(Test-Path $report) -or (Get-Item $report).LastWriteTime -lt (Get-Item 'ag_board.sv').LastWriteTime) { throw 'Missing fresh timing report.' }
    if ([IO.File]::ReadAllText($report) -notmatch 'Design Summary : All Constraints Met\.') { throw "Timing is not closed: $report" }
    $manifest['routed_timing_passed'] = $true
    if ($Bitstream) {
        $bitscript = $setup + 'gen_bit_stream -force_to_run -input_db_file {place_route/ag_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED'
        [IO.File]::WriteAllLines((Join-Path $out 'bitstream.tcl'), $bitscript, $utf8)
        $project = [IO.File]::ReadAllText((Join-Path $out 'ag_board.pds'))
        try {
            & (Join-Path $PdsRoot 'bin/pds_shell.exe') -file bitstream.tcl -work_dir $out -project_name ag_board
            if ($LASTEXITCODE -ne 0) { throw 'Bitstream generation failed.' }
        } finally { [IO.File]::WriteAllText((Join-Path $out 'ag_board.pds'), $project, $utf8) }
        $bit = Get-Item 'generate_bitstream/ag_board_top.sbit'
        if ($bit.LastWriteTime -lt (Get-Item 'ag_board.sv').LastWriteTime) { throw 'Stale bitstream.' }
        $manifest['bitstreams'] = @(@{path=$bit.FullName; sha256=(Get-FileHash $bit.FullName).Hash})
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $out 'manifest.json')
} finally { Pop-Location }
Write-Output "Prepared $out/ag_board.pds; board test still required."
