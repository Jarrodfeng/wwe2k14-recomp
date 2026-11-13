r"""Fails when a tracked text file holds control bytes that an escape-processed Windows path leaves behind (S101, 2026-11-12).

`docs\renderer` written through a non-raw Python string or a shell heredoc becomes `docs` + CR + `enderer`; `\f`, `\b`, `\v`, `\a`
become FF / BS / VT / BEL. 32 files had them (lane 151's review found the first). Checked: FF, BS, VT, BEL, and a CR not followed by LF.
  py -I tools\harness\text_hygiene.py [--selftest]      exit 1 and one line per file when any is found
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXT = ('.md', '.py', '.ps1', '.json', '.txt', '.h', '.cpp', '.toml', '.vert', '.frag', '.comp', '.glsl', '.cmake', '.yml', '.yaml')
BAD = re.compile(rb'[\x07\x08\x0b\x0c]|\r(?!\n)')


def scan(paths):
    found = []
    for rel in paths:
        if not rel.lower().endswith(EXT):
            continue
        try:
            b = (ROOT / rel).read_bytes()
        except OSError:
            continue
        hits = [m.start() for m in BAD.finditer(b)]
        if hits:
            line = b.count(b'\n', 0, hits[0]) + 1
            found.append('%s: %d control byte(s), first on line %d (%r)' % (rel, len(hits), line, b[hits[0]:hits[0] + 1]))
    return found


def selftest():
    import tempfile
    global ROOT
    with tempfile.TemporaryDirectory() as d:
        old, ROOT = ROOT, Path(d)
        (ROOT / 'ok.md').write_bytes(b'a `docs\\renderer\\x.md` line\r\nnext\n')
        (ROOT / 'bad.md').write_bytes(b'a `docs\renderer` line\n')         # the real slip: CR + 'enderer'
        (ROOT / 'ff.py').write_bytes(b'# tools\x0cview\n')
        got = scan(['ok.md', 'bad.md', 'ff.py'])
        ROOT = old
    ok = len(got) == 2 and got[0].startswith('bad.md') and got[1].startswith('ff.py')
    print('text_hygiene selftest: %s (%s)' % ('PASS' if ok else 'FAIL', got))
    return 0 if ok else 1


def main():
    if '--selftest' in sys.argv:
        return selftest()
    files = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True).stdout.splitlines()
    found = scan(files)
    for f in found:
        print(f)
    print('text_hygiene: %s (%d tracked text files with control bytes)' % ('FAIL' if found else 'PASS', len(found)))
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
