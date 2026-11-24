# tools/q: code index + query CLI (Session 8, task 5)

One SQLite index over the generated code, the loaded guest image and the linker map, and a query CLI that
answers in well under a second. It replaces the throwaway pickle indexes of earlier sessions.

Everything the index holds is derived from the game (addresses, strings, RTTI names), so it lives only in
`out\harness\index\` (gitignored). Never copy query output into `docs\` or `db\` beyond addresses, names and
one-line summaries. This README deliberately quotes no game strings or class names.

## The front door (S108 lane FD)

- `py tools\q\q.py find X [X2 ...] [--in PATH] [--all] [--json]` (tools\q\qfind.py): one call tries every index for X (C++ symbol, python / ps1
  name, file name / stem, path literal, cvar, env var, log line, game function / address / db name, SDK, code text, docs, fact) from what is cached
  (never parses or rebuilds; ~0.6 s) and prints one labelled block per kind that matched; the closing `no hit as:` line names the rest. Exit 0 /
  1 (a question with no hit at all). `q X ...` with an unknown first word is `q find X ...`; a mistyped verb (`q decmp`) says `did you mean`
  first on stdout and exits 2.
- `py tools\q\q.py multi "ours callers F" "show FILE:10-40" "decomp sub_X --fields" [--json]` (tools\q\qmulti.py): several q questions in one
  process, each answered whole between `== [k/n]` and `-- [k/n] exit E`; the last line names the failures; exit = the highest.
- Sizing (tools\q\qcap.py, one rule for find, ours refs / callers / writes / reads, py refs / def, ours text, ours lit): every list says its
  total; a cut list ends `showing N of M ...; all: <the exact call>, or narrow: add --in PATH`; a list only a few over the cap is shown whole;
  `--in PATH` narrows to paths containing PATH; `--all` = everything; `--json` is never cut.
- No dead ends: a `no such symbol` (ours / py) or `unknown function` (game) answer adds what the other probes found (`found elsewhere: ...
  -> q find X`); a bare script stem (`q py refs qfact`) answers as the script.
- Selftest: `py -I tools\q\qfind_selftest.py` (a fixture repo through the real CLI, plus three real-repo kinds).

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
| `decomp` | S103 QD: `py tools\q\q.py decomp FUNC [--fields] [--callers N] [--json] [--no-cache]`: a compact, mechanical view of ONE function (loads / stores as `[base+off]` with provenance folded, calls with db names, branches as if / do-while where the shape is plain, the return; a fields / calls summary on top; the listing line of each block). Cached under `out\harness\index\decomp\` keyed by the generated source's sha1. `--callers N` = one line per caller with its call-site arguments. `q fn` shows its one-line summary. Usage, notation, limits and the proof (`tools\q\decomp_selftest.py`): `docs\agents\q_decomp.md`. |

### Host offsets

`addr --host` uses the map that was indexed (`out\build\win-amd64-release\wwe2k14.map`). Host offsets are only
meaningful against the map of the build that produced them: every relink moves code. For a crash from another
build, pass that build's map with `--map` (parsed live, under a second). Both `sub_X` and `__imp__sub_X` map
symbols resolve to guest `sub_X`. An RVA outside every section of the map, past the last symbol, or an absolute
address that is not inside the image at its preferred base 0x140000000 (an ASLR-relocated 0x7FF6... address) is
refused with an error: pass `wwe2k14.exe+0xOFFSET` (the address minus the module base) instead. The answer
includes the section and the symbol's extent (up to the next symbol or the section end).

## The knowledge library: `q fact` (Session 98)

```
py tools\q\q.py fact WORDS... [--json] [--limit N] [--kind format|system|module|db|session] [--rebuild]
```

One ranked search over what we have LODGED (CLAUDE.md "Lodge what you learn"): `docs\formats\*.md` and
`docs\systems\*.md` (weight 1.0), `docs\modules\*.md` (0.85), the function db `db\*.json` (name, summary, evidence;
0.7) and the `SESSION*_SUMMARY.md` running logs (0.35, lowest). Each hit: file:line, the section heading path, up to 4
matching lines, and every guest address in that section with its db name / confidence / file (`fn:` line).

- All words must match (SQLite FTS5, bm25); when no section has them all, any word ("partial"). Words of 4+ letters
  match as prefixes (`particle` finds particles), shorter ones also with a trailing s (`cap` -> caps); every spelling of
  an address matches (`0x8230C688`, `sub_8230C688`, `8230c688`); `a_b` is the phrase "a b". Sections with the words side
  by side, or all of them in their own heading, rank higher.
- Index: `out\harness\index\fact.db` (our own text only), built by `tools\q\qfact.py` (no code index needed). It
  rebuilds itself when any source file's size / mtime changes (~1-2 s); a query on a fresh index takes ~0.13 s.
- `q fn` / `q db` print `documented in: <page>:<line>  <heading>` for every format / system / module section naming the
  function (from the same index).
- Write it down where `fact` finds it: a format page (`docs\formats\_template.md`), the system page, db entries.

## `q menu` (S105 QM): menu pages for route lanes

`py tools\q\q.py menu [list N | path WORDS | caw | screen NAME | seq | text WORDS | build]`: a list page's rows (text id candidates, label, what each row opens), the
press lines from the main menu to a screen, the Create a Superstar editor's tabs / pages / rows, sequence ids -> constructor names, labels -> editor code sites, and the
state words a route waits on. Code `tools\q\qmenu.py` (+ `qmenu_lodge.py` for what the flow table does not hold, `qmenu_selftest.py`); data `out\harness\index\menu.json`
(rebuilt when the atlas or the lodge changes). Format page `docs\formats\menu_pages.md`.

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

## q ours: questions about OUR code by plain text search (S103 lane QO; the index went in S110 QTEXT)

py tools\q\q.py ours def|refs|callers|callees|writes|reads|fn|body|outline|enclosing|covers|info ... answers about src\, tools\, probes\ (and android\) by a whole-word, case-sensitive text search of the files on disk (tools\q\qsym.py): no index, no compiler, no worktree overlay, the same answer in every checkout, ~0.3-0.8 s. Each hit carries its enclosing function, a code / comment / string tag and its #if chain; `Scope::name` narrows a noisy name; `rows`, `diff` and `--branch` on the symbol verbs are gone. The plan and the measurements that decided it: docs\research\s109\QOURS_plan.md. Usage: docs\agents\q_ours.md. Selftest: `py -I tools\q\qsym_selftest.py`.

## q field: the field index of the game (S103 lane QF)

py tools\q\q.py field OFF [--type T|--vtable V|--global G] [--writes|--reads] | field fn FUNC | field type T | field info answers "who reads / writes offset +N of this object" across the whole recompiled game, with the base register's provenance folded (a `this` argument, a global such as `[0x83930050]`, a vtable-identified type, a call's return). Built from the generated sources by tools\q\build_fields.py (9 s, 84 MB, out\harness\index\fields.s<SCHEMA>.db: S105 D, the schema is in the file name, a rebuild runs under a lock with a per-process temp and a second caller answers from the old file and says so; rebuilt on the first query after generated\ changes). Selftest (tools\q\qfield_selftest.py: a fixture, a planted wrong offset, three real-data proofs). Usage, roots, types, the proofs and the limits (indexed accesses are not indexed; every hit is a lead): docs\agents\q_field.md.

## q const: which code writes shader constant register N (S103 lane QC)

py tools\q\q.py const N [--vs|--ps] [--callers K] [--unresolved] | const --name g_f4EyePos | const --setters answers "which game code writes shader constant register N, and from what": every call of the constant command recorders (vs / ps / vs+ps, float, x/y/z/w, bool; the recorder list is discovered from the code and checked), r4 resolved as a literal or as a name handle (field index -> name string -> fx.json registers), the data (r5) folded into a formula, and a count of the sites it could not decide. Built from QD's decomp cache and QF's field index (tools\q\qconst.py; cache out\harness\index\qconst\, keyed by the generated sources' hash; cold about 30 s, cached instant). Selftest (tools\q\qconst_selftest.py: a fixture, planted twins, real proofs c184 / c72 EyePos). Usage, limits: docs\agents\q_const.md.

## q ours additions (S103 QX1-QX5; details in `tools/q/qours.py` docstring)
- (S110: the worktree overlay and the index are gone; every `q ours` answer is the text of the checkout it runs in.)
- `q ours covers FILE` for tools python / ps1; `def|refs|callers|callees|fn NAME` fall back to the tools python + ps1 index (`check.main`; `--tools` asks it first).
- `q ours line "<tag or log line>"` (print site + parsing tools), `q ours lit VALUE` (hex + decimal, word-bounded), `q ours includes|includers FILE`.
- S105 QB (builder transcripts: ~40 % of raw reads were `sed -n N,Mp FILE`, ~20 % searches over our python tools): `q show FILE:N-M` (also `FILE:N`, `FILE:N+-C`, `FILE:A-B,C-D`, a unique basename; numbered lines, never game data; `tools/q/qshow.py`); `q py def|refs|callers|callees|writes|reads|fn NAME` = `ours ... --tools` (our python / ps1 first, so a name that also exists in C++ answers from the tools); `q ours line` with no printf site falls back to a text match of the line's pieces (a line built by concatenation, split literals, a constant), with a WEAK marker when only one word matched.
- Selftest: `py -I tools\q\qx_selftest.py`. Not yet: shader / fx.json / SPIR-V lookups (next piece).

## q ours QGL (S103; the misses of 2026-11-18 night: two probes and a reviewer fell back to grep)
- `q ours glsl NAME` (uniform / interpolator component `inUvFog.w` / field `fc.Sweat.y` / define: read | write | decl | define | guard rows with the #if guard, #else inverted, own include guard dropped); `q ours fxreg [--dupes]` (fx_registry.h: duplicate hashes by owner, cross-owner flagged, twins with a constants table on one side only, variants no hash row names); `q ours str LITERAL` (string literal sites, printf literals also by prefix and as split literals, comments ignored; `--tools` adds scripts / json).
- Selftest: `py -I tools\q\qgl_selftest.py`, planted twins for each.

## q ours census (S103 QCF; the top miss class of probes HOL / HOL2: "which line bumps the counter behind this log field?")
- `q ours census FIELD [--in PATH] [--run N]`: the format literal holding FIELD -> the argument it prints for it (printf spec N = argument N, `%%` and `*` handled, adjacent / split literals and PRIu64 joined; `" field " + std::to_string(x)` / `<< x` concatenation) -> the counter (member / global / array element; a local is expanded through its assignments, depth 3) -> every write (`++ -- += = .store .fetch_* .exchange`, file:line, enclosing function) + the declaration. An array counter whose subscript names a member (`pic_why[q.why]`) also lists who sets that member inside functions naming its type. A counter found only by a name near the literal is marked `BY NAME (unconfirmed)`; a FIELD that is no literal but a counter name lists its writers. Text level (this checkout, always current, no index).
- Does NOT: a format held in a variable, a literal inside a `#define` (reported, not expanded), writes through a pointer handed to another function, memset / std::fill, struct-vs-name sharing (all writers by NAME are listed; `--in PATH` narrows).
- `q runlog field FIELD [--run RUN]` (also `q ours census FIELD --run N`): that field's value on every window of a run log (run number = the log file number, a path, or the newest log), identical neighbours collapsed, deltas for numbers.
- Selftest: `py -I tools\q\qcensus_selftest.py`, planted twins (a renamed counter does not resolve to the stale line, a longer name / comment / string is not a writer).

## q ours / q sdk additions, QX5 (S103; closes the q_missed.log entries of 2026-11-18 20:24-20:55)

- `q ours cvar NAME`: the REXCVAR_DEFINE_* (ours and the SDK's) with type / default / category / help, its `#if` block (stream = ON only, stream-free = OFF only, Android), every REXCVAR_GET / SET / DECLARE, every by-name string use (GetFlagByName("x"), flag_int("x")), every `--NAME=value` in tools / routes / .tsv / android. A part of a name lists the matches; `q ours def|refs|writes|reads NAME` on a cvar name answers the same (a cvar is a macro, not a C++ symbol). Text scan with a preprocessor-condition stack, not the AST. `--all` = every line.
- `q ours writes|reads|refs NAME` list every line, also inside `#if defined(__ANDROID__)` branches (text: each line shows its #if chain).
- `q ours lit VALUE --all` lists every line of every file (default 3 per file); `--tools` = tools / android / config / probes scripts only (py, ps1, bat, json ...), each py / ps1 hit marked string / comment / code.
- `q ours refs|callers FUNC` for a ps1 function also lists the OTHER scripts that call it (dot-sourced).
- `q sdk def|refs|callers NAME [--part P]` (a tail or `Class::Name`; macros, enumerators, members and SDK cvars included) answers over `thirdparty\rexglue-sdk-src\src` + `include` AND the SDK's own third-party code (QSD, S103): SDL3 (src + include: e.g. `SkipAltGrLeftControl`, the AltGr fake-Ctrl drop, is `q sdk def SkipAltGrLeftControl`), Dear ImGui (+ backends), VMA, spdlog, fmt, CLI11, toml++, crypto, disasm, pe. Every hit is tagged `[sdk]` / `[sdl3]` / `[imgui]` / ...; `--part sdl3` keeps one part. Skipped on purpose: tracy, simde, glslang / spirv-* / dxc / dxbc (build-time shader tools), vulkan-headers / vulkan-loader (generated / huge), FFmpeg, catch2, moltenvk and small vendored libs agents never read; test / docs / examples folders under the third-party roots. `q sdk info` lists the parts and their file counts. A lexer-level index, not libclang: no compile flags, overloads show as several rows.
- **`q sdk` is automatic (QSD).** There is no `update` step to forget: every call builds a missing index (7 s cold, 2575 files) and re-reads files whose size / mtime changed (0.4 s warm: a stat walk). At most 4 workers, fewer when free RAM is low (the free-RAM rule of qm.free_ram_gb). In a worktree the one index lives in the MAIN checkout's `out\harness\index\sdk.s<SCHEMA>.db` (the SDK source is there); a lock file (`sdk.s<SCHEMA>.db.lock`; S105 D: the schema is in the file name too, older files such as `sdk.db` are left alone and named in the header) keeps it to one writer, and a call that cannot refresh says `WARNING STALE` in its header instead of answering silently. The first line of every answer carries `[index built T (age ago), N files; K files re-read just now | 0 files changed since | WARNING STALE ...]`. `q sdk update` still forces a refresh. Selftest: `py -I tools\q\qx_selftest.py` (the QSD block plants a stale SDL file, a held lock, and re-asks the AltGr question on the real tree).
- Selftest: `py -I tools\q\qx_selftest.py` (QX5 cases).

## QX3: q asm, q ours hooks / text, shader names (S103; tools/q/qx6.py, selftest tools/q/qx6_selftest.py)

- `q asm FUNC|ADDR [FROM TO] [--into FROM TO]`: the per-address window of a game function's generated listing (`ADDR  mnemonic operands`, branches `=> TARGET`, branch targets `<= SOURCE,...`). `--into FROM TO` = the branches (cond / uncond / switch) that enter the range from outside, and whether the fall-through from the instruction before FROM enters it. Reads generated\default of the MAIN checkout; only branches inside the same function (other callers: `q callers`). Proof: the BELT adversary's window of sub_8272D710 and `--into 0x8272E030 0x8272E4EC` (0 branches from outside; only the fall-through from 0x8272E02C).
- `q ours hooks [ADDR|sub_X|NAME]`: REX_HOOK_RAW / REX_HOOK in src (also a function-like #define whose body holds one, invoked with a sub_X), and the [[midasm_hook]] rows of config\*.toml with registers, toml file:line, the callback's definition file:line, and the generated `extern void NAME(` declaration + call-site count. A function = also the mid-asm hooks inside it. Comments and strings are masked first; macros are expanded transitively with `##` pastes (the 76 `__savegprlr_N` hooks); each hit is tagged `[tools\: not the game build]`, `[NOT COMPILED: no translation unit includes this file]`, `[builds: ...]`, `[#if ...]`.
- `q ours text WORDS...`: every line holding all the words, case-insensitive; an address word matches every spelling; C++ / py / ps1 / toml hits are marked comment / string / code (a line-local heuristic, not a lexer). Uses `git grep` (0.5 s; tracked + untracked, never ignored files, spirv / spv skipped); Python only applies the exact word rule and the mark.
- `q ours def|refs NAME` with no C++ symbol falls back to shader text (src, tools, assets: .glsl .frag .vert .hlsl .fx.json): declarations first, then uses.
- Selftest: `py -I tools\q\qx6_selftest.py` (a fixture with planted misses, then the real-tree proofs when generated\ and the index exist).

## QX4: q vcall, q nodes, q runlog, q ours refs ADDRESS (S103; tools/q/qx7.py, qrunlog.py, selftest tools/q/qx7_selftest.py)

- `q vcall SLOT_OFFSET [--class VT|NAME] [--all] [--json]`: every virtual call site in the generated listing with that vtable slot byte offset (0x34 = slot 13, 0x4C = 19, 0x80 = 32): `lwz rX,OFF(rY)` + `mtctr rX` + `bctrl` (`bctr` tail too). Row = site, enclosing function, call / tail, the receiver (`this` = the function's own r3, `field+N of ...`, `global 0xADDR`, `result of sub_X()`, `arg rN`) and the class where known: a vtable constant stored at +0 of the object in the same function, or `this` of a function that is a slot of an RTTI vtable. Default = vtable calls (`lwz rY,0(rZ)`); `--all` adds callback-field calls. `--class` lists only sites with that known class and COUNTS the unresolved ones (never guessed). A site whose r3 at the call is not the object the vtable was read from is flagged `[FLAG: r3 ...]` (about 3 %).
  Tracking: a forward flow over the function's own branches (paths met at labels; a def survives a join only when every path agrees, else it is a per-register unknown; no fall-through after b / blr / bctr; a backward branch feeds the next pass, up to 5 passes). A call kills the volatile registers, except a register save / restore helper (`__savegprlr_N`, 236 of them in the index), which kills only r0 / r11 / r12. `this_recv` sites: 7,039 of 30,645 vtable calls.
- `q nodes [CATEGORY] [--slot N]`: the draw-node registry, the 229 `bl 0x82C30A10` (node, category, on) sites: function, category (`li r4,N`, else the unresolved source), the node's vtable (the constant stored at +0 / +off of the node in the SAME function), class, and the image word at vtable + 4 * N (default slot 1 = the draw method; `?` when it is not a function).
- Both read `out\harness\index\vcall.s<VERSION>.db` (S105 D: the version is in the file name, older files are left alone; WWE_QVCALL_DB=FILE = another file; a scan of the 345 generated .cpp, <= 4 workers at below-normal priority, RAM-capped, 15-25 s; rebuilt when the codegen fingerprint or the table version changes; temp file named with the pid, the replace retries). `--db FILE` reads / builds another file. Image override for tests: env `WWE_Q_IMAGE`.
- `q runlog WORDS... [--exe X] [--route R]`, `q runlog which --mode X`, `q runlog info|update`: (S105 D: the index file is `runlog.s<VERSION>.db`, the version in the file name; a new version starts from the older file's rows of vanished logs, which is only read and left alone; WWE_QRUNLOG_DB=FILE = another file) an index of our run logs (`logs\wwe2k14_N.log`, `out\build\*\logs\wwe2k14_N.log`, `out\release\build\logs`): per run the exe, the build time (from the game's own `exe built (file time, local)` log line when the log has it, else the exe file's time at first index, and then `REBUILT AFTER this run` when that file is newer than the run), route script, EVERY `modified:` line as `k=v@source` (command-line / runtime/code / default / a config file), version, start time, the first AND last text of each census line shape (`x N` repeats; shapes differ in hex or glued digits, only standalone decimals are normalised; 500 shapes per kind per run, a `capped` note past that), and the pacs it opened with the first frame. Words are phrases split at `_`: `rdr_owner`, `rdr owner` and `wwe2k14_rdr_owner` all find the flag (prefix on the last part).
  Incremental (stat walk, then only new / changed logs); `out\harness\index\runlog.db`; writes take `BEGIN IMMEDIATE` and re-read what is known; a log the tidy deleted keeps its rows (`[log gone]`), and a version bump MIGRATES (gone logs' rows are copied into the new tables, logs still on disk are re-parsed).
- `q ours refs|def 0x836F38D0` (a literal address where a symbol goes): the named constants whose WHOLE initialiser is the address: `constexpr / const / static` (one of several declarators, an array element, a `(uint32_t)` cast, `u` suffix, any hex case), an `enum` enumerator, a `#define NAME value`; comments and strings are blanked first; NOT `Foo(addr)`, `addr + 4`, `base + addr`. Then the refs of each name (`q ours lit ADDR` stays the every-literal answer).
- Timed (this busy PC): see `py -I tools\q\qx7_time.py` (uses temp databases; never deletes the real ones). Selftest: `py -I tools\q\qx7_selftest.py` (fixture with planted misses and one case per review finding, then the real-tree proofs).

## q asset: the index of the game's FILES (S103 lane QAS; tools/q/qasset.py, selftest tools/q/qasset_selftest.py)

`py tools\q\q.py asset find WORDS | pac PATH [PACH [ENTRY]] | tex NAME [--like] | mat NAME [PAC] [--pac P] | id evtobj|bgetc|weapon N | slot ARENA [N] | info | update [-j N] [--full] [FRAG]`

An index of every pac under game_data (extracted + dlc), read-only, with our own readers (tools/wm30/pac.py unpack / BPE / ZLIB / PACH walk; the JBOY layout of tools/arena/arena_files.py; filecensus_flags for the arena slot text). Built from the PLAYER'S files into `out\harness\index\asset.s4.db` (the main checkout's when run from a worktree; **the schema is in the file name**: a q of another schema uses another file and never opens, wipes or rebuilds this one, and the header names older index files beside it, left for the tidy): names, ids, counts and alpha statistics only, never a byte of game data, never committed.
- Levels: pac -> PACH (index, virtual path from the EPAC header's own directory, e.g. `/STG/CAGE`, `/SOBJ/2038`) -> entry (key path `1/0` for nested PACHs; kind model / texdir / pach / other / raw) -> model (nodes, meshes, vertices) / material per mesh (effect, parameters with values, sampler -> texture) / texture (format, size, mips, alpha max, share 0, share >= 224, mean of the top mip; DXT1 (1-bit: c0 <= c1 blocks, index 3 transparent) / DXT3 / DXT5 / BGRA8 / BGRA4). A directory row that is not a PACH (EVP / EVZ, CASD, bink) is its own `blob` entry with its tag and size. A model with a mesh record that fits neither layout is SUSPECT (vertex count unknown, entry note, counted in `info`); the shifted layout of bgEtc /STG3/WPO2 (effect +0x78) is decoded.
- `find` searches every name level (all words must match one name). `pac` browses. `tex` = format + alpha stats + where. `mat NAME` = a material class (effect, e.g. yCh_Reflect) or the material using a texture name; a pac stem (`mat yCh_Reflect ch139`) narrows it. `id evtobj N` = the SOBJ / BOBJ / SPMT row N of evtobj*.pac -> PACH and its entries; `id bgetc N` = bgEtc PACH N; `id weapon N` = m2.pac PACH 0 (/MOT/WEAP) key 1000/N (the path our loader LoadM2Weapon reads, labelled so) AND the bgEtc WPON / WPBS / WPO2 key N (the game's stage-weapon sets, labelled not read by LoadM2Weapon).
- **A builder testing `q asset` in a worktree sets `WWE_QASSET_DB=<a file under its own out\>` (e.g. `out\qac\asset.s4.db`) on EVERY call, build and selftest** (never the shared index; a call without it reads / builds the main checkout's file); a rebuild prints `q asset: writing <file>` first.
- `cull [PATH [PACH [ENTRY]]] [--list] [--set|--clear]` (S103 QAC): the mesh record's cull word (+0x8C; bit 0x2 set = the game draws the mesh one-sided, clear = two-sided) and technique word (+0x90), stored per mesh row (schema 4). Shifted layout (bgEtc /STG3/WPO2 + two p62 models, record 0xB4): the reader finds the effect name at +0x78, and the game's record is name, cull word at name + 16, technique word at name + 20, nothing between, so the words are at +0x88 / +0x8C [C by position]; src/assets/jboy.h:446 / 695 / 696 reads only the standard layout (name +0x7C, words +0x8C / +0x90), so it cannot confirm it; the data agrees (every shifted mesh: bit 0x4 set in the +0x88 word, values 4 / 6 only, like the standard cull word). No PATH = one row per pac family with the word values; PATH = per PACH, PACH = per entry, ENTRY = per mesh (`--list` = one line per mesh with the palette node (the first bone-palette node: unconfirmed as the mesh's node), effect, word). Arena bg pacs also split stage slots (keys 0..399) from other keys. `asset pac PATH PACH ENTRY` mesh lines show the words too. A db of an older schema is rebuilt by the first call (header says REBUILDING while partial; `q asset update` finishes it).
- Automatic: every call stats the pacs and re-reads only new / changed ones (the cold build is one `q asset update`, about 6 minutes; a query that finds the index missing builds for at most min(90 s, half of q's own timeout) (WWE_QASSET_BUDGET overrides), checked against the wall clock while waiting for a pac, and says PARTIAL in the header; a query waits at most 10 s for another writer's lock (owner-pid checked: a dead writer's lock is taken over at once) and then answers STALE). The header line of every answer says the index age. At most 4 workers, fewer when free RAM is low, one pac per worker.
- Not decoded on purpose (entries listed with size only): motion / sound / movie pacs and parameter tables (mot*, dlc_mot*, sound, evtmov*, m3 = parameter tables, mvMot, string, wuniData). m2.pac IS decoded (weapon models).
- **A cold build fills `out\wm30\cache` (~3.4 GB)**: every XCTD-packed pac is unpacked there once (tools/wm30/pac.py; the cache file name carries the source's size + mtime and is written temp-then-rename, so a replaced pac never reads a stale unpack). out is scratch: the end-of-session tidy clears that folder (and the index), so the next cold build unpacks again; the index itself is ~60 MB.
- Selftest: `py -I tools\q\qasset_selftest.py` (a fixture pac with planted misses; the real-tree proofs run when the index exists).

## S105 QG: five game-side miss classes (tools\q\qgx.py, qstores.py; `tools\q\qgx_selftest.py`)
- `q switch FUNC` (= `q decomp FUNC --cases`): every jump-table switch of FUNC as one row per target: `values [idx a..b] -> @target  li rN=K | st stw K->off(rB) | calls sub_X(rN=K)  [how the body ends, k cond branches]`, plus the bound and default. A biased switch (`addi rX,rY,-N` before the bound) prints `switch on rY - N` and the case VALUES (index + N). Constants set before the switch (function-wide single `li`) are seeded and named; `bctrl` is `calls (indirect)`; the body scan is linear (guarded / merged paths are leads). Example `q switch sub_8264FAB0` (case 501, not 0).
- `q field OFF --stores [--global G | --root-name R] [--var-span N] [--all-copies]`: `field OFF --writes` plus the writers q field never modelled: indexed stores (scalar, byte-reversed, stvx / stvx128 / stvewx / stvlx / stvrx, stfiwx, stwcx. / stdcx., dcbz: covered by width), block writers (memcpy / memmove / memset / zero-fill, calls, tail calls and the 1-instruction `b` thunks to them, e.g. the memcpy thunk sub_82C2F7B8) and `add`-formed bases (linear shadow, marked). The output ends with counts of what its filters hid and what is not modelled. Own db `out\harness\index\stores.s2.db` (~12 s build under the rebuild lock). `q field 15229 --stores` re-finds the apply writer 0x822CA0E4; `q field 525481 --stores` the three `stbx` writers behind `q imm 0x804A9`.
- `q text ID [--lang N] [--all]`: text id -> string from `pac\string.pac` (+ the newest DLC string pack with --all): `q text 282` = `SELECT ARENA`. Format: docs\formats\string_pac.md (entry name `GGLL`, LL = language).
- `q csdt [ERA [KEY]] [--lang L] [--find KEY]`: the evtdata.pac /CSDT caption schedule: `q csdt 0 10` = key 10, frames 256-804 (the caption text is the player's own game data: never quoted in the repo). Format: docs\formats\evtdata_csdt.md.
- `q save [titles|options|dlc|caw|threads|all] [--data NAME|PATH] [--json] [--names]`, `q save check EXPR...` (S106 C1): the user-data mirror's SaveData.Dat decoded (title holders / belts / plates, caption + blood options, saved DLC mask, CAWs, Threads sets); `check` exits 1 when a precondition is false (`holder:title=1:char=322`, `plate:title=1`, `option:caption=1`, `dlc:mask&0x40`). Grammar: tools\q\qsave.py header; layout docs/formats/save_data.md.
- `q ours includes HEADER --included-by | --transitive | --closure`: the include graph of our src / tools / probes (direct includers; every file that reaches HEADER; every file HEADER pulls in), with the translation-unit count.
