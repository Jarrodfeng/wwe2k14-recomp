# The renderer arc, part 2: back to 60 on the Surface, and the game stops drawing

**CLOSED 2026-11-15 12:1x (Session 102, main session Opus 5.5).** Outcome: v0.3.3 released (PC full, Android pre-release) with the kept levers as the player default (bundle 3, `src\bundle3.h`); Surface, sustained, player defaults: Bryan's entrance 37.7 -> 45.8 fps (+21 %), Brock's 40.7 -> 51.3 (+26 %), the match holds 60. **The goal (Bryan at 60) was not reached** (~46 sustained). Most lanes made the record -> read -> replay pipeline cheaper (memos, lite paths, frame skips) instead of removing it; the "game stops drawing" half of this plan never became a lane (LESSONS: "a plan built for many parallel lanes drifts into band-aids"). Part 3 (`RENDERER_ARC_PT3.md`) makes that structural cut its spine. Record: `SESSION102_SUMMARY.md`, `docs\renderer\BUDGET_LEDGER.csv`, `docs\DECISIONS.md` (2026-11-14 / 15).

Written 2026-11-13 22:50-23:54 by the planning session (Fable 5.2) from an interview with Jarrod and twelve read-only readers, for
the main session (Opus 5.5) to run as Session 102.
**Scope:** speed on weak hardware and the rest of the ownership the arc promised: our renderer draws whole passes itself and the
game stops recording them. It ships as v0.3.3. **Expires** when Bryan's entrance holds 60 on the Surface at the default settings for
integrated graphics, or when Jarrod replaces it. Part 1 is `RENDERER_ARC.md`; its decisions stand unless section 1 changes them.
Lane contracts: `docs\agents\s102\LANES.md`. Reader pages (exact code locations): `docs\research\part2\R1..R12`. Kickoff:
`session102_kickoff_prompt.md`. Nothing here is built.

Marks: **[C]** read in the repo, a log or a tool's output by this session (source named). **[L]** a lead: a reader's page (named) or
my reasoning, to verify before building on it. **[U]** unknown.

---

## 1. Jarrod's decisions (interview, 2026-11-13 22:50-23:5x)

| # | Topic | His decision, his words |
|---|---|---|
| 1 | What part 2 is | "Part 2 and Speed kinda go hand in hand no, for part 2 I'd love both Speed and finishing our ownership and separation from the 360 architecture." "Ownership should continue, but performance is the key target at the moment!!!" |
| 2 | Why now | "Since release I've already received 5 performance complaints which is troublesome. I'm preferring to just speedrun 0.3.3 a performance and ownership focused updates, so I want for the part 2 session to build as many lanes as possible, staying as effecient and speedy as possible too!" |
| 3 | The judge | "The surface is our benchmark! ... by far the weakest device I have, being a wonderful floor of a target for everyone else, Bryan entrance 60fps is the true target, has been for awhile now." It "can and will stay on throughout". |
| 4 | Settings for the target | "what we have set as the default for iGPUs, I believe this is AUTO, 720p Resolution." |
| 5 | The complaints' hardware | "Let's not change for the complaints, the Surface is definitely the weakest device in the chain even compared to the tester devices, thermally throttling especially." |
| 6 | The shape | "I like this three track, many lanes process, it feels like the fastest, most efffecient way to do it that takes advantage of my system being quite powerful on its own. 4090 makes an awful benchmarking card heh, but it's EXCELLENT for development work!" |
| 7 | Weak hardware until then | No quick patch: "we're on the road back to 60 on that Surface Pro ... continuing forward to give everyone a far more performant 0.3.3!" |
| 8 | The title card | "Players have been reporting black screens on the main menu title card, which is troublesome and I think stems from us perhaps not owning it properly?" |
| 9 | Music | Nothing needed for 0.3.3 ("I thought 0.3.2 ended music modding on a fairly comfortable state"). The four small known issues are parked. |
| 10 | One long session | "Endless session with no natural stops is fine, you auto-compact, and your updates to STatus, summary, and other notes keeps you in check. It's the preferred route for me for unattended sesssions as it's the most hands off, and starting sessions remotely is difficult." |
| 11 | Sheets | "I actually thought there weren't enough sheets, I like having the visual, though the work continuing without me is more important than that ... under no means should you ever stop for me to look at one especially if I've told you we're unattended, which you usually are." |
| 12 | Test builds | "the ones you gave me had settings switched off which was counter intuitive. We'd just been doing work, that I was presented an APK with without that work turned on." |
| 13 | Stalls | "the most damaging is when you wait on me for unjsut reasoning, or as I said, become 'Lazy' less lanes than asked, not effeciently moving to the goal." |
| 14 | The bug button | "has never actually been used ... I've not actually used it myself on either PC or phone, regardless, you've been good at finding issues." |
| 15 | Storage | "The file space issues were definitely prominent, and sneaky too especially on unattended sessions." The project stays on C:: "I'd rather not have to make a scratch disk on another drive, just staying on top of the maintenance of the project drive is fine." |
| 16 | Reading before the session | "Yes, run the readers." |
| 17 | The release gate | "both assumptions are good": v0.3.3 ships at the first build where the Surface is at least as fast as the old renderer in the match and both entrances, and the black title card is fixed. Bryan at 60 is the arc's finish and ships as soon as it is reached. |
| 18 | Surface runs | A standing part of the work, counted apart from rule 7's check runs. Not after every merge: "after every merge sounds a like a LOT of benchmarks on the Surface no? Do we need that many?" Three tiers (section 7): about 4 to 8 full benches a day. |

| 19 | Knock-out runs | "sound like a smart way to go, though I worry you'll immediately determine a goal isn't feasible, since surely there's work on top of those runs that can be done." They rank and order the work; they never cancel a goal (section 8). |
| 20 | The landing strip | "widening that landing strip is a great idea, no waiting, god 32% of the time, we gotta cut that down pronto!" Track captains and three merge trains. |
| 21 | Lazy made impossible | "Lazy mechnically impossible sounds great, I dig it." The ready-lanes file and the stop hook. |
| 22 | Auto-resume | "unnecessary, highly isolated incident." Not built. |
| 23 | The phone overnight | Not possible: "I need it near me, which is away from the comptuer." Phone evidence stays his hands-on. |
| 24 | Other graphics cards | "Simulating other graphics cards would be excellent, particularly AMD ones, I agree with the tester ring as well, benchmark button would be a great feature there, can have it be the Lesnar vs Bryan we've been using for consistency." |
| 25 | Review findings | "Findings becoming a check is SPECTACULRA and I can see that saving a huge amount of time." |
| 26 | Hard bugs | "Agree with the hard bug measuring, parallel investigations are smart for anything particularly tricky." |
| 27 | The Surface's idle time | "it spends 99% of its own time waiting for benchmarks, different combinations and such are probably helpful!" |
| 28 | The summary | Written by the main session, not generated: "your strength is that you know about the project, so you writing it makes sense no?" The board and the ledger are generated; the judgement is not. |
| 29 | Cleaning up | "smart, though it should be measured, don't want to delete any good code, though I suppose it's all committed ... anyways so no real loss." |
| 30 | Usage | "No pacing, full send. I can watch the weekly limit, I don't want it to influence anything." |

| 31 | True 720p (2026-11-14 00:00) | Asked: if the GPU cannot reach 60 at true 720p, may AUTO use a lower internal resolution with upscaling? "Lower is fine." True 720p is tried first; a lower internal resolution with upscaling is an allowed lever for AUTO when the GPU lanes at 720p fall short. |

---

## 2. How part 1 went (measured, for what part 2 must keep and fix)

### 2.1 The process
| | Session 100 | Session 101 |
|---|---|---|
| Clock | 42 h 53 | 54 h 04 |
| Agents | 24 | 548 |
| At least one agent working | 7 % of the clock | 91 % |
| Main session compiling in the foreground | 2 h 27 | 0 h 40 |
| Opus reviews | a handful | 194 |
| Game runs | ~205 | ~196 (the arc estimated ~80) |

[C: `py -I tools\harness\session_time.py --since "2026-11-11 16:24" --session f2e609d0...` and `4ee217f6...`; STATUS.]
1,817 commits, 131 offline gate lanes, v0.3.2 released (PC full, Android pre-release).

- **Keep:** lanes; contracts first; an offline proof per piece and a past bug re-found; an Opus review of every hand-back; names for
  every draw (the guessing family of faults fell from 18 to 3 [L: the two fault ledgers]).
- **Fix:** builders waited 82 h 25 of their 254 h 31 on checks and reviews [C: same tool]: the queue moved from one worker to one
  integrator. 13 faults were lanes misbehaving on the shared machine (disk, wrong base, windows, writes outside their folder) and
  13 were tooling (stale builds gave false results) [L: ledger]. The PC restarted at 03:31 on the 12th and nothing ran until 10:01.

### 2.2 The product
- **Reached:** every draw named; the frame viewer, case zips, gate, atlas, solver, judge, farm; the viewer on our renderer; every
  scene class promoted and on by default; `goto` and scene ids; no emulated GPU in the player build.
- **Not reached: we do not drive.** A promoted class puts our asset inside the game's own draw: the game still records every draw,
  the 360 D3D library replays it, our reader reads it, our binder swaps the asset in [C: `SKIP_GAME_WORK_MAP.md` section 0]. The five
  cuts that skip the game's work for one object shipped OFF [C: STATUS]. `reader.h` grew 272 -> 400 KB, `binder.h` 49 -> 243 KB;
  116 `wwe2k14_rdr_*` switches [C].
- **Went backwards:** the Surface. A match showed 60 on v0.2.19's old renderer and shows 47-53 on v0.3.2 [C: `docs\BENCHMARKS.md`].
  Found at 18:38 on release day: the Surface was out of the loop for two days.
- **Open at the release:** a black title card on players' machines; the selection-screen picture; the glitch behind the title logo;
  the phone column of the coverage board empty; text routes not retired.

### 2.3 The lessons part 2 is built on
1. **The PC hides everything.** The device that judges must be in the loop from the first hour.
2. **Offline proof is the first gate, not the goal.** 23 of 71 faults were new scene code that passed its offline proof and failed
   on first contact with the live game [L: ledger].
3. **Our assets inside the game's draws cannot be cheaper than the game's version.** It adds our work to the game's.
4. **A cut grafted onto the stream inherits the stream's bookkeeping** (markers, lasting binds) [C: `docs\BENCHMARKS.md`].
5. **We have been measuring the wrong thing.** The Surface bench has never run what a player gets (3.2), its "GPU ms" includes
   waiting (3.3), and it logged no heat on release day (3.4). Three numbers I quoted to Jarrod in this interview were wrong or
   overstated for these reasons. Step 0 fixes the instrument before anything is ranked.

---

## 3. What the Surface says today (and how far to trust it)

Surface Pro 6: i5-8250U (4 cores, 8 threads), Intel UHD 620, 60 Hz, panel 2736x1824, 8 GB.

### 3.1 The numbers we have (release day, build b22, STANDARD tier, cuts ON in the entrances)
| Scene | Shown | Our render thread | The game's swap thread | "GPU ms" (upper bound, 3.3) |
|---|---|---|---|---|
| Match (cuts off) | ~53 (S102 review: BENCHMARKS' 47.4 counted present rows, not presents; run 186 = 53.1) | parse 3.4 + bind 8.4 + record / submit 4.0 per drawn frame | 15.4 ms (our reader 9.4 of it) | 14.7-15.0 median |
| Brock's entrance | ~30 | not split | ~21 ms (48 swaps a second) | 16.6-19.2 |
| Bryan's entrance | ~31 | **34.4 ms per shown frame**: parse 7.2 per handled frame, bind 17.5 per drawn frame, record / submit 4.5, lasting work 13.5 per lasting frame | ~23 ms (44 swaps a second) | 23.1-28.5 |

[C: `out\s101\surface_rebench\y\` logs 186 / 187 / 188; `docs\BENCHMARKS.md`. The 34.4 is R3's accounting of log 187, window 2.]
Budget for 60: 16.7 ms on each side.

### 3.2 The bench has never run the player's default [L: R10, R1, R2 agree; `tools\surface\bench.ps1:73`]
- The bench writes a settings file that selects STANDARD, and because two keys are missing every LOW lever is off.
- A player on this machine gets AUTO = LOW: crowd 70 %, wrestler shadow maps every other frame, and the SGSR upscaler over the
  full 2736x1824 panel. Only those three LOW levers work in the new renderer; the rest of LOW's flags are ignored [L: R1, R2].
- The entrances were measured with the cuts on. v0.3.2 ships them off: a player's Bryan entrance holds about 370 more member draws
  a frame than the logs [L: R6].
- **So the real baseline is unknown.** It may be better (lighter crowd) or worse (the upscale pass, the extra draws).

### 3.3 "GPU ms" is not pure GPU work [L: R1, `renderer.cpp:3128 / 3160`]
The time stamps bracket the whole command buffer, which includes waiting for the next screen image. A boot or menu frame of five
trivial passes reads 11-12 ms. The GPU column is an upper bound; the split between work and waiting is unknown. Windows' own GPU
load figure reads 90-95 % in the entrances and ~85 % in the match [L: R1; R5 says every power log on disk is from 2026-11-07, so
which runs that figure belongs to must be re-checked].

### 3.4 Heat and power: nothing recorded on release day [L: R5, R10]
- No power, clock or temperature record exists for any 2026-11-13 run (`-Power` is off by default; the frequency counter the
  bench logs is a constant). The "1.8 GHz" I read from the thread log is a fixed-rate counter, not the clock: **retracted.**
- From 2026-11-07: the clock averaged about 1.9-2.1 GHz under load, and it fell as the GPU's load rose (about 2.7 GHz at half
  GPU load) [L: R1's reading]. If that holds, the GPU and the CPU share one power budget and the three tracks help each other.

### 3.5 Where the waste is, as read from code and logs [L unless marked]
- **Two thirds of Bryan's draws are never drawn.** ~2,250 stream draws a frame, ~760 bound. The far crowd cards (about 1,666 a
  frame on average) are recorded by the game, read, written out, parsed and then dropped by the binder, because our card pass
  already draws them as 20 instanced draws. The cut that stops this is built and proven offline and was never switched on
  (`src\scene_live\card_thin_core.h:26`) [R3, R6].
- **"Parse" is most likely one thing:** every frame each block of shader constants is expanded to a full 4 KB copy in fresh
  memory, up to ~18 MB a frame (`capture_reader.h:170-187`, `live.cpp:492`). Skipped frames pay it too [R3; unsplit by any timer].
- **Nothing bound survives a frame:** constants packed twice and copied three times, ~530 bytes hashed a byte at a time, the
  de-dup tables freed and rebuilt each frame (`frame_builder.h:87-112`) [R3].
- **A skipped frame is never cheap:** it pays the parse, and its lasting bind keeps 85-96 % of the draws [R3; C: BENCHMARKS].
- **Crowd members are one draw each**, 4-5 us a member; only one value differs between members of a type [R6].
- **About one core goes to threads nobody can name:** the process uses 4.2-5.1 of 8 logical processors; our render thread, the
  swap thread and the update thread account for ~2.2; four or five unnamed steady threads take 1.0-1.4 [R5].
- **The Surface is short of memory:** committed 6.9-8.5 GB against a limit of 8.6-9.5 GB, 5,000-8,600 hard page faults a
  second during play [R5]. Not traced.
- **Dead work:** the crowd card bake does nothing visible in a match (15 of 35 passes and 15 of 49 copies of a match frame); in
  entrances the game bakes five light slots a frame and three are never used [R7, R4].
- **Built and not shipped:** the AVX2 build of the game code (`"v3": false` in the release record), profile-guided builds
  (the newest profile covers a match only; our renderer code never gets one), LOW's small shadow maps (the switch exists,
  nothing sets it) [R8, R2].
- **The old renderer's 49.4 in Bryan was STANDARD plus LOW's levers by flag plus 17 weak-graphics switches** [R2]. Part of the
  31-versus-49 gap is settings, not renderer.

---

## 4. The bar

1. **The finish:** Bryan's entrance holds 60 shown on the Surface at AUTO, 720p, plugged in, at a steady temperature, with a
   1 % low no worse than 54.
2. **The release (decision 17):** v0.3.3 ships when the Surface, at AUTO 720p, shows at least 60 in the match, 56.7 in Brock's
   entrance and 49.4 in Bryan's (the old renderer's best), and the black title card is fixed.
3. **No picture regressions:** every owned pass is judged by a sheet (eyes) and by counts, never by draw-for-draw equality.
4. **Ownership:** a pass counts as ours only when the game does not walk it: no records, no replay, no reader, no markers.
5. **Nothing a player can see is traded for speed at STANDARD and above.** LOW may draw maps less often and smaller, as its
   half-rate shadow maps already do.

---

## 5. The design in one page

**Three tracks, each with its own budget line, many lanes in each, and one power budget under all three.**

- **Our render thread.** Stop doing work for draws that are never drawn; stop rebuilding what did not change; make a skipped frame
  cost nothing or draw it. R3's first five cuts are estimated at 10-18 ms of Bryan's 34.4 per shown frame, without touching a hook.
- **The game's side.** Own whole passes. The frame has ONE render walk whose body is the pass list (`sub_82C2FA10`); every pass
  driver is a draw node; the whole-pass switch sits on a node's slot-1 method, and two passes already have the game's own off
  switch there [L: R4]. When a pass is ours the game does not walk it and our renderer draws it from the scene record.
- **The GPU.** First find out what is work and what is waiting. Then: LOW's missing levers, the output pass over the panel, maps
  taking turns instead of all redrawn each frame, dead bakes removed.

**Across all three:** the crowd (the cards first, then members instanced); wasted CPU on other threads; memory.

**Order of attack, cheapest and surest first:**
1. Make the instrument honest (step 0).
2. Switch on and finish what is already built (the card cut, the bake cut, AVX2, LOW's shadow size).
3. The render thread's own waste (R3 cuts 1-6).
4. Whole passes, offscreen ones first (R4's order).
5. Everything else, ranked by the ledger.

---

## 6. Design details per track (exact locations in the reader pages)

### 6.1 Our render thread [R3 sections 7-8, R6]
| Cut | What | Estimate, Bryan, per shown frame | Risk |
|---|---|---|---|
| T1 | No bank expansion on the live path: resolve a draw's constants only when packed; reuse the frame's memory | 4.5-7.5 ms | low-medium |
| T2 | Never send the draws we drop: switch the far-card cut on, then the same for any class ours fully draws | 2-4 ms, plus the game's reader time | medium |
| T3 | A lasting frame is drawn and shown instead of skipped; then a decaying image guard and no group rule | ~1.6 ms turned into pictures; more unknown | low, then high |
| T4 | A cheaper frame builder: word-wise hashes, tables that keep their storage | 0.8-1.5 ms | low |
| T5 | Pack constants once, straight into the ring | 0.5-1.5 ms | low-medium |
| T6 | Crowd members as one instanced draw per body type (60 or fewer) | members cost 1.8 ms a frame today [R6] | medium |
| T7 | A bound-draw memo for static objects (arena, ring, props, screens) | 2-4 ms | high |
| T8 | Hand the frame over in memory, serialise only for a capture | small on our thread; the game thread's share unknown | low-medium |
| T9 | Record passes on two workers | record 2.0 -> ~1.2 ms | high |
All estimates are R3's from the logs, not measurements. T1-T5 together: 10-18 ms.

### 6.2 The game's side: own the pass [R4]
- **The contract (P0, the main session writes it first):** the switch is a hook on a draw node's slot-1 method; the hook decides at
  record time, per frame, and leaves one stamp the render side checks; the fallback is "the game walks the pass this frame", never
  "our pass is missing"; **no value comes from the stream** (a whole-pass skip has no stream draws to read, which is what the
  refused crowd injection of Session 101 leaned on).
- **One blocker is common:** the caster, circle and plane records are written by hooks inside the walkers, so they stop when the
  walker is skipped. Read those inputs where the game computes them (the fit task `sub_827E8158`, the plane task `sub_827EE380`,
  the module's state at frame close).
- **Order (R4 section 6):** P1 the crowd card bake; P2 pre-light and sweat; P3 the plane chain; P4 the shadow module and P5 the rim
  map as one "category 1" contract; P6 post; P7 particles; P8 the crowd node (after the member cut); P9 the main scene and UI, the
  arc's end. Offscreen passes come first because nothing of the game's is drawn into their targets, so order is ours.
- **Each owned pass deletes what it replaces** in the same lane: the in-place hook, the thin path, the object markers, their
  switches.

### 6.3 The GPU [R1, R2, R7]
- **G0 (step 0):** per-pass GPU stamps that exclude the wait (`renderer.cpp:736` pool, the step loop, a stamp after the wait).
- **G1 LOW's missing levers:** the small shadow-map size (`reader.h:350`, never set by LOW); the present clears the whole panel
  every frame (`swapchain.cpp:119`); whether Bryan's full-size copies are still made; whether skinning runs once.
- **G2 the output pass:** time SGSR at 2736x1824 under LOW (`gfx_settings.h:820`, `renderer.cpp` ~2755); a cap on the swapchain's
  size or the unmeasured 720p display lever (`swapchain.cpp:166-187`). **If true 720p still falls short after the GPU lanes, a
  lower internal resolution with upscaling for AUTO (decision 31):** the upscaler is already in the output pass, so its cost and
  its picture are measured here first.
- **G3 maps take turns:** count drawn frames, not binds (`binder.h:282, 286`: today's count goes irregular on the Surface); then
  the wrestler chain and rim on even frames, props, circles and plane on odd, pre-light slots alternating; the sweat map always
  (it accumulates). Exact "inputs unchanged" keys almost never hit in motion (0 of 31,527 draws in an entrance) and are not the
  lever [R7]. Jarrod approved half rates for the crowd pose, light strip and rim map on the old renderer; a sheet re-checks them.
- **G4 dead work:** the crowd bake in matches; pre-light slots 1-3 in entrances.
- **G5:** the 20-25 points of GPU load outside our own command buffers [R1]: find them.

### 6.4 The whole program [R8]
- **W1:** ship the AVX2 build of the game code; build our own sources and the SDK for it too (an older-CPU build stays available).
- **W2:** profile-guided builds: train on entrances as well as a match, include the renderer's sources, wire the SDK's
  profile-guided + link-time build into the release (its one Surface measurement: 2-3 %).
- **W3:** native versions of hot game functions only after the fresh profile: the last list is six days old and flat.

### 6.5 Heat, CPU waste and memory [R5]
- **H1:** name the unnamed threads (one log column: the thread's start module), then remove what is waste.
- **H2:** the timing thread's spin at every match load (60-97 % of a core for 3-4 s); the audio event thread (12-22 %, ten times
  the PC); the fence wait.
- **H3:** memory: what is committed, by component; cut duplicates; T1 removes ~18 MB of churn a frame.

### 6.6 Players [R9]
- **U1 the black title card.** Most likely: our renderer failed to start and there is nothing behind it. It logs one line
  (`[rdr][live] could not start:`, `reader.h:2400-2404`) and the window stays black with sound and input alive; the boot screens
  are black by design, so the title card is the first thing ever drawn. Build: a plain on-screen message, the launcher's graphics
  card choice honoured (`device.cpp` picks by type alone), submit and fence results checked, a renderer verdict in every report.
  Ranked from code only: no report from an affected player exists on this machine.
- **U2:** the selection-screen picture and the logo glitch are one class (a texture rewritten without notice): the phone run in
  STATUS "Next" item 1.
- **U3 (decision 24):** other graphics cards simulated on the PC with Vulkan's profiles layer, AMD first, in the gate [U: not
  checked it is installed]; a release candidate to a ring of volunteer testers before any release, one clean report per vendor.
- **U4 (decision 24):** a benchmark button that runs Lesnar vs Bryan (the match and the entrances the bench already uses) through
  `goto` and sends fps, timings and hardware.

### 6.7 Upkeep: the disk [R12]
The project stays on C: (decision 15). Fixes, each exact in R12: the 72-hour blanket on captures (`tidy.py:212`); the KEEP match
that accepts any text (`:785`: `out\s100`, 39.8 GB, is held by a comment); one path literal holding 39.3 GB of scratch
(`check_effect.py:57`); the scratch folder (57-59 GB) outside the tidy's roots with no rule at all; captures stored twice or
four times; no history of free space (free fell from 81 GB to 0.1 GB in 91 minutes); the hourly pass skipped when a run holds the
lock. About 137 GB should not be there today.

---

## 7. How we measure (the part that makes many lanes safe)

1. **The budget ledger.** One table: per scene, per side, the baseline, the target, and one row per lane with the milliseconds it
   claimed and the milliseconds the Surface measured. A lane without a measured row is not done. The release's defaults are the
   measured-best set of switches.
2. **Every lane ships behind a switch the Surface can flip inside one run.** Today most `wwe2k14_rdr_*` options are copied once
   at start (`reader.h:2110-2147`): making lane switches readable per frame is step 0 work [R10].
3. **Count on the PC, time on the Surface.** Integer counters in the recorder (calls by kind, bytes into the ring, vertices), a
   counts file from the replay player that is identical across two runs, a budget lane in the gate on the fixed Bryan capture; a
   per-pass pixel count on the PC only; then one calibration on the Surface. None of this exists offline today [R11].
4. **The Surface lab.** The replay player builds standalone but nothing of it reaches the Surface: put it in the kit, finish the
   farm's Surface host, capture at LOW, queue jobs [R10].
5. **Three tiers (decision 18).** Counts on the PC at every merge (seconds). A lab job on the Surface once per lane that claims a
   GPU or render-thread saving (about a minute, an estimate). The full bench only when it can change the answer: step 0; a
   game-side lane landing; the counts moving more than ~5 %; a drift check after ~6 quiet hours; a release candidate. About 4 to
   8 a day, ~6 minutes each [L: R10]. A run outside the steady thermal band is rejected and repeated once.
6. **Live smoke on the PC** after every merge batch: one hidden, muted run through a short fixed list of scenes; read only when
   it fails.

---

## 8. Step 0, then the lanes

**Step 0: make the instrument honest, then measure once** (the main session with three small builder lanes; before any lane's
saving is ranked).
1. The bench at the real default: AUTO (LOW), 720p, crowd 70 %, the LOW levers on [R10 gap (a)].
2. Heat on the record: power logging on by default, performance percentage, limit and package watts on the `[perf][sys]` line, the
   thread log's start-module column, the false GHz reading removed [R5 section 7].
3. GPU stamps per pass, excluding the wait; a render-thread column in the frames file [R1, R10].
4. Lane switches readable per frame; the in-run on / off route and its verdict script [R10 gap (b)].
5. **The baseline:** v0.3.2 as released at AUTO 720p, the three scenes, twice for the spread; and the same at STANDARD for
   continuity with release day.
6. **One symbols profile** (about 25 minutes) [R8].
7. **Knock-out runs in Bryan's entrance:** our renderer draws nothing; our render thread drops every frame; our reader reads
   nothing. Each gives the ceiling that track could reach today. **They rank and order the lanes; they never cancel a goal
   (decision 19).** A ceiling under 60 means the layer beneath has to move too (the game's own recording, which owning passes
   removes; the settings; the compiler), and the report says which, never "not feasible".
8. Memory: committed bytes by component, hard faults.

**The lanes** (contracts in `docs\agents\s102\LANES.md`; a lane starts the moment its inputs exist):

| Track | Lanes |
|---|---|
| Measure | M1 counters and the budget gate lane; M2 the Surface lab; M3 the board, ledger and one-line bench; M4 a fast gate that knows what it tests |
| Render thread | T1 lazy banks; T2 the card cut on; T3 lasting frames drawn; T4 frame builder; T5 constants packed once; T6 members instanced; T7 bound-draw memo; T8 in-memory hand-off; T9 parallel record |
| Game side | P0 the node-switch contract; P1 crowd bake; P2 pre-light and sweat; P3 plane chain; P4 shadow module; P5 rim map; P6 post; P7 particles; P8 crowd node; P9 main scene and UI |
| GPU | G1 LOW's missing levers; G2 the output pass; G3 maps take turns; G4 dead work; G5 the unexplained load; G6 whatever the per-pass stamps rank next |
| Whole program | W1 AVX2 shipped and widened; W2 profile-guided builds; W3 natives from the fresh profile |
| Heat and memory | H1 the unnamed threads; H2 spins and waits; H3 memory |
| Players | U1 the black title card; U2 selection screen and logo; U3 simulated cards and a tester ring; U4 the benchmark button |
| Upkeep | K1 the disk |
| Flow | F1 track captains and three merge trains; F2 the ready-lanes file and stop hook; F3 the pre-flight command; F4 review findings become checks; F5 the Surface's idle-time sweep of LOW's switches |

**Can start in the first message, with no input from step 0:** T1, T2, T4, T5, M1, M2, M3, M4, G1, G4, W1, W2, H1 (the log
column), U1, K1, F1-F5, P0 and P1's offline step. **Wait for step 0's numbers:** the ranking of G2, G3, G5, G6, T7-T9, W3, H2, H3.

---

## 9. How Session 102 runs

**Decided (Jarrod):**
1. **One long session** (decision 10). STATUS and the summary are the memory; auto-compaction is expected.
2. **As many lanes as there are pieces whose files do not overlap** (decision 2), never fewer than six while useful work exists.
   A free lane takes the next ready contract at once: no asking.
3. **The only reasons to wait on Jarrod** (decision 13): a release or any push; a default players get; game or Surface runs
   beyond what this plan grants; deleting something outside the tidy's own rules. Anything else is decided by the plan, the ledger
   or the main session, logged, and reported afterwards.
4. **Sheets** (decision 11): every check, every owned pass and every look change posts one sheet to the chat and keeps it under a
   hold. Work never waits for it to be looked at.
5. **A build handed to Jarrod has the session's work switched on** (decision 12), with one line saying what is on.
6. **Nothing on screen or speakers:** runs hidden, muted, behind, unfocused; tools headless.
7. **Storage** (decision 15): K1 first; free space and its rate of fall logged every ten minutes; below the reserve, lanes that
   write captures stop being started and one phone notice goes out. Lanes read shared captures in place and never copy them.

8. **Track captains and three merge trains** (decision 20). Each track has an Opus captain that briefs, reviews and merges its
   lanes on the track's own branch; conflicts stay inside a track; the main session stays small. The board shows waiting time
   per stage, and the target is builders waiting under 10 % of their time (it was 32 %).
9. **A ready-lanes file and a stop hook** (decision 21). The main session cannot end a turn while ready contracts exist and fewer
   than the minimum are running, or while it is waiting on Jarrod for a reason not on the list. [U: the hook is not built or
   tried; it is the first flow lane.]
10. **One pre-flight command before an unattended stretch:** disk reserve, the Surface (reachable, plugged in, at temperature),
    enough ready contracts for the hours ahead. No auto-resume (decision 22).
11. **Reviews:** always for a hook in the game, a format, a check tool or the bench; the reviewer writes the planted fault; **every
    finding that can become a lint or a gate check becomes one** (decision 25), so each class is caught by a tool the next time.
12. **Hard faults in the picture** (decision 26): measure first; when a fault is particularly tricky or one diagnosis has failed,
    three independent investigators at once.
13. **The Surface never idles** (decision 27): between benches it tries combinations of LOW's switches, scored on fps and on how
    much the picture changed, and reports the best set for AUTO.
14. **The main session writes the summary** (decision 28). The board and the ledger are generated; the judgement is not.
15. **Clean up, measured** (decision 29): a lane deletes the old path only after its replacement has its measured Surface row and
    a sheet; one commit per deletion, so anything can be brought back from git.
16. **No usage pacing** (decision 30): full send. Jarrod watches the weekly limit; it does not shape the work.
17. **Phone evidence is Jarrod's hands** (decision 23): each build for the phone has the work on and one line saying what to look at.

## 10. Runs (rule 7, as changed for this plan)
- Builders and reviewers: 0 game runs, 0 Surface runs of their own. They use the PC counters and the Surface lab's queue.
- The PC smoke run after each merge batch and the Surface's lab jobs and full benches (section 7 item 5) are standing lanes,
  counted apart, logged on the board.
- Step checks keep rule 7's shape: one check per step, a pass never re-run, at most 2 diagnosis runs per question.
- Step 0 on the Surface: the baseline (three scenes, twice, at AUTO and once at STANDARD), the knock-outs (three runs), one
  symbols profile.

## 11. Not in part 2
- New features, music follow-ups, the importer, replay / theatre work, the sandbox tools.
- The bug button for players.
- Fresh contexts per phase; a second drive or a virtual disk.
- Draw-for-draw equality with the game as the proof of an owned pass.
- Caching maps by "inputs unchanged" as a speed lever in motion (R7: it does not hit).
- Frame generation: parked, a last resort only if Bryan stalls short of 60.
- Re-ordering the work around the complaints' hardware.

## 12. Unknowns the readers could not settle [U] (each named in its page's last section)
- The real baseline at AUTO; the split of GPU time between work and waiting; SGSR's cost at the panel's size; what loads the GPU
  outside our command buffers (R1, R2, R10).
- How "parse" splits between constant blocks, texture uploads and the content feed; where the other ~13 ms of bind goes (R3).
- The range and order tables of the render walk; the categories of the crowd, particle, sweat, pre-light and UI nodes; whether
  the colour pass reads what the shadow walker leaves behind (R4).
- What the unnamed threads are; the real clock on the release build; whether the Intel driver spins in the fence wait (R5).
- Whether the draw op supports instanced indexed draws; why 239 of 740 frames fell back to the stream in the cuts-off match (R6).
- Whether a skipped rim map shows in the new renderer; change rates in a moving match (R7).
- Whether the release still has flat guest memory and any profile-guided part; Surface timings for AUTO builds with AVX2 (R8).
- Everything about the black title card until one affected player's log arrives (R9).
- Whether the replay player runs on the Surface at all (R10); whether the pixel-count query exists on these drivers (R11).

## 13. For Jarrod to answer
Nothing open. True 720p was answered (decision 31): the GPU lanes aim at true 720p first; when they fall short of 60 in Bryan's
entrance, AUTO may render at a lower internal resolution and upscale. That change posts a sheet (the picture at the lower
resolution beside 720p) and its measured row; it stays on the session branch until he has seen it at the release.

## 14. Rules to write into CLAUDE.md and DECISIONS (on decisions 11-13, 17, 18)
1. Rule 7, Surface runs: for the renderer arc part 2, Surface lab jobs and full benches are standing lanes in three tiers
   (replaces "Timed runs (Surface, T3) only at milestones" and "one batched Surface A/B per round" for this plan).
2. A build handed to Jarrod has the session's work switched on, with one line saying what is on.
3. More sheets, never a stop: every check and look change posts a sheet; work never waits on it.
4. The only reasons to wait on Jarrod: releases and pushes, player defaults, runs beyond the plan's grant, deletions outside the
   tidy's rules.
5. A bench states its settings, its tier and its thermal state, or it is not a number.
