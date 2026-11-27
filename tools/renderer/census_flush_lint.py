r"""S102 CEN1: every census line printed from a 600-frame / 600-run counter must also honour the census flush (src\renderer_adapter\census_flush.h), so a flip run
(a phase = 60 frames) shows each lever's census at every phase end, not after the first frame only (docs\renderer\IN_RUN_AB.md).

  py -I tools\renderer\census_flush_lint.py [--root DIR]     scans src\**\*.{h,cpp,inc} (not *_test.cpp, fview, rplay); exit 1 on a 600-gate that prints a census without census_flush:: near it
  py -I tools\renderer\census_flush_lint.py --list            prints every 600-gate with its verdict (honours / allowed / MISSING)
  py -I tools\renderer\census_flush_lint.py --selftest        planted twin: a census gate without the flush must be flagged, one with it must not

A "600-gate" is a line with `>= 600`, `% 600 ==` / `% 600 !=` or `+ 600`. It is a census gate when a line within WINDOW lines after it logs a `[rdr][` / `[owner][` line (Log / REXLOG_INFO / log(line)),
or the gate sits inside a function whose header comment says census. It honours the flush when `census_flush::` appears within WINDOW lines around it. NOT_CENSUS lists the gates that are not census lines (a memo sweep, a drop count), each with its reason."""
import os
import re
import sys

WINDOW_AFTER = 14
WINDOW_BEFORE = 4
GATE = re.compile(r'(>=\s*600\b|%\s*600\s*[!=]=|\+\s*600\b)')
LOGS = re.compile(r'\[(rdr|owner|ko)\]\[|\[rdr\]\[|\[ko\]')
SKIP_DIRS = {'fview', 'player'}
# (file name, snippet of the gate line) -> why it is not a census line
NOT_CENSUS = {
    ('crowd_rank.h', '% 600 == 0'): 'a position-map sweep, not a census',
    ('reader.h', '% 600 != 300'): 'a record-frame sampling gate for a probe, not a lever census',
    ('live.cpp', 'ko_dropped % 600'): 'the knock-out test counter',
    ('binder.h', 'frames_unnamed >= 600'): 'a one-time "no draw origins" warning',
    ('reader.h', 'g_record_frame'): 'the memo sweep window',
    ('live.cpp', 'guest_frame > want + 600'): 'S103 stage_page give-up timeout (a one-time warning per pending stage frame, not a lever census; found by LNT2)',
}


def gates(text):
    lines = text.split('\n')
    for i, ln in enumerate(lines):
        code = ln.split('//')[0]
        if GATE.search(code):
            yield i, lines


def classify(name, i, lines):
    ln = lines[i]
    for (f, snip), why in NOT_CENSUS.items():
        if f == name and snip in ln:
            return 'allowed', why
    after = '\n'.join(lines[i:i + WINDOW_AFTER])
    around = '\n'.join(lines[max(0, i - WINDOW_BEFORE):i + WINDOW_AFTER])
    if not LOGS.search(after) and 'Census' not in after and 'census' not in ln.lower():
        return 'quiet', 'no census log near the gate'
    if 'census_flush::' in around:
        return 'honours', ''
    return 'MISSING', ln.strip()[:140]


def scan(root):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path, part_files, line_map  # a split parent is read with its parts expanded (the part files are not scanned alone); a hit names its part file and line
    from pathlib import Path
    parts = part_files(os.path.join(root, 'src'))
    out = []
    for d, dirs, files in os.walk(os.path.join(root, 'src')):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in files:
            if not f.endswith(('.h', '.cpp', '.inc')) or f.endswith('_test.cpp'):
                continue
            p = os.path.join(d, f)
            if Path(p).resolve() in parts:
                continue
            text = read_path(p)
            lmap = None
            for i, lines in gates(text):
                v, why = classify(f, i, lines)
                if lmap is None:
                    lmap = line_map(p)
                pf, pn = lmap[i]
                out.append((v, os.path.relpath(str(pf), root), pn, why))
    return out


def selftest():
    bad = 'void F() {\n  if (++window_ >= 600) {\n    rdr::Log("[rdr][x] frames %d", 1);\n    window_ = 0;\n  }\n}\n'
    good = 'void F() {\n  if (++window_ >= 600 || census_flush::Due(flush_)) {\n    rdr::Log("[rdr][x] frames %d", 1);\n    window_ = 0;\n  }\n}\n'
    ok = True
    for text, want in ((bad, 'MISSING'), (good, 'honours')):
        got = [classify('t.h', i, l)[0] for i, l in gates(text)]
        if got != [want]:
            print('FAIL: selftest wanted %s, got %s' % (want, got))
            ok = False
    if ok:
        print('census_flush_lint selftest: the planted gate without the flush is flagged, the one with it is not')
    return 0 if ok else 1


def main():
    argv = sys.argv[1:]
    if '--selftest' in argv:
        return selftest()
    root = '.'
    if '--root' in argv:
        root = argv[argv.index('--root') + 1]
    rows = scan(root)
    if '--list' in argv:
        for v, f, ln, why in sorted(rows):
            print('%-8s %s:%d %s' % (v, f, ln, why))
    missing = [r for r in rows if r[0] == 'MISSING']
    for v, f, ln, why in missing:
        print('MISSING census_flush:: %s:%d: %s' % (f, ln, why))
    n = {k: sum(1 for r in rows if r[0] == k) for k in ('honours', 'allowed', 'quiet', 'MISSING')}
    print('census_flush_lint: %d gates: honours %d, allowed (not census) %d, quiet %d, MISSING %d' % (len(rows), n['honours'], n['allowed'], n['quiet'], n['MISSING']))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
