r"""S111 UIPRDEC selftest: tools\menus\uipr.py against the game's reader rule and the player's own MnUIHD / MnUISD pacs.

    py -I tools\menus\uipr_selftest.py

1. The POF0 token rule (sub_82C42760) on a hand-made list, and the chunk rule (sub_82C42850: no POF0 after the body = no load).
2. Every key-0 UIPR leaf of MnUIHD.pac and MnUISD.pac decodes and passes the reader's invariants (uipr.check).
3. The planted twins fail: a page stride of 232 (the record is 236) and a POF0 two-byte token read as one byte.
4. The re-find (menus.md "The option screens", MENUNAMES 4, MENUTREE): in /MENU/UI06 (p7/0) page 24 is Menu/Options and
   page 41 Menu/Gameplay, whose item 3 (record 212, screen code 1300) targets page 72, a nameless root page; 62 of the 80
   pages are lists (kind 4, not a root); in p6/0 the page named Gameplay_Visual_Presentation builds sub_82B1B0C0 and
   has seven items (the frame's seven rows) on a node carrying the frame's sprite names.
5. Text element labels (S111 UIPRLABELS, sub_82B53DC0): a planted kind-2 element in id form (+216 = text id, +220 = -1) and one in
   string form (+220 = name-table index) give ('id', n) / ('str', i, string); the twin (offsets +216 / +220 swapped) must not. The
   re-find: in p6/0 the VISUAL PRESENTATION node's font000_Name00..06 are the ids 0xA100 0xA101 0x906 0xA102 0xA103 0xA131 0xA104
   (named CAMERA CUTS ... BRIGHTNESS CONTROL by the string tables) and VIBRATION's font000_Name00..03 are the strings 1P..4P.
Reads the player's files only; prints nothing of their contents beyond names and counts.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uipr  # noqa: E402

FAILS = []


def ok(cond, what):
    print('%s  %s' % ('PASS' if cond else 'FAIL', what))
    if not cond:
        FAILS.append(what)


def synthetic():
    pof = bytes([0x41, 0x80, 0x02, 0xC0, 0x00, 0x00, 0x03, 0x00, 0x00])
    ok(uipr.relocate(pof) == [4, 12, 24], 'POF0 tokens 01 / 10 / 11 / 00 give offsets 4, 12, 24')
    ok(uipr.relocate(pof, plant_width=True) != [4, 12, 24], 'planted width twin reads the same POF0 differently')
    body = b'UIPR' + b'\0' * 28
    leaf = b'\0\0\0\1' + struct.pack('>I', len(body)) + body
    try:
        uipr.load_chunk(leaf + b'POF0' + struct.pack('>I', 0))
        loaded = True
    except uipr.UiprError:
        loaded = False
    ok(loaded, 'an unknown first tag (value 0) is the body, POF0 after it loads')
    try:
        uipr.load_chunk(leaf + b'DATA' + struct.pack('>I', 0))
        refused = False
    except uipr.UiprError:
        refused = True
    ok(refused, 'no POF0 after the body: the load fails like sub_82C42850 returning 0')


def planted_text_leaf():
    """A made-up UIPR leaf: one node, two kind-2 elements: record +216 = 0xA100 / +220 = -1 (id form) and +216 = 5 /
    +220 = 2 (string form, the name 'Hello'); names txtId, txtStr, Hello."""
    names = b'txtId\0txtStr\0Hello\0'
    body = bytearray(b'UIPR' + struct.pack('>I', 111) + b'\0' * 24)
    H = len(body)
    body += b'UIPR' + b'\0' * 96
    P = len(body)
    body += struct.pack('>IIII', 3, 0, P + 16, P + 28)
    body += struct.pack('>III', 0, 6, 13)
    body += names
    body += b'\0' * (-len(body) % 4)
    node_at = len(body)
    body += b'\0' * 96
    el_at = len(body)
    for ni, tid, ts in ((0, 0xA100, -1), (1, 5, 2)):
        rec = bytearray(336)
        struct.pack_into('>i', rec, 0, ni)
        struct.pack_into('>i', rec, 16, 2)
        struct.pack_into('>i', rec, 216, tid)
        struct.pack_into('>i', rec, 220, ts)
        body += rec
    struct.pack_into('>i', body, H + 16, 1)
    struct.pack_into('>I', body, H + 40, node_at)
    struct.pack_into('>I', body, H + 48, P)
    struct.pack_into('>i', body, node_at + 12, 2)
    struct.pack_into('>I', body, node_at + 64, el_at)
    return b'\0\0\0\1' + struct.pack('>I', len(body)) + bytes(body) + b'POF0' + struct.pack('>I', 0)


def labels():
    texts = {0xA100: 'LABEL'}
    got = []
    for plant in (None, 'offsets'):
        lay = uipr.Layout(planted_text_leaf(), plant)
        got.append([lay.label(e, texts.get) for e in lay.nodes[0]['elements']])
    ok(got[0] == [('id', 0xA100, 'LABEL'), ('str', 2, 'Hello')],
       'planted text elements: id form names the text id, string form the name-table string')
    ok(got[1] != got[0], 'planted twin (+216 / +220 swapped) gives other labels')


def leaves(name):
    path = os.path.join(uipr.pac.GAME, 'pac', 'menu', name)
    if not os.path.isfile(path):
        ok(False, 'the player file %s is present' % name)
        return []
    return list(uipr.leaves_of_pac(path))


def main():
    synthetic()
    labels()
    hd = leaves('MnUIHD.pac')
    sd = leaves('MnUISD.pac')
    for pac_name, ls in (('MnUIHD', hd), ('MnUISD', sd)):
        bad = []
        for n, leaf in ls:
            try:
                f = uipr.check(uipr.Layout(leaf))
            except (uipr.UiprError, struct.error, IndexError) as e:
                f = [str(e)]
            if f:
                bad.append('%s: %s' % (n, f[0]))
        ok(len(ls) >= 10 and not bad, '%s: %d UIPR leaves decode and pass the reader invariants%s'
           % (pac_name, len(ls), (' (' + '; '.join(bad[:3]) + ')') if bad else ''))
    by = dict(hd)
    for plant in ('stride', 'width'):
        caught = []
        for n in ('p6/0', 'p7/0'):
            try:
                f = uipr.check(uipr.Layout(by[n], plant))
            except (uipr.UiprError, struct.error, IndexError) as e:
                f = [str(e)]
            caught.append(bool(f))
        ok(all(caught), 'planted twin %s fails the check on p6/0 and p7/0' % plant)
    ui06 = uipr.Layout(by['p7/0'])
    pg = ui06.pages
    ok(pg[24]['name'] == 'Menu/Options' and pg[41]['name'] == 'Menu/Gameplay', 'p7/0 page 24 Menu/Options, page 41 Menu/Gameplay')
    it = ui06.item_nth(pg[41], 3)
    ok(it is not None and it['target_page'] == 72 and it['flow']['screen_code'] == 1300 and it['flow']['record_id'] == 212,
       'GAMEPLAY item 3: record 212, screen code 1300, target page 72')
    ok(pg[72]['root'] == 1 and pg[72]['name'] is None, 'page 72 is a nameless root page')
    lk = uipr.link_of(pg[72], uipr.Links(hd))
    ok(lk is not None and (lk['layout'], lk['page'], lk['leaf'], lk['rec152'], lk['flag160']) == (5, 15, 'p6/0', 0, 1)
       and lk['page_name'] == 'ChangeObserver_Visual' and lk['items'][0]['name'] == 'Bright'
       and lk['items'][0]['target_name'] == 'Gameplay_Visual_Presentation',
       'M100_Q6: code 1300 -> UI06 page 72 -> (5, 15) ChangeObserver_Visual -> item Bright -> p6 page 14 Gameplay_Visual_Presentation')
    ok(uipr.link_of(pg[41], uipr.Links(hd)) is None and uipr.link_of(pg[72], None)['page'] == 15,
       'a non-root page has no link; the link words print without the other layout')
    ok(ui06.find_record(212) == (41, 3), 'sub_82B43C18 finds record 212 at page 41 item 3')
    ok(sum(1 for p in pg if p['kind'] == 4 and p['root'] == 0) == 62 and len(pg) == 80, '80 pages, 62 lists (MENUTREE)')
    opt = uipr.Layout(by['p6/0'])
    vp = [p for p in opt.pages if p['name'] == 'Gameplay_Visual_Presentation']
    ok(len(vp) == 1 and vp[0]['class'].startswith('sub_82B1B0C0') and vp[0]['item_count'] == 7,
       'p6/0 Gameplay_Visual_Presentation -> sub_82B1B0C0, 7 items%s' % (' (page %d)' % vp[0]['index'] if vp else ''))
    if vp:
        names = {e['name'] for e in opt.nodes[vp[0]['node']]['elements']}
        want = {'font000_Name%02d' % i for i in range(7)} | {'corsor_set%02d' % i for i in range(7)} | {'gauge_set06'}
        ok(want <= names, 'its node carries font000_Name00..06, corsor_set00..06, gauge_set06')
        rows = {e['name']: opt.label(e) for e in opt.nodes[vp[0]['node']]['elements'] if e['name'].startswith('font000_Name')}
        ids = [rows['font000_Name%02d' % i][1] for i in range(7)]
        ok(ids == [0xA100, 0xA101, 0x906, 0xA102, 0xA103, 0xA131, 0xA104] and rows['font000_Name00'][2] == 'CAMERA CUTS'
           and rows['font000_Name06'][2] == 'BRIGHTNESS CONTROL', 'VISUAL PRESENTATION labels: ids 0xA100..0xA104 named by the text tables')
    vib = [p for p in opt.pages if p['name'] == 'Option_Vibration']
    if vib:
        rows = {e['name']: opt.label(e) for e in opt.nodes[vib[0]['node']]['elements'] if e['name'].startswith('font000_Name')}
        ok([rows['font000_Name%02d' % i] for i in range(4)] == [('str', 97 + i, '%dP' % (i + 1)) for i in range(4)],
           'VIBRATION labels are the literal strings 1P..4P (name-table indices 97..100)')
    else:
        ok(False, 'p6/0 has Option_Vibration')
    print('uipr selftest: %d failed' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
