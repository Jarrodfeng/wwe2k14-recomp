"""qnote (S105 QC): one section of a research note or system page, not the file.

    q note                               list the notes (docs/research/**, docs/systems/*.md, docs/formats/*.md): path, lines
    q note NAME                          the headings of one note with line spans (NAME = file stem, a unique substring, or a path)
    q note NAME --verdict                the verdict: the section headed Verdict / Summary / Answer / Result / TL;DR (else the text before the first `##`)
    q note NAME --sites                  the `## Sites the build will touch` section (the probe contract)
    q note NAME --section HEADING        every section whose heading contains HEADING (case-insensitive), to the next heading of the same or higher level
    q note NAME --max N                  cap on printed lines per section (default 120)

Headings inside ``` fences are not headings. The note is read from this checkout, then from the main checkout (a note written there is not in a worktree yet).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
VERDICT_WORDS = ('verdict', 'summary', 'answer', 'result', 'tl;dr', 'tldr', 'bottom line', 'conclusion')


class NoteError(Exception):
    pass


def roots():
    out = [ROOT]
    if os.path.normcase(MAIN) != os.path.normcase(ROOT) and os.path.isdir(MAIN):
        out.append(MAIN)
    return out


def list_notes(root):
    out = []
    for top, deep in (('docs/research', True), ('docs/systems', False), ('docs/formats', False)):
        base = os.path.join(root, *top.split('/'))
        for dp, dn, fn in os.walk(base):
            for f in fn:
                if f.endswith('.md'):
                    out.append(os.path.relpath(os.path.join(dp, f), root).replace('\\', '/'))
            if not deep:
                break
    return sorted(out)


def _prefer_main(root, rel):
    """notes are written in the main checkout: when both copies exist and differ, read main's (QC M4); -> root to read"""
    if os.path.normcase(root) == os.path.normcase(MAIN):
        return root
    mp, wp = os.path.join(MAIN, rel), os.path.join(root, rel)
    try:
        if os.path.isfile(mp) and os.path.isfile(wp):
            with open(mp, 'rb') as a, open(wp, 'rb') as b:
                if a.read() != b.read():
                    return MAIN
    except OSError:
        pass
    return root


def find_note(name):
    n = name.replace('\\', '/').strip()
    base = n if n.endswith('.md') else n + '.md'
    for root in roots():
        p = os.path.join(root, n)
        if os.path.isfile(p):
            rel = os.path.relpath(p, root).replace('\\', '/')
            return _prefer_main(root, rel), rel
    for root in roots():
        notes = list_notes(root)
        exact = [x for x in notes if x.lower().endswith('/' + base.lower()) or x.lower() == base.lower()]
        if len(exact) == 1:
            return _prefer_main(root, exact[0]), exact[0]
        if len(exact) > 1:
            raise NoteError(f'{name}: ambiguous: ' + ', '.join(exact[:8]))
        sub = [x for x in notes if n.lower() in x.lower()]
        if len(sub) == 1:
            return _prefer_main(root, sub[0]), sub[0]
        if len(sub) > 1:
            raise NoteError(f'{name}: several notes match: ' + ', '.join(sub[:8]))
    raise NoteError(f'{name}: no note under docs/research, docs/systems or docs/formats (q note, no argument, lists them)')


def headings(lines):
    """[(level, text, line_no)] outside code fences"""
    out = []
    fence = False
    for i, t in enumerate(lines, 1):
        if t.lstrip().startswith('```'):
            fence = not fence
            continue
        if fence:
            continue
        m = re.match(r'^(#{1,6})\s+(.*?)\s*#*\s*$', t)
        if m:
            out.append((len(m.group(1)), m.group(2), i))
    return out


def span(hs, k, total):
    """(start, end) lines of heading k: to the next heading of the same or a higher level"""
    lvl = hs[k][0]
    end = total
    for j in range(k + 1, len(hs)):
        if hs[j][0] <= lvl:
            end = hs[j][2] - 1
            break
    return hs[k][2], end


def emit(lines, a, b, cap, out):
    for n in range(a, min(b, a + cap - 1) + 1):
        out.append(f'{n}\t{lines[n - 1]}')
    if b - a + 1 > cap:
        out.append(f'... ({b - a + 1 - cap} more lines; q show FILE:{a + cap}-{b} or --max N)')


def where(root, rel):
    """which copy was read, always said (QC M4)"""
    same = os.path.normcase(ROOT) == os.path.normcase(MAIN)
    if os.path.normcase(root) == os.path.normcase(MAIN) and not same:
        return (f'[main checkout {MAIN}; the worktree copy differs]' if os.path.isfile(os.path.join(ROOT, rel)) else f'[main checkout {MAIN}; no worktree copy]')
    return f'[{"main" if same else "this worktree"} checkout {root}]'


def answer(name, mode=None, arg=None, cap=120):
    root, rel = find_note(name)
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    hs = headings(lines)
    out = []
    wh = where(root, rel)
    if mode is None:
        out.append(f'== {rel}  ({len(lines)} lines)  {wh}')
        for k, (lvl, tx, ln) in enumerate(hs):
            a, b = span(hs, k, len(lines))
            out.append(f'{"  " * (lvl - 1)}{"#" * lvl} {tx}   {a}-{b}')
        if not hs:
            out.append('(no headings)')
        return out
    cut = []
    if mode == 'sites':
        want = [k for k, h in enumerate(hs) if h[1].lower().startswith('sites the build will touch')]
        if not want:
            raise NoteError(f'{rel}: no `## Sites the build will touch` section (a survey note has no sites to hand a builder); headings: ' + '; '.join(h[1] for h in hs[:12]))
    elif mode == 'verdict':
        vh = [k for k, h in enumerate(hs) if h[0] > 1 and h[1].lower().lstrip('0123456789. ').startswith(VERDICT_WORDS)]
        vh.sort(key=lambda k: (VERDICT_WORDS.index(next(w for w in VERDICT_WORDS if hs[k][1].lower().lstrip('0123456789. ').startswith(w))), k))   # `Verdict` outranks an earlier `Summary of ...` (QC L1)
        want = vh[:1]
        if not want:
            first = hs[1][2] - 1 if len(hs) > 1 and hs[0][0] == 1 else (hs[0][2] - 1 if hs else min(len(lines), 15))
            out.append(f'== {rel}:1-{first}  (no Verdict heading: the text before the first section)  {wh}')
            emit(lines, 1, max(first, 1), cap, out)
            return out
    else:
        key = (arg or '').lower()
        want = [k for k, h in enumerate(hs) if key and key in h[1].lower()]
        if not want:
            raise NoteError(f'{rel}: no heading contains "{arg}"; headings: ' + '; '.join(h[1] for h in hs[:20]))
        keep = []
        for k in want:        # drop a sub-heading that also matches and lies inside an earlier hit's span
            if not any(j in keep and span(hs, j, len(lines))[0] < hs[k][2] <= span(hs, j, len(lines))[1] for j in want):
                keep.append(k)
        cut = keep[6:]
        want = keep[:6]
    for k in want:
        a, b = span(hs, k, len(lines))
        out.append(f'== {rel}:{a}-{b}  {hs[k][1]}  {wh}')
        emit(lines, a, b, cap, out)
    if cut:
        out.append(f'... {len(cut)} more sections match and are NOT shown: ' + '; '.join(f'{hs[k][1]} ({hs[k][2]})' for k in cut[:8]) + ' (use a longer --section heading)')
    return out


def main(argv):
    args = list(argv)
    mode = arg = None
    cap = 120
    name = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--verdict':
            mode = 'verdict'
        elif a == '--sites':
            mode = 'sites'
        elif a == '--section' and i + 1 < len(args):
            mode, arg = 'section', args[i + 1]
            i += 1
        elif a == '--max' and i + 1 < len(args):
            try:
                cap = max(1, int(args[i + 1]))
            except ValueError:
                print(f'q note: --max needs a number, got "{args[i + 1]}"', file=sys.stderr)
                return 2
            i += 1
        elif a in ('-h', '--help'):
            print(__doc__)
            return 0
        elif name is None:
            name = a
        else:
            name = name + ' ' + a
        i += 1
    try:
        if name is None:
            root = roots()[0]
            for r in list_notes(root):
                try:
                    with open(os.path.join(root, r), encoding='utf-8', errors='replace') as f:
                        n = f.read().count('\n') + 1
                except OSError:
                    n = 0
                print(f'{r}  ({n} lines)')
            return 0
        print('\n'.join(answer(name, mode, arg, cap)))
        return 0
    except NoteError as e:
        print(f'q note: {e}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
