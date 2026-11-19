r"""PN5 review B1 lint: no literal tier / texture counts in tools\ui\*.py. The first tier's pac count is kUiTier1Count (src\renderer_scene\ui_file_index.h, read by ui_consts.py);
its texture count is pinned in tools\ui\ui_tier1_textures.txt. A script that types either number goes stale the next time a pac joins the first tier (S103: game2dHD, 10 -> 11).
  py -I tools\ui\ui_consts_lint.py     exit 1 on any hit"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = {'ui_consts.py', 'ui_consts_lint.py'}
RULES = [
    (re.compile(r'\b\d{4,5}\s+textures?\b|\btextures?\b\s*==\s*\d{3,5}\b|group\(1\)\)\s*==\s*\d{3,5}\b'), 'a literal texture count'),
    (re.compile(r'(?i)\btier1\w*\s*=\s*\d+'), 'a literal first-tier pac count'),
    (re.compile(r'int\(\w+\.group\(\d\)\)\s*==\s*\d+\b[^\n#]*\bpacs?\b|\bpacs?\b[^\n#]*==\s*\d+\b'), 'a literal pac count'),
    (re.compile(r'\bwant\s*=\s*\d+\s*\+|\bwant\s*-\s*\d+\b'), 'a literal tier arithmetic'),
]


def main():
    bad = 0
    for path in sorted(glob.glob(os.path.join(HERE, '*.py'))):
        if os.path.basename(path) in SKIP:
            continue
        for n, line in enumerate(open(path, encoding='utf-8'), 1):
            code = line.split('#')[0]
            for rx, why in RULES:
                if rx.search(code):
                    print('FAIL %s:%d %s: %s' % (os.path.relpath(path, os.path.join(HERE, '..', '..')), n, why, line.strip()[:160]))
                    bad += 1
    print('%s  ui_consts_lint: %s' % ('FAIL' if bad else 'PASS', '%d literal tier / texture counts in tools\\ui\\*.py' % bad if bad else 'none'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
