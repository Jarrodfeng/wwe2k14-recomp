"""out_dir_lint: a gate-run check script may not default its OUTPUT folder to a fixed path under the MAIN checkout (S103 PSO).

Why: frame_rig.main_checkout() is the same folder from every worktree, so `out = <main>\\out\\s103\\ps1` is shared by every gate, every parallel twin and every
agent. Two runs rewrite each other's reference / served pictures (false FAIL, half-written PNGs, a possible false PASS). A check writes to its --out, else
GATE_CHECK_DIR (the gate gives each check its own folder), else a per-run folder: tools\\scene\\run_dirs.py run_out(name, explicit).

  py -I tools\\gate\\out_dir_lint.py             lint every script a gate lane runs (tools\\gate\\lanes\\*.json checks[].cmd)
  py -I tools\\gate\\out_dir_lint.py FILE...     lint the named scripts
  py -I tools\\gate\\out_dir_lint.py --selftest  plants a fixed main-checkout default (must fail), a pid / GATE_CHECK_DIR / run_out default (must pass)

What is flagged: an assignment to an output-like name (out, outdir, out_dir, work, ref_root, workdir, out_root, ...) or an argparse --out/--work/--dir default whose
value is built from the main checkout (a main_checkout() call, RDR_MAIN_CHECKOUT, or a name assigned from one: MAIN, main_root, main_co, ...) and the path part 'out',
with no getpid / GATE_CHECK_DIR / run_out / tempfile in it. Per-checkout defaults (rooted at this script's own ROOT) are private to a worktree and not flagged.
Mark a deliberate shared product (an atlas every run reads) with `# out-dir-ok: <why>` on the line.
"""
import ast
import glob
import json
import os
import re
import sys
import tempfile

import warnings
warnings.simplefilter('ignore')   # (other scripts' odd escapes are not this lint's business)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT_NAMES = {'out', 'outdir', 'out_dir', 'work', 'workdir', 'work_dir', 'ref_root', 'out_root', 'odir', 'outroot', 'base_out', 'run_dir', 'rundir'}
OUT_FLAGS = ('--out', '--work', '--dir', '--outdir', '--out-dir', '--workdir')
BASELINE = os.path.join(HERE, 'out_dir_lint_baseline.txt')
SAFE = ('getpid', 'GATE_CHECK_DIR', 'run_out', 'tempfile', 'mkdtemp', 'TemporaryDirectory')


def _names(node):
    return {n.id for n in ast.walk(node) if isinstance(n, ast.Name)} | {n.attr for n in ast.walk(node) if isinstance(n, ast.Attribute)}


def _is_main_source(node, tainted):
    for n in ast.walk(node):
        if isinstance(n, ast.Call):
            f = n.func
            nm = f.id if isinstance(f, ast.Name) else f.attr if isinstance(f, ast.Attribute) else ''
            if 'main_checkout' in nm or nm == '_main_checkout':
                return True
        if isinstance(n, ast.Constant) and n.value == 'RDR_MAIN_CHECKOUT':
            return True
        if isinstance(n, ast.Name) and n.id in tainted:
            return True
    return False


def _has_out_part(node):
    for n in ast.walk(node):
        if isinstance(n, ast.Constant) and isinstance(n.value, str) and re.search(r'(^|[\\/])out($|[\\/])', n.value):
            return True
    return False


def lint_source(src, name='<src>'):
    """-> list of 'file:line: text' problems"""
    try:
        tree = ast.parse(src)
    except SyntaxError:
        return []
    lines = src.splitlines()
    tainted = set()
    for _ in range(3):   # a few passes: names assigned from the main checkout, then names built from them
        for n in ast.walk(tree):
            if isinstance(n, ast.Assign) and _is_main_source(n.value, tainted) and not _has_out_part(n.value):
                for t in n.targets:
                    if isinstance(t, ast.Name):
                        tainted.add(t.id)
    bad = []

    def check(value, line, what):
        seg = ast.get_source_segment(src, value) or ''
        if not (_is_main_source(value, tainted) and _has_out_part(value)):
            return
        if any(s in seg for s in SAFE) or 'out-dir-ok' in (lines[line - 1] if 0 < line <= len(lines) else ''):
            return
        bad.append('%s:%d: %s defaults to a fixed main-checkout path shared by every run: %s' % (name, line, what, ' '.join(seg.split())[:110]))

    for n in ast.walk(tree):
        if isinstance(n, (ast.Assign, ast.AnnAssign)):
            targets = n.targets if isinstance(n, ast.Assign) else [n.target]
            for t in targets:
                tn = t.id if isinstance(t, ast.Name) else ''
                if tn in OUT_NAMES and n.value is not None:
                    check(n.value, n.lineno, "'%s'" % tn)
        elif isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr == 'add_argument':
            flags = [a.value for a in n.args if isinstance(a, ast.Constant) and isinstance(a.value, str)]
            if any(f in OUT_FLAGS for f in flags):
                for k in n.keywords:
                    if k.arg == 'default':
                        check(k.value, n.lineno, 'argparse ' + '/'.join(flags))
    return bad


def lane_scripts():
    out = set()
    for f in glob.glob(os.path.join(ROOT, 'tools', 'gate', 'lanes', '*.json')):
        try:
            d = json.load(open(f, encoding='utf8'))
        except (OSError, ValueError):
            continue
        for ck in d.get('checks', []):
            cmd = ck.get('cmd', '')
            cmd = cmd if isinstance(cmd, str) else ' '.join(map(str, cmd))
            for m in re.findall(r'tools[\\/][\w\\/.\-]+\.py', cmd):
                p = os.path.join(ROOT, m.replace('/', os.sep))
                if os.path.isfile(p):
                    out.add(p)
    return sorted(out)


def lint_files(files):
    bad = []
    for p in files:
        try:
            src = open(p, encoding='utf8', errors='ignore').read()
        except OSError:
            continue
        bad += lint_source(src, os.path.relpath(p, ROOT))
    return bad


def selftest():
    fails = []

    def expect(label, src, want_bad):
        got = bool(lint_source(src, 't.py'))
        print(('PASS' if got == want_bad else 'FAIL') + '  ' + label + (' (flagged)' if got else ' (clean)'))
        if got != want_bad:
            fails.append(label)

    head = "import os, argparse\nimport frame_rig\n"
    expect('planted: out = <main>\\out\\s103\\ps1 (fixed, shared)', head + "main_root = frame_rig.main_checkout()\nout = os.path.join(main_root, 'out', 's103', 'ps1')\n", True)
    expect('planted: a.out or <main>\\out\\... default', head + "def main(a):\n    main_root = frame_rig.main_checkout()\n    out = a.out or os.path.join(main_root, 'out', 's103', 'D2')\n", True)
    expect('planted: argparse --out default under MAIN', head + "MAIN = frame_rig.main_checkout()\nap = argparse.ArgumentParser()\nap.add_argument('--out', default=os.path.join(MAIN, 'out', 's103', 'D'))\n", True)
    expect('planted: ref_root under the main checkout', head + "main_co = os.environ.get('RDR_MAIN_CHECKOUT')\nref_root = os.path.join(main_co, 'out', 's103', 'ps1_ref')\n", True)
    expect('ok: a pid in the path', head + "main_root = frame_rig.main_checkout()\nout = os.path.join(main_root, 'out', 'ps1_%d' % os.getpid())\n", False)
    expect('ok: run_out(...)', head + "import run_dirs\nout = run_dirs.run_out('ps1', a.out)\n", False)
    expect('ok: GATE_CHECK_DIR', head + "main_root = frame_rig.main_checkout()\nout = os.environ.get('GATE_CHECK_DIR') or os.path.join(main_root, 'out', 'x')\n", False)
    expect('ok: per-checkout ROOT default (private to a worktree)', head + "ROOT = os.path.dirname(__file__)\nout = os.path.join(ROOT, 'out', 's103', 'L8')\n", False)
    expect('ok: main checkout read as an INPUT (captures)', head + "main_root = frame_rig.main_checkout()\ncaps = os.path.join(main_root, 'out', 'captures')\n", False)
    expect('ok: marked shared product', head + "main_root = frame_rig.main_checkout()\nout = os.path.join(main_root, 'out', 'atlas')  # out-dir-ok: the atlas every run reads\n", False)
    # a planted script on disk, run through the file path too
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, 'planted_check.py')
        open(p, 'w').write(head + "main_root = frame_rig.main_checkout()\nout = os.path.join(main_root, 'out', 's103', 'ps1')\n")
        got = bool(lint_files([p]))
        print(('PASS' if got else 'FAIL') + '  planted script file is flagged by lint_files')
        if not got:
            fails.append('file')
    print('PASS  out_dir_lint selftest' if not fails else 'FAIL  out_dir_lint selftest: ' + ', '.join(fails))
    return 1 if fails else 0


def load_baseline():
    """entries 'relpath:name | reason' of defaults that are shared on purpose (unpacked-input caches reused by later runs) or not yet converted; the list must only shrink"""
    out = set()
    try:
        for ln in open(BASELINE, encoding='utf8'):
            ln = ln.split('#')[0].strip()
            if ln:
                out.add(ln.split('|')[0].strip().replace(chr(92), '/'))
    except OSError:
        pass
    return out


def key_of(problem):
    m = re.match(r"(.+?):\d+: (.+?) defaults to", problem)   # 'file:line: 'name' defaults to ...' / 'file:line: argparse --out defaults to ...'
    return (m.group(1).replace(chr(92), '/') + ':' + m.group(2).replace("'", '').replace('argparse ', '')) if m else problem


def main(argv):
    if '--selftest' in argv:
        return selftest()
    files = [a for a in argv if not a.startswith('-')] or lane_scripts()
    base = load_baseline() if not [a for a in argv if not a.startswith('-')] else set()
    bad = [b for b in lint_files(files) if key_of(b) not in base]
    for b in bad:
        print('FAIL  ' + b)
    print(('FAIL  out_dir_lint: %d gate-run script default(s) share a fixed main-checkout output folder (use tools\\scene\\run_dirs.py run_out)' % len(bad)) if bad
          else 'PASS  out_dir_lint: %d scripts, no new fixed shared output default (%d baseline entries: tools/gate/out_dir_lint_baseline.txt)' % (len(files), len(base)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
