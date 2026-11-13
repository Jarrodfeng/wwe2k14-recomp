"""S101 lane 32 (the scene director): the offline acceptance test (the gate entry, tools/gate/lanes/scene_director.json). No game run.

    py -I tools/goto/scene_selftest.py

1. The three new address forms (scene:ID, match:SPEC@scene:IDS, match:SPEC@victory:ID[/L]) round-trip and malformed ones are refused.
2. The bank-id table (tools/goto/scene_ids.py, read from the player's plist360.def / plist360.arc; counts only are printed): the per-pac
   counts of the victory pacs match the reader's (S-2: 71 / 63 / 50 / 10), every id lies in its pac family's range, and EVERY id resolves
   (`scene:ID` -> a plan whose scene_play step (S101 lane 38; was scene_menu) names the pac the table holds); an id no pac holds is an ERROR, never a plan.
3. Three known past scenes re-found by id from the record: Lesnar's victory 6343 in Winning00 (= 6000 + his character id 343, S-2 [L], the
   chooser's per-wrestler rule), the default story scene 4025 in WSD00 (S-3; also read from the recompiled sub_82A19A18 when generated/
   is present), Bryan's entrance bank 324 in the entrance archive db/ids/characters.json names (ENTRANCE02 -> Entrance02).
4. The game-side parse rule (src/scene_director_args.h, what LiveCommand runs) is COMPILED with clang++ (tools/goto/scene_args_test.cpp)
   and fed the exact strings the executor builds (resolve.scene_arg) plus typos; three planted rule faults must be caught.
5. A static check of src/scene_director.h (TEXT PINS, said as such: the file is compiled only by the game build, whose syntax-only compile
   is the builder's): each verb calls only the functions of its recipe, FireMid in the recipe's order, the armed words default off, each
   hook's first act is one relaxed load, the pipe thread reads no guest memory, the wiring lines exist. Five planted text faults must
   be caught by the same checker.
6. The listed addresses are re-read in the recompiled code when generated/ is present (the caller return addresses of the two hooks).
Exit 0 = all passed. Game files are read only through tools/goto/scene_ids.py; nothing from them is printed except counts.
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
import scene_ids  # noqa: E402

problems = []


def check(ok, what):
    if not ok:
        problems.append(what)
        print('FAIL: ' + what)
    return ok


SPEC = 'Daniel Bryan vs Brock Lesnar; type=one on one > normal'
VALID = ['scene:4025', 'scene:100', 'scene:65535', 'match:%s@scene:324' % SPEC, 'match:%s@scene:324,343' % SPEC,
         'match:%s@scene:1,2,3,4,5,6' % SPEC, 'match:%s@after:324' % SPEC, 'match:%s@after:324,343' % SPEC, 'match:%s@victory:6343' % SPEC, 'match:%s@victory:6343/4' % SPEC,
         'match:%s@victory:7000/17' % SPEC, 'match:Cena vs Orton@bell', 'wm30:3']
MALFORMED = ['scene:', 'scene:0', 'scene:65536', 'scene:12a', 'scene:1,2', 'scene:-5', 'scene:4025@bell', 'match:x@scene:', 'match:x@scene:0',
             'match:x@scene:1,2,3,4,5,6,7', 'match:x@scene:1,,2', 'match:x@scene:a', 'match:x@scene:1@scene:2', 'match:x@victory:',
             'match:x@victory:6343/18', 'match:x@victory:6343/', 'match:x@victory:0', 'match:x@victory:1@victory:2',
             'match:x@scene:1@victory:2', 'match:x@after:', 'match:x@after:0', 'match:x@after:1,2,3,4,5,6,7', 'match:x@after:1@scene:2', 'match:x@after:1@bell', 'match:x@after:1@victory:2', 'match:x@scene:1@bell', 'match:x@victory:6343@entrance:1', 'match:x@bell@scene:1']


def forms():
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
    a = address.parse('match:%s@victory:6343/4' % SPEC)
    check(a.victory == 6343 and a.layout == 4 and not a.scene_ids, 'victory fields: %s' % (a,))
    a = address.parse('match:%s@scene:324,343' % SPEC)
    check(a.scene_ids == (324, 343) and a.victory == -1, 'scene fields: %s' % (a,))
    print('1. forms: %d valid addresses parse and print back; %d malformed refused with a reason' % (len(VALID), refused))


EXPECT_WINNING = {'Winning00': 71, 'Winning01': 63, 'Winning02': 50, 'Winning03': 10}  # S-2 (plist360.arc group rows)
RANGES = {'Season': (1010, 2110), 'Universe': (5000, 5661), 'Winning': (6100, 9500), 'WSD': (4000, 4670)}


def table_checks(ctx):
    t = ctx.scene_ids
    ids, pacs = t['ids'], t['pacs']
    check(len(ids) > 1500, 'the bank table has only %d ids' % len(ids))
    for pac, n in EXPECT_WINNING.items():
        check(pacs.get(pac, {}).get('count') == n, '%s holds %s ids, the reader (S-2) found %d' % (pac, pacs.get(pac, {}).get('count'), n))
    check(sum(p['count'] for p in pacs.values()) == len(ids), 'per-pac counts do not add up to the id count')
    for k, pac in ids.items():
        i = int(k)
        fam = re.match(r'[A-Za-z]+', pac).group(0)
        if fam == 'Entrance':
            check(i < 1000 or 10000 <= i < 15000, 'Entrance id %d of %s outside <1000 / 10000-14999 (the attire / variant copies)' % (i, pac))
        else:
            lo, hi = RANGES[fam]
            check(lo <= i <= hi, '%s id %d outside %d-%d' % (pac, i, lo, hi))
        check(scene_ids.family(i) in (None, fam.lower()), 'family(%d) = %s but the pac is %s' % (i, scene_ids.family(i), pac))
    # every id resolves: scene:ID is a plan whose scene_play step names the table's pac
    bad = 0
    for k, pac in ids.items():
        p = resolve.resolve('scene:%s' % k, ctx)
        if not (1 <= int(k) <= 699 or 4000 <= int(k) <= 5999):  # S101 lane 59: banks that do not hold in the stage are not_reachable (STAGE_CRASH.md)
            bad += p.status != 'not_reachable'
            continue
        ok = (p.status == 'plan' and [s['op'] for s in p.steps] == ['nav', 'scene_stage', 'expect_scene_log', 'scene_play', 'expect_scene_log'] and
              p.steps[3]['id'] == int(k) and p.steps[3]['pac'] == pac)  # S101 lane 38: scene:ID is the stage push, not the old menu verb
        bad += not ok
    check(bad == 0, '%d bank ids did not resolve to a plan naming their pac' % bad)
    # an id no pac holds is an ERROR, never a plan (every gap next to a held id, the reader's 6385, and the extremes)
    held = {int(k) for k in ids}
    gaps = sorted({i + 1 for i in held if i + 1 not in held} | {i - 1 for i in held if i - 1 not in held and i > 1} | {6385, 3999, 65535, 1})
    gaps = [g for g in gaps if g not in held]
    wrong = [g for g in gaps if resolve.resolve('scene:%d' % g, ctx).status != 'error']
    check(not wrong, 'ids in no pac that did not error: %s' % wrong[:6])
    for g in (6385, 3999):
        p = resolve.resolve('scene:%d' % g, ctx)
        check(p.status == 'error' and not p.steps and 'no scene pac' in p.reason, 'scene:%d must be an error naming the missing pac, got %s %s' % (g, p.status, p.reason))
    mid = resolve.resolve('match:%s@scene:324,6385' % SPEC, ctx)
    check(mid.status == 'error' and not mid.steps, 'a mid list holding an id no pac holds must be an error, got %s' % mid.status)
    vic = resolve.resolve('match:%s@victory:4025' % SPEC, ctx)
    check(vic.status == 'error', 'a victory id outside the Winning pacs must be an error, got %s' % vic.status)
    for gone in (6324, 6385):  # check C: 6324 (Bryan's would-be victory id) is in no pac: every verb's plan is an ERROR, never an arm
        for form in ('match:%s@victory:%d' % (SPEC, gone), 'scene:%d' % gone, 'match:%s@after:%d' % (SPEC, gone), 'match:%s@scene:324,%d' % (SPEC, gone)):
            check(resolve.resolve(form, ctx).status == 'error', 'a missing id must be refused: %s' % form)
    cena = resolve.resolve('match:%s@victory:6139' % SPEC, ctx)
    check(cena.status == 'plan' and ctx.scene_ids['ids'].get('6139') == 'Winning00', 'the check id 6139 (Cena, in Winning00, not Lesnar\'s 6343) must plan: %s' % cena.status)
    vic_gap = resolve.resolve('match:%s@victory:6385' % SPEC, ctx)
    check(vic_gap.status == 'error', 'a victory id in no pac must be an error, got %s' % vic_gap.status)
    print('2. bank table: %d ids in %d scene pacs; victory pacs %s; every id resolves to its pac (%d plans); %d ids in no pac (every gap edge, 6385, 3999, 65535) are ERRORs' % (
        len(ids), len(pacs), '/'.join(str(pacs[p]['count']) for p in sorted(EXPECT_WINNING)), len(ids), len(gaps)))


def past_scenes(ctx):
    t = ctx.scene_ids['ids']
    chars = json.load(open(os.path.join(REPO, 'db', 'ids', 'characters.json'), encoding='utf-8'))['entries']
    by_name = {c['name']: c for c in chars}
    lesnar, bryan = by_name['Brock Lesnar'], by_name['Daniel Bryan']
    # 1. Lesnar's victory: the chooser's per-wrestler id is 6000 + character id (S-2 point 2), in Winning00 (live: WINNING00)
    v = 6000 + lesnar['id']
    check(v == 6343 and t.get(str(v)) == 'Winning00', 'Lesnar (char %s): 6000 + id = %s is in %s, want 6343 in Winning00' % (lesnar['id'], v, t.get(str(v))))
    p = resolve.resolve('match:%s@victory:%d' % (SPEC, v), ctx)
    check(p.status == 'plan' and any(s['op'] == 'scene_victory' and s['id'] == 6343 for s in p.steps), 'the victory address for 6343 is not a plan: %s' % p.lines())
    # 2. the default story scene 4025 (S-3), WSD00
    check(t.get('4025') == 'WSD00', 'scene 4025 is in %s, want WSD00' % t.get('4025'))
    gen = os.path.join(resolve.main_checkout(), 'generated', 'default', 'wwe2k14_recomp.194.cpp')
    note = 'recompiled sub_82A19A18 not re-read (generated/ absent)'
    if os.path.isfile(gen):
        text = open(gen, encoding='utf-8', errors='replace').read()
        at = text.index('DEFINE_REX_FUNC(sub_82A19A18)')
        body = text[at:at + 20000]
        check('ctx.r5.s64 = 4025;' in body, 'sub_82A19A18 no longer holds the default 4025')
        note = 'default 4025 re-read in the recompiled sub_82A19A18'
    # 3. Bryan's entrance bank: the entrance archive characters.json names -> bank id = his character id (1v1 banks 343 / 324, S-1 [L])
    arch = bryan['entrance_archive']
    check(t.get(str(bryan['id'])) == 'Entrance' + arch[-2:], "Bryan's bank %s is in %s, the entrance archive %s says Entrance%s" % (bryan['id'], t.get(str(bryan['id'])), arch, arch[-2:]))
    check(t.get(str(lesnar['id'])) == 'Entrance' + lesnar['entrance_archive'][-2:], "Lesnar's bank is not in his entrance archive")
    print("3. past scenes re-found by id: Lesnar's victory 6343 = 6000 + %d, in Winning00; the default story scene 4025 in WSD00 (%s); Bryan's entrance bank %d in %s (%s)" % (
        lesnar['id'], note, bryan['id'], t[str(bryan['id'])], arch))


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH): the !scene parse test cannot run')


def build_args_test(header_text, tmp, tag):
    """Compiles tools/goto/scene_args_test.cpp against `header_text` as src/scene_director_args.h (a copy under tmp: a planted twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'scene_director_args.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'scene_args_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'scene_args_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('scene_args_test did not compile: %s' % (r.stdout + r.stderr)[-600:])
    return exe


def run_args(exe, strings):
    r = subprocess.run([exe], input='\n'.join(strings) + '\n', capture_output=True, text=True, timeout=60)
    lines = r.stdout.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    return lines


def parse_cases(ctx):
    """[(string, expected tuple)]: the good ones are exactly the strings the executor builds (resolve.scene_arg over the steps of real plans)."""
    good = []
    ids = [int(k) for k in ctx.scene_ids['ids']]
    for i in sorted(ids)[::7] + [4025, 6343, 100, 14198]:
        good.append((resolve.scene_arg('menu', [i]), ('MENU', i, 0, 2)))
    for lst in ([324], [324, 343], [1010, 4025, 5000, 6100, 7000, 8000], [9500, 100]):
        good.append((resolve.scene_arg('mid', lst), ('MID', len(lst)) + tuple(lst) + (0,)))
        good.append((resolve.scene_arg('after', lst), ('AFTER', len(lst)) + tuple(lst) + (0,)))
    for vid in [6343, 6100, 6800, 7000, 8803, 9101, 9500]:
        d = 4 if 6000 <= vid <= 6999 else -1
        good.append((resolve.scene_arg('victory', [vid]), ('VICTORY', vid, -1, d)))
    for vid, lay in ((6343, 4), (7000, 17), (8000, 0), (6343, 12)):
        d = 4 if 6000 <= vid <= 6999 else -1
        good.append((resolve.scene_arg('victory', [vid], lay), ('VICTORY', vid, lay, d)))
    good += [('off', ('OFF',)), ('  off \t', ('OFF',)), ('menu 4025 force', ('MENU', 4025, 1, 2)), ('mid 324,343 force', ('MID', 2, 324, 343, 1)),
             ('after 324 force', ('AFTER', 1, 324, 1)), ('menu 4025 mode=0', ('MENU', 4025, 0, 0)), ('menu 4025 mode=1 force', ('MENU', 4025, 1, 1)),
             ('menu 4025 force mode=2', ('MENU', 4025, 1, 2)), (resolve.scene_arg('menu', [4025], mode=0), ('MENU', 4025, 0, 0)),
             ('menu 65535', ('MENU', 65535, 0, 2)), ('menu 1', ('MENU', 1, 0, 2))]
    bad = ['', '   ', 'menu', 'menu 0', 'menu 65536', 'menu 12a', 'menu -5', 'menu +5', 'menu 4025 forse', 'menu 4025 force force', 'menu 4025 mode=3', 'menu 4025 mode=', 'menu 4025 mode=22', 'menu 4025 mode=0 mode=1', 'menu 4025 mode', 'menu 4025 force mode=0 x', 'after', 'after 1,2,3,4,5,6,7', 'after 0', 'after 1,,2', 'after 324 forse', 'after mode=1', 'menu 1,2', 'mid', 'mid 1,2,3,4,5,6,7',
           'mid 1,,2', 'mid ,1', 'mid 1,', 'mid 0', 'mid 1 2', 'mid 324,343 forse', 'victory', 'victory 6343/18', 'victory 6343/', 'victory /4', 'victory 6343/4/4',
           'victory 6343/-1', 'victory 0', 'victory 6343 4', 'victory 6343/004', 'off 1', 'off off', 'wm30 5', '5', 'scene menu 5', 'MENU 5', 'menu 1234567']
    return good + [(b, ('ERR',)) for b in bad]


def check_parse(exe, cases):
    out = run_args(exe, [c[0] for c in cases])
    if len(out) != len(cases):
        return ['scene_args_test answered %d lines for %d inputs' % (len(out), len(cases))]
    bad = []
    for (text, want), got in zip(cases, out):
        g = got.split()
        have = tuple([g[0]] + [int(x) for x in g[1:]]) if g and g[0] != 'ERR' else (g[0] if g else '',)
        if have != want:
            bad.append('%r: want %s, the game parse gave %s' % (text, want, got[:70]))
    return bad


def parse_rule(ctx):
    header = open(os.path.join(REPO, 'src', 'scene_director_args.h'), encoding='utf-8').read()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_args_test(header, tmp, 'real')
        cases = parse_cases(ctx)
        bad = check_parse(exe, cases)
        check(not bad, 'the game parse rule disagrees with what the executor builds / the typo list: %s' % bad[:4])
        ngood = sum(1 for c in cases if c[1][0] != 'ERR')
        twins = {
            'id zero allowed': header.replace('v < 1 ||', 'v < 0 ||'),
            'layout cap removed': header.replace('constexpr int kMaxLayout = 17;', 'constexpr int kMaxLayout = 18;'),
            'unknown option allowed': header.replace('else return "error: menu: the only options are', 'else if (true) {} else return "error: menu: the only options are'),
            'mode range widened': header.replace("w[k][5] <= '2'", "w[k][5] <= '3'"),
            'after treated as mid': header.replace('out.verb = name == "mid" ? Verb::kMid : Verb::kAfter;', 'out.verb = Verb::kMid;'),
            'wrong per-wrestler layout': header.replace('? 4 : -1', '? 5 : -1'),
        }
        for what, text in twins.items():
            if text == header:
                check(False, 'planted twin "%s" did not change the header (the test lost its anchor)' % what)
                continue
            tb = check_parse(build_args_test(text, tmp, 'twin_' + re.sub(r'\W', '', what)), cases)
            check(bool(tb), 'planted fault (%s) was not caught' % what)
    print('4. game side: the real !scene parse rule (src/scene_director_args.h, compiled with clang++) gave the expected answer on %d strings (%d the executor '
          'builds, %d typos refused); %d planted rule faults caught' % (len(cases), ngood, len(cases) - ngood, len(twins)))


# ---- 5. the static check (text pins) ----

MENU_ALLOWED = {'sub_8283CFF0'}
BANK_ALLOWED = {'sub_8231BEC8'}
MID_ORDER = ['sub_827D7398', 'sub_827F6060', 'sub_827F5CC0', 'sub_827D7578', 'sub_827F5CD8', 'sub_827F5CF0', 'sub_82C2D808', 'sub_82C2D8C0', 'sub_82C2DE28',
             'sub_82C2E1E0', 'sub_82C2D8C0', 'sub_82C2D940', 'sub_82730CE8', 'sub_8231D9D8', 'sub_82C2D940', 'sub_82C2DE28']  # sub_82738060 0x827380AC-0x827381A4
AFTER_ORDER = ['sub_827D7398', 'sub_827F6060', 'sub_827F5CC0', 'sub_827D7578', 'sub_827F5CD8', 'sub_827F5CF0']  # the sub_82738060 table writes; no kind-6 record (review: type 10 would resume "play the match")
FUNC_RE = re.compile(r'sub_[0-9A-F]{8}')


def body_of(text, signature):
    """The brace-balanced body of the function whose line starts with `signature` ('' if absent)."""
    at = text.find(signature)
    if at < 0:
        return ''
    start = text.find('{', at)
    depth, i = 0, start
    while i < len(text):
        depth += text[i] == '{'
        depth -= text[i] == '}'
        i += 1
        if depth == 0:
            break
    return re.sub(r'//[^\n]*', '', text[start:i])  # comments name functions too: not part of the check


def static_problems(src, live, scen, app):
    """Problems in the game-side text (the real files must have none; every planted twin must have at least one)."""
    p = []

    def calls(body):
        return re.findall(r'call\((sub_[0-9A-F]{8}|__imp__sub_[0-9A-F]{8})', body)

    menu, bank, mid = body_of(src, 'inline std::string FireMenu('), body_of(src, 'inline bool BankExists('), body_of(src, 'inline std::string FireMid(')
    live_cmd = body_of(src, 'inline std::string LiveCommand(')
    for name, body in (('FireMenu', menu), ('BankExists', bank), ('FireMid', mid), ('LiveCommand', live_cmd)):
        if not body:
            p.append('%s not found' % name)
    # a function name anywhere in a body that is not in its recipe (call sites or otherwise)
    extra = set(FUNC_RE.findall(menu)) - MENU_ALLOWED
    if extra:
        p.append('FireMenu names functions outside its recipe: %s' % sorted(extra))
    extra = set(FUNC_RE.findall(bank)) - BANK_ALLOWED
    if extra:
        p.append('BankExists names functions outside its recipe: %s' % sorted(extra))
    extra = set(FUNC_RE.findall(mid)) - set(MID_ORDER)
    if extra:
        p.append('FireMid names functions outside its recipe: %s' % sorted(extra))
    order = [c for c in calls(mid)]
    if order != MID_ORDER:
        p.append('FireMid calls %s, the recipe (sub_82738060) is %s' % (order, MID_ORDER))
    if calls(menu) != ['sub_8283CFF0']:
        p.append('FireMenu calls %s, want only the exit request' % calls(menu))
    # armed words default off
    for decl in ('std::atomic<int32_t> g_menu_id{-1};', 'std::atomic<int32_t> g_victory_id{-1};', 'std::atomic<int32_t> g_victory_layout{-1};',
                 'std::atomic<int32_t> g_pending{0};'):
        if decl not in src:
            p.append('armed word not defaulting off: %s' % decl)
    # hooks: one function each, first act a relaxed load, only the original is called
    for hook, orig, flag in (('REX_HOOK_RAW(sub_82A19A18) {', '__imp__sub_82A19A18', 'g_menu_id'), ('REX_HOOK_RAW(sub_8231BEC8) {', '__imp__sub_8231BEC8', 'g_victory_id')):
        body = body_of(src, hook)
        if not body:
            p.append('hook %s not found' % hook)
            continue
        first = body.find(orig)
        load = body.find('%s.load(std::memory_order_relaxed)' % flag)
        if load < 0 or first < 0 or load > first:
            p.append('hook %s: the first act is not one relaxed load of %s' % (hook, flag))
        if set(FUNC_RE.findall(body)) - {orig[len('__imp__'):]}:
            p.append('hook %s calls other functions: %s' % (hook, sorted(set(FUNC_RE.findall(body)))))
    if src.count('REX_HOOK_RAW(sub_82A19A18)') != 1 or src.count('REX_HOOK_RAW(sub_8231BEC8)') != 1:
        p.append('not exactly one REX_HOOK_RAW per function')
    # the pipe thread must not read guest memory
    if re.search(r'U32\(|Guest\(|TranslateVirtual|SequenceId\(', live_cmd):
        p.append('LiveCommand reads guest memory on the pipe thread')
    # wiring and constants (text pins)
    for need in ('kMenuExitRet = 0x827DD2A4', 'kVictoryTestRet = 0x827C5780', 'kManagerPtr = 0x836F2E58', 'kG = 0x8376AAF4', 'kExitActionMainScene = 5',
                 'g_scene_live = &LiveCommand', 'g_director_tick = &Tick'):
        if need not in src:
            p.append('constant / wiring missing in scene_director.h: %s' % need)
    if 'directive == "scene"' not in live or 'g_scene_live' not in live:
        p.append('the !scene directive is not in src/live_control.h')
    if 'g_director_tick' not in scen or 'g_director_tick(ctx, base)' not in scen:
        p.append('the director tick is not wired into src/scenario.h')
    if '#include "scene_director.h"' not in app:
        p.append('src/wwe2k14_app.h does not include scene_director.h')
    # review fixes (lane 32 review): F1 T saved before the clear and restored after sub_8231D9D8; F2 a victory scene dies with the match; F3 the
    # record +18 byte is written explicitly; F4 a failed create is an error and rolls back, `!scene off` stops the log; the forced marker.
    tick = body_of(src, 'inline void Tick(')
    vic = body_of(src, 'REX_HOOK_RAW(sub_8231BEC8) {')
    menu_hook = body_of(src, 'REX_HOOK_RAW(sub_82A19A18) {')
    save, clear, restore = mid.find('std::memcpy(saved_table, Guest(t)'), mid.find('call(sub_827F6060'), mid.find('std::memcpy(Guest(t), saved_table')
    if not (0 <= save < clear and mid.find('call(sub_8231D9D8') < restore):
        p.append('F1: FireMid must save T before the clear and restore it after sub_8231D9D8')
    if 'return "failed:' not in mid or 'if (!r) {' not in mid or mid.find('if (!r) {') < restore:
        p.append('F4: FireMid must roll T back and answer an error when R is 0')
    if 'g_prev_seq == kSeqInGame' not in tick or 'g_victory_id.store(-1)' not in tick:
        p.append('F2: Tick must drop an armed victory scene when the match is left (9 -> 75)')
    if '(id >= 6000 && id <= 6999 && ctx.r19.u32 == 4) ? 1 : 0;' not in vic:
        p.append('F3: the victory hook must write the record +18 byte explicitly')
    if 'g_cancel_log.store(true)' not in live_cmd or 'g_cancel_log.exchange' not in tick:
        p.append('F4: `!scene off` must stop the mid-match log')
    if 'g_pending.load(std::memory_order_relaxed) != 0 ?' not in tick:
        p.append('Tick must gate its exchange behind a relaxed load')
    for name, body in (('menu hook', menu_hook), ('victory hook', vic)):
        if 'scene_forced::Set(' not in body:
            p.append('forced marker: the %s does not set it' % name)
    if 'scene_forced::Set(scene_forced::kMid' not in mid:
        p.append('forced marker: FireMid does not set it')
    # check C: every id armed is validated against the pac index on the game thread (a missing id is refused with a WARN naming it, never armed)
    arm = body_of(src, 'inline std::string ArmVictory(')
    if not arm or 'BankExists(call, id)' not in arm or 'REXLOG_WARN' not in arm or 'return "refused:' not in arm or arm.find('BankExists') > arm.find('g_victory_id.store'):
        p.append('ArmVictory must refuse an id the pac index does not hold (WARN naming it) before arming')
    for name, body in (('ArmVictory', arm), ('FireMenu', menu), ('FireMid', mid), ('FireAfter', body_of(src, 'inline std::string FireAfter('))):
        if 'g_menu_seen' not in body:
            p.append('%s must wait until the main menu has been seen (the /EVP lookup reads the pac index)' % name)
    if 'ArmVictory(ctx, base, a.ids[0]' not in tick:
        p.append('Tick must serve the victory request (kind 4) through ArmVictory')
    if 'DefaultVictoryLayout(' in src:
        p.append('the director forces a layout only when the user gave /L (the ladder keeps its own otherwise)')
    if 'the earlier arm' not in arm:
        p.append('a refused victory arm must say that an earlier arm stays')
    if 'ArmVictory(ctx, base, va.ids[0]' not in tick or 'g_victory_id.store(va.ids' in tick or 'g_victory_id.store(a.ids' in live_cmd:
        p.append('the cvar and the directive must arm only through ArmVictory (validated), never store the word directly')
    for name, body in (('FireMenu', menu), ('FireMid', mid), ('FireAfter', body_of(src, 'inline std::string FireAfter('))):
        if 'BankExists(call' not in body or '!a.force' not in body:
            p.append('%s must validate its ids with BankExists unless forced' % name)
    # check B follow-ups: `after` (state 25 via G+468), the program mode hook, the early victory arm
    after = body_of(src, 'inline std::string FireAfter(')
    if calls(after) != AFTER_ORDER:
        p.append('FireAfter calls %s, want %s' % (calls(after), AFTER_ORDER))
    if set(FUNC_RE.findall(after)) - set(AFTER_ORDER):
        p.append('FireAfter names functions outside its recipe: %s' % sorted(set(FUNC_RE.findall(after)) - set(AFTER_ORDER)))
    if '468' in re.sub(r'refused: G\+468[^\n]*', '', after.replace('kG + 468', 'KG468OK')).replace('KG468OK', '') and '*Guest(g + 468) = 1' in after:
        p.append('FireAfter must not write G+468 (the sub_827C18E0 hook does, at the match-end step)')
    if 'sub_82730CE8' in after or 'sub_8231D9D8' in after or 'sub_827F5D90' in after:
        p.append('FireAfter must not create the manager, start a list, or add a kind-6 record')
    if '*Guest(kG + 468) != 0' not in after:
        p.append('FireAfter must refuse when G+468 is already set')
    if 'g_after_armed) return' not in mid:
        p.append('FireMid must refuse while scenes are queued (after)')
    end_hook = body_of(src, 'REX_HOOK_RAW(sub_827C18E0) {')
    if not end_hook or end_hook.find('g_after_hook.load(std::memory_order_relaxed)') < 0 or end_hook.find('g_after_hook.load') > end_hook.find('__imp__sub_827C18E0') or '*wwe2k14::scenario::Guest(kG + 468) = 1;' not in end_hook:
        p.append('the match-end hook must start with one relaxed load and set G+468 = 1')
    if 'g_menu_swapped.exchange(false)' not in src or 'g_menu_swapped.store(true)' not in menu_hook:
        p.append('the mode hook must apply only after the sub_82A19A18 hook swapped in the same exit')
    if 'g_menu_mode.store(-1);\n    g_menu_swapped.store(false);' not in tick:
        p.append('the 600-frame menu timeout must clear the mode too')
    if 'g_prev_seq == kSeqInGame && g_after_armed' not in tick:
        p.append('an unhanded after arm must drop when the match is left')
    if not re.search(r'g_cancel_after\.exchange\(false\) && g_after_armed\) \{\s*if \(U32\(kG \+ 472\) != 0\) \{', tick):
        p.append('`!scene off` after the match end must be too late, not a withdraw')
    if 'menu: playback finished' not in tick:
        p.append('Tick must log the menu scene list end')
    if 'g_cancel_after.store(true)' not in live_cmd or 'g_cancel_after.exchange' not in tick:
        p.append('`!scene off` must withdraw queued after scenes')
    mode_hook = body_of(src, 'REX_HOOK_RAW(sub_8225E620) {')
    if not mode_hook or mode_hook.find('g_menu_mode.load(std::memory_order_relaxed)') < 0 or mode_hook.find('g_menu_mode.load') > mode_hook.find('__imp__sub_8225E620'):
        p.append('the program-mode hook must start with one relaxed load of g_menu_mode')
    if 'kMenuStartRet = 0x827DD2A8' not in src or 'kProgramMode = 0x839295C0' not in src:
        p.append('mode hook constants missing')
    if 'std::atomic<int32_t> g_menu_mode{-1};' not in src:
        p.append('g_menu_mode must default off')
    if 'wwe2k14_scene_victory' not in src or 'cvar_applied' not in tick:
        p.append('--wwe2k14_scene_victory is not applied by Tick')
    return p


def static_check():
    rd = lambda n: open(os.path.join(REPO, 'src', n), encoding='utf-8').read()
    src, live, scen, app = rd('scene_director.h'), rd('live_control.h'), rd('scenario.h'), rd('wwe2k14_app.h')
    got = static_problems(src, live, scen, app)
    check(not got, 'static check: %s' % got[:4])
    man = open(os.path.join(REPO, 'src', 'case', 'case_manifest.h'), encoding='utf-8').read()
    bm = rd('bookmark.h')
    intake = open(os.path.join(REPO, 'tools', 'case', 'intake.py'), encoding='utf-8').read()
    check('\\"forced\\": {\\"verb\\"' in man and 'std::optional<Forced> forced;' in man, 'the manifest does not carry where.forced')
    check('scene_forced::Get(' in bm and 'm.where.forced' in bm, 'bookmark.h FillCaseManifest does not fill where.forced')
    check("w.get('forced')" in intake and 'FORCED SCENE' in intake, 'tools/case/intake.py does not print where.forced on line 2')
    twins = {
        'FireMenu calls a function outside its recipe': src.replace('call(sub_8283CFF0, {sm,', 'call(sub_82AAAAAA, {sm,'),
        'FireMid out of recipe order': src.replace('call(sub_827F5CC0, {t, 3});', 'call(sub_827F5CD8, {t, 3});', 1),
        'armed word defaults on': src.replace('g_menu_id{-1};', 'g_menu_id{4025};', 1),
        'hook without the relaxed load first': src.replace('const int32_t id = g_victory_id.load(std::memory_order_relaxed);',
                                                                 'const int32_t id = 0; (void)id; __imp__sub_8231BEC8(ctx, base); const int32_t id2 = g_victory_id.load(std::memory_order_relaxed);', 1),
        'FireAfter creates the manager': src.replace('  g_after_hook.store(true);\n  g_after_armed = true;', '  call(sub_82730CE8, {});\n  g_after_hook.store(true);\n  g_after_armed = true;', 1),
        'victory ArmVictory without the menu-seen guard': src.replace('inline std::string ArmVictory(PPCContext& ctx, uint8_t* base, int32_t id, int layout, const char* source) {\n  if (!g_menu_seen)', 'inline std::string ArmVictory(PPCContext& ctx, uint8_t* base, int32_t id, int layout, const char* source) {\n  if (false)', 1),
        'tick victory branch does not call ArmVictory': src.replace('ArmVictory(ctx, base, a.ids[0], a.layout, "!scene")', 'std::string("armed")', 1),
        'director defaults a layout': src.replace('ArmVictory(ctx, base, a.ids[0], a.layout, "!scene")', 'ArmVictory(ctx, base, a.ids[0], scene::DefaultVictoryLayout(a.ids[0]), "!scene")', 1),
        'victory armed without the id check': src.replace('  if (!BankExists(call, id)) {\n    const int32_t earlier', '  if (false) {\n    const int32_t earlier', 1),
        'victory cvar stores the word directly': src.replace('ArmVictory(ctx, base, va.ids[0], va.layout, "--wwe2k14_scene_victory");', 'g_victory_id.store(va.ids[0]);', 1),
        'mid and after skip the id check': src.replace('BankExists(call, a.ids[i])', 'false'),
        'FireAfter writes G+468 early': src.replace('  g_after_hook.store(true);\n  g_after_armed = true;', '  *Guest(g + 468) = 1;\n  g_after_hook.store(true);\n  g_after_armed = true;', 1),
        'FireAfter adds the kind-6 record': src.replace('  g_after_hook.store(true);\n  g_after_armed = true;', '  call(sub_827F5D90, {t});\n  call(sub_827F5CC0, {t, 6});\n  g_after_hook.store(true);\n  g_after_armed = true;', 1),
        'FireAfter does not refuse a set G+468': src.replace('*Guest(kG + 468) != 0', '*Guest(kG + 468) > 255', 1),
        'FireMid allowed while after is queued': src.replace("  if (g_after_armed) return \"refused: scenes are queued for this match's end ('after');", "  if (false) return \"refused: scenes are queued for this match's end ('after');", 1),
        'match-end hook never sets the byte': src.replace('*wwe2k14::scenario::Guest(kG + 468) = 1;', '', 1),
        'mode applies without the swap': src.replace(' && g_menu_swapped.exchange(false)', '', 1),
        'menu timeout keeps the mode': src.replace('    g_menu_mode.store(-1);\n    g_menu_swapped.store(false);\n  }\n  if (g_menu_watch', '  }\n  if (g_menu_watch', 1),
        'after arm survives the match': src.replace('g_prev_seq == kSeqInGame && g_after_armed', 'false', 1),
        'off after the end withdraws': src.replace('U32(kG + 472) != 0) {  // review F2', 'false) {  // review F2', 1),
        'off keeps after queued': src.replace('g_cancel_after.store(true);', '', 1),
        'mode hook without the relaxed load': src.replace('const int32_t mode = g_menu_mode.load(std::memory_order_relaxed);', 'const int32_t mode = 0;', 1),
        'F1 T not restored': src.replace('std::memcpy(Guest(t), saved_table, kTableBytes);', '', 1),
        'F2 victory survives the match': src.replace('g_prev_seq == kSeqInGame', 'false', 1),
        'F3 byte only for 6xxx': src.replace('(id >= 6000 && id <= 6999 && ctx.r19.u32 == 4) ? 1 : 0;', '1;', 1).replace('*wwe2k14::scenario::Guest(ctx.r1.u32 + 80) = 1;', '', 1),
        'F4 failed create answers fired': src.replace('return "failed:', 'return "fired:', 1).replace('if (!r) {', 'if (false) {', 1),
        'F4 off leaves the log running': src.replace('g_cancel_log.store(true);', '', 1),
        'forced marker not set by the menu hook': src.replace('wwe2k14::scene_forced::Set(wwe2k14::scene_forced::kMenu', '(void)(wwe2k14::scene_forced::kMenu', 1),
        'Tick exchanges without the relaxed gate': src.replace('g_pending.load(std::memory_order_relaxed) != 0 ?', 'true ?', 1),
        'pipe thread reads guest memory': src.replace('const scene::SceneArgs a;', '', 1).replace(
            'const std::string error = scene::ParseSceneArgs(args, a);', 'const std::string error = scene::ParseSceneArgs(args, a); (void)wwe2k14::scenario::U32(0x8376AAF4);', 1),
    }
    for what, text in twins.items():
        if text == src:
            check(False, 'planted twin "%s" did not change the file (the test lost its anchor)' % what)
            continue
        check(bool(static_problems(text, live, scen, app)), 'planted fault (%s) was not caught' % what)
    print('5. static check (text pins) of src/scene_director.h: verb recipes, FireMid order (%d calls), armed words default off, hooks start with one relaxed load, '
          'no guest reads on the pipe thread, wiring in live_control.h / scenario.h / wwe2k14_app.h; %d planted text faults caught' % (len(MID_ORDER), len(twins)))


def provenance():
    """The two caller return addresses in the hooks are read back from the recompiled code when generated/ is present."""
    gen = os.path.join(resolve.main_checkout(), 'generated', 'default')
    pins = (('wwe2k14_recomp.38.cpp', 'ctx.lr = 0x827DD2A4;\n\tsub_82A19A18(ctx, base);', '0x827DD2A4 = sub_827DD240 calling sub_82A19A18'),
            ('wwe2k14_recomp.46.cpp', 'ctx.lr = 0x827C5780;\n\tsub_8231BEC8(ctx, base);', '0x827C5780 = sub_827C4430 calling sub_8231BEC8'),
            ('wwe2k14_recomp.89.cpp', 'loc_8283353C:\n\t// lis r10,-31901\n\tctx.r10.s64 = -2090663936;\n\t// li r11,1\n\tctx.r11.s64 = 1;\n\t// stw r11,-30068(r10)\n\tREX_STORE_U32(ctx.r10.u32 + -30068, ctx.r11.u32);\n\t// bl 0x827dd240',
             'dispatcher case 4 (exit action 5) calls sub_827DD240'),
            ('wwe2k14_recomp.38.cpp', 'ctx.lr = 0x827DD2A8;\n\tsub_8225E620(ctx, base);', '0x827DD2A8 = sub_827DD240 calling sub_8225E620 (mode hook)'),
            ('wwe2k14_recomp.138.cpp', 'RE:REX_LOAD_U8\\(ctx\\.r3\\.u32 \\+ 468\\);(?s:.{0,200}?)goto loc_827C1910;(?s:.{0,60}?)ctx\\.r11\\.s64 = 25;', 'sub_827C18E0: G+468 byte != 0 selects flow step 25 (after)'))
    done = []
    for f, needle, what in pins:
        path = os.path.join(gen, f)
        if not os.path.isfile(path):
            print('6. provenance: %s not present, %s not re-read' % (f, what))
            continue
        text = open(path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
        if check((re.search(needle[3:], text) is not None) if needle.startswith('RE:') else needle in text, 'the recompiled code no longer shows: %s' % what):
            done.append(what)
    print('6. provenance re-read in the recompiled code: %s' % ('; '.join(done) if done else 'none (generated/ absent)'))


def _plan_arena(ctx, text):
    """-> (arena of the scene_play step or None, all notes joined) for a scene address."""
    plan = resolve.resolve(address.parse(text), ctx)
    if plan.status != 'plan':
        return None, '%s: %s' % (plan.status, plan.reason if hasattr(plan, 'reason') else '')
    play = [st for st in plan.steps if st.get('op') == 'scene_play'][0]
    return play['arena'], ' | '.join(plan.notes)


def set_pins(ctx):
    """S101 lane 77: scene:ID without @arena plans the game's own set (tools/goto/scene_sets.py); an explicit @arena wins; the show's arena and an
    arena without a pac fall back to 1 (RAW) with a note. Returns the number of pins that failed (for the planted twins)."""
    bad = 0
    for text, want, note in (('scene:4670', 78, 'catalogue'), ('scene:4670@arena:4', 4, 'reloads the set from BG04'),
                             ('scene:5000', 1, "the show's arena"), ('scene:4244', 1, 'no BG18.PAC')):
        got, notes = _plan_arena(ctx, text)
        if got != want or note not in notes:
            bad += 1
            problems.append('set pin %s: arena %r (wanted %d), note has %r: %s' % (text, got, want, note, note in notes)) if ctx is REAL else None
    return bad


REAL = None


def set_pin_checks(ctx):
    global REAL
    import scene_sets
    REAL = ctx
    n0 = len(problems)
    set_pins(ctx)
    ok = len(problems) == n0
    # planted twins: a table that is wrong in each way must make the pins fail (positive control)
    real = ctx.scene_sets
    fakes = {'4670 -> 4 (catalogue ignored)': {4670: 4}, '5000 -> 78 (the show arena treated as backstage)': {5000: 78},
             '4244 -> 4 (the no-pac fallback bypassed)': {4244: 4}}
    for what, patch in fakes.items():
        t = dict(real)
        for b, a in patch.items():
            t[b] = dict(real[b], arena=a)
        twin = resolve.Context(scene_sets=t)
        if set_pins(twin) == 0:
            check(False, 'PLANT NOT CAUGHT in the set pins: %s' % what)
    # an explicit @arena must win even over a table that says otherwise
    check(_plan_arena(resolve.Context(scene_sets={4670: dict(real[4670], arena=9)}), 'scene:4670@arena:4')[0] == 4, 'an explicit @arena:4 must beat the table')
    print('7. scene sets: scene:4670 -> 78, @arena:4 keeps 4, 5000 -> 1 with the show note, 4244 (arena 18, no pac) -> 1; %d planted tables caught%s' % (len(fakes), '' if ok else ' (REAL PINS FAILED)'))


def main():
    try:
        ctx = resolve.Context()
        forms()
        table_checks(ctx)
        past_scenes(ctx)
        parse_rule(ctx)
        static_check()
        provenance()
        set_pin_checks(ctx)
    except RuntimeError as x:
        check(False, 'cannot read an input: %s' % x)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('scene director selftest: all passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
