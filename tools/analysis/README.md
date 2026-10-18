# tools/analysis

Scripts used in session 2 to find and verify function-entry hints and switch-table fixes.
They read ReXGlue's generated C++ (`generated/default/`, where each PPC instruction is a `//` comment)
and our run logs. They print metadata only, never game code. Put outputs in `logs/analysis/` (gitignored).
Paths assume the repo lives at `C:\Users\Jarrod\wwe2k14-recomp`.

| Script | What it does |
|---|---|
| `classify_candidates.py <run log> <out.tsv>` | Parses `[wwe2k14][scan] candidate/lone` lines from a run log and classifies each target against generated code: `GAP_START` / `IN_GAP` / `INSIDE_FUNC` |
| `check_boundaries.py <tsv> <excluded hex,...>` | Accepts `GAP_START` only after an unconditional `blr`/`b`/`bctr`, `IN_GAP` only chained behind an accepted target; writes `accepted.txt` next to the TSV |
| `scan_code_refs.py <out.tsv>` | Static scan: `lis`+`addi/ori` pairs building an unregistered code address (found the 0x82BB6020 crash) |
| `postcheck_hints.py [file-substring]` | After codegen: every hint registered, predecessor ends exactly there with an unconditional transfer |
| `audit_switches.py` | Flags switches whose inline jump table looks mis-sized (rough; most flags are false alarms) |
| `true_table_sizes.py <dir>` | With `<dir>/audit_switches.txt` + `<dir>/dump_switches.log` (from `--wwe2k14_dump_words`), measures each table's true length from guest memory |
| `compare_funcs.py <genA> <prefA> <fnA> <genB> <prefB> <fnB>` | Side-by-side PPC listing of two generated functions (`=` same shape, `≠` differs) |
| `inspect_hint.py <target> <source>` | Shows a hinted function, the tail of its predecessor, and the function branching to it |
| `find_addr_refs.py <hex>...` | Finds code that builds a given address with `lis`+`addi/ori` |
| `binstrings.py <file> <regex> [max]` | Printable strings in a binary matching a regex (the shell has no `strings`) |
| `list_function.py <addr> [max]` | A recompiled function's PPC instructions with guest addresses (from generated code), checked and re-synced against its labels; use it to pick `[[midasm_hook]]` addresses |
| `timeline.py <timeline.csv> [--frames A-B] [--dump N]` | Per-frame CPU/GPU breakdown of a `--wwe2k14_timeline=true` run (game GPU waits by reason; command processor busy / idle / WAIT_REG_MEM / host-GPU wait; what ends the WAIT_REG_MEM waits) |
| `profile_stacks.py <stacks.txt> [--frames A-B] [--thread NAME] [--focus NAME] [--lines]` | Inclusive / self time per function from a `--wwe2k14_profile_stacks=true` run, one sampled thread at a time (`--thread`, default `GPU Commands`; others via `--wwe2k14_profile_threads`, e.g. `GPU Submit`, or `main thread`; `all`). `--focus` breaks a function down by direct callee; `--lines` gives self time per source line, and with `--focus` the function's time per line of its source (incl. what each call site costs). Lines need `--wwe2k14_profile_lines=true` (default) and the PDBs next to the DLLs |
| `barrier_report.py NNN [--ms 25]` | Session 8 task B: from `logs/barrier_NNN.csv` (`--wwe2k14_barrier_probe=true`, `src/barrier_probe.h`): per frame-barrier client its wake latency and work, the render thread's own time and wait per scene, per scene the dropped frames split client-bound / render-side, and per slow frame the client the render thread waited for. Starts with a pairing check on the slow rows (warns unless barrier row F-1 = frame F). `docs/research/entrance_drops_4k.md` |
| `thread_stacks.py <stacks.txt> --thread NAME [--frames A-B]` | Whole call stacks of one sampled thread grouped (guest frames + the host leaf, db names), e.g. the update thread `bclient_82260E18` in a slow-frame `frame_<N>_stacks.txt` |
| `drop_positions.py NNN...` | Every slow frame with its seconds after the scene anchor (343/324/103.bik2, m3.pac): same moments across runs = content-driven drops |
| `ab_drops.py NNN... --cvar NAME [--update-fn sub_X]` | Dropped frames per arm of an in-run A/B (arm from the timeline `cvar` events), per scene, with the update thread's work from the barrier CSV (slot picked by the function of its barrier call, via `tools/q`; default `sub_82260588` game_main_thread_loop) |
| `sleep_stats.py NNN... [--from-frame A --to-frame B]` | Totals of the `[wwe2k14][sleep]` lines (`--wwe2k14_sleep_stats=true`, `src/precise_sleep.h`): per path (precise / sdk-eligible / sdk-other) delays, mean requested / measured / overshoot, and the 1-ms histogram; leaves old-build windows with "forever" delays out of the means |
| `resolve_backtrace.py <log> [map]` | Resolves the last `[wwe2k14][crash]` backtrace in a log to `sub_…+offset` via `wwe2k14.map` (map must be from the same link) |
| `resolve_ftrace.py <log> [map]` | Resolves `[wwe2k14][ftrace]` file open/read backtraces (`--wwe2k14_file_trace=true`, see `src/file_trace.h`) to `sub_…+offset` (map from the same link; output under `out/harness/`) |
| `../bench/bench_report.py`, `../bench/run_bench.ps1`, `../bench/shot_diff.py` | Benchmark harness (Session 6): fast per-frame extractor with timeline.py's definitions, scene reports, entrance variant classifier, in-run A/B, interleaved runner, screenshot diff. See `tools/bench/README.md` |

See PLAYBOOK.md "Session 2" for the method and the pitfalls each step guards against.
