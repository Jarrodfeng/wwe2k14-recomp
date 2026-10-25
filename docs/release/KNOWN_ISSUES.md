- CineCam's timeline is keyboard and mouse only for now (the controller still flies the camera in photo mode). There
  is no undo: K at a key's time replaces that key. Clicking the timeline moves the camera to the shot's pose at that
  time, so choose the time first, then fly.
- CineCam's look-at framing has not been checked on ultrawide screens.
- 30 Years of WrestleMania: what the game does after you win a match you jumped into, and whether that marks it done in
  your save out of order, has not been checked yet.
- DLC: a package straight off a console has not been tried yet (extracted packs and packages made from them work). If
  one doesn't show up, please send a report (launcher: Report a problem lists your packs, never their files).
- Discord status needs an application id (Settings -> Game) until the project has its own.
- Our own menu items in the game (QUIT TO DESKTOP, GRAPHICS, PHOTO MODE) stay in English in every language.
- Red patches on wrestlers' skin / a wrestler drawn glowing white (Intel laptops): this build closes one more
  possible cause, and we can no longer make them appear here. If you still see them, please send a report with your logs
  (launcher: Report a problem).
- A custom wrestler added to a save with an outside tool freezes (and so does his opponent) when he starts a custom
  finisher that the save does not have. Create that finisher in your save (CREATIONS > SPECIAL MOVE) or give him one
  of the game's own finishers. The freeze happens with all of our changes turned off too.
- Typing on the PC keyboard into the game's name fields was tested in the championship name; other fields use the
  same on-screen keyboard. Keyboard layouts other than US English type what their keys show, but only the characters
  the game's on-screen keyboard has.
- The LIGHT renderer can crash on older Intel graphics (HD 520): use NATIVE there (it runs well on those laptops).
- NATIVE on graphics cards without fragment shader interlock (older AMD drivers / graphics) and the LIGHT fallback
  have not been tested on such a card by us yet (only on our NVIDIA card forced onto the same path). With FRAME RATE
  30 the match timer (and anything else the game counts in frames) runs at half speed.
- 360p - 540p and 1080p: the speed gain on integrated graphics has not been measured by us yet (no such PC here).
- v0.2.3's NATIVE speed-ups were checked and measured on our NVIDIA card only. If pictures look wrong on yours (shadows,
  reflections, the crowd), please send a report (launcher: Report a problem).
- v0.2.4's CPU gains were measured on our desktop (Ryzen 9 5950X) only; on handhelds and laptops they are expected,
  not measured. If the crowd looks wrong in NATIVE (missing or flickering spectators), please send a report.
- The in-game GRAPHICS screen has no RENDERER choice yet (the launcher only).
- CineCam's setup panel (F8 outside a match) and the automatic entrance freeze are new and lightly tested.
