# Read-only: Windows event log entries that crash triage uses (tools\triage\triage.py calls this).
#   Application log, event 1000 (application error) for wwe2k14.exe: exception code, faulting module, fault offset,
#   plus the identity of the crashed process (path, process id, process start time, PE time stamp of the exe), so
#   triage can match an event to the run that produced it instead of to a time window.
#   System log, provider nvlddmkm, event 153 (NVIDIA GPU reset / channel error, see PLAYBOOK Session 2).
# Output: one JSON object on stdout {"app": [...], "gpu": [...]} with local ISO times.
# Usage: pwsh -NoProfile -File tools\triage\winevents.ps1 -Start 2026-10-07T20:00:00 -End 2026-10-07T21:00:00
param([Parameter(Mandatory)][datetime]$Start, [Parameter(Mandatory)][datetime]$End)
$ErrorActionPreference = 'Stop'
$app = @()
try {
    $app = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $Start; EndTime = $End } -ErrorAction Stop |
        Where-Object { $_.Message -match 'wwe2k14' } | ForEach-Object {
            $m = $_.Message
            $get = { param($re) if ($m -match $re) { $Matches[1].Trim() } else { '' } }
            # 'Faulting application start time' is a FILETIME in hex (100 ns ticks since 1601 UTC)
            $startHex = & $get 'Faulting application start time: 0x([0-9a-fA-F]+)'
            $pstart = ''
            if ($startHex) {
                try { $pstart = [DateTime]::FromFileTime([Convert]::ToInt64($startHex, 16)).ToString('yyyy-MM-ddTHH:mm:ss.fff') } catch { $pstart = '' }
            }
            [pscustomobject]@{
                time    = $_.TimeCreated.ToString('yyyy-MM-ddTHH:mm:ss.fff')
                app     = & $get 'Faulting application name: ([^,\r\n]+)'
                stamp   = & $get 'Faulting application name: [^\r\n]*?time stamp: (0x[0-9a-fA-F]+)'
                module  = & $get 'Faulting module name: ([^,\r\n]+)'
                code    = & $get 'Exception code: (0x[0-9a-fA-F]+)'
                offset  = & $get 'Fault offset: (0x[0-9a-fA-F]+)'
                pid     = & $get 'Faulting process id: (0x[0-9a-fA-F]+)'
                pstart  = $pstart
                path    = & $get 'Faulting application path: ([^\r\n]+)'
                modpath = & $get 'Faulting module path: ([^\r\n]+)'
            }
        })
} catch { if ($_.FullyQualifiedErrorId -notmatch 'NoMatchingEventsFound') { $app = @() } }
$gpu = @()
try {
    $gpu = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'nvlddmkm'; Id = 153; StartTime = $Start; EndTime = $End } -ErrorAction Stop |
        ForEach-Object { $_.TimeCreated.ToString('yyyy-MM-ddTHH:mm:ss.fff') })
} catch { $gpu = @() }
[pscustomobject]@{ app = $app; gpu = $gpu } | ConvertTo-Json -Depth 4 -Compress
