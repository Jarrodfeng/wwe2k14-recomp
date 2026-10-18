# tools/bench

Unattended benchmark and A/B harness (Session 6; specs B3-B8 in `docs/optimization_candidates.md`, section
"Unattended benchmark and A/B harness", with the reviewers' corrections). Everything these tools write goes under
`out\bench\` (gitignored): user data copies, manifests, reports, variant references. Nothing from the game is read
except our own run output (logs, timelines, frame CSVs, captures), and nothing they produce is committed.

| Tool | What it does |
|---|---|
| `bench_report.py report RUN...` | Per-run scene report, per-CP-frame CSV, error histogram, hard-fail check, optional in-run A/B (`--ab NAME`) |
| `bench_report.py summary RUN...` | One table per scene across runs (arm, camera variant, key metrics), arms compared within a variant, settings that differ, script sync spread. `run_bench.ps1` prints this at the end |
| `bench_report.py classify RUN...` | Entrance camera variant of every titantron scene (B3) |
| `bench_report.py refs --scene S --variant V RUN...` | Builds or refreshes a variant reference curve in `out\bench\reference\` |
| `bench_report.py verify RUN --frames A-B` | `tools/analysis/timeline.py`'s per-frame means over a guest-frame range, for checking the extractor |
| `run_bench.ps1` | Interleaved A/B runner: frozen user data per run, watchdog, manifest per run, summary at the end |
| `shot_diff.py` | Screenshot regression check (MAD and % of pixels off by more than 16 / 32), single pair or two runs by label; `--cross-res` compares different sizes (Lanczos downscale, Session 8) |
| `shot_refs.py build/check/frozen/show` | Screenshot references in `out\harness\screenshots\refs\` (manifest, A/A thresholds, stable-pixel masks for animated screens) and the check against them; regress plugin `tools\regress\checks_screens.py`. Docs: `docs\harness\screens.md` (Session 8) |
| `ab_paired.py RUN... [--baseline AA_RUN]` | Independent in-run A/B check (main session): per-scene arm means and adjacent-segment pair differences; `--baseline` subtracts an A/A run's pair differences (same `make_ab.py` schedule) to remove the schedule's content bias (difference-in-differences) |
| `anatomy_report.py anatomy_NNN_K.csv` | Frame anatomy from a `gpu_anatomy_capture`: draws/copies/CP time per frame, passes by render-target configuration (with the CP gap between commands), top shader pairs by CP time, skipped draws, pipeline changes. `--gpu` (Session 7b) joins the per-row GPU marks (`gpu_anatomy_gpu_marks`): GPU prep vs draw time per pass and shader pair, biggest stalls. Marks cost ~1 us each on the GPU, so trust outliers and totals, not per-draw values |
| `fps60.py RUN` | 60-fps entrance check (Session 7b): per scene (Brock / second entrance / match) fps, frames > 20/25/35 ms, wall time and host GPU ms per frame, per 600-frame window |
| `upstream_report.py [--only NAME] [--stdout]` | Upstream-style tables for one SDK setting in `docs\upstream\<setting>.md` (numbers only, neutral wording): verify counts, in-run A/B (bench_report + ab_paired cross-check), end-to-end runs (fps60), all recomputed from the raw run outputs and checked against the session notes' values (Provenance section). Studies in its `STUDIES` list. Exit 1 when a notes value differs or could not be checked (Session 8, task 18) |

Related scripts elsewhere: `tools/input_scripts/make_route.py` (recording -> replay script with file-load sync
points), `build_bench_routes.py` (regenerates `bench_lesnar_bryan.txt`, `bench_lesnar_undertaker.txt`,
`bench_taker_wm.txt`), `make_ab.py` (in-run A/B variant of a route: `--cvar NAME` toggled every `--period` frames
through both entrances and the match; `--on/--off` for non-bool settings); `tools/sdk_check.py` (syntax-check SDK
files in a worktree with the real flags); `tools/build_sdk.ps1` (SDK build + install + stage DLLs and PDBs).
`run_list.ps1` is the simple batch runner used for every Session 6 measurement: a loop over
`tools/run_and_triage.ps1` that mirrors the frozen user data before every run (`run_bench.ps1` is the fuller
runner with manifests and a watchdog; not used in Session 6).

Run numbers can be written `73`, `073` or as a file name. `--logs DIR` points at another logs folder (default
`out\build\win-amd64-release\logs`).

## bench_report.py

### Numbers per CP frame

A CP frame is one `cp_swap` to the next on the GPU command-processor thread. Definitions are exactly
`tools/analysis/timeline.py`'s (checked: 0 ns difference per frame against timeline.py's own functions on 909 frames
of runs 066, 071, 073 and 074; 073 frames 3896-5300 give busy 16.2188, wrm 16.7701, idle 0.1196 ms, same as
timeline.py). The k-th `cp_swap` is paired with the k-th VdSwap, which labels each CP frame with its guest frame.

| Column | Meaning |
|---|---|
| `busy` | span - cp_idle - wrm - hwait - intr (the swap drain, endsub and vswap count as busy) |
| `busy_nd` | busy minus the `drain_b`/`drain_e` scope (CP waiting for the submit thread) |
| `asub` | GPU Submit thread time (`asub` scopes) overlapping the CP frame |
| `bneck` | max(busy_nd, asub): the pipeline bottleneck once the CP gets faster than the submit thread |
| `kicks` | the game's GPU kicks (`wptr`) in the paired guest frame; optimizations don't change it |
| `vd_ms`, `lag` | the guest frame's VdSwap interval; cp_swap minus its VdSwap |
| counters | `cpf` draws / host_draws / copies, `cpf_t0` t0_packets / t0_regs / t0_float_regs, `cpf_t0x` t0_fetch_regs / t0_boolloop_regs / t3_packets, `cpf_rr` rr_calls / rr_allvalid / rr_kib, `cpf_bind` tex_sets / const_sets / sampler_params, `cpf_fc` fc_writes / fc_changed / pipeline_changes, `cpf_cpu` cp_cpu / checkpoint / submissions (when the timeline has them) |
| `gpu_ms` | `gpu_ts` GPU time of the submissions that carry this frame's draws (swap submission: the one right after its cp_swap) |

`report` writes `out\bench\reports\cpframes_NNN.csv` (plus `scene` and `mark` columns, and `cvar:NAME` with the
value in force for each `--ab` setting), `report_NNN.json` and `scenes_NNN.csv`. About 1-2 s per run.

### Scenes and windows

Scenes are guest-frame ranges. "+0" is the frame being built when the anchor happened: a `[wwe2k14][file]` line with
`frame=N` gives +0 = N+1; old logs without the field fall back to the log timestamp against `frames_NNN.csv` (the
first VdSwap at or after the open; +-1 frame, B2). With both available the report prints the difference.

| Scene | From | To |
|---|---|---|
| `tron_NNN` | `titantron\NNN.bik2` opens | next titantron open or `LOADHD.PAC` |
| `match` | `M3.PAC` + 300 frames | `WINNING*.PAC`, `LOADHD.PAC`, end of data, or `--match-frames` (3600) |
| `mark:TEXT` | script `mark ID TEXT` (timeline `mark`, a = ID, b = guest frame) | next mark or end of data; a mark whose text is `end` only closes the previous one |

A second occurrence in one run gets a suffix (`tron_103_2`, `match_2`). Windows are fixed offsets inside a scene:
`tron_343_open` = Brock +30..+150 (light shot: exactly 11 kicks/frame), `tron_324_690` = Bryan +0..+690,
`tron_324_pan` = Bryan +380..+500 (heavy in every variant). Add more with `--window LABEL=SCENE:START:END`, e.g.
`--window taker_heavy=tron_103:0:900`.

Per scene: CP busy mean/p50/p95/p99/max and the heaviest 60-frame window; slow frames by cp_swap cadence (primary:
interval >= 45 ms in entrances, >= 25 ms in matches; "cp" = of those, frames whose busy was over budget) and by VdSwap
(secondary); asub, bneck, drain (and how many frames drained); kicks; counters, draws per frame and us of CP busy per
draw; GPU ms; processor numbers of the CP (`cpf_cpu`) and submit (`asub_b` b) threads; vblank lateness. Marked
scenes take the entrance thresholds when their median frame is >= 25 ms. Percentiles use linear interpolation.

Log checks: warning/error/critical lines as a histogram of message kinds (numbers and hex replaced by `#`), counted
per scene; hard fails (`[wwe2k14][crash]`, `device lost`, `Failed to await`, `script FAILED`) make a run invalid, and
so do the route checks below.

### Entrance camera variants (B3)

Kicks per guest frame for 690 frames from the titantron open, 15-frame moving average, Pearson r (best lag within
+-3) against `out\bench\reference\<scene>_<variant>.csv`. A run is left out of a reference it belongs to when other
runs remain.

- **shape:** best variant with r >= 0.9 (the reviewers' cut; 0.98 rejected the valid X run 067 at 0.977).
- **level:** only against a flat reference (Brock's light shot is exactly 11 kicks/frame, so r is noise there):
  mean |difference| <= 0.5 kicks/frame over the first 300 frames. Brock light vs heavy differ by about 2.0.
- otherwise **unknown**.
- A 6-letter **pattern** gives the closest reference per 115-frame segment (`=` several match, `?` none).
- When the next anchor cut a scene short (an entrance skipped with START), its last 100 frames are dropped first:
  after a skip the game loads the next entrance at about 2 kicks/frame.

References seeded from Session 5 (commands below). Classification of the existing runs:

| Run | tron_343 (Brock) | tron_324 (Bryan) |
|---|---|---|
| 066 | heavy (r 1.000, self) | X (r 0.996) |
| 067 | light (level 0.11) | X (r 0.977) |
| 068 | light (level 0.14) | Y (r 0.998) |
| 070 | light (level 0.14) | X (r 0.996) |
| 073 | light (level 0.11) | Y (r 0.998) |
| 074 | light (level 0.15) | X (r 0.994) |
| 077 (Session 6) | light (level 0.13) | **unknown**: r 0.889 X / 0.898 Y, pattern `=XXYYY` |

Run 077 follows X for about 300 frames and Y after that, so Bryan's entrance is not just two whole variants: the
camera seems to pick shots part-way through (unconfirmed how many choices there are). Compare heavy windows by
pattern, not only by variant label, and add a reference for each new combination once it repeats.

    py tools/bench/bench_report.py refs --scene tron_324 --variant X 066 067 070 074
    py tools/bench/bench_report.py refs --scene tron_324 --variant Y 068 073
    py tools/bench/bench_report.py refs --scene tron_343 --variant light 067 068 070 073 074
    py tools/bench/bench_report.py refs --scene tron_343 --variant heavy 066
    py tools/bench/bench_report.py classify 066 067 068 070 073 074

For a new entrance (e.g. CH103, `tron_103`), run `refs` with the first runs, then `classify` new runs against it.

### In-run A/B (`--ab NAME`)

The input script's `cvar NAME VALUE` records timeline event `cvar` (a = FNV-1a 32 of NAME as typed, b = the value
as a number, true/false = 1/0, c = guest frame). `--ab NAME` (or `--ab 0xHASH`) splits CP frames by the value in
force: arm A = 0, arm B = 1 (`--ab-a/--ab-b` for other values). Every cvar event starts a new segment; the CP frame
containing an event is dropped, and after a value change the next `--settle` (6) frames too; frames before the first
event are unknown and dropped. A segment ends at the next event. The last one has no next event (the value just
stays in force to the end of the run), so it is cut at the median event-to-event interval; so is any gap more than
twice that long (a script toggling in two blocks with a long wait between). Without that cut, the last segment
covered the rest of the run and was still paired. `--ab-scene` restricts it to one scene or window. Output: per-arm busy
mean/p50/p95/p99 plus busy_nd, asub, bneck, span, draws and gpu_ms; then the difference B - A over non-overlapping
adjacent segment pairs (mean, median, sign test, and a bootstrap 95% CI from 5 pairs up; with fewer, a "CI" is just
the range of the values) and the mean over every adjacent pair. The `cvar:NAME` column in `cpframes_NNN.csv` is the
value in force, not the arm used: it runs on past the cut.

Keep every toggle inside one shot. Brock's light shot is steady from about +24 to +167 (CP busy about 16 ms in
070/073/074). At +168 busy halves in every light run (067, 068, 070, 073, 074; 16 -> 8 ms in 070/073/074), with kicks
unchanged at 11/frame. Script pattern (B A A B, so drift cancels; +30..+150, then the reset):

    wait_file 343.bik2 1800
    wait_frames 30
    repeat 3
    cvar SETTING true
    wait_frames 10
    cvar SETTING false
    wait_frames 10
    cvar SETTING false
    wait_frames 10
    cvar SETTING true
    wait_frames 10
    end_repeat
    cvar SETTING false

    py tools/bench/bench_report.py report NNN --ab SETTING --ab-scene tron_343_open

Checked in memory on eight real light-shot runs (067 068 070 073 074 077 083 084), with cvar events injected at
the scripted frames' VdSwap times. In-run A/A (a setting that does nothing): B - A between -0.41 and +0.24 ms
(sd about 0.2, 6 pairs per run). With 1.0 ms taken off every arm-B frame, the measured result was -0.76 to -1.41 ms.
So one run resolves effects of about 0.5 ms (3%) on this shot, and averaging runs shrinks the floor. The previous
recipe here (12-frame segments x 4, +30..+222) crossed the shot change and gave a false -0.48 to -1.14 ms in every run.

The CI only covers the scatter between the pairs of one run. Content differences between one run's A frames and B
frames are systematic, so an in-run A/A can show a CI that excludes 0: the reviewer's A/A run 904 (073, old recipe,
`--ab-scene tron_343_open`) gives +0.35 ms, CI +0.12..+0.51. Trust an effect only when it clears the A/A floor (about
+-0.4 ms on this shot) and keeps its sign across runs; measure the floor for any other scene with an in-run A/A first.

### summary

Rows per scene and run (arm from the manifest, variant, whole-scene length and stats, slow frames, asub, bneck), then
the arms compared within each camera variant. Only valid runs are compared, and only like with like:

- **unknown variants are never compared** (B3/B7: r below 0.9 against every reference means a desync or a camera
  path without a reference yet; the run's notes say so);
- a run much shorter than the others (under 0.8 x the median length: an entrance skipped earlier, a match that ended
  early) is left out, and the rest are compared over the frames they all cover, +0..+L (minus the last 100 frames
  when the lengths differ, because the shortest run ends in the next load);
- in entrances, a run whose kick curve over +0..+L doesn't match at least half of the others (r >= 0.9, or for a flat
  curve mean |difference| <= 0.5 kicks/frame) is left out. The variant label only covers +0..+690, and a camera can
  switch shots later (077), so this checks the rest. It also covers entrances with no reference yet (CH103: 083, 085
  and 086 all match). Checked: 073 (Y) or 077 (X then Y) labelled X are left out (r 0.76-0.89); 066's heavy shot
  labelled light is left out (level 4.0); 077 vs 084 over Bryan's whole entrance (r 0.71) are both left out.

Then arm medians, per-pair deltas in run order with the B6 starting rule (win if the median per-pair change beats
max(3%, 2 x `--aa-sd`, default 2%) and the signs agree), and the B6 regression guard: any scene's p99 +5%, and the
match's p95 +5%. The match only gets the regression guard, never FASTER/SLOWER, because its content differs from run
to run. The reference arm is the first arm in the run list.

Also: the settings that differ between runs (from the game's own `[wwe2k14] modified:` lines, so a flag that didn't
take shows up); per input script, the spread of each sync point across its runs (> 10 frames flagged, B7), keyed by
the text waited for and its occurrence (`m3.pac #2`), because a `repeat` block logs the same line number on every
pass and an edited script moves line numbers; and log message kinds that appear in only one arm (B8).

### Route checks (B7)

A run is invalid when a scene the route must reach is missing or has the wrong length. Declare them in the input
script as a comment (the game ignores it), or with `--expect` (`run_bench.ps1 -Expect`), which wins per scene:

    # bench_expect: tron_343=1562:3 tron_324=2271:3 match

`SCENE` must be present; `SCENE=LENGTH[:TOL]` must also be LENGTH +- TOL guest frames long (default 3). A tron
scene's length is the gap to the next titantron or LOADHD.PAC, so `tron_343=1562:3` is B1's Brock -> Bryan gap on
the full Lesnar-Bryan route (077: 1563, 084: 1562; Brock -> Undertaker is 1576-1578 in 083/085/086). Without any
declaration, `summary` still compares runs of one script: a scene missing in some of them, or found in only some of
them, makes those runs invalid (082 is flagged next to 083/085/086: its script failed waiting for BG56.PAC and quit
during Brock's entrance, so it has no tron_103 or match). Also invalid:
cp_swaps that can't be paired with VdSwaps by index. The time fallback labels frames late: 073 forced into it (6
extra cp_swaps before the first VdSwap) gets +1 guest frame on 5324 of 5337 CP frames. Every run 066-087 pairs by
index at offset 0.

## run_bench.ps1

    pwsh -File tools\bench\run_bench.ps1 -Script tools\input_scripts\<route>.txt -Label c1 -Arms 'A=--clear_memory_page_state=true;B=--clear_memory_page_state=false' -Repeat 3 -Order ABBA -MaxMinutes 8
    pwsh -File tools\bench\run_bench.ps1 -Script tools\input_scripts\bench_lesnar_bryan.txt -Label aa -Repeat 4 -Expect 'tron_343=1562:3 tron_324=2271:3 match'

Per run: refuses if a wwe2k14 process is running; mirrors `out\bench\userdata_frozen` (or `userdata_coldcache` with
`-ColdCache`) into `out\bench\userdata_work` with robocopy /MIR; launches the exe with WorkingDirectory = the exe
folder and one argument string (`--user_data_root=<work> --cache_root=<work>\cache --wwe2k14_timeline=true
--wwe2k14_input_script=<absolute script>`, then `-ExtraArgs`, then the arm's flags); finds the run number from the new
`wwe2k14_NNN.log`; with `-Perf` starts `tools\perf_sample.ps1 -Out logs\perf_NNN.csv` (and stops it when the run
ends: perf_sample follows the first wwe2k14 process it finds). Watchdog: frames_NNN.csv not growing for
`-StallSeconds` (30), no frames file after `-StartupSeconds` (90), or past `-MaxMinutes` (10) -> kill, run invalid.
A run is also invalid on a non-zero exit code, a hard-fail log line, a missing timeline, or an NVIDIA driver error
while it ran (System log, provider nvlddmkm, event 153; B8). `-WarmUp` adds one discarded run of the first arm.
`-Expect` is passed to the summary as `--expect`. `-DryRun` prints the schedule and argument strings only.

Manifest per run: `out\bench\runs\<exe folder>_<hash>\run_NNN.json`, one subfolder per logs folder, because every exe
folder numbers its runs from 001 (snapshot folders `out\bench\<label>\` would otherwise overwrite each other's
manifests). It holds run, label, arm, flags, full argument string, script path + SHA-256, SHA-256 of the exe and
every `rex*.dll`, repo and SDK commits, start time, duration, exit code, valid + reason, the user-data files the run
changed (the save should never change in a benchmark route), and the nvlddmkm events during the run.
`bench_report.py` finds a run's manifest under `--manifests` (default `out\bench\runs`, subfolders included) by its
`logs_dir`, and only if its start time is within 10 minutes of the log's first line, so a reused run number
(logs deleted) doesn't pick up an old manifest.

Tested with a stand-in exe (`-Exe`, `-BenchDir`, `-ProcessName` keep the real folders and a running game untouched):
normal runs, a save write, a hang (killed after the stall), a crash line with exit code 3, a timeout, the
per-folder manifests with `-Expect`, and the driver-event query (it finds the 8 event-153 entries logged on
2026-10-07 20:06-20:41; none of this machine's 121 such events in the last 60 days overlap runs 050-087).

## shot_diff.py

    py tools/bench/shot_diff.py --runs 052 053
    py tools/bench/shot_diff.py --runs 069 072 --animated-labels "title|main_menu|storage_notice"
    py tools/bench/shot_diff.py A.bmp B.bmp --animated --heatmap out\bench\shotdiff\a_vs_b.png

MAD = mean absolute difference over every R, G, B value; off% = pixels where any channel differs by more than 16.
Static pass: MAD <= 0.5. Animated pass: MAD <= 5 or off% <= 10. Reproduces the B8 calibration exactly: 052 vs 053
static screens 0.00 / 0.00%; 069 vs 072 title 1.50 / 3.30%, storage_notice 0.42 / 0.28%, main_menu 2.44 / 6.53%.
Exit code 1 when any pair fails. Heatmaps are game output: keep them under `out\`.

Session 8 (task 16): every line also prints off32 (pixels off by > 32). `--cross-res` allows different sizes: the
larger image is downscaled to the smaller with Lanczos (reproduces the run-175 numbers). A resized static pair passes
at the resampling level, MAD <= 2.0 and off32 <= 0.5 % (`py tools/bench/shot_diff.py --runs 146 175 --cross-res`);
a resized animated pair keeps the animated rule plus that level, MAD <= 5 + 2.0 or off <= 10 + 2.0 % (a moving
screen's A/A noise alone is above 2.0 MAD).
Per-label references with A/A thresholds: `shot_refs.py`, `docs\harness\screens.md`.
