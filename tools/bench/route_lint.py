r"""H2 / L1 ROUTE / FLAG LINT (S103: failed runs kept coming from forgotten switches; runs 24 / 25 / 26).

  py -I tools\bench\route_lint.py                       lint every route under tools\input_scripts (the gate lane): exit 1 on an ERROR
  py -I tools\bench\route_lint.py --route R [--final "FLAGS"] [--exe EXE] [--extra "FLAGS"]
                                                        one route; --final = the run's whole final flag line (run_list.ps1 passes it: the route's header flags + what run_list added)
  py -I tools\bench\route_lint.py --selftest             the planted twins + the re-found bugs (exit 1 if one is missed)

Checks (a route is the .txt; its flags are its '# harness: flags ...' header lines, as run_list.ps1 merges them):
  (a) every --flag names a cvar the code defines: REXCVAR_DEFINE_* in src\ and the SDK source (thirdparty\rexglue-sdk-src, found from the main checkout when run in a worktree).
      A flag not defined = ERROR, unless the exe being run (or a DLL in its folder) holds the name as a string; WARN when --exe is not under this checkout (a misspelt switch makes the game refuse its command line or silently ignore it).
  (b) dev-only flags (DEV_ONLY below: the sentinel, forced inputs, traces, the case ring ...) on a RELEASE exe (--exe under out\release or release\build) = ERROR for force_inputs / sentinel (ignored there), WARN for the diagnostics (trace, coverage, dump_targets, census, file_trace).
  (c) the FINAL flag line is printed. A route header line '# intent: owner on, scene 2, sentinel 30' is checked against it: every term must hold in the final flags
      (terms: '<name> on|off|<value>' resolved as wwe2k14_rdr_<name> / wwe2k14_<name> / <name> / any wwe2k14 cvar ending _<name>; 'flag --x[=v]'; 'no flag --x'). A mismatch = ERROR.
      A route with no intent line = WARN (--verbose lists them).
  (d) run_list.ps1 hands the final line here before every game start and refuses on an ERROR; its hidden additions are printed ('[run_list] HIDDEN ADDITION: ...').
  Standalone (no --final) the lint adds run_list's one semantic addition itself: --wwe2k14_s101_promotions=false unless the flags name rdr_owner or s101_promotions (run 25 ran the OLD renderer by it)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FLAG_RX = re.compile(r'(?<![\w-])--([A-Za-z][\w.]*)(?:=(\S*))?')
DEF_RX = re.compile(r'REXCVAR_DEFINE_[A-Z0-9_]+\(\s*(\w+)\s*,\s*([^,]*?)\s*,')
# flags only a +dev build honours or that only make sense there (one regex each, matched against the flag name)
DEV_ERROR = [r'rdr_scene_force_inputs', r'rdr_scene_sentinel']   # a release exe ignores them: the run silently measures something else = ERROR
DEV_WARN = [r'trace', r'coverage', r'^wwe2k14_case(_dir)?$', r'dump_targets', r'rdr_scene_census', r'file_trace']   # diagnostics: WARN
SKIP_DIRS = ('android',)   # phone routes carry adb-side flags the PC game does not define


def sdk_src(root):
    for c in (os.environ.get('WWE_SDK_SRC'), os.path.join(root, 'thirdparty', 'rexglue-sdk-src')):
        if c and os.path.isdir(c):
            return c
    try:   # a worktree: the main checkout holds thirdparty
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        main = os.path.dirname(os.path.abspath(os.path.join(root, common)))
        c = os.path.join(main, 'thirdparty', 'rexglue-sdk-src')
        if os.path.isdir(c):
            return c
    except OSError:
        pass
    return None


_CV = {}


def cvars(root):
    """({name: default text}, sdk_found); the SDK ones only when its source is found."""
    if root in _CV:
        return _CV[root]
    out, sdk = {}, sdk_src(root)
    for base in [os.path.join(root, 'src')] + ([os.path.join(sdk, 'src'), os.path.join(sdk, 'include')] if sdk else []):
        for d, dirs, files in os.walk(base):
            for f in files:
                if f.endswith(('.h', '.cpp', '.hpp', '.cc', '.inc')):
                    try:
                        t = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
                    except OSError:
                        continue
                    if 'REXCVAR_DEFINE' in t:
                        for m in DEF_RX.finditer(t):
                            out.setdefault(m.group(1), m.group(2).strip().strip('"'))
    _CV[root] = (out, bool(sdk))
    return _CV[root]


def header_lines(text):
    return text.split('\n')[:40]


def header_flags(text):
    toks = {}
    for ln in header_lines(text):
        m = re.match(r'^#\s*harness:\s*flags\s+(.+)$', ln.strip())
        if m:
            for t in m.group(1).split():
                if t.startswith('--'):
                    toks[t.split('=', 1)[0]] = t   # the last wins, as run_list
    return list(toks.values())


def intent(text):
    for ln in header_lines(text):
        m = re.match(r'^#\s*intent:\s*(.+)$', ln.strip())
        if m:
            return [t.strip() for t in m.group(1).split(',') if t.strip()]
    return None


def parse_flags(line):
    d = {}
    for m in FLAG_RX.finditer(line):
        d[m.group(1)] = m.group(2) if m.group(2) is not None else 'true'
    return d


def hidden_additions(flags):
    add = []
    if not re.search(r'--wwe2k14_(rdr_owner|s101_promotions)=', flags):
        add.append('--wwe2k14_s101_promotions=false')
    return add


def truthy(v):
    return str(v).lower() in ('1', 'true', 'on', 'yes')


def holds(val, got):
    v = val.lower()
    return truthy(got) if v == 'on' else (not truthy(got)) if v == 'off' else got == val


def term_check(term, flags, defs):
    """None if the term holds, else why not."""
    m = re.match(r'^(no )?flag\s+--?([\w.]+)(?:=(\S+))?$', term)
    if m:
        neg, name, val = m.groups()
        have = name in flags
        if neg:
            return None if not have else f"'{term}': --{name} is set"
        if not have:
            return f"'{term}': --{name} is not in the final flags"
        return None if val is None or flags[name] == val else f"'{term}': --{name}={flags[name]}"
    m = re.match(r'^([\w.]+)\s+(\S+)$', term)
    if not m:
        return f"'{term}': not understood (use '<name> on|off|<value>')"
    name, val = m.groups()
    cands = [f'wwe2k14_rdr_{name}', f'wwe2k14_{name}', name]
    cands += sorted(k for k in list(flags) + list(defs) if k.startswith('wwe2k14') and k.endswith('_' + name))
    seen = []
    for c in cands:
        if c in seen:
            continue
        seen.append(c)
        if c in flags:
            return None if holds(val, flags[c]) else f"'{term}': --{c}={flags[c]} in the final flags"
    for c in seen:   # not on the final line: defaults are decided at run time (S101 promotions set ~30 switches), never read from source text
        if c in defs:
            return f"'{term}': --{c} is not on the final flag line: set --{c}=... explicitly in the header (defaults are decided at run time)"
    return f"'{term}': no flag or cvar named like '{name}'"


_EXE = {}


def exe_knows(exe, name):
    """True when the exe or a DLL in its folder holds the flag name as an ASCII string (a branch-only cvar of a worktree / SDK-worktree exe)."""
    import mmap
    if not exe or not os.path.isfile(exe):
        return False
    folder = os.path.dirname(os.path.abspath(exe))
    if folder not in _EXE:
        paths = [exe] + [os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith('.dll')]
        maps = []
        for q in paths:
            try:
                fh = open(q, 'rb')
                if os.path.getsize(q):
                    maps.append(mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ))
            except OSError:
                pass
        _EXE[folder] = maps
    needle = name.encode()
    return any(m.find(needle) >= 0 for m in _EXE[folder])


def under_root(exe, root):
    try:
        return os.path.commonpath([os.path.abspath(exe), os.path.abspath(root)]) == os.path.abspath(root)
    except ValueError:
        return False


def is_release(exe):
    e = (exe or '').lower().replace('/', '\\')
    return '\\out\\release\\' in e or 'release\\build' in e or e.startswith('release\\')


def input_names(root):
    """Input names the mask switches accept (inputs_core.h's table: {kInXxx, "name", ...})."""
    p = os.path.join(root, 'src', 'scene_live', 'inputs_core.h')
    try:
        return set(re.findall(r'\{\s*kIn\w+\s*,\s*"(\w+)"', open(p, encoding='utf-8', errors='replace').read()))
    except OSError:
        return set()


def lint_route(path, root, final=None, exe=None, extra=''):
    """(errors, warns, final flag line, hidden additions)."""
    text = open(path, encoding='utf-8', errors='replace').read()
    errs, warns = [], []
    defs, sdk_found = cvars(root)
    hdr = header_flags(text)
    hidden = []
    if final is None:
        base = ' '.join(hdr + ([extra] if extra else []))
        hidden = hidden_additions(base)
        final = (base + ' ' + ' '.join(hidden)).strip()
    flags = parse_flags(final)
    for name in list(parse_flags(' '.join(hdr))) + (list(parse_flags(extra)) if extra else []):
        if name in defs or not (name.startswith('wwe2k14_') or sdk_found):
            continue
        if exe_knows(exe, name):
            continue   # a branch-only cvar the exe being run has
        msg = f'--{name}: no REXCVAR_DEFINE in src\\ or the SDK source (misspelt, or removed)'
        if exe and not under_root(exe, root):
            warns.append(msg + f'; the exe {exe} is not under this checkout and does not hold the name either (a branch-only cvar of a worktree exe would)')
        else:
            errs.append(msg)
    if is_release(exe):
        for name in flags:
            if any(re.search(p, name) for p in DEV_ERROR):
                errs.append(f'--{name}: dev-only flag on a release exe ({exe}): it is ignored there')
            elif any(re.search(p, name) for p in DEV_WARN):
                warns.append(f'--{name}: diagnostic flag on a release exe ({exe})')
    # S103: the input-mask switches take bit numbers or names from src\scene_live\inputs_core.h; an unknown token is silently
    # dropped by the game (run 33 used 'crowd' and unrequired nothing).
    names = input_names(root)
    for sw in ('wwe2k14_rdr_scene_unrequire', 'wwe2k14_rdr_scene_hold'):
        if sw in flags and names:
            for tok in [t.strip() for t in str(flags[sw]).strip('"').split(',') if t.strip()]:
                if not tok.isdigit() and tok not in names:
                    errs.append(f'--{sw}: unknown token "{tok}" (the game ignores it; names: {", ".join(sorted(names))}, or a bit number)')
    it = intent(text)
    if it is None:
        warns.append('no "# intent:" header line (what the run must have on, e.g. "# intent: owner on, scene 2, sentinel 30")')
    else:
        for t in it:
            why = term_check(t, flags, defs)
            if why:
                errs.append('intent mismatch ' + why + (f'; run_list adds {" ".join(hidden)}' if hidden else ''))
    return errs, warns, final, hidden


def routes(root):
    base = os.path.join(root, 'tools', 'input_scripts')
    for d, dirs, files in os.walk(base):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in sorted(files):
            if f.endswith('.txt'):
                yield os.path.join(d, f)


def lint_all(root, verbose=False):
    nerr = nwarn = n = 0
    for p in routes(root):
        text = open(p, encoding='utf-8', errors='replace').read()
        if not header_flags(text) and intent(text) is None:
            continue   # a plain route: nothing of ours to check
        n += 1
        errs, warns, final, hidden = lint_route(p, root)
        rel = os.path.relpath(p, root)
        for e in errs:
            print(f'ERROR {rel}: {e}')
            nerr += 1
        if verbose:
            for w in warns:
                print(f'WARN  {rel}: {w}')
        nwarn += len(warns)
    note = '' if cvars(root)[1] else '; SDK source NOT found: only wwe2k14_ flags checked'
    print(f'route_lint: {n} routes with header flags, {nerr} ERROR, {nwarn} WARN (no intent line; --verbose lists them){note}')
    return nerr


def selftest(root):
    import tempfile
    ok = True
    d = tempfile.mkdtemp(prefix='route_lint_')

    def w(name, body):
        p = os.path.join(d, name)
        open(p, 'w').write(body)
        return p

    def expect(label, path, want_err, **kw):
        nonlocal ok
        errs, warns, final, hidden = lint_route(path, root, **kw)
        good = bool(errs) == want_err
        print(f"  {'ok  ' if good else 'FAIL'} {label}: {len(errs)} error(s) {errs[:1]}")
        ok &= good
        return final
    base = '--wwe2k14_launch=lesnar_bryan --wwe2k14_rdr_scene=2 --wwe2k14_rdr_scene_sentinel=30 --wwe2k14_rdr_owner=true --wwe2k14_s101_promotions=true'
    expect('clean route with intent', w('clean.txt', f'# harness: flags {base}\n# intent: owner on, scene 2, sentinel 30, promotions on\n'), False)
    # re-find run 25: the route forgot the owner switch, run_list added promotions=false, the intent says owner on
    base25 = base.replace('--wwe2k14_rdr_owner=true ', '').replace('--wwe2k14_s101_promotions=true', '')
    final = expect('run 25 (owner forgotten, promotions=false added silently)', w('r25.txt', f'# harness: flags {base25}\n# intent: owner on, scene 2\n'), True)
    ok &= 'promotions=false' in final
    # re-find run 33: --wwe2k14_rdr_scene_unrequire=crowd ('crowd' is not an input name: the game dropped it and unrequired nothing)
    expect('run 33 (unrequire=crowd, unknown token)', w('r33.txt', f'# harness: flags {base} --wwe2k14_rdr_scene_unrequire=crowd\n# intent: owner on\n'), True)
    expect('unrequire=crowd_snapshot,4 (known)', w('r33ok.txt', f'# harness: flags {base} --wwe2k14_rdr_scene_unrequire=crowd_snapshot,4\n# intent: owner on\n'), False)
    expect('misspelt flag (rdr_scene_sentinal)', w('typo.txt', '# harness: flags --wwe2k14_rdr_scene_sentinal=30\n'), True)
    expect('sentinel on a release exe', w('dev.txt', f'# harness: flags {base}\n# intent: owner on\n'), True, exe=r'C:\x\out\release\build\wwe2k14.exe')
    expect('same route on the +dev exe', w('dev2.txt', f'# harness: flags {base}\n# intent: owner on\n'), False, exe=r'C:\x\out\build\win-amd64-release\wwe2k14.exe')
    expect('final line given (run_list) disagrees', w('fin.txt', f'# harness: flags {base}\n# intent: owner on, promotions off\n'), True, final=base)
    e, wn, f, h = lint_route(w('nointent.txt', f'# harness: flags {base}\n'), root)
    good = (not e) and bool(wn)
    print(f"  {'ok  ' if good else 'FAIL'} no intent line: warn only {wn[:1]}")
    ok &= good
    e, wn, f, h = lint_route(w('trace2.txt', '# harness: flags --wwe2k14_trace=true --wwe2k14_rdr_owner=true\n# intent: owner on\n'), root, exe=r'C:\x\out\release\build\wwe2k14.exe')
    good = (not e) and any('trace' in x for x in wn)
    print(f"  {'ok  ' if good else 'FAIL'} trace on a release exe: WARN only {wn[:1]}")
    ok &= good
    # a branch-only cvar: unknown in the source, but a fake exe holds the string -> accepted; an exe outside the checkout without it -> WARN only
    fx = os.path.join(d, 'fake.exe')
    open(fx, 'wb').write(b'MZ\0\0--wwe2k14_branch_only_flag_x\0\0')
    r = w('branch.txt', '# harness: flags --wwe2k14_branch_only_flag_x=1\n')
    e1 = lint_route(r, root, exe=fx)
    good = not e1[0] and not any('branch_only' in x for x in e1[1])
    print(f"  {'ok  ' if good else 'FAIL'} branch-only cvar found in the exe: {e1[0][:1]}")
    ok &= good
    r2 = w('branch2.txt', '# harness: flags --wwe2k14_branch_only_flag_y=1\n')
    e2 = lint_route(r2, root, exe=fx)
    good = not e2[0] and any('branch_only_flag_y' in x for x in e2[1])
    print(f"  {'ok  ' if good else 'FAIL'} unknown flag with an exe outside the checkout: WARN only {e2[1][:1]}")
    ok &= good
    e3 = lint_route(r2, root)
    good = bool(e3[0])
    print(f"  {'ok  ' if good else 'FAIL'} the same unknown flag with no exe: ERROR")
    ok &= good
    e4 = lint_route(w('nodefault.txt', '# harness: flags --wwe2k14_launch=x\n# intent: owner on\n'), root)
    good = bool(e4[0]) and 'explicitly' in e4[0][0]
    print(f"  {'ok  ' if good else 'FAIL'} intent never reads source defaults: {e4[0][:1]}")
    ok &= good
    expect('bad intent term', w('badint.txt', f'# harness: flags {base}\n# intent: glorp\n'), True)
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    if '--selftest' in a:
        sys.exit(0 if selftest(ROOT) else 1)

    def opt(n):
        return a[a.index(n) + 1] if n in a else None
    r = opt('--route')
    if r:
        errs, warns, final, hidden = lint_route(r, ROOT, final=opt('--final'), exe=opt('--exe'), extra=opt('--extra') or '')
        print('[route_lint] final flags: ' + final)
        for w_ in warns:
            print('[route_lint] WARN ' + w_)
        for e in errs:
            print('[route_lint] ERROR ' + e)
        sys.exit(1 if errs else 0)
    sys.exit(1 if lint_all(ROOT, '--verbose' in a) else 0)


if __name__ == '__main__':
    main()
