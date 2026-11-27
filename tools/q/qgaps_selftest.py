"""Selftest for the S111 QGAPS verbs: asm --at / FUNC+OFF, callers --in, seam --cites, show --ref, ours text (any-of, phrase, --in FILE, -E), ours body --in.
0 game runs. Planted twins on throwaway git repos; the two checks that need the game index (callers --in) SKIP without it. Exit 1 = a check failed.
(q words and q shader have their own selftests: qwords_selftest.py, qshader_selftest.py.)"""
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
Q = os.path.join(HERE, 'q.py')
fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:300]))
    if not ok:
        fails.append(what)


def q(*args, cwd=None):
    r = subprocess.run([sys.executable, '-I', Q] + list(args), capture_output=True, text=True, cwd=cwd, encoding='utf-8', errors='replace')
    return r.returncode, r.stdout, r.stderr


def write(root, rel, text):
    p = os.path.join(root, *rel.split('/'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def git(root, *a):
    r = subprocess.run(['git', '-C', root, '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false'] + list(a), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('git %s: %s' % (a, r.stderr))


def at_window_cases():
    import qx6
    if not hasattr(qx6, 'at_window'):
        check('asm --at: qx6.at_window exists', False)
        return
    lo, hi = qx6.at_window('B90', 0x82403D50, 0x14BC, 2)
    check('asm --at OFF: +-ctx instructions round the offset', (lo, hi) == ('824048D8', '824048E8'), (lo, hi))
    check('asm --at A..B: the range as given (a leading + and 0x allowed)', qx6.at_window('+0xB90..+BA0', 0x82403D50, 0x14BC, 2) == ['824048E0', '824048F0'], qx6.at_window('+0xB90..+BA0', 0x82403D50, 0x14BC, 2))
    check('twin: --at near the entry stays inside the function', qx6.at_window('4', 0x82403D50, 0x14BC, 12)[0] == '82403D50', qx6.at_window('4', 0x82403D50, 0x14BC, 12))
    try:
        qx6.at_window('xyz', 0x82403D50, 0x100, 2)
        check('asm --at: a non-hex offset is refused', False)
    except qx6.XError:
        check('asm --at: a non-hex offset is refused', True)
    try:      # review S111: past the end used to reach asm_answer as FROM > TO ("FROM must not be above TO")
        qx6.at_window('99999', 0x82403D50, 0x14BC, 2)
        check('asm --at past the end of the function says so', False)
    except qx6.XError as e:
        check('asm --at past the end of the function says so', 'past the end' in str(e), str(e))
    check('twin: --at the last instruction is inside', qx6.at_window('14B8', 0x82403D50, 0x14BC, 0)[0] == '82405208', qx6.at_window('14B8', 0x82403D50, 0x14BC, 0))
    check('asm --at an unaligned offset is the instruction holding it', qx6.at_window('B92', 0x82403D50, 0x14BC, 0) == ['824048E0', '824048E0'], qx6.at_window('B92', 0x82403D50, 0x14BC, 0))


def seam_cites_case(tmp):
    write(tmp, 'src/a.h', 'l1\nl2\nint anchor_two = 2;\nint anchor_one = 1;\nl5\nl6\n')
    write(tmp, 'src/xa.h', 'l1\n')
    seam = {'topic': 'tt', 'summary': 'the rule sits at a.h:4', 'game': [{'addr': '0x82000000', 'what': 'w', 'evidence': 'see src/a.h:3-5', 'mark': 'C', 'source': 'docs/n.md'}],
            'ours': [{'path': 'src/a.h', 'anchor': 'int anchor_two = 2;', 'verdict': 'match', 'why': 'row at 3'},
                     {'path': 'src/a.h', 'anchor': 'int anchor_one = 1;', 'verdict': 'replace', 'why': 'stored 3, moved to 4'},
                     {'path': 'src/xa.h', 'anchor': 'l1', 'verdict': 'match', 'why': 'twin file'}], 'open': []}
    write(tmp, 'db/seams/tt.json', json.dumps(seam))
    write(tmp, 'db/claims/c.json', json.dumps([{'claim': 'c', 'evidence': 'q show a.h:2-3 and other.h:4', 'ours': 'src/a.h:9', 'address': '0x82000004'}]))
    rc, out, err = q('seam', '--cites', 'src/a.h', '--root', tmp)
    check('cites FILE: both a.h ours rows, the game row text, the summary and the claim are listed; the xa.h twin is not', rc == 0 and '5 rows' in out and 'xa.h' not in out, out)
    rc, out, err = q('seam', '--cites', 'a.h:4', '--root', tmp)
    check('cites FILE:LINE: the row whose anchor is at 4 NOW (stored 3), the 3-5 range and the summary cite; not the row at 3, not the 2-3 claim',
          rc == 0 and 'anchor' not in out and 'stored 3, moved to 4' in out and 'cites src/a.h:3-5' in out and 'db/claims' not in out and out.count('db/seams') == 3, out)
    rc, out, err = q('seam', '--cites', 'a.h:2', '--root', tmp)
    check('twin: line 2 is cited by the claim range 2-3 only', rc == 0 and 'db/claims/c.json' in out and 'db/seams' not in out, out)
    rc, out, err = q('seam', '--cites', 'other.h', '--root', tmp)
    check('a cite of another file answers for that file only', rc == 0 and '1 rows' in out and 'db/claims/c.json' in out, out)
    rc, out, err = q('seam', '--cites', 'a.h:4', '--json', '--root', tmp)
    check('--json is one document with n', rc == 0 and json.loads(out)['n'] == 3, out)


def show_ref_case(tmp):
    git(tmp, 'init', '-q')
    write(tmp, 'src/f.h', 'old one\nold two\n')
    git(tmp, 'add', 'src/f.h')
    git(tmp, 'commit', '-q', '-m', 'one')
    write(tmp, 'src/f.h', 'new one\nnew two\n')
    rc, out, err = q('show', 'src/f.h:1-2', '--root', tmp)
    check('show: the working file', rc == 0 and 'new one' in out and 'old' not in out, out)
    rc, out, err = q('show', 'src/f.h:1-2', '--ref', 'HEAD', '--root', tmp)
    check('show --ref HEAD: the committed lines (twin of the line above), the ref in the header', rc == 0 and 'old one' in out and 'new' not in out and '@ HEAD' in out, out)
    rc, out, err = q('show', 'f.h:1', '--ref', 'HEAD', '--root', tmp)
    check('show --ref: a unique basename resolves in the ref\'s tree', rc == 0 and 'old one' in out, out)
    rc, out, err = q('show', 'src/f.h:1', '--ref', 'no_such_ref', '--root', tmp)
    check('show --ref: an unknown ref is an error', rc == 2, (rc, err))
    rc, out, err = q('show', 'src/f.h:1', '--ref', '--cached', '--root', tmp)
    check('show --ref: a ref starting with - is refused', rc == 2, (rc, err))
    git(tmp, 'rm', '-q', '-f', 'src/f.h')
    git(tmp, 'commit', '-q', '-m', 'gone')
    rc, out, err = q('show', 'src/f.h:1', '--ref', 'HEAD~1', '--root', tmp)
    check('show --ref: a file that is gone now still reads at the older commit', rc == 0 and 'old one' in out, (rc, out, err))


def text_cases(tmp):
    write(tmp, 'src/a.cpp', 'int alpha_only_main = 1;\nint beta_both = 2;\nstruct Scene Header_x {};\n')
    write(tmp, 'db/seams/s.json', '{"k": "needle_in_db"}\n')
    write(tmp, 'ROOTSUM.md', 'root summary mentions wetlook_word\n')
    for i in range(6):      # review: six files so --limit 1 cuts the list and the trim line names the call for the rest
        write(tmp, 'src/g%d.cpp' % i, 'int gamma_w = 1;\nint delta_w = 2;\nint both = gamma_w + delta_w;\n')
    git(tmp, 'init', '-q')
    git(tmp, 'add', '-A')
    git(tmp, 'commit', '-q', '-m', 'x')
    rc, out, err = q('ours', 'text', 'alpha_only_main', 'beta_both', '--root', tmp)
    check('text A B: any of them (2 lines)', rc == 0 and ': 2 lines' in out, out)
    rc, out, err = q('ours', 'text', 'alpha_only_main', 'beta_both', '--and', '--root', tmp)
    check('text A B --and: all on one line (twin of the line above: 0 lines)', rc == 0 and ': 0 lines' in out, out)
    rc, out, err = q('ours', 'text', 'Scene Header_x', '--root', tmp)
    check('text "A B": a quoted phrase stays one phrase', rc == 0 and ': 1 lines' in out and 'a.cpp:3' in out, out)
    rc, out, err = q('ours', 'text', 'needle_in_db', '--in', 'db/seams/s.json', '--root', tmp)
    check('text --in FILE: a file outside the code paths is searched', rc == 0 and ': 1 lines' in out and 's.json:1' in out, out)
    rc, out, err = q('ours', 'text', 'needle_in_db', '--root', tmp)
    check('twin: the same word without --in is not in the default paths', rc == 0 and ': 0 lines' in out, out)
    rc, out, err = q('ours', 'text', 'wetlook_word', '--in', 'ROOTSUM.md', '--root', tmp)
    check('text --in a root *.md file', rc == 0 and ': 1 lines' in out, out)
    rc, out, err = q('ours', 'text', r'beta_both = \d', '--re', '--root', tmp)
    check('text --re is -E', rc == 0 and ': 1 lines' in out, out)
    rc, out, err = q('ours', 'text', 'absent_word_zz', '--root', tmp)
    check('0 lines says how to widen the search', rc == 0 and '-E' in out and '--and' in out, out)
    # review: the `all:` call a cut answer names must give the SAME answer (several words stay several words, --and / -E / --docs ride along)
    for flags, want in (([], 18), (['--and'], 6)):
        rc, out, err = q('ours', 'text', 'gamma_w', 'delta_w', '--limit', '1', *flags, '--root', tmp)
        m = re.search(r'all: (q ours text .*?)(?:, or narrow|$)', out, re.M)
        again = shlex.split(m.group(1)) if m else []
        rc2, out2, err2 = q(*(again[1:] + ['--root', tmp])) if again else (1, '', '')
        check('the trim line\'s all: call repeats the same question (%s)' % (' '.join(flags) or 'any-of'),
              rc == 0 and (': %d lines' % want) in out and rc2 == 0 and (': %d lines' % want) in out2, (m and m.group(1), out[:200], out2[:200]))
    rc, out, err = q('ours', 'text', 'needle_in_db', '--in', 'db/seams/s.json', '--root', tmp)
    check('text --in an existing path says only that path was searched (not "0 more outside it")', rc == 0 and 'only that path was searched' in out and 'more outside it' not in out, out)
    rc, out, err = q('ours', 'text', 'gamma_w', '--in', 'g1.cpp', '--root', tmp)
    check('twin: --in a basename that is no path still counts what it left outside', rc == 0 and '10 more outside it' in out, out)
    rc, out, err = q('ours', 'body', 'run', '--in', 'tools/q/qasset_selftest.py', '--root', os.path.dirname(os.path.dirname(HERE)))
    check('body NAME --in FILE: the definition in that file, none of the others', rc == 0 and 'tools/q/qasset_selftest.py:217' in out and out.count('== function') == 1, out)


def callers_case():
    idx = os.path.join(os.path.dirname(HERE), '..', 'out', 'harness', 'index', 'index.db')
    main_idx = r'C:\Users\Jarrod\wwe2k14-recomp\out\harness\index\index.db'
    if not (os.path.exists(idx) or os.path.exists(main_idx)):
        print('SKIP callers --in: no game index')
        return
    rc, out, err = q('callers', 'sub_82BB16A0', '--in', 'sub_82C04DF0')
    check('callers X --in F: only F\'s call sites', rc == 0 and 'in 1 functions' in out and 'sub_82C04DF0' in out, out[:300])
    rc, out, err = q('callers', 'sub_82BB16A0', '--in', 'sub_82C04DF4')
    check('twin: --in an address inside the same function is that function', rc == 0 and 'in 1 functions' in out, out[:300])
    rc, out, err = q('callers', 'sub_82BB16A0', '--in', 'sub_82BB16A0')
    check('twin: --in a function that does not call X lists no caller', rc == 0 and ' 0 call sites' in out, out[:300])


def main():
    at_window_cases()
    callers_case()
    for fn in (seam_cites_case, show_ref_case, text_cases):
        with tempfile.TemporaryDirectory(prefix='qgaps_', ignore_cleanup_errors=True) as tmp:
            fn(tmp)
    print('FAILED: %s' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
