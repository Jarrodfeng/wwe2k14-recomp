# S102 lane U3: the release-candidate checklist line "one clean report per vendor from the tester ring". Dot-sourced by release.ps1. NON-BLOCKING (decided by the
# main session): it prints the checklist and returns the text that goes into last_build.json and the final summary line, so it reaches the ONE pre-publish line to Jarrod.
# It never refuses a release.
#   -VendorReports "amd: <who/when>; intel: <who/when>; arm: <who/when>"   one clean report (a zip whose log has no [rdr][PROBLEM] / BLACK / HANG line) per vendor
#   -WaiveVendorReports "<reason>"                                          why a vendor has none
# "amd: none" (or n/a, -, empty) is NOT a report. Which lines make a report clean: docs\research\part2\R9_title_card_and_reports.md section 4.4.
# Simulated cards (tools\vkprofiles) do NOT replace this line.
# Returns: "vendor reports: amd: ...; intel: ...; arm: ..." | "vendor reports: none recorded (missing: ...)" | "... / waived: <text>"
function Get-VendorReportLine([string]$Reports, [string]$Waive) {
    Write-Host 'release: CHECKLIST  one clean report per vendor from the tester ring (AMD, Intel, Arm/Mali phone; NVIDIA and Adreno are covered by the author).'
    $have = @()
    foreach ($part in ($Reports -split ';')) {
        $p = $part.Trim()
        if ($p -notmatch '^(?i)(amd|intel|arm)\s*:\s*(.+)$') { continue }
        $vendor = $Matches[1].ToLower(); $what = $Matches[2].Trim()
        if ($what -match '^(?i)(-+|(none|n/?a|no|nil|missing)\b.*)$') { continue }   # "amd: none" is not a report
        $have += "${vendor}: $what"
    }
    $vendors = @($have | ForEach-Object { ($_ -split ':')[0] })
    $missing = @('amd', 'intel', 'arm' | Where-Object { $vendors -notcontains $_ })
    $line = if ($have.Count) { 'vendor reports: ' + ($have -join '; ') } else { 'vendor reports: none recorded' }
    if ($missing.Count -and $have.Count) { $line += " (missing: $($missing -join ', '))" }
    if ($Waive) { $line += " / waived: $Waive" }
    Write-Host "release: $line"
    return $line
}
