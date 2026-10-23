"""Export our ReXGlue SDK changes as a verified chain of diffs (Session 25).

The SDK fork (thirdparty\\rexglue-sdk-src, branch wwe2k14-local) is a separate git repository that this repo ignores, so
these diffs are the only copy of our SDK changes that travel with the project. The older patches\\rexglue-sdk\\00NN-*.patch
files are kept as the historical record, but applied in order they do NOT rebuild the branch (some are summaries,
commits 87-103 are missing, and 35 merge commits can't be expressed as per-commit patches).

This writes patches\\rexglue-sdk\\full\\NN-<session>.diff: tree-to-tree diffs between the last pinned SDK build of each
session (tools\\sdk_pins.json), so merges are included. Then it checks the chain: upstream base + every diff in order
must give exactly the branch head's tree (a temporary git index; no files in the SDK checkout are touched).

  py tools\\sdk_patches\\export_series.py           export + verify (run after any SDK change you keep)
  py tools\\sdk_patches\\export_series.py --check   verify the committed diffs only
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

R = Path(__file__).resolve().parents[2]
SDK = R / 'thirdparty' / 'rexglue-sdk-src'
OUT = R / 'patches' / 'rexglue-sdk' / 'full'
BASE = 'f5337cd'          # upstream rexglue/rexglue-sdk commit the fork starts from
BRANCH = 'wwe2k14-local'
# The last pinned build of each session (tools\sdk_pins.json), in order. Add a line per new session.
STEPS = [
    ('sessions03-07c', 'd2512163c84bfe1dfdbd6ae62964650b72dd25bc'),
    ('session08', '728667b8531b99288e51cde46f9f6c528580ae79'),
    ('session14', '32fae132798bd73ef98386ccaf9fe4148efa560b'),
    ('session15', 'f77004c2896740c30b958f0c2aeb39bac085f7b9'),
    ('session16', '69a47ab172944d4bf2546f57d621dfc84e179683'),
    ('session18', '4057aa9912c8303530458578f79c6562c607dc63'),
    ('session19', '2d9a84f0718e5c19148dfdc54a9d49c0570711cf'),
    ('session20', 'a63272c9e3a9bcda8c637fff647722cb68217b48'),
    ('session21', 'fef3684baf86d68e9f2563a2d9d3b74ea9906889'),
    ('session22', '63b87cfa2981455d61b5e22f1908f70da0780041'),
    ('session23', 'cf61391d3b4090206fc4b37708312b0c7a26b2b9'),
    ('session24', '6a2666fe6dadfe587bf50c4e84caa0b69858e538'),
    ('session26', '2aaec24b364c7d5494f6469089fd85047628a793'),
    ('session26b', '7412a803920fa9ecf587caa8a69d71db3f687a50'),
    ('session27', '8794393499be25948763faa8c0b0831566a901d3'),
    ('session27_5', 'e5ebb5bbbb41c2b970f378ad60a72ce37f1fcefc'),
    ('session30', '0750a5bd1b4a53b37b4f06a17ae6bfbc1a1985cd'),
    ('session32', 'b82208db3eaf6fd068298a57c1a6fc9f7e22efe6'),
    ('session33', '5cdb5475bb5c2c71b122b0752e9a93a4a654e5b9'),
    ('session37', '48c1e6cc7ff26a3f1bd688011b431a9529eaf26a'),
    ('sessions38-53', 'e0f72778d351da15341cf1b7e963d4f39ee68e4e'),
    ('session54', 'ec136eb164d9aef10fb99d8e1cb7087f28691f13'),
    ('session54b', '50c9137a0a1f166989d82696ed97407ced02d1e1'),
]


def git(*args, env=None, text=True):
    r = subprocess.run(['git', '-C', str(SDK), *args], capture_output=True, text=text, env=env)
    if r.returncode:
        sys.exit(f'git {" ".join(args)} failed: {r.stderr if text else r.stderr.decode(errors="replace")}')
    return r.stdout


def verify():
    files = sorted(OUT.glob('*.diff'))
    head_tree = git('rev-parse', f'{BRANCH}^{{tree}}').strip()
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / 'index'))
        git('read-tree', BASE, env=env)
        for f in files:
            r = subprocess.run(['git', '-C', str(SDK), 'apply', '--cached', '--binary', str(f)], capture_output=True,
                               text=True, env=env)
            if r.returncode:
                sys.exit(f'FAIL: {f.name} does not apply: {r.stderr.strip()[:300]}')
        tree = git('write-tree', env=env).strip()
    if tree != head_tree:
        sys.exit(f'FAIL: base + {len(files)} diffs = tree {tree}, but {BRANCH} is {head_tree}')
    head = git('rev-parse', '--short', BRANCH).strip()
    print(f'PASS: upstream {BASE} + {len(files)} diffs = {BRANCH} ({head}) exactly (tree {tree[:12]})')


def export():
    head = git('rev-parse', BRANCH).strip()
    if STEPS[-1][1] != head:
        sys.exit(f'{BRANCH} is at {head[:7]}, the last step is {STEPS[-1][1][:7]}: add a STEPS line for the new SDK state')
    prev = BASE
    for _, c in STEPS:  # each step must build on the one before
        if subprocess.run(['git', '-C', str(SDK), 'merge-base', '--is-ancestor', prev, c]).returncode:
            sys.exit(f'{prev[:7]} is not an ancestor of {c[:7]}')
        prev = c
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob('*.diff'):
        old.unlink()
    prev = BASE
    for i, (label, c) in enumerate(STEPS, 1):
        diff = git('diff', '--binary', '--full-index', prev, c, text=False)
        subjects = git('log', '--first-parent', '--format=  %h %s', f'{prev}..{c}').rstrip()
        header = (f'# ReXGlue SDK changes for WWE 2K14 ({label}): {prev[:12]}..{c[:12]} on {BRANCH}\n'
                  f'# Apply in order on upstream rexglue/rexglue-sdk {BASE} (git apply). Commits (first parent):\n'
                  + ''.join(f'#{line}\n' for line in subjects.splitlines()) + '\n')
        path = OUT / f'{i:02d}-{label}.diff'
        path.write_bytes(header.encode() + diff)
        print(f'{path.name}: {len(diff) // 1024} KB, {len(subjects.splitlines())} commits')
        prev = c
    verify()


if __name__ == '__main__':
    verify() if '--check' in sys.argv else export()
