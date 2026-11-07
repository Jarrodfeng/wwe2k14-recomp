# Changelog

## v0.3.0

**v0.3.0: TRUE NATIVE grows up.**

When TRUE NATIVE became the default in v0.2.19, it was the biggest change this project had ever made: the game's drawing moved off the Xbox 360's way of working and onto a renderer I own from top to bottom. It was fast on my machines. Then it reached yours, and it landed with more bugs than I expected: invisible wrestlers, broken shapes, flickering crowd signs, black screens, stutter on some laptops, low menu frame rates on some phones. A lot of that work looked like it had not paid off yet.

This release is the payoff. Every one of your reports was read line by line. I went back through the whole graphics path, measured where the power and the time really go on weak machines, and rebuilt what was wrong at the root instead of patching around it. Along the way I found faults that had been hiding for a long time, including one that made every single frame stall. The result is the fastest and smoothest this game has ever run here, on PCs and on phones.

### Performance

- **Every frame now reaches the screen.** On weak GPUs about a quarter of the frames the game drew were thrown away before they reached you, so it looked worse than the fps counter claimed. Every drawn frame now arrives. This alone took Bryan's entrance on my test tablet from about 34 to about 44 shown fps.
- **A hidden stall in every frame is gone.** Each frame stopped, waited for the one before it, then rebuilt its final drawing step, because a value that should have remembered the screen's format was never written. Removing it took the worst moments of Bryan's entrance from about 24 to about 33 fps. This should also help the stutter reported on Intel Iris Xe, which I could not test.
- **The pyro stutter is gone.** Big pyro used to bring TRUE NATIVE to its knees. About 2,700 tiny particle draws a frame now merge into a few long ones, and those frames no longer stall the renderer.
- **Huge gains on weak machines.** My Surface Pro 6 (Intel UHD 620, no fan) is a harsh test: it throttles hard, and its CPU and GPU share about 14 watts once warm. Shown fps there, after a long warm-up:
  - Match: about 49 to **60**
  - Brock's entrance: about 45 to **56-58**
  - Bryan's entrance: about 36 to **49-52**
- **Phones fly.** On my phone (Snapdragon 8 Elite) at 1080p in 120FPS mode, entrances run at **80 to 100 fps** and the match is close to a **locked 120**. The smoke in Undertaker's entrance, which used to drag everything down, no longer does.
- **Less work everywhere.** Wrestlers are skinned once a frame instead of once per pass. Uploads are merged into far fewer GPU commands. The drawing thread does less per draw. The game's hottest maths and memory routines now run as native code. Guest memory on Windows no longer pays a check on every access, which also shrinks the game's code by more than a quarter.
- **Far fewer first-play stutters.** The list of shaders prepared in advance nearly doubled, built from a run across the whole game.

### TRUE NATIVE on phones

- **The picture comes back when you return to the game.** Pressing Home and coming back could leave you with a black picture, because Android replaces the game's window and the renderer never picked up the new one. Confirmed fixed on my phone.
- **Phones no longer depend on the 128 MiB graphics memory limit.** This is the structural fix I most wanted. Many phone GPUs can only see 128 MiB at once, which forced the game through a fragile workaround. Wrestlers, arenas and the crowd now draw from real vertex buffers, the way any ordinary mobile game does. This is the fix aimed at the broken shapes, spikes and invisible wrestlers on Adreno phones. It runs clean on my phone and on every phone GPU I can emulate, but I do not own the phones that reported those bugs, so it **should fix them, not confirmed yet**.
- **Crashes now name themselves.** A bug in the crash handler hid many phone crashes from their own logs. They are now recorded, so reports can say what went wrong.
- **QUIT TO DESKTOP no longer crashes on phones.**
- **Silent freezes are detected and logged,** and repeated failures offer a safe mode instead of a dead end.
- **Unknown GPUs start in a plain, safe mode,** so a phone I have never seen starts on the most proven path.
- **Low menu frame rates on Mali phones: should fix, not confirmed on that phone.** Menus could overflow a fixed buffer and take a slow path.

### Bugs you reported

- **Crowd signs no longer flicker.** One of your biggest complaints. The crowd density choice used a spectator's exact position, so anyone moving re-rolled in and out of the crowd every frame, signs included. Fixed on my PC; should fix on phones, not confirmed on a phone showing the bug.
- **The 540P draw height no longer gives a black screen in TRUE NATIVE.**
- **ORIGINAL LOOK no longer freezes in matches in TRUE NATIVE.** It now runs without MSAA.

### Settings

- **Your settings now win over QUALITY LOW.** LOW used to override your other choices. It now only sets starting values, and whatever you change stays changed.
- **720p is the new default resolution.** Far more sensible for most machines, and you can still pick higher.
- **LOW is lighter on PC.** Shadows, crowd poses, light strips and the wrestlers' rim light update at half rate on LOW. I checked this in motion and it looks good.

### DIAGNOSTIC RUN: help me fix what I cannot see

My own devices hide a lot of these bugs, so testers should not have to send log after log. **DIAGNOSTIC RUN** is one button, in the launcher, and on phones under LOGS AND UPDATES. It plays a short route by itself for about 3 minutes and saves one zip with everything I need: picture by picture through each stage of a frame, what the GPU really did with the wrestlers' vertices, and what your device reports. The game looks strange during the run, with flat grey or black scenes. That is on purpose. Your own save is not used.

### Known

- **Windows:** the game now uses a memory feature of Windows 10 1803 or newer. I have only tested it on Windows 11 build 26100. If your Windows cannot do it, the game shows a clear message and stops: please tell me your version.
- **Very weak machines:** Bryan's entrance is not a locked 60 on the Surface. CLASSIC (30 fps entrances) has plenty of headroom there.
- **The purple Randy Savage** in 30 Years of WrestleMania is not fixed. It looks like part of the attire build going missing on some device, and I do not know which one yet.
- **Phone fixes marked "should fix"** need you: a DIAGNOSTIC RUN from any phone with a graphics bug is the best help you can give.

Thank you for every report. They are the reason this release exists.

## v0.2.19

TRUE NATIVE, my own renderer, is now the default, and it is faster than when I first showed it in v0.2.18.

- **TRUE NATIVE is now on by default.** In v0.2.18 it was an experiment I had only tried on one match. Since then I have taken it through the whole game on my PC: the arenas, the roster including the DLC wrestlers, all 62 match types and the modes, with full entrances, and it now draws all of it itself. Everyone who updates gets it, including people whose launcher had NATIVE saved. The TRUE NATIVE switch is still in Settings -> Advanced: if something looks wrong on your PC, turn it off and send me a report (launcher: Report a problem).
- **It no longer falls back to the Xbox 360's way of drawing.** Early TRUE NATIVE still handed parts of a frame back to the old 360-style path. That fallback is gone: anything my renderer cannot draw is counted and logged instead. On my PC the pictures stayed within my check's tolerance, and a timed Lesnar vs Bryan match at normal speed ran at the same 60.0 fps as before.
- **Faster.** Measured on my PC (RTX 4090) at 720p with the frame limiter off, Lesnar vs Bryan, one run each way: Brock's entrance went from 155 to 218 fps on average, Bryan's from 171 to 210, and the match from 247 to 288. A second round of trimming (less work per object the game draws) added about 2% on top, and the match's 1% low went from 197 to 209 fps. The causes: the game asks for the same drawing work many times a frame and my renderer now reuses the answer; the crowd is drawn in fewer, bigger batches; and objects outside the camera's view are skipped. One test each on one PC, limiter off, so the gain at a normal 60 fps cap will be smaller.
- **A steadier crowd.** Thinning the crowd with CROWD DENSITY used to make spectators pop in and out as the camera moved. The cut-off is now steadier.
- **30 Years of WrestleMania matches load again in TRUE NATIVE.** Picking one could crash on load, and its opening arena could draw black. Both are fixed, and Heenan's clothes now draw correctly.
- **The picture no longer freezes after minimising the window.** Minimising the game and restoring it could leave the picture frozen while the game kept running at 60 fps behind it. Cause: the window's drawing surface was lost and was only re-created if the window size changed. It is now re-created when the window is restored; I checked it with a minimise and restore.
- **Integrated graphics: automatic LOW quality.** QUALITY AUTO now picks LOW on integrated graphics (for example Intel laptops): the picture is drawn at 720p and scaled up with a sharpening upscaler, and the crowd is thinned to 70%. You can change it by hand in the launcher. I have checked that it chooses the right settings, but not yet measured how much faster it is. Other graphics cards keep STANDARD, unchanged.
- **Shaders are prepared before you play.** The launcher's PREPARING SHADERS step now also converts the extra shaders TRUE NATIVE needs, from your own game files, so the game does not stutter converting them mid-match. Nothing from the game is shipped: the list I ship only names which shaders to prepare.
- **Graphics cards that can only see a small slice of memory.** Some GPUs can address only 128 MiB of memory at once, which gave broken shapes in TRUE NATIVE. The game now detects this and works around it. I tested it on my PC by pretending to be such a card; real cards are tested on phones (see the Android notes).
- **Dynamic resolution works with TRUE NATIVE.** With DYNAMIC RESOLUTION on in the launcher, the game now lowers the 3D scene's resolution a little when frames run slow (the menus and the HUD stay sharp), as it already did for NATIVE. On my PC a forced 75% looked only slightly softer, with everything in place. It stays behind its toggle.
- **Vulkan 1.1 compatibility.** A GPU stuck on Vulkan 1.1 (typical of older phones) rejects the game's shader data layout. The game now asks for the two extensions that allow it. Checked against an emulated Mali-G57 profile (12 errors before, 0 after), not on a real one.

## v0.2.18

A fix for v0.2.17's endless loading screens, new graphics settings in the launcher, and an experimental renderer
switch.

- **No more endless loading screens.** Since v0.2.17 some loading screens never finished, most often in Defeat the
  Streak / Defend the Streak, Create an Entrance and the moveset editor. The cause: v0.2.17 gave the game an Xbox 360
  hard-drive cache so MATCH HIGHLIGHTS could work, and the game then also switched on two large caches of its own
  that had never been used here before. Those two are now turned away, as they were in every version before v0.2.17;
  MATCH HIGHLIGHTS and post-match replays keep working. If you played v0.2.17, the files `CAR.cac` and `EditEx.cac`
  in your user data's `cache_partition` folder (about 600 MB) are no longer used and can be deleted.
- **New graphics settings: SHADOWS, CROWD DENSITY and DEPTH OF FIELD.** The launcher's Graphics page can now turn
  the shadows off, thin the crowd in 10% steps, and turn the background blur off, for more speed on weaker PCs. They
  work with the NATIVE renderer. The defaults keep the game's look. ORIGINAL LOOK moved to Settings -> Advanced.
- **NATIVE on graphics cards without fragment shader interlock.** On these cards (some older AMD cards and
  drivers) most of the match frame fell back from v0.2.16's faster whole-frame drawing to the older, slower way: the
  game's screen clears weren't understood on that path. Two fixes, and now the whole match frame takes the fast path
  there too, with the same picture. Tested on my NVIDIA card forced onto that path, and on a phone (which uses it).
- **TRUE NATIVE (experimental, off by default).** A new switch in Settings -> Advanced that hands the game's drawing
  straight to my own renderer, with my own shaders, instead of going through the Xbox 360's graphics commands. I have
  only tested it with Lesnar vs Bryan (both entrances and the match) on my PC. Leave it off unless you want to try
  it, and please send a report if something looks wrong.

## v0.2.17

A bug fix release: four fixes from your reports, plus a keyboard fix for the ladder match.

- **Royal Rumble: wrestlers no longer freeze on a finisher.** With RUN-INS switched on in the launcher, any finisher,
  signature or running move in a Royal Rumble froze both wrestlers for good while the rest of the arena carried on.
  The cause: my run-in switch turned on the game's interference system in every match, including the ones the game
  itself keeps it out of. Now it only adds run-ins where the game allows them (one-on-one matches). Reported on
  GitHub (#92, #93).
- **Match highlights and post-match replays work.** After a match, the replay of the finish now plays and MATCH
  HIGHLIGHTS can be picked; neither had ever worked. The game keeps its replays on the Xbox 360's hard-drive cache,
  which wasn't there. It now gets one in your user data folder (about 750 MB; the game sizes these files itself).
  Reported on GitHub (#61, #80).
- **30 Years of WrestleMania in full screen: no more double picture.** In the old-era matches (WrestleMania I to
  X8) with the picture filling a wider screen, part of the screen showed the old-footage colouring and the rest a
  second, shifted copy of the picture. The game draws that colouring after its picture is done, where my full screen
  mode treated it like the HUD and squeezed it into the middle. Now it covers the whole screen; the soft dark edge
  around the picture stays 16:9. Reported on GitHub (#69, #98).
- **Royal Rumble: the ring bell and the announcer's intro are back at FRAME RATE 30.** With the 30 fps setting the
  Rumble started silent (crowd only): the game plays some sounds at an exact moment, and at 30 fps its clock jumped
  over that moment. Reported on GitHub (#63).
- **FRAME RATE 30 is now marked experimental.** The game was made to run matches at 60, so some timings can still be
  off at 30 (the match timer counts at half rate, get-ups take longer). Use 60 if anything looks or sounds wrong.
- **Keyboard: the right stick behaves more like a real stick.** Pressing up and down together no longer snaps the
  stick back to the centre (the key pressed last wins), and a very quick tap is no longer missed. This is for the
  ladder match's belt grab on the keyboard (GitHub #77); I couldn't test the belt grab itself yet, so please tell me
  whether it works for you now.

## v0.2.16

Matches now hold 60 fps on my Intel test laptop: the NATIVE renderer draws the whole match frame itself, without going
through the Xbox 360's way of drawing at all.

- **Matches at a steady 60 fps on modest graphics.** On my Intel laptop (a Surface Pro 6 with UHD 620 graphics, at
  720p) a Lesnar vs Bryan match averaged 54.8 fps in v0.2.15, with its slowest 1% of frames at 38.8 fps; now it holds
  the 60 fps cap, with the slowest 1% at 50 fps. Frames that took longer than 20 ms (the stutter you feel) went from
  17% of the match to 1%. The cause: even with v0.2.15's own shaders, every part of the picture still went through a
  copy of the 360's special picture memory (EDRAM): each step loaded its pixels from it and stored them back, and every
  copy the game made went through it too. Now every step of the match's picture is drawn straight into the PC's own
  images, and the copies the next effect needs are served from them. On a fast graphics card the game was already at
  its cap; there it now needs less of the card's time for the same picture.
- **Entrances: a first part.** Most of each entrance's picture is now drawn the same way. On the same laptop Brock's
  entrance went from 45.2 to 48.4 fps (its slowest 1% from 17.8 to 24.9 fps) and Bryan's from 39.5 to 40.8 fps.
  Entrances are what I'm working on next.
- **The same picture.** I compared it with v0.2.15's at 720p and 1440p: identical or within a level or two of
  brightness in a few pixels. It's on with the NATIVE renderer (the default); the extra flag
  `--gpu_native_owned_frame=false` (Settings -> Advanced) brings back v0.2.15's NATIVE.
- **The launcher now installs its own updates.** From this version on, UPDATE NOW downloads the new release, checks it
  against the release's SHA-256, and RESTART AND FINISH swaps the files in, keeping a backup and putting it back if
  anything goes wrong (UPDATE FAILED then says why, with REPORT A PROBLEM). Release notes now show with their
  formatting. **Download v0.2.16 by hand this once:** v0.2.15 and older launchers can't install updates themselves.
- **Gameplay is unchanged.**

## v0.2.15

The biggest speed update yet, aimed at laptops and integrated graphics: a reworked NATIVE renderer with my own
versions of every shader the game draws matches and entrances with, a faster first start, and shorter hitches when an
entrance begins.

- **Faster matches and entrances, most of all on modest graphics.** On integrated graphics such as Intel's HD / UHD 620,
  the game was held back by its graphics chip, and much of that time went into work the PC does not need: the game
  copies its picture several times a frame for the next effect in line, and my translations of the Xbox 360's shaders
  carried overhead that only made sense on the 360. NATIVE now:
  - draws every match and entrance with my own versions of the game's shaders: the crowd, the superstars, the arena and
    stage, the shadows and lighting, the post effects and the on-screen sprites. They compute the same picture as the
    game's own (identical but for tiny rounding) with far less overhead;
  - draws the crowd as one batch instead of one member at a time;
  - skips copies of the picture that nothing reads, and lets the effects that need the picture read it where it already
    is, instead of from a copy;
  - lets the game run ahead while the picture is drawn behind it, instead of waiting on the drawing at every step.
  
  I measured each step on its own on my Intel test laptop (a Surface Pro 6 with UHD 620 graphics, at 720p). For
  example: my own shaders made matches 15% faster and entrances 17-19% faster; my own versions of the superstars'
  materials, another 14% in matches; dropping the unused copies, 14% in Bryan's entrance; reading the scene in place,
  7% in matches. These come on top of each other. On a fast graphics card the game was already at its frame cap; there
  it now needs less of the card's time for the same picture. It's on with the NATIVE renderer (the default) and stays off
  with DLSS, DLAA, FSR or the ACCURATE / LIGHT renderers. The extra flag `--wwe2k14_native_frame=false`
  (Settings -> Advanced) brings back v0.2.14's NATIVE.
- **A much faster first start on slow PCs.** If you had played with the older renderers, the game prepared their
  stored graphics at every start, even when NATIVE draws everything. On my Intel laptop that was about 3 minutes of
  "not responding" before the first frame. NATIVE now skips them. The shipped list of graphics to prepare (PREPARING
  SHADERS on the first launch) is also up to date and leaner: 461 to make instead of 761 on most PCs (448 instead of
  633 on graphics without the feature NATIVE's accurate path uses).
- **Shorter hitch when an entrance starts.** Since v0.2.10's fix for Superstar Threads outfits, the first second of each
  entrance could run at about 23 fps while the game read back pictures it only needs for building outfits. It now stops
  reading back the ones it doesn't need after 4 frames, and outfits still build correctly.
- **Look:** the far-away crowd is thinner (every other distant member is left out; nearby fans are unchanged), and
  texture filtering STANDARD is now the game's own instead of a forced 4x (floors and mats at an angle may look a little
  softer; choose 8x or 16x in GRAPHICS for sharper).
- **Gameplay is unchanged.**

## v0.2.14

Two players on one controller and the keyboard, a fix for PCs whose sound device won't start, and a fix attempt for
black eyes on Intel graphics.

- **New: the keyboard is a player of its own.** With KEYBOARD CONTROLS on and a controller plugged in, the keyboard
  used to drive the same player as the controller, so two people with one controller couldn't play each other. Now
  the keyboard is the next player after the controllers: player 2 next to one controller, player 1 with none. On the
  superstar select screen, press up on the keyboard (W) to join with your own cursor, as a second controller would.
  Whoever presses Start first on the title screen is player 1 and plays with your save. The mouse in the menus goes
  with the keyboard player. To go back to one shared player, turn off SEPARATE PLAYER in the launcher's KEYBOARD
  CONTROLS window (SETTINGS > ADVANCED > MAP KEYS).
- **Fixed: the game crashing as it starts on some PCs (0xC0000005).** When Windows' default sound device couldn't be
  opened, the game crashed a moment after it started, every time. The Xbox 360's sound never fails to start, so the
  game didn't expect it. The game now starts without sound instead, and its log says why ("no audio device could be
  opened"). If you have no sound, check the default playback device in Windows' sound settings.
- **Intel graphics: a fix attempt for black eyes in entrances.** On Intel graphics (reported on an HD 520) the
  superstars' eyes could turn all black during entrances, while matches looked fine. I couldn't reproduce it on my PC.
  My best lead is a speed shortcut in the NATIVE renderer's maths that Intel's driver handles differently: it rounds
  tiny numbers to zero, and that can end in a black pixel. On Intel graphics the game now does those multiplications
  exactly as the Xbox 360 does. If the eyes are still black, or the game got slower for you, please send a report
  (launcher: Report a problem).
- **Gameplay is unchanged.**

## v0.2.13

Your own songs as new entrance themes, and entrances that start smoothly.

- **New: add your own songs as new entrance themes.** The game's own way to use your music (USER PLAYLIST) needs the
  Xbox 360's music library, so it never worked on the PC, and until now a song of yours could only replace a
  superstar's theme. Now every song you add becomes a new entry of its own at the end of the game's MUSIC list, next to
  the game's themes, and no theme is replaced. Add songs in the launcher under MODS > MUSIC > Add a song... (MP3, M4A,
  WAV, FLAC, WMA or AAC), or copy them into the folder that Open the songs folder shows. Then in the game: CREATIONS >
  ENTRANCE > pick a superstar > EASY CREATION > MUSIC, pick your song (it's named after its file, in capitals), and
  ACCEPT. That superstar now walks out to it in matches. Each song keeps its own number (in `slots.txt` in the same
  folder), so adding or removing other songs never changes who has which song. You can change the name the game shows
  on the MUSIC tab. Needs USE MY MUSIC on (the same switch as USE MY VIDEOS; on by default).
- **Music has its own tab in the launcher.** MODS > MUSIC holds your new songs and the per-superstar theme replacements
  that used to be on the TRONS tab.
- **Entrances and matches start smoothly.** The first second of an entrance or a match could run in slow motion
  before it reached full speed. The game tells the graphics driver to hold back any frame that is even slightly late
  until the next screen refresh, so a frame that misses by 1 ms waits a whole extra refresh, and at the start of a
  scene, where a few frames run late, the game ran at half speed for a moment. A late frame is now shown as soon as
  it's ready (whole frames, no tearing). In my tests the first half second of an entrance ran at 95% of full speed
  instead of 82-86%, and frames of 30-40 ms over a whole run went from 34-36 to 9. Also, the graphics the game
  remembers from your earlier runs are now all prepared before you play (some were left until first use before),
  which on my PC halved the longest pause as an entrance starts (80 to 37 ms) and shortened the match start's (49 to
  32 ms). The extra flag `--wwe2k14_late_frame_threshold=-1` (Settings -> Advanced) brings back the game's own frame
  timing.
- **ASPECT: AUTO now fills 16:10 and 4:3 screens too.** Such monitors used to get the game's 16:9 picture with bars
  above and below. Matches and entrances now show more above and below to fill the screen; the menus and the HUD stay
  a centred 16:9, as on ultrawide monitors. 16:9 stays the default.
- **Ultrawide: no more red patches on the superstars.** With ASPECT: AUTO on an ultrawide monitor, blood and marks
  could be painted in the wrong places on a superstar's skin and show up as red patches. The widening for the wider
  screen also caught the step where the game paints those marks onto the skin (an off-screen picture almost as wide as
  the screen) and squeezed it. That step is now left alone. Also on ultrawide, the mouse in the menus now clicks what
  it points at: the menus are a centred 16:9, but the mouse was mapped across the whole screen.
- **Gameplay is unchanged.**

## v0.2.12

Entrances are lighter on the graphics card: two steps the game needed on the Xbox 360 but a PC doesn't are now left out.

- **Entrances cost the graphics card less, same picture.** During entrances the game copies the finished scene (its
  colours and its depth) out into textures and then immediately draws those textures back over the very same picture.
  On the Xbox 360 the scene had to leave its fast video memory to be copied, so drawing it back was necessary; on a PC
  the picture never went anywhere, so the redraw changes nothing. The game now skips it when that's the case, and also
  skips a copy that repeats one it just made from an unchanged picture. In my tests the pictures came out identical
  pixel for pixel (and every copy the game makes byte for byte), and entrances took 7-10% less graphics card time at
  1440p (3-5% at 720p). Matches are unchanged. It's on for everyone with the NATIVE renderer (the default).
- **Gameplay is unchanged.**

## v0.2.11

The game's post effects (depth of field, haze, bloom) now run on a faster shader of my own, and there is a new
optional look for them.

- **Faster post effects, same picture.** The last step of every frame (the game's anti-aliasing, the background blur,
  the blue-grey haze on the far crowd and, in entrances, bloom and the star glare) was running as the game's own
  Xbox 360 shader, translated for the PC. I rewrote that step by hand to do the same maths. In my same-frame tests 92-95%
  of the pixels come out identical and nearly all the rest differ by one shade, and the step takes 38% (matches) to
  50% (entrances) less graphics card time at 1440p. It's on for everyone with the NATIVE renderer (the default).
- **New: Settings -> Graphics -> POST EFFECTS: ENHANCED.** A lens-like background blur instead of the game's soft
  one: the wrestlers stay sharp while the crowd and the stage fall properly out of focus, and lights in the
  background spread into round discs, like a real camera lens ("bokeh"). It follows the game's own focus, which
  changes during entrances. ORIGINAL (the default) keeps the game's look. ENHANCED costs about as much as the game's
  own shader did in entrances and a little more in matches (about 0.05 ms a frame on my RTX 4090 at 1440p).
  Not with upscaling yet: with DLSS, DLAA or FSR on, the game's own post effects run as before (the option is greyed
  out there).
- **Gameplay is unchanged.**

## v0.2.10

Two fixes from player reports: entrances that froze for good on 2-core PCs, and Superstar Threads outfits that came
out wrong in matches.

- **No more entrances freezing for good on 2-core PCs.** On a PC with few CPU threads (reported on an Intel Core i3
  with 2 cores / 4 threads), the game could stop about a second into an entrance and never recover; turning entrances
  off was the only way around it. The cause was the order in which the game hands a job to one of its helper threads:
  it wakes the helper first and only then marks the job "not done yet". On the Xbox 360 that is always in time; on a
  PC with few cores, Windows can let the helper run the whole job first, and the late mark then wipes out its "done"
  signal, so the game waits forever. The game now marks the job before waking the helper. In my tests with the game
  held to 4 CPU threads, the same match froze 3 times out of 6 before the fix and 0 times out of 7 after it.
- **Superstar Threads outfits look right in matches.** A saved Superstar Threads outfit could come out garbled, or
  still in its original colours, once the superstar was in a match, even though the editor showed it correctly. The
  game builds a saved outfit on the graphics card over several frames and then reads the finished picture back; a
  loading speed-up of mine (v0.2.4) skipped reading back anything redrawn more than 4 frames in a row, and a Threads
  outfit takes 5 or more, so its last, finished frame was the one skipped. The limit is now 32 frames: outfits are
  read back whole, and the speed-up still skips the copies the game redraws every frame (those run 130+ frames).
- **Gameplay is unchanged** otherwise.

## v0.2.9

Smooth entrances again on slower PCs: the game now prepares its graphics before you play.

- **No more freezes at the start of entrances.** Since v0.2.6, some players saw the game freeze for a few seconds as
  an entrance began (Brock Lesnar's was the worst: up to 4-5 seconds in my tests), or entrances that felt slow and
  jerky. The cause: v0.2.6 made the graphics faster by building each shader in several tailored versions, and the
  game had to build the ones it was missing the moment an entrance first needed them - dozens at once, each taking
  a noticeable moment on a slower PC or graphics card. Built in the middle of an entrance, they stalled it. v0.2.8
  already removed most of this; v0.2.9 removes the rest of the cause: the versions are now all built before you play.
- **New: "Preparing shaders" before the first Play.** When you press PLAY after updating (or after a graphics driver
  update, or changing the resolution), the launcher first builds every graphics pipeline the game knows about, using
  your whole CPU, with a progress bar. It's a one-time wait: on my PC about 10-20 seconds; a slower PC may take a
  minute or two. Your graphics driver keeps the result, so later sessions start straight away. SKIP is always there
  (the game then builds what it needs as you play, as before). Settings -> Game -> PREPARE SHADERS turns it off.
- **Less competition for the CPU while you play.** The background threads that prepare graphics now run at a lower
  priority than the game's own threads, so they no longer take CPU time from an entrance or a match on a 4-core PC.
- **Less repeated work at start-up over time.** Since v0.2.8 the game prepared two versions of many older remembered
  pipelines at every start, because it couldn't tell which one you use. Now, once you've used one, it remembers that
  exactly and drops the guess, so this extra work shrinks as you play.
- **Gameplay is unchanged:** replays play out identically, and pictures are the same (checked frame by frame).

**What to expect on your first launch after updating:** the "PREPARING SHADERS" window, then the game. The first
match may still build a few new pipelines (content you haven't played since updating); those are remembered too.

## v0.2.8

The red squares on wrestlers are fixed, and much less stutter on slower graphics (Intel laptops especially).

- **No more red squares on wrestlers' backs.** The long-reported red (or dark red) block on a wrestler's back, from the
  start of a match, is fixed. The game clears each wrestler's damage map at the start of a match partly with a tiny
  clear that the renderer dropped: the edge of that map kept old picture data, which the skin shading then showed as
  red. It happened on every graphics card, but was easiest to see on dark gear (The Undertaker's singlet).
- **Far less stutter on slower graphics.** The renderer remembers the graphics pipelines it prepared, so the next
  session can prepare them at start-up instead of mid-match. About half of a match's pipelines were remembered wrongly
  and prepared again in every session - on an integrated GPU that took up to two seconds each, in the middle of the
  action. They are now remembered exactly (what you have stored already still works), and several CPU threads prepare
  them at once instead of one. In my tests with an integrated GPU's preparation times, a match that froze for seconds
  now runs without a single hitch from the second session on. Advanced: `--gpu_native_pipeline_workers=N` sets the
  number of threads (0 = automatic).

## v0.2.7

Map your keyboard in the launcher, a fix for runaway Backspace, and the mouse in the menus.

- **Map your keys in the launcher.** Settings -> Advanced: the option is now called KEYBOARD CONTROLS, with a MAP KEYS
  button beside it. It opens a window with a controller picture: each control is joined to its keys; click a key to
  change it, right-click to remove it, "+" to add another, and hold Shift / Ctrl / Alt for a combination. Hovering a
  row lights that control up on the picture, and a key used twice is pointed out. Changes save at once into the same
  file as the in-game F4 overlay, and the game reads them when it starts. Keys follow your keyboard's own labels
  (French, German, ...).
- **Holding Backspace no longer runs away.** In the game's name fields, letting go of Backspace now stops the deleting
  straight away (before, it kept going for a second or two and could wipe the whole name). Holding Enter or Escape
  presses them once.
- **The mouse in the menus (first version).** Point at an item to highlight it, click to select it, roll the wheel to
  move or scroll, right-click to go back. It covers the menu lists and the whole road to a match: superstar select
  (portraits, tabs and the 1P / COM slots), the PLAY / MATCH RULES / SELECT ARENA screen (with its ENTRANCE and MATCH
  PACE settings), MATCH RULES (click a row or the arrows beside it to change a setting, LB / RB and the page marks to
  change pages) and SELECT ARENA (all three tabs, the wheel scrolls the long ones). Everywhere else, the button bar's
  A and B and a right click work, so you can always back out. The highlight jumps straight to what you point at, with
  the game's own sounds. The mouse is never used in a match. Turn it off with the extra flag
  `--wwe2k14_mouse_menus=false`.

## v0.2.6

A lighter renderer for weaker GPUs and less CPU work animating the wrestlers.

- **Graphics shaders about half as long.** The pixel shaders the renderer builds from the game's own are now about
  half as long on average (measured offline for an AMD integrated GPU), and the vertex shaders lose a large part of
  their emulation code too. Each shader is now tailored to what a draw actually uses (texture formats, how its colour
  is written) instead of checking everything at run time. On our test PC the GPU time per frame fell 6-12% and the
  memory traffic in a match by about a fifth; the bigger win should be on laptops and handhelds, where the GPU is the
  limit - not measured there yet.
- **Wrestler animation costs less CPU.** The game deforms the wrestlers' meshes on the CPU; that work now runs as
  native code, about 40% less time on that thread.
- **Fewer stalls when the game reads pictures back** (portraits, the match start): many copies now share one wait.
- **Gameplay is unchanged:** replays play out identically. Three of the shader shortcuts can change a pixel here and
  there by a hair (hardly ever visible); if anything looks wrong, Settings -> Advanced -> Extra flags
  `--gpu_native_ps_mul_mode=1 --gpu_native_ps_implicit_lod=false --gpu_native_fma=0` turns them off - please send a
  report if you need it.

## v0.2.5

One renderer for everyone, upscalers that keep its speed, far fewer first-time stutters, and fixes for the create
modes.

- **One renderer: NATIVE.** The RENDERER choice is gone from the launcher; everyone now runs NATIVE, the fastest one.
  If you picked ACCURATE or LIGHT at some point (maybe while troubleshooting an older version), you are moved to
  NATIVE automatically. For troubleshooting only, Settings -> Advanced -> Extra flags still takes
  `--wwe2k14_renderer=light` or `--wwe2k14_renderer=accurate` - if you need one of them, please send a report.
- **DLSS, DLAA and FSR now keep NATIVE's speed-up.** Until now, turning on any upscaler quietly switched NATIVE back to
  its slower path. Now they run on NATIVE's fast path, with the HUD kept out of DLSS and the wrestlers' motion vectors
  as before. We checked the pictures and that every frame stays on the fast path; how much faster it is with an
  upscaler we have not measured yet.
- **Far fewer first-time stutters.** The game prepares its graphics shaders the first time it meets each wrestler,
  arena or effect - the hitches and missing frames you could see the first time something new appeared, worst on
  laptops. Now:
  - The game ships with a list of the shaders it uses (276 so far: every wrestler's entrance, all arenas, matches and
    Create a Superstar). The list holds only their fingerprints, no game code: on the first launch the game rebuilds
    the shaders from your own game files (under a second) and prepares them in the background during the logos and
    menus.
  - NATIVE now remembers its own work between launches (it used to redo it every time, and some of its shaders were
    never saved at all).
  - On our desktop, a first run with nothing saved went from 292 frames with missing parts to none. Anything not in the
    list yet still hitches once and is remembered after that.
- **Fixed: Create a Title's export came out as garbled noise** (like Create a Superstar's and Create an Arena's
  pictures before v0.2.2).
- **Faster: Create a Superstar, Create an Arena and Create a Title.** The fix for their saved pictures made the
  processor wait for the graphics card on every frame the whole time you were in those modes. Now it waits only for
  the pictures they actually save. Some of you reported Create a Superstar running slowly: this should help; on our
  desktop it held 60 fps either way, so we could not measure the difference ourselves.

## v0.2.4

A processor (CPU) update: the game needs less CPU time per frame. That matters most where the processor is the limit
(handhelds, older and low-power laptops); on a fast desktop at 60 fps you will not see a difference, and laptops held
back by their graphics chip gain little from this one.

- **Faster: crowd scenes (NATIVE).** The game used to prepare each spectator as a separate drawing step, thousands of
  them in some entrance shots. Now it hands over each group of spectators once, and the whole group is drawn in one
  go. In the most crowd-heavy entrance shot we measured, the part of the processor that feeds the graphics card does
  about 20% less work per frame. Pictures are unchanged: we compared them pixel for pixel.
- **Faster: the game's own code.** The recompiled game code now keeps more of the Xbox 360 processor's state in the
  PC's registers instead of in memory. In our measurements the main game thread needs about 15% less CPU time, and
  the program is 13 MB smaller. The game runs exactly as before: we replayed a whole match and compared the game's
  state every frame.
- **Fixed: two background threads using a whole processor core each** while the monitor is off or asleep (and
  possibly on some unusual display setups). The window's screen-refresh timer kept looping instead of waiting. On a
  laptop that power goes back to the game and the graphics chip.

A big speed update for the NATIVE renderer, plus a fix for slow entrance starts:

- **Faster: NATIVE at 720p and 1440p.** The graphics card now does far less of the slow back-and-forth the Xbox 360's
  drawing steps needed. Copies of drawn pictures (shadows, reflections, depth) go straight where the game reads them,
  instead of through an in-between buffer and a second copy, and many tiny drawing steps no longer wait for each other.
  In our measurements on an RTX 4090 the graphics card needs about 10-15% less time per frame in a match and up to about
  20% less in some entrances, at 720p and at 1440p. Pictures are unchanged: we compared them pixel for pixel at 720p
  and 1440p, and every copy the game reads back byte for byte.
- **Fixed: entrances starting in slow motion for a few seconds** (since v0.2.2, on every renderer). v0.2.2's fix for
  garbled attires copied every picture back from the graphics card for a few seconds after a wrestler loads. It now
  copies back only the one-off pictures (the attires the game builds) and skips the ones the game redraws every frame,
  so an entrance's first seconds run at full speed again. A slow PC can still drop a few frames there while the
  attires are built.

## v0.2.2

Everything in the v0.2.1-native-s56-playtest build (NATIVE faster at 720p, 1440p and 4K; faster on graphics cards
without Resizable BAR), plus fixes for your reports on v0.2.1:

- **Fixed: importing a custom logo with a save that uses DLC** - the import's own game run did not get your DLC
  settings, so the game refused the save ("missing or damaged downloadable content"). Build previews had the same
  problem. If an import still stops before the main menu, the launcher now tells you to turn on your save's DLC packs
  (Settings -> GAME).
- **Fixed: a custom superstar's picture on the selection screen, and a custom arena's thumbnail, showing as noise** -
  the game reads these pictures back from the graphics card, and we had that copy-back off for speed. It is now on
  inside Create a Superstar and Create an Arena. Pictures saved before this build stay garbled: edit the superstar or
  arena and save it again (ACCEPT) to get a new one.
- **Fixed (we think): red patches on wrestlers' skin** (Intel laptops) - while a match loads, the game now waits for
  each drawing step to be ready instead of skipping it. Load times are unchanged on our PC; a very slow PC may stay
  about a second longer on the loading screen. Textures the game draws wider than the screen (a wrestler's attire)
  now always wait too, at any time. If you still see patches, please send a report: the log now shows where any
  skipped drawing went.
- **Fixed: typing on the PC keyboard into a custom superstar's name form** (it could swallow keys).
- **Fixed: wrestlers and managers drawn as coloured noise, or black with magenta streaks** (Bobby Heenan, Miss
  Elizabeth and Randy Savage in 30 Years of WrestleMania, custom superstars) - some attires are drawn on the graphics
  card and then read back by the game, and we had that copy-back off for speed. It is now on while a match loads and
  for a few seconds after a wrestler loads, and those drawings are never skipped. Match loads can take a little longer.
- **Fixed: Bobby Heenan red at ringside** in 30 Years of WrestleMania (right in his entrance, red once the match
  started) - our faster file decompressor raced the game while a menu built his look; in the menus the game now uses
  its own decompressor again (the faster one stays for match loads, where it saves the most time). This may also be
  what was left of the red patches on wrestlers picked in the menus - please tell us.
- **New: better problem reports.** Press **F11** in the game when you see something wrong: it marks the moment (a
  screenshot and what the game was doing). **Report a problem** now packs your last 3 games' logs (with which
  wrestlers, arena and match you played, the scenes you went through, and how your graphics card coped: shaders made
  and how long they took, video memory), small screenshots of each scene, your F11 marks, your graphics drivers and a
  note you can write - compressed, so it is small enough to post.

## v0.2.1-native-s56-playtest

A test build for playtesters (pre-release): v0.2.1 plus a round of speed work on the NATIVE renderer. Pictures are
unchanged (we compared them pixel for pixel at 720p, 1440p and 4K).

- **Faster: NATIVE at 720p** - on our test PC about 6% faster in Daniel Bryan's entrance, 13% in Brock Lesnar's and 18%
  in the match. The many small drawing steps the game does each frame now wait on each other only where they must.
- **Faster: NATIVE at 1440p and 4K** - about 10-14% in the match and 4-6% in entrances: the same speed-ups as 720p
  now work at higher resolutions too.
- **Faster on graphics cards without Resizable BAR**: the numbers each drawing step needs now sit in the card's own
  memory instead of being read across the PCIe bus, about 7% less graphics-card time (it costs a little more
  processor time; no change on integrated graphics).
- A little less processor time per frame in matches (about 3%).
- **Integrated graphics players** (Intel / AMD laptops, handhelds): please tell us whether it runs better for you -
  we measured on a desktop card.

## v0.2.1

Everything in the v0.2.0 playtest builds (more resolutions, NATIVE on cards without interlock, the AMD fixes), plus:

- **Fixed: red patches on wrestlers' skin and wrestlers drawn glowing white** (seen on Intel laptops). On a slow
  laptop the game's shaders take long to compile, and the drawing that builds the wrestlers' textures (skin, attire,
  damage) was skipped meanwhile and never redone. We made it happen on our PC by slowing the compiles down the same
  way, and with the fix the same match looks right. The cost: a few short pauses on the loading screen the first time
  (no pauses in play). **Intel laptop players: please tell us whether the red patches and the white wrestler are
  gone.**
- **Fixed: Quick Matches that never ended** (for example a six-man Hell in a Cell where nobody went for a pin). Quick
  Match now sets a match's rules the way the game's own menus do. We played every match type to its finish with all
  CPU wrestlers: all of them end.
- **New: type on your PC keyboard in the game's name fields** (Create a Story, championships, ...). Each key you type
  jumps the on-screen keyboard to that key and presses it, with the game's own red highlight. Shift, Backspace, Enter
  (done), Escape (cancel), Space and the arrow keys work too. While the on-screen keyboard is up, the keys you type
  don't also work as controller buttons (keyboard mode).

## v0.2.0-native-s54b-playtest

A test build for playtesters (pre-release), the s54 playtest plus two fixes from a ROG Ally tester:

- **Fixed: 360p, 480p and 540p (and 1080p) drew at 720p on AMD graphics** (ROG Ally, Legion Go, Radeon cards).
  AMD's driver can't do one kind of image copy those sizes used, so the game quietly fell back to 720p. They now
  use a different copy that works there. Your log says which one it uses ("depth twins by ...").
- **Fixed: a resolution picked in the game's GRAPHICS menu was undone by the launcher** when the game closed (360p -
  540p and 1080p only; 720p / 1440p / 4K were kept). Both now show and keep the same choice.
- The game's GRAPHICS menu now offers 360p - 540p and 1080p only without upscaling, like the launcher.

## v0.2.0-native-s54-playtest

A test build for playtesters (pre-release). Everything in v0.2.0, plus:

- **More resolutions: 360p, 480p, 540p, 720p, 1080p, 1440p and 4K.** Pick them in Settings -> Display -> RESOLUTION,
  in the first-run setup, or in the game's own GRAPHICS menu. 360p - 540p and 1080p are really drawn at that size by
  the NATIVE renderer, so the graphics card does less work (a softer picture), then scaled to your screen. They need
  NATIVE without an upscaler; with ACCURATE, LIGHT or DLSS / DLAA / FSR the choices are 720p, 1440p and 4K.
- **Integrated graphics now start at 540p** (NATIVE, frame rate 30). If your settings still had the old automatic
  720p on integrated graphics, they move to 540p once. You can change it back any time.
- **NATIVE now also runs on graphics cards without fragment shader interlock** (AMD cards on older drivers, older AMD
  graphics). It is the default there now instead of LIGHT, and settings that still had LIGHT from the old automatic
  choice move to NATIVE once. On our PC (forced onto that path) it draws exactly the same picture as NATIVE elsewhere
  and needs about 13% less graphics-card time than LIGHT in a match. **AMD players: please try it and tell us** -
  if the picture looks wrong, Settings -> Graphics -> RENDERER: LIGHT and send a report.
- Note for AMD: AMD's own Windows driver has had the feature NATIVE prefers since Adrenalin 24.12.1 (December 2024),
  so Radeon RX 5000 and newer on a current driver already used NATIVE.
- Fixed: when the game read your settings by itself (started without the launcher), the NATIVE renderer's settings
  were dropped.

## v0.2.0

- **A new renderer, NATIVE, now the default for everyone.** Instead of imitating the Xbox 360's graphics chip and its
  special EDRAM memory step by step, NATIVE draws each frame straight on your graphics card: a match frame no longer
  touches the emulated EDRAM at all. It was built to give laptops and integrated graphics a playable game. Playtesters
  on Intel Iris Xe report 60 fps. On our desktop it draws the same picture as before.
  - Your settings move to NATIVE once if they still had the old default (ACCURATE). LIGHT stays LIGHT. ACCURATE and
    LIGHT are still in Settings -> Graphics -> RENDERER as fallbacks.
  - NATIVE needs a graphics card with fragment shader interlock (most NVIDIA and Intel cards). Elsewhere (AMD on
    Windows) the game uses the LIGHT renderer instead, as before.
  - With DLSS or DLAA (NVIDIA RTX) NATIVE runs in an earlier, partly emulated mode.
  - Integrated graphics start at 720p and 30 fps by themselves (the first-run setup), and both are normal settings
    you can change.
- **Less work for the processor and the graphics card**, on top of the new renderer:
  - The renderer's command thread does about 9-14% less work in entrances and matches (draws that didn't change are
    recorded by a shorter path, and vertex data the game didn't touch is no longer checked every frame).
  - The game's copy passes are done straight from the renderer's own images, merged and batched. On our desktop the
    graphics card's time per frame fell by about 12-16% at 1440p.
  - A background clock thread that kept part of a CPU core busy now sleeps (7.6% of a core down to under 1%).
- **Start the game without the launcher window:**
  - Fixed: starting `wwe2k14.exe` directly (double-click, a shortcut) stopped with "--game_data_root was not
    provided". It now starts with your launcher settings (game files, save, mods, language, graphics); if the game
    files aren't set up yet, it tells you where to pick them.
  - Settings -> Game -> STARTING THE GAME: **QUICK START** starts the game as soon as the launcher opens (press any key
    within 3 seconds to stay in the launcher, or hold Shift while opening it). The launcher closes with the game.
  - **Make a desktop shortcut** (same place) puts a shortcut on your desktop that starts the game directly.
- **Fixed: Build previews** (Mods -> Trons) failed with the same "--game_data_root was not provided" error.
- **Keyboard binds are kept.** Keys you change in the keyboard mode's settings (F4) used to last only until you closed
  the game. Every change is now saved (in your save folder, as `keybinds.toml`) and comes back the next time.
- **Logo import: 256x256 or 128x128.** Mods -> Logos has a size choice: the game's own two canvas sizes.
- **A little less stutter right before entrances.** The game checks its free memory there, and every failed check
  used to be written to the log on its own (about 1,000 lines in one frame). Those lines are gone.
- The game's log now also names the save folder it uses.

## v0.1.10

- **For laptops and older graphics cards: a LIGHT renderer.** Settings -> Graphics -> RENDERER: ACCURATE is the exact
  Xbox 360 renderer (as before); LIGHT uses your graphics card's own render targets and blending, which should be much
  lighter on integrated graphics. In our checks it draws the same picture (on the same replayed match, under 0.001% of
  the pixels differ noticeably). The first-run setup picks LIGHT and 720p on integrated graphics. We have not been able
  to test it on integrated graphics ourselves yet: please tell us how it runs.
- **AMD graphics cards (Windows): no more black match frames.** AMD's Windows driver can't run the accurate renderer,
  and the game fell back to an older path that showed mostly black frames in matches. It now uses the LIGHT renderer's
  fixes there by itself. (Untested on a real AMD card so far: reports welcome.)
- **Uses far less CPU.** The game used to keep up to three CPU cores fully busy just waiting (for the graphics card,
  for the next frame). It now rests while it waits: about two cores less in a match, at the same frame rate. On a
  laptop that power and heat goes to the graphics instead. (Advanced: `--wwe2k14_low_power=false` brings the old
  behaviour back.)
- **FSR can't break a game any more.** If AMD FSR can't run on a graphics card, the game turns it off by itself with a
  message on screen and carries on (before, the picture could shake and the game hang at a loading screen), and the
  launcher shows why. FSR is no longer offered on integrated graphics, which it does not support.
- **FRAME RATE 30** (Settings -> Display), for PCs that can't hold 60: below 60 the game runs in slow motion, so this
  uses the game's own 30 fps mode instead (entrances and matches at a steady 30, at normal speed). The match timer
  counts at half speed in this mode.
- **A true freeze for photo mode and CineCam.** Freezing the scene now really stops it (wrestlers, crowd, effects)
  instead of slowing it down a lot, and on release everything carries on from the same moment. In photo mode the
  titantron and sound hold too; CineCam's freeze keeps them running. New for CineCam: freeze automatically at the start
  of every entrance, and F8 outside a match opens a CineCam setup panel.
- The game's log now says which graphics card it uses and which renderer path that card gets.

## v0.1.9

- **Fixed: the game crashing at start on PCs with Overwolf** (an access violation, error 0xC0000005, before the title
  screen). Overwolf's in-game overlay hooks into the game's graphics start-up and crashed there. The game now keeps
  Overwolf's overlay out by itself; OBS game capture works as before. Thanks to the player who reported it on GitHub
  with a problem report. (Advanced: `--wwe2k14_vulkan_disable_layers=` with nothing after it lets Overwolf back in.)
- **Launcher:** the update check understands four-number versions such as v0.1.9.1, so small fixes can be offered as
  updates too.

## v0.1.8

- **CineCam keyframed shots.** With CINECAM on (Settings -> Game), press F8 in a match: besides the free camera there
  is now a timeline along the bottom of the screen. Set the camera somewhere, press K, move the playhead, set it
  somewhere else, press K again, and CineCam makes a smooth move between them.
  - Five separate tracks, each with its own keys: Move, Aim, Roll, Lens and Focus, so a long dolly, a zoom and a focus
    pull can each have their own timing. The lens is shown as a focal length in mm (35 mm equivalent).
  - Moves ease in and out (per key: smooth, linear or hold, and how much it slows into the key); a constant-speed
    option gives an even dolly glide. Zooms and focus pulls change at an even-looking rate, without overshoot.
  - Targets: look at a wrestler (with the subject placed anywhere in the frame, e.g. on a third), orbit around them
    (a 270-degree orbit stays centred on them while they move), or keep the focus on them. A "pin" is a fixed point
    instead, e.g. the titantron.
  - Freeze scene nearly stops the game while you design the move; Play previews it (also at half or quarter speed);
    Take plays it from the start in step with the game, for recording. Drag keys in time, click one to edit it.
  - Keys: K key all tracks, Space play / pause, [ ] previous / next key, , and . one frame (Shift: one second),
    Backspace delete the selected key.
  - Shots are saved in Documents\WWE 2K14\CineCam\shots. A saved shot can be used for a re-shoot of a recorded match.
- **Re-shoots:** the first frame of every re-shoot clip used to show the game's own camera; clips now start on the
  shot's first frame.

## v0.1.7

- **30 Years of WrestleMania, any match.** Play -> 30 YEARS OF WRESTLEMANIA lists all 46 matches of the mode by era,
  with the show, date, venue, title, match type, arena, who is in it and the objectives (read from your own game
  files). PLAY MATCH starts the game and it goes to that match by itself after the title screen, the mode's own way:
  its videos, story card, objectives and the match. Locked matches work too. From a command line:
  `wwe2k14.exe --wwe2k14_wm30=N` (N from the list, 0-45).
- **AMD FSR 3 upscaling, on any graphics card.** Settings -> Graphics -> Upscaling (and the game's own GRAPHICS
  screen) now offer FSR NATIVE (anti-aliasing at your resolution), FSR QUALITY (1440p drawn, shown as 4K) and FSR
  PERFORMANCE (720p drawn, shown as 1440p), with the HUD kept sharp as with DLSS. AMD's runtime ships with the game.
- **Texture filtering.** Settings -> Graphics -> Texture filtering (and the game's own GRAPHICS screen): STANDARD (as
  before), 8X or 16X anisotropic filtering for sharper floors, mats and ramps seen at an angle. Changes at once.
- **Photo mode** (Settings -> Game -> Photo mode; off by default). In a match pick PHOTO MODE in the pause menu, press
  F10, or click and hold both sticks:
  the scene nearly stops, the camera is yours (left stick move, right stick look, triggers up / down, bumpers zoom;
  W/A/S/D, Q/E and the right mouse button on the keyboard), the HUD is hidden and depth of field is on (X on / off,
  D-pad up / down focus, left / right the sharp zone, Y automatic focus). A or Enter saves a PNG to
  Pictures\WWE 2K14. B or F10 again to leave.
- **Your own entrance music.** Mods -> pick a wrestler -> Assign music (MP3, M4A, WAV, FLAC, WMA or AAC). In normal
  play their theme is replaced by your track (replays keep the original). Needs USE MY VIDEOS on.
- **Discord status** (Settings -> Game; off by default): shows the match (the wrestlers, the match type, the time) or
  "In the menus" in your Discord status. Needs the Discord app running and an application id.
- **Ultrawide** (Settings -> Display -> Aspect: AUTO; off by default): on a 21:9 or 32:9 screen you see more of the
  arena at the sides; the HUD and menus stay a centred 16:9.
- **DLC wrestlers in Quick Match.** With your DLC on, the wrestler pickers (and Random) include the wrestlers of every
  pack that is switched on, named from your own DLC files. A match with a wrestler whose pack is off doesn't start
  (the launcher says which pack to switch on).

## v0.1.6

- **Your own DLC ("bring your own packs").** Put the DLC from your console in a folder called `dlc` next to your game
  files, or pick a folder in Settings -> Game -> Downloadable content. Both kinds work: the packages exactly as they
  sit on the console's drive (e.g. copied with FATXplorer from `Content\0000000000000000\545408B2\00000002\`), or packs
  you have extracted (one folder per pack). They are read in place: nothing is copied or changed.
  - Settings shows each pack by its own name, with its size and an ON / OFF switch; the Play page says how many are on.
  - The DLC wrestlers are under DOWNLOADED SUPERSTARS on the wrestler select screen, with their entrances and
    titantrons; the Accelerator unlocks the whole roster.
  - A save made with DLC on needs those packs: with one switched off, the game says the save can't be used. Switch
    the pack back on and everything is there again.
  - Replays remember which packs were on, and say so instead of playing when one is missing.
  - Damaged files, packs for other games and duplicates are listed with the reason and left out; they can't stop the
    game from starting.
- **NVIDIA DLSS / DLAA need no setting up:** Settings -> Graphics simply uses the NVIDIA files included with the game
  (a "Use other files..." button picks another folder), and a folder chosen earlier that no longer has the files
  falls back to the included ones.
- Fixed: a folder typed with a `\` at the end in Settings could garble the game's start-up options.

Known issues:
- DLC: tested with extracted packs and with packages made from them; a package straight off a console has not been
  tried yet. Please send a report (Report a problem lists your packs, never their files) if one doesn't show up.
- DLC wrestlers are not in Quick Match yet (use PLAY GAME and the game's own menus).

## v0.1.5-playtest

- **A new look for the launcher**, in the style of the game's own menus: the red page band with PLAY, SETTINGS, MODS
  and ABOUT, the game's own menu lettering (read from your game files), slanted buttons, and the logo.
- **Quick Match has every match type from the game's PLAY menu** (58), laid out like the game: one on one (Extreme
  Rules, Hell in a Cell, I Quit, Inferno, Iron Man, Ladder, Last Man Standing, Steel Cage, Submission, Table, TLC),
  two on two (tag, tornado and elimination tag in every variant, mixed tag), triple threat and fatal 4-way variants,
  6-man (battle royal, Elimination Chamber, Hell in a Cell, ladder, tag), handicap (incl. gauntlet) and backstage
  brawls. The wrestlers are grouped by team. The Royal Rumble still needs "My saved setups".
- **New settings:**
  - **Language**: English, Deutsch, Français, Español or Italiano (the five on the disc); Automatic follows Windows.
  - **Graphics card**: the game now uses your dedicated graphics card by itself (laptops used to start on the built-in
    one); you can also pick one by name.
  - **Display**: fullscreen or windowed.
  - **Frame rate** is now 60 / 120 / Uncapped (Uncapped = your monitor's refresh rate), in the launcher and in the
    game's Options -> GRAPHICS. 60 fps entrances are the default; the Xbox 360's 30 fps entrances are in "Original
    look".
- The first-run wizard lets you set language, graphics card, resolution and frame rate before your first game.
- **About** page with the project's link, the credits and the licences.
- Mods: the trons grid shows two columns at the default size, and the titles have a grid view too.
- **The launcher checks for updates**: when a newer full release is out on GitHub, an UPDATE button appears at the
  bottom of the red band; it shows what's new and opens the release page. Settings -> Game -> Check for updates turns
  it off.

Known issues:
- The graphics-card choice is new: if a laptop still starts on its built-in GPU, please send a report.
- Our own menu items in the game (QUIT TO DESKTOP, GRAPHICS) stay in English in every language.
- FSR (upscaling on non-NVIDIA cards) is still not offered.

## v0.1.4-playtest

- **GRAPHICS settings in Options.** A new GRAPHICS item sits after GAMEPLAY in the game's own Options menu. It is the
  same Upscaling, Frame rate and Resolution as in the launcher's Settings, changed from inside the game:
  - DLAA on / off and Classic / 60 everywhere take effect at once.
  - The DLSS modes, High refresh and the resolution take effect the next time you start the game (the game says so).
  - Choices are saved to the launcher's settings, so the launcher and the game always agree.
- **QUIT TO DESKTOP** at the bottom of the main menu (it asks first). Alt+F4 still works.
- **Starting the game without the launcher** now uses the Resolution, Frame rate and Upscaling you set in the launcher.

Known issues:
- The "some changes apply the next time you start the game" message has Yes / No buttons; either one just closes it.
- FSR (upscaling on non-NVIDIA cards) is still not offered.

## v0.1.3-playtest

- **Fixed: John Cena's title glitching around as he walks out** at 60 fps and higher frame rates. The swing in his
  hand now looks like the original 30 fps entrance, only smoother; the classic frame rate is unchanged.
- **Advanced (replays and slow-motion re-shoots above 60 fps):** the stage video screens no longer go black, and the
  titantron plays in step with the scene, so in a slow-motion re-shoot it plays slowed down too (custom MP4 trons
  as well).

## v0.1.2-playtest

- **Fixed: the rare crash a few seconds into starting the game** (in the sound start-up). "Try again" is no longer
  needed for it.
- **Quick Match without setting it up in the menus first:** Play Game -> Quick Match -> **Build a match**:
  - pick the match type, the wrestlers (type to search; Random per slot) and the arena, or **Random everything**;
  - match types: One on One, Triple Threat, Fatal 4-Way, Tag Team, 6-Pack Challenge, Elimination Chamber, Hell in a
    Cell;
  - you play wrestler 1 with controller 1, or tick **CPU vs CPU** to watch;
  - your saved setups are still there ("My saved setups"); the Royal Rumble still needs one.
- **Logos:** the import now checks every screen it passes and stops safely (the game closes without saving, the
  launcher says where) if anything looks unexpected. It also works on a save that has never used Create-a-Logo.
- **DLSS:** no smear from the previous shot right after a camera cut.

Known issues:
- John Cena's title can glitch around as he walks out when entrances run at 60 fps (Settings -> Frame rate -> classic
  avoids it). Being looked at.
- FSR (upscaling on non-NVIDIA cards) is still not offered.

## v0.1.1-playtest
- **Play from a disc image:** in the launcher, Settings -> **Disc image...** and pick an `.iso` of your own disc (a
  full image or a trimmed one). Nothing needs unpacking. A folder copy still works as before.
- The launcher's game-file check reads inside the disc image too, and says plainly when an image is incomplete or
  damaged ("copy it again").
- A title update file (`default.xexp`) next to your game files is ignored: the game plays the disc version.
- If the game can't start (game files not found, a damaged or incomplete disc image), it now shows its message and
  closes, instead of crashing. The launcher shows a short note instead of the crash dialog.

Known issues:
- Disc images were tested with images made from a copied disc; an image ripped straight from a disc drive hasn't been
  tried yet.
- Otherwise the same as v0.1.0 below.

## v0.1.0-playtest (first playtest build)

The first build for testers. What's in it:

- **The game** runs natively on Windows (Vulkan):
  - menus, Create-a-Wrestler, Create-a-Logo, exhibition matches with entrances, titantrons and music;
  - WrestleMania arenas, the Royal Rumble, Hell in a Cell, Elimination Chamber.
- **Looks and speed:**
  - 1440p at 60 fps by default; 720p and 4K too.
  - High-refresh matches (up to your monitor's rate).
  - Fast loads.
- **Upscaling:** DLAA and DLSS on NVIDIA RTX cards (NVIDIA's DLSS files included).
- **Nothing to install:** the Visual C++ runtime files are included.
- **The launcher:**
  - Play, plus Quick Match (save a match setup once, start it in one click).
  - Mods: your own trons, title reveals, and PNGs as real Create-a-Logo logos (save backed up first).
  - Settings with a check of your game files.
  - A first-run wizard and Report a problem.
- **Extras:** CineCam (F8, free camera); run-ins (F9, experimental).

Known issues:
- A rare audio crash right at start-up: Try again fixes it.
- FSR (upscaling on non-NVIDIA cards) is not in yet.
- Keyboard play is untested; use a controller.
