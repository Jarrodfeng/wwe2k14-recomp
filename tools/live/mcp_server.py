"""Session 29: MCP server (stdio) for the game's live control channel (tools/live/live.py, src/live_control.h).

Registered in .mcp.json as "wwe2k14-live" (Claude Code asks once before it starts a project MCP server). Tools:
  game_start       boot a game with the live channel (run_list: frozen user data, run lock, test settings file)
  game_run         send input-script lines (press, wait_state, log_state, cvar, check, ...); optional screenshot
  game_screenshot  a screenshot, returned as an image (JPEG, default 1280 px wide)
  game_check       an on-screen question Jarrod answers with F1 / F2 / F5
  game_status / game_stop / game_quit
Screenshots come back inside the tool result, so looking costs one call. No dependencies (JSON-RPC over stdio).
"""
import atexit
import base64
import json
import os
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live  # noqa: E402

NAME_PROP = {'type': 'string', 'description': 'live channel name (default "main"; one per game)'}
TOOLS = [
    {'name': 'game_start',
     'description': 'Boot WWE 2K14 with the live control channel through run_list (frozen user data, run lock, a test '
                    'settings file). script: "menu" (boot to the main menu, default), "idle" (title screen) or a '
                    'route file. launch: a scenario name (--wwe2k14_launch). fast: fast-forward (default: normal '
                    'speed, for Jarrod to watch). extra: more game flags. window: fullscreen without the focus, '
                    '"behind" other windows (default) or "visible" on top; "windowed" (1280x720, on top) or '
                    '"foreground" (the old activated window).',
     'inputSchema': {'type': 'object', 'properties': {
         'name': NAME_PROP, 'script': {'type': 'string'}, 'launch': {'type': 'string'},
         'fast': {'type': 'boolean'}, 'extra': {'type': 'string'},
         'window': {'type': 'string', 'enum': ['behind', 'visible', 'windowed', 'foreground']}}}},
    {'name': 'game_run',
     'description': 'Run input-script lines in the live game (src/input_script.h commands: press BUTTONS [HOLD] [GAP], '
                    'set, wait_frames N, wait_file TEXT, wait_state NAME VALUE [TIMEOUT], press_until_state, '
                    'log_state NAME, assert_state, cvar NAME VALUE, screenshot LABEL, check TEXT, ...). Waits until '
                    'the batch ends; returns status, frames and the batch log lines; screenshot=true adds one at '
                    'the end and returns it as an image.',
     'inputSchema': {'type': 'object', 'required': ['lines'], 'properties': {
         'lines': {'type': 'string', 'description': 'script lines, one per line'},
         'screenshot': {'type': 'boolean'}, 'width': {'type': 'integer'},
         'timeout': {'type': 'integer', 'description': 'seconds (default 120)'},
         'log': {'type': 'string', 'enum': ['ours', 'files', 'all', 'none']}, 'name': NAME_PROP}}},
    {'name': 'game_goto',
     'description': 'Run navigation snippets from tools/input_scripts/nav (e.g. "menu_to_options", "boot_to_menu"; '
                    'each file header says where it starts and ends; README.md lists them) in the live game, in order. '
                    'Same answer as game_run.',
     'inputSchema': {'type': 'object', 'required': ['names'], 'properties': {
         'names': {'type': 'string', 'description': 'snippet names, separated by spaces'},
         'screenshot': {'type': 'boolean'}, 'width': {'type': 'integer'},
         'timeout': {'type': 'integer', 'description': 'seconds (default 120)'}, 'name': NAME_PROP}}},
    {'name': 'game_where',
     'description': 'Session 91 navigator: which screen the live game is on (atlas node id and its path name from the '
                    'main menu), the highlighted item ("item i of n", its label, where A leads), the raw states.',
     'inputSchema': {'type': 'object', 'properties': {'name': NAME_PROP}}},
    {'name': 'game_nav',
     'description': 'Session 91 navigator: go to any screen of the atlas (docs/systems/game_atlas.json) from wherever '
                    'the game is. target: a node id (list:7, setup:0, rules:49, screen:1300, mode:caw, ...) or words '
                    'of its path name ("one on one > ladder", "options", "create a superstar", "royal rumble > 30"); '
                    'ambiguous words return the choices. Presses are state-confirmed, wrong screens re-plan, unknown '
                    'screens recover with B. Returns the plan log and where it ended.',
     'inputSchema': {'type': 'object', 'required': ['target'], 'properties': {
         'target': {'type': 'string'}, 'screenshot': {'type': 'boolean'}, 'name': NAME_PROP}}},
    {'name': 'game_match',
     'description': 'Session 91 match builder: build and launch any match from names, through the game\'s own menu '
                    'path (tools/atlas/match.py; the navigator first takes the game to the main menu). spec: parts '
                    'separated by ";" - the wrestlers ("Daniel Bryan vs Brock Lesnar" or "Cena, Orton, Bryan, ..."), '
                    'type=<path name or number> ("one on one > ladder", "royal rumble > 30"), arena=<name>, '
                    'pace=normal|quick|epic, players=<humans, default 1>, rumble=<N random | names>, title=T0/T1, '
                    'o=K:V raw rule bytes. Names match by their words in any order (fewest extra words wins: "Cena" '
                    '= John Cena, "Retro Cena" = John Cena (Retro)); an unsure name returns candidates "Name (#id)": '
                    'pick one and pass "#id" in its place. lookup="words" (what=wrestler|arena) lists candidates '
                    'with ids without building. dry=true returns the numeric spec only. Returns when the match '
                    'sequence starts.',
     'inputSchema': {'type': 'object', 'properties': {
         'spec': {'type': 'string'}, 'lookup': {'type': 'string'},
         'what': {'type': 'string', 'enum': ['wrestler', 'arena']},
         'dry': {'type': 'boolean'}, 'compare': {'type': 'string'},
         'screenshot': {'type': 'boolean'}, 'name': NAME_PROP}}},
    {'name': 'game_events',
     'description': 'Session 91 event bus: the game\'s transitions as they happened (src/event_bus.h; on in every '
                    'live game): seq (mode changes), screen (menu id / screen mode), exit (exit actions), msgbox, '
                    'match (clock_started, finish), entrance (step per slot), load (archives). last=N (default 30), '
                    'kind="seq,match" to filter.',
     'inputSchema': {'type': 'object', 'properties': {'last': {'type': 'integer'}, 'kind': {'type': 'string'},
                                                       'name': NAME_PROP}}},
    # S101 goto (N1): the go-anywhere verb (tools/goto/goto.py, docs/systems/goto.md)
    {'name': 'game_go',
     'description': 'S101 go-anywhere: take the live game to an ADDRESS through the game\'s own functions and states (never timed '
                    'presses). Forms: menu:PATH (an atlas node id or the words of its path name), match:SPEC (the game_match spec; '
                    'optional @entrance:N and @bell suffixes wait on the event bus), wm30:N (30 Years of WrestleMania match 0-45), '
                    'wm30:N@moment:K (K = 0 / 1 / 2 for the match\'s X00 / X01 / X02 SCEX code, or a raw code), and the parsed-only '
                    'caw:CATEGORY/ITEM, universe:PAGE, cutscene:ID (not reachable yet, with the reason). dry=true returns the plan with no game. '
                    'A goto that cannot arrive says where the game is and takes the bookmark bundle.',
     'inputSchema': {'type': 'object', 'required': ['address'], 'properties': {
         'address': {'type': 'string'}, 'dry': {'type': 'boolean'}, 'name': NAME_PROP}}},
    {'name': 'game_match_state',
     'description': 'Session 91 match state snapshot (src/event_bus.h): the match clock (frames), the referee\'s mode '
                    '(0 watch, 1 pin count, 3 DQ count, 5 count-out) and count, the finish (type + name, winner / '
                    'loser), and every wrestler: slot, character id (db/ids/characters.json), team, role (1 = referee), '
                    'state (0 active, 1 apron, 4 eliminated, 5 over the top), health / max. Names are filled in from '
                    'db/ids. "not in a match" outside one.',
     'inputSchema': {'type': 'object', 'properties': {'name': NAME_PROP}}},
    {'name': 'game_screenshot',
     'description': 'Screenshot of the live game, returned as an image (default 1280 px wide).',
     'inputSchema': {'type': 'object', 'properties': {'width': {'type': 'integer'}, 'name': NAME_PROP}}},
    {'name': 'game_check',
     'description': 'Show a question on the game screen; Jarrod answers F1 (yes / looks right), F2 (no / wrong) or F5 '
                    '(skip). Returns PASS / FAIL / SKIP / TIMEOUT.',
     'inputSchema': {'type': 'object', 'required': ['question'], 'properties': {
         'question': {'type': 'string'}, 'name': NAME_PROP}}},
    {'name': 'game_bookmark',
     'description': 'The "that, just now" bundle (as Jarrod\'s F11 key): screenshot (returned as an image), every '
                    'state value, the last 5 s of frame times, warnings from the last 10 s.',
     'inputSchema': {'type': 'object', 'properties': {'width': {'type': 'integer'}, 'name': NAME_PROP}}},
    {'name': 'game_window',
     'description': 'Coverage build only (game_start with extra "--wwe2k14_coverage=true" and the coverage exe, or live.py '
                    'start --coverage): close the coverage window that ran since the last one under LABEL. Then '
                    'game_diffcov B A lists the functions that ran in window B and not in A.',
     'inputSchema': {'type': 'object', 'required': ['label'], 'properties': {'label': {'type': 'string'},
                                                                           'name': NAME_PROP}}},
    {'name': 'game_diffcov',
     'description': 'Functions that ran in coverage window B but not in window A (or not in any other window), rarest '
                    'first, with function-database names (tools/coverage/diffcov.py).',
     'inputSchema': {'type': 'object', 'required': ['b'], 'properties': {'b': {'type': 'string'}, 'a': {'type': 'string'},
                                                                       'top': {'type': 'integer'}, 'name': NAME_PROP}}},
    {'name': 'game_text', 'description': 'The text on screen now (strings the game fetched through its text manager: '
                                         'menu labels, notices) and the 40 newest distinct strings. No screenshot.',
     'inputSchema': {'type': 'object', 'properties': {'name': NAME_PROP}}},
    {'name': 'game_status', 'description': 'Run number, guest frame, fps now (1 s / 5 s), menu and scene states, '
                                           'text on screen, bookmarks taken, whether a script runs.',
     'inputSchema': {'type': 'object', 'properties': {'name': NAME_PROP}}},
    {'name': 'game_stop', 'description': 'End the script or batch that runs now (a long wait).',
     'inputSchema': {'type': 'object', 'properties': {'name': NAME_PROP}}},
    {'name': 'game_quit', 'description': 'Close the live game (like Alt+F4); run_list then frees the run lock.',
     'inputSchema': {'type': 'object', 'properties': {'name': NAME_PROP}}},
    # Session 96: the asset viewer (wwe2k14_assets.exe, src\assets\view\; tools\assets\viewer_link.py). Not the game:
    # no run lock, no game run (CLAUDE.md rule 7 counts game runs); it reads the player's own files read-only.
    {'name': 'asset_start',
     'description': 'Start the asset viewer (our own program: loads arenas and wrestlers straight from the game files, '
                    'no game boot). visible=true opens a window Jarrod can see; default hidden (shots still work).',
     'inputSchema': {'type': 'object', 'properties': {'visible': {'type': 'boolean'}}}},
    {'name': 'asset_load',
     'description': 'Load an arena or a wrestler into the asset viewer (replaces what is shown; add=true keeps it, e.g. '
                    'a wrestler in an arena, with at=[x,y,z]). Returns meshes, vertices, textures, loader warnings, '
                    'times. shot=preset name (front, side, three_quarter, top, close, ring) also returns a picture.',
     'inputSchema': {'type': 'object', 'required': ['kind', 'id'], 'properties': {
         'kind': {'type': 'string', 'enum': ['arena', 'wrestler']}, 'id': {'type': 'integer'},
         'attire': {'type': 'integer'}, 'add': {'type': 'boolean'},
         'at': {'type': 'array', 'items': {'type': 'number'}}, 'shot': {'type': 'string'}}}},
    {'name': 'asset_view',
     'description': 'Point the asset viewer camera (preset, or yaw / pitch / dist / target / fov) and return a picture.',
     'inputSchema': {'type': 'object', 'properties': {
         'preset': {'type': 'string'}, 'yaw': {'type': 'number'}, 'pitch': {'type': 'number'},
         'dist': {'type': 'number'}, 'fov': {'type': 'number'},
         'target': {'type': 'array', 'items': {'type': 'number'}}, 'width': {'type': 'integer'}}}},
    {'name': 'asset_cam',
     'description': 'Asset viewer free camera pose (Session 98). No pose fields (or get=true): returns the full pose {mode, pos, yaw, pitch, roll, fov, orbit}. '
                    'With pos [x,y,z] / yaw / pitch / roll (degrees) / fov: sets the free camera (missing fields keep the current pose; '
                    'mode="orbit" goes back to the orbit camera afterwards) and returns the pose. shot=true also returns a picture, so a shot can be repeated.',
     'inputSchema': {'type': 'object', 'properties': {
         'get': {'type': 'boolean'}, 'pos': {'type': 'array', 'items': {'type': 'number'}}, 'yaw': {'type': 'number'},
         'pitch': {'type': 'number'}, 'roll': {'type': 'number'}, 'fov': {'type': 'number'},
         'mode': {'type': 'string', 'enum': ['free', 'orbit']}, 'shot': {'type': 'boolean'}, 'width': {'type': 'integer'}}}},
    {'name': 'asset_command',
     'description': 'Send one raw command to the asset viewer pipe, e.g. {"cmd":"list","kind":"arena"} or '
                    '{"cmd":"stats"}; returns its JSON answer.',
     'inputSchema': {'type': 'object', 'required': ['command'], 'properties': {'command': {'type': 'object'}}}},
    {'name': 'asset_motions',
     'description': 'List the motions in the motion pacs of the game (asset viewer, Session 97). No arguments: the pac files and their '
                    'banks. file (mot00..mot08): its banks. file+bank: the (top, act) pairs, paged (offset, limit). '
                    'file+bank+top+act: the motions of that act (name, frames, tracks). Only the full-detail (ZLIB) half is read.',
     'inputSchema': {'type': 'object', 'properties': {
         'file': {'type': 'string'}, 'bank': {'type': 'string'}, 'top': {'type': 'integer'}, 'act': {'type': 'integer'},
         'offset': {'type': 'integer'}, 'limit': {'type': 'integer'}}}},
    {'name': 'asset_motion',
     'description': 'Pose the loaded wrestler(s) with a real motion (asset viewer, Session 97). Give file / bank / top / act / name '
                    '(default name="idle" = each wrestler\'s own in-ring stance, mot00 CA01 top 30 act 29000-29022 "00000000" picked '
                    'per character like the game; name="crouch" = the old knees-bent idle mot00 CA01 40 23101), or leave them out to '
                    'keep the current motion. frame=N sets the frame (60 per second; a stance has 200-270), play=true/false animates in a '
                    'visible window, pose="bind" returns to the T-pose, pose="motion" back. shot=preset also returns a picture. '
                    'Legs follow our own two-bone solve (the original solver is not reproduced).',
     'inputSchema': {'type': 'object', 'properties': {
         'file': {'type': 'string'}, 'bank': {'type': 'string'}, 'top': {'type': 'integer'}, 'act': {'type': 'integer'},
         'name': {'type': 'string'}, 'frame': {'type': 'number'}, 'play': {'type': 'boolean'},
         'pose': {'type': 'string', 'enum': ['bind', 'motion']}, 'shot': {'type': 'string'},
         'actor': {'type': 'string', 'description': 'an actor id (asset_place); default: every actor, or the one loaded wrestler'}}}},
    {'name': 'asset_stage',
     'description': 'Tron preview in the asset viewer (Session 97): dress the loaded arena\'s entrance stage with wrestler N\'s tron on the main '
                    'rectangle (big screen + banner ring) and his MVSW / MVSB movies on the other stage parts (fallback: the arena\'s MOV / MVAB). '
                    'wrestler=N sets it (-1 = the match look: arena movies, the screen dark = the live camera feed), step=1 / -1 moves to the next / '
                    'previous wrestler that has media, list=true lists them (and the media folder). media = a folder of the game\'s tron export '
                    '(default: the launcher\'s tron cache; none -> a note "run the tron export"). The arena is not reloaded from disk by you: '
                    'load one first. shot=preset (e.g. front) also returns a picture.',
     'inputSchema': {'type': 'object', 'properties': {
         'wrestler': {'type': 'integer'}, 'step': {'type': 'integer'}, 'list': {'type': 'boolean'}, 'media': {'type': 'string'},
         'shot': {'type': 'string'}, 'width': {'type': 'integer'}}}},
    {'name': 'asset_place',
     'description': 'Place a wrestler (an actor) in the loaded arena of the asset viewer (Session 98 scenes). spot = stage | ramp | ring | '
                    'ring_ne | ring_nw | ring_se | ring_sw | ringside (ramp: t 0 = stage end .. 1 = ring end); or pos=[x,y,z] for a free '
                    'position. Each actor has its own position and facing (yaw override in degrees: 0 faces the stage, 180 faces the ring '
                    'from the stage), attire, motion (default the idle; name + top + act + file + bank pick another, as asset_motion) and frame, '
                    'and stands on the floor of its spot. Lit by the zone light set at the stage / ramp, the default rig elsewhere. '
                    'id names the actor (placing again with the same id replaces it). Load an arena first (asset_load). '
                    'shot=true also returns a picture from the spot camera (side / dist / pitch / fov tune it).',
     'inputSchema': {'type': 'object', 'required': ['wrestler'], 'properties': {
         'wrestler': {'type': 'integer'}, 'attire': {'type': 'integer'}, 'spot': {'type': 'string'}, 't': {'type': 'number'},
         'pos': {'type': 'array', 'items': {'type': 'number'}}, 'yaw': {'type': 'number'}, 'id': {'type': 'string'},
         'motion': {'type': 'string'}, 'frame': {'type': 'number'}, 'file': {'type': 'string'}, 'bank': {'type': 'string'},
         'top': {'type': 'integer'}, 'act': {'type': 'integer'}, 'light': {}, 'ground': {'type': 'boolean'},
         'anchor': {'type': 'boolean'}, 'shot': {'type': 'boolean'}, 'side': {'type': 'number'}, 'dist': {'type': 'number'},
         'pitch': {'type': 'number'}, 'fov': {'type': 'number'}, 'width': {'type': 'integer'}}}},
    {'name': 'asset_actors',
     'description': 'List the actors placed in the asset viewer (id, wrestler, spot, position, yaw, feet height, motion, frame). remove=ID '
                    'removes one, clear=true removes all. spot_cam=SPOT (or an actor id with actor=ID) also returns a picture from the '
                    'camera that frames that spot (t for the ramp).',
     'inputSchema': {'type': 'object', 'properties': {
         'remove': {'type': 'string'}, 'clear': {'type': 'boolean'}, 'spot_cam': {'type': 'string'}, 'actor': {'type': 'string'},
         't': {'type': 'number'}, 'width': {'type': 'integer'}}}},
    {'name': 'asset_path',
     'description': 'Camera paths + offline renders in the asset viewer (Session 98 step 12). action = add (key at time t from the current camera, or '
                    'with pos / yaw / pitch / roll / fov / ease_in / ease_out 0..1), clear, list, set_key (index + fields, from_camera=true), delete '
                    '(index), set (loop, link = the scene clock follows the path time), time (t = put the camera + linked actors at path time t; '
                    'returns the pose), play (on, from), save / load (path = a file, or a bare name in %LOCALAPPDATA%\\wwe2k14\\viewer\\paths), '
                    'render (path = file or "current", out = folder, w, h, fps, ss = supersampling 1..4, motion_blur = shutter 0..1, samples = '
                    'sub-frames, t0 / t1, scene, mp4 = also encode with ffmpeg if it is on PATH; wait=true polls to the end, else it starts and '
                    'you call render_status / render_cancel). Frames are frame_00000.png ... + camera.csv (per-frame pose); deterministic.',
     'inputSchema': {'type': 'object', 'required': ['action'], 'properties': {
         'action': {'type': 'string', 'enum': ['add', 'clear', 'list', 'set_key', 'delete', 'set', 'time', 'play', 'save', 'load', 'render',
                                               'render_status', 'render_cancel']},
         't': {'type': 'number'}, 'index': {'type': 'integer'}, 'pos': {'type': 'array', 'items': {'type': 'number'}},
         'yaw': {'type': 'number'}, 'pitch': {'type': 'number'}, 'roll': {'type': 'number'}, 'fov': {'type': 'number'},
         'ease_in': {'type': 'number'}, 'ease_out': {'type': 'number'}, 'from_camera': {'type': 'boolean'},
         'loop': {'type': 'boolean'}, 'link': {'type': 'boolean'}, 'on': {'type': 'boolean'}, 'from': {'type': 'number'},
         'path': {'type': 'string'}, 'out': {'type': 'string'}, 'w': {'type': 'integer'}, 'h': {'type': 'integer'},
         'fps': {'type': 'integer'}, 'ss': {'type': 'integer'}, 'motion_blur': {'type': 'number'}, 'samples': {'type': 'integer'},
         't0': {'type': 'number'}, 't1': {'type': 'number'}, 'scene': {'type': 'boolean'}, 'mp4': {'type': 'boolean'},
         'wait': {'type': 'boolean'}}}},
    {'name': 'asset_entrance',
     'description': 'Play a wrestler\'s whole entrance in the asset viewer (Session 98 step 6). With wrestler=ID: places him (the actor "ent") at his '
                    'walk and plays act 25000\'s cues 0 / 5 / 10 / 15 / 20 back to back (hard cuts), with his entrance lights and camera. '
                    'Options: attire, cue (start there) + f (local frame), t (seconds), play, speed, root_y = ground (default) | raw | scale, '
                    'camera = free | entrance, take, join = chain | absolute, intro_hold (seconds cue 0 is held), cue0 = hide | show, '
                    'light_dirs = keyed | arena, lights, stage (his tron / stage look, reloads the arena), cam_dy. Without wrestler it moves '
                    'the running entrance (cue / f / t / play / pause / speed / camera / take / root_y ...); info=true only returns the cue list, '
                    'lengths, the current cue and time and the join distances. Load an arena first (asset_load). shot=true returns a picture.',
     'inputSchema': {'type': 'object', 'properties': {
         'wrestler': {'type': 'integer'}, 'attire': {'type': 'integer'}, 'cue': {'type': 'integer'}, 'f': {'type': 'number'},
         't': {'type': 'number'}, 'play': {'type': 'boolean'}, 'pause': {'type': 'boolean'}, 'speed': {'type': 'number'},
         'root_y': {'type': 'string', 'enum': ['ground', 'raw', 'scale']}, 'camera': {'type': 'string', 'enum': ['free', 'entrance']},
         'take': {'type': 'integer'}, 'join': {'type': 'string', 'enum': ['chain', 'absolute']}, 'intro_hold': {'type': 'number'},
         'cue0': {'type': 'string', 'enum': ['hide', 'show']}, 'light_dirs': {'type': 'string', 'enum': ['keyed', 'arena']},
         'lights': {'type': 'boolean'}, 'stage': {'type': 'boolean'}, 'cam_dy': {'type': 'number'}, 'info': {'type': 'boolean'},
         'shot': {'type': 'boolean'}, 'width': {'type': 'integer'}}}},
    {'name': 'asset_stop', 'description': 'Close the asset viewer.',
     'inputSchema': {'type': 'object', 'properties': {}}},
]

_ASSET = {}  # the one viewer this server drives


def _asset_viewer():
    sys.path.insert(0, os.path.join(live.REPO, 'tools', 'assets'))
    import viewer_link
    if 'v' not in _ASSET:
        _ASSET['v'] = viewer_link.Viewer(name='mcp')
        atexit.register(_ASSET['v'].stop)  # (the viewer also exits by itself when this process ends: --parent)
    return _ASSET['v']


_ASSET_N = [0]


def _asset_shot(v, width):
    _ASSET_N[0] += 1
    path = os.path.join(live.REPO, 'out', 'assets', 'mcp', 'view_%s_%03d.png' % (time.strftime('%Y%m%d_%H%M%S'), _ASSET_N[0]))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        os.remove(path)
    r = v.send({'cmd': 'shot', 'path': path, 'w': width, 'h': width * 9 // 16})
    if not r.get('ok', False) or not os.path.exists(path):  # (success = the viewer said ok AND this file is new)
        return [text('shot failed: %s' % json.dumps(r))]
    with open(path, 'rb') as f:
        return [{'type': 'image', 'data': base64.b64encode(f.read()).decode('ascii'), 'mimeType': 'image/png'}]


def _asset_cam(v, args):  # Session 98 (freecam): cam_get / cam_set
    fields = ('pos', 'yaw', 'pitch', 'roll', 'fov', 'mode')
    if args.get('get') or not any(k in args for k in fields):
        r = v.send({'cmd': 'cam_get'})
    else:
        cmd = {'cmd': 'cam_set'}
        cmd.update({k: args[k] for k in fields if k in args})
        r = v.send(cmd)
    out = [text(json.dumps(r))]
    if args.get('shot') and r.get('ok', False):
        out += _asset_shot(v, int(args.get('width') or 1280))
    return out, not r.get('ok', True)


def _asset_path(v, args):  # Session 98 (step 12): path_* / render commands
    action = args.get('action')
    if action in ('render', 'render_status', 'render_cancel'):
        cmd = {'cmd': action}
    else:
        cmd = {'cmd': 'path_' + str(action)}
    cmd.update({k: val for k, val in args.items() if k not in ('action', 'wait', 'name')})
    r = v.send(cmd)
    if action == 'render' and args.get('wait') and r.get('ok', False):
        deadline = time.time() + 3600
        while r.get('state') in (None, 'rendering', 'encoding') and time.time() < deadline:
            time.sleep(2)
            r = v.send({'cmd': 'render_status'})
    return [text(json.dumps(r, indent=1))], not r.get('ok', True)


def _asset_entrance(v, args):  # Session 98 (step 6): entrance / entrance_time / entrance_info
    keys = ('wrestler', 'attire', 'cue', 'f', 't', 'play', 'pause', 'speed', 'root_y', 'camera', 'take', 'join', 'intro_hold', 'cue0',
            'light_dirs', 'lights', 'stage', 'cam_dy')
    if args.get('info') and 'wrestler' not in args:
        r = v.send({'cmd': 'entrance_info'})
    else:
        cmd = {'cmd': 'entrance' if 'wrestler' in args else 'entrance_time'}
        cmd.update({k: args[k] for k in keys if k in args})
        r = v.send(cmd, timeout=180.0)
    out = [text(json.dumps(r, indent=1))]
    if args.get('shot') and r.get('ok', False):
        out += _asset_shot(v, int(args.get('width') or 1280))
    return out, not r.get('ok', True)


def asset_call(tool, args):
    v = _asset_viewer()
    if tool == 'asset_entrance':
        return _asset_entrance(v, args)
    if tool == 'asset_start':
        v.start(hidden=not args.get('visible'))
        return [text('asset viewer started (pipe %s)' % v.name)], False
    if tool == 'asset_stop':
        v.stop()
        return [text('asset viewer closed')], False
    if v.pipe is None:
        v.start()
    if tool == 'asset_command':
        r = v.send(args['command'])
        return [text(json.dumps(r, indent=1))], not r.get('ok', True)
    if tool == 'asset_load':
        cmd = {'cmd': 'add' if args.get('add') else 'load', 'kind': args['kind'], 'id': int(args['id']),
               'attire': int(args.get('attire') or 0)}
        if args.get('at'):
            cmd['at'] = args['at']
        r = v.send(cmd)
        content = [text(json.dumps(r, indent=1))]
        if args.get('shot') and r.get('ok', True):
            cr = v.send({'cmd': 'camera', 'preset': args['shot']})
            if not cr.get('ok', False):  # a bad preset name: report it, no picture
                return content + [text('camera: %s' % json.dumps(cr))], True
            content += _asset_shot(v, 1280)
        return content, not r.get('ok', True)
    if tool == 'asset_motions':
        cmd = {'cmd': 'motions'}
        cmd.update({k: args[k] for k in ('file', 'bank', 'top', 'act', 'offset', 'limit') if k in args})
        r = v.send(cmd)
        return [text(json.dumps(r))], not r.get('ok', True)
    if tool == 'asset_motion':
        out = []
        sel = [k for k in ('file', 'bank', 'top', 'act', 'name') if k in args]
        if sel:
            cmd = {'cmd': 'motion'}
            cmd.update({k: args[k] for k in sel})
            if 'actor' in args:
                cmd['actor'] = args['actor']
            r = v.send(cmd)
            out.append(text(json.dumps(r)))
            if not r.get('ok', False):
                return out, True
        if 'frame' in args:
            fc = {'cmd': 'frame', 'f': args['frame']}
            if 'actor' in args:
                fc['actor'] = args['actor']
            r = v.send(fc)
            out.append(text(json.dumps(r)))
            if not r.get('ok', False):
                return out, True
        if 'pose' in args:  # (after frame: frame switches to motion mode)
            pc = {'cmd': 'pose', 'mode': args['pose']}
            if 'actor' in args:
                pc['actor'] = args['actor']
            r = v.send(pc)
            out.append(text(json.dumps(r)))
            if not r.get('ok', False):
                return out, True
        if 'play' in args:
            r = v.send({'cmd': 'play', 'on': bool(args['play'])})
            out.append(text(json.dumps(r)))
        if args.get('shot'):
            cr = v.send({'cmd': 'camera', 'preset': args['shot']})
            if not cr.get('ok', False):
                return out + [text('camera: %s' % json.dumps(cr))], True
            out += _asset_shot(v, 1280)
        return out or [text('nothing to do: give a motion (file/bank/top/act/name), pose, frame, play or shot')], not out
    if tool == 'asset_place':
        cmd = {'cmd': 'place'}
        cmd.update({k: args[k] for k in ('wrestler', 'attire', 'spot', 't', 'pos', 'yaw', 'id', 'motion', 'frame', 'file', 'bank',
                                         'top', 'act', 'light', 'ground', 'anchor') if k in args})
        r = v.send(cmd, timeout=120.0)
        out = [text(json.dumps(r, indent=1))]
        if args.get('shot') and r.get('ok', False):
            sc = {'cmd': 'spot_cam', 'actor': r['id']}
            sc.update({k: args[k] for k in ('side', 'dist', 'pitch', 'fov') if k in args})
            cr = v.send(sc)
            if not cr.get('ok', False):
                return out + [text('spot_cam: %s' % json.dumps(cr))], True
            out += _asset_shot(v, int(args.get('width') or 1280))
        return out, not r.get('ok', True)
    if tool == 'asset_actors':
        out = []
        if args.get('clear'):
            r = v.send({'cmd': 'remove', 'all': True})
            out.append(text(json.dumps(r)))
            if not r.get('ok', False):
                return out, True
        elif args.get('remove'):
            r = v.send({'cmd': 'remove', 'id': args['remove']})
            out.append(text(json.dumps(r)))
            if not r.get('ok', False):
                return out, True
        r = v.send({'cmd': 'actors'})
        out.append(text(json.dumps(r, indent=1)))
        if args.get('spot_cam') or args.get('actor'):
            sc = {'cmd': 'spot_cam'}
            if args.get('actor'):
                sc['actor'] = args['actor']
            else:
                sc['spot'] = args['spot_cam']
            if 't' in args:
                sc['t'] = args['t']
            cr = v.send(sc)
            if not cr.get('ok', False):
                return out + [text('spot_cam: %s' % json.dumps(cr))], True
            out += _asset_shot(v, int(args.get('width') or 1280))
        return out, not r.get('ok', True)
    if tool == 'asset_stage':
        cmd = {'cmd': 'stage'}
        for k in ('wrestler', 'step', 'media'):
            if k in args:
                cmd[k] = args[k]
        if args.get('list'):
            cmd['list'] = True
        r = v.send(cmd, timeout=120.0)
        out = [text(json.dumps(r, indent=1))]
        if args.get('shot') and r.get('ok', True) and not args.get('list'):
            cr = v.send({'cmd': 'camera', 'preset': args['shot']})
            if not cr.get('ok', False):
                return out + [text('camera: %s' % json.dumps(cr))], True
            out += _asset_shot(v, int(args.get('width') or 1280))
        return out, not r.get('ok', True)
    if tool == 'asset_path':
        return _asset_path(v, args)
    if tool == 'asset_cam':
        return _asset_cam(v, args)
    if tool == 'asset_view':
        cmd = {'cmd': 'camera'}
        cmd.update({k: args[k] for k in ('preset', 'yaw', 'pitch', 'dist', 'fov', 'target') if k in args})
        r = v.send(cmd)
        return [text(json.dumps(r))] + _asset_shot(v, int(args.get('width') or 1280)), not r.get('ok', True)
    return [text('unknown asset tool %s' % tool)], True


def text(t):
    return {'type': 'text', 'text': t}


def image(path):
    with open(path, 'rb') as f:
        return {'type': 'image', 'data': base64.b64encode(f.read()).decode('ascii'), 'mimeType': 'image/jpeg'}


def call(tool, args):
    if tool.startswith('asset_'):
        return asset_call(tool, args)
    name = args.get('name') or 'main'
    if tool == 'game_start':
        t = live.start(name, args.get('script') or 'menu', args.get('launch') or '', bool(args.get('fast')),
                       args.get('extra') or '', window=args.get('window') or 'behind')
        return [text(t)], not t.startswith(('started', 'already'))
    if tool == 'game_bookmark':
        t, look = live.bookmark(name, int(args.get('width') or 1280))
        return [text(t)] + ([image(look)] if look else []), False
    if tool == 'game_diffcov':  # Session 30 (#6)
        fields, _ = live.parse(live.request(name, '!status'))
        cmd = [sys.executable, os.path.join(live.REPO, 'tools', 'coverage', 'diffcov.py'), args['b']]
        if args.get('a'):
            cmd.append(args['a'])
        cmd += ['--run', str(int(fields.get('run'))), '--top', str(int(args.get('top') or 40))]
        out = subprocess.run(cmd, capture_output=True, text=True)
        return [text((out.stdout + out.stderr).strip())], out.returncode != 0
    if tool == 'game_match_state':  # Session 91 step 8: the match snapshot (tools/live/events.py snapshot)
        import events
        snap = events.snapshot(name)
        if not snap:
            return [text('not in a match')], False
        try:
            with open(os.path.join(live.REPO, 'db', 'ids', 'characters.json'), encoding='utf-8') as f:
                names = {e['id']: e['name'] for e in json.load(f)['entries']}
        except (OSError, ValueError, KeyError):
            names = {}
        for w in snap.get('wrestlers', []):
            w['name'] = names.get(w.get('id'), '?')
        return [text(json.dumps(snap, indent=1))], False
    if tool == 'game_events':  # Session 91 step 7: the event bus (tools/live/events.py)
        import events
        fields, _ = live.parse(live.request(name, '!status'))
        ev = events.Events(int(fields.get('run')))
        kinds = set(str(args.get('kind') or '').split(',')) - {''}
        got = [e for e in ev.read() if not kinds or e.get('kind') in kinds][-int(args.get('last') or 30):]
        return [text('\n'.join(json.dumps(e) for e in got) or 'no events (is --wwe2k14_events on?)')], False
    if tool == 'game_go':  # S101 goto (N1)
        sys.path.insert(0, os.path.join(live.REPO, 'tools', 'goto'))
        import resolve as goto_resolve
        plan = goto_resolve.resolve(args['address'])
        out = plan.lines()
        if plan.status != 'plan' or args.get('dry'):
            return [text('\n'.join(out))], plan.status == 'error'
        try:
            goto_resolve.execute(plan, name, say=out.append)
        except goto_resolve.Stopped as x:
            return [text('\n'.join(out) + '\n' + goto_resolve.failure_report(name, str(x)))], True
        return [text('\n'.join(out))], False
    if tool == 'game_match':  # Session 91: the match builder (tools/atlas/match.py)
        sys.path.insert(0, os.path.join(live.REPO, 'tools', 'atlas'))
        import match
        if args.get('lookup'):  # candidates with ids, for the caller to choose from (then "#id" in the spec)
            hits = match.lookup(args['lookup'], args.get('what') or 'wrestler')
            return [text('\n'.join('#%d  %s' % h for h in hits) or 'no match')], False
        try:
            spec, summary = match.build_spec(args['spec'])
        except match.SpecError as x:
            return [text('SPEC: %s' % x)], True
        if args.get('dry'):
            return [text('spec: %s (%s)' % (spec, summary))], False
        lines = []
        try:
            failed, st = match.run(spec, name, args.get('compare') or '', True, say=lines.append)
        except match.nav.Lost as x:
            return [text('LOST on the way to the main menu: %s' % x)], True
        content = [text('%s\nspec: %s (%s)\n%s' % ('\n'.join(lines), spec, summary,
                                                  'FAILED: %s' % failed if failed else 'match sequence started'))]
        if args.get('screenshot'):
            _, _, pairs = live.run_batch(name, ['wait_frames 600'], 120, True, 1280)
            content += [image(jpg) for _, jpg in pairs if jpg]
        return content, bool(failed)
    if tool in ('game_where', 'game_nav'):  # Session 91: the navigator (tools/atlas/nav.py)
        sys.path.insert(0, os.path.join(live.REPO, 'tools', 'atlas'))
        import nav
        n = nav.Nav(name)
        if tool == 'game_where':
            w = n.where()
            return [text('%s\nstates: %s\ntext: %s' % (nav.Nav.describe(w), w['states'], w['text']))], False
        try:
            out, bad = '\n'.join(n.go(args['target'])), False
        except nav.Lost as x:
            out, bad = 'LOST: %s' % x, True
        content = [text(out)]
        if args.get('screenshot'):
            _, _, pairs = live.run_batch(name, [], 60, True, 1280)
            content += [image(jpg) for _, jpg in pairs if jpg]
        return content, bad
    if tool == 'game_text':  # Session 30
        return [text(live.request(name, '!text').replace('\n.\n', '\n').rstrip())], False
    if tool in ('game_status', 'game_stop'):
        fields, _ = live.parse(live.request(name, '!' + tool.split('_')[1]))
        return [text('  '.join('%s: %s' % kv for kv in fields.items()))], False
    if tool == 'game_quit':
        try:
            live.request(name, 'quit', connect_wait=2.0)
        except live.NoGame:
            pass
        return [text('quit sent')], False
    width = int(args.get('width') or 1280)
    if tool == 'game_screenshot':
        lines, shot, timeout = ['screenshot live_%s' % time.strftime('%H%M%S')], False, 60
    elif tool == 'game_check':
        lines, shot, timeout = ['check %s' % args['question']], False, 330
    elif tool == 'game_window':  # Session 30 (#6): a coverage window edge
        lines = ['mark %d %s' % (int(time.time() * 10) % 1000000, str(args['label']).replace(' ', '_'))]
        shot, timeout = False, 60
    elif tool == 'game_goto':  # Session 30: navigation snippets (tools/input_scripts/nav)
        lines = ['include %s' % n for n in str(args.get('names', '')).split()]
        shot, timeout = bool(args.get('screenshot')), int(args.get('timeout') or 120)
    else:
        lines = [x for x in str(args.get('lines', '')).splitlines() if x.strip()]
        shot, timeout = bool(args.get('screenshot')), int(args.get('timeout') or 120)
    fields, log, pairs = live.run_batch(name, lines, timeout, shot, width)
    content = [text(live.describe(fields, log, pairs if tool in ('game_run', 'game_goto') else [],
                                  args.get('log') or 'ours'))]
    for _, jpg in pairs:
        if jpg:
            content.append(image(jpg))
    return content, fields.get('status') != 'ok'


def reply(msg_id, result=None, error=None):
    out = {'jsonrpc': '2.0', 'id': msg_id}
    if error is not None:
        out['error'] = error
    else:
        out['result'] = result
    sys.stdout.write(json.dumps(out) + '\n')
    sys.stdout.flush()


def main():
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except ValueError:
            continue
        method, msg_id = msg.get('method'), msg.get('id')
        if msg_id is None:  # a notification (initialized, cancelled): no answer
            continue
        if method == 'initialize':
            version = (msg.get('params') or {}).get('protocolVersion') or '2025-06-18'
            reply(msg_id, {'protocolVersion': version, 'capabilities': {'tools': {}},
                           'serverInfo': {'name': 'wwe2k14-live', 'version': '1.0'}})
        elif method == 'tools/list':
            reply(msg_id, {'tools': TOOLS})
        elif method == 'tools/call':
            p = msg.get('params') or {}
            try:
                content, is_error = call(p.get('name'), p.get('arguments') or {})
            except live.NoGame as e:
                content, is_error = [text('no game: %s (start one with game_start)' % e)], True
            except Exception:
                content, is_error = [text('error: ' + traceback.format_exc()[-1500:])], True
            reply(msg_id, {'content': content, 'isError': is_error})
        elif method == 'ping':
            reply(msg_id, {})
        else:
            reply(msg_id, error={'code': -32601, 'message': 'method not found: %s' % method})


if __name__ == '__main__':
    main()
