r"""S111 CDCHECKALL: the check.py rows of the code-diff extractors beside `codediff` (render state): levels (q codediff --field levels, tools\q\qlevels.py), targetfmt
(q targetfmt, tools\q\qtargetfmt.py), draworder (q draworder, tools\q\qdraworder.py). (q fieldsdiff is a LEAD tool by design: no row.)

A row FAILS when the tree holds a DIFF / GAME-ONLY (draworder: DIFF) row that the BASE tree does not hold (new_failing: the same key failing at the base is a known
difference, not a finding). Keys: levels (our function, game function, the list's game address); targetfmt (role, field); draworder (family a, family b).
UNSETTLED / OURS-ONLY / DEPARTED / OK never fail. Reading both sides is cheap when the tree has no failing row at all: the base is read only when it does.
Each extractor reads our code through the same reader the q verb uses (the working tree; the base through git), the game side from the q index. 0 game runs.

COLLECT[name](ref) -> [{'key', 'verdict', 'text'}] (ref None = the working tree); the selftest swaps these for planted rows. The gates (check_gates.py) read INPUTS."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
QDIR = os.path.join(REPO, 'tools', 'q')
if QDIR not in sys.path:
    sys.path.append(QDIR)

FAILING = {'levels': ('DIFF', 'GAME-ONLY'), 'targetfmt': ('DIFF', 'GAME-ONLY'), 'draworder': ('DIFF',)}


def collect_levels(ref=None):
    import qlevels
    rows = qlevels.collect(ref)[0]
    return [{'key': (r['fn'], r['game'], (r['list'] or {}).get('at')), 'verdict': r['verdict'],
             'text': f"{r['verdict']} {r['fn']} vs {r['game']} levels: {r['why']} (ours @{', '.join(o['at'] for o in r['ours']) or 'no directory'})"} for r in rows]


def collect_targetfmt(ref=None):
    import q as qmod
    import qcodediff
    import qostate
    import qtargetfmt
    read = qostate.default_reader(ref)
    sites = qtargetfmt.default_sites(qmod.connect(qmod.DB))
    rows = qtargetfmt.build(sites, read, marks=qcodediff.Marks(read))[0]
    return [{'key': (r['role'], r['field']), 'verdict': r['verdict'],
             'text': f"{r['verdict']} {r['role']}.{r['field']}: game {' | '.join(r['game_values']) or '-'}; ours {' | '.join(r['ours_values']) or '-'}"
                     + (f" ({r['detail']})" if r['detail'] else '')} for r in rows]


def collect_draworder(ref=None):
    import qdraworder as D
    if ref:
        read = D.git_reader(ref)
        doc = read(D.DOC) or ''      # the doc of the same tree: a doc edit that makes a DIFF must not hide behind itself
    else:
        read = D.disk_reader()
        with open(os.path.join(D.ROOT, D.DOC), encoding='utf-8') as fh:
            doc = fh.read()
    rows = D.run(read, doc)['rows']
    return [{'key': (r['a'], r['b']), 'verdict': r['order'],
             'text': f"order {r['order']} {r['a']} | {r['b']}: {r['why']} (game first: {D.first_text(r)[0]}; ours first: {D.first_text(r)[1]}; ours {r['ours_a']} | {r['ours_b']})"}
            for r in rows]


COLLECT = {'levels': collect_levels, 'targetfmt': collect_targetfmt, 'draworder': collect_draworder}


def new_failing(name, now, was):
    old = {r['key'] for r in was if r['verdict'] in FAILING[name]}
    return [r for r in now if r['verdict'] in FAILING[name] and r['key'] not in old]


def check(name, base):
    """-> (failure lines, rows judged, what)"""
    now = COLLECT[name](None)
    bad = [r for r in now if r['verdict'] in FAILING[name]]
    if not bad:
        return [], len(now), f'{len(now)} row(s), none {"/".join(FAILING[name])}'
    if not base:
        return [f"{name}: {r['text']}" for r in bad], len(now), 'no base: every failing row counts'
    was = COLLECT[name](base)
    new = new_failing(name, now, was)
    fails = [f"{name} NEW at {base[:10]}: {r['text']}" for r in new]
    return fails, len(now), f'{len(now)} row(s), {len(bad)} {"/".join(FAILING[name])} ({len(bad) - len(new)} already at the base {base[:10]})'


# ---------------------------------------------------------------------------------------------------------------------------- the gates' input lists
def inputs(name):
    """-> (exact paths, path prefixes) the extractor reads from the tree, besides every src code file (levels) -- read from the tools' own constants"""
    tools = ('tools/q/qcodediff.py', 'tools/q/qostate.py', 'tools/q/q.py')
    if name == 'levels':
        return tools + ('tools/q/qlevels.py', 'tools/q/qdecomp.py', 'tools/q/qdecomp_phi.py'), ()
    if name == 'targetfmt':
        import qtargetfmt
        files = {f for r in qtargetfmt.ROLES for f, _fn, _var in r['ours']}
        return tools + ('tools/q/qtargetfmt.py', 'tools/q/qconstdiff.py', 'tools/q/qargs.py') + tuple(sorted(files)), (qtargetfmt.FX_DIR + '/',)
    import qdraworder as D
    return ('tools/q/qdraworder.py', 'tools/q/qwhen.py', D.DOC, D.RECORD, D.BODY_ORDER, D.CLASSES, D.CROWS, D.SEE, D.ATTACH, 'src/renderer_scene/frame_hooks.h'), ()
