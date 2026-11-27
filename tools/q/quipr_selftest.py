"""quipr_selftest: `q uipr` (S111 QFIX5). 0 game runs. The branch rule on planted page records (a root page with rec+152 non-zero = saved return, 0 = the fixed
link, a non-root page = none), and, when the player's MnUIHD.pac is readable, the re-find: MnUIHD p2/0 (MatchCreator) root page 4 has rec+152 = 1 -> saved return.
Exit 1 = a check failed."""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import quipr  # noqa: E402

fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:400]))
    if not ok:
        fails.append(what)


def q(*args):
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py')] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace',
                       stdin=subprocess.DEVNULL)
    return r.returncode, r.stdout, r.stderr


def planted():
    base = {'root': 1, 'link_layout': -1, 'link_page': -1, 'link_152': 0, 'link_flag': 0}
    check('planted root page, rec+152 = 1: saved return', quipr.branch_of(dict(base, link_152=1))['kind'] == 'saved_return')
    check('planted root page, rec+152 = 0: the fixed link with its layout / page',
          quipr.branch_of(dict(base, link_layout=3, link_page=7)) == {'kind': 'fixed_link', 'layout': 3, 'leaf': 'p4/0', 'page': 7, 'flag160': 0})
    check('planted non-root page: no branch', quipr.branch_of(dict(base, root=0, link_152=1))['kind'] is None)


def real():
    rc, so, se = q('uipr', 'MnUIHD', 'p2/0', '--root', '--json')
    if rc != 0 and 'no such pac' in se:
        print('skip real MnUIHD p2/0: no pac (%s)' % se.strip()[:80])
        return
    doc = json.loads(so) if rc == 0 else None
    pg = doc[0]['page_list'] if doc else []
    check('MnUIHD p2/0 --root: exactly one root page (4), rec+152 = 1, branch saved_return',
          len(pg) == 1 and pg[0]['index'] == 4 and pg[0]['rec152'] == 1 and pg[0]['branch']['kind'] == 'saved_return', (rc, so[:300], se))
    check('its item leads to page 3 MatchCreator_Observer', pg and pg[0]['items'][0]['target_name'] == 'MatchCreator_Observer', pg)
    rc, so, se = q('uipr', 'MnUIHD', 'p2/0', '--pages')
    check('--pages lists the 5 pages with the record words and the root branch line', so.count('  page ') == 5 and '+152 1 +160 0' in so and 'SAVED RETURN' in so, so)
    rc, so, se = q('uipr', 'MnUIHD', 'p2/0', '--commands', '120')
    check('--commands passes through (exit 0)', rc == 0 and so.startswith('== MnUIHD.pac p2/0'), (rc, so[:200], se))
    rc, so, se = q('uipr', 'MnUIHD', 'nope/9')
    check('an unknown leaf is `q: ...` exit 2', rc == 2 and 'not a UIPR layout leaf' in se, (rc, se))


if __name__ == '__main__':
    planted()
    real()
    print('quipr selftest: %d problems' % len(fails))
    sys.exit(1 if fails else 0)
