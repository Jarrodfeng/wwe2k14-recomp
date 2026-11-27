r"""Self-test of q seam (S111 lane SEAM; tools/q/qseam.py). 0 game runs; the planted cases run on temp files only.

    py -I tools\q\qseam_selftest.py [--root DIR]      (--root: the checkout whose db/seams/depth.json is case a; default this one)

a  the real `depth` entry prints both sides: the viewport reversal's address 0x82C224A0 under GAME, the quads2d_draw.cpp line under OURS,
   no STALE row; `--check` of the checkout exits 0; q find's block and q fact's chunk name it.
b  a planted entry whose game row has no address is REFUSED: by --check (exit 1, "no address") and by the writer add_entry (SeamError).
c  a planted ours row whose anchor line moved prints the line it is at NOW (no stored line); once its anchor text is gone it prints STALE and
   --check exits 1.
f  (lane SEAMDIFF) --diffs / --files / --json, two identical anchor lines (`occurrence`; a row without it is AMBIGUOUS, plant no_occurrence;
   a row that stores `line` is refused, plant no_line_rule), a missing source note, a ../ path.
d  (this checkout only) the wiring: `q find depth` prints the seam entry line, `q fact ... --kind seam` finds db/seams/depth.json.
Every planted case is run again with the rule switched off (WWE_QSEAM_PLANT / qseam.PLANT = no_addr_rule | no_stale | no_occurrence | no_line_rule) and
must then FAIL: a case that passes without its rule proves nothing.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qseam  # noqa: E402

QSEAM = os.path.join(HERE, 'qseam.py')
QPY = os.path.join(HERE, 'q.py')
FAILS = []


def check(ok, what, detail=''):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        FAILS.append(what)
        if detail:
            print('     ' + detail.replace('\n', '\n     ')[:2000])


def run(args, root, plant=''):
    env = dict(os.environ)
    env.pop('WWE_QSEAM_PLANT', None)
    if plant:
        env['WWE_QSEAM_PLANT'] = plant
    r = subprocess.run([sys.executable, '-I', QSEAM] + args + ['--root', root], capture_output=True, text=True, encoding='utf-8',
                       errors='replace', env=env)
    return r.returncode, r.stdout + r.stderr


def write(root, rel, text):
    p = os.path.join(root, *rel.split('/'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def section(out, start, end):
    i = out.find(start)
    j = out.find(end, i + 1) if i >= 0 else -1
    return out[i:j if j >= 0 else len(out)] if i >= 0 else ''


def case_a(root):
    rc, out = run(['depth'], root)
    game, ours = section(out, '\nGAME (', '\nOURS ('), section(out, '\nOURS (', '\nOPEN (')
    check(rc == 0, 'a: q seam depth exits 0', out)
    check('0x82C224A0' in game and 'max 0' in game, 'a: GAME holds the viewport reversal 0x82C224A0 (min 1, max 0)', out)
    check('src/renderer_scene/quads2d_draw.cpp:' in ours, 'a: OURS holds the quads2d_draw.cpp line', out)
    check('STALE' not in out and 'REFUSED' not in out, 'a: no STALE row, not refused', out)
    rc, out = run(['--check'], root)
    check(rc == 0 and 'ok   depth' in out, 'a: q seam --check of the checkout exits 0', out)
    fb = qseam.find_block(root, 'depth') or {'topics': []}
    check(any(t['topic'] == 'depth' and t['exact'] for t in fb['topics']), 'a: q find depth reaches the entry (find_block)', json.dumps(fb))
    head, body = qseam.fact_chunk(qseam.entry_path('depth', root))
    check(head == 'seam depth' and any('0x82C224A0' in x for x in body), 'a: q fact chunk of the entry names 0x82C224A0', '\n'.join(body))


def entry(topic, game, anchor):
    return {'topic': topic, 'summary': 'A planted entry.', 'game': game,
            'ours': [{'path': 'src/a.cpp', 'anchor': anchor, 'verdict': 'match', 'why': 'planted'}], 'open': []}


GOOD_GAME = [{'addr': '0x82000000', 'what': 'planted', 'evidence': 'planted', 'mark': 'C', 'source': 'docs/n.md'}]
NOADDR_GAME = [{'what': 'planted', 'evidence': 'planted', 'mark': 'C', 'source': 'docs/n.md'}]


def case_b(tmp):
    root = os.path.join(tmp, 'b')
    write(root, 'docs/n.md', 'note\n')
    write(root, 'src/a.cpp', '// a\nint x = 1;\nint y = 2;\n')
    write(root, 'db/seams/noaddr.json', json.dumps(entry('noaddr', NOADDR_GAME, 'int y = 2;'), indent=1))
    rc, out = run(['--check'], root)
    check(rc == 1 and 'no address' in out, 'b: --check refuses a game row with no address (exit 1)', out)
    rc_p, out_p = run(['--check'], root, plant='no_addr_rule')
    check(rc_p == 0 and 'no address' not in out_p, 'b: without the rule (plant no_addr_rule) the same entry passes: the case catches it', out_p)
    refused = False
    try:
        qseam.add_entry('noaddr2', 'A planted entry.', NOADDR_GAME, [qseam.ours_row('src/a.cpp', 3, 'match', 'planted', root=root)], root=root)
    except qseam.SeamError as e:
        refused = 'no address' in str(e)
    check(refused and not os.path.exists(qseam.entry_path('noaddr2', root)), 'b: the writer add_entry refuses it and writes nothing')
    old, qseam.PLANT = qseam.PLANT, 'no_addr_rule'
    try:
        qseam.add_entry('noaddr3', 'A planted entry.', NOADDR_GAME, [qseam.ours_row('src/a.cpp', 3, 'match', 'planted', root=root)], root=root,
                        dry_run=True)
        planted_refused = False
    except qseam.SeamError:
        planted_refused = True
    finally:
        qseam.PLANT = old
    check(not planted_refused, 'b: without the rule the writer accepts it: the case catches it')
    empty = False
    try:
        qseam.add_entry('nogame', 'A planted entry.', [], [qseam.ours_row('src/a.cpp', 3, 'match', 'planted', root=root)], root=root, dry_run=True)
    except qseam.SeamError as e:
        empty = 'no game row' in str(e)
    check(empty, 'b: an entry with no game row at all is refused')
    noours = False
    try:
        qseam.add_entry('noours', 'A planted entry.', GOOD_GAME, [], root=root, dry_run=True)
    except qseam.SeamError as e:
        noours = 'no ours row' in str(e)
    check(noours, 'b: an entry with no ours row is refused')


def case_c(tmp):
    root = os.path.join(tmp, 'c')
    write(root, 'docs/n.md', 'note\n')
    write(root, 'src/a.cpp', '// a\nint x = 1;\nint y = 2;\nint z = 3;\n')
    path, _ = qseam.add_entry('moving', 'A planted entry.', GOOD_GAME, [qseam.ours_row('src/a.cpp', 3, 'match', 'planted', root=root)], root=root)
    check(os.path.exists(path), 'c: the writer stores a good entry')
    rc, out = run(['moving'], root)
    check(rc == 0 and 'src/a.cpp:3  match' in out and 'moved' not in out, 'c: at its line the row prints plain (ok)', out)
    write(root, 'src/a.cpp', '// a\n// new 1\n// new 2\nint x = 1;\nint y = 2;\nint z = 3;\n')
    rc, out = run(['moving'], root)
    check(rc == 0 and 'src/a.cpp:5  match' in out and 'src/a.cpp:3' not in out, 'c: the anchor two lines lower prints the line it is at now (5), no stored line to rot', out)
    check('line' not in qseam.load(qseam.entry_path('moving', root))['ours'][0], 'c: the stored row holds no `line`')
    write(root, 'src/a.cpp', '// a\nint x = 1;\nint y = 7;\nint z = 3;\n')
    rc, out = run(['moving'], root)
    check(rc == 1 and 'STALE' in out, 'c: the anchor text gone prints STALE (exit 1)', out)
    rc, out = run(['--check'], root)
    check(rc == 1 and 'STALE' in out, 'c: --check exits 1 on the STALE row', out)
    rc_p, out_p = run(['--check'], root, plant='no_stale')
    check(rc_p == 0 and 'STALE' not in out_p, 'c: without the rule (plant no_stale) --check passes: the case catches it', out_p)
    bad = False
    try:
        qseam.add_row('moving', 'ours', {'path': 'src/a.cpp', 'anchor': 'int y = 2;', 'verdict': 'match', 'why': 'x'}, root=root)
    except qseam.SeamError as e:
        bad = 'ours row 2' in str(e) and 'anchor' in str(e)
    check(bad, 'c: the writer add_row refuses a NEW row whose anchor is not its line now (row 2 named)')


def case_e(tmp):
    """review (S111 SEAM): the tree moves under the lodging lane. An EXISTING row that moved (or went stale) must not block add_row of
    a good new row: only the new row's anchor is checked; the lookup shows the moved row at its line now and a stale
    one still fails --check."""
    root = os.path.join(tmp, 'e')
    write(root, 'docs/n.md', 'note\n')
    write(root, 'src/a.cpp', '// a\nint x = 1;\nint y = 2;\nint z = 3;\n')
    qseam.add_entry('shift', 'A planted entry.', GOOD_GAME, [qseam.ours_row('src/a.cpp', 3, 'match', 'planted', root=root)], root=root)
    write(root, 'src/a.cpp', '// a\n// new 1\n// new 2\nint x = 1;\nint y = 2;\nint z = 3;\n')
    err = ''
    try:
        qseam.add_row('shift', 'ours', qseam.ours_row('src/a.cpp', 6, 'match', 'planted', root=root), root=root)
    except qseam.SeamError as e:
        err = str(e)
    e = qseam.load(qseam.entry_path('shift', root))
    check(not err and len(e['ours']) == 2 and 'line' not in e['ours'][0] and 'line' not in e['ours'][1],
          'e: add_row of a good new row is accepted while an existing row has moved (no row stores a line)', err)
    rc, out = run(['shift'], root)
    check(rc == 0 and 'src/a.cpp:5' in out, 'e: the lookup shows the existing row at line 5 now', out)
    write(root, 'src/a.cpp', '// a\n// new 1\n// new 2\nint x = 1;\nint y = 9;\nint z = 3;\n')
    err = ''
    try:
        qseam.add_row('shift', 'ours', qseam.ours_row('src/a.cpp', 4, 'match', 'planted', root=root), root=root)
    except qseam.SeamError as e2:
        err = str(e2)
    check(not err, 'e: add_row of a good new row is accepted while an existing row is STALE', err)
    rc, out = run(['--check'], root)
    check(rc == 1 and 'STALE' in out, 'e: --check still exits 1 on that STALE row', out)


def case_f(tmp):
    """lane SEAMDIFF: --diffs, identical anchors, the missing source note, a path that leaves the repo, occurrence / line / ctx rules"""
    root = os.path.join(tmp, 'f')
    write(root, 'docs/n.md', 'note\n')
    write(root, 'x', 'outside\n')
    two = '// a\nvoid f() {\n  v[5] = 1;\n}\nvoid g() {\n  v[5] = 1;\n}\n'
    write(root, 'src/a.cpp', two)
    write(root, 'src/b.cpp', 'int p = 1;\nint q = 2;\nint r = 3;\n')
    gm = [dict(GOOD_GAME[0], addr='0x82000010', what='first game rule\nsecond line'), dict(GOOD_GAME[0], addr='0x82000020', what='other rule')]
    ro = qseam.ours_row
    rows = [ro('src/a.cpp', 3, 'replace', 'WHYREPLACE ours own way', root=root), ro('src/a.cpp', 6, 'match', 'WHYMATCH same', root=root),
            ro('src/b.cpp', 2, 'repeat', 'WHYREPEAT redone', root=root)]
    qseam.add_entry('planted', 'A planted entry.', gm, rows, root=root)
    check(rows[0].get('occurrence') == 1 and rows[1].get('occurrence') == 2 and 'line' not in rows[0],
          'f: the writer stores `occurrence` (which of the lines) for a row whose anchor text repeats in its file, and no line', json.dumps(rows[0]))
    check('occurrence' not in rows[2] and 'ctx' not in rows[2], 'f: a unique anchor stores no occurrence')
    rc, out = run(['--diffs'], root)
    check(rc == 0 and 'WHYREPLACE' in out and 'WHYREPEAT' in out and 'WHYMATCH' not in out, 'f: --diffs lists the planted replace and repeat rows, not the match row', out)
    check('seam diffs: 2 rows: replace 1, repeat 1' in out and 'planted: replace 1, repeat 1' in out, 'f: --diffs totals per verdict and per topic are right', out)
    check('vs 0x82000010  first game rule' in out and 'second line' not in out and 'src/a.cpp:3' in out,
          'f: --diffs shows the game row (address + first line of what) and the path:line', out)
    rc, out = run(['--diffs', '--verdict', 'repeat'], root)
    check('WHYREPEAT' in out and 'WHYREPLACE' not in out and 'repeat 1' in out, 'f: --diffs --verdict repeat narrows', out)
    # review (S111 SEAMDIFF): --topic narrows (it shared its dest with the positional TOPIC, whose empty default overwrote it)
    rc, out = run(['--diffs', '--topic', 'nosuch'], root)
    check(rc == 1 and 'WHYREPLACE' not in out and 'no entry' in out, 'f: --diffs --topic of no entry lists nothing and exits 1', out)
    rc, out = run(['--diffs', '--topic', 'planted'], root)
    check(rc == 0 and 'WHYREPLACE' in out and 'in 1 topics' in out, 'f: --diffs --topic planted lists that topic', out)
    rc, out = run(['--diffs', 'nosuch'], root)
    check(rc == 1 and 'WHYREPLACE' not in out, 'f: --diffs TOPIC (positional) narrows the same way', out)
    rc, out = run(['--diffs', '--files'], root)
    check('== file src/a.cpp' in out and '== file src/b.cpp' in out and out.index('== file src/a.cpp') < out.index('WHYREPLACE') < out.index('== file src/b.cpp'),
          'f: --diffs --files groups the rows by our file', out)
    rc, out = run(['--diffs', '--json'], root)
    try:
        t = json.loads(out)['totals']
    except ValueError:
        t = {}
    check(t.get('replace') == 1 and t.get('repeat') == 1 and t.get('rows') == 2, 'f: --diffs --json carries the totals', out)
    # two identical anchor lines: the first one's line deleted -> occurrence 2 is past the last line: ITS row (the second) is STALE
    write(root, 'src/a.cpp', '// a\nvoid f() {\n}\nvoid g() {\n  v[5] = 1;\n}\n')
    rc, out = run(['planted'], root)
    check(rc == 1 and '!! STALE src/a.cpp' in out and 'occurrence 2 asked' in out and 'src/a.cpp:5' in out,
          'f: two identical anchor lines, one deleted: the occurrence-2 row is STALE, the occurrence-1 row reads the line that is left (5)', out)
    rc_p, out_p = run(['planted'], root, plant='no_stale')
    check('STALE' not in out_p, 'f: without the rule (plant no_stale) nothing is STALE: the case catches it', out_p)
    # lines inserted above both: each row follows its own occurrence
    write(root, 'src/a.cpp', '// top\n// top2\n' + two)
    rc, out = run(['planted'], root)
    check(rc == 0 and 'src/a.cpp:5  replace' in out and 'src/a.cpp:8  match' in out, 'f: lines inserted above: each row reads the line its occurrence is at now (5 and 8)', out)
    # an anchor on two lines with no `occurrence`: AMBIGUOUS (the only case a line number was resolving)
    write(root, 'src/a.cpp', two)
    amb = {'topic': 'ambig', 'summary': 'A planted entry.', 'game': GOOD_GAME,
           'ours': [{'path': 'src/a.cpp', 'anchor': 'v[5] = 1;', 'verdict': 'match', 'why': 'planted'}], 'open': []}
    write(root, 'db/seams/ambig.json', json.dumps(amb, indent=1))
    rc, out = run(['--check'], root)
    check(rc == 1 and 'AMBIGUOUS' in out and 'occurrence' in out, 'f: --check fails an anchor on two lines that has no `occurrence`', out)
    rc_p, out_p = run(['--check'], root, plant='no_occurrence')
    check('AMBIGUOUS' not in out_p, 'f: without the rule (plant no_occurrence) it passes: the case catches it', out_p)
    rc, out = run(['ambig'], root)
    check(rc == 1 and '!! AMBIGUOUS src/a.cpp' in out, 'f: the lookup prints the AMBIGUOUS row', out)
    refused = False
    try:
        qseam.add_row('ambig', 'ours', {'path': 'src/a.cpp', 'anchor': 'v[5] = 1;', 'verdict': 'match', 'why': 'x'}, root=root)
    except qseam.SeamError as e:
        refused = 'AMBIGUOUS' in str(e)
    check(refused, 'f: the writer add_row refuses a row whose anchor is on two lines and has no occurrence')
    os.remove(qseam.entry_path('ambig', root))
    # a row that keeps writing `line` (the twin of the old format) or the retired `ctx`
    old = {'topic': 'oldrows', 'summary': 'A planted entry.', 'game': GOOD_GAME,
           'ours': [{'path': 'src/b.cpp', 'line': 2, 'anchor': 'int q = 2;', 'verdict': 'match', 'why': 'planted'}], 'open': []}
    write(root, 'db/seams/oldrows.json', json.dumps(old, indent=1))
    rc, out = run(['--check'], root)
    check(rc == 1 and 'stored `line`' in out, 'f: --check refuses an ours row that stores a `line`', out)
    rc_p, out_p = run(['--check'], root, plant='no_line_rule')
    check('stored `line`' not in out_p, 'f: without the rule (plant no_line_rule) it passes: the case catches it', out_p)
    old['ours'][0].pop('line')
    old['ours'][0]['ctx'] = ['int p = 1;']
    write(root, 'db/seams/oldrows.json', json.dumps(old, indent=1))
    rc, out = run(['--check'], root)
    check(rc == 1 and '`ctx` is retired' in out, 'f: --check refuses the retired `ctx`', out)
    os.remove(qseam.entry_path('oldrows', root))
    # a missing source note
    write(root, 'src/a.cpp', two)
    gone = dict(GOOD_GAME[0], source='docs/missing_note.md')
    qseam.add_entry('nosrc', 'A planted entry.', [gone], [ro('src/b.cpp', 1, 'match', 'planted', root=root)], root=root)
    rc, out = run(['--check'], root)
    check(rc == 1 and 'docs/missing_note.md' in out and 'does not exist' in out, 'f: --check reports a game row whose source note is missing', out)
    # a path that leaves the repo
    refused = False
    try:
        ro('../x', 1, 'match', 'planted', root=root)
    except qseam.SeamError as e:
        refused = 'never leaves the repo' in str(e)
    check(refused, 'f: ours_row refuses ../x')
    bad = {'topic': 'esc', 'summary': 'A planted entry.', 'game': GOOD_GAME,
           'ours': [{'path': '../x', 'anchor': 'outside', 'verdict': 'match', 'why': 'planted'}], 'open': []}
    write(root, 'db/seams/esc.json', json.dumps(bad, indent=1))
    rc, out = run(['--check'], root)
    check(rc == 1 and 'leaves the repo' in out, 'f: --check refuses a stored ../x path', out)
    os.remove(os.path.join(root, 'db', 'seams', 'esc.json'))
    os.remove(os.path.join(root, 'db', 'seams', 'nosrc.json'))


def git(root, *args):
    r = subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'commit.gpgsign=false', '-C', root] + list(args),
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.stdout.strip()


def case_g(tmp):
    """check.py's `seam` row on planted diffs: a deleted anchored line FAILs, a shifted one passes, an unrelated file passes
    even when a row of an untouched file is stale (it is not judged); a stored `line` / an ambiguous anchor FAILs."""
    chk = os.path.abspath(os.path.join(HERE, '..', 'handback', 'check.py'))

    def repo(name, a_text, entry_row_anchor='int y = 2;', extra=None, game=GOOD_GAME):
        root = os.path.join(tmp, name)
        os.makedirs(root)
        git(root, 'init', '-q')
        write(root, 'docs/n.md', 'note\n')
        write(root, 'src/a.cpp', a_text)
        write(root, 'src/c.cpp', 'int c = 1;\n')
        row = {'path': 'src/a.cpp', 'anchor': entry_row_anchor}
        row.update(extra or {})
        row.update({'verdict': 'replace', 'why': 'planted'})
        ent = {'topic': 'twin', 'summary': 'A planted entry.', 'game': game, 'ours': [row], 'open': []}
        write(root, 'db/seams/twin.json', json.dumps(ent, indent=1))
        git(root, 'add', '-A')
        git(root, 'commit', '-q', '-m', 'c0')
        return root, git(root, 'rev-parse', 'HEAD')

    def seam_row(root, base):
        r = subprocess.run([sys.executable, '-I', chk, '--only', 'seam', '--root', root, '--base', base], capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        return r.returncode, r.stdout + r.stderr
    base_text = '// a\nint x = 1;\nint y = 2;\nint z = 3;\n'
    root, base = repo('g1', base_text)
    write(root, 'src/a.cpp', '// a\nint x = 1;\nint z = 3;\n')
    rc, out = seam_row(root, base)
    check(rc == 1 and 'seam' in out and 'STALE' in out and 'src/a.cpp' in out, 'g: check.py seam row: a diff that deletes the anchored line FAILs', out)
    root, base = repo('g2', base_text)
    write(root, 'src/a.cpp', '// a\n// new\n' + base_text[5:])
    rc, out = seam_row(root, base)
    check(rc == 0 and 'none STALE' in out, 'g: a diff that only shifts the anchored line passes (the anchor locates the row)', out)
    root, base = repo('g3', base_text, entry_row_anchor='int gone = 0;')
    write(root, 'src/c.cpp', 'int c = 2;\n')
    rc, out = seam_row(root, base)
    check(rc == 0, 'g: a diff touching an unrelated file passes, a stale row of an untouched file is not judged', out)
    write(root, 'src/a.cpp', base_text + '// touched\n')
    rc, out = seam_row(root, base)
    check(rc == 1 and 'STALE' in out, 'g: the same stale row FAILs once its file is in the diff', out)
    # a row with an occurrence: a diff that inserts lines above both identical anchors passes
    twin_text = '// a\nvoid f() {\n  v[5] = 1;\n}\nvoid g() {\n  v[5] = 1;\n}\n'
    root, base = repo('g4', twin_text, entry_row_anchor='v[5] = 1;', extra={'occurrence': 2})
    write(root, 'src/a.cpp', '// top\n' + twin_text)
    rc, out = seam_row(root, base)
    check(rc == 0 and 'seam' in out, 'g: a diff that inserts a line above an anchor with an occurrence passes', out)
    # the same anchor on two lines with NO occurrence: the change that touches the file FAILs (AMBIGUOUS)
    root, base = repo('g4b', twin_text, entry_row_anchor='v[5] = 1;')
    write(root, 'src/a.cpp', '// top\n' + twin_text)
    rc, out = seam_row(root, base)
    check(rc == 1 and 'AMBIGUOUS' in out, 'g: an anchor on two lines without `occurrence` FAILs the seam row', out)
    # a row that stores `line` (the twin of the old format) FAILs
    root, base = repo('g4c', base_text, extra={'line': 3})
    write(root, 'src/a.cpp', base_text + '// touched\n')
    rc, out = seam_row(root, base)
    check(rc == 1 and 'stored `line`' in out, 'g: a row that stores a `line` FAILs the seam row', out)
    # a missing source note is the index's own problem (q seam --check reports it): it never fails a code change
    root, base = repo('g5', base_text, game=[dict(GOOD_GAME[0], source='docs/missing_note.md')])
    write(root, 'src/a.cpp', base_text + '// touched\n')
    rc, out = seam_row(root, base)
    check(rc == 0, 'g: a row of a game row whose source note is missing does not fail the code change', out)
    # an entry the change edits is judged whole: a hand-edited row whose anchor is not in its (untouched) file FAILs
    root, base = repo('g6', base_text)
    ent = qseam.load(os.path.join(root, 'db', 'seams', 'twin.json'))
    ent['ours'][0]['anchor'] = 'int gone = 0;'
    write(root, 'db/seams/twin.json', json.dumps(ent, indent=1))
    rc, out = seam_row(root, base)
    check(rc == 1 and 'STALE' in out, 'g: a change that edits a seam entry FAILs on a STALE row of it, even when the row\'s file is untouched', out)


def case_d(root):
    if os.path.normcase(root) != os.path.normcase(qseam.ROOT):
        print('skip d: --root is not the checkout holding the tool (q find / q fact answer for their own checkout)')
        return
    r = subprocess.run([sys.executable, '-I', QPY, 'find', 'depth'], capture_output=True, text=True, encoding='utf-8', errors='replace')
    check('seam entry: depth' in r.stdout and 'q seam depth' in r.stdout, 'd: q find depth prints the seam entry', r.stdout[-1500:] + r.stderr[-500:])
    r = subprocess.run([sys.executable, '-I', QPY, 'fact', 'viewport', 'reversed', 'depth', '--kind', 'seam'], capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    check(r.returncode == 0 and 'seams' in r.stdout and 'depth.json' in r.stdout, 'd: q fact --kind seam finds db/seams/depth.json',
          r.stdout[-1500:] + r.stderr[-500:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.abspath(os.path.join(HERE, '..', '..')))
    a = ap.parse_args()
    case_a(os.path.abspath(a.root))
    case_d(os.path.abspath(a.root))
    with tempfile.TemporaryDirectory(prefix='qseam_') as tmp:
        case_b(tmp)
        case_c(tmp)
        case_e(tmp)
        case_f(tmp)
        case_g(tmp)
    print(f'qseam selftest: {"FAIL " + str(len(FAILS)) if FAILS else "all passed"}')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
