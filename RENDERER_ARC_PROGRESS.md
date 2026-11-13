# The renderer arc: how it is going (a progress report for review)

Written 2026-11-11 20:27 by the main session (Opus 5.5) at Jarrod's request, for Fable 5.2 (the arc's author) to review: is the plan
still right, and is it being executed well? It reports against `RENDERER_ARC.md` (the plan; not re-planned here). Branch
`s101-renderer-arc`, 4h on the clock since the arc was written (16:2x -> 20:27), ~110 commits.

Marks as in the arc: **[C]** checked in the repo or by a passing offline check; **[L]** an agent's report, reviewed but not re-run by me;
**[U]** unconfirmed. "0 game runs" means no `wwe2k14.exe` launch; the viewer, `rplay`, `fview` and the tools are not game runs.

---

## 1. One paragraph

Phase A is done and checked in the game (Check A: 7 runs, over budget by 1). Since Check A, **0 game runs**: everything below was
built and proven offline. Phase B is nearly done (arena, wrestlers and entrances drawn by our renderer from the files; a census over
every arena and every wrestler x attire; the constant solver). Phase C has started a step ahead of the plan's order: the shared live
scene writer is merged, and the crowd, arena and wrestler classes are built as contributors (proven offline, not yet run live). Go
anywhere moved furthest: a first reading said the game has no callable scene starters; a second, deeper reading (three Opus readers)
found that **every scene in the game is a bank id in one id space, and the game has its own paths that play a scene by id** (menu,
mid-match, victory). The execution model the arc asked for (section 5A: lanes, contracts first, offline proof, the gate) works, with one
addition the arc did not have: **an Opus review of every hand-back**, which found real bugs in every lane reviewed so far.

---

## 2. Where each phase stands

### Phase A: foundations (done; Jarrod's play with the button pending)
| Piece | State |
|---|---|
| A-1 the three formats | done: `docs\renderer\CASE_ZIP.md`, `SCENE_RECORD.md`, `NAMES.md` [C] |
| A0 reading | done: 14 + 1 answers lodged |
| A1 names | live in the game: every draw of every Check A scene named (0 untagged), surface roles by creator, texture provenance [C, Check A]. Since then (offline): provenance accuracy (the review found the memcpy hook read its source after the copy, a bug older than the arc) and live names for every 2D draw incl. widget + screen and particles [C offline, compile-only in the game] |
| A2 frame viewer | done (the `s100cards` click test) [C] |
| A3 bug button + case zip + intake | done on PC; the zip now carries build stamp, device, settings, the way back, names, timing; intake flags "NOT OUR BUILD" on line 1; a truncated on-screen picture found in a real Check A zip and fixed [C offline] |
| A4 run contract | done [C] |
| **Check A** | **passed** (runs 7040-7046) |
| Jarrod's play with the button | not yet (PC ready; phone APK waits on the merges; Jarrod did not pick toggle vs always-on, so the default is "on in his test APKs") |

### Phase B: the scene renderer from files (0 game runs, as planned)
| Piece | State |
|---|---|
| B1 scene front-end, materials bound by name | done [C] |
| B2 viewer "rdr" backend | arena + wrestlers + attires done; **entrances** built (light rig, props, trons as live video surfaces): 17 of 18 comparison panels match the viewer's own picture, 1 listed open; review fixes in progress (a carried belt took the wrong light set, Session 98's "dull belt" class) [L] |
| Tool 5 file-fed census | built: **wrestlers 235 / 235 pass**; **arenas 24 pass / 14 tile fault / 19 "no effect yet"** (reflection cube maps; the tile faults are likely the viewer's camera-feed titantron surface rdr does not feed, per Jarrod) [L]; review fixes in progress (its "half arena" detector could only find its own plant) |
| B3 constant solver (tool 6) | merged: joins 10,658 captured draws to their file meshes over Check A's 6 zips; per effect FILE / SCENE (declared, untested) / RUNTIME; **the RUNTIME list is Phase C's hook list** (e.g. characters: bone palettes, fade, mip level, wrinkle / muscle levels, sphere occlusion, light limit, hair highlight colours) [C] |
| Picture vs the game (my sheet, Over the Limit) | rdr matches the viewer's own renderer nearly everywhere; **both are flatter and brighter than the game**: the game's live light set and its post chain (bloom, glare, DoF) are missing [C: sheet] |

### Phase C: the game as a source (started early, see section 4)
| Piece | State |
|---|---|
| The live scene writer (contract `src\renderer_scene\live_scene.h`) | merged: contributors per class, sinks (capture folder `scene\frame_N.rsc`, the case ring), off = ~7 ns a frame, gated behind `--wwe2k14_scene_records` [C offline] |
| C1 UI / menus / video | P1 (names live, compile-only) merged; the offline 2D pass covers A4R4G4B4 textures, GPU-picture sprites by role, movies, blend, wrap / clip [C offline] |
| C2 crowd, C3 arena, C4 wrestlers | each built as a contributor and proven offline (its record drawn by rdr against the viewer's picture, planted faults caught); C4 merged, C3 merging, C2 fixing (its Emit was O(n^2), LOD 1/2 members undrawable) [L] |
| C5 ring / props / particles / post | read only: post settings are already in the final pass's constants; ropes have a hook point; props have no per-frame list (take them from the draw) [L] |
| **Live** | **nothing of Phase C has run in the game yet.** Check B (4 runs) is the first time. |

### Phase N: go anywhere
| Piece | State |
|---|---|
| N1 `goto` + addresses + the 46-open test save | built (S101 earlier); the executor has never run [U] |
| N2 widget driver | waits for one live Create a Superstar trace (folded into Check B) |
| N3 builders | **the big finding.** All scenes are EV bank ids in one space (entrances < 1000, 30 Years 1010-2110, WSD 4000-4670, Universe 5000-5661, victories 6100-9500), resolved as `/EVP/<id>` through the pac index. The game's own paths: the cutscene startup sequence (from the main menu), the 30 Years Lua "Action" starter `sub_82738060` (mid-match), the victory chooser `sub_827C4430` (one forced value) [C listings, U live]. **Lane 32 is building `scene:ID`, `match:...@scene:ID`, `match:...@victory:ID` on these.** |
| N4 situation director | the lever is found (override the CPU's chosen action id, default-off hook); which ids mean pin / carry / finisher needs one live log [L] |
| N5 moments from recordings | merged: `moment:NAME@frame:F`, hand-over at any frame, a hash-stream track check that re-found Session 77's slip [C offline] |

### Tools (section 4)
Frame viewer, case zip, atlas, solver, run contract: merged. **The judge v1** (two arms of the same capture, differences named by
class): re-found Session 100's far crowd, but its cause rule picked effects by draw count and it replays every earlier frame per pixel
(257 s for a heavy frame): fixes in progress. **The replay farm:** re-found Session 100's double zero-fill slowdown on 1 of 8 captures
only (a CPU-bound PC hides GPU regressions; GPU timestamps being added, off by default). **Live shader reload:** ~3 s from edit to
picture; fixes in progress (it leaked programs per reload). New, not in the arc: **light match (lane 33)**, the game's own camera and
evaluated light sets from captures into the viewer (Jarrod: the viewer's entrance and match lighting do not match the game).

### Phase D: the gate
Not started (as planned).

---

## 3. How it is being executed (the arc's section 5A, measured)

- **Lanes, not a line:** this context (19:13-20:27) spawned ~50 agents: 19 builders, 11 readers (3 of them Opus for the scene machine),
  14 Opus reviews, 2 lodgers, plus each builder resumed once for its review fixes. Up to ~17 at once. Weekly usage **7%** for all of it.
  Earlier context (16:2x-18:0x): ~45 agents.
- **Contracts first:** formats before lanes (A-1); for Phase C I wrote `live_scene.h` before the four lanes that implement / use it, so
  writer and three classes were built in parallel and fit.
- **Every piece proves itself offline** in its own gate lane, with a planted fault and, where one exists, **a real past bug re-found**
  (far crowd, unannounced writes, case 7041, S97's half arena, S98's light direction, tester report #72, S77's slip, S100's zero-fill).
- **New: an Opus review of every hand-back, fixes back to the same builder (it keeps its context), merge in file order.** 14 reviews so
  far, **every one found real bugs** the builder's own tests passed: a widget name keyed by a stack-buffer pointer (every 2D draw got the
  first one's name), timing rows off by one frame, an O(n^2) emit, a multiply-order test that was circular, a scene record that could
  overwrite another, a track check that reads a half-written CSV. Merged code has been reviewed and fixed; nothing unreviewed is merged.
- **Briefs gained rules from what lanes hit** (`docs\agents\s101\common.md` "Shared machine"): never fixed script names in `%TEMP%` (one
  agent's script edited another lane's worktree), the GPU is shared (retry only the "frame ring" memory error), selftests must run the
  shipped code path (the widget bug passed a Python-written frame).
- **Game runs:** Check A 7 (budget ~6). Since then 0. Check B is planned at 4 + 2 per failure.

## 4. Where reality differed from the plan

1. **Go anywhere needed two readings.** N3-2 said "no callable starter"; S-1..S-3 found the bank-id space and the game's own starters.
   The arc's design (builders through the game's own functions) holds; the route is the scene manager, not per-scene functions.
2. **Phase C ran in parallel, not in order** (arc: C1 UI first, then crowd ...). With the writer contract fixed, C2-C4 could be built
   and proven offline at once. The ladder per class is unchanged: none is "promoted" until its live check.
3. **Readers' hook claims are leads, and some were wrong:** the wrestler skin matrices at the proposed hook are stale (the builder found
   the right derivation); 2 of 5 "sprite functions" in my own brief were not recorders; the scene ring's "advance" function only peeks.
   The arc's [L] discipline caught each one before it was built on.
4. **The frame boundary is `reader.h`, not `live.cpp`** (the writer lane found it; the plan and my brief said live.cpp).
5. **Light match was not in the arc;** it came from comparing the viewer with the game's own pictures.
6. **Phone / Surface: no work today** (the APK waits on the merges; no Surface run planned).

## 5. Risks I see (please weigh in)

1. **Check B is the first live run of a lot of code**: provenance fixes, UI live names, the complete zip, the scene writer, three
   class contributors, moments: all compile-checked only. Every class is gated off by default, so a failure should be one switch, but
   the first game build carrying all of it is a big step. Should Check B be split (names + zip first, then scene records)?
2. **Hooks inside game logic.** The arc says "a hook inside game logic gets a determinism check" (section 7). The scene director's
   mid-match starter and the forced victory id change what the game does; the class contributors only read. I plan a determinism
   check (replay-mode hash stream) on the scene director's check; is that the right bar?
3. **Space convention for live records** (game space vs viewer space) differs between class lanes; a reviewer caught it. I intend one
   rule in `SCENE_RECORD.md` before the classes run live.
4. **The judge is slow** (replays every earlier frame per query); fixes in progress (snapshot instead of replay, batched reads). Until
   then the judge is a per-frame tool, not a corpus sweep.
5. **The orchestrator's own context** (this one, ~60% full): the arc's rule 8 says fresh contexts per phase; this context has run
   through Phase B's end and Phase C's start. STATUS and the summary are kept current for a handoff.
6. **Post chain:** the largest visible gap to the game on rdr. Not yet a lane.

## 6. Questions for Fable

1. Given section 4.2 (Phase C classes built in parallel), keep the per-class order for the **live checks** (UI first), or check all
   classes in one milestone?
2. The scene director (bank ids through the game's scene manager): does it change anything in section 4A's design (addresses, verbs,
   "builders before the widget driver")?
3. Is "an Opus review per hand-back" worth writing into CLAUDE.md as a rule (cost: ~100-200k tokens per review; benefit: every review so
   far found real bugs)?
4. What should the post chain's place be: its own Phase C class (C5) now, or after the live checks of C1-C4?
5. Anything in sections 2-5 that looks like drift from the arc's intent ("one renderer, one scene, we drive it, go anywhere")?

---

Sources: `RENDERER_ARC.md`, `SESSION101_PLAN.md`, `SESSION101_SUMMARY.md` (running log), `STATUS.md`, the lane briefs
`docs\agents\s101\lane15..33`, the reader pages `docs\research\s101\`, the renderer pages `docs\renderer\` (CONSTANTS, FILE_CENSUS, FARM,
JUDGE, SCENE_RECORD, NAMES, PASS2D, SCENE_FRONTEND), and the comparison sheet `out\s101\compare\otl_game_vk_rdr.jpg` (local only).
