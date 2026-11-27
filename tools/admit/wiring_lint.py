r"""Lint: every wide compile loop admits itself through the compile queue, and the table tells the truth (lane CQW; docs\agents\admit_wiring.md).
    py -I tools\admit\wiring_lint.py [--root DIR] [--plant] [--write]

Rules (each a FAIL):
  1. PowerShell: every compile loop under tools\ of 8 or more wide (ForEach-Object -Parallel, the -ThrottleLimit on the same line, a continuation line or
     after the block) takes its count from `Request-BuildParallel -Want N`, its script dot-sources tools\admit\Admit.ps1, and the loop sits in
     `try { ... } finally { Release-BuildParallel }`. A compile loop whose -ThrottleLimit is a variable, and a Start-Job / Start-ThreadJob compile
     loop, fail too (their width cannot be read: use ForEach-Object -Parallel with Request-BuildParallel). A compile block is one that runs clang / g++ /
     cl.exe, a $using: compiler variable ($using:cxx0, $using:ccExe, $using:clang ...), or passes both -c and -o. "not_yet_wired" (the ratchet) is empty.
  2. Python: every ThreadPoolExecutor / ProcessPoolExecutor / multiprocessing.Pool whose submitted function (or what it calls in the same file) runs a
     compiler is opened in the same `with` as an admit_py hold. Any finding in a file listed in "exempt" (with its reason: tier 2, or not a compile) is
     not a FAIL.
  3. tools\admit\Admit.ps1 has no top-level statement (only functions: dot-sourcing it runs nothing).
  4. The table (tools\admit\tool_build_parallel.json "scripts", GENERATED: --write) equals this scan: one row per tools\**\build*.ps1, count = its widest
     compile loop (the -Want of a wired loop, else its -ThrottleLimit, PowerShell's default 5 when none), 1 with no compile loop; wired = every compile
     loop uses Request-BuildParallel. A `cmake --build` / `ninja` line counts its literal -j; without one: 1 and a NOTE (tier 2). The table is for this
     lint and the docs only: nothing reads it at build time.
Every run also lints PLANTED faults (a 16-wide clang loop with no helper; a ThrottleLimit on a continuation line with a $using:compiler block; a variable
ThrottleLimit; a Start-ThreadJob compile loop; an unwired Python compile pool; a top-level statement in a copy of Admit.ps1) and FAILs if one is not caught.
--plant: lint the tree WITH the plants and report as usual (exit 1 = caught)."""
import argparse
import ast
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TABLE = 'tools/admit/tool_build_parallel.json'
LOOP = re.compile(r'ForEach-Object\b(?P<head>(?:[^\n]|`\r?\n)*?)-Parallel\s*(?=\{)', re.I)   # (the head may hold braces: build_rig's $(if ...) count)
JOB = re.compile(r'\bStart-(?:Thread)?Job\b(?:[^\n{]|`\r?\n)*?(?=\{)', re.I)
TL = re.compile(r'-ThrottleLimit\s+', re.I)
COMPILE = re.compile(r'clang|\bg\+\+|\bgcc\b|\bcl(?:\.exe)?\s|\$using:(?:cxx|cc|clang|compiler|cl|gcc)\w*|\s-c\s[^\n]*\s-o\s|\s-o\s[^\n]*\s-c\s', re.I)
DOT = re.compile(r"""\.\s+\(Join-Path\s+\$PSScriptRoot\s+['"]\.\.\\admit\\Admit\.ps1['"]\)""", re.I)
FINALLY = re.compile(r'\}\s*\}\s*finally\s*\{\s*Release-BuildParallel\s*\}', re.I)
WANT = re.compile(r'Request-BuildParallel\s+-Want\s+(.*)', re.I)
CMAKE = re.compile(r'(?m)^[^#\n]*?(?:\bcmake(?:\.exe)?[\'"]?\s+--build\b|&\s*[\'"]?\$?\w*ninja(?:\.exe)?[\'"]?\s)', re.I)
JOBS = re.compile(r'(?:\s-j\s*|--parallel\s+)(\d+)\b')
PY_POOL = re.compile(r'^(?:ThreadPoolExecutor|ProcessPoolExecutor|Pool)$')
PY_COMPILE = re.compile(r"""\bCLANG\w*\b|clang(?:\+\+|-cl)?(?:\.exe)?['"]|\bcl\.exe\b|['"]-fsyntax-only['"]|['"]-c['"]""")
SUBMIT = ('map', 'submit', 'imap', 'imap_unordered', 'starmap', 'apply_async', 'map_async', 'starmap_async')

PLANTS_PS = {
    'tools/zz_plant/build_planted.ps1': "$srcs = @('a.cpp')\n$srcs | ForEach-Object -ThrottleLimit 16 -Parallel {\n  & $using:clang -c $_ -o ($_ + '.o')\n}\n",
    'tools/zz_plant/build_planted_cont.ps1': "$srcs | ForEach-Object `\n  -ThrottleLimit 12 `\n  -Parallel {\n  & $using:compiler @using:flags -c $_ -o ($_ + '.o')\n}\n",
    'tools/zz_plant/build_planted_var.ps1': "$n = 16\n$srcs | ForEach-Object -ThrottleLimit $n -Parallel {\n  & clang++ -c $_ -o ($_ + '.o')\n}\n",
    'tools/zz_plant/build_planted_job.ps1': "$jobs = foreach ($s in $srcs) { Start-ThreadJob -ScriptBlock { & clang++ -c $using:s -o ($using:s + '.o') } }\n",
}
PLANT_PY = ('tools/zz_plant/planted_pool.py',
            "import subprocess\nfrom concurrent.futures import ThreadPoolExecutor\nCLANG = 'clang++'\n\n"
            "def one(src):\n    return subprocess.run([CLANG, '-c', src, '-o', src + '.o']).returncode\n\n"
            "def main(srcs):\n    with ThreadPoolExecutor(max_workers=16) as ex:\n        return list(ex.map(one, srcs))\n")
PLANT_ADMIT = "$AdmitMaxHoldS = 900\nfunction Request-BuildParallel { param($Want) $Want }\n"
# S111 ADMITCALLERS: no production caller passes a queue limit (admit.exe refuses --timeout outside its selftest). The selftests that DO are listed.
TIMEOUT_PY = re.compile(r'\b_?admit(?:_py)?\.hold\([^\n]*\btimeout_s\s*=')
TIMEOUT_PS = re.compile(r'Request-BuildParallel\b[^\n]*\s-TimeoutS\b|\badmit(?:\.exe)?[\'"]?\s+(?:run|hold)\b[^\n]*--timeout\b', re.I)
TIMEOUT_OK = {'tools/harness/alone_selftest.ps1'}   # a selftest running admit in its test mode
PLANT_TIMEOUT_PY = ('tools/zz_plant/planted_timeout.py', "import admit_py as _admit\nwith _admit.hold('cc.tool', 4, timeout_s=120) as n:\n    pass\n")
PLANT_TIMEOUT_PS = ('tools/zz_plant/planted_timeout.ps1', "$n = Request-BuildParallel -Want 8 -TimeoutS 180\n")


def balanced(text, i, open_ch, close_ch):
    """text[i] == open_ch -> the index just past its matching close_ch (naive: strings are not skipped; good enough for these loops)."""
    depth = 0
    for j in range(i, len(text)):
        if text[j] == open_ch:
            depth += 1
        elif text[j] == close_ch:
            depth -= 1
            if depth == 0:
                return j + 1
    return len(text)


def code_only(text):
    return '\n'.join(re.sub(r'(^|\s)#.*$', r'\1', ln) for ln in text.splitlines())


def _throttle(head):
    t = TL.search(head)
    if not t:
        return None
    k = t.end()
    if k < len(head) and head[k] == '(':
        return head[k:balanced(head, k, '(', ')')]
    w = head[k:].split()
    return w[0] if w else ''


def loops(text):
    """-> [(line, throttle text or None, block text)] for every ForEach-Object -Parallel in a .ps1 (the -ThrottleLimit may sit before the block, on a
    continuation line, or after the block's closing brace on the same line)."""
    out = []
    for m in LOOP.finditer(text):
        b = m.end()
        e = balanced(text, b, '{', '}')
        tail_end = text.find('\n', e)
        tail = text[e:tail_end if tail_end >= 0 else len(text)]
        tl = _throttle(m.group('head')) or _throttle(tail)
        out.append((text.count('\n', 0, m.start()) + 1, tl, text[b:e]))
    return out


def job_loops(text):
    out = []
    for m in JOB.finditer(text):
        b = m.end()
        out.append((text.count('\n', 0, m.start()) + 1, text[b:balanced(text, b, '{', '}')]))
    return out


def max_int(s):
    nums = [int(x) for x in re.findall(r'\b\d+\b', s or '')]
    return max(nums) if nums else 5   # no -ThrottleLimit: PowerShell's default of 5


def loop_count(tl):
    m = WANT.search(tl or '')
    return max_int(m.group(1) if m else tl)


def compile_loops(text):
    return [x for x in loops(text) if COMPILE.search(x[2])]


def is_build_script(rel):
    p = Path(rel)
    return rel.startswith('tools/') and p.name.lower().startswith('build') and p.suffix.lower() == '.ps1'


def scan_entry(text):
    """-> ({'count': N, 'wired': bool}, note or '') for one build script's text."""
    ls = compile_loops(text)
    if ls:
        return {'count': max(loop_count(tl) for _ln, tl, _b in ls), 'wired': all('Request-BuildParallel' in (tl or '') for _ln, tl, _b in ls)}, ''
    code = code_only(text)
    js = []
    for m in CMAKE.finditer(code):
        nl = code.find('\n', m.start())
        j = JOBS.search(code[m.start():nl if nl >= 0 else len(code)])
        js.append(int(j.group(1)) if j else None)
    if js and all(j is not None for j in js):
        return {'count': max(js), 'wired': False}, ''
    if js:
        return {'count': 1, 'wired': False}, 'a cmake --build / ninja line without a literal -j (tier 2: its parallelism cannot be read): counted as 1'
    return {'count': 1, 'wired': False}, ''


def admit_top_level(text):
    """The statements at the top level of Admit.ps1 that are not function definitions (each one runs in the caller's scope when it dot-sources)."""
    bad = []
    depth = 0
    i = 0
    code = code_only(text)
    lines = code.split('\n')
    for n, ln in enumerate(lines, 1):
        s = ln.strip()
        if depth == 0 and s and not re.match(r'function\s+[\w-]+', s, re.I) and s not in ('{', '}'):
            bad.append(f'line {n}: {s[:60]}')
        depth += ln.count('{') - ln.count('}')
        i += 1
    return bad


def py_pools(rel, text):
    """-> [(line, wired)] for every executor / pool in a python file whose submitted function (or what it calls in this file) runs a compiler."""
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return []
    funcs = {}
    for n in ast.walk(tree):
        if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)):
            funcs.setdefault(n.name, []).append(n)

    def compiles(node, seen):
        src = ast.get_source_segment(text, node) or ''
        if PY_COMPILE.search(src):
            return True
        for c in ast.walk(node):
            if isinstance(c, ast.Call) and isinstance(c.func, ast.Name) and c.func.id in funcs and c.func.id not in seen:
                seen.add(c.func.id)
                if any(compiles(f, seen) for f in funcs[c.func.id]):
                    return True
        return False

    def callee(arg):
        if isinstance(arg, ast.Lambda):
            return [arg]
        if isinstance(arg, ast.Name) and arg.id in funcs:
            return funcs[arg.id]
        return []

    out = []
    for w in ast.walk(tree):
        if not isinstance(w, ast.With):
            continue
        pools, held = [], False
        for it in w.items:
            c = it.context_expr
            if isinstance(c, ast.Call):
                name = c.func.attr if isinstance(c.func, ast.Attribute) else c.func.id if isinstance(c.func, ast.Name) else ''
                if PY_POOL.match(name or '') and isinstance(it.optional_vars, ast.Name):
                    pools.append((it.optional_vars.id, c.lineno))
                if name == 'hold':
                    held = True
        for var, line in pools:
            fns = []
            for c in ast.walk(w):
                if isinstance(c, ast.Call) and isinstance(c.func, ast.Attribute) and c.func.attr in SUBMIT and isinstance(c.func.value, ast.Name) \
                        and c.func.value.id == var and c.args:
                    fns += callee(c.args[0])
            if any(compiles(f, set()) for f in fns):
                out.append((line, held))
    return out


def read_tree(root, extra=None):
    files, probs = {}, []
    for pat in ('*.ps1', '*.py'):
        for p in (Path(root) / 'tools').rglob(pat):
            rel = p.relative_to(root).as_posix()
            try:
                files[rel] = p.read_text(encoding='utf-8', errors='replace')
            except OSError as e:
                probs.append(f'{rel}: cannot read ({e})')
    files.update(extra or {})
    return files, probs


def scan(files):
    """-> (table scripts {rel: entry}, notes [str])."""
    table, notes = {}, []
    for rel in sorted(files):
        if rel.endswith('.ps1') and is_build_script(rel):
            ent, note = scan_entry(files[rel])
            table[rel] = ent
            if note:
                notes.append(f'{rel}: {note}')
    return table, notes


def lint(root, extra=None):
    """-> (problems, notes, scanned table). extra: {rel: text} of virtual files linted as if they were in the tree (the plants)."""
    root = Path(root)
    t = json.loads((root / TABLE).read_text(encoding='utf-8'))
    known = {k.lower(): int(v) for k, v in (t.get('not_yet_wired') or {}).items()}
    exempt = {k.lower() for k in (t.get('exempt') or {})}
    files, probs = read_tree(root, extra)
    table, notes = scan(files)
    real = {k: v for k, v in table.items() if k not in (extra or {})}
    committed = t.get('scripts', {})
    if committed != real:
        diff = sorted(set(committed) ^ set(real)) + sorted(k for k in set(committed) & set(real) if committed[k] != real[k])
        probs.append(f'{TABLE} "scripts" differs from the scan ({len(diff)}: {", ".join(diff[:6])}): run  py -I tools\\admit\\wiring_lint.py --write')
    seen_known = set()
    for rel, text in sorted(files.items()):
        lo = rel.lower()
        if not rel.startswith('tools/admit/') and rel not in TIMEOUT_OK:
            for m in (TIMEOUT_PY if rel.endswith('.py') else TIMEOUT_PS).finditer(code_only(text)):
                probs.append(f'{rel}: passes a queue limit to admit (timeout_s= / -TimeoutS / --timeout): the queue waits until admitted and admit.exe refuses --timeout outside its selftest (S111 ADMITCALLERS): remove it')
                break
        if rel.endswith('.py'):
            if rel.startswith('tools/admit/'):
                continue
            for line, held in py_pools(rel, text):
                if not held and lo not in exempt:
                    probs.append(f'{rel}:{line}: a Python compile pool not opened under an admit_py hold (with admit_py.hold(kind, n) as n, Pool(n))')
            continue
        if rel == 'tools/admit/Admit.ps1' or rel.endswith('/Admit.ps1'):
            for b in admit_top_level(text):
                probs.append(f'{rel}: a top-level statement ({b}): Admit.ps1 holds functions only (dot-sourcing it must run nothing)')
            continue
        ls = compile_loops(text)
        wired_loops = [x for x in ls if 'Request-BuildParallel' in (x[1] or '')]
        if wired_loops:
            if not DOT.search(text):
                probs.append(f'{rel}: calls Request-BuildParallel but does not dot-source tools\\admit\\Admit.ps1')
            if not FINALLY.search(text):
                probs.append(f'{rel}: its wired loop is not inside `try {{ ... }} finally {{ Release-BuildParallel }}` (the hold must end with the loop)')
        for ln, _b in job_loops(text):
            if COMPILE.search(_b) and lo not in exempt:
                probs.append(f'{rel}:{ln}: a Start-Job / Start-ThreadJob compile loop (its width cannot be read): use ForEach-Object -Parallel with Request-BuildParallel')
        for ln, tl, _b in ls:
            if 'Request-BuildParallel' in (tl or ''):
                continue
            if tl and tl.lstrip('(').startswith('$'):
                probs.append(f'{rel}:{ln}: a compile loop whose -ThrottleLimit is a variable ({tl}): use Request-BuildParallel -Want {tl}')
                continue
            n = max_int(tl)
            if n < 8:
                continue
            if lo in known:
                seen_known.add(lo)
                if n != known[lo]:
                    probs.append(f'{rel}:{ln}: a {n}-wide compile loop; "not_yet_wired" lists it at {known[lo]}')
            else:
                probs.append(f'{rel}:{ln}: a NEW {n}-wide compile loop without Request-BuildParallel: wire it (tools\\admit\\Admit.ps1, as the other build scripts)')
    for lo in sorted(set(known) - seen_known):
        probs.append(f'{lo}: listed in "not_yet_wired" but has no unwired compile loop of 8 or more now (wired or gone: drop the entry)')
    return probs, notes, table


def write_table(root, table):
    p = Path(root) / TABLE
    t = json.loads(p.read_text(encoding='utf-8'))
    t['scripts'] = {k: table[k] for k in sorted(table)}
    lines = ['{']
    keys = list(t.keys())
    for i, k in enumerate(keys):
        end = ',' if i < len(keys) - 1 else ''
        if k == 'scripts':
            rows = [f'    {json.dumps(r)}: {json.dumps(v)}' for r, v in t['scripts'].items()]
            lines.append('  "scripts": {\n' + ',\n'.join(rows) + '\n  }' + end)
        else:
            lines.append(f'  {json.dumps(k)}: {json.dumps(t[k])}{end}')
    lines.append('}')
    p.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def plants():
    d = dict(PLANTS_PS)
    d[PLANT_PY[0]] = PLANT_PY[1]
    d['tools/zz_plant/Admit.ps1'] = PLANT_ADMIT
    d[PLANT_TIMEOUT_PY[0]] = PLANT_TIMEOUT_PY[1]
    d[PLANT_TIMEOUT_PS[0]] = PLANT_TIMEOUT_PS[1]
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=str(HERE.parent.parent))
    ap.add_argument('--plant', action='store_true')
    ap.add_argument('--write', action='store_true', help='rewrite the table\'s "scripts" from the scan, then lint')
    a = ap.parse_args()
    if a.write:
        files, _p = read_tree(a.root)
        write_table(a.root, scan(files)[0])
        print(f'wrote {TABLE} "scripts" from the scan')
    pl = plants()
    probs, notes, table = lint(a.root, pl if a.plant else None)
    for n in notes:
        print('NOTE ' + n)
    for p in probs:
        print('FAIL ' + p)
    if a.plant:
        print(f'(--plant: {len(pl)} planted files added) ' + ('caught: exit 1' if probs else 'NOT caught: the lint is blind'))
        return 1 if probs else 0
    got = lint(a.root, pl)[0]
    missed = [r for r in pl if not any(r in p for p in got)]
    if missed:
        print(f'FAIL wiring_lint is blind: planted fault(s) not caught: {", ".join(missed)}')
        return 1
    print(f'PLANT caught: all {len(pl)} planted faults (16-wide loop, continuation-line ThrottleLimit, variable ThrottleLimit, Start-ThreadJob, Python pool, Admit.ps1 top level, a caller passing timeout_s / -TimeoutS)')
    n = len(table)
    wired = sum(1 for v in table.values() if v['wired'])
    ones = sum(1 for v in table.values() if v['count'] == 1)
    um = sorted(k for k, v in table.items() if not v['wired'] and v['count'] > 1)
    print(f'table: {n} build scripts, {wired} wired, {ones} at 1, {len(um)} unwired with count > 1 (all under 8 or cmake -j, no hold in this piece): '
          + ', '.join(f'{k} {table[k]["count"]}' for k in um))
    if probs:
        print(f'FAIL wiring_lint: {len(probs)} problem(s)')
        return 1
    print('PASS wiring_lint: the table matches the scan; every compile loop of 8+ and every Python compile pool admits itself; Admit.ps1 has no top level')
    return 0


if __name__ == '__main__':
    sys.exit(main())
