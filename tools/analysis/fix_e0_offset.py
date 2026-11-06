"""Session 90: replace every hard-coded 0xE0000000+ host offset (`(a >= 0xE0000000u ? 0x1000u : 0u)`) in our headers
with the SDK's own rex::memory::detail::PhysicalHostOffset(a) - the offset is 0x1000 on Windows / Apple Silicon only
and 0 on Linux / Android (the TRUE NATIVE phone stall: docs\\native\\d3d\\S90_PHONE_STALL.md). Also --check: exit 1 if
any hard-coded form is left (the build guard).

  py -I tools\\analysis\\fix_e0_offset.py [--check] src
"""
import pathlib
import re
import sys

PAT = re.compile(r'\(\s*\(?\s*([A-Za-z_][\w.]*)\s*>=\s*0xE0000000u\s*\)?\s*\?\s*0x1000u\s*:\s*0u\s*\)')
INCLUDE = '#include <rex/system/xmemory.h>'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    root = pathlib.Path(args[0] if args else 'src')
    left = 0
    changed = 0
    for path in sorted(list(root.rglob('*.h')) + list(root.rglob('*.cpp'))):
        text = path.read_text(encoding='utf-8')
        hits = PAT.findall(text)
        if not hits:
            continue
        if check:
            left += len(hits)
            print('%s: %d hard-coded 0xE0000000 offset(s)' % (path, len(hits)))
            continue
        new = PAT.sub(lambda m: '::rex::memory::detail::PhysicalHostOffset(%s)' % m.group(1), text)
        if INCLUDE not in new:
            # after the first #include line (or #pragma once)
            m = re.search(r'^#include [<"].*$', new, re.M) or re.search(r'^#pragma once.*$', new, re.M)
            pos = m.end() if m else 0
            new = new[:pos] + '\n' + INCLUDE + '  // PhysicalHostOffset (Session 90: never a literal 0x1000)' + new[pos:]
        path.write_text(new, encoding='utf-8')
        changed += len(hits)
        print('%s: %d replaced' % (path, len(hits)))
    if check:
        print('left: %d' % left)
        return 1 if left else 0
    print('replaced: %d' % changed)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
