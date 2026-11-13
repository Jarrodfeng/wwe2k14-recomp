"""S101 N1 (go-anywhere): the goto self test (the gate entry, tools/gate/lanes/goto.json). No game run: plans only.

    py -I tools/goto/selftest.py

1. Every address form round-trips (parse -> print -> parse); malformed ones are refused with a reason.
2. --dry-run plans: every atlas node as menu:<id>, five match specs, all 46 wm30:N, one wm30:N@moment:K per era, the parse-only
   schemes. Each is a plan or "not reachable yet: why"; none is an error. The counts are printed.
3. The SCEX code list (out\\goto\\scex.json, made from the player's game files by tools/wm30/list.py --scex) has codes for all five
   eras and the X00 code of every one of the 46 matches (counts only here; the list stays under out\\).
4. The game-side parse rule (src/wm30_args.h, what LiveCommand runs) is COMPILED with clang++ (tools/goto/args_test.cpp) and fed the exact
   strings the executor builds (resolve.arm_arg) plus the typos of the S101 review; wiring text checked in src/wm30_jump.h /
   src/live_control.h. That the game itself compiles is the syntax-only compile of src/main.cpp, reported by the builder.
5. Planted faults, each must be caught: an atlas node that does not exist resolves to an ERROR (not a plan); a malformed address;
   a SCEX list with an era emptied; three faulted copies of the parse rule.
Exit 0 = all passed. Game files are read only through tools/wm30/list.py; nothing from them is printed except counts.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import address  # noqa: E402
import resolve  # noqa: E402

problems = []


def check(ok, what):
    if not ok:
        problems.append(what)
        print('FAIL: ' + what)
    return ok


VALID = [
    'menu:list:7', 'menu:setup:55', 'menu:play > one on one > ladder', 'menu:options',
    'match:Daniel Bryan vs Brock Lesnar; type=one on one > normal', 'match:Cena vs Orton@bell', 'match:Cena vs Orton@entrance:1',
    'match:Cena vs Orton@entrance:0@bell', 'wm30:0', 'wm30:45', 'wm30:12@moment:0', 'wm30:12@moment:2', 'wm30:12@moment:10501',
    'caw:head/hair', 'universe:week-3', 'cutscene:HH01_SCN_1A',
]
MALFORMED = [
    '', '   ', 'nowhere', 'foo:bar', 'menu:', 'match:', 'match:@bell', 'match:x@bell@bell', 'match:x@entrance:9', 'match:x@moment:1',
    'wm30:', 'wm30:46', 'wm30:-1', 'wm30:abc', 'wm30:3@moment:7', 'wm30:3@moment', 'wm30:3@bell', 'caw:head', 'caw:a/b/c', 'universe:a b',
    'cutscene:', 'menu:list:7@bell',
]
MATCH_SPECS = [
    'Daniel Bryan vs Brock Lesnar; type=one on one > normal',
    'John Cena vs Randy Orton; type=one on one > ladder',
    'Cena, Orton, Bryan, Punk, Sheamus, Del Rio; type=elimination chamber > normal',
    'royal rumble; type=royal rumble > 30; rumble=30 random',
    'Triple H vs The Undertaker; type=one on one > normal; pace=epic',
]


def roundtrip():
    for t in VALID:
        try:
            a = address.parse(t)
            b = address.parse(str(a))
        except address.AddressError as x:
            check(False, 'valid address refused: %s (%s)' % (t, x))
            continue
        check(a == b, 'round trip changed %s: %s -> %s' % (t, a, b))
    refused = 0
    for t in MALFORMED:
        try:
            address.parse(t)
            check(False, 'malformed address accepted: %r' % t)
        except address.AddressError as x:
            check(bool(str(x)), 'refusal without a reason: %r' % t)
            refused += 1
    print('1. round trips: %d valid forms parse and print back; %d malformed refused with a reason' % (len(VALID), refused))


def tally(plans):
    c = {'plan': 0, 'not_reachable': 0, 'error': 0}
    for p in plans:
        c[p.status] += 1
    return c


def plans():
    ctx = resolve.Context()
    atlas = ctx.atlas
    nodes = sorted(atlas.nodes)
    menu = [resolve.resolve('menu:' + n, ctx) for n in nodes]
    cm = tally(menu)
    check(len(nodes) > 200, 'the atlas has only %d nodes' % len(nodes))
    check(cm['error'] == 0, 'atlas nodes that do not resolve: %s' % [p.address for p in menu if p.status == 'error'][:5])
    kinds = {}
    for n in nodes:
        kinds[atlas.nodes[n].get('kind', '?')] = kinds.get(atlas.nodes[n].get('kind', '?'), 0) + 1
    print('2a. menu: %d atlas nodes (%s): %d plans, %d not reachable yet, %d errors' % (
        len(nodes), ', '.join('%s %d' % kv for kv in sorted(kinds.items())), cm['plan'], cm['not_reachable'], cm['error']))
    for p in menu:
        if p.status == 'not_reachable':
            print('     not reachable: %s' % p.lines()[0][:160])

    mt = [resolve.resolve('match:' + s, ctx) for s in MATCH_SPECS]
    mt += [resolve.resolve('match:' + MATCH_SPECS[0] + '@entrance:1', ctx), resolve.resolve('match:' + MATCH_SPECS[0] + '@bell', ctx)]
    cmt = tally(mt)
    check(cmt['error'] == 0, 'match specs that error: %s' % [(p.address, p.reason) for p in mt if p.status == 'error'])
    print('2b. match: %d addresses (5 specs, + @entrance, + @bell): %d plans, %d not reachable yet, %d errors' % (
        len(mt), cmt['plan'], cmt['not_reachable'], cmt['error']))

    w = [resolve.resolve('wm30:%d' % n, ctx) for n in range(46)]
    cw = tally(w)
    check(cw['plan'] == 46, 'wm30:N plans: %d of 46' % cw['plan'])
    d = ctx.scex
    first_of_era = {}
    for i in range(46):
        first_of_era.setdefault(d['matches'][i]['era'], i)
    mo = []
    for era, i in sorted(first_of_era.items()):
        for k in (0, 1, 2):
            mo.append(resolve.resolve('wm30:%d@moment:%d' % (i, k), ctx))
    mo.append(resolve.resolve('wm30:0@moment:99999', ctx))  # a raw code the era does not have: not reachable, not a plan
    cmo = tally(mo)
    check(cmo['error'] == 0, 'moment addresses that error: %s' % [(p.address, p.reason) for p in mo if p.status == 'error'])
    check(mo[-1].status == 'not_reachable', 'a raw code absent from the era list must be not reachable, got %s' % mo[-1].status)
    # S101 review finding 5: X01 / X02 skip X00's setup, so until a check covers them only X00 is a plan: 5 plans (one X00 per era),
    # 10 + 1 not reachable. Anything else means the gate in resolve.py (MOMENTS_CHECKED) moved without this test being told.
    ne = len(first_of_era)
    check(not resolve.MOMENTS_CHECKED, 'resolve.MOMENTS_CHECKED is on: update this count and say what check covered X01 / X02')
    check(cmo['plan'] == ne and cmo['not_reachable'] == 2 * ne + 1,
          'moments: want %d plans (X00 of each era) and %d not reachable, got %d / %d' % (ne, 2 * ne + 1, cmo['plan'], cmo['not_reachable']))
    for p in mo[:-1]:
        k = p.address.split('moment:')[1]
        check((p.status == 'plan') == (k == '0'), 'moment %s of %s is %s' % (k, p.address, p.status))
        if k != '0':
            check('X00' in p.reason and 'setup' in p.reason, 'the X01 / X02 refusal does not say why: %s' % p.reason)
    # a raw code that is in the era's script but in no match's trio: the game side refuses it, so the plan must too
    stray = [(e, c) for e in range(5) for c in d['eras'][e] if not resolve.era_accepts(d, e, c)]
    for e, c in stray[:3]:
        i = first_of_era[e]
        p = resolve.resolve('wm30:%d@moment:%d' % (i, c), ctx)
        check(p.status == 'not_reachable', 'a stray raw code %d of era %d resolved to %s' % (c, e, p.status))
    print('2c. wm30: 46 matches: %d plans, %d not reachable, %d errors; moments (X00 / X01 / X02 of the first match of each of %d eras + 1 absent '
          'raw code): %d plans (the X00s), %d not reachable yet (X01 / X02 need X00\'s setup, unchecked; the absent code), %d errors; %d stray raw '
          'codes in era scripts (in no match trio): refused' % (cw['plan'], cw['not_reachable'], cw['error'], ne, cmo['plan'],
                                                              cmo['not_reachable'], cmo['error'], len(stray)))

    import tempfile  # S101 lane 118 review: the caw: resolver reads the learned page table; this check must not depend on the real out\n2\caw_pages.json
    caw_tmp = os.path.join(tempfile.mkdtemp(prefix='goto_selftest_caw_'), 'caw_pages.json')
    open(caw_tmp, 'w').write('')   # an empty (unreadable) table = no table
    os.environ['WWE2K14_CAW_TABLE'] = caw_tmp
    other = [resolve.resolve(t, ctx) for t in ('caw:head/hair', 'universe:week-3', 'cutscene:HH01_SCN_1A')]
    check(all(p.status == 'not_reachable' and p.reason for p in other), 'caw / universe / cutscene must be not reachable with a reason (caw with no learned page table)')
    print('2d. caw / universe / cutscene: %d not reachable yet, each with its reason' % len(other))

    allp = menu + mt + w + mo + other
    ca = tally(allp)
    print('   TOTAL: %d addresses: %d reachable (a plan), %d not reachable yet, %d errors' % (len(allp), ca['plan'], ca['not_reachable'], ca['error']))
    return ctx


def scex_problems(d):
    """Problems in a SCEX list dict (the real one must have none; the planted twin must have some)."""
    out = []
    for era in range(5):
        if len(d['eras'].get(era, [])) < 10:
            out.append('era %d has %d codes' % (era, len(d['eras'].get(era, []))))
    for i in range(46):
        m = d['matches'].get(i)
        if not m:
            out.append('match %d missing' % i)
        elif m['code'] not in d['eras'].get(m['era'], []):
            out.append('match %d: X00 %d not in era %d list' % (i, m['code'], m['era']))
    return out


def scex(ctx):
    d = ctx.scex
    bad = scex_problems(d)
    check(not bad, 'SCEX list problems: %s' % bad[:5])
    per = [len(d['eras'][e]) for e in range(5)]
    full = sum(1 for i in range(46) if len([c for c in (d['matches'][i]['code'] + k for k in range(3)) if c in d['eras'][d['matches'][i]['era']]]) == 3)
    print('3. SCEX codes per era: %s; %d of 46 matches have all of X00 / X01 / X02' % (per, full))
    # positive control for the checker itself: a list with an era emptied must be reported (this one only tests scex_problems; the code
    # that READS the list is tested by every wm30 plan above, which would error on a list with no era)
    twin = {'eras': dict(d['eras']), 'matches': d['matches']}
    twin['eras'][2] = []
    check(bool(scex_problems(twin)), 'planted fault (era 2 emptied) was not caught')


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH): the !wm30 parse test cannot run')


def build_args_test(header_text, tmp, tag):
    """Compiles tools/goto/args_test.cpp against `header_text` as src/wm30_args.h (a copy under tmp, so a planted twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'wm30_args.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'args_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'args_test.cpp'), '-o', exe], capture_output=True, text=True,
                       timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('args_test did not compile: %s' % (r.stdout + r.stderr)[-600:])
    return exe


def run_args(exe, strings):
    r = subprocess.run([exe], input='\n'.join(strings) + '\n', capture_output=True, text=True, timeout=60)
    lines = r.stdout.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    return lines


def parse_cases(ctx):
    """[(string, expected)]: expected = ('ARM', n, code) | ('OFF',) | ('ERR',). The good strings are exactly those the executor builds
    (resolve.arm_arg over every wm30_arm step the resolver can make, X01 / X02 / raw codes included by lifting the MOMENTS_CHECKED gate
    for the test only); the bad ones are the typos the review found."""
    d = ctx.scex
    good = []
    keep = resolve.MOMENTS_CHECKED
    resolve.MOMENTS_CHECKED = True
    try:
        for n in range(46):
            for k in (0, 1, 2):
                pl = resolve.resolve('wm30:%d@moment:%d' % (n, k), ctx)
                for st in pl.steps:
                    if st['op'] == 'wm30_arm':
                        good.append((resolve.arm_arg(st['n'], st['code']), ('ARM', st['n'], st['code'])))
    finally:
        resolve.MOMENTS_CHECKED = keep
    for n in range(46):
        pl = resolve.resolve('wm30:%d' % n, ctx)
        for st in pl.steps:
            if st['op'] == 'wm30_arm':
                good.append((resolve.arm_arg(st['n'], st['code']), ('ARM', st['n'], st['code'])))
    good += [('off', ('OFF',)), ('  off \t', ('OFF',)), ('0', ('ARM', 0, 0)), ('45', ('ARM', 45, 0)), ('5 65535', ('ARM', 5, 65535)),
             (' 7   10300 ', ('ARM', 7, 10300))]
    bad = ['5 X01', '5 70000', '5 10101 junk', '5 10101 6', '46', '-1', '+3', '', '   ', '5 9999', '5 -10100', 'off 5', '5 off', 'off off',
           '5x', 'x5', '5 10100x', '5 1e4', '1000', '5 0x2774', '5 99999999999999999999', '3 3 3']
    return good + [(b, ('ERR',)) for b in bad]


def check_parse(exe, cases):
    """Problems from running the compiled parse rule over the cases (the real rule's, not a copy of it)."""
    out = run_args(exe, [c[0] for c in cases])
    if len(out) != len(cases):
        return ['args_test answered %d lines for %d inputs' % (len(out), len(cases))]
    bad = []
    for (text, want), got in zip(cases, out):
        g = got.split()
        have = ('ARM', int(g[1]), int(g[2])) if g[0] == 'ARM' else (g[0],)
        if have != want:
            bad.append('%r: want %s, the game parse gave %s' % (text, want, got[:60]))
    return bad


def game_side(ctx):
    jump = open(os.path.join(REPO, 'src', 'wm30_jump.h'), encoding='utf-8').read()
    live = open(os.path.join(REPO, 'src', 'live_control.h'), encoding='utf-8').read()
    header = open(os.path.join(REPO, 'src', 'wm30_args.h'), encoding='utf-8').read()
    # pieces present (the compile of the whole game is the main session's; this checks the wiring text)
    check(all(s in jump for s in ('LiveCommand', 'g_live_arm', 'g_boot_pending', 'ParseArmArgs', 'g_wm30_live', 'EraHasCode')),
          'the game-side live command is not in src/wm30_jump.h')
    check(all(s in live for s in ('g_wm30_live', 'directive == "wm30"', 'wm30_args', 'add_note')), 'the !wm30 directive is not in src/live_control.h')
    check('REXCVAR_GET(wwe2k14_wm30)' in jump and 'kStartPathRet' in jump, 'the original boot-time jump is gone')
    # finding 1: the hook consumes the boot jump; `!wm30 off` must not re-arm it
    live_cmd = jump[jump.index('inline std::string LiveCommand'):jump.index('// Registers LiveCommand')]
    check('g_boot_pending' not in live_cmd and 'wwe2k14_wm30' not in live_cmd, 'LiveCommand touches the boot jump again (!wm30 off would re-arm it)')
    # finding 4: the pipe thread must not read guest memory
    check('Lookup(' not in live_cmd and 'U32(' not in live_cmd, 'LiveCommand reads guest memory on the pipe thread')
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_args_test(header, tmp, 'real')
        cases = parse_cases(ctx)
        bad = check_parse(exe, cases)
        check(not bad, 'the game parse rule disagrees with what the executor builds / the typo list: %s' % bad[:4])
        ngood = sum(1 for c in cases if c[1][0] != 'ERR')
        # planted twins, each must be caught by the same comparison
        twins = {
            'extra words allowed': header.replace('w.size() > 2', 'w.size() > 3'),
            'code ceiling removed': header.replace('out.code > kMaxCode', 'false'),
            'off not recognised': header.replace('w[0] == "off"', 'w[0] == "of"'),
        }
        for what, text in twins.items():
            if text == header:
                check(False, 'planted twin "%s" did not change the header (the test lost its anchor)' % what)
                continue
            tb = check_parse(build_args_test(text, tmp, 'twin_' + re.sub(r'\W', '', what)), cases)
            check(bool(tb), 'planted fault (%s) was not caught' % what)
    print('4. game side: the real !wm30 parse rule (src/wm30_args.h, compiled with clang++) gave the expected answer on %d strings (%d the executor '
          'builds, %d typos refused); 3 planted rule faults caught; hook / live_control wiring present (compile = the main session\'s)' % (
              len(cases), ngood, len(cases) - ngood))


def planted(ctx):
    p = resolve.resolve('menu:list:9999', ctx)
    check(p.status == 'error' and not p.steps, 'planted: a node that is not in the atlas resolved to %s with %d steps' % (p.status, len(p.steps)))
    p2 = resolve.resolve('menu:there is no such screen anywhere', ctx)
    check(p2.status == 'error' and not p2.steps, 'planted: unknown path words resolved to %s' % p2.status)
    p3 = resolve.resolve('wm30:46', ctx)
    check(p3.status == 'error', 'planted: wm30:46 resolved to %s' % p3.status)
    p4 = resolve.resolve('match:Nobody Atall vs Nobody Else; type=one on one > normal', ctx)
    check(p4.status == 'error', 'planted: a match with unknown wrestlers resolved to %s' % p4.status)
    print('5. planted faults caught: unknown node -> ERROR "%s"; unknown words, wm30:46, unknown wrestlers -> ERROR' % p.reason[:70])


def main():
    roundtrip()
    try:
        ctx = plans()
        scex(ctx)
        planted(ctx)
        game_side(ctx)
    except RuntimeError as x:
        check(False, 'cannot read an input: %s' % x)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('goto selftest: all passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
