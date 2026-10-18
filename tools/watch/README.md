# tools/watch: memory watch and snapshots (Session 8, task 10)

Two input-script commands, implemented in `src/guest_watch.h`. Nothing runs unless a script uses them, so normal
behaviour is unchanged.

| command | does |
|---|---|
| `watch ADDR [LEN] [any\|change\|VALUE]` | x64 hardware data breakpoint on guest `ADDR` (LEN 1/2/4/8, default 4; ADDR a multiple of LEN) on every thread. Logs each write (rate-limited) with the guest frame, the old and new value, the writing guest function, a host call chain mapped to guest functions, and a guest stack walk. `any` = every write (default), `change` = writes that change the value, `VALUE` = writes storing that big-endian value (decimal, negative or 0x hex). Up to 4 at once. |
| `unwatch [ADDR]` | stop one (or all) and log the final per-site summary |
| `watch_zero ADDR` | the Session 3 command, now an alias: writes that turn a non-zero word to zero |
| `snap ADDR:LEN [NAME]` | copy guest `ADDR..ADDR+LEN-1` (up to 16384 bytes; 8 ranges, 64 KiB in all) at every VdSwap to `out\harness\watch\<run>\snap_<NAME>.bin`. A NAME can be used once per run, case-insensitively (default `snapN`) |
| `unsnap [NAME]` | stop one (or all) |

`<run>` is the log number (`wwe2k14_212.log` -> `212`; other exe folders are prefixed, e.g. `win-amd64-coverage_012`).
Everything under `out\harness\watch\` is game-derived memory: it never goes into the repo.

## What a hit line says

```
[wwe2k14][watch] hit #N 0xADDR/LEN OLD -> NEW frame F by sub_XXXXXXXX+0xOFF (thread T, guest thread G, site hit K);
  host: sub_A+0x.. < sub_B+0x.. < ...; guest lr L r1 S ret: R1 R2 ...
```

- **Writer** `sub_XXXXXXXX+0xOFF`: data breakpoints trap after the store, so this is the host instruction after it,
  looked up in `wwe2k14.map` (loaded in the background at the first `watch`). `OFF` is a **host** byte offset into
  the generated body, not a guest offset. To find the guest store, list the function
  (`py tools\analysis\list_function.py 0xXXXXXXXX`) and look for stores of LEN bytes to the address.
- **host**: the host call chain from the trap context (RtlVirtualUnwind). Guest `bl` calls are real host calls, so
  this is the guest call chain; tail calls (`b`) fold away. Frames outside the exe show as `module+0x...`.
- **guest**: the guest thread's `lr`, `r1` and a back-chain walk of its stack (as in `src/crash_handler.h`). The
  generated code keeps registers in host registers between calls, so `r1`/`lr` can lag inside the writer; the
  first return address may be the caller's caller. `watch_report.py` resolves them with `tools\q`.
- The same rows go to `out\harness\watch\<run>\hits.tsv` with raw `wwe2k14.exe+0x...` offsets
  (`py tools\q\q.py addr --host wwe2k14.exe+0x...` resolves them offline against the same build's map).

**Rate limit** (a hot word is written every frame): per distinct writing instruction only the first
`--wwe2k14_watch_stacks_per_site` hits (default 4) are logged, and per watch at most
`--wwe2k14_watch_lines_per_second` lines (default 20) per **60 guest frames** (despite the name: at the 30 fps
entrances that is 2 seconds). The rest are counted per site and summarised every `--wwe2k14_watch_summary_frames`
frames (default 600) while hits go unlogged, by `unwatch`, and by the crash handler (`crash summary`). A `cvar` step
that changes these limits takes effect on the next frame:

```
[wwe2k14][watch] final summary 0xADDR/LEN (any write) at frame F: N writes since frame A, M matching, K logged;
  now V; sites: sub_X+0x.. xCOUNT (frames a-b, last V); ...
```

Limits: 4 hardware slots in all. Every 120 frames a helper re-reads every thread's debug registers and re-sets
the ones that differ (new threads, reused thread IDs, registers put back by exception dispatch); writes on such a
thread before that pass are missed, and the pass logs `armed N threads ...: X new, Y had lost or never got the
registers`. Each trapped write costs an exception (microseconds); a word written thousands of times per frame slows
the game while watched. Each `watch` / `unwatch` / `watch_zero` step stops the script's (VdSwap) thread for about
40-60 ms while ~110 threads are suspended and set one at a time (160 ms for the 4 watches of run 276; the
`unwatch` frames of run 298 took 59 and 58 ms): put watches
before the moment you measure, not inside it.

A `watch` or `snap` that is refused when it runs (4 slots in use, address already watched, arming failed, 9th range,
more than 64 KiB, a NAME used before, file not writable) fails the script (`on_fail`), so a route never passes with
a watch silently missing. `unwatch` / `unsnap` of something that isn't there only warns.

### What a watch cannot see

The breakpoint is on one host view of the guest memory, so "N writes" means "N CPU writes through this address".
The SDK maps the same memory more than once (`thirdparty/rexglue-sdk-src/src/system/xmemory.cpp`, heap setup and
`map_info`):

- 0x80000000-0x8FFFFFFF and 0x90000000-0x9FFFFFFF map the same pages: a write through the other alias is missed
  (`watch` logs a note).
- 0xA0000000 / 0xC0000000 / 0xE0000000 are separate host views of physical memory, and the GPU / command processor
  writes physical memory through yet another view (`TranslatePhysical`: resolves, memexport, EVENT_WRITE, ring
  read-pointer writebacks). A resolve target or GPU-written word can show 0 writes while it changes every frame
  (`watch` logs a warning for 0xA0000000 and up). Use `snap` for those: it reads the memory, whoever wrote it.
- Kernel-mode writes (e.g. a ReadFile filling a guest buffer) are probably missed too (user-mode debug registers;
  unconfirmed).
- 0x7F000000-0x7FFFFFFF (emulated GPU writeback / MMIO) is refused by both `watch` and `snap`.

## Snap files and the report

Per range a 64-byte header (`W2KSNAP1`, u32 guest address, u32 length, u32 header size, name) and per frame
`u64 frame, u8 status` (0 unreadable, 1 bytes follow, 2 unchanged) plus LEN bytes for status 1. Copies are
fault-safe; after a failed copy the range is retried every 60 frames (each retry also produces one SDK
"Unhandled guest access violation" log line: 38 lines for the 2258 unreadable frames of run 284). The copy is
taken at VdSwap on the presenting thread, which is not synchronised with the update thread (a snapshot can catch the
game mid-update). Files are flushed every frame, so a crash, a hang or a run_list kill keeps every frame up to the
last one presented. Script `mark ID TEXT` lines are also written to `marks.tsv` in the same folder while a
watch/snap runs; `snap_report.py` merges them with the marks in the run's log, so earlier and later marks work too.
A LEN that is not a multiple of 4 is reported in whole words, the last one zero-padded (padding never counts as a
change).

```
py tools\watch\snap_report.py RUN                         every word that changed in the run (count, first/last value)
py tools\watch\snap_report.py RUN --mark ID [--window N]  before (mark-N) vs after (mark+N): changed bytes per word
                                                          (mask), values as u32/s32/float, frames that changed it,
                                                          per-frame changed-byte counts relative to the mark
py tools\watch\watch_report.py RUN [--address 0x...]      hits per watched address, callers, resolved guest stack,
                                                          plus the log's summaries and marks
```

## Demo route

`py tools\watch\make_demo.py` writes `demo_lesnar_watch.txt` (the Lesnar vs Bryan benchmark route plus watches on
the frame-rate word, the scene word, the match clock and a `watch_zero`, snaps of the timing block and the
0x836F39xx globals, and marks at both entrances and the match). Run it with:

```
pwsh -NoProfile -Command "& C:\Users\Jarrod\wwe2k14-recomp\tools\bench\run_list.ps1 -Scripts @('..\..\..\tools\watch\demo_lesnar_watch.txt')"
```

Results (run 276, 209 s): the frame-rate word gets exactly 4 writes (halve `sub_82BB6D50` from `sub_82331628`
at frames 3541 / 5104, restore `sub_82BB6DF0` from `sub_8232F7D8` at 5027 / 7357; the same frames as the `[pacing]`
hook log of that run); the match clock is ticked by `sub_827C1B28` (865 of the clock's 870 writes; 4 of them
logged with stacks, the rest summarised) and zeroed by three other functions; the scene word has four writers. Write-ups: `docs/systems/frame_pacing.md` and
`docs/systems/match_state.md` ("Session 8 task 10 update"). Frame times with 4 watches and 2 snaps matched the
baseline run 208 (mean 16.67 / 33.33 / 16.67 ms in menus / entrances / match).

`selftest_watch.txt` (title route, ~40 s) checks the variants: VALUE filter, LEN 1 and 8, `change`,
`unwatch ADDR`, an unnamed hex-length snap, a 6-byte snap, an unmapped snap range, a mark before the first watch, and
(as its last step, on purpose) a refused snap name that fails the script. Run 284 checked the first version (with a
refused duplicate watch that the script then still ran past; now that would fail the script).
