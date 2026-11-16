# v1.4.1 HI 首帧修复闭环验证 (通过进程 MainWindowHandle 截图)
$ErrorActionPreference='Continue'
$root='C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e'
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class W{
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L;public int T;public int R;public int B;}
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int cmd);
}
"@
function Shot($p,$out){
  $p.Refresh()
  $h=$p.MainWindowHandle
  if($h -eq [IntPtr]::Zero){ Write-Host ('  无主窗口: '+$p.ProcessName); return }
  Write-Host ('  窗口标题: '+$p.MainWindowTitle)
  [W]::ShowWindow($h,9) | Out-Null
  [W]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 700
  $r=New-Object W+RECT
  [W]::GetWindowRect($h,[ref]$r) | Out-Null
  $wd=$r.R-$r.L; $ht=$r.B-$r.T
  if($wd -le 0 -or $ht -le 0){ Write-Host '  窗口尺寸异常'; return }
  $bmp=New-Object System.Drawing.Bitmap($wd,$ht)
  $g=[System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.L,$r.T,0,0,$bmp.Size)
  $g.Dispose()
  $bmp.Save($out,[System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host ('  已截图: '+$out)
}
Get-Process Teacher,Student -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 1
Start-Process "$root\dist\Teacher.exe"
Start-Sleep 2
Start-Process "$root\dist\Student.exe"
Start-Sleep 16
$t=Get-Process Teacher -ErrorAction SilentlyContinue
$s=Get-Process Student -ErrorAction SilentlyContinue
if($t){ Shot $t "$root\analysis\shot_teacher.png" }
if($s){ Shot $s "$root\analysis\shot_student.png" }
Get-Process Teacher,Student -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'DONE'
