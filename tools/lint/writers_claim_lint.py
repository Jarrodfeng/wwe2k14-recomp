#!/usr/bin/env python3
r"""S111 WRITERLINT: a claim that says who writes a game field must name every writer the field index lists.

  py -I tools\lint\writers_claim_lint.py --root DIR --base REF     the notes the diff BASE -> tree changed (check.py's `writers_claim` row): FAIL = exit 1
  py -I tools\lint\writers_claim_lint.py --root DIR --all          a sweep of every docs\research\**.md note: prints, never fails (exit 0)

The fault (MOUSESCROLL, S111): a note claimed the word at SM+268 "flips 0 / 1" at sub_8283A5E8, while `q field 268 --global 0x8392945C --writes` listed
three more writers (sub_8283A6A0, sub_8283A778, sub_8283E150). Rule: a ```claims row (format: tools\handback\claims.py) whose CLAIM text
  - names a field offset (`SM+268`, `r3+0x10C`, `field +268`: an identifier or `]` / `)` right before the `+`; a bare `+0x204` is a code offset, not a field), AND
  - uses a writer-exhaustive word (WORDS below: only writer, sole writer, the writer, written only by, flips, alternates, never written elsewhere ...), AND
  - has a root: a `g:ADDR` or `[0xADDR]` in the row (claim / evidence), else the ONE such global the note names (none or several: not judged, listed by --all),
must name (a `sub_XXXXXXXX` in its claim or evidence) every function `q field OFF --global ADDR --writes` returns, or carry the mark [L].
The writers come from tools\q\qfield.py in this process (no subprocess per row; --no-rebuild: a stale index answers, no index = the row is BLOCKED).
Planted twins run in memory on every run (no index needed): MOUSESCROLL's old row must FAIL, the corrected row (all writers named) and an [L] row must PASS.
Not a proof of completeness: the index misses indexed accesses (stwx ...) and a writer whose root the fold cannot name (sub_8283D488 stores through a bare a0).
Exit: 0 clean (or --all), 1 a finding or a failed twin, 2 could not run. Stdlib only; 0 game runs."""
import argparse
import contextlib
import importlib.util
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))

# the one list of writer-exhaustive words (matched case-insensitively, whole words)
WORDS = ('only writer', 'only writers', 'sole writer', 'the writer', 'written only by', 'only written by', 'never written elsewhere',
         'flips', 'flipped', 'alternates')
WORD_RE = re.compile(r'(?<![A-Za-z])(?:' + '|'.join(re.escape(w).replace(r'\ ', r'\s+') for w in WORDS) + r')(?![A-Za-z])', re.I)
OFF_RE = re.compile(r'(?:(?<![\w.])[A-Za-z_]\w*|[\]\)])\s?\+\s?(0x[0-9A-Fa-f]+|\d+)\b|\b(?:field|offset|word)\s+\+(0x[0-9A-Fa-f]+|\d+)\b')
ROOT_RE = re.compile(r'\bg:([0-9A-Fa-f]{8})\b|\[0x([0-9A-Fa-f]{8})\]')
SUB_RE = re.compile(r'\bsub_([0-9A-Fa-f]{8})\b')


class CannotRun(Exception):
    pass


def load_claims():
    spec = importlib.util.spec_from_file_location('claims', os.path.join(REPO, 'tools', 'handback', 'claims.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def roots_in(text):
    return sorted({int(a or b, 16) for a, b in ROOT_RE.findall(text)})


def offsets_in(claim):
    return sorted({int(a or b, 0) for a, b in OFF_RE.findall(claim)})


def judge(rows, note_text, writers_of):
    """-> (findings, unresolved, judged). rows: parse_claims rows; writers_of(root, off) -> set of function addresses (or raises CannotRun).
    finding = (line, word, off, root, missing list); unresolved = (line, why)."""
    note_roots = roots_in(note_text)
    findings, unresolved, judged = [], [], 0
    for r in rows:
        if r['mark'] == 'L':
            continue
        m = WORD_RE.search(r['claim'])
        offs = offsets_in(r['claim'])
        if not m or not offs:
            continue
        roots = roots_in(r['claim'] + ' ' + r['evidence']) or (note_roots if len(note_roots) == 1 else [])
        if len(roots) != 1:
            unresolved.append((r['line'], 'no single root (g:ADDR / [0xADDR]) in the row or the note: %d named' % len(roots)))
            continue
        named = {int(x, 16) for x in SUB_RE.findall(r['claim'] + ' ' + r['evidence'])}
        for off in offs:
            judged += 1
            missing = sorted(writers_of(roots[0], off) - named)
            if missing:
                findings.append((r['line'], m.group(0), off, roots[0], missing))
    return findings, unresolved, judged


def qfield_writers(root_addr, off):
    """The functions `q field OFF --global ADDR --writes` returns, through qfield.main (in process, JSON)."""
    qdir = os.path.join(REPO, 'tools', 'q')
    if qdir not in sys.path:
        sys.path.insert(0, qdir)
    import qfield
    buf, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        try:
            rc = qfield.main([str(off), '--global', '0x%08X' % root_addr, '--writes', '--json', '--limit', '5000', '--no-rebuild'])
        except SystemExit as e:
            rc = e.code or 2
    if rc:
        raise CannotRun('q field %d --global 0x%08X: exit %s: %s' % (off, root_addr, rc, (err.getvalue() or buf.getvalue()).strip()[:160]))
    return {int(x['func'][4:], 16) for x in json.loads(buf.getvalue())['rows']}


def make_cached(fn):
    cache = {}

    def get(root_addr, off):
        k = (root_addr, off)
        if k not in cache:
            cache[k] = fn(root_addr, off)
        return cache[k]
    return get


# ---------------------------------------------------------------------------------------------------------------- planted twins (MOUSESCROLL)
TWIN_WRITERS = {0x8283A5E8, 0x8283A6A0, 0x8283A778, 0x8283E150, 0x8283D488}
TWIN_ROOT_NOTE = 'The SmartMenu ([0x8392945C]) keeps two list contexts; the live one is the word at SM+268.\n'
TWIN_OLD = ('0x8283A5E8 | C | the live list context word SM+268 is advanced by one and reset to 0 when it reaches 2 or is negative, so it alternates 0 / 1 '
            '| evidence: decomp sub_8283A5E8 +0..+0x1C')
TWIN_NEW = ('0x8283A5E8 | C | the live list context word SM+268 alternates 0 / 1 at sub_8283A5E8 and goes down at sub_8283A6A0, sub_8283A778, '
            'sub_8283E150 and sub_8283D488 | evidence: decomp sub_8283A5E8 +0..+0x1C; q field 268 --writes')
TWIN_OLD_L = TWIN_OLD.replace('| C |', '| L |')


def twins():
    """-> list of failure texts (empty = the planted old row fails, the corrected row and the [L] row pass)."""
    claims = load_claims()

    def run(row):
        text = TWIN_ROOT_NOTE + '```claims\n' + row + '\n```\n'
        return judge(claims.parse_claims(text), text, lambda root, off: {0x8283A6A0, 0x8283A778, 0x8283E150} if (root, off) == (0x8392945C, 268) else set())
    bad = []
    f, _u, n = run(TWIN_OLD)
    if not (n == 1 and len(f) == 1 and f[0][4] == [0x8283A6A0, 0x8283A778, 0x8283E150]):
        bad.append('planted twin: MOUSESCROLL\'s old row (alternates, only sub_8283A5E8) was not flagged: %s' % (f,))
    f, _u, n = run(TWIN_NEW)
    if f or n != 1:
        bad.append('planted twin: the corrected row (every writer named) was flagged or not judged: %s (judged %d)' % (f, n))
    f, _u, n = run(TWIN_OLD_L)
    if f or n:
        bad.append('planted twin: the same row marked [L] was judged: %s' % (f,))
    f, u, n = judge(claims.parse_claims('```claims\n' + TWIN_OLD + '\n```\n'), 'no root here', lambda root, off: TWIN_WRITERS)
    if f or not u:
        bad.append('planted twin: a row with no root must be listed as unresolved, not judged: %s / %s' % (f, u))
    return bad


# ---------------------------------------------------------------------------------------------------------------- notes
def git_lines(root, *args):
    r = subprocess.run(['git', '-C', root] + list(args), capture_output=True)
    if r.returncode:
        raise CannotRun('git %s failed: %s' % (' '.join(args), r.stderr.decode('utf-8', 'replace').strip()[:200]))
    return r.stdout.decode('utf-8', 'replace').splitlines()


def changed_notes(root, base):
    names = set(git_lines(root, 'diff', '--name-only', '--no-renames', '--diff-filter=AM', base)) | set(git_lines(root, 'ls-files', '--others', '--exclude-standard'))
    return sorted(n.replace('\\', '/') for n in names if n.replace('\\', '/').startswith('docs/research/') and n.endswith('.md'))


def all_notes(root):
    return sorted(n for n in git_lines(root, 'ls-files', '--', 'docs/research') if n.endswith('.md'))


def lint_notes(root, notes, writers_of=None):
    """-> (findings [(rel, line, text)], unresolved [(rel, line, why)], judged, skipped notes). A note with no claims block or a malformed one is skipped (lodge_lint judges the format)."""
    claims = load_claims()
    writers_of = make_cached(writers_of or qfield_writers)
    out, unres, judged, skipped = [], [], 0, 0
    for rel in notes:
        try:
            with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
                text = f.read()
            rows = claims.parse_claims(text)
        except (OSError, claims.ClaimsError):
            skipped += 1
            continue
        if not rows:
            continue
        f_, u_, n_ = judge(rows, text, writers_of)
        judged += n_
        for line, word, off, rt, missing in f_:
            out.append((rel, line, 'says "%s" about +%d on g:%08X but q field lists %d writer(s) it does not name: %s (name them, or mark the row [L])'
                        % (word, off, rt, len(missing), ', '.join('sub_%08X' % a for a in missing))))
        unres += [(rel, line, why) for line, why in u_]
    return out, unres, judged, skipped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--base', default=None)
    ap.add_argument('--all', action='store_true', help='sweep every docs\\research note: print, never fail')
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    if not a.all and not a.base:
        print('writers_claim_lint: give --base REF (the changed notes) or --all (a sweep)')
        return 2
    bad = twins()
    for b in bad:
        print('FAIL ' + b)
    try:
        notes = all_notes(root) if a.all else changed_notes(root, a.base)
        found, unres, judged, skipped = lint_notes(root, notes)
    except CannotRun as e:
        print('writers_claim_lint: could not run: %s' % e)
        return 2
    for rel, line, why in found:
        print('%s %s:%d: %s' % ('FOUND' if a.all else 'FAIL', rel.replace('/', '\\'), line, why))
    if a.all:
        for rel, line, why in unres:
            print('unresolved %s:%d: %s' % (rel.replace('/', '\\'), line, why))
    print('writers_claim_lint: %d note(s), %d field claim(s) judged, %d finding(s), %d unresolved (no single root), %d note(s) unreadable, planted twins %s'
          % (len(notes), judged, len(found), len(unres), skipped, 'FAILED' if bad else 'ok'))
    return 1 if bad or (found and not a.all) else 0


if __name__ == '__main__':
    sys.exit(main())
