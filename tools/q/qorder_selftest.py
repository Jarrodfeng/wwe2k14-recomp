r"""qorder_selftest: proves `q order` (tools\q\qorder.py, S111 ORDER) on the real tree, with PLANTED wrong facts (no game, 0 runs).

The proof is the Session 110 miss (docs\research\s110\ENTRBLACK.md, ENTRWAIT.md, ENTRWAIT2.md: about ten probe rounds re-derived this order from listings). EXPECT is
typed HERE from those notes (never read from db\order): in one frame of the game
  1. sub_8273D0C0 (the match load; step 3 = the no-entrance start) is run by the in-game flow's task, update sub_826FCDF0, group 1; it waits on step 3 / the
     scene word 0x836F3940 (ENTRWAIT2 claims 11-12);
  2. sub_8232E728 (the entrance part's first update) is run by the part's task, update sub_8232E1E8, group 1, AFTER the flow in that group; it has no wait of its
     own and sets [part+124] = 1 (ENTRWAIT2 claims 2-3);
  3. sub_82331BC0 (the loading card's release) is run by the card item's task, update sub_8232F2B0, group 33: after both; it waits while [[card+84]+16] == 0 and
     drops the card with sub_827E62A8 (ENTRWAIT2 claims 6, 8).
The computed task table must also hold the registrations those rest on (sub_8232E460 group 1, sub_8232DE10 group 33, the scene manager sub_8231C688 group 0) and
`q.py order FUNC` must answer through q.py (the wiring).
PLANTS (each must FAIL the same check): swap = group 1's `within` order reversed (the part before the flow); nowait = the card release's wait removed;
group = the card item's task computed in group 0 (before group 1); word = a data-word site (sub_82254B00) given a literal group; range = a computed group 69
(past the 35). The review's checks on the real tree: sub_823BC218's groups are the r5 its callers pass (the walk starts from the register r4 copies), a partly
open registration answers [U], and `q order sub_8232E728` says the part reads no card word and that a wait holds only its own task.
Run:  py -I tools\q\qorder_selftest.py      exit 0 = PASS, 1 = a check failed, 2 = BLOCKED (no index / no generated listing on this PC)
"""
import copy
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qorder  # noqa: E402
import qx7  # noqa: E402

EXPECT = [
    {'func': 'sub_8273D0C0', 'update': 'sub_826FCDF0', 'group': 1, 'wait_words': ['step 3', '0x836F3940']},
    {'func': 'sub_8232E728', 'update': 'sub_8232E1E8', 'group': 1, 'wait_words': ['no wait', '[part+124] = 1']},
    {'func': 'sub_82331BC0', 'update': 'sub_8232F2B0', 'group': 33, 'wait_words': ['[[card+84]+16] == 0', 'sub_827E62A8']},
]
REGS = [(0x8232E460, 1, 0x8200A4BC, 0x8232E1E8), (0x8232E460, 1, 0x8200A410, 0x8232E1E8), (0x8232DE10, 33, 0x8200A550, 0x8232F2B0),
        (0x8231C688, 0, 0x82009D18, 0x8231C868)]
FAILS = []


def ok(cond, what):
    print(('  ok    ' if cond else '  FAIL  ') + what)
    if not cond:
        FAILS.append(what)


def q_run(*args):
    return subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py')] + list(args), capture_output=True, text=True, timeout=300, env=dict(os.environ))


def main():
    if not os.path.isdir(qx7.GEN) or not os.path.exists(os.path.join(qx7.IDX, 'index.db')):
        print(f'BLOCKED: no generated listing ({qx7.GEN}) or no code index ({qx7.IDX}\\index.db) on this PC')
        return 2
    tmp = tempfile.mkdtemp(prefix='qorder_')
    os.environ['WWE_QORDER_CACHE'] = os.path.join(tmp, 'order.json')      # a table of its own: the shared out\harness\index copy is left alone
    icon = qx7._index_con()
    t = qorder.table(True, icon)
    facts = qorder.frame_facts()
    scon = qorder._seen_con()
    ok(scon is not None, 'the observed-pair table opens (tools/q/qseen.py)')
    print('the computed task table:')
    for f, g, vt, up in REGS:
        rs = [r for r in t['sites'] if r['func'] == f]
        hit = [r for r in rs if r['groups'] == [g] and any(c['vt'] == vt and c['update'] == up for c in r['classes'])]
        ok(bool(hit), f'{qorder.fn(f)} registers group {g}, class {qorder.hx(vt)} with update {qorder.fn(up)} (its sites: '
           + '; '.join(f"+{r['site'] - f:X} groups {r['groups']}" for r in rs) + ')')
    ok(len(t['sites']) >= 300, f"{len(t['sites'])} registration sites of sub_82C307B0 read (the index: 350)")
    print('the group walk (S111 ORDER review):')
    rs = [r for r in t['sites'] if r['func'] == 0x823BC218]
    gs = rs[0]['groups'] if rs else []
    ok(bool(rs) and 16 in gs and 21 in gs and 30 not in gs and 69 not in gs,
       f'sub_823BC218 (r4 at its bl = its own entry r5): the groups are the r5 literals its callers pass (16, 21 ...), never their r4 (30, 69); got {gs}')
    ok(not qorder.range_bad(t), 'every computed group is one of the 35 groups (' + '; '.join(qorder.range_bad(t))[:160] + ')')
    ok(not qorder.word_bad(t), 'a site whose r4 is a data word has no computed group (' + '; '.join(qorder.word_bad(t))[:160] + ')')
    rw = [r for r in t['sites'] if r['func'] == 0x82254B00]
    ok(bool(rw) and rw[0]['groups'] == [] and any('field+12' in o for o in rw[0]['open']),
       f'sub_82254B00 (r4 = field+12 of its arg r4): open, no group ({rw[0] if rw else "no row"})')
    rp = [r for r in t['sites'] if r['func'] == 0x823E6D18]
    u = rp[0]['classes'][0]['update'] if rp and rp[0]['classes'] else None
    txt = qorder.group_text(u, t, facts) if u else ''
    ok(bool(rp) and bool(rp[0]['groups']) and rp[0]['n_open'] > 0 and '[U' in txt,
       f'sub_823E6D18 (13 from some callers, a data word from sub_83422628): its task answer says [U]: {txt[:140]}')
    rt = [r for r in t['sites'] if r['func'] == 0x82656580]
    ok(bool(rt) and any('tail branch' in o for o in rt[0]['open']),
       f"sub_82656580 (its walk reaches sub_82657758, also entered by tail branches): the tail callers are reported open ({rt[0]['open'] if rt else 'no row'})")
    print('the Session 110 order, against the real tree:')
    bad = qorder.check(t, facts, EXPECT, icon, scon)
    for b in bad:
        print('        ' + b)
    ok(not bad, 'flow (sub_8273D0C0) -> part (sub_8232E728) -> card release (sub_82331BC0): tasks, groups, run order and waits as ENTRWAIT2 settled')
    print('planted wrong facts (each must fail):')
    f2 = copy.deepcopy(facts)
    for w in f2['within']:
        if w['group'] == 1:
            w['order'] = list(reversed(w['order']))
    b = qorder.check(t, f2, EXPECT, icon, scon)
    ok(any(x.startswith('order:') for x in b), 'swap: the part ranked before the flow in group 1 fails on the order (' + '; '.join(b)[:160] + ')')
    f3 = copy.deepcopy(facts)
    f3['waits'] = [w for w in f3['waits'] if w['func'] != 'sub_82331BC0']
    b = qorder.check(t, f3, EXPECT, icon, scon)
    ok(any('sub_82331BC0: its wait' in x for x in b), 'nowait: the card release without its wait fails (' + '; '.join(b)[:160] + ')')
    t4 = copy.deepcopy(t)
    for r in t4['sites']:
        if r['func'] == 0x8232DE10:
            r['groups'] = [0]
    b = qorder.check(t4, facts, EXPECT, icon, scon)
    ok(any('expected [33]' in x for x in b) and any(x.startswith('order:') for x in b),
       'group: the card item task in group 0 fails on its group and on the order (' + '; '.join(b)[:160] + ')')
    t5 = copy.deepcopy(t)
    for r in t5['sites']:
        if r['func'] == 0x82254B00:
            r['groups'] = [12]
    b = qorder.word_bad(t5)
    ok(bool(b), 'word: the data-word site sub_82254B00 given a literal group fails (' + '; '.join(b)[:160] + ')')
    t6 = copy.deepcopy(t)
    t6['sites'][0]['groups'] = t6['sites'][0]['groups'] + [69]
    b = qorder.range_bad(t6)
    ok(bool(b), 'range: a computed group 69 (past the 35 groups) fails (' + '; '.join(b)[:160] + ')')
    print('the wiring (q.py order):')
    r = q_run('order', 'sub_8232E728')
    ok(r.returncode == 0 and 'task: update sub_8232E1E8' in r.stdout and 'group [1]' in r.stdout and '<== this' in r.stdout
       and 'reads no card word' in r.stdout and 'holds only its own task' in r.stdout,
       f'q.py order sub_8232E728 answers (exit {r.returncode}): ' + (r.stdout.strip().splitlines()[1:2] or [r.stderr.strip()[:160]])[0][:150])
    r = q_run('order', 'sub_82C3B450')
    ok(r.returncode == 0 and 'not a task' in r.stdout and '`events`' in r.stdout,
       f'q.py order sub_82C3B450 (the runner calls it itself, +1A4): a phase, not "no task found" (exit {r.returncode}): '
       + (r.stdout.strip().splitlines()[1:2] or [r.stderr.strip()[:160]])[0][:150])
    r = q_run('order', 'sub_8273D0C0', '--json')
    try:
        j = json.loads(r.stdout)
    except ValueError:
        j = {}
    ok(bool(j.get('tasks')) and j['tasks'][0]['update'] == 'sub_826FCDF0' and j['tasks'][0]['groups'] == [1] and j.get('where') == 'groups g1 r0',
       f"q.py order sub_8273D0C0 --json: the flow task, group 1, rank 0 (got {str(j.get('tasks', [{}])[:1])[:120]}, {j.get('where')})")
    r = q_run('order')
    ok(r.returncode == 0 and 'group 33:' in r.stdout and 'sub_8232F2B0' in r.stdout and '[C] merge' in r.stdout, 'q.py order (the frame) lists the phases and group 33 with sub_8232F2B0')
    print(('FAIL' if FAILS else 'PASS') + f' ({len(FAILS)} failed)')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
