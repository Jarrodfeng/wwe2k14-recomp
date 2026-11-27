# tools/q: code index + query CLI (Session 8, task 5)

One SQLite index over the generated code, the loaded guest image and the linker map, and a query CLI that
answers in well under a second. It replaces the throwaway pickle indexes of earlier sessions.

Everything the index holds is derived from the game (addresses, strings, RTTI names), so it lives only in
`out\harness\index\` (gitignored). Never copy query output into `docs\` or `db\` beyond addresses, names and
one-line summaries. This README deliberately quotes no game strings or class names.

## Adding a verb (S111 QVERBS): a NEW file, q.py is not edited

Every verb that is not one of the hand-wired special ones (find, multi, switch / text / csdt, decomp, menu, py, missed, asm, args, vcall, nodes, the argparse
verbs callers ... info) is registered by ONE file `tools\q\verbs\NAME.toml` (tools\q\qverbs.py has the full key list). q.py builds its usage text (`q -h`),
its first screen (`ROUTING`), the `VERBS` tuple (typo hints) and the dispatch from those files, so two lanes adding verbs never touch the same line.

1. Write the module `tools\q\qNAME.py` with `main(argv)` (argv = the words after the verb; add `con` / `con_resolve` in `needs` for `main(argv, connect(DB)[, resolve])`;
   `main = "other_name"` when the function is not `main`). Return the exit code.
2. Write `tools\q\verbs\NAME.toml`: `name`, `module`, `errors = ["qNAME.MyError"]` (your error classes; `q: MESSAGE` on stderr, exit 2; QError is caught too, a
   closed pipe returns 0), one `[[usage]]` table (`text` = the usage lines as `q -h` prints them) and one `[[route]]` table (`text` = `- the question: the command`).
   Leave `usage_slot` / `route_slot` out: the lines then go at the end of their list (`order` only sorts inside a slot). Copy `tools\q\verbs\slice.toml`.
3. `py -I tools\q\qverbs_selftest.py` (every verb file loads, its module exists, its slots exist). Nothing else in `tools\q` changes; the docs row goes in this README.

A verb file that does not parse is skipped with one `q: verb file X skipped: ...` line; the other verbs still run. Hand-wired verbs and the markers
(`{{usage:SLOT}}` / `{{route:SLOT}}` lines in q.py's docstring and `ROUTING`) are listed in the comment above `import qverbs` in q.py.

## The front door (S108 lane FD)

- `py tools\q\q.py find X [X2 ...] [--in PATH] [--all] [--json]` (tools\q\qfind.py): one call tries every index for X (C++ symbol, python / ps1
  name, file name / stem, path literal, cvar, env var, log line, game function / address / db name, SDK, code text, docs, fact) from what is cached
  (never parses or rebuilds; ~0.6 s) and prints one labelled block per kind that matched; the closing `no hit as:` line names the rest. Exit 0 /
  1 (a question with no hit at all). `q X ...` with an unknown first word is `q find X ...`; a mistyped verb (`q decmp`) says `did you mean`
  first on stdout and exits 2.
- `py tools\q\q.py pairs --coverage [--all] [--json] [--no-game]` (tools\q\qpairs_cov.py; selftest qpairs_cov_selftest.py): S111 PAIRCOV: every q ostate
  site (DrawState block, kTable row, InstancedDraw store) PAIRED / PAIRED-NO-FACTS / UNPAIRED / OURS-ONLY / NEVER-COMPARED by q codediff's own pairing rule
  (qcodediff.cite_scope), the unpaired ones with candidates ([C] a kTable addr cell; [L] kind class, fn cite, seam, q assume, render_categories.md), and the
  OUTSIDE list: DrawState field stores no q ostate fact reads (draws the code diff cannot see).
- `py tools\q\q.py pairs OURS_NAME | FILE | --game ADDR | --stats [--json]` (tools\q\qpairs.py; selftest qpairs_selftest.py): THE JOIN of the code diff, which
  game function(s) (+ offset) each of our functions / constants mirrors: the `sub_8XXXXXXX` / `0x82..` citations of src comments attributed to the enclosing
  function (split parts keyed to their parent), plus db\claims `ours` and db\seams rows, each with its relation (MIRRORS / WATCHES / CITES, tools\q\qpairs_rel.py: the diff tools pair only MIRRORS); cache out\harness\index\pairs.s3.db, incremental by file stamp.
- `py tools\q\q.py marks [--mark U|L|C] [--seam T] [--note N] [--addr A] [--json]` (tools\q\qmarks.py; selftest qmarks_selftest.py): the [U] / [L] rows of
  db\claims and db\seams plus the seams' open items, totals first, each with its address, note and the sentence that says what settles it.
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
`out\harness\index\index.db`. It records what it was built from (`py tools\q\q.py info`): codegen
fingerprint (`generated\default\codegen.stamp`), size/mtime/SHA-256 of `wwe2k14_init.cpp` and the image dump.
The linker map is NOT an input (S110 QREBUILD: it changes with every relink and made every `addr` / `fn` call
rebuild): `q addr --host` and the host RVA in `addr` / `fn` read it through `tools\q\qhostmap.py`, one small
`hostmap.<size>.<mtime>.db` per map version, parsed on the first call after a relink.

A stale index (codegen or image changed) never blocks a lookup: the call answers at once from the newest complete
index with ONE line `q: WARNING stale: this answer is from the index built ...; <what is being done>` and starts at
most one background build (log `out\harness\index\build_index.log`). Or it says `a rebuild is running (pid N)`, or
`the last rebuild for these inputs FAILED at T (...); no automatic retry before T+10 min` (the attempt record
`index.db.attempt.json`). `WWE_Q_NO_AUTOUPDATE=1`: no background build; the line still prints.

The build does not take the run lock (it would wait behind every queued game run). Instead:
- it runs below normal priority, so a rebuild during someone's perf-measured game run does not take CPU from it;
- one build at a time (`index.db.lock`, holding the pid; a dead holder's lock is taken over);
- it stats the generated files, `init.cpp` and the image before and after, and if any changed meanwhile
  (another agent's `build.ps1` reran codegen) it retries (up to 3 times, 15 s apart) and otherwise
  leaves the old index in place. A torn index is never installed;
- a reader holding `index.db` open (Windows refuses to replace an open file) never fails or delays the build: the
  new file waits beside it as `index.db.pending-<ns>-<pid>`, q reads it from there, and the first q call that finds
  nobody holding `index.db` puts it in place (`tools\q\qpublish.py`; every tool that opens `index.db` by name reads
  that copy too: `qnames.ro` / `stamp`, qx7, qcopier, qx6, qfield, qmenu, triage, labelling, validate);
- the background build is started through a launcher that exits at once, so it is not in the starting q call's
  process tree (q's 120 s watchdog kills that tree), and it uses 6 workers (a build by hand keeps the default);
- the temporary `.tmp` database is removed whatever happens (a killed build's by the next build).

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
| `words` | S111 QGAPS: `q words ADDR [N] [--as u32\|u16\|u8\|f32\|str]` (`ADDR..ADDR2` = a range): the data words of the game image (`out\harness\index\image.bin`): hex, decimal, a float when the bits are one, a pointer's C string (a name table in one call). The image's initial values. (`qwords.py`, selftest `qwords_selftest.py`) |
| `cstr` | S111 QCSTR: `q cstr ADDR [--n N] [--wide]` = the NUL-terminated ASCII (UTF-16BE with `--wide`) at an image address; `q cstr --table ADDR --count N [--stride S]` = a table of string pointers (index, entry address, pointer, string); `--inline [--len L]` = the tag text sits in each entry (the UIPR chunk tags: `--table 0x8362D3C0 --count 10 --stride 8 --inline`). Same image as `q words`. (`qdatatag.py`, selftest `qdatatag_selftest.py`) |
| `imm` as data | S111 QCSTR: `q imm POF0` / `q imm 'BEEF'` (a 4-char token that is not a number = the tag's word; quote one that reads as hex) and any value above 0xFFFF also lists where its 4 bytes sit as DATA in the image: address, unaligned or not, the next word, the data words that point at that address and the code that loads that address as a constant. Bytes inside game functions are left out (counted). A tag read from an asset file (UIPR) is in no image data: only its code immediate shows. (`qdatatag.py::data_hits`, hook `cmd_imm` in q.py) |
| `shader` | S111 QGAPS: `q shader HASH\|VARIANT [--effect E] --reads`: the constant lanes (`c155 .w x2`), indexed constants, bool constants, samplers (slot, 2D / Cube, destination swizzle) and fetches a game shader reads, from the SDK's disassembly of the player's microcode. First call per effect scans the pacs (~20 s); the read list (no bytes) is cached in `out\harness\shaderreads\`. (`qshader.py`, selftest `qshader_selftest.py`) |
| `gstate` | S111 CDSTATE: `q gstate sub_X [...] [--json]`, `--setter KEY [--value V] [--all]`, `--list`, `--facts`, `--direct`: per game function, the render-state setter calls (the 26 recorder wrappers of vtable 0x8206C6D0, the texture record `sub_82BB1750`, the save / apply pair `sub_82BD5460` / `sub_82BD54D0`) in listing order with their constant arguments (traced over the listing's paths by `qrstdiff.flow`, else `reg ROOT` from the decomp); a getter read is a save, a setter fed that getter's value prints `restore of +OFF`; a function passing its own rN to a setter is a forwarder and its callers' value is listed `via` it (one level). `--facts` = the code-diff fact form `{side, fn, field, value, cond, at, kind}`. Index `out\harness\index\gstate.<key>.json`, built on first use (~5 s). (`qgstate.py`, selftest `qgstate_selftest.py`; `docs\research\s111\CDSTATE.md`) |
| `slice` | S111 SLICER: `q slice sub_X+OFF REG\|[mem] [--depth N] [--budget N] [--expr] [--json] [--no-cache]`: where a value in the GAME's code comes from, walked def-use BACKWARDS over qdecomp's interpreter (one engine: every operand value is qdecomp's expression). REG = the register as the instruction reads it (a store's value register, a call argument), or as it writes it when it only writes it; `[mem]` = the value a store stores / a load loads; OFF hex as `q decomp` prints it. Each hop is one line `label = value   -- what it is`: a join value per path with that path's condition; a loop induction as `base + step*?loop`, a loop-carried value `?loop`; an argument climbs into every direct caller (`--depth`, default 6; 8 sliced, all counted; a slot function with no direct caller names its vtable); a call result goes into the callee's return value(s) with the callee's arguments bound to the call's; an indirect call's result is `?vcall(+SLOT)`; a load is a FIELD named by its canonical root: a class (the vtable a slot function's `this` carries; a subobject vtable stored at this+off by a constructor is that class +off; a global word a constructor's result is stored in is that class; slots come from the data words pointing at a function, cut at the next vtable start or code-built vtable address, never from the field index's slot table, which reads 0x82023270 past its end), a global word, an absolute address (`0.1[0x82003E28]`: the image value), a table (`[4*idx + 0x83622ACC]` with its first image words), a pointer at +D of another root; its WRITERS = the stores to the same root and offset lattice (an array element `base + stride*k`; two strides must divide) in the class's slot functions, the functions using the class's globals, their direct callees (bound through the call) and, for an absolute word, the field index's W rows; each writer's value is sliced in its turn (`[C]`; `[L]` = through a callee whose base is a global word not proven to hold the object). `--expr` = the value as one expression in the start function's input terms (callee returns inlined, `phi(cond: a \| ...)`). Per-function facts cached in `out\harness\index\slice\` (keyed by the generated source hash + qslice / qdecomp code); a re-find reads 1-800 functions: ~0.1-15 s cold, ~1-10 s warm. Known over-approximation: a lattice has no upper bound (an array's records past its end can look like a later array's field). (`qslice.py`, selftest `qslice_selftest.py`: planted phi / call return / argument / field / class array / loop + the MATSIZE, CUBELEVELS, ALPHAWREST re-finds) |
| `struct` / `class` | S111 STRUCTREC: `q struct CLASS\|ROOT [--range LO-HI] [--json] [--no-cache]` = an object's layout from every access in the GAME's code, one row per offset: width(s), kind (u32 / s16 / f32; an `extsh` / `extsb` of the load = signed), R / W / P counts (P = this+off handed to a callee), the constants written, the functions (`sN:` = slot N of the class, `+OFF.sN:` = of a subobject's vtable, `ctor:` / `dtor body:`, `via sub_X` = a direct callee read with the object bound through the call), the index of an array row (`+1388 + 5292*k`, `k = 5292*[this+268]`; a multi-index access keeps every stride; a loop-walked row joins another only as an unrolled loop of the same field: stride a multiple <= 16, base inside one element), and a name: a lodged field claim (`kind field` keys vt:VT+OFF, also through a subobject's vtable, g:ADDR+OFF, ctor:sub_X+OFF; [C] / [L] carried) else `f_0x18_u32` / `a_0x9A0_x48_f32` / `vptr` / `sub_0x20_vptr`. Header: ctors (a slot = dtor; a function storing the vptr before calling another class's vptr-storing function on this = `dtor body`), the allocation size (the constant r3 of the allocator whose result a ctor's caller hands it), subobjects (a vptr a ctor stores at this+off), the globals holding an instance. CLASS = `0xVTABLE` / `vt:ADDR` (a subobject's vtable names the outer class); ROOT = `g:ADDR` (the object the word points to) / `sub_X` / `ctor:sub_X`. `q class CLASS [--json]` = the vtable's slots (cut by `vtcut.py`) with db names, ctors / dtors, parents (a ctor calling another class's vptr-storing function on this+0 first; a vtable whose every slot is ours at the same index = a base by prefix; a named parent's shared-slot count; other partial shares only counted), subobjects, the classes calling our ctor on their own this, the globals. One engine: qslice's per-function facts and canonical roots (`Slicer.canon`, `norm_vt`, `globals_of`, `candidates`); a callee with > 64 direct callers (a library routine) is not read inside. Warm ~1-10 s (the SmartMenu class reads 155 functions + 71 callees), cold up to ~100 s while qslice's cache fills. (`qstruct.py`, selftest `qstruct_selftest.py`: a planted class with a ctor, a base, a subobject at +32, a record array with an unrolled writer, a context-indexed word, twins; the splat pool, SmartMenu scroll, placement table re-finds) |
| `ostate` | S111 OSTATE: `q ostate [--fn NAME] [--field F] [--json] [--facts] [--ref REF]`: per function of OURS, the render state it sets, in `q gstate --facts`' fact form (`side ours`, `fn` = the enclosing name as `q assume` gives it, `field` = gstate's setter key: `alpha_test`, `alpha_func`, `alpha_ref`, `z_enable`, `z_write`, `z_func`, `blend_enable`, `cull`, `colour_write_mask`, `stencil_enable`, ...; `--field` also takes rstdiff's `atest` / `zfunc` / ...). Sources: every `rdr::DrawState X{};` block of `qrstdiff.COVER_DIRS` read by the one C++ evaluator (moved here from `qrstdiff.py`, which imports it), once per pass class when it reads `pass_state::Of(pcls)`; the draw's alpha test from its `Dynamic(...)` (the material flag `b.alpha_test`, a caller-stated `InstancedDraw::alpha_test`, a rule of ours, or `system[9]` = 7 = off) with AlphaDynamic's func / ref per class; the `pass_state.h` kTable rows (keyed `pass_state::kTable[NAME]`, with their `addr` cell and game functions; z func through `ForwardZ`); the `InstancedDraw` field stores of each caller (`alpha_test`, `cull_none`, `no_blend`) with their if / for conditions. Values are in OUR units (VkCompareOp, forward z, alpha ref 0..255). `--ref` reads the tree at a git ref (`git show` / `git grep`, never a checkout). S111 OSTATEWIDE (`qostate_wide.py`, selftest `qostate_wide_selftest.py`): also every copy block `rdr::DrawState X = SRC;` (each field inherits SRC's facts, kind `copy`: a DrawState above it in the function, a struct member's writers, or the game's word when the member is decoded from the device words, `DrawFixed::state` by `rdr_reader::DecodeFixed`; else a rule naming SRC; its own stores override), the stores on a DrawState by reference / pointer (`st.f =`, `s->f =`, `out.state.f =`, one unit per function and base, types from the declarations and struct definitions), and the bind records a block's alpha test reads (`b.alpha_test`, b a `Bound`: `BindMaterial` / `BindRule`'s `b->alpha_test` stores, kind `bind`, linked as the alpha fact's `bind`); a same-name store on another type, a decoder's own stores and a store after `State(X)` are set aside with the reason (`counts['aside']`, printed by `q pairs --coverage`). ~3 s. (`qostate.py`, selftest `qostate_selftest.py`; `docs\research\s111\WHOLEDIFF_PLAN.md` A.3) |
| `codediff` | S111 CODEDIFF: `q codediff [--fn NAME\|--game sub_X] [--field F] [--flag DIFF ...] [--ref REF] [--json] [--no-rank]`: per pair (our function or DrawState block, the game function it mirrors: `q pairs` precise rows, a kTable row's `addr` cell, `qrstdiff.KINDS`), the game's `q gstate --facts` against our `q ostate` facts, field by field, units converted in one named table each (alpha func codes by `enums.json` names, z func codes by `qrstdiff.ZFUNC_CODE` then `pass_state.h` `ForwardZ`, `VK_CULL_ON`, blend codes by `qrstdiff.BLEND_CODES`). Verdicts DIFF, GAME-ONLY, UNSETTLED, OURS-ONLY (low), DEPARTED, OK (moot included), ranked DIFF first, scene before 2D, then our call sites (`q ours callers`). Pairs are compared arm by arm: a fact tagged with its own rstdiff kind stands against its kinds' functions only, and a block with a fact under several kinds (`FrontEnd::Record`) takes a citation only for the facts it sits on. A departure is declared at the site: `// DEPARTS: <class> <GAME_BUG_FIXES\|DECISIONS\|360_HABITS>:<row ref> [@sub_X]; <reason>` (the row must exist; `@sub_X` scopes it to that pair; a `mod` only on a fact behind the mod's switch). `--ref` reads our code and its pairs at a git ref (a `git archive` of src + db in the temp folder, once per commit, ~20 s). check.py row `codediff`. ~5 s plus the call-site count. (`qcodediff.py`, selftest `qcodediff_selftest.py`; `docs\research\s111\WHOLEDIFF_PLAN.md` A.4). S111 LEVELSDIFF: `--field levels [--facts]` is the texture level extractor (`qlevels.py`, selftest `qlevels_selftest.py`; WHOLEDIFF_PLAN A.5.1): per list the game appends, the level count in force (the count word's sets / restores / per-name lookups in block order, inherited one caller up) against every `TexSet` call of src and `game_levels.h` Relevel's upload rule; paired by citation or a shared key >= 100; `--ref` ~6 s. Not in the check.py row yet. |
| `asm FUNC --at OFF` | S111 QGAPS: a window inside one function by hex byte offset from its start: `--at B90 [--ctx N]`, `FUNC+0xB90`, `FUNC +B90..+BA0`. |
| `fieldsdiff` | S111 FIELDSDIFF: `q fieldsdiff [--fn NAME\|--game sub_X] [--flag GAME-ONLY ...] [--ref REF] [--min-overlap N] [--json\|--facts]` (also `q codediff --field reads`): per pair (`q pairs` precise rows whose game side is a function, plus the named records of `tools\q\fieldsdiff_records.json`), the record fields the GAME function reads (`q decomp`'s field reads, plus the copy-through reads of a constant-size memcpy into a stack local, signed under `extsh` / `extsb`, each with its use: gate / arg / copy / read) against the constant-offset reads of OURS (reader calls `Rd16(b, 2)` / `Rd32(base, m + kOff)` / `Be32(p + 8)`, cast loads, byte subscripts; constexpr names resolved). Bases bind by a record row, the global word K (one common address), or the game base with the most same-width offsets (>= 2, no tie: `overlap N` [L]). Verdicts GAME-ONLY READ (ranked by use, gate / arg first), WIDTH, OURS-ONLY READ, OK. Not in check.py: a GAME-ONLY READ is a lead. ~65 s whole tree. (`qfieldsdiff.py`, selftest `qfieldsdiff_selftest.py`; `docs\research\s111\WHOLEDIFF_PLAN.md` A.5 row 5) |
| `args F` | S111 QARGS: `q args F [--reg rN] [--value V] [--in CALLER] [--all] [--json]`: one line per call site (call and tail call) of F, only the argument registers F itself reads (the `in_rN` its decomp mentions; r6 set by a caller but never read by F is dropped). Each value is folded to a constant, an image address (with its string / function / db name), `in_rN` (the caller's own, passed through) or the expression. `--reg rN` = a histogram over all sites (`r5: 7 x90, 1 x40, computed x20`), `--value V` = the sites passing V (a number is compared as a number), `--in CALLER` = one caller. Values come from qdecomp's interpreter (`regs` of each call event, `vsites` for indirect calls in the decomp summary). First call after a qdecomp change rebuilds every caller's decomp (~45 s for 300 sites). Limit: a register F only forwards untouched to a deeper call is not seen as read. (`qargs.py`, selftest `qargs_selftest.py`) |
| `vcall OFF --vt 0xVT` | S111 QVCALLVT: `--class` for a RAW vtable address (a menu page / frame class has no RTTI). The class = the vtable's family (`q struct`'s class target + the classes built on it, `q class`'s children, depth 3); a site is kept when its receiver is the family: `vptr` (stored in the same function), `slice` (qslice.canon of r3, `rotlwi rN,rM,0` copies looked through), `slot` (`this` of a family slot function), `field` (r3 loaded from a field whose writers on the receiver's class store the family), `field [L]` (only a store of that displacement anywhere does). Sites traced to another class and sites the slicer cannot type are counted, not listed. Cold ~1-4 min (the family's constructors and every site's function are read once; raise `WWE_Q_TIMEOUT`), then cached. (`qvcallvt.py`, hook in `qx7.vcall_main`, selftest `qvcallvt_selftest.py`) |
| `range LO HI [--loads OFF] [--stores OFF] [--calls sub_X] [--exclude LO-HI]` | S111 QVCALLVT: the functions whose start lies in [LO, HI), optionally only those with a load / store of displacement OFF (any base register, any width) or a direct call (`bl` / tail `b`) of X; all given filters must hold. Reads each listing with `qdecomp.read_listing` (no dataflow): ~11,000 functions in ~30 s. Indexed forms have no displacement and never match. (`qrange.py`, verb file `verbs\range.toml`, selftest `qvcallvt_selftest.py`) |
| `vcall OFF --reg rN [--value V]` | S111 QARGS: the same fold for an indirect slot: the value of rN at every listed `bctrl` site, histogram, `--value` lists the sites. |
| `asm FUNC [FROM TO\|--at ..] --writes\|--reads rN\|sp+D` | S111 QARGS: every instruction that writes / reads the register or stores to / loads the stack slot. Writes: destinations (mr / li / lis / addi / loads / ALU), the BASE of an update form (lwzu / stwu), a `bl` or `bctrl` clobbers r0 and r3-r12 (a `__savegpr` / `__restgpr` helper only r0, r11, r12), a first `<entry>` row = the value at function start (an argument, or the caller's callee-saved value with the prologue instruction that saves it). Stack: base r1 counts (a store covers its width, offsets from the sp after the prologue's `stwu`), a base that is a frame copy (`mr rX,r1` / `addi rX,r1,N`, rX written only there) counts, any other base (r31 as an ordinary pointer) does not; `--reads sp+D` also lists `addi rX,r1,D` (address taken). (`qregs.py`) |
| `callers X --in FUNC` | S111 QGAPS: only X's call sites inside one function. `q field OFF --reads\|--writes\|--stores --in FUNC` is `--func FUNC` (`--in` used to abbreviate `--int`). |
| `seam --cites FILE[:LINE]` | S111 QGAPS: which `db\seams` ours rows (path, the line the anchor holds now; a row stores no line) and which seam / `db\claims\*.json` rows cite the file or `file:line` / `file:a-b` in their text (`qseam_cites.py`). |
| `show FILE:N-M --ref REF` | S111 QGAPS: the lines as they were at a git ref (a file deleted since still reads at the older ref). |
| `ours text W1 W2 ...` | S111 QGAPS: several words = ANY of them (`--and` = all on one line); a quoted argument is one phrase; `-E` / `--re` = PCRE regexes; `--in FILE\|FOLDER` that exists is searched whatever its type (db\seams, root `*.md`, one note); S111 QTEXT: `--in` may repeat (every one must exist; searched together, the trim line's `all:` call keeps them), words are literal text with any characters (`system[9]`, `st{};`; `--lit` says so and refuses `-E`), `-E` takes alternations with spaces and braces (`"Dynamic\(\|DrawState \w+\{\}"`); `ours body NAME --in FILE` picks the definition in that file. Selftests `qgaps_selftest.py`, `qourstext_selftest.py`. |
| `decomp` | S103 QD: `py tools\q\q.py decomp FUNC [--fields] [--callers N] [--json] [--no-cache]`: a compact, mechanical view of ONE function (loads / stores as `[base+off]` with provenance folded, calls with db names, branches as if / do-while where the shape is plain, the return; a fields / calls summary on top; the listing line of each block). Cached under `out\harness\index\decomp\` keyed by the generated source's sha1. `--callers N` = one line per caller with its call-site arguments. `q fn` shows its one-line summary. Usage, notation, limits and the proof (`tools\q\decomp_selftest.py`): `docs\agents\q_decomp.md`. S111 FACTCARD (`tools\q\qfactcard.py`): the settled claims print INSIDE this output (a header of <= 8 lines, and a `; [C] NOTE xN: ...` comment before the block holding a cited offset); the same card prints in `asm --at`, `callers` (one fact per caller) and `field OFF` (the claim citing each access site). `--facts none|all` (default: the capped card); `[L]` rows only under `leads:`; DISPUTED / STALE are flagged; other tools plug in with `qfactcard.register(kind, fn)` (docstring of qfactcard.py). Proof: `tools\q\qfactcard_selftest.py`. |

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
- `q ours census FIELD [--in PATH]`: the format literal holding FIELD -> the argument it prints for it (printf spec N = argument N, `%%` and `*` handled, adjacent / split literals and PRIu64 joined; `" field " + std::to_string(x)` / `<< x` concatenation) -> the counter (member / global / array element; a local is expanded through its assignments, depth 3) -> every write (`++ -- += = .store .fetch_* .exchange`, file:line, enclosing function) + the declaration. An array counter whose subscript names a member (`pic_why[q.why]`) also lists who sets that member inside functions naming its type. A counter found only by a name near the literal is marked `BY NAME (unconfirmed)`; a FIELD that is no literal but a counter name lists its writers. Text level (this checkout, always current, no index).
- Does NOT: a format held in a variable, a literal inside a `#define` (reported, not expanded), writes through a pointer handed to another function, memset / std::fill, struct-vs-name sharing (all writers by NAME are listed; `--in PATH` narrows).
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
- `q ours text WORDS...`: every line holding ANY of the words (S111 QGAPS; `--and` = all of them on one line; a quoted argument is one phrase), case-insensitive; an address word matches every spelling; C++ / py / ps1 / toml hits are marked comment / string / code (a line-local heuristic, not a lexer). Uses `git grep` (0.5 s; tracked + untracked, never ignored files, spirv / spv skipped); Python only applies the exact word rule and the mark.
- `q ours def|refs NAME` with no C++ symbol falls back to shader text (src, tools, assets: .glsl .frag .vert .hlsl .fx.json): declarations first, then uses.
- Selftest: `py -I tools\q\qx6_selftest.py` (a fixture with planted misses, then the real-tree proofs when generated\ and the index exist).

## QX4: q vcall, q nodes, q ours refs ADDRESS (S103; tools/q/qx7.py, selftest tools/q/qx7_selftest.py)

- `q vcall SLOT_OFFSET [--class VT|NAME] [--all] [--json]`: every virtual call site in the generated listing with that vtable slot byte offset (0x34 = slot 13, 0x4C = 19, 0x80 = 32): `lwz rX,OFF(rY)` + `mtctr rX` + `bctrl` (`bctr` tail too). Row = site, enclosing function, call / tail, the receiver (`this` = the function's own r3, `field+N of ...`, `global 0xADDR`, `result of sub_X()`, `arg rN`) and the class where known: a vtable constant stored at +0 of the object in the same function, or `this` of a function that is a slot of an RTTI vtable. Default = vtable calls (`lwz rY,0(rZ)`); `--all` adds callback-field calls. `--class` lists only sites with that known class and COUNTS the unresolved ones (never guessed). A site whose r3 at the call is not the object the vtable was read from is flagged `[FLAG: r3 ...]` (about 3 %).
  Tracking: a forward flow over the function's own branches (paths met at labels; a def survives a join only when every path agrees, else it is a per-register unknown; no fall-through after b / blr / bctr; a backward branch feeds the next pass, up to 5 passes). A call kills the volatile registers, except a register save / restore helper (`__savegprlr_N`, 236 of them in the index), which kills only r0 / r11 / r12. `this_recv` sites: 7,039 of 30,645 vtable calls.
- `q nodes [CATEGORY] [--slot N]`: the draw-node registry, the 229 `bl 0x82C30A10` (node, category, on) sites: function, category (`li r4,N`, else the unresolved source), the node's vtable (the constant stored at +0 / +off of the node in the SAME function), class, and the image word at vtable + 4 * N (default slot 1 = the draw method; `?` when it is not a function).
- Both read `out\harness\index\vcall.s<VERSION>.db` (S105 D: the version is in the file name, older files are left alone; WWE_QVCALL_DB=FILE = another file; a scan of the 345 generated .cpp, <= 4 workers at below-normal priority, RAM-capped, 15-25 s; rebuilt when the codegen fingerprint or the table version changes; temp file named with the pid, the replace retries). `--db FILE` reads / builds another file. Image override for tests: env `WWE_Q_IMAGE`.
- `q ours refs|def 0x836F38D0` (a literal address where a symbol goes): the named constants whose WHOLE initialiser is the address: `constexpr / const / static` (one of several declarators, an array element, a `(uint32_t)` cast, `u` suffix, any hex case), an `enum` enumerator, a `#define NAME value`; comments and strings are blanked first; NOT `Foo(addr)`, `addr + 4`, `base + addr`. Then the refs of each name (`q ours lit ADDR` stays the every-literal answer).
- Timed (this busy PC): see `py -I tools\q\qx7_time.py` (uses temp databases; never deletes the real ones). Selftest: `py -I tools\q\qx7_selftest.py` (fixture with planted misses and one case per review finding, then the real-tree proofs).

## q asset: the index of the game's FILES (S103 lane QAS; tools/q/qasset.py, selftest tools/q/qasset_selftest.py)

`py tools\q\q.py asset find WORDS | pac PATH [PACH [ENTRY]] | tex NAME [--like] | mat NAME [PAC] [--pac P] | params EFFECT [--has N] [--lacks N] | vcolor PAC [ENTRY] [--mesh N] | id evtobj|bgetc|weapon N | slot ARENA [N] | info | update [-j N] [--full] [FRAG]`

An index of every pac under game_data (extracted + dlc), read-only, with our own readers (tools/wm30/pac.py unpack / BPE / ZLIB / PACH walk; the JBOY layout of tools/arena/arena_files.py; filecensus_flags for the arena slot text). Built from the PLAYER'S files into `out\harness\index\asset.s6.db` (schema 6 since S111 ASSETCOV, 5 = S111 MODINDEX; the main checkout's when run from a worktree; **the schema is in the file name**: a q of another schema uses another file and never opens, wipes or rebuilds this one, and the header names older index files beside it: the tidy holds `out\harness\index`, so an older file is deleted by hand once no older worktree's q reads it): names, ids, counts and alpha statistics only, never a byte of game data, never committed.
- Levels: pac -> PACH (index, virtual path from the EPAC header's own directory, e.g. `/STG/CAGE`, `/SOBJ/2038`) -> entry (key path `1/0` for nested PACHs; kind model / texdir / pach / other / raw) -> model (nodes, meshes, vertices) / material per mesh (effect, parameters with values, sampler -> texture) / texture (format, size, mips, alpha max, share 0, share >= 224, mean of the top mip; DXT1 (1-bit: c0 <= c1 blocks, index 3 transparent) / DXT3 / DXT5 / BGRA8 / BGRA4). A directory row that is not a PACH (EVP / EVZ, CASD, bink) is its own `blob` entry with its tag and size. A model with a mesh record that fits neither layout is SUSPECT (vertex count unknown, entry note, counted in `info`); the shifted layout of bgEtc /STG3/WPO2 (effect +0x78) is decoded.
- `find` searches every name level (all words must match one name). `pac` browses. `tex` = format + alpha stats + where. `mat NAME` = a material class (effect, e.g. yCh_Reflect) or the material using a texture name; a pac stem (`mat yCh_Reflect ch139`) narrows it. `id evtobj N` = the SOBJ / BOBJ / SPMT row N of evtobj*.pac -> PACH and its entries; `id bgetc N` = bgEtc PACH N; `id weapon N` = m2.pac PACH 0 (/MOT/WEAP) key 1000/N (the path our loader LoadM2Weapon reads, labelled so) AND the bgEtc WPON / WPBS / WPO2 key N (the game's stage-weapon sets, labelled not read by LoadM2Weapon).
- **A builder testing `q asset` in a worktree sets `WWE_QASSET_DB=<a file under its own out\>` (e.g. `out\qac\asset.s6.db`) on EVERY call, build and selftest** (never the shared index; a call without it reads / builds the main checkout's file; the selftests' fixture calls drop the caller's `WWE_QASSET_*`, so a set `WWE_QASSET_DB` never reaches a fixture call); a rebuild prints `q asset: writing <file>` first.
- `cull [PATH [PACH [ENTRY]]] [--list] [--set|--clear]` (S103 QAC): the mesh record's cull word (+0x8C; bit 0x2 set = the game draws the mesh one-sided, clear = two-sided) and technique word (+0x90), stored per mesh row (schema 4). Shifted layout (bgEtc /STG3/WPO2 + two p62 models, record 0xB4): the reader finds the effect name at +0x78, and the game's record is name, cull word at name + 16, technique word at name + 20, nothing between, so the words are at +0x88 / +0x8C [C by position]; src/assets/jboy.h:446 / 695 / 696 reads only the standard layout (name +0x7C, words +0x8C / +0x90), so it cannot confirm it; the data agrees (every shifted mesh: bit 0x4 set in the +0x88 word, values 4 / 6 only, like the standard cull word). No PATH = one row per pac family with the word values; PATH = per PACH, PACH = per entry, ENTRY = per mesh (`--list` = one line per mesh with the palette node (the first bone-palette node: unconfirmed as the mesh's node), effect, word). Arena bg pacs also split stage slots (keys 0..399) from other keys. `asset pac PATH PACH ENTRY` mesh lines show the words too. A db of an older schema is rebuilt by the first call (header says REBUILDING while partial; `q asset update` finishes it).
- Automatic: every call stats the pacs and re-reads only new / changed ones (the cold build is one `q asset update`, about 6 minutes; a query that finds the index missing builds for at most min(90 s, half of q's own timeout) (WWE_QASSET_BUDGET overrides), checked against the wall clock while waiting for a pac, and says PARTIAL in the header; a query waits at most 10 s for another writer's lock (owner-pid checked: a dead writer's lock is taken over at once) and then answers STALE). The header line of every answer says the index age. At most 4 workers, fewer when free RAM is low, one pac per worker.
- Not decoded on purpose (entries listed with size only): motion / sound / movie pacs and parameter tables (mot*, dlc_mot*, sound, evtmov*, m3 = parameter tables, mvMot, string, wuniData). m2.pac IS decoded (weapon models).
- **A cold build fills `out\wm30\cache` (~3.4 GB)**: every XCTD-packed pac is unpacked there once (tools/wm30/pac.py; the cache file name carries the source's size + mtime and is written temp-then-rename, so a replaced pac never reads a stale unpack). out is scratch: the end-of-session tidy clears that folder (and the index), so the next cold build unpacks again; the index itself is ~60 MB.
- Selftest: `py -I tools\q\qasset_selftest.py` (a fixture pac with planted misses; the real-tree proofs run when the index exists).
- **Mods (S111 MODINDEX, schema 5 = `asset.s5.db`; tools/q/qasset_mods.py, selftest tools/q/qasset_mods_selftest.py):** the player's mods folder (the launcher's `mods_dir`, else `game_data\mods`) is indexed too, read-only, every .pac under the path `mods/<path in the folder>`. Each mod row is tagged `mod:<name>` (`superstars/frame`, `superstars/library/<KEY>` with the card's character name, `superstars/pack6` = the old pack (never mounted), `superstars/sources/<stem>` = an importer input, `characters/<stem>` = viewer only) with whether the game reads it NOW (USE MY SUPERSTARS from the launcher's settings, worked out per query: a toggle flip re-reads nothing). `mat` / `tex` take `--source all|game|mods|mod:NAME` (default all; a mod material is its own group beside the stock one); `q constdiff` / `q texdiff` read `--source game` by default; `info` lists each mod source with its counts. A missing `asset.s5.db` is seeded from the `asset.s4.db` beside it (the same tables), so the first call reads only the mods. Tests: `WWE_QASSET_MODS=<folder>|none`, `WWE_QASSET_SETTINGS=<file>`.
- **Parameter counts, vertex colour, replaced character pacs (S111 ASSETCOV, schema 6 = `asset.s6.db`; tools/q/qasset_cov.py, selftest `py -I tools\q\qasset_cov_selftest.py`):**
  - `params EFFECT [--has NAME] [--lacks NAME] [--values NAME] [--like] [--source S]`: the materials (one row per mesh) of one effect (exact, case-insensitive; `--like` = substring; `*` = every effect), counted by the parameter / sampler names they carry: how many carry each name, and the value histogram of every `--has` / `--values` name. BODYSPEC's questions are one line each: `params yCh_skin --has g_fSpecLev --lacks g_fSpecPow` (the strength without the power) and `params "*" --has g_fSpecularLev` (any material naming it). Names match case-insensitively; `yCh_skin` does not take `yCh_skin_flex` unless `--like`.
- `params` as a census (S111 QCENSUS; tools/q/qasset_census.py, selftest `py -I tools/q/qasset_census_selftest.py`; `--by` / `--sets` / `--pac` / `@pseudo-names` / `--all`): `--by effect|family|pac|source|technique` prints every count and histogram once per group (family = `qasset.family()`, source = `game` / `mod:NAME`, technique = the technique word as 0x hex; a group with no passing mesh is only counted); `--sets` = a histogram of each mesh's FULL set of names (one signature per effect); `--pac P` (repeatable; stem / path tail / substring, as `mat --pac`) narrows any query; the pseudo-names `@technique` `@cull` (the words, 0x hex), `@verts`, `@pal0` (first bone-palette node index) work in `--has` / `--lacks` / `--values`. Every list prints its total; a cut list ends with `showing N of M ...; all: <the exact call>` (`--all` = no cut, `--json` is never cut). E.g. `params yCh_trans --by family --values @technique` (technique words of one effect per family), `params yCh_skin --sets`, `params "*" --has g_mCubeLPrm --by family --values g_mCubeLPrm`. Not in the index (needs a schema bump): every bone-palette entry beyond pal0.
  - `vcolor PAC [ENTRY] [--mesh N]`: per mesh, per colour channel, min / max / the number of distinct values and the top values. The colour word is S0 file bytes 24..27 of each 28-byte vertex; for a skin mesh the shader reads it as (byte 25, 26, 27, 24) = x y z w: byte 25 = the first lobe's gain, byte 24 = the packed ID-map (low 3 bits) / wrinkle (bits 3+) slot (docs\research\s111\VERTEXSEAM.md). Stored at build time (table `vcolor`) for every mesh whose streams tile (S1 = S0 + 4 + 28 n); a mesh that does not tile has no rows and says so (never an invented histogram).
  - `REPLACED?`: `pac` and `info` mark a game-folder `pac\ch\*.pac` whose first four bytes (stored per pac, `pacs.magic`) are not XCTD (`0F F5 12 ED`): every stock character pac is XCTD, a community pac is EPAC / EPK8. A mods-folder pac and a dlc pac are never marked (they are not the game's own character files); `info` prints the count and the first names.
  - Schema 6: a missing `asset.s6.db` is seeded from the `asset.s5.db` (else s4) beside it, so the old rows answer at once, but every decoded pac is marked for a re-read (the colour and the magic exist only in the files): the first query reads for its budget and says PARTIAL, `q asset update` finishes (about 6 minutes, like a cold build). `vcolor` says so for a pac not yet re-read.

## S105 QG: five game-side miss classes (tools\q\qgx.py, qstores.py; `tools\q\qgx_selftest.py`)
- `q switch FUNC` (= `q decomp FUNC --cases`): every jump-table switch of FUNC as one row per target: `values [idx a..b] -> @target  li rN=K | st stw K->off(rB) | calls sub_X(rN=K)  [how the body ends, k cond branches]`, plus the bound and default. A biased switch (`addi rX,rY,-N` before the bound) prints `switch on rY - N` and the case VALUES (index + N). Constants set before the switch (function-wide single `li`) are seeded and named; `bctrl` is `calls (indirect)`; the body scan is linear (guarded / merged paths are leads). Example `q switch sub_8264FAB0` (case 501, not 0).
- `q field OFF --stores [--global G | --root-name R] [--var-span N] [--all-copies]`: `field OFF --writes` plus the writers q field never modelled: indexed stores (scalar, byte-reversed, stvx / stvx128 / stvewx / stvlx / stvrx, stfiwx, stwcx. / stdcx., dcbz: covered by width), block writers (memcpy / memmove / memset / zero-fill, calls, tail calls and the 1-instruction `b` thunks to them, e.g. the memcpy thunk sub_82C2F7B8) and `add`-formed bases (linear shadow, marked). The output ends with counts of what its filters hid and what is not modelled. Own db `out\harness\index\stores.s2.db` (~12 s build under the rebuild lock). `q field 15229 --stores` re-finds the apply writer 0x822CA0E4; `q field 525481 --stores` the three `stbx` writers behind `q imm 0x804A9`.
- `q text ID [--lang N] [--all]`: text id -> string from `pac\string.pac` (+ the newest DLC string pack with --all): `q text 282` = `SELECT ARENA`. Format: docs\formats\string_pac.md (entry name `GGLL`, LL = language).
- `q csdt [ERA [KEY]] [--lang L] [--find KEY]`: the evtdata.pac /CSDT caption schedule: `q csdt 0 10` = key 10, frames 256-804 (the caption text is the player's own game data: never quoted in the repo). Format: docs\formats\evtdata_csdt.md.
- `q save [titles|options|dlc|caw|threads|all] [--data NAME|PATH] [--json] [--names]`, `q save check EXPR...` (S106 C1): the user-data mirror's SaveData.Dat decoded (title holders / belts / plates, caption + blood options, saved DLC mask, CAWs, Threads sets); `check` exits 1 when a precondition is false (`holder:title=1:char=322`, `plate:title=1`, `option:caption=1`, `dlc:mask&0x40`). Grammar: tools\q\qsave.py header; layout docs/formats/save_data.md.
- `q ours includes HEADER --included-by | --transitive | --closure`: the include graph of our src / tools / probes (direct includers; every file that reaches HEADER; every file HEADER pulls in), with the translation-unit count.

## S110 IDXFACTS: per-function structural facts of the listing (tools\q\qfnfacts.py; `tools\q\qfnfacts_selftest.py`; docs\research\s110\IDXFACTS.md)
- `q fnfacts FUNC | --stats | --build [--out FILE] [--gen DIR]`: own db `out\harness\index\fnfacts.s1.db` (WWE_QFNFACTS_DB = another file), one pass over generated\default (~25 s, 4 workers below normal priority), published with qpublish; a lookup on a changed listing says STALE and never rebuilds itself. One must-dataflow per function (entry args, `mr` copies, li / lis / ori constants, calls kill r0 r3-r12) feeds the facts; `--stats` checks the parse against index.db (instruction count per body). Every row is a SHAPE, never a meaning.
- loops: `loop(func, head, latch, n_body, n_calls, n_icalls)` = a back edge of a depth-first walk (head = the loop's entry, so a loop entered at its bottom test has head > latch); the body is the NATURAL loop (an inner loop never takes in its outer loop's path); `loop_ind(func, head, reg, kind, step, init, bound)` = one `addi rX,rX,K` / `subi` / update form is the register's only write in the body: ptr (an address operand there: a K(rX) base or either register of an indexed load / store, `lvx128` too, so a byte offset as well) / arg (not an address operand, copied into r3..r6) / idx (neither); link (`lwz rX,K(rX)`), ctr (bdnz, init = the CTR constant); bound = the constant of a compare whose branch can LEAVE the loop.
- switches: `switch(func, site, reg, n_cases, n_targets, lo, hi, bias, deflt)` + `switch_case(func, site, idx, value, target)` for every function (the case table from qgx.parse_switches; the bias = the add chain since the index register's last other write, the same on every path into the bctr that does not come around a loop, NULL when paths differ; the default from the bound compare the register still holds. `q switch FUNC` still uses qgx's own line windows for bias / default: S110 review, 116 of 1,159 biases and 67 defaults differ from it).
- compare chains: `chain(func, val, reg, first, last, n, n_eq)` + `chain_val(func, first, site, value, cond)` = >= 3 compares of ONE value (arg:rN / ret:SITE / def:SITE / bits:...) against distinct constants (immediates or lis / ori constants: FourCC, HRESULT); cond eq / rng / none.
- entry-arg bit tests: `argbit(func, arg, mask, site, how)` = bits of the ENTRY value of rN tested (rlwinm-family / andi. compared with 0, or record form): `arg=4 AND mask=1` is the deleting-dtor flag test.
- clones: `fn.h_exact` (text, in-function targets relative, a bctr's case -> target map included), `h_noimm` (immediates -> #, rlwinm shift / mask fields too), `h_nocall` (call / tail targets -> F), `h_shape` (both); equal hash = a group (GROUP BY; filter small bodies by `n_insns`).
- not here, held already: the vtable slot of an indirect call = `vcall.s4.db icall.off` per site (tools\q\qx7.py; `q vcall`).

## S111 PAGEREAD: `q read system NAME` reads a stored page back (tools\q\qreadsys.py `stored_page`; test in qread_selftest.py)
With no --from, any stored name (create_story, ...) prints its newest stored page (explain, open lines, writer facts). A fixed name prints its page
first (if one is stored), then the old bundle unchanged. A name neither stored nor fixed exits 2 and lists the stored names (`q read systems`).

## S111 PICKPATH: the reach set is the selection (tools\q\qreadfrom.py, qreadsys.py; `tools\q\qreadfrom_selftest.py`)
- `q read pick --from ... --out DIR`: every sheet's second line is `reached: depth D from sub_SEED via sub_PARENT (unit U)` (the BFS parent that first added the unit, and the entry its path started at; the first parent in walk order, not the lowest address); an entry unit says `reached: depth 0 (entry)`.- `q read pick --callers-of GATE[,GATE..] [--depth D] [--out DIR] [--count]` (also `q read system NAME --callers-of ...`; composes with `--from` as a union; S111 GATEPICK): a match type lives in the CALLERS of its rule gate, but `--from` walks callees only. The seeds are the gate itself plus every function that CALLS it (index calls / tail calls, observed and static indirect callers), all at depth 0; from them the ordinary callee walk runs at `--depth` (default 1 here: the callers' own bodies and their direct callees; with `--from` the default stays 2), with the same library and middleware cuts. A seed unit's sheet says `reached: caller of sub_GATE` (the gate: `reached: the gate sub_GATE`). The report adds a `callers of ...: N seed functions in M units` line. A gate's callee that no caller of the gate uses is reached only at depth >= 1 from the gate itself (so not at depth 0). A unit first reached through a class slot ends `(unit U; class slot: sub_PARENT builds the table that holds it)` (the parent does not call it and is at the same depth; a bundle heading says `(class slot)`). Line 1 and the rest are unchanged. batch.txt rows carry the depth as before.
- `q read system NAME --from ENTRY[,ENTRY..] [--depth D] [--k K] [--classes on|off] [--middleware on|off] [--max N] [--skip N]`: the bundle holds every unit of that reach set that has a stored reading, whatever its `system` field says, in reach order; each `## unit` heading ends `depth D via sub_PARENT` (or `depth 0 (entry)`). Units past `--skip` + `--max` (default 250) are listed by id with their count and the exact call for the rest. NAME may be any `[a-z0-9_]+` with `--from`; without `--from` the old behaviour (NAME one of the fixed systems, selection by the `system` field) is unchanged. `q read store-system` accepts any `[a-z0-9_]+` name.
- `--middleware on|off` (default on): a unit `q lib` labels Havok (how = strings; never the `range` guess) is cut like a library unit (not listed, not walked through; an entry unit is never cut) when its OWN functions carry the evidence: at least half of them reference a Havok string, carry a Havok game name or sit in a Havok RTTI class's vtable, and none has a db name that is not Havok's (`qreadfrom.middleware_units`; the qlib label alone rests on one hit anywhere in the unit: the CAE screen's unit 100 has one hkMultipleVertexBuffer slot among 150 functions). The report prints `cut as middleware: N units` with names; `--recall` names the middleware cut as the reason of a miss.

## S111 NOTESEAM: the lane notes and the seam rows in the fact card (tools\q\qnoteseam.py; `py -I tools\q\qnoteseam_selftest.py`)
- Two qfactcard providers (header lines of `q decomp` / `q asm`, below the claim headers: prio 35 / 36, under the card's `facts` line, so the byte budget drops them first; no new verb; `q brief` stays unmerged).
- `; notes: NAME (answer: ...; settled N) | NAME | NAME | +K more (q ours text sub_X --docs)`: the research notes (docs\research\s1*\*.md) that name the function (`sub_X` / `0xX`); a note with a claim AT the function first, then the most mentions. The first carries its `## Answer` / `## Verdict` text and the count of OTHER functions its lodged claims settled, the rest are names; every note is either named or counted in `+K more` with the exact call. `--facts all` = one line per note with the full answer and the settled functions.
- `; seams: TOPIC own ours=PATH:LINE (open N) | TOPIC via NOTE ours=... | TOPIC named ...`: db\seams topics at the function. own = a claim row at it carries `seam`, or a game row of the seam lies inside it; via NOTE = a note that has a claim at the function and a claim naming a seam (the commonest 2 topics; this is how `q decomp sub_823C8548` reaches pose_timing: CUTSTUTTER); named = the function's own claim text names a COMPOUND topic (`pose timing`, `light sets`) as a whole word (a one-word topic such as `latch` is in every claim's prose, so it is never matched by words). `ours=` is the seam claim row's own line, else the seam file's first `ours` row. Cut: `+K more (q seam TOPIC)`.
- Reads docs\research and db\seams once per process (cold ~5 s on a cold disk cache, 0.2 s warm).
