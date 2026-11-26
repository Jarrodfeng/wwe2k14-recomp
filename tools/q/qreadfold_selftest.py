"""Selftest of tools/q/qreadfold.py (S111 FOLD): the three folds on temp databases, never the real read.s2.db.

    py -I tools/q/qreadfold_selftest.py
"""
import json
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qread  # noqa: E402
import qreadfold as F  # noqa: E402

fails = []


def check(cond, what):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        fails.append(what)


def ur(c, unit, conf, model='opus-low', batch='b', system=''):
    return c.execute('insert into unit_read(unit_id,purpose,confidence,because,batch,model,when_,system) values (?,?,?,?,?,?,?,?)',
                     (unit, 'p%d' % unit, conf, 'x', batch, model, 't', system)).lastrowid


def nr(c, func, name, ev, conf, unit, batch='b'):
    return c.execute('insert into name_read(func,name,evidence,confidence,unit_id,batch,model) values (?,?,?,?,?,?,?)',
                     (func, name, ev, conf, unit, batch, 'm')).lastrowid


def main():
    tmp = tempfile.mkdtemp(prefix='qfold_t')

    # ---- 3. verdicts: the separate table is folded onto the readings and dropped
    db = os.path.join(tmp, 'a', 'read.s2.db')
    c = qread.open_rw(db)
    u1, u2 = ur(c, 1, 'low'), ur(c, 2, 'low')
    n1 = nr(c, 0x100, 'one_name', 'ev', 'medium', 1)
    c.executescript('create table verify_read(id integer primary key autoincrement, target_kind text, target integer, verdict text, note text,'
                    ' source text, when_ text, read_id integer);')
    ins = 'insert into verify_read(target_kind,target,verdict,note,source,when_,read_id) values (?,?,?,?,?,?,?)'
    c.execute(ins, ('unit', 1, 'partly', 'first', 'docs/A.md [C]', 'w1', u1))
    c.execute(ins, ('unit', 1, 'confirmed', 'second on the same reading', 'docs/A.md', 'w2', u1))
    c.execute(ins, ('func', 0x100, 'refuted', 'name is wrong', 'docs/B.md [C]', 'w3', n1))
    c.execute(ins, ('func', 0x200, 'confirmed', 'a function with no reader name', 'docs/C.md', 'w4', None))
    c.commit()
    folded, replaced, orphans = F.fold_verdicts(c)
    c.commit()
    check(folded == 3 and len(replaced) == 1 and len(orphans) == 1 and orphans[0][1] == 0x200, 'verdicts: 3 folded (1 replaced an earlier one), the verdict pinned to no reading is returned (%s %s %s)' % (folded, replaced, orphans))
    check(replaced == [('unit', 1, 'partly', 'first', 'docs/A.md [C]', 'w1')], 'verdicts: the replaced earlier verdict is returned whole (it is written to a history file, not lost) (%s)' % replaced)
    check(c.execute("select count(*) from sqlite_master where name like 'verify_read%'").fetchone()[0] == 0, 'verdicts: the separate table (and its index) is gone')
    check(c.execute('select verdict,v_note,v_source,v_when from unit_read where id=?', (u1,)).fetchone() == ('confirmed', 'second on the same reading', 'docs/A.md', 'w2')
          and c.execute('select verdict,v_when from name_read where id=?', (n1,)).fetchone() == ('refuted', 'w3')
          and c.execute('select verdict from unit_read where id=?', (u2,)).fetchone()[0] is None, 'verdicts: each landed on the reading it was pinned to, the others have none')
    check(F.fold_verdicts(c) == (0, [], []), 'verdicts: a store already folded is a no-op')
    c.close()

    # ---- 1. names: only two independent agreeing proposals reach the db; the rest are dropped
    dbdir = os.path.join(tmp, 'dbdir')
    os.makedirs(dbdir)
    json.dump([{'address': '0x00000500', 'name': 'already_named', 'summary': 's', 'evidence': ['e'], 'confidence': 'medium', 'system': 'other', 'source': 'x'},
               {'address': '0x00000999', 'name': 'name_taken_elsewhere', 'summary': 's', 'evidence': ['e'], 'confidence': 'medium', 'system': 'other', 'source': 'x'}],
              open(os.path.join(dbdir, 'x.json'), 'w'))
    db = os.path.join(tmp, 'b', 'read.s2.db')
    c = qread.open_rw(db)
    ur(c, 10, 'medium', batch='B1', system='ui')
    nr(c, 0x1000, 'only_label', 'ev a', 'medium', 10, 'B1')                       # single label
    nr(c, 0x1100, 'two_labels', 'ev one', 'medium', 10, 'B1')                     # same unit, same batch, different evidence: independent
    nr(c, 0x1100, 'two_labels', 'ev two', 'high', 10, 'B1')
    nr(c, 0x1200, 'stored_twice', 'same text', 'medium', 10, 'B1')                # one answer stored twice: not two labels
    nr(c, 0x1200, 'stored_twice', 'same text', 'medium', 10, 'B1')
    nr(c, 0x1300, 'reread_agrees', 'first', 'medium', 10, 'B1')                   # a later batch was shown the first name by the sheet
    nr(c, 0x1300, 'reread_agrees', 'second', 'medium', 10, 'B2')
    nr(c, 0x1400, 'disagree_a', 'e1', 'medium', 10, 'B1')                         # two labels that do not agree
    nr(c, 0x1400, 'disagree_b', 'e2', 'medium', 10, 'B1')
    nr(c, 0x500, 'already_named_x', 'e1', 'medium', 10, 'B1')                     # two labels, but the db names the address: a conflict, not an overwrite
    nr(c, 0x500, 'already_named_x', 'e2', 'medium', 10, 'B1')
    nr(c, 0x1500, 'name_taken_elsewhere', 'e1', 'medium', 10, 'B1')               # the name is the db's for another address: a conflict
    nr(c, 0x1500, 'name_taken_elsewhere', 'e2', 'medium', 10, 'B1')
    nr(c, 0x1600, 'low_pair', 'e1', 'low', 10, 'B1')                              # two agreeing labels, one low: no evidence above low
    nr(c, 0x1600, 'low_pair', 'e2', 'medium', 10, 'B1')
    judged = nr(c, 0x1700, 'judged_name', 'e1', 'medium', 10, 'B1')               # single, but a probe judged it: kept with its verdict
    c.execute("update name_read set verdict='confirmed', v_note='n', v_source='docs/A.md', v_when='w' where id=?", (judged,))
    c.execute("create table claim_read(id integer primary key autoincrement, read_kind text, read_id integer, target integer, kind text, claim text, verdict text, fact text, when_ text)")
    for rid in [r[0] for r in c.execute('select id from name_read')]:
        c.execute("insert into claim_read(read_kind,read_id) values ('name',?)", (rid,))
    c.execute("insert into claim_read(read_kind,read_id) values ('unit',1)")
    c.commit()
    total = c.execute('select count(*) from name_read').fetchone()[0]
    r = F.fold_names(c, dbdir)
    c.commit()
    check([e['name'] for e in r['promoted']] == ['two_labels'], 'names: only the independent agreeing pair is promoted (%s)' % [e['name'] for e in r['promoted']])
    e = r['promoted'][0]
    check(e['address'] == '0x00001100' and e['confidence'] == 'medium' and e['system'] == 'ui' and e['evidence'][-1] == 'labels: A two_labels; B two_labels (agree)'
          and set(e) == {'address', 'name', 'summary', 'evidence', 'confidence', 'system', 'source'},
          'names: the entry has the db fields and the labels: A ..; B .. item the validator reads (%s)' % e)
    check(sorted(x[1] for x in r['conflicts']) == ['already_named_x', 'name_taken_elsewhere'], 'names: a db-named address and a taken name are conflicts, not written (%s)' % r['conflicts'])
    check([x[1] for x in r['low']] == ['low_pair'], 'names: two agreeing labels with a low one are not promoted')
    left = [x[0] for x in c.execute('select name from name_read')]
    check(left == ['judged_name'] and r['dropped'] == total - 1 and r['kept'] == 1,
          'names: every other proposal (single label, stored twice, re-read, disagreeing) is dropped; the one a probe judged is kept (%s)' % left)
    check(c.execute("select count(*) from claim_read where read_kind='name'").fetchone()[0] == 1 and c.execute("select count(*) from claim_read where read_kind='unit'").fetchone()[0] == 1
          and r['claims'] == total - 1, 'names: the claim rows of the dropped proposals go with them, unit claims stay')
    out = os.path.join(tmp, 'reader_names.json')
    F.write_entries(out, r['promoted'])
    F.write_entries(out, r['promoted'])
    check(len(json.load(open(out))) == 1, 'names: writing the entries twice does not duplicate one')
    c.close()

    # ---- 2. confidence: a listing reading's confidence from its neighbour lines goes back to the unit's own value
    db = os.path.join(tmp, 'c', 'read.s2.db')
    c = qread.open_rw(db)
    a1 = ur(c, 1, 'low')
    a2 = ur(c, 1, 'high', model='opus-low-listing')            # raised on a sheet with neighbour lines
    b1 = ur(c, 2, 'low')
    b2 = ur(c, 2, 'medium', model='opus-low-listing')          # raised, but unit 2 had no neighbour lines: its own listing evidence
    d1 = ur(c, 3, 'high')
    d2 = ur(c, 3, 'medium', model='opus-low-listing')          # lowered by the reader: untouched
    e1 = ur(c, 4, 'medium', model='opus-low-listing')          # no earlier reading to go back to: untouched
    f1 = ur(c, 5, 'low')
    f2 = ur(c, 5, 'medium', model='opus-low-listing')          # listing, with neighbours
    f3 = ur(c, 5, 'high', model='opus-low-listing12')          # a second listing pass: still only the neighbour-free value counts
    g1 = ur(c, 6, 'low')
    g2 = ur(c, 6, 'medium', model='opus-low-listing', batch='L01')      # a listing pass whose OWN sheet had no neighbour lines (S110 listing1)
    g3 = ur(c, 6, 'medium', model='opus-low-listing', batch='L02')      # the next pass, with neighbour lines: its own value is g2's
    c.commit()
    ch, seen = F.fold_confidence(c, [a2, d2, e1, f2, f3, g3])            # the READINGS whose own sheet carried neighbour lines
    c.commit()
    conf = dict(c.execute('select id,confidence from unit_read'))
    check(conf[a2] == 'low' and conf[f2] == 'low' and conf[f3] == 'low', 'confidence: a reading raised on neighbour lines goes back to the value of the last reading without them')
    check(conf[b2] == 'medium' and conf[d2] == 'medium' and conf[e1] == 'medium' and conf[a1] == 'low' and conf[d1] == 'high',
          'confidence: no neighbour lines, a lowered one, no earlier reading, and first-pass readings are not touched')
    check(conf[g2] == 'medium' and conf[g3] == 'medium', 'confidence: a listing reading whose own sheet had no neighbour lines is never lowered, and it is the '
          'value a later neighbour reading of the unit goes back to (got %s %s)' % (conf[g2], conf[g3]))
    check(len(ch) == 3 and seen == 5 and sorted((x[2], x[3]) for x in ch) == [('high', 'low'), ('high', 'low'), ('medium', 'low')], 'confidence: 3 changed of 5 examined (%s %s)' % (ch, seen))
    n, a = F.confidence_counts(c)
    check(dict(n) == {'low': 2, 'medium': 4} and dict(a) == {'low': 7, 'medium': 5, 'high': 1}, 'confidence counts (newest per unit, all rows): %s %s' % (dict(n), dict(a)))

    # which readings had neighbour lines: read from each reading's OWN sheet file (batch + unit), not from the unit
    sh = os.path.join(tmp, 'sheets')
    os.makedirs(sh)
    open(os.path.join(sh, 'L02_6.txt'), 'w').write('# unit 6\n\n# NEIGHBOURS (EARLIER MODEL READINGS ...)\ncalled by unit 9 (x2) [ui, high]: p\n\n# LISTINGS\n')
    open(os.path.join(sh, 'L01_6.txt'), 'w').write('# unit 6\n\n# LISTINGS\n')

    def sheet_of(batch, unit):
        p = os.path.join(sh, '%s_%d.txt' % (batch, unit))
        return p if os.path.exists(p) else None
    # S111 FOLD2 (the coordinator's rule): only a reading whose OWN TEXT cites a neighbour of its sheet is lowered; one that cites only
    # its listing keeps the confidence the listing earned
    nb_sheet = '# unit %d\n\n# NEIGHBOURS (EARLIER MODEL READINGS ...)\ncalled by unit 9 (x2) [ui, high]: p\ncalls unit 12 (x1) [ui, low]: q\n\n# LISTINGS\n'
    for u in (20, 21, 22, 23):
        open(os.path.join(sh, 'P01_%d.txt' % u), 'w').write(nb_sheet % u)

    def urt(unit, conf, because, purpose='p'):
        return c.execute('insert into unit_read(unit_id,purpose,confidence,because,batch,model,when_) values (?,?,?,?,?,?,?)',
                         (unit, purpose, conf, because, 'P01', 'opus-low-listing', 't')).lastrowid
    k1 = urt(20, 'medium', '"neighbour 9 camera director calls it"; listing 82614D00 calls x')     # cites a neighbour (the word)
    k2 = urt(21, 'medium', 'listing 82EB6E48 copies n bytes; "check_bkhd(m)" callers')               # listing only
    k3 = urt(22, 'medium', 'divide by 60, cap 99 and 59', purpose='the HUD clock; unit 12 reads it')  # cites a listed neighbour by unit id
    k4 = urt(23, 'medium', 'like unit 777 (not listed on this sheet); listing 82000000')         # a unit id that is NOT on its sheet
    c.commit()
    rows, shown, missing = F.rows_with_neighbours(c, sheet_of)
    check(shown == {g3, k1, k2, k3, k4} and sorted(m[0] for m in missing) == sorted([a2, b2, d2, e1, f2, f3]),
          'neighbour rows: the readings whose own sheet has a neighbour line; listing readings with no sheet on disk are reported (%s %s)' % (shown, missing))
    check(rows == {k1, k3}, 'neighbour rows: of those, only the readings whose own text cites a neighbour of their sheet (the word, or a listed '
          'unit id); listing-only and an unlisted unit id do not count (%s)' % rows)
    c.close()

    # ---- names, by the reading's own sheet: the later reading was not shown the earlier name -> independent (S111 FOLD2)
    db = os.path.join(tmp, 'e', 'read.s2.db')
    c = qread.open_rw(db)
    open(os.path.join(sh, 'L02_30.txt'), 'w').write('# unit 30\n82292DD0 400B 90i\n')                   # the later sheet does not show the name
    open(os.path.join(sh, 'P02_31.txt'), 'w').write('# unit 31\n82300000 40B ; read shown_name [r:medium]\n')   # it shows it
    open(os.path.join(sh, 'L01_30.txt'), 'w').write('# unit 30\n')
    open(os.path.join(sh, 'B10_32.txt'), 'w').write('# unit 32\n')
    nr(c, 0x82292DD0, 'setting_value_text', 'switch on +4', 'medium', 30, 'L01')
    nr(c, 0x82292DD0, 'setting_value_text', 'fallback string', 'high', 30, 'L02')
    nr(c, 0x82300000, 'shown_name', 'e1', 'medium', 31, 'B01')
    nr(c, 0x82300000, 'shown_name', 'e2', 'medium', 31, 'P02')
    nr(c, 0x82400000, 'stored_twice_b', 'same', 'medium', 32, 'B10')
    nr(c, 0x82400000, 'stored_twice_b', 'same', 'medium', 32, 'B10')
    c.commit()
    r = F.fold_names(c, dbdir, F.independent_by_sheet(sheet_of))
    check([e['name'] for e in r['promoted']] == ['setting_value_text'], 'names by sheet: only the pair whose later sheet did not show the name '
          '(one shown, one stored twice) (%s)' % [e['name'] for e in r['promoted']])
    e = r['promoted'][0] if r['promoted'] else {}
    check(e.get('confidence') == 'medium' and e.get('evidence', [''])[-1] == 'labels: A setting_value_text; B setting_value_text (agree)'
          and 'L01' in e.get('evidence', ['', ''])[0] and 'L02' in e.get('evidence', ['', ''])[1] and 'did not show' in e.get('summary', ''),
          'names by sheet: the entry names both readings and the labels item (%s)' % e)
    check([F.db_system(s) for s in ('menus', 'engine', 'ui', 'camera', 'material', 'unknown', '')] == ['ui', 'other', 'ui', 'camera', 'render', 'other', 'other'],
          'names: a reading system becomes a db system validate.py accepts (camera|render|match|assets|audio|io|ui|other)')
    c.close()

    # a fold already applied is corrected from the untouched backup: listing readings get their stored value back, nothing else moves
    bk = os.path.join(tmp, 'd', 'backup.db')
    c = qread.open_rw(bk)
    h1 = ur(c, 7, 'low')
    h2 = ur(c, 7, 'high', model='opus-low-listing')
    c.commit()
    c.close()
    db = os.path.join(tmp, 'd', 'read.s2.db')
    import shutil
    shutil.copy(bk, db)
    c = qread.open_rw(db)
    c.execute("update unit_read set confidence='medium' where id in (?,?)", (h1, h2))
    c.commit()
    n = F.restore_confidence(c, bk)
    conf = dict(c.execute('select id,confidence from unit_read'))
    check(n == 1 and conf[h2] == 'high' and conf[h1] == 'medium', 'restore: only listing readings take the backup value back (%s %s)' % (n, conf))
    c.close()
    print('%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
