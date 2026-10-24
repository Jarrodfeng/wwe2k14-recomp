# Changelog

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
