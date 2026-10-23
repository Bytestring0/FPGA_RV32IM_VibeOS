param(
    [Parameter(Mandatory)][string]$Port,
    [string]$Output = 'build\board-uart.log',
    [int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$path = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $root $Output }
New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($path)) | Out-Null
$serial = [IO.Ports.SerialPort]::new($Port, 115200, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
$serial.Handshake = [IO.Ports.Handshake]::None
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$text = [Text.StringBuilder]::new()
$timer = [Diagnostics.Stopwatch]::new()
try {
    $serial.Open()
    Write-Host "Listening on $Port, 115200 8N1. Press board KEY0 to restart the loaded program."
    $timer.Start()
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $chunk = $serial.ReadExisting()
        if ($chunk) {
            [void]$text.Append($chunk)
            Write-Host -NoNewline $chunk
            # Match the full line, not a partial hex value arriving in a chunk.
            if ($text.ToString() -match 'MEASURED_CYCLES_HEX=[0-9a-fA-F]{16}\r?\n' -or
                $text.ToString() -match 'QingLong RV32IM boot OK, hash=f60ac131\r?\n') { break }
        }
        Start-Sleep -Milliseconds 20
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    [IO.File]::WriteAllText($path, $text.ToString(), [Text.UTF8Encoding]::new($false))
    Write-Host "Saved UART output: $path"
}
if (!$text.Length) { throw 'No UART data received. Check port, bitstream, KEY0 and board revision.' }
if ($text.ToString() -notmatch 'MEASURED_CYCLES_HEX=[0-9a-fA-F]{16}\r?\n|QingLong RV32IM boot OK, hash=f60ac131\r?\n') {
    throw 'UART capture timed out before a complete benchmark or smoke result.'
}
