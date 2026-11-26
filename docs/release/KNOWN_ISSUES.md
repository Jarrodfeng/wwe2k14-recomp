- Weak graphics (laptops with integrated graphics such as an Intel UHD 620): on my Surface Pro 6, v0.4.0 holds 60 pictures a second in Daniel Bryan's entrance and in a match, but those were short runs while the laptop was still at full clock speed. After a long session, when it heats up and slows down, it has not been measured yet. If it feels choppy on yours, please send a report with the speed shown (launcher: Report a problem).
- Weak Intel graphics (seen on my Surface Pro 6, Intel UHD 620, after long sessions of back-to-back test runs): about one entrance in 10 to 20 can freeze for a second or two and then stay frozen. Windows reports a graphics-driver fault under heavy desktop compositing; I have the cause narrowed down but not fixed. Restarting the game recovers it. If it happens to you, please send a report.
- Custom music: for now the song you pick in the Custom Playlist picker applies to every superstar who uses USER PLAYLIST, not one song per superstar, because I have not found what identifies the entering superstar yet. Choosing USER PLAYLIST sets it straight away, so closing the picker without picking still leaves the superstar on USER PLAYLIST. In the entrance editor's preview your songs sound a little quieter than the game's own (at entrances they match). A song shorter than the entrance stops when it ends; it does not loop. Songs over 20 minutes and silent or unreadable files are refused. I have not tested how the game's music volume option affects custom songs.
- Custom music on phones: the launcher converts songs on the phone and the game plays them; I have tried it on my own phone (OPPO Find X8 Ultra) only, so a first run on another phone is a real test. If a song is silent or the game stutters when it starts, please send a report (launcher: Report a problem).
- Selection screens: the superstar's picture on the character select and in Create an Entrance's superstar list can show blank or as coloured blocks, most often when moving quickly through the menus or after a match. Moving the cursor to another superstar and back fixes it. Seen on my phone; I am still finding the cause.
- The superstar library (new in v0.4.2) moves your imported superstars out of the old pack the first time you press PLAY in the launcher with USE MY SUPERSTARS on. I tested the move through the launcher on a copy of my own imports (and loaded a v0.4.1 save with an edited entrance afterwards), and with libraries of up to 60 superstars; not on a phone. The move also removes the importer's second copy of each pac you imported: keep your downloads if you want the original files. The old pack stays in the importer's folder as `legacy.old` until the game has run once on the library. If an import goes missing, keep that folder and send a report (launcher: Report a problem). If you start the game without the launcher before the move, your imports are hidden (nothing is lost) until you press PLAY in the launcher once.
- The game can show about 100 imported wrestlers at a time (fewer when a DLC pack takes some of those places); the rest of your library goes on further pages of the DOWNLOADED SUPERSTARS tab. There is no set maximum that I know of, but I tested the pages with small libraries only, not with hundreds of wrestlers in the game. In a Royal Rumble, CHANGE PARTICIPANTS has no pages, and an imported superstar entering a Rumble has not been seen yet; a superstar from a later page has not been played through a whole match yet.
- Universe: when you replace or remove an imported wrestler, a Royal Rumble or battle royal already booked keeps the superstar who left. Special matches, and later Universe days after the change, have not been tested yet.
- Imported superstars: on some base wrestlers ADVANCED CREATION in Create an Entrance can still drop back to the list; the highlighted card of an import in a picker can show a garbled name and no picture; the light beams can differ from the base wrestler's colours (seen on a Kane base); a titantron placed for the import's own slot number is not played (his entrance wrestler's is): give him a custom titantron slot instead.
- To show the right picture for a superstar on a later page, the game now reads pictures and models from their files every time, not from memory by name. In my one comparison on PC the loading time was the same (within a few hundredths of a second); it is not measured on a phone.
- v0.4.3 fixes that were made from the game's code and not yet seen in the game by me: the ring breaking fully in the OMG corner move, Create a Superstar logos, the flaming table going out when the replay is skipped, and the old-film grain's speed in 30 Years cutscenes. Not yet seen on a phone: the caption / lens flare jump and the arena's mini and wall screens. If one of these is still wrong for you, please send a report (launcher: Report a problem).
- Imported superstars with very detailed models (new in v0.4.3): checked on PC with one model about twice the usual size, through his entrance and a match. Since then also checked on PC in the Create an Entrance preview. Not checked: more than one of them in a match, and phones. (The move-set editor shows a stand-in figure, not the superstar's own model, so it is not affected.) The game still has a fixed amount of memory overall, so a model far larger than that can still fail.
- New in v0.4.4, the loading screen before a match stays up about two seconds longer on my PC: it now waits until my renderer has the arena, the crowd and the wrestlers ready, so the entrance no longer starts on a black screen. About a tenth of a second of black is left at an entrance (three tenths with entrances off). Not seen yet: 30 Years of WrestleMania scenes, story and Universe entrances, slow disks. If a loading screen ever stays up and does not go away, please send a report (launcher: Report a problem).
- Crowd camera flashes now follow the original game again (v0.4.4 cut them wherever a wrestler stood in front): a flash whose centre is behind a wrestler is hidden whole, and a flash whose centre can be seen is drawn whole, so its rays can show over the edge of a wrestler, as they do on the Xbox 360. This is read from the game's code and tested outside the game, not yet seen in a game run. If a flash whose centre is behind a wrestler still shows over him, please send a report with a screenshot.
- In some 30 Years of WrestleMania cutscenes (seen in Austin vs The Rock), a few crowd camera flashes appear on the floor or in the air near the wrestlers. On the Xbox 360 the flashes stay in the crowd. The game places every flash out in the crowd and my renderer reads that point correctly, so the fault is in how my renderer shows those few; I am still narrowing it down.
- A Create a Superstar crash was reported to me; its cause is not found yet. v0.4.3 writes more about it into the log: if it happens to you, please send a report straight after (launcher: Report a problem).
- On the phone, the first time you see a crowd camera flash or another effect I draw myself, it can hitch once while the graphics driver prepares it; after that it is remembered. (The PC build prepares these ahead of time since v0.4.2.)
- Custom titantrons (new in v0.4.2): big screen only. The phone launcher has no screen for them yet, and a removed custom titantron cannot be brought back from the launcher. The launcher's CUSTOM TRONS section and the importer's TRON row are new and lightly tested.
- Select screen (new in v0.4.2): LT / RT now jump by letter instead of their old use there. To get the old use back, add `--wwe2k14_letter_jump=false` in Settings -> Advanced (extra flags). The letter box has no page row yet, and the CHANGE PARTICIPANTS list of a Royal Rumble has no letter box. The letter jump has not been tried with a phone's touch controls.
- A crash in the Move-set editor was reported to me; I could not make it happen. If it happens to you, please send a report (launcher: Report a problem).
- The game now uses a memory feature of Windows 10 1803 or newer, tested by me only on Windows 11 build 26100. If your Windows cannot
  do it, the game shows a clear message and stops: please send a report with your Windows version.
- The new renderer now draws everything, with no fallback to the Xbox 360's way of drawing. It was tested on my PC (NVIDIA RTX 4090), my Surface Pro 6 (Intel UHD 620) and one phone (OPPO Find X8 Ultra, Adreno 830). AMD graphics, other Intel graphics and other phones are untested. If something is missing or looks wrong (a missing or oddly coloured part, flicker, wrong shadows, a freeze), send a report (launcher: Report a problem, or the bug-button zip).
- DLSS, DLAA and FSR are not available with the new renderer yet. In v0.4.1 the UPSCALING choice and the screen shape choice are gone from the launcher (they did nothing with the new renderer); they come back when they work again.
- Screens wider or taller than 16:9 (ultrawide monitors, phones with a wide screen, 16:10 and 4:3): v0.4.1 fills more of the picture (crowd, stage, shadows, post effects), with the HUD kept as a centred 16:9 and the menus at 16:9 with bars. Checked at 21:9, 32:9 and 16:10 on my PC, not on a real ultrawide monitor: if something pops in or out near the screen edge, please send a report (launcher: Report a problem).
- The keyboard as its own player (new in v0.2.14) was tested with a simulated second controller, not by two people
  yet. With four controllers the keyboard shares player 1 again. Not tried yet: a controller switching off in the
  middle of a match (with no controller left, the keyboard becomes player 1 until the controller is back).
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
- 30 Years of WrestleMania: PLAY THIS MATCH (the launchers' 30 Years page) was played to the end of a match with every cutscene and event,
  but whether winning a match started this way marks it done in your save, out of order, has not been checked yet.
- DLC: a package straight off a console has not been tried yet (extracted packs and packages made from them work). If
  one doesn't show up, please send a report (launcher: Report a problem lists your packs, never their files).
- Discord status needs an application id (Settings -> Game) until the project has its own.
- My own menu items in the game (QUIT TO DESKTOP, GRAPHICS, PHOTO MODE) stay in English in every language.
- A black screen at the start or at 1080p (reported on an Intel laptop): v0.4.1 shows a message when the graphics driver stops.
  A black screen without one: please send a report (launcher: Report a problem).
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
- Screen tearing on monitors without G-SYNC / FreeSync (most laptops): the game shows each frame the moment it is
  ready. A fix is ready but made entrances a little choppier in my tests, so it is off for now; to try it, add
  `--wwe2k14_present_mailbox=true` in Settings -> Advanced (extra flags). GitHub #79.
- FRAME RATE 30 is experimental: the game was made to run matches at 60, so some match timings can be off. The match
  timer (and anything else the game counts in frames) runs at half speed, and get-ups take longer. Use 60 if anything
  looks or sounds wrong.
- 360p - 540p and 1080p: the speed gain on integrated graphics has not been measured by me yet (no such PC here).
- CineCam's setup panel (F8 outside a match) and the automatic entrance freeze are new and lightly tested.
- v0.2.5's shader list does not cover everything yet (Quick Match types, 30 Years of WrestleMania matches, some menus
  and cutscenes): those still hitch the first time, then are remembered.
- After an update that changes how the game draws (v0.4.1 changes wrestlers, the ring and props), the first play of a mode can hitch once while the graphics driver builds its pipelines; "Preparing shaders" now covers most of them (91 of 94 in my test, 6 left on the second play), and it does not come back.
- Sign boards in the arena show the arena's own picture, not the crowd-sign picture the game swaps in (next update).
- 30 Years of WrestleMania captions show only when the game's own caption option is on in your save.
- Blood: draws in my test (turn BLOOD on in the game's options); not played through a whole match.
- Created superstars: Superstar Threads were tried with one test colour set. Hogan's ring attires were not seen.
- Phones: the heat fix is confirmed on my own phone (the temperature dropped and stayed stable, even while charging); other phones, the
  release-build change and the message when the graphics driver stops have not been seen on a phone yet.
