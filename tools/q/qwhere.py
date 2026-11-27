"""q --where REGEX [--context N] on ANY verb (S111 QWHERE): keep only the output lines that match, with N lines around each.

  q decomp sub_X --where "stw.*1056"          the matching instruction lines only
  q multi "decomp sub_X --where stw.*1056" "field 268 --writes --where sub_8283"      works per question inside multi
  q field 268 --writes --where sub_8283 --context 1

Why it exists: the habit hook refuses `q ... | head` / `| tail`, so probes saved a whole decomp / a 4943-access field list to a scratch file and grepped it
(the same cut, two extra calls and a re-read). The filter runs INSIDE the call, once, whatever the verb.

Kept whatever the REGEX says (the verb's header and totals): the first line; a line starting `== ` / `-- ` / `... ` / `q: ` / `WARNING`; a line at column 0
that reads like a total (`322 accesses in 266 functions`, `showing 6 of 126`, `truncated`, `N more`). The last line is always

  where: shown K of M lines (REGEX); the rest: <the same call without --where>

--json: the one JSON document keeps every non-list key (the totals); each list of rows is filtered to the rows whose JSON text matches; a `where` key says
shown K of M rows. stderr passes through untouched. A verb that prints through a subprocess straight to the terminal is not filtered (none does).

Where it hooks: q.main() calls run() first, ONE seam that every verb and every `q multi` question passes (decomp / asm / field are hand-written in q.py, so the
verb loader qverbs.dispatch alone does not see them). Pure text, no index: the unit is tools\\q\\qwhere_selftest.py.
"""
import contextlib
import io
import json
import re
import sys

TOTAL_RE = re.compile(r'^\S.*(?:\b\d[\d,]*\s+(?:more|total|accesses|functions|callers|lines|rows|hits|sites|matches|files)\b|\b(?:total|showing|truncated)\b)', re.I)
HEAD_RE = re.compile(r'^(?:== |-- |\.\.\. |q: |WARNING)')

_ACTIVE = [False]


def active():
    """True while a --where call runs: a verb that cuts long output by default (asm) prints it whole then, the filter is the cut."""
    return _ACTIVE[0]


class WhereError(ValueError):
    pass


def split_args(argv):
    """-> (argv without --where / --context, regex, context) or (argv, None, 0). `--where=R` and `--where R` both."""
    out, rx, ctx, i = [], None, 0, 0
    if not argv or argv[0] == 'multi' or not any(a == '--where' or a.startswith('--where=') for a in argv):
        return list(argv), None, 0
    while i < len(argv):
        a = argv[i]
        if a == '--where' or a == '--context':
            if i + 1 >= len(argv):
                raise WhereError(f'{a} needs a value')
            if a == '--where':
                rx = argv[i + 1]
            else:
                try:
                    ctx = int(argv[i + 1])
                except ValueError:
                    raise WhereError(f'--context wants a number, got {argv[i + 1]!r}')
            i += 2
        elif a.startswith('--where=') or a.startswith('--context='):
            k, v = a.split('=', 1)
            if k == '--where':
                rx = v
            else:
                try:
                    ctx = int(v)
                except ValueError:
                    raise WhereError(f'--context wants a number, got {v!r}')
            i += 1
        else:
            out.append(a)
            i += 1
    return out, rx, max(0, ctx)


def quote_word(w):
    """One argv word as it must be typed to come back as one word (q multi's shlex and a shell both read it): a word with a space or a quote is wrapped.
    S111 QFIX5: the rest-call line printed `q ours text a b` for the word `a b`, a different question when pasted."""
    if w and not any(c.isspace() or c in '\'"' for c in w):
        return w
    return f'"{w}"' if '"' not in w else f"'{w}'"


def _is_keep(i, ln):
    return i == 0 or bool(HEAD_RE.match(ln)) or bool(TOTAL_RE.match(ln))


def filter_text(text, rx, ctx, rest_call):
    """The filtered text (ends with the where: line)."""
    pat = re.compile(rx)
    lines = text.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    keep = set()
    for i, ln in enumerate(lines):
        if _is_keep(i, ln):
            keep.add(i)
        if pat.search(ln):
            keep.update(range(max(0, i - ctx), min(len(lines), i + ctx + 1)))
    out = [lines[i] for i in sorted(keep)]
    out.append(f'where: shown {len(keep)} of {len(lines)} lines ({rx}); the rest: {rest_call}')
    return '\n'.join(out) + '\n'


def _walk(o, pat, tot):
    """Filter every list of rows in a JSON value; tot = [shown, total]."""
    if isinstance(o, dict):
        return {k: _walk(v, pat, tot) for k, v in o.items()}
    if isinstance(o, list) and o and all(isinstance(x, (dict, list, str)) for x in o):
        kept = [x for x in o if pat.search(json.dumps(x, ensure_ascii=False))]
        tot[0] += len(kept)
        tot[1] += len(o)
        return kept
    return o


def filter_json(text, rx, rest_call):
    pat = re.compile(rx)
    try:
        doc = json.loads(text)
    except ValueError:
        return None
    tot = [0, 0]
    doc2 = _walk(doc, pat, tot)
    note = {'shown': tot[0], 'of': tot[1], 'regex': rx, 'rest': rest_call}
    if isinstance(doc2, dict):
        doc2['where'] = note
    else:
        print(f'where: shown {tot[0]} of {tot[1]} rows ({rx}); the rest: {rest_call}', file=sys.stderr)
    return json.dumps(doc2, indent=1, ensure_ascii=False) + '\n'


class _Buf(io.StringIO):
    def reconfigure(self, **kw):      # q.py's own sys.stdout.reconfigure(errors='replace') calls
        pass


def run(argv, qmain):
    """None when argv has no --where (the caller goes on); else the exit code of qmain(argv without --where) with its stdout filtered."""
    try:
        args, rx, ctx = split_args(argv)
    except WhereError as e:
        print(f'q: {e}', file=sys.stderr)
        return 2
    if rx is None:
        return None
    try:
        re.compile(rx)
    except re.error as e:
        print(f'q: --where {rx!r} is not a regular expression: {e}', file=sys.stderr)
        return 2
    rest_call = 'q ' + ' '.join(quote_word(x) for x in args)
    buf = _Buf()
    _ACTIVE[0] = True
    try:
        with contextlib.redirect_stdout(buf):
            rc = qmain(args)
    finally:
        _ACTIVE[0] = False
    text = buf.getvalue()
    res = filter_json(text, rx, rest_call) if '--json' in args and text.lstrip().startswith(('{', '[')) else None
    if res is None:
        res = filter_text(text, rx, ctx, rest_call)
    try:
        sys.stdout.write(res)
    except BrokenPipeError:
        return 0
    return 0 if rc is None else rc
