- New songs (new in v0.2.13) are silent in the entrance editor's PREVIEW: they play in matches only. They are also
  silent in replays and re-shoots, as theme replacements are. A superstar given a new song walks out in silence if the
  song's file is gone or USE MY MUSIC is off (the editor then shows USER PLAYLIST for him); nothing breaks.
- I saw one crash while entering the entrance editor (CREATIONS > ENTRANCE) in my tests, and it did not come back in
  72 more tries. If it happens to you, please send a report (launcher: Report a problem).
- The 16:10 / 4:3 fill and the ultrawide red-patch fix (new in v0.2.13) were checked on my PC with a test window of
  that shape and with a phone's screen shape, not on such monitors. The ultrawide mouse fix was worked out from the
  code, not tried on an ultrawide monitor yet.
- The entrance savings (new in v0.2.12) were checked on my NVIDIA card at 720p and 1440p. If an entrance looks wrong
  after the update (a missing or frozen background, wrong colours behind the wrestler), the extra flags
  `--gpu_native_truth_skip_restores=false --gpu_native_truth_skip_repeat_resolves=false` (Settings -> Advanced) bring
  back the old behaviour; please send a report too (launcher: Report a problem).
- POST EFFECTS does nothing yet with DLSS, DLAA or FSR on (the game's own post effects run there; the option is greyed
  out).
- POST EFFECTS (new in v0.2.11) was checked on my NVIDIA card at 720p and 1440p on two entrances and a match. If the
  picture looks wrong after the update (a blurry or blocky background, missing glare), the extra flag
  `--gpu_native_shader_swap=false` (Settings -> Advanced) brings back the game's own shader; please send a report too
  (launcher: Report a problem).
- CineCam's timeline is keyboard and mouse only for now (the controller still flies the camera in photo mode). There
  is no undo: K at a key's time replaces that key. Clicking the timeline moves the camera to the shot's pose at that
  time, so choose the time first, then fly.
- CineCam's look-at framing has not been checked on ultrawide screens.
- 30 Years of WrestleMania: what the game does after you win a match you jumped into, and whether that marks it done in
  your save out of order, has not been checked yet.
- DLC: a package straight off a console has not been tried yet (extracted packs and packages made from them work). If
  one doesn't show up, please send a report (launcher: Report a problem lists your packs, never their files).
- Discord status needs an application id (Settings -> Game) until the project has its own.
- My own menu items in the game (QUIT TO DESKTOP, GRAPHICS, PHOTO MODE) stay in English in every language.
- Superstars looking shiny or sparkly (seen on Intel Iris Xe): being looked into. If you see it, please try the extra
  flags `--gpu_native_ps_mul_mode=0 --gpu_native_fma=0 --gpu_native_ps_implicit_lod=false` (Settings -> Advanced) and
  tell me whether it goes away.
- A black screen at the start or at 1080p (reported on an Intel laptop): I need a log from a run where it happens
  (launcher: Report a problem).
- The first time you meet a superstar, arena or match type the shipped pipeline list does not cover, its loading screen
  can pause while the graphics driver prepares it (longest on integrated graphics); after that it is remembered, and
  the next "Preparing shaders" includes it.
- "Preparing shaders" (new in v0.2.9) was timed on my PC, also with slowed-down compiles to stand in for a laptop
  (about 20 seconds there); how long it takes on integrated graphics has not been measured on real hardware yet.
- Superstar Threads (fixed in v0.2.10): checked with a recoloured T-shirt. If an outfit with more edits (designs,
  logos) still looks wrong in a match, please send a report with a screenshot of the editor and of the match.
- A custom wrestler added to a save with an outside tool freezes (and so does his opponent) when he starts a custom
  finisher that the save does not have. Create that finisher in your save (CREATIONS > SPECIAL MOVE) or give him one
  of the game's own finishers. The freeze happens with all of my changes turned off too.
- The mouse in the menus is new. Not covered yet: the player slots of handicap, Elimination Chamber and Royal Rumble
  matches (use the controller or keys there) and the menus inside Create a Superstar, Create an Arena, 30 Years of
  WrestleMania and other modes (there: the button bar's A / B and right click only). Custom arenas were tried on empty
  slots only. Clicking a page mark goes one page at a time, so a jump of two pages shows the page between briefly.
- Typing on the PC keyboard into the game's name fields was tested in the championship name; other fields use the
  same on-screen keyboard. Keyboard layouts other than US English type what their keys show, but only the characters
  the game's on-screen keyboard has.
- The LIGHT renderer (now only an advanced flag) can crash on older Intel graphics (HD 520).
- NATIVE on graphics cards without fragment shader interlock (older AMD drivers / graphics) and the LIGHT fallback
  have not been tested on such a card by me yet (only on my NVIDIA card forced onto the same path). With FRAME RATE
  30 the match timer (and anything else the game counts in frames) runs at half speed.
- 360p - 540p and 1080p: the speed gain on integrated graphics has not been measured by me yet (no such PC here).
- v0.2.3's NATIVE speed-ups were checked and measured on my NVIDIA card only. If pictures look wrong on yours (shadows,
  reflections, the crowd), please send a report (launcher: Report a problem).
- v0.2.4's CPU gains were measured on my desktop (Ryzen 9 5950X) only; on handhelds and laptops they are expected,
  not measured. If the crowd looks wrong in NATIVE (missing or flickering spectators), please send a report.
- CineCam's setup panel (F8 outside a match) and the automatic entrance freeze are new and lightly tested.
- v0.2.5's DLSS / DLAA / FSR on NATIVE was checked on my test routes (a match, the pause menu) on an NVIDIA card, not
  through whole play sessions yet. If something looks wrong with an upscaler on (the HUD, ghosting on the wrestlers),
  please send a report.
- v0.2.5's shader list does not cover everything yet (Quick Match types, 30 Years of WrestleMania matches, some menus
  and cutscenes): those still hitch the first time, then are remembered.
