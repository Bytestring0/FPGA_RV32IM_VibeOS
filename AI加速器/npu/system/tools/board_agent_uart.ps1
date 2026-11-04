param(
 [string]$Port='COM12',
 [Parameter(Mandatory)][string]$Assets,
 [string]$Bitstream,
 [string]$PdsRoot='D:/App/pango/PDS_2025.2-ads',
 [int]$TimeoutSeconds=1800,
 [ValidateSet('none','calculate','query','timer')][string]$Task='none',
 [switch]$CoreMark,
 [uint32]$CoreMarkIterations=10000,
 [uint32]$ClockHz=102000000,
 [string]$HandoffAssets,
 [switch]$UiCoreMark,
 [switch]$UiBootOnly,
 [ValidateRange(16,256)][int]$PacketBytes=64
)
$ErrorActionPreference='Stop'
$assetRoot=(Resolve-Path -LiteralPath $Assets).Path
$known=@{'APPRES.BIN'=(Join-Path $assetRoot 'APPRES.BIN');'FONT.BIN'=(Join-Path $assetRoot 'FONT.BIN')}
foreach($file in $known.Values){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing board asset: $file"}}
Add-Type -TypeDefinition @'
using System;
public static class AgentAssetCrc {
 static readonly uint[] table=Build();
 static uint[] Build(){var t=new uint[256];for(uint n=0;n<256;n++){uint c=n;for(int k=0;k<8;k++)c=(c&1)!=0?(c>>1)^0xedb88320u:c>>1;t[n]=c;}return t;}
 public static uint Compute(byte[] bytes){uint c=0xffffffffu;foreach(byte b in bytes)c=(c>>8)^table[(c^b)&255];return c^0xffffffffu;}
}
'@
$serial=[IO.Ports.SerialPort]::new($Port,115200,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
$serial.Handshake=[IO.Ports.Handshake]::None;$serial.DtrEnable=$false;$serial.RtsEnable=$false
$serial.ReadTimeout=1000;$serial.WriteTimeout=10000
function Read-Exact([int]$Length){
 $buffer=[byte[]]::new($Length);$at=0
 while($at -lt $Length){$n=$serial.Read($buffer,$at,$Length-$at);if($n -le 0){throw 'UART closed during transfer'};$at+=$n}
 return ,$buffer
}
function Read-Ack { $a=Read-Exact 5;return @{Code=[int]$a[0];Offset=[BitConverter]::ToUInt32($a,1)} }
function Send-Asset([string]$Name,[uint32]$Expected){
 if(!$known.ContainsKey($Name)){throw "Unexpected requested asset: $Name"}
 $data=[IO.File]::ReadAllBytes($known[$Name]);if($data.Length -ne $Expected){throw "Asset size mismatch: $Name"}
 $header=[byte[]]::new(12);[BitConverter]::GetBytes([uint32]0x31525341).CopyTo($header,0)
 [BitConverter]::GetBytes([uint32]$data.Length).CopyTo($header,4);[BitConverter]::GetBytes([AgentAssetCrc]::Compute($data)).CopyTo($header,8)
 $serial.Write($header,0,12);$ack=Read-Ack;if($ack.Code -ne 6 -or $ack.Offset -ne 0){throw 'Asset header rejected'}
 $offset=0;$lastProgress=-1
 while($offset -lt $data.Length){
  $length=[Math]::Min($PacketBytes,$data.Length-$offset);$body=[byte[]]::new(8+$length)
  [BitConverter]::GetBytes([uint32]$offset).CopyTo($body,0);[BitConverter]::GetBytes([uint16]$length).CopyTo($body,4)
  [Array]::Copy($data,$offset,$body,8,$length)
  $packet=[byte[]]::new($body.Length+4);$body.CopyTo($packet,0);[BitConverter]::GetBytes([AgentAssetCrc]::Compute($body)).CopyTo($packet,$body.Length)
  $accepted=$false
  for($attempt=0;$attempt -lt 3 -and !$accepted;$attempt++){
   $serial.Write($packet,0,$packet.Length);$ack=Read-Ack
   if($ack.Code -eq 6 -and $ack.Offset -eq $offset+$length){$accepted=$true}
   elseif($ack.Code -ne 21 -or $ack.Offset -ne $offset){throw ("Unexpected packet acknowledgement asset={0} sent_offset={1} length={2} code={3} ack_offset={4}" -f $Name,$offset,$length,$ack.Code,$ack.Offset)}
  }
  if(!$accepted){throw 'Asset transfer retry limit reached'};$offset+=$length
  $progress=[int](100*$offset/$data.Length)
  if([int]($progress/10) -ne $lastProgress){$lastProgress=[int]($progress/10);Write-Host "ASSET_UPLOAD $Name $progress%"}
 }
 $done=Read-Ack;if($done.Code -ne 4 -or $done.Offset -ne $data.Length){throw 'Whole asset CRC rejected'}
 Write-Host "ASSET_UPLOAD_PASS $Name bytes=$($data.Length)"
}
$uiBenchmarkSent=$false;$program=$null;$sentTask=$false;$bootSeen=$false;$handoffEntered=$false;$handoffComplete=$false;$line=[Text.StringBuilder]::new();$all=[Text.StringBuilder]::new()
try{
 $serial.Open();Write-Host "Listening on $Port,115200 8N1"
 if($Bitstream){
  $image=(Resolve-Path -LiteralPath $Bitstream).Path.Replace('\','/')
  $program=[Diagnostics.Process]::new();$program.StartInfo.FileName=Join-Path $PdsRoot 'bin/cdt_cfg_shell.exe'
  $program.StartInfo.WorkingDirectory=Join-Path $PdsRoot 'bin';$program.StartInfo.UseShellExecute=$false;$program.StartInfo.CreateNoWindow=$true
  $program.StartInfo.RedirectStandardInput=$true;$program.StartInfo.RedirectStandardOutput=$true;$program.StartInfo.RedirectStandardError=$true
  [void]$program.Start();$stdout=$program.StandardOutput.ReadToEndAsync();$stderr=$program.StandardError.ReadToEndAsync()
  $program.StandardInput.WriteLine('cfg_set_tcl_break -flag true');$program.StandardInput.WriteLine('cfg_connect -ip 127.0.0.1 -port 65420')
  $program.StandardInput.WriteLine("cfg_add_device -file {$image}");$program.StandardInput.WriteLine('cfg_read_device_property -mode 0 -device_index 0');$program.StandardInput.WriteLine('cfg_program -device_index 0')
 }
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds){
  try{$b=$serial.ReadByte()}catch [TimeoutException]{if($program -and $program.HasExited){throw 'FPGA programmer exited before model test'};continue}
  [void]$line.Append([char]$b)
  if($b -ne 10){continue}
  $text=$line.ToString();[void]$line.Clear();[void]$all.Append($text);Write-Host -NoNewline $text
  if($text -match '^TCM_HANDOFF_ENTER'){$handoffEntered=$true}
  if($text -match '^UI_BOOT_READY' -and !$uiBenchmarkSent){if($UiCoreMark){$serial.Write("ssss`r");$uiBenchmarkSent=$true}elseif($UiBootOnly){Write-Host 'BOARD_UI_BOOT_PASS';return}}
  if($UiCoreMark -and $text -match '^COREMARK_UI cycles_hi=([0-9a-fA-F]{8}) cycles_lo=([0-9a-fA-F]{8}) valid=(\d+)'){if($Matches[3] -ne '1'){throw 'Live CoreMark UI validation failed'};Write-Host 'BOARD_COREMARK_UI_PASS';return}
  if($text -match '^TENSOR_AGENT_COOPERATIVE_CANDIDATE'){
   if($bootSeen){
    if(!$HandoffAssets -or !$handoffEntered -or $handoffComplete){throw 'Board restarted during model session'}
    $targetRoot=(Resolve-Path -LiteralPath $HandoffAssets).Path
    $known=@{'APPRES.BIN'=(Join-Path $targetRoot 'APPRES.BIN');'FONT.BIN'=(Join-Path $targetRoot 'FONT.BIN')}
    foreach($file in $known.Values){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing target firmware asset: $file"}}
    $handoffComplete=$true;Write-Host 'TCM_HANDOFF_TARGET_STARTED'
   }
   $bootSeen=$true
  }
  if($text -match '^ASSET_READY (APPRES\.BIN|FONT\.BIN) bytes=(\d+)\s*$'){Send-Asset $Matches[1] ([uint32]$Matches[2]);continue}
  if($CoreMark -and $text -match '^MEASURED_CYCLES_HEX=([0-9a-fA-F]{16})'){
   $cycles=[Convert]::ToUInt64($Matches[1],16);$seconds=$cycles/[double]$ClockHz;$captured=$all.ToString()
   if($seconds -lt 10 -or $captured -notmatch 'Correct operation validated' -or $captured -match 'Errors detected|ERROR!|Cannot validate'){throw 'CoreMark duration or CRC validation failed'}
   if($captured -notmatch ('Iterations\s*:\s*'+$CoreMarkIterations+'\s') -or $captured -notmatch 'seedcrc\s*:\s*0xe9f5' -or $captured -notmatch 'crclist\s*:\s*0xe714' -or $captured -notmatch 'crcmatrix\s*:\s*0x1fd7' -or $captured -notmatch 'crcstate\s*:\s*0x8e3a'){throw 'CoreMark iteration or CRC evidence missing'}
   $score=$CoreMarkIterations/$seconds
   Write-Host ('COREMARK_BOARD_PASS cycles={0} seconds={1:F6} coremark={2:F6} per_mhz={3:F6} execution=TCM phy_board=real' -f $cycles,$seconds,$score,($score/($ClockHz/1e6)))
   return
  }
  if($text -match 'HDMI_UI_INIT rc=(-?\d+)' -and [int]$Matches[1] -ne 0){throw 'Board HDMI initialization failed'}
  if($text -match 'NPU_MODEL_TOKEN .*decode_ticks=0x([0-9a-fA-F]{16}).*clock_hz=0x([0-9a-fA-F]{16})'){
   $ticks=[Convert]::ToUInt64($Matches[1],16);$clock=[Convert]::ToUInt64($Matches[2],16)
   if($ticks){Write-Host ('BOARD_DECODE tokens_per_second={0:F3} seconds_per_token={1:F3}' -f ($clock/[double]$ticks),($ticks/[double]$clock))}
  }
  if($text -match 'AGENT_UI_READY' -and !$sentTask -and $Task -ne 'none'){
   $keys=switch($Task){'calculate'{"`r"};'query'{"s`r"};'timer'{"ss`r"}}
   $serial.Write($keys);$sentTask=$true
  }
  if($text -match 'CANDIDATE_EXIT rc=|UNKNOWN preserve_|FAIL_'){throw 'Board reported a failed or unresolved execution'}
  if(($Task -eq 'none' -and $text -match 'AGENT_UI_READY') -or ($sentTask -and $text -match 'AGENT_STATUS=')){
   if($all.ToString() -notmatch 'NPU_MODEL_COMPLETE'){throw 'Full-model smoke did not complete'}
   if($sentTask -and $text -notmatch 'AGENT_STATUS=(FINISHED|CLARIFY)'){throw 'Agent task did not finish successfully'}
   if($Task -eq 'calculate' -and $all.ToString() -notmatch 'MODEL_REAL_RECEIPT .*60'){throw 'Real calculation receipt missing'}
   Write-Host 'BOARD_MODEL_AGENT_SESSION_COMPLETE';return
  }
 }
 throw 'Board run timed out before complete model and application result'
}finally{
 if($program -and !$program.HasExited){$program.StandardInput.WriteLine('cfg_disconnect');$program.StandardInput.WriteLine('exit');$program.StandardInput.Close();[void]$program.WaitForExit(10000)}
 if($program -and $program.HasExited){$programText=$stdout.Result+$stderr.Result;Write-Host $programText}
 if($serial.IsOpen){$serial.Close()};$serial.Dispose()
}
