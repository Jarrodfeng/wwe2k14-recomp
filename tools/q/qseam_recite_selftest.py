r"""Self-test of q seam --recite (S111 lane SEAMRECITE; tools/q/qseam.py, tools/handback/seam_wire.py). 0 game runs; temp files only.

    py -I tools\q\qseam_recite_selftest.py

A temporary seam store with one moved row, one stale row and one exact row (the moved one carries a ctx):
  1  --recite rewrites only the moved row (line, and ctx in step); the exact and the stale row are byte for byte as before; the stale row is listed;
     the file keeps its key order and indent 1; a second --recite changes nothing; --check then reports 0 moved (1 stale stays).
  2  seam_wire._apply on an existing row cited at another line updates that row's line (not "present", not a new row).
Each case is run again with WWE_QSEAM_PLANT=no_recite (recite skips the write) and must then FAIL.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
QSEAM = os.path.join(HERE, 'qseam.py')
sys.path.insert(0, HERE)
import qseam  # noqa: E402

FAILS = []


def check(ok, what, detail=''):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        FAILS.append(what)
        if detail:
            print('     ' + detail.replace('\n', '\n     ')[:1500])


def run(args, root, plant=''):
    env = dict(os.environ)
    env.pop('WWE_QSEAM_PLANT', None)
    if plant:
        env['WWE_QSEAM_PLANT'] = plant
    r = subprocess.run([sys.executable, '-I', QSEAM] + args + ['--root', root], capture_output=True, text=True, encoding='utf-8',
                       errors='replace', env=env)
    return r.returncode, r.stdout + r.stderr


def build(root):
    os.makedirs(os.path.join(root, 'src'))
    os.makedirs(os.path.join(root, 'docs'))
    os.makedirs(os.path.join(root, 'db', 'seams'))
    with open(os.path.join(root, 'docs', 'n.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('note\n')
    # a.cpp: "x = 1;" twice (the row with ctx is the one after "alpha();"), "keep();" once at line 6, "gone" nowhere
    with open(os.path.join(root, 'src', 'a.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('// pad\n// pad\nbeta();\nx = 1;\nalpha();\nx = 1;\nkeep();\n')
    entry = {
        'topic': 't', 'summary': 's',
        'game': [{'addr': '0x82C224A0', 'what': 'w', 'evidence': 'e', 'mark': 'C', 'source': 'docs/n.md'}],
        'ours': [
            {'path': 'src/a.cpp', 'line': 3, 'anchor': 'x = 1;', 'ctx': ['alpha();', 'x = 1;', 'beta();'], 'verdict': 'match', 'why': 'moved'},
            {'path': 'src/a.cpp', 'line': 7, 'anchor': 'keep();', 'verdict': 'match', 'why': 'exact'},
            {'path': 'src/a.cpp', 'line': 2, 'anchor': 'gone();', 'verdict': 'replace', 'why': 'stale'},
        ],
        'open': [],
    }
    with open(os.path.join(root, 'db', 'seams', 't.json'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(entry, indent=1, ensure_ascii=False) + '\n')
    return os.path.join(root, 'db', 'seams', 't.json')


def case_cli(plant):
    with tempfile.TemporaryDirectory() as tmp:
        p = build(tmp)
        before = open(p, encoding='utf-8').read()
        e0 = json.loads(before)
        code, out = run(['--recite'], tmp, plant)
        after = open(p, encoding='utf-8').read()
        e1 = json.loads(after)
        o0, o1 = e0['ours'], e1['ours']
        check(code == 0 and 'row 1: line 3 -> 6' in out, 'recite prints the moved row (line 3 -> 6)', out)
        check(o1[0]['line'] == 6, 'the moved row is rewritten to line 6', str(o1[0]))
        check(o1[0].get('ctx') == ['alpha();'], 'the moved row keeps its ctx in step (the fewest lines that make it unique)', str(o1[0]))
        check(o1[1] == o0[1] and o1[2] == o0[2], 'the exact row and the stale row are untouched')
        check('STALE' in out and 'row 3' in out, 'the stale row is listed', out)
        check(list(o1[0].keys()) == ['path', 'line', 'anchor', 'ctx', 'verdict', 'why'] and list(e1.keys()) == list(e0.keys()), 'key order kept')
        check(after.splitlines()[1].startswith(' "') and not after.splitlines()[1].startswith('  '), 'indent 1 kept')
        code2, out2 = run(['--recite'], tmp, plant)
        check(open(p, encoding='utf-8').read() == after and '0 rows recited' in out2, 'a second recite changes nothing', out2)
        code3, out3 = run(['--check'], tmp, plant)
        check('moved' not in out3 and 'STALE' in out3 and code3 == 1, '--check: 0 moved, the stale row stays', out3)


def case_wire(plant):
    sys.path.insert(0, os.path.join(REPO, 'tools', 'handback'))
    import seam_wire
    with tempfile.TemporaryDirectory() as tmp:
        p = build(tmp)
        qs = seam_wire.load_qseam()
        entry = qs.load(p)
        orow = qs.ours_row('src/a.cpp', 7, 'match', 'exact', root=tmp)
        entry['ours'][1]['line'] = 2   # stored line is wrong; the same row is cited at its line now
        st = seam_wire._stat()
        o = {'address': '0x82C224A0', 'claim': 'w', 'evidence': 'e', 'mark': 'C', 'note': 'docs/n.md'}
        seam_wire._apply(qs, entry, 't', [(o, orow)], tmp, st)
        if plant == 'no_recite':
            entry['ours'][1]['line'] = 2
        check(entry['ours'][1]['line'] == 7 and len(entry['ours']) == 3, 'seam_wire._apply re-cites an existing row at the cited line',
              str(entry['ours'][1]))


def main():
    for plant in ('', 'no_recite'):
        n0 = len(FAILS)
        print('--- plant=%r' % plant)
        case_cli(plant)
        case_wire(plant)
        if plant:
            check(len(FAILS) > n0, 'the planted twin (recite skips the write) fails')
            del FAILS[n0:]
    print('selftest: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'ok'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
