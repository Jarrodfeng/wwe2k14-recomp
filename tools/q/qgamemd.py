r"""q rendermd, one document per CATEGORY of the game (S110 lane GAMEMD).

    py tools/q/q.py rendermd [--doc NAME] [--out PATH] [--check]

  no argument   write EVERY document under docs/, one line per file (bytes, systems, clusters); a file whose only difference is the date
                is left alone ("unchanged")
  --doc NAME    write one document only (RENDERER, MATCH, ... with or without .md)
  --out PATH    where to write a single document (default RENDERER.md, or the --doc one)
  --check       write nothing; exit 1 when a file on disk differs from what would be generated (the date line is ignored)

The generator of the sections is tools/q/qrendermd.py (render() takes one document's entry from here). This file holds the map
"which reader system lands in which document" and the loop. RENDERER.md holds the renderer only: it alone has the feed bar, the
"seen writing our read sites" notes and the unread structs. Every other document has Part 1 (how its systems work), Part 2 (fun facts,
one per unit) and Part 3 (read clusters of its systems, listing needed, name conflicts, wrong-name flags).
"""
import argparse
import collections
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# ---------------------------------------------------------------- THE MAP: one line per document. A new reader system (e.g. `creation`)
# is added by one line here (`'CREATION.md': 'creation'`); a system named in no line makes the tool fail.
DOCUMENTS = collections.OrderedDict([
    ('RENDERER.md', 'pose model material arena crowd light particle camera screens liquids ui'),
    ('MATCH.md', 'match ai'),
    ('MENUS.md', 'menus'),
    ('CREATION.md', 'creation'),
    ('AUDIO.md', 'audio'),
    ('ENGINE.md', 'engine'),
    ('UNSORTED.md', 'other unknown'),       # also: a unit whose reading states no system ('')
])
TITLES = {
    'RENDERER.md': 'How the game builds what gets drawn',
    'MATCH.md': 'How the game runs a match',
    'MENUS.md': 'How the game builds its menus',
    'CREATION.md': 'How the creation suite works',
    'AUDIO.md': 'How the game makes sound',
    'ENGINE.md': 'The engine under everything',
    'UNSORTED.md': 'Readings that fit no category yet',
}
RENDERER_DOC = 'RENDERER.md'     # the only document with the renderer-only sections
UNSORTED_DOC = 'UNSORTED.md'     # also takes units with no system
MAX_BYTES = 60000                # printed per document, never enforced


class MapError(SystemExit):
    pass


def doc_of_system(system, docs=DOCUMENTS):
    """-> document file name for a reader system; '' (no system stated) -> UNSORTED; a system in no document (or two) FAILS"""
    if not system:
        return UNSORTED_DOC
    hit = [n for n, s in docs.items() if system in s.split()]
    if len(hit) != 1:
        raise MapError('q rendermd: system %r is in %d documents (tools/q/qgamemd.py DOCUMENTS): add it to exactly one' % (system, len(hit)))
    return hit[0]


def make_doc(name, docs=DOCUMENTS):
    systems = tuple(docs[name].split())
    return {'file': name, 'name': name[:-3], 'title': TITLES.get(name, name[:-3]), 'systems': systems,
            'renderer': name == RENDERER_DOC, 'blank': name == UNSORTED_DOC}


def validate(d, docs=DOCUMENTS):
    """every system known to the reader (qread.SYSTEMS) and every system found in the data lands in exactly one document"""
    import qread
    for sy in qread.SYSTEMS:
        doc_of_system(sy, docs)
    for a in d['answers'].values():
        doc_of_system(a['system'], docs)
    for f in d['facts']:
        doc_of_system(f[3], docs)
    for sy in d['sysstate']:
        doc_of_system(sy, docs)


def doc_units(d, doc, docs=DOCUMENTS):
    """{unit: answer} of the read units whose newest reading belongs in this document"""
    return {u: a for u, a in d['answers'].items() if doc_of_system(a['system'], docs) == doc['file']}


def conflict_unit(text):
    m = re.match(r'unit (\d+):', text)
    return int(m.group(1)) if m else None


def other_part3(d, doc, units):
    """Part 3 of a document that is not the renderer: read clusters of its systems, listing needed, conflicts, wrong-name flags"""
    import qrendermd as R
    o = ['## Part 3: Reference', '']
    o.append('Bookkeeping for the readings of this category (the renderer\'s feed bar and read-site notes are in RENDERER.md).')
    o.append('')
    o.append('### Read clusters by system')
    o.append('')
    by = collections.defaultdict(list)
    for u, a in units.items():
        by[a['system'] or 'no system stated'].append((u, a))
    for sy in list(doc['systems']) + ['no system stated']:
        cls = by.get(sy, [])
        if not cls:
            continue
        cls.sort(key=lambda t: (R.CONF_RANK.get(t[1]['confidence'], 3), t[0]))
        o.append('#### %s' % sy)
        o.append('%d read clusters; best-rated first. [reader: lead]' % len(cls))
        for u, a in cls[:R.CAP_READ_PER_SYSTEM]:
            o.append('- unit %d: %s %s' % (u, R.one_line(a['purpose']), R.mark_reader(a['confidence'])))
        if len(cls) > R.CAP_READ_PER_SYSTEM:
            o.append('- not shown: %d more read clusters of this system (cap %d); `py tools\\q\\q.py read show UNIT` has each, `py tools\\q\\q.py read system %s` all of them' %
                     (len(cls) - R.CAP_READ_PER_SYSTEM, R.CAP_READ_PER_SYSTEM, sy))
        o.append('')
    if not units:
        o.append('- no cluster of this category has a stored reading yet.')
        o.append('')
    o.append('### Not known yet')
    o.append('')
    lst = sorted(u for u, a in units.items() if a['listing'])
    o.append('Readings that said they need the full listing (%d of %d stored): %s%s. %s' %
             (len(lst), len(units), ' '.join(str(u) for u in lst[:R.CAP_LISTING]) or 'none',
              (' ... (+%d, cap %d)' % (len(lst) - R.CAP_LISTING, R.CAP_LISTING)) if len(lst) > R.CAP_LISTING else '', '[reader: lead]'))
    cf = [t for t in d['conflicts'] if conflict_unit(t) in units]
    o.append('Name conflicts on read clusters (our name against the game\'s own share no word): %d.' % len(cf))
    for t in cf[:R.CAP_CONFLICTS]:
        o.append('- %s [game]' % R.one_line(t))
    if len(cf) > R.CAP_CONFLICTS:
        o.append('- not shown: %d more (cap %d)' % (len(cf) - R.CAP_CONFLICTS, R.CAP_CONFLICTS))
    wr = [w for w in d.get('wrong', []) if w[3] in units]
    o.append('Names a reader flagged as wrong (a lead, not a fact): %d.' % len(wr))
    for func, name, why, unit in wr[:R.CAP_CONFLICTS]:
        o.append('- unit %s: sub_%08X is named `%s`: %s [reader: lead]' % (unit, func, name, R.one_line(why)))
    if len(wr) > R.CAP_CONFLICTS:
        o.append('- not shown: %d more (cap %d)' % (len(wr) - R.CAP_CONFLICTS, R.CAP_CONFLICTS))
    o.append('')
    return o


def doc_clusters(d, doc, docs=DOCUMENTS):
    return len(doc_units(d, doc, docs))


def main(argv):
    import qrendermd as R
    ap = argparse.ArgumentParser(prog='q rendermd', description='write the category documents under docs/ from the q index')
    ap.add_argument('--doc', default=None, help='one document only (name with or without .md)')
    ap.add_argument('--out', default=None, help='where to write a single document')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args(argv)
    names = list(DOCUMENTS)
    if a.doc or a.out:
        want = (a.doc or RENDERER_DOC)
        want = want if want.lower().endswith('.md') else want + '.md'
        hit = [n for n in names if n.lower() == want.lower()]
        if not hit:
            print('q rendermd: no document %s (have: %s)' % (a.doc, ', '.join(names)))
            return 2
        names = hit
    d = R.real_data()
    validate(d)
    now = time.strftime('%Y-%m-%d %H:%M')
    rc = 0
    for n in names:
        doc = make_doc(n)
        path = a.out if (a.out and len(names) == 1) else os.path.join(os.path.dirname(R.DEFAULT_OUT), n)
        text = R.render(d, now, doc) + '\n'
        size = len(text.encode('utf-8'))
        old = open(path, encoding='utf-8', newline='').read() if os.path.exists(path) else None
        if a.check:
            if old is None or not R.same_but_date(old, text):
                print('q rendermd --check: %s is STALE; run `py tools\\q\\q.py rendermd`' % path)
                rc = 1
            else:
                print('q rendermd --check: %s is current' % path)
            continue
        info = '%d bytes (cap %d, not enforced); systems: %s; clusters: %d' % (size, MAX_BYTES, ' '.join(doc['systems']), doc_clusters(d, doc))
        if old is not None and R.same_but_date(old, text):
            print('unchanged %s: %s' % (path, info))
            continue
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
        print('wrote %s: %s' % (path, info))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
