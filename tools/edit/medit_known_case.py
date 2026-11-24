r"""S110 MEDIT, the known case: turns a real multi-file commit into ONE medit EDITFILE, applies it to the parent's files in a temp folder and
compares every touched file byte for byte with the commit's own version.

    py -I tools\edit\medit_known_case.py [COMMIT]        (default 0fa8b6e1f, the SEAT-A review-fix commit)

FIND = a hunk's old lines (git diff -U3 context, grown line by line when that is not unique in the parent file, never into a neighbouring
hunk); WITH = the same lines with the hunk's change. Blocks of one file are written bottom hunk first, so no FIND sees another block's change.
The working tree is never touched: only the touched files of COMMIT^ are extracted (git archive | tar) into a temp folder.
Prints blocks, files, the wall time of the ONE medit call and whether every file is byte-identical (CRLF included)."""
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MEDIT = os.path.join(HERE, 'medit.py')
REPO = os.path.dirname(os.path.dirname(HERE))
P = '%%medit '


def git(*a, binary=False):
    r = subprocess.run(['git', '-C', REPO] + list(a), capture_output=True, timeout=300)
    if r.returncode != 0:
        raise SystemExit('git %s failed: %s' % (' '.join(a), r.stderr.decode('utf-8', 'replace')))
    return r.stdout if binary else r.stdout.decode('utf-8', 'replace')


def hunks_of(commit, path):
    """[(old_start, old_count, [(tag, line)])] from git diff -U3; lines without their line ending"""
    raw = git('diff', '-U3', '--no-color', commit + '^', commit, '--', path, binary=True).decode('utf-8', 'replace')
    out, cur = [], None
    for ln in re.split(r'\r?\n', raw):
        m = re.match(r'@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@', ln)
        if m:
            cur = (int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1), [])
            out.append(cur)
        elif cur is not None and ln[:1] in (' ', '-', '+'):
            cur[2].append((ln[0], ln[1:]))
    return out


def fetch(rev, files, mode):
    """{path: bytes} of the files at rev: 'archive' = git archive | tar (what a checkout export holds), 'blob' = the committed blobs"""
    if mode == 'blob':
        return {f: git('cat-file', 'blob', rev + ':' + f, binary=True) for f in files}
    tar = git('archive', rev, '--', *files, binary=True)
    out = {}
    with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
        for m in tf.getmembers():
            if m.isfile():
                out[m.name] = tf.extractfile(m).read()
    return out


def main(argv):
    keep = argv[argv.index('--keep') + 1] if '--keep' in argv else None      # copy the generated editfile there (look at it)
    argv = [a for i, a in enumerate(argv) if a != '--keep' and (i == 0 or argv[i - 1] != '--keep')]
    commit = argv[0] if argv else '0fa8b6e1f'
    files = [f for f in git('diff', '--name-only', commit + '^', commit).split('\n') if f]
    return max(known(commit, files, mode, keep) for mode in ('archive', 'blob'))


def known(commit, files, mode, keep):
    tmp = tempfile.mkdtemp(prefix='medit_known_')
    try:
        for f, data in fetch(commit + '^', files, mode).items():
            os.makedirs(os.path.dirname(os.path.join(tmp, f)), exist_ok=True)
            with open(os.path.join(tmp, f), 'wb') as fh:
                fh.write(data)
        wants = fetch(commit, files, mode)
        blocks, notes = [], []
        for f in files:
            parent = open(os.path.join(tmp, f), 'rb').read().decode('utf-8')
            plines = re.split(r'\r?\n', parent)
            joined = '\n'.join(plines)
            hs = hunks_of(commit, f)
            for hi in range(len(hs) - 1, -1, -1):
                start, cnt, body = hs[hi]
                old = [l for t, l in body if t in ' -']
                new = [l for t, l in body if t in ' +']
                # the hunk's own context may already be inside the neighbours' territory: clamp the growth to them
                o0 = start - 1                      # index of the hunk's first old line (context included, -U3)
                o1 = o0 + len(old)
                lo = (hs[hi - 1][0] - 1 + hs[hi - 1][1]) if hi > 0 else 0
                hi_lim = (hs[hi + 1][0] - 1) if hi + 1 < len(hs) else len(plines)
                k = 0
                while True:
                    a, b = max(lo, o0 - k), min(hi_lim, o1 + k)
                    ctx_up, ctx_dn = plines[a:o0], plines[o1:b]
                    find = '\n'.join(ctx_up + old + ctx_dn)
                    if joined.count(find) == 1:
                        break
                    if a == lo and b == hi_lim:
                        notes.append('%s hunk @%d: FIND not unique even grown to the neighbours' % (f, start))
                        break
                    k += 1
                with_ = '\n'.join(ctx_up + new + ctx_dn)
                blocks.append(P + 'REPLACE ' + f + '\n' + P + 'FIND\n' + find + '\n' + P + 'WITH\n' + with_ + '\n' + P + 'END\n')
                if k:
                    notes.append('%s hunk @%d: FIND grown by %d line(s) of context' % (f, start, k))
        ef = os.path.join(tmp, 'known.medit')
        text = ''.join(blocks)
        # a blank WITH line is one blank line, but a WITH with no line at all is a deletion: a hunk that only deletes keeps its context lines
        with open(ef, 'w', encoding='utf-8', newline='') as fh:
            fh.write(text)
        if keep:
            shutil.copyfile(ef, keep)
        t0 = time.time()
        r = subprocess.run([sys.executable, '-I', MEDIT, ef, '--root', tmp], capture_output=True, text=True, timeout=300, encoding='utf-8', errors='replace')
        dt = time.time() - t0
        print(r.stdout.strip())
        print(r.stderr.strip())
        same = 0
        for f in files:
            want = wants[f]
            got = open(os.path.join(tmp, f), 'rb').read()
            ok = got == want
            same += ok
            print('%s %s (%d bytes, %d CRLF)' % ('IDENTICAL' if ok else 'DIFFERS  ', f, len(want), want.count(b'\r\n')))
            if not ok:
                for i, (x, y) in enumerate(zip(got, want)):
                    if x != y:
                        print('   first difference at byte %d' % i)
                        break
                else:
                    print('   lengths %d vs %d' % (len(got), len(want)))
        for n in notes:
            print('note: ' + n)
        print('known case %s [%s]: %d block(s), %d file(s), one medit call %.2f s, rc %d, %d/%d files byte-identical%s' % (
            commit, mode, len(blocks), len(files), dt, r.returncode, same, len(files), '' if same == len(files) else '  <-- NOT IDENTICAL'))
        return 0 if same == len(files) and r.returncode == 0 else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
