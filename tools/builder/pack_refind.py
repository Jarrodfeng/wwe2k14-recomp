"""Re-find check of tools/builder/pack.py on real probe -> fix pairs of 2026-11-19 (BPK).

    py -I tools/builder/pack_refind.py [--set dev|sealed|all] [--verbose] [--no-q] [--floor F]

For each pair: the pack is built from the probe's note(s) (read at the note's own commit) with the target tree set to the commit just before
each fix commit (its first parent). Measure: of the fix's changed hunks in files that existed before it (new files, docs\\ and root *.md
excluded), the share whose old-side lines lie inside a pack block. A pure insertion (old count 0 after line a) is inside when a block holds
both a and a+1. Every miss is listed with its reason.

Round 2 (coordinator, 2026-11-20): the SCORE is the probe-led commits only (the builder's commits before its first review); review-fix
commits are a separate row with no bar (a probe note cannot foresee a review). Each block carries the rule that made it (cite = the note's
own sites; head / unit / lane / readers / plants = the companion rules, pack.py), and hits, KB and hits per KB are printed per rule.
Two sets: DEV (the four pairs the tool was designed on) and SEALED (four pairs frozen in this file BEFORE the companion rules were built
or run on them: one run; a rule changed after seeing them needs fresh pairs). The 80 % is the tool's printed target; --floor (the gate) is
a regression floor on the DEV probe-led score. Planted fault: the bare-filename resolver off must lower the DEV score.
Reads git history and committed notes only (nothing under out\\ with --no-q). Prints counts and paths, never a line of the fix or of game code.
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pack  # noqa: E402

TARGET = 0.80
MAX_SECS = 15.0

# (name, set, notes, probe-led commits, review-fix commits, remark)
# DEV: RFX's builder also made SVY's c8aac66ef (note SVD); ea92067d8 is their joint review-fix commit (row RFX, review).
# SEALED (frozen 2026-11-20 before any round-2 rule was built; chosen from commit subjects and parents only, no fix diff read):
#   HPR -> HPF 164757818 (the HPR note was read in round 1 for citation shapes only; its fix was never opened)
#   WMS -> WMBD 1a203e31a (the WMBD builder's first commit; b3dfe4508 follows the later WMS2 note: left out)
#   PEP -> DTP core e76f4cbeb + tests cc4b57e97 + lane 37f0edf51; review row: "DTP fix" a2481bc6d
#   CRK -> CRR core 6d057f5d8 + proofs bc88443ed + d637d644d; review row: "CRR fix" 8e5c64b3f
#   dropped: T2B, EYES, BELT, INF (no fix commit names them; no clear first fix commit)
# Each note is pinned to the commit the probe wrote it in (review 12: `git log -1` would take a later edit, e.g. CRK.md was edited by the
# CRR fix 8e5c64b3f itself, and feed post-fix knowledge into the score). Checked 2026-11-20: each is the note's first (and for the DEV
# notes its only) commit, older than the first fix commit.
CASES = [
    ('RFX', 'dev', [('docs/agents/s103/RFX.md', '6a91bae61'), ('docs/agents/s103/RFX_ADV.md', 'bad2c194b')], ['8b3c031c6'], ['ea92067d8'],
     'review commit is joint RFX / SVY'),
    ('SVD', 'dev', [('docs/agents/s103/SVD.md', '85b686de7')], ['c8aac66ef'], [], 'SVY, made by the RFX builder'),
    ('TRN4', 'dev', [('docs/agents/s103/TRN4.md', '6bdf63b5b')], ['444651748'], [], ''),
    ('AUS2', 'dev', [('docs/agents/s103/AUS2.md', '6b825a18b')], ['bb5b179b4'], ['0a9d5350d'], ''),
    ('HPR', 'sealed', [('docs/agents/s103/HPR.md', '8e9bef92e')], ['164757818'], [], 'HPF'),
    ('WMS', 'sealed', [('docs/agents/s103/WMS.md', '19adb72e4')], ['1a203e31a'], [], 'WMBD'),
    ('PEP', 'sealed', [('docs/research/s103/PEP.md', '376141770')], ['e76f4cbeb', 'cc4b57e97', '37f0edf51'], ['a2481bc6d'], 'DTP'),
    ('CRK', 'sealed', [('docs/research/s103/CRK.md', '376141770')], ['6d057f5d8', 'bc88443ed', 'd637d644d'], ['8e5c64b3f'], 'CRR'),
]
RULES = ('cite', 'head', 'unit', 'lane', 'readers', 'plants')


def note_commit(rel):
    return pack.git('log', '-1', '--format=%H', '--', rel).strip()


def hunks(parent, commit):
    """-> [(file, old_start, old_count)] for files that existed in parent; plus a list of excluded files with why"""
    out, excluded = [], []
    status = pack.git('diff', '--name-status', '--no-renames', parent, commit)
    added = set()
    for line in status.splitlines():
        parts = line.split('\t')
        if parts and parts[0].startswith('A'):
            added.add(parts[-1])
    diff = pack.git('diff', '-U0', '--no-color', '--no-renames', parent, commit)
    cur = None
    for line in diff.splitlines():
        if line.startswith('diff --git '):
            m = re.match(r'diff --git a/(.*) b/(.*)$', line)
            cur = m.group(1) if m else None
            if cur in added:
                excluded.append((cur, 'new file'))
                cur = None
            elif cur and (cur.startswith('docs/') or ('/' not in cur and cur.endswith('.md'))):
                excluded.append((cur, 'docs / notes'))
                cur = None
            continue
        if cur and line.startswith('@@'):
            m = re.match(r'@@ -(\d+)(?:,(\d+))? \+', line)
            a = int(m.group(1))
            n = int(m.group(2)) if m.group(2) is not None else 1
            out.append((cur, a, n))
    return out, excluded


def holds(b, f, a, n):
    if b.file != f:
        return False
    if n == 0:
        return b.start <= max(a, 1) and a + 1 <= b.end + (1 if a == 0 else 0)
    return b.start <= a and a + n - 1 <= b.end


def holds_any(blocks, f, a, n):
    """the hunk's old-side lines (an insertion: lines a and a+1) are all inside the union of the file's blocks: a hunk that spans two
    adjacent blocks is held (review 2, B8)"""
    need = [max(a, 1), a + 1] if n == 0 else list(range(a, a + n))
    bs = [b for b in blocks if b.file == f]
    return all(any(b.start <= x <= b.end for b in bs) for x in need) if bs else False


def rules_holding(blocks, f, a, n):
    """the rules whose blocks hold the hunk: one block alone, or (B8) the union of adjacent blocks; then every rule that contributes"""
    single = {getattr(b, 'rule', 'cite') for b in blocks if holds(b, f, a, n)}
    if single or not holds_any(blocks, f, a, n):
        return single
    need = [max(a, 1), a + 1] if n == 0 else list(range(a, a + n))
    return {getattr(b, 'rule', 'cite') for b in blocks if b.file == f and any(b.start <= x <= b.end for x in need)}


def miss_reason(p, notes_text, f, a, n):
    sites = [s for s in p.sites if s.file == f]
    base = f.rsplit('/', 1)[-1]
    if not sites:
        if any(b.file == f for b in p.blocks):
            return 'a companion holds the file, not these lines'
        if base in notes_text:
            return 'parser (the note names the file; no site came out)'
        return 'the note never cited it'
    lo, hi = a, a + max(n, 1) - 1
    if any(x <= hi + 1 and lo <= y + 1 for s in sites for x, y in s.gone_regions):
        return 'drift (the hunk is where a cited range was rewritten since the note: GONE)'
    return 'the note never cited these lines (it cites this file elsewhere)'


# Cap policy of the SCORE (round 3, B9): the score is measured on the pack with NO size cap (every block the rules make), so a change in
# rendered text never changes which companions drop and moves the score. The pack a builder gets (the default 120 KB cap) is measured
# beside it, with its headroom to the cap, and must stay under its cap.
SCORE_KB = 100000
CAP_KB = 120


def count(p, hs, text):
    got, misses, per_rule, only = 0, [], {}, {}
    for f, a, n in hs:
        rs = rules_holding(p.blocks, f, a, n)
        for r in rs:
            per_rule[r] = per_rule.get(r, 0) + 1
        if len(rs) == 1:
            r = next(iter(rs))
            only[r] = only.get(r, 0) + 1
        if rs:
            got += 1
        else:
            misses.append((f, a, n, miss_reason(p, text, f, a, n)))
    return got, misses, per_rule, only


def measure(name, notes, commit, use_q, bare, companions=True, drift=True, capped=True):
    text = '\n'.join(n.text for n in notes)
    parent = pack.git('rev-parse', commit + '^1').strip()
    target = pack.GitTree(parent)
    hs, excl = hunks(parent, commit)
    t0 = time.time()
    p = pack.build(notes, target, use_q=use_q, bare=bare, max_kb=SCORE_KB, companions=companions, drift=drift)
    secs = time.time() - t0
    got, misses, per_rule, only = count(p, hs, text)
    row = {'name': name, 'commit': commit, 'parent': parent[:9], 'pack': p, 'secs': secs, 'kb': len(p.md.encode('utf-8')) / 1024,
           'hunks': len(hs), 'inside': got, 'misses': misses, 'excluded': excl, 'per_rule': per_rule, 'only': only}
    if capped:
        t1 = time.time()
        pc = pack.build(notes, target, use_q=use_q, bare=bare, max_kb=CAP_KB, companions=companions, drift=drift)
        row.update({'cap_pack': pc, 'cap_secs': time.time() - t1, 'cap_kb': len(pc.md.encode('utf-8')) / 1024,
                    'cap_inside': count(pc, hs, text)[0]})
    return row


def notes_of(case):
    return [pack.note_at(rel, rev) for rel, rev in case[2]]


def run_set(cases, use_q, bare, companions=True, drift=True, capped=True):
    """-> (probe-led rows, review rows)"""
    led, rev = [], []
    for case in cases:
        name, _, _, probe, review, _ = case
        notes = notes_of(case)
        led += [measure(name, notes, c, use_q, bare, companions, drift, capped) for c in probe]
        rev += [measure(name, notes, c, use_q, bare, companions, drift, capped) for c in review]
    return led, rev


def score(rows):
    h = sum(r['hunks'] for r in rows)
    i = sum(r['inside'] for r in rows)
    return i, h, (i / h if h else 0.0)


def table(title, rows, fails):
    print(title)
    print(f'  score: the pack with no size cap; "@120" = the pack a builder gets (default cap {CAP_KB} KB), its KB and headroom to the cap')
    print(f'  {"pair":<6} {"fix":<10} {"target":<10} {"hunks":>5} {"inside":>6} {"share":>6} {"KB":>6} | {"@120":>5} {"KB":>6} {"room":>5} {"secs":>5}  sites/blocks')
    for r in rows:
        p = r['pack']
        share = r['inside'] / r['hunks'] if r['hunks'] else 1.0
        cap = (f'{r["cap_inside"]:>5} {r["cap_kb"]:>6.1f} {CAP_KB - r["cap_kb"]:>5.1f} {r["cap_secs"]:>5.1f}' if 'cap_pack' in r
               else f'{"":>5} {"":>6} {"":>5} {r["secs"]:>5.1f}')
        print(f'  {r["name"]:<6} {r["commit"][:9]:<10} {r["parent"]:<10} {r["hunks"]:>5} {r["inside"]:>6} {share:>6.0%} {r["kb"]:>6.1f} | {cap}  '
              f'{p.counts["sites"]}/{p.counts["blocks"]}')
        for k in ('secs', 'cap_secs'):
            if r.get(k, 0) >= MAX_SECS:
                fails.append(f'{r["name"]} {r["commit"][:9]}: pack took {r[k]:.1f} s (>= {MAX_SECS:.0f})')
        if 'cap_pack' in r and r['cap_pack'].over_cap:
            fails.append(f'{r["name"]} {r["commit"][:9]}: the builder\'s pack is over its size cap ({r["cap_kb"]:.1f} KB)')
    i, h, s = score(rows)
    kb = sum(r['kb'] for r in rows)
    ci = sum(r.get('cap_inside', 0) for r in rows)
    print(f'  {"TOTAL":<6} {"":<10} {"":<10} {h:>5} {i:>6} {s:>6.0%} {kb:>6.1f} | {ci:>5}  hits per KB {i / max(kb, 0.001):.2f} (no cap); '
          f'at the {CAP_KB} KB cap {ci} / {h} ({ci / max(h, 1):.0%})')
    print('  per rule (hunks held by a block of the rule / held ONLY by it / KB of its blocks / hits per KB):')
    for rule in RULES:
        hit = sum(r['per_rule'].get(rule, 0) for r in rows)
        only = sum(r['only'].get(rule, 0) for r in rows)
        rkb = sum(r['pack'].rule_bytes.get(rule, 0) for r in rows) / 1024
        print(f'    {rule:<8} {hit:>4} {only:>4} {rkb:>7.1f} KB  {hit / rkb if rkb else 0:.2f}')
    return s


def misses(rows, verbose):
    by = {}
    for r in rows:
        for f, s, n, why in r['misses']:
            by[why] = by.get(why, 0) + 1
            if verbose:
                print(f'    {r["name"]} {r["commit"][:9]} {f}:{s},{n} - {why}')
    for why, k in sorted(by.items(), key=lambda x: -x[1]):
        print(f'    misses by reason: {k:>3}  {why}')


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--set', default='all', choices=('dev', 'sealed', 'all'))
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--no-q', action='store_true', help='packs without the q context (the measure does not depend on it)')
    ap.add_argument('--floor', type=float, default=None, help='gate mode: a regression floor on the DEV probe-led score (not the target)')
    ap.add_argument('--pair', nargs='+', metavar='ARG',
                    help='NOTE[@REV][,NOTE2[@REV]] FIXCOMMIT [FIXCOMMIT ...]: score any note(s) against any fix (an audit of how complete a '
                         'probe\'s hand-back was); without @REV the note is read as git has it (an edited or uncommitted note: as written)')
    a = ap.parse_args(argv)
    use_q = not a.no_q
    fails = []
    if a.pair:
        if len(a.pair) < 2:
            ap.error('--pair needs a note and at least one fix commit')
        notes = []
        for spec in a.pair[0].split(','):
            path, _, rev = spec.partition('@')
            if rev:
                try:
                    rel = os.path.relpath(os.path.abspath(path), pack.ROOT).replace('\\', '/')
                except ValueError:
                    rel = path.replace('\\', '/')
                notes.append(pack.note_at(rel, rev))
            else:
                notes.append(pack.note_from_file(path))
        rows = [measure('pair', notes, c, use_q, True) for c in a.pair[1:]]
        print(f'pack_refind --pair: {", ".join(n.name + "@" + (n.rev[:9] if n.rev else "as written") for n in notes)} against {", ".join(a.pair[1:])}')
        table('the fix commits:', rows, fails)
        misses(rows, True)
        for f in fails:
            print(f'FAIL: {f}')
        return 1 if fails else 0
    sets = ['dev', 'sealed'] if a.set == 'all' else [a.set]
    print(f'pack_refind: target {TARGET:.0%} of probe-led hunks inside the pack (printed, not a gate); each pack < {MAX_SECS:.0f} s and under its cap')
    dev_score = None
    for st in sets:
        cases = [c for c in CASES if c[1] == st]
        led, rev = run_set(cases, use_q, True)
        print('')
        s = table(f'{st.upper()} probe-led commits (THE SCORE):', led, fails)
        misses(led, a.verbose)
        if rev:
            table(f'{st.upper()} review-fix commits (separate row, no bar):', rev, fails)
            misses(rev, a.verbose)
        base, _ = run_set(cases, False, True, companions=False, capped=False)
        bi, bh, bs = score(base)
        print(f'  {st.upper()} report only: the note\'s own sites alone (no companion rules): {bi} / {bh} ({bs:.0%})')
        if st == 'dev':
            dev_score = s
            pl, _ = run_set(cases, False, False, capped=False)
            pi, ph, ps = score(pl)
            print(f'  planted fault (bare-filename resolver off): {pi} / {ph} = {ps:.0%}  ({"drops: the check can fail" if ps < s else "DOES NOT DROP"})')
            if ps >= s:
                fails.append('planted fault: the DEV score did not drop with the bare-filename resolver off')
            # the drift mapper's planted fault is a selftest case (its DEV notes sit at their fix's parent: no drift to lose here);
            # report only: the review rows, whose targets moved since the note, with the mapping off
            if rev:
                _, dr = run_set(cases, False, True, drift=False, capped=False)
                di, dh, ds = score(dr)
                ri, rh, rs = score(rev)
                print(f'  report only: review rows with the drift mapping off {di} / {dh} ({ds:.0%}) vs on {ri} / {rh} ({rs:.0%})')
    print('')
    if dev_score is not None and a.floor is not None and dev_score < a.floor:
        fails.append(f'DEV probe-led score {dev_score:.0%} < regression floor {a.floor:.0%}')
    if fails:
        for f in fails:
            print(f'FAIL: {f}')
        print('pack_refind: FAIL')
        return 1
    print('pack_refind: PASS' + (f' (DEV probe-led {dev_score:.0%}' + (f' >= floor {a.floor:.0%}' if a.floor is not None else '')
                                  + (f'; below the {TARGET:.0%} target' if dev_score < TARGET else '') + ')' if dev_score is not None else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
