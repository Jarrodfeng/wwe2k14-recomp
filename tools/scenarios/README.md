# Scenario launcher (Session 9)

Set up a match once in the game's own menus (**capture**), then start it straight from the main menu any time
(**launch**), skipping the menus. Code: `src/scenario.h` (+ hooks in `config/scenario_hooks.toml`); how it works and
the evidence: `docs/systems/match_setup.md` and `PLAYBOOK.md` Session 9. Default off: nothing here runs unless one of
the settings below is set.

**Scenario files are local only** (`out\scenarios\`, gitignored; Jarrod's decision): they are copies of game memory.
Only these tools are committed.

## Capture

Start the game with `--wwe2k14_scenario_capture=NAME` and set up matches in the menus as usual. Each time the menus
apply a match (the moment you confirm it, before anything loads) four files are written:

| File | What | Used by the launch |
|---|---|---|
| `NAME.m.bin` | the match description M (154,664 bytes) as the menus left it: match type, arena, pace (M+528), participants and their slots, rules | written back |
| `NAME.x.bin` | extras (32 bytes): the menus' arena record in P and their active controller index | written back |
| `NAME.g.bin` / `NAME.a.bin` | the settings struct G and the participant slots A just after apply | compared; written back when they differ (Royal Rumble entry order) |
| `NAME.toml` | readable summary: type, mode, arena, participants, extra ids, the menus' arena record and pad, file hashes, setter calls (`list.py` also shows the pace, read from M+528) | read (hashes) |

Never overwrites: a taken name becomes `NAME_2`, `NAME_3`, ... Every match set up in one session is captured. A hands-on
capture session on the frozen user data (one line):

```
& C:\Users\Jarrod\wwe2k14-recomp\tools\bench\run_list.ps1 -Scripts @('C:\Users\Jarrod\wwe2k14-recomp\tools\input_scripts\manual.txt') -Extra '--wwe2k14_scenario_capture=mymatch' -WaitSeconds 7200
```

(`manual.txt` presses nothing: press through the title yourself. Add `--wwe2k14_input_record=..\..\recordings\NAME.txt`
to record your inputs as well.)

## List, check, rename

```
py tools\scenarios\list.py
py tools\scenarios\list.py NAME
py tools\scenarios\list.py --rename OLD NEW
py tools\scenarios\list.py --describe NAME "text"
```

`list.py` checks every file's size and hash and that the summary equals the bytes. `--rename` never overwrites.

## Launch

`--wwe2k14_launch=NAME`: once the main menu has been idle for 60 frames (`--wwe2k14_launch_idle_frames`), the launcher
restores the scenario, runs the game's own apply and the steps finalize runs after it, and leaves the main menu the
game's own way (exit action 1). From boot to the first titantron takes ~20 s less than the menu route.

Watched (the script presses through the title, then the controller is yours; one line):

```
& C:\Users\Jarrod\wwe2k14-recomp\tools\bench\run_list.ps1 -Scripts @('C:\Users\Jarrod\wwe2k14-recomp\tools\input_scripts\launch_boot_watch.txt') -Extra '--wwe2k14_launch=rumble_40' -WaitSeconds 3600
```

Unattended check (quits 900 frames after the in-game sequence starts; up to 3 at once with `-Parallel 3`):
`tools\input_scripts\short\launch_boot_check.txt`. Check the log's `[wwe2k14][scenario] launch` lines and the
`BGnn.PAC` / `CHnnn.PAC` / `NNN.bik2` opens.

### Overrides (through the game's own setters)

| Setting | Effect |
|---|---|
| `--wwe2k14_launch_arena=N` | another arena (ids in `db\ids\arenas.json`; locked WrestleMania arenas 28-55 work: run 492) |
| `--wwe2k14_launch_participants=ID[:PAD],...` | other wrestlers for slots 0, 1, ... (ids in `db\ids\characters.json`; PAD 0-3 = that controller, default CPU) |
| `--wwe2k14_launch_restore_ga=false` | keep the G / A this apply made (e.g. a fresh random Rumble entry order) |
| `--wwe2k14_scenario_dir=DIR` | another scenario folder |

Example: Stone Cold vs The Rock at WrestleMania X-Seven from the `lesnar_bryan` capture:
`--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_arena=44 --wwe2k14_launch_participants=101,100`. With an override the
captured G / A no longer match, so they are not written back (a Rumble then gets a fresh entry order).

## Diagnostic tools

| Tool | Use |
|---|---|
| `--wwe2k14_scenario_observe=true` | log every match-setup call (setters, finalize, apply, launch, dispatcher, sequence switch) with arguments and caller; snapshot M / G / A / the menu state object / the static data / P to `logs\scenario_NNN_*.bin` |
| `--wwe2k14_snapshot_trigger=PATH` | on-demand snapshot: write a NAME into the file PATH and the game saves the static data + all 512 MB of physical memory (`logs\snap_NNN_NAME_D/H.bin`) within 30 frames |
| `diff_bin.py OLD NEW [--layout M/G/A]` | changed words between two snapshots, by region (header / entry i / slot i) |
| `diff_runs.py MENU_A MENU_B LAUNCH [--base ADDR]` | words two menu runs agree on that a launch has differently (how the missing wrestler list was found) |
| `--wwe2k14_launch_poke=ADDR=VALUE,...` | debug: write words (absolute, `P+off`, `S+off`) just before the launcher's exit request |

## Known limits

- A launch uses the save it runs with for anything the save holds (title holders, unlocks): use the frozen user data
  (`run_list.ps1` does) for repeatable runs.
- The game's attract-mode demo sets demo-mode values; start launches with `launch_boot_watch.txt` or
  `short\launch_boot_check.txt` so the title never times out.
- Presses made while the launcher fires can arrive when the match starts (keystroke buffering, unconfirmed).
- Captures from before scenario format 2 (no `.x.bin`) are refused: capture again.
- 21 flag-shaped words of static data still differ between a menu run and a launch (no visible effect; PLAYBOOK).
