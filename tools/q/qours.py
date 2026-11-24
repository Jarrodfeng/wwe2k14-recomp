"""q ours: questions about OUR code (src\\, tools\\, probes\\) answered by PLAIN TEXT SEARCH: no index, no compiler, the same in the main checkout and in any worktree.

    py tools/q/q.py ours COMMAND ...        (S110 QTEXT: tools/q/qsym.py; a whole-word, case-sensitive `git grep` of the name over the files on disk, ~0.3-0.8 s)

  def NAME            where it is declared / defined (definition-shaped lines: function, class / struct / enum / namespace, using, #define, variable / field, enumerator)
  refs NAME           every non-definition line: calls, reads, writes, comments, strings (each tagged), grouped by enclosing function, with its #if chain
  callers NAME        the code lines that call it or name it (call / ref), grouped by function; comment / string lines are only counted
  writes NAME         the code lines that store it (= op= ++ -- [i]= .store / .exchange / .fetch_* / container mutators; a class-type `s_ = ...` too)
  reads NAME          the other code lines (a compound op= is both)
  callees FUNC        the names followed by ( in FUNC's body (text: not resolved to symbols)
  fn FUNC             one screen: where (line-endline), callers, the names it calls, REXCVAR switches read, log strings
  body FUNC           the function's lines (every function of that name, up to 12; --limit N lines each)
  outline FILE        a C++ file's namespaces / types / functions with line-endline (a .py / .ps1 file: the python ast / ps1 scan)
  enclosing FILE:LINE the function holding that line
  covers FILE         the unit .cpp files that include it (the text include graph, transitive), where a CMake list names each, and the scripts that name it
  info                what the text form is (no index to describe)
  update              a no-op: the index is gone (kept so an old hook line exits 0)
  line "<tag or log line>"  the src printf / Log site(s) that print a log line and every tools\\ regex / string that parses or names it
  lit VALUE           every src / tools / probes file holding the literal (integer: hex + decimal forms, word-bounded; else fixed text); --tools: only tools text
  includes FILE / includers FILE   the direct #include lines of a C++ file / every file that includes it (.inc too, a quoted glob or several files in one walk)
  runs FILE|EXE       the tools / config script lines that mention it (marked invoke); env NAME  every read / set of an environment variable
  cvar NAME           a cvar: every REXCVAR_DEFINE_* (ours + the SDK's), every REXCVAR_GET / SET / by-name string use, every `--NAME=value` in tools / routes
  glsl NAME / fxreg [--dupes] / str LITERAL / census FIELD [--in PATH] [--run N] / hooks [ADDR|NAME]   (qgl.py, qcensus.py, qx6.py)
  text W1 W2 [--any] [-E] [--docs] [--branch REF]   words on one line (OR, a regex, the docs instead of code, a git revision)

NAME: a name or `Scope::name`. The tail is matched as a whole word, CASE-SENSITIVE (Hold is not hold). `Scope::name` keeps the hits whose line, enclosing
namespace / class / function qualifier, or file names Scope; the others are counted ("N lines ... not shown"), not hidden. A common name (Read, Get) is noisy:
qualify it, narrow with --in PATH, or `--limit N`. Every answer says what it cut and the exact call for the rest.
A name with no C++ text hit falls through to the tools python / ps1 index (`--tools` asks it first), then cvars, shader text and `q find`'s probes.
GONE with the index (exit 2, the message names the text form): rows, diff, `--branch` on the symbol verbs (text / lit keep it), `--via`. A site carries no build
(ON / OFF) tag: a line under an #if says its condition.
--json on any command (the shape of the old index answers: {'symbol', 'matches': [{'qname', 'file', 'line', 'sites': [{'func', 'file', 'lines', 'kinds', 'guards'}], 'n'}]});
--limit N (default 30 groups); --all; --in PATH; --worktree PATH = the text of another checkout; --db is accepted and ignored.
"""
import contextlib
import argparse
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


class QOError(Exception):
    pass


def _main_checkout(root):
    """The main checkout of a git worktree (via the shared .git dir), or None."""
    if root in _MAIN_CACHE:       # S105 QC: one git subprocess per root per process (is_worktree and callers asked: ~40 ms each)
        return _MAIN_CACHE[root]
    try:
        _MAIN_CACHE[root] = _main_checkout_uncached(root)
    except Exception:             # a transient git failure is answered None this time only, never remembered (QC L5)
        return None
    return _MAIN_CACHE[root]


_MAIN_CACHE = {}


def _main_checkout_uncached(root):
    import subprocess
    r = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        raise OSError('git rev-parse failed')
    common = r.stdout.strip()
    if not common:
        return None
    common = os.path.abspath(os.path.join(root, common))
    main = os.path.dirname(common) if os.path.basename(common) == '.git' else None
    return main if main and os.path.normcase(main) != os.path.normcase(os.path.abspath(root)) else None


def is_worktree(root=None):
    """True inside a git worktree (not the main checkout)."""
    return _main_checkout(root or ROOT) is not None


def again_call(args, extra='--all'):
    """the exact q call that repeats this question with `extra` (S108 FD: every trim line names it)"""
    import qcap
    cmd, arg = getattr(args, 'cmd', None), getattr(args, 'arg', None)
    if not cmd or arg is None:
        return None
    inn = f' --in {qcap.requote([args.in_path])}' if getattr(args, 'in_path', None) else ''
    return f'q ours {cmd} {qcap.requote([arg])} {extra}{inn}'


TEXT_CMDS = ('cvar', 'line', 'lit', 'includes', 'includers', 'hooks', 'text', 'glsl', 'fxreg', 'str', 'census', 'env', 'runs')   # S106 QM: + env, runs (qm.py)


def _with_kw(fn, what, kw, *a, **base_kw):
    """call fn with lane D's keyword arguments (rev / docs / regex / any_) only when they are set; a checkout whose qx6 / qtext predates them says so in one line"""
    import inspect
    try:
        params = inspect.signature(fn).parameters
    except (TypeError, ValueError):
        params = {}
    missing = [k for k in kw if k not in params and not any(p.kind == p.VAR_KEYWORD for p in params.values())]
    if missing:
        flag = {'rev': '--branch', 'docs': '--docs', 'regex': '-E', 'any_': '--any'}
        raise QOError(f'{what} {" ".join(flag.get(k, k) for k in missing)}: this checkout\'s {fn.__module__}.{fn.__name__} does not take '
                      f'{", ".join(missing)} yet (S108 lane D adds it)')
    return fn(*a, **base_kw, **kw)


def text_cmd_x6(args):
    import qx6
    try:
        if args.cmd == 'hooks':
            r = qx6.cmd_hooks(args.root, args.arg)
            pr = lambda r, a: qx6.print_hooks(r, a.limit)
        elif args.cmd == 'text':
            # S108: lane D's qx6.cmd_text takes rev (a git revision), docs (docs / root *.md / .claude / CMake text instead of code), regex (-E), any_ (OR);
            # they are passed only when asked, so a checkout without lane D's qx6 still answers the plain form
            kw = {k: v for k, v in (('rev', getattr(args, 'rev', None)), ('docs', getattr(args, 'docs', False)), ('regex', getattr(args, 'regex', False)),
                                    ('any_', getattr(args, 'any_', False))) if v}
            r = _with_kw(qx6.cmd_text, 'ours text', kw, args.root, [w for w in re.split(r'\s+', args.arg.strip()) if w], limit_files=args.limit, allhits=args.all)
            scope_answer(r, getattr(args, 'in_path', None))
            r['again'] = again_call(args, '--all')
            pr = lambda r, a: qx6.print_text(r)
    except qx6.XError as e:
        raise QOError(str(e))
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
        print_outside(r, args)
    return 0


def _summary(groups):
    import qsym
    return qsym.summary(groups)


def lazy_encl(args):
    """enclosing-function lookup for lit / line / str hits: the brace scan of qsym over the file's own text (one scan per file per call)"""
    import qsym
    return qsym.encl_func(args.root)


def scope_answer(r, spec):
    """S108 FD: --in PATH narrows an answer to the files whose path contains PATH; -> how many sites / hits it left out (also r['outside_in'])"""
    if not spec or not isinstance(r, dict):
        return 0
    import qcap
    cut = 0
    ms = r.get('matches')
    if isinstance(ms, list) and ms and all(isinstance(m, dict) and 'defined' in m and 'sites' not in m and 'callers' not in m and 'callees' not in m for m in ms):   # `def`: the definitions in PATH
        keep = [m for m in ms if qcap.in_scope(m.get('file', ''), spec)]
        cut += len(ms) - len(keep)
        r['matches'] = keep
    for m in (r.get('matches') if isinstance(r.get('matches'), list) else []):
        if not isinstance(m, dict):
            continue
        for key in ('sites', 'address_taken', 'callers'):
            if isinstance(m.get(key), list):
                keep = [s for s in m[key] if qcap.in_scope(s.get('file', ''), spec)]
                cut += len(m[key]) - len(keep)
                m[key] = keep
        if 'sites' in m and 'summary' in m:
            m['summary'] = _summary(m['sites'])
        if 'sites' in m and 'n' in m:
            m['n'] = sum(len(s.get('lines', [])) for s in m['sites'])
    if isinstance(r.get('sites'), list):         # the tools python / ps1 index
        keep = [s for s in r['sites'] if qcap.in_scope(s.get('file', ''), spec)]
        cut += len(r['sites']) - len(keep)
        r['sites'] = keep
        r['n'] = sum(len(s.get('lines', [])) for s in keep)
    if isinstance(r.get('hits'), list):          # text / lit
        keep = [h for h in r['hits'] if qcap.in_scope(h.get('file', ''), spec)]
        cut += len(r['hits']) - len(keep)
        r['hits'] = keep
        r['n'] = len(keep)
        r['files'] = len({h['file'] for h in keep})
    r['outside_in'] = cut
    r['in'] = spec
    return cut


def print_outside(r, args):
    if isinstance(r, dict) and r.get('in'):
        print(f'  (--in {r["in"]}: {r.get("outside_in", 0)} more outside it, not shown)')


def text_cmd_gl(args):
    """S103 QGL: glsl NAME / fxreg [--dupes] / str LITERAL (tools/q/qgl.py): this checkout's own files, no index, always current"""
    import qgl
    try:
        if args.cmd == 'glsl':
            r, pr = qgl.cmd_glsl(args.root, args.arg), qgl.print_glsl
        elif args.cmd == 'fxreg':
            r, pr = qgl.cmd_fxreg(args.root, dupes=args.dupes or args.arg == '--dupes'), qgl.print_fxreg
        else:
            r, pr = qgl.cmd_str(args.root, args.arg, lazy_encl(args), tools=args.tools), qgl.print_str
    except qgl.GError as e:
        raise QOError(str(e))
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
    return 0


def text_cmd_census(args):
    """S103 QCF: census FIELD (tools/q/qcensus.py): the format literal -> the counter it prints -> every write to it; this checkout's own files, no index"""
    import qcensus
    import qrunlog
    try:
        r = qcensus.cmd_census(args.root, args.arg, only_in=getattr(args, 'in_path', None), run=getattr(args, 'run', None))
    except (qcensus.CError, qrunlog.RunlogError) as e:
        raise QOError(str(e))
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        qcensus.print_census(r, args)
    return 0


def text_cmd(args):
    if args.cmd in ('env', 'runs', 'outline'):   # S106 QM (tools/q/qm.py); `outline` only for a .py / .ps1 file (the main() test)
        import qm
        try:
            return qm.run(args)
        except RuntimeError as e:
            raise QOError(str(e))
    if args.cmd in ('hooks', 'text'):
        return text_cmd_x6(args)
    if args.cmd in ('glsl', 'fxreg', 'str'):
        return text_cmd_gl(args)
    if args.cmd == 'census':
        return text_cmd_census(args)
    import qtext
    rev = getattr(args, 'rev', None)
    encl = (lambda rel, ln: '') if rev else lazy_encl(args)     # the index is not that revision's: no enclosing-function names for a REF
    if args.cmd == 'cvar':
        import qx5
        r, pr = qx5.cmd_cvar(args.root, args.arg), qx5.print_cvar
    elif args.cmd == 'line':
        r, pr = qtext.cmd_line(args.root, args.arg, encl), qtext.print_line
        if r is None:
            raise QOError('line: give a log tag like [scene][maps][e2] and / or the words of the line')
    elif args.cmd == 'lit':
        r, pr = _with_kw(qtext.cmd_lit, 'ours lit', {'rev': rev} if rev else {}, args.root, args.arg, encl, tools=args.tools), qtext.print_lit
        scope_answer(r, getattr(args, 'in_path', None))
        r['again'] = again_call(args, '--all')
    elif args.cmd == 'includers' and (len(args.arg.split()) > 1 or any(c in args.arg for c in '*?[')):   # S106 QM: a glob / several files (the .inc data files), one walk
        rels = sorted({x for part in args.arg.split() for x in (qtext.expand_glob(args.root, part) if any(c in part for c in '*?[') else [qtext.find_cpp(args.root, part)]) if isinstance(x, str)})
        if not rels:
            raise QOError(f'{args.arg}: the glob matches no file (name its folder: src/renderer/shaders/spv/fx_y*_data.inc)')
        r, pr = qtext.cmd_includers_many(args.root, rels), qtext.print_includers
    else:
        rel = qtext.find_cpp(args.root, args.arg)
        if not rel or isinstance(rel, list):
            raise QOError(f'{args.arg}: ' + ('ambiguous: ' + ', '.join(rel[:6]) if rel else 'no such C++ file under src / tools / probes'))
        r, pr = (qtext.cmd_includes(args.root, rel), qtext.print_includes) if args.cmd == 'includes' else (qtext.cmd_includers(args.root, rel), qtext.print_includers)
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
        print_outside(r, args)
    return 0


SYMBOL_CMDS = ('def', 'refs', 'callers', 'callees', 'writes', 'reads', 'fn', 'body', 'outline', 'enclosing', 'covers')
TOOLS_CMDS = ('def', 'refs', 'callers', 'callees', 'writes', 'reads', 'fn')
GONE = {
    'rows': 'rows: gone with the index (it listed who constructs / compares each enumerator by the AST). By text: `q ours writes kSlName`, `q ours reads kSlName`, `q ours refs kSlName`',
    'diff': 'diff: gone with the index (the branch overlay). A branch\'s changes: `git diff REF --stat`; its text: `q ours text WORD --branch REF`, `q ours lit VALUE --branch REF`',
}


def _emit(r, pr, args):
    if args.json:
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, args)
        print_outside(r, args)


def main(argv=None, root=ROOT, db=None):
    argv = list(argv or [])
    if argv and argv[0] == 'update':
        print('q ours: there is no index any more (S110: q ours answers by text search over the files on disk): nothing to update')
        return 0
    branch = worktree = None          # --branch REF (text / lit only) | --worktree PATH (another checkout's files), anywhere before the command
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--branch', '--worktree') and i + 1 < len(argv):
            if a == '--branch':
                branch = argv[i + 1]
            else:
                worktree = argv[i + 1]
            del argv[i:i + 2]
        elif a.startswith(('--branch=', '--worktree=')):
            k, v = a.split('=', 1)
            if k == '--branch':
                branch = v
            else:
                worktree = v
            del argv[i]
        else:
            i += 1
    ap = argparse.ArgumentParser(prog='q ours', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--json', action='store_true')
    common.add_argument('--limit', type=int, default=30)
    common.add_argument('--db', default=db, help='accepted and ignored (there is no index)')
    common.add_argument('--tools', action='store_true', help='look in the tools python / ps1 index first (default: only when the C++ text has no such name)')
    common.add_argument('--all', action='store_true', help='every line / site (default: --limit groups, 3 per file for lit, 6 line numbers per file for cvar)')
    common.add_argument('--root', default=root)
    common.add_argument('--in', dest='in_path', default=None,
                        help='narrow the answer to files whose path contains PATH (separators / case ignored): refs callers callees writes reads text lit, '
                             'the tools index too; census: only writers in such files')
    sub = ap.add_subparsers(dest='cmd', required=True)
    for n in ('def', 'refs', 'callers', 'callees', 'writes', 'reads', 'fn', 'covers'):
        p = sub.add_parser(n, parents=[common]); p.add_argument('arg')
        if n in ('writes', 'reads'):
            p.add_argument('--via', type=int, nargs='?', const=2, default=0, metavar='N', help='gone with the index (exit 2)')
    for n in ('line', 'lit', 'includes', 'includers', 'cvar', 'glsl', 'str', 'env', 'runs'):
        p = sub.add_parser(n, parents=[common]); p.add_argument('arg', nargs='+' if n == 'includers' else None)   # includers takes several files (an unquoted glob the shell expanded)
    for n, lim in (('body', 400), ('outline', 300), ('enclosing', 30)):
        p = sub.add_parser(n, parents=[common]); p.add_argument('arg'); p.set_defaults(limit=lim)
    p = sub.add_parser('census', parents=[common]); p.add_argument('arg', nargs='+')
    p.add_argument('--run', default=None, help='census: also print the field\'s values per window from this run log (number or path)')
    p = sub.add_parser('hooks', parents=[common]); p.add_argument('arg', nargs='?', default='')
    p = sub.add_parser('fxreg', parents=[common]); p.add_argument('arg', nargs='?', default=''); p.add_argument('--dupes', action='store_true')
    p = sub.add_parser('text', parents=[common]); p.add_argument('arg', nargs='+')
    p.add_argument('--any', dest='any_', action='store_true', help='any of the words on a line (OR); default: all of them')
    p.add_argument('-E', dest='regex', action='store_true', help='the words are regular expressions')
    p.add_argument('--docs', action='store_true', help='search docs, root *.md, .claude, CMakeLists.txt and cmake instead of the code')
    p = sub.add_parser('rows', parents=[common]); p.add_argument('arg', nargs='?', default='')
    p = sub.add_parser('diff', parents=[common]); p.add_argument('arg', nargs='?', default='')
    sub.add_parser('info', parents=[common])
    args = ap.parse_args(argv)
    if getattr(args, 'in_path', None):      # an absolute / ./ --in matched nothing (qcap.scope_spec)
        import qcap
        args.in_path = qcap.scope_spec(args.in_path, args.root)
    if isinstance(getattr(args, 'arg', None), list):
        args.arg = ' '.join(args.arg)
    if isinstance(getattr(args, 'arg', None), str) and args.arg.startswith('$') and args.cmd in TOOLS_CMDS + ('body',):
        args.arg = args.arg.lstrip('$')   # a PowerShell variable written with its sigil: the text holds the bare name
    try:
        if args.cmd in TEXT_CMDS or (args.cmd == 'outline' and args.arg.lower().endswith(('.py', '.ps1', '.psm1'))):
            if branch and args.cmd not in ('text', 'lit'):
                raise QOError(f'{args.cmd} reads the files of a checkout, not a git ref: --branch is not supported here; use --worktree PATH (or run it in that checkout)')
            if branch:
                args.rev = branch          # text / lit at a git revision (qx6 / qtext `rev` argument)
            if worktree:
                if not os.path.isdir(worktree):
                    raise QOError(f'--worktree {worktree}: not a folder')
                args.root = os.path.abspath(worktree)
            return text_cmd(args)
        if args.cmd in GONE:
            raise QOError(GONE[args.cmd])
        if branch:
            raise QOError(f'{args.cmd} --branch: gone with the index (the branch overlay). Text / lit take --branch REF; for the rest, run it in that branch\'s checkout / worktree')
        if getattr(args, 'via', 0):
            raise QOError('--via: gone with the index (it followed callers of the writing functions). By text: q ours callers FUNC for each function the answer names')
        if worktree:
            if not os.path.isdir(worktree):
                raise QOError(f'--worktree {worktree}: not a folder')
            args.root = os.path.abspath(worktree)
        if args.cmd == 'info':
            print(f'q ours: plain text search (tools/q/qsym.py) over src, tools, probes, android of {os.path.abspath(args.root)}: whole word, case-sensitive, the files on disk, no index to build or go stale')
            print('  the verbs: ' + ' '.join(SYMBOL_CMDS) + ' + the text verbs (q ours -h); gone: rows diff --branch (symbol verbs) --via')
            return 0
        sys.path.insert(0, HERE)
        import qsym
        import qtools
        want_tools = getattr(args, 'tools', False) or (args.cmd in TOOLS_CMDS + ('body',) and qtools.looks_scripty(args.root, args.arg, args.cmd))
        if want_tools and args.cmd in TOOLS_CMDS + ('body',):
            got = qtools.answer(args.cmd, args.arg, args, args.root)       # `q py` / --tools asks the tools index BEFORE the C++ text
            if got:
                r, pr = got
                scope_answer(r, getattr(args, 'in_path', None))
                _emit(r, pr, args)
                return 0
        if args.cmd == 'covers':
            tf = qtools.find_file(args.root, args.arg)
            if tf and not isinstance(tf, list) and not tf.endswith(qsym.CPP):
                r, pr = qtools.cmd_script_covers(tf, args.root, qsym.scripts_naming), qtools.print_script_covers
                _emit(r, pr, args)
                return 0
        if args.cmd in ('refs', 'def'):   # a literal address given for a symbol: the named constants that hold it (kCharSlotTable = 0x836F38D0), then their refs
            import qx6
            if qx6.is_addr_arg(args.arg):
                hold = qx6.addr_holders(args.root, args.arg)
                if hold:
                    names = []
                    for hn, hf, hl, htx in hold:
                        if hn not in names:
                            names.append(hn)
                    hr = {'address': args.arg, 'held_in': [{'name': a_, 'file': b_, 'line': c_, 'text': d_} for a_, b_, c_, d_ in hold], 'matches': []}
                    for hn in names[:12]:
                        try:
                            hr['matches'].append(qsym.answer(args.cmd, hn, args.root)[0])
                        except qsym.SymError as e2:
                            hr['matches'].append({'symbol': hn, 'error': str(e2)})
                    if args.json:
                        print(json.dumps(hr, indent=1, default=str))
                        return 0
                    print(f'{args.arg} is held in {len(hold)} named constant(s) in {len({h[1] for h in hold})} file(s) (a `NAME = {args.arg}` line; `q ours lit {args.arg}` = every literal use):')
                    for hn, hf, hl, htx in hold:
                        print(f'  {hn}  {hf}:{hl}   {htx}')
                    for mr in hr['matches']:
                        if 'error' in mr:
                            print(f'  ({mr["symbol"]}: {mr["error"]})')
                        else:
                            qsym.PRINTERS[args.cmd](mr, args)
                    if len(names) > 12:
                        print(f'  ... {len(names) - 12} more constant name(s): q ours refs NAME')
                    return 0
        try:
            r, pname = qsym.answer(args.cmd, args.arg, args.root, args.limit)
        except qsym.SymError as e:
            msg = str(e)
            if args.cmd == 'body' or 'no such symbol' in msg or 'no function body' in msg:
                # the C++ text has no such name: a python / ps1 name of the tools, a cvar, shader text, then what `q find` found elsewhere
                def cvar_answer(verified):
                    import qx5
                    if not (verified or qsym.sdk_defines_cvar(args.root, args.arg)):
                        return False
                    cr = qx5.cmd_cvar(args.root, args.arg)
                    tl = [f'{rf}:{d[3]} ({d[2]} {d[0]})' for rf, d in qtools.resolve(qtools.load(args.root), args.arg)[0]]
                    if args.json:
                        cr['also_tools_symbol'] = tl
                        print(json.dumps(cr, indent=1, default=str))
                    else:
                        print(f'q ours: ({args.arg} is a cvar: REXCVAR_DEFINE_*, not a C++ symbol)', file=sys.stderr)
                        if tl:
                            print(f'q ours: also a tools symbol: {"; ".join(tl[:4])}{" ..." if len(tl) > 4 else ""}  (--tools asks the tools python / ps1 index first)', file=sys.stderr)
                        qx5.print_cvar(cr, args)
                    return True
                cvar_cmd = args.cmd in ('def', 'refs', 'writes', 'reads') and 'no such symbol' in msg
                if cvar_cmd and 'it is a cvar' in msg and cvar_answer(True):      # the text showed the REXCVAR_DEFINE_*( line: a cvar is a macro, not a symbol
                    return 0
                if args.cmd in TOOLS_CMDS + ('body',):
                    got = qtools.answer(args.cmd, args.arg, args, args.root)
                    if got:
                        r, pr = got
                        scope_answer(r, getattr(args, 'in_path', None))
                        if not args.json:
                            print('q ours: (a script symbol: tools python / ps1 index)', file=sys.stderr)
                        _emit(r, pr, args)
                        return 0
                if cvar_cmd and cvar_answer(False):      # an SDK cvar (not defined in our text)
                    return 0
                if args.cmd in ('def', 'refs') and 'no such symbol' in msg:      # a shader block field (Param1)
                    import qx6
                    sr = qx6.shader_hits(args.root, args.arg)
                    if sr['n']:
                        if args.json:
                            print(json.dumps(sr, indent=1, default=str))
                        else:
                            qx6.print_shader_hits(sr, args.limit)
                        return 0
                if 'no such symbol' in msg:      # never a dead end: what the file / text / docs / fact probes found for the name
                    import qfind
                    alt = qfind.elsewhere(args.root, args.arg, getattr(args, 'in_path', None))
                    if args.json:
                        print(json.dumps({'error': msg, 'elsewhere': alt}))
                        return 2
                    print(f'q ours: {msg} (also tried: the tools python / ps1 index, cvars, shader text)', file=sys.stderr)
                    for ln in alt:
                        print(ln)
                    return 2
            raise QOError(msg)
        scope_answer(r, getattr(args, 'in_path', None))
        if args.cmd in TOOLS_CMDS and not any(m.get('sites') or m.get('callers') or m.get('defined') is not None and m.get('file') for m in r.get('matches', [])):
            # no C++ code site or definition (the name only in comments / strings, or only OUTSIDE --in): a tools name? (QTEXT review: a C++ comment naming a
            # python function used to answer "0 sites" and hide its python callers)
            got = qtools.answer(args.cmd, args.arg, args, args.root)
            if got:
                r2, pr2 = got
                scope_answer(r2, getattr(args, 'in_path', None))
                if r2.get('sites') or r2.get('matches'):
                    if not args.json:
                        print('q ours: (no C++ code line has this name' + (' inside --in' if getattr(args, 'in_path', None) else '') + ': the tools python / ps1 index)', file=sys.stderr)
                    _emit(r2, pr2, args)
                    return 0
        _emit(r, qsym.PRINTERS[pname], args)
        return 0
    except QOError as e:
        if getattr(args, 'json', False):
            print(json.dumps({'error': str(e)}))
        else:
            print(f'q ours: {e}', file=sys.stderr)
        return 2
    except BrokenPipeError:
        import qm
        qm.silence_stdout()
        return 0
    except OSError as e:     # Windows reports a write to a closed pipe (`q ... | head`) as OSError [Errno 22], not BrokenPipeError
        import qm
        if not qm.closed_pipe(e):
            raise
        qm.silence_stdout()
        return 0


if __name__ == '__main__':
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(errors='replace')
        except (AttributeError, ValueError):
            pass
    sys.path.insert(0, HERE)
    import qm
    sys.exit(qm.guarded(main, sys.argv[1:]))
