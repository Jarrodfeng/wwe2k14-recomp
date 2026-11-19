r"""One shared game_syntax run per gate (Session 103, lane DIB; GS_gate_speed 2.d.iii: 101 lanes carry a game_syntax check, each re-ran the same two units + the
planted twin; DI measured 101 near-identical compiles).

A check whose command is exactly  py -I tools/gate/game_syntax.py --root . [--no-twin] [--files a.cpp,b.cpp]  is a SHARED check: the gate runs ONE
`game_syntax.py --root . --files <union of every selected shared check's files>` (twin on when any of them wants it) and reads each check's verdict from that
run's per-unit lines:
  - the check PASSes when every unit it asks for (the default units + its --files) printed `ok   <unit>` and, unless it passed --no-twin, the planted twin printed
    `ok   planted twin ...` (the twin line only appears when the twin failed to compile, as planted);
  - otherwise it FAILs, with the unit's own lines. A shared run that dies before it reports any unit (ninja missing, build dir missing) FAILs every check, never passes it.
Honesty: nothing is skipped. Every unit and the twin that any check would have compiled is compiled once in the shared run (game_syntax's own preprocess-hash cache applies
as before); a check never passes on a unit that did not print ok.
"""
import re

DEFAULT_FILES = ['src/main.cpp', 'src/renderer_adapter/live.cpp']
SCRIPT = 'tools/gate/game_syntax.py'


def parse(cmd):
    """-> (files, twin) for a shared-form command, else None (a different invocation runs on its own)."""
    cmd = [str(a) for a in cmd]
    if len(cmd) < 5 or cmd[0] not in ('py', 'python') or cmd[1] != '-I' or cmd[2].replace('\\', '/') != SCRIPT or cmd[3:5] != ['--root', '.']:
        return None
    files, twin, i = [], True, 5
    while i < len(cmd):
        if cmd[i] == '--no-twin':
            twin = False
            i += 1
        elif cmd[i] == '--files' and i + 1 < len(cmd):
            files += [f.strip().replace('\\', '/') for f in cmd[i + 1].split(',') if f.strip()]
            i += 2
        else:
            return None   # an option we do not model: run it alone
    return files, twin


def union(parsed):
    """[(files, twin)] -> the shared run's (files, twin): every file once, in first-seen order, twin when any wants it."""
    files, twin = [], False
    for f, t in parsed:
        for x in f:
            if x not in files and x not in DEFAULT_FILES:
                files.append(x)
        twin = twin or t
    return files, twin


def shared_cmd(files, twin):
    cmd = ['py', '-I', SCRIPT, '--root', '.']
    if not twin:
        cmd.append('--no-twin')
    if files:
        cmd += ['--files', ','.join(files)]
    return cmd


_OK = re.compile(r'^ok\s+(\S+)\s*$', re.M)
_TWIN_OK = re.compile(r'^ok\s+planted twin', re.M)
_FAIL = re.compile(r'^FAIL\s+(\S+)\s*$', re.M)


def verdict(cmd, rc, text):
    """One shared check's verdict from the shared run's output: ('PASS' | 'FAIL', tail)."""
    p = parse(cmd)
    files, twin = p
    want = DEFAULT_FILES + [f for f in files if f not in DEFAULT_FILES]
    oks = set(_OK.findall(text))
    miss = [f for f in want if f not in oks]
    twin_ok = bool(_TWIN_OK.search(text))
    if not miss and (twin_ok or not twin):
        lines = [l for l in text.splitlines() if l.startswith('PASS') or l.startswith('pp-cache')]
        return 'PASS', f'shared game_syntax run: {len(want)} unit(s)' + (' + the planted twin' if twin else '') + ' compile; ' + (lines[-1] if lines else '')
    bad = [f for f in _FAIL.findall(text) if f in want]
    why = []
    if miss:
        why.append('no ok line for ' + ', '.join(miss))
    if twin and not twin_ok:
        why.append('the planted twin did not fail to compile as planted (or never ran)')
    out = [l for l in text.strip().splitlines() if l.strip()]
    return 'FAIL', 'shared game_syntax run (rc %d): %s | %s' % (rc, '; '.join(why), ' | '.join(out[-3:])[:200])


if __name__ == '__main__':
    print('library for tools\\gate\\gate.py; test: py -I tools\\gate\\deps_selftest.py')
