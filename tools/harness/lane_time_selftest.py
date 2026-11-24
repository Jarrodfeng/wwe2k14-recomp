r"""Selftest of tools\harness\lane_time.py (S109 LT). Fixture transcripts are written to a temp folder (one reply with an Edit and a Bash
block; a background call and its notice; a wait_commit call; a queued image message; a probe-builder with a plan stop; a chain whose report
is the last Jarrod message before its first spawn) and each assertion drives the production path (agent_stats / main_events / load_session /
chain_rows ...), not a copy of it.

    py -I tools\harness\lane_time_selftest.py
"""
import json
import os
import sys
import tempfile
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lane_time as LT  # noqa: E402

T0 = datetime(2026, 11, 22, 1, 0, 0, tzinfo=timezone.utc)
FAILS = []
SEQ = [0]


def at(sec):
    return (T0 + timedelta(seconds=sec)).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'


def uid(p='toolu_'):
    SEQ[0] += 1
    return '%s%04d' % (p, SEQ[0])


def req(sec, rid):
    return {'type': 'api-request', 'id': rid, 'timestamp': at(sec)}


def asst(sec, rid, block, mid):
    return {'type': 'assistant', 'requestRef': rid, 'timestamp': at(sec), 'message': {'id': mid, 'role': 'assistant', 'model': 'm', 'content': [block]}}


def tool_use(name, inp, tid=None):
    return {'type': 'tool_use', 'id': tid or uid(), 'name': name, 'input': inp}


def result(sec, tid, text='ok', err=False):
    return {'type': 'user', 'timestamp': at(sec), 'message': {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': tid, 'content': text, 'is_error': err}]}}


def prompt(sec, text):
    return {'type': 'user', 'timestamp': at(sec), 'message': {'role': 'user', 'content': text}}


def write(path, recs):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        for r in recs:
            f.write(json.dumps(r) + '\n')


def check(name, cond, detail=''):
    print('  %-62s %s %s' % (name, 'ok' if cond else 'FAIL', '' if cond else detail))
    if not cond:
        FAILS.append(name)


def guard(name, fn):
    try:
        fn()
    except Exception as e:  # an assertion that cannot even run is a failure, shown as such
        check(name, False, '%s: %s' % (type(e).__name__, e))


def agent_file(root, aid, desc, atype, recs, folder='sess'):
    p = os.path.join(root, folder, 'subagents', 'agent-%s.jsonl' % aid)
    write(p, recs)
    with open(p[:-6] + '.meta.json', 'w', encoding='utf-8') as f:
        json.dump({'description': desc, 'agentType': atype}, f)
    return p


def t_reply_end(root):
    """one reply holding an Edit block (written at +3.0) and a Bash block (+5.0); the Edit runs after the reply ends (+5.0)"""
    e, b = uid(), uid()
    recs = [prompt(0, 'go'), req(0.5, 'r1'),
            asst(3.0, 'r1', tool_use('Edit', {'file_path': 'x.cpp'}, e), 'm1'),
            asst(5.0, 'r1', tool_use('Bash', {'command': 'git status'}, b), 'm1'),
            result(5.2, e), result(5.5, b)]
    g = LT.agent_stats(agent_file(root, 'a0000000000000001', 'SON lane Q x', 'lane-builder', recs))
    ed = next(c for c in g['calls'] if c['name'] == 'Edit')
    check('call time runs from the reply end (Edit 0.2 s, not 2.2 s)', abs(ed['dur'] - 0.2) < 0.05, 'dur=%s' % ed['dur'])
    check('the block-based time is kept beside it (2.2 s)', abs(ed.get('dur_block', -1) - 2.2) < 0.05, 'dur_block=%s' % ed.get('dur_block'))


def t_background(root):
    """a background call returns at once; its end is the task-notification naming its tool-use-id"""
    out = os.path.join(root, 'bg.output')
    with open(out, 'w', encoding='utf-8') as f:
        f.write('build done\nexit 0\n')
    b = uid()
    notice = '<task-notification>\n<task-id>bx</task-id>\n<tool-use-id>%s</tool-use-id>\n<status>completed</status>\n</task-notification>' % b
    recs = [prompt(0, 'go'), req(0.5, 'r1'),
            asst(2.0, 'r1', tool_use('Bash', {'command': 'py -I tools\\compile\\game_syntax.py', 'run_in_background': True}, b), 'm1'),
            result(2.1, b, 'Command running in background. Output is being written to: %s' % out),
            {'type': 'queue-operation', 'operation': 'enqueue', 'timestamp': at(122), 'content': notice}]
    g = LT.agent_stats(agent_file(root, 'a0000000000000002', 'SON lane Q y', 'lane-builder', recs))
    c = g['calls'][0]
    check('background call: end = its task-notification (120 s)', c.get('dur_bg') is not None and abs(c['dur_bg'] - 120.0) < 0.5, 'dur_bg=%s' % c.get('dur_bg'))
    check('background call: output file read', 'exit 0' in (c.get('bgout') or ''), 'bgout=%r' % c.get('bgout'))


def t_wait(root):
    """wait_commit / wait_any are waiting, not tool time; other scripts are named"""
    w, k = uid(), uid()
    recs = [prompt(0, 'go'), req(0.5, 'r1'),
            asst(2.0, 'r1', tool_use('Bash', {'command': 'py -I tools\\harness\\wait_commit.py --branch x'}, w), 'm1'),
            result(302.0, w, 'commit seen'),
            req(302.5, 'r2'),
            asst(304.0, 'r2', tool_use('Bash', {'command': 'py -I tools\\check\\inputs_check.py'}, k), 'm2'),
            result(310.0, k)]
    g = LT.agent_stats(agent_file(root, 'a0000000000000003', 'OPUS review lane Q z', 'review-fixbuilder', recs))
    cls = {c['id']: c['cls'] for c in g['calls']}
    check('wait_commit is class wait:commit', cls[w] == 'wait:commit', cls[w])
    check('another script is named (sh:py inputs_check.py)', cls[k] == 'sh:py inputs_check.py', cls[k])
    check('wait_commit time is counted as waiting, not tool (300 s)', abs(g.get('wcall', -1) - 300.0) < 1 and g['tool'] < 20, 'wcall=%s tool=%s' % (g.get('wcall'), g['tool']))


def t_messages(root):
    """a queued image message and a typed one are Jarrod's; agent messages and task notices are not; SendUserFile is a send"""
    img = {'type': 'user', 'timestamp': at(10), 'message': {'role': 'user', 'content': [
        {'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/png', 'data': 'AAAA'}}, {'type': 'text', 'text': 'still off in the route'}]}}
    queued = {'type': 'attachment', 'timestamp': at(20), 'attachment': {'type': 'queued_command', 'prompt': 'the second thing I typed'}}
    note = {'type': 'attachment', 'timestamp': at(30), 'attachment': {'type': 'queued_command', 'prompt': '<task-notification>\n<tool-use-id>t</tool-use-id></task-notification>'}}
    am = {'type': 'queue-operation', 'operation': 'enqueue', 'timestamp': at(40), 'content': '<agent-message from="a0000000000000001">done</agent-message>'}
    s = uid()
    recs = [prompt(0, 'start of the session'), img, queued, note, am, req(50, 'r1'),
            asst(52.0, 'r1', tool_use('SendUserFile', {'file_path': 'C:\\x\\out\\exe\\lane-q-abc\\captures\\001_sheet.jpg'}, s), 'm1'), result(53.0, s)]
    p = os.path.join(root, 'sess.jsonl')
    write(p, recs)
    ev = LT.main_events(p)
    kinds = [e[1] for e in ev]
    check('Jarrod messages: typed + queued image + queued text = 3', kinds.count('jarrod') == 3, str([(e[1], e[3]) for e in ev]))
    check('the image message is labelled [image]', any(e[1] == 'jarrod' and '[image]' in e[3] for e in ev))
    check('task notice and agent message are not Jarrod', not any(e[1] == 'jarrod' and ('task-notification' in e[3] or 'agent-message' in e[3]) for e in ev))
    check('SendUserFile is a send event', 'send' in kinds)


def t_chain(root):
    """a probe-builder that stopped at its plan (two hand-backs): probe up to the plan stop, builder after; the chain's report is the last
    Jarrod message before its first spawn; a chain with no message close before its spawn has its report named not derivable"""
    sess = os.path.join(root, 'cs')
    hb = lambda sec, rid, dur=1: [req(sec, rid), asst(sec + dur, rid, tool_use('SubagentHandback', {'message': 'report'}), 'm' + rid)]
    pb = [prompt(120, 'brief')] + hb(121, 'p1', 59) + [prompt(400, 'continue')] + hb(401, 'p2', 299)
    agent_file(root, 'a0000000000000010', 'OPUS lane ZZ1 thing', 'probe-builder', pb, 'cs')
    lone = [prompt(5000, 'brief')] + hb(5001, 'q1')
    agent_file(root, 'a0000000000000011', 'OPUS probe QQ2 other', 'lane-probe', lone, 'cs')
    main = [prompt(0, 'Jarrod: the first message'), prompt(100, 'Jarrod: ZZ1 is broken'),
            req(110, 'm0'), asst(111, 'm0', tool_use('Agent', {'description': 'OPUS lane ZZ1 thing'}, 'toolu_spawn1'), 'mm0'),
            result(112, 'toolu_spawn1', 'agentId: a0000000000000010'),
            req(4990, 'm1'), asst(4991, 'm1', tool_use('Agent', {'description': 'OPUS probe QQ2 other'}, 'toolu_spawn2'), 'mm1'),
            result(4992, 'toolu_spawn2', 'agentId: a0000000000000011')]
    write(sess + '.jsonl', main)
    agents, mainrec = LT.load_session(sess)
    g = next(a for a in agents if a['lane'] == 'ZZ1')
    ps = LT.plan_stop(g)
    check('probe-builder plan stop = its first hand-back', ps is not None and abs((ps - (T0 + timedelta(seconds=180))).total_seconds()) < 1, str(ps))
    rows, notes = LT.chain_table(agents, mainrec, [], [], [], {'tz': timezone.utc, 'nights': []})
    row = {r['chain']: r for r in rows}
    z = row.get('ZZ1')
    # no record ties a Jarrod message to a lane (S108: PRT was spawned in a turn an agent's hand-back opened, the message before it was MOB's;
    # EXEB's was BLD's verdict): the span starts at the chain's first spawn, never at a guessed message (+100 s here)
    check('chain ZZ (ZZ1) span starts at its first spawn (+120 s), not at the Jarrod message before it', z is not None and abs((z['t0'] - (T0 + timedelta(seconds=120))).total_seconds()) < 1, str(z and z['t0']))
    check('chain ZZ (ZZ1): probe 60 s (to the plan stop), builder 300 s after it', z is not None and 55 < z['split'].get('probe', 0) < 65 and 295 < z['split'].get('builder', 0) < 305, str(z and dict(z['split'])))
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        LT.print_chains(rows, notes, [])
    out = buf.getvalue()
    check('the table says the report time is not derivable, and guesses none (~)', 'report time: not derivable' in out and '~' not in out, out[-400:])
    check('a lane key is read from the description (BLD7 / SHOT-A / EXEB-B1 / AL1)', [LT.lane_key(d) for d in ('SON build lane BLD7 x', 'SON build lane SHOT-A y', 'OPUS review lane EXEB-B1 from plan', 'OPUS lane AL1 shortfall')] == ['BLD7', 'SHOT-A', 'EXEB-B1', 'AL1'])
    allk = ['BLD', 'BLD7', 'BLD2', 'ALM', 'AL1', 'ALM2', 'SHOT', 'SHOT-B', 'EXEB-A', 'EXEB-B1', 'CAP', 'CAP3', 'P4', 'P5', 'A', 'B', 'GAV', 'GAVF']
    cm = LT.chain_map(allk)
    want = {'BLD7': 'BLD', 'BLD2': 'BLD', 'AL1': 'ALM', 'ALM2': 'ALM', 'SHOT-B': 'SHOT', 'EXEB-B1': 'EXEB', 'CAP3': 'CAP', 'P4': 'P4', 'P5': 'P5', 'A': 'A', 'B': 'B', 'GAVF': 'GAV'}
    check('chains group by stem (BLD7 -> BLD, AL1 -> ALM, SHOT-B -> SHOT, GAVF -> GAV; P4 / A / B stay apart)', all(cm[k] == v for k, v in want.items()), str({k: cm[k] for k in want if cm[k] != want[k]}))


def t_run_lane():
    """a run's lane: its route's own token naming a lane first, else its exe folder's lane column, else not derivable"""
    keys = {'FAN', 'PRT', 'BAN', 'BLD4', 'THR', 'CAP2'}
    check('route token names the lane (s108_ban_arena53 on a prt exe -> BAN)', LT.run_lane('s108_ban_arena53.txt', 's108prt', keys)[0] == 'BAN')
    check('exe folder names the lane (fandango route on a fan exe -> FAN)', LT.run_lane('s108_fandango_entrance.txt', 's108fan', keys)[0] == 'FAN')
    r = LT.run_lane('s108_fandango_entrance.txt', 's108base', keys)
    check('neither names a lane -> not derivable, with what is missing', r[0] is None and 'not derivable' in r[1], str(r))


def t_run_launch():
    """a run's lane read from the call that started it (S108): the route lane agent's own runs are its lane's (MENU's states route is named
    for THB); one main-session call that ran the BAN3 tip exe on the banner route and the created-arena route is BAN's check, not CAA's;
    a call whose exe names neither route's lane (the PRT exe on the BAN and BLD routes) and a single-route call keep the route's lane"""
    t = lambda s: T0 + timedelta(seconds=s)
    sc = lambda *rs: '@(' + ','.join('"tools\\input_scripts\\scenes\\%s"' % x for x in rs) + ')'
    call = lambda cid, s, e, cmd, bg: dict(id=cid, name='PowerShell', inp={'command': cmd, 'run_in_background': bg}, start=t(s), res=t(s + 1) if bg else t(e),
                                           done=t(e), bg=bg)
    main = dict(calls=[
        call('c1', 100, 400, '& tools\\bench\\run_list.ps1 -Scripts %s -Exe "out\\exe\\lane-s108tip-cb873c14b\\wwe2k14.exe"' % sc('s108_ban_arena53.txt', 's108_caa_entrance.txt'), True),
        call('c3', 2000, 2300, '& tools\\bench\\run_list.ps1 -Scripts %s -Exe "out\\exe\\lane-s108prt-28d28bf4e\\wwe2k14.exe"' % sc('s108_ban_arena53.txt', 's108_bld_replay.txt'), True),
        call('c4', 3000, 3300, '& tools\\bench\\run_list.ps1 -Scripts %s -Exe "out\\exe\\lane-s108tip-cb873c14b\\wwe2k14.exe"' % sc('s108_bld5_late.txt'), True)])
    menu = dict(aid='a0000000000000020', lane='MENU', desc='SON route lane MENU cap route states', role='route', links=[], calls=[
        call('c2', 1000, 1100, '& tools\\bench\\run_list.ps1 -Scripts %s -Exe "out\\exe\\lane-s108tip-086c0fa41\\wwe2k14.exe"' % sc('s108_thb_cap_colour_states.txt'), False)])
    run = lambda tag, folder, route, s, key: dict(tag=tag, folder=folder, route=route, first=t(s), last=t(s + 60), key=key, how='route name')
    runs = [run('1', 'lane-s108tip-cb873c14b', 's108_ban_arena53.txt', 150, 'BAN'), run('2', 'lane-s108tip-cb873c14b', 's108_caa_entrance.txt', 155, 'CAA'),
            run('3', 'lane-s108tip-086c0fa41', 's108_thb_cap_colour_states.txt', 1010, 'THB'),
            run('4', 'lane-s108prt-28d28bf4e', 's108_ban_arena53.txt', 2050, 'BAN'), run('5', 'lane-s108prt-28d28bf4e', 's108_bld_replay.txt', 2055, 'BLD'),
            run('6', 'lane-s108tip-cb873c14b', 's108_bld5_late.txt', 3050, 'BLD5')]
    exes = [dict(name='lane-s108tip-cb873c14b', lane='s108tip'), dict(name='lane-s108tip-086c0fa41', lane='s108tip'), dict(name='lane-s108prt-28d28bf4e', lane='s108prt')]
    merges = [(t(0), 'cb873c14b', ['BAN3'], 'S108: merge lane BAN3'), (t(900), '086c0fa41', ['MENU'], 'S108: merge lane MENU')]
    keys = {'BAN', 'BAN3', 'CAA', 'CAP', 'MENU', 'THB', 'BLD', 'BLD5', 'PRT'}
    LT.launch_lanes(runs, [menu], main, exes, merges, keys)
    got = {r['tag']: r['key'] for r in runs}
    cm = LT.chain_map(sorted(keys))
    check('one call, BAN3 tip exe, banner + created-arena routes: both runs are BAN\'s check', cm.get(got['2']) == 'BAN' and cm.get(got['1']) == 'BAN', str(got))
    check('a run the MENU route lane launched is MENU\'s, though its route is named for THB', got['3'] == 'MENU', str(got))
    check('the PRT exe on the BAN and BLD routes, and a single-route call, keep the route\'s lane', (got['4'], got['5'], got['6']) == ('BAN', 'BLD', 'BLD5'), str(got))


def t_nights(root):
    ts = [T0 + timedelta(minutes=m) for m in (0, 5, 10, 200, 205)]
    n = LT.find_nights(ts, 3600)
    check('night = a gap over 60 min with no record', len(n) == 1 and abs((n[0][1] - n[0][0]).total_seconds() - 190 * 60) < 1, str(n))
    # S108: a compaction's summary (03:39) and a system record, with no reply after them, sat inside the 02:44-08:37 night
    p = os.path.join(root, 'night.jsonl')
    write(p, [prompt(0, 'go'), req(1, 'r1'), asst(5, 'r1', {'type': 'text', 'text': 'done'}, 'm1'),
              {'type': 'system', 'subtype': 'compact_boundary', 'timestamp': at(3 * 3600)},
              prompt(3 * 3600 + 1, 'This session is being continued from a previous conversation that ran out of context. The summary below ...'),
              prompt(6 * 3600, 'Yes to the run'), req(6 * 3600 + 1, 'r2'), asst(6 * 3600 + 5, 'r2', {'type': 'text', 'text': 'ok'}, 'm2')])
    n = LT.find_nights(LT.parse_transcript(p)['times'], 3600)
    check('a compaction summary / system record inside a night does not split it', len(n) == 1 and abs((n[0][1] - n[0][0]).total_seconds() - (6 * 3600 - 5)) < 1,
          str([(a.isoformat(), b.isoformat()) for a, b in n]))


def mini_agent(root, folder, n, desc, atype, start, end, handback=False):
    """a made-up agent: starts at `start`, last record at `end`; with handback=True its last block is a SubagentHandback call at `end`"""
    block = tool_use('SubagentHandback', {'message': 'done'}) if handback else {'type': 'text', 'text': 'working'}
    recs = [prompt(start, 'brief'), req(start + 1, 'r%d' % n), asst(end, 'r%d' % n, block, 'm%d' % n)]
    return agent_file(root, 'a%016d' % n, desc, atype, recs, folder)


def mini_session(root, folder, agents, jarrod=()):
    """agents = [(desc, atype, start, end, handback)]; jarrod = seconds at which Jarrod typed a message -> (agents, main) via load_session"""
    for i, (d, at_, s, e, hb) in enumerate(agents, 1):
        mini_agent(root, folder, 100 + i + SEQ[0] * 100, d, at_, s, e, hb)
    SEQ[0] += 1
    write(os.path.join(root, folder + '.jsonl'), [prompt(0, 'start')] + [prompt(s, 'Jarrod message') for s in jarrod])
    return LT.load_session(os.path.join(root, folder))


def rows_of(agents, main, merges=()):
    rows, notes = LT.chain_table(agents, main, list(merges), [], [], {'tz': timezone.utc, 'nights': []})
    return {r['chain']: r for r in rows}, notes


def t_lane_forms():
    """where the lane key sits: after the model word and the role words, up to the colon (S110 LT: tonight's descriptions)"""
    want = {'SON builder TRON-B: tron slot screens in the launcher and importer UI': 'TRON-B', 'OPUS review TRON-B: tron slot screens': 'TRON-B',
            'SON builder SEAT-A0: the seat card format': 'SEAT-A0', 'SON builder SEAT-A: importer library + frame writer': 'SEAT-A',
            'OPUS review SEAT-A: importer library + frame': 'SEAT-A', 'SON builder SEAT-B: game seating from cards': 'SEAT-B',
            'SON builder LODGE3: lodge the later probe notes': 'LODGE3', 'OPUS probe SEAT2: the shape of step 2': 'SEAT2',
            'SON builder SEAT2-HA: the hand-over core and units': 'SEAT2-HA', 'OPUS review SEAT2: hand-over hook points re-derived': 'SEAT2',
            'SON builder SEAT-C: launchers on the library': 'SEAT-C', 'OPUS route lane TRONROW: x': 'TRONROW', 'OPUS lane AL1 shortfall': 'AL1',
            'SON build lane BLD7 x': 'BLD7', 'OPUS probe-builder B2 y': 'B2', 'OPUS reader A01': 'other', 'OPUS probe: OMG barricade recipe': 'other',
            'SON builder: director place verb + route': 'other', 'OPUS writer: ai': 'other'}
    got = {d: LT.lane_key(d) for d in want}
    check('the lane key sits after the model + role words, up to the colon (19 forms)', got == want, str({d: g for d, g in got.items() if g != want[d]}))


def t_chain_exact(root):
    """one chain per EXACT key: LODGE / LODGE2 / LODGE3, SEAT-A0 / SEAT-A / SEATREL are separate rows; a reviewer joins its builder's chain"""
    ag = [('SON builder LODGE: a', 'lane-builder', 0, 100, True), ('SON builder LODGE2: b', 'lane-builder', 10, 100, True),
          ('SON builder LODGE3: c', 'lane-builder', 20, 100, True), ('SON builder SEAT-A0: d', 'lane-builder', 30, 100, True),
          ('SON builder SEAT-A: e', 'lane-builder', 40, 100, True), ('OPUS review SEAT-A: f', 'review-fixbuilder', 60, 300, True),
          ('SON builder SEATREL: g', 'lane-builder', 50, 100, True)]
    agents, main = mini_session(root, 'exact', ag)
    rows, _ = rows_of(agents, main)
    check('KEY, KEY2, KEY3 and SEAT-A0 / SEAT-A / SEATREL stay six apart rows', set(rows) == {'LODGE', 'LODGE2', 'LODGE3', 'SEAT-A0', 'SEAT-A', 'SEATREL'}, str(sorted(rows)))
    check('a reviewer of SEAT-A joins its builder\'s chain (2 agents)', rows.get('SEAT-A', {}).get('nag') == 2, str(rows.get('SEAT-A', {}).get('nag')))


def t_chain_split(root):
    """a merge of the key closes its chain and a later agent of the key opens KEY#2; two agents far apart with no merge split too; an agent soon
    after, with no merge between, stays in the chain; another key's merge closes nothing"""
    h = 3600
    ag = [('SON builder LODGE: a', 'lane-builder', 0, 100, True), ('OPUS review LODGE: b', 'review-fixbuilder', 150, 300, True),
          ('SON builder LODGE: later', 'lane-builder', 8 * h, 8 * h + 600, True),
          ('SON builder QA: x', 'lane-builder', 0, 100, True), ('OPUS review QA: y', 'review-fixbuilder', 500, 900, True),
          ('SON builder QB: x', 'lane-builder', 0, 100, True), ('SON builder QB: z', 'lane-builder', 6 * h, 6 * h + 100, True),
          ('SON builder QC: x', 'lane-builder', 0, 100, True), ('SON builder QC: z', 'lane-builder', 1000, 1100, True)]
    agents, main = mini_session(root, 'split', ag)
    t = lambda s: T0 + timedelta(seconds=s)
    merges = [(t(400), 'abc1234', ['LODGE'], 'merge lane LODGE'), (t(700), 'def5678', ['OTHER'], 'merge lane OTHER'),
              (t(800), 'aaa9999', ['QC'], 'merge lane QC')]
    rows, _ = rows_of(agents, main, merges)
    check('a merge of LODGE closes the chain: LODGE (2 agents) and LODGE#2 (1)', rows.get('LODGE', {}).get('nag') == 2 and rows.get('LODGE#2', {}).get('nag') == 1, str({k: v['nag'] for k, v in rows.items()}))
    check('a merge of another key closes nothing: QA stays one chain of 2', rows.get('QA', {}).get('nag') == 2 and 'QA#2' not in rows, str(sorted(rows)))
    check('hours apart, no merge: QB and QB#2', rows.get('QB', {}).get('nag') == 1 and rows.get('QB#2', {}).get('nag') == 1, str(sorted(rows)))
    check('a merge of QC between its agents splits them (QC / QC#2)', 'QC#2' in rows, str(sorted(rows)))
    check('LODGE#2 starts at its own first agent, not the first chain\'s', rows.get('LODGE#2') is not None and abs((rows['LODGE#2']['t0'] - t(8 * h)).total_seconds()) < 2, str(rows.get('LODGE#2', {}).get('t0')))


def t_wait_rule(root):
    """a gap holding a Jarrod message is 'wait on Jarrod' only after a hand-back; with none before it the minutes are 'idle (cause unknown)'"""
    ag = [('SON builder WA: a', 'lane-builder', 0, 60, True), ('OPUS review WA: b', 'review-fixbuilder', 1000, 1100, True),
          ('SON builder WB: a', 'lane-builder', 0, 60, False), ('OPUS review WB: b', 'review-fixbuilder', 1000, 1100, True)]
    agents, main = mini_session(root, 'wait', ag, jarrod=[500])
    rows, _ = rows_of(agents, main)
    wa, wb = rows['WA']['split'], rows['WB']['split']
    check('WA handed back, Jarrod answered at 500: wait on Jarrod ~440 s, idle 0', 400 < wa.get('read-back / reply wait', 0) < 480 and wa.get('idle (cause unknown)', 0) == 0, str(dict(wa)))
    check('WB never handed back: the same gap is idle (cause unknown), wait 0', 400 < wb.get('idle (cause unknown)', 0) < 480 and wb.get('read-back / reply wait', 0) == 0, str(dict(wb)))
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        LT.print_chains(list(rows_of(agents, main)[0].values()), [], [])
    check('the header states the waiting rule and the idle label', 'idle (cause unknown)' in buf.getvalue() and 'had handed back' in buf.getvalue(), buf.getvalue()[-300:])


def t_no_lane_list(root):
    """agents naming no lane are listed, each description once with its count"""
    ag = [('OPUS reader A01', 'general-purpose', 0, 50, True), ('OPUS reader A01', 'general-purpose', 10, 50, True), ('OPUS writer: ai', 'general-purpose', 0, 50, True)]
    agents, main = mini_session(root, 'nolane', ag + [('SON builder ZK: x', 'lane-builder', 0, 50, True)])
    rows, notes = rows_of(agents, main)
    txt = '\n'.join(notes)
    check('descriptions naming no lane listed with counts', '3 agents name no lane' in txt and '  2 x OPUS reader A01' in txt and '  1 x OPUS writer: ai' in txt, txt)


def main():
    root = tempfile.mkdtemp(prefix='lane_time_selftest_')
    print('lane_time selftest (fixtures in %s)' % root)
    guard('reply end', lambda: t_reply_end(root))
    guard('background', lambda: t_background(root))
    guard('wait', lambda: t_wait(root))
    guard('messages', lambda: t_messages(root))
    guard('chain', lambda: t_chain(root))
    guard('run lane', t_run_lane)
    guard('run launch', t_run_launch)
    guard('nights', lambda: t_nights(root))
    guard('lane forms', t_lane_forms)
    guard('chain exact keys', lambda: t_chain_exact(root))
    guard('chain split', lambda: t_chain_split(root))
    guard('wait rule', lambda: t_wait_rule(root))
    guard('no-lane list', lambda: t_no_lane_list(root))
    import shutil
    shutil.rmtree(root, ignore_errors=True)
    print('FAIL: %d  (%s)' % (len(FAILS), '; '.join(FAILS)) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
