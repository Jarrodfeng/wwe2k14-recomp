# The HD 520 arc: locked 60 fps at true 720p

Every optimisation so far has been a patch on a frame we don't own. The game's D3D library writes Xbox GPU packets,
Xenia's command processor reads them back, and NATIVE works out what the game meant, so it's exact only when it can
prove it. This arc is the transformative version: several sessions with one destination. My stance is "right or
better, not exact" for pictures; gameplay stays exact.

This file is the brief for the whole arc. Read it at the start of every arc session, then write that session's own
plan as `SESSIONNN_PLAN.md`. Keep a short progress log at the bottom of this file. **How to get there is yours to
decide.** Earlier plans hold ideas (`NATIVE2_PLAN.md`, `docs\native\UNLEASHED_BACKEND.md`,
`docs\research\perf_fundamentals_s60.md`, `docs\native\PASS_GAPS_S72.md`); use whatever helps, none of it binds you.

The arc starts once Session 77's branches are finished and merged.

## The target

- **Machine:** an Intel HD 520 laptop: Skylake, 6th-gen Core i3/i5/i7 U-series, 2 cores / 4 threads, 15 W, Intel's
  legacy graphics driver.
- **Picture:** true 1280x720. No upscaling, and no dynamic resolution counting toward the target.
- **Locked:** no frame over 16.7 ms, in entrances and in matches. Load screens and the first frame of a scene cut
  don't count. It has to hold once the machine is warm, not only in the first minutes.
- **First launch:** after installing, a game update or a driver update, all shader preparation finishes in **under 30
  seconds on the HD 520**. "Cold" means the driver's own cache is cold too, not just ours. After that, nothing compiles
  inside a frame: locked means locked from the first match.
- **Milestone 1:** 1v1 matches and entrances. **Milestone 2:** 6-man, Royal Rumble and Elimination Chamber.
- **Gameplay unchanged:** replays deterministic, and phone = PC on the Session 77 comparer. Flag any divergence; never
  hide one.

## Decisions I've made

| Question | Answer |
|---|---|
| Architecture | Our own implementation of the game's D3D calls is the core of the arc. The command processor stays only as a checker and fallback. Session 77's comparer is the determinism gate (Session 51's detached mode was parked because replays diverged, and we couldn't see why). |
| Look levers | Allowed if they pass my eye: half-resolution post effects, a simpler distant crowd, smaller shadow maps, half-precision shaders. Anything else that changes the look: ask me. |
| Intel bench | My Surface Pro 6 (Intel UHD 620, the same GPU family as the HD 520 but a little faster; an 8th-gen quad-core; 3:2 screen; the i5 is fanless as far as I know). **Used only in sessions I'm present for, and not until the native frame stands** (What I want, item 3). Until then, everything is built and checked on this PC. In those sessions it's reached over SSH, and by me for feel checks. It's quicker than an HD 520, so work out how much stricter its target must be. |
| The tester | Their HD 520 gets test builds at milestones. It's the final judge of the target. |
| Priorities | The arc comes first. Only crashes and can't-play bugs interrupt it. |
| Releases | This arc is **v0.3**. No release at the end of a session. The tester gets v0.3 preview builds at milestones; v0.3.0 goes public only when I say. Meanwhile I keep shipping small v0.2.x updates from other sessions. Those releases ship with the arc's switch off and must behave exactly as before. |
| Merging | Arc work lands on main as it goes, behind its own switch, off by default. That's a standing OK: no need to ask each time. **A merge carries arc work only,** never anything from my side sessions or unrelated changes. If a merge would include anything else, stop and ask. Switching it on, new defaults and releases wait for my word. |
| Intel multiply rule | Open again: the Surface is our first Intel hardware. In the first Surface session, find out whether the black eyes happen on it and what the exact rule costs, then bring me the choice. |
| Phone | Checked at milestones: no slower, and phone = PC on the comparer. Phone runs need me there, so these checks happen in attended sessions. |
| Attendance | Implementation runs unattended. Surface and phone testing only happen while I'm present, starting once the native frame stands. Eye checks and decisions are batched for when I'm back. **Updated 2026-11-03 ~02:10 (Jarrod): "Surface testing can happen unattended it's plugged in"** - the Surface (plugged in, logged in) may run unattended; phone runs still need him. |

## What we know

- **The tester** (who called it a "UHD 520"; the PLAYBOOK says HD 520), on v0.2.6 in 720p 30 fps mode: menus 60,
  entrances 20-25 fps, 6-man matches a solid 30, capped, so the match's real cost is unknown. Their black eyes
  (v0.2.12) led to `src\intel_exact_mul.h`: on Intel, NATIVE keeps the exact 360 multiply rule. Its GPU cost on Intel
  hasn't been measured. Unknown: whether their laptop's RAM is single- or dual-channel (I'll ask them).
- **On iGPUs the GPU was 92-99% busy** in testers' logs. On the 4090 the frame is a CPU chain: the game's threads,
  the render thread, then the command thread.
- **Session 72 (4090):** copies and barriers between passes were 34-46% of NATIVE's GPU time, post effects 9-12%,
  63-99 passes a frame. The match's remaining overhead needs look-ahead. Bryan's entrance renders a glare at zero
  weight, and skipping its shader saved nothing because the passes around it still ran.
- **Bryan's entrance** draws about 4,800 things a frame, about 3,246 of them crowd members.
- **Session 51:** the state log captured every packet the D3D library writes, with 0 differences from the register
  file, for 0.074 ms a frame. Detached mode drew 134 of 143 windows from it, but determinism diverged (write-pointer
  jumps, a shifted frame moment).
- **Session 77:** the determinism scheduler drains the command processor and the XMA decoder before every decision,
  and there are about 43 hand-offs a frame. In determinism mode the command thread's load is mostly spinning. In
  normal play, low power (on by default) already stops the spin, so what's left there is real work.
- **Still open from Session 73:** a 70-170 ms frame at every scene start, warm cache too.
- **This target has been set before:** NATIVE 2 (25 October) aimed at "iGPU 720p 60", kept the packet stream, put
  exact items first, and lost the room to other work after one session. This arc exists so that doesn't happen again.

## What I want

1. **Start with the best picture this PC can give.** Before building: an estimate of the HD 520's 16.7 ms budget from
   what this PC and the testers' logs can tell you, plus the tester's numbers if a run has come in. Where the CPU time
   is likely to go at 2 cores / 4 threads, where the GPU time goes, and which features the HD 520's driver has. It
   guides what to build first. It's not the verdict; that comes from the Surface in item 3.
2. **The native frame:** the game's D3D calls into our own layer, with the command processor kept only to check and
   fall back. Done when 1v1 matches and both entrances run through it with the comparer showing MATCH on the PC; the
   phone half of that check happens in item 3's session. **A firm requirement:** the native frame gives the game
   identical answers whether or not it draws anything. Online play's rollback will re-simulate frames without drawing
   them, so this is built in from the start, not added later.
3. **The first Surface session, with me there,** once the native frame stands:
   - set up the bench together, and work out how much stricter its target must be than the HD 520's;
   - measure the real budget, the cold first launch, and the Intel multiply rule;
   - do the phone check from item 2.

   If the gap looks bigger than everything available can close, tell me then, not at the end. From here on, the
   Surface's numbers set the order.
4. **Milestone 1:** HD 520 locked at 60, true 720p, in 1v1 matches and entrances, confirmed on the tester's
   machine.
5. **Milestone 2:** the same in 6-man, Royal Rumble and Elimination Chamber.

Everything between those points (GPU, CPU, shaders, the crowd, hitches, pacing) is your call, in whatever order the
numbers say. Say what's slowest first and why it's next.

## Testing: lean

I've watched sessions spend more time proving things than building them. For this arc:

- **The target is checked where it's real:** on the Surface, in sessions I'm present for, and on the tester's HD 520
  at milestones. Numbers from this PC guide the work, but they never count as meeting the target.
- **Benchmarks are rationed, not run after every change.** A timed run is exclusive and plays in real time, so every
  one blocks everything else on the PC behind the run lock. Measure performance when a piece of work is finished, a
  batch of changes at once, not after each change.
- **The Surface is its own machine.** Its benchmarks don't take this PC's run lock, so it can bench while the PC keeps
  building.
- **One check per change,** the cheapest one that would catch it being wrong: usually a fast-forwarded, untimed run
  that shares the lanes. A pass is a pass: don't re-run it to be sure. If a number is truly in doubt (noise), run it
  once more, not five times.
- **Determinism:** while you build the native frame, the comparer is the main tool; use it as much as that work
  needs. Outside that, it's a milestone check, plus any change that alters what the game's CPU reads.
- **Pictures:** not byte-identical. My eye at review points: a few same-frame shots or a short clip per scene, not
  sweeps.
- **No sweeps, no robustness matrices, and no tools that add more checking.** A tool that makes an existing check
  faster (one that skips the boot, the run lock or real-time play) is welcome if it pays for itself within the arc.
- If checking is taking longer than building, stop and tell me.

## How the arc runs

- **Don't stop at the first win.** A session keeps going until its goal is met or the context runs out, then hands
  over cleanly.
- **Architecture problems come to me.** The "three fixes, then move on" rule still holds for ordinary bugs, but a
  problem in the core design gets raised with me, not parked.
- **Effort:** I set it. High by default. When you reach the design of how the native frame answers the game's GPU
  questions (fences, interrupts, vblank: where Session 51 failed), tell me and I'll raise it to Extra high. Same if
  anything stalls.
- **Agents** only with a concrete goal each. A reviewer on big default changes is welcome.
- **The Surface's one-time setup is mine:** turning on its SSH server, and copying my own game files onto it. If I
  haven't done it before item 3, we'll do it together then. Benchmark runs plugged in, on best-performance power mode.
- **The Surface is mine for this project.** Family borrow it now and then: if someone happens to be on it, wait.

## Hard rules

- **Nothing from the game is committed or shipped,** including compiled pipelines, saved caches and optimised
  shaders: they're built on each player's machine from their own files. Replay recordings and hash-only reference
  streams are the exception, as before.
- CLAUDE.md as always: run lock, frozen user data, a test settings file, never my real save. No pushes, releases or
  new defaults without my word.
- **Downloads and installs: ask me first, and do ask when one would pay for itself.** Already worth asking about:
  - PresentMon on the Surface: what actually reached the screen, frame by frame, independent of the game's own
    counters.
  - Intel VTune on the Surface: the CPU at 2 cores / 4 threads, on real Intel hardware counters.
  - The Windows Performance Toolkit (GPUView): for pacing hitches the other tools can't explain.
  - Mesa's Intel shader compiler with its Skylake stub, in a Linux container: offline shader statistics for this GPU
    family. It's a different compiler from the Windows driver's, so it's only good for comparing versions of a shader.

  Leave the Surface's graphics driver as it is, unless the numbers point at the driver.

## Each session's summary

- Before the first Surface session: where the PC-side estimate says each scene stands, and what changed it.
- From then on: where each scene stands against 16.7 ms on the Surface (and on the tester's machine if a run came in),
  compared with the first Surface session, plus first-launch shader preparation, fully cold, against the 30 seconds.
- What changed, and what's on by default now.
- Anything waiting for my eye: the switch, what to look at, where the shots are.
- What's slowest now, and what's next.

---

## Jarrod's overnight decisions (2026-10-31 ~23:20, interview before an 8-hour unattended run)

"I want you to work on as much implementation as you can, research what you know about Intel before deferring me to
try the surface ... under no condition should you stop, the goal is as native as we can and I believe you can hit
Milestone 1, or at least your closest estimation to it overnight." Answers:
- **Look levers** (half-res post, simpler distant crowd, smaller shadow maps, 16-bit shaders): build them, each off by
  default behind its own flag, with same-frame before / after shots for his morning check.
- **Defaults:** new exact sub-features (byte-identical pictures, replay MATCH) default on inside the arc switch.
- **Research:** web research only tonight (no Mesa / tool installs).
- **Exactness:** "Allow if invisible" - if same-frame shots show no visible difference, a change may be turned on
  under the arc switch.

## Progress log (append: date, time, what)

- 2026-10-31 - brief written. Nothing built.
- 2026-10-31 20:26 - brief saved to the repo as `HD520_ARC.md`. Session 77 is merged (main d3817fe; SDK
  `wwe2k14-local` 4eacaff), so the arc can start. First arc session: Session 78 (`SESSION78_PLAN.md`), item 1.
- 2026-10-31 20:48 - item 1 done: `docs\native\HD520_ESTIMATE.md`. HD 520 GPU (estimate): match ~43 ms today, ~32 ms
  with no EDRAM; Bryan's entrance ~55 ms average, ~85 ms heaviest pan. CPU chain ~8-10 ms match, ~11-14 ms entrance.
  About half of the HD 520's entrance frame is EDRAM work (restores, copies, interlocked clears / blends) that the
  native frame removes. Runs 4970-4972 + the tester's zip rerun on this PC.
- 2026-10-31 20:57 - **estimate revised:** the 20:48 version took the interlock frame as "today"; players' NATIVE
  plays truth frames (no EDRAM), as the tester's normal play did. On that basis, HD 520 GPU: match ~20-30 ms,
  Bryan's entrance ~35-45 ms (heaviest ~50-65). This PC's later GPU gains (0.4x on the 4090) were mostly idle time
  between passes and barely reached the HD 520 (the tester's v0.2.6 report). The native frame's GPU value is the
  whole frame in hand (exact look-ahead); its direct gain is the CPU.
- 2026-10-31 21:00 - item 2 outline: `docs\native\NATIVE_FRAME_DESIGN.md` (half A, drawing, first; half B, the
  game's GPU answers, waits for Jarrod at Extra high) and `NATIVE_FRAME_INPUTS.md`.
- 2026-10-31 21:15-21:45 - Jarrod: Extra high, design it alone. **Native frame step A built and merged, off by
  default** (`--gpu_native_frame=true`; SDK `wwe2k14-local` 6fd63c0): the command processor answers the game as its
  no-draw mode does, a drawing side runs the same commands behind it on its own register state and catches up before
  every guest-visible point. **Item 2's PC check passes for step A:** stage A replay and both entrances MATCH the old
  path in every layer including the scheduler, 12 same-frame screenshots byte-identical. Costs +13-17% command thread
  for now; NF-B / NF-C (drawing by library call, lean recorder) are next, profile in the design doc section 3c.
- 2026-10-31 22:00 - Session 79 (same conversation): NF-C step 1 merged - the answers side stores only control
  registers (census: the game never reads render state or constants back). Replays MATCH; the native frame's command
  thread overhead about halved (Bryan 4.12 -> 3.79 ms vs 3.60 on the old path). Next: per-draw dirty mask.
- 2026-10-31 23:13-23:48 - **dead star glare merged** (look-ahead through the game's library calls: the combine's
  glare strength known before the glare passes; draws and copies skipped when it is 0; exact: shots byte-identical,
  0 differences against the register; Bryan GPU -4% on the 4090; entrance replay MATCH). 23:20 Jarrod's overnight
  decisions recorded above. 23:46 **the arc switch `--wwe2k14_native_frame`** (native frame + its exact parts; off).
- 2026-10-31 23:55-00:08 - Intel research (`docs\native\INTEL_GEN9.md`; web only) and our frame against it; crowd
  thin look lever merged off (review sheets for Jarrod); Surface session kit (`tools\surface\bench.ps1`, dry run
  passed); **Milestone 1 estimate (`HD520_ESTIMATE.md` 8b): not reached - GPU ~1.3-2x over in matches, 2-3x in
  entrances.**
- 2026-11-01 00:24-00:38 - three exact Session 52 / 56 flags under the arc switch (real depth clears, state dedupe,
  covered fills); store / alias census: stores are real work; store dispatch merge tried and dropped (2-3 of ~200).
- 2026-11-01 00:45-01:04 - Surface bench `-Only levers` (each lever on its own); **reflection cube faces filled by
  their resolves** (5 cubes x 6 faces in Bryan's entrance; the 3 sampled cubes no longer reloaded each frame;
  shots byte-identical; on under the arc switch). Found: 2 of the 5 cubes are rendered but never sampled (cheap: one
  draw each).
- 2026-11-01 01:13-02:12 - cube face copies batched; **alias loads straight from the owner images: store dispatches
  214 -> 19 a frame (Bryan), 208 -> 17 (match)**, ~17 compute blocks a frame -> ~2 (on Gen9 each a pipeline flush),
  4090 GPU -2 / -3 / -7% (Brock / Bryan / match); memory-writing copies batched (-11 barrier pairs a frame in Bryan's
  entrance). All exact (shots byte-identical; resolve hashes where they can check), all under the arc switch. The
  Surface bench's `-Only levers` can now switch each of them off on Intel.
- 2026-11-01 02:22-03:16 - checks of the whole switch: a second scene (Cena vs Undertaker, NATIVE 720p) 12 / 12 shots
  identical and the replay comparer MATCH in every layer; an independent review of tonight's SDK changes found 4 bugs
  in rare orderings (fixed, re-checked); **Vulkan validation on the whole replay route: 0 errors with the switch on and
  off** (a positive control with wrong layouts gives 24, so the layer checks). Found outside the arc: Session 71's
  readback fix costs every player about a second at each entrance start; a narrower rule is built off for Jarrod
  (`--wwe2k14_readback_skip_recurring_narrow=4`: back to a 4-frame hitch, the Threads outfit still right). Look lever
  added for the Surface: texture filtering (players' forced 4x anisotropic vs the game's own / none).
- 2026-11-01 03:30-04:05 - the entrances replay with the whole switch: 14 / 14 shots identical, MATCH over 18,660
  frames. Every thread's CPU at 60 fps measured (all threads ~15-17 ms a frame on this PC; on the i3 ~45-50 of ~67:
  under, tight); **the switch now sets the vblank spin to 250 us** (VSync thread 1.0 -> 0.2-0.3 ms a frame, pacing
  the same, stage A MATCH). "Sample instead of copy" checked against Bryan's frame and dropped (no copy qualifies);
  4 unread full-size copies a frame in Bryan's entrance found (look-ahead lead; none in the match). Surface bench
  `probes` step added.
- 2026-11-01 04:10-04:50 - readback gap rule built off with the narrow rule (the reflection cubes' faces were most of
  what was still read back: per window 3,693 -> 423; Jarrod's call, both flags together); Surface bench records every
  thread's CPU and has a pinned 2-core / 4-thread step; **depth copies batched under the arc switch** (4 + 4 a frame
  in Bryan's entrance no longer run alone; shots identical, validation 0 errors, both replays MATCH).
- 2026-11-01 04:56-05:10 - **the small full copies batched under the arc switch too**: every direct copy is now in a
  batch (barrier pairs from copies 42 -> 38 a frame in Bryan's entrance, ~33 -> ~29 in the match); same checks.
- 2026-11-01 05:18-05:40 - **the last stores gone under the arc switch**: RGBA16 owners read straight by the alias load
  (store dispatches 19 -> 2 a frame in Bryan's entrance, 17 -> 0 in the match); shots identical, 500 / 500 resolve
  hashes, validation 0 errors, both replays MATCH.
- 2026-11-01 05:50-06:10 - the copy batch no longer flushed before every direct alias read (only for a renamed owner):
  batches 38 -> 33 / 26.7 -> 24 a frame, same checks; full regress 05:50: all pass but the lesnar bench's n45 (the
  known Session 71 readback stall, the narrow rule waiting for Jarrod).
- 2026-11-01 06:10-06:38 - an independent review of steps 57-61 found one bug (a deferred small full copy could read a
  renamed image outside its rectangle, rare ordering) and one hazard (the lazy flush and a renamed own image): fixed
  and re-checked (shots, hashes, validation, both replays), step 62. It also found the same gap in batched fills since
  Session 59 (players' path): closed behind `gpu_native_truth_fill_rename_check`, on under the arc switch.
- 2026-11-01 09:20 - Session 80 (Jarrod here): his calls - the readback rules, the fill safety fix, the thinner distant
  crowd and the game's own texture filtering are everyone's defaults (not only the arc switch's). Replays equal in
  every game layer, Lesnar bench and T2 PASS (`SESSION80_SUMMARY.md`). Next: the first Surface session (item 3).
- 2026-11-01 22:55 - Session 80 (Jarrod here), the own-renderer work (`OWN_RENDERER_PLAN.md`): own crowd, all 11
  wrestler materials, and under the arc switch the frame structure's exact parts - texture-only depth copies (step 73),
  dead copies (step 74), the dead pass (step 75; Surface in-run Bryan -13.9% / -2.1% for the last two). Found on the
  way: the color_none bug (one-draw passes got STORE_OP_NONE; likely the phone's white patches - unconfirmed). Next:
  owning the match's scene-copy readers (combine, downsample, arena screen); SDK branch `s80-carry` (unmerged).
- 2026-11-01 23:45 - Session 80: the match's two colour scene copies served from the scene image (exact rows, the
  swap in the view, live and own-target sources; SDK `s80-carry` 7cf7129, review then merge under the arc switch).
  Exact, validation clean; Surface in-run match 23.74 -> 22.15 ms (-6.7%). Next: merge, then arena ownership.
- 2026-11-02 00:15 - merged: SDK step 76 (`wwe2k14-local` 83df32f), on under the arc switch, after an Opus review's
  fixes. Next: arena ownership.
- 2026-11-02 03:54 - every pixel shader of the match and both entrances ours (68 + the 2 combines; SDK steps 77-78);
  Surface in-run (run 032, clean): match -14.6%, Bryan -17.0%, Brock -19.2%.
- 2026-11-02 04:45 - **v0.2.15 released with the whole arc on by default** (Jarrod: "Whole arc on"; "not 0.3 yet, not
  til our goal is hit"): where NATIVE's truth frames run, the native frame + its exact parts + our shaders + our crowd.
  PC only; Android after phone tests. The milestone (720p 60 on the HD 520) is not reached yet.
- 2026-11-02 04:50 - 07:05 - overnight (unattended), frame structure: the scene's clears begun by the render pass
  (SDK step 79) and draws narrowed to their vertices' box (SDK step 81: the sweat maps' pass alias-loads 8 tiles
  instead of 416 - 0.85-0.90 ms of every scene's Surface frame before; CPU 0.2 ms a frame on the PC with step 83) and the
  arena screen's image made readable before the scene pass (step 82: the match's scene pass no longer split once a
  frame), all on under the arc switch, exact on the PC (shots byte-identical, validation 0, Opus reviews). The Surface
  was offline from 04:59: no Intel numbers yet (route `s80_tonight_multi.txt`: all on / all off / each off alone).
  Measured 09:37-09:44 (Surface run 033), within each frame-time state: match -6.1% / -5.0%, Bryan -7.1% / -6.9%,
  Brock -4.5% / -4.4%.
- 2026-11-02 09:20 - the shadows planned (Jarrod: "less ownership than I'd have imagined"; interview ~09:30-09:42):
  the shadow maps + the ring's plane shadow (match 4.5, Bryan 3.2, Brock 4.0 ms of the Surface's passes) as one owned
  system, the 360 look first (`OWN_RENDERER_PLAN.md` "The shadows").
- 2026-11-02 10:22 - the Surface's slow frames were the desktop compositor (the taskbar over the bench's game);
  `bench.ps1 -Front`: all on, match 18.82 -> 16.06 ms GPU a frame (every sampled match frame under 16.7 ms).
- 2026-11-02 12:15 - owned steps merged (SDK step 84) and on under the arc switch: the ring shadow's downscale + two
  blurs drawn into our own images, their copies gone (exact; the Surface's ring chain 1.08 -> 0.83 ms).
