"""S103 RGF lint: every offline tool that draws wrestlers through rf_wrestlers feeds the body scale or says why not.

W2 made an unknown body scale a HOLE (rf_wrestlers.cpp: a Live wrestler with no scale is never drawn unscaled); the rf_b rig drew 0 of 8 for three merges because
nothing told it a scale (S103 RG). A tool under tools\\scene\\*.cpp that includes record_frame.h / rf_wrestlers.h (it can build the wrestlers class) must either
call BodyScaleTable (SetByName / Set / ParseBodyScales: it feeds the scale) or carry one line  `// body-scale: none (<reason>)`  naming why it needs none.

  py -I tools\\scene\\body_scale_feed_lint.py            exit 0 = clean
  py -I tools\\scene\\body_scale_feed_lint.py --self-test  the planted fault (a tool with neither) must be flagged
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DRAWS = re.compile(r'#include\s+"[^"]*(record_frame|rf_wrestlers)\.h"')
FEEDS = re.compile(r'BodyScaleTable|body_scale\.h')
DECL = re.compile(r'//\s*body-scale:\s*none\s*\(([^)]{8,})\)')


def check(text):
    if not DRAWS.search(text):
        return None   # not a wrestler drawer
    if FEEDS.search(text):
        return None
    return None if DECL.search(text) else 'draws through record_frame / rf_wrestlers but neither feeds BodyScaleTable nor declares `// body-scale: none (<reason>)`'


def main():
    if '--self-test' in sys.argv:
        bad = '#include "../../src/renderer_scene/record_frame.h"\nint main(){}\n'
        good_feed = bad + 'rdr_scene::BodyScaleTable::Get().SetByName("a", 1.f);\n'
        good_decl = '// body-scale: none (draws no wrestlers: families masked)\n' + bad
        ok = check(bad) is not None and check(good_feed) is None and check(good_decl) is None and check('int main(){}') is None
        print('PASS' if ok else 'FAIL', 'body_scale_feed_lint self-test (planted tool with neither must be flagged)')
        return 0 if ok else 1
    bad = []
    for p in sorted(HERE.glob('*.cpp')):
        msg = check(p.read_text(encoding='utf-8', errors='replace'))
        if msg:
            bad.append(f'{p.name}: {msg}')
    for b in bad:
        print('FAIL', b)
    print('PASS' if not bad else 'FAIL', f'body_scale_feed_lint: {len(bad)} tool(s) without a body-scale feed or declaration')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
