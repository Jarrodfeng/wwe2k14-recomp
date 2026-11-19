r"""Lint: every <name>.exe a lane's build script names (outside comments) is in that check's write footprint (S103 GST review 15).

  py -I tools\gate\build_footprint_lint.py [--root DIR] [--list]      (--selftest: the planted-fault proof)

WHY: a build .ps1's footprint is the exe(s) it names (sched.py: the .exe literals joined with its $outDir), and the tool cache publishes only outputs inside that footprint (gate.py
tool_publish). An exe the script builds under a name the scan cannot see (built from a variable, a here-string, a .exe named only in a function) was in NO footprint: its build
was not cached, and two builds of it could run at once. This lint reads each lane check that runs a tools\...\build*.ps1 and fails when the script names an .exe (a quoted or
bare `name.exe`, comments stripped; `$var.exe` is a name built at run time: not read) that is neither in the check's write footprint nor on the list of programs a build only RUNS (compilers, linkers, shells: TOOLS below).
`"writes": [...]` on the lane check declares the footprint explicitly and wins (the lint then checks the declaration)."""
import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sched as gs      # noqa: E402
import toolcache as gtc  # noqa: E402

EXE = re.compile(r"(?<![\w.$-])([\w.+-]+\.exe)\b", re.I)
TOOLS = {'clang.exe', 'clang++.exe', 'clang-cl.exe', 'cl.exe', 'link.exe', 'lld-link.exe', 'llvm-ar.exe', 'lib.exe', 'ninja.exe', 'cmake.exe', 'ctest.exe', 'pwsh.exe', 'powershell.exe',
         'python.exe', 'py.exe', 'git.exe', 'sccache.exe', 'rc.exe', 'mt.exe', 'dumpbin.exe', 'where.exe', 'taskkill.exe', 'tasklist.exe', 'cmd.exe', 'robocopy.exe', 'xcopy.exe',
         'glslangvalidator.exe', 'spirv-val.exe', 'spirv-opt.exe', 'glslc.exe', 'rexglue.exe', 'vswhere.exe', 'msbuild.exe', 'curl.exe', 'tar.exe', 'ssh.exe', 'scp.exe', 'adb.exe'}


def strip_comments(text):
    text = re.sub(r'(?s)<#.*?#>', '', text)
    out = []
    for ln in text.splitlines():
        # a `#` that starts a comment: at the line start or after whitespace (a `#` inside a quoted colour / format string is rare in a build script)
        out.append(re.sub(r'(?:^|\s)#.*$', '', ln))
    return '\n'.join(out)


def named_exes(text):
    return sorted({m.group(1).lower() for m in EXE.finditer(strip_comments(text))} - TOOLS)


def lint(root, checks):
    """-> [(check name, script, [missing exe names])]"""
    bad = []
    for c in checks:
        bs = gtc.build_script_of(c.get('cmd', []))
        if bs is None or c.get('retired'):
            continue
        p = Path(root) / bs[0]
        if not p.exists():
            continue
        names = named_exes(p.read_text(encoding='utf-8', errors='replace'))
        fp = gs.footprint(c, Path(root), Path(root), [])
        have = {Path(w).name.lower() for w in fp['writes']} | {Path(w).name.lower() for w in fp['reads']}
        miss = [n for n in names if n not in have]
        if miss:
            bad.append((c['name'], bs[0], miss))
    return bad


def load_checks(root):
    out = []
    for f in sorted((Path(root) / 'tools' / 'gate' / 'lanes').glob('*.json')):
        try:
            data = json.loads(f.read_text(encoding='utf-8'))
        except ValueError:
            continue
        for c in data.get('checks', []):
            c['lane'] = f.stem
            out.append(c)
    return out


def selftest():
    fails = []

    def ok(cond, text):
        print(('ok    ' if cond else 'FAIL  ') + text)
        if not cond:
            fails.append(text)
    d = Path(tempfile.mkdtemp(prefix='bfl_'))
    (d / 'tools' / 'x').mkdir(parents=True)
    (d / 'tools' / 'x' / 'build_good.ps1').write_text("$repo = 'x'\n$outDir = Join-Path $repo 'out\\tools'\n# also builds hidden.exe (a comment: ignored)\n& clang++ a.cpp -o (Join-Path $outDir 'good_rig.exe')\n")
    (d / 'tools' / 'x' / 'build_bad.ps1').write_text("$repo = 'x'\n$outDir = Join-Path $repo 'out\\tools'\n$n = 'bad_' + 'rig'\n& clang++ a.cpp -o (Join-Path $outDir 'bad_rig.exe')\n& clang++ b.cpp -o (Join-Path $outDir ($n + '_two.exe'))\nWrite-Host unnamed_unit.exe\n")
    mk = lambda n, s: {'name': n, 'cmd': ['pwsh', '-NoProfile', '-File', f'tools/x/{s}']}
    bad = lint(d, [mk('l.good', 'build_good.ps1')])
    ok(not bad, f'BF1 a build that names only the exe it builds passes (a comment naming another exe is ignored): {bad}')
    bad = lint(d, [mk('l.bad', 'build_bad.ps1')])
    ok(bad and bad[0][2] == ['unnamed_unit.exe'], f'BF2 PLANTED: an exe the script names but whose build the footprint cannot see is reported: {bad}')
    c = dict(mk('l.bad2', 'build_bad.ps1'), writes=['out/tools/bad_rig.exe', 'out/tools/_two.exe', 'out/tools/unnamed_unit.exe'])
    ok(not lint(d, [c]), 'BF3 a declared "writes" that lists it fixes the finding')
    print('build_footprint_lint selftest: ' + ('FAIL' if fails else 'all checks hold'))
    return 1 if fails else 0


def main():
    if '--selftest' in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(HERE.parents[1]))
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    bad = lint(a.root, load_checks(a.root))
    for n, s, miss in bad:
        print(f'FAIL  {n}: {s} names {", ".join(miss)} but the check\'s footprint does not (declare "writes": [...] on the lane check, or name the output in the script)')
    print(f'build_footprint_lint: {len(bad)} build check(s) name an exe their footprint misses')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
