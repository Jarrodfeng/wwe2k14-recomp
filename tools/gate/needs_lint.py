"""Gate check (S102 TIDY2): a gate lane names EXACT inputs, never a container.

  py -I tools\\gate\\needs_lint.py [--root DIR] [--out-dir DIR] [--selftest]

Why (Jarrod, 2026-11-15, the disk at 418 GB): the lane s102_rt1 listed "out/cases" in "needs"; the tidy held the whole folder, 265 unpack copies
of ~0.5 GB each (140 GB). A name in a lane protects exactly that path (tools\\harness\\tidy_out.py), so a name that is a container (a session folder
out/sNNN, out/cases, out/captures, a runs folder, a glob over a folder's children, or a folder that holds two or more capture / case / run folders)
is an ERROR: list the exact captures / case zips / files the check reads. An unpack / scratch copy (fview_unpack_*, *_unpack) is also an error:
nothing ever holds those. Lanes are strict; check SCRIPTS are only reported (WARN) when they name out/cases, out/captures or a runs folder, because a
script that joins out/captures and then picks one capture looks the same. --out-dir: the out\\ folder to test folders against on disk (default: the
main checkout's); without one only the static rules run. Exit 1 on any error.
S103 WTG, the UNDECLARED-NEED rule: a check whose script (the first .py / .ps1 in its cmd) names generated\\default or game_data\\extracted must declare it, in "needs"
(read from the MAIN checkout: a missing path is a SKIP) or in "root_needs" (read from the tree the check runs in: a worktree that lacks it reports the row NEEDS-MAIN at once,
tools\\gate\\needs_main.py). Without it a builder's worktree runs the check, which dies on the missing file (or waits out its timeout) and reads as a failure.
"root_needs" entries must be plain relative paths. --selftest: plants (a lane naming out/cases fails; the exact path
passes; a scratch copy fails; a glob over children fails; the same lane with the exact path passes).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'harness'))
import tidy_out      # noqa: E402
import tidy_disk     # noqa: E402
sys.path.insert(0, str(HERE))
import needs_main as gnm   # noqa: E402

def main_out(root):
    try:
        c = subprocess.run(['git', '-C', str(root), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                           timeout=30).stdout.strip()
        if c:
            o = Path(c).parent / 'out'
            if o.is_dir():
                return str(o)
    except Exception:
        pass
    return None


def lint_lane_file(path, disk):
    """-> [error strings] for one lane json."""
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        return ['%s: cannot read: %s' % (path, e)]
    errs = tidy_out.lint_lane_data(data, Path(path).name, disk)
    # S103: a cmd argument that climbs out of the checkout ("../../../out/...", "../../../game_data/...") only resolves from an agent worktree;
    # write out/... or game_data/... instead (gate.py maps both to the MAIN checkout when it runs from a worktree).
    for c in data.get('checks', []):
        for rn in c.get('root_needs', []):   # S103 WTG: a path inside the checkout the check runs in
            if not isinstance(rn, str) or re.match(r'^([A-Za-z]:|[/\\])', rn) or '..' in rn.replace('\\', '/').split('/'):
                errs.append('%s: %s: root_needs %r must be a relative path inside the checkout' % (Path(path).name, c.get('name', '?'), rn))
        for a in c.get('cmd', []):
            if isinstance(a, str) and a.replace('\\', '/').startswith('../'):
                errs.append('%s: %s: cmd argument %r climbs out of the checkout: write out/... or game_data/... (gate.py resolves them)' % (Path(path).name, c.get('name', '?'), a))
    return errs


UNDECLARED_PATS = {'generated/default': r'generated[/\\]+default', 'game_data/extracted': r'game_data[/\\]+extracted'}


def _code_text(path):
    """A script's text without comments / docstrings (a mention there is not an input; tidy_disk.strip_py), raw for a .ps1."""
    t = Path(path).read_text(encoding='utf-8', errors='replace')
    return tidy_disk.strip_py(t) if str(path).endswith('.py') else t


_MOD_INDEX = {}


def _module_index(root):
    """module name -> [paths] over every tools .py file, built once per root (an rglob per import made the lint take minutes)."""
    key = str(Path(root).resolve())
    if key not in _MOD_INDEX:
        idx = {}
        for dp, dn, fn in os.walk(Path(root) / 'tools'):
            dn[:] = [d for d in dn if d not in ('__pycache__', 'node_modules')]
            for f in fn:
                if f.endswith('.py'):
                    idx.setdefault(f[:-3], []).append(Path(dp) / f)
        _MOD_INDEX[key] = idx
    return _MOD_INDEX[key]


def _imports_one_level(root, script, text):
    """The stripped text of the tools\\ modules the script imports (one level, no recursion): `import x` / `from x import` where x is tools/**/x.py, the script's own folder first."""
    out = []
    here = (Path(root) / script).parent
    names = set(re.findall(r'^\s*(?:from|import)\s+([A-Za-z_][A-Za-z0-9_]*)', text, re.M))
    for n in sorted(names):
        cand = here / (n + '.py')
        if not cand.is_file():
            hits = _module_index(root).get(n, [])
            cand = hits[0] if len(hits) == 1 else None
        if cand is not None and cand.resolve() != (Path(root) / script).resolve():
            try:
                out.append(_code_text(cand))
            except OSError:
                pass
    return out


def check_script(root, c):
    """The first .py / .ps1 of a check's command that exists under ROOT, and its code text (comments / docstrings stripped) plus its tools\\ imports one level deep ('' when none)."""
    for a in c.get('cmd', []):
        if isinstance(a, str) and re.search(r'\.(py|ps1)$', a) and (Path(root) / a).is_file() and not a.replace('\\', '/').endswith('/needs_lint.py'):   # this file's own text names the paths
            try:
                text = _code_text(Path(root) / a)
                if a.endswith('.py'):
                    text += '\n' + '\n'.join(_imports_one_level(root, a, text))
                return a, text
            except OSError:
                break
    return None, ''


def undeclared_in(c, text):
    """-> [the paths (keys of UNDECLARED_PATS) the script names and the check declares in neither needs nor root_needs]."""
    declared = [str(d).replace('\\', '/') for d in list(c.get('needs', [])) + gnm.declared(c)]   # declared() = root_needs + what the command is known to read (build.ps1)
    return [k for k, pat in UNDECLARED_PATS.items() if re.search(pat, text) and not any(d.startswith(k.split('/')[0] + '/') for d in declared)]


def undeclared(root):
    """-> [(lane file, check name, [missing keys])] over every lane json."""
    out = []
    for lf in sorted((Path(root) / 'tools' / 'gate' / 'lanes').glob('*.json')):
        try:
            data = json.loads(lf.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        for c in data.get('checks', []):
            if c.get('retired'):
                continue
            script, text = check_script(root, c)
            miss = undeclared_in(c, text) if script else []
            if miss:
                out.append((str(lf), c.get('name', '?'), miss))
    return out


def lint_scripts(root):
    warns = []
    for dp, dn, fn in os.walk(Path(root) / 'tools'):
        dn[:] = [d for d in dn if d not in ('__pycache__', 'node_modules')]
        for n in fn:
            if not n.endswith('.py') or not re.search(r'check|selftest|oracle|gate', n, re.I) or n.startswith('tidy') or n == 'needs_lint.py':
                continue
            fp = Path(dp) / n
            try:
                text = tidy_disk.strip_py(fp.read_text(encoding='utf-8', errors='replace'))
            except OSError:
                continue
            for k in set(tidy_out.names_in(text)):
                parts = k.split('/')
                if len(parts) == 2 and (parts[1] in tidy_out.INPUT_CONTAINERS or re.fullmatch(r's\d+', parts[1])):
                    warns.append('%s: names the container "%s" (a script that joins it and then picks one capture is fine; one that reads a fixed copy must name it)' % (
                        fp.relative_to(root).as_posix(), k))
    return warns


def run(root, out_dir):
    errs, n = [], 0
    for lf in sorted((Path(root) / 'tools' / 'gate' / 'lanes').glob('*.json')):
        n += 1
        errs += lint_lane_file(lf, out_dir)
    for lf, name, miss in undeclared(root):
        errs.append('%s: %s: its script names %s but the check declares no "needs" / "root_needs" for it (a worktree runs it and it dies on the missing file; declare it so the row is SKIP / NEEDS-MAIN at once)'
                    % (Path(lf).name, name, ', '.join(miss)))
    return n, errs


def selftest():
    fails = []

    def case(name, lane, want_err):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'x.json'
            p.write_text(json.dumps(lane), encoding='utf-8')
            errs = lint_lane_file(p, None)
        ok = bool(errs) == want_err
        print(('PASS  ' if ok else 'FAIL  ') + name + ('' if ok else '  -> %r' % errs))
        if not ok:
            fails.append(name)

    case('PLANT: a lane naming out/cases fails', {'checks': [{'name': 'a', 'needs': ['out/cases']}]}, True)
    case('the exact case zip passes', {'checks': [{'name': 'a', 'needs': ['out/cases/case_20261111_174607_799_0_pc_run_exit/capture']}]}, False)
    case('PLANT: out/captures fails', {'checks': [{'name': 'a', 'needs': ['out/captures']}]}, True)
    case('one capture passes', {'checks': [{'name': 'a', 'needs': ['out/captures/s100cards']}]}, False)
    case('PLANT: a session folder fails', {'checks': [{'name': 'a', 'cmd': ['py', 'x.py', 'out/s102']}]}, True)
    case('a folder inside a session passes', {'checks': [{'name': 'a', 'cmd': ['py', 'x.py', 'out/s102/c4']}]}, False)
    case('PLANT: a runs folder fails', {'checks': [{'name': 'a', 'needs': ['out/s101/runs']}]}, True)
    case('PLANT: a lane naming out/tools (the whole exe folder) fails (S103: s103_B3 held 5.7 GB)', {'checks': [{'name': 'a', 'cmd': ['New-Item -Force out/tools; clang++ x.cpp -o out/tools/x.exe']}]}, True)
    case('the exact exe passes', {'checks': [{'name': 'a', 'needs': ['out/tools/x.exe']}]}, False)
    case('PLANT: a glob over the children fails', {'checks': [{'name': 'a', 'needs': ['out/cases/*']}]}, True)
    case('PLANT: an fview unpack copy fails', {'checks': [{'name': 'a', 'needs': ['out/cases/fview_unpack_20261112-185530/capture/images.bin']}]}, True)
    case('an allowlisted build tree named exactly passes', {'checks': [{'name': 'a', 'needs': ['out/renderer/dev/fview.exe']}]}, False)
    with tempfile.TemporaryDirectory() as td:        # on disk: a folder holding two capture folders is a container even with an innocent name
        for c in ('a', 'b'):
            d = Path(td) / 'many' / c
            d.mkdir(parents=True)
            (d / 'images.bin').write_bytes(b'x')
        why = tidy_out.container_reason('out/many', td)
        ok = bool(why)
        print(('PASS  ' if ok else 'FAIL  ') + 'PLANT: a folder holding two captures is a container (disk test)')
        if not ok:
            fails.append('disk')
        why = tidy_out.container_reason('out/many/a', td)
        ok = not why
        print(('PASS  ' if ok else 'FAIL  ') + 'a leaf capture folder is not')
        if not ok:
            fails.append('leaf')
    # S103 WTG: the undeclared-need rule, with its planted twin (a script naming generated\\default without declaring it fails; declared in needs or root_needs passes)
    with tempfile.TemporaryDirectory() as td:
        (Path(td) / 'x.py').write_text("p = 'generated/default'\n", encoding='utf-8')
        (Path(td) / 'doc.py').write_text('"""reads generated/default maybe"""\n# game_data/extracted\nprint(1)\n', encoding='utf-8')
        (Path(td) / 'tools').mkdir()
        (Path(td) / 'tools' / 'helper_mod.py').write_text("p = 'generated/default'\n", encoding='utf-8')
        (Path(td) / 'imp.py').write_text('import helper_mod\n', encoding='utf-8')
        (Path(td) / 'y.py').write_text("p = r'game_data\\extracted'\n", encoding='utf-8')
        cases = [('PLANT: a script naming generated/default with no declaration fails', {'name': 'a', 'cmd': ['py', 'x.py']}, ['generated/default']),
                 ('generated/default declared in needs passes', {'name': 'a', 'cmd': ['py', 'x.py'], 'needs': ['generated/default']}, []),
                 ('generated/default declared in root_needs passes', {'name': 'a', 'cmd': ['py', 'x.py'], 'root_needs': ['generated/default']}, []),
                 ('PLANT: game_data/extracted with a generated/default declaration only fails', {'name': 'a', 'cmd': ['py', 'y.py'], 'needs': ['generated/default']}, ['game_data/extracted']),
                 ('a docstring / comment mention alone passes', {'name': 'a', 'cmd': ['py', 'doc.py']}, []),
                 ('PLANT: the mention in an imported tools module fails', {'name': 'a', 'cmd': ['py', 'imp.py']}, ['generated/default']),
                 ('a script that names neither passes', {'name': 'a', 'cmd': ['py', 'nothing.py']}, [])]
        for name, c, want in cases:
            script, text = check_script(td, c)
            got = undeclared_in(c, text) if script else []
            ok = got == want
            print(('PASS  ' if ok else 'FAIL  ') + name + ('' if ok else '  -> %r' % got))
            if not ok:
                fails.append(name)
        bad = lint_lane_file(Path(td) / 'z.json', None)   # unreadable -> an error string, not a crash
        errs = []
        (Path(td) / 'r.json').write_text(json.dumps({'checks': [{'name': 'a', 'cmd': ['py', 'x'], 'root_needs': ['../x']}]}), encoding='utf-8')
        errs = lint_lane_file(Path(td) / 'r.json', None)
        ok = bool(errs) and bool(bad)
        print(('PASS  ' if ok else 'FAIL  ') + 'PLANT: a root_needs that climbs out of the checkout fails')
        if not ok:
            fails.append('root_needs')
    print('%d failed' % len(fails))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(HERE.parent.parent))
    ap.add_argument('--out-dir')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    out_dir = a.out_dir or main_out(a.root)
    n, errs = run(a.root, out_dir)
    for e in errs:
        print('ERROR ' + e)
    for w in lint_scripts(a.root):
        print('WARN  ' + w)
    print('needs_lint: %d lanes, %d errors (folders judged %s)' % (n, len(errs), 'on disk at ' + out_dir if out_dir else 'by name only'))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
