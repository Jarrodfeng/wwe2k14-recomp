# S102 U3: the checklist function's own test (does not run release.ps1). Exit 0 = all cases right. It is NON-BLOCKING: no case may throw or return an empty line.
# Planted twin: 'amd: none' must NOT count as a report (a version that counted it would show amd in the line).
. (Join-Path $PSScriptRoot 'vendor_reports.ps1')
$bad = 0
function Case([string]$name, [string]$reports, [string]$waive, [string[]]$mustHave, [string[]]$mustNot) {
    $r = Get-VendorReportLine $reports $waive 6>$null
    $ok = [bool]$r
    foreach ($m in $mustHave) { if ($r -notlike "*$m*") { $ok = $false } }
    foreach ($m in $mustNot) { if ($r -like "*$m*") { $ok = $false } }
    if ($ok) { Write-Host "PASS  $name" } else { Write-Host "FAIL  $name : '$r'"; $script:bad++ }
}
Case 'nothing recorded' '' '' @('none recorded') @('waived')
Case 'all three' 'amd: A 11-14; intel: B 11-14; arm: C 11-14' '' @('amd: A 11-14', 'intel: B 11-14', 'arm: C 11-14') @('missing', 'none recorded')
Case 'arm missing is named' 'amd: A; intel: B' '' @('amd: A', 'intel: B', 'missing: arm') @('none recorded')
Case 'PLANT amd: none is not a report' 'amd: none; intel: B; arm: n/a' '' @('intel: B', 'missing: amd, arm') @('amd: none', 'arm: n/a')
Case 'only fake reports = none recorded' 'amd: none; intel: -; arm: no' '' @('none recorded') @('amd: none')
Case 'waiver is recorded' '' 'no Mali tester this week' @('none recorded', 'waived: no Mali tester this week') @()
Case 'waiver beside reports' 'amd: A' 'no Intel box' @('amd: A', 'waived: no Intel box') @()
exit $(if ($bad) { 1 } else { 0 })
