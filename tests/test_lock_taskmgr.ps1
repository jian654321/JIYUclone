# 锁屏 + 任务管理器看门狗实测
$ErrorActionPreference='Continue'
$root='C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e'
Set-Location $root
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
Get-Process Teacher,Student,Taskmgr -ErrorAction SilentlyContinue | Stop-Process -Force
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
Write-Host '=== 发送 LOCK ==='
SendFrame $s 'LOCK'
Start-Sleep 2
Write-Host '=== 启动任务管理器 ==='
Start-Process "$env:WINDIR\System32\Taskmgr.exe"
Start-Sleep 2
$tm=Get-Process Taskmgr -ErrorAction SilentlyContinue
if($tm){ Write-Host ('  Taskmgr 仍在运行 (pid '+$tm.Id+') — 等待看门狗...') } else { Write-Host '  Taskmgr 未启动成功' }
Start-Sleep 2
$tm2=Get-Process Taskmgr -ErrorAction SilentlyContinue
if($tm2){ Write-Host '  !! FAIL: Taskmgr 1.5 秒后仍存活' } else { Write-Host '  PASS: Taskmgr 已被看门狗结束' }
Start-Process "$env:WINDIR\System32\Taskmgr.exe"
Start-Sleep 2
$tm3=Get-Process Taskmgr -ErrorAction SilentlyContinue
if($tm3){ Write-Host '  复测: Taskmgr 再次被启动, 等待看门狗...' } else { Write-Host '  复测: 未启动' }
Start-Sleep 2
$tm4=Get-Process Taskmgr -ErrorAction SilentlyContinue
if($tm4){ Write-Host '  !! FAIL: 仍存活' } else { Write-Host '  PASS: 复测同样被结束' }
Write-Host '=== 解除锁定 ==='
SendFrame $s 'UNLOCK'
Start-Sleep 1
Start-Process "$env:WINDIR\System32\Taskmgr.exe"
Start-Sleep 2
$tm5=Get-Process Taskmgr -ErrorAction SilentlyContinue
if($tm5){ Write-Host '  PASS: 解锁后 Taskmgr 正常存活 (看门狗已停止)' } else { Write-Host '  解锁后 Taskmgr 未存活 (看门狗未停?)' }
Get-Process Student,Taskmgr -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'DONE'
