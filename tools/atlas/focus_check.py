"""Session 91 step 6's check: the focused-widget cursor (src/focus_widget.h + tools/atlas/nav.py where) on screens the
atlas has no cursor for, including deep ones.

  py tools/atlas/focus_check.py [--name fchk] [--data userdata_unlocked_s91]

One game (fast-forwarded): for each screen, go there (navigator), read where(), press DOWN then RIGHT, read where()
again; PASS for a screen = a focused widget is reported and its row / column moves with the presses (or the widget
says it keeps no cursor, an answer too). Also logs every widget seen (the census focus_widget.md asked for).
"""
import argparse
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import nav  # noqa: E402

# (target, presses to go deeper after arriving) - deep = a screen below the atlas's own nodes
# Run 5906 (Jarrod's extra run): the deep screens first, the MATCH RULES page last (its RIGHT changes a value and B
# then stays on the page: what the game shows after that B is screenshotted at the end).
SCREENS = [('universe', ['A']), ('30 years', ['A']), ('create a superstar', []), ('mode:moves', []),
           ('create an arena', []), ('entrance', []), ('visual presentation', []), ('one on one > normal', []),
           ('match rules > one on one > ladder', [])]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', default='fchk')
    ap.add_argument('--data', default='userdata_unlocked_s91')
    a = ap.parse_args()
    print(nav.live.start(a.name, fast=True, data=a.data), flush=True)
    n = nav.Nav(a.name)
    n.batch(['wait_state menu 1 3600 30'], timeout=600)
    passed = 0
    try:
        for target, deeper in SCREENS:
            try:
                n.go(target)
                if deeper:
                    n.batch(['press %s 5 12' % b for b in deeper] + ['wait_frames 240'], timeout=300)
                before = n.where()
                n.batch(['press DOWN 4 8', 'wait_frames 30'])
                mid = n.where()
                n.batch(['press RIGHT 4 8', 'wait_frames 30'])
                after = n.where()
                moved = (before.get('widget') or {}).get('row') != (mid.get('widget') or {}).get('row') or \
                    (mid.get('widget') or {}).get('col') != (after.get('widget') or {}).get('col')
                ok = bool(before.get('widget')) and (moved or 'grid' not in before)
                passed += ok
                print('%s %-36s %s%s' % ('PASS' if ok else 'FAIL', target + (' +' + '+'.join(deeper) if deeper else ''),
                                        nav.Nav.describe(before), ' -> ' + nav.Nav.describe_widget(after)), flush=True)
                for w in n.focus():
                    print('      widget vt=%s row=%d/%d col=%d/%d pads=%X' % (w['vt'], w['row'], w['rows'], w['col'],
                                                                         w['cols'], w['pads']))
            except nav.Lost as x:
                print('LOST %-36s %s' % (target, x), flush=True)
        print('%d / %d PASS' % (passed, len(SCREENS)))
        # the question left from runs 5904 / 5905: what B shows on a MATCH RULES page after a value changed
        st, text, _ = n.batch(['press B 5 12', 'wait_frames 90', 'log_state menu', 'log_state msgbox',
                               'log_state screen_mode', 'log_text', 'screenshot rules_after_b'], timeout=120)
        print('after B on the changed rules page: states %s; text: %s' % (st, text))
    finally:
        try:
            nav.live.request(a.name, 'quit', connect_wait=2.0)
        except nav.live.NoGame:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
