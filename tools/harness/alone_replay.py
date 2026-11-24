"""S106 lane C5, step 8 of the planted test (docs/research/s106/C5.md): the S105 run 098 re-find, offline, from the recorded histories.

  py -I tools\\harness\\alone_replay.py --at "2026-11-20 19:48:48.474"             -> what was running at that moment, and what `-Alone` would have said
  py -I tools\\harness\\alone_replay.py --at ... --plant blind                      -> the planted twin: every evidence source removed (must say ALONE and exit 1)

Reads (all read-only, all gitignored data of the MAIN checkout, nothing copied):
  out\\harness\\runlock\\history.tsv       run-lock holds: acquire / release rows (shared slot, exclusive, build); games at the take
  admit histories (--admit-history PATH, repeatable; default <LOCALAPPDATA>\\wwe2k14-admit\\history.tsv and <main>\\out\\harness\\admit\\history.tsv):
                                           one row per admit hold, written at its RELEASE (time = end, held_s = its length); `timeout_run` rows are callers
                                           that gave up waiting and compiled with no slot (the unseen ones)
`-Alone` says ALONE only when none of these show anything running at the asked moment (a real run drains up to 300 s, so this is the instantaneous answer).
# entry: alone_replay.main (the same busy test as runlock_alone.ps1 Get-RunLockAloneBusy, over the recorded histories)
Exit codes: 0 = NOT alone at that moment (run 098's truth: it ran beside game run 097 and many compiles), 1 = it would have said alone (the fault),
2 = unreadable input. --expect alone flips the meaning (for a quiet moment). Evidence lines name the run, pid, slot and times.
"""
import argparse
import csv
import datetime as dt
import os
import re
import subprocess
import sys

# entry: main
FMT = '%Y-%m-%d %H:%M:%S.%f'


def parse_t(s):
    s = s.strip()
    for f in (FMT, '%Y-%m-%d %H:%M:%S'):
        try:
            return dt.datetime.strptime(s, f)
        except ValueError:
            pass
    raise ValueError('bad time %r' % s)


def main_checkout():
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=30).stdout.strip()
    except Exception:
        return None
    return os.path.dirname(os.path.normpath(common)) if common else None


def read_tsv(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        rows = list(csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE))
    return rows[0], rows[1:]


def window(head_rows, at, secs=900.0, tcol=0):
    """The rows of a (head, rows) table whose time column lies within `secs` before / after `at` (a fixture holds only these)."""
    head, rows = head_rows
    keep = []
    for r in rows:
        try:
            if abs((parse_t(r[tcol]) - at).total_seconds()) <= secs:
                keep.append(r)
        except (ValueError, IndexError):
            pass
    return head, keep


def runlock_holds(table, at, max_age_h=3.0):
    """Run-lock holds open at `at` (acquired strictly before it, within max_age_h hours, not released by it): [(mode, slot, name, pid, acquired, games_at_take)].
    (A holder that crashed never wrote its release row: such an acquire row would stay "open" for ever, so an hold older than max_age_h does not count.)"""
    head, rows = table
    col = {c: i for i, c in enumerate(head)}
    opened = {}
    for r in rows:
        if len(r) < len(head) or r[0] == 'time':
            continue
        try:
            t = parse_t(r[col['time']])
        except ValueError:
            continue
        ev, mode, slot, tok = r[col['event']], r[col['mode']], r[col['slot']], r[col['token']]
        key = (tok, mode, slot)
        if ev == 'acquire' and t < at:
            opened[key] = (mode, slot, r[col['name']], r[col['pid']], t, r[col['games']])
        elif ev == 'release' and t <= at:
            opened.pop(key, None)
    return [h for h in opened.values() if (at - h[4]).total_seconds() <= max_age_h * 3600]


def admit_holds(table, at, unseen_window_s):
    """-> (admitted holds open at `at`: [(kind, pid, start, end, prog)], timeout_run callers that began compiling within the window before `at`)."""
    head, rows = table
    col = {c: i for i, c in enumerate(head)}
    live, unseen = [], []
    for r in rows:
        if len(r) < len(head) or r[0] == 'time':
            continue
        try:
            end = parse_t(r[col['time']])
            held = float(r[col['held_s']] or 0)
        except (ValueError, KeyError):
            continue
        out = r[col['outcome']]
        prog = r[col['prog']] if 'prog' in col else ''
        if out in ('admitted', 'nested_hold') and end - dt.timedelta(seconds=held) <= at < end:
            live.append((r[col['kind']], r[col['pid']], end - dt.timedelta(seconds=held), end, prog))
        elif out == 'timeout_run' and 0 <= (at - end).total_seconds() <= unseen_window_s:
            unseen.append((r[col['kind']], r[col['pid']], end, r[col['waited_s']]))
    return live, unseen


def load_live(a, root, at):
    """The two sources as tables: ({head, rows} run-lock, [admit tables]) from the main checkout's out\\ files."""
    rl = a.runlock_history or os.path.join(root, 'out', 'harness', 'runlock', 'history.tsv')
    ah = a.admit_history or [p for p in (os.path.join(os.environ.get('LOCALAPPDATA', ''), 'wwe2k14-admit', 'history.tsv'),
                                         os.path.join(root, 'out', 'harness', 'admit', 'history.tsv')) if os.path.isfile(p)]
    return read_tsv(rl), [read_tsv(p) for p in ah], '%s; admit %d file(s)' % (rl, len(ah))


def make_fixture(path, tables, at, secs=900.0):
    """A small committed reference (rows within +-15 min of `at`; our own tools' history lines: no game data) so the replay runs where out\\ is absent."""
    import json
    rl, ahs, _ = tables
    rlw = window(rl, at, secs)
    fx = {'at': a_str(at), 'runlock': {'head': rlw[0], 'rows': rlw[1]},
          'admit': [{'head': h, 'rows': window((h, r), at, secs)[1]} for h, r in ahs]}
    fx['runlock']['rows'] = [r[:4] + [r[4][:80]] + r[5:] for r in fx['runlock']['rows']]   # (a command line is cut to 80 chars)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(fx, f, indent=0)
    return fx


def a_str(t):
    return t.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]


def load_fixture(path):
    import json
    with open(path, encoding='utf-8') as f:
        fx = json.load(f)
    return ((fx['runlock']['head'], fx['runlock']['rows']), [(t['head'], t['rows']) for t in fx['admit']], 'fixture ' + path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--at', required=True, help='local time "YYYY-MM-DD HH:MM:SS[.fff]"')
    ap.add_argument('--root', help='the main checkout (default: found through git)')
    ap.add_argument('--runlock-history')
    ap.add_argument('--admit-history', action='append')
    ap.add_argument('--expect', choices=('busy', 'alone'), default='busy')
    ap.add_argument('--plant', choices=('games', 'admit', 'blind'), help='the unsafe twin: drop an evidence source (blind = all of them)')
    ap.add_argument('--unseen-window', type=float, default=120.0)
    ap.add_argument('--fixture', help='read the sources from this committed reference (tools\\harness\\fixtures\\alone_run098.json) instead of out\\')
    ap.add_argument('--make-fixture', metavar='OUT', help='write that reference from the live histories (+-15 min around --at), then replay')
    a = ap.parse_args()
    at = parse_t(a.at)
    try:
        if a.fixture and not a.make_fixture:
            tables = load_fixture(a.fixture)
        else:
            root = a.root or main_checkout()
            if not root:
                print('alone_replay: no main checkout found', file=sys.stderr)
                return 2
            tables = load_live(a, root, at)
            if a.make_fixture:
                fx = make_fixture(a.make_fixture, tables, at)
                print('alone_replay: wrote %s (%d run-lock rows, %d admit tables)' % (a.make_fixture, len(fx['runlock']['rows']), len(fx['admit'])))
        rlt, aht, src = tables
        holds = runlock_holds(rlt, at)
        live, unseen = [], []
        for t in aht:
            l, u = admit_holds(t, at, a.unseen_window)
            live += l
            unseen += u
    except (OSError, ValueError, KeyError) as e:
        print('alone_replay: cannot read %s' % e, file=sys.stderr)
        return 2
    drop = a.plant
    if drop in ('games', 'blind'):
        holds = []
    if drop in ('admit', 'blind'):
        live, unseen = [], []
    print('alone_replay at %s%s  (%s)' % (a_str(at), ('  PLANT ' + drop) if drop else '', src))
    for mode, slot, name, pid, t, games in holds:
        print('  run-lock %s hold, slot %s, pid %s since %s (games at take %s): %s' % (mode, slot, pid, t.strftime('%H:%M:%S.%f')[:-3], games, name[:90]))
    for kind, pid, s, e, prog in live:
        print('  admit hold %s pid %s %s-%s %s' % (kind, pid, s.strftime('%H:%M:%S'), e.strftime('%H:%M:%S'), prog))
    for kind, pid, e, w in unseen:
        print('  admit timeout_run %s pid %s at %s after waiting %s s: its caller then compiled with no slot (unseen)' % (kind, pid, e.strftime('%H:%M:%S'), w))
    busy = bool(holds or live or unseen)
    print('  => games/holds %d, admitted compiles %d, unseen compiles %d: -Alone would say %s' %
          (len(holds), len(live), len(unseen), 'NOT ALONE (it drains, or says NO after its limit)' if busy else 'ALONE'))
    ok = busy if a.expect == 'busy' else not busy
    if not ok:
        print('  FAIL: expected %s' % ('busy (run 098 had a game and compiles beside it)' if a.expect == 'busy' else 'alone'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
