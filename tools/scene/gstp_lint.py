#!/usr/bin/env python3
r"""S103 lane GSTP lints (0 game runs, no build):
  G1  every FrameBuilder draw in src\renderer_scene\** names its group: `fb.Draw(op, group)` (an owner that does not know its group says rdr::kGrpOther on purpose). A one-argument
      `fb.Draw(op)` there would inherit the previous draw's group (SetGroup is sticky) and the colour-pass split would book its time to the wrong group.
  G2  every WaitHandled( in the host rig's sources has its result used (if / ! / return / assignment): a discarded wait is how a hung host went unnoticed.
  G3  the host rig never calls LogGpuSummary (the renderer's group tables are unlocked while the host's render thread is alive: they are read after it stops).
  py -I tools\scene\gstp_lint.py [--root DIR] [--selftest]    exit 0 = clean; --selftest plants one fault per rule and requires each to be reported."""
import argparse, os, re, sys

DRAW1 = re.compile(r'(?<![:\w])(?:fb|b|builder|builder->|frame|fbld)(?:\.|->)Draw\(\s*[A-Za-z_]\w*\s*\)')
WAIT = re.compile(r'WaitHandled\(')


def strip_comment(line):
    i = line.find('//')
    return line if i < 0 else line[:i]


def lint_draw(text, name):
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        code = strip_comment(line)
        if DRAW1.search(code):
            out.append('%s:%d: G1 a one-argument Draw(): name its group (fb.Draw(op, rdr::kGrp...)): %s' % (name, n, code.strip()[:100]))
    return out


def lint_rig(text, name):
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        code = strip_comment(line).strip()
        if WAIT.search(code):
            before = code[:code.index('WaitHandled(')]
            used = any(t in before for t in ('if (', '!', 'return', '= ', '&&', '||', '? '))
            if not used:
                out.append('%s:%d: G2 the result of WaitHandled is discarded (check it and fail naming the tick): %s' % (name, n, code[:100]))
        if 'LogGpuSummary' in code:
            out.append('%s:%d: G3 LogGpuSummary in the rig (a data race with the host render thread): %s' % (name, n, code[:100]))
    return out


def scan(root):
    out = []
    base = os.path.join(root, 'src', 'renderer_scene')
    for dp, _, fs in os.walk(base):
        for f in fs:
            if f.endswith(('.cpp', '.h', '.inc')):
                p = os.path.join(dp, f)
                out += lint_draw(open(p, encoding='utf-8', errors='replace').read(), os.path.relpath(p, root))
    for f in ('host_rig.cpp',):
        p = os.path.join(root, 'tools', 'scene', f)
        if os.path.isfile(p):
            out += lint_rig(open(p, encoding='utf-8', errors='replace').read(), os.path.relpath(p, root))
    return out


def selftest():
    bad = 0
    for what, got in (('G1', lint_draw('    fb.Draw(op);\n', 'x')), ('G1 (builder->)', lint_draw('    builder->Draw(draw);\n', 'x')), ('G2', lint_rig('  host.WaitHandled(posted, std::chrono::milliseconds(1));\n', 'x')),
                      ('G3', lint_rig('  host.renderer()->LogGpuSummary();\n', 'x'))):
        ok = len(got) == 1
        print('%s selftest %s: %s' % ('PASS' if ok else 'FAIL', what, got[0] if got else 'NOT REPORTED'))
        bad += 0 if ok else 1
    for what, got in (('clean G1', lint_draw('    fb.Draw(op, rdr::kGrpRing);\n    fdump::Draw(fdf, "emit", 1);\n    void Pass2d::Draw(const X& x) {}\n', 'x')), ('clean G2', lint_rig('    if (!host.WaitHandled(posted, t)) return 3;\n    const bool w = host.WaitHandled(n, t);\n', 'x'))):
        ok = not got
        print('%s selftest %s: %s' % ('PASS' if ok else 'FAIL', what, got or 'no report'))
        bad += 0 if ok else 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    bad = selftest() if a.selftest else 0
    found = scan(a.root)
    for f in found:
        print('FAIL ' + f)
    print('gstp_lint: %d problem(s)%s' % (len(found) + bad, ' (selftest included)' if a.selftest else ''))
    return 1 if found or bad else 0


if __name__ == '__main__':
    sys.exit(main())
