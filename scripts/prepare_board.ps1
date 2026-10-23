param(
    [ValidateSet(27,81,96,102,108)][int]$ClockMHz = 27,
    [string]$Firmware = 'build\smoke.hex',
    [string]$OutputDirectory = '',
    [string]$PdsRoot = 'D:\App\PDS_2022.2-SP6.1',
    [ValidateRange(0,2147483647)][int]$PlacementSeed = 0,
    [ValidateRange(0,200)][int]$HoldMarginPs = 100,
    [switch]$RefineRouting,
    [switch]$Build,
    [switch]$Bitstream
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$out = if (!$OutputDirectory) { Join-Path $root "build\board$ClockMHz" }
    elseif ([IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory }
    else { Join-Path $root $OutputDirectory }
$fw = if ([IO.Path]::IsPathRooted($Firmware)) { $Firmware } else { Join-Path $root $Firmware }
if (!(Test-Path -LiteralPath $fw -PathType Leaf)) { throw "Firmware not found: $fw" }
if ($Bitstream -and !$Build) { throw '-Bitstream requires -Build to verify current routed timing.' }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$source = [Text.StringBuilder]::new()
$names = @(Get-Content (Join-Path $root 'rtl/files.f') | Where-Object { $_.Trim() -and !$_.StartsWith('#') })
$names += 'rtl/qinglong_fpga_top.sv', 'fpga/board/qinglong_board_clock.sv', 'fpga/board/qinglong_board_system.sv'
foreach ($name in ($names | Select-Object -Unique)) {
    [void]$source.AppendLine("// Source: $name")
    [void]$source.AppendLine([IO.File]::ReadAllText((Join-Path $root $name)))
}
[void]$source.AppendLine(@"
module qinglong_board_top(input wire clk_27m, reset_n, output wire uart_txd, output wire [7:0] led);
    qinglong_board_system #(.CLOCK_MHZ($ClockMHz)) board (
        .clk_27m(clk_27m), .reset_n(reset_n), .uart_txd(uart_txd), .led(led));
endmodule
"@)
$sv = Join-Path $out 'qinglong_board.sv'
[IO.File]::WriteAllText($sv, $source.ToString(), $utf8)
Copy-Item -LiteralPath $fw -Destination (Join-Path $out 'firmware.hex') -Force
$constraint = @'
create_clock -name ref_clk -period 37.037037 [get_ports clk_27m]
set_false_path -from [get_ports reset_n]
# UART/LED pads have no synchronous receiver clock. Internal paths stay timed.
set_false_path -to [get_ports {uart_txd led[*]}]
'@
if ($ClockMHz -ne 27) {
    $multiply = if ($ClockMHz -in 96,102) { $ClockMHz / 3 } else { $ClockMHz / 27 }
    $divide = if ($ClockMHz -in 96,102) { 9 } else { 1 }
    $constraint += "`ncreate_generated_clock -name cpu_clk -source [get_ports clk_27m] -multiply_by $multiply -divide_by $divide [get_pins {board/clocking_unit/pll_mode.pll/CLKOUT0}]`n"
}
$constraint += "`n" + [IO.File]::ReadAllText((Join-Path $root 'fpga/board/board_pins.fdc'))
$fdc = Join-Path $out 'board.fdc'
[IO.File]::WriteAllText($fdc, $constraint, $utf8)
$project = [IO.File]::ReadAllText((Join-Path $root 'fpga/RiscV.template.pds'))
$project = $project.Replace('fpga/generated/qinglong_all.sv', 'qinglong_board.sv').Replace('fpga/timing.fdc', 'board.fdc').Replace('qinglong_fpga_top', 'qinglong_board_top')
$project = $project.Replace('__SOURCE_TIME__', (Get-Item $sv).LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss'))
$project = $project.Replace('__CONSTRAINT_TIME__', (Get-Item $fdc).LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss'))
[IO.File]::WriteAllText((Join-Path $out 'qinglong_board.pds'), $project, $utf8)
$manifest = [ordered]@{
    board = 'MES2L676-100HP'; device = 'PG2L100H-6IFBG676'; clock_hz = $ClockMHz * 1000000
    firmware_origin = $fw; firmware_sha256 = (Get-FileHash (Join-Path $out 'firmware.hex')).Hash
    source_sha256 = (Get-FileHash $sv).Hash; constraint_sha256 = (Get-FileHash $fdc).Hash
    board_tested = $false
}
$manifest | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $out 'manifest.json')
Write-Output "Prepared $out\qinglong_board.pds ($ClockMHz MHz, MES2L676-100HP pins)."
if (!$Build) { return }
$tcl = @'
set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design qinglong_board.sv
add_constraint board.fdc
compile -top_module qinglong_board_top -system_verilog -multi_file_compilation_unit TRUE
synthesize -ads -resource_sharing FALSE
dev_map
pnr
report_timing
'@
$pnrCommand = "pnr -gplace_seed $PlacementSeed -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree $HoldMarginPs"
if ($RefineRouting) {
    $pnrCommand += ' -enable_route_constraint_iterate -route_constraint_iterate_times 3 -route_constraint_slack 0.1'
}
$tcl = $tcl.Replace("`npnr`n", "`n$pnrCommand`n")
$manifest['pnr_options'] = $pnrCommand
$manifest | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $out 'manifest.json')
[IO.File]::WriteAllText((Join-Path $out 'build.tcl'), $tcl, $utf8)
Push-Location $out
try {
    & (Join-Path $PdsRoot 'bin/pds_shell.exe') -file build.tcl -work_dir $out -project_name qinglong_board
    if ($LASTEXITCODE -ne 0) { throw "PDS failed with exit code $LASTEXITCODE" }
    $reportPath = Join-Path $out 'report_timing/qinglong_board_top.rtr'
    if (!(Test-Path $reportPath) -or (Get-Item $reportPath).LastWriteTime -lt (Get-Item $sv).LastWriteTime) {
        throw 'No fresh routed timing report was produced.'
    }
    $report = [IO.File]::ReadAllText($reportPath)
    if ($report -notmatch 'Design Summary : All Constraints Met\.') {
        throw "Board timing has not passed; do not program this profile. Inspect $reportPath"
    }
    Write-Output "Routed timing passed: $reportPath"
    if ($Bitstream) {
        # PDS pre-parses Tcl, including file reads, before executing flow commands.
        # Check the fresh timing report in PowerShell, then use the routed DB in
        # a second invocation with the device explicitly selected.
        $bitTcl = @'
set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design qinglong_board.sv
add_constraint board.fdc
gen_bit_stream -force_to_run -input_db_file {place_route/qinglong_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED
'@
        [IO.File]::WriteAllText((Join-Path $out 'bitstream.tcl'), $bitTcl, $utf8)
        $projectPath = Join-Path $out 'qinglong_board.pds'
        $routedProject = [IO.File]::ReadAllText($projectPath)
        try {
            & (Join-Path $PdsRoot 'bin/pds_shell.exe') -file bitstream.tcl -work_dir $out -project_name qinglong_board
            if ($LASTEXITCODE -ne 0) { throw "Bitstream generation failed with exit code $LASTEXITCODE" }
        } finally {
            # The standalone bitstream invocation rewrites the project context.
            # Preserve the completed compile/synthesis/route stages for the GUI.
            [IO.File]::WriteAllText($projectPath, $routedProject, $utf8)
        }
        $bits = @(Get-ChildItem (Join-Path $out 'generate_bitstream') -Filter *.sbit -Recurse -ErrorAction SilentlyContinue |
            Where-Object LastWriteTime -ge (Get-Item $sv).LastWriteTime)
        if (!$bits) { throw 'No .sbit file was generated; inspect the PDS log.' }
        $manifest['bitstreams'] = @($bits | ForEach-Object { @{path=$_.FullName; sha256=(Get-FileHash $_.FullName).Hash} })
        $manifest['routed_timing_passed'] = $true
        $manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $out 'manifest.json')
        Write-Output "Bitstream: $($bits.FullName -join ', ')"
    }
} finally { Pop-Location }
