r"""q read curiosities [SYSTEM] [--limit N] [--low]: the one-line curiosities the readers (`fun:`) and the system-writers (`facts:`) stored
in read.s2.db (table fact_read), grouped by system. A lookup, not a document (S111: the generated category documents are gone, the curiosities stay).

    py tools/q/q.py read curiosities              every system, at most 40 lines
    py tools/q/q.py read curiosities pose         one system
    py tools/q/q.py read curiosities --limit 0    no cap
    py tools/q/q.py read curiosities --low        also the low-confidence ones (default: high and medium only)

Deduped by text (a writer's line, then the higher confidence, wins). Each line ends with its unit and who said it: a model's reading of a
tool-made fact sheet is a LEAD, not a fact. A line holding a network address or a licence key is never printed.
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qread  # noqa: E402

CONF_RANK = {'high': 0, 'medium': 1, 'low': 2}
NOT_SHOWN = re.compile(r'\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|licen[cs]e key', re.I)


def pick(facts, system=None, low=False):
    """facts: [(text, unit, conf, system, source)] -> {system: [(text, unit, conf, source)]}, deduped by text, systems in qread.SYSTEMS order"""
    best = {}
    for text, unit, conf, sy, source in facts:
        if conf not in CONF_RANK or (conf == 'low' and not low) or NOT_SHOWN.search(text):
            continue
        sy = sy or 'unknown'
        if system and sy != system:
            continue
        key = re.sub(r'[^a-z0-9]+', ' ', text.lower()).strip()
        rank = (0 if source == 'writer' else 1, CONF_RANK[conf], unit or 0, text)
        if key not in best or rank < best[key][0]:
            best[key] = (rank, text, unit, conf, sy, source)
    by = collections.defaultdict(list)
    for rank, text, unit, conf, sy, source in best.values():
        by[sy].append((rank, text, unit, conf, source))
    order = list(qread.SYSTEMS) + sorted(s for s in by if s not in qread.SYSTEMS)
    return collections.OrderedDict((sy, [(t, u, c, s) for _r, t, u, c, s in sorted(by[sy])]) for sy in order if sy in by)


def run(system=None, limit=40, low=False, path=None, out=print):
    """prints the lookup; returns the number of curiosities that exist for the question"""
    c = qread.open_ro(path)
    if c is None:
        out('nothing stored yet (no read.s2.db)')
        return 0
    facts = list(c.execute('select text,unit_id,confidence,system,source from fact_read order by id'))
    c.close()
    got = pick(facts, system, low)
    total = sum(len(v) for v in got.values())
    out('curiosities%s: %d (%s)' % (' of system ' + system if system else '', total, 'high, medium and low' if low else 'high and medium; --low adds the rest'))
    shown = 0
    for sy, fl in got.items():
        if limit and shown >= limit:
            break
        out('### %s' % sy)
        for text, unit, conf, source in fl:
            if limit and shown >= limit:
                break
            out('- %s (unit %s) [%s: %s]' % (re.sub(r'\s+', ' ', text).strip(), unit if unit is not None else '?', source, conf))
            shown += 1
    if shown < total:
        out('... %d more: `py tools\\q\\q.py read curiosities%s --limit 0` lists all' % (total - shown, ' ' + system if system else ''))
    return total


def main(rest):
    pos = [x for i, x in enumerate(rest) if not x.startswith('--') and (i == 0 or rest[i - 1] != '--limit')]
    system = pos[0] if pos else None
    if system and system not in qread.SYSTEMS:
        raise qread.ReadError('q read curiosities [SYSTEM] [--limit N] [--low]; SYSTEM is one of: %s' % ' | '.join(qread.SYSTEMS))
    limit = int(rest[rest.index('--limit') + 1]) if '--limit' in rest and rest.index('--limit') + 1 < len(rest) else 40
    run(system, limit, '--low' in rest)
    return 0
