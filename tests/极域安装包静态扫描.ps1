# Static ASCII string scan of PE files (read-only, never executes the targets)
$ErrorActionPreference = 'Stop'
$files = @(
  'C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e\analysis\pe\EduMythwareMonitor.exe',
  'C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e\analysis\pe\EduShellInstall.exe',
  'C:\Users\jiank\.zcode\workspace\e62ab6c9-1f6a-4a85-9656-9e4de3af479e\analysis\pe\PackTeacher.exe'
)
$kw = @('port','5555','multicast','mythware','udp','tcp','channel','239.','broadcast','socket','topdomain')
foreach($f in $files){
  if(-not (Test-Path -LiteralPath $f)){ Write-Host ('MISS ' + $f); continue }
  $b = [IO.File]::ReadAllBytes($f)
  $s = [Text.Encoding]::ASCII.GetString($b)
  $ms = [regex]::Matches($s, '[\x20-\x7E]{5,}')
  Write-Host ('==== ' + [IO.Path]::GetFileName($f) + '  strings=' + $ms.Count)
  $n = 0
  foreach($m in $ms){
    $v = $m.Value.ToLower()
    $hit = $false
    foreach($k in $kw){ if($v.Contains($k)){ $hit = $true; break } }
    if($hit){
      $len = [Math]::Min(140, $m.Value.Length)
      Write-Host ('  ' + $m.Value.Substring(0, $len))
      $n++
      if($n -ge 80){ break }
    }
  }
  Write-Host ''
}
Write-Host 'SCAN-DONE'
