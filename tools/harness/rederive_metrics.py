#!/usr/bin/env python3
r"""Metrics M1-M8 of docs\research\s111\INDEX_PLAN.md section 4, for rederive_audit.py (S111 Lane I). Pure functions over the loaded
agents (rederive_audit.load_agent: calls carry id, pairs, out = the result text of lookup calls, outlen, files = our-file reads) and the
claim rows; nothing here reads the game or writes a file.

  M1 re-read pairs after a claim existed   (call, address, kind) pairs where ANOTHER agent had looked at the address earlier AND a claim
                                           row of another lane for it was already committed
  M2 claim-seen rate                       share of those M1 pairs whose own call output printed (the first 30 characters of) such a claim
  M3 first-reply coverage                  STUB: needs Lane F's hook (the outputs of a probe's FIRST reply vs the addresses its note rests on)
  M4 lookups per lane                      code-lookup calls per lane key (median, mean; probe lanes apart)
  M5 tokens / wall per lane                mean over lane keys
  M6 lodge completeness                    notes of the newest docs\research\sNNN folder with a ```claims block / all its notes; claims files
  M7 output bloat                          bytes of single-address `decomp` outputs: top re-derived addresses, and the overall median
  M8 our-file re-reads                     our files shown (q show FILE:N-M, Read of a non-generated file) by 3+ agents: files, reads, and
                                           lines seen by 3+ agents (ranges only; a whole-file Read has no range)
"""
import collections
import glob
import os
import re
import statistics

SHOW_RE = re.compile(r'show\s+"?([^\s":]+\.[A-Za-z0-9]+):(\d+)-(\d+)')
WT_RE = re.compile(r'^.*?(?:\.claude/worktrees/[^/]+|wwe2k14-recomp)/', re.I)
OUR_DIRS = ('src/', 'tools/', 'docs/', 'db/', 'cmake/', 'tests/', 'assets/', 'shaders/')


def norm_path(p):
    p = (p or '').replace('\\', '/').strip('"\'')
    if re.match(r'^[A-Za-z]:/', p) or p.startswith('/'):
        p = WT_RE.sub('', p, count=1)
        if re.match(r'^[A-Za-z]:/', p) or p.startswith('/'):
            return None
    p = re.sub(r'^\./', '', p)
    return p if p.startswith(OUR_DIRS) else None


def our_files(name, inp, is_lookup, gen_path):
    """-> [(file, start|None, end|None)] our files this call shows (normalised, forward slashes)"""
    out = []
    if name == 'Read':
        p = inp.get('file_path', '')
        if is_lookup or gen_path.search(p):
            return []
        f = norm_path(p)
        if f:
            off, lim = inp.get('offset'), inp.get('limit')
            out.append((f, off or 1, (off or 1) + lim - 1) if lim else (f, None, None))
    elif name in ('Bash', 'PowerShell'):
        c = inp.get('command', '')
        for m in SHOW_RE.finditer(c):
            f = norm_path(m.group(1))
            if f and not gen_path.search(f):
                out.append((f, int(m.group(2)), int(m.group(3))))
    return out


def re_read_pairs(agents, claims):
    """the M1 / M2 numbers. -> dict(total, after_claim, seen, per_call_kinds)"""
    by_claim = collections.defaultdict(list)
    for c in claims:
        by_claim[c['addr']].append(c)
    ev = sorted(((c['t'], a['name'], c) for a in agents for c in a['calls'] if c['lookup'] and c['pairs']), key=lambda e: e[0])
    seen = collections.defaultdict(set)      # addr -> agents that looked
    total = after = shown = 0
    for t, ag, c in ev:
        out = re.sub(r'\s+', ' ', c.get('out') or '').lower()
        for k, ad in c['pairs']:
            if seen[ad] - {ag}:
                total += 1
                prior = [x for x in by_claim.get(ad, []) if x['t'] < t and re.sub(r'^s\d+_', '', x['stem']).upper() != ag.upper()]
                if prior:
                    after += 1
                    if any(re.sub(r'\s+', ' ', x['claim']).lower()[:30] in out for x in prior if len(x['claim']) >= 12):
                        shown += 1
        for k, ad in c['pairs']:
            seen[ad].add(ag)
    return dict(total=total, after=after, shown=shown)


def lodge_completeness(repo):
    """-> (notes with a ```claims block, notes, claims files of that session) for the newest docs\\research\\sNNN folder"""
    dirs = sorted(glob.glob(os.path.join(repo, 'docs', 'research', 's[0-9]*')), key=lambda d: int(re.sub(r'\D', '', os.path.basename(d)) or 0))
    if not dirs:
        return 0, 0, 0, ''
    d = dirs[-1]
    notes = glob.glob(os.path.join(d, '*.md'))
    with_block = 0
    for p in notes:
        try:
            if re.search(r'^```claims\b', open(p, encoding='utf-8', errors='replace').read(), re.M):
                with_block += 1
        except OSError:
            pass
    files = glob.glob(os.path.join(repo, 'db', 'claims', os.path.basename(d) + '_*.json'))
    return with_block, len(notes), len(files), os.path.basename(d)


def decomp_bytes(agents, rows):
    """-> (overall list of bytes, {addr: [bytes]}) for calls that are exactly one single-address decomp"""
    allb, per = [], collections.defaultdict(list)
    for a in agents:
        for c in a['calls']:
            if c['lookup'] and len(c['pairs']) == 1 and c['pairs'][0][0] == 'decomp' and c.get('outlen'):
                allb.append(c['outlen'])
                per[c['pairs'][0][1]].append(c['outlen'])
    return allb, per


def file_rereads(agents):
    """-> list of (file, agents, reads, lines_3plus) for files shown by 3+ agents, most reads first"""
    reads = collections.defaultdict(list)       # file -> [(agent, start, end)]
    for a in agents:
        for c in a['calls']:
            for f, s, e in c.get('files') or []:
                reads[f].append((a['name'], s, e))
    res = []
    for f, rs in reads.items():
        ags = {r[0] for r in rs}
        if len(ags) < 3:
            continue
        per = collections.defaultdict(list)      # agent -> merged-free ranges
        for ag, s, e in rs:
            if s is not None and e is not None and e >= s:
                per[ag].append((s, e))
        ev = []
        for ag, rr in per.items():
            for s, e in merge(rr):
                ev.append((s, 1))
                ev.append((e + 1, -1))
        ev.sort()
        cur, last, n3 = 0, 0, 0
        for pos, d in ev:
            if cur >= 3:
                n3 += pos - last
            cur += d
            last = pos
        res.append((f, len(ags), len(rs), n3))
    res.sort(key=lambda r: (-r[2], r[0]))
    return res


def merge(rr):
    out = []
    for s, e in sorted(rr):
        if out and s <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], e))
        else:
            out.append((s, e))
    return out


def metrics_lines(agents, claims, rows, A, repo, fmt_t):
    """the markdown block (list of lines) and a dict of the headline numbers"""
    L = []
    w = L.append
    m = {}
    rp = re_read_pairs(agents, claims)
    m['M1'], m['M2'] = rp['after'], (100.0 * rp['shown'] / rp['after'] if rp['after'] else 0.0)
    lanes = list(A.values())
    lk = [g['lookups'] for g in lanes]
    probe = collections.defaultdict(int)
    for a in agents:
        if 'probe' in (a['atype'] or '').lower() or 'probe' in (a['desc'] or '').lower():
            probe[a['name']] += sum(1 for c in a['calls'] if c['lookup'])
    pl = list(probe.values())
    nlook = sum(lk)
    wb, nn, nf, sess = lodge_completeness(repo)
    allb, per = decomp_bytes(agents, rows)
    fr = file_rereads(agents)
    w('## 7. Measurement baseline (INDEX_PLAN section 4; computed from these transcripts and the repo notes)')
    w('')
    w('| metric | value |')
    w('|---|---|')
    w('| M1 re-read pairs after a claim existed (pairs where another agent had looked earlier / of those, after a claim row of another lane was committed) | %d / %d |' % (rp['total'], rp['after']))
    w('| M2 claim-seen rate (M1 pairs whose own output printed that claim) | %d of %d = %.1f%% |' % (rp['shown'], rp['after'], m['M2']))
    w('| M3 first-reply coverage | not computed: needs Lane F\'s hook (the outputs of a probe\'s first reply against the addresses its note rests on) |')
    w('| M4 code-lookup calls per lane (all lanes: median / mean, %d lanes; probe lanes: median / mean, %d lanes) | %.0f / %.1f; %s |' % (
        len(lanes), len(pl), statistics.median(lk) if lk else 0, nlook / max(len(lanes), 1),
        ('%.0f / %.1f' % (statistics.median(pl), sum(pl) / len(pl))) if pl else 'none'))
    w('| M5 tokens / wall per lane (mean over %d lane keys) | %.2fM tokens, %s |' % (
        len(lanes), sum(g['tokens'] for g in lanes) / max(len(lanes), 1) / 1e6, fmt_t(sum(g['wall'] for g in lanes) / max(len(lanes), 1))))
    w('| M6 lodge completeness (%s notes with a ```claims block / notes; claims files %s_*.json) | %d / %d (%.0f%%); %d files |' % (
        sess or 'none', sess or '-', wb, nn, 100.0 * wb / max(nn, 1), nf))
    w('| M7 decomp output (single-address decomp calls: n / median / max bytes) | %d / %d / %d |' % (
        len(allb), statistics.median(allb) if allb else 0, max(allb) if allb else 0))
    w('| M8 our files shown by 3+ agents (files / reads) | %d / %d |' % (len(fr), sum(r[2] for r in fr)))
    w('')
    w('M1 counts (call, address, kind) pairs; M2 matches the first 30 characters of a prior claim in the call\'s output (a claim printed in other words is not seen, so M2 is a floor).')
    w('')
    w('### M7: decomp output size of the most re-derived addresses (single-address calls only)')
    w('')
    w('| address | agents | calls | median bytes | max bytes |')
    w('|---|---|---|---|---|')
    for r in rows[:15]:
        b = per.get(r['addr'])
        if b:
            w('| %s | %d | %d | %d | %d |' % (r['addr'], r['n'], len(b), statistics.median(b), max(b)))
    w('')
    w('### M8: our files shown by 3+ agents, top 25 by reads')
    w('')
    w('"lines 3+" = lines inside the ranges of 3 or more agents at once (whole-file reads carry no range and are not in that column).')
    w('')
    w('| file | agents | reads | lines 3+ |')
    w('|---|---|---|---|')
    for f, na, nr, n3 in fr[:25]:
        w('| %s | %d | %d | %d |' % (f, na, nr, n3))
    w('')
    return L, m
