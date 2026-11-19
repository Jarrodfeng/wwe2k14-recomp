#!/usr/bin/env python3
r"""S103 GT review: no control characters in our text files. A `\b` written through a shell / python string became a literal 0x08 (backspace) in a comment or a path
(renderer.cpp, scene_record.h, keep_list.txt were found damaged); a stray control character is invisible in an editor and breaks greps and path lookups.

  py -I tools\lint\no_control_chars.py [--root DIR] [--selftest]

Scans every git-tracked text file under src/, tools/, docs/ and the root *.md / *.txt / *.json / *.toml / *.cmake (C++ / python / PowerShell / markdown / text / json / csv / toml / cmake / html / js /
java / kotlin / xml / gradle). A control character is a byte below 0x20 other than TAB, LF, CR, or 0x7F, or a form feed / vertical tab. Each hit is `file:line: 0xNN near "..."` (the 20 characters
before it, controls shown as <0xNN>). Exit 0 = none; 1 = hits. --selftest plants a file with a 0x08 in a comment and checks it is reported (and a clean file is not).
"""
import argparse, os, subprocess, sys, tempfile

EXT = {'.h', '.hpp', '.c', '.cc', '.cpp', '.cxx', '.py', '.ps1', '.psm1', '.md', '.txt', '.json', '.csv', '.toml', '.cmake', '.html', '.js', '.java', '.kt', '.xml', '.gradle', '.sh', '.bat', '.glsl', '.vert', '.frag', '.comp', '.yml', '.yaml'}
OKAY = {0x09, 0x0A, 0x0D}


def tracked(root):
    p = subprocess.run(['git', '-C', root, 'ls-files', '-z', '--', 'src', 'tools', 'docs', '*.md', '*.txt', '*.json', '*.toml', '*.cmake'], capture_output=True, timeout=120)
    return [x.decode('utf-8', 'replace') for x in p.stdout.split(b'\0') if x]


def scan_bytes(data):
    """[(line, byte, context)] for every control byte in `data`."""
    hits = []
    line = 1
    start = 0
    for i, b in enumerate(data):
        if b == 0x0A:
            line += 1
            start = i + 1
        elif (b < 0x20 and b not in OKAY) or b == 0x7F:
            ctx = ''.join(chr(c) if 0x20 <= c < 0x7F else '<0x%02X>' % c for c in data[max(start, i - 20):i + 1])
            hits.append((line, b, ctx))
    return hits


def scan_tree(root):
    out = []
    for rel in tracked(root):
        if os.path.splitext(rel)[1].lower() not in EXT:
            continue
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue
        with open(path, 'rb') as f:
            data = f.read()
        for line, b, ctx in scan_bytes(data):
            out.append('%s:%d: 0x%02X near "%s"' % (rel.replace('\\', '/'), line, b, ctx))
    return out


def selftest():
    bad = scan_bytes(b'// path tools\\surface\x08ench_line.py\nint x;\n')
    clean = scan_bytes(b'// path tools\\surface\\bench_line.py\r\n\tint x;\n')
    ok = len(bad) == 1 and bad[0][0] == 1 and bad[0][1] == 8 and not clean
    print(('PASS' if ok else 'FAIL') + '  selftest: a planted 0x08 in a comment is reported (line %s), a clean file with TAB / CR / LF is not' % (bad[0][0] if bad else '-'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    rc = selftest() if a.selftest else 0
    hits = scan_tree(os.path.abspath(a.root))
    for h in hits:
        print('FAIL  ' + h)
    if not hits:
        print('PASS  no control characters in the tracked text files')
    return 1 if hits or rc else 0


if __name__ == '__main__':
    sys.exit(main())
