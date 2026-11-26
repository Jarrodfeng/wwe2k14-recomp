#!/usr/bin/env python3
r"""REDERIVE audit (S111): how much of a session's agent work re-derived facts that other agents had already found.
Reads the session's agent transcripts (the loaders of tools\harness\lane_time.py / tool_calls.py: agent_time_audit.load, lane_key,
meta_of) and the repo's claim files (db\claims\*.json, db\*.json) with their first-commit times from git; writes a markdown report.
No game, no writes besides the report.

  py -I tools\harness\rederive_audit.py [SESSION_ID] [--out FILE] [--also FILE2] [--selftest]

Definitions (also printed in the report):
  tokens          = new input + cache-creation + output tokens, once per API request (cache reads are listed apart)
  code lookup     = a q call with decomp / callers / callees / asm / args / vtable / words / imm / field(s) / vcall / show / addr / rtti,
                    a Read of a generated listing, a Grep / shell grep of generated code
  lookup pair     = (call, game address, kind); the addresses are the sub_XXXXXXXX / 0x8XXXXXXX that follow the verb
  agent (identity)= the lane key of the description, the review: prefix dropped (a lane's builder, probe and reviewer are one agent)
  redundant call  = a lookup call naming an address that an EARLIER call of ANOTHER agent had already looked up
"""
import collections
import datetime
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import agent_time_audit as ata  # noqa: E402
import lane_time  # noqa: E402
import tool_calls  # noqa: E402
import rederive_metrics as rm  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
SESSION = '573afad2-529e-4f9e-9796-72da9c004df1'
PROJ = tool_calls.ROOT

ADDR = r'(?:sub_[0-9A-Fa-f]{8}|0x8[0-9A-Fa-f]{7}|8[0-9A-Fa-f]{7})'
PREFIXED = r'(?:sub_[0-9A-Fa-f]{8}|0x8[0-9A-Fa-f]{7})'     # a bare 8-digit number in a grep is not taken for an address
VERBS = ['decomp', 'callers', 'callees', 'asm', 'args', 'vtable', 'words', 'imm', 'fields', 'field', 'vcall', 'addr', 'rtti', 'nodes']
MECH = {'callers', 'callees', 'vtable', 'field', 'fields', 'imm', 'words', 'args', 'asm', 'vcall', 'addr', 'rtti'}
VERB_RE = re.compile(r'\b(' + '|'.join(VERBS) + r')((?:\s+(?:--\S+|' + ADDR + r'))+)')
ADDR_RE = re.compile(r'(?:sub_|0x)?(8[0-9A-Fa-f]{7})\b')
GEN_PATH = re.compile(r'generated|recomp|decomp|listing|\.asm\b|\.s$', re.I)
QSHOW = re.compile(r'q[\\/]q\.py\b.*\bshow\s+\S+:\d+', re.S)
SHELL = ('Bash', 'PowerShell')


def norm(a):
    m = ADDR_RE.search(a or '')
    return m.group(1).upper() if m else None


def agent_name(desc, atype, aid):
    key = lane_time.lane_key(desc)
    return key if key != 'other' else ((desc or aid)[:28])


def call_lookups(name, inp):
    """-> (is_lookup, [(kind, addr)]) for one tool call"""
    if name in SHELL:
        c = inp.get('command', '')
        pairs = []
        if re.search(r'q[\\/]q\.py', c):
            for v, rest in VERB_RE.findall(c):
                for a in re.findall(ADDR, rest):
                    pairs.append((v, norm(a)))
            is_l = bool(pairs) or bool(QSHOW.search(c)) or bool(re.search(r'\b(?:' + '|'.join(VERBS) + r')\b', c) and re.search(r'q[\\/]q\.py\s+(?:multi|' + '|'.join(VERBS) + ')', c))
            return is_l, pairs
        if re.search(r'\b(grep|rg|findstr|Select-String)\b', c) and GEN_PATH.search(c):
            return True, [('grep-gen', norm(a)) for a in re.findall(PREFIXED, c)]
        return False, []
    if name == 'Read':
        p = inp.get('file_path', '')
        if GEN_PATH.search(p) and not p.lower().endswith(('.md', '.py', '.json', '.toml')):
            return True, [('listing', norm(a)) for a in re.findall(ADDR, p)]
        return False, []
    if name == 'Grep':
        s = (inp.get('path') or '') + ' ' + (inp.get('glob') or '')
        if GEN_PATH.search(s):
            return True, [('grep-gen', norm(a)) for a in re.findall(PREFIXED, inp.get('pattern', ''))]
    return False, []


def load_agent(path):
    desc, atype = tool_calls.meta_of(path)
    aid = os.path.basename(path)[6:-6]
    recs = ata.load(path)
    usage, calls, handback, misses = {}, [], [], []
    seen = set()
    first = last = None
    for r in recs:
        t = ata.ts(r)
        if t:
            first = t if first is None or t < first else first
            last = t if last is None or t > last else last
        m = r.get('message') or {}
        if r.get('type') != 'assistant':
            continue
        u = m.get('usage')
        rid = m.get('id') or r.get('requestId')
        if u and rid:
            old = usage.get(rid, (0, 0, 0, 0))
            usage[rid] = tuple(max(a, b) for a, b in zip(old, (u.get('input_tokens', 0), u.get('cache_creation_input_tokens', 0),
                                                              u.get('output_tokens', 0), u.get('cache_read_input_tokens', 0))))
        for b in m.get('content') or []:
            if not (isinstance(b, dict) and b.get('type') == 'tool_use') or b.get('id') in seen:
                continue
            seen.add(b.get('id'))
            inp = b.get('input') or {}
            is_l, pairs = call_lookups(b.get('name'), inp)
            calls.append(dict(t=t.timestamp() if t else 0.0, name=b.get('name'), lookup=is_l, pairs=[p for p in pairs if p[1]], id=b.get('id'),
                              files=rm.our_files(b.get('name'), inp, is_l, GEN_PATH)))
            if b.get('name') == 'SubagentHandback':
                handback.append(inp.get('message', ''))
            c = inp.get('command', '') if b.get('name') in SHELL else ''
            mm = re.search(r'q\.py\s+missed\s+(?:"([^"]*)"|\'([^\']*)\')', c)
            if mm:
                misses.append(('logged', mm.group(1) or mm.group(2)))
    results = {}                # tool_use id -> result text, for the lookup calls only (M2 / M7)
    want = {c['id'] for c in calls if c['lookup'] and c['pairs']}
    for r in recs:
        if r.get('type') != 'user':
            continue
        cont = (r.get('message') or {}).get('content')
        for b in cont if isinstance(cont, list) else []:
            if isinstance(b, dict) and b.get('type') == 'tool_result' and b.get('tool_use_id') in want:
                x = b.get('content')
                results[b['tool_use_id']] = x if isinstance(x, str) else ' '.join(i.get('text', '') for i in x or [] if isinstance(i, dict))
    for c in calls:
        txt = results.get(c['id'])
        if txt is not None:
            c['outlen'] = len(txt.encode('utf-8', 'replace'))
            c['out'] = txt[:80000]
    for h in handback:
        for ln in h.splitlines():
            if re.search(r'\bq[ -]?miss', ln, re.I) and not re.search(r'no q miss|none|\b0 q miss|zero|nothing|no miss', ln, re.I):
                misses.append(('handback', ln.strip().lstrip('-*0123456789. ')[:300]))
    tot = [sum(v[i] for v in usage.values()) for i in range(4)]
    return dict(aid=aid, desc=desc, atype=atype, name=agent_name(desc, atype, aid), tokens=tot[0] + tot[1] + tot[2], out=tot[2],
                cread=tot[3], reqs=len(usage), calls=calls, first=first, last=last,
                wall=(last - first).total_seconds() if first and last else 0.0, misses=misses)


def load_agents(session):
    files = sorted(glob.glob(os.path.join(PROJ, session, 'subagents', 'agent-*.jsonl')))
    return [load_agent(f) for f in files]


# ---- claims -----------------------------------------------------------------------------------------------------------------------

def git_add_times(repo):
    """claim file (posix path) -> epoch of the commit that first added it (earliest)"""
    out = {}
    try:
        txt = subprocess.run(['git', '-C', repo, 'log', '--format=@%at', '--name-only', '--diff-filter=A', '--', 'db'], capture_output=True,
                             text=True, encoding='utf-8', errors='replace', timeout=300).stdout
    except (OSError, subprocess.SubprocessError):
        return out
    t = None
    for ln in txt.splitlines():
        if ln.startswith('@'):
            t = int(ln[1:])
        elif ln.strip().endswith('.json') and t is not None:
            out[ln.strip()] = min(out.get(ln.strip(), t), t)
    return out


def load_claims(repo, times):
    """-> list of dict(file, stem, t, addr, claim, mark, s111)"""
    rows = []
    paths = glob.glob(os.path.join(repo, 'db', 'claims', '*.json')) + glob.glob(os.path.join(repo, 'db', '*.json'))
    for p in paths:
        rel = os.path.relpath(p, repo).replace('\\', '/')
        try:
            d = json.load(open(p, encoding='utf-8'))
        except (OSError, ValueError):
            continue
        if isinstance(d, dict):
            d = next((v for v in d.values() if isinstance(v, list)), [])
        t = times.get(rel)
        if t is None:
            t = os.path.getmtime(p)
        stem = os.path.basename(p)[:-5]
        for r in d:
            if not isinstance(r, dict):
                continue
            a = norm(str(r.get('address') or '')) or norm(str(r.get('function') or ''))
            if a:
                rows.append(dict(file=rel, stem=stem, t=t, addr=a, claim=str(r.get('claim') or r.get('text') or ''), mark=r.get('mark', ''),
                                 s111=os.path.basename(p).startswith('s111_') and 'claims' in rel))
    return rows


def stem_lane(stem):
    return re.sub(r'^s\d+_', '', stem).upper()


# ---- analysis ---------------------------------------------------------------------------------------------------------------------

TOPICS = [
    ('chamber', r'chamber'),
    ('Create a Championship', r'championship|\bbelt|cacmap|cacthumb|cacbelt|cacbuild|cacmatch|cacplate|caccam|cacbg'),
    ('crowd', r'crowd'),
    ('read-backs', r'read-?back'),
    ('wrestler latch', r'latch'),
    ('shadow passes', r'shadow'),
    ('props / CWeapon', r'cweapon|prop(?:order|spec|rekey)|\bprops?\b'),
    ('render categories', r'categor|cat33|cattable|\bcat\d'),
    ('frame / task order', r'frame order|task group|task order|frame task|per-frame|task list|scheduler|taskorder|\border\b'),
    ('vtables / indirect calls', r'vtable|indirect|vcall|slot \d'),
]


def topic_of(text):
    t = text.lower()
    for n, rx in TOPICS:
        if re.search(rx, t):
            return n
    return 'other'


def analyze(agents, claims):
    A = {}                      # agent name -> aggregate
    for a in agents:
        g = A.setdefault(a['name'], dict(name=a['name'], n=0, tokens=0, out=0, cread=0, wall=0.0, calls=0, lookups=0, redundant=0, reqs=0))
        g['n'] += 1
        g['tokens'] += a['tokens']
        g['out'] += a['out']
        g['cread'] += a['cread']
        g['wall'] += a['wall']
        g['reqs'] += a['reqs']
        g['calls'] += len(a['calls'])
        g['lookups'] += sum(1 for c in a['calls'] if c['lookup'])
    # lookup events in time order
    ev = []
    for a in agents:
        for i, c in enumerate(a['calls']):
            if c['lookup']:
                ev.append((c['t'], a['name'], a['aid'], i, c))
    ev.sort(key=lambda e: e[0])
    seen_by = collections.defaultdict(list)           # addr -> [(t, agent, kind)]
    for t, ag, aid, i, c in ev:
        red = False
        for k, ad in c['pairs']:
            if any(x[1] != ag for x in seen_by[ad]):
                red = True
        if red:
            A[ag]['redundant'] += 1
            c['red'] = True
        for k, ad in c['pairs']:
            seen_by[ad].append((t, ag, k))
    by_addr = {}
    for ad, ls in seen_by.items():
        ags = sorted({x[1] for x in ls})
        if len(ags) >= 2:
            by_addr[ad] = ls
    by_claim = collections.defaultdict(list)
    for c in claims:
        by_claim[c['addr']].append(c)
    rows = []
    for ad, ls in by_addr.items():
        first = {}
        for t, ag, k in sorted(ls):
            first.setdefault(ag, t)
        order = sorted(first, key=lambda g: first[g])
        later_with = 0
        pre_files = set()
        for ag in order[1:]:
            pre = [c for c in by_claim.get(ad, []) if c['t'] < first[ag] and stem_lane(c['stem']) != ag.upper()]
            if pre:
                later_with += 1
                pre_files.update(c['stem'] for c in pre)
        kinds = collections.Counter(k for t, ag, k in ls)
        cl = by_claim.get(ad, [])
        text = (' '.join(c['claim'] for c in cl) + ' ' + ' '.join(c['stem'] for c in cl)) if cl else ' '.join(order)   # the claims name the topic; the lookers only when there is none
        rows.append(dict(addr=ad, agents=order, n=len(order), kinds=kinds, lookups=len(ls), later=len(order) - 1, later_with=later_with,
                         pre=sorted(pre_files), claimed=bool(cl), topic=topic_of(text), claim0=(cl[0]['claim'] if cl else '')))
    rows.sort(key=lambda r: (-r['n'], -r['lookups'], r['addr']))
    # claims in 2+ notes (s111 claim files)
    multi = []
    g = collections.defaultdict(list)
    for c in claims:
        if c['s111']:
            g[c['addr']].append(c)
    for ad, cs in g.items():
        if len({c['stem'] for c in cs}) >= 2:
            multi.append((ad, cs))
    multi.sort(key=lambda x: (-len({c['stem'] for c in x[1]}), x[0]))
    # topics
    raddr = {r['addr']: r for r in rows}
    topics = {}
    for n, _ in TOPICS + [('other', '')]:
        topics[n] = dict(addrs=0, agents=set(), lookups=0, with_claim=0, mech=0, calls=0, tok=0.0)
    lane_tok = {n: g['tokens'] / max(g['calls'], 1) for n, g in A.items()}
    for ad, r in raddr.items():
        T = topics[r['topic']]
        T['addrs'] += 1
        T['agents'].update(r['agents'])
    for t, ag, aid, i, c in ev:
        done = set()
        for k, ad in c['pairs']:
            r = raddr.get(ad)
            if not r:
                continue
            T = topics[r['topic']]
            T['lookups'] += 1
            earlier_claim = any(x['t'] < t and stem_lane(x['stem']) != ag.upper() for x in by_claim.get(ad, []))
            if earlier_claim:
                T['with_claim'] += 1
            elif k in MECH:
                T['mech'] += 1
            if r['topic'] not in done:
                done.add(r['topic'])
                T['calls'] += 1
                T['tok'] += lane_tok[ag]
    return A, rows, multi, topics


def miss_kind(s):
    s = s.lower()
    for n, rx in [('other-branch / outside-checkout file access', r'branch|git show|worktree|commit|outside the checkout'),
                  ('capped / truncated / failed output', r'\bcap|truncat|\bcut\b|limit|memoryerror|noisy|failed|--all'),
                  ('thread / caller-tree / include graph', r'thread|caller tree|--depth|include graph|which header'),
                  ('vtable / indirect-call / slot', r'vtable|vcall|indirect|\bslot'),
                  ('argument / register-passed value tracking', r'\bargs\b|register|wrapper|argument|in_r\d|stack param'),
                  ('field / displacement / writers', r'field|displacement|\bstores?\b|writers?|load / store'),
                  ('range / address-window query', r'range|window|between|0x8[0-9a-f]{7}-'),
                  ('asset / texture / material query', r'asset|\bpac\b|\btex(?:ture)?s?\b|\bmat\b|material|dds|mesh|stage ?key'),
                  ('text / regex / doc-table search', r'regex|ours text|table|\brow\b|docs|grep|include|return statements|loop'),
                  ('render-state / category table', r'render|categor|blend|z-write|state')]:
        if re.search(rx, s):
            return n
    return 'other'


def read_missed_log(paths, t0, t1):
    out = []
    for p in paths:
        if not os.path.exists(p):
            continue
        for ln in open(p, encoding='utf-8', errors='replace'):
            parts = ln.rstrip('\n').split('\t', 2)
            if len(parts) < 3:
                continue
            try:
                t = datetime.datetime.strptime(parts[0], '%Y-%m-%d %H:%M')
            except ValueError:
                continue
            if t0 and t1 and not (t0 <= t <= t1):
                continue
            out.append((parts[1], parts[2]))
        break
    return out


# ---- report -----------------------------------------------------------------------------------------------------------------------

def fmt_t(s):
    s = int(s)
    return '%dh%02dm' % (s // 3600, s % 3600 // 60) if s >= 3600 else '%dm%02ds' % (s // 60, s % 60)


def short(s, n=150):
    s = re.sub(r'\s+', ' ', s or '').replace('|', '/')
    return s if len(s) <= n else s[:n - 1] + '~'


def report(agents, claims, missed_log, now=None):
    A, rows, multi, topics = analyze(agents, claims)
    L = []
    w = L.append
    ntok = sum(a['tokens'] for a in agents)
    ncr = sum(a['cread'] for a in agents)
    nw = sum(a['wall'] for a in agents)
    ncalls = sum(len(a['calls']) for a in agents)
    nl = sum(1 for a in agents for c in a['calls'] if c['lookup'])
    nred = sum(1 for a in agents for c in a['calls'] if c.get('red'))
    tok_call = {n: g['tokens'] / max(g['calls'], 1) for n, g in A.items()}
    est_call = sum(g['redundant'] * tok_call[n] for n, g in A.items())
    dom = [g for g in A.values() if g['calls'] and g['redundant'] / g['calls'] >= 0.25]
    est_dom = sum(g['tokens'] for g in dom)
    firsts = [a['first'] for a in agents if a['first']]
    lasts = [a['last'] for a in agents if a['last']]
    span = (max(lasts) - min(firsts)).total_seconds() if firsts else 0
    w('# REDERIVE audit (S111): how much agent work re-derived what other agents had found')
    w('')
    w('Generated by `tools\\harness\\rederive_audit.py` from the session transcripts and `db\\claims`. Tokens = new input + cache-creation + output per API request;')
    w('cache reads are listed apart. "Agent" = the lane key, builder / probe / reviewer of one lane counted once.')
    w('')
    w('## 0. Numbers first')
    w('')
    w('| measure | value |')
    w('|---|---|')
    w('| subagent transcripts / agents (lane keys) | %d / %d |' % (len(agents), len(A)))
    w('| total agent tokens (new in + cache-create + out) | %.1fM (out %.1fM) |' % (ntok / 1e6, sum(a['out'] for a in agents) / 1e6))
    w('| cache-read tokens (context replay, not counted above) | %.0fM |' % (ncr / 1e6))
    w('| agent wall time (sum of agents) / session span | %s / %s |' % (fmt_t(nw), fmt_t(span)))
    w('| tool calls / code-lookup calls | %d / %d (%.0f%%) |' % (ncalls, nl, 100.0 * nl / max(ncalls, 1)))
    w('| redundant lookup calls (an address another agent had looked up earlier) | %d (%.0f%% of lookups, %.0f%% of all calls) |' % (nred, 100.0 * nred / max(nl, 1), 100.0 * nred / max(ncalls, 1)))
    w('| game addresses looked up by 2+ agents | %d |' % len(rows))
    w('| of those, with a claim row from ANOTHER lane committed before a later agent looked | %d |' % sum(1 for r in rows if r['later_with']))
    w('| addresses claimed in 2+ s111 claim files | %d |' % len(multi))
    w('| ESTIMATE A: tokens of redundant calls (lane tokens per call x redundant calls) | %.1fM = %.0f%% of total |' % (est_call / 1e6, 100.0 * est_call / max(ntok, 1)))
    w('| ESTIMATE B: tokens of lanes where >= 25%% of calls were redundant lookups (%d lanes, whole lane counted) | %.1fM = %.0f%% of total |' % (len(dom), est_dom / 1e6, 100.0 * est_dom / max(ntok, 1)))
    w('')
    w('Both estimates are rough: A prices every call at its lane\'s mean tokens per call (a lookup call is cheaper than a mean call, so A overstates in tokens, but it ignores the')
    w('reasoning around each re-read); B counts whole lanes. A re-read can also be a deliberate check (a reviewer, a second angle); this tool cannot tell.')
    w('Address matching is by text in the call (verb + address); a lookup of a function by name or by file is not seen, so the true figure is higher.')
    w('')
    w('## 1. Per lane, top 40 by tokens')
    w('')
    w('| lane | agents | tokens | cache-read | wall | calls | lookup calls | lookup share | redundant |')
    w('|---|---|---|---|---|---|---|---|---|')
    for g in sorted(A.values(), key=lambda g: -g['tokens'])[:40]:
        w('| %s | %d | %.2fM | %.0fM | %s | %d | %d | %.0f%% | %d |' % (g['name'], g['n'], g['tokens'] / 1e6, g['cread'] / 1e6, fmt_t(g['wall']), g['calls'],
                                                                      g['lookups'], 100.0 * g['lookups'] / max(g['calls'], 1), g['redundant']))
    w('')
    w('(%d lanes in all.)' % len(A))
    w('')
    w('## 2. Re-derivation by address (looked up by 2+ agents), top 40 by agents')
    w('')
    w('"claim before" = later agents (of N-1) that looked after a claim row of another lane for that address was committed (git first-add time of the claim file).')
    w('')
    w('| address | agents | lookups | kinds | claim before | topic | agents (first looker first) |')
    w('|---|---|---|---|---|---|---|')
    for r in rows[:40]:
        w('| %s | %d | %d | %s | %d/%d%s | %s | %s |' % (r['addr'], r['n'], r['lookups'], ' '.join('%s %d' % kv for kv in r['kinds'].most_common(4)), r['later_with'], r['later'],
                                                         (' (' + ','.join(r['pre'][:2]) + ')') if r['pre'] else '', r['topic'], ', '.join(r['agents'][:8])))
    w('')
    tl = sum(r['later'] for r in rows)
    tw = sum(r['later_with'] for r in rows)
    w('All %d re-derived addresses: %d later-agent re-reads, of which %d (%.0f%%) came after a claim row for the address already existed ("the index had it").' % (len(rows), tl, tw, 100.0 * tw / max(tl, 1)))
    w('Re-derived addresses that carry a claim at all (now): %d of %d.' % (sum(1 for r in rows if r['claimed']), len(rows)))
    w('')
    w('## 3. Facts claimed in 2+ notes (s111 claim files)')
    w('')
    w('%d addresses. Top 40 by number of notes; each cell is the note and its claim text.' % len(multi))
    w('')
    w('| address | notes | claims |')
    w('|---|---|---|')
    for ad, cs in multi[:40]:
        seen = set()
        cells = []
        for c in sorted(cs, key=lambda c: c['t']):
            if c['stem'] in seen:
                continue
            seen.add(c['stem'])
            cells.append('%s: %s' % (stem_lane(c['stem']), short(c['claim'], 110)))
        w('| %s | %d | %s |' % (ad, len(seen), ' // '.join(cells[:4])))
    w('')
    w('## 4. Topic clusters (re-derived addresses, by claim text, lane names and notes)')
    w('')
    w('Lookup columns count (call, address, kind) pairs on re-derived addresses. "fact store" = a claim of another lane existed before the lookup (a typed fact lookup would have')
    w('answered); "mechanical" = no claim yet, but the kind is callers / callees / vtable / field / imm / words / args / asm / vcall (a generated table answers it without reading code).')
    w('Tokens: "attributed" = lane tokens per call x calls that touched the topic; summed over lanes that touched it, so topics overlap.')
    w('')
    w('| topic | addresses | agents | lookups | fact store | mechanical | answerable | attributed tokens |')
    w('|---|---|---|---|---|---|---|---|')
    for n, T in sorted(topics.items(), key=lambda kv: -kv[1]['tok']):
        if not T['addrs']:
            continue
        ans = T['with_claim'] + T['mech']
        w('| %s | %d | %d | %d | %d | %d | %.0f%% | %.2fM |' % (n, T['addrs'], len(T['agents']), T['lookups'], T['with_claim'], T['mech'], 100.0 * ans / max(T['lookups'], 1), T['tok'] / 1e6))
    w('')
    tl_ = sum(T['lookups'] for T in topics.values())
    ta = sum(T['with_claim'] + T['mech'] for T in topics.values())
    w('All topics: %d lookups on re-derived addresses, %d (%.0f%%) answerable by a fact store or a mechanical table.' % (tl_, ta, 100.0 * ta / max(tl_, 1)))
    w('')
    w('## 5. q misses')
    w('')
    mh = collections.defaultdict(list)
    for a in agents:
        for src, s in a['misses']:
            mh[miss_kind(s)].append((a['name'], src, s))
    nh = sum(len(v) for v in mh.values())
    w('From the transcripts (hand-back lines naming a q miss, and `q.py missed` calls): %d. The `q missed` log (%s): %d lines in the session window.' % (nh, missed_log[0], len(missed_log[1])))
    w('')
    lg = collections.defaultdict(list)
    for prj, s in missed_log[1]:
        lg[miss_kind(s)].append((prj, s))
    w('| kind | transcripts | log | example (log, else transcript) |')
    w('|---|---|---|---|')
    for k in sorted(set(mh) | set(lg), key=lambda k: -(len(mh.get(k, [])) + len(lg.get(k, [])))):
        ex = lg[k][0][1] if lg.get(k) else mh[k][0][2]
        w('| %s | %d | %d | %s |' % (k, len(mh.get(k, [])), len(lg.get(k, [])), short(ex, 150)))
    w('')
    w('## 6. Totals')
    w('')
    w('- Total agent tokens: %.1fM new (+ %.0fM cache reads); agent wall %s over a %s session span; %d tool calls.' % (ntok / 1e6, ncr / 1e6, fmt_t(nw), fmt_t(span), ncalls))
    w('- Estimated re-derivation: A %.1fM (%.0f%%), B %.1fM (%.0f%%); see section 0 for what each counts.' % (est_call / 1e6, 100.0 * est_call / max(ntok, 1), est_dom / 1e6, 100.0 * est_dom / max(ntok, 1)))
    w('- Lanes most dominated by redundant lookups: ' + ', '.join('%s (%d/%d)' % (g['name'], g['redundant'], g['calls']) for g in sorted(dom, key=lambda g: -g['tokens'])[:10]))
    w('')
    ml_, mnums = rm.metrics_lines(agents, claims, rows, A, REPO, fmt_t)
    L.extend(ml_)
    return '\n'.join(L) + '\n', dict(tokens=ntok, est_a=est_call, est_b=est_dom, rows=rows, topics=topics, multi=multi, A=A, metrics=mnums)


def default_logs():
    cands = [os.path.join(REPO, 'out', 'harness', 'q_missed.log')]
    main = lane_time.main_root(REPO) if hasattr(lane_time, 'main_root') else None
    if main:
        cands.append(os.path.join(main, 'out', 'harness', 'q_missed.log'))
    cands.append(os.path.join(os.path.expanduser('~'), 'wwe2k14-recomp', 'out', 'harness', 'q_missed.log'))
    return [c for c in cands if os.path.exists(c)] or cands[:1]


def run(session, out, also):
    agents = load_agents(session)
    if not agents:
        print('rederive_audit: no agent transcripts for session %r under %s: nothing written (a report from 0 agents would only overwrite a real one)' % (session, PROJ),
              file=sys.stderr)
        return 2
    claims = load_claims(REPO, git_add_times(REPO))
    firsts = [a['first'] for a in agents if a['first']]
    lasts = [a['last'] for a in agents if a['last']]
    t0 = min(firsts).astimezone().replace(tzinfo=None) if firsts else None
    t1 = max(lasts).astimezone().replace(tzinfo=None) if lasts else None
    logs = default_logs()
    ml = (os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(logs[0])))) + '\\out\\harness\\q_missed.log', read_missed_log(logs, t0, t1))
    txt, info = report(agents, claims, ml)
    for p in [out] + ([also] if also else []):
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(txt)
    print('wrote %s (%d lines): tokens %.1fM, est A %.1fM, est B %.1fM, %d re-derived addresses' % (out, txt.count('\n'), info['tokens'] / 1e6, info['est_a'] / 1e6, info['est_b'] / 1e6, len(info['rows'])))
    for ln in txt.splitlines():
        if ln.startswith('| M') and ln[3:4] in ' 12345678':
            print(ln)
    return 0


# ---- selftest ---------------------------------------------------------------------------------------------------------------------

def selftest():
    tmp = tempfile.mkdtemp(prefix='rederive_')
    ids = [0]

    def rec(t, content, usage=None, mid=None):
        ids[0] += 1
        return json.dumps(dict(type='assistant', timestamp='2026-11-26T10:%02d:00Z' % t, message=dict(id=mid or 'm%d' % ids[0], role='assistant', content=content, usage=usage)))

    def call(cid, cmd):
        return [dict(type='tool_use', id=cid, name='Bash', input=dict(command=cmd))]

    def agent(aid, desc, lines):
        p = os.path.join(tmp, 'agent-%s.jsonl' % aid)
        open(p, 'w').write('\n'.join(lines) + '\n')
        json.dump(dict(description=desc, agentType='lane-probe'), open(p[:-6] + '.meta.json', 'w'))
        return p

    u = dict(input_tokens=10, cache_creation_input_tokens=100, output_tokens=50, cache_read_input_tokens=1000)
    p1 = agent('a1', 'OPUS probe AAA: x', [rec(1, call('c1', 'py tools\\q\\q.py decomp sub_8272C5F8 --fields'), u, 'r1'),
                                           rec(2, call('c2', 'py tools\\q\\q.py multi "callers sub_82001000" "show a.h:1-9"'), u, 'r2')])
    p2 = agent('a2', 'OPUS probe BBB: y', [rec(30, call('d1', 'py tools\\q\\q.py decomp sub_8272C5F8'), u, 's1'),
                                           rec(31, call('d2', 'py tools\\q\\q.py decomp sub_89999999'), u, 's2'),
                                           rec(32, [dict(type='tool_use', id='d3', name='SubagentHandback', input=dict(message='done\n- q miss: no thread-of query\nok'))], u, 's3')])
    a1, a2 = load_agent(p1), load_agent(p2)
    assert a1['tokens'] == 2 * 160 and a1['name'] == 'AAA', a1['name']
    assert [c['pairs'] for c in a1['calls']] == [[('decomp', '8272C5F8')], [('callers', '82001000')]]
    assert a1['calls'][1]['lookup'] and a2['misses'] and a2['misses'][0][0] == 'handback'
    claims = [dict(file='db/claims/s111_AAA.json', stem='s111_AAA', t=datetime.datetime(2026, 11, 26, 10, 10, tzinfo=datetime.timezone.utc).timestamp(), addr='8272C5F8',
                   claim='the CWDT category-33 node draws the CWeapon model', mark='C', s111=True),
              dict(file='db/claims/s111_BBB.json', stem='s111_BBB', t=datetime.datetime(2026, 11, 26, 10, 59, tzinfo=datetime.timezone.utc).timestamp(), addr='8272C5F8',
                   claim='again category 33', mark='C', s111=True)]
    A, rows, multi, topics = analyze([a1, a2], claims)
    assert len(rows) == 1 and rows[0]['addr'] == '8272C5F8' and rows[0]['n'] == 2, rows
    assert rows[0]['later'] == 1 and rows[0]['later_with'] == 1, rows[0]      # AAA's claim (10:10) predates BBB's look (10:30)
    assert len(multi) == 1 and A['BBB']['redundant'] == 1 and A['AAA']['redundant'] == 0
    assert rows[0]['topic'] == 'props / CWeapon', rows[0]['topic']
    txt, info = report([a1, a2], claims, ('x', [('p', 'q show cannot read a file on another branch')]))
    assert '8272C5F8' in txt and 'other-branch' in txt and len(txt.splitlines()) < 400
    print('rederive_audit selftest: ok')
    return 0


def selftest_twins():
    """planted twins: `--help` and a 0-agent session write nothing (the S111 clobber: --help was taken as a session id)"""
    import contextlib
    import io
    tmp = tempfile.mkdtemp(prefix='rederive_twin_')
    target = os.path.join(tmp, 'REPORT.md')
    open(target, 'w').write('KEEP')
    for argv, want in ((['--help', '--out', target], 0), (['--bogus', '--out', target], 2), (['no-such-session-id', '--out', target], 2)):
        err, so = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(so):
            try:
                code = main(argv)
            except SystemExit as e:
                code = e.code
        assert code == want, (argv, code, err.getvalue())
        assert open(target).read() == 'KEEP', 'a run with %s wrote the report' % argv
    assert not os.path.exists(os.path.join(REPO, 'out', 'harness', 'rederive', 'no-such-session-id.md')), 'a 0-agent run wrote the default report'
    print('rederive_audit twins: ok (--help, unknown flag and 0 agents write nothing)')


def build_parser():
    import argparse
    ap = argparse.ArgumentParser(prog='rederive_audit.py', description='REDERIVE audit: re-derivation of facts across a session\'s agents, plus the INDEX_PLAN section 4 metrics M1-M8.')
    ap.add_argument('session', nargs='?', default=SESSION, help='session id (default: the S111 session)')
    ap.add_argument('--out', help=r'report path. Default out\harness\rederive\<session>.md; give a committed doc path only on purpose')
    ap.add_argument('--also', help='a second path to write the same report to')
    ap.add_argument('--selftest', action='store_true', help='run the selftest and the planted twins; writes nothing')
    return ap


def main(argv):
    args = build_parser().parse_args(argv)        # --help exits 0, an unknown flag exits 2, both before anything is read or written
    if args.selftest:
        selftest()
        selftest_twins()
        return 0
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', args.session):
        print('rederive_audit: %r is not a session id' % args.session, file=sys.stderr)
        return 2
    out = args.out or os.path.join(REPO, 'out', 'harness', 'rederive', args.session + '.md')
    return run(args.session, out, args.also)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
