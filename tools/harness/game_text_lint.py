#!/usr/bin/env python3
r"""Rule 1 text lint (lane S105-R1L, 2026-11-20): the game's own text must never appear in a tracked file (tools\ ships to the public copy).

  py -I tools\harness\game_text_lint.py --all | --staged | --changed BASE | FILE... [--selftest] [--rebuild]

Builds ONCE (cached, never committed: <main>\out\harness\index\game_text_set.json, keyed by the size + mtime of the player's own files) the set of
the game's text from the PLAYER'S files on this PC: every string.pac entry of every language (+ the newest DLC string pack) and every /CSDT caption
of evtdata.pac. Strings are normalised (case-fold; every run of non letter/digit characters = one space) and kept only at >= 5 words or >= 24
characters (short menu labels such as SELECT ARENA are ids we may name). A file is normalised the same way and searched for any kept string as a
word-sequence substring. Scope: --staged (index blobs added / changed), --changed BASE (files changed since BASE, working tree), --all (every tracked
file), or the files named. Exit 1 on a hit; 2 when the game data is missing (cannot run). It prints file:line, the string's id and its first 3
words, never the text.

Scope: only PUBLISHED files fail (tools\release\publish_github.ps1 leaves notes, docs\, db\, STATUS ... out; the regexes below copy its
$exclude list); hits in local-only files are counted on one NOTE line (--local lists them, --strict makes them fail too).
  --tree REV scans a git tree (publish_github's final check: the tree about to be committed).
Allow list: tools\harness\game_text_allow.txt, lines `path-glob | id-or-* | reason` (the reason is mandatory). 0 game runs."""
import bisect
import fnmatch
import gzip
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'q'))
import qgx   # noqa: E402

MIN_WORDS, MIN_CHARS = 5, 24
MAX_FILE = 3 * 1024 * 1024
SKIP_EXT = ('.png', '.jpg', '.jpeg', '.ico', '.svg', '.bin', '.dll', '.exe', '.zip', '.gz', '.apk', '.jar', '.spv', '.ttf', '.otf', '.woff', '.pdf', '.rec', '.csv', '.so', '.a', '.lib', '.obj', '.pyc', '.7z', '.mp4', '.gif')
LOCAL_ONLY = [re.compile(x) for x in (r'^CLAUDE\.md$', r'^PLAYBOOK\.md$', r'^MORNING_REPORT\.md$', r'^SESSION[^/]*\.md$', r'^docs/(?!release/)', r'^\.claude/',
                                      r'^\.mcp\.json$', r'^db/(?!ids/)', r'^tools/labelling/prompts/', r'^tools/release/release\.ps1$', r'^[^/]+_PLAN\.md$',
                                      r'^[^/]+_prompt\.md$', r'^STATUS\.md$', r'^[^/]+_ARC\.md$', r'^tools/q/map/', r'^docs/release/(S\d+_[^/]*|[^/]*_draft)\.md$')]
SELF = ('tools/harness/game_text_lint.py', 'tools/harness/game_text_allow.txt')
ALLOW_FILE = os.path.join(HERE, 'game_text_allow.txt')
TOK = re.compile(r'[^\W_]+', re.UNICODE)


def norm_words(s):
    return TOK.findall(s.casefold())


def keep(words):
    return len(words) >= MIN_WORDS or len(' '.join(words)) >= MIN_CHARS


# ------------------------------------------------------------------------------------------------------------------------- the set
def _cache_path():
    main = os.path.dirname(os.path.dirname(qgx._game_root()))   # <checkout>\game_data\extracted -> <checkout>
    return os.path.join(main, 'out', 'harness', 'index', 'game_text_set.json.gz')


def _sources():
    root = qgx._game_root()
    src = [p for _l, p in qgx.string_pacs()] + [qgx.evtdata_pac()]
    return [p for p in src if os.path.exists(p)]


def _stamp(paths):
    return [[os.path.basename(p), os.path.getsize(p), int(os.path.getmtime(p))] for p in paths]


def build_set():
    """-> [[id_label, 'normalised text'], ...] from every string table entry (all languages) and every /CSDT record."""
    pac = qgx._wm30_pac()
    out, seen = [], set()

    def add(label, text):
        w = norm_words(text)
        if keep(w):
            t = ' '.join(w)
            if t not in seen:
                seen.add(t)
                out.append([label, t])

    for label, path in qgx.string_pacs():
        if not os.path.exists(path):
            continue
        data = pac.load(path)
        for nm, st, sz in qgx.epac_dir(data):
            try:
                leaves = list(pac.walk(qgx.epac_entry(data, st, sz)))
            except Exception:
                continue
            for _p, leaf in leaves:
                for tid, text in qgx.parse_text_table(leaf).items():
                    add('%s:%s:%d' % (label, nm, tid), text)
    ev = qgx.evtdata_pac()
    if os.path.exists(ev):
        data = pac.load(ev)
        for nm, st, sz in qgx.epac_dir(data):
            if not re.fullmatch(r'[A-Z]\d{3}', nm):
                continue
            try:
                leaves = list(pac.walk(qgx.epac_entry(data, st, sz)))
                recs = qgx.csdt_records(leaves[0][1])
            except Exception:
                continue
            for r in recs:
                add('csdt:%s:key%d' % (nm, r['key']), r['text'])
    return out


def load_set(rebuild=False):
    srcs = _sources()
    if not srcs:
        raise qgx.GxError('no game string files')
    cp = _cache_path()
    stamp = _stamp(srcs)
    if not rebuild and os.path.exists(cp):
        try:
            with gzip.open(cp, 'rt', encoding='utf-8') as f:
                d = json.load(f)
            if d.get('stamp') == stamp and d.get('v') == 1:
                return d['set']
        except Exception:
            pass
    s = build_set()
    os.makedirs(os.path.dirname(cp), exist_ok=True)
    tmp = cp + '.tmp'
    with gzip.open(tmp, 'wt', encoding='utf-8') as f:
        json.dump({'v': 1, 'stamp': stamp, 'set': s}, f)
    os.replace(tmp, cp)
    return s


class Index:
    def __init__(self, items):
        self.k2, self.k1 = {}, {}
        for label, text in items:
            w = text.split(' ')
            if len(w) >= 2:
                self.k2.setdefault((w[0], w[1]), []).append((w, label))
            else:
                self.k1.setdefault(w[0], []).append((w, label))

    def find(self, text):
        """[(line, label, first3)] for every kept string found in text (word-sequence match)."""
        toks = [(m.group(0), m.start()) for m in TOK.finditer(text.casefold())]
        words = [t for t, _ in toks]
        nl = [m.start() for m in re.finditer('\n', text)]
        hits = []
        for i, w in enumerate(words):
            cands = []
            if i + 1 < len(words):
                cands += self.k2.get((w, words[i + 1]), [])
            cands += self.k1.get(w, [])
            for sw, label in cands:
                if words[i:i + len(sw)] == sw:
                    hits.append((bisect.bisect_right(nl, toks[i][1] - 1) + 1, label, ' '.join(sw[:3])))
        return hits


# ------------------------------------------------------------------------------------------------------------------------- scope
def load_allow():
    rows = []
    if os.path.exists(ALLOW_FILE):
        for ln in open(ALLOW_FILE, encoding='utf-8'):
            ln = ln.strip()
            if not ln or ln.startswith('#'):
                continue
            p = [x.strip() for x in ln.split('|')]
            if len(p) < 3 or not p[2]:
                raise SystemExit('game_text_allow.txt: a row needs `path | id-or-* | reason`: %r' % ln)
            rows.append((p[0], p[1]))
    return rows


def allowed(rows, path, label):
    return any(fnmatch.fnmatch(path, g) and (i == '*' or i == label) for g, i in rows)


def is_local(path):
    return any(r.match(path) for r in LOCAL_ONLY)


def git(*a, root=REPO, binary=False):
    r = subprocess.run(['git', '-C', root] + list(a), capture_output=True, timeout=300)
    if r.returncode:
        raise SystemExit('git %s failed: %s' % (' '.join(a), r.stderr.decode('utf-8', 'replace')[:200]))
    return r.stdout if binary else r.stdout.decode('utf-8', 'replace')


def files_for(args, root):
    """[(path, bytes)]"""
    out = []
    if args.get('staged'):
        for p in git('diff', '--cached', '--name-only', '--diff-filter=AM', '-z', root=root).split('\0'):
            if p:
                out.append((p, git('show', ':' + p, root=root, binary=True)))
        return out
    if args.get('tree'):
        names = [p for p in git('ls-tree', '-r', '--name-only', '-z', args['tree'], root=root).split('\0') if p]
        want = [p for p in names if not is_local(p) and not p.lower().endswith(SKIP_EXT)]
        pr = subprocess.run(['git', '-C', root, 'cat-file', '--batch'], input=''.join('%s:%s\n' % (args['tree'], p) for p in want).encode(), capture_output=True, timeout=900)
        buf, pos = pr.stdout, 0
        for p in want:
            nl = buf.index(b'\n', pos)
            head = buf[pos:nl].split()
            if len(head) < 3:   # "<name> missing"
                pos = nl + 1
                continue
            n = int(head[2])
            out.append((p, buf[nl + 1:nl + 1 + n]))
            pos = nl + 1 + n + 1
        return out
    if args.get('changed'):
        names = set(git('diff', '--name-only', '--diff-filter=AM', '-z', args['changed'], root=root).split('\0'))
        names |= set(git('ls-files', '--others', '--exclude-standard', '-z', root=root).split('\0'))
        names.discard('')
    elif args.get('all'):
        names = set(git('ls-files', '-z', root=root).split('\0')) - {''}
    else:
        names = set(args.get('files', []))
    for p in sorted(names):
        fp = p if os.path.isabs(p) else os.path.join(root, p)
        if os.path.isfile(fp):
            with open(fp, 'rb') as f:
                out.append((p.replace('\\', '/'), f.read(MAX_FILE + 1)))
    return out


def scan(idx, files, allow):
    found = []
    for p, b in files:
        pn = p.replace('\\', '/')
        if pn in SELF or pn.lower().endswith(SKIP_EXT) or len(b) > MAX_FILE or b'\0' in b[:4096]:
            continue
        seen = set()
        for line, label, first3 in idx.find(b.decode('utf-8', 'replace')):
            if allowed(allow, pn, label) or (line, label) in seen:
                continue
            seen.add((line, label))
            found.append('%s:%d: game text %s ("%s ...")' % (pn, line, label, first3))
    return found


def selftest(idx, items):
    """Planted: a caption line and a string-table line built from the player's data now, in a temp file -> caught; a clean file -> not;
    an allow row -> excused; a short label -> ignored."""
    cap = next((x for x in items if x[0].startswith('csdt:') and len(x[1].split()) >= 8), None)
    stab = next((x for x in items if x[0].startswith('string') and len(x[1].split()) >= 6), None)
    bad = 0
    if not cap or not stab:
        print('selftest: BLOCKED no caption / string of the needed size in the set')
        return 2
    d = tempfile.mkdtemp(prefix='gtl_')
    try:
        for tag, (label, text) in (('caption', cap), ('string', stab)):
            body = 'x = 1\n# note\n"' + text.upper().replace(' ', '  ') + '"\n'
            hit = scan(idx, [('planted_%s.txt' % tag, body.encode())], [])
            ok = any(label in h for h in hit) and all(':3:' in h and text not in h for h in hit)
            print('selftest planted %s: %s' % (tag, 'caught' if ok else 'MISSED %r' % hit))
            bad += not ok
            ok2 = not scan(idx, [('planted_%s.txt' % tag, body.encode())], [('planted_*', '*')])
            print('selftest allow row for %s: %s' % (tag, 'excused' if ok2 else 'NOT excused'))
            bad += not ok2
        clean = scan(idx, [('clean.txt', b'def f():\n    return "SELECT ARENA"\n')], [])
        print('selftest clean file: %s' % ('no hit' if not clean else 'FALSE HIT %r' % clean))
        bad += bool(clean)
    finally:
        import shutil
        shutil.rmtree(d, ignore_errors=True)
    return 1 if bad else 0


def main(argv):
    args, files, rebuild, st = {}, [], False, False
    it = iter(argv)
    for a in it:
        if a == '--all':
            args['all'] = True
        elif a == '--staged':
            args['staged'] = True
        elif a == '--changed':
            args['changed'] = next(it)
        elif a == '--tree':
            args['tree'] = next(it)
        elif a == '--local':
            args['local'] = True
        elif a == '--strict':
            args['strict'] = True
        elif a == '--rebuild':
            rebuild = True
        elif a == '--selftest':
            st = True
        elif a == '--root':
            args['root'] = next(it)
        elif a in ('-h', '--help'):
            print(__doc__)
            return 0
        else:
            files.append(a)
    args['files'] = files
    try:
        items = load_set(rebuild)
    except qgx.GxError as e:
        print('game_text_lint: cannot run: %s' % e)
        return 2
    idx = Index(items)
    if st:
        return selftest(idx, items)
    if not (args.get('all') or args.get('staged') or args.get('changed') or args.get('tree') or files):
        print(__doc__)
        return 2
    root = args.get('root', REPO)
    found = scan(idx, files_for(args, root), load_allow())
    pub = [f for f in found if args.get('strict') or not is_local(f.split(':')[0])]
    loc = [f for f in found if f not in pub]
    for f in pub + (loc if args.get('local') else []):
        print(f)
    if loc:
        print('NOTE: %d hit(s) in %d local-only file(s) (never published: docs, db, notes); --local lists them, --strict fails on them' % (len(loc), len({f.split(':')[0] for f in loc})))
    print('game_text_lint: %d game strings in the set, %d hit(s) in published files' % (len(items), len(pub)))
    return 1 if pub else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
