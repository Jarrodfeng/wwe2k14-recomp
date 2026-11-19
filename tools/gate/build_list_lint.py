r"""H2 / L4 BUILD-LIST LINT (S103 B2F: build_e5_unit.ps1 compiled live_map_rope_input.cpp but not live_map_rope.cpp, which defines what it calls: a link error found only when a builder ran it).

  py -I tools\gate\build_list_lint.py [--root DIR] [--verbose]    exit 1 on a violation
  py -I tools\gate\build_list_lint.py --selftest                   plants the B2F fault and a game-list fault on a copy

A BUILD LIST = the .cpp files one tool command compiles together: on a .ps1 / .bat the .cpp literals of one line that calls clang / cl; on a .py the .cpp literals of one list / call; plus CMakeLists.txt's
WWE2K14_SOURCES. Symbol level, cheap: for every listed file F (and the project headers it includes, one level) take each function name F CALLS (`Name(` word, not a keyword) and look for the .cpp under src\ or
tools\ that DEFINES it at namespace / class scope (`Name(...) {` at the start of a line, or `Class::Name(...) {`):
  1. a called function whose only definition is a .cpp that is NOT in the same list = ERROR (the list links short), unless some listed file or the listed headers define it (stubs, inline, header-only).
  2. a src\ .cpp that a tool list names, whose definitions the game's WWE2K14_SOURCES files call, but that is not in WWE2K14_SOURCES = ERROR (the game build lacks it).
Names must be 8+ characters and contain an uppercase letter or '_' (Init / size / Get would match everywhere) and be defined in exactly one .cpp under the scanned folders (an ambiguous name is skipped). BASELINE = known false
positives with their reason."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CPP_LIT = re.compile(r'''['"]([^'"\s]*\.cpp)['"]''')
CALL_RX = re.compile(r'(?<![\w:.>])(?:\w+::)*([A-Za-z_]\w{7,})\s*\(')
DEF_RX = re.compile(r'^[A-Za-z_][\w:<>, \t\*&~]*?\b(?:(\w+)::)?(~?[A-Za-z_]\w{7,})\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{', re.M)
INC_RX = re.compile(r'#\s*include\s*"([^"]+)"')
KEYWORDS = {'sizeof', 'static_cast', 'reinterpret_cast', 'const_cast', 'dynamic_cast', 'defined', 'alignof', 'decltype', 'static_assert', 'noexcept', 'unlikely', 'likely'}
BASELINE = {
    # (list label, called name): reason
    ('tools/scene/build_ks1_unit.ps1:8', 'StateFor'): 'ks1_unit.cpp defines its own file-static StateFor (line 66); d3_caster_test.cpp has an unrelated one (S103 KS1)',
}


def rd(p):
    try:
        return open(p, encoding='utf-8', errors='replace').read()
    except OSError:
        return ''


def norm(p):
    return p.replace('\\', '/').lstrip('./')


def gather_lists(root):
    """[(label, [repo-relative .cpp])]."""
    lists = []
    tools = os.path.join(root, 'tools')
    for d, dirs, files in os.walk(tools):
        dirs[:] = [x for x in dirs if x not in ('__pycache__', 'node_modules')]
        for f in files:
            full = os.path.join(d, f)
            rel = os.path.relpath(full, root).replace('\\', '/')
            if f.endswith(('.ps1', '.bat', '.cmd')):
                for i, ln in enumerate(rd(full).split('\n')):
                    if re.search(r'\bclang(\+\+)?\b|\bcl(\.exe)?\b\s', ln):
                        cp = [norm(m.group(1)) for m in CPP_LIT.finditer(ln)]
                        if cp:
                            lists.append((f'{rel}:{i + 1}', cp, os.path.dirname(rel)))
            elif f.endswith('.py'):
                t = rd(full)
                if 'clang' not in t and 'cl.exe' not in t:
                    continue
                for m in re.finditer(r'\[[^\[\]]*?\.cpp[^\[\]]*?\]', t):
                    lit = m.group(0)
                    if not re.search(r'clang|CLANG|\bCXX\b', lit) or not re.search(r"['\"]-o['\"]", lit) or re.search(r"-fsyntax-only|['\"]-c['\"]", lit):
                        continue   # only a command list that links (clang ... .cpp ... -o exe): a WIRING table of file names is not a build list
                    cp = [norm(x.group(1)) for x in CPP_LIT.finditer(lit)]
                    if cp:
                        lists.append((f'{rel}:{t[:m.start()].count(chr(10)) + 1}', cp, os.path.dirname(rel)))
    return lists


def resolve(root, cpp, base_dir):
    """repo-relative existing path of a listed literal ($repo\\src\\x.cpp, src/x.cpp, x.cpp next to the script, tools/...), else None."""
    c = cpp.replace('$repo/', '').replace('$PSScriptRoot/', base_dir + '/')
    c = re.sub(r'^\.\./\.\./', '', c)
    for cand in (c, base_dir + '/' + c, 'src/' + c, 'tools/' + c):
        if os.path.isfile(os.path.join(root, cand)):
            return cand
    return None


def cmake_sources(root):
    t = rd(os.path.join(root, 'CMakeLists.txt'))
    a = t.find('set(WWE2K14_SOURCES')
    if a < 0:
        return []
    b = t.find(')', a)
    out = []
    for ln in t[a:b].split('\n'):
        ln = ln.split('#')[0].strip()
        if ln.endswith('.cpp'):
            out.append(ln)
    return out


class Index:
    def __init__(self, root):
        self.root = root
        self.defs = {}   # name -> set of cpp rel paths defining it
        self.cache = {}
        for base in ('src', 'tools'):
            for d, dirs, files in os.walk(os.path.join(root, base)):
                dirs[:] = [x for x in dirs if x not in ('__pycache__', 'node_modules')]
                for f in files:
                    if f.endswith('.cpp'):
                        rel = os.path.relpath(os.path.join(d, f), root).replace('\\', '/')
                        for m in DEF_RX.finditer(self.text(rel)):
                            tx = self.text(rel)
                            if re.search(r'\bstatic\b', tx[tx.rfind(chr(10), 0, m.start(2)) + 1:m.start(2)]):
                                continue   # file-local: never satisfies another file
                            n = m.group(2)
                            if re.search(r'[A-Z_]', n):
                                self.defs.setdefault(n, set()).add(rel)

    def text(self, rel):
        if rel not in self.cache:
            self.cache[rel] = rd(os.path.join(self.root, rel))
        return self.cache[rel]

    def headers(self, rel):
        base = os.path.dirname(rel)
        out = []
        for inc in INC_RX.findall(self.text(rel)):
            for cand in (base + '/' + inc, 'src/' + inc):
                cand = os.path.normpath(cand).replace('\\', '/')
                if os.path.isfile(os.path.join(self.root, cand)):
                    out.append(cand)
                    break
        return out

    def calls(self, rel):
        t = re.sub(r'//[^\n]*|/\*.*?\*/', '', self.text(rel), flags=re.S)
        return {m.group(1) for m in CALL_RX.finditer(t)} - KEYWORDS


def defined_in_headers(ix, files, name):
    rx = re.compile(r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{')
    for f in files:
        for h in ix.headers(f) + [f]:
            if h.endswith(('.h', '.hpp', '.inc')) and rx.search(ix.text(h)):
                return True
    return False


def check_list(ix, label, files):
    """[(name, defining cpp)] called by the list but defined only outside it."""
    bad = []
    fset = set(files)
    for f in files:
        for n in sorted(ix.calls(f)):
            where = ix.defs.get(n)
            if not where or len(where) != 1:
                continue
            d = next(iter(where))
            if d in fset or d == f:
                continue
            if any(n in ix.defs and f2 in ix.defs[n] for f2 in fset):
                continue
            if defined_in_headers(ix, files, n):
                continue
            bad.append((n, d, f))
    return bad


def lint(root, verbose=False, extra_lists=None, cmake=None):
    ix = Index(root)
    errs = []
    game = cmake if cmake is not None else cmake_sources(root)
    lists = gather_lists(root) + (extra_lists or [])
    gset = set(game)
    for label, cpps, base in lists:
        files = [r for r in (resolve(root, c, base) for c in cpps) if r]
        if len(files) < 2 and not any(f.startswith('src/') for f in files):
            continue
        for n, d, f in check_list(ix, label, files):
            if (label, n) in BASELINE:
                continue
            errs.append(f'{label}: {os.path.basename(f)} calls {n}(), defined only in {d}, which this build list lacks')
        # rule 2: a src .cpp a tool list names that the game files call into, but WWE2K14_SOURCES lacks
        for f in files:
            if f.startswith('src/') and f not in gset:
                names = [n for n, w in ix.defs.items() if w == {f}]
                for g in game:
                    hit = [n for n in names if n in ix.calls(g)]
                    if hit:
                        errs.append(f'{label}: {f} is not in CMakeLists.txt WWE2K14_SOURCES but the game file {g} calls {hit[0]}() defined there')
                        break
    return errs


def selftest(root):
    import shutil
    import subprocess
    import tempfile
    ok = True
    clean = lint(root)
    print(f"  {'ok  ' if not clean else 'FAIL'} the current tree: {len(clean)} violation(s) {clean[:3]}")
    ok &= not clean
    # synthetic twin of B2F: a.cpp (the unit's file) calls BuildRopeInput, defined in b.cpp; the list lacks b.cpp
    d = tempfile.mkdtemp(prefix='build_list_lint_')

    def w(rel, body):
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write(body)
    w('src/x/a.cpp', 'void RunUnit() {\n  BuildRopeInput(1);\n}\n')
    w('src/x/b.cpp', 'void BuildRopeInput(int n) {\n}\n')
    w('tools/scene/build_u_unit.ps1', "& clang++ -O1 (Join-Path $repo 'src\\x\\a.cpp') -o (Join-Path $outDir 'u.exe')\n")
    w('CMakeLists.txt', 'set(WWE2K14_SOURCES\n    src/x/a.cpp\n)\n')
    e = lint(d)
    hit = [x for x in e if 'BuildRopeInput' in x and 'build_u_unit.ps1' in x]
    print(f"  {'ok  ' if hit else 'FAIL'} synthetic B2F (unit list lacks the defining .cpp): {hit[:1]}")
    ok &= bool(hit)
    w('tools/scene/build_u_unit.ps1', "& clang++ -O1 (Join-Path $repo 'src\\x\\a.cpp') (Join-Path $repo 'src\\x\\b.cpp') -o (Join-Path $outDir 'u.exe')\n")
    e = [x for x in lint(d) if 'build_u_unit.ps1' in x and 'build list lacks' in x]
    print(f"  {'ok  ' if not e else 'FAIL'} twin: the list with b.cpp is clean: {e[:1]}")
    ok &= not e
    # rule 2: the tool list names b.cpp, the game's a.cpp calls into it, CMake lacks it
    e = [x for x in lint(d) if 'WWE2K14_SOURCES' in x and 'b.cpp' in x]
    print(f"  {'ok  ' if e else 'FAIL'} synthetic game-list fault (CMake lacks b.cpp): {e[:1]}")
    ok &= bool(e)
    w('CMakeLists.txt', 'set(WWE2K14_SOURCES\n    src/x/a.cpp\n    src/x/b.cpp\n)\n')
    e = lint(d)
    print(f"  {'ok  ' if not e else 'FAIL'} twin: CMake with b.cpp is clean: {e[:1]}")
    ok &= not e
    shutil.rmtree(d, ignore_errors=True)
    # the REAL bug, from history (skipped when the commit is not in this clone): bc8e0ae7e's tree
    try:
        r = subprocess.run(['git', '-C', root, 'archive', 'bc8e0ae7e', 'src', 'tools/scene', 'CMakeLists.txt'], capture_output=True)
        if r.returncode == 0 and r.stdout:
            import io
            import tarfile
            d2 = tempfile.mkdtemp(prefix='build_list_lint_hist_')
            tarfile.open(fileobj=io.BytesIO(r.stdout)).extractall(d2, filter='data')
            e = [x for x in lint(d2) if 'build_e5_unit.ps1' in x and 'BuildRopeInput' in x]
            print(f"  {'ok  ' if e else 'FAIL'} REAL B2F (git bc8e0ae7e, build_e5_unit.ps1 without live_map_rope.cpp): {e[:1]}")
            ok &= bool(e)
            shutil.rmtree(d2, ignore_errors=True)
        else:
            print('  skip REAL B2F re-find: commit bc8e0ae7e is not in this clone')
    except (OSError, ImportError) as ex:
        print(f'  skip REAL B2F re-find: {ex}')
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    root = a[a.index('--root') + 1] if '--root' in a else ROOT
    if '--selftest' in a:
        sys.exit(0 if selftest(root) else 1)
    errs = lint(root, '--verbose' in a)
    for e in errs:
        print('ERROR ' + e)
    print(f'build_list_lint: {len(errs)} violation(s)')
    sys.exit(1 if errs else 0)


if __name__ == '__main__':
    main()
