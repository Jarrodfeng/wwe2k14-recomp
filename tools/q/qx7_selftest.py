"""qx7_selftest: proves the QX4 verbs (q vcall, q nodes, q runlog, q ours refs ADDRESS) on a fixture in a TEMP folder, with PLANTED misses (no game, 0 runs):
  vcall    a listing with: a `this` virtual call (+52) in a vtable-slot function (class known from the slot table), a callback-field call (not a vtable load: hidden
           unless --all), a call whose class is known from a vtable constant stored in the same function (+76), a virtual call on a call result (class unknown:
           --class must COUNT it as unresolved, never list it), and a PLANTED miss: `lwz r10,52(r11)` whose `mtctr` comes after a call (the register died: no site);
  nodes    a registration `bl 0x82C30A10` with `li r4,36` and a vtable constant stored at +16 of the object (the node is r31+16): category, vtable, class and slot 1 found;
           a registration with a category in a register is listed as unresolved, not guessed;
  runlog   two fixture logs (route, flags, census lines, pac opens): words, --exe / --route filters, `which --mode` (a pac opened by only one route; a PLANTED
           name no run opened answers none), incremental update (a changed log is re-read) and a deleted log keeps its rows (`log gone`);
  refs     `q ours refs 0x836F38D0` finds the constexpr names (a multi-declarator line, a #define) and NOT a string literal or another address.
Then the real tree (skipped when generated\\ or the index is absent): COV1's misses (slot-13 site 0x82697C0C in sub_826979F8, slot-19 site of sub_827C9BC8), categories 23 / 26 /
36 of the node registry, the address-held constants.
Run:  py -I tools/q/qx7_selftest.py
"""
import contextlib
import io
import json
import os
import re
import sqlite3
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qrunlog  # noqa: E402
import qx6  # noqa: E402
import qx7  # noqa: E402

VT = 0x82100000
F1, F2, F3, F4, F5, F6, F7 = 0x82001000, 0x82002000, 0x82003000, 0x82004000, 0x82005000, 0x82006000, 0x82007000
F8, F9, F10, F11 = 0x82008000, 0x82009000, 0x8200A000, 0x8200B000
F12, F13, F14, F15, F16 = 0x8200C000, 0x8200D000, 0x8200E000, 0x8200F000, 0x82010000
HELPER = 0x82C78DF4


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{str(out)[-1500:]}')
    print(f'PASS  {what}')


def fn(name, ins):
    return f'DEFINE_REX_FUNC({name}) {{\n' + ''.join(f'\t// {i}\n' for i in ins) + '}\n'


def raw(name, items):
    """like fn() but an item `loc_XXXXXXXX:` is a label line"""
    return f'DEFINE_REX_FUNC({name}) {{\n' + ''.join((i + '\n') if i.startswith('loc_') else f'\t// {i}\n' for i in items) + '}\n'


def listing():
    s = '#include "wwe2k14_funcs.0.h"\n\n'
    s += fn('sub_82001000', ['mr r31,r3', 'lwz r11,0(r3)', 'lwz r10,52(r11)', 'mtctr r10', 'bctrl ', 'blr'])                                  # bctrl = F1 + 16
    s += fn('sub_82002000', ['lwz r10,52(r11)', 'mtctr r10', 'bctrl ', 'blr'])                                                                 # callback field
    s += fn('sub_82003000', ['mr r31,r3', 'lis r9,-32240', 'addi r9,r9,0', 'stw r9,0(r31)', 'lwz r11,0(r31)', 'lwz r10,76(r11)', 'mtctr r10', 'bctrl ', 'blr'])
    s += fn('sub_82004000', ['mr r31,r3', 'lis r11,-32240', 'addi r11,r11,0', 'stw r11,16(r31)', 'addi r3,r31,16', 'li r4,36', 'li r5,1', 'bl 0x82c30a10', 'blr'])
    s += fn('sub_82005000', ['lwz r10,52(r11)', 'bl 0x82001000', 'mtctr r10', 'bctrl ', 'blr'])                                                  # the planted miss
    s += fn('sub_82006000', ['bl 0x82009999', 'lwz r11,0(r3)', 'lwz r10,52(r11)', 'mtctr r10', 'bctrl ', 'blr'])                                  # class unknown
    s += fn('sub_82007000', ['mr r31,r3', 'addi r3,r31,8', 'mr r4,r5', 'li r5,1', 'bl 0x82c30a10', 'blr'])                                       # category in a register
    # QX4 review fixes: a save helper at entry must not kill r3; a join of two different values is unknown; a loop that rewrites the register; r3 != receiver
    s += fn('sub_82008000', ['mflr r12', 'bl 0x82c78df4', 'mr r31,r3', 'lwz r11,0(r31)', 'lwz r10,52(r11)', 'mtctr r10', 'bctrl ', 'blr'])
    s += raw('sub_82009000', ['mr r31,r3', 'cmpwi cr6,r4,0', 'beq cr6,0x82009010', 'mr r31,r5', 'loc_82009010:', 'lwz r11,0(r31)', 'lwz r10,52(r11)', 'mtctr r10', 'bctrl ', 'blr'])
    s += raw('sub_8200A000', ['mr r31,r3', 'loc_8200A004:', 'lwz r11,0(r31)', 'lwz r10,52(r11)', 'mtctr r10', 'bctrl ', 'lwz r31,4(r31)', 'cmpwi cr6,r31,0', 'bne cr6,0x8200A004', 'blr'])
    s += fn('sub_8200B000', ['lwz r11,0(r4)', 'lwz r10,52(r11)', 'mtctr r10', 'bctrl ', 'blr'])
    # QAF: the builder F7 (category = its argument r5) has three callers: two literals and a PLANTED non-literal (`mr r5,r4`); F13's only caller passes a register
    s += fn('sub_8200C000', ['li r5,22', 'bl 0x82007000', 'blr'])
    s += fn('sub_8200D000', ['li r5,30', 'bl 0x82007000', 'blr'])
    s += fn('sub_8200E000', ['mr r5,r4', 'bl 0x82007000', 'blr'])
    s += fn('sub_8200F000', ['mr r31,r3', 'addi r3,r31,8', 'mr r4,r6', 'li r5,1', 'bl 0x82c30a10', 'blr'])                                    # category = argument r6
    s += fn('sub_82010000', ['mr r6,r4', 'bl 0x8200f000', 'blr'])                                                                              # its only caller: not a literal
    return s


def build_fixture(tmp):
    gen = os.path.join(tmp, 'gen')
    os.makedirs(gen)
    with open(os.path.join(gen, 'wwe2k14_recomp.0.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(listing())
    img = bytearray(0x100100)
    struct.pack_into('>III', img, VT - 0x82000000, F1, F2, F8)       # slot 0 = F1, slot 1 = F2 (node's draw method), slot 2 = F8
    imgp = os.path.join(tmp, 'image.bin')
    with open(imgp, 'wb') as f:
        f.write(bytes(img))
    os.environ['WWE_Q_IMAGE'] = imgp
    os.environ['WWE_Q_GEN'] = gen        # QAF: caller_values reads the callers' listings from the fixture, not generated\
    con = sqlite3.connect(os.path.join(tmp, 'index.db'))
    con.row_factory = sqlite3.Row
    con.executescript('''CREATE TABLE funcs(addr INTEGER PRIMARY KEY, name TEXT, file TEXT, line INTEGER);
        CREATE TABLE edges(src INTEGER, dst INTEGER, target INTEGER, site INTEGER, kind TEXT);
        CREATE TABLE rtti_class(td INTEGER PRIMARY KEY, mangled TEXT, name TEXT);
        CREATE TABLE rtti_vtable(vt INTEGER PRIMARY KEY, col INTEGER, td INTEGER, nslots INTEGER);
        CREATE TABLE rtti_slot(vt INTEGER, idx INTEGER, func INTEGER);''')
    lines = listing().split('\n')
    for a in (F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15, F16):
        ln = next((i + 1 for i, l in enumerate(lines) if l.startswith(f'DEFINE_REX_FUNC(sub_{a:08X})')), 0)
        con.execute('INSERT INTO funcs VALUES (?,?,?,?)', (a, f'sub_{a:08X}', 'wwe2k14_recomp.0.cpp', ln))
    con.execute("INSERT INTO funcs VALUES (?,?,?,?)", (HELPER, '__savegprlr_29', None, None))
    for src, dst in ((F12, F7), (F13, F7), (F14, F7), (F16, F15)):      # each caller's `bl` is its second instruction
        con.execute('INSERT INTO edges VALUES (?,?,?,?,?)', (src, dst, dst, src + 4, 'call'))
    con.execute("INSERT INTO rtti_class VALUES (1, '.?AVModel@Fixture@@', 'Fixture::Model')")
    con.execute('INSERT INTO rtti_vtable VALUES (?,?,?,?)', (VT, 0, 1, 3))
    con.executemany('INSERT INTO rtti_slot VALUES (?,?,?)', [(VT, 0, F1), (VT, 1, F2), (VT, 2, F8)])
    con.commit()
    return gen, con


def jrun(fnc, con, *args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        fnc(list(args), con)
    return json.loads(buf.getvalue())


def test_vcall(tmp):
    gen, con = build_fixture(tmp)
    dbp = os.path.join(tmp, 'vcall.db')
    qx7.build(dbp, gen, jobs=2, quiet=True, index_con=con)
    r = jrun(qx7.vcall_main, con, '0x34', '--db', dbp, '--json')
    sites = {s['site']: s for s in r['sites']}
    check(f'0x{F1 + 16:08X}' in sites and sites[f'0x{F1 + 16:08X}']['receiver'] == 'this', 'vcall 0x34: the `this` virtual call of F1 is found (bctrl = F1+16)', r)
    check(f'0x{F1 + 16:08X}' in sites and sites[f'0x{F1 + 16:08X}']['classes'][0]['name'] == 'Fixture::Model', '  its class comes from the vtable slot table (F1 is slot 0)', r)
    check(f'0x{F2 + 8:08X}' not in sites and r['non_vtable_hidden'] == 1, '  the callback-field call (not a vtable load) is hidden and counted', r)
    check(f'0x{F5 + 12:08X}' not in sites and not any(s['func'] == 'sub_82005000' for s in r['sites']), '  PLANTED MISS: a `mtctr` after a call (the register died) is NOT a site', r)
    check(f'0x{F6 + 16:08X}' in sites and sites[f'0x{F6 + 16:08X}']['receiver'].startswith('field') is False and 'sub_82009999' in sites[f'0x{F6 + 16:08X}']['receiver'], '  a call on a call result names the callee (sub_82009999)', r)
    h = sites.get(f'0x{F8 + 24:08X}')
    check(h is not None and h['receiver'] == 'this' and h['classes'] and h['classes'][0]['name'] == 'Fixture::Model' and h['r3_is_receiver'],
          '  REVIEW 1: `mflr r12; bl __savegprlr_29; mr r31,r3` keeps r3 = this (the helper call kills only r0 / r11 / r12): receiver `this` + class', r)
    j = sites.get(f'0x{F9 + 28:08X}')
    check(j is not None and j['receiver'] != 'this' and not j['classes'], '  REVIEW 2: a join of r31 = this (one path) and r5 (the other) is NOT `this`, no class', r)
    j = sites.get(f'0x{F10 + 16:08X}')
    check(j is not None and j['receiver'] != 'this' and not j['classes'], '  REVIEW 2: a loop that rewrites the register (backward branch) kills `this`', r)
    j = sites.get(f'0x{F11 + 12:08X}')
    check(j is not None and j['r3_is_receiver'] is False and 'arg r4' in j['receiver'], '  REVIEW 2: r3 != the object the vtable was read from: flagged (r3_is_receiver false)', r)
    ra = jrun(qx7.vcall_main, con, '0x34', '--db', dbp, '--json', '--all')
    check(any(s['func'] == 'sub_82002000' for s in ra['sites']), '  --all lists the callback-field call', ra)
    rc = jrun(qx7.vcall_main, con, '0x34', '--db', dbp, '--json', '--class', 'Fixture::Model')
    check([s['func'] for s in rc['sites']] == ['sub_82001000', 'sub_82008000'] and rc['unresolved_omitted'] >= 1, '  --class lists only the known class and COUNTS the unresolved site (F6), never lists it', rc)
    rv = jrun(qx7.vcall_main, con, '0x4C', '--db', dbp, '--json', '--class', f'0x{VT + 4:08X}')
    check([s['func'] for s in rv['sites']] == ['sub_82003000'], 'vcall 0x4C --class <a slot address of the vtable>: the class stored in the same function (F3)', rv)
    check(qx7.parse_num('036') == 36 and qx7.parse_num('0x36') == 54, '  parse_num: 036 is decimal 36, 0x36 is 54')
    try:
        jrun(qx7.vcall_main, con, '0x35', '--db', dbp, '--json')
        check(False, '  an offset that is not a multiple of 4 is refused')
    except qx7.XError:
        check(True, '  an offset that is not a multiple of 4 is refused')
    rn = jrun(qx7.nodes_main, con, '36', '--db', dbp, '--json')
    n = rn['nodes']
    check(len(n) == 1 and n[0]['func'] == 'sub_82004000' and n[0]['vtable'] == f'0x{VT:08X}' and n[0]['class'] == 'Fixture::Model' and n[0]['slot1'] == 'sub_82002000' and n[0]['node'].endswith('+16'),
          'nodes 36: category, the vtable constant stored at +16 of the object, class and slot 1 (the node is r31+16)', rn)
    ra = jrun(qx7.nodes_main, con, '--db', dbp, '--json')
    un = [x for x in ra['nodes'] if x['category'] is None]
    check(len(ra['nodes']) == 3 and len(un) == 2 and all(x['category_source'] for x in un), 'nodes: a category in a register is listed as unresolved (source named), nothing guessed', ra)
    # QAF: one level of argument passing: the builder's category is the literals its callers pass
    b = {x['func']: x for x in ra['nodes'] if x['category'] is None}
    mixed = b['sub_82007000']
    check(mixed['category_from_callers'] == [22, 30] and mixed['category_callers_nonliteral'] == ['sub_8200E000']
          and mixed['category_text'] == '22|30|? (from callers; non-literal from: sub_8200E000)',
          'nodes QAF: a category that is an ARGUMENT shows the literals its callers pass (22, 30) and names the planted non-literal caller; it is never a guessed single value', mixed)
    only = b['sub_8200F000']
    check(only['category_from_callers'] is None and only['category_text'] == '? (argument r6; callers: sub_82010000)',
          'nodes QAF: callers that pass no literal give `? (argument rN; callers: ...)`, not a number', only)
    check([x['func'] for x in jrun(qx7.nodes_main, con, '22', '--db', dbp, '--json')['nodes']] == ['sub_82007000'], 'nodes QAF: `nodes 22` includes the builder whose callers pass 22', None)
    check(not any(x['func'] in ('sub_82007000', 'sub_8200F000') for x in jrun(qx7.nodes_main, con, '36', '--db', dbp, '--json')['nodes']), 'nodes QAF: `nodes 36` does not include a builder whose callers never pass 36', None)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        qx7.nodes_main(['--db', dbp], con)
    check('22|30|? (from callers; non-literal from: sub_8200E000)' in buf.getvalue(), 'nodes QAF: the printed line carries it', buf.getvalue())
    # warm open does not rebuild (same file untouched)
    m1 = os.path.getmtime(dbp)
    qx7.open_vdb(dbp)
    check(os.path.getmtime(dbp) == m1, 'a warm open of vcall.db does not rebuild it')


LOG = '''[2026-11-18 10:00:00.100] [info] [core] [t1] [wwe2k14] run log number: 5 (pid 99)
[2026-11-18 10:00:00.100] [info] [core] [t1] [wwe2k14] version v9.9.9+dev
[2026-11-18 10:00:00.200] [info] [core] [t1] [wwe2k14] modified: wwe2k14_input_script = 'C:\\x\\tools\\input_scripts\\scenes\\route_{r}.txt' (source: command line)
[2026-11-18 10:00:00.200] [info] [core] [t1] [wwe2k14] modified: wwe2k14_rdr_owner = 'true' (source: command line)
[2026-11-18 10:00:00.210] [info] [core] [t1] [wwe2k14] modified: wwe2k14_foo_switch = '7' (source: runtime/code)
[2026-11-18 10:00:00.220] [info] [core] [t1] [wwe2k14] modified: wwe2k14_bar_switch = '1' (source: default)
[2026-11-18 10:00:01.000] [info] [core] [t2] [wwe2k14][file] NtCreateFile 'D:\\PAC\\BG\\{pac}.PAC' open access=0x1 -> 0x0 handle=0x1 frame=300
[2026-11-18 10:00:02.000] [info] [core] [t2] [scene][ready] calls 600 (warm 1, live 599), ready 1; holes 12 skipped [arena 3 (first: the follow spot wants the entrant's root)]; draw error 0
[2026-11-18 10:00:03.000] [info] [core] [t2] [scene][ready] calls 601 (warm 2, live 598), ready 1; holes 12 skipped [arena 3 (first: the follow spot wants the entrant's root)]; draw error 0
[2026-11-18 10:00:04.000] [info] [core] [t2] [rdr][weapons] drawn 2 (ids 4 5), unknown id 0
[2026-11-18 10:00:05.000] [info] [core] [t2] [wwe2k14][census] sub_82ABCDEF call 1 NEW KEY
[2026-11-18 10:00:06.000] [info] [core] [t2] [wwe2k14][census] sub_82ABCD00 call 1 NEW KEY
'''


def test_runlog(tmp):
    logs = os.path.join(tmp, 'out', 'build', 'bldA', 'logs')
    logs2 = os.path.join(tmp, 'out', 'build', 'bldB', 'logs')
    os.makedirs(logs)
    os.makedirs(logs2)
    p1 = os.path.join(logs, 'wwe2k14_005.log')
    p2 = os.path.join(logs2, 'wwe2k14_005.log')
    with open(p1, 'w', encoding='utf-8', newline='\n') as f:
        f.write(LOG.format(r='bryan', pac='BG04'))
    with open(p2, 'w', encoding='utf-8', newline='\n') as f:
        f.write(LOG.format(r='cage', pac='BG07'))
    exe2 = os.path.join(tmp, 'out', 'build', 'bldB', 'wwe2k14.exe')       # an exe whose file time is AFTER the run (2030)
    with open(exe2, 'wb') as f:
        f.write(b'MZ')
    os.utime(exe2, (1893456000, 1893456000))
    dbp = os.path.join(tmp, 'runlog.db')
    qrunlog.update(dbp, logs=[p1, p2], quiet=True)

    def go(*args):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            qrunlog.main(list(args) + ['--db', dbp])
        return buf.getvalue()
    qrunlog.find_logs = lambda: [p for p in (p1, p2) if os.path.exists(p)]     # main() calls update() on the REAL log folders: point it at the fixture
    out = go('scene', 'ready', 'holes', '--json')
    j = json.loads(out)
    check(j['n'] == 4 and {x['which'] for x in j['results']} == {'first', 'last'} and all(x['times'] == 2 for x in j['results']),
          'runlog words: the census line of both runs (digits differ: one shape) is stored as its FIRST and LAST text, each says x2', out)
    out = go('weapons', '--route', 'bryan', '--json')
    j = json.loads(out)
    check(j['n'] == 1 and 'route_bryan' in j['results'][0]['route'], '  --route keeps only the matching run', out)
    out = go('which', '--mode', 'BG04', '--json')
    j = json.loads(out)
    check(j['runs'] == 1 and list(j['routes'])[0].endswith('route_bryan.txt'), 'runlog which --mode BG04: only the route that opened that pac', out)
    check(json.loads(go('which', '--mode', 'BG99', '--json'))['runs'] == 0, '  PLANTED name no run opened (BG99): none', out)
    for words in (['wwe2k14_rdr_owner'], ['rdr_owner'], ['rdr', 'owner'], ['rdr_own']):
        out = go(*words, '--json')
        check(json.loads(out)['n'] == 2, f'  REVIEW 3: flag searchable as {" ".join(words)} (a_b is the phrase "a b", prefix on the last part)', out)
    j = json.loads(go('wwe2k14_foo_switch', '--json'))
    check(j['n'] == 2 and '=7@runtime/code' in j['results'][0]['text'], '  REVIEW 4: a runtime / code `modified:` line is indexed with its source (k=v@runtime/code)', j)
    j = json.loads(go('wwe2k14_bar_switch', '--json'))
    check(j['n'] == 2 and '=1@default' in j['results'][0]['text'], '  REVIEW 4: a default-sourced line too', j)
    j = json.loads(go('--exe', 'bldB', '--json'))
    check(j['results'][0]['exe_note'].find('REBUILT AFTER') >= 0, '  REVIEW 5: an exe file newer than the run says it was rebuilt after the run', j)
    j = json.loads(go('--exe', 'bldA', '--json'))
    check('REBUILT' not in j['results'][0]['exe_note'] or j['results'][0]['exe_note'] == 'exe gone', '  (no exe file: `exe gone`, never a made-up build time)', j)
    j = json.loads(go('sub_82ABCDEF', '--json'))
    check(j['n'] == 2 and all(x['times'] == 1 for x in j['results']), '  REVIEW 7: sub_82ABCDEF and sub_82ABCD00 differ in hex: two shapes (only STANDALONE decimals are normalised)', j)
    # capped: parse in-process with a cap of 1 shape per kind
    old_cap = qrunlog.MAX_PER_TAG
    qrunlog.MAX_PER_TAG = 1
    try:
        r = qrunlog.parse_log(p2)
    finally:
        qrunlog.MAX_PER_TAG = old_cap
    check(r['capped'].get('census') == 1, '  REVIEW 7: a shape past the per-kind cap is counted in `capped` (and the output prints a capped note)', r['capped'])
    # incremental: a changed log is re-read; a deleted log keeps its rows
    with open(p1, 'a', encoding='utf-8', newline='\n') as f:
        f.write('[2026-11-18 10:00:09.000] [info] [core] [t2] [wwe2k14][census] sub_82FFFFFF call 1 NEW KEY\n')
    out = go('sub_82FFFFFF', '--json')
    check(json.loads(out)['n'] == 1, '  a changed log is re-read (the appended line is found)', out)
    os.remove(p1)
    out = go('which', '--mode', 'BG04', '--json')
    j = json.loads(out)
    check(j['runs'] == 1 and list(j['routes'].values())[0][0]['log_gone'] is True, '  a deleted log keeps its rows, marked log_gone', out)
    # REVIEW 8: a VERSION bump MIGRATES: the rows of a gone log survive, a log still on disk is re-parsed
    con = sqlite3.connect(dbp, isolation_level=None)
    con.execute("UPDATE meta SET value = '1' WHERE key = 'version'")
    con.close()
    out = go('which', '--mode', 'BG04', '--json')
    j = json.loads(out)
    check(j['runs'] == 1 and list(j['routes'].values())[0][0]['log_gone'] is True, '  REVIEW 8: after a version bump the gone log\'s rows are still there (migrated, not dropped)', out)
    check(json.loads(go('wwe2k14_rdr_owner', '--json'))['n'] == 2, '  and the log still on disk was re-parsed into the new tables', out)


def test_refs(tmp):
    root = os.path.join(tmp, 'rt')
    os.makedirs(os.path.join(root, 'src'))
    body = '\n'.join([
        'constexpr uint32_t kA = 1, kCharSlotTable = 0x836F38D0, kB = 2;',
        '#define SLOT_TABLE 0x836f38d0',
        'const char* s = "c0=0x836F38D0>0:3840";',
        'constexpr uint32_t kOther = 0x836F38D4;',
        'int x = Read(0x836F38D0);   // a literal use, not a holder',
        'inline constexpr uint32_t kSuffix = 0x836F38D0u;',
        'static const uint32_t kTab[] = {',
        '  1,',
        '  0x836F38D0,   // an array initialiser',
        '  3 };',
        'enum Slots : uint32_t { kEnumA = 0, kEnumTable = 0x836F38D0 };',
        'constexpr uint32_t kPlus = 0x836F38D0 + 4;',
        'constexpr uint32_t kCall = Foo(0x836F38D0);',
        'constexpr uint32_t kBase = kBaseAddr + 0x836F38D0;',
        'constexpr uint32_t kCast = (uint32_t)0x836F38D0;',
        'uint32_t Fn() { const uint32_t kLocalCall = Bar(0x836F38D0); return 0; }',
        '// constexpr uint32_t kCommented = 0x836F38D0;',
        ''])
    with open(os.path.join(root, 'src', 'a.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(body)
    h = qx6.addr_holders(root, '0x836F38D0')
    names = sorted(x[0] for x in h)
    check(names == ['SLOT_TABLE', 'kCast', 'kCharSlotTable', 'kEnumTable', 'kSuffix', 'kTab'],
          'refs ADDRESS: holders = multi-declarator, #define, u suffix, array element, enumerator, cast; NOT a string, a comment, kOther (another address), a bare call, `+ 4`, `Foo(addr)`, `base + addr`', names)
    lines = dict((x[0], x[2]) for x in h)
    check(lines['kTab'] == 7 and lines['kCharSlotTable'] == 1, '  the line of the NAME is reported (kTab declared on line 7)', h)
    check(qx6.is_addr_arg('0x836F38D0') and not qx6.is_addr_arg('kFoo') and not qx6.is_addr_arg('0x12'), '  an address argument is told from a name', '')


def test_map_entry():
    """QAF item 3: the hand entry for 'which shader files draw the arena screens (keys 800-809)': found FIRST by both questions an agent asked, and none of its anchors stale"""
    import subprocess
    q = os.path.join(HERE, 'q.py')
    env = dict(os.environ)
    env.pop('WWE_Q_GEN', None)

    def qq(*a):
        return subprocess.run([sys.executable, '-I', q] + list(a), capture_output=True, text=True, timeout=120, env=env).stdout
    for words in (('screen', 'shader'), ('screens', '800', 'shader')):
        o = qq('map', *words)
        first = next((l for l in o.splitlines() if l.startswith('[')), '')
        check('which shader files draw the arena screens' in first, f'map {" ".join(words)}: the screens / shader entry is the FIRST answer', o[:600])
    o = qq('map', 'screens', '800', 'shader')
    check('yBG_Default.frag' in o and 'yMovieDefault.frag' in o and 'rf_c_core.cpp' in o, 'map screens 800 shader: names the scene shader, the game-draw shader and the loop', o[:900])
    c = qq('map', '--check')
    check('which shader files draw the arena screens' not in c, 'map --check: no stale anchor on the screens entry', c)


def real_tree():
    if not (os.path.isdir(qx7.GEN) and os.path.exists(os.path.join(qx7.IDX, 'index.db')) and os.path.exists(qx7.IMAGE)):
        print('SKIP  real tree (generated\\ or the index is absent)')
        return
    os.environ.pop('WWE_Q_IMAGE', None)
    os.environ.pop('WWE_Q_GEN', None)
    import subprocess
    q = os.path.join(HERE, 'q.py')

    def qq(*a):
        return subprocess.run([sys.executable, '-I', q] + list(a), capture_output=True, text=True, timeout=300).stdout

    o = qq('vcall', '0x34', '--limit', '1000')
    check('0x82697C0C' in o and 'sub_826979F8' in o, 'REAL vcall 0x34 (slot 13): the COV1 miss 0x82697C0C in sub_826979F8', o[:400])
    o = qq('vcall', '0x4C', '--limit', '1000')
    check('sub_827C9BC8' in o, 'REAL vcall 0x4C (slot 19): sub_827C9BC8\'s site', o[:400])
    o = qq('vcall', '0x80', '--limit', '3')
    check('slot 32' in o and 'site(s) listed' in o, 'REAL vcall 0x80 (slot 32) answers', o[:300])
    o = qq('nodes', '23')
    check('sub_827C7BD8' in o, 'REAL nodes 23: registered by sub_827C7BD8', o)
    o = qq('nodes', '26')
    check('sub_826FA0B8' in o, 'REAL nodes 26: the node whose slot 1 is sub_826FA0B8', o)
    o = qq('nodes', '36')
    check('sub_827C7BD8' in o and '0x82032E64' in o, 'REAL nodes 36: includes sub_827C7BD8 (vtable 0x82032E64)', o)
    o = qq('nodes')
    check(re.search(r'cat 22\|30\|35\|36 \(from callers\)\s+0x[0-9A-F]+\s+sub_825B96D8', o) is not None,
          'REAL nodes QAF: the stage builder sub_825B96D8 shows its five groups\' categories 22|30|35|36 (its callers pass them; it was `cat ?`)', o)
    check(re.search(r'cat\s+33\s+0x[0-9A-F]+\s+sub_825ABB98.*slot1 sub_825ABC20', o) is not None, 'REAL nodes QAF: the wrestlers\' translucent node (slot 1 sub_825ABC20) is category 33', o)
    o = qq('ours', 'refs', '0x836F38D0')
    check('kCharSlotTable' in o and 'kSlotTable' in o, 'REAL q ours refs 0x836F38D0: kCharSlotTable / kSlotTable found', o[:500])


def main():
    import shutil
    tmp = tempfile.mkdtemp(prefix='qx7_selftest_')
    os.environ['WWE_QVCALL_DB'] = os.path.join(tmp, 'real_vcall.db')      # S105 D (F-3): the real-tree proofs build a TEMP table, never the shared index of any checkout
    try:
        test_vcall(tmp)
        test_runlog(tmp)
        test_refs(tmp)
        test_map_entry()
        real_tree()
    except Fail as e:
        print(f'FAIL  {e}')
        return 1
    finally:
        os.environ.pop('WWE_QVCALL_DB', None)
        os.environ.pop('WWE_Q_IMAGE', None)
        os.environ.pop('WWE_Q_GEN', None)
        shutil.rmtree(tmp, ignore_errors=True)
    print('ALL PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
