# Renderer arc, part 3: the structural cut (draft, written 2026-11-15 12:0x at the end of Session 102)

**Goal:** Bryan's entrance at 60 on the Surface Pro 6 at AUTO, true 720, SUSTAINED (the burst-aware bar: `Warm=60;Repeats=3`, median of the
SUSTAINED repeats, `tools\surface\bar_report.py`). Today (v0.3.3, sustained ~14 W): Bryan step 15 ~46-47, Bryan heavy ~45 (= ~22 ms), Brock ~51,
match ~60. Every wall must reach 16.7 ms.

**Shape (Jarrod, 2026-11-15: spine and satellites; memory `spine-and-satellites`, LESSONS "a plan built for many parallel lanes drifts into
band-aids"):** ONE spine owned end to end by the main session; satellite lanes only in its service or as knock-out-priced targeted levers on the
wall the newest profile names. Small levers never displace the spine.

## The walls today (R26 four walls, prof6; ab26-ab34 since)

| Wall | ms (heavy, sustained) | What it is |
|---|---|---|
| The game's update thread (CGameMainThread) | ~20 (the pace) | ~60 % is the game RECORDING draw commands for the replay (crowd ~4 ms, model submit ~6, ring ~1.5, casters / shadows); our code ~5 (U2 / U3 cut ~1) |
| The command / swap thread | ~17-18 | our per-draw hooks ~9 (reading ~1,250 recorded draws) + the game's replay ~6.6 |
| GPU | ~17-18 (16.2-16.7 at 14 W) | colour pass ~5 ms drained (the biggest step; group stamps `wwe2k14_rdr_gpu_group_stamps` now split it), 3 full-size frame copies, depth copy |
| Our render thread | ~15-16 (slack) | not the limit |

## The spine: stop recording and replaying what our renderer redraws

For every class our renderer already draws itself from the scene record (crowd, arena, ring, wrestlers, props, shadows / casters), the game
still records each draw on the update thread, the swap thread replays it, our reader reads it, and our binder throws most of it away. The cut
removes that whole path per class, so the update thread AND the command thread both shrink.

Evidence: the UQ1 knock-out (all crowd recording off, TEST ONLY) = update thread -2.2 ms, shown fps +5 (ab31 flipko); the safe UQ2 version only
reached ~1 ms because our crowd renderer still uses the game's first recorded draw of each run as its injection trigger and reads materials /
kept sets from the stream (P2b refusal, `docs\systems\crowd.md`): the spine must make our renderer independent of the recorded draws per class
(take its inputs from the scene record / our own hooks), THEN stop the recording.

Steps (each: knock-out first, ~6 min Surface flip; build only if the ceiling is >= ~1 ms on the pace wall; re-profile after each landed class):
1. **Knock-outs per class** (test-only bits, like `wwe2k14_rdr_record_skip` 0x80): crowd (done: +5 fps), arena, ring, wrestlers, props,
   shadow casters, the model-submit path (`sub_82C13C88` / `sub_82C04DF0`, 6.1 ms in R22). One batch of flips prices them all.
2. **Crowd independence:** our crowd renderer takes its run triggers / materials / kept sets from our own observers, not the recorded first draw;
   then the member and card records stop (the UQ1 / UQ2 machinery generalised), the replay and the reader never see them.
3. **The next classes by measured ceiling** (likely arena and wrestlers: the colour pass's 53 % arena + model parts), same pattern.
4. **Re-profile** (symbols profile, update thread named) after every landed class; the spine aims at whatever wall is then the pace.

## Satellites (parallel lanes)

- **Serving the spine:** per-class census tools (which recorded draws reach the binder and are dropped), the knock-out builds, the proofs
  (picture sheets per class: Jarrod judges; the in-run flip with CEN1's per-phase census), per-class code reading.
- **GPU (the structural cut barely touches it; it may become the wall once the CPU threads drop):** the colour pass by group (R24-F stamps on
  the Surface first), the remaining frame-buffer copies (R24-B ping-pong, parked on its branch: knock-out it first), the depth copy.
- **Targeted small levers** only against the wall the newest profile names, knock-out priced, measured in one combined flip per batch.

## Rules carried (CLAUDE.md, docs\agents\s102\common.md "Build fast, prove small")

Knock-out first; aim at the pace-setter; one capture while developing, the full proof once at a gate under ~10 min; small levers measured together;
no crash hunts on weak test hardware (the Surface lost device is a known issue: `docs\renderer\DEVICE_LOST.md` section 14); the bench base
(`tools\surface\base_flags.txt`) and the player bundle (`src\bundle3.h`) stay equal (`tools\bundle3\lint.py`); every kept lever joins bundle 3;
watches alert on silence; out\ is scratch (TIDY2).

## Not in part 3 unless the numbers ask

K3 / D3 frame skips (shown fps -16), SW2 / particle lite, present_fetch, colour cuts G10 / C4, swap_tail2 (game-timing input), the observer hooks
(knock-out ~0).
