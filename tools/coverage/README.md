# tools/coverage: scenario coverage (Session 8 task 6)

Which guest functions run in each scenario (boot, title, main menu, Create a Wrestler, Create a Logo, entrances,
in-ring, pause menu, replay, ...). Everything derived from the game goes to `out\harness\coverage\` (gitignored).

## How it works

- **Separate build preset `win-amd64-coverage`** (`CMakePresets.json`, `-DWWE2K14_COVERAGE=ON`). Only there,
  `CMakeLists.txt` adds `src/coverage_prologue.h` to the generated code's precompiled header, after
  `generated/default/wwe2k14_pch.h`. It redefines `REX_FUNC_PROLOGUE()` (the first statement of every generated
  function) as: the original alignment assumption + a per-function `static uint32_t` "epoch seen" compared with a
  global epoch. Per call: two loads, a compare, a not-taken branch. The first call of a function in each window takes
  the slow path (`wwe2k14_cov_hit` in `src/coverage.h`): store the epoch, append `__func__` to the window's list.
- `generated/` is never touched; no SDK change; no `REXGLUE_ENABLE_PROFILING` (that define also changes
  `rex/dbg.h`, `rex/hook.h` and `rex/perf/counter.h`, a header/ODR mismatch against the release SDK DLLs).
- The normal presets do not see `coverage_prologue.h` and do not define `WWE2K14_COVERAGE`: `src/coverage.h` then
  only defines two no-op macros, used by `src/input_script.h`.
- **Setting** (coverage build only): `--wwe2k14_coverage=true` (default false = epoch 0 = nothing recorded),
  `--wwe2k14_coverage_dir` (default `../../harness/coverage`; a relative path is taken from the folder of
  `wwe2k14.exe`, whatever the working directory, so the default is always `out\harness\coverage`). If the run's
  folder already holds an older run (log numbers restarted), it is renamed to `<run>_replaced_<date>_<time>` first.
- **Windows**: every input-script `mark ID TEXT` closes the current window and writes
  `out\harness\coverage\<run>\NN_<TEXT>.txt` (sorted guest addresses of every function that ran at least once in the
  window). `quit` writes the last window (`NN_quit.txt`). `segments.tsv` has frames and counts; `run.txt` the script.
  The mark text names the window that **ends** at the mark. `scenario.part` texts group windows into a scenario
  (`in_ring.first60` + `in_ring.rest` = `in_ring`); `.first60` windows are the first 60 frames once a scenario's
  screen is up (the "window around a mark").
- **Transition windows** `to_<scenario>`: where a press moves to another screen (START in the match, A on REPLAY,
  A on a Create a Story animation), the press and the frames until the new screen is up are their own window,
  checked by a screenshot taken at its mark (`cov_pause_enter`, `cov_replay_enter`, `cov_story3_s2_300`). They
  belong to no scenario (like `quit`), so e.g. `pause_menu` holds no in-ring play from before the pause took effect.
  Load windows keep the press + load in the scenario they load, by name: `match_load`, `caw.loading`,
  `story_scene.editor_load`, `creations_menu` (the press on the main menu + the load); subtract them with
  `cov.py diff caw 003:caw.loading` if needed.
- **Import thunks**: the 270 `__imp__` entries of the generated table (kernel/XAM imports the SDK implements) have
  no `REX_FUNC_PROLOGUE`, so coverage cannot see them. They are not in `never` nor in the total (83,012
  instrumented functions); `cov.py func` says so for such an address.
- Addresses: parsed from the generated function name (`__imp__sub_822504B8` -> `0x822504B8`); the few named helpers
  (`__savegprlr_28`, ...) are mapped through the generated table in `generated/default/wwe2k14_init.cpp`.

## Use

    pwsh -NoProfile -File tools\runlock.ps1 run -Name cov-configure -- pwsh -NoProfile -File tools\coverage\configure.ps1
    pwsh -NoProfile -File tools\build.ps1 -Preset win-amd64-coverage -Run cov_001
    pwsh -NoProfile -File tools\coverage\run_coverage.ps1                    (all routes; or -Routes cov_logo,cov_caw)
    py tools\coverage\cov.py build                                           (coverage.json + summary.txt)
    py tools\coverage\cov.py sets
    py tools\coverage\cov.py unique caw
    py tools\coverage\cov.py diff in_ring entrances --count
    py tools\coverage\cov.py intersect logo caw
    py tools\coverage\cov.py range 0x82A00000 0x82B00000 replay
    py tools\coverage\cov.py func 0x822504B8
    py tools\coverage\cov.py show 012:in_ring.first60
    py tools\coverage\cov.py unique 012:pause_menu.first60     (a window against every scenario but its own)

Routes: `py tools\coverage\make_routes.py` regenerates `tools\coverage\routes\cov_*.txt` from the existing routes
(marks only, timing unchanged; the match route gets a pause-menu + replay tail; the story route is its own tail
on the Create a Wrestler route's start). Never edit the generated routes or the source routes for coverage: edit
`make_routes.py`. `run_coverage.ps1` stops if `make_routes.py` fails or a named route does not exist, and uses a
run only if it exited with code 0, logged no failure and wrote its `quit` window; any other run's folder is
renamed `<run>_failed` (not read by `cov.py`).

Screenshot checks: the BMPs stay in `out\build\win-amd64-coverage\captures`; any converted copies (PNG) go to
`out\harness\coverage\shots\`, never to a temp or scratch folder (hard rule: game-derived files only under
`out\harness`).

`coverage.json` (read by `tools\q fn`): `scenarios.<name>.functions|unique` (address lists), `functions.<addr>` (the
scenarios that ran it), `runs.<run>.windows` (frames, counts).

## Routes and scenarios (Session 8 coverage-build runs 001-012)

| Route | Scenarios (window texts) |
|---|---|
| `cov_title_menu` (selftest_title) | boot, title.first60/.rest, storage_notice, main_menu.first60/.rest |
| `cov_logo` (repro_logo_lettering) | boot, title_to_menu, main_menu, creations_menu, logo.grid/.editor/.stamps/.lettering |
| `cov_caw` (review_caw) | ..., caw.loading/.first60/.templates/.editor |
| `cov_match` (bench_lesnar_bryan) | ..., match_setup_menus, match_load, entrances.first60/.rest, entrance_to_match_load, in_ring.first60/.rest, `to_pause_menu` (START + 60 frames, transition), pause_menu.first60/.rest, `to_replay` (A on REPLAY + 60 frames, transition), replay.first60/.paused (viewer open, paused), replay_play.rt_hold (RT held = replay plays, screenshot-checked) / .after_release (paused again) |
| `cov_story` (review_caw start; built from three screenshot-checked probes, runs 004 / 006 / 009) | CREATIONS -> STORY -> NEW: story.menu, story.open ("Welcome to Create-A-Story!"), story.calendar, story.show_page, story.moment_type; SCENE: story_scene.editor_load (The Miz in the RAW arena), .anim_list, .anim_category; `to_story_cutscene` (A on BACKSTAGE ASSAULT + 300 frames: the list closes, the backstage set loads, the animation starts; transition); story_cutscene.anim_chosen (the animation playing in the editor), .icons (ADVANCED EDITOR), .after_icon_a (ADJUST LENGTH). Not the scene's playback inside a show (not reached) |

Run 001 was made by the exe before the rebuild that added the `script` line to `run.txt` (it shows as `run 001 ()`);
its data matches the later runs (identical boot set). Runs 004 / 006 / 009 used earlier probe routes (window names
`story.later`, `story_probe.p1`..`p5_later`, `story_scene.anim_chosen` / `.icons` / `.after_icon_a`); `cov.py` maps
those names to the scenarios above (`ALIASES`, each with the screenshot that shows it).

Runs 005 / 007 (match) and 009 / 010 (story) were made before the routes had transition windows: their
`pause_menu.first60`, `replay.first60` and `story_*.anim_chosen` windows start at the press. `cov.py` reads them as
the transitions (`LEGACY_TRANSITIONS`: no scenario; in the match route the old window is exactly the new
transition, press + 60 frames). Runs 011 (match) and 012 (story) use the current routes.

Overhead (for information; the coverage build is not for perf work): match 60.0 fps and entrances 30.7 / 30.1 fps in
both builds, boot to "Press Start" 18.9 s in both (runs 206 vs 001). CP busy in the match 5.47 / 5.90 ms (coverage
005 / 007) vs 4.60 / 5.02 / 4.93 ms (release 178 / 183 / 208), with more GPU kicks per frame in the coverage runs
(10.3 / 10.4 vs 8.4 / 9.7 / 8.5): busy per kick is the same range (0.53-0.57 vs 0.52-0.58 ms). Guest CPU time per
frame was not measured directly (frames are capped at 60 so it does not show up in fps).

## Limits

- A guest function our `src/` code replaces (a hook overriding the weak `sub_` alias) only counts when the override
  calls the original body.
- Window edges are approximate by microseconds (a first call racing the mark lands in the next window).
- Coverage says a function ran, not what it did, nor how often.
- The instrumented build is slower (see the overhead numbers in the task result / PLAYBOOK); don't use it for perf.
- After codegen changes, rebuild the coverage preset (`tools\build.ps1 -Preset win-amd64-coverage`): it compiles
  the same `generated/default` sources, so it follows the manifest automatically.
