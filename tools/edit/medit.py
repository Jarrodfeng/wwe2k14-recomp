#!/usr/bin/env python3
r"""medit: ONE call that applies MANY edits across files, all or none (S110 lane MEDIT; the q multi for builders).

    py -I tools\edit\medit.py EDITFILE [--check] [--root DIR]

The agent writes EDITFILE with the Write tool (no shell quoting is ever involved; name it *.medit in the scratch folder), then runs this.
EDITFILE is plain text made of blocks. The markers are WHOLE LINES that start with the fixed prefix `%%medit ` (no line of this repo's code or
docs starts so: checked with git grep, S110). The text between markers is taken EXACTLY (no escaping, no trimming) except the one newline
before the next marker. A text line that starts with `%%medit ` and is not the marker expected next is an error, so a marker can never be
swallowed into a text silently (the price: such a line cannot be written inside a FIND / WITH).

    %%medit REPLACE path\to\file.h          FIND must match exactly ONCE in the file (as edited so far)
    %%medit FIND
    <exact text, one or more lines>
    %%medit WITH
    <replacement text>
    %%medit END
    %%medit REPLACE-ALL path                same, every occurrence (at least one); prints the count
    %%medit INSERT-AFTER path               FIND = the anchor (once); WITH = the text placed right after it
    %%medit INSERT-BEFORE path              same, placed right before the anchor
    %%medit CREATE path                     WITH = the whole content (a final newline is added); refuses if the file exists
    %%medit DELETE path                     refuses if the file is missing (no FIND / WITH, just END)
    %%medit # a comment line, anywhere between blocks

Line semantics (so a line edit needs no newline juggling):
  * An empty WITH (the WITH marker directly followed by END, no line at all) DELETES the FIND; when the FIND is whole lines, their line break
    goes too. A WITH of one blank line replaces the FIND by nothing and keeps the line break.
  * INSERT-AFTER with an anchor that ends at a line end, or ends WITH its line break (a FIND whose last line is blank), puts WITH on NEW
    line(s) after it; INSERT-BEFORE with an anchor that starts a line, or starts WITH a line break, puts WITH on new line(s) before it. An
    anchor inside a line gets a literal insertion.
  * A blank line right before a marker is part of the text (the FIND / WITH then ends with a line break).
Several blocks may target one file: they apply in order, each FIND checked against the text AS EDITED SO FAR.

ALL OR NONE: every block is resolved in memory first; any failure (0 / many matches, missing file, CREATE over a file, a path outside the
root, a refused folder, a read-only or non-UTF-8 file, a control character in a WITH, an EDITFILE parse error) writes NOTHING and exits 1.
Files are then written temp + rename.
Guarantee: a failure during the resolve phase, a temp write that fails (disk full) or a rename that fails (file locked) leaves every file as it
was (renamed files are restored from the originals held in memory; created files and the folders made for them removed; deleted ones
recreated). NOT guaranteed: a process
kill or power cut in the few milliseconds of the rename phase (Windows has no multi-file atomic rename), and a file another process changes
between our read and our rename (re-checked just before the renames; a change then aborts with nothing written, but not after).
Matching ignores the line ending (a FIND written with LF matches CRLF text); the file keeps its own endings LINE BY LINE (S110 review: in a
mixed file no untouched line changes): a WITH line that is a FIND line kept or changed in place keeps that line's ending, a new line takes
the matched text's majority ending (a tie or a one-line match: the ending after the match), inserted lines the ending next to the anchor; a
new file: LF unless a same-extension sibling in its folder is CRLF. A UTF-8 BOM, the final-newline state, tabs and trailing spaces are
preserved. Non-UTF-8 files are refused; so is a control character in a WITH (below 0x20 except TAB and the line break, or 0x7F:
tools\lint\no_control_chars.py's rule; a \b or a lone CR that came through a string).
Root = the git top level of the current directory (or --root). Refused: paths outside the root (.. components, absolute paths elsewhere,
symlink / junction escapes), a ':' past the drive (a data stream, a drive-relative C:x), a name ending in a dot or a space (Windows drops
it, so it names another file), read-only files, and these folders, read from the git top level that holds the root (so a --root inside
one of them is still refused): game_data\, generated\ (except generated\rexglue.cmake), out\build*, out\bench\userdata* (the test
user-data copies every run mirrors), thirdparty\, .git, .claude\settings*.json (the owner's permission rules), .claude\worktrees\ (another
checkout: run medit with the current folder inside it). One file under two names (an 8.3 short name, a junction inside the root) is ONE
file: blocks are keyed by the real path.
Exit 0 applied (or --check clean), 1 failures, 2 usage."""
import difflib
import os
import re
import shutil
import stat
import subprocess
import sys

PREFIX = '%%medit '
OPS = ('REPLACE', 'REPLACE-ALL', 'INSERT-AFTER', 'INSERT-BEFORE', 'CREATE', 'DELETE')
HEAD_RE = re.compile(r'^%%medit (' + '|'.join(re.escape(o) for o in OPS) + r')[ \t]+(\S.*?)[ \t]*$')
SECT_RE = re.compile(r'^%%medit (FIND|WITH|END)[ \t]*$')
NL = r'\r?\n'          # a line break in a pattern: CRLF or LF
CTRL_RE = re.compile(r'[\x00-\x08\x0b-\x1f\x7f]')   # a control character a WITH may not hold (tools\lint\no_control_chars.py's set)


class Fail(Exception):
    pass


# ---------------------------------------------------------------- the EDITFILE

def parse(raw):
    """-> (blocks, errors). block = dict(op, path, find, with_, line, nwith) ; with_ is None for 'no lines' (a deletion)."""
    text = raw.decode('utf-8-sig')
    lines = re.split(r'\r?\n', text)
    if lines and lines[-1] == '':
        lines.pop()
    blocks, errors = [], []
    i, n = 0, len(lines)

    def section(start, stops):
        """lines from start up to the next marker line; -> (list, index of the marker line, marker name)"""
        j = start
        while j < n:
            if lines[j].startswith(PREFIX):
                m = SECT_RE.match(lines[j])
                if m and m.group(1) in stops:
                    return lines[start:j], j, m.group(1)
                errors.append('editfile line %d: unexpected marker line "%s" (expected %s)' % (j + 1, lines[j][:60], ' / '.join('%%medit ' + s for s in stops)))
                return None, j, None
            j += 1
        errors.append('editfile line %d: block not closed (expected %s)' % (start, ' / '.join('%%medit ' + s for s in stops)))
        return None, n, None

    while i < n:
        ln = lines[i]
        if ln.strip() == '' or ln.startswith(PREFIX + '#'):
            i += 1
            continue
        m = HEAD_RE.match(ln)
        if not m:
            errors.append('editfile line %d: expected a block header (%%medit REPLACE path ...), got "%s"' % (i + 1, ln[:60]))
            i += 1
            while i < n and not lines[i].startswith(PREFIX):
                i += 1
            continue
        op, path = m.group(1), m.group(2)
        blk = dict(op=op, path=path, find=None, with_=None, line=i + 1, has_with=False)
        i += 1
        ok = True
        if op == 'DELETE':
            body, j, name = section(i, ('END',))
            if body is None:
                ok = False
            elif any(b.strip() for b in body):
                errors.append('editfile line %d: DELETE takes no FIND / WITH' % blk['line'])
                ok = False
            i = j + 1
        else:
            if op != 'CREATE':
                body, j, name = section(i, ('FIND',))
                if body is None:
                    ok = False
                    i = j + 1
                else:
                    if any(b.strip() for b in body):
                        errors.append('editfile line %d: text before %%medit FIND' % blk['line'])
                        ok = False
                    fbody, j, name = section(j + 1, ('WITH',))
                    if fbody is None:
                        ok = False
                        i = j + 1
                    else:
                        if not fbody:
                            errors.append('editfile line %d: FIND has no line' % blk['line'])
                            ok = False
                        blk['find'] = '\n'.join(fbody)
                        i = j + 1
                        wbody, j, name = section(i, ('END',))
                        if wbody is None:
                            ok = False
                        else:
                            blk['has_with'] = True
                            blk['with_'] = '\n'.join(wbody) if wbody else None
                        i = j + 1
            else:
                body, j, name = section(i, ('WITH',))
                if body is None:
                    ok = False
                    i = j + 1
                else:
                    if any(b.strip() for b in body):
                        errors.append('editfile line %d: CREATE takes only a WITH' % blk['line'])
                        ok = False
                    wbody, j, name = section(j + 1, ('END',))
                    if wbody is None:
                        ok = False
                    else:
                        blk['has_with'] = True
                        blk['with_'] = '\n'.join(wbody) if wbody else ''
                    i = j + 1
        if ok:
            blocks.append(blk)
    return blocks, errors


# ---------------------------------------------------------------- paths

def git_top(cwd):
    try:
        r = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=cwd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    out = r.stdout.strip()
    return os.path.normpath(out) if r.returncode == 0 and out else None


def _nc(p):
    return os.path.normcase(os.path.normpath(p))


def resolve_path(root, root_real, given, anchor_real=None):
    """-> (full path, real path, root-relative path). Raises Fail with the reason. The folder rules are read relative to anchor_real (the
    git top level that holds the root), so a --root inside a refused folder does not lift its refusal."""
    drive, tail = os.path.splitdrive(given)
    if ':' in tail or (drive and not os.path.isabs(given)):
        raise Fail('path "%s": a ":" past the drive (an alternate data stream or a drive-relative path) is refused' % given)
    p = given.replace('/', os.sep)
    if any(part == '..' for part in re.split(r'[\\/]+', given)):
        raise Fail('path "%s": ".." components are refused' % given)
    if any(x not in ('', '.') and x[-1] in ' .' for x in re.split(r'[\\/]+', tail)):
        raise Fail('path "%s": a name ending in a dot or a space is refused (Windows drops it, so the name is another file\'s)' % given)
    full = os.path.normpath(p if os.path.isabs(p) else os.path.join(root, p))
    real = os.path.realpath(full)
    for cand, base, via in ((full, root, ''), (real, root_real, ' (through a symlink / junction)')):
        try:
            inside = os.path.commonpath([_nc(cand), _nc(base)]) == _nc(base)
        except ValueError:
            inside = False
        if not inside:
            raise Fail('path "%s" is outside the root %s%s' % (given, root, via))
    rel = os.path.relpath(real, anchor_real or root_real)
    parts = [x.lower() for x in re.split(r'[\\/]+', rel) if x]
    if not parts:
        raise Fail('path "%s" is the root itself' % given)
    relnorm = '\\'.join(parts)
    refused = None
    if '.git' in parts:
        refused = '.git'
    elif parts[0] in ('game_data', 'thirdparty'):
        refused = parts[0] + '\\'
    elif parts[0] == 'out' and len(parts) > 1 and parts[1].startswith('build'):
        refused = 'out\\build*'
    elif parts[:2] == ['out', 'bench'] and len(parts) > 2 and parts[2].startswith('userdata'):
        refused = 'out\\bench\\userdata* (the test user-data copies every run mirrors)'
    elif parts[0] == 'generated' and relnorm != 'generated\\rexglue.cmake':
        refused = 'generated\\ (only generated\\rexglue.cmake may be edited)'
    elif parts[0] == '.claude' and len(parts) == 2 and parts[1].startswith('settings') and parts[1].endswith('.json'):
        refused = '.claude\\settings*.json (the permission rules are the owner\'s)'
    elif parts[:2] == ['.claude', 'worktrees'] and len(parts) > 2:
        refused = '.claude\\worktrees\\ from this checkout (another checkout: run medit with the current folder inside that worktree)'
    if refused:
        raise Fail('path "%s": %s is never edited' % (given, refused))
    return full, real, os.path.relpath(full, root)


# ---------------------------------------------------------------- file state

def majority_eol(data):
    crlf = data.count('\r\n')
    lf = data.count('\n') - crlf
    if crlf == 0 and lf == 0:
        return None
    return '\r\n' if crlf > lf else '\n'


def sibling_eol(path):
    d, ext = os.path.dirname(path), os.path.splitext(path)[1].lower()
    try:
        names = sorted(os.listdir(d))
    except OSError:
        return '\n'
    seen = 0
    for nm in names:
        fp = os.path.join(d, nm)
        if os.path.splitext(nm)[1].lower() != ext or _nc(fp) == _nc(path) or not os.path.isfile(fp):
            continue
        try:
            with open(fp, 'rb') as f:
                head = f.read(65536).decode('utf-8', 'replace')
        except OSError:
            continue
        e = majority_eol(head)
        if e:
            return e
        seen += 1
        if seen >= 5:
            break
    return '\n'


class FileState:
    def __init__(self, full, rel):
        self.full, self.rel = full, rel
        self.orig = None            # original bytes (None: did not exist)
        self.bom = False
        self.text = None            # current text, None = absent
        self.eol = '\n'
        self.blocks = self.plus = self.minus = 0
        self.touched = False
        self.done = []              # the editfile block numbers applied to it so far (named in a later block's failure)

    def load(self):
        if os.path.isdir(self.full):
            raise Fail('"%s" is a folder' % self.rel)
        if os.path.exists(self.full):
            if not os.access(self.full, os.W_OK):
                raise Fail('"%s" is read-only: refused' % self.rel)
            with open(self.full, 'rb') as f:
                self.orig = f.read()
            data = self.orig
            if data.startswith(b'\xef\xbb\xbf'):
                self.bom, data = True, data[3:]
            if b'\0' in data:
                raise Fail('"%s" is binary (NUL bytes): refused' % self.rel)
            try:
                self.text = data.decode('utf-8')
            except UnicodeDecodeError as e:
                raise Fail('"%s" is not UTF-8 (byte %d): refused' % (self.rel, e.start))
            self.eol = majority_eol(self.text) or sibling_eol(self.full)
        else:
            self.eol = sibling_eol(self.full)


def nlines(s):
    return 0 if not s else s.count('\n') + 1


def find_pat(find):
    """the regex for a FIND: its line breaks match CRLF or LF; a FIND that starts with a line break must not start between the CR and the
    LF of a CRLF (else '\\r\\n' + text matches twice, at the CR and at the LF)"""
    return ('(?<!\r)' if find.startswith('\n') else '') + NL.join(re.escape(p) for p in find.split('\n'))


# ---------------------------------------------------------------- failure hints

def _short(s, n=150):
    s = s.replace('\t', '\\t')
    return s if len(s) <= n else s[:n - 3] + '...'


def zero_hint(text, find):
    """the closest candidate for a FIND that matched nowhere: line number, first differing line, whitespace verdict"""
    flines = [x.rstrip('\r') for x in text.split('\n')]
    fl = find.split('\n')
    k = max(range(len(fl)), key=lambda q: len(fl[q].strip()))
    target = fl[k].strip()
    if not target:
        return ['  (the FIND is blank lines only)']
    sm = difflib.SequenceMatcher(None, autojunk=False)
    sm.set_seq2(target)
    scored = []
    for i, ln in enumerate(flines):
        s = ln.strip()
        if not s:
            continue
        sm.set_seq1(s)
        if sm.real_quick_ratio() < 0.4 or sm.quick_ratio() < 0.4:
            continue
        scored.append((sm.ratio(), i))
    if not scored:
        return ['  no line of the file resembles the FIND (its longest line: "%s")' % _short(target, 80)]
    top = max(s for s, _ in scored)
    cands = [i for s, i in scored if s >= top - 1e-9][:30]

    def window(i):
        st = max(0, i - k)
        return st, flines[st:st + len(fl)]

    def eqcount(i):
        st, w = window(i)
        return sum(1 for a, b in zip(w, fl) if a == b)
    best = max(cands, key=eqcount)
    st, w = window(best)
    out = []
    j = next((q for q in range(len(fl)) if q >= len(w) or w[q] != fl[q]), None)
    if j is None:
        return ['  closest window (line %d) equals the FIND line by line: the difference is only in the line breaks' % (st + 1)]
    a = fl[j]
    b = w[j] if j < len(w) else '<end of file>'
    out.append('  closest: line %d (similarity %.0f%%); first difference at FIND line %d = file line %d' % (st + 1, top * 100, j + 1, st + j + 1))
    out.append('    FIND: ' + _short(a))
    out.append('    FILE: ' + _short(b))
    if j < len(w):
        if a.strip() == b.strip():
            out.append('    same text; the leading / trailing whitespace differs (FIND "%s" vs FILE "%s")' % (_short(a[:len(a) - len(a.lstrip())] + '|' + a[len(a.rstrip()):], 40).replace(' ', '.'),
                                                                                                    _short(b[:len(b) - len(b.lstrip())] + '|' + b[len(b.rstrip()):], 40).replace(' ', '.')))
        elif ' '.join(a.split()) == ' '.join(b.split()):
            out.append('    same words; a whitespace run differs inside the line')
        else:
            ops = [o for o in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes() if o[0] != 'equal'][:3]
            out.append('    differs: ' + '; '.join('FIND "%s" vs FILE "%s" (col %d)' % (_short(a[o[1]:o[2]], 30), _short(b[o[3]:o[4]], 30), o[1] + 1) for o in ops))
    strip_ok = len(w) == len(fl) and all(x.strip() == y.strip() for x, y in zip(w, fl))
    coll_ok = len(w) == len(fl) and all(' '.join(x.split()) == ' '.join(y.split()) for x, y in zip(w, fl))
    out.append('    would match if leading / trailing whitespace were ignored: %s; if whitespace runs were collapsed: %s' % ('yes' if strip_ok else 'no', 'yes' if coll_ok else 'no'))
    return out


def many_hint(text, starts, limit=8):
    lines = text.split('\n')
    out = []
    for s in starts[:limit]:
        ln = text.count('\n', 0, s)
        cur = lines[ln].rstrip('\r')
        prev = lines[ln - 1].rstrip('\r') if ln > 0 else ''
        out.append('    line %d: %s   (line before: %s)' % (ln + 1, _short(cur, 90), _short(prev.strip(), 60)))
    if len(starts) > limit:
        out.append('    ... %d more' % (len(starts) - limit))
    out.append('  lengthen the FIND with a neighbouring line so it matches once')
    return out


def edited_note(st):
    return ' (in the text as edited by block(s) %s before this one)' % ', '.join(str(b) for b in st.done) if st.done else ''


def zero_fail(st, blk, text, find):
    """the 0-match failure: the closest candidate (zero_hint) and, when the WITH is already in the file, "already applied?" with its line"""
    lines = ['0 matches' + edited_note(st)] + zero_hint(text, find)
    w = blk.get('with_')
    if w and w.strip():
        m = re.search(find_pat(w), text)
        if m:
            lines.append('  the WITH text is already in the file at line %d: already applied? (a rerun of an editfile that succeeded, or an '
                         'earlier block wrote it)' % (text.count('\n', 0, m.start()) + 1))
    return Fail('\n'.join(lines))


# ---------------------------------------------------------------- applying one block

def line_end_at(text, e):
    return e >= len(text) or text[e] in '\r\n'


def line_start_at(text, s):
    return s == 0 or text[s - 1] == '\n'


def eol_after(text, e, default):
    if text[e:e + 2] == '\r\n':
        return '\r\n'
    if text[e:e + 1] == '\n':
        return '\n'
    return default


def delete_span(text, s, e):
    """a deleted span that is whole lines takes its line break too"""
    if line_start_at(text, s) and line_end_at(text, e):
        if text[e:e + 2] == '\r\n':
            e += 2
        elif text[e:e + 1] == '\n':
            e += 1
    return s, e


def apply_block(st, blk):
    """edits st.text in place or raises Fail(message lines). Counts the lines added / removed."""
    w0 = blk.get('with_')
    bad = CTRL_RE.search(w0) if w0 else None
    if bad:
        raise Fail('WITH line %d holds a control character 0x%02X: refused (tools\\lint\\no_control_chars.py\'s rule; a \\b or a lone CR '
                   'written through a string?)' % (w0.count('\n', 0, bad.start()) + 1, ord(bad.group(0))))
    op = blk['op']
    if op == 'CREATE':
        if st.text is not None:
            raise Fail('CREATE: "%s" already exists' % st.rel)
        body = blk['with_'] or ''
        st.text = (body + '\n').replace('\n', st.eol) if body != '' else ''
        st.plus += nlines(body)
        st.touched = True
        return
    if op == 'DELETE':
        if st.text is None:
            raise Fail('DELETE: "%s" does not exist' % st.rel)
        st.minus += nlines(st.text.rstrip('\n'))
        st.text = None
        st.touched = True
        return
    if st.text is None:
        raise Fail('"%s" does not exist' % st.rel)
    text = st.text
    find = blk['find']
    if find == '':
        raise Fail('empty FIND')
    pat = find_pat(find)
    if op == 'REPLACE-ALL':
        spans = [(m.start(), m.end()) for m in re.finditer(pat, text)]
        if not spans:
            raise zero_fail(st, blk, text, find)
        w = blk['with_']
        for s, e in reversed(spans):
            text = _replace_span(st, text, s, e, w)
        st.text = text
        st.touched = True
        blk['count'] = len(spans)
        return
    starts = [m.start() for m in re.finditer('(?=' + pat + ')', text)]
    if not starts:
        raise zero_fail(st, blk, text, find)
    if len(starts) > 1:
        raise Fail('%d matches%s' % (len(starts), edited_note(st)) + '\n' + '\n'.join(many_hint(text, starts)))
    m = re.compile(pat).match(text, starts[0])
    s, e = m.start(), m.end()
    w = blk['with_']
    if op == 'REPLACE':
        text = _replace_span(st, text, s, e, w)
    else:
        if w is None:
            raise Fail('%s with an empty WITH inserts nothing' % op)
        st.plus += nlines(w)
        if op == 'INSERT-AFTER':
            if text[e - 1] == '\n' and (e >= len(text) or text[e] not in '\r\n'):    # the anchor ends WITH its line break
                eol = '\r\n' if text[e - 2:e] == '\r\n' else '\n'
                text = text[:e] + w.replace('\n', eol) + eol + text[e:]
            elif line_end_at(text, e):
                eol = eol_after(text, e, st.eol)
                text = text[:e] + eol + w.replace('\n', eol) + text[e:]
            else:
                text = text[:e] + w.replace('\n', st.eol if '\n' in w else '\n') + text[e:]
        else:
            if line_start_at(text, s):
                nxt = re.search(r'\r?\n', text[s:])
                eol = nxt.group(0) if nxt else st.eol
                text = text[:s] + w.replace('\n', eol) + eol + text[s:]
            elif text[s] in '\r\n':             # the anchor starts WITH a line break (after a line that has text)
                eol = '\r\n' if text[s:s + 2] == '\r\n' else '\n'
                text = text[:s] + eol + w.replace('\n', eol) + text[s:]
            else:
                text = text[:s] + w.replace('\n', st.eol if '\n' in w else '\n') + text[s:]
    st.text = text
    st.touched = True


def _with_eols(text, s, e, w, default):
    """WITH in the file's own endings, line by line: a WITH line that is a FIND line kept or changed in place (difflib over the lines) is
    followed by that line's ending in the file; a new line by the matched text's majority ending (a tie or a one-line match: the ending
    after the match, else the file's majority). So in a mixed file no line the edit did not change gets another ending."""
    wl = w.split('\n')
    if len(wl) == 1:
        return w
    parts = re.split(r'(\r?\n)', text[s:e])
    mt, me = parts[0::2], parts[1::2]
    crlf = me.count('\r\n')
    fb = '\r\n' if crlf * 2 > len(me) else '\n' if crlf * 2 < len(me) else eol_after(text, e, default)
    src = [None] * len(wl)
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, mt, wl, autojunk=False).get_opcodes():
        if tag in ('equal', 'replace'):
            for j in range(j1, min(j2, j1 + i2 - i1)):
                src[j] = i1 + j - j1
    out = [wl[0]]
    for j in range(1, len(wl)):
        i = src[j - 1]
        out.append(me[i] if i is not None and i < len(me) else fb)
        out.append(wl[j])
    return ''.join(out)


def _replace_span(st, text, s, e, w):
    matched = text[s:e]
    st.minus += nlines(matched)
    if w is None:
        s2, e2 = delete_span(text, s, e)
        return text[:s2] + text[e2:]
    st.plus += nlines(w)
    return text[:s] + _with_eols(text, s, e, w, st.eol) + text[e:]


# ---------------------------------------------------------------- writing

def encode(st):
    data = st.text.encode('utf-8')
    return (b'\xef\xbb\xbf' + data) if st.bom else data


def _remove(path):
    """removes a temp file, also when it copied a read-only mode"""
    try:
        os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    except OSError:
        pass
    try:
        os.remove(path)
    except OSError:
        pass


def _remove_dirs(made):
    """removes the folders write_atomic made for a CREATE (deepest first; a folder that is not empty stays)"""
    for d in sorted(set(made), key=len, reverse=True):
        try:
            os.rmdir(d)
        except OSError:
            pass


def write_atomic(path, data, like=None, made=None):
    """writes data to a temp file beside path and returns its name; each folder it has to make is added to made"""
    d = os.path.dirname(path) or '.'
    if made is not None:
        q = d
        while q and not os.path.isdir(q) and os.path.dirname(q) != q:
            made.append(q)
            q = os.path.dirname(q)
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, '.' + os.path.basename(path) + '.medit.tmp')
    with open(tmp, 'wb') as f:
        f.write(data)
    if like and os.path.exists(like):
        try:
            shutil.copymode(like, tmp)
        except OSError:
            pass
    return tmp


def commit_all(states):
    """phase 2: temps for every changed file, then renames; on any failure restore. -> error text or None"""
    plan = [st for st in states if st.touched and (st.text is not None or st.orig is not None)]
    temps, made = {}, []
    try:
        for st in plan:
            if st.text is not None:
                temps[st.full] = write_atomic(st.full, encode(st), st.full, made)
        for st in plan:                     # nobody changed a file since we read it
            cur = open(st.full, 'rb').read() if os.path.exists(st.full) else None
            if cur != st.orig:
                raise OSError('"%s" changed on disk while medit was resolving the edits' % st.rel)
    except OSError as e:
        for t in temps.values():
            _remove(t)
        _remove_dirs(made)
        return 'nothing written (%s)' % e
    done = []
    try:
        for st in plan:
            if st.text is None:
                os.remove(st.full)
            else:
                os.replace(temps[st.full], st.full)
            done.append(st)
    except OSError as e:
        problems = []
        for st in reversed(done):
            try:
                if st.orig is None:
                    os.remove(st.full)
                else:
                    os.replace(write_atomic(st.full, st.orig), st.full)
            except OSError as e2:
                problems.append('%s (%s)' % (st.rel, e2))
        for t in temps.values():
            _remove(t)
        _remove_dirs(made)
        return 'a rename failed (%s); %d file(s) restored%s' % (e, len(done) - len(problems), '; COULD NOT RESTORE: ' + ', '.join(problems) if problems else '')
    return None


# ---------------------------------------------------------------- main

def run(argv):
    try:
        sys.stdout.reconfigure(errors='backslashreplace')    # a non-ASCII hint on a cp1252 pipe must not crash the report
    except (AttributeError, ValueError):
        pass
    check = '--check' in argv
    root = None
    args = []
    it = iter(argv)
    for a in it:
        if a == '--root':
            root = next(it, None)
        elif a == '--check':
            continue
        elif a.startswith('--'):
            print('medit: unknown option ' + a)
            return 2
        else:
            args.append(a)
    if len(args) != 1:
        print('usage: py -I tools\\edit\\medit.py EDITFILE [--check] [--root DIR]')
        return 2
    try:
        with open(args[0], 'rb') as f:
            raw = f.read()
    except OSError as e:
        print('medit: cannot read the editfile: %s' % e)
        return 2
    root_given = bool(root)
    root = os.path.normpath(os.path.abspath(root)) if root else git_top(os.getcwd())
    if not root or not os.path.isdir(root):
        print('medit: no root (not inside a git checkout; pass --root DIR)')
        return 2
    root_real = os.path.realpath(root)
    anchor_real = os.path.realpath(git_top(root) or root) if root_given else root_real    # the folder rules hold at the checkout top
    try:
        blocks, perr = parse(raw)
    except UnicodeDecodeError:
        print('medit: the editfile is not UTF-8')
        return 2
    failures = ['  ' + e for e in perr]
    states, order = {}, []
    for bi, blk in enumerate(blocks, 1):
        tag = '  block %d (%s %s, editfile line %d): ' % (bi, blk['op'], blk['path'], blk['line'])
        try:
            full, real, rel = resolve_path(root, root_real, blk['path'], anchor_real)
            key = _nc(real)             # one file under two names (an 8.3 short name, a junction inside the root) is ONE state
            st = states.get(key)
            if st is None:
                st = FileState(full, rel)
                st.load()
                states[key] = st
                order.append(st)
            apply_block(st, blk)
            st.blocks += 1
            st.done.append(bi)
            if blk.get('count'):
                st.notes = getattr(st, 'notes', []) + ['REPLACE-ALL block %d: %d occurrence(s)' % (bi, blk['count'])]
        except Fail as e:
            msg = str(e).split('\n')
            failures.append(tag + msg[0])
            failures.extend(msg[1:])
    if failures:
        nfail = len([f for f in failures if f.startswith('  block') or f.startswith('  editfile')])
        print('medit: NOTHING written: %d block(s) failed' % nfail)
        print('\n'.join(failures))
        return 1
    changed = [st for st in order if st.touched]
    for st in changed:
        print('%s: %d block(s) applied, +%d -%d lines' % (st.rel, st.blocks, st.plus, st.minus))
        for nt in getattr(st, 'notes', []):
            print('  ' + nt)
    tail = '%d block(s) in %d file(s)' % (sum(s.blocks for s in changed), len(changed))
    if check:
        print('medit --check: %s would apply (nothing written)' % tail)
        return 0
    err = commit_all(order)
    if err:
        print('medit: NOTHING written (or restored): ' + err)
        return 1
    print('medit: %s applied' % tail)
    return 0


if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))
