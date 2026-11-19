#!/usr/bin/env python3
"""off_arming_lint (S103 PN4): the stream-free build arms its own picture path (the 2D record, the picture source, the movie planes) WITHOUT a switch, and that arming must exist
ONLY in a build without the stream. The three taps' Active() read a dev-only cvar in the ON build; in the OFF build they return 1. The OFF arming is the marker line
`const int v = 1;` and it must sit in the `#if !WWE2K14_STREAM()` branch (never in an #else, never under `#if WWE2K14_STREAM()`, never unconditional): a twin that arms it in ON would
change what every player's stream build does.

  py -I tools\\scene\\off_arming_lint.py --root .        scan the three taps (exit 1 on a violation)
  py -I tools\\scene\\off_arming_lint.py --selftest      planted twins: each bad snippet MUST be flagged, the good one must pass

Also checks that each tap still has exactly one OFF arming (a removed arming would silently leave the OFF host without its pictures / rows / planes) and that its ON line still reads the dev cvar
behind IsDevBuild() (the default of the ON build / the player is unchanged).
"""
import argparse

import os
import re
import sys

TAPS = ['src/scene_live/picture_tap.h', 'src/scene_live/quads2d_tap.h', 'src/scene_live/movie_planes_tap.h']
MARK = re.compile(r'\bconst\s+int\s+v\s*=\s*1\s*;')
ON_LINE = re.compile(r'IsDevBuild\s*\(\s*\)\s*&&\s*REXCVAR_GET')


# S103 PN4F: the ON build's line is pinned too (the #else text of the `#if !WWE2K14_STREAM()` conditional, comments dropped, whitespace squeezed): a change to what the stream build / the player arms must be a deliberate edit
# of this table (and of DECISIONS.md), never a side effect. sha256, stored as the text itself.
ELSE_PINS = {
    'src/scene_live/picture_tap.h': 'const int v = dev_on ? 1 : 0;',
    'src/scene_live/quads2d_tap.h': 'const int v = (wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_rdr_2d_record)) ? 1 : 0;',
    'src/scene_live/movie_planes_tap.h': 'const int v = (wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_rdr_movie_planes)) ? 1 : 0;',
}
HASH_STORE = re.compile(r'\bg_stream_hash_tap\s*\.\s*store\s*\(')
V_ASSIGN = re.compile(r'\bv\s*=(?!=)')
STATE_LITERAL = re.compile(r'\bg_state\s*\.\s*(?:store|exchange)\s*\(\s*[1-9]')


def _code(line):
    return re.sub(r'\s+', ' ', line.split('//')[0]).strip()


def regions(text):
    """[(line_no, code, region)] with region 'off' (true branch of `#if !WWE2K14_STREAM()`), 'on' (its #else) or 'other'."""
    out, stack = [], []
    for no, line in enumerate(text.splitlines(), 1):
        st = line.strip()
        if st.startswith('#'):
            d = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)', st)
            if d:
                kw, rest = d.group(1), d.group(2).split('//')[0].strip()
                if kw in ('if', 'ifdef', 'ifndef'):
                    stack.append([kw + ' ' + rest, False])
                elif kw == 'elif' and stack:
                    stack[-1][0], stack[-1][1] = 'elif ' + rest, True
                elif kw == 'else' and stack:
                    stack[-1][1] = True
                elif kw == 'endif' and stack:
                    stack.pop()
            continue
        reg = 'other'
        for cond, in_else in stack:
            if re.fullmatch(r'if\s+!\s*WWE2K14_STREAM\s*\(\s*\)', cond):
                reg = 'on' if in_else else 'off'
        out.append((no, _code(line), reg))
    return out


def else_text(text):
    return ' '.join(c for _, c, r in regions(text) if r == 'on' and c)


def pin_of(text):
    return else_text(text)


def check_extra(text, name, pin=None):
    """PN4F: the pinned ON text, no other arming outside the OFF branch, the hash-parity store only behind dev_on."""
    probs = []
    if pin is not None and pin_of(text) != pin:
        probs.append(f'{name}: the ON build text (`#else` of `#if !WWE2K14_STREAM()`: `{else_text(text)[:90]}`) differs from its pinned text: the stream build / player arming changed')
    in_active = False
    for no, code, reg in regions(text):
        if re.search(r'\bbool\s+Active\s*\(\s*\)', code):
            in_active = True   # (only Active() arms a tap: `v` elsewhere in the file is somebody else's variable)
        elif code == '}' and in_active:
            in_active = False
        if reg == 'off' or not code or not in_active:
            continue
        if V_ASSIGN.search(code) and 'REXCVAR_GET' not in code and 'dev_on' not in code:
            probs.append(f'{name}:{no}: `{code[:70]}` arms outside `#if !WWE2K14_STREAM()` without a cvar (non-cvar `v =`)')
        if STATE_LITERAL.search(code):
            probs.append(f'{name}:{no}: `{code[:70]}` stores a literal arming outside the OFF branch')
    for no, code, reg in regions(text):
        if HASH_STORE.search(code) and not re.search(r'\bif\s*\(\s*dev_on\s*\)', code):
            probs.append(f'{name}:{no}: the stream-hash parity tap is stored without `if (dev_on)` (the OFF arming must not register it)')
    if name.endswith('picture_tap.h'):
        if not any(re.search(r'\bif\s*\(\s*dev_on\s*\).*g_stream_hash_tap\s*\.\s*store', c) for _, c, _r in regions(text)):
            probs.append(f'{name}: the `if (dev_on)` store of the hash parity tap is gone')
        if not any(re.search(r'\bdev_on\s*=\s*wwe2k14::IsDevBuild\s*\(\s*\)\s*&&\s*REXCVAR_GET', c) for _, c, _r in regions(text)):
            probs.append(f'{name}: `dev_on` is no longer IsDevBuild() && the cvar')
    return probs


def check(text, name):
    """Returns a list of problems."""
    probs = []
    stack = []   # (condition text, in_else)
    marks = 0
    on_ok = False
    for no, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if s.startswith('#'):
            d = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)', s)
            if d:
                kw, rest = d.group(1), d.group(2).split('//')[0].strip()
                if kw in ('if', 'ifdef', 'ifndef'):
                    stack.append([kw + ' ' + rest, False])
                elif kw == 'elif' and stack:
                    stack[-1][0], stack[-1][1] = 'elif ' + rest, True
                elif kw == 'else' and stack:
                    stack[-1][1] = True
                elif kw == 'endif' and stack:
                    stack.pop()
            continue
        if ON_LINE.search(line):
            on_ok = True
        if MARK.search(line):
            marks += 1
            ok = bool(stack) and not stack[-1][1] and re.fullmatch(r'if\s+!\s*WWE2K14_STREAM\s*\(\s*\)', stack[-1][0]) is not None
            if not ok:
                where = stack[-1][0] + (' (else branch)' if stack and stack[-1][1] else '') if stack else 'no #if at all'
                probs.append(f'{name}:{no}: the OFF arming `const int v = 1;` is not inside `#if !WWE2K14_STREAM()` ({where})')
    if marks != 1:
        probs.append(f'{name}: {marks} OFF armings (`const int v = 1;`), expected exactly 1')
    if not on_ok:
        probs.append(f'{name}: the ON line no longer reads the dev cvar behind IsDevBuild()')
    return probs


GOOD = """
inline bool Active() {
#if !WWE2K14_STREAM()
  const int v = 1;
#else
  const int v = (wwe2k14::IsDevBuild() && REXCVAR_GET(x)) ? 1 : 0;
#endif
  return v != 0;
}
"""


def selftest():
    twins = {
        'unconditional': GOOD.replace('#if !WWE2K14_STREAM()\n  const int v = 1;\n#else\n', '  const int v = 1;\n#if 1\n'),
        'stream test as the condition': GOOD.replace('#if !WWE2K14_STREAM()', '#if WWE2K14_STREAM()'),
        'in the else branch': "inline bool Active() {\n#if !WWE2K14_STREAM()\n  const int v = (wwe2k14::IsDevBuild() && REXCVAR_GET(x)) ? 1 : 0;\n#else\n  const int v = 1;\n#endif\n  return v != 0;\n}\n",
        'under the stream test': GOOD.replace('#if !WWE2K14_STREAM()', '#if WWE2K14_STREAM()').replace('#else', '#endif\n#if 1'),
        'armed twice': GOOD.replace('#else', '  const int v = 1;\n#else'),
        'on line lost the dev test': GOOD.replace('wwe2k14::IsDevBuild() && ', ''),
        'no OFF arming': GOOD.replace('  const int v = 1;\n', ''),
    }
    bad = 0
    pin = pin_of(GOOD)
    good_pic = GOOD.replace('  const int v = 1;', '  const bool dev_on = wwe2k14::IsDevBuild() && REXCVAR_GET(x);\n  const int v = 1;', 1).replace('  return v != 0;', '  if (dev_on) g_stream_hash_tap.store(&F);\n  return v != 0;')
    extra = {
        'ON text changed': (GOOD.replace('? 1 : 0', '? 1 : 1'), 'differs from its pinned text'),
        'ON text dropped the dev test': (GOOD.replace('wwe2k14::IsDevBuild() && ', ''), 'differs from its pinned text'),
        'stray non-cvar arming': (GOOD.replace('  return v != 0;', '  v = 1;\n  return v != 0;'), 'non-cvar `v =`'),
        'stray literal store': (GOOD.replace('  return v != 0;', '  g_state.store(1);\n  return v != 0;'), 'literal arming'),
        'hash parity store unguarded': (good_pic.replace('if (dev_on) g_stream_hash_tap', 'if (v) g_stream_hash_tap'), 'without `if (dev_on)`'),
        'hash parity store lost': (good_pic.replace('  if (dev_on) g_stream_hash_tap.store(&F);\n', ''), 'gone'),
    }
    for k, (t, want) in extra.items():
        p = check_extra(t, 'x/picture_tap.h', pin)
        ok = any(want in q for q in p)
        print(('PASS' if ok else 'FAIL') + f'  twin "{k}": ' + ('caught' if ok else 'NOT CAUGHT ' + str(p)))
        bad += 0 if ok else 1
    if check_extra(good_pic, 'x/picture_tap.h', pin_of(good_pic)):
        print('FAIL  the good picture_tap snippet was flagged: ' + '; '.join(check_extra(good_pic, 'x/picture_tap.h', pin_of(good_pic))))
        bad += 1
    else:
        print('PASS  good picture_tap snippet clean (pin, dev_on store)')
    if check(GOOD, 'good'):
        print('FAIL  the good snippet was flagged: ' + '; '.join(check(GOOD, 'good')))
        bad += 1
    else:
        print('PASS  good snippet clean')
    for k, t in twins.items():
        p = check(t, k)
        print(('PASS' if p else 'FAIL') + f'  twin "{k}": ' + ('caught (' + p[0].split(': ', 1)[-1][:60] + ')' if p else 'NOT CAUGHT'))
        bad += 0 if p else 1
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    rc = 0
    for rel in TAPS:
        p = os.path.join(a.root, rel)
        if not os.path.isfile(p):
            print('FAIL  missing ' + rel)
            rc = 1
            continue
        src = open(p, encoding='utf-8', errors='replace').read()
        probs = check(src, rel) + check_extra(src, rel, ELSE_PINS.get(rel))
        for q in probs:
            print('FAIL  ' + q)
        if not probs:
            print('PASS  ' + rel + ': one OFF arming, under #if !WWE2K14_STREAM(), the ON line unchanged')
        rc |= 1 if probs else 0
    return rc


if __name__ == '__main__':
    sys.exit(main())
