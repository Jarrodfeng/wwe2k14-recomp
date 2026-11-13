r"""S101 lane PN: the offline tools must give the same results on a capture whose post quads carry the lane PN naming (call_site = the effect's call,
part = the generic queue site) as on the same capture with the old naming.

  py -I tools\names\post_site_synth_test.py [CAPTURE]       default: Check B's case zip

For each of post_census.py, particles\census.py and shadow\shadow_census.py: run its main() on the capture twice, the second time with every op-68 origin
whose call_site is a generic queue site (0x82C1F48C / 514 / 674) rewritten in memory to {call_site = an effect call, part = the generic site}. The two
outputs (timings removed) must be identical. Planted twin: the same rewrite with the tools' generic-site reader replaced by plain call_site must DIFFER
(so the test can fail). Exit 1 on any difference. Nothing is written.
"""
import contextlib, io, os, re, runpy, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
for sub in ('particles', 'solver', 'post', 'shadow'):
    sys.path.insert(0, str(REPO / 'tools' / sub))
import numpy as np
import pcap

DEFAULT = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
EFFECT_SITE = 0x82C1CBC4
TOOLS = [('post_census', REPO / 'tools' / 'post' / 'post_census.py', ['brock={cap}']),
         ('particles_census', REPO / 'tools' / 'particles' / 'census.py', ['brock={cap}']),
         ('shadow_census', REPO / 'tools' / 'shadow' / 'shadow_census.py', ['brock={cap}'])]

_orig_init = pcap.FrameData.__init__


def synth_init(self, *a, **k):
    _orig_init(self, *a, **k)
    o = self.origins
    if o is not None and len(o):
        o = o.copy()
        gen = (o['op'] == 68) & np.isin(o['call_site'], pcap.GENERIC_QUEUE_SITES)
        o['part'][gen] = o['call_site'][gen]
        o['call_site'][gen] = EFFECT_SITE
        self.origins = o


def run_tool(path, args):
    buf = io.StringIO()
    old = sys.argv
    sys.argv = [str(path)] + args
    try:
        with contextlib.redirect_stdout(buf):
            try:
                runpy.run_path(str(path), run_name='__main__')
            except SystemExit as e:
                code = e.code or 0
    finally:
        sys.argv = old
    out = re.sub(r'\d+(\.\d+)? s\b', 'N s', buf.getvalue())
    out = re.sub(r'in \d+(\.\d+)?', 'in N', out)
    return out


def main():
    cap = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    if not os.path.isabs(cap) and not os.path.exists(cap):
        cap = os.path.join(pcap.MAIN, cap)
    if not os.path.exists(cap):
        print('FAIL  input missing: %s' % cap)
        return 1
    bad = 0
    for name, path, args in TOOLS:
        args = [a.format(cap=cap) for a in args]
        pcap.FrameData.__init__ = _orig_init
        a = run_tool(path, args)
        pcap.FrameData.__init__ = synth_init
        b = run_tool(path, args)
        ok = a == b and len(a) > 100
        # planted twin: the old reader (plain call_site) must NOT survive the rewrite
        saved = pcap.queue_site, pcap.queue_part
        pcap.queue_site = lambda o: np.asarray(o['call_site']).astype(np.int64) if np.asarray(o['call_site']).ndim else int(o['call_site'])
        pcap.queue_part = lambda o: np.asarray(o['part']).astype(np.int64) if np.asarray(o['part']).ndim else int(o['part'])
        c = run_tool(path, args)
        pcap.queue_site, pcap.queue_part = saved
        pcap.FrameData.__init__ = _orig_init
        twin = c != a
        print('%s  %-18s same output with the rewritten sites (%d chars); planted plain-call_site reader %s' % ('PASS' if ok and twin else 'FAIL', name, len(a), 'differs (caught)' if twin else 'SAME (not caught)'))
        if not ok:
            bad += 1
            for i, (x, y) in enumerate(zip(a.splitlines(), b.splitlines())):
                if x != y:
                    print('      line %d:\n        old:   %s\n        synth: %s' % (i, x[:200], y[:200]))
                    break
        bad += not twin
    print('%d FAIL' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
