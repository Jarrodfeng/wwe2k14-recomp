r"""Gate tiers (S103 GST, Jarrod 2026-11-18 21:08: "The gate is CHEAP checks"; CLAUDE.md, docs\DECISIONS.md last line).

The per-piece gate runs builds, both-build compiles, lints, units / selftests and the lane's OWN proof; the wide rig / picture / GPU proofs leave it (a milestone runs them).
Every check has a `tier`: "cheap" | "rig". A check (or its lane json, top level) may DECLARE "tier"; else it is derived (derive()):
  1. takes a GPU slot (footprint gpu: a rig render, fview, rplay)            -> rig
  2. its name's tail says build / compile / syntax / link / lint / selftest / unit / twin -> cheap
  3. needs a capture (a `needs` path under out/captures, out/s101, out/cases_in or a case zip) -> rig
  4. its name's tail or command says rig / capture / proof / replay / picture / owned_check / regress / sweep / parity / e2e / scenario -> rig
  5. anything else                                                      -> cheap
`gate.py --tier cheap|all` (default all: a milestone, gate_main.py and the selftests; gate_wait.py passes `cheap` for --only unless told otherwise). A check NAMED in --only (lane.check) always
runs, whatever its tier: that is how a lane's own rig proof stays in its builder's gate. A lane named in --only runs its cheap checks. The gate line ends with `rig skipped: N`.
Selftest: sched_selftest.py (TIER cases)."""
import re

CHEAP_NAME = re.compile(r'(?:^|[._-])(?:build|builds|compile|compiles|syntax|link|lint|lints|selftest|self_test|unit|units|twin|twins|check_lint)(?:$|[._-])', re.I)
RIG_NAME = re.compile(r'(?:^|[._-])(?:rig|capture|captures|proof|proofs|replay|picture|pictures|owned_check|regress|sweep|parity|e2e|scenario|scenarios|census|frames)(?:$|[._-])', re.I)
CAPTURE_NEED = ('out/captures', 'out/s101', 'out/cases_in', 'out/cases')


def tail(name):
    return name.split('.', 1)[-1]


def derive(c, gpu=False):
    """-> 'cheap' | 'rig' for one check dict (`gpu` = the footprint's GPU inference / declaration)."""
    d = c.get('tier')
    if d in ('cheap', 'rig'):
        return d
    if gpu or c.get('gpu') is True:
        return 'rig'
    t = tail(c.get('name', ''))
    if CHEAP_NAME.search(t):
        return 'cheap'
    for n in c.get('needs', []) or []:
        n = str(n).replace('\\', '/')
        if n.startswith(CAPTURE_NEED) or n.endswith('.zip'):
            return 'rig'
    if RIG_NAME.search(t):
        return 'rig'
    return 'cheap'


def split(checks, tier, only, gpu_of):
    """-> (kept, skipped). tier 'all' keeps everything. Else a check NAMED in `only` is kept whatever its tier, and so is every cheap check; a rig-tier check selected only by its lane /
    --changed / the whole gate is skipped. gpu_of(c) -> bool is called only for a check with no declared tier."""
    if tier == 'all':
        return list(checks), []
    kept, skipped = [], []
    for c in checks:
        if c['name'] in only:
            kept.append(c)
            continue
        t = derive(c, False if c.get('tier') in ('cheap', 'rig') else bool(gpu_of(c)))
        (kept if t == 'cheap' else skipped).append(c)
    return kept, skipped
