param(
    [string]$PdsRoot = 'D:\App\PDS_2022.2-SP6.1',
    [string]$OutputDirectory = 'build\ag-ddr-ip'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$out = if ([IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory } else { Join-Path $root $OutputDirectory }
$model = Join-Path $PdsRoot 'ip\system_ip\ips2l_hmic_s\ips2l_hmic_eval\ips2l_hmic_s'
$generator = Join-Path $PdsRoot 'bin\ip_generate.exe'
if (!(Test-Path -LiteralPath (Join-Path $model 'index.xml'))) { throw "Logos2 HMIC_S 1.10 is required: $model" }
[xml]$index = Get-Content -LiteralPath (Join-Path $model 'index.xml') -Raw
if ($index.ip_index.header.version -ne '1.10') { throw 'This configuration requires reviewed IP version 1.10.' }
New-Item -ItemType Directory -Path $out -Force | Out-Null
$idf = Join-Path $out 'ag_ddr3.idf'
Copy-Item -LiteralPath (Join-Path $root 'fpga\ip\ag_ddr3\ag_ddr3.idf') -Destination $idf
& $generator -i $idf -m $model -family Logos2 -device PG2L100H -pack FBG676 -speed -6 -disable-syn
if ($LASTEXITCODE -ne 0) { throw "IP generation failed: $LASTEXITCODE" }
$log = [IO.File]::ReadAllText((Join-Path $out 'generate.log'))
if ($log -notmatch 'Done: 0 error\(s\), 0 warning\(s\)') { throw 'Review IP generation diagnostics before building.' }
Write-Output "Generated board DDR3 IP: $out (generation only; not a timing or hardware pass)."
