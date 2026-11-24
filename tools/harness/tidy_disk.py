# S102 K1: pure helpers for tools\harness\tidy.py (no import of tidy, no deleting): reading a check script's real out\ inputs, the free-space
# log with its rate of fall, and the duplicate-capture report. tidy.py imports this file by path.
import ast, collections, hashlib, io, os, re, time, tokenize

PRAGMA = 'tidy: output'      # a line carrying this comment names an OUTPUT folder, not an input (R12 change 2)


def strip_py(text):
    """A check script's source without what cannot be an input: comments, docstrings, and any line marked `# tidy: output`.
    Falls back to the raw text when the file does not parse (a missing input is worse than a spurious one)."""
    import warnings
    lines = text.split('\n')
    blank = set()
    for i, ln in enumerate(lines):
        if PRAGMA in ln:
            blank.add(i)
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('ignore')
            tree = ast.parse(text)
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                b = node.body
                if b and isinstance(b[0], ast.Expr) and isinstance(getattr(b[0], 'value', None), ast.Constant) and isinstance(b[0].value.value, str):
                    blank.update(range(b[0].lineno - 1, b[0].end_lineno))
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.COMMENT:
                r, c = tok.start
                lines[r - 1] = lines[r - 1][:c]
    except (SyntaxError, ValueError, tokenize.TokenError, IndentationError):
        return text
    return '\n'.join('' if i in blank else ln for i, ln in enumerate(lines))


# ---------------------------------------------------------------- free-space log (R12 change 8)
def log_free(tidy_dir, free_gb, now=None, keep_rows=2000):
    """Append `time,free_gb` to out\\tidy\\free.csv (one row per watch run; the file is trimmed to keep_rows)."""
    now = time.time() if now is None else now
    os.makedirs(tidy_dir, exist_ok=True)
    p = os.path.join(tidy_dir, 'free.csv')
    rows = read_free(tidy_dir)
    rows.append((now, free_gb))
    rows = rows[-keep_rows:]
    tmp = p + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write('time,free_gb\n')
        for t, g in rows:
            f.write('%.0f,%.2f\n' % (t, g))
    os.replace(tmp, p)


def read_free(tidy_dir):
    rows = []
    try:
        with open(os.path.join(tidy_dir, 'free.csv'), encoding='utf-8') as f:
            for ln in f.read().splitlines()[1:]:
                a = ln.split(',')
                if len(a) == 2:
                    rows.append((float(a[0]), float(a[1])))
    except (OSError, ValueError):
        pass
    return rows


def burn_rate(rows, free_now, now):
    """-> (gb_per_hour fall (positive = falling) or None, minutes spanned or None): against the newest row at least 55 min old,
    else the oldest row at least 10 min old."""
    old = [r for r in rows if now - r[0] >= 55 * 60]
    ref = old[-1] if old else None
    if ref is None:
        young = [r for r in rows if now - r[0] >= 10 * 60]
        ref = young[0] if young else None
    if ref is None:
        return None, None
    span = (now - ref[0]) / 3600.0
    return (ref[1] - free_now) / span, span * 60


def disk_line(free_now, rows, now, reserve_gb):
    rate, mins = burn_rate(rows, free_now, now)
    s = '[disk] C: free %.1f GB (reserve %d GB)' % (free_now, reserve_gb)
    if rate is None:
        return s + '; no history yet (out\\tidy\\free.csv)', None
    if rate <= 0.05:
        return s + '; free did not fall in the last %.0f min (%+.1f GB/h)' % (mins, -rate), None
    hours = (free_now - reserve_gb) / rate if free_now > reserve_gb else 0.0
    return s + '; fell %.1f GB in the last %.0f min (%.1f GB/h), reserve reached in %.1f h' % (rate * mins / 60, mins, rate, hours), hours


# ---------------------------------------------------------------- duplicate captures (R12 change 5, report only)
def _file_hash(p):
    h = hashlib.sha1()
    with open(p, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


def folder_signature(path, full):
    """(sorted [(relpath, size)] or [(relpath, size, sha1)], total bytes) of a folder's files, or None when it has none."""
    ent, total = [], 0
    for dp, dn, fn in os.walk(path):
        for n in fn:
            fp = os.path.join(dp, n)
            try:
                sz = os.path.getsize(fp)
                rel = os.path.relpath(fp, path).replace('\\', '/').lower()
                ent.append((rel, sz, _file_hash(fp)) if full else (rel, sz))
                total += sz
            except OSError:
                return None
    return (sorted(ent), total) if ent else None


def find_dupes(folders):
    """folders: paths of capture-like folders. -> [(bytes each, [paths])] for byte-identical folders (same files, same content), biggest
    first. Stage 1 groups by (names, sizes); only those groups are hashed. Reads, never writes."""
    by = collections.defaultdict(list)
    for p in folders:
        s = folder_signature(p, False)
        if s:
            by[hashlib.sha1(repr(s[0]).encode()).hexdigest()].append((p, s[1]))
    out = []
    for grp in by.values():
        if len(grp) < 2:
            continue
        full = collections.defaultdict(list)
        for p, b in grp:
            s = folder_signature(p, True)
            if s:
                full[hashlib.sha1(repr(s[0]).encode()).hexdigest()].append((p, s[1]))
        for g in full.values():
            if len(g) > 1:
                out.append((g[0][1], sorted(x[0] for x in g)))
    return sorted(out, key=lambda x: -x[0] * (len(x[1]) - 1))


def dupe_lines(groups, named, human):
    """Report lines. A duplicate set is reported ONCE; the copy a check names is marked and is never the one proposed for removal."""
    L, saved = [], 0
    for b, paths in groups:
        keep = next((p for p in paths if named(p)), paths[0])
        extra = [p for p in paths if p != keep]
        saved += b * len(extra)
        L.append('%s x %d identical: keep %s' % (human(b), len(paths), keep) + ''.join('\n      duplicate: ' + p for p in extra))
    return L, saved
