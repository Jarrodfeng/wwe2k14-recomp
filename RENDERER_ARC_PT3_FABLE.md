# Renderer arc, part 3: leaving the command stream, wholly

Written 2026-11-15 21:1x-21:4x by the planning session (Fable 5.2) with Jarrod, after the audit of part 2
(`docs\research\FABLE_AUDIT_S102.md`). It replaces my two earlier versions of this page and revises the draft `RENDERER_ARC_PT3.md`.
**One step, one spine:** the game stops describing its picture as draw commands, and our renderer draws the frame from the scene's
state. Every lane serves that. Nothing here is built.
**Expires** when the command stream is gone from the player's game, or when Jarrod replaces it.

Marks: **[C]** read in the file named. **[L]** my reasoning. **[U]** unknown.

---

## 1. Jarrod's decisions (2026-11-15, evening, his words)
| # | Topic | His words |
|---|---|---|
| 1 | Measuring | "Is the profile needed? ... just *how* much measuring was done, felt like an excessive use of time." |
| 2 | Gates | "we had two gates, one before the review, and after the Opus review, which was a massive waste of time because almost every single time the reviews would land changes/fixes that made the first gates pointless." |
| 3 | The flow | "we know the big cuts we need to take, and we just need to get there, review to make sure it's clean and only then measure just how much we've improved." |
| 4 | The shape | "we should be dedicated to the big movement sticks, potentially one by one, spines and satellites. Things like removing the Command thread and such you know, the biggest levers." |
| 5 | Ambition | "seems like you weren't very ambitious in part 2, work not walls, we just had to bust down those walls." |
| 6 | The step | "Alright, finally leaving the command stream, wholly and fully." |
To record in `docs\DECISIONS.md` by the session that runs this (the audit brief limited me to my own memo files).

## 2. The goal and the one bar
- **The step is done** when no part of the player's picture comes through the game's command stream: nothing recorded for it, nothing
  replayed, nothing read, nothing bound.
- **The bar it serves:** Bryan's entrance at 60 shown on the Surface Pro 6, AUTO, true 720, sustained (the burst-aware bar,
  `tools\surface\bar_report.py`). Today about 46 [C: ledger row 163]. No nearer bar steers the work.

## 3. Why this is the biggest stick
The same description of the scene is paid for four times, on three threads, every frame (Bryan heavy, sustained):
| Stage | Thread | ms a frame | Source |
|---|---|---|---|
| The game walks the scene and records the draws | update thread (the pace) | 12.4 of 20.6 | R26:9, R26:114 |
| The game replays them through the 360's D3D library | command thread | 6.6 | R26:63 |
| Our reader reads each replayed draw | command thread | 8.7 | R26:62 |
| Our binder binds each one again | our render thread | about 14 of 18-19.5 | R23:138, R26:14 |
About 42 ms of CPU work a frame for about 1,250 draws. Only 30-60 of them are not the 3D scene (UI 10-33, post 14-26, one movie)
[C: R26:73, `WHATS_LEFT.md:53-64`].

## 4. We are not starting from zero
| Already built | State | Source |
|---|---|---|
| A drawer that needs no stream: the scene front-end. Arena, wrestlers in attires, entrances, from our loaders, with the game's effects. "Nothing reads game memory; no D3D stream; none of the binder is used" | built for the viewer; judged against the viewer's picture, not the game's | `docs\renderer\SCENE_FRONTEND.md` lines 9-17 |
| The live scene record and its writers: arena, arena worlds, casters, circles, crowd, light states, plane, ring rows, spheres, wrestlers | live in the game since Session 101 | `src\scene_live\*_contrib*`, `docs\renderer\SCENE_RECORD.md` |
| A tool that draws a scene record alone | built | `tools\scene\rsc_draw.cpp` |
| Our post chain, our UI pictures and text, our crowd, our shadow casters and plane shadows | on by default, but hung on the stream's draws | bundle `wwe2k14_s101_promotions`; `docs\renderer\*_OURS.md` |
| Whole passes owned offline: shadow module, rim map, plane, pre-light, sweat, particles, the crowd card pictures | offline arms; live switches never wired | `OWN_SHADOW_RIM.md:7`, `PLANE_OURS_WHOLE.md:5`, `OWN_SMALL_PASSES.md:8`, `OWN_PARTICLES_NODE.md:9` |
| The contract for "the game does not walk this pass" | written, never hooked | `docs\renderer\OWN_THE_PASS.md` |
| The game-side skips for ring, casters, plane, crowd members, arena | built; -3.74 ms on the update thread when forced | ledger row 159 |
| The list of values the game's logic sets at run time, per effect | generated | `docs\renderer\CONSTANTS.md` "The RUNTIME list" |
**So the step is: the live game's frame comes out of the scene front-end fed by the live scene record, not out of the binder fed by
the stream.** Session 101 built both halves and then joined them the timid way, class by class inside the game's draws.

## 5. The step, in two landings
### Landing 1: the 3D scene
One switch, decided per frame: **the scene is ours.** When it is:
- the game does not run the draw half of its scene tasks (crowd, model submit, ring, shadow module, plane, particles, the card
  pictures) [C: the tasks, R26:41];
- our renderer builds the frame from the scene record: our shadow and light maps, the colour pass in our own order, our post;
- the 2D remnant (UI, text, movies) is still replayed and laid over our frame.
When it is not (a scene or mode that is not ready): the game's own renderer draws that frame as today. Two clean states, no mix.

### Landing 2: the remnant, and the deletion
- UI, text and movies are drawn by us from our own observers. The command thread then has nothing to replay.
- Every mode is covered (the `goto` sweeps of Session 101: match types, arenas, entrances, menus, create modes, 30 Years, Universe),
  each with a sheet.
- **Then delete:** the replay hooks, our reader, our binder, the stream format, the in-place hooks, the thin paths, the markers, the
  skip and lasting-work logic, and bundle 3's levers that only made the stream cheaper. "Wholly and fully" is this line.
  **Scope (Jarrod, 2026-11-16, DECISIONS.md):** "delete" means compiled OUT of the PLAYER build (release wwe2k14.exe / v3): the player's game
  draws only through our renderer, no fallback. The dev / coverage builds and the repo KEEP the reader, binder and stream code: mirror mode,
  captures, fview, rplay and feature_diff depend on them (S103_FABLE_REVIEW.md section 2.3).

## 6. The lanes (all in the spine's service; six or more at once from the first hour)
The main session holds the spine: the frame's contract, the switch and its fallback, and the integration. It takes the hardest lane
itself.
| Lane | What | Starts from |
|---|---|---|
| S | **The frame contract:** a live frame = the scene record + our frame graph; the switch; the fallback; who reads what, where | `OWN_THE_PASS.md`, `SCENE_RECORD.md` |
| R | **The offline rig, first:** draw a captured frame from its scene record alone, beside the game's picture, per class and whole. This is the proof tool for every other lane, and its first sheet shows how far a record-drawn frame is from the game today | `rsc_draw.cpp`, the frame viewer, the gate's kept captures |
| A | **Inputs complete:** every run-time value on the RUNTIME list read by our observers outside the walks (poses, light states, fades, material values); our hooks inside the walks moved out | `CONSTANTS.md`, the model-entry hook (R26:49) |
| B | **Wrestlers** through the front-end, live: skinning, attires, created superstars, hair | `SCENE_FRONTEND.md` "Wrestlers", `wrestler_contrib.cpp` |
| C | **Arena, ring, props, stage screens, flashes** through the front-end, live | `arena_contrib.cpp`, `ring_rows_contrib.h` |
| D | **Crowd** with no trigger draw: members, cards, floor, signs from the record | `crowd_contrib.cpp`, `docs\systems\crowd.md` |
| E | **Shadows and maps as our own passes:** casters, plane, circles, rim, pre-light, sweat | the offline arms in section 4 |
| F | **Order and see-through parts; particles** in our colour pass | `OWN_PARTICLES_NODE.md` |
| G | **Our render thread draws from lists it keeps**, and the hand-off never collapses: the shown rate is the slower of game and renderer, never less | `SKIP_COST.md`, R16 |
| H | **The game side:** the scene tasks' draw half off under the switch; the barrier and the serial tail | `OWN_THE_PASS.md`, R22 section 5 |
| I | **The 2D remnant ours** (landing 2) | `UI_OURS.md`, `MOVIE_OURS.md` |
| J | **Coverage:** the sweeps and their sheets; the list of scenes still on the fallback | `tools\goto`, the coverage board |
| K | **Deletion** (landing 2) | - |

## 7. How a piece of work flows
1. Build. The builder checks its own work on one capture with lane R's rig. No gate.
2. Opus review, started at hand-back, while the next piece is being built.
3. Fixes go back to the same builder.
4. One gate, once, after the fixes. Never before the review.
5. Merge.
**Before landing 1, live pictures cost nothing to compare:** a mirror mode draws our frame hidden beside the game's in the same run
and posts the sheet (the way Session 100's first live stage worked). No measuring until the landing.
**Measuring:** at landing 1 and at landing 2, one PC check with sheets and one Surface flip of the switch, then a bar. A profile
only if a landing gives clearly less than section 3 says.

## 8. What gets easier once the stream is gone, and what does not
**Easier:**
- **The GPU's big cut.** The frame's passes are ours, so the copies and frame passes that exist for the 360's memory layout can go by
  design: 4.5 ms of frame passes and 2.1 ms of full-size copies in the 19.7 ms per-step table [C: R24 section 9].
- **Cores.** Lists we keep can be recorded on several threads; nothing is tied to the game's one replay thread (Jarrod, DECISIONS
  2026-11-14 19:4x).
- **Pacing.** No barrier-bound replay and no serial tail: the frame is the game's simulation, and we present at our own rate.
- **The crowd.** Instanced and skinned on the GPU, with no need to match the game's draw per member.
- **The code.** Texture and geometry tracking for the stream, the proofs that run in the game, most of the 224 renderer switches.
- **The sandbox.** A recording of scene records is a replay from any camera.
**Not easier:**
- The game's own simulation on the update thread (about 8 ms) stays the game's.
- The GPU's work per pixel is the same until the frame is reworked.
- Rare modes: each needs its sheet before the old path can be deleted.

## 9. Risks, said plainly
1. **Part 2's reader judged the main scene "cannot be owned whole yet"**: many classes, the order of see-through parts, screens and
   UI after it [C: R4:23]. I judge those to be work, not walls [L]. Lanes F and C carry them.
2. **How far a record-drawn frame is from the game's today is unknown.** In Session 101 the front-end matched the viewer and both were
   "flatter and brighter than the game" [C: `RENDERER_ARC_PROGRESS.md` line 46]. Light states and our post chain were added since
   [C: `SCENE_RECORD.md` 179]. Lane R's first sheet answers it, with 0 runs.
3. **The first speed number comes at landing 1,** later than a class-by-class plan would give one. The mirror mode and the rig keep
   the picture honest until then; the speed is the one thing we wait for.
4. **The wrestlers' run-time values** are the longest list in `CONSTANTS.md`. Lane A starts in the first hour for that reason.
5. **Our render thread's cost with the front-end live is unmeasured** [U]. Lane G exists so it is designed, not discovered.

## 10. Run budget (CLAUDE.md rule 7; builders and reviewers 0 game runs, 0 Surface runs)
| What | Game runs (PC) | Surface jobs |
|---|---|---|
| Building, until landing 1 | mirror-mode sheet runs: about 2 per scene family, about 8 in all | 0 |
| Landing 1 | one check, about 6 (sheets, counts, validation), + 2 per real failure | 1 flip, 1 bar |
| Coverage sweep (landing 2) | about 16-30 starts, announced to Jarrod first | 0 |
| Landing 2 | one check, about 6, + 2 per real failure | 1 flip, 1 bar |
Estimates, not measurements. Over budget is said in the report.

## 11. Rules
Carried from Session 102: remove work, do not work around it; one capture while developing; no crash hunts on the Surface; the bench
base equals the player's defaults; `out\` is scratch.
For this step:
1. **Build, review, one gate, then measure.**
2. **The spine comes first each round.** A round with no spine commit says so in the first line of its report.
3. **A lane is admitted only if it names the landing it serves.**
4. **Distance in one line each round, from counts:** lanes of section 6 landed and left; scenes on the fallback list; recorded draws a
   frame still in the stream.
5. **Nothing is proved inside the player's game.** Proof is the rig, the mirror sheets, the one check.
6. **No lever that makes record, replay, read or bind cheaper.** That pipeline is being removed.
7. **A short audit at landing 1,** before landing 2's deletion starts (Jarrod's call who does it).

## 12. To read alongside the building (none of it blocks a lane) [U]
- Which of our input hooks sit inside the walks that will be skipped.
- The order the game draws see-through parts in, per arena.
- Whether the game's clock follows real time when a frame is long (shapes lane G).
- What the scene record still lacks for create modes and menus with 3D previews.

## 13. Not in this step
- The GPU frame rework itself (the next step; section 8 says why it gets easier).
- Any small lever, on any wall. New features.
- A lower internal resolution as an arc lever (Jarrod, DECISIONS 2026-11-14 18:3x).
