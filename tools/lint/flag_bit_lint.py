#!/usr/bin/env python3
r"""S111 FLAGLINT: two members of one flag set never share a bit value (at the PROPORDER merge kObjGameUndrawn (PIECEHOLES) and kObjNodeAlpha (PROPORDER) both took 131072 in ObjectFlags;
git merges two lines that add a bit on different lines without a conflict).

  py -I tools\lint\flag_bit_lint.py [--root DIR] [--selftest]

Finds every flag group in src\ by pattern (no list to edit; a new enum is covered at once):
  1. `enum [class] [Name] [: type] { kA = 1, kB = 2, ... }`: one group per enum body;
  2. `constexpr [uint32_t ...] kXyzAbc = N;` declarations: one group per (file, prefix), the prefix being `k` + the first CamelCase word (kFaultNoRows, kFaultDrawUnposed -> kFault).
A member counts when its value is a power of two (decimal, hex, or `1u << N`); other values (0, masks like 65535, 3) are not bits and are skipped. Within a group a bit value used by two
members is a finding (file:line of both). Comments are blanked first. Every run also feeds planted twins (synthetic texts) and requires each to fire.
Exit 0 = pass, 1 = a finding (or a twin that did not fire), 2 = src missing. 0 game runs."""
import argparse
import os
import re
import sys

ENUM = re.compile(r'\benum\b(?:\s+(?:class|struct))?[^;{}()=]*\{(?P<body>[^{}]*)\}')
# One constexpr statement, with every declarator of it (`constexpr uint32_t kA = 1, kB = 2;`: S111, the multi-declarator form was missed).
CONSTEXPR = re.compile(r'\bconstexpr\s+(?:[\w:]+\s+)+?(?P<decls>k[A-Z]\w*\s*=[^;{}]+);')
DECL = re.compile(r'\s*(?P<name>k[A-Z]\w*)\s*=\s*(?P<val>[^,]+?)\s*$', re.S)
PREFIX = re.compile(r'^k[A-Z][a-z0-9]*')
# Prefixes that name a flag / planted-fault set by their own word, whatever values they hold (S111 OWNERLESS review: kFaultDrtrNewest /
# kFaultDrtrNoDoubt took PERIODALL's kFaultLinksLead / kFaultNameLeads bits; that kFault set holds no 1, 2, 4, so it was never checked).
FLAG_PREFIXES = {'kFault', 'kPlant', 'kFlag', 'kFlags', 'kBit', 'kBits', 'kObj'}
SHIFT = re.compile(r'^1[uUlL]*\s*<<\s*(\d+)$')
NUM = re.compile(r'^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$')


def blank_comments(t):
    """The text with // and /* */ comments replaced by spaces (newlines kept, so line numbers hold)."""
    def blank(m):
        return re.sub(r'[^\n]', ' ', m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/', blank, t, flags=re.S)


def value(expr):
    """The int of a literal / 1<<N expression, else None."""
    e = expr.strip()
    m = SHIFT.match(e)
    if m:
        return 1 << int(m.group(1))
    m = NUM.match(e)
    if m:
        s = m.group(1)
        return int(s, 16) if s[:2] in ('0x', '0X') else int(s)
    return None


def is_bit(v):
    return v is not None and v > 0 and v & (v - 1) == 0


def groups(path, text):
    """[(group label, [(line, name, value)])] of one file."""
    t = blank_comments(text)
    out = []
    for m in ENUM.finditer(t):
        base = m.start('body')
        mem, pos = [], 0
        for part in m.group('body').split(','):
            e = re.match(r'\s*(k?\w+)\s*=\s*(.+?)\s*$', part, re.S)
            if e and is_bit(value(e.group(2))):
                line = t.count('\n', 0, base + pos + part.find(e.group(1))) + 1
                mem.append((line, e.group(1), value(e.group(2))))
            pos += len(part) + 1
        out.append(('enum at %s:%d' % (path, t.count('\n', 0, m.start()) + 1), mem))
    by_prefix = {}
    for m in CONSTEXPR.finditer(t):
        line = t.count('\n', 0, m.start('decls')) + 1
        for part in m.group('decls').split(','):
            d = DECL.match(part)
            if not d:
                continue
            v = value(d.group('val'))
            p = PREFIX.match(d.group('name'))
            if p and is_bit(v):
                by_prefix.setdefault(p.group(0), []).append((line, d.group('name'), v))
    for p, mem in by_prefix.items():
        if p not in FLAG_PREFIXES and not {1, 2, 4} <= {v for _l, _n, v in mem}:
            continue   # counts / sizes / layout numbers (kMaxX = 8, kXVersion = 1): a flag set holds the run 1, 2, 4 or names itself (FLAG_PREFIXES)
        out.append(('%s* constants in %s' % (p, path), mem))
    return out


# A deliberate alias (a mask member naming the bits that do something, a row's own bit reusing bit 0 under another meaning) is marked
# where it is written: a `flag-alias: kName[, kName2]` comment on the member's own line. An alias cannot be told from a clash by
# pattern, so the source says which; no hand list here (S111 FLAGLINT, main session: the marker replaced the lint's ALIASES set).
ALIAS_MARK = re.compile(r'flag-alias:\s*(k\w+(?:\s*,\s*k\w+)*)')


def marked_aliases(raw_line):
    m = ALIAS_MARK.search(raw_line)
    return {s.strip() for s in m.group(1).split(',')} if m else set()


def judge(files):
    """(findings, group count) for {path: text}."""
    bad, n = [], 0
    for path in sorted(files):
        raw = files[path].splitlines()
        for label, mem in groups(path, files[path]):
            if len(mem) < 2:
                continue
            n += 1
            seen = {}
            for line, name, v in mem:
                if 0 < line <= len(raw) and name in marked_aliases(raw[line - 1]):
                    continue
                if v in seen:
                    bad.append('%s:%d: %s = %d is the value of %s (line %d) too, in the %s: two flags share one bit' % (path, line, name, v, seen[v][1], seen[v][0], label))
                else:
                    seen[v] = (line, name)
    return bad, n


GOOD = {'a.h': 'enum F : uint32_t {\n  kFa = 1,  // x = 2\n  kFb = 2, kFc = 0x4, kFd = 1u << 3,\n  kFall = 15, kFnone = 0,\n};\n'
               'inline constexpr uint32_t kFaultOne = 1;\ninline constexpr uint32_t kFaultTwo = 2;\ninline constexpr uint32_t kFaultFour = 4;\nstatic constexpr uint32_t kOtherX = 1;\n'
               'enum G : uint32_t { kGa = 1, kGb = 2, kGall = 1 };  // flag-alias: kGall\n'}
TWINS = [
    ('two kFault twins of one bit with no 1, 2, 4 in the set (kFaultDrtrNewest / kFaultLinksLead 536870912)',
     {'b.h': 'struct W {\n  static constexpr uint32_t kFaultLinksLead = 536870912;\n  static constexpr uint32_t kFaultDrtrNewest = 536870912;\n};\n'}),
    ('a twin inside a multi-declarator constexpr line',
     {'b.h': 'struct W {\n  static constexpr uint32_t kFaultHoldAny = 32768, kFaultHoldNoSlotCheck = 65536, kFaultHoldNoExtras = 32768;\n};\n'}),
    ('an alias without its flag-alias marker', {'a.h': GOOD['a.h'].replace('  // flag-alias: kGall', '')}),
    ('two enum members of one value (kObjGameUndrawn / kObjNodeAlpha 131072)', {'a.h': GOOD['a.h'].replace('kFc = 0x4', 'kFc = 2')}),
    ('a hex twin of a decimal bit', {'a.h': GOOD['a.h'].replace('kFc = 0x4', 'kFc = 0x1')}),
    ('a shift twin of a decimal bit', {'a.h': GOOD['a.h'].replace('1u << 3', '1 << 1')}),
    ('two constexpr flags of one prefix', {'a.h': GOOD['a.h'] + 'static constexpr uint32_t kFaultThree = 2;\n'}),
    ('twins on one line', {'a.h': GOOD['a.h'].replace('kFb = 2,', 'kFb = 2, kFb2 = 2,')}),
]


def selftest():
    out = []
    if judge(GOOD)[0] or judge(GOOD)[1] < 2:
        out.append('selftest: the good synthetic file fails or finds fewer than 2 groups: %s' % (judge(GOOD),))
    for name, files in TWINS:
        if not judge(files)[0]:
            out.append('selftest: the planted twin "%s" did not fire' % name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true', help='only the planted twins')
    a = ap.parse_args()
    st = selftest()
    for s in st:
        print('FAIL ' + s)
    if a.selftest:
        print('flag_bit_lint selftest: %s' % ('FAIL' if st else 'every twin fires'))
        return 1 if st else 0
    src = os.path.join(a.root, 'src')
    if not os.path.isdir(src):
        print('flag_bit_lint: no src folder under %s' % a.root)
        return 2
    files = {}
    for d, _dirs, names in os.walk(src):
        for nm in names:
            if nm.endswith(('.h', '.hpp', '.cpp', '.cc', '.inc')):
                p = os.path.join(d, nm)
                with open(p, encoding='utf-8', errors='replace') as f:
                    files[os.path.relpath(p, a.root).replace('\\', '/')] = f.read()
    bad, n = judge(files)
    for b in bad:
        print('FAIL ' + b)
    print('flag_bit_lint: %s (%d files, %d flag groups; %d twins fire)' % ('FAIL (%d)' % (len(bad) + len(st)) if bad or st else 'pass', len(files), n, len(TWINS) - len(st)))
    return 1 if bad or st else 0


if __name__ == '__main__':
    sys.exit(main())
