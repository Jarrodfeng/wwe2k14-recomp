# Changelog

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
