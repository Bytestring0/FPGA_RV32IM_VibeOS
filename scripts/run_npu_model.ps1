param(
    [Parameter(Mandatory)][string]$Port,
    [string]$Run = 'npu_system_pds/npu_pipeline81_20261103',
    [string]$PdsRoot = 'D:/App/pango/PDS_2025.2-ads',
    [int]$TimeoutSeconds = 7200
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runPath = if ([IO.Path]::IsPathRooted($Run)) { $Run } else { Join-Path $projectRoot $Run }
$runPath = (Resolve-Path $runPath).Path
$timingPath = Join-Path $runPath 'prj_tasks/pnr_1/report_timing/ag_npu_board_top.rtr'
$database = Join-Path $runPath 'prj_tasks/pnr_1/place_route/ag_npu_board_top_pnr.adf'
$bit = Join-Path $runPath 'prj_tasks/pnr_1/generate_bitstream/ag_npu_board_top.sbit'
foreach ($path in @($timingPath,$database,$bit)) {
    if (!(Test-Path -LiteralPath $path)) { throw ('Required implementation artifact missing: ' + $path) }
}
$timing = Get-Content -LiteralPath $timingPath -Raw
if ($timing -notmatch 'Design Summary : All Constraints Met[.]') {
    throw 'Routed setup and hold constraints must all pass before programming.'
}
if ((Get-Item -LiteralPath $timingPath).LastWriteTimeUtc -lt (Get-Item -LiteralPath $database).LastWriteTimeUtc -or
    (Get-Item -LiteralPath $bit).LastWriteTimeUtc -lt (Get-Item -LiteralPath $database).LastWriteTimeUtc) {
    throw 'Timing and bitstream must be newer than the optimized routed database.'
}
if (!(Get-NetTCPConnection -LocalPort 65420 -State Listen -ErrorAction SilentlyContinue)) {
    $server = Start-Process (Join-Path $PdsRoot 'bin/cdt_js.exe') -ArgumentList '-port','65420' -WorkingDirectory (Join-Path $PdsRoot 'bin') -PassThru -WindowStyle Hidden
    $serverTimer = [Diagnostics.Stopwatch]::StartNew()
    while (!(Get-NetTCPConnection -LocalPort 65420 -State Listen -ErrorAction SilentlyContinue)) {
        if ($server.HasExited -or $serverTimer.Elapsed.TotalSeconds -gt 15) { throw 'JTAG server could not start.' }
        Start-Sleep -Milliseconds 200
    }
}
$serial = [IO.Ports.SerialPort]::new($Port,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$serial.Handshake = [IO.Ports.Handshake]::None
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$cfg = [Diagnostics.Process]::new()
$cfg.StartInfo.FileName = Join-Path $PdsRoot 'bin/cdt_cfg_shell.exe'
$cfg.StartInfo.WorkingDirectory = $projectRoot
$cfg.StartInfo.UseShellExecute = $false
$cfg.StartInfo.CreateNoWindow = $true
$cfg.StartInfo.RedirectStandardInput = $true
$cfg.StartInfo.RedirectStandardOutput = $true
$cfg.StartInfo.RedirectStandardError = $true
$capture = [Text.StringBuilder]::new()
$ok = $false
$cfgStarted = $false
try {
    $serial.Open()
    [void]$cfg.Start()
    $cfgStarted = $true
    $stdout = $cfg.StandardOutput.ReadToEndAsync()
    $stderr = $cfg.StandardError.ReadToEndAsync()
    $bitPath = $bit.Replace('\','/')
    $scriptPath = (Join-Path $PSScriptRoot 'program_board.tcl').Replace('\','/')
    $cfg.StandardInput.WriteLine("set bitstream {$bitPath}")
    $cfg.StandardInput.WriteLine("source {$scriptPath}")
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $chunk = $serial.ReadExisting()
        if ($chunk) { [void]$capture.Append($chunk); Write-Host -NoNewline $chunk }
        $text = $capture.ToString()
        if ($text -match 'NPU_MODEL_COMPLETE' -and $text -match 'CANDIDATE_EXIT rc=0') { $ok = $true; break }
        if ($text -match 'FAIL_|UNKNOWN preserve|CANDIDATE_EXIT rc=-?[1-9]') { break }
        if ($cfg.HasExited) { break }
        Start-Sleep -Milliseconds 20
    }
    $cfg.StandardInput.WriteLine('cfg_disconnect')
    $cfg.StandardInput.WriteLine('exit')
    if (!$cfg.WaitForExit(10000)) { $cfg.Kill() }
    Write-Host ($stdout.Result + $stderr.Result)
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    if ($cfgStarted -and !$cfg.HasExited) { $cfg.Kill() }
    $cfg.Dispose()
}
if (!$ok) { throw 'Full model completion was not observed; inspect the displayed UART and programmer output.' }
Write-Host 'Actual NPU full-model execution completed.'
