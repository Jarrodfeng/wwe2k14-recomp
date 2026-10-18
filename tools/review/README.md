# tools\review: visual review queue (Session 8, task 4)

Jarrod's backlog of things to look at. For a route and a list of arms (flag sets), record the same moments in each
arm, put them side by side, and build one HTML page per batch with a one-line "look at" note and an empty verdict
line per item. Everything recorded is game imagery: it lives in `out\harness\review\<batch>\` (gitignored) and is
never committed. Only the code and the hand-written CAW route are tracked.

## Commands (from the repo root)

```
py tools\review\record.py --batch batch1 --jobs bryan:default,bryan:1440p,bryan:4k
py tools\review\record.py --batch batch1 --jobs taker:default,taker:1440p,taker:4k,caw:default,caw:4k
py tools\review\compose.py --batch batch1 --check
```

Then open `out\harness\review\batch1\index.html`.

## In-game capture (the default since Session 8 phase 2, task t1)

Desktop capture failed whenever the display slept (0 frames in 60 s) and only ever saw the 1280x720 desktop. Clips
are now recorded by the game itself (`src\clip_capture.h`, default off):

- `--wwe2k14_clip_dir=DIR` turns it on; input-script commands `clip_start NAME [FPS] [SCALE]` / `clip_stop [NAME]`
  write `DIR\NNN_clip_NAME.mp4` (NNN = the run's log number, as `captures\NNN_LABEL.bmp`, so runs that share DIR in
  shared run-lock mode never write the same files; the presented guest output at the internal render resolution:
  1280x720 / 2560x1440 / 3840x2160), `NNN_clip_NAME.json` (the game's own counts) and
  `NNN_clip_NAME.mp4.ffmpeg.txt`. `record.py` renames them to `clip_NAME.*` in its per-job folder. Without the
  setting the commands are logged and ignored.
- NAME (1-64 of letters, digits, `_`, `-`), FPS (1-240) and SCALE (1, 0.5, 0.333, 0.25) are checked when the
  script loads, whatever the setting, and a NAME may be used by one `clip_start` per script (after `repeat`
  expansion): a bad route fails at once ("can't parse ..."), not after the boot.
- Every end of a script stops a running clip: `quit`, a script failure (`on_fail quit` holds the quit), and the last
  step. A quit waits (up to 1800 frames) until every clip is finished, json included: the log shows `quit waits for
  N clip(s)` then `quitting after the failure or quit (... clips in flight: 0)`.
- If ffmpeg does not start or dies mid-clip, the clip is abandoned at once (capture threads stop; json
  `"abandoned": true`, log `clip abandoned`), so a broken ffmpeg costs the game nothing.
- How: the VdSwap hook posts a trigger; capture threads call `Presenter::CaptureGuestOutput` (the screenshot
  path); a writer thread pipes rgb0 frames to ffmpeg (`h264_nvenc`, RGB in, colour conversion on the GPU). The clip
  is constant frame rate in real time: frame slot = round((VdSwap time - first frame) x FPS); empty slots repeat the
  previous frame (30 fps entrances, hitches, misses), a second frame in one slot is skipped. The game never waits:
  a trigger no capture thread took by the next VdSwap is dropped (`dropped_busy`), a frame over the encoder backlog
  cap is dropped (`dropped_backlog`); both show as repeats. `identical_to_previous` counts captures that got the
  same guest output as the one before (static scenes, or a capture that ran before the GPU refreshed the output).
- Settings: `--wwe2k14_clip_ffmpeg` (record.py passes the full path), `--wwe2k14_clip_codec` (h264_nvenc),
  `--wwe2k14_clip_cq` (19), `--wwe2k14_clip_workers` (3), `--wwe2k14_clip_queue` (8 frames).
- `record.py` (default `--capture ingame`) passes `--wwe2k14_clip_dir=<job folder>` and runs each job with
  `run_list.ps1 -Exclusive`; it finds its own log by the route file and job folder named in it (other games may run
  in shared mode). `make_routes.py` puts `clip_start NAME 60 1` / `clip_stop NAME` next to the `log review:` anchor
  lines; an arm may set `clip_args` (e.g. `'30 1'`) in `record.py`'s `ARMS`. Desktop capture stays as the fallback
  (`--capture auto|ddagrab|gdigrab`). `capture_ok` needs: run_list's line clean (exit code 0 and nothing in its
  failure / triage columns, so a route whose `wait_file` timed out is not OK), a finished clip (json, ffmpeg rc 0,
  not abandoned, frames) for every `clip_start` in the route file, and stills. A clip that missed more than 5% of
  the game's frames stays OK but is listed in `capture_degraded` (job.json) and marked DEGRADED on the page.
  record.py refuses an ffmpeg path or job folder with a space or quote (`-Extra` cannot carry them).
- `compose.py`: in-game arms show the clip's counts and the game's fps during the clip on their line (a clip that
  missed more than 2% of the game's frames is flagged; more than 5% = DEGRADED, also in the video label), link each
  arm's native-resolution clip under the side-by-side video, and skip the timestamp-gap check (constant frame rate
  by construction). The side-by-side video is as long as the longest arm (shorter arms hold their last frame; before
  the fixer pass it was cut to the shortest, which dropped the last 1.7 s of the slower 4K arm).
- The cheaper 4K arms: `record.py --jobs taker:4k30` records `runs\taker__4k30`; `compose.py` shows a route's
  4k30 job as the item's 4K arm by default (`--arm-map 4k=4k30`; labelled "4K preset (clip 30 fps)"; routes without
  a 4k30 job keep their 4k job). `--arm-map ""` shows the 4k jobs (60 fps clips); `--arm-map 4k=4khalf` the
  half-size ones.
- Measured (2026-10-09, exclusive runs; cross-run numbers vary ~20%): 720p costs nothing measurable (main-menu
  in-run A/B run 393: p95 16.96 vs 16.94 ms; re-record 397/398 vs desktop-capture runs 194/198: same frame times,
  no missed frames). 1440p: fps unchanged and no missed frames, a small frame-time cost in entrances (Bryan: p95
  18.6 vs 17.2 ms, 9 vs 2 frames over 20 ms; run 400 vs 187). 4K60 is too heavy in entrances: Undertaker entrance 57.9 fps with the clip vs 59.9 (run 395
  vs 196), Bryan entrance 54.8 vs 60.0 (396 vs 188, 120 slow frames vs 2), and the clips missed 3-7% of the game's
  frames (busy capture threads, or the same image captured twice); in the 4K match the encoder fell behind (199 of
  900 frames dropped, run 395) with a small game cost (p95 19.1 vs 17.1 ms). In the 4K main menu (run 394) it was
  fine (no misses, p95 17.8 vs 17.0 ms). For 4K use the `4k30` arm (`clip_args '30 1'`) or `4khalf` (`'60 0.5'`,
  1920x1080). Measured in the fixer pass (Undertaker route, exclusive, `clip_cost.py 416 --review e2 --vs 417 395
  196`): entrance window 4K60 clip (395) 57.89 fps, 78 frames over 20 ms; `4k30` (416) 59.94 fps, 16 over 20 ms,
  p95 19.4 ms; `4khalf` (417) 59.94 fps, 3 over 20 ms, p95 18.2 ms; desktop capture (196) 59.94 fps, 1 over 20 ms,
  p95 17.1 ms. Both cheap arms missed 0-3 frames (no degraded clip). Run 416 still captured every game frame and
  dropped half in the writer; since build s8_t2detfix_1 (17:39, which compiled this change) a 30 fps clip skips
  those VdSwaps before capturing (`capture_requests` / `skipped_before_capture` in the json). Likely 4K cost: CaptureGuestOutput allocates a 33 MB readback buffer
  and converts 10-bit to 8-bit on the CPU for every frame (46 ms per capture in the heavy entrance); a persistent
  readback ring in the SDK would remove both (not done: needs an SDK patch).
- Cost: `py tools\review\clip_cost.py RUN` (frame times inside each clip vs the frames just before it; the in-run
  A/B route is `routes\cliptest_ab_menu.txt`, run exclusive); `--review NAME --vs RUN2 ...` compares a review
  window across runs. Self-test routes (untimed, shared run-lock mode is fine): `routes\cliptest_title.txt` (two
  short clips at the title screen), `cliptest_quitwait.txt` (30 fps and half-size clips, `quit` while a clip
  records), `cliptest_failquit.txt` (a script failure while a clip records), `cliptest_badscale.txt` and
  `cliptest_dupname.txt` (must fail at load; run them with `-WaitSeconds 40`, the game idles until run_list kills
  it). Fixer pass, shared runs 410-415 (three at a time, one clip folder): every clip json written, ffmpeg rc 0,
  no file collisions, 0 crashes.

## Desktop capture (fallback)

- `record.py --capture auto` takes the run lock once for all its jobs (it waits in the queue behind other agents),
  does a 2-second desktop-capture self-test (no game running), then runs each job through
  `tools\bench\run_list.ps1` (frozen user data, timeline on, killed after 720 s). It tails the live game log:
  `log review: clip_start NAME` starts an ffmpeg capture of the game window's client area,
  `log review: clip_stop NAME` stops it. Stills come from the route's
  `screenshot rv_*` lines (native internal resolution) and are copied as PNG. Per job:
  `runs\<route>__<arm>\job.json` (run number, run_list line, route file and its sha256, clip timing, ffmpeg spawn
  latency and CPU), `clip_*.mp4`, `still_*.png`. ddagrab does not report frames it missed (no dup= / drop= in its
  output); `compose.py --check` measures those as frame-timestamp gaps instead. A job records into
  `runs\<route>__<arm>.new` and replaces the old folder only when the new recording is `capture_ok` (a failed
  re-record keeps the good one). If every `--jobs` entry is already recorded OK (and there is no todo.txt),
  `record.py` exits without queueing for the lock.
- `make_routes.py` generates the review variants of the bench routes into `out\harness\review\routes\` (never edits
  the bench routes): the part after the second entrance's tron is replaced by clip and still lines at fixed times,
  and the run quits after the match clip. Entrance waits are in seconds and converted per arm: entrances run at
  30 fps at default settings and 60 fps with the presets, so the same moment needs twice the frames.
- `routes\review_caw.txt`: main menu -> CREATIONS -> first item (Create a Wrestler), stills only, never saves.
- `compose.py` builds the side-by-side clips (each arm scaled to 1280x720, labelled, `hstack`, h264_nvenc), still
  sheets (full frames plus crops, every crop magnified by an integer nearest-neighbour factor to a common size:
  720p x6 / 1440p x3 / 4K x2 for three arms, x3 / x1 for default vs 4K, so no arm gets fake stair steps), the
  Create a Logo sheet from runs 146/175, `checks.json` (`--check`: frames sampled from every clip, mean luma = black
  check, difference between samples = frozen check; per-arm capture gaps > 25 ms on the page; every
  batch1 clip has 3-13 such gaps, so an arm is flagged only when it has more than 1.5 x the item's median arm + 3
  (Undertaker 4K entrance: 21 vs 6); a content check that matches each clip against the native stills taken
  during it, best grey MAD at 320x180 within -0.8..+0.4 s: 0.4-2.6 for every batch1 clip, 37-67 for a clip
  against another route's stills, flagged "WRONG CONTENT" above 5, which is what a capture of the wrong window
  gives) and `index.html`. Only arms with `capture_ok` go into clips and sheets;
  failed arms are listed in red as "FAILED, not compared". Slow-frame counts on the page use 1.25 x the clip's
  median frame time (30 fps default entrances are not "slow"). compose takes the run lock (it is heavy CPU/GPU work
  and must not overlap a game run). The batch's items and notes are the `BATCH1` list in `compose.py` (they name
  batch1's runs; a new batch needs its own list).
- `winrect.py` prints the desktop size and the game window's client rectangle (physical pixels).

## Arms

| arm | flags | entrances |
|---|---|---|
| default | none (4x MSAA, 1280x720) | 30 fps |
| 1440p | `--wwe2k14_preset_1440p60=true` (no MSAA, 2560x1440) | 60 fps |
| 4k | `--wwe2k14_preset_4k60=true` (no MSAA, 3840x2160) | 60 fps |

## Capture notes

- The desktop is a single 1280x720 display, so a desktop capture only ever shows the presented 1280x720 image: at
  the presets that is the supersampled downscale (edges and shimmer show, resolution does not). The stills carry the
  resolution. The page says so.
- Capture = ffmpeg 8.0 `ddagrab` (Desktop Duplication: captures the Vulkan swapchain through the compositor; frames
  stay on the GPU and go straight to `h264_nvenc`); `gdigrab` is the fallback if the self-test fails. Measured
  (Session 8): ~0.5 s of ffmpeg CPU per 18-s clip, ffmpeg spawned within 0.03 s of the log line (the first captured
  frame is ~0.2 s after it, the same in every arm), and in the clip window the 4K preset had 2/1080 frames over
  20 ms with capture running (run 188), as runs 171/168 without it. The capture itself can miss frames at 4K
  (Undertaker 4K entrance: 21 gaps > 25 ms while the game rendered 1080 frames at 59.94 fps); the page flags it.
- If the game window disappears mid-clip (crash, early quit), `record.py` stops the running clips after about 1 s
  (`clips_stopped_window_gone` in job.json) and the job is not `capture_ok`, so compose leaves it out; the attempt
  stays in the `.new` folder when an older good recording exists. (Not yet exercised by a real crash.)
- Desktop capture records whatever is on top. The game does not come to the front when launched from a background
  agent (run 186's first clips recorded the Claude app instead), so `record.py` makes the game window topmost
  without activating it (`winrect.make_topmost`, no focus change) as soon as it appears, and again at each
  clip_start. `job.json` records `game_window_topmost` and `capture_ok`.
- `<batch>\todo.txt` (one `route:arm` per line) is the batch's backlog: the next `record.py` run of that batch picks
  those jobs up after its own. Jobs already recorded OK (`capture_ok`) are skipped; `--force` re-records only the
  `--jobs` entries, not the todo ones. A todo job that fails is retried by every later run of the batch until it is
  removed from todo.txt by hand.
- Create a Wrestler route (`routes\review_caw.txt`): main menu DOWN x3 = CREATIONS, A; the first item is SUPERSTAR,
  A opens NEW / EDIT; NEW = Create a Superstar (loading screen, then HEAD > TEMPLATES, no model yet; syncs on
  `CREATEMODE.PAC`, opened only when entering it); A on the SUPERSTAR template shows the model on SELECT A PART TO
  EDIT (syncs on `ED02.PAC`). It never saves.
- Arms are separate runs: camera variants and animation can differ (Bryan's entrance has two camera variants).

## Capture re-check (Session 8, task C, 2026-10-09 12:44)

`py tools\review\record.py --capture-test` (run lock taken, no game running) still fails, the same as overnight:

- `ddagrab`: timed out after 60 s with 0 frames (the 60-s guard in `capture_test` stopped it).
- `gdigrab`: rc 0 but only 9 frames in 2 s (about 4 fps, 1280x720), below the 60-frame pass mark.
- So `chosen: null` in `out\harness\review\batch1\capture_test\result.json`; `caw:default` was **not**
  re-recorded and `compose.py --check` was not re-run (one try, as planned). The page and `checks.json` are still
  the 03:55 ones; `runs\caw__default` is still run 193 (recorded OK by the earlier record.py).
- The session was not locked (no LogonUI process; `query user` shows the console session Active, idle none).
  The likely cause is the display being off or asleep while Jarrod is away (Desktop Duplication delivers no frames
  and GDI capture crawls), but that is **unconfirmed**: nothing here reads the monitor's power state.
- Next: re-run the capture test (and then `--jobs caw:default --force` plus `compose.py --batch batch1 --check`)
  while the display is on.
