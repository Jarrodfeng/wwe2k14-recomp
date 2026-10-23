# Session 53 launcher / direct-start tests (items 2.1, 2.2, 2.4, 2.6)

Scripts used to reproduce and check the Session 53 fixes. They expect their inputs in `out\s53\` (settings files from
`mk_settings.py`, args files, a test user-data copy of `out\bench\userdata_frozen`) and a release-configured build from
`relbuild.ps1` (`out\s53\relbuild`, version suffix `-s53test`) copied to a folder **outside the repo** (inside it, the
exe finds `game_data\extracted` by searching upwards and the bug does not show). Run each through `tools\runlock.ps1 run`.

- `direct_test.ps1 -Exe X -ArgsFile F [-KeepLauncherAway]`: starts an exe, reports processes, their windows (visible or
  not), command lines and the game log's path lines.
- `close_test.ps1 -Launcher X -ArgsFile F -How close|crash`: QUICK START, then the game closed normally (the launcher
  must exit) or killed (the launcher must show its crash window).
- `launcher_ui_test.ps1 -Launcher X`: desktop shortcut into `out\s53\shortcut`, a shot of Settings -> GAME, and a key
  (posted to the launcher's own window) cancelling QUICK START.
- `previews_test.ps1 -Launcher X`: Build previews into `out\s53\localdata\trons` (`--local-data`), counts the MP4s.
- `kb_run.ps1 -Script kb_set.txt|kb_check.txt [-More FLAGS]`: dev build, test save folder `out\s53\userdata_kb`:
  binds changed with `cvar` (what the F4 overlay's Rebind does), then read back at the next start.
- Never Jarrod's own settings or save: every run passes a test settings file (`--settings` for the launcher,
  `--wwe2k14_settings_file` for the game) and a test save folder.
