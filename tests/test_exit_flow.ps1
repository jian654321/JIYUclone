# 退出流程验证: 退出程序 → 密码框 → 654321 → 确认 → 进程必须退出且密码框关闭
$ErrorActionPreference='Continue'
$root='C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e'
Set-Location $root
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class WE{
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c,string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@
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
Get-Process Student -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
Write-Host '=== 启动学生端 + 假教师接入 ==='
Start-Process "$root\dist\Student.exe"
Start-Sleep 3
$c=New-Object Net.Sockets.TcpClient
$c.Connect('127.0.0.1',15670)
$s=$c.GetStream()
SendFrame $s 'TEA|127.0.0.1'
$hi=ReadFrame $s
Write-Host ('  握手: '+$hi)
$sp=Get-Process Student
$main=$sp.MainWindowHandle
Write-Host '=== 点击 退出程序 ==='
[WE]::PostMessageW([WE]::GetDlgItem($main,901),[UInt32]0x00F5,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
Start-Sleep 2
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class WF{
  public delegate bool EnumCB(IntPtr h,IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCB cb,IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder sb,int n);
}
"@
$script:pw=[IntPtr]::Zero
$cb=[WF+EnumCB]{ param($h,$l)
  $p2=0
  [WF]::GetWindowThreadProcessId($h,[ref]$p2) | Out-Null
  if($p2 -eq $sp.Id){
    $sb=New-Object System.Text.StringBuilder 256
    [WF]::GetWindowTextW($h,$sb,256) | Out-Null
    $tt=$sb.ToString()
    Write-Host ('  窗口: title="'+$tt+'"')
    if($tt -eq '退出被控端'){ $script:pw=$h }
  }
  return $true
}
[WF]::EnumWindows($cb,[IntPtr]::Zero) | Out-Null
$pw=$script:pw
Write-Host ('密码框: '+$pw)
if($pw -eq [IntPtr]::Zero){ Write-Host '!! 密码框未弹出'; exit }
Write-Host '=== 输入 654321 并确认 ==='
$ed=[WE]::GetDlgItem($pw,2003)
foreach($ch in '6','5','4','3','2','1'){
  [WE]::PostMessageW($ed,[UInt32]0x0102,[IntPtr][int][char]$ch,[IntPtr]0) | Out-Null
  Start-Sleep -Milliseconds 50
}
[WE]::PostMessageW([WE]::GetDlgItem($pw,2001),[UInt32]0x00F5,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
Start-Sleep 3
$alive=Get-Process Student -ErrorAction SilentlyContinue
if(-not $alive){ Write-Host '  PASS: 进程已退出, 密码框已随之关闭' }
else{ Write-Host '  !! FAIL: 进程仍存活' }
Write-Host 'DONE'
