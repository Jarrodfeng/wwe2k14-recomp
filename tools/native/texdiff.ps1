# Per-texture comparator (Session 82, harness builder): ONE batch (owned frame on, then off) that
# RenderDoc-captures the same match frame in both arms and names every texture the scene pass samples whose contents
# differ, by content hash (tonight's light cubes took two captures and hand scripts; this is those scripts, kept).
#   & .\tools\native\texdiff.ps1 -Exe C:\Users\Jarrod\wwe2k14-recomp\out\harness\bisect\exe\live_p0\wwe2k14.exe
#   & .\tools\native\texdiff.ps1 -Exe <exe> -Scale 1440            2560x1440 scene (default 720)
#   & .\tools\native\texdiff.ps1 -Analyze out\texdiff\<stamp>      only re-run the analysis on that folder's off.rdc / on.rdc
# Steps: pre-flight -> the arms one after the other (NOT -Parallel: RenderDoc names a capture by the MINUTE, so two captures in
# one minute overwrite each other; the second arm waits for a later minute) of tools\input_scripts\short\s82_texdiff_cap.txt (quick
# match replay, capture of two frames at +400 via gpu_native_debug_rdc_now, RenderDoc on the PATH and ENABLE_VULKAN_RENDERDOC_CAPTURE=1;
# 2 games, ~1-1.5 min) -> each arm's new .rdc from %TEMP%\RenderDoc moved to out\texdiff\<stamp>\{on,off}.rdc (game imagery: local
# only, gitignored) -> qrenderdoc.exe --python tools\native\texdiff_rdc.py (hashes every texture each scene draw samples,
# keyed by pixel shader hash + slot + occurrence; compares the arms) -> tools\native\texdiff.py (the table, and
# out\texdiff\<stamp>\sheet.jpg: off | on | difference for the RGBA8 ones). RenderDoc 1.46 at C:\Program Files\RenderDoc
# (memory renderdoc-capture-recipe); its first launch may show a popup that blocks the analysis (ask Jarrod once).
# Needs an s82 build (gpu_native_debug_rdc_now). Caveats: the two arms capture the same replay frame, but a replay frame
# comes in a few fixed versions (owned_verdict's note), so a texture that differs by timing shows up too - confirm a
# suspect with the shot comparison. Game runs used: 2.
param([string]$Exe = '', [string]$OnFlags = '--gpu_native_owned_frame=true', [ValidateSet(720, 1440)][int]$Scale = 720,
      [string]$Analyze = '', [int]$Top = 25)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
Set-Location $R
$rdDir = 'C:\Program Files\RenderDoc'
$rdTemp = Join-Path $env:TEMP 'RenderDoc'
if (-not (Test-Path "$rdDir\qrenderdoc.exe") -or -not (Test-Path "$rdDir\renderdoc.dll")) { "texdiff: RenderDoc is not installed at $rdDir"; exit 2 }
$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
if ($Analyze) {
    $out = [System.IO.Path]::GetFullPath((Join-Path $R $Analyze))
    if (-not ((Test-Path "$out\off.rdc") -and (Test-Path "$out\on.rdc"))) { "texdiff: $out has no off.rdc / on.rdc"; exit 2 }
} else {
    if (-not $Exe) { 'texdiff: -Exe is required'; exit 2 }
    $out = "$R\out\texdiff\$stamp"
    & "$R\tools\owned\preflight.ps1" -Exe $Exe | Where-Object { "$_" -match 'FAIL|pre-flight' } | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { exit 6 }
    $null = New-Item -ItemType Directory -Force -Path $out
    $set = "$R\out\owned_check\settings_native$Scale.json"
    if (-not (Test-Path $set)) { Set-Content -NoNewline -Path $set -Value "{`"renderer`": 2, `"frame_rate`": 1, `"resolution`": $(if ($Scale -eq 1440) { 1 } else { 0 })}" }
    $before = @{}
    if (Test-Path $rdTemp) { Get-ChildItem $rdTemp -Filter *.rdc -File | ForEach-Object { $before[$_.FullName] = $_.LastWriteTimeUtc } }
    $env:PATH = "$rdDir;$env:PATH"
    $env:ENABLE_VULKAN_RENDERDOC_CAPTURE = '1'
    $scr = 'tools\input_scripts\short\s82_texdiff_cap.txt'
    $common = "--wwe2k14_replay_play=s80_lb_quick --wwe2k14_use_launcher_settings=true --wwe2k14_native_frame=true --vsync=false --game_data_root=$R\game_data\extracted --wwe2k14_replay_dir=$R\out\replays --wwe2k14_scenario_dir=$R\out\scenarios"
    $armFlags = @{ on = "--wwe2k14_settings_file=$set $OnFlags"; off = "--wwe2k14_settings_file=$set --gpu_native_own_steps=0" }
    # RenderDoc names a capture wwe2k14_<date>_<hh.mm>_capture.rdc: two captures in the same minute OVERWRITE each other
    # (found when the first version ran both arms in parallel: 1 file for 2 captures). So the arms run one after the
    # other and the second is launched so that its capture falls in a later minute than the first's.
    $lastMinute = ''
    foreach ($arm in 'on', 'off') {
        if ($lastMinute) {
            while ((Get-Date).AddSeconds(14).ToString('HH:mm') -eq $lastMinute) { Start-Sleep -Seconds 1 }
        }
        $ro = @(& "$R\tools\bench\run_list.ps1" -Scripts @($scr) -ScriptExtras @($armFlags[$arm]) -Extra $common -Exe $Exe 2>&1)
        $ro | Where-Object { "$_" -match ' -> wwe2k14_\d+' } | ForEach-Object { "[$arm] $_" }
        $new = @(Get-ChildItem $rdTemp -Filter *.rdc -File | Where-Object { -not $before.ContainsKey($_.FullName) -or $before[$_.FullName] -ne $_.LastWriteTimeUtc } | Sort-Object LastWriteTime)
        if ($new.Count -ne 1) { "texdiff: after the $arm arm expected 1 new capture in $rdTemp, found $($new.Count) ($(($ro | Select-Object -Last 3) -join ' / '))"; Remove-Item Env:ENABLE_VULKAN_RENDERDOC_CAPTURE -ErrorAction SilentlyContinue; exit 3 }
        $lastMinute = $new[0].LastWriteTime.ToString('HH:mm')
        Move-Item -LiteralPath $new[0].FullName -Destination "$out\$arm.rdc" -Force
        "capture of the $arm arm = $($new[0].Name) -> $out\$arm.rdc"
    }
    Remove-Item Env:ENABLE_VULKAN_RENDERDOC_CAPTURE -ErrorAction SilentlyContinue
}
# --- the analysis
$minw = if ($Scale -eq 1440) { 2560 } else { 1280 }; $minh = if ($Scale -eq 1440) { 1440 } else { 720 }
$env:TEXDIFF_OFF = "$out\off.rdc"; $env:TEXDIFF_ON = "$out\on.rdc"; $env:TEXDIFF_OUT = $out
$env:TEXDIFF_MINW = "$minw"; $env:TEXDIFF_MINH = "$minh"
Remove-Item "$out\result.json", "$out\FAILED" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$rdDir\qrenderdoc.exe" -ArgumentList @('--python', "$R\tools\native\texdiff_rdc.py") -PassThru
$deadline = (Get-Date).AddMinutes(12)
while ((Get-Date) -lt $deadline -and -not (Test-Path "$out\result.json") -and -not (Test-Path "$out\FAILED") -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
if (-not (Test-Path "$out\result.json")) { "texdiff: the RenderDoc analysis produced no result ($out\analyze.log; a first-launch popup may be waiting in qrenderdoc)"; exit 4 }
py -I "$R\tools\native\texdiff.py" $out --top $Top
exit $LASTEXITCODE
