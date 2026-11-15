#!/usr/bin/env python3
"""S102 lane G2c offline check (0 game runs, 0 Surface runs, no window): the pick rule of wwe2k14_display_720p_mode=auto:N (docs\\renderer\\OUTPUT_PASS.md "Lane G2c").

  py -I tools\\g2c\\g2c_check.py [--root DIR]

1. unit: tools\\g2c\\pick_test.cpp against the real src\\display_pick.h (planted mode lists: the Surface's, no square match, only higher modes, several refresh rates, a 16:9 panel;
   the auto:N parser). Seven planted twin headers must each FAIL it.
2. wiring (regex on the real src\\display_mode.h, planted twins that must be caught): the pick is used once, behind the auto:N parse; the chosen mode is looked up in the display's
   own list; DM_DISPLAYFIXEDOUTPUT = DMDFO_STRETCH is set only for auto; and NOTHING in the Session 95 restore paths changed (RestoreQuiet, RestoreForGood, ApplyMode, Restore, OnFocus
   are compared with src\\display_mode.h of the base branch s102-arc-pt2 by `git show`).
Exit 0 only when everything holds."""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
BASE = 's102-arc-pt2'
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def read(root, rel):
    with open(os.path.join(root, rel), newline='') as f:
        return f.read().replace(chr(13) + chr(10), chr(10))


def unit(root, work, tag, header_text):
    d = os.path.join(work, tag)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'display_pick.h'), 'w', newline='') as f:
        f.write(header_text)
    exe = os.path.join(d, 't.exe')
    c = subprocess.run([CLANG, '-std=c++20', '-I', d, os.path.join(root, 'tools/g2c/pick_test.cpp'), '-o', exe], capture_output=True, text=True)
    if c.returncode:
        print(c.stdout, c.stderr)
        return None
    return subprocess.run([exe], capture_output=True, text=True).returncode == 0


def plant(text, old, new):
    out = text.replace(old, new)
    assert out != text, 'plant did not apply: ' + old
    return out


def func_text(text, name):
    """The text of `inline <ret> name(...) {` ... up to its closing brace at column 0 of the inline block (2-space indent function)."""
    m = re.search(r'\n(inline [^\n]*\b%s\([^\n]*\{.*?\n\})\n' % re.escape(name), text, re.S)
    return m.group(1) if m else None


def wiring_facts(dm, base):
    facts = []
    facts.append(('the pick is used once', len(re.findall(r'wwe2k14::display_pick::Pick\(', dm)) == 1))
    facts.append(('the pick runs only behind the auto:N parse', re.search(r'if \(auto_h\) \{  // S102 G2c: the pick rule[^\n]*\n.*?display_pick::Pick\(', dm, re.S) is not None))
    facts.append(('the chosen WxH is looked up in the display list (an entry the display itself listed)', re.search(r'want_w = int\(chosen\.w\), want_h = int\(chosen\.h\);', dm) is not None))
    facts.append(('stretch is set only for auto', re.search(r'if \(auto_h\) \{[^\n]*\n\s*g_mode\.dmFields \|= DM_DISPLAYFIXEDOUTPUT;\s*g_mode\.dmDisplayFixedOutput = DMDFO_STRETCH;', dm) is not None
                  and dm.count('DMDFO_STRETCH') == 1))
    facts.append(('a named WxH mode is unchanged (the "already that size" early return keeps its test for non-auto)', '!auto_h && int(cur.dmPelsWidth) <= want_w' in dm))
    facts.append(('the cvar help names auto:N', 'auto:N' in dm))
    for fn in ('RestoreQuiet', 'RestoreForGood', 'ApplyMode', 'Restore', 'OnFocus'):
        a, b = func_text(dm, fn), func_text(base, fn)
        facts.append(('Session 95 restore path untouched: ' + fn, a is not None and a == b))
    return facts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    root = os.path.abspath(ap.parse_args().root)
    work = os.path.join(root, 'out', 'g2c_check')
    header = read(root, 'src/display_pick.h')

    line(unit(root, work, 'real', header) is True, 'unit: the real pick header passes pick_test.cpp')
    plants = [
        ('aspect ignored (the 16:9 mode is taken for the 3:2 panel)', 'if (uint64_t(m.w) * panel_h != uint64_t(m.h) * panel_w) continue;', ''),
        ('refresh ignored', 'if (!m.w || !m.h || m.hz != current_hz) continue;', 'if (!m.w || !m.h) continue;'),
        ('the LARGEST height taken, not the smallest', 'if (!found || m.h < out.h) {', 'if (!found || m.h > out.h) {'),
        ('heights below the asked one allowed', 'if (m.h < asked_h) continue;', ''),
        ('the panel\'s own mode allowed (no gain)', 'if (m.h >= panel_h) continue;', ''),
        ('the parser accepts a trailing junk value', 'return text[i] == 0 ? uint32_t(v) : 0;', 'return uint32_t(v);'),
        ('the parser accepts a named mode as auto', "if (!text || text[0] != 'a' || text[1] != 'u' || text[2] != 't' || text[3] != 'o' || text[4] != ':') return 0;", 'if (!text) return 0;'),
    ]
    for i, (name, old, new) in enumerate(plants):
        line(unit(root, work, 'plant%d' % i, plant(header, old, new)) is False, 'unit planted (%s): caught' % name)

    dm = read(root, 'src/display_mode.h')
    g = subprocess.run(['git', 'show', '%s:src/display_mode.h' % BASE], capture_output=True, text=True, cwd=root)
    line(g.returncode == 0, 'the base display_mode.h is readable (git show %s)' % BASE)
    base = g.stdout.replace(chr(13) + chr(10), chr(10))
    for name, ok in wiring_facts(dm, base):
        line(ok, 'wiring: ' + name)
    twins = [
        ('stretch set for every change', plant(dm, 'if (auto_h) {  // S102 G2c: a lower mode', 'if (true) {  // S102 G2c: a lower mode')),
        ('the pick used twice', dm + '\n// wwe2k14::display_pick::Pick(\n'),
        ('the restore path edited (a focus-lost restore dropped)', plant(dm, '    Restore("focus lost");', '    (void)0;')),
        ('the restore call edited (flags changed)', plant(dm, 'ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);', 'ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, CDS_UPDATEREGISTRY, nullptr);')),
        ('the named-mode early return dropped its test', plant(dm, '!auto_h && int(cur.dmPelsWidth) <= want_w', 'int(cur.dmPelsWidth) <= want_w')),
    ]
    for name, tmp in twins:
        bad = [n for n, ok in wiring_facts(tmp, base) if not ok]
        line(bool(bad), 'wiring planted (%s): caught by "%s"' % (name, bad[0] if bad else 'nothing'))
    print('g2c_check:', 'FAILED %d' % fails if fails else 'ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
