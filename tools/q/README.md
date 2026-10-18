# tools/q: code index + query CLI (Session 8, task 5)

One SQLite index over the generated code, the loaded guest image and the linker map, and a query CLI that
answers in well under a second. It replaces the throwaway pickle indexes of earlier sessions.

Everything the index holds is derived from the game (addresses, strings, RTTI names), so it lives only in
`out\harness\index\` (gitignored). Never copy query output into `docs\` or `db\` beyond addresses, names and
one-line summaries. This README deliberately quotes no game strings or class names.

## Build

```
py tools\q\build_index.py
```

About 10-13 s (16 worker processes at below-normal priority; 345 generated files, 363 MB). Writes
`out\harness\index\index.db` (~135 MB). It records what it was built from (`py tools\q\q.py info`): codegen
fingerprint (`generated\default\codegen.stamp`), size/mtime/SHA-256 of `wwe2k14_init.cpp`, the image dump and the
map, and the map's link timestamp and section extents. Every `q` call compares those with the files on disk and
prints `q: WARNING ...` on stderr when codegen, the map or the image changed since: rebuild then.

The build does not take the run lock (it would wait behind every queued game run). Instead:
- it runs below normal priority, so a rebuild during someone's perf-measured game run does not take CPU from it;
- it stats the generated files, `init.cpp`, the map and the image before and after, and if any changed meanwhile
  (another agent's `build.ps1` reran codegen or relinked) it retries (up to 3 times, 15 s apart) and otherwise
  leaves the old index in place. A torn index is never installed;
- the temporary `.tmp` database is removed whatever happens.

Options: `--gen DIR` (another generated tree, e.g. a scratch codegen `out\harness\codegen_repro\<stamp>\default`;
without `--out` this writes `out\harness\index\index_<stamp>.db`, never the main index, and `q --index THAT.db`
notes on stderr that it answers from a scratch tree), `--image FILE`, `--map FILE`, `--out FILE`, `-j N`.

### The image dump (strings, data pointers, RTTI)

Strings and RTTI need the guest image as loaded. The app has a setting for it (default empty = off; code in
`src\image_dump.h`, one call in `OnPreLaunchModule`):

```
pwsh -NoProfile -Command "& C:\Users\Jarrod\wwe2k14-recomp\tools\bench\run_list.ps1 -Scripts @('..\..\..\tools\input_scripts\selftest_title.txt') -Extra '--wwe2k14_dump_image=C:/Users/Jarrod/wwe2k14-recomp/out/harness/index/image.bin'"
```

It writes 0x82000000 .. 0x82000000 + image size (27,656,192 bytes) once, before guest code runs, and logs
`[wwe2k14][image_dump] wrote N of N bytes ...`. The image only changes when the XEX or its patches change, so this
is needed once (two dumps from different builds tonight, runs 210 and 226, are byte-identical). Without
`image.bin` the index still builds (functions, edges, immediates, host map), without strings/RTTI/data pointers.

## Queries

```
py tools\q\q.py callers FUNC
py tools\q\q.py callees FUNC [--depth N] [--all]
py tools\q\q.py str REGEX [-i] [--unref] [--maybe]
py tools\q\q.py imm VALUE [--maybe]
py tools\q\q.py rtti NAME [--slots]
py tools\q\q.py addr ADDR [--maybe]
py tools\q\q.py addr --host wwe2k14.exe+0xOFFSET [--map OTHER.map]
py tools\q\q.py db FUNC
py tools\q\q.py fn FUNC [--maybe]
py tools\q\q.py info
```

Every command takes `--json` (one JSON object on stdout) and `--limit N` (default 25 lines per list in text mode;
JSON is not truncated except `addr` code refs at 200). Errors: `{"error": ...}` on stdout with `--json` (otherwise
`q: ...` on stderr) and exit code 2; a bug in q itself gives `{"error": "internal error: ..."}` and exit code 3
(traceback on stderr). Text output never crashes on characters the console code page lacks (they print as `?`).

`FUNC` can be `sub_82CB1F50`, `0x82CB1F50`, `82cb1f50`, a mapped name (`__imp__XamInputGetState`, `xstart`,
`__savegprlr_29`) or a db name (`gpu_wait_poll_step`). An address inside a function resolves to that function
and says so. A db name or address of a data entry (a global or table) works with `db`; `fn`/`callers`/`callees`
refuse it with a pointer to `q addr`.

`VALUE`/`ADDR`: `0x...` and anything with a hex letter are hex; a bare 8-digit token is hex (an address, the way
`FUNC` reads it); for `addr` every bare token is hex. For `imm` a bare 1-7 digit token is decimal (`100`, `-1`)
and a leading zero (`010`) is refused as ambiguous. The output says how the value was read.

| command | answers |
|---|---|
| `callers` | every direct branch into FUNC from another function: `call` (bl), `tail` (b), `cond` (b<cond>), with the site address; branches into the middle of FUNC are shown with their target. Also data words that point at FUNC (vtables, callback tables) and RTTI vtable slots. |
| `callees` | direct callees with call counts and kinds; `--depth N` prints a tree (each function expanded once). Save/restore helpers (`__savegprlr_*`, `__restgprlr_*`, fpr/vmx) are hidden unless `--all`. Also the number of indirect calls (`bctrl`) and indirect jumps (`bctr`). |
| `str` | strings matching a Python regex, each with the functions that reference it (site addresses) and data words pointing at it. Strings nobody references are hidden unless `--unref`. |
| `imm` | functions using a 32-bit value: `li`/`addi`/`ori`/`cmpwi`/`cmplwi`/... immediates and full addresses from lis pairs (`lis+addi`, `lis+ori`, `lis+lwz`, `lis+stw`, `lis+lfs`, ...). So `imm 0x8362F5B4` finds every function that loads or stores that global. |
| `rtti` | RTTI classes whose name contains NAME: TypeDescriptor, base classes, every vtable (with its this-offset) and, for up to 3 matches or with `--slots`, the slot functions. |
| `addr` | guest address -> containing function + offset + generated file:line, string at/around it, vtable slot, what the data word there points to, code that builds this address (lis pairs), host RVA of the function. `--host`: host offset/RVA -> map symbol -> guest function. |
| `db` | the `db\*.json` entry (read live, so always current) + docs\systems / docs\modules mentions with line numbers. A db file that does not parse (e.g. half-written) is named in a `q: WARNING` on stderr, so "no db entry" really means none. |
| `fn` | one screen: size, generated file:line, host RVA, db entry + docs, callers, data pointers, vtable slots, callees, strings, lis-pair addresses, constants, coverage scenarios (from `out\harness\coverage\coverage.json` when task 6 has written it). |
| `info` | build metadata, counts, flow-analysis statistics, staleness warnings. |

### Host offsets

`addr --host` uses the map that was indexed (`out\build\win-amd64-release\wwe2k14.map`). Host offsets are only
meaningful against the map of the build that produced them: every relink moves code. For a crash from another
build, pass that build's map with `--map` (parsed live, under a second). Both `sub_X` and `__imp__sub_X` map
symbols resolve to guest `sub_X`. An RVA outside every section of the map, past the last symbol, or an absolute
address that is not inside the image at its preferred base 0x140000000 (an ASLR-relocated 0x7FF6... address) is
refused with an error: pass `wwe2k14.exe+0xOFFSET` (the address minus the module base) instead. The answer
includes the section and the symbol's extent (up to the next symbol or the section end).

## What the data means (and its limits)

- **Function list**: `PPCFuncMappings` in `wwe2k14_init.cpp` (83,012 recompiled bodies + 270 mapped entries without
  a body: import thunks `__imp__*`).
- **Sizes**: guest bytes, measured by walking the `// instruction` comment lines 4 bytes each and re-syncing at
  `loc_XXXXXXXX:` labels (same method as `tools\codegen_repro\compare.py`).
- **Edges**: parsed from the PPC instruction comments (`// bl 0x...`, `// b 0x...`, `// beq cr6,0x...`), kept when
  the target is outside the function's own range, or inside it but in another function that starts at or before
  the target (258 functions overlap the next one, mostly exception funclets; 38 such branches). Imports show up as
  edges to `__imp__Name`. Indirect calls are not resolved (count only). `setjmp`/`longjmp` call sites are
  ordinary edges to 0x83438D70 / 0x83438A50.
- **lis pairs** come from a forward must-analysis over each function's control flow. The in-function branches
  are the generated `goto loc_X;` lines (every conditional/unconditional branch and every case of a resolved
  `bctr` switch). At a label, a lis value survives only when every predecessor (the fall-through and each goto
  source) carries the same value; after an unconditional `b`/`blr`/`bctr` only gotos reach the next instruction;
  loops are iterated to a fixed point (all functions converge, 2 passes on average); volatile registers die at
  calls. A pair that only the old linear heuristic sees (value kept across labels and unconditional branches, the
  source of phantom references) is stored with a `?` on its op (`lis+lwz?`, about 200 of 155,000) and hidden by
  `imm`/`addr`/`str`/`fn` unless `--maybe`; the output says how many were hidden. Compared with the first version,
  the analysis also finds ~330 true pairs the linear pass missed and corrects 5 wrong values.
- **Strings**: ASCII runs of 4+ printable characters ending in NUL and UTF-16BE runs of 4+ characters outside the
  code range; mostly-punctuation runs are dropped. Code references: a lis pair that lands on a string start
  (`direct`), inside a scanned run (`split`: e.g. a float byte glued in front of a format string; not when every
  use of the built address is a float/vector load or store), on a short 2-3 character string (`short`; refused
  when any use of the address is a float/vector access, and for an aligned 2-character string followed by two
  zero bytes that reads as a float constant), or a `lis+lwz` whose image word points at a string (`ptr@ADDR`).
  A reference whose address register feeds a float/vector access is tagged `...,fpvec`.
- **Data pointers** (`ptrs`): every aligned word outside the code range whose value is a function start or a string
  start, as loaded (before guest code runs; runtime-built tables are not seen).
- **RTTI**: MSVC layout parsed from the image (big-endian): `.?AV`/`.?AU` name -> TypeDescriptor (name - 8) ->
  Complete Object Locator (word pointing at it from COL+12, signature 0) -> vtable (word pointing at the COL, + 4) ->
  slots (following words that are function starts). Base classes from the COL's class hierarchy descriptor.
  Classes without RTTI (compiled without it, or pure-data structs) are not seen. Names are demangled simply
  (`.?AVInner@Outer@@` -> `Outer::Inner`; templates left mangled).

## Coverage

When `out\harness\coverage\coverage.json` exists (task 6, `tools\coverage\cov.py build`, format
`wwe2k14-coverage/1`), `fn` prints the scenarios the function ran in ("ran in 2 of 9 scenarios: ...") and
`callers` tags each caller `[ran: scenario,...]` or `[ran: never seen]`. Other JSON shapes fall back to a generic
search for the address. Coverage is read live, so it is always current.

## Measured (2026-10-09, Session 8, codegen fingerprint 1d6b5c9a...)

- Build: 9-13 s (parse 4-6 s with 16 workers). Index 135 MB: 83,012 functions (+270 mapped without body),
  385,713 edges (335,955 calls = exactly the number of `// bl` lines; 49,758 tail/jumps), 1.2 M immediates of
  which 155,442 lis pairs (200 uncertain), ~26,000 strings, ~16,100 string references, 204,260 data pointers,
  724 RTTI classes, 717 vtables, 181,243 host symbols. Deterministic.
- Query latency: 100-175 ms per call (mostly Python start-up); broad ones (`str .`, `imm 0 --json`) a few seconds.
- Checks against known facts:
  - Sizes and instruction counts: identical to `tools\codegen_repro\compare.py` for all 83,012 functions.
  - `callers 0x83438D70` (setjmp): 3 call sites; `callers 0x83438A50` (longjmp): 4 (3 bl + 1 tail call), as
    PLAYBOOK (Lettering fix) says.
  - `callers sub_82CB1F50`: the 7 wait loops listed in `db\gpu_sync.json` (`gpu_wait_poll_step`), same functions.
  - `imm 0x8362F5B4`: only `sub_82CB1F50` (the watchdog threshold global named in its db evidence).
  - RTTI: whole-table cross-check against ReXGlue's own vtable scanner (`VTableScanner: vtable at ... has N slots`
    lines in the debug codegen log `logs\codegen_005.log`): all 717 vtables at the same addresses with the same
    slot counts; names differ only in formatting (the scanner cuts nested names at the first `@`, the index
    demangles `Inner@Outer` as `Outer::Inner`).
  - Strings: a tron-path lookup (`str -i` with the tron folder name) leads to `sub_822C3350`, checked by hand
    against its instructions (`lis r10,-32256` + `addi r6,r10,27888` = 0x82006CF0).
  - Phantom check (review finding): in `sub_83455068` the label at 0x834550A8 is reached only from a `bne` that
    comes before the `lis r11`; the old index reported lis+lwz references there, the new one marks them `?` and
    hides them. Hand-checked samples of the remaining `?` pairs show the same shape.
  - Host offsets: `addr --host wwe2k14.exe+0x3303970` -> `sub_82CE3868+0x400` in the current map. The PLAYBOOK's
    `0x3303185` belongs to an earlier build whose map is gone (relinks move code), so it cannot be re-checked
    literally; use `--map` with the crashed build's map.
