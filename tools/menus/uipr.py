r"""S111 UIPRDEC: decode a UIPR menu layout leaf the way the game's own reader builds it (read-only, 0 game runs).

    py -I tools\menus\uipr.py FILE|--pac PAC [--leaf PATH] [--node NAME] [--items] [--elements] [--json] [--check]
                              [--find-record N] [--labels] [--plant stride|width|offsets]

FILE = one dumped leaf (`py -I tools\wm30\pac.py menu/MnUIHD.pac --dump out\...`); --pac PAC (a path, or a name under the
game's pac folder such as menu/MnUIHD.pac) decodes every PACH's key-0 leaf that is a UIPR layout (the game asks the PACH
for key 0: sub_82B4D548 -> sub_82C351E8(view, 0)); --leaf p6/0 keeps one of them. Output goes to stdout only: the
player's files are read, nothing is written (never commit a decoded dump).

The rule, ported from the listing (docs\formats\uipr.md has the layout; every function named is the game's):
  1. Chunk load, sub_82C42850 (called by the UIPR resource loader sub_82B4D548): the leaf is a chunk {tag, BE32 size, body};
     the tag is looked up in the 10-entry table at 0x8362D3C0 (sub_82C426B8: DATA 0, POF0 1, YOBJ 2, YANM 3, YATR 4, 'BPE ' 5,
     YMPC 6, YAT2 7, POFF 8, DUMY -1); a -1 tag is skipped, any other value (an unknown tag is 0) is the body. The body is
     copied, and the chunk after it must be POF0 (value 1), else the load fails (returns 0).
  2. Relocation, sub_82C42760: the POF0 bytes are tokens by their top two bits: 01 = one byte, delta (b & 0x3F) * 4;
     10 = two bytes BE, delta (v & 0x3FFF) * 4; 11 = four bytes BE, delta (v & 0x3FFFFFFF) * 4; 00 = skip one byte. A running
     address starts at the body and moves by each delta; the word there gets the body address added (an offset becomes a
     pointer). Here the words stay body offsets and the set of relocated offsets is kept (the checks use it).
  3. sub_82B4D548: version = body+4; below 107 the game reports an error (sub_82C34EF8); the header H = body+32 (100 bytes).
     Version < 111: every node's elements get +264 = -1; version < 109: every page record gets +116 = -1 (applied here too).
  4. Nodes, sub_82B3DB68: H+16 count, H+40 the 96-byte records; node i = sub_82B3E8F0(rec): rec+12 element count, rec+64
     the 336-byte element records (sub_82B42B58: element +0 name index, +16 kind 2..10 picks the class, else the name
     ScissorSet / ScissorEnd, else the base element sub_82B41D58).
  5. Pages, sub_82B43D00: H+20 count, H+44 the 236-byte records; page j = Init(page, layout, rec) (sub_82B556F8 ->
     sub_82B58290 -> sub_82B58808 -> sub_82B45B98): rec[0..192) copied to page+372 and page+564, so page+N = rec+(N-564):
     +0 name index (page+564), +12 node index (page+576, bounds-checked by sub_82B3D600), +16 root (page+580), +20 kind
     (page+584), +36 / +40 parent page / item (page+600 / +604), +44 a word count (page+1000), +48 item count (page+612 =
     page+764), +60 the page id (the id -> page map, below), +192 (page+1004), +196 the item array (page+760), +200 a 48-byte
     block or 0 (copied to page+952), +208 (page+768). Then the factory slot (sub_82B13AE0) builds the page's frame class
     from its NAME (sub_82B44CD0: the name table entry of page+564) and its kind (5 = sub_82B36AD8).
  6. The id map, sub_82B43D00 +1E4..+25C: map[i] = the first page j (index order) whose rec+60 == i, else -1.
  7. Names, sub_82B3DA28 (r3 = the scene, [scene+172] = H+48): P+0 count, P+8 offset array, P+12 string base; entry i =
     base + offsets[i] for 0 <= i < count, else none.
  8. Items, 80 bytes at page rec+196 (sub_82B45CE8: the Nth not-hidden item, +32 u16 bit 0 = hidden, all items when the
     page kind is 2); +28 target page; +48 the item's 48-byte flow data D (0 = none; D+0 = record id, the key sub_82B43C18
     finds a page by). D's words: docs\formats\flow_table.md.
  9. Text elements (kind 2, ctor sub_82B54210 -> sub_82B53DC0; S111 UIPRLABELS): sub_82B41D58 copies the 336-byte record to
     element +28 and +364, so element +N = record +(N-364). sub_82B53DC0 reads record +216 (element +580: the text id) and
     +220 (element +584: -1 = show GetString(+216), else the layout name-table string with index +220, through sub_82B53D70),
     record +176 (element +540: an index into the table sub_82B3D6D8 / sub_82B4CFF8 reach through [layout+20]; a hit with a tag
     > 0 builds a 176-byte struct, sub_82BBA870 / sub_82BBEBA0) and record +328 (element +692: when nonzero, sub_82B43908
     of it gives element +1268). --labels prints each kind-2 element's label: the text id named through the game's string
     tables (the q text reader, tools\q\qgx.py cmd_text) or the literal string.
"""
import argparse
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import pac  # noqa: E402  (finds the main checkout's game_data and unpacker from a worktree)

# sub_82C426B8's table at 0x8362D3C0 (q words 0x8362D3C0 20).
TAGS = {b'DATA': 0, b'POF0': 1, b'YOBJ': 2, b'YANM': 3, b'YATR': 4, b'BPE ': 5, b'YMPC': 6, b'YAT2': 7, b'POFF': 8,
        b'DUMY': -1}
VERSION_MIN = 107          # sub_82B434E0
VERSION_FIXUPS = 111       # sub_82B434D8 (nodes); pages use 109 (sub_82B4D548 +200)
HEAD = 32                  # sub_82B4D548 +130: H = body + 32
NODE = 96                  # sub_82B3DB68 +E8..+114
ELEMENT = 336              # sub_82B3E8F0 +A4
PAGE = 236                 # sub_82B43D00 +E0..+14C, +21C..+23C
ITEM = 80                  # sub_82B45CE8
FLOW = 48                  # docs\formats\flow_table.md (sub_834256E8)
TEXT_KIND = 2              # sub_82B42B58 kind 2 -> sub_82B54210 -> sub_82B53DC0 (the text element)
REC_TEXT_ID = 216          # sub_82B53DC0 +14..+5C: element +580 = record +216 (element +N = record +N-364)
REC_TEXT_STR = 220         # element +584 = record +220: -1 = GetString(+216), else the layout name-table index
REC_TEXT_AUX = 176         # element +540 (sub_82B3D6D8 index) [L meaning]
REC_TEXT_REF = 328         # element +692 (sub_82B43908 argument when nonzero) [L meaning]
# The project's layout order (the UISO leaf p0/0's entries, docs\research\s111\M100_Q6.md section 1 step 10): a root page's
# record +144 names one of these.
LAYOUT_ORDER = {3: 'p4/0', 4: 'p5/0', 5: 'p6/0', 6: 'p7/0'}

# The options factory sub_82B127D0: node name -> page ctor (docs\research\s111\MENUNAMES.md section 4, [C]).
OPTIONS = {}
for _names, _ctor in (
        (['ChangeObserver_SaveManager'], 'sub_82B21FB8(1)'),
        (['ChangeObserver_Sound', 'ChangeObserver_AI', 'ChangeObserver_Damage', 'ChangeObserver_General',
          'ChangeObserver_Vibratin'], 'sub_82B21FB8(0)'),
        (['ChangeObserver_ContType', 'ChangeObserver_TargetSettings', 'ChangeObserver_HUD', 'ChangeObserver_HPHUD',
          'ChangeObserver_SpeRefHUD'], 'sub_82B21FB8(2)'),
        (['ChangeObserver_InGameOpt'], 'sub_82B21FB8(4)'),
        (['ChangeObserver_Visual'], 'sub_82B21FB8(3)'),
        (['Option_SaveManager', 'Option_SaveManager_Delete'], 'sub_82B20CB0'),
        (['MatchOption_Sound'], 'sub_82B1FE28'),
        (['Option_Vibration'], 'sub_82B1A7B8'),
        (['MatchCreator_Observer'], 'sub_82B192D8'),
        (['MatchCreator_Environment', 'MatchCreator_Environment_NoR'], 'sub_82B17A00'),
        (['MatchCreator_WinCondition'], 'sub_82B16418'),
        (['MatchCreator_Rules'], 'sub_82B152A0'),
        (['Controls_ControlTypes4', 'Controls_ControlTypes6'], 'sub_82B1F5E8'),
        (['Controls_TargetSettings4', 'Controls_TargetSettings6'], 'sub_82B1E9A8'),
        (['MatchOptions_InGameOpt', 'MatchOptions_InGameOpt_NoW'], 'sub_82B1DE18'),
        (['MatchOptions_Balancing_AI'], 'sub_82B1D1C0'),
        (['MatchOptions_Balancing_Damage'], 'sub_82B1C770'),
        (['MatchOptions_Balancing_General'], 'sub_82B1BDC0'),
        (['Gameplay_Visual_Presentation', 'Gameplay_Visual_Presentation_NoB'], 'sub_82B1B0C0'),
        (['Gameplay_HUD4', 'Gameplay_HUD6'], 'sub_82B19DD8(0)'),
        (['Gameplay_HPHUD4', 'Gameplay_HPHUD6'], 'sub_82B19DD8(1)'),
        (['Gameplay_SpeRefHUD4', 'Gameplay_SpeRefHUD6'], 'sub_82B19DD8(2)')):
    for _n in _names:
        OPTIONS[_n] = _ctor
# sub_82B13AE0's own compares after its four sub-factories (strings read with q words).
NODE_FACTORY = {'UNLOCK_MENU': 'sub_82B11980', 'MusicJukeBoxMain': 'sub_82B390D8', 'SimpleSSSelect': 'sub_82B37958',
                'ArenaSelect': 'sub_82B37470', 'SoftwareKeyboard': 'sub_82B37300'}


class UiprError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def s32(b, o):
    return struct.unpack_from('>i', b, o)[0]


def u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def relocate(pof, plant_width=False):
    """sub_82C42760 over the POF0 bytes: the body offsets whose words the game turns into pointers (in order).
    plant_width (a planted twin): read the two-byte token as one byte."""
    out = []
    at = 0
    p = 0
    while p < len(pof):
        b = pof[p]
        top = b & 0xC0
        if top == 0x40:
            at += (b & 0x3F) * 4
            out.append(at)
            p += 1
        elif top == 0x80:
            if plant_width:
                at += (b & 0x3F) * 4
                p += 1
            else:
                at += (((b << 8) | pof[p + 1]) & 0x3FFF) * 4
                p += 2
            out.append(at)
        elif top == 0xC0:
            at += (struct.unpack_from('>I', pof, p)[0] & 0x3FFFFFFF) * 4
            out.append(at)
            p += 4
        else:
            p += 1
    return out


def load_chunk(leaf, plant_width=False):
    """sub_82C42850: (body bytes, relocated offsets, POF0 length) or UiprError when the game's load would return 0."""
    p = 0
    while True:
        if p + 8 > len(leaf):
            raise UiprError('no body chunk')
        if TAGS.get(bytes(leaf[p:p + 4]), 0) != -1:
            break
        p += 8 + u32(leaf, p + 4)
    size = u32(leaf, p + 4)
    body = bytearray(leaf[p + 8:p + 8 + size])
    q = p + 8 + size
    if len(body) != size or q + 8 > len(leaf) or TAGS.get(bytes(leaf[q:q + 4]), 0) != 1:
        raise UiprError('the chunk after the body is not POF0 (the game frees the copy and returns 0)')
    plen = u32(leaf, q + 4)
    return body, relocate(leaf[q + 8:q + 8 + plen], plant_width), plen


class Layout:
    """The decoded layout: header, nodes (with elements), pages (with items), names, id map."""

    def __init__(self, leaf, plant=None):
        self.plant = plant
        self.body, self.relocs, self.pof_len = load_chunk(leaf, plant_width=(plant == 'width'))
        self.reloc_set = set(self.relocs)
        b = self.body
        if len(b) < HEAD + 100 or b[0:4] != b'UIPR' or b[HEAD:HEAD + 4] != b'UIPR':
            raise UiprError('not a UIPR body')
        self.version = u32(b, 4)
        self.warnings = []
        if self.version < VERSION_MIN:
            self.warnings.append('version %d < %d: the game reports an error (sub_82C34EF8)' % (self.version, VERSION_MIN))
        H = HEAD
        self.H = H
        self.node_count = s32(b, H + 16)
        self.page_count = s32(b, H + 20)
        self.nodes_at = u32(b, H + 40)
        self.pages_at = u32(b, H + 44)
        self.names_at = u32(b, H + 48)
        self.other = [u32(b, H + 52), u32(b, H + 56), u32(b, H + 60)]
        page_stride = PAGE - 4 if plant == 'stride' else PAGE
        self.page_stride = page_stride
        self.names = self._names()
        self.nodes = []
        for i in range(max(self.node_count, 0)):
            r = self.nodes_at + i * NODE
            n = {'index': i, 'at': r, 'element_count': s32(b, r + 12), 'elements_at': u32(b, r + 64), 'elements': []}
            for j in range(max(n['element_count'], 0)):
                e = n['elements_at'] + j * ELEMENT
                if self.version < VERSION_FIXUPS:
                    struct.pack_into('>i', b, e + 264, -1)
                ni = s32(b, e)
                kind = s32(b, e + 16)
                o_id, o_str = (REC_TEXT_STR, REC_TEXT_ID) if plant == 'offsets' else (REC_TEXT_ID, REC_TEXT_STR)
                n['elements'].append({'index': j, 'at': e, 'name_index': ni, 'name': self.name(ni), 'kind': kind,
                                      'class': element_class(kind, self.name(ni)), 'text_id': s32(b, e + o_id),
                                      'text_str': s32(b, e + o_str), 'text_aux': s32(b, e + REC_TEXT_AUX),
                                      'text_ref': s32(b, e + REC_TEXT_REF)})
            self.nodes.append(n)
        self.pages = []
        for j in range(max(self.page_count, 0)):
            r = self.pages_at + j * page_stride
            if self.version < 109:
                struct.pack_into('>i', b, r + 116, -1)
            ni = s32(b, r)
            pg = {'index': j, 'at': r, 'name_index': ni, 'name': self.name(ni), 'node': s32(b, r + 12),
                  'root': s32(b, r + 16), 'kind': s32(b, r + 20), 'parent_page': s32(b, r + 36),
                  'parent_item': s32(b, r + 40), 'word_count': s32(b, r + 44), 'item_count': s32(b, r + 48),
                  'id': s32(b, r + 60), 'w192': u32(b, r + 192), 'items_at': u32(b, r + 196), 'block_at': u32(b, r + 200),
                  'w208': u32(b, r + 208), 'link_layout': s32(b, r + 144), 'link_page': s32(b, r + 148),
                  'link_152': s32(b, r + 152), 'link_flag': s32(b, r + 160)}
            pg['class'] = page_class(pg['kind'], pg['name'])
            pg['items'] = []
            for k in range(max(pg['item_count'], 0)):
                it = pg['items_at'] + k * ITEM
                d = u32(b, it + 48)
                item = {'index': k, 'at': it, 'target_page': s32(b, it + 28), 'hidden': u16(b, it + 32) & 1, 'flow_at': d,
                        'name_index': s32(b, it), 'name': self.name(s32(b, it))}
                if d:
                    item['flow'] = {'record_id': s32(b, d), 'label': s32(b, d + 4), 'unlock': s32(b, d + 12),
                                    'drop': u16(b, d + 20) & 1, 'screen_code': s32(b, d + 36), 'match_type': s32(b, d + 40)}
                pg['items'].append(item)
            self.pages.append(pg)
        # sub_82B43D00 +1E4..+25C: map[i] = first page whose +60 == i.
        self.id_map = []
        for i in range(max(self.page_count, 0)):
            self.id_map.append(next((p['index'] for p in self.pages if p['id'] == i), -1))

    def _names(self):
        b = self.body
        P = self.names_at
        if not P:
            return {'count': 0, 'offsets_at': 0, 'base_at': 0}
        return {'count': s32(b, P), 'offsets_at': u32(b, P + 8), 'base_at': u32(b, P + 12)}

    def name(self, i):
        """sub_82B3DA28: base + offsets[i] for 0 <= i < count, else None."""
        n = self.names
        if not n['offsets_at'] or not n['base_at'] or i < 0 or i >= n['count']:
            return None
        o = n['base_at'] + u32(self.body, n['offsets_at'] + 4 * i)
        if o >= len(self.body):
            return None
        e = self.body.find(b'\0', o)
        return self.body[o:e if e >= 0 else len(self.body)].decode('latin-1')

    def item_nth(self, page, n):
        """sub_82B45CE8: the Nth item of a page, hidden ones skipped unless the page kind is 2."""
        if n < 0 or n >= page['item_count']:
            return None
        if page['kind'] == 2:
            return page['items'][n] if n < len(page['items']) else None
        seen = 0
        for it in page['items']:
            if not it['hidden']:
                if seen == n:
                    return it
                seen += 1
        return None

    def find_record(self, key):
        """sub_82B43C18: the first (page, item) in page order whose indexed item's flow data has D+0 == key."""
        for p in self.pages:
            for n in range(p['item_count']):
                it = self.item_nth(p, n)
                if it and it['flow_at'] and it['flow']['record_id'] == key:
                    return p['index'], n
        return None

    def find_element(self, node, name):
        """sub_82B3E2C0 with start -1: the first element of a node whose name equals name."""
        for e in node['elements']:
            if e['name'] == name:
                return e['index']
        return -1

    def label(self, e, lookup=None):
        """sub_82B53DC0: a kind-2 element's label, ('id', n, text) when record +220 is -1 (GetString(+216)), else
        ('str', index, string) = the layout name-table string at +220 (sub_82B53D70). None for any other kind.
        lookup(id) -> the game's string for a text id (None = unnamed); default = the q text reader."""
        if e['kind'] != TEXT_KIND:
            return None
        if e['text_str'] == -1:
            return ('id', e['text_id'], (lookup or text_of)(e['text_id']))
        return ('str', e['text_str'], self.name(e['text_str']))


_TEXTS = {}


def text_of(tid):
    """The game's string for a text id: qgx.cmd_text (tools/q) (what `q text ID` prints), cached; None = not in the tables."""
    if tid not in _TEXTS:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'q'))
        import qgx  # noqa: E402
        h = qgx.cmd_text(tid) if tid > 0 else []
        _TEXTS[tid] = h[0]['text'] if h else None
    return _TEXTS[tid]


def page_class(kind, name):
    """sub_82B13AE0: kind 5 -> sub_82B36AD8; else the sub-factories by name (only the options table and the factory's
    own names are tabled here)."""
    if kind == 5:
        return 'sub_82B36AD8 (kind 5)'
    if name is None:
        return None
    if name in OPTIONS:
        return OPTIONS[name] + ' (options factory sub_82B127D0)'
    if name in NODE_FACTORY:
        return NODE_FACTORY[name] + ' (sub_82B13AE0)'
    return 'shop / match-up / MY WWE factories or the base frame sub_82B13CC8 (names not tabled here)'


def element_class(kind, name):
    """sub_82B42B58: kinds 2..10 by the jump table, else ScissorSet / ScissorEnd -> sub_82B51F50, else sub_82B41D58."""
    by_kind = {2: 'sub_82B54210', 3: 'sub_82B4C868', 4: 'sub_82B528A0', 5: 'sub_82B4CA08', 6: 'sub_82B525D8',
               7: 'sub_82B51F50', 8: 'sub_82B51F50', 9: 'sub_82B51E30', 10: 'sub_82B516D0'}
    if kind in by_kind:
        return by_kind[kind]
    if name in ('ScissorSet', 'ScissorEnd'):
        return 'sub_82B51F50'
    return 'sub_82B41D58'


def check(lay):
    """The reader's own invariants; returns a list of failures (empty = consistent)."""
    f = []
    b = lay.body
    rs = lay.reloc_set
    size = len(b)
    # POF0: every relocated word is aligned, inside the body and points inside the body (the game adds the base to it).
    for o in lay.relocs:
        if o % 4 or o + 4 > size:
            f.append('POF0 offset 0x%X outside the body or unaligned' % o)
            break
        if u32(b, o) >= size:
            f.append('relocated word at 0x%X = 0x%X does not point inside the body' % (o, u32(b, o)))
            break
    if len(rs) != len(lay.relocs):
        f.append('POF0 lists an offset twice')

    def ptr(o, what, null_ok):
        v = u32(b, o)
        if v == 0 and null_ok:
            return
        if o not in rs:
            f.append('%s at 0x%X (0x%X) is dereferenced by the reader but not relocated' % (what, o, v))

    def count(o, what):
        if o in rs:
            f.append('%s at 0x%X is a count but POF0 relocates it' % (what, o))

    H = lay.H
    count(H + 16, 'node count')
    count(H + 20, 'page count')
    ptr(H + 40, 'H+40 nodes', lay.node_count <= 0)
    ptr(H + 44, 'H+44 pages', lay.page_count <= 0)
    ptr(H + 48, 'H+48 names', False)
    if lay.names_at:
        count(lay.names_at, 'name count')
        ptr(lay.names_at + 8, 'name offsets', lay.names['count'] <= 0)
        ptr(lay.names_at + 12, 'name base', lay.names['count'] <= 0)
        for i in range(lay.names['count']):
            if lay.name(i) is None:
                f.append('name %d unreadable' % i)
                break
    if lay.nodes_at + max(lay.node_count, 0) * NODE > lay.pages_at and lay.page_count > 0:
        f.append('node records run into the page records')
    if lay.pages_at + max(lay.page_count, 0) * lay.page_stride > size:
        f.append('page records run past the body')
    for n in lay.nodes:
        count(n['at'] + 12, 'node %d element count' % n['index'])
        ptr(n['at'] + 64, 'node %d elements' % n['index'], n['element_count'] <= 0)
        for e in n['elements']:
            count(e['at'], 'node %d element %d name index' % (n['index'], e['index']))
            if e['name'] is None:
                f.append('node %d element %d name index %d outside the name table' % (n['index'], e['index'], e['name_index']))
            elif lay.find_element(n, e['name']) == -1:
                f.append('node %d element %s not found by name' % (n['index'], e['name']))
    ids = {}
    for p in lay.pages:
        tag = 'page %d' % p['index']
        count(p['at'], tag + ' name index')
        count(p['at'] + 48, tag + ' item count')
        count(p['at'] + 12, tag + ' node index')
        ptr(p['at'] + 196, tag + ' items', p['item_count'] <= 0)
        ptr(p['at'] + 200, tag + ' block', True)
        # The reader tolerates a page with no name (sub_82B3DA28 returns 0) or no node (sub_82B3D600 returns 0); in the
        # player's files exactly the root pages (+16 == 1, where sub_83424D60's parent walk stops) are such, so a
        # non-root page without them, or a root page with a parent, is a misread.
        if p['root'] not in (0, 1):
            f.append('%s root word %d' % (tag, p['root']))
        elif p['root'] == 0:
            if p['name'] is None:
                f.append('%s name index %d outside the name table' % (tag, p['name_index']))
            if not 0 <= p['node'] < lay.node_count:
                f.append('%s node index %d outside 0..%d (sub_82B3D600 refuses it)' % (tag, p['node'], lay.node_count - 1))
            if not 0 <= p['id'] < lay.page_count:
                f.append('%s id %d outside the id map' % (tag, p['id']))
            ids.setdefault(p['id'], []).append(p['index'])
        elif p['parent_page'] != -1 or p['name'] is not None or 0 <= p['node'] < lay.node_count:
            f.append('%s is a root with a parent, a name or a node' % tag)
        if p['kind'] not in (0, 1, 2, 3, 4, 5, 6, 7):
            f.append('%s kind %d' % (tag, p['kind']))
        if p['parent_page'] != -1 and not 0 <= p['parent_page'] < lay.page_count:
            f.append('%s parent page %d out of range' % (tag, p['parent_page']))
        if p['items_at'] + p['item_count'] * ITEM > size:
            f.append(tag + ' items run past the body')
            continue
        for it in p['items']:
            ptr(it['at'] + 48, '%s item %d flow' % (tag, it['index']), True)
            if it['target_page'] != -1 and not 0 <= it['target_page'] < lay.page_count:
                f.append('%s item %d target page %d out of range' % (tag, it['index'], it['target_page']))
    # The game's lookup finds what is there: sub_82B43C18 returns a (page, item) for every record id an indexed item
    # carries. (Page ids repeat, p9 pages 19 / 20 'Bts_Ingame' hold id 0 like page 0: the id map keeps the first.)
    for p in lay.pages:
        for n in range(p['item_count']):
            it = lay.item_nth(p, n)
            if it and it['flow_at']:
                hit = lay.find_record(it['flow']['record_id'])
                if hit is None:
                    f.append('record %d not found' % it['flow']['record_id'])
    return f


def leaves_of_pac(path):
    """Every PACH's key-0 leaf (the one sub_82B4D548 asks for) that holds a UIPR body."""
    data = pac.load(path)
    for name, dec in pac.entries(data, depth=1):
        if name.endswith('/0') and len(dec) > 16 and dec[8:12] == b'UIPR':
            yield name, dec


def commands(lay, opcode=None, nops=4):
    """S111 QEVENT: the page command records (page record +44 = count, +192 = 32-byte records {+0 cond, +4 opcode, +16 operand list of
    8-byte {type, value}}; sub_82B58290 copies them, sub_82B584F8 runs them through the {opcode, handler} table 0x820664F0): one dict per
    record of `opcode` (all when None): page, name, cmd, opcode, operands [(type, value)]. Opcode 120 = the message box post
    (handler sub_82B58080): operand 0 = kind selector (0 / 1 / 2 = box kind 0 / 2 / 4), operand 1 = text id."""
    b, out = lay.body, []
    for pg in lay.pages:
        n, at = pg['word_count'], pg['w192']
        if n <= 0 or not at or at + 32 * n > len(b):
            continue
        for k in range(n):
            r = at + 32 * k
            op = s32(b, r + 4)
            if opcode is not None and op != opcode:
                continue
            ob = u32(b, r + 16)
            ops = [(s32(b, ob + 8 * i), s32(b, ob + 8 * i + 4)) for i in range(nops)] if ob and ob + 8 * nops <= len(b) else []
            out.append({'page': pg['index'], 'name': pg['name'], 'cmd': k, 'opcode': op, 'operands': ops})
    return out


class Links:
    """The decoded layouts of one pac by leaf name, for a root page's cross-layout link."""

    def __init__(self, leaves):
        self.raw = dict(leaves)
        self.cache = {}

    def get(self, leaf):
        if leaf not in self.cache:
            try:
                self.cache[leaf] = Layout(self.raw[leaf]) if leaf in self.raw else None
            except (UiprError, struct.error, IndexError):
                self.cache[leaf] = None
        return self.cache[leaf]


def link_of(p, links):
    """sub_82B44548 +9C..+118 (M100_Q6 step 6): a root page (+580 == 1) leaving to another layout carries record +144 = the layout
    index (LAYOUT_ORDER), +148 = the page, +152 = 0 (nonzero: it replays the saved return instead), +160 a flag; resolved through the
    other layout (its page's name and items with their names: the item the game selects by name leads on)."""
    if p['root'] != 1 or p['link_layout'] == -1:
        return None
    info = {'layout': p['link_layout'], 'page': p['link_page'], 'rec152': p['link_152'], 'flag160': p['link_flag'],
            'leaf': LAYOUT_ORDER.get(p['link_layout'])}
    tl = links.get(info['leaf']) if links and info['leaf'] else None
    if tl and 0 <= p['link_page'] < len(tl.pages):
        tp = tl.pages[p['link_page']]
        info['page_name'] = tp['name']
        info['items'] = [{'index': it['index'], 'name': it['name'], 'target_page': it['target_page'],
                          'target_name': tl.pages[it['target_page']]['name'] if 0 <= it['target_page'] < len(tl.pages) else None}
                         for it in tp['items']]
    return info


def summary(lay, name, a, links=None):
    out = {'leaf': name, 'version': lay.version, 'body': len(lay.body), 'relocations': len(lay.relocs),
           'nodes': lay.node_count, 'pages': lay.page_count, 'names': lay.names['count'], 'warnings': lay.warnings}
    pages = lay.pages
    if a.node:
        pages = [p for p in pages if p['name'] == a.node]
    out['page_list'] = []
    for p in pages:
        d = {k: p[k] for k in ('index', 'name', 'id', 'node', 'root', 'kind', 'parent_page', 'parent_item', 'item_count',
                               'class')}
        d['link'] = link_of(p, links)
        if a.items or a.node:
            d['items'] = []
            for it in p['items']:
                it = dict(it)
                tp = lay.pages[it['target_page']] if 0 <= it['target_page'] < len(lay.pages) else None
                it['reach'] = link_of(tp, links) if tp else None
                d['items'].append(it)
        out['page_list'].append(d)
    if a.labels:
        keep = {p['node'] for p in pages}
        out['labels'] = []
        for n in lay.nodes:
            if a.node and n['index'] not in keep:
                continue
            rows = []
            for e in n['elements']:
                lb = lay.label(e)
                if lb:
                    rows.append({'element': e['index'], 'name': e['name'], 'form': lb[0], 'value': lb[1], 'text': lb[2],
                                 'aux176': e['text_aux'], 'ref328': e['text_ref']})
            if rows:
                out['labels'].append({'node': n['index'], 'pages': [p['name'] for p in lay.pages if p['node'] == n['index']],
                                      'elements': rows})
    if a.elements or a.node:
        nodes = lay.nodes
        if a.node:
            keep = {p['node'] for p in pages}
            nodes = [n for n in nodes if n['index'] in keep or any(e['name'] == a.node for e in n['elements'])]
        out['node_list'] = [{'index': n['index'], 'elements': [{k: e[k] for k in ('index', 'name', 'kind', 'class')}
                                                               for e in n['elements']]} for n in nodes]
    return out


def print_text(s):
    print('== %s  version %d, body %d bytes, %d relocations, %d nodes, %d pages, %d names'
          % (s['leaf'], s['version'], s['body'], s['relocations'], s['nodes'], s['pages'], s['names']))
    for w in s['warnings']:
        print('   warning: ' + w)
    for p in s['page_list']:
        print('  page %3d  id %3d  node %3d  kind %d root %d  parent %3d/%-3d items %3d  %-34s %s'
              % (p['index'], p['id'], p['node'], p['kind'], p['root'], p['parent_page'], p['parent_item'], p['item_count'],
                 p['name'], p['class']))
        lk = p.get('link')
        if lk:
            print('      root link -> layout %d (%s) page %d %s  rec+152 %d flag %d' % (
                lk['layout'], lk['leaf'], lk['page'], lk.get('page_name') or '', lk['rec152'], lk['flag160']))
            for x in lk.get('items', []):
                print('          its item %2d %-14s -> page %s %s' % (x['index'], x['name'], x['target_page'], x['target_name'] or ''))
        for it in p.get('items', []):
            fl = it.get('flow')
            print('      item %3d  %-12s target %3d  hidden %d  %s' % (
                it['index'], it['name'] or '-', it['target_page'], it['hidden'],
                ('record %d label %d code %d type %d drop %d' % (fl['record_id'], fl['label'], fl['screen_code'],
                                                                fl['match_type'], fl['drop'])) if fl else 'no flow data'))
            rc = it.get('reach')
            if rc:
                print('          reaches layout %d (%s) page %d %s' % (rc['layout'], rc['leaf'], rc['page'], rc.get('page_name') or ''))
                for x in rc.get('items', []):
                    print('              item %2d %-14s -> page %s %s' % (x['index'], x['name'], x['target_page'], x['target_name'] or ''))
    for n in s.get('labels', []):
        print('  node %3d  text elements (pages: %s)' % (n['node'], ', '.join(str(x) for x in n['pages']) or '-'))
        for r in n['elements']:
            what = ('text id 0x%X (%d)' % (r['value'], r['value'])) if r['form'] == 'id' else 'string index %d' % r['value']
            print('      %-22s %-22s %r' % (r['name'], what, r['text']))
    for n in s.get('node_list', []):
        print('  node %3d  %d elements' % (n['index'], len(n['elements'])))
        for e in n['elements']:
            print('      element %3d  kind %2d  %-30s %s' % (e['index'], e['kind'], e['name'], e['class']))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', nargs='?')
    ap.add_argument('--pac')
    ap.add_argument('--leaf')
    ap.add_argument('--node')
    ap.add_argument('--items', action='store_true')
    ap.add_argument('--elements', action='store_true')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--find-record', type=int)
    ap.add_argument('--labels', action='store_true')
    ap.add_argument('--plant', choices=['stride', 'width', 'offsets'])
    ap.add_argument('--commands', type=int, metavar='OPCODE', help='list the page command records of this opcode (120 = message box post) with their operands')
    a = ap.parse_args(argv)
    if a.pac:
        path = a.pac if os.path.exists(a.pac) else os.path.join(pac.GAME, 'pac', a.pac)
        every = list(leaves_of_pac(path))
        links = Links(every)
        leaves = [(n, d) for n, d in every if not a.leaf or n == a.leaf]
    elif a.file:
        leaves = [(a.file, open(a.file, 'rb').read())]
        links = None
    else:
        ap.error('FILE or --pac')
    failed = 0
    outs = []
    for name, leaf in leaves:
        try:
            lay = Layout(leaf, a.plant)
        except (UiprError, struct.error, IndexError) as e:
            print('%s: %s' % (name, e))
            failed += 1
            continue
        if a.check:
            f = check(lay)
            print('%s: %s (%d nodes, %d pages, %d relocations)' % (name, 'ok' if not f else '%d FAIL' % len(f),
                                                                  lay.node_count, lay.page_count, len(lay.relocs)))
            for x in f[:5]:
                print('    ' + x)
            failed += bool(f)
            continue
        if a.commands is not None:
            cs = commands(lay, a.commands)
            if a.json:
                outs.append({'leaf': name, 'commands': cs})
            else:
                for c in cs:
                    print('%s page %d %s cmd %d opcode %d operands %s' % (name, c['page'], c['name'], c['cmd'], c['opcode'], c['operands']))
            continue
        if a.find_record is not None:
            print('%s: record %d -> %s' % (name, a.find_record, lay.find_record(a.find_record)))
            continue
        s = summary(lay, name, a, links)
        if a.node and not s['page_list'] and not s.get('node_list'):
            continue
        outs.append(s)
        if not a.json:
            print_text(s)
    if a.json:
        print(json.dumps(outs, indent=1))
    if a.check:
        print('uipr check: %d leaves, %d failed' % (len(leaves), failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
