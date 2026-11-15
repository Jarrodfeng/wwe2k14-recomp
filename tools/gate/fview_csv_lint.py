r"""S102 lane G5 (class fix for review F1): a python check that runs fview with --hash-frames must not read the hash csv alone.
  py -I tools\gate\fview_csv_lint.py [--root DIR] [--selftest]
fview writes the --hash-frames file BEFORE it returns exit 4 on validation errors, so a check that only compares csv files passes while the validation layer reports errors (the batch's missing barrier
proof was blind to sync faults for exactly this reason). Rule, per file under tools\ (static, by the python parser): every top-level function or class that holds a '--hash-frames' argument must also
hold (a) a read of the run's exit code (a name or attribute `returncode` / `rc`, or a parameter or variable named `rc`) AND (b) a parse of the count: a string containing `validation errors`.
Waiver: a comment `# fview-lint: waive <reason>` (a reason of 10+ characters) on the line of the '--hash-frames' argument or the line above it, for an arm that expects validation errors by design.
--selftest runs the rule on synthetic sources: csv only (must be flagged), csv + exit code + count (clean), a waiver with a reason (clean), a waiver without one (flagged).
Exit 0: no violations, 1: any. A file that does not parse is a violation (never silently skipped)."""
import argparse, ast, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
WAIVE_RE = re.compile(r'#\s*fview-lint:\s*waive\s+(\S.{9,})')


def scopes(tree):
    """top-level functions and classes (a method belongs to its class)."""
    return [n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))]


def check_source(src, name='<src>'):
    """-> list of (line, text) violations."""
    try:
        tree = ast.parse(src)
    except SyntaxError as e:
        return [(e.lineno or 0, 'does not parse: %s' % e)]
    lines = src.splitlines()
    out = []
    count_names = set()   # module-level names bound to a string holding `validation errors` (a compiled regex, a constant): a function that uses the name parses the count
    for top in tree.body:
        if isinstance(top, ast.Assign) and any(isinstance(n, ast.Constant) and isinstance(n.value, str) and 'validation errors' in n.value for n in ast.walk(top.value)):
            count_names |= {t.id for t in top.targets if isinstance(t, ast.Name)}
    funcs = [n for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
    flags = [n for n in ast.walk(tree) if isinstance(n, ast.Constant) and n.value == '--hash-frames']
    for f in flags:
        # the innermost function holding the argument (module-level code is its own scope): the exit code and the count must be read THERE, not by some other function of the file
        inside = [fn for fn in funcs if fn.lineno <= f.lineno <= (fn.end_lineno or fn.lineno)]
        scope = min(inside, key=lambda fn: (fn.end_lineno or 0) - fn.lineno) if inside else tree
        nodes = list(ast.walk(scope))
        has_rc = any((isinstance(n, ast.Name) and n.id in ('rc', 'returncode')) or (isinstance(n, ast.Attribute) and n.attr in ('rc', 'returncode')) or (isinstance(n, ast.arg) and n.arg == 'rc') for n in nodes)
        has_count = any(isinstance(n, ast.Constant) and isinstance(n.value, str) and 'validation errors' in n.value for n in nodes) or any(isinstance(n, ast.Name) and n.id in count_names for n in nodes)
        near = lines[max(0, f.lineno - 2):f.lineno]
        if any(WAIVE_RE.search(l) for l in near):
            continue
        if any('fview-lint' in l for l in near):
            out.append((f.lineno, "a 'fview-lint' marker without a reason of 10+ characters"))
            continue
        if not (has_rc and has_count):
            out.append((f.lineno, 'runs fview with --hash-frames but %s%s: read the exit code and parse `validation errors N` in the same function (or waive with `# fview-lint: waive <reason>`)' % (
                '' if has_rc else 'never reads the exit code', ('' if has_count else (' and ' if not has_rc else '') + 'never parses the validation count'))))
    return out


def selftest():
    csv_only = "import subprocess\ndef arm(exe, z, csv):\n    subprocess.run([exe, '--case', z, '--replay', '--hash-frames', csv])\n    return open(csv).read()\n"
    full = "import subprocess, re\ndef arm(exe, z, csv):\n    p = subprocess.run([exe, '--case', z, '--replay', '--hash-frames', csv], capture_output=True, text=True)\n    v = re.search(r'validation errors (\\d+)', p.stdout)\n    return p.returncode, v\n"
    waived = csv_only.replace("subprocess.run([exe, '--case'", "# fview-lint: waive the probe arm expects one validation error by design\n    subprocess.run([exe, '--case'")
    bare = csv_only.replace("subprocess.run([exe, '--case'", "# fview-lint: waive x\n    subprocess.run([exe, '--case'")
    cases = [('csv only', csv_only, True), ('csv + exit code + count', full, False), ('waiver with a reason', waived, False), ('waiver without a reason', bare, True)]
    bad = 0
    for label, src, must_flag in cases:
        flagged = bool(check_source(src))
        ok = flagged == must_flag
        bad += 0 if ok else 1
        print(('PASS  ' if ok else 'FAIL  ') + 'selftest %s: %s (expected %s)' % (label, 'flagged' if flagged else 'clean', 'flagged' if must_flag else 'clean'))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    bad = selftest()
    if args.selftest:
        return 1 if bad else 0
    tools = os.path.join(os.path.abspath(args.root), 'tools')
    checked = 0
    for d, _, files in os.walk(tools):
        for f in sorted(files):
            if not f.endswith('.py'):
                continue
            path = os.path.join(d, f)
            if os.path.abspath(path) == os.path.abspath(__file__):
                continue
            try:
                src = open(path, encoding='utf-8', errors='replace').read()
            except OSError as e:
                print('FAIL  cannot read %s: %s' % (path, e))
                bad += 1
                continue
            if '--hash-frames' not in src:
                continue
            checked += 1
            for line, text in check_source(src, path):
                print('FAIL  %s:%d: %s' % (os.path.relpath(path, args.root), line, text))
                bad += 1
    print('checked %d python checks that name --hash-frames' % checked)
    if checked < 5:
        print('FAIL  fewer than 5 checks found: the walk reads nothing')
        bad += 1
    print('RESULT ' + ('PASS' if not bad else 'FAIL (%d)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
