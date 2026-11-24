#!/usr/bin/env python3
r"""tokens.py selftest (0 game runs): a synthetic session with planted twins (a message written twice counts once).

  py -I tools\harness\tokens_selftest.py"""
# entry: main
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOK = HERE / 'tokens.py'
FAILS = []

def check(name, ok, detail=''):
    print(('PASS  ' if ok else 'FAIL  ') + name + ((' :: ' + detail[:400]) if detail and not ok else ''))
    if not ok:
        FAILS.append(name)


def msg(mid, i, o, cc, cr, text='SECRET PROMPT TEXT'):
    return json.dumps({'type': 'assistant', 'uuid': 'u' + mid + str(i), 'message': {'id': mid, 'content': [{'type': 'text', 'text': text}],
                       'usage': {'input_tokens': i, 'output_tokens': o, 'cache_creation_input_tokens': cc, 'cache_read_input_tokens': cr}}})


def tokens_test(tmp):
    """tokens section on a synthetic session: lane AA landed (builder + fix round + review), lane BB not landed; message m1 is written twice in the builder's
    file (once per content block) and again in the main file: it must count once (the twin: a double count would add 100 to AA's work)."""
    sess = tmp / 'sess.jsonl'
    sd = tmp / 'sess' / 'subagents'
    sd.mkdir(parents=True)

    def agent(aid, desc, typ, branch, lines):
        (sd / f'agent-{aid}.meta.json').write_text(json.dumps({'description': desc, 'agentType': typ, 'worktreeBranch': branch, 'spawnedWithWorktree': bool(branch)}), encoding='utf-8')
        (sd / f'agent-{aid}.jsonl').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    agent('a1', 'SON AA builder', 'lane-builder', 'worktree-agent-a1', [msg('m1', 10, 20, 30, 1000), msg('m1', 10, 20, 30, 1000), msg('m2', 1, 2, 3, 400)])
    agent('a2', 'SON AAF fix round', 'lane-builder', 'worktree-agent-a2', [msg('m3', 5, 5, 5, 50)])
    agent('a3', 'OPUS review AA', 'reviewer', '', [msg('m4', 7, 7, 7, 70)])
    agent('b1', 'SON BB builder', 'lane-builder', 'worktree-agent-b1', [msg('m5', 100, 100, 100, 5)])
    agent('p1', 'OPUS settle probe CC', 'lane-probe', '', [msg('m6', 2, 2, 2, 2)])
    sess.write_text('\n'.join([msg('m1', 10, 20, 30, 1000), msg('mm1', 1000, 0, 0, 7), msg('mm2', 0, 500, 0, 9), json.dumps({'type': 'user', 'message': {'content': 'x'}})]) + '\n', encoding='utf-8')
    landed = tmp / 'landed.txt'
    landed.write_text('worktree-agent-a1\n', encoding='utf-8')
    p = subprocess.run([sys.executable, '-I', str(TOK), '--tokens', str(sess), '--landed-file', str(landed), '--json'],
                       capture_output=True, text=True)
    t = json.loads(p.stdout[p.stdout.index('{'):])['tokens'] if p.returncode == 0 else {}
    L = {r['lane']: r for r in t.get('lanes', [])}
    check('T0 runs and groups lanes: AA (builder + fix round AAF + review), BB, CC', p.returncode == 0 and set(L) == {'AA', 'BB', 'CC'} and L['AA']['agents'] == 3, p.stdout[:300] + p.stderr[:300])
    # AA work: m1 60 (once) + m2 6 + m3 15 + m4 21 = 102 ; BB 300 ; CC 6
    check('T1 twin: a message written twice and repeated in the main file counts once (AA work 102, not 162 or 222)', L.get('AA', {}).get('work') == 102, str(L.get('AA')))
    check('T2 cache reads apart: AA 1000 + 400 + 50 + 70 = 1520', L.get('AA', {}).get('cache_read') == 1520, str(L.get('AA')))
    check('T3 landed: AA yes (its builder branch merged), BB no', L.get('AA', {}).get('landed') is True and L.get('BB', {}).get('landed') is False, str(L))
    check('T4 main loop apart: m1 went to the agent, so main = 1000 + 500 work', t.get('main_loop', {}).get('work') == 1500 and t['main_loop']['messages'] == 2, str(t.get('main_loop')))
    check('T5 per landed lane: landed-only 102, all agents (102 + 300 + 6) / 1, with main 1908', t.get('per_landed_lane_landed_only') == 102 and t.get('per_landed_lane_all_agents') == 408
          and t.get('per_landed_lane_with_main') == 1908, str({k: v for k, v in t.items() if k.startswith('per_')}))
    roles = {(r['role'], r['model']): r['agents'] for r in t.get('by_role_model', [])}
    check('T6 roles by description / agent type: builder SON 3, reviewer OPUS 1, probe OPUS 1', roles == {('builder', 'SON'): 3, ('reviewer', 'OPUS'): 1, ('probe', 'OPUS'): 1}, str(roles))
    p2 = subprocess.run([sys.executable, '-I', str(TOK), '--tokens', str(sess), '--landed-file', str(landed)],
                        capture_output=True, text=True)
    check('T7 text output has the figure and no transcript text', 'tokens per landed lane' in p2.stdout and 'SECRET' not in p2.stdout + p.stdout, p2.stdout[-300:])


def main():
    tmp = Path(tempfile.mkdtemp(prefix='tokens_st_'))
    tokens_test(tmp)
    print('tokens_selftest: %s' % ('FAIL ' + str(len(FAILS)) if FAILS else 'all ok'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
