"""Session 29: MCP server (stdio) for the game's live control channel (tools/live/live.py, src/live_control.h).

Registered in .mcp.json as "wwe2k14-live" (Claude Code asks once before it starts a project MCP server). Tools:
  game_start       boot a game with the live channel (run_list: frozen user data, run lock, test settings file)
  game_run         send input-script lines (press, wait_state, log_state, cvar, check, ...); optional screenshot
  game_screenshot  a screenshot, returned as an image (JPEG, default 1280 px wide)
  game_check       an on-screen question Jarrod answers with F1 / F2 / F5
  game_status / game_stop / game_quit
Screenshots come back inside the tool result, so looking costs one call. No dependencies (JSON-RPC over stdio).
"""
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
]


def text(t):
    return {'type': 'text', 'text': t}


def image(path):
    with open(path, 'rb') as f:
        return {'type': 'image', 'data': base64.b64encode(f.read()).decode('ascii'), 'mimeType': 'image/jpeg'}


def call(tool, args):
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
