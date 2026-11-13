- Weak graphics (laptops with integrated graphics such as an Intel UHD 620): on my Surface Pro 6 a match now shows about 47 to 53 pictures a second, where v0.2.19 showed 60 on the same laptop. The new renderer's own drawing work is the limit there, not the graphics chip; the cuts that skip the game's drawing work for the ring, the shadows and the crowd are built and checked, but on that laptop they cost more than they saved, so they are off in this release. Making the new renderer faster on weak hardware is my next big piece of work. If matches feel choppy on yours, please send a report with the speed shown (launcher: Report a problem).
- Custom music: for now the song you pick in the Custom Playlist picker applies to every superstar who uses USER PLAYLIST, not one song per superstar, because I have not found what identifies the entering superstar yet. Choosing USER PLAYLIST sets it straight away, so closing the picker without picking still leaves the superstar on USER PLAYLIST. In the entrance editor's preview your songs sound a little quieter than the game's own (at entrances they match). A song shorter than the entrance stops when it ends; it does not loop. Songs over 20 minutes and silent or unreadable files are refused. I have not tested how the game's music volume option affects custom songs.
- Custom music on phones: the launcher converts songs on the phone and the game plays them; I have tried it on my own phone (OPPO Find X8 Ultra) only, so a first run on another phone is a real test. If a song is silent or the game stutters when it starts, please send a report (launcher: Report a problem).
- Selection screens: the superstar's picture on the character select and in Create an Entrance's superstar list can show blank or as coloured blocks, most often when moving quickly through the menus or after a match. Moving the cursor to another superstar and back fixes it. Seen on my phone; I am still finding the cause.
- The Superstar Importer (new in v0.3.1) was tested in the game with one character, in his first attire, on my PC and my phone. The pack is built for up to 103 characters, but a full pack has not been played. If an imported superstar goes wrong, switch USE MY SUPERSTARS off and send a report (launcher: Report a problem).
- The game now uses a memory feature of Windows 10 1803 or newer, tested by me only on Windows 11 build 26100. If your Windows cannot
  do it, the game shows a clear message and stops: please send a report with your Windows version.
- The new renderer is on for everyone (PC and phone). It was tested on my PC (NVIDIA RTX 4090) and on one phone (OPPO Find X8 Ultra, Adreno 830). AMD and Intel graphics, other phones, 4K and the LOW preset with it are untested. If your graphics card shows problems (a missing or oddly coloured part, flicker, wrong shadows, a freeze), start with the extra flag `--wwe2k14_s101_promotions=false` (Settings -> Advanced -> extra flags) to get the previous way of drawing, and send a report (launcher: Report a problem, or the bug-button zip).
- The keyboard as its own player (new in v0.2.14) was tested with a simulated second controller, not by two people
  yet. With four controllers the keyboard shares player 1 again. Not tried yet: a controller switching off in the
  middle of a match (with no controller left, the keyboard becomes player 1 until the controller is back).
- The black-eyes fix for Intel graphics (new in v0.2.14) has not been tried on Intel hardware by me, and how much it
  slows Intel laptops is not measured. Please tell me whether the eyes look right now (launcher: Report a problem).
- New songs (new in v0.2.13) are silent in replays and re-shoots, as theme replacements are. A superstar given a new song walks out in silence if the
  song's file is gone or USE MY MUSIC is off (the editor then shows USER PLAYLIST for him); nothing breaks.
- I saw one crash while entering the entrance editor (CREATIONS > ENTRANCE) in my tests, and it did not come back in
  72 more tries. If it happens to you, please send a report (launcher: Report a problem).
- The 16:10 / 4:3 fill and the ultrawide red-patch fix (new in v0.2.13) were checked on my PC with a test window of
  that shape and with a phone's screen shape, not on such monitors. The ultrawide mouse fix was worked out from the
  code, not tried on an ultrawide monitor yet.
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
- Superstars looking shiny or sparkly (seen on Intel Iris Xe): being looked into. Since v0.2.14 Intel graphics already
  use the exact multiplications (the first of the flags I asked about before). If you still see it, please try the
  extra flags `--gpu_native_fma=0 --gpu_native_ps_implicit_lod=false` (Settings -> Advanced) and tell me whether it
  goes away.
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
  have not been tested on such a card by me yet (only on my NVIDIA card forced onto the same path).
- Screen tearing on monitors without G-SYNC / FreeSync (most laptops): the game shows each frame the moment it is
  ready. A fix is ready but made entrances a little choppier in my tests, so it is off for now; to try it, add
  `--wwe2k14_present_mailbox=true` in Settings -> Advanced (extra flags). GitHub #79.
- FRAME RATE 30 is experimental: the game was made to run matches at 60, so some match timings can be off. The match
  timer (and anything else the game counts in frames) runs at half speed, and get-ups take longer. Use 60 if anything
  looks or sounds wrong.
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
