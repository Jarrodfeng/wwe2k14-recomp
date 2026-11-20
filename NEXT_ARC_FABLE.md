# The next arc: hold the floor, then build on it

Written 2026-11-19 23:06-23:12 by the planning session (Fable 5.2), with the audit `docs\research\FABLE_AUDIT_S103.md` (cited here as
"audit A4", "audit B2" and so on). **A proposal. Nothing here is built, and nothing starts without Jarrod.**
**Expires** when step 3's bar is met, or when Jarrod replaces it.

Marks: **[C]** read in the file named. **[L]** my reasoning. **[U]** unknown, with the probe that settles it.

---

## 1. The goal, in one paragraph
v0.4.0 put a renderer with no fallback in players' hands. It was drawn on 10 of the game's 49 match arenas and 2 of its 46 30 Years
matches before it shipped (audit B1). **First make that floor one we can stand on. Then build the first piece of what the project is
for on top of it:** a match recorded once and replayed from any camera.

## 2. The order, and why
| # | Step | Why here |
|---|---|---|
| 0 | **Measure and arm** (hour 1) | One long Surface run closes or reopens the HD 520 goal: the plan's bar was *sustained* 60 and only bursts were measured (audit A2). The tripwire goes in before any other run, so every later run is read by it |
| 1 | **Coverage to a known list**, then v0.4.1 | Anything the new renderer does not draw is a hole on a player's screen. Every later step sits on this. The tools it builds are the ones that will tell us a recorded match is whole |
| 2 | **Wide and tall screens** | The one regression that touches Jarrod's own phone and every ultrawide player. It is also the camera's projection: a free camera with its own field of view is the same code |
| 3 | **Theater v0: record a match, replay it from any camera** | The sandbox's base. `docs\IDEAS.md` 296 asked "do the records carry everything a frame needs to be drawn without the stream". v0.4.0 answers it: the player's game draws from the record alone |

**Not in this arc** (section 9 says why): DLSS / DLAA / FSR, the multi-threaded recorder, the smarter look levers, a timeline UI.

## 3. The steps, their bars and their run budget (CLAUDE.md rule 7)
Builders and reviewers: 0 game runs, 0 Surface runs. Probes: 0 runs. A step's check is one uncapped run (or one batch) over the scenes
it touches, read by the tripwire and a sheet.

### Step 0: measure and arm
- **0a. The long Surface run.** The released v0.4.0 exe, Bryan's entrance and a match, AUTO LOW 720p, the player's default settings, 60 Hz,
  run until the heat is steady, read with the burst-aware bar (`tools\surface\bar_report.py`). **Bar: 60 shown, sustained.** The row goes
  into `docs\renderer\BUDGET_LEDGER.csv` (it has no Session 103 row).
  - If it holds: the HD 520 goal is closed. Say so in STATUS.
  - If it does not: one symbols profile, and the result names the wall. A speed step is planned from that, not guessed here.
- **0b. The tripwire** (lane T). A run's verdict is FAIL when a "not drawn" counter is above zero and not on a committed accepted list
  (each entry with its reason): 2D hole sites, 2D frames dropped outside photo mode, a body dropped while its slot is occupied, pictures
  refused, read-backs unserved, particle textures unnamed, a named load that failed. **Bar: over the logs that exist it gives FAIL on
  042, 046, 047, 048 and 049 (`hole 0x82BC799C`) and names the site.** `run_list.ps1` prints the verdict.
- **0c. One frozen player-settings file** for check routes (the launcher's defaults: vintage look on, AUTO tier), passed with
  `--wwe2k14_settings_file`. Check runs then draw what a player's game draws (audit B3, point 6).

| | Game runs (PC) | Surface jobs |
|---|---|---|
| Step 0 | 0 | 1 long run; +1 profile only if under 60 |

### Step 1: coverage to a known list
**Bar: each of these has one of three marks, and the list is generated, never written by hand.**
(a) proven offline by the census; (b) one sweep start with a tripwire PASS and a sheet a shot-checker read like a player; (c) a line in
`KNOWN_ISSUES.md` with its reason.
- every match arena (49); every 30 Years match (46); each match-type family; every 2D draw site (36; today 12 are known holes and 14 are
  unread, audit B4); every movie file; the create modes; photo mode and CineCam.

**How, in order:**
1. **The census lanes** (section 5), all 0 runs. Each lists every distinct value that reaches one of our draw decisions, marks it proven
   or never drawn, and draws one offline frame per unproven value with a known colour as input (the way TRN4 settled the black screens).
2. **The no-walk audit:** every place the stream-free path refuses, holds or drops something, what the player sees, and the rule.
3. **Gaps** go probe -> adversary -> builder -> review on the first commit -> one cheap gate -> merge, as in Session 103.
4. **One sweep** for what the census cannot prove offline. Its list is shown to Jarrod before it starts.
5. **One soak** of the final build (rule 7: a soak only on a final build): ten matches back to back across arenas, memory lines read.
6. `STREAMFREE_COVERAGE.md` is rebuilt by a tool from the census and sweep results.

| | Game runs (PC) | Surface jobs |
|---|---|---|
| The sweep | about 40 starts expected; the ceiling is about 95 (39 arenas + 43 matches + 13 match-type families). **Jarrod's OK first, with the list** | 0 |
| Step checks | one per landed group, about 6 each, about 3 groups; + 2 per real failure | 0 |
| The soak | 1 long run | 1 long run |

### Step 2: wide and tall screens on the new renderer
- **Probes first** (each one question, settled from code):
  1. Where our camera's projection is made, and every reader of the screen's shape: shadow fit, the stage visibility tree, crowd culling,
     depth of field and post sizes, soft particles, the 2D layer's placement.
  2. What v0.3.2's fill did (the widening rule, where 2D was anchored, the red-patch fix) and which parts only existed because of the stream.
  3. What the *game* still leaves out for its own 16:9 view before our renderer sees it (objects it never submits at the edges). [U]
- **Bar:** at 21:9, 32:9, 16:10, 4:3 and the phone's shape the 3D fills the screen with nothing popping in at the new edges; the 2D is
  not stretched and sits where v0.3.2 put it; the 16:9 picture is unchanged within `owned_check`'s tolerance.
- **Offline proof:** rig frames of a kept case at each shape. The 16:9 crop of a wide frame equals the 16:9 frame within tolerance; the
  new edge columns are not empty.

| | Game runs (PC) | Surface jobs |
|---|---|---|
| Step 2 | one check, about 6 (an entrance and a match at three shapes), + 2 per real failure. Jarrod's phone is the judge of the tall shape | 0 |

### Step 3: theater v0
- **Probes first:**
  1. What a frame's record points to outside itself: movie pictures, 2D pictures, combined attires, the sweat and blood map, crowd card
     bakes, particle state, models by key. That list is what a recording must carry. [U]
  2. Bytes per frame and per minute of match, packed. [U]
  3. What a free camera breaks: anything the game or our renderer skipped because the game's own camera could not see it. [U]
- **Bar:** Bryan's entrance and 60 seconds of a match, recorded in one PC run, replay with no game running, at 60 or more on the PC:
  1. from the game's own camera, equal to the live picture within `owned_check`'s tolerance;
  2. from a free camera, with nothing missing on a sheet Jarrod judges.
  The size on disk is stated.
- **Scope guard:** no timeline UI, no editing, no sound. A dev-build tool first. It proves the base exists; whether and how players get
  it is Jarrod's call afterwards.
- **Hard rule 1:** a scene recording holds poses and pictures that come from the game. It is made on the player's machine from their
  own files. It is never committed and never shipped. (The existing `.rec` files, controller states only, are a different thing.)

| | Game runs (PC) | Surface jobs |
|---|---|---|
| Step 3 | 2 recording runs + one check, about 4, + 2 per real failure | 0 |

**Total, estimates:** about 70-85 PC runs if the sweep stays near 40, 2-3 Surface jobs. Over budget is said in the report and in STATUS,
and STATUS carries the session's run count taken from the logs (audit A4, drift 3).

## 4. The first day (the first session = steps 0 and 1)
1. `session_start.py`, STATUS, LESSONS. The Surface pre-flight. **Start the long Surface run (0a)**; it needs nobody.
2. **Read what testers sent** since the release. Each report becomes one settle probe from its own log.
3. **Start lanes T, N, C-SCR, C-2D (three), C-MOV, C-ARN, C-U, REL and G in the first hour** (section 5): eleven lanes, no file overlap,
   0 runs each. Show Jarrod lane T's acceptance bar before builders start.
4. When 0a returns: the ledger row, and one line in STATUS (closed, or the profile's wall).
5. As census lanes hand back: an adversary re-derives every row a build will rely on; gaps become builder lanes, ranked by audit B5.
6. **Write the sweep list** from what the census could not prove; show it to Jarrod; run it in batches of three, uncapped, on the frozen
   player settings. The tripwire and one sheet per start are the result.
7. One step check per landed group. Then the soak on the final build.
8. STATUS and the summary rewritten each round. The v0.4.1 question goes to Jarrod with: which checks passed, the coverage list's
   three counts (proven, swept, known issue), anything waived.

## 5. The lanes, and what each proves offline
Every lane carries a re-find of a real past fault (Jarrod: prove tools on known bugs). All 0 game runs.

| Lane | Who | What it makes | Its offline proof (the re-find) |
|---|---|---|---|
| **T** tripwire | builder | the run verdict and its accepted list | FAIL on logs 042, 046-049 for `0x82BC799C`; PASS on 037-039; a planted counter flips it |
| **N** no-walk audit | Opus probe + adversary | one table: every refuse / hold / drop site in the stream-free path, what the player sees, the rule | every row [C] with file and line; it must list the stale-pose drop that AUS2 found; the adversary re-derives the five most exposed |
| **C-SCR** screens | Opus probe, then a small tool | every arena x screen key 800-809 x material values; one rig frame per distinct value with solid colours | with the TRN4 fix switched off (its planted twin) the tool shows the 22 meshes black on arenas 30, 33, 35, 44, 50, 53, 54 |
| **C-2D** (three lanes) | Opus probes | for each of the 12 hole leaves and the 4 unread functions: who builds that object, so which screens draw it | from code alone it must name the season videos for `0x82BC799C` |
| **C-MOV** movies | Sonnet inventory | every `.bik2`: planes, alpha, size, which player opens it | it must list `season\0411.bik2` as a no-alpha movie |
| **C-ARN** arenas | Opus probe | the slot flags nobody reads (`b z k c x n`), the second UV set, arenas 78 / 80, the per-arena post table (which arenas turn tonemap and night on) | the effect table and one rig frame per distinct value; it must re-find the barricade's alpha class on arena 4 |
| **C-U** unknowns | Sonnet inventory | the `[U]` marks that sit on a draw decision in the stream-free path (428 marks in `src\` in all) | on the commit before RFX it must list the 2D drawer's sampler note |
| **REL** release path | builder | a gate lane that runs the release scripts' own checks, and puts their needs under the tidy | on the commits before each fix it re-finds release night's four: the SDK STEPS line, `check_build_flags`, the cleared entrance store (reported as blocked, not found at 22:10), the 422 |
| **G** gate verdicts | builder | FAIL (it ran and it is wrong) apart from BLOCKED (it could not run) | over `out\gate\times.csv` the 7,356 FAIL rows under 2 seconds move to BLOCKED; a planted real failure stays FAIL |
| **S** spine | main session | the Surface run, tester reports, the sweep list, integration, STATUS | - |

Later lanes, named now so they are not forgotten: created superstars in a match; cloth (`yCh_cloth` was never bound in any run,
`docs\agents\s103\COV.md` 28); Universe cutscenes; photo mode and CineCam; the created-content read-backs that are unserved by design.

## 6. Rules for this arc (each from a finding in the audit)
1. **A run is read by the tripwire before anyone reads its pictures.** (audit B2: five logs said it.)
2. **"COVERED" names its values.** A coverage row says which values were proven and how. A rule fitted on some files says which files. (B3)
3. **A wrong-pixel question on something the rig can draw starts with the rig**: known inputs, one switch at a time. At most one reading
   probe before an experiment. (C2: TRN took three reads, then 14 minutes.)
4. **A stalled run that is still alive is evidence.** Read its memory before closing it. (C1: WMS.)
5. **No per-frame mix of old and new.** A new path is judged whole against the old path whole: two builds, an A/B pair. (A4, drift 1.)
6. **A plan that removes something states the cost of what replaces it, as its own bar.** (A3)
7. **A unit enters through the hook or the public call**, never the inner function. Reviewers ask this first. (C2)
8. **The session's run count comes from the logs**, with the grants beside it. (A4, drift 3)
9. Carried from Session 103 unchanged: probe first; Opus settles; adversaries before builders; review on the first commit; one cheap gate
   per piece; the game's code is the source of truth; every game choice sorted look or 360 habit; no artificial load.

## 7. Risks, said plainly
1. **The census may find more than one session can fix.** The bar allows a `KNOWN_ISSUES.md` line. Rank by audit B5 and ship v0.4.1 with
   the top of the list fixed and the rest named.
2. **Tester reports will arrive mid-arc** and outrank everything. Each is one settle probe from its log. [U] whether the report zip
   carries the census lines the tripwire reads: lane T checks this first.
3. **The long Surface run may come in under 60.** Then one profile, then a plan. Nothing is pre-built for it.
4. **A free camera may show what the game never submits.** Step 3's third probe exists so this is designed, not discovered (the mistake
   audit A3 names).
5. **Vendors we have never seen** (AMD, other Intel, Mali, older Adreno). No lane can fix what no machine here shows. The tripwire's
   counters in the report zip are the best tool we have for them.

## 8. Decisions that are Jarrod's
1. The sweep's starts (the list comes first).
2. v0.4.1, asked right before publishing, as always.
3. UPSCALING still offers DLSS / DLAA / FSR and they do nothing (`KNOWN_ISSUES.md` 10). Hide them until they work?
4. Which settings the frozen player-settings file holds.
5. Whether theater v0 stays a dev tool at first.
6. Whether a sustained result under 60 reopens the HD 520 goal, or 60 in ordinary play is enough.

## 9. Not in this arc
- **DLSS / DLAA / FSR.** Speed no longer needs them: 239 fps at 4K on the PC, 60 at 720p on the Surface (`SESSION103_SUMMARY.md` 249, 259).
  They return as picture quality, once the renderer supplies its own motion vectors. A later step.
- **The multi-threaded recorder and the GPU tier** (`docs\IDEAS.md` 309-319). The PC is capped near 250 by our one host thread
  (summary 258). Nobody needs more today. When it is taken up it is a Fable plan backed by a Fable probe, as decided (`DECISIONS.md` 330).
- **The smarter look levers** (cached static shadows and the rest, `docs\IDEAS.md` 321-336). After the look matches everywhere.
- **A timeline, editing, sound in the theater.** After v0 proves the base.

## 10. Addendum, 2026-11-20 04:35: what the tools found on their first night, and what it changes here
The same night this plan was written, the tools the audit asked for were built (0 game runs) and two of them ran over all 49 match arenas.
State and evidence: `STATUS.md`; notes in `docs\research\harness\`.

1. **The audit's main claim held on the first run.** The whole-game diff (per mesh, the game's own draw rule against what our draw really
   picks) found two differences that every PC check had passed, both omissions nobody had compared against the game:
   - the game culls 10,465 arena meshes that we draw two-sided (`CUL.md`, `CUL_ADV.md`; contract `SCU_contract.md`);
   - the game writes depth on 1,339 see-through stage meshes and we do not, and the reverse on 7 (`ZWR.md`, `ZWR_ADV.md`, `ZW1.md`, `ZW2.md`).
   Step 1 ("coverage to a known list") therefore starts with these two fixes, each with one game check, in that order (same files). The
   stored pipeline list must be re-recorded before v0.4.1, or the Surface and the phone build the new state pairs on first use.
2. **"Whole game" is not whole yet.** The diff covers arena stage meshes, and partly screens and ring keys. Not compared: 890 ring rows our
   offline draw lacks, the shadow pass (2,354 rows), textures per slot, the crowd, characters (they take another cull path: not audited),
   props outside arenas, 2D. Each is a lane of the same shape: the game's rule from the code, ours from the real draw, one sheet.
3. **The offline arena sheet is a standing check.** Both looks of all 49 arenas with test-pattern screens take 165 s and re-find the
   titantron fault with the fix switched off. Every step that touches arena drawing ends with it, and its sheet goes to Jarrod.
4. **A new check is reviewed for the pass it should not give.** All three checks built that night could say PASS on a wrong result in
   their first version (the sheet's judge passed a screen at 3 % brightness; the diff's "equal" total held rows that differed or were never
   compared; the link lint missed real link errors twice). Each was caught only by an Opus review whose first question was "can this say
   PASS when the thing is wrong?" (`docs\LESSONS.md`, Pictures and checks). Budget one such review per new check; it is cheaper than a
   check that is believed.
5. **A probe is not the last word either.** Both settle probes were right about the game and incomplete about the fix: the adversaries found
   a flip test that would have turned mirrored stands inside out, and that depth writes need an alpha test on every see-through draw and a
   changed draw order. The probe-then-adversary pair stays as it is; a fix this wide does not start from one reader.
6. **Promote what probes rewrite.** Four probes each wrote their own script to read a draw's render state out of a stored capture, while a
   decoder sat in the repo with no q entry (`tools\particles\pcap.py`). Lane CDT makes it one tool with q entries. Every hand-back's
   "q could not answer" list is a lane candidate, read at each break.

## 11. Jarrod's answer to this proposal, 2026-11-20 11:5x (his words; nothing below is started)
"Don't start anything here, it's for a proper opus session to take on, I only wanted you to build workflow/harness tools here." The arc is
the next Opus session's; the planning session records and stops. What he said, and what it moves:

1. **Step 0a, the long Surface run:** "Not confident the surface is on but I've had multiple players report the performance improvements."
   The run needs the Surface reachable; it does not block the rest.
2. **Android heat, new and the biggest performance issue:** "The biggest issue is actually on android, where it seems even in the launcher,
   there's always one core at 100% usage, which I believe is the cause of heat. We have tons of headroom, but it seems phones are still having
   thermal issues." Reported, not measured: [U] which thread it is, whether it is the same thread in the launcher and in a match, and whether
   it is ours (a loop that never sleeps) or the system's. It was not in this plan; it belongs with step 0 (measure first), on the phone.
3. **Step 1, the coverage pass:** "Coverage pass is good, I have to imagine we missed some stuff." Approved as proposed; it now starts with
   the cull and depth fixes (section 10).
4. **Step 2, wide screens:** "Ultrawide should start working again already got a complaint about that." Confirmed, with a player waiting.
5. **A regression players report, not in this plan:** "their selected resolution in the launcher isn't working, picking 1440p or 4k and it
   not actually changing resolution visually in the game." Reported, not looked at: [U] whether the stream-free renderer reads the setting
   at all, or reads it and does not apply it. It is a v0.4.0 regression in players' hands, so it sits beside ultrawide ahead of theater.

[L] The order this suggests for the Opus session, to confirm with him at its start: the two things players are waiting on (the resolution
setting, ultrawide), the Android core that never rests, then the coverage pass with the two fixes, then theater v0.
