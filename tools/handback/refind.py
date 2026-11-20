#!/usr/bin/env python3
r"""Re-find of real Session 103 slips with the hand-back check (lane HBK acceptance bar). 0 game runs.
  py -I tools\handback\refind.py [--diff-only] [--keep-tree] [--root DIR]

Each case runs tools\handback\check.py with the slip commit as the tree and its parent (or the named base) as the base, in ONE temporary detached worktree
(<root>\out\hbk_refind\wt) handled by tools\handback\tree.py: the exact `worktree <path>` line must be listed (after `git worktree prune`), the folder must be
its own top level before every checkout, HEAD is verified after it; removed at the end with `git worktree remove` (never --force).
check f compiles the old tree with tools\gate\game_syntax.py --changed (the main checkout's generated\ and configured flags; game_syntax itself runs up to 8
clang parses; this script never runs two f.compile cases at once).
  1 7a085561d  CPB2 core deleted 13 files by accident                         -> a.deleted FAIL
  2 5658a7c3a  RGP: a missing ';' in the OFF-only block of start_settings.cpp -> f.compile FAIL (OFF)       [compile]
  3 ab72e865f  ENV broke the ON game build (front_end.cpp, start_settings.cpp) -> f.compile FAIL (ON)       [compile]
  4 c6c875418  SCS read a cvar defined only in the OFF build                  -> b.cvar_builds FAIL
  5 ef2188694  the TRF2 merge reverted 4059fb473's census-line fix            -> a.stale_base FAIL (base 4059fb473)
  6 94b04db0b  ARM2: the unit handed kDesc to SlotOwner, skipping the hook    -> e.units FAIL (SlotOwner's only production call site is in the hook)
A case is FAIL-found (the check FAILS, naming what it should) or NOTE-found (only a NOTE names it: does not count, a NOTE never stops a hand-back).
Ordinary S103 lane commits (a31eeaf64 HGF, b5ea2bd47 SMX review, 934639450 RML) must give no FAIL and no BLOCKED; checks a-e timed with a cold base cache
(their cache files deleted first) and a warm one (the same run again).
Planted fault: the delete rule switched off INSIDE a.deleted (check.RULES['deleted'] = False): re-find 1 MUST go missing.
--diff-only: cases 2 and 3 are not run and f is left out everywhere (the gate's cheap tier).
Bar: >= 5 of 6 FAIL-found (all 4 diff-only cases with --diff-only), the ordinary commits clean, the plant missing. Exit 0 = bar met."""
import argparse
import glob
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check as hb   # noqa: E402
import tree as _tree   # noqa: E402

CASES = [
    (1, '7a085561d', None, 'a.deleted', 'CPB2 core: 13 files deleted', False, None),
    (2, '5658a7c3a', None, 'f.compile', 'RGP: missing ";" in the OFF block', True, '[off]'),
    (3, 'ab72e865f', None, 'f.compile', 'ENV: ON game build broken', True, '[on]'),
    (4, 'c6c875418', None, 'b.cvar_builds', 'SCS: cvar defined only in OFF', False, 'wwe2k14_rdr_scene2d'),
    (5, 'ef2188694', '4059fb473', 'a.stale_base', 'TRF2 merge reverted a census fix', False, '4059fb473'),
    (6, '94b04db0b', None, 'e.units', 'ARM2 unit skipped the hook', False, 'SlotOwner'),
]
ORDINARY = [('a31eeaf64', 'HGF'), ('b5ea2bd47', 'SMX review'), ('934639450', 'RML')]


class Args:
    def __init__(self, skip_compile):
        self.allow_delete, self.skip_compile = [], skip_compile


def run_case(root, wt, tree_sha, base, names, skip_compile):
    _tree.checkout(root, wt, tree_sha)   # refuses unless wt is exactly a registered worktree, its own top level
    b = hb.git(wt, 'rev-parse', base or (tree_sha + '^')).strip()
    res, summ = hb.run_checks(wt, b, Args(skip_compile), names, out=lambda *_a: None)
    return {r.name: r for r in res}, summ, b


def judge(r, want):
    """('FAIL-found' | 'NOTE-found' | 'MISSING', why)."""
    if r is None:
        return 'MISSING', 'not run'
    text = r.reason + ' ' + ' '.join(r.details)
    if r.verdict == 'FAIL' and (want is None or want in text):
        return 'FAIL-found', r.reason
    if want and any(want in n for n in r.notes):
        return 'NOTE-found', next(n for n in r.notes if want in n)
    return 'MISSING', '%s: %s' % (r.verdict, r.reason)


def drop_cache(base):
    for p in glob.glob(os.path.join(hb.CACHE_DIR, base[:12] + '_*.json')):
        os.remove(p)


def main():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser()
    ap.add_argument('--diff-only', action='store_true')
    ap.add_argument('--keep-tree', action='store_true')
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    wt = os.path.join(root, 'out', 'hbk_refind', 'wt')
    for ln in _tree.sweep(root, hb.BASE_DIR):   # base trees of dead runs
        print(ln)
    t_all = time.time()
    rows, ord_rows, plant_missing = [], [], False
    try:
        for cid, sha, base, name, what, needs_compile, want in CASES:
            if needs_compile and a.diff_only:
                rows.append((cid, sha, name, 'not run (--diff-only)', '', 0.0))
                continue
            t0 = time.time()
            v, _s, _b = run_case(root, wt, sha, base, [name], skip_compile=False)
            if want and want.startswith('['):
                got, why = judge(v.get(name), None)
                if got == 'FAIL-found' and want not in v[name].reason + ' '.join(v[name].details):
                    got, why = 'MISSING', 'failed, but not in the %s build: %s' % (want, why)
            else:
                got, why = judge(v.get(name), want)
            rows.append((cid, sha, name, got, why, time.time() - t0))
        hb.RULES['deleted'] = False   # planted: one rule inside a.deleted switched off
        try:
            v, _s, _b = run_case(root, wt, CASES[0][1], None, ['a.deleted'], skip_compile=True)
        finally:
            hb.RULES['deleted'] = True
        plant_missing = judge(v.get('a.deleted'), None)[0] == 'MISSING'
        names = [n for n in hb.ORDER if not (a.diff_only and n.startswith('f.'))]
        for sha, lane in ORDINARY:
            drop_cache(hb.git(root, 'rev-parse', sha + '^').strip())
            v, s, _b = run_case(root, wt, sha, None, names, skip_compile=a.diff_only)                       # cold base cache
            v2, s2, _b = run_case(root, wt, sha, None, [n for n in names if n != 'f.compile'], skip_compile=True)   # warm
            fb = ['%s %s: %s' % (r.name, r.verdict, r.reason[:120]) for r in v.values() if r.verdict in ('FAIL', 'BLOCKED')]
            ord_rows.append((sha, lane, s['secs_a_e'], s2['secs_a_e'], s['secs_f'], fb))
    finally:
        if not a.keep_tree:
            why = _tree.remove(root, wt)
            if why:
                print('note: the temporary worktree was not removed (%s)' % why)
    print('%-3s %-10s %-14s %-11s %6s  %s' % ('#', 'slip', 'check', 'result', 'secs', 'why'))
    for cid, sha, name, got, why, secs in rows:
        print('%-3d %-10s %-14s %-11s %6.1f  %s' % (cid, sha, name, got, secs, why[:150]))
    n_fail = sum(1 for r in rows if r[3] == 'FAIL-found')
    n_note = sum(1 for r in rows if r[3] == 'NOTE-found')
    n_run = sum(1 for r in rows if not r[3].startswith('not run'))
    need = 4 if a.diff_only else 5
    print('FAIL-found: %d of %d run; NOTE-found (not counted): %d; bar >= %d FAIL-found' % (n_fail, n_run, n_note, need))
    print('planted (the delete rule switched off inside a.deleted): re-find 1 %s' % ('goes missing, as planted' if plant_missing else 'is STILL found: the test cannot fail'))
    ok_ord = True
    for sha, lane, cold, warm, f, fb in ord_rows:
        state = 'clean' if not fb and cold < 60 and warm < 60 else 'NOT CLEAN'
        ok_ord &= state == 'clean'
        print('ordinary %s (%s): a-e cold %.1f s, warm %.1f s, f %.1f s: %s%s' % (sha, lane, cold, warm, f, state, ('; ' + ' | '.join(fb)) if fb else ''))
    met = n_fail >= need and plant_missing and ok_ord
    print('BAR %s (%.0f s)' % ('MET' if met else 'NOT MET', time.time() - t_all))
    return 0 if met else 1


if __name__ == '__main__':
    sys.exit(main())
