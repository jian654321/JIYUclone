# 密码框全流程验证: LOCK → 5xF8 → 密码框出现且不被覆盖 → 输密码 → 解锁
$ErrorActionPreference='Continue'
$root='C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e'
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class WP{
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c,string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,StringBuilder l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L;public int T;public int R;public int B;}
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
function Shot([IntPtr]$h,[string]$out){
  $r=New-Object WP+RECT
  [WP]::GetWindowRect($h,[ref]$r) | Out-Null
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
Get-Process Student,Taskmgr -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
Write-Host '=== 启动学生端 + 假教师 + LOCK ==='
Start-Process "$root\dist\Student.exe"
Start-Sleep 3
$c=New-Object Net.Sockets.TcpClient
$c.Connect('127.0.0.1',15670)
$s=$c.GetStream()
SendFrame $s 'TEA|127.0.0.1'
$hi=ReadFrame $s
SendFrame $s 'LOCK'
Start-Sleep 2
$lock=[WP]::FindWindowW('CC_Stu_Lock',$null)
Write-Host ('锁屏窗口: '+$lock)
if($lock -eq [IntPtr]::Zero){ Write-Host '!! 锁屏未出现'; exit }

Write-Host '=== 模拟连按 5 次 F8 (向锁屏窗口投递 WM_KEYDOWN) ==='
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class WQ{
  public delegate bool EnumCB(IntPtr h,IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCB cb,IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder sb,int n);
}
"@
for($i=0;$i -lt 5;$i++){ [WP]::PostMessageW($lock,[UInt32]0x0100,[IntPtr]0x77,[IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 150 }
Start-Sleep 2
$sp=Get-Process Student
Write-Host ('学生进程: '+$sp.Id)
$script:pw=[IntPtr]::Zero
$cb=[WQ+EnumCB]{ param($h,$l)
  $p2=0
  [WQ]::GetWindowThreadProcessId($h,[ref]$p2) | Out-Null
  if($p2 -eq $sp.Id){
    $sb=New-Object System.Text.StringBuilder 256
    [WQ]::GetWindowTextW($h,$sb,256) | Out-Null
    $tt=$sb.ToString()
    Write-Host ('  窗口: hwnd='+$h+' title="'+$tt+'"')
    if($tt -eq '紧急解除全屏'){ $script:pw=$h }
  }
  return $true
}
[WQ]::EnumWindows($cb,[IntPtr]::Zero) | Out-Null
$pw=$script:pw
Write-Host ('密码框: '+$pw)
if($pw -eq [IntPtr]::Zero){ Write-Host '!! 密码框未弹出'; SendFrame $s 'UNLOCK'; exit }

Write-Host '=== 验证密码框可被输入 (向编辑框投递密码字符) ==='
$ed=[WP]::GetDlgItem($pw,2003)
$okb=[WP]::GetDlgItem($pw,2001)
foreach($ch in '6','5','4','3','2','1'){
  $k=[int][char]$ch
  [WP]::PostMessageW($ed,[UInt32]0x0102,[IntPtr]$k,[IntPtr]0) | Out-Null   # WM_CHAR
}
Start-Sleep 1
$okb2=[WP]::GetDlgItem($pw,2001)
[WP]::PostMessageW($okb2,[UInt32]0x00F5,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null  # 点确认
Start-Sleep 2
$lock2=[WP]::FindWindowW('CC_Stu_Lock',$null)
$pw2=[WP]::FindWindowW($null, '紧急解除全屏')
if($lock2 -eq [IntPtr]::Zero -and $pw2 -eq [IntPtr]::Zero){
  Write-Host '  PASS: 密码正确 → 锁屏解除, 密码框关闭'
}else{ Write-Host ('  FAIL: lock='+$lock2+' pw='+$pw2) }
Shot $lock2 "$root\analysis\shot_after_pw.png"
SendFrame $s 'UNLOCK'
Get-Process Student -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'DONE'
