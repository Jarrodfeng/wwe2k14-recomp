#!/usr/bin/env python3
r"""Twin of host_rig.py's parallel arms (S103 GSP). 0 game runs.

  py -I tools\scene\host_rig_serial_twin.py [--root DIR] [--only a04_entrance]

Runs host_rig.py twice on the same capture with the gate's own flags (--no-tween), once in parallel (the default workers) and once forced serial (--serial, the old
order: one host_rig.exe at a time), each into its own output folder, and requires:
  1. both exit codes equal (and 0),
  2. the verdict text BYTE-identical (same lines, same order; wall-clock number lines cut by normalise(): they differ between any two runs, even two serial ones),
  3. the parallel run really ran arms side by side (--workers 4 given; the serial run's PROC limit is 1): checked by the wall time being shorter only when asked for (--expect-faster).
S103 GSP review fixes: (4, bug 7) a parallel run WITHOUT --no-tween exits 0 and prints the verdict-line count line (3 frames + 3 missing-input + 1 stamps + 5 stamps twins + 6 plant twins + 12 tween); (5, bug 1) --break-plants (planted runs get a missing scene folder): exit non-zero, all 11 planted twin lines read ERROR, none `caught`; (6, bug 2) lint: every run in stamps_section has excl=True.
Exit 0 = identical and all hold."""
import argparse
import ast
import re
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WORKERS = os.environ.get('GATE_GPU_PROCS') or '4'   # the gate gives this check gpu_procs slots and says how many in this variable: the parallel runs use exactly that many processes


def run_rig(cmd):
    """host_rig.py with a hard timeout (S103 GST: both runs had none; a hung one held GPU slots until an outer cap killed the whole gate). On expiry: exit 124 and the tail of what it printed.
    WWE_TWIN_TIMEOUT_S overrides the 1200 s default."""
    tmo = float(os.environ.get('WWE_TWIN_TIMEOUT_S') or 1200)
    # S103 GST review 9: Popen + taskkill /T (as gate.run_tree): subprocess.run's timeout killed only the python child and left its host_rig.exe grandchildren holding GPU slots;
    # the child's own checked waits get a deadline 30 s before ours (host_rig.cpp reads WWE_RIG_DEADLINE_S), so a hung arm ends itself first
    env = dict(os.environ, WWE_RIG_DEADLINE_S=str(max(30, int(tmo) - 30)))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors='replace', env=env)
    try:
        out, err = proc.communicate(timeout=tmo)
        return subprocess.CompletedProcess(cmd, proc.returncode, out, err)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)], capture_output=True)
        else:
            proc.kill()
        try:
            out, err = proc.communicate(timeout=20)
        except Exception:
            out, err = '', ''
        out, err = out or '', err or ''
        print(f'host_rig_serial_twin: host_rig.py did not finish in {tmo:.0f} s ({" ".join(cmd[-4:])}); last output:' + chr(10) + chr(10).join((out + err).splitlines()[-12:]), flush=True)
        return subprocess.CompletedProcess(cmd, 124, out, err)


def normalise(text):
    """The rig prints wall-clock numbers that differ between ANY two runs (even two serial ones): the per-frame post counts / frame totals of the host's free-running thread and the GPU stamp timings. Those lines are cut to their verdict part; every other line is compared whole."""
    out = []
    for ln in text.splitlines():
        if ln.startswith('   host frame') or ln.startswith('   [host] frames'):
            continue
        k = ln.find(': gpu stamps: PC numbers')
        out.append(ln[:k] if k >= 0 else ln)
    return chr(10).join(out) + chr(10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    ap.add_argument('--only', default='a04_entrance')
    ap.add_argument('--expect-faster', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    rig = os.path.join(root, 'tools', 'scene', 'host_rig.py')
    res = {}
    for mode in ('par', 'ser'):
        out = os.path.join(root, 'out', 'gsp_twin_rig', mode)
        cmd = [sys.executable, '-I', rig, '--root', root, '--no-tween', '--only', a.only, '--out', out] + (['--serial'] if mode == 'ser' else ['--workers', WORKERS])
        t = time.time()
        p = run_rig(cmd)
        res[mode] = (p.returncode, p.stdout, time.time() - t, p.stderr)
    (rp, tp, wp, ep), (rs, ts, ws, es) = res['par'], res['ser']
    raw_p, raw_s = tp, ts
    tp, ts = normalise(tp), normalise(ts)
    ok = True
    if rp != rs or rp != 0:
        print('FAIL: exit codes differ or non-zero (parallel %d, serial %d)\n%s%s' % (rp, rs, tp[-1500:], ep[-500:]))
        ok = False
    if tp != ts:
        a_l, b_l = tp.splitlines(), ts.splitlines()
        first = next((i for i in range(max(len(a_l), len(b_l))) if (a_l[i] if i < len(a_l) else None) != (b_l[i] if i < len(b_l) else None)), -1)
        print('FAIL: the verdict text differs (line %d):\n  parallel: %s\n  serial:   %s' % (first + 1, a_l[first] if 0 <= first < len(a_l) else '(none)', b_l[first] if 0 <= first < len(b_l) else '(none)'))
        ok = False
        print('--- raw parallel run:')
        print(raw_p[-6000:])
        print('--- raw serial run:')
        print(raw_s[-6000:])
    if a.expect_faster and not wp < ws:
        print('FAIL: the parallel run (%.0f s) was not faster than the serial run (%.0f s)' % (wp, ws))
        ok = False
    print('%s host_rig parallel vs serial: %d lines each, byte-identical %s; wall parallel %.0f s, serial %.0f s' % ('PASS' if ok else 'FAIL', len(tp.splitlines()), tp == ts, wp, ws))
    ok &= stamps_lint(rig)
    ok &= tween_run(root, rig, a.only)
    ok &= break_run(root, rig, a.only)
    return 0 if ok else 1


def stamps_lint(rig):
    """Bug 2: every run(...) in stamps_section is exclusive (excl=True) inside the rig."""
    tree = ast.parse(open(rig, encoding='utf-8').read())
    fn = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == 'stamps_section')
    runs = [c for c in ast.walk(fn) if isinstance(c, ast.Call) and isinstance(c.func, ast.Name) and c.func.id == 'run']
    bare = [c.lineno for c in runs if not any(k.arg == 'excl' and isinstance(k.value, ast.Constant) and k.value.value is True for k in c.keywords)]
    ok = bool(runs) and not bare
    print('%s stamps lint: %d host_rig.exe runs in stamps_section, %s' % ('PASS' if ok else 'FAIL', len(runs), 'all excl=True' if ok else 'without excl=True at line(s) %s' % bare))
    return ok


def go(root, rig, only, tag, extra):
    out = os.path.join(root, 'out', 'gsp_twin_rig', tag)
    cmd = [sys.executable, '-I', rig, '--root', root, '--only', only, '--out', out, '--workers', WORKERS] + extra
    t = time.time()
    p = run_rig(cmd)
    return p.returncode, p.stdout + p.stderr, time.time() - t


def tween_run(root, rig, only):
    """Bug 7: the arm the gate's --no-tween never covers: a parallel run with the tween section, every verdict line there."""
    rc, txt, secs = go(root, rig, only, 'tween', [])
    m = re.search(r'verdict lines \[%s\]: all there \(([^)]*)\)' % re.escape(only), txt)
    ok = rc == 0 and m is not None and '12 tween' in m.group(1)
    print('%s host_rig parallel WITH tween (no --no-tween): exit %d, %s, %.0f s%s' % ('PASS' if ok else 'FAIL', rc, m.group(0) if m else 'no verdict-lines line', secs, '' if ok else '\n' + txt[-2500:]))
    return ok


def break_run(root, rig, only):
    """Bug 1: planted runs that die for their own reasons are ERROR, never caught."""
    rc, txt, secs = go(root, rig, only, 'break', ['--break-plants'])
    planted = [ln for ln in txt.splitlines() if re.match(r'^twin (stale_record|skip_copy|wrong_size|shot_no_curve|shot_late|stale_walks|tween \w+) \[', ln) and not ln.startswith('twin tween lookup')]
    errs = [ln for ln in planted if ': ERROR (' in ln]
    caught = [ln for ln in planted if ': caught' in ln]
    ok = rc != 0 and len(planted) == 11 and len(errs) == 11 and not caught
    print('%s host_rig --break-plants: exit %d, %d planted twin lines (want 11), %d ERROR, %d caught (want 0), %.0f s%s' % ('PASS' if ok else 'FAIL', rc, len(planted), len(errs), len(caught), secs, '' if ok else '\n' + '\n'.join(planted)[-2500:] + '\n' + txt[-1500:]))
    return ok


if __name__ == '__main__':
    sys.exit(main())
