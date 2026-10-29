param(
    [string]$PdsRoot = 'D:\App\PDS_2022.2-SP6.1',
    [string]$IcarusRoot = 'C:\iverilog\bin'
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$netlist = Join-Path $projectRoot 'build/pds/synthesize/ag_fpga_top_syn.vm'
if (!(Test-Path -LiteralPath $netlist)) { throw 'Run build_fpga.ps1 with the default smoke firmware first.' }
$library = Join-Path $PdsRoot 'arch/vendor/pango/verilog/simulation'
$executable = Join-Path $projectRoot 'build/gate_smoke.vvp'
& (Join-Path $IcarusRoot 'iverilog.exe') -g2012 -s gate_smoke_tb -y $library -o $executable (Join-Path $projectRoot 'sim/gate_smoke_tb.sv') $netlist
if ($LASTEXITCODE -ne 0) { throw 'Gate-level simulation compilation failed.' }
& (Join-Path $IcarusRoot 'vvp.exe') $executable
if ($LASTEXITCODE -ne 0) { throw 'Gate-level smoke failed.' }
