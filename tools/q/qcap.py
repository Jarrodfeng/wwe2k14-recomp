"""qcap (S108 lane FD): ONE sizing rule for q's answers, so no answer is cut silently.

    shown, cut = qcap.cap(items, limit, all_)     # all_ / limit None = everything; a small list is never cut
    qcap.trim_line(len(shown), total, 'files', 'q ours text X --all')   # 'showing 6 of 751 files; all: q ... --all, or narrow with --in PATH'
    qcap.in_scope(path, spec)                       # --in PATH: path contains spec (separators and case ignored)
    qcap.rank(text, needle)                         # 0 = whole word, exact case; 1 = whole word, any case; 2 = substring

The rule: every list prints its TOTAL; a cut list ends with the trim line naming the total and the exact call that gives the rest; a list is cut only
when it is more than max(3, cap // 2) items over the cap (a 22-item list with cap 20 is shown whole); `--all` = no cap; `--json` is never cut (callers
pass all_=True for json).
"""
import os
import re


def slack(limit):
    return max(3, limit // 2)


def cap(items, limit, all_=False):
    """-> (shown, n_cut)"""
    items = list(items)
    if all_ or limit is None or limit <= 0 or len(items) <= limit + slack(limit):
        return items, 0
    return items[:limit], len(items) - limit


def cut_n(total, limit, all_=False):
    """how many items a list of `total` shows under the rule"""
    if all_ or limit is None or limit <= 0 or total <= limit + slack(limit):
        return total
    return limit


def trim_line(shown, total, what, all_call=None, narrow=True):
    """the line a cut list ends with: the total and the exact way to get the rest"""
    s = f'showing {shown:,} of {total:,} {what}'
    how = []
    if all_call:
        how.append(f'all: {all_call}')
    if narrow:
        how.append('narrow: add --in PATH')
    return s + ('; ' + ', or '.join(how) if how else '')


def norm(p):
    return (p or '').replace('\\', '/').lower().strip('/')


def scope_spec(spec, root=None):
    """--in PATH as typed -> the repo-relative text in_scope compares (review: an absolute path or a ./ prefix matched NOTHING, silently).
    An absolute path, or one starting with ./ or ../, is resolved against the cwd and made relative to `root` when it lies inside it; otherwise a
    leading ./ is dropped. The root itself -> '' (no narrowing)."""
    if not spec:
        return spec
    s = spec.strip().strip('"')
    if root and (os.path.isabs(s) or re.match(r'\.\.?(?:[\\/]|$)', s)):
        try:
            rel = os.path.relpath(os.path.abspath(s), os.path.abspath(root))
        except ValueError:        # another drive
            rel = None
        if rel is not None and rel != '..' and not rel.startswith('..' + os.sep):
            return '' if rel == '.' else rel.replace('\\', '/')
    while re.match(r'\.[\\/]', s):
        s = s[2:]
    return s


def in_scope(path, spec):
    """--in PATH: True when the path contains PATH (a folder, a file, a basename), separators and case ignored"""
    if not spec:
        return True
    return norm(spec) in norm(path)


def rank(text, needle):
    """exactness of a hit: 0 whole word exact case, 1 whole word any case, 2 substring, 3 absent"""
    if not needle:
        return 3
    w = r'(?<![A-Za-z0-9_])' + re.escape(needle) + r'(?![A-Za-z0-9_])'
    if re.search(w, text):
        return 0
    if re.search(w, text, re.I):
        return 1
    return 2 if needle.lower() in text.lower() else 3


def requote(argv):
    """an argv as one command line an agent can paste (words with spaces quoted)"""
    return ' '.join(a if a and not re.search(r'[\s"\'|&;<>()]', a) else '"' + a.replace('"', '\\"') + '"' for a in argv)
