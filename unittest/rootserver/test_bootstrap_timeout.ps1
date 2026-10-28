param(
  [Parameter(Mandatory = $true)][string]$Seekdb,
  [Parameter(Mandatory = $true)][string]$Mysql,
  [Parameter(Mandatory = $true)][string]$WorkDir,
  [int]$Port = 32942,
  [string]$RpcTimeout = '1ms',
  [int]$WaitSeconds = 180
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $WorkDir) { throw "WorkDir must not already exist: $WorkDir" }
New-Item -ItemType Directory -Path $WorkDir | Out-Null
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
$Seekdb = (Resolve-Path -LiteralPath $Seekdb).Path
$Mysql = (Resolve-Path -LiteralPath $Mysql).Path
$argsList = @('--nodaemon', '--base-dir', ('"' + $WorkDir + '"'), '--port', $Port,
  '--parameter', 'memory_budget=1G', '--parameter', 'log_disk_size=2G',
  '--parameter', ("rpc_timeout=" + $RpcTimeout))
$p = Start-Process -FilePath $Seekdb -ArgumentList $argsList -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $WorkDir 'stdout.txt') `
  -RedirectStandardError (Join-Path $WorkDir 'stderr.txt')
$ready = $false
try {
  $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
  while ([DateTime]::UtcNow -lt $deadline) {
    $p.Refresh()
    if ($p.HasExited) { break }
    $listener = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
      Where-Object { $_.OwningProcess -eq $p.Id }
    if (-not $listener) { Start-Sleep -Seconds 1; continue }
    # A listening socket can appear before MySQL is ready. PowerShell 5 treats
    # redirected native stderr as an error, so keep this probe non-terminating.
    $previousErrorPreference = $ErrorActionPreference
    try {
      $ErrorActionPreference = 'Continue'
      $result = & $Mysql --protocol=TCP --host=127.0.0.1 --port=$Port --user=root --connect-timeout=2 --batch --skip-column-names -e 'SELECT 1' 2>$null
      $probeExitCode = $LASTEXITCODE
    } finally {
      $ErrorActionPreference = $previousErrorPreference
    }
    if ($probeExitCode -eq 0 -and ($result -join '').Trim() -eq '1') { $ready = $true; break }
    Start-Sleep -Seconds 1
  }
  if (-not $ready) {
    $p.Refresh()
    $state = if ($p.HasExited) { "server exited with code $($p.ExitCode)" } else { 'server did not become ready' }
    Write-Error "$state. Inspect $WorkDir\log\seekdb.log"
  }
  $log = Join-Path $WorkDir 'log\seekdb.log'
  if (-not (Select-String -LiteralPath $log -SimpleMatch 'STEP_3.9:init_global_stat execute success' -Quiet)) {
    throw "Server became ready without the expected bootstrap milestone in $log"
  }
  & $Mysql --protocol=TCP --host=127.0.0.1 --port=$Port --user=root --connect-timeout=2 --batch --skip-column-names -e 'CREATE DATABASE bootstrap_timeout_test; CREATE TABLE bootstrap_timeout_test.t (id INT PRIMARY KEY); INSERT INTO bootstrap_timeout_test.t VALUES (42); SELECT id FROM bootstrap_timeout_test.t;' | Out-String | ForEach-Object { if ($_.Trim() -ne '42') { throw "Unexpected SQL result: $_" } }
  if ($LASTEXITCODE -ne 0) { throw 'SQL smoke test failed' }
  Write-Output "PASS: bootstrap completed and SQL succeeded. Logs: $WorkDir"
} finally {
  $p.Refresh()
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
