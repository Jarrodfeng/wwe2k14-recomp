# Big optimisation session: low-end PCs, general speed, and a native renderer feasibility pass

I'm at work today, so this runs unattended. Work in the main session and don't fan out into big agent swarms. Use
High effort, and step up to Extra high only if something goes wrong.

## Why now

The recomp got posted to the decomps subreddit without my knowledge, and the first low-end report came in:

- **Intel UHD 620 laptop, 720p:** 25–30 fps in the menus, **5–10 fps in entrances and matches.** A friend's
  RTX 4070 runs it smoothly.
- **FSR on that laptop:** the screen started shaking, and the game never got past the loading screen into the
  entrance or match, though the audio kept playing.
- The original post said "Please optimise this for low end pc and laptops, we all have been waiting for this." A lot
  of the people who love this game are on old laptops. I want them to be able to play it.

## What we know (from research this morning)

**The two render target paths (Xenia's own write-up, linked below)**
- Interlock (our `fsi`) does the 360's eDRAM work (blending, depth, formats) inside pixel shaders. It's the most
  accurate, but the shaders are very long, run under interlock, and can't use early depth rejection.
- Host render targets (`fbo`) use the GPU's own hardware and copy between targets when the game reuses eDRAM as a
  different format. Xenia measured it as the fastest path in all four games they tested on a GTX 1070, by 1.2× to
  3.4×.
- Its accuracy costs: the 360's 7e3 float colour and 24-bit float depth have no PC equivalent, so they're
  approximated. That can cause transparency and depth problems. Xenia added depth-conversion options that trade
  accuracy against early depth rejection.
- We moved to `fsi` early because it fixed depth problems, black flashes and pop-in. Session 20 measured the two
  paths at the same speed on the 4090, which tells us nothing about weak GPUs.

**Vendors**
- AMD's Windows Vulkan driver doesn't expose fragment shader interlock. AMD's stated stance was that it doesn't
  want to implement it. One unverified claim says it arrived in Adrenalin 24.12.1.
- Mesa's RADV driver (Linux, Steam Deck) has supported interlock since Mesa 23.2 (2023).
- **I don't know what our build does on an AMD Windows card today:** fall back, fail, or something worse. Find out.
- Intel moved its 6th–10th gen integrated GPUs (UHD 620 included) to legacy support in 2022, with security and
  critical fixes only. Driver bugs there won't be fixed, so fragile features need to fail safely.
- FSR 3.1's official support list starts at Radeon RX 5000 and GeForce RTX 20. Intel integrated graphics aren't
  listed.
- At a 720p render there's no smaller size to upscale from, so FSR can only add cost on a laptop like that.

**Laptops**
- On laptops, the CPU and the integrated GPU share one power and heat budget. Intel's own power work found that
  keeping the CPU in a lower power state can leave the integrated GPU more headroom. CPU threads that spin or
  busy-wait may be costing the GPU frame rate on 15 W machines.
- A UHD 620 is only about 1.7× the 360's GPU on paper, and it has nothing like the eDRAM. A steady 30 at 720p is the
  realistic goal there, not 60. The Steam Deck (about 4× a UHD 620) should be able to do 60.
- **My test machine for tonight:** my Gigabyte AERO 15 laptop has an Intel integrated GPU next to its RTX card. The
  GRAPHICS CARD picker can force the Intel chip. Its Intel GPU is a newer generation than the UHD 620, so treat its
  results as optimistic. This also finally tests the dual-GPU automatic pick (open since Session 31).

**Native renderers in the wild**
- **Skate 3 Recomp 2.0** (July 2026, built on ReXGlue's 0.8 line) replaced GPU emulation with a native D3D12 / Vulkan
  renderer and reports more than 2× the frame rate at about a quarter of the GPU power.
  - It hooks the game's own device paths (constant uploads, buffer bindings, render state), builds each frame's
    scene from guest memory, and hand-ports the game's material shaders, verified per pixel against the originals.
  - Menus, loading and the editors stay on the emulated renderer, there's a live toggle, and it falls back on
    failure.
  - The renderer is roughly 1.4 MB of C++.
- **Unleashed Recompiled** wrote a renderer from scratch that translates the game's draw calls to modern APIs, with
  shaders auto-converted by XenosRecomp. XenosRecomp is admittedly game-specific and relies on reflection data in
  the shader binaries.
- **The Call of Duty 3 port** (October 2026) uses a native D3D11 renderer with shaders translated to HLSL.
- **The Simpsons Game recomp** still uses Xenia-style GPU emulation and names the renderer as its bottleneck.
- **ReXGlue's developers** say they "fully intend to replace [the Xenia GPU backend] with native rendering over
  time". The graphics system is an abstract interface in the SDK.

## What I want

**1. Low-end PCs playable.**
- Aim for a steady, playable frame rate on integrated graphics: 30 at 720p on UHD 620-class chips, and as much as
  possible above that.
- A lighter rendering mode is the obvious candidate, with as many of its old glitches fixed as possible and the rest
  written down honestly.
- The first-run wizard should pick it on integrated graphics.
- Leave me a build to try on the laptop's Intel GPU tonight.

**2. FSR never breaks a game.**
- If it can't run properly on a card, the player gets a clean fallback and a message, never shaking or a stuck
  loading screen.
- It's only offered where it works and where it makes sense.

**3. Faster in general.** Anything that makes every PC faster or smoother is in scope:
- CPU overhead on 4-core laptops;
- power use on laptops;
- first-play stutter;
- frame pacing.

Nothing should look different on a high-end PC with the defaults.

**4. The AMD question answered:** what an AMD Windows card gets today, and what it should get.

**5. A native renderer feasibility pass.** I want a written report, not a renderer:
- which approach fits 2K14 (Unleashed-style draw-call level, Skate 3-style scene level, a hybrid by context such as
  matches native and menus emulated, or something else) and why;
- roughly how many distinct shaders or materials a match actually uses, and whether they could be converted
  automatically or would need hand-porting;
- whether the game always goes through its graphics library or ever writes GPU commands directly;
- what we already have that a native renderer would use: skinned draw tracking, bone matrices, the HUD split, motion
  vectors, the harness;
- what a first milestone would be, a size estimate in sessions, the biggest risks, and your recommendation.

A small proof of concept is welcome **only** if it's cheap and tells us something reading can't.

## How I want it run

- **Lean checks:** one per thing you promise. I'll play-test the low-end side on the laptop tonight.
- **New options start switched off,** and defaults wait for me. With everything off, the game should behave exactly
  as it does now.
- **No merges, pushes or releases without me.**
- If something stalls, write down where it stopped and move on to the next goal.
- **Summary:**
  - what changed, with numbers where you have them;
  - a short "try this tonight" list for the laptop (what to switch, what to look for);
  - the feasibility report;
  - the decisions waiting for me.

## Reading list

- Xenia: [Leaving No Pixel Behind (render target paths, depth conversion, vendor notes)](https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html)
- [Skate 3 Recomp (README, native renderer)](https://github.com/mchughalex/skate3recomp) and its
  [`src/skate3_native_scene.h`](https://raw.githubusercontent.com/mchughalex/skate3recomp/main/src/skate3_native_scene.h)
- [Skate 3 Recomp 2.0 announcement (GamingOnLinux)](https://www.gamingonlinux.com/2026/07/skate-3-recomp-2-0-brings-native-rendering-to-the-pc-port-of-the-xbox-classic-skate-3/)
- [Unleashed Recompiled README](https://cdn.jsdelivr.net/gh/hedge-dev/unleashedrecomp@main/README.md) and
  [XenosRecomp README](https://github.com/hedge-dev/XenosRecomp/blob/main/README.md)
- [ReXGlue interview (native rendering plans)](https://readonlymemo.com/rexglue-xbox-360-recompilation-interview/)
- [AMDVLK issue #108 (interlock on AMD)](https://github.com/GPUOpen-Drivers/AMDVLK/issues/108) and
  [RADV interlock (Phoronix)](https://www.phoronix.com/news/RADV-Adds-Frag-Shader-Interlock)
- [Intel CPU / integrated GPU power sharing (Phoronix)](https://www.phoronix.com/news/Intel-LPC-2020-Tuning-Pstate)
- [FSR 3.1 release notes (supported GPUs)](https://gpuopen.com/learn/amd_fsr_3_1_release/)
