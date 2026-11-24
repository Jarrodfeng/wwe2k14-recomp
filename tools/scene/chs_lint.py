#!/usr/bin/env python3
"""S103 CHS lint (review fixes): the present_direct switch cannot drift from its rig, and every planted value is run by its check.

  py -I tools\\scene\\chs_lint.py [--root DIR] [--selftest]

R1  a HostOptions field that mirrors a cvar (CVAR_FIELDS) has the cvar's default, or its line carries `RIG-DEFAULT: why` (a rig / unit that builds a HostOptions by hand must test the game's default).
R2  host_rig.cpp's local for the same switch has the cvar's default too, or carries `RIG-DEFAULT: why` (the row / shot / GT / WB lanes then run the game's path).
R3  every `direct_plant` value the TestSeam documents (`N = ...` in the comment on direct_plant) appears as `'--direct-plant', 'N'` in tools\\scene\\chs_c6_check.py, and every `--fault-kind` value the rig
    accepts (1, 2) appears there too.
Exit 0 = clean."""
import re
import sys
from pathlib import Path

CVAR_FIELDS = [  # (HostOptions field, cvar, rig local)
    ('present_direct', 'wwe2k14_rdr_present_direct', 'present_direct'),
]


def cvar_default(start_text, cvar):
    m = re.search(r'REXCVAR_DEFINE_BOOL\(\s*%s\s*,\s*(true|false)' % re.escape(cvar), start_text)
    return m.group(1) if m else None


def check(start_text, host_h, rig_cpp, check_py):
    pr = []
    for field, cvar, local in CVAR_FIELDS:
        want = cvar_default(start_text, cvar)
        if want is None:
            pr.append('R1: cvar %s is not defined in start_settings.cpp' % cvar)
            continue
        for what, text, pat in (('R1 HostOptions::%s' % field, host_h, r'^\s*bool\s+%s\s*=\s*(true|false);(.*)$' % field),
                                ('R2 host_rig local %s' % local, rig_cpp, r'^\s*bool\s+%s\s*=\s*(true|false);(.*)$' % local)):
            m = re.search(pat, text, flags=re.M)
            if not m:
                pr.append('%s: declaration not found' % what)
            elif m.group(1) != want and 'RIG-DEFAULT:' not in m.group(2):
                pr.append('%s defaults to %s, the cvar %s to %s: mark the line `// RIG-DEFAULT: why` or make them agree' % (what, m.group(1), cvar, want))
    m = re.search(r'int\s+direct_plant\s*=\s*0;([^\n]*)', host_h)
    vals = sorted(set(re.findall(r'(\d)\s*=', m.group(1)))) if m else []
    if not vals:
        pr.append('R3: no direct_plant values documented in scene_host.h')
    for v in vals:
        if not re.search(r"'--direct-plant',\s*'%s'" % v, check_py):
            pr.append('R3: direct_plant %s is documented but chs_c6_check.py never runs it' % v)
    for v in ('1', '2'):
        if not re.search(r"'--fault-kind',\s*'%s'" % v, check_py):
            pr.append('R3: --fault-kind %s is never run by chs_c6_check.py' % v)
    return pr


def load(root):
    r = Path(root)
    rd = lambda p: (r / p).read_text(encoding='utf-8')
    return rd('src/start_settings.cpp'), rd('src/renderer_host/scene_host.h'), rd('tools/scene/host_rig.cpp'), rd('tools/scene/chs_c6_check.py')


def main():
    root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else '.'
    st, hh, rig, chk = load(root)
    pr = check(st, hh, rig, chk)
    if '--selftest' in sys.argv:
        if pr:
            print('SELFTEST: the clean tree must pass: %s' % pr)
            return 1
        plants = {
            'HostOptions default flipped': (st, re.sub(r'(bool\s+present_direct\s*=\s*)true;[^\n]*', r'\1false;', hh, count=1), rig, chk),
            'rig default flipped': (st, hh, re.sub(r'(bool\s+present_direct\s*=\s*)true;[^\n]*', r'\1false;', rig, count=1), chk),
            'plant 3 not run by the check': (st, hh, rig, chk.replace("'--direct-plant', '3'", "'--direct-plant', '9'")),
            'fault kind 2 not run': (st, hh, rig, chk.replace("'--fault-kind', '2'", "'--fault-kind', '1'")),
        }
        bad = 0
        for name, args in plants.items():
            p = check(*args)
            print('plant %-34s -> %s' % (name, 'caught' if p else 'NOT CAUGHT'))
            bad += 0 if p else 1
        print('SELFTEST %s' % ('FAIL' if bad else 'ok'))
        return 1 if bad else 0
    for p in pr:
        print('PROBLEM: ' + p)
    print('chs_lint: %d problem(s)' % len(pr))
    return 1 if pr else 0


if __name__ == '__main__':
    sys.exit(main())
