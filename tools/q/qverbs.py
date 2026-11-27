"""qverbs: the q verb registry (S111 QVERBS). One file per verb in tools/q/verbs/NAME.toml; q.py builds its usage text, routing screen, VERBS tuple and the
dispatch of every registered verb from them, so adding a verb adds a NEW file and never edits q.py (two lanes adding verbs no longer conflict at merge).

A verb file (TOML; every key but name and module is optional):

    name = "slice"                    # the first word of the call (= the file name)
    module = "qslice"                 # tools/q/qslice.py, imported on first use only (a broken module breaks only its own verb)
    main = "main"                     # the function called with the words after the verb (default main)
    needs = "con"                     # extra arguments: "none" (default) | "con" (connect(DB)) | "con_resolve" (connect(DB), resolve)
    errors = ["qslice.SliceError", "qslice.qdecomp.DecompError"]   # the module's own error classes (dotted: module, then attributes)
    qerror = true                     # also catch q.QError (default true)       } a caught error prints `q: MESSAGE` on stderr, exit 2;
    broken_pipe = true                # a closed pipe returns 0 (default true)   } with neither errors nor these two, the call is bare
    usage_slot = "new"                # where the verb's usage lines go in `q -h`: a {{usage:SLOT}} marker line of q.py's docstring (default new)
    route_slot = "new"                # the same for the first screen (ROUTING)
    [[usage]]                         # one table per block of usage lines (order = sort key inside the slot, default 1000, then the name)
    order = 61
    text = '''  slice sub_X+OFF REG ...   what it answers (tools/q/qslice.py)'''
    [[route]]                         # one table per routing line ("- the question: the command")
    order = 147
    text = '''- where a value comes from: slice sub_X+OFF REG'''

WWE_Q_VERBS_EXTRA=DIR adds a second folder of verb files (and puts it on sys.path): the selftest's planted verb; nothing else sets it.
"""
import importlib
import os
import re
import sys
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
VERB_DIR = os.path.join(HERE, 'verbs')
ENV_EXTRA = 'WWE_Q_VERBS_EXTRA'
KEYS = {'name', 'module', 'main', 'needs', 'errors', 'qerror', 'broken_pipe', 'usage_slot', 'route_slot', 'usage', 'route'}
NEEDS = ('none', 'con', 'con_resolve')
MARK = re.compile(r'^\{\{(usage|route):([A-Za-z0-9_]+)\}\}$')


class VerbError(Exception):
    pass


def _check(d, path):
    bad = sorted(set(d) - KEYS)
    if bad:
        raise VerbError(f'unknown key(s) {bad}')
    for k in ('name', 'module'):
        if not isinstance(d.get(k), str) or not d[k]:
            raise VerbError(f'{k} missing')
    if d['name'] != os.path.splitext(os.path.basename(path))[0]:
        raise VerbError(f'name {d["name"]!r} is not the file name')
    if d.get('needs', 'none') not in NEEDS:
        raise VerbError(f'needs must be one of {NEEDS}')
    if not all(isinstance(e, str) and '.' in e for e in d.get('errors', [])):
        raise VerbError('errors: a list of "module.Class" strings')
    for k in ('usage', 'route'):
        for t in d.get(k, []):
            if not isinstance(t.get('text'), str):
                raise VerbError(f'[[{k}]] needs text')


def load(extra=None):
    """-> (entries sorted by name, warnings). A bad verb file is skipped with a warning: it must not break every other q call."""
    dirs = [VERB_DIR]
    ex = extra if extra is not None else os.environ.get(ENV_EXTRA)
    if ex:
        dirs.append(ex)
        if ex not in sys.path:
            sys.path.insert(0, ex)
    out, warns, seen = [], [], {}
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith('.toml'):
                continue
            p = os.path.join(d, fn)
            try:
                with open(p, 'rb') as f:
                    e = tomllib.load(f)
                _check(e, p)
            except (OSError, tomllib.TOMLDecodeError, VerbError) as ex_:
                warns.append(f'q: verb file {fn} skipped: {ex_}')
                continue
            if e['name'] in seen:
                warns.append(f'q: verb file {fn} skipped: {e["name"]!r} is already registered by {seen[e["name"]]}')
                continue
            seen[e['name']] = p
            out.append(e)
    out.sort(key=lambda e: e['name'])
    return out, warns


def expand(text, kind, entries):
    """The text with every `{{KIND:SLOT}}` marker line replaced by the registered lines of that slot (sorted by order, then verb name);
    an empty slot removes its marker line."""
    key = 'usage' if kind == 'usage' else 'route'
    slot_of = 'usage_slot' if kind == 'usage' else 'route_slot'
    out = []
    for ln in text.split('\n'):
        m = MARK.match(ln)
        if not m or m.group(1) != kind:
            out.append(ln)
            continue
        rows = []
        for e in entries:
            if e.get(slot_of, 'new') == m.group(2):
                for t in e.get(key, []):
                    rows.append((t.get('order', 1000), e['name'], t['text']))
        rows.sort(key=lambda r: (r[0], r[1]))
        out.extend(r[2] for r in rows)
    return '\n'.join(out)


def _errclasses(names, q):
    out = []
    for dotted in names:
        parts = dotted.split('.')
        obj = importlib.import_module(parts[0])
        for a in parts[1:]:
            obj = getattr(obj, a)
        out.append(obj)
    return tuple(out)


def dispatch(e, argv, q):
    """Run one registered verb the way q.py's hand-written blocks did: import the module, call main(argv[1:] [, connect(DB) [, resolve]]),
    `q: MESSAGE` + exit 2 on QError / the module's errors, a closed pipe -> 0. `q` is the q module (connect, resolve, DB, QError)."""
    mod = importlib.import_module(e['module'])
    fn = getattr(mod, e.get('main', 'main'))
    errs = _errclasses(e.get('errors', []), q)
    catch = (q.QError,) * bool(e.get('qerror', True)) + errs
    bp = e.get('broken_pipe', True)
    if not (catch or bp):
        return fn(*_args(e, argv, q))
    try:
        return fn(*_args(e, argv, q))
    except catch as ex:
        print(f'q: {ex}', file=sys.stderr)
        return 2
    except BrokenPipeError:
        if not bp:
            raise
        return 0


def _args(e, argv, q):
    a = [argv[1:]]
    need = e.get('needs', 'none')
    if need in ('con', 'con_resolve'):
        a.append(q.connect(q.DB))
    if need == 'con_resolve':
        a.append(q.resolve)
    return a
