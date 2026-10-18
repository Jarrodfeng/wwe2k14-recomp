# Flight recorder (Session 8 task 11)

In-app code: `src/flight_recorder.h` (header comment = full reference). Default **off**.

## Turn it on

```
--wwe2k14_flight_recorder=true
```

Optional settings:

| Setting | Default | What it does |
|---|---|---|
| `wwe2k14_flight_hang_seconds` | 10 | hang dump when no frame is presented for this long (0 = no watchdog) |
| `wwe2k14_flight_hang_samples` | 3 | host stack samples per thread in a hang dump, 300 ms apart |
| `wwe2k14_flight_dump_events` | 32 | newest events written per thread (main thread: 4x) |
| `wwe2k14_flight_gpu_dump_records` | 48 | newest GPU checkpoint records in a dump |
| `wwe2k14_flight_max_hang_dumps` | 5 | at most this many hang dumps per run (0 = no limit) |
| `wwe2k14_flight_test` | "" | debug: `hang@FRAME[:SECONDS]`, `crash@FRAME`, `abort@FRAME`, `dump@FRAME`, `bench@FRAME` (comma-separated) |

Turning it on mid-run (`cvar wwe2k14_flight_recorder true` in an input script) starts recording at once, and crash
and abort dumps then work (to this run's `flight_NNN.txt`). The hang watchdog, the forced tests and the GPU checkpoint
records need the setting on at startup; a dump says `(off ...)` in the GPU section otherwise.

The recorder sets the SDK setting `vulkan_flight_gpu_records=true` (our SDK patch, branch `s8/flight`) when it
starts (SDK patch `patches/rexglue-sdk/0006-session8-flight-gpu-records.patch`), so dumps include the Vulkan command processor's newest checkpoint records (draws, copies, submission
starts; needs `vulkan_gpu_crash_diagnostics`, which the app turns on by default).

## Output

`out\build\win-amd64-release\logs\flight_NNN.txt` (NNN = the run's `wwe2k14_NNN.log`), one section per dump:
crash (unhandled exception), abort, hang (watchdog), requested (`dump@FRAME`). Each section:

1. summary: one line per guest thread, what it is doing now (`IN NtWaitForSingleObjectEx h=0x... timeout=inf for
   12.3 s ... from <guest caller>`), main thread first, then the threads blocked longest;
2. crash only: the faulting thread's host stack, unwound from the exception context;
   then the milestones: one shared ring of the low-rate events of all threads (script steps, file opens, thread
   create/exit, tests), since a busy thread's own 512-event ring wraps in ~16 frames;
3. per guest thread, newest first: calls in progress, host stack samples (identical samples merged), guest stack
   (`lr` + r1 back chain), the ring's last events;
4. guest threads without events, host-only threads (GPU Commands, audio, ...) with their stacks;
5. GPU checkpoint records.

Names: exe frames come from `wwe2k14.map` (`sub_XXXXXXXX` = the recompiled guest function at that address), DLL
frames through DbgHelp (PDBs next to the DLLs; skipped while the sampling profiler runs, which owns DbgHelp).
Guest addresses are shown as `ADDR (sub_START+0xOFF)`, the nearest recompiled function at or below.

Add our database names (db/*.json) to a dump:

```
py tools\flight\flight_names.py out\build\win-amd64-release\logs\flight_NNN.txt
```

writes `flight_NNN_named.txt` next to it. Dumps of real runs contain game-derived addresses and names: they stay
in `logs\` / `out\` (gitignored).

Thread states in the summary: `IN <call> for N s` (blocked in a hooked kernel call), `running; last event N s ago`,
`exited at frame N (ExTerminateThread code X)`, `gone` (no live thread with that id and creation time: the thread
ended without ExTerminateThread). Rings are matched to live threads by thread id AND creation time, because Windows
reuses thread ids; exited / gone rings get no host or guest stack (it would be another thread's). Every ring is
copied before the dump's reference time is taken, so no age is negative and the summary agrees with the per-thread
sections.

## Overhead

Measured with `--wwe2k14_flight_test=bench@700` (run 297, selftest, main thread inside the VdSwap hook, 200,000
calls each): recording one kernel call (FlightCall + Done: two QueryPerformanceCounter reads, the ThreadState lookup
for ctx.lr, a 96-byte ring write) costs about 57 ns; a point event about 22 ns. All threads together recorded about
5,300 events/s in the selftest, so about 0.3 ms of recording per second of game, under 0.05% of one core, spread
over the guest threads (the main thread records about 30 events per frame: about 1.7 us of a 16.7 ms frame).
The run-280 in-run A/B (CP busy, frame span) can't see this cost: the CP thread makes no guest kernel calls and the
frame span is held at the vsync cap. Off: one setting read and a not-taken branch per hooked call (disassembly of
FlightNtClose), and a flags-off full regress pass (runs 300-303, 4/4 PASS against the A/A-derived thresholds,
build s8_flightfix_2; the last build only changed dump formatting, which runs only when on). Recorder on: run 304
(selftest PASS, 3/3 shots, same 1,144 error lines as flags-off run 300; bench 57.5 ns per call, 5,269 events/s).

## Not done

- Coverage build: recent guest function entries per thread. The coverage prologue (`src/coverage_prologue.h`) only
  leaves its fast path on the first call of a function per window, so a per-call ring push would mean a second
  check in every guest function's prologue (a coverage-preset-only rebuild of all generated code). Not built.
  The recorder itself (kernel calls, files, frames, script steps, dumps) compiles and works in the coverage preset
  too (it is the same header); only the guest-function-entry ring is missing. Task status: partial.
- Not exercised on the game: the "gone" state (no selftest thread ended without ExTerminateThread), the hang-dump
  cap (one hang per test run), and crash/abort dumps after a live `cvar` toggle (the forced tests are parsed only
  when the recorder is on at startup). They are small code paths; see the header comment.
