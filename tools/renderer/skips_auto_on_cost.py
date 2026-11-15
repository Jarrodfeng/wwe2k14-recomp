#!/usr/bin/env python3
"""S102 K3 rework: what the D3 chains cost the render thread per handled frame while ON, offline (0 game runs). fview with the throttle (schedule 0:on: every frame is bound with the chains, the skips apply), the
d3_timers.h census line per chain: arena / caster / ring (Begin + End; the End carries Observe, Publish and the census walks). The numbers are fview's, on a 4090-class PC with the capture's frames, so they are a
RELATIVE picture of where the time goes (and a floor for the Surface), not Surface milliseconds.
  py -I tools\\renderer\\skips_auto_on_cost.py [--root DIR]
Prints one line per chain: us a handled frame by slot, over the last 100 frames of the run. Exit 0 when every chain printed a census line."""
import argparse, os, re, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'd3'))
import d3_check as dc  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = dc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    cap = os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture')
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    out = os.path.join(root, 'out', 'k3oncost')
    os.makedirs(out, exist_ok=True)
    chk = os.path.join(root, 'out', 'k3check')
    cap_c = os.path.join(main_co, 'out', 'd3caster', 'z7147', 'capture')
    if not os.path.isfile(os.path.join(cap_c, 'images.bin')):
        cap_c = os.path.join(chk, 'z7147', 'capture')
    arms = [('arena', cap, 341, ['--arena-ours', '--thin', 'arena', '--d3', 'arena', '--d3-ledger-in', os.path.join(chk, 'ledger_B.txt')]),
            ('ring', cap, 341, ['--ring-ours', '--thin', 'ring', '--d3', 'ring', '--d3-ledger-in', os.path.join(chk, 'ledger_ring.txt')]),
            ('casters', cap_c, len(os.listdir(os.path.join(cap_c, 'frames'))) if os.path.isdir(os.path.join(cap_c, 'frames')) else 0, ['--d3', 'casters', '--d3-mode', 'stream', '--d3-ledger-in', os.path.join(chk, 'ledger_casters.txt')])]
    fails = 0
    for name, c, n, extra in arms:
        if not os.path.isdir(c) or not n or not all(os.path.exists(x) for x in extra if x.endswith('.txt')):
            print('FAIL  [%s] input missing (run skips_auto_check.py first for the ledgers): %s' % (name, c))
            fails += 1
            continue
        cmd = [exe, '--capture', c, '--game', game, '--frames', str(n), '--thin-run', os.path.join(out, 'arm_' + name)] + extra
        rc, text = dc.run(cmd, 1500, {'D3_AUTO_SCHED': '0:on', 'D3_AUTO_THROTTLE': '1'})
        lines = re.findall(r'(\[rdr\]\[d3_time\] per handled frame[^\n]*)', text)
        if rc not in (0, 4) or not lines:
            print('FAIL  [%s] no d3_time line (exit %d)' % (name, rc))
            fails += 1
            continue
        print('INFO  [%s] %s' % (name, lines[-1][:700]))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
