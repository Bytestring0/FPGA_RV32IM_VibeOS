param(
    [Parameter(Mandatory)][string]$Port,
    [string]$Profile = 'build\board27',
    [string]$PdsRoot = 'D:\App\PDS_2022.2-SP6.1',
    [string]$Output = 'build\board-uart.log',
    [string]$Expected = 'QingLong RV32IM boot OK, hash=f60ac131\r?\n',
    [int]$TimeoutSeconds = 120,
    [switch]$ExperimentalTiming
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$profilePath = if ([IO.Path]::IsPathRooted($Profile)) { $Profile } else { Join-Path $root $Profile }
$outputPath = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $root $Output }
$manifestPath = Join-Path $profilePath 'manifest.json'
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
if ((!$manifest.routed_timing_passed -and !$ExperimentalTiming) -or @($manifest.bitstreams).Count -ne 1) {
    throw 'Build this profile with prepare_board.ps1 -Build -Bitstream first.'
}
$checks = @(
    @{Path=(Join-Path $profilePath 'qinglong_board.sv'); Hash=$manifest.source_sha256},
    @{Path=(Join-Path $profilePath 'board.fdc'); Hash=$manifest.constraint_sha256},
    @{Path=(Join-Path $profilePath 'firmware.hex'); Hash=$manifest.firmware_sha256},
    @{Path=$manifest.bitstreams[0].path; Hash=$manifest.bitstreams[0].sha256}
)
foreach ($check in $checks) {
    if ((Get-FileHash -LiteralPath $check.Path -Algorithm SHA256).Hash -ne $check.Hash) {
        throw "Build provenance mismatch: $($check.Path)"
    }
}
$timingPath = Join-Path $profilePath 'report_timing\qinglong_board_top.rtr'
$timingReport = [IO.File]::ReadAllText($timingPath)
if ($timingReport -notmatch 'Design Summary : All Constraints Met\.') {
    if (!$ExperimentalTiming -or !$manifest.experimental_setup_wns_ns -or
        $manifest.experimental_setup_wns_ns -ge 0 -or
        (Get-FileHash -LiteralPath $timingPath).Hash -ne $manifest.experimental_timing_report_sha256) {
        throw "Routed timing has not passed: $timingPath"
    }
    # Explicit characterization mode; never mark this profile timing-closed.
    # Reject negative hold slack even for the setup-frequency experiment.
    $holdRows = [regex]::Matches($timingReport, '(?s)Hold Summary\([^)]*\):.*?\n\s*cpu_clk\s+cpu_clk\s+(-?\d+\.\d+)')
    if ($holdRows.Count -ne 2 -or @($holdRows | Where-Object { [double]$_.Groups[1].Value -lt 0 }).Count) {
        throw 'Experimental profile must pass hold timing in both corners.'
    }
    Write-Warning "EXPERIMENTAL frequency: setup WNS $($manifest.experimental_setup_wns_ns) ns. A successful run does not establish timing closure."
}
New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
$serial = [IO.Ports.SerialPort]::new($Port,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$serial.Handshake = [IO.Ports.Handshake]::None
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$text = [Text.StringBuilder]::new()
$timer = [Diagnostics.Stopwatch]::new()
$process = [Diagnostics.Process]::new()
$process.StartInfo.FileName = Join-Path $PdsRoot 'bin\cdt_cfg_shell.exe'
$process.StartInfo.WorkingDirectory = $root
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.CreateNoWindow = $true
$process.StartInfo.RedirectStandardInput = $true
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.RedirectStandardError = $true
$started = $false
$programLog = ''
try {
    # Listen before configuration starts: smoke prints only once at startup.
    $serial.Open()
    Write-Host "Listening on $Port at 115200 8N1; programming FPGA configuration RAM."
    [void]$process.Start()
    $started = $true
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $bitstream = $manifest.bitstreams[0].path.Replace('\','/')
    $scriptPath = (Join-Path $PSScriptRoot 'program_board.tcl').Replace('\','/')
    $process.StandardInput.WriteLine("set bitstream {$bitstream}")
    $process.StandardInput.WriteLine("source {$scriptPath}")
    $timer.Start()
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $chunk = $serial.ReadExisting()
        if ($chunk) { [void]$text.Append($chunk); Write-Host -NoNewline $chunk }
        if ($text.ToString() -match $Expected -or $process.HasExited) { break }
        Start-Sleep -Milliseconds 20
    }
    # Configuration readback disrupts a program still executing on this board.
    # Wait for the complete UART result before verification, then end the session.
    if (!$process.HasExited) {
        if ($text.ToString() -match $Expected) {
            $process.StandardInput.WriteLine('cfg_verify -device_index 0')
            $process.StandardInput.WriteLine('cfg_read_device_property -mode 3 -device_index 0')
        }
        $process.StandardInput.WriteLine('cfg_disconnect')
        $process.StandardInput.WriteLine('exit')
        $process.StandardInput.Close()
        if (!$process.WaitForExit(30000)) { throw 'Fabric Configuration verification timed out.' }
    }
    $programLog = $stdoutTask.Result + $stderrTask.Result
    if ($text.ToString() -notmatch $Expected) {
        throw 'No complete expected UART result received; inspect output and press KEY0 if needed.'
    }
    if ($process.ExitCode -ne 0 -or
        $programLog -notmatch 'Executing : cfg_program -device_index 0 successfully\.' -or
        $programLog -notmatch 'Executing : cfg_verify -device_index 0 successfully\.' -or
        $programLog -match '(?im)^\s*(ERROR|E:|Error:)') {
        throw 'Programming or verification failed; inspect the .program.log next to UART output.'
    }
    Write-Host 'FPGA programming, UART result and subsequent configuration verification passed.'
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    if ($started) {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        if (!$programLog) { $programLog = $stdoutTask.Result + $stderrTask.Result }
    }
    $process.Dispose()
    [IO.File]::WriteAllText($outputPath,$text.ToString(),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText("$outputPath.program.log",$programLog,[Text.UTF8Encoding]::new($false))
}
