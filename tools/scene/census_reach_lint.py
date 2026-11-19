r"""S103 lane LVF lint: the census lines must reach the run log in the stream-free (OFF) build.

  py -I tools\scene\census_reach_lint.py [--root .] [--selftest]

  1. No `fprintf(stderr, "[rdr]...` / `"[scene]...` in src\renderer_scene or src\scene_live: stderr never reaches the run log (REXLOG); use rdr::Log (the renderer's log sink, set in OFF by
     SceneHost::Start and in ON by LiveSink::Start). (Run 006 / 007: `[rdr][weapons] drawn` was lost this way.)
  2. The OFF host reaches the contributors' census: src\renderer_host\scene_host.cpp calls opts_.contributor_census in the 600-frame window block AND in LogFinal, and
     src\renderer_host\host_start.cpp sets that option to rdr_scene::live::CensusLog and sets the writer's log (rdr_scene::live::SetLog), without which CensusLog prints nothing.
     (The option, not a direct call: the host rig links scene_host.cpp but no scene writer.) (Run 006 / 007: 0 [scene][live] lines; its only caller was the stream reader.)
Exit 0 = clean."""
import os
import re
import sys

ROOT = '.'
STDERR_RE = re.compile(r'fprintf\s*\(\s*stderr\s*,\s*"\s*\[(rdr|scene)\]')


def scan_stderr(files):
    bad = []
    for path, text in files:
        for i, ln in enumerate(text.splitlines(), 1):
            if STDERR_RE.search(ln):
                bad.append('%s:%d: fprintf(stderr) of a [rdr]/[scene] line (use rdr::Log)' % (path, i))
    return bad


def check_reach(host_cpp, start_cpp):
    bad = []
    n = len(re.findall(r'opts_\.contributor_census\s*\(', host_cpp))
    if n < 2:
        bad.append('scene_host.cpp: opts_.contributor_census called %d time(s); need the window block and LogFinal (2)' % n)
    if not re.search(r'contributor_census\s*=[^;]*rdr_scene::live::CensusLog\s*\(', start_cpp):
        bad.append('host_start.cpp: o.contributor_census is not set to rdr_scene::live::CensusLog')
    if not re.search(r'rdr_scene::live::SetLog\s*\(', start_cpp):
        bad.append('host_start.cpp: rdr_scene::live::SetLog is not called (CensusLog has no log function in OFF)')
    # the window call must sit inside the 600-frame block
    # (S103 LNT2 added `|| host_flush_due` to the window test: accept one extra `|| name` term)
    m = re.search(r'last_census_\s*>=\s*600(?:\s*\|\|\s*\w+)?\s*\)\s*\{(.*?)\n  \}', host_cpp, re.S)
    if not m or 'contributor_census' not in m.group(1):
        bad.append('scene_host.cpp: contributor_census is not called inside the `c.frames - last_census_ >= 600` block')
    return bad


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def collect(root):
    out = []
    for d in ('src/renderer_scene', 'src/scene_live'):
        for dp, _, fs in os.walk(os.path.join(root, d)):
            for fn in fs:
                if fn.endswith(('.h', '.cpp')):
                    p = os.path.join(dp, fn)
                    out.append((os.path.relpath(p, root).replace('\\', '/'), open(p, encoding='utf-8', errors='replace').read()))
    return out


def selftest():
    bad = 0
    def expect(name, ok):
        nonlocal bad
        print('%s  %s' % ('PASS' if ok else 'FAIL', name))
        bad += 0 if ok else 1
    expect('planted stderr [rdr] line is caught', len(scan_stderr([('x.cpp', '  std::fprintf(stderr, "[rdr][weapons] drawn\\n");')])) == 1)
    expect('planted stderr [scene] line is caught', len(scan_stderr([('x.cpp', 'fprintf(stderr, "[scene][x] y");')])) == 1)
    expect('dev stderr line is not', not scan_stderr([('x.cpp', 'fprintf(stderr, "[mats] x");')]))
    good_host = 'if (c.frames - last_census_ >= 600) {\n    opts_.contributor_census(a, b);\n  }\nvoid LogFinal() { opts_.contributor_census(a, b); }'
    good_start = 'o.contributor_census = [](uint64_t a, uint64_t b) { rdr_scene::live::CensusLog(a, b); };\nrdr_scene::live::SetLog(f);'
    expect('good host passes', not check_reach(good_host, good_start))
    expect('no SetLog is caught', len(check_reach(good_host, good_start.replace('rdr_scene::live::SetLog(f);', ''))) == 1)
    expect('option not set is caught', len(check_reach(good_host, 'rdr_scene::live::SetLog(f);')) == 1)
    expect('one census call is caught', len(check_reach(good_host.replace('void LogFinal() { opts_.contributor_census(a, b); }', ''), good_start)) >= 1)
    return bad


def main():
    global ROOT
    argv = sys.argv[1:]
    if '--root' in argv:
        ROOT = argv[argv.index('--root') + 1]
    if '--selftest' in argv:
        b = selftest()
        print('census_reach_lint selftest: %s' % ('FAIL' if b else 'ALL PASS'))
        return 1 if b else 0
    bad = scan_stderr(collect(ROOT)) + check_reach(read(ROOT, 'src/renderer_host/scene_host.cpp'), read(ROOT, 'src/renderer_host/host_start.cpp'))
    for b in bad:
        print('FAIL  ' + b)
    print('census_reach_lint: %s' % ('%d problem(s)' % len(bad) if bad else 'clean'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
