# Starting the native renderer: a long-term side project

I want to properly lock in on a native renderer for 2K14: drawing the game ourselves instead of emulating the 360's
GPU. Session 37's feasibility report recommended "not now". That was a fair call for a rescue, but this isn't a
rescue. The light renderer and low power are handling the laptops for now. This is a long-term project I genuinely
want to try, because I want to see if we can do it.

This session's job is to **get it started properly**: settle the approach with evidence, set up the project, and draw
something real.

Use High effort, and step up to Extra high when something goes wrong. Keep the thinking in the main session, with no
big agent fan-outs. If I'm not around, never end a turn on a question: take the most reasonable reading, write it
down, and carry on.

## Why I want it

- **Performance where it matters:** laptops and integrated graphics, the Steam Deck, and eventually a Mac (I'd like
  an M5 Pro MacBook) and Android. Emulating the eDRAM is the cost that holds weak hardware back, and a renderer that
  understands the game doesn't have to pay it.
- **Everything we've built gets better:**
  - DLSS / FSR with exact motion vectors;
  - true ultrawide;
  - any render size, including below 720p for weak laptops;
  - photo mode and CineCam working with the scene directly.
- **It would be among the first native renderers for a 360 recomp,** and the first for a wrestling game.

## It lives on the side

- **A side branch, and an SDK branch if it needs one.** `main` and releases never wait on it. How it stays in step
  with `main` is your call.
- **The emulated renderer stays available the whole way:**
  - as the fallback when anything isn't handled yet;
  - as the reference to compare against;
  - and, ideally, switchable live, so I can flip between the two and see the difference.
- **The game's behaviour must not change.** Only how it's drawn. Replays and determinism still have to hold.

## What we already know

- **Session 37's report** (`docs\research\native_renderer_feasibility_s37.md`) is the starting point. Build on it,
  and say where this session's findings change it.
- **Skate 3 Recomp 2.0** (July 2026) is built on ReXGlue's 0.8 line and shipped exactly this. It reports more than
  2× the frame rate at about a quarter of the GPU power.
  - It hooks the game's own device paths, builds each frame's scene from guest memory, and hand-ports the game's
    material shaders, verified per pixel against the originals.
  - Menus, loading and the editors stay emulated. It has a live toggle and falls back on failure.
  - Its renderer is about 1.4 MB of C++.
  - Commenters called that code "a giant unreadable mess". **Ours shouldn't be.**
- **Unleashed Recompiled** wrote its renderer from scratch at the draw-call level, with shaders auto-converted by
  XenosRecomp. XenosRecomp is game-specific and relies on reflection data in the shader binaries.
- **The Call of Duty 3 port** uses a native D3D11 renderer with shaders translated to HLSL.
- **ReXGlue's developers** say they intend to replace the Xenia GPU backend with native rendering over time. The
  graphics system is an abstract interface in the SDK.
- **Our own head start:**
  - skinned-draw tracking, bone matrices and draw identification by shader (Sessions 24 and 27);
  - the HUD split and wrestler motion vectors (DLSS);
  - the light renderer work and its diagnostics (Session 37);
  - the harness: tracing, diffcov, hot-reload probes, the struct library, the memory scanner;
  - **deterministic replays with frame-locked capture.** These give us the same frame twice, emulated and native,
    which is the ideal way to measure parity.

## Questions I want answered (this session or as the project goes)

- **The approach for 2K14:** the draw-call level (Unleashed), the scene level (Skate 3), a hybrid split by context
  (for example, native matches with emulated menus), or something else. Which, and why, based on how *this* game
  actually draws.
- **How the game talks to the GPU:** does it always go through its graphics library, or does it ever write GPU
  commands directly?
- **What the game reads back from the GPU,** such as occlusion queries or copies the CPU reads, and what breaks if
  those answers change.
- **How the game waits on the GPU,** so its pacing and determinism survive.
- **Shaders:** how many distinct shaders or materials an entrance, a match and the menus use, and whether they can be
  translated automatically or need hand-porting.
- **The hard parts I suspect:**
  - the crowd (thousands of sprites);
  - the game's post-processing;
  - the trons and videos;
  - the 2D menus;
  - the creation suite.
  Where should each one sit on the road?
- **What a native renderer unlocks per platform,** and what else each platform would still need.

## The hard rule, applied to a renderer

**Nothing from the game is ever committed or shipped.** That includes shaders.
- If shaders are translated, they come from the player's own files on their machine, like the generated code does.
- If something has to be hand-written to match the game's look, **propose where the line is and check with me
  before committing any of it.** Learn from Skate 3 Recomp's approach, but don't copy its code unless its licence
  allows it and I've said yes.
- If the work suggests something ReXGlue itself should offer, such as a clean way to plug in a native renderer, note
  it for the upstream list. No contact with upstream projects without my OK.

## What I'd like out of this first session

1. **The approach, decided with evidence,** in a design doc that will keep growing with the project.
2. **The project set up on the side,** with the native renderer existing as a switchable alternative, even if it
   draws very little yet.
3. **The comparison loop working:** native vs emulated on the same replay frames, for both picture parity and cost.
4. **A first real milestone you can show me:** something recognisable from a match, drawn by our own renderer,
   side by side with the emulated frame. You choose what's most telling.
5. **The road ahead:**
   - milestones from here to "a whole match drawn natively";
   - what each one unlocks;
   - honest size estimates;
   - the biggest risks;
   - what this session learned that changes Session 37's estimate.

## How I want it run

- **Lean checks:** one per promise. Parity and cost numbers are the yardstick, not screenshot piles.
- **Write it so a human contributor could follow it.** This code will be public one day.
- **Nothing merged into `main`, pushed or released without me.**
- **Summary:**
  - side-by-side pictures of emulated vs native;
  - the numbers;
  - the decided approach;
  - the roadmap;
  - the decisions waiting for me.

## Reading list

- [Skate 3 Recomp README](https://github.com/mchughalex/skate3recomp) and
  [`src/skate3_native_scene.h`](https://raw.githubusercontent.com/mchughalex/skate3recomp/main/src/skate3_native_scene.h)
- [Skate 3 Recomp 2.0 announcement (GamingOnLinux)](https://www.gamingonlinux.com/2026/07/skate-3-recomp-2-0-brings-native-rendering-to-the-pc-port-of-the-xbox-classic-skate-3/)
- [Unleashed Recompiled README](https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md) and
  [XenosRecomp README](https://github.com/hedge-dev/XenosRecomp/blob/main/README.md)
- [ReXGlue interview (native rendering plans)](https://readonlymemo.com/rexglue-xbox-360-recompilation-interview/)
- [Xenia: Leaving No Pixel Behind (how the 360's eDRAM and render targets work)](https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html)
- [Call of Duty 3 native PC port (Digital Citizen)](https://www.digitalcitizen.life/call-of-duty-3-gets-unofficial-native-pc-port-with-4k-and-unlocked-frame-rates/)
