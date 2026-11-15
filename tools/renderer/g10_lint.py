r"""S102 lane G10 lints (review F1, F10): two source rules, each with a planted twin.
  py -I tools\renderer\g10_lint.py [--root DIR]
  F1  in Renderer::Impl::CollectGpuTime every line that touches a global container (rdr::g_*samples / g_*rows / g_arm_*) sits within 3 lines of an `if (rdr::g_...)` guard. (The game runs this function
      every frame: an unguarded push_back is a leak that only a long run shows.)
  F10 no `DrawState name;` declaration without an initialiser under src\ (a DrawState has a `variant` byte the pipeline store hashes: it must be built with {} or copied from a state).
Exit 1 on any finding or when a file cannot be read."""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def f1(text):
    i = text.find('void Renderer::Impl::CollectGpuTime')
    if i < 0:
        return ['CollectGpuTime not found']
    j = text.find('\nvoid Renderer::Impl::', i + 10)
    lines = text[i:j].splitlines()
    bad = []
    for k, line in enumerate(lines):
        if re.search(r'rdr::g_\w*(samples|rows|arm_)\w*', line) and 'if (' not in line:
            window = '\n'.join(lines[max(0, k - 3):k + 1])
            if not re.search(r'if \(rdr::g_\w+', window):
                bad.append('unguarded global container use: ' + line.strip()[:110])
    return bad


def f10(text, name):
    return ['%s: DrawState without an initialiser: %s' % (name, m.group(0)) for m in re.finditer(r'\bDrawState\s+\w+\s*;', text)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    a = ap.parse_args()
    fails = []
    p = os.path.join(a.root, 'src', 'renderer', 'renderer.cpp')
    try:
        text = open(p, encoding='utf-8').read()
    except OSError as e:
        print('FAIL cannot read', p, e)
        sys.exit(1)
    bad = f1(text)
    print('%s F1: %d unguarded container uses in CollectGpuTime' % ('FAIL' if bad else 'PASS', len(bad)))
    fails += bad
    # twin: the guard removed must be found
    twin = text.replace('if (rdr::g_arms_on.load(std::memory_order_relaxed)) {', '{', 1)
    planted = twin != text
    caught = planted and bool(f1(twin))
    print('%s F1 twin: guard removed is caught (applied %s)' % ('PASS' if caught else 'FAIL', planted))
    if not caught:
        fails.append('F1 twin')
    n = 0
    for dp, _, fs in os.walk(os.path.join(a.root, 'src')):
        for f in fs:
            if f.endswith(('.h', '.cpp')):
                t = open(os.path.join(dp, f), encoding='utf-8', errors='replace').read()
                n += 1
                fails += f10(t, f)
    print('%s F10: DrawState declarations without {} (%d files read)' % ('FAIL' if [x for x in fails if 'DrawState without' in x] else 'PASS', n))
    caught = bool(f10('DrawState s;', 'twin')) and not f10('DrawState s{};', 'twin') and not f10('DrawState s = x;', 'twin')
    print('%s F10 twin: a bare declaration is caught, {} and copies are not' % ('PASS' if caught else 'FAIL'))
    if not caught:
        fails.append('F10 twin')
    for x in fails:
        print('  ', x)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
