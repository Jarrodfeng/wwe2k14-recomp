"""Self-test of qwhen (S111 Lane C WHEN): `py -I tools/q/qwhen_selftest.py`   (0 game runs; invented functions, then the real index when it is there)

A. parse_doc / parse_cats on a planted render_categories table: ranges, lists, `slot 1` vs builder mentions, the passes table, the doc mark.
B. The planted world answers: a helper of a task update (task, group, thread by db name), a draw node (categories, side walks, [L doc] mark, the walk's own frame
   phase, the thread through the runner), a helper of a node (the path), a builder named in a row (named, not drawn), a side walk, a function no root reaches
   (`no root found`), the card line.
C. Planted twins that must FAIL the same checks: a wrong walk (the doc row says another category), a missing thread (no db name above the runner), a wrong task
   group, a builder printed as a slot 1, a walk with no caller (no frame phase for the walk).
D. The real tree (skipped when the index is missing): `q when` re-finds of the plan: sub_825A3908 = categories 13..21 + pA pB rim redraw, in phase events;
   sub_8273D0C0 = group 1 (the load wait); sub_823C8548 = task group 5 [C], thread named.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qwhen as W  # noqa: E402

TASK, HELP, GAME, FRAME = 0x82100000, 0x82100100, 0x82200000, 0x82200100
NODE, NHELP, BUILD, SIDE = 0x82300000, 0x82300100, 0x82300010, 0x82700000
PHASE, UNK = 0x82C3B4B8, 0x82999999
# review (S111 Lane C): a second thread above the walk, a non-root second caller, a task whose group is open [U], a spawner named *_thread, an observed hop
PAR, OTHER, TASK2, HELP2, SPAWNED, SPAWNER, NHELP2 = 0x82400000, 0x82100300, 0x82100400, 0x82100500, 0x82100600, 0x82100700, 0x82300200

DOC = '''
| Cat | What registers there (builder / slot 1) | Reset | State | Passes | Re-keys | Ours | Marks |
|---|---|---|---|---|---|---|---|
| 13..21 | the node (`sub_82300010`, slot 1 `sub_82300000`) | R | x | C, pA, rim, redraw | - | - | [C] |
| 4, 5 | nothing | R | - | C | - | - | [C] |
## The passes
| Pass | Function | Categories it walks (in order) | State | Mark |
|---|---|---|---|---|
| map A | `sub_82700000` | 13..21, 42 | own | [C] |
'''


def world(doc=DOC, names=None, group='5', walk_callers=True, builder_slot1=False):
    if builder_slot1:
        doc = doc.replace('the node (`sub_82300010`, slot 1 `sub_82300000`)', 'the node (slot 1 `sub_82300010`)')
    nm = {TASK: 'fake_task_update', HELP: 'fake_helper', GAME: 'game_main_thread_loop', NODE: 'fake_node_draw', W.WALK: 'fake_walk',
          PAR: 'parallel_draw_thread_loop', SPAWNER: 'start_fake_manager_thread'}
    nm = names if names is not None else nm
    cal = {HELP: [(TASK, 1, 'call'), (OTHER, 7, 'call')], NHELP: [(NODE, 2, 'call')], W.RUNNER: [(FRAME, 4, 'call')], FRAME: [(GAME, 5, 'call')],
           HELP2: [(TASK2, 8, 'call')], SPAWNED: [(SPAWNER, 9, 'call')], SPAWNER: [(OTHER, 10, 'call')], NHELP2: [(TASK, 11, 'seen in 3 runs')]}
    if walk_callers:
        cal[W.WALK] = [(PHASE, 3, 'call'), (PAR, 6, 'call')]
    tasks = {TASK: {'group': group, 'where': f'groups g{group}', 'reg': 'sub_82100200 +20'},
             TASK2: {'group': '3, 7, 11, 16, 21, 34 [U: 28 open (a data word or a caller not read): sub_82100800: field+4 of this; sub_82100900: field+4 of this ...]',
                     'where': 'groups', 'reg': 'sub_82100810 +20'}}
    phases = {PHASE: {'id': 'events', 'what': 'the frame events', 'src': 'decomp sub_82C302A0', 'where': 'events', 'pos': '6 of 10'}}
    return W.World(nm, lambda a: cal.get(a, []), tasks, phases, {}, W.parse_doc(doc), {HELP: '[C] the helper waits'})


def checks(w):
    """-> [failure text]"""
    bad = []

    def need(ok, why):
        if not ok:
            bad.append(why)
    r = W.compute(w, HELP)
    a = r['anchors']
    need(len(a) == 1 and a[0]['kind'] == 'task' and a[0]['root'] == TASK and a[0]['mark'] == 'C', 'helper: a [C] task anchor on the task update')
    need(a and a[0]['info']['group'] == '5', 'helper: the task group is 5')
    need([t[1] for t in r['threads']] == ['game_main_thread_loop'], 'helper: the thread is the db-named game loop above the runner')
    need((W.card_text(w, r) or '').startswith('[C] task sub_82100000 group 5'), 'helper: the card line starts with the [C] task')
    need('every frame' not in (W.card_text(w, r) or ''), 'helper: the card does not give the task\'s period to a function below it')
    need(any('1 other direct caller' in x for _, x in W.lines(w, r)), 'helper: its second (non-root) caller is said, not dropped')
    need('every frame' in (W.card_text(w, W.compute(w, TASK)) or ''), 'task itself: the card says every frame')
    r = W.compute(w, HELP2)
    need(r['anchors'] and r['anchors'][0]['kind'] == 'task' and r['anchors'][0]['mark'] == 'L', 'helper of a task whose group is open [U]: the task anchor is [L]')
    need(len(W.card_text(w, r) or '') <= 160 - len('; [when] '), 'card: the text fits the card header with its `; [when] ` prefix')
    r = W.compute(w, SPAWNED)
    need([t[1] for t in r['threads']] == [], 'spawned: a spawner named start_*_thread is not a thread entry')
    r = W.compute(w, NHELP2)
    need(r['anchors'] and r['anchors'][0]['kind'] == 'task' and r['anchors'][0]['mark'] == 'L' and any('observed-indirect' in x for _, x in W.lines(w, r)),
         'a helper reached from the task by an observed hop: [L], the hop said')
    r = W.compute(w, NODE)
    n = [x for x in r['anchors'] if x['kind'] == 'node']
    need(len(n) == 1 and n[0]['cats'] == list(range(13, 22)), 'node: categories 13..21')
    need(n and n[0]['passes'] == ['pA', 'rim', 'redraw'], 'node: side walks pA rim redraw')
    need(n and n[0]['mark'] == 'L' and any(t == 'L doc' for t, _ in W.lines(w, r)), 'node: the doc-only answer prints [L doc], never [C]')
    wk = (n[0].get('walk') or {'anchors': []})['anchors'] if n else []
    need(any(x['kind'] == 'phase' and x['info']['id'] == 'events' for x in wk), 'node: the walk sits in the frame phase `events`')
    need([t[1] for t in r['threads']] == ['game_main_thread_loop', 'parallel_draw_thread_loop'], 'node: the thread through the walk and the runner first, then the second thread above the walk')
    need('+1' in (W.card_text(w, r) or ''), 'node: the card says a second thread reaches it')
    r = W.compute(w, NHELP)
    need(r['anchors'] and r['anchors'][0]['root'] == NODE and len(r['anchors'][0]['chain']) == 1, 'node helper: the path up to the node slot 1')
    r = W.compute(w, BUILD)
    need(not [x for x in r['anchors'] if x['kind'] == 'node'] and r['named'], 'builder: named in the doc row, not a drawn node')
    need(not any('not drawn there' in x for _, x in W.lines(w, r)), 'builder: a doc mention is not printed as "not drawn there" (slot-1 callees are mentioned too)')
    r = W.compute(w, SIDE)
    need(r['anchors'] and r['anchors'][0]['kind'] == 'side' and r['anchors'][0]['info']['tag'] == 'pA', 'side walk map A found')
    r = W.compute(w, UNK)
    need(not r['anchors'] and any('no root found' in x for _, x in W.lines(w, r)), 'unknown function: no root found, said so')
    return bad


SEQUP, SEQHELP, SEQVT = 0x82500000, 0x82500100, 0x82048D84


def slot_world(with_slots=True):
    """S111 QFIX4: a sequence's update is a vtable word only (no call edge reaches it); its helper is called from it"""
    w = world()
    w.callers = lambda a, base=w.callers: {SEQHELP: [(SEQUP, 3, 'call')]}.get(a) or base(a)
    if with_slots:
        w.slots = lambda a: [{'vt': SEQVT, 'slot': 3, 'class': 'COnlineMenuSequence', 'ctors': [0x829428D0], 'seq': [21]}] if a == SEQUP else []
    return w


def slot_checks(w):
    bad = []
    r = W.compute(w, SEQHELP)
    s = [x for x in r['anchors'] if x['kind'] == 'slot']
    if not (len(s) == 1 and s[0]['root'] == SEQUP and s[0]['slots'][0]['seq'] == [21] and s[0]['mark'] == 'C' and len(s[0]['chain']) == 1):
        bad.append(f"slot helper: want one [C] slot anchor on the update, one call up, sequence 21, got {[(x['kind'], x['mark']) for x in r['anchors']]}")
    txt = ' '.join(x for _, x in W.lines(w, r))
    if not ('slot 3 of vtable 0x82048D84' in txt and 'sequence id 21' in txt and 'no root found' not in txt):
        bad.append('slot helper: the line names the vtable, slot and sequence id')
    if 'seq 21' not in (W.card_text(w, r) or ''):
        bad.append('slot helper: the card names the sequence')
    s2 = [x for x in W.compute(w, SEQUP)['anchors'] if x['kind'] == 'slot']
    if not (s2 and s2[0]['chain'] == []):
        bad.append('slot update itself: a slot anchor with an empty chain')
    t = [x for x in W.compute(w, HELP)['anchors'] if x['kind'] == 'slot']
    if t:
        bad.append('a task helper must keep its task anchor, no slot anchor')
    return bad


def real_checks():
    """-> (failures, skipped text or None)"""
    try:
        w = W.world()
    except Exception as e:      # noqa: BLE001
        return [], f'{type(e).__name__}: {e}'
    bad = []
    r = W.compute(w, 0x825A3908)
    n = [x for x in r['anchors'] if x['kind'] == 'node']
    lp = getattr(W, 'layout_problems', None)
    if lp is None or lp(w.doc, full=True):
        bad.append(f'render_categories.md layout: {lp(w.doc, full=True) if lp else "no layout check"}')
    if not (n and n[0]['mark'] == 'L' and n[0]['src'] == 'doc'):
        bad.append('sub_825A3908: the node answer is doc-only, so [L doc]')
    if not any(t[1] == 'parallel_draw_thread_loop' for t in r['threads']):
        bad.append(f"sub_825A3908: the walk is also reached from parallel_draw_thread_loop (sub_82C3B640 +B8 -> sub_82C30518), got {[t[1] for t in r['threads']]}")
    if not (n and n[0]['cats'] == list(range(13, 22)) and n[0]['passes'] == ['pA', 'pB', 'rim', 'redraw']):
        bad.append(f"sub_825A3908: want node cats 13..21 + pA pB rim redraw, got {[(x['kind'], x.get('cats'), x.get('passes')) for x in r['anchors']]}")
    elif not any(x['kind'] == 'phase' for x in n[0]['walk']['anchors']):
        bad.append('sub_825A3908: the walk has no frame phase')
    r = W.compute(w, 0x8273D0C0)
    t = [x for x in r['anchors'] if x['kind'] == 'task']
    if not (t and t[0]['mark'] == 'L'):
        bad.append('sub_8273D0C0: its path has observed-indirect hops, so [L]')
    if not (t and t[0]['info']['group'] == '1' and r['wait']):
        bad.append(f"sub_8273D0C0: want task group 1 + the load wait, got {[(x['kind'], x.get('info')) for x in r['anchors']]} wait={bool(r['wait'])}")
    for fa, vt, seq in ((0x82942B38, 0x82048D84, [21]), (0x82A6BD80, 0x8205B3A0, [])):      # the M100_SM re-finds: a sequence update's callee, a class slot
        sl = [x for x in W.compute(w, fa)['anchors'] if x['kind'] == 'slot']
        if not (sl and sl[0]['slots'][0]['vt'] == vt and sl[0]['slots'][0]['seq'] == seq):
            bad.append(f"sub_{fa:08X}: want a vtable slot anchor on 0x{vt:08X} seq {seq}, got {[(x['kind'], x.get('slots')) for x in W.compute(w, fa)['anchors']]}")
    r = W.compute(w, 0x823C8548)
    t = [x for x in r['anchors'] if x['kind'] == 'task']
    if not (t and t[0]['info']['group'] == '5' and t[0]['mark'] == 'C' and r['threads']):
        bad.append(f"sub_823C8548: want [C] task group 5 and a thread, got {[(x['kind'], x['mark']) for x in r['anchors']]} threads={r['threads']}")
    if not any('7 other direct caller' in x for _, x in W.lines(w, r)):
        bad.append('sub_823C8548: 8 calling functions, the task update is one: the other 7 are said')
    return bad, None


def main():
    fails = []
    # A
    if W.parse_cats('13..21') != list(range(13, 22)) or W.parse_cats('4, 5') != [4, 5] or W.parse_cats('Cat') != []:
        fails.append('parse_cats')
    d = W.parse_doc(DOC)
    if NODE not in d['slot1'] or BUILD not in d['named'] or BUILD in d['slot1'] or d['slot1'][NODE][0]['mark'] != 'C' or SIDE not in d['side']:
        fails.append(f'parse_doc: slot1={list(d["slot1"])} named={list(d["named"])} side={list(d["side"])}')
    # B
    fails += [f'planted world: {x}' for x in checks(world())]
    fails += [f'planted slot world: {x}' for x in slot_checks(slot_world())]
    # C
    twins = {'wrong walk (doc row says category 22)': world(doc=DOC.replace('| 13..21 | the node', '| 22 | the node')),
             'missing thread (no db name above the runner)': world(names={TASK: 'fake_task_update', NODE: 'fake_node_draw'}),
             'wrong task group': world(group='9'),
             'builder printed as a slot 1': world(builder_slot1=True),
             'walk with no caller (no frame phase)': world(walk_callers=False)}
    missed = []
    for name, tw in twins.items():
        if not checks(tw):
            missed.append(name)
    if not slot_checks(slot_world(False)):
        missed.append('no vtable step (the old `no root found`)')
    # code twins (review): the same checks must fail when the code is wrong
    lp = getattr(W, 'layout_problems', None)
    moved = DOC.replace('| Cat | What registers there (builder / slot 1) | Reset | State |', '| Cat | What registers there (builder / slot 1) | Reset | Extra | State |')
    if lp is None or not lp(W.parse_doc(moved)):
        missed.append('a render_categories.md table with an inserted column is not reported')
    if lp is None or lp(W.parse_doc(DOC)):
        fails.append(f'layout check on the planted doc: {lp(W.parse_doc(DOC)) if lp else "missing"}')
    keep = W.chain_mark
    W.chain_mark = lambda chain: 'C'
    try:
        if not checks(world()):
            missed.append('chain_mark always [C] (an observed hop printed [C])')
    finally:
        W.chain_mark = keep
    # D
    rb, skipped = real_checks()
    fails += rb
    for f in fails:
        print('FAIL', f)
    for m in missed:
        print('NOT CAUGHT', m)
    n_tw = len(twins) + 3
    print(f"qwhen_selftest: planted world {'ok' if not [f for f in fails if f.startswith('planted')] else 'FAILED'}; {n_tw - len(missed)}/{n_tw} planted twins caught; "
          + (f'real tree skipped ({skipped})' if skipped else f"real tree {'ok' if not rb else 'FAILED'}"))
    return 1 if fails else (2 if missed else 0)


if __name__ == '__main__':
    sys.exit(main())
