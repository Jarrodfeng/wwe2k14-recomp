r"""Lane 158 (S101): which on-screen boxes do the roster-wide routes meet, and what would the stall watchdog do with each? Offline, 0 game runs.

  py -I tools\census\roster_boxes.py [--strings PATH] [--selftest]

1. THE BOXES THE ROUTES MEET. A table (below) of every message box / prompt on the roster routes' way, each with the text shape the watchdog would read (`!status` text:), the sequence
   it is seen in, and the decision `stall_guard.decide()` makes (tools\live\stall_safe.py is the only list of what it may press). The input-script routes
   (scenes\s101_roster_*.txt) press their own counted presses and have no watchdog; the table is for the LIVE runs around them (the CAW make leg 3a, which the main session may drive
   live with `live.py run --guard` / goto) and for any goto that meets a box on the way.
2. THE MENU TEXT TABLES, READ OFFLINE. The player's own string pac (pac\string.pac: zlib tables of UTF-8 texts, docs\formats\string_pac.md) is scanned for message-box vocabulary
   (save / overwrite / delete / create / storage / accept / name / quit / discard). Each hit is shown to the watchdog as `<text> | OK` and `<text> | YES | NO`; the check is that the
   watchdog presses NONE of the data-touching ones (it must stop on every text that names a save, an overwrite, a delete or a choice). It prints counts and the first examples
   as quoted text lengths only (the texts are game content: nothing is written to a file, nothing is committed).

Exit 0 when every data-touching text is a STOP; 1 otherwise.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools' / 'live'))
sys.path.insert(0, str(HERE / 'tools' / 'ids'))
import stall_guard as SG  # noqa: E402

# (route, step, status text shape, seq, expected decision). seq per states.toml: 3 title/boot, 75 main menu, 76 CAW editor, 9 in game.
BOXES = [
    ('every route', 'boot: title screen', 'PRESS START', 3, 'press'),
    ('every route', 'boot: storage notice (the ATTENTION box after START, answered by A in boot_to_menu)', 'ATTENTION | OK', 3, 'stop'),   # "attention" alone is not on the safe list: the route presses A itself
    ('sixpack / fatal4 / caw match', 'in the match: no box expected', '', 9, 'stop'),
    ('fatal4', 'entrances: none expected (a skip / tutorial hint box is possible, unconfirmed)', 'OK', 9, 'press'),
    ('caw make 3a', 'after ACCEPT: a name box (unconfirmed)', 'ENTER NAME | OK', 76, 'stop'),
    ('caw make 3a', 'after ACCEPT: a save prompt (unconfirmed)', 'Save this Superstar? | YES | NO', 76, 'stop'),
    ('caw make 3a', 'after ACCEPT: "Superstar saved" notice (unconfirmed)', 'OK', 76, 'press'),   # a lone OK is the policy's one-button case; a live driver reads the text first
    ('caw make 3a', 'overwrite an existing superstar (unconfirmed)', 'Overwrite existing Superstar? | YES | NO', 76, 'stop'),
]

WORDS = re.compile(r'\b(save|saved|saving|overwrite|delete|erase|create|storage|device|accept|discard|quit|exit|name|replace|remove|unsaved|lost|changes)\b', re.I)
DATA = re.compile(r'\b(save|saving|overwrite|delete|erase|storage|device|replace|remove|discard|unsaved|lost|changes)\b', re.I)   # words that mean the box touches data


def tables_of(path):
    """Yield (id, text) of every record of every string table in the pac."""
    import names_from_strings as NS
    data = Path(path).read_bytes()
    for out, count in NS.tables(data):
        for i in range(count):
            off, ln, tid = struct.unpack_from('<III', out, 8 + 12 * i)
            if ln and off + ln <= len(out):
                yield tid, out[off:off + ln].split(b'\0', 1)[0].decode('utf-8', 'replace')


def find_strings():
    c = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
    main = Path(c).parent if c else HERE
    return main / 'game_data' / 'extracted' / 'pac' / 'string.pac'


def selftest():
    ok = True
    # the policy on the four planted data boxes must be STOP, and a plain one-button OK in a match a press
    for t, seq, want in (('Save this Superstar? | YES | NO', 76, 'stop'), ('Overwrite existing Superstar? | OK', 76, 'stop'), ('Delete this Superstar? | OK', 76, 'stop'),
                         ('Storage device removed | OK', 75, 'stop'), ('OK', 9, 'press')):
        got = SG.decide(t, seq).kind
        print(('ok   ' if got == want else 'FAIL ') + f'decide({t!r}, {seq}) = {got}')
        ok = ok and got == want
    print('roster_boxes selftest: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--strings')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    kind = 'kind'
    if a.selftest:
        return selftest()
    print('| route | where | text shape | seq | watchdog | expected | |')
    print('|---|---|---|---|---|---|---|')
    bad = 0
    for route, where, text, seq, want in BOXES:
        d = SG.decide(text, seq)
        got = getattr(d, kind)
        mark = '' if got == want else ' MISMATCH'
        bad += got != want
        print(f"| {route} | {where} | `{text or '(no text)'}` | {seq} | {got.upper()}{(' ' + d.button) if getattr(d, 'button', '') else ''} | {want} |{mark} |")
    path = Path(a.strings) if a.strings else find_strings()
    if not path.exists():
        print(f'\n[roster_boxes] {path} not found: the menu text tables were not read')
        return 1 if bad else 0
    seen, hits, pressed = {}, 0, []
    for tid, t in tables_of(path):
        if tid in seen or not WORDS.search(t):
            continue
        seen[tid] = t
    n_data = 0
    for tid, t in seen.items():
        if not DATA.search(t):
            continue
        n_data += 1
        for shown in (t + ' | OK', t + ' | YES | NO', t + ' | A'):
            if getattr(SG.decide(shown, 76), kind) == 'press':
                pressed.append((tid, shown))
    print(f'\n[roster_boxes] string tables of {path.name}: {len(seen)} distinct texts with box vocabulary, {n_data} of them name a save / overwrite / delete / storage / replace / remove / discard;'
          f' shown as "<text> | OK" / "<text> | YES | NO" / "<text> | A" in the CAW editor (seq 76) the watchdog pressed {len(pressed)} of {3 * n_data}')
    for tid, s in pressed[:10]:
        print(f'  PRESSED text id {tid} ({len(s)} chars)')
    bad += len(pressed)
    print('[roster_boxes] ' + ('FAIL' if bad else 'ok: every data-touching text is a STOP, every table row matches its expected decision'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
