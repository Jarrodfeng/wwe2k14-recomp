r"""One line on the compile queue for a session-start screen (lane CQW; docs\agents\admit_wiring.md).   py -I tools\admit\status.py

  admit: INSTALLED <path> (built HH:MM, current) | mode observe | last history row 2026-11-20 14:02 (cc.tool admitted) | <state folder>
  admit: NOT INSTALLED <path>: wired builds are not queued (build it: pwsh -NoProfile -File tools\admit\build_admit.ps1 in the main checkout)
  admit: STALE <path> is older than tools\admit\admit.cpp (rebuild it) | ...
The exe and the config are the MAIN checkout's (what the helpers use); the state folder is env WWE_ADMIT_DIR, else %LOCALAPPDATA%\wwe2k14-admit.
Reads files only: never starts admit. Exit 0 when installed and current, 1 otherwise (a caller may print it either way)."""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import admit_py  # noqa: E402


def stamp(t):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(t))


def main():
    exe = admit_py.exe_path()
    if not exe:
        print('admit: UNKNOWN (no git checkout found): wired builds are not queued')
        return 1
    main_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(exe))))   # <main>\out\tools\admit\admit.exe -> <main>
    src = os.path.join(main_root, 'tools', 'admit', 'admit.cpp')
    cfg = os.path.join(main_root, 'tools', 'admit', 'admit.json')
    mode = '?'
    try:
        with open(cfg, encoding='utf-8') as f:
            c = json.load(f)
        mode = f"{c['mode']} (admit.json)" if c.get('mode') else 'observe (the program default)'
    except (OSError, ValueError):
        mode = f'? ({cfg} missing)' if not os.path.exists(cfg) else f'? ({cfg} unreadable: admit then grants `open`)'
    if os.environ.get('WWE_ADMIT_MODE'):
        mode = f"{os.environ['WWE_ADMIT_MODE']} (env WWE_ADMIT_MODE, over {mode})"
    state = os.environ.get('WWE_ADMIT_DIR') or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-admit')
    hist = os.path.join(state, 'history.tsv')
    last = 'no history yet'
    try:
        with open(hist, encoding='utf-8', errors='replace') as f:
            lines = [ln for ln in f.read().splitlines() if ln.strip()]
        if len(lines) > 1:
            head, row = lines[0].split('\t'), lines[-1].split('\t')
            d = dict(zip(head, row))
            last = f"last history row {d.get('time', '?')} ({d.get('kind', '?')} {d.get('outcome', '?')})"
    except OSError:
        pass
    tail = f'mode {mode} | {last} | {state}'
    if not os.path.isfile(exe):
        print(f'admit: NOT INSTALLED {exe}: wired builds are not queued (build it in the main checkout: pwsh -NoProfile -File tools\\admit\\build_admit.ps1) | {tail}')
        return 1
    built = os.path.getmtime(exe)
    if os.path.isfile(src) and os.path.getmtime(src) > built:
        print(f'admit: STALE {exe} (built {stamp(built)}) is older than {src} (rebuild it) | {tail}')
        return 1
    print(f'admit: INSTALLED {exe} (built {stamp(built)}, current) | {tail}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
