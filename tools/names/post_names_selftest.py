r"""S101 lane PN selftest (gate lane post_names). 0 game runs.

  1  post_names_check.py --static: the q index's 22 queue sites in 13 effect functions and the direct callers all have a row with a technique
  2  post_origin_test.cpp: the SHIPPED PostSiteScope / ApplyPostSite -> names_draw.h -> origin_table.h path names every site (clang++)
  3  planted twin: the same test built with -DS101_PLANT_POST_DROP_LR (the hook drops the LR) must FAIL: the quads stay unnamed
  4  the live checker's positive control: Check B's case zip (taken before the hooks) is FAILED by post_names_check.py (all chain draws on the
     generic queue site); with --synth (call sites rewritten in memory to the expected functions) it PASSES. Skipped when the zip is absent.
Exit 1 on any failure.
"""
import subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
MAIN = Path('C:/Users/Jarrod/wwe2k14-recomp')
ZIP = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
bad = []


def run(cmd, timeout=600):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, cwd=REPO)
    return p.returncode, p.stdout + p.stderr


def report(name, ok, detail=''):
    print('%s  %s  %s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        bad.append(name)


def main():
    code, out = run([sys.executable, '-I', HERE / 'post_names_check.py', '--static'])
    report('1 static: queue callers named', code == 0, out.strip().splitlines()[-2] if code == 0 else out.strip()[-300:])
    clang = Path('C:/Program Files/LLVM/bin/clang++.exe')
    outd = REPO / 'out' / 'names'
    outd.mkdir(parents=True, exist_ok=True)
    exe, exe_p = outd / 'post_origin_test.exe', outd / 'post_origin_test_plant.exe'
    base = [clang, '-std=c++20', '-Wall', '-Wextra', '-I', REPO / 'src' / 'renderer_adapter', HERE / 'post_origin_test.cpp']
    code, out = run(base + ['-o', exe]) if clang.exists() else (1, 'clang++ not found')
    report('2 post_origin_test builds', code == 0, out.strip()[-200:] or 'ok')
    if code == 0:
        code, out = run([exe])
        report('2 shipped path names every post site', code == 0 and 'FAIL' not in out, next((l for l in out.splitlines() if l.startswith('FAIL')), '0 failures'))
        code, out = run(base[:2] + ['-DS101_PLANT_POST_DROP_LR'] + base[2:] + ['-o', exe_p])
        if code == 0:
            code, out = run([exe_p])
        report('3 planted twin: a hook that drops the LR leaves the quads unnamed (test fails)', code != 0 and 'FAIL  every queue site' in out,
               f'exit {code}')
    if (MAIN / ZIP).exists():
        code, out = run([sys.executable, '-I', HERE / 'post_names_check.py', f'brock={ZIP}'])
        report('4a checker FAILS a capture from before the hooks', code != 0 and 'on a generic queue site' in out, out.strip().splitlines()[0][:160] if out.strip() else '')
        code, out = run([sys.executable, '-I', HERE / 'post_names_check.py', '--synth', f'brock={ZIP}'])
        report('4b checker PASSES the same capture with names synthesised', code == 0 and '0 FAIL' in out, out.strip().splitlines()[0][:160] if out.strip() else '')
    else:
        print('SKIP  4 checker positive control (zip absent)')
    print('%d FAIL' % len(bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
