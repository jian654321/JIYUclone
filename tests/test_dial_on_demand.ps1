# Phase1-v2: 假学生带 UDP 心跳 → 验证教师端按需拨号/RDP/终端
$ErrorActionPreference='Continue'
$root='C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e'
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class WA{
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c,string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L;public int T;public int R;public int B;}
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
function Shot([IntPtr]$h,[string]$out){
  $r=New-Object WA+RECT
  [WA]::GetWindowRect($h,[ref]$r) | Out-Null
  $wd=$r.R-$r.L; $ht=$r.B-$r.T
  if($wd -le 0 -or $ht -le 0){ Write-Host '  尺寸异常'; return }
  $bmp=New-Object System.Drawing.Bitmap($wd,$ht)
  $g=[System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.L,$r.T,0,0,$bmp.Size)
  $g.Dispose(); $bmp.Save($out,[System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  Write-Host ('  已截图: '+$out)
}
function ReadFrame($s){
  $h=New-Object byte[] 4
  $n=$s.Read($h,0,4)
  if($n -lt 4){ return $null }
  $len=[BitConverter]::ToUInt32($h,0)
  $b=New-Object byte[] $len
  $off=0
  while($off -lt $len){ $k=$s.Read($b,$off,$len-$off); if($k -le 0){ return $null }; $off+=$k }
  return [Text.Encoding]::ASCII.GetString($b)
}
function SendFrame($s,[string]$t){
  $b=[Text.Encoding]::ASCII.GetBytes($t)
  $h=[BitConverter]::GetBytes([UInt32]$b.Length)
  $s.Write($h,0,4); $s.Write($b,0,$b.Length)
}
Get-Process Teacher,Student -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1

Write-Host '===== Phase1-v2: 假学生 (UDP 心跳 + 15670 监听) ====='
Start-Process "$root\dist\Teacher.exe"
Start-Sleep 2
# UDP 心跳任务 (让教师端登记 FAKE 并保持 lastUDP 新鲜)
$hbRun=$true
$hb=[Threading.Thread]::new([Threading.ThreadStart]{
  $u=New-Object Net.Sockets.UdpClient
  $e=New-Object Net.IPEndPoint ([Net.IPAddress]::Loopback,15556)
  $b=[Text.Encoding]::ASCII.GetBytes('STUHB|FAKE')
  while($hbRun){ $u.Send($b,$b.Length,$e) | Out-Null; Start-Sleep -Milliseconds 1000 }
  $u.Close()
})
$hb.IsBackground=$true
$hb.Start()
# 15670 监听 (等教师按需拨号)
$lst=[System.Net.Sockets.TcpListener]::new([Net.IPAddress]::Any,15670)
$lst.Start()
$cl=$null
for($i=0;$i -lt 30;$i++){
  Start-Sleep -Milliseconds 500
  if($lst.Pending){ $cl=$lst.AcceptTcpClient(); break }
}
if($cl){
  Write-Host '  教师已按需拨号接入!'
  $st=$cl.GetStream()
  $tea=ReadFrame $st
  Write-Host ('  身份帧: "'+$tea+'"')
  SendFrame $st 'HI|FAKE'
  Start-Sleep 2
  $tp=Get-Process Teacher
  $main=$tp.MainWindowHandle
  [WA]::PostMessageW([WA]::GetDlgItem($main,118),[UInt32]0x00F5,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  Start-Sleep 2
  $cmd=ReadFrame $st
  if($cmd -eq 'RDP_START'){ Write-Host '  PASS: 收到 RDP_START' }
  elseif($null -eq $cmd){ Write-Host '  !! 未收到 RDP_START' }
  else{ Write-Host ('  收到: '+$cmd) }
  $rdp=[WA]::FindWindowW('CC_T_Rdp',$null)
  Write-Host ('  RDP 窗口: '+$rdp)
  if($rdp -ne [IntPtr]::Zero){
    [WA]::SetForegroundWindow($rdp) | Out-Null
    $w2=64;$h2=64;$sz=$w2*$h2*2
    $px=New-Object byte[] $sz
    for($k=0;$k -lt $sz;$k+=2){ $px[$k]=0x1F; $px[$k+1]=0xF8 }
    $hdr=[BitConverter]::GetBytes([UInt32]0x52434150)+[BitConverter]::GetBytes([UInt32]1)+[BitConverter]::GetBytes([UInt32]$w2)+[BitConverter]::GetBytes([UInt32]$h2)+[BitConverter]::GetBytes([UInt32]$sz)
    $st.Write($hdr,0,20); $st.Write($px,0,$sz)
    Start-Sleep 2
    [WA]::SetForegroundWindow($rdp) | Out-Null
    Shot $rdp "$root\analysis\shot_rdp_fake.png"
  }
  [WA]::PostMessageW([WA]::GetDlgItem($main,119),[UInt32]0x00F5,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  Start-Sleep 2
  $ex=[WA]::FindWindowW('CC_T_Exec',$null)
  if($ex -ne [IntPtr]::Zero){
    $inp=[WA]::GetDlgItem($ex,1201)
    $p1=[Marshal]::StringToHGlobalUni('echo FAKE-TEST')
    [WA]::SendMessageW($inp,[UInt32]0x000C,[IntPtr]::Zero,$p1) | Out-Null
    [Marshal]::FreeHGlobal($p1)
    [WA]::PostMessageW([WA]::GetDlgItem($ex,1202),[UInt32]0x00F5,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
    Start-Sleep 2
    $c3=ReadFrame $st
    Write-Host ('  收到终端命令: "'+$c3+'"')
    SendFrame $st ("EXECOUT|8`nFAKE-OK!")
    Start-Sleep 2
    $outEd=[WA]::GetDlgItem($ex,1200)
    $len=[Int32][Int64][WA]::SendMessageW($outEd,[UInt32]0x000E,[IntPtr]::Zero,[IntPtr]::Zero)
    Write-Host ('  终端输出长度: '+$len)
    if($len -gt 0){
      $sb=New-Object System.Text.StringBuilder ($len+2)
      [WA]::SendMessageW($outEd,[UInt32]0x000D,[IntPtr]($len+1),$sb) | Out-Null
      Write-Host ('  终端输出: '+$sb.ToString().Substring(0,[Math]::Min(200,$len)))
    }
    Shot $ex "$root\analysis\shot_exec_fake.png"
  }
}else{ Write-Host '  !! 教师端 15 秒内未按需拨号 (ensure_sock/dial 失败)' }
$hbRun=$false
$lst.Stop()
Get-Process Teacher -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'DONE'
