#!/usr/bin/env python3
r"""S111 FXSKIP review: every `constexpr <type> kXxLast = kName;` in src\ names the LAST kind of kName's enum (the one with the highest value).

  py -I tools\lint\kind_last_lint.py [--root DIR] [--selftest]

Why: the record reader keeps a section's rows of kinds lo..kXxLast and skips (and counts) every other row (src\renderer_scene\record_kinds.h
KeepKnownKinds, called by ParseRecord). A lane that adds a row kind to the enum and forgets to move kXxLast makes OUR reader skip OUR new rows:
no compile error, no unit failure (a round trip of the old kinds still passes), and the moved needle checks (car_rope_check / slot_vis_check /
stage_posed_check: the KeepKnownKinds call text) still pass. This lint is what fails.
Found by pattern, no list to edit: every `constexpr ... kXxLast = kName;` whose kName is an enumerator of some `enum ... { ... }` body in src\.
Values: decimal / hex literals, `1u << N`, another enumerator of the same body, or implicit (previous + 1); a value it cannot read is a finding
(it cannot judge, so it never passes silently). Comments are blanked first. Every run also feeds planted twins (synthetic texts, and the real
tree with a kind added after the last one) and requires each to fire.
Exit 0 = pass, 1 = a finding (or a twin that did not fire), 2 = src missing. 0 game runs."""
import argparse
import os
import re
import sys

ENUM = re.compile(r'\benum\b(?:\s+(?:class|struct))?[^;{}()=]*\{(?P<body>[^{}]*)\}')
LAST = re.compile(r'\bconstexpr\s+(?:[\w:]+\s+)+?(?P<last>k\w*Last)\s*=\s*(?P<name>k\w+)\s*;')
MEMBER = re.compile(r'\s*(?P<name>[A-Za-z_]\w*)\s*(?:=\s*(?P<val>.+?))?\s*$', re.S)
SHIFT = re.compile(r'^1[uUlL]*\s*<<\s*(\d+)$')
NUM = re.compile(r'^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$')


def blank_comments(t):
    """The text with // and /* */ comments replaced by spaces (newlines kept, so line numbers hold)."""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/', blank, t, flags=re.S)


def enums(text):
    """[(line of the enum, {name: value or None})] of one file's text (comments blanked)."""
    t = blank_comments(text)
    out = []
    for m in ENUM.finditer(t):
        vals, prev = {}, -1
        for part in m.group('body').split(','):
            if not part.strip():
                continue
            e = MEMBER.match(part)
            if not e:
                prev = None
                continue
            expr = (e.group('val') or '').strip()
            if not expr:
                v = None if prev is None else prev + 1
            else:
                s, n = SHIFT.match(expr), NUM.match(expr)
                if s:
                    v = 1 << int(s.group(1))
                elif n:
                    v = int(n.group(1), 16) if n.group(1)[:2] in ('0x', '0X') else int(n.group(1))
                else:
                    v = vals.get(expr)
            vals[e.group('name')] = v
            prev = v
        out.append((t.count('\n', 0, m.start()) + 1, vals))
    return out


def judge(files):
    """(findings, number of kXxLast constants judged) for {path: text}."""
    all_enums = [(p, line, vals) for p in sorted(files) for line, vals in enums(files[p])]
    bad, n = [], 0
    for path in sorted(files):
        t = blank_comments(files[path])
        for m in LAST.finditer(t):
            last, name = m.group('last'), m.group('name')
            line = t.count('\n', 0, m.start()) + 1
            homes = [(p, l, v) for p, l, v in all_enums if name in v]
            if not homes:
                continue   # not an enumerator (a plain constant): not this lint's rule
            n += 1
            if len(homes) > 1:
                bad.append('%s:%d: %s = %s: %s is an enumerator of %d enums (%s): cannot tell which one it ends' % (path, line, last, name, name, len(homes),
                                                                                                                 ', '.join('%s:%d' % (p, l) for p, l, _v in homes)))
                continue
            p, l, vals = homes[0]
            unread = [k for k, v in vals.items() if v is None]
            if unread:
                bad.append('%s:%d: %s = %s: the enum at %s:%d has members whose value this lint cannot read (%s): it cannot judge the last kind' % (path, line, last, name, p, l, ', '.join(unread)))
                continue
            top = max(vals.values())
            if vals[name] != top:
                above = [k for k, v in vals.items() if v > vals[name]]
                bad.append('%s:%d: %s = %s (%d), but the enum at %s:%d holds %s above it (max %d): move %s to the last kind, or the record reader skips those rows (record_kinds.h KeepKnownKinds)'
                           % (path, line, last, name, vals[name], p, l, ', '.join(above), top, last))
    return bad, n


GOOD = {'a.h': 'enum FxKind : uint8_t { kFxA = 1, kFxB = 2,   // a comment, kFxZ = 99\n'
               '  kFxC = 3 };\n'
               'inline constexpr uint8_t kFxLast = kFxC;\n'
               'enum LookKind : uint8_t { kSlA = 1,\n  // S1: a note\n  kSlB,\n  kSlC = 0x3 };\n'
               'inline constexpr uint8_t kSlLast = kSlC;\n'
               'enum Ml : uint8_t { kMlSet = 0, kMlMesh = 1 };\ninline constexpr uint8_t kMlLast = kMlMesh;\n'
               'constexpr int kDeltaFirst = -2, kDeltaLast = 3;\n'}
TWINS = [
    ('a kind added after the last one, kFxLast not moved', {'a.h': GOOD['a.h'].replace('kFxC = 3 };', 'kFxC = 3, kFxD = 4 };')}),
    ('a kind added with an implicit value', {'a.h': GOOD['a.h'].replace('kSlC = 0x3 };', 'kSlC = 0x3, kSlD };')}),
    ('a kind added in a multi-line enum between comments', {'a.h': GOOD['a.h'].replace('  // S1: a note\n', '  // S1: a note\n  kSlE = 9,  // new\n')}),
    ('a kLast naming a middle kind', {'a.h': GOOD['a.h'].replace('kMlLast = kMlMesh', 'kMlLast = kMlSet')}),
    ('a member value it cannot read', {'a.h': GOOD['a.h'].replace('kFxB = 2', 'kFxB = SOME_MACRO')}),
]
# The real tree with one kind added after each family's last one: each must fire (the lint reads the real headers, not only the synthetic text).
REAL_PLANTS = [
    ('src/renderer_scene/rows_stage_fx.h', 'kFxCarRope = 9 }', 'kFxCarRope = 9, kFxPlantedLater = 10 }'),
    ('src/renderer_scene/rows_stage_fx.h', 'kMlMesh = 1 }', 'kMlMesh = 1, kMlPlanted = 2 }'),
    ('src/renderer_scene/rows_arena_world.h', 'kAwClock = 2 }', 'kAwClock = 2, kAwPlanted = 3 }'),
    ('src/renderer_scene/rows_crowd.h', 'kCpHand = 2 }', 'kCpHand = 2, kCpPlanted = 3 }'),
    ('src/renderer_scene/rows_crowd.h', 'kCsBody = 1 }', 'kCsBody = 1, kCsPlanted = 2 }'),
    ('src/renderer_scene/rows_stage_look.h', 'kSlPage = 11 }', 'kSlPage = 11, kSlPlanted = 12 }'),
]


def selftest(files=None):
    out = []
    bad, n = judge(GOOD)
    if bad or n != 3:
        out.append('selftest: the good synthetic file fails or judges %d constants, not 3: %s' % (n, bad))
    for name, fs in TWINS:
        if not judge(fs)[0]:
            out.append('selftest: the planted twin "%s" did not fire' % name)
    if files is not None:
        for path, old, new in REAL_PLANTS:
            text = files.get(path)
            if text is None or old not in text:
                out.append('selftest: real-tree plant: %s does not hold "%s" (the enum moved: update REAL_PLANTS)' % (path, old))
                continue
            planted = dict(files)
            planted[path] = text.replace(old, new, 1)
            if not judge(planted)[0]:
                out.append('selftest: real-tree plant "%s" in %s did not fire' % (new, path))
    return out


def read_src(root):
    files = {}
    src = os.path.join(root, 'src')
    for d, _dirs, names in os.walk(src):
        for nm in names:
            if nm.endswith(('.h', '.hpp', '.cpp', '.cc', '.inc')):
                p = os.path.join(d, nm)
                with open(p, encoding='utf-8', errors='replace') as f:
                    files[os.path.relpath(p, root).replace('\\', '/')] = f.read()
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true', help='only the planted twins (synthetic, then the real tree)')
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(a.root, 'src')):
        print('kind_last_lint: no src folder under %s' % a.root)
        return 2
    files = read_src(a.root)
    st = selftest(files)
    for s in st:
        print('FAIL ' + s)
    if a.selftest:
        print('kind_last_lint selftest: %s' % ('FAIL' if st else 'every twin fires (%d synthetic, %d on the real tree)' % (len(TWINS), len(REAL_PLANTS))))
        return 1 if st else 0
    bad, n = judge(files)
    for b in bad:
        print('FAIL ' + b)
    print('kind_last_lint: %s (%d files, %d kXxLast constants judged; %d synthetic + %d real-tree twins, %d did not fire)' % ('FAIL (%d)' % (len(bad) + len(st)) if bad or st else 'pass',
                                                                                                                          len(files), n, len(TWINS), len(REAL_PLANTS), len(st)))
    return 1 if bad or st else 0


if __name__ == '__main__':
    sys.exit(main())
