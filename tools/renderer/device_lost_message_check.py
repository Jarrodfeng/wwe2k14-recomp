r"""S105 A9: builds and runs tools\renderer\device_lost_message_test.cpp (host clang, 0 game runs): the plain run must PASS, each planted twin must FAIL.
  py -I tools\renderer\device_lost_message_check.py [--root DIR]
Exit 1 on any failure."""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    root = os.path.abspath(ap.parse_args().root)
    out = os.path.join(root, 'out', 'A9')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'device_lost_message_test.exe')
    cmd = [r'C:\Program Files\LLVM\bin\clang++.exe', '-std=gnu++20', '-I', os.path.join(root, 'src'), '-I', os.path.join(root, 'src', 'renderer_adapter'),
           os.path.join(root, 'tools', 'renderer', 'device_lost_message_test.cpp'), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print('FAIL  build:', r.stdout + r.stderr)
        return 1
    bad = 0
    p = subprocess.run([exe], capture_output=True, text=True)
    ok = p.returncode == 0
    print(('PASS  ' if ok else 'FAIL  ') + 'the plain run passes')
    bad += not ok
    for plant in ('plant_switch_off', 'plant_text', 'plant_unknown_off'):
        q = subprocess.run([exe, plant], capture_output=True, text=True)
        ok = q.returncode == 1
        print(('PASS  ' if ok else 'FAIL  ') + 'planted twin %s is caught' % plant)
        bad += not ok
    print('device_lost_message_check: %s' % ('PASS' if not bad else 'FAIL'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
