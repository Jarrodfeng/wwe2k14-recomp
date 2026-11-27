r"""S111 YXMLDEC selftest: tools\menus\yxmlbin.py against the game's reader rule (sub_823C59E8), planted twins first seen to fail.

    py -I tools\menus\yxmlbin_selftest.py [--no-game]

1. Synthetic leaves (written here, no game bytes): a version-3 leaf decodes to the fields the reader stores; each reader
   condition (magic, counts, group sum, element overrun, short leaf, a 16-byte name, a digit in header byte 15) is refused;
   a version-2 leaf takes its second name by cutting the first out of the 32-byte name (sub_82C3E518 / sub_82C3E2D0).
2. Twins (the decoder with ONE constant wrong): a 2-byte group element count, a 60-byte record name (every later offset
   shifted), the name read 4 bytes early. Each must FAIL on the synthetic leaf and on the player's MnLayHD p13/7000.
3. The player's files (read-only; skipped with --no-game or when game_data is absent, and then said so): every YXMLBIN
   leaf of the 16 pac\menu pacs decodes, each read whole to its last byte, every element link inside its record table.
4. Re-finds of lodged [C] claims: docs\formats\select_layout.md (claims 3, section 3: MnLayHD p13/7000 counts, the
   Draw_charasen0k records -> groups 33 + k, groups 7..10 at elements 19 / 52 / 85 / 118, CharSet_02L / R / C / CC at
   6 / 7 / 8 / 9 of group 33, the bar pieces' native sizes and centre anchor) and MENULOAD's sub_8281CC48 claim (the loading
   pac's layouts are its key-2 and key-5 leaves: sub_8281CAB0 / sub_8281C928 parse exactly those keys).
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import yxmlbin as Y  # noqa: E402

FAILS = []
MENU_PACS = ['CAE.pac', 'fontHD.pac', 'fontSD.pac', 'LoadHD.pac', 'LoadSD.pac', 'MatchHD.pac', 'MatchSD.pac', 'menu.pac',
             'menuHD.pac', 'menuSD.pac', 'MnLayHD.pac', 'MnLaySD.pac', 'MnUIHD.pac', 'MnUISD.pac', 'mvMot.pac', 'sas.pac']


def ok(cond, what):
    print('  %s %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        FAILS.append(what)


def refused(data, what, rule=Y.GAME):
    try:
        Y.parse(data, rule)
    except Y.YxmlError as ex:
        ok(True, '%s refused (%s)' % (what, ex))
        return
    ok(False, '%s refused (it decoded)' % what)


def name(s, n):
    b = s.encode('latin-1')
    return b + b'\0' * (n - len(b))


def element(rec_id, link, nm, x=1.5, y=-2.0, w=16.0, h=8.0, anchor=4, text=0, version=3, full2='', n2='', ext_words=(7, 8, 9, 10),
            filt=(0, 3, 0, 5), w116=-1):
    words = struct.pack('<I6fI6fIfiII', rec_id, x, y, 1.0, 1.0, w, h, anchor, 0.0, 0.0, 0.25, 0.5, 0.75, 1.0, 2, 0.0, link,
                        1 if text else 0, text)
    b = words + name(nm, 16)
    if version == 2:
        b += name(full2, 32) + struct.pack('<4I', *ext_words) + bytes(filt)
    elif version >= 3:
        b += name(n2, 16) + struct.pack('<4I', *ext_words) + bytes(filt) + struct.pack('<i', w116)
    return b


def leaf(version=3, groups=None, records=None, tail=(0, 40356), nel=None, ngrp=None, head=None):
    records = records if records is not None else [(0, 0, 0, 'start'), (1000, 1, 0, 'font000'), (3000, 3, 0, 'pic'),
                                                   (11000, 11, 1, 'scene')]
    groups = groups if groups is not None else [[element(0, 0, 'root', n2='')],
                                                [element(1000, 1, 'font000', n2='_ttl', text=1),
                                                 element(3000, 2, 'pic', n2='_L', w116=7)]]
    out = head if head is not None else name('YXMLBIN VER%04d' % version, 16)
    nel = nel if nel is not None else sum(len(g) for g in groups)
    out += struct.pack('<4I', len(records), nel, ngrp if ngrp is not None else len(groups), len(tail))
    for rid, kind, grp, nm in records:
        out += struct.pack('<3I', rid, kind, grp) + (name(nm, 64) if version >= 2 else b'')
    for g in groups:
        out += struct.pack('<I', len(g)) + b''.join(g)
    return out + struct.pack('<%dI' % len(tail), *tail)


TWINS = [('a 2-byte group element count', Y.Rule(count_width=2)),
         ('a 60-byte record name (later offsets shifted)', Y.Rule(record_name=60)),
         ('the element name read at byte 72', Y.Rule(elem_name_at=72))]


def twin_fails(data, rule, expect_names=None):
    """A twin FAILS when the reader's conditions refuse it, or (decoded) when its names differ from the game rule's."""
    try:
        lay = Y.parse(data, rule)
    except Y.YxmlError:
        return True
    return expect_names is not None and [e['name'] for e in lay['elements']] != expect_names


def synthetic():
    print('1. synthetic leaves')
    data = leaf()
    lay = Y.parse(data)
    els = lay['elements']
    ok(lay['version'] == 3 and lay['counts'] == {'records': 4, 'elements': 3, 'groups': 2, 'tail': 2}, 'header: version 3, counts 4 / 3 / 2 / 2')
    ok(lay['consumed'] == len(data), 'read to the last byte (%d of %d)' % (lay['consumed'], len(data)))
    ok([g['first'] for g in lay['groups']] == [0, 1] and [g['count'] for g in lay['groups']] == [1, 2], 'groups: first 0 / 1, counts 1 / 2')
    ok([g['record'] for g in lay['groups']] == [0, 3], 'group -> the first record whose group word is its index (0 -> 0, 1 -> 3)')
    ok([e['+188'] for e in els] == [0, 11000, 11000], 'element +188 = that record\'s id')
    e = els[1]
    ok((e['name'], e['name2'], e['link'], e['x'], e['y'], e['w'], e['h'], e['anchor']) == ('font000', '_ttl', 1, 1.5, -2.0, 16.0, 8.0, 4),
       'element 1: names, link, x / y, size, anchor byte')
    ok(e['colour'] == [0.25, 0.5, 0.75, 1.0] and e['+53'] == 2 and e['+64'] == 1 and e['skipped_word17'] == 1, 'element 1: colour words, +53, +64, word 17')
    ok(e['+104'] == [7, 8, 9, 10] and e['+112'] == [0, 3, 0, 5] and els[2]['+116'] == 7, 'element v3 extension: +104..+110, +112..+115, +116')
    ok(lay['tail'] == [0, 40356], 'tail words')
    print('   reader conditions')
    refused(b'YXMLBIN VAR0003\0' + data[16:], 'a leaf without "YXMLBIN VER"')
    refused(leaf(nel=4), 'the groups hold fewer elements than the header (4)')
    refused(leaf(nel=2), 'a group running past the element count (2)')
    refused(leaf(ngrp=3), 'fewer groups than the header (3)')
    refused(data[:-1], 'a leaf one byte short')
    refused(leaf(groups=[[element(0, 0, 'x' * 16)]], records=[(0, 0, 0, 'start')]), 'a 16-byte element name (left unwritten by the reader)')
    refused(leaf(head=b'YXMLBIN VER00031'), 'a digit in header byte 15 (strtol reads past the copy)')
    v2 = leaf(version=2, groups=[[element(0, 0, 'root', version=2, full2='rootbar')], [element(1000, 1, 'font000', version=2, full2='font000_ttl'),
                                                                                       element(3000, 2, 'pic', version=2, full2='pic')]])
    lay2 = Y.parse(v2)
    ok([e['name2'] for e in lay2['elements']] == ['bar', '_ttl', ''] and lay2['elements'][0]['+116'] == -1,
       'version 2: second name = the 32-byte name with the first cut out; +116 = -1')
    v1 = leaf(version=1, groups=[[element(0, 0, 'root', version=1)], [element(1000, 1, 'a', version=1), element(3000, 2, 'b', version=1)]])
    lay1 = Y.parse(v1)
    ok(lay1['records'][1]['name'] == '' and lay1['elements'][1]['+104'] == [0, 0, 0, 0] and lay1['consumed'] == len(v1),
       'version 1: 12-byte records, 92-byte elements, extension zeroed')
    print('2. twins on the synthetic leaf (each must FAIL)')
    names = [e['name'] for e in els]
    for what, rule in TWINS:
        ok(twin_fails(data, rule, names), 'twin fails: %s' % what)


def game():
    try:
        pac = Y.pac_module()
    except SystemExit as ex:
        print('3. SKIPPED: %s' % ex)
        return False
    if not os.path.isdir(os.path.join(pac.GAME, 'pac', 'menu')):
        print('3. SKIPPED: no game_data\\extracted\\pac\\menu on this checkout')
        return False
    print('3. the player\'s pac\\menu pacs')
    total = bad = whole = inrange = 0
    loading_keys = set()
    p13 = None
    for nm in MENU_PACS:
        for leafname, dec in Y.pac_leaves(nm):
            total += 1
            try:
                lay = Y.parse(dec)
            except Y.YxmlError as ex:
                bad += 1
                print('  FAIL %s %s: %s' % (nm, leafname, ex))
                continue
            x = Y.check(lay)
            whole += x['trailing_bytes'] == 0
            inrange += x['links_out_of_range'] == 0
            if nm.startswith('Load'):
                loading_keys.add(leafname.split('/')[-1])
            if nm == 'MnLayHD.pac' and leafname == 'p13/7000':
                p13 = (dec, lay)
    ok(total > 0 and bad == 0, '%d YXMLBIN leaves decode, %d refused' % (total, bad))
    ok(whole == total - bad, 'every leaf read to its last byte (%d of %d)' % (whole, total - bad))
    ok(inrange == total - bad, 'every element link inside its record table (%d of %d)' % (inrange, total - bad))
    print('4. re-finds')
    ok(loading_keys == {'2', '5'}, 'loading pacs: the layouts are exactly the key-2 and key-5 leaves (%s): the keys sub_8281CAB0 / sub_8281C928 parse'
       % ' '.join(sorted(loading_keys)))
    if p13 is None:
        ok(False, 'MnLayHD p13/7000 found')
        return True
    dec, lay = p13
    c = lay['counts']
    ok((c['records'], c['elements'], c['groups']) == (148, 1285, 82), 'p13/7000: 148 records, 1285 elements, 82 groups (select_layout.md claim 3)')
    rec, grp, els = lay['records'], lay['groups'], lay['elements']
    ok(all(rec[119 + k]['name'] == 'Draw_charasen0%d' % k and rec[119 + k]['group'] == 33 + k for k in range(6)),
       'records 119 + k = Draw_charasen0k owning group 33 + k (the table 0x82038480 pairs)')
    ok([grp[g]['first'] for g in (7, 8, 9, 10)] == [19, 52, 85, 118], 'groups 7 / 8 / 9 / 10 start at elements 19 / 52 / 85 / 118')
    g33 = grp[33]
    ok(g33['count'] == 31 and [els[g33['first'] + i]['name'] for i in (6, 7, 8, 9)] == ['CharSet_02L', 'CharSet_02R', 'CharSet_02C', 'CharSet_02CC'],
       'group 33: 31 elements, 6 / 7 / 8 / 9 = CharSet_02L / R / C / CC (the +37020 values)')
    bar = {(e['name'], e['name2']): e for e in els[19:52]}
    sizes = [(bar[('panel_waku01', s)]['w'], bar[('panel_waku01', s)]['h'], bar[('panel_waku01', s)]['anchor']) for s in ('_L', '_C', '_R')] \
        if all(('panel_waku01', s) in bar for s in ('_L', '_C', '_R')) else None
    ok(sizes == [(16.0, 16.0, 4), (160.0, 16.0, 4), (16.0, 16.0, 4)], 'bar pieces panel_waku01 _L / _C / _R: 16 x 16, 160 x 16, 16 x 16, anchor 4')
    names = [e['name'] for e in els]
    print('   twins on the player\'s p13/7000 (each must FAIL)')
    for what, rule in TWINS:
        ok(twin_fails(dec, rule, names), 'twin fails: %s' % what)
    return True


def main():
    synthetic()
    ran = False if '--no-game' in sys.argv else game()
    if not ran:
        print('(the player\'s files were not read: parts 3 and 4 did not run)')
    print('yxmlbin_selftest: %s, %d failed%s' % ('FAIL' if FAILS else 'PASS', len(FAILS), '' if ran else ' (synthetic only)'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
