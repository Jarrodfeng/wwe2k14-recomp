# Navigation library (Session 30, #2)

Snippets of input-script lines that take the game from one documented state to another. A route or a live batch
pulls one in with `include NAME` (src/input_script.h: looked up beside the script, then here, then in
tools/input_scripts; `.txt` may be left out). Live: `py tools\live\live.py goto NAME` (MCP `game_goto`).

Every snippet says in its header where it starts and where it ends (states from `states.toml`), and drives the menus
by state (`press_until_state`), so it works fast-forwarded (the menus drop presses there; a dropped press is pressed
again; `press_until_state ... if_needed` skips the press when the game already remembers the
cursor there). Snippets never change the speed (`cvar vsync`) or take screenshots: the route does that.

| Snippet | Start | End |
|---|---|---|
| `boot_to_menu` | game booting | main menu (menu 1, cursor 0 PLAY) |
| `menu_to_play` | main menu, any cursor | PLAY list (menu 2, cursor 0 ONE ON ONE) |
| `menu_to_creations` | main menu, any cursor | CREATIONS (menu 3, cursor 0) |
| `menu_to_options` | main menu, any cursor | OPTIONS (menu 4, cursor 0 IN-GAME MANUAL) |
| `menu_to_wm30` | main menu, any cursor (waits for menu 1 first) | 30 YEARS OF WRESTLEMANIA era cards (Session 34) |
| `back_to_menu` | a list opened from the main menu | main menu (menu 1, cursor kept) |
| `creations_to_logo_grid` | CREATIONS, cursor 0 | Create a Logo grid ready (logo_phase 124) |
| `logo_grid_to_editor` | logo grid ready, slot 1 empty | editor, new 256x256 logo (logo_screen 3) |
| `editor_to_categories` | editor, Tools closed | LOGO CATEGORIES (logo_screen 2, W stamp tool) |

Chains used by routes: `boot_to_menu` + `menu_to_creations` + `creations_to_logo_grid` + `logo_grid_to_editor` +
`editor_to_categories` = the T2 logo route up to the categories (`repro_logo_lettering.txt`).

Main menu items (docs/systems/menus.md): 0 PLAY, 1 30 YEARS OF WRESTLEMANIA, 2 UNIVERSE, 3 CREATIONS, 4 Xbox Live,
5 MY WWE, 6 OPTIONS, 7 SHOP, 8 QUIT TO DESKTOP.
