# S102 TS1 test fixture: stands in for ssh / scp. First argument: 'hang' (default) sleeps forever and ignores its stdin; 'fail' exits 1 at once;
# 'sthang' answers the bench START (an ssh whose -EncodedCommand holds Start-ScheduledTask: prints "task: Running") and every scp (exit 0), and hangs on anything else
# (the status poll). Used by wer_timeout_test.ps1.
# (No param block: ssh's own -o options must not be bound as parameters.)
$mode = if ($args.Count -gt 0) { $args[0] } else { 'hang' }
if ($mode -eq 'fail') { [Console]::Error.WriteLine('fake ssh: connection refused'); exit 1 }
if ($mode -eq 'sthang') {
  $enc = $null
  foreach ($a in $args) { if ($a -match '-EncodedCommand (\S+)') { $enc = $Matches[1] } }
  if ($null -eq $enc) { exit 0 }   # an scp
  $code = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($enc))
  if ($code -match 'Start-ScheduledTask') { 'task: Running'; 'status: '; exit 0 }
}
if ($mode -eq 'stfresh') {   # like sthang, but the FIRST status poll answers "status: running <now>" (a fresh status), every later poll hangs
  $enc = $null
  foreach ($a in $args) { if ($a -match '-EncodedCommand (\S+)') { $enc = $Matches[1] } }
  if ($null -eq $enc) { exit 0 }
  $code = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($enc))
  $flag = Join-Path ([IO.Path]::GetTempPath()) "wer_fake_stfresh_$($env:WWE_REMOTE_FAKE_STATE).flag"
  if ($code -match 'Start-ScheduledTask') { Remove-Item $flag -ErrorAction SilentlyContinue; 'task: Running'; 'status: '; exit 0 }
  if (-not (Test-Path $flag)) { Set-Content $flag 'x'; 'status: running ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'); 'task: Running'; exit 0 }
}
Start-Sleep -Seconds 3600
