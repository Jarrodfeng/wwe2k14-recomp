#!/usr/bin/env python3
r"""Tokens per landed lane (from the S106 trial's bars.py; the trial itself is removed).

  py -I tools\harness\tokens.py --tokens <session>.jsonl [--branch s107] [--json]
      tokens per landed lane (usage blocks of the session + subagents\ folder, each message id once; work = input + output + cache_creation, cache_read apart;
      role from the description / agent type, lane = the description's lane tag; landed = the lane's branch is merged into --branch per board.read_merges).
  py -I tools\harness\tokens_selftest.py"""
# entry: main
import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent


# ---------------------------------------------------------------- tokens per landed lane (S106 lane TOK)
USAGE_FIELDS = ('input_tokens', 'output_tokens', 'cache_creation_input_tokens', 'cache_read_input_tokens')
WORK = ('input_tokens', 'output_tokens', 'cache_creation_input_tokens')   # "work" = fresh input + output + cache writes; cache reads are reported apart
ROLE_WORDS = {'review', 're-review', 'rereview', 'reviewer', 'builder', 'fix', 'round', 'probe', 'settle', 'settler', 'adversary', 'skeptic', 'pack', 'lane', 'tool',
              'shot-checker', 'shotchecker', 'reader', 'research', 'survey', 'the', 'a', 'of'}
MODEL_WORDS = ('OPUS', 'SON', 'SONNET', 'HAIKU', 'FABLE', 'OPS', 'HAI')
ROLE_OF_KIND = {'builder': 'builder', 'fix': 'builder', 'review': 'reviewer', 'probe': 'probe', 'other': 'probe', 'adversary': 'probe', 'pack': 'probe', 'shot-checker': 'probe'}


def load_board():
    sys.path.insert(0, str(HERE))
    import board  # noqa: PLC0415
    return board


def lane_key(desc):
    """'SON 0CF compile queue fix round' -> '0CF'; 'OPUS review G4-4 PCH' -> 'G4-4': the first word after the model prefix and the role words."""
    words = re.findall(r'[A-Za-z0-9][A-Za-z0-9\-_]*', desc or '')
    if words and words[0].upper() in MODEL_WORDS:
        words = words[1:]
    for w in words:
        if w.lower() not in ROLE_WORDS:
            return w.upper()
    return ''


def read_usage(path, seen):
    """Sum one transcript's per-message usage blocks. Each message id counts once (a message is written once per content block, and another file may
    repeat it): per field the largest value seen for the id; `seen` {id: usage} is shared across files, so a message in two files counts in the first
    file read only (agents are read before the main loop). -> ({field: n}, messages counted)."""
    tot = dict.fromkeys(USAGE_FIELDS, 0)
    p = Path(path)
    if not p.exists():
        return tot, 0
    best = {}
    with open(p, encoding='utf-8', errors='replace') as f:
        for ln in f:
            if '"usage"' not in ln:
                continue
            try:
                j = json.loads(ln)
            except ValueError:
                continue
            m = j.get('message')
            if j.get('type') != 'assistant' or not isinstance(m, dict) or not isinstance(m.get('usage'), dict):
                continue
            mid = m.get('id') or j.get('requestId') or j.get('uuid')
            if not mid or mid in seen:
                continue
            cur = best.setdefault(mid, dict.fromkeys(USAGE_FIELDS, 0))
            for k in USAGE_FIELDS:
                v = m['usage'].get(k)
                if isinstance(v, int) and v > cur[k]:
                    cur[k] = v
    for mid, cur in best.items():
        seen[mid] = cur
        for k in USAGE_FIELDS:
            tot[k] += cur[k]
    return tot, len(best)


def read_agents(sess):
    """[{id, desc, type, branch, worktree, jsonl, wf}] from <session>\\subagents (agent-*.meta.json) and subagents\\workflows\\<run>. Metadata only."""
    res = []
    base = sess.with_suffix('') / 'subagents'
    for mp in sorted(base.glob('agent-*.meta.json')) + sorted(base.glob('workflows/*/agent-*.meta.json')):
        try:
            m = json.loads(mp.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        wf = mp.parent.name if mp.parent != base else ''
        wt = m.get('worktreePath') or ''
        br = m.get('worktreeBranch') or ('worktree-' + re.split(r'[\\/]', wt.rstrip('\\/'))[-1] if wt else '')
        res.append({'id': mp.name[len('agent-'):-len('.meta.json')], 'desc': m.get('description') or '', 'type': m.get('agentType') or '', 'branch': br,
                    'worktree': bool(m.get('spawnedWithWorktree') or wt), 'jsonl': mp.with_name(mp.name[:-len('.meta.json')] + '.jsonl'), 'wf': wf})
    return res


def tokens_section(sess, landed_srcs, board=None):
    """The tokens section. landed_srcs = branch names merged into the session branch. Numbers only: no transcript text is kept or printed."""
    sess = Path(sess)
    seen = {}
    agents = read_agents(sess)
    for a in agents:
        a['tok'], a['msgs'] = read_usage(a['jsonl'], seen)
        mw = (re.findall(r'[A-Za-z]+', a['desc'])[:1] or [''])[0].upper()
        a['model'] = mw if mw in MODEL_WORDS else '?'
        if a['wf']:
            a['role'] = 'workflow'
            a['key'] = 'wf:' + a['wf'][:8]
        else:
            kind = board.classify(a['desc'], a['type'], a['worktree']) if board else 'other'
            a['role'] = ROLE_OF_KIND.get(kind, 'probe')
            a['key'] = lane_key(a['desc']) or ('agent:' + a['id'][:8])
    keys = {a['key'] for a in agents}
    for a in agents:   # a fix round's key 'G1F' (and 'G1FF') joins lane 'G1' when that lane exists
        k = a['key']
        while k.endswith('F') and len(k) >= 3 and k[:-1] in keys:
            k = k[:-1]
        a['key'] = k
    main_tok, main_n = read_usage(sess, seen)
    srcs = {s.lower() for s in landed_srcs}
    stoks = [set(re.split(r'[-_/]', s)) for s in srcs]
    lanes = {}
    for a in agents:
        L = lanes.setdefault(a['key'], {'key': a['key'], 'agents': [], 'landed': False})
        L['agents'].append(a)
        if a['branch'] and a['branch'].lower() in srcs:
            L['landed'] = True
    for L in lanes.values():   # a named lane branch holds the lane key as a whole token (s106-tok -> TOK)
        if any(L['key'].lower() in t for t in stoks):
            L['landed'] = True

    def tot(ags, fields):
        return sum(a['tok'][f] for a in ags for f in fields)
    rows = []
    for L in sorted(lanes.values(), key=lambda x: -tot(x['agents'], WORK)):
        ags = L['agents']
        rows.append({'lane': L['key'], 'landed': L['landed'], 'agents': len(ags), 'roles': {r: sum(1 for a in ags if a['role'] == r) for r in sorted({a['role'] for a in ags})},
                     'work': tot(ags, WORK), 'cache_read': tot(ags, ('cache_read_input_tokens',)), 'output': tot(ags, ('output_tokens',))})
    byrole = {}
    for a in agents:
        d = byrole.setdefault((a['role'], a['model']), {'agents': 0, 'work': 0, 'cache_read': 0})
        d['agents'] += 1
        d['work'] += sum(a['tok'][f] for f in WORK)
        d['cache_read'] += a['tok']['cache_read_input_tokens']
    landed_rows = [r for r in rows if r['landed']]
    agents_work = sum(r['work'] for r in rows)
    out = {'name': 'tokens per landed lane', 'work_fields': list(WORK), 'lanes': rows,
           'by_role_model': [{'role': k[0], 'model': k[1], **v} for k, v in sorted(byrole.items())],
           'main_loop': {'messages': main_n, 'work': sum(main_tok[f] for f in WORK), 'cache_read': main_tok['cache_read_input_tokens']},
           'landed_lanes': len(landed_rows), 'lanes_total': len(rows), 'agents': len(agents), 'agent_messages': sum(a['msgs'] for a in agents),
           'landed_lane_work': sum(r['work'] for r in landed_rows), 'agents_work': agents_work}
    n = len(landed_rows)
    out['per_landed_lane_landed_only'] = out['landed_lane_work'] / n if n else None
    out['per_landed_lane_all_agents'] = agents_work / n if n else None
    out['per_landed_lane_with_main'] = (agents_work + out['main_loop']['work']) / n if n else None
    return out


def print_tokens(t):
    def k(n):
        return f'{n / 1e6:.2f}M' if abs(n) >= 1e6 else f'{n / 1e3:.0f}k'
    print(f"\nTOKENS  per landed lane   (work = {' + '.join(WORK)}; cache reads shown apart)")
    print(f"   {t['agents']} agents ({t['agent_messages']} distinct messages), {t['lanes_total']} lane keys, {t['landed_lanes']} landed")
    print('   by role / model:')
    for r in t['by_role_model']:
        print(f"     {r['role']:<9} {r['model']:<5} agents {r['agents']:>3}  work {k(r['work']):>8}  cache-read {k(r['cache_read']):>8}")
    ml = t['main_loop']
    print(f"   main loop ({ml['messages']} messages): work {k(ml['work'])}, cache-read {k(ml['cache_read'])}")
    print('   lanes (work tokens; * = landed):')
    for r in t['lanes']:
        roles = ','.join(f'{n}{x[0]}' for x, n in r['roles'].items())
        print(f"     {'*' if r['landed'] else ' '} {r['lane']:<12} agents {r['agents']:>2} {roles:<10} work {k(r['work']):>8}  output {k(r['output']):>7}  cache-read {k(r['cache_read']):>8}")
    if t['landed_lanes']:
        print(f"   tokens per landed lane: {k(t['per_landed_lane_landed_only'])} (landed lanes' own agents), {k(t['per_landed_lane_all_agents'])} (all agents, "
              f"unlanded lanes' cost included), {k(t['per_landed_lane_with_main'])} (plus the main loop)")
    else:
        print('   tokens per landed lane: n/a (no landed lane)')


def landed_srcs_for(sess, branch, landed_file=None):
    """Branch names merged into the session branch since the transcript began: a file of names (selftest) or board.read_merges (the reflog + git readers)."""
    if landed_file:
        return [x.strip() for x in Path(landed_file).read_text(encoding='utf-8').splitlines() if x.strip()]
    board = load_board()
    first = None
    with open(sess, encoding='utf-8', errors='replace') as f:
        for ln in f:
            i = ln.find('"timestamp":"')
            if i >= 0:
                first = ln[i + 13:i + 32]
                break
    t0 = board.utc_to_local(first) if first else datetime.now()
    merges, _ = board.read_merges(branch, t0 - timedelta(hours=1), datetime.now() + timedelta(minutes=5))
    return [m['src'] for m in merges]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--tokens', metavar='SESSION_JSONL', required=True, help='tokens per landed lane from this session transcript (+ its subagents folder)')
    ap.add_argument('--branch', default=None, help='session branch whose merges count as landed (default: the current branch)')
    ap.add_argument('--landed-file', help='test hook: a file of landed branch names instead of the git readers')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    if not a.branch:
        a.branch = subprocess.run(['git', '-C', str(HERE), 'branch', '--show-current'], capture_output=True, text=True).stdout.strip() or 'main'
    try:
        board = load_board()
    except Exception:  # noqa: BLE001
        board = None
    tok = tokens_section(a.tokens, landed_srcs_for(a.tokens, a.branch, a.landed_file), board)
    if a.json:
        print(json.dumps({'tokens': tok}, indent=1))
    else:
        print_tokens(tok)
    return 0


if __name__ == '__main__':
    sys.exit(main())
