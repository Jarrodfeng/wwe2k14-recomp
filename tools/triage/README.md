# tools/triage: crash auto-triage (Session 8, task 13)

One command after any run that went wrong (or any run at all):

```
py tools\triage\triage.py 274                 (run number: wwe2k14_274.log in out\build\win-amd64-release\logs)
py tools\triage\triage.py path\to\wwe2k14_NNN.log
py tools\triage\triage.py 274 --brief         (verdict, culprit, next command only)
py tools\triage\triage.py 274 --json          (everything as one JSON object, for other tools)
py tools\triage\triage.py 274 --no-events     (skip the Windows event log)
py tools\triage\triage.py 274 --map OTHER.map (a saved map of the build that ran)
py tools\triage\triage.py 274 --save          (also out\harness\triage\run_274.json)
py tools\triage\triage.py 12 --switch sub_822BAAF8   (switch recipe on one function with run 12's [dump] words)
```

Read-only: it never launches the game, takes no run lock and writes nothing outside `out\harness\triage\`.
Runs in about a second (the first map lookup after a relink parses the map once, ~3 s, then it is cached in
`out\harness\triage\cache\`).

## What it reads

- The run log, all rotated parts (`wwe2k14_NNN.K.log`, oldest first): `[FATAL]`, `[wwe2k14][crash]` blocks (host
  backtrace + guest context), guest access violations, GPU crash diagnostics (device lost, device fault, checkpoints,
  records), readback-buffer eviction lines, input-script results, shutdown lines, file opens, `[frames]` summaries,
  `[wwe2k14][flight]` dumps and forced tests, `[wwe2k14][dump]` words, `[scan]` candidates.
- `logs\frames_NNN.csv` (did frames stop before the log did; longest frame).
- The Windows event log through `winevents.ps1` (read-only): Application event 1000 entries of **this run's process
  only**: the faulting application path must be this exe (not the coverage build, a bisect snapshot or another
  program) and the process start time ("Faulting application start time") must be 0-3 s before the log's first line
  (measured on all 27 real entries: +0.04 to +0.59 s). A time window alone is not enough: 91 of 279 release run
  pairs start less than 5 s after the previous log ends, and a crash event is written ~0.5 s after its last line.
  An event never turns a log with shutdown lines into a crash (it is noted as a crash on exit). Entries near the run
  that belong to other processes are listed under "also seen". System nvlddmkm event 153 (GPU resets) has no
  process identity: counted between the previous run's last line and the next run's first line. Plus the same
  exception at the same exe offset in other processes ("same signature"), each marked same build / other build by
  the exe's PE time stamp.
- Every crash block in the same log folder (raw byte search, cached per file in `out\harness\triage\cache\`): a
  **guest-side signature** (exception code, the guest AV tied to the crash, guest lr / ctr / last indirect target,
  first 3 guest return addresses) finds the same crash in other runs even after a relink moved its host offset
  (run 050 at +0x33031BD links to 030-049 at +0x3303185 and 051).
- The code index (`out\harness\index\index.db`, task 5) for function extents and references, the image dump
  (`out\harness\index\image.bin`) for jump tables, the linker map for host offsets, `db\*.json` for names,
  `config\*.toml` for hints that already list an address.

## Classes (strongest evidence first) and the recipe each one runs

| class | evidence | automated recipe (PLAYBOOK) |
|---|---|---|
| `missing_function` | `[FATAL] Call to invalid or unregistered function` | Session 2 classifier: REGISTERED (fixed since, names the hint file) / INSIDE_FUNC / GAP_START / IN_GAP against the index, check_boundaries acceptance (predecessor ends in `blr`/`b`/`bctr`, no label past the target; IN_GAP only chained), the run's own `[scan]` evidence, data words and lis pairs pointing at it, the hints_live line to add |
| `switch_out_of_range` | exception 0xC000001D (crash block, or event 1000 when the log has none) | host offset -> map -> sub_; checks the bytes there are `ud2`; walks the function's switches (bctr address, register, ReXGlue's labels, table address); measures each table's true length from the image (consecutive entries inside the function); prints `--wwe2k14_dump_words=<bctr>:<n>` and, when the table is longer, the `[[switch_tables]]` block |
| `guest_access_violation` | 0xC0000005 with an "Unhandled guest access violation" line right before the crash block (same host thread, within 1 s; or within 0.1 s), or, with no crash record, an AV where log and frames stopped | near-null = null object + field offset; faulting function from the map; host backtrace resolved; guest lr/ctr/stack resolved against the index; r31 = 0 -> the setjmp/longjmp lesson (Session 3); same-signature runs from the event log; llvm-objdump command for the faulting instruction. Earlier guest AVs (fault-safe probes: det_hash, peek) are only a secondary note |
| `host_access_violation` | 0xC0000005 in host code, no guest line tied to it | the same, culprit = host symbol. **Fault outside the exe** (the crash handler prints every address as `wwe2k14.exe+offset`; offset past the exe's SizeOfImage or wrapped = an SDK/system DLL): the module comes from the first backtrace frame after the exception dispatch (or the event's faulting module), the map is not used, confidence medium; rexgpu-xenos.dll / rexruntime.dll frames are symbolised with their PDBs (llvm-symbolizer) while nothing was rebuilt since the run |
| `device_lost` | "Failed to await submission completion" / GPU crash diagnostics; abort() through rexgpu-xenos.dll; or (D3D12 era) ucrtbase 0xC0000409 + GPU resets in the last minute | device-lost frame and submissions in flight, fault types (write-invalid = freed memory), checkpoint stages and the records around them by kind, readback eviction state (over the cap with the original policy right before the loss = the Session 3 pattern), GPU reset count |
| `abort_other`, `host_exception` | abort() / other exception codes without the above (0xC000001D outside the exe is a `host_exception`, not a switch) | backtrace and guest context |
| `script_failure` | `[wwe2k14][input] script FAILED` | failing line, failure screenshot, last steps |
| `script_stall` | no shutdown, script stuck in a wait for > 600 frames while frames kept coming | how long, whether it was the run_list 720 s kill, the last fps (a slowdown can starve a wait) |
| `hang` | no shutdown, frames stopped > 10 s before the log's last line (or no frames at all) | last frame, last steps, flight recorder pointer |
| `ended_abruptly` | no shutdown, no crash record, no event for this process: killed from outside, or a silent hang where every thread stopped logging with the frames (run_list's KILLED line tells which) | duration vs the run_list timeout, last step |
| `startup_error` | the log stops before guest code ran | the error lines |
| `clean_exit` | shutdown lines, nothing failed | PASS / quit line |
| `no_log` | the file has no timestamped lines | - |

Also reported when present: GPU resets during a run that ended otherwise, readback over-cap evictions survived,
PPC trap lines, logged guest access violations that did not end the run (fault-safe probes such as det_hash),
flight recorder dumps and deliberate `--wwe2k14_flight_test` failures, stale code index.

**Dump runs:** any run with `[wwe2k14][dump]` lines (`--wwe2k14_dump_words`), whatever its class, gets every dumped
range matched to its switch (range at the bctr, at the inline table, at an offsets table or at a relative table's
base) and re-measured with the live words; it also compares the live words with the image dump. So the
switch_out_of_range recipe's "dump run" step works: run the printed run_list command, then triage that run (or
`--switch sub_X` for one function). Run 012 (Session 2, 66 tables): all 66 matched, all ok, 3,389 live words equal
to the image.

## Host offsets and stale maps

A host offset is only meaningful against the map of the exact build that ran. Triage resolves host offsets only when
the map's link time and the exe's mtime are both before the run's first log line; otherwise it says "map stale" and
leaves them unresolved (it never resolves against a later build: Session 8 relinks several times an hour). So run
triage right after a crash, before the next build. Guest addresses (lr, ctr, stack, FATAL address) do not depend on
the map and are always resolved against the current index (note: function starts can move when hints are added).

## Validation

```
py tools\triage\validate.py --runs 1-59        (known outcome per run from PLAYBOOK / the event log; table in out\harness\triage\validation.md)
py tools\triage\validate.py --batch 60-300     (verdict per run, no known table; out\harness\triage\batch_60-300.md)
py tools\triage\selftest.py [--log LOG] [--tsv classify_candidates.tsv]
```

Results (2026-10-09, Session 8): 59/59 historical runs (001-059) classified as PLAYBOOK / the event log say;
runs 060-300 all classify (clean exits, script failures, one script stall at the run_list kill, two killed runs);
run 279 (a real game run with `--wwe2k14_flight_test=...,crash@2000`, map fresh) resolved the deliberate null write to
the exact instruction in our flight-test code (checked with llvm-objdump), the 15 s stall at frame 1001, the event log
entry and the three flight dumps. Culprits of Session 2-3 host crashes are "map stale" (those builds are gone).

Switch walker re-audit (selftest): all 1,159 generated switches are found; 1,090 use absolute 32-bit tables right after
the bctr and measure exactly ReXGlue's label count in the image; **69 use 8/16-bit offset tables in .rdata plus a base**
(`lbzx`/`lhzx`, sometimes `<< 2`), contrary to PLAYBOOK Session 2's "all 1,159 tables are inline"; for those the
index's bound check (`cmplwi` on the switch register) equals ReXGlue's labels in all 68 that have one. The two fixed
decoders show `cmplwi 7` (8 cases) while their tables hold 11: that is exactly how ReXGlue got 8.

`selftest.py` covers what the historical logs cannot: the classifier against `classify_candidates.py` on a run's
`[scan]` targets, a replay of the three Session 2 switch crashes with ReXGlue's old label counts (must propose exactly
today's `switch_tables.toml` labels), synthetic ud2 / access-violation crash logs at real host offsets of today's
build (fresh map path), a replay of the FATAL addresses with their hint removed, a synthetic hang, event identity
(synthetic entries, and every real wwe2k14 entry in the event log maps to exactly one run), synthetic DLL faults
(huge and wrapped offsets, 0xC000001D in a DLL), guest-AV correlation (a probe AV long before a host crash, a probe
run killed while frames kept coming), an empty log, a map with no exe, and the dump run 012. Synthetic logs live in
`out\harness\triage\selftest\` and say so in their first line.

Review fixes (Session 8, after the task 13 review): event attribution by process identity, DLL faults, guest-AV
correlation, dump runs, same-signature build marking, map-cache writes made atomic and pruned to 3, `no_log`,
map/exe edge cases, `validate --batch` no longer overwrites validation.md. Validation unchanged: 59/59 (56/59 with
`--no-events`), and the batch over 060-282 gives the same classes as before the fixes.
