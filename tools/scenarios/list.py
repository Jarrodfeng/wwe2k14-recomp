"""List the captured scenarios and check each one reads back (Session 9, scenario launcher; src/scenario.h).

    py tools\\scenarios\\list.py                 every scenario in out\\scenarios: one line each + its check
    py tools\\scenarios\\list.py NAME [NAME...]  only these, with participants and setter calls
    py tools\\scenarios\\list.py --dir DIR       another folder (--wwe2k14_scenario_dir)
    py tools\\scenarios\\list.py --rename OLD NEW        rename a scenario (its files and the names inside the .toml)
    py tools\\scenarios\\list.py --describe NAME "TEXT"  set its description (a top-level `description` in the .toml)

--rename checks OLD first and never overwrites (refused if any NEW.* exists); names are letters, digits, _ and -
(1-64), as for --wwe2k14_scenario_capture. The hashes don't change (the bytes don't). The launcher ignores the
description (it reads only the [files] lines).

Check per scenario: the .toml parses; m / g / a (and from format 2 x, the extras) files exist with the recorded size
and FNV-1a 64 hash; the summary (match type, mode, arena, extra ids, participant ids; menu arena record and pad)
equals what the .m.bin / .x.bin hold at the documented offsets (docs/systems/match_setup.md, src/scenario.h).
Exit code 1 if any check fails. Scenario files are local only (copies of game memory:
never commit them); this tool prints ids and offsets only.
"""
import argparse
import json
import os
import re
import struct
import sys
import tomllib

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DIR = os.path.join(REPO, 'out', 'scenarios')
M_SIZE = 724 + 43 * 3580


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return '%016X' % h


def be32(d, off, signed=False):
    return struct.unpack_from('>i' if signed else '>I', d, off)[0]


def check(dirname, name, verbose):
    problems = []
    path = os.path.join(dirname, name + '.toml')
    try:
        with open(path, 'rb') as f:
            t = tomllib.load(f)
    except Exception as e:  # noqa: BLE001 - report any parse error as a failed check
        return None, ['toml does not parse: %s' % e]
    blobs = {}
    keys = ('m', 'g', 'a', 'x') if t.get('format', 1) >= 2 else ('m', 'g', 'a')
    if t.get('format', 1) < 2:
        problems.append('format %s: no extras file (captured before format 2); --wwe2k14_launch needs a new capture'
                        % t.get('format', 1))
    for key in keys:
        info = t.get('files', {}).get(key)
        if not info:
            problems.append('[files] %s missing' % key)
            continue
        fp = os.path.join(dirname, info['file'])
        if not os.path.isfile(fp):
            problems.append('%s missing' % info['file'])
            continue
        data = open(fp, 'rb').read()
        blobs[key] = data
        if len(data) != info['size']:
            problems.append('%s size %d, toml says %d' % (info['file'], len(data), info['size']))
        if fnv1a64(data) != info['fnv1a64']:
            problems.append('%s hash %s, toml says %s' % (info['file'], fnv1a64(data), info['fnv1a64']))
    m = blobs.get('m')
    s = t.get('summary', {})
    if m is not None and len(m) == M_SIZE:
        expect = {'match_type': be32(m, 8, True), 'mode': be32(m, 4, True), 'arena': be32(m, 532, True),
                  'extra_ids': [be32(m, 544, True), be32(m, 548, True), be32(m, 552, True)]}
        for k, v in expect.items():
            if s.get(k) != v:
                problems.append('summary %s = %r, M holds %r' % (k, s.get(k), v))
        ids = [(i, be32(m, 724 + 3580 * i + 56)) for i in range(43)]
        ids = [(i, v) for i, v in ids if v < 500]
        got = [(p.get('index'), p.get('id')) for p in s.get('participant', [])]
        if got != ids:
            problems.append('summary participants %r, M holds %r' % (got, ids))
    elif m is not None:
        problems.append('m size %d, expected %d' % (len(m), M_SIZE))
    x = blobs.get('x')
    if x is not None and len(x) == 32:
        expect = {'menu_arena_record': [be32(x, 4, True), be32(x, 8, True)], 'menu_pad': be32(x, 16, True)}
        for k, v in expect.items():
            if s.get(k) != v:
                problems.append('summary %s = %r, x holds %r' % (k, s.get(k), v))
    elif x is not None:
        problems.append('x size %d, expected 32' % len(x))
    return t, problems


NAME_RE = re.compile(r'^[A-Za-z0-9_-]{1,64}$')


def write_text_atomic(path, text):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    os.replace(tmp, path)


def rename(dirname, old, new):
    if not NAME_RE.match(new):
        print('refused: "%s" is not a valid name (letters, digits, _ and -, 1-64)' % new)
        return 1
    t, problems = check(dirname, old, False)
    if t is None or problems:
        print('refused: %s does not pass the check: %s' % (old, '; '.join(problems) or 'unreadable'))
        return 1
    taken = [f for f in os.listdir(dirname) if f == new + '.toml' or f.startswith(new + '.')]
    if taken:
        print('refused: %s already exists (%s)' % (new, ', '.join(sorted(taken))))
        return 1
    toml_path = os.path.join(dirname, old + '.toml')
    text = open(toml_path, encoding='utf-8').read()
    text = text.replace('\nname = "%s"\n' % old, '\nname = "%s"\n' % new, 1)
    for key, info in t['files'].items():
        new_file = new + info['file'][len(old):] if info['file'].startswith(old + '.') else None
        if not new_file:
            print('refused: %s file "%s" does not start with "%s."' % (key, info['file'], old))
            return 1
        text = text.replace('file = "%s"' % info['file'], 'file = "%s"' % new_file, 1)
    # Data files first, then the new .toml, then the old .toml goes (a scenario counts as present by its .toml).
    for key, info in t['files'].items():
        os.rename(os.path.join(dirname, info['file']), os.path.join(dirname, new + info['file'][len(old):]))
    write_text_atomic(os.path.join(dirname, new + '.toml'), text)
    os.remove(toml_path)
    _, problems = check(dirname, new, False)
    print('renamed %s -> %s%s' % (old, new, '' if not problems else '  BUT the check fails: ' + '; '.join(problems)))
    return 1 if problems else 0


def describe(dirname, name, desc):
    path = os.path.join(dirname, name + '.toml')
    if not os.path.isfile(path):
        print('no scenario %s in %s' % (name, dirname))
        return 1
    text = open(path, encoding='utf-8').read()
    line = 'description = %s\n' % json.dumps(desc, ensure_ascii=False)  # a JSON string is a valid TOML basic string
    lines = text.split('\n')
    lines = [x for x in lines if not x.startswith('description = ')]
    text = '\n'.join(lines)
    at = text.find('\n[')  # before the first table: a top-level key
    text = text[:at + 1] + line + text[at + 1:] if at >= 0 else text + '\n' + line
    write_text_atomic(path, text)
    with open(path, 'rb') as f:
        tomllib.load(f)  # still parses
    print('%s: description = %s' % (name, json.dumps(desc, ensure_ascii=False)))
    return 0


PACES = {0: 'Normal', 1: 'Quick', 2: 'Epic'}


def pace_of(dirname, t):
    """Match pace from the scenario's M (M+528: 0 Normal, 1 Quick, 2 Epic; PLAYBOOK Session 9 'Match pace: found')."""
    info = t.get('files', {}).get('m')
    try:
        m = open(os.path.join(dirname, info['file']), 'rb').read(532)
        v = be32(m, 528)
    except Exception:  # noqa: BLE001 - the check reports missing files; here it is just "?"
        return '?'
    return PACES.get(v, str(v))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('names', nargs='*')
    ap.add_argument('--dir', default=DEFAULT_DIR)
    ap.add_argument('--rename', nargs=2, metavar=('OLD', 'NEW'))
    ap.add_argument('--describe', nargs=2, metavar=('NAME', 'TEXT'))
    a = ap.parse_args()
    if a.rename:
        return rename(a.dir, *a.rename)
    if a.describe:
        return describe(a.dir, *a.describe)
    if not os.path.isdir(a.dir):
        print('no scenario folder: %s (capture one with --wwe2k14_scenario_capture=NAME)' % a.dir)
        return 0 if not a.names else 1
    names = a.names or sorted(f[:-5] for f in os.listdir(a.dir) if f.endswith('.toml'))
    if not names:
        print('no scenarios in %s' % a.dir)
        return 0
    failed = 0
    for name in names:
        t, problems = check(a.dir, name, bool(a.names))
        if t is None:
            print('%-24s  FAIL  %s' % (name, '; '.join(problems)))
            failed += 1
            continue
        s = t.get('summary', {})
        people = ' vs '.join(str(p['id']) for p in s.get('participant', []))
        status = 'ok' if not problems else 'FAIL'
        pace = pace_of(a.dir, t)
        print('%-24s  %-4s  run %-4s type %-3s mode %-2s arena %-3s pace %-6s %s  (extra %s)' % (
            name, status, t.get('captured_run'), s.get('match_type'), s.get('mode'), s.get('arena'), pace, people,
            ', '.join(str(x) for x in s.get('extra_ids', []))))
        for p in problems:
            print('    problem: %s' % p)
        if t.get('description'):
            print('    %s' % t['description'])
        if a.names:
            for p in s.get('participant', []):
                print('    participant %2d: id %d, slot_id %d' % (p['index'], p['id'], p['slot_id']))
            for c in t.get('call', []):
                print('    call %-16s %-34s from %s frame %s' % (c['fn'], str(c['args']), c['from'], c['frame']))
        failed += bool(problems)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
