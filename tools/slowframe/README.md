# Slow-frame report (Session 8 task 12)

Any guest frame over a budget gets a short report: what the main thread and the GPU Commands thread were doing
(call stacks), that frame's command-processor and GPU time from the timeline with the per-frame cause counters,
and the frame-anatomy cause columns (sparse GPU marks) of the frames right after it.

Off by default. In-app code: `src/slow_frame.h` (setting `--wwe2k14_slowframe_ms`) plus a sample ring in
`src/sampling_profiler.h`. Everything it writes is game-derived and goes to `out\harness\slowframes\<run>\`
(gitignored).

## Use

1. Run with a budget, e.g. 60 fps play at the 4K preset (frames >= 25 ms are dropped frames):

       pwsh -NoProfile -Command "& C:\Users\Jarrod\wwe2k14-recomp\tools\bench\run_list.ps1 -Scripts @('..\..\..\tools\input_scripts\bench_taker_wm.txt') -Extra '--wwe2k14_preset_4k60=true --wwe2k14_slowframe_ms=25 --wwe2k14_slowframe_ignore_ms=400'"

   30 fps entrances at default settings: `--wwe2k14_slowframe_ms=45`.
2. Build the reports (run number from the run_list line):

       py tools\slowframe\slowframe_report.py NNN [--quiet]

   (NNN can also be a log or timeline file name; `--logs DIR` reads a copied logs folder.)

   -> `out\harness\slowframes\NNN\summary.txt` (one line per slow frame), `report_<frame>.txt` per frame,
   `anatomy_<frame>.txt` (full `anatomy_report.py --gpu` output of the capture requested at it).

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `wwe2k14_slowframe_ms` | 0 (off) | budget: frames at least this long (VdSwap to VdSwap, as in `frames_NNN.csv`) are reported |
| `wwe2k14_slowframe_gap_ms` | 1000 | rate limit: at least this long between two reports |
| `wwe2k14_slowframe_max` | 20 | at most this many reports per run |
| `wwe2k14_slowframe_ignore_ms` | 0 | frames at least this long are skipped (loading screens), 0 = none |
| `wwe2k14_slowframe_from_frame` | 60 | frames before this one are skipped (boot: the profiler starts at the first frame) |
| `wwe2k14_slowframe_anatomy` | 2 | anatomy capture frames per report (from the CP's next frame boundary), 0 = none |
| `wwe2k14_slowframe_dir` | "" | output folder (default `<exe>\..\..\harness\slowframes`) |
| `wwe2k14_slowframe_force_every` / `_force_ms` | 0 / 60 | debug: sleep on the main thread every N frames (in `SlowFrameForcedStall`) to make slow frames on purpose |

Turning it on also sets, each only if left at default: `wwe2k14_timeline`, `vulkan_gpu_timestamps`,
`wwe2k14_profile_main`, `wwe2k14_profile_gpu_commands`, `gpu_anatomy_gpu_marks`, `gpu_anatomy_gpu_marks_mode=1`.

## What each part means (and its limits)

- **Stacks:** while on, the sampling profiler (1 ms) keeps every sample's whole stack in an in-memory ring
  (16,384 samples: ~15 s of two threads at the observed ~540 samples/s each; 8.6 MB) instead of a file. At the slow
  frame N the samples tagged N-3..N-1 are copied aside (a cheap ring scan, so a long frame after it cannot push them
  out of the ring); two frames later those plus the samples tagged N..N+1 are written to `frame_<N>_stacks.txt`
  (stacks_NNN.txt format, so `tools\analysis\profile_stacks.py` reads it: `--frames A-B --thread "main thread"`). The
  write resolves names, during which the sampler takes no samples, so it is not done during frame N+1. A run that ends
  before frame N+2 (the usual close hard-exits) gets no stacks file for that report. The profiler's own 5-second
  window report (`wwe2k14_profile_report_seconds`) also stops the sampler for ~10-20 ms; when that falls right after
  a slow frame the "samples per tag" line shows the gap (seen in runs 290/291: no samples tagged N..N+1 for the
  frame-301 report, which fell on the first window). The report uses tag N-1: a sample's tag is the VdSwap count when it was
  taken, so frame N's own work (VdSwap N-1 to N) is tagged N-1. Both threads are shown for that same wall-clock window;
  the GPU thread usually works 1-3 frames behind the game, so its stacks are what it did while the game built frame N,
  not necessarily frame N's own commands.
- **CP frame / GPU ms:** `bench_report.py`'s per-CP-frame table (the CP frame that presents guest frame N): busy,
  idle, WAIT_REG_MEM, host-GPU waits, GPU ms (gpu_ts) and the cpf_* counters the timeline records every frame (draws,
  texture loads, barrier batches, uploads, ...). "Unusual" columns are >= 1.5x the median of the previous 120 CP
  frames: that per-frame record is the cheap rolling buffer for the slow frame itself.
- **Anatomy:** a capture starts at the command processor's next frame boundary after the request (made at VdSwap
  N). The CP runs 16-65 ms behind VdSwap, so that boundary can open frame N's own CP frame (10 of 29 reports in runs
  275/278/282) or the next one. Each capture frame is mapped to its guest frame through the 'anatomy' timeline event,
  which the SDK records right after the cp_swap that ends the captured frame (checked against the timeline's draw
  and copy counts: every capture frame matches its mapped guest frame). When the capture includes frame N, the
  report flags it, because frame N's CP busy and GPU ms then include the capture's per-draw recording and GPU marks
  (282 frame 5263: CP busy 17.8 ms against 13.9 the frame before).
- **Capture side effects:** in heavy scenes the capture makes the next frames slow itself: run 282's capture at
  frame 5263 (~12k draws per frame) wrote 24,403 rows (9.4 MB) on the GPU Commands thread, and frames 5265 (54.8 ms)
  and 5266 (44.1 ms) went over budget where baseline run 208 has steady 33 ms frames. So `summary.txt` counts the
  over-budget frames inside a capture window (N+1 .. last captured frame + 2) apart from the rest and lists them.
  The rate limit keeps them from being reported. Set `--wwe2k14_slowframe_anatomy=1` (or 0) to disturb less.
- **Transitions:** a slow frame within 8 frames of one of the game's frame-rate halve / restore calls (the `[pacing]`
  log lines, entrance starts and ends) is marked as a transition in its report and in the summary.
- **Scene:** `bench_report.py` scenes (tron_NNN, match, mark:NAME), the last input-script mark and the last files
  opened.
- **"main thread"** is the profiler's label for the thread that calls VdSwap. `db\` identifies its loop as
  `render_cmd_thread_main` (sub_82BB5CC0, CRenderCmdManager), the game's render thread, not its update thread: the
  update thread is not sampled. Guest function names in the reports get their `db\*.json` name in brackets.
- **Reading:** one line per frame from fixed rules on the measured numbers (`verdict()` in the script): forced
  stall; main thread in a guest kernel wait (an xboxkrnl wait called from guest code, with the guest callers) for
  >= 40% of its samples; main thread waiting for the GPU (timeline gwait >= 40% of the frame, or >= 40% of samples
  inside the game's GPU-wait functions sub_82CB1F50 / sub_82CB5A20 from `src\gpu_timeline.h`); CP busy >= 70% of its
  frame (CP-bound) or idle >= 50% (waiting for the game's commands); host GPU >= 70% of the frame (GPU-bound); and
  "neither CP nor GPU busy" when the game waited for the GPU anyway (a pacing / handshake wait). What a wait waits
  for is never guessed ("unconfirmed").
- **Sampling rate:** observed ~540 samples/s per thread at the 1 ms setting (Sleep granularity plus the suspend /
  unwind), so a 33 ms frame has ~18 samples per thread: percentages are coarse.
