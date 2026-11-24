# S106 0a: the flag run_list adds so a failed route quits the game at once (frees its run slot) whatever its `on_fail` says.
# Dot-sourced by run_list.ps1; tools\bench\run_fail_quit_selftest.ps1 tests it. $Named = the run's own flags (route header + -Extra).
function Get-RunFailQuitFlag([string]$Named, [bool]$NoKill) {
    if ($NoKill) { return '' }
    if ($Named -match '(^|\s)--(no-)?wwe2k14_input_fail_quit(=|\s|$)') { return '' }
    # S106 0aF: a live session (live.py, the launcher's Test harness) keeps the game up after a failed boot script
    if ($Named -match '(^|\s)--wwe2k14_live=') { return '' }
    return ' --wwe2k14_input_fail_quit=true'
}
