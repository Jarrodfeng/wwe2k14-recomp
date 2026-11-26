"""qshow: read exact lines of one of OUR files through q (S105 QB). Builders spent ~40 % of their raw reads on `sed -n N,Mp FILE`.

    py tools/q/q.py show src/foo.h:120-160          lines 120 to 160, numbered
    py tools/q/q.py show foo.h:120+-10              lines 110 to 130 (a unique basename works, whole path components)
    py tools/q/q.py show foo.h:120                  line 120 with 12 lines either side
    py tools/q/q.py show foo.h:40-60,200-210        several ranges of one file
    py tools/q/q.py show a.h:10-20 b.py:5-9         several files
    py tools/q/q.py show foo.h:120-160 --root DIR   another checkout / worktree
    py tools/q/q.py show foo.h:120-160 --ref REF    the same lines as they were at a git ref (branch, tag, commit; a file that is gone now works)

Default root: the git checkout of the WORKING DIRECTORY (a worktree reads its own files), else this script's checkout. The header names the root when
it is not the checkout this script lives in.
Our own text: src, tools, probes, android, config, docs, assets, the top-level files, and out\\ / logs\\ text by path. Never game_data\\ or generated\\
(decided on the realpath, in any spelling: absolute, ./, .., case, symlink): the game's code is `py tools\\q\\q.py decomp sub_XXXXXXXX`.
A range past the end of the file is said, never clamped silently. At most 400 lines per range.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOPS = ('src', 'tools', 'probes', 'android', 'config', 'docs', 'assets', 'patches', 'db')
BANNED = ('game_data', 'generated')
MAX_LINES = 400
DEFAULT_CTX = 12
SPEC = re.compile(r'^(?P<file>.+?):(?P<rng>\d+(?:(?:-|\+-)\d+)?(?:,\d+(?:(?:-|\+-)\d+)?)*)$')


class ShowError(Exception):
    pass


def checkout_of_cwd():
    """the git checkout the caller stands in (a worktree's own files), else this script's checkout"""
    try:
        r = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True, timeout=20)
        top = r.stdout.strip()
        if r.returncode == 0 and top and os.path.isdir(os.path.join(top, 'tools', 'q')):
            return os.path.abspath(top)
    except Exception:
        pass
    return ROOT


def refuse_if_game(root, path):
    """game_data / generated are never read through q show. Decided on the REALPATH under the root with the first component lower-cased, so an
    absolute path, ./generated/x, src/../generated/x, Generated/x and a symlink all land on it. -> the path relative to the root"""
    rr = os.path.realpath(root)
    rp = os.path.realpath(path)
    try:
        rel = os.path.relpath(rp, rr).replace('\\', '/')
    except ValueError:
        raise ShowError(f'{path}: not under the checkout {root}')
    if rel == '..' or rel.startswith('../'):
        raise ShowError(f'{path}: outside the checkout {root}')
    first = rel.split('/')[0].lower()
    if first in BANNED:
        raise ShowError(f'{path}: {first}\\ is never read through q show (game data / generated game code); the game\'s functions: `py tools\\q\\q.py decomp sub_XXXXXXXX`')
    return rel


def find_file(root, name):
    """-> repo-relative path (forward slashes). name = a repo-relative path, an absolute path inside root, or a unique basename (whole path components)"""
    n = name.replace('\\', '/')
    cand = n if os.path.isabs(n) else os.path.join(root, n)
    rel = refuse_if_game(root, cand)       # first: a path that lands in game data is refused whatever else is true of it
    if os.path.isfile(cand):
        return rel
    base = n.split('/')[-1]
    hits = []
    for top in TOPS:
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in ('__pycache__', 'node_modules', '.git') and not d.startswith('.')]
            if base in fn:
                r2 = os.path.relpath(os.path.join(dp, base), root).replace('\\', '/')
                if r2 == n or r2.endswith('/' + n):
                    hits.append(r2)
    if len(hits) == 1:
        return hits[0]
    if not hits:
        raise ShowError(f'{name}: no such file under {", ".join(TOPS)}')
    raise ShowError(f'{name}: ambiguous: ' + ', '.join(hits[:6]) + (' ...' if len(hits) > 6 else ''))


def parse_ranges(rng, nlines):
    out = []
    for part in rng.split(','):
        if '+-' in part:
            c, k = (int(x) for x in part.split('+-'))
            out.append((max(1, c - k), max(1, c + k)))
        elif '-' in part:
            a, b = (int(x) for x in part.split('-'))
            a, b = (a, b) if a <= b else (b, a)
            out.append((max(1, a), max(1, b)))
        else:
            c = int(part)
            out.append((max(1, c - DEFAULT_CTX), max(1, c + DEFAULT_CTX)))
    return out


def read_lines(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        text = f.read()
    return text.replace('\r\n', '\n').replace('\r', '\n').split('\n')


def enclosing_name(rel, lines, ln):
    """the nearest def / class line above ln (python), the nearest column-0 function head (C++), or ''"""
    if rel.endswith('.py'):
        rx = re.compile(r'^(\s*)(?:async\s+)?(?:def|class)\s+(\w+)')
        for i in range(min(ln, len(lines)) - 1, -1, -1):
            m = rx.match(lines[i])
            if m and (len(m.group(1)) == 0 or i + 1 < ln):
                return f'{m.group(2)} (line {i + 1})'
        return ''
    if rel.endswith(('.h', '.hpp', '.cpp', '.inl', '.cc', '.inc')):
        rx = re.compile(r'^\s{0,4}[\w:<>,~\*&\s]+?\b([\w:~]+)\s*\([^;]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:\{|$)')
        for i in range(min(ln, len(lines)) - 1, -1, -1):
            l = lines[i]
            if l and not l[0].isspace() and not l.startswith(('#', '//', '}')) and '(' in l:
                m = rx.match(l)
                if m and m.group(1) not in ('if', 'for', 'while', 'switch', 'return'):
                    return f'{m.group(1)} (line {i + 1})'
        return ''
    return ''


def ref_file(root, ref, name):
    """S111 QGAPS: -> (repo-relative path, text) of a file at a git ref. name = a repo-relative path or a unique basename (searched in the ref's tree)"""
    if ref.startswith('-'):
        raise ShowError(f'--ref {ref!r}: a REF starting with - would reach git as an option')
    n = name.replace('\\', '/')
    rel = refuse_if_game(root, n if os.path.isabs(n) else os.path.join(root, n))

    def cat(path):
        p = subprocess.run(['git', '-C', root, 'show', f'{ref}:{path}'], capture_output=True, timeout=60)
        return p.stdout.decode('utf-8', 'replace') if p.returncode == 0 else None
    t = cat(rel)
    if t is not None:
        return rel, t
    ls = subprocess.run(['git', '-C', root, 'ls-tree', '-r', '--name-only', ref, '--'] + list(TOPS), capture_output=True, text=True, timeout=60)
    if ls.returncode:
        raise ShowError(f'--ref {ref}: git cannot read that revision: {ls.stderr.strip()[:200]}')
    hits = [x for x in ls.stdout.splitlines() if x == n or x.endswith('/' + n)]
    if len(hits) == 1:
        return hits[0], cat(hits[0]) or ''
    if not hits:
        raise ShowError(f'{name}: no such file at {ref} under {", ".join(TOPS)}')
    raise ShowError(f'{name}: ambiguous at {ref}: ' + ', '.join(hits[:6]) + (' ...' if len(hits) > 6 else ''))


def cmd_show(root, specs, ref=None):
    res = []
    for sp in specs:
        m = SPEC.match(sp.strip())
        if not m:
            raise ShowError(f'{sp}: give FILE:N, FILE:N-M, FILE:N+-C or FILE:N-M,P-Q')
        if ref:
            rel, text = ref_file(root, ref, m.group('file'))
            lines = text.replace('\r\n', '\n').replace('\r', '\n').split('\n')
        else:
            rel = find_file(root, m.group('file'))
            lines = read_lines(os.path.join(root, rel))
        if lines and lines[-1] == '':
            lines = lines[:-1]
        for a, b in parse_ranges(m.group('rng'), len(lines)):
            note = ''
            if a > len(lines):
                res.append({'file': rel, 'a': a, 'b': b, 'n': len(lines), 'rows': [], 'note': f'beyond the end: the file has {len(lines)} lines (not clamped)', 'func': ''})
                continue
            if b > len(lines):
                note = f'the file ends at line {len(lines)} (asked for {b})'
                b = len(lines)
            if b - a + 1 > MAX_LINES:
                note = (note + '; ' if note else '') + f'capped at {MAX_LINES} lines (asked {a}-{b}): show the next range'
                b = a + MAX_LINES - 1
            res.append({'file': rel, 'a': a, 'b': b, 'n': len(lines), 'rows': [(i, lines[i - 1]) for i in range(a, b + 1)], 'note': note,
                        'func': enclosing_name(rel, lines, a)})
    return res


def print_show(res, root=None):
    if root and os.path.realpath(root) != os.path.realpath(ROOT):
        print(f'-- root: {root} (not the checkout of this q.py: {ROOT})')
    for r in res:
        print(f'== {r["file"]}:{r["a"]}-{r["b"]}' + (f' @ {r["ref"]}' if r.get('ref') else '') + f'  ({r["n"]} lines in the file)' + (f'  nearest def above: {r["func"]}' if r['func'] else '') + (f'  [{r["note"]}]' if r['note'] else ''))
        w = len(str(r['b']))
        for i, l in r['rows']:
            print(f'{i:>{w}}  {l}')


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(errors='replace')      # a legacy console must not crash on a character of the file (review 7)
        except Exception:
            pass
    root = None
    ref = None
    if '--root' in argv:
        k = argv.index('--root')
        root = os.path.abspath(argv[k + 1])
        del argv[k:k + 2]
    if '--ref' in argv:
        k = argv.index('--ref')
        if k + 1 >= len(argv):
            print('q show: --ref needs a git ref', file=sys.stderr)
            return 2
        ref = argv[k + 1]
        del argv[k:k + 2]
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    root = root or checkout_of_cwd()
    try:
        res = cmd_show(root, argv, ref)
        for r_ in res:
            r_['ref'] = ref
        print_show(res, root)
    except ShowError as e:
        print(f'q show: {e}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
