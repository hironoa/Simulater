# check_jp_path.ps1 - run a CSV whose folder and file name are Japanese, on Windows.
# ASCII only: the Japanese name is built from code points so that this file reads the same on any code page.
#   powershell -NoProfile -ExecutionPolicy Bypass -File check_jp_path.ps1 <sim.exe> <input.csv> <work dir>
# Exit 0 when sim ran the file, wrote the result under the same name, and printed the name as UTF-8.
param([string]$Sim, [string]$Csv, [string]$Work)
$n = -join ([char]0x65E5, [char]0x672C, [char]0x8A9E)
$d = Join-Path $Work $n
New-Item -ItemType Directory -Force -Path $d | Out-Null
$in = Join-Path $d ($n + '.csv')
Copy-Item -LiteralPath $Csv -Destination $in -Force
$out = Join-Path $d 'out'
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Sim
$psi.Arguments = 'run "' + $in + '" -o "' + $out + '"'
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [Text.Encoding]::UTF8
$p = [System.Diagnostics.Process]::Start($psi)
$so = $p.StandardOutput.ReadToEnd()
$se = $p.StandardError.ReadToEnd()
$p.WaitForExit()
$wrote = Test-Path -LiteralPath (Join-Path $out ($n + '.csv'))
$shown = ($so + $se).Contains($n)
if ($p.ExitCode -eq 0 -and $wrote -and $shown) { exit 0 }
Write-Output ("exit=" + $p.ExitCode + " wrote=" + $wrote + " shown=" + $shown)
Write-Output $so
Write-Output $se
exit 1
