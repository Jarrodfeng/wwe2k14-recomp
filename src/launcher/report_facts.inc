R"RPT_TABLE(
# S108 lane RPT (docs\systems\problem_report.md): THE FACTS TABLE of the problem report's IN PLAY / PROBLEMS / SHORTFALLS / REPRODUCE blocks.
# ONE list, read by both twins: src\launcher\report_findings.h compiles this file in (the first and last lines make it a C++ raw string
# literal) and tools\report\triage.py reads it as text (dropping those two lines). A new fact is ONE row here, never a list in either reader.
# Row: KIND | ARG | TEXT | WRITER      (`#` starts a comment line; fields are split on " | ", trimmed)
#   build     MARKER  LABEL     the first line holding MARKER prints `LABEL: the rest of the line after MARKER`
#   renderer  MARKER  TEXT      any line holding MARKER prints TEXT once (in table order); none = "a stream build"
#   mod       FLAG    LABEL     a `modified: FLAG = 'V'` line with V not false / 0 / empty: LABEL is listed under mods on
#   modline   MARKER  LABEL     lines holding MARKER are counted: `LABEL: N line(s)`
#   problem   MARKER  -         a line holding MARKER is listed whole from MARKER on, deduplicated on that text
#   problem#  MARKER  -         the same, deduplicated with every digit run folded to #
#   shortfall PHRASE  MEANING   a census counter `... PHRASE N` (whole words) whose last value is not 0
#   line      MARKER  ROLE      a line the reader parses by its structure (ROLE names what for)
#   switch    ROLE    CVAR      a switch REPRODUCE's launch line prints (the reader takes the name from here, never from its own text)
# WRITER (linted by tools\report\triage_selftest.py, a moved or renamed writer FAILS it):
#   PATH            the file holds ARG;   PATH @ LIT  the file holds LIT;   -  not in our source (the SDK / a format of ours)
#   for mod and switch rows: PATH holds `REXCVAR_DEFINE_...(CVAR,` (the switch exists: a misspelt flag is silently ignored by the game)
build | [wwe2k14] version | version | src/wwe2k14_app.h
build | [wwe2k14] exe built (file time, local): | exe built | src/wwe2k14_app.h
build | [wwe2k14] quality tier: | quality tier | src/gfx_menu.h
renderer | [wwe2k14] built with WWE2K14_KEEP_STREAM=OFF | stream-free build: our scene host draws every frame | src/wwe2k14_app.h
renderer | [rdr][live] owner mode: the old path draws nothing | owner mode: the old path draws nothing | src/wwe2k14_app.h
mod | wwe2k14_mods | mods folder | src/mods.h
mod | wwe2k14_mods_lighting | lighting mods | src/mods.h
mod | wwe2k14_mods_superstars | superstar mods | src/mods.h
mod | wwe2k14_entrance_music | entrance music | src/entrance_music.h
mod | wwe2k14_dlc | DLC | src/dlc.h
modline | [wwe2k14][mods] titantron | titantron overrides | src/mods.h
modline | [wwe2k14][mods] lighting: | lighting mod lines | src/lighting_mod.h
problem | [rdr][PROBLEM] | - | src/renderer/problem.cpp
problem# | ROWS REFUSED | - | src/scene_live/weapon_rows_glue.h
problem | [wwe2k14][crash] | - | src/crash_handler.h
problem# | [hang] HANG? | - | src/hang_guard.h @ HANG? no
problem# | [hang] HANG: | - | src/hang_guard.h @ HANG: %s
problem# | [hang] BLACK: | - | src/hang_guard.h @ BLACK: no present
problem | [rdr][DEVICE_LOST] ===== | - | src/renderer/renderer.cpp
problem# | [wwe2k14][load] STALL: | - | src/load_probe.h
problem# | [wwe2k14][load] WATCH: | - | src/load_probe.h
problem# | [log] async sink dropped | - | thirdparty/rexglue-sdk-src/include/rex/logging/async_sink.h
shortfall | capped | rows / nodes cut at a constant (the rest has no row) | -
shortfall | overflow | a pool was full: the call / item was dropped | -
shortfall | unreadable | the game's bytes could not be read: not drawn | -
shortfall | slots refused | slots refused | -
shortfall | refused | a read or a draw was refused | -
shortfall | bad rows | rows the plan dropped as bad | -
shortfall | holes | something the game drew was not drawn | -
shortfall | hole | something the game drew was not drawn | -
shortfall | missing | data the game named was not there | -
shortfall | not in index | not found in the asset index | -
shortfall | no texture | no texture: drawn white / not drawn | -
shortfall | unnamed skipped | an unnamed object skipped | -
shortfall | fallback | a substitute was drawn | -
shortfall | stale | data older than the frame was used | -
shortfall | dropped | dropped | -
shortfall | late | arrived too late for its frame | -
shortfall | count unknown | the game's own count was unknown: 0 rows | -
shortfall | mgr missing | the manager object was not found | -
shortfall | no handle | no handle | -
shortfall | no header | no header | -
shortfall | no key | no key | -
line | [wwe2k14] modified: | modified | src/wwe2k14_app.h
line | [wwe2k14][scene] | scene | src/session_log.h
line | [wwe2k14][match] card ( | card | src/session_log.h
line | [wwe2k14][match] titles ( | titles | src/session_log.h
line | [wwe2k14][match] setup: | setup | src/session_log.h
line | [wwe2k14][match] setup+: | setup_more | src/session_log.h
line | [wwe2k14][dlc] pack | dlc | src/dlc.h
line | event prop id | prop | src/renderer_scene/live_assets.cpp
line | event prop made: id | prop_made | src/scene_live/weapon_rows_glue.h
line | Execution complete | end | -
line | Title terminated | end | -
switch | launch | wwe2k14_launch | src/scenario.h
switch | quick_type | wwe2k14_quick_type | src/scenario.h
switch | quick_pace | wwe2k14_quick_pace | src/scenario.h
switch | quick_rules | wwe2k14_quick_rules | src/scenario.h
switch | launch_arena | wwe2k14_launch_arena | src/scenario.h
switch | launch_participants | wwe2k14_launch_participants | src/scenario.h
)RPT_TABLE"
