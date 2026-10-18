# Session 16 item 2: multi-angle re-shoot. Replays out\replays\NAME.rec once per shot (CineCam path files, relative to
# out\harness\cinecam\paths\), each pass capturing its shot as a frame-locked clip (src\reshoot.h), one game at a time,
# fast-forwarded (the clips still play at real speed: one video frame per guest frame). Output out\reshoots\NAME\:
#   shot01.mp4, shot01.json (sidecar: replay, shot, anchor, anchor frame, first / last guest frame, run), shot01_clip.json
#   (the clip's own stats), shot02...
#   pwsh -NoProfile -File tools\replay\reshoot.ps1 -Name NAME -Shots s16_shotA.txt,s16_shotB.txt [-Anchor clock|frame:N|step5:N] [-Hash] [-More '--x=1']
# Session 17: -Director: NAME is a director script (out\director\NAME.txt) instead of a recording.
# -Hash: per-frame state hashes (--wwe2k14_det_hash) so tools\determinism\det_compare.py can compare the passes.
# Session 18: -Audio: after the shots, one more replay pass at normal speed records the game's sound
# (--wwe2k14_audio_capture -> audio.wav + audio.stamps.csv, until the last shot's last frame + 1 s), then
# tools\replay\audio_mux.py writes shotNN_av.mp4 (the clip with its sound; shotNN.mp4 stays silent). -AudioOnly: only
# that (the shots exist already).
param([Parameter(Mandatory)][string]$Name, [string[]]$Shots = @(), [string]$Anchor = 'clock',
      [switch]$Hash, [string]$More = '', [int]$WaitSeconds = 900, [switch]$Director, [switch]$Audio,
      [switch]$AudioOnly)
$R = 'C:\Users\Jarrod\wwe2k14-recomp'
$shotList = @($Shots | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
# Session 17: -Director re-shoots a director script (out\director\NAME.txt, src\director.h) instead of a recording.
if ($Director) {
    if (-not (Test-Path -LiteralPath "$R\out\director\$Name.txt")) { "ABORT: no director script out\director\$Name.txt"; exit 2 }
} elseif (-not (Test-Path -LiteralPath "$R\out\replays\$Name.rec")) { "ABORT: no recording out\replays\$Name.rec"; exit 2 }
$dir = "$R\out\reshoots\$Name"
[void][IO.Directory]::CreateDirectory($dir)
$k = 0
if ($AudioOnly) { $shotList = @() }
foreach ($shot in $shotList) {
    $k++
    $tag = 'shot{0:D2}' -f $k
    $label = "${tag}_" + [IO.Path]::GetFileNameWithoutExtension($shot)
    $source = if ($Director) { "director:$Name" } else { $Name }
    $extra = "--wwe2k14_reshoot=$source --wwe2k14_reshoot_shot=$shot --wwe2k14_reshoot_label=$label --wwe2k14_cinecam_path_anchor=$Anchor"
    if ($Hash) { $extra += ' --wwe2k14_det_hash=default,objects --wwe2k14_det_hash_raw_every=30' }
    if ($More) { $extra += " $More" }
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @("$R\tools\replay\reshoot_pass.txt") -Extra $extra -WaitSeconds $WaitSeconds 2>&1
    $line = @($out | Where-Object { "$_" -match '->' }) | Select-Object -Last 1
    "$tag ($shot): $line"
    if ("$line" -notmatch 'wwe2k14_(\d+)') { continue }
    $run = [int]$Matches[1]
    $log = "$R\out\build\win-amd64-release\logs\wwe2k14_$run.log"
    $anchorFrame = $null
    $m = Select-String -LiteralPath $log -Pattern '\[cinecam\]\[path\] anchor \S+ at frame (\d+)' | Select-Object -First 1
    if ($m) { $anchorFrame = [int64]$m.Matches[0].Groups[1].Value }
    $clipJson = Join-Path $dir ('{0:D3}_clip_{1}.json' -f $run, $label)
    $clipMp4 = [IO.Path]::ChangeExtension($clipJson, '.mp4')
    $clip = $null
    if (Test-Path -LiteralPath $clipJson) { $clip = Get-Content -Raw -LiteralPath $clipJson | ConvertFrom-Json }
    if (Test-Path -LiteralPath $clipMp4) { Copy-Item -LiteralPath $clipMp4 (Join-Path $dir "$tag.mp4") -Force }
    if ($clip) { Copy-Item -LiteralPath $clipJson (Join-Path $dir "${tag}_clip.json") -Force }
    $side = [ordered]@{ replay = $Name; shot = $shot; anchor = $Anchor; anchor_frame = $anchorFrame; run = $run
                        clip = "$tag.mp4"; first_guest_frame = $(if ($clip) { $clip.first_guest_frame }); last_guest_frame = $(if ($clip) { $clip.last_guest_frame })
                        video_frames = $(if ($clip) { $clip.video_frames }); width = $(if ($clip) { $clip.width }); height = $(if ($clip) { $clip.height })
                        frame_locked = $(if ($clip) { $clip.frame_locked }) }
    [IO.File]::WriteAllText((Join-Path $dir "$tag.json"), ($side | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
    "  sidecar $dir\$tag.json: anchor frame $anchorFrame, guest frames $($side.first_guest_frame)..$($side.last_guest_frame), $($side.video_frames) video frames"
}

if ($Audio -or $AudioOnly) {
    $last = 0
    foreach ($j in Get-ChildItem -LiteralPath $dir -Filter 'shot??.json') {
        $sj = Get-Content -Raw -LiteralPath $j.FullName | ConvertFrom-Json
        if ($sj.last_guest_frame -and [int64]$sj.last_guest_frame -gt $last) { $last = [int64]$sj.last_guest_frame }
    }
    if (-not $last) { "audio: no shot sidecars with guest frames in $dir"; exit 2 }
    $until = $last + 60
    $source = if ($Director) { "--wwe2k14_director=$Name" } else { "--wwe2k14_replay_play=$Name" }
    $extra = "$source --wwe2k14_audio_capture=$dir\audio.wav --wwe2k14_audio_capture_until=$until"
    if ($More) { $extra += " $More" }
    "audio pass (normal speed, until guest frame $until) ..."
    $out = & "$R\tools\bench\run_list.ps1" -Scripts @("$R\tools\replay\reshoot_pass.txt") -Extra $extra -RealTime -WaitSeconds $WaitSeconds 2>&1
    $line = @($out | Where-Object { "$_" -match '->' }) | Select-Object -Last 1
    "audio: $line"
    py "$R\tools\replay\audio_mux.py" $Name
}
