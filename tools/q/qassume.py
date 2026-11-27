r"""q assume (S111 SWEEP1; docs\research\s111\SWEEP_PLAN.md step 1): where our code MAY hard-code a choice the game makes at run time.

    py tools/q/q.py assume [--kind A,B,..] [--in PATH] [--ref REF] [--json] [--all] [--limit N] [--out FILE] [--no-g]

A MECHANICAL list of candidate sites in src\renderer_scene, src\scene_live, src\renderer_adapter (and its split parts), src\renderer (renderer_parts),
src\assets and the src\*.h hooks. Not a verdict: step 2 of the sweep sorts each row (reads the game's answer / hard-codes a game choice / our own
design / unknown). The kinds (the patterns and their sub-kinds are in qassume_rules.py):
  A literal game-id / key tables      B count rules (a count vs a literal or a Need-style name)    C caller / return-address / game-address gates
  D keep-one / pick-one / first-match E assumption comments ([U] [L] until assume fitted guess never always)   F switch-gated forks (a cvar / env switch)
  G known diffs: the rows of `q seam --diffs` and the open rows of `q constdiff --diff`, folded in so there is ONE list
Each row: kind + sub-kind, file, the ENCLOSING NAME (function / type; a line is printed only as a convenience), the game addresses cited within 3 lines,
the seam / claims rows that cite the place (the `q seam --cites` index, by the function's span), the matched code.
--ref REF reads the tree (src and db) as it was at a git ref (the proof on known bugs: run it at the commit before a fix). --in PATH narrows the
files (path contains PATH). --out FILE writes the whole listing (text, or json with --json) and prints the totals; the default screen shows the
first --limit rows of each kind (rows with a game address and a seam row first) and the total, --all shows everything.
"""
import argparse
import bisect
import collections
import contextlib
import glob
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import qassume_rules as R  # noqa: E402
import qcap  # noqa: E402

KIND_NAMES = {
    'A': 'literal game-id / key tables', 'B': 'count rules', 'C': 'caller / address gates', 'D': 'keep-one / pick-one / first-match',
    'E': 'assumption comments', 'F': 'switch-gated forks', 'G': 'known diffs (seam --diffs, constdiff --diff)',
}
SCOPE_DIRS = ('src/renderer_scene', 'src/scene_live', 'src/renderer_adapter', 'src/renderer', 'src/assets')
EXTS = ('.h', '.hpp', '.cpp', '.inc')
SKIP_DIRS = {'shaders', 'spv', 'spirv', 'tests', 'test', '__pycache__'}
SKIP_NAME = re.compile(r'(?:_test|_selftest|_unit)\.(?:h|cpp)$|(?:^|/)test_')


class AssumeError(Exception):
    pass


def repo_root(root=None):
    return os.path.abspath(root) if root else os.path.abspath(os.path.join(HERE, '..', '..'))


def norm(p):
    return p.replace('\\', '/')


# ------------------------------------------------------------------------------------------------------------------ the tree

def in_scope_file(rel):
    rel = norm(rel)
    if SKIP_NAME.search(rel) or not rel.endswith(EXTS):
        return False
    if any(part in SKIP_DIRS for part in rel.split('/')[:-1]):
        return False
    if any(rel.startswith(d + '/') for d in SCOPE_DIRS):
        return True
    return rel.startswith('src/') and rel.count('/') == 1 and rel.endswith('.h')


def walk_files(root):
    out = []
    for top, dirs, files in os.walk(os.path.join(root, 'src')):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            rel = norm(os.path.relpath(os.path.join(top, f), root))
            if in_scope_file(rel) and (rel.count('/') > 1 or game_header(root, rel)):
                out.append(rel)
    return sorted(out)


GAME_MARK = re.compile(r'sub_[0-9A-Fa-f]{8}|0x8[23][0-9A-Fa-f]{6}')


def game_header(root, rel):
    """a src/*.h hook counts only when it names a game function / address somewhere (the app, audio, android, launcher headers do not)"""
    try:
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
            return bool(GAME_MARK.search(f.read()))
    except OSError:
        return False


def git(root, *args, binary=False):
    p = subprocess.run(['git', '-C', root] + list(args), capture_output=True, timeout=300)
    if p.returncode:
        raise AssumeError(f'git {" ".join(args[:3])}: {p.stderr.decode("utf-8", "replace").strip()[:200]}')
    return p.stdout if binary else p.stdout.decode('utf-8', 'replace')


def extract_ref(root, ref, dest):
    """the in-scope src files and db/seams + db/claims as they were at `ref`, extracted under dest"""
    if ref.startswith('-'):
        raise AssumeError(f'--ref {ref!r}: a REF starting with - would reach git as an option')
    listing = git(root, 'ls-tree', '-r', '--name-only', ref, '--', 'src', 'db/seams', 'db/claims').splitlines()
    tops = [f for f in listing if f.startswith('src/') and f.count('/') == 1 and f.endswith('.h')]
    dirs = [d for d in list(SCOPE_DIRS) + ['db/seams', 'db/claims'] if any(f.startswith(d + '/') for f in listing)]
    args = ['archive', '--format=tar', ref, '--'] + dirs + tops
    data = git(root, *args, binary=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        try:
            tf.extractall(dest, filter='data')
        except TypeError:
            tf.extractall(dest)


# ------------------------------------------------------------------------------------------------------------------ the seam index

CITE_RX = re.compile(r'([A-Za-z0-9_./\\-]+\.(?:h|hpp|cpp|inc))(?::(\d+)(?:-(\d+))?)?')
SEAM_TEXT = ('what', 'evidence', 'source', 'why')
CLAIM_TEXT = ('claim', 'evidence', 'note', 'source', 'ours', 'why')


def _load(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


class SeamIndex:
    """basename -> [(cited path, line a, line b, label)] over db/seams (ours rows + text cites) and db/claims (text cites): the `q seam --cites` rows, indexed once"""

    def __init__(self, root):
        self.by = collections.defaultdict(list)
        try:
            import qseam
        except ImportError:
            qseam = None
        for p in sorted(glob.glob(os.path.join(root, 'db', 'seams', '*.json'))):
            e = _load(p)
            if not isinstance(e, dict):
                continue
            topic = e.get('topic') or os.path.splitext(os.path.basename(p))[0]
            ours = e.get('ours') or []
            for o in ours:
                if isinstance(o, dict) and o.get('path'):
                    line = None
                    if qseam is not None:
                        try:
                            line = qseam.row_state(o, root).get('line')
                        except Exception:
                            pass
                    if line:
                        self._add(str(o['path']), line, line, topic)
            for side in ('game', 'ours'):
                for r in e.get(side) or []:
                    if isinstance(r, dict):
                        self._text(' '.join(str(r.get(k, '')) for k in SEAM_TEXT), topic)
            self._text(str(e.get('summary', '')) + ' ' + ' '.join(str(t) for t in (e.get('open') or [])), topic)
        for p in sorted(glob.glob(os.path.join(root, 'db', 'claims', '*.json'))):
            rows = _load(p)
            if isinstance(rows, list):
                base = os.path.splitext(os.path.basename(p))[0]
                for r in rows:
                    if isinstance(r, dict):
                        self._text(' '.join(str(r.get(k, '')) for k in CLAIM_TEXT), str(r.get('seam') or 'claims:' + base))

    def _add(self, path, a, b, label):
        p = norm(path).strip('/').lower()
        self.by[p.rsplit('/', 1)[-1]].append((p, a, b, label))

    def _text(self, text, label):
        for m in CITE_RX.finditer(text):
            a = int(m.group(2)) if m.group(2) else None
            b = int(m.group(3)) if m.group(3) else a
            self._add(m.group(1), a, b, label)

    def labels(self, rel, lo, hi):
        """the topics whose rows cite `rel` at a line in [lo, hi] (line-less cites are not a place)"""
        rel = norm(rel).lower()
        seen = []
        for p, a, b, label in self.by.get(rel.rsplit('/', 1)[-1], ()):
            if a is None or not (p == rel or rel.endswith('/' + p) or p.endswith('/' + rel)):
                continue
            if a <= hi and (b if b is not None else a) >= lo and label not in seen:
                seen.append(label)
        return seen


# ------------------------------------------------------------------------------------------------------------------ rows

def near_addrs(v, a, b):
    seen = []
    for k in range(max(1, a - 3), min(v.n(), b + 3) + 1):
        for m in R.ADDR_NEAR_RX.finditer(v.r(k)):
            s = m.group(0)
            s = 'sub_' + s[4:].upper() if s.startswith('sub_') else '0x' + s[2:].upper()
            if s not in seen:
                seen.append(s)
    return seen


def make_rows(root, rels, kinds, seam_index, note=None):
    import qsym
    srcs = []
    for rel in rels:
        srcs.append(R.View(qsym.Src(root, rel)))
    consts = R.addr_consts(srcs) if 'C' in kinds else set()
    rows = []
    for v in srcs:
        for h in R.scan(v, consts, kinds):
            name, how = v.scope_name(h['line'])
            span = v.func_span(h['line'])
            lo, hi = (span if span and span[1] - span[0] <= 80 else (h['line'] - 10, h['end'] + 10))
            lo, hi = min(lo, h['line']) - 4, max(hi, h['end']) + 4
            rows.append({
                'kind': h['kind'], 'sub': h['sub'], 'file': v.rel, 'name': name, 'how': how, 'sym': h['sym'], 'line': h['line'], 'end': h['end'],
                'addrs': near_addrs(v, h['line'], h['end']), 'seam': seam_index.labels(v.rel, lo, hi), 'text': h['text'], 'why': h['why'],
            })
    return rows


def g_rows(root, ref, no_constdiff, seam_index, views_cache):
    """kind G: the rows of `q seam --diffs` (at this root) and the open rows of `q constdiff --diff` (the working tree only)"""
    import qseam
    rows = []
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        qseam.main(['--diffs', '--json', '--root', root])
    try:
        sd = json.loads(buf.getvalue())
    except ValueError:
        sd = {'rows': []}
    import qsym

    def name_of(rel, line):
        if not rel or not os.path.exists(os.path.join(root, rel)) or not rel.endswith(EXTS):
            return '(file scope)', 'file'
        v = views_cache.get(rel)
        if v is None:
            v = views_cache[rel] = R.View(qsym.Src(root, rel))
        return v.scope_name(line or 1)
    for r in sd.get('rows', []):
        rel = norm(str(r.get('path', '')))
        line = r.get('line') or r.get('stored_line') or 1
        nm, how = name_of(rel, line)
        addrs = []
        for g in r.get('game') or []:
            a = str(g.get('addr', '')).strip()
            if a and a not in addrs:
                addrs.append(a)
        rows.append({'kind': 'G', 'sub': 'G1', 'file': rel, 'name': nm, 'how': how, 'sym': '', 'line': line, 'end': line, 'addrs': addrs[:4],
                     'seam': [r.get('topic')], 'text': f'[seam {r.get("topic")} {r.get("verdict")}, {r.get("state")}] ' + re.sub(r'\s+', ' ', str(r.get('why', '')))[:200],
                     'why': 'a known diff: a seam row says ours departs from the game'})
    if ref is None and not no_constdiff:
        try:
            p = subprocess.run([sys.executable, os.path.join(HERE, 'q.py'), 'constdiff', '--diff', '--json'], capture_output=True, text=True, encoding='utf-8',
                               errors='replace', timeout=300, cwd=root)
            cd = json.loads(p.stdout) if p.returncode == 0 else {'rows': []}
        except (OSError, ValueError, subprocess.SubprocessError):
            cd = {'rows': []}
        groups = collections.OrderedDict()
        for r in cd.get('rows', []):
            if not r.get('diff'):
                continue
            at = (r.get('ours_at') or ['-'])[0]
            m = re.match(r'(?:(\w+)\s+)?(\S+?):(\d+)$', at)
            rel, line = (norm(m.group(2)), int(m.group(3))) if m else ('', 0)
            key = (rel, line, r.get('field'), r.get('resolver'), r.get('ours'))
            g = groups.setdefault(key, {'effects': [], 'game': [], 'row': r})
            g['effects'].append(f'{r.get("effect")}.{r.get("stage")}.c{r.get("reg")}')
            for gr in r.get('game_rows') or []:
                for k in ('site', 'row'):
                    if gr.get(k) and gr[k] not in g['game']:
                        g['game'].append(gr[k])
        for (rel, line, field, res, ours), g in groups.items():
            nm, how = name_of(rel, line) if rel else ('(constdiff: no source site)', 'file')
            eff = g['effects']
            rows.append({'kind': 'G', 'sub': 'G2', 'file': rel or '(none)', 'name': nm, 'how': how, 'sym': str(field or ''), 'line': line, 'end': line, 'addrs': g['game'][:4],
                         'seam': ['constdiff'], 'text': f'[constdiff] {field}: ours {ours} ({res}) vs game {g["row"].get("game")} in {len(eff)} inputs: ' + ', '.join(eff[:3]) + (' ...' if len(eff) > 3 else ''),
                         'why': 'a known diff: a shader input ours feeds differently from the game'})
    return rows


# ------------------------------------------------------------------------------------------------------------------ the listing

def strength(r):
    return (0 if r['addrs'] and r['seam'] else 1 if r['addrs'] else 2 if r['seam'] else 3, r['sub'] if r['kind'] == 'E' else '')


def totals(rows):
    t = {}
    for k in R.ALL_KINDS:
        sel = [r for r in rows if r['kind'] == k]
        subs = collections.Counter(r['sub'] for r in sel)
        t[k] = {'n': len(sel), 'subs': dict(sorted(subs.items())), 'files': len({r['file'] for r in sel})}
    return t


def row_line(r):
    ident = r['name'] + (('::' + r['sym']) if r['sym'] and r['how'] in ('file', 'type', 'next') and r['sym'] not in r['name'] else '')
    ad = ' [' + ' '.join(r['addrs'][:3]) + (' +%d' % (len(r['addrs']) - 3) if len(r['addrs']) > 3 else '') + ']' if r['addrs'] else ''
    sm = ' {seam: ' + ', '.join(str(x) for x in r['seam'][:4]) + (' +%d' % (len(r['seam']) - 4) if len(r['seam']) > 4 else '') + '}' if r['seam'] else ''
    return f'  {r["sub"]:<3} {ident}  (line {r["line"]}){ad}{sm}  {r["text"][:170]}'


def print_rows(rows, out, per_file=True):
    by = collections.defaultdict(list)
    for r in rows:
        by[r['file']].append(r)
    for f in sorted(by):
        rs = sorted(by[f], key=lambda r: (r['kind'], r['line'], r['sub']))
        out.write(f'== {f}  ({len(rs)} rows: ' + ' '.join(f'{k}={n}' for k, n in sorted(collections.Counter(r['kind'] for r in rs).items())) + ')\n')
        cur = None
        for r in rs:
            if r['kind'] != cur:
                cur = r['kind']
                out.write(f' {cur} {KIND_NAMES[cur]}\n')
            out.write(row_line(r) + '\n')


def summary_lines(rows, files, ref, total_note=''):
    t = totals(rows)
    ls = [f'q assume: {len(rows)} candidate rows in {files} files' + (f' at {ref}' if ref else '') + total_note]
    for k in R.ALL_KINDS:
        if t[k]['n']:
            subs = ' '.join(f'{s}={n}' for s, n in t[k]['subs'].items())
            ls.append(f'  {k} {KIND_NAMES[k]}: {t[k]["n"]} rows in {t[k]["files"]} files   ({subs})')
    return ls


def run(argv, root=None):
    ap = argparse.ArgumentParser(prog='q assume', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kind', action='append', default=[], help='A..G, comma separated or repeated (default all)')
    ap.add_argument('--in', dest='in_path', default=None, help='only files whose path contains PATH')
    ap.add_argument('--ref', default=None, help='read the tree (src, db) at this git ref instead of the working tree')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--all', action='store_true', help='every row on screen (default: --limit per kind)')
    ap.add_argument('--limit', type=int, default=25, help='rows per kind on screen (default 25)')
    ap.add_argument('--out', default=None, help='write the whole listing (text, or json with --json) to FILE; print the totals')
    ap.add_argument('--no-g', action='store_true', help='leave out kind G (seam --diffs / constdiff --diff)')
    ap.add_argument('--root', default=None, help='answer for another checkout (default: the one holding this tool)')
    a = ap.parse_args(argv)
    kinds = set()
    for k in a.kind:
        for c in k.upper().replace(' ', '').split(','):
            if c and c not in R.ALL_KINDS:
                print(f'q assume: unknown kind {c!r} (A..G)', file=sys.stderr)
                return 2
            kinds.add(c)
    kinds = kinds or set(R.ALL_KINDS)
    if a.no_g:
        kinds.discard('G')
    root = repo_root(a.root or root)
    tmp = None
    try:
        if a.ref:
            tmp = tempfile.mkdtemp(prefix='qassume_')
            extract_ref(root, a.ref, tmp)
            tree = tmp
        else:
            tree = root
        rels = [r for r in walk_files(tree) if qcap.in_scope(r, a.in_path)]
        sidx = SeamIndex(tree)
        rows = make_rows(tree, rels, kinds, sidx)
        if 'G' in kinds:
            rows += [r for r in g_rows(tree, a.ref, False, sidx, {}) if qcap.in_scope(r['file'], a.in_path)]
    except AssumeError as e:
        print(f'q assume: {e}', file=sys.stderr)
        return 2
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    rows.sort(key=lambda r: (r['file'], r['kind'], r['line'], r['sub']))
    files = len({r['file'] for r in rows})
    if a.json:
        doc = {'ref': a.ref, 'files_scanned': len(rels), 'totals': totals(rows), 'rows': rows}
        if a.out:
            with open(a.out, 'w', encoding='utf-8') as f:
                json.dump(doc, f, indent=1)
            print('\n'.join(summary_lines(rows, files, a.ref)) + f'\nwrote {a.out}')
        else:
            print(json.dumps(doc, indent=1))
        return 0
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write('\n'.join(summary_lines(rows, files, a.ref)) + '\n\n')
            print_rows(rows, f)
        print('\n'.join(summary_lines(rows, files, a.ref)) + f'\nwrote {a.out}')
        return 0
    print('\n'.join(summary_lines(rows, files, a.ref)))
    shown = []
    cut = {}
    for k in R.ALL_KINDS:
        sel = sorted((r for r in rows if r['kind'] == k), key=lambda r: (strength(r), r['file'], r['line']))
        s, c = qcap.cap(sel, a.limit, a.all)
        shown += s
        if c:
            cut[k] = (len(s), len(sel))
    print()
    print_rows(shown, sys.stdout)
    for k, (n, tot) in cut.items():
        print(f'  {k}: ' + qcap.trim_line(n, tot, 'rows', f'q assume --kind {k} --all', narrow=True))
    return 0


def main(argv=None, root=None):
    return run(sys.argv[1:] if argv is None else argv, root)


if __name__ == '__main__':
    sys.exit(main())
