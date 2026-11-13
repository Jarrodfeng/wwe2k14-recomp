r"""Lane 69 (S101) selftest of tools\particles\tex_owner.py: the shipped tool is run as a program on Check B's case zip (and Bryan's), clean and with its plant. A positive control.

  py -I tools\particles\tex_selftest.py

1. clean: exit 0; the table names 10 images (Check B 9 particle + 1 flash; Bryan 1 particle + 1 flash) and none reads "not found".
2. plant corrupt_image (the first particle image's bytes flipped in the loaded copy): exit 1, the planted image's line reads "not found" with the PLANTED marker, and the owner check FAILs
   for exactly that one image; every other image is still named.
3. a missing input fails loudly (a nonexistent capture path: exit 1, 'read' FAIL).
0 game runs; reads the case zips from the main checkout's out\ (RDR_MAIN_CHECKOUT or git's common dir)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pcap  # noqa: E402

RUNS = os.path.join(pcap.MAIN, 'out', 's101', 'runs')
BROCK = os.path.join(RUNS, '20261111_215201_s100_owner_s89_brock', 'case_20261111_215233_217_0_pc_run_exit.zip')
BRYAN = os.path.join(RUNS, '20261111_174549_s100_owner_bryan_entrance', 'case_20261111_174607_799_0_pc_run_exit.zip')
fails = []


def check(name, ok, detail=''):
    print('%s  %s %s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        fails.append(name)


def run(args):
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'tex_owner.py')] + args, capture_output=True, text=True, timeout=1800)
    return r.returncode, r.stdout


def main():
    for p in (BROCK, BRYAN):
        if not os.path.exists(p):
            print('FAIL  cannot read %s (a selftest that reads nothing must fail)' % p)
            return 1
    code, out = run(['brock=' + BROCK, 'bryan=' + BRYAN, '-q'])
    lines = [l for l in out.splitlines() if l.strip().startswith('image ')]
    check('clean.exit', code == 0, 'exit %d' % code)
    check('clean.images', len(lines) == 12, '%d image lines (want 12: Check B 9 + 1, Bryan 1 + 1)' % len(lines))
    check('clean.named', lines and not any('not found' in l for l in lines), 'no "not found" line')
    code, out = run(['brock=' + BROCK, '--plant', 'corrupt_image', '-q'])
    lines = [l for l in out.splitlines() if l.strip().startswith('image ')]
    planted = [l for l in lines if 'PLANTED' in l]
    nf = [l for l in lines if 'not found' in l]
    check('plant.exit', code == 1, 'exit %d (want 1)' % code)
    check('plant.not_found', len(planted) == 1 and len(nf) == 1 and nf[0] == planted[0], 'planted %d, not found %d, the same line' % (len(planted), len(nf)))
    m = re.search(r'FAIL\s+owner\s+.*not found \[(\d+)\]', out)
    check('plant.owner_fail', bool(m), 'owner FAIL naming the one image %s' % (m.group(1) if m else '-'))
    code, out = run(['nothing=' + os.path.join(pcap.MAIN, 'out', 'no_such_capture.zip'), '-q'])
    check('missing.exit', code == 1 and 'FAIL  read' in out, 'exit %d, read FAIL' % code)
    print('%d problems' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
