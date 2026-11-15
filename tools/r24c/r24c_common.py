"""S102 lane R24-C shared helpers: the captures, the arms, the fview runner (exit code AND `validation errors N` are read, never the csv's existence)."""
import os
import re
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPTURES = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'brock': 'out/s101/runs/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip',
    'wm30': 'out/s101/runs/20261112_131029_s100_owner_s90_wm30/case_20261112_131111_940_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
# the whole bundle the players run (the S101 promotions, wwe2k14_app.h); same list as tools/g12/g12_check.py
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
LOW = ['--shadow-map-size', '256', '--shadow-half', '--copy-alias', '2', '--copy-alias-classes', '11', '--alias-fullsize', '1']


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def read_hashes(path):
    out = {}
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                a, b = ln.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


class Run:
    def __init__(self, exe, zip_path, game, work, name, extra, frames=1000, timeout=1700):
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', str(frames), '--validation', '1', '--game', game, '--hash-frames', hashes] + extra
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text and not (p.returncode not in (0, 4) and 'validation errors' not in self.text):   # a crash with no verdict (0xC0000409 when another lane's heavy run overlaps) is retried; exit 4 / a verdict is real
                break
            time.sleep(20)
        self.rc = p.returncode
        with open(os.path.join(work, name + '.log'), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.hashes = read_hashes(hashes) if os.path.isfile(hashes) else {}
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = re.search(r'frames planned (\d+) \(skipped (\d+)\), violations (\d+)', self.text)
        self.frames = int(m.group(1)) if m else 0
        self.clean = self.rc == 0 and self.valid == 0

    def lines(self, prefix):
        return [l for l in self.text.splitlines() if l.startswith(prefix)]

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


def run_fview(exe, zip_path, game, work, name, extra, **kw):
    return Run(exe, zip_path, game, work, name, extra, **kw)
