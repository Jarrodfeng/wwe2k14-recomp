r"""S111 YXMLDEC: decode a Yuke's YXMLBIN menu layout the way the game's reader does (read-only, from the player's own files).

    py tools\menus\yxmlbin.py FILE [--json]                      one layout leaf (a raw .bin under out\, never in the repo)
    py tools\menus\yxmlbin.py --pac PAC [--leaf PATH] [--json]   every YXMLBIN leaf of a pac (tools\wm30\pac.py paths, e.g. p13/7000)
    py tools\menus\yxmlbin.py --census PAC [PAC ...]             one line per YXMLBIN leaf: counts, invariants (exit 1 on any failure)

The parse rule is PORTED from the game, not fitted to bytes (docs\formats\yxmlbin.md):
- `CMenuLayoutReader::ReadEndProcess` sub_823C6870: the leaf must start with the 11 bytes "YXMLBIN VER" (string 0x8200F738,
  strncmp 11), else nothing is parsed.
- `yxmlbin_layout_parse` sub_823C59E8 (this file's `parse`): the version is strtol of header byte 11 on; four LE u32 counts at
  16 / 20 / 24 / 28 (records R, elements E, groups G, tail words S); R records (12 bytes {id, kind, group}, + a 64-byte name when
  version >= 2); then groups, each a LE u32 element count followed by that many elements (92 bytes: 19 LE words + a 16-byte name;
  + 52 bytes when version == 2, + 40 bytes when version >= 3); then S LE u32 words (the reader's array at +16).
- The in-memory field each file byte lands in is named by its offset in the game's 192-byte element / 124-byte record / 16-byte
  group (the offsets the draw code and `docs\formats\select_layout.md` use).

Only what the parse itself writes is decoded. The runtime fields (element +120.. offsets, +140 item pointer, +144 hide word,
+164 text) and the mode-1 group instancing (sub_823C4618 -> sub_823C2C18: records of kind 9 / 10 / 11) are NOT applied here.
Game bytes and decoded dumps go to stdout or out\ only, never into the repo.
"""

import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MAGIC = b'YXMLBIN VER'          # 0x8200F738, compared over 11 bytes by sub_823C6870
# the allocation guards of sub_823C59E8 (a count above one makes the size -1: the allocation fails)
LIMIT_RECORDS = 0x2108421        # x 124
LIMIT_ELEMENTS = 0x1555555       # x 192
LIMIT_GROUPS = 0xFFFFFFF         # x 16
LIMIT_TAIL = 0x3FFFFFFF          # x 4


class YxmlError(Exception):
    """The leaf breaks one of the reader's own conditions (or reads past its end)."""


class Rule:
    """The reader's layout constants. The defaults ARE the game's (sub_823C59E8); a test plants a twin by changing one."""
    def __init__(self, count_width=4, record_head=12, record_name=64, elem_base=92, elem_name_at=76, ext2=52, ext3=40):
        self.count_width = count_width      # the per-group element count: a LE u32 (+4F8 memcpy 4)
        self.record_head = record_head      # 3 LE words (+3B8 memcpy 12)
        self.record_name = record_name      # + 64 bytes when version >= 2 (+478 memcpy 64)
        self.elem_base = elem_base          # 92 bytes (+55C memcpy 92; 19 words swapped by sub_823C28D0)
        self.elem_name_at = elem_name_at    # the first name half at element byte 76 (sp+412 = sp+336 + 76)
        self.ext2 = ext2                    # version 2 extension (+6BC memcpy 52)
        self.ext3 = ext3                    # version >= 3 extension (+8A0 memcpy 40)


GAME = Rule()


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def need(self, n, what):
        if self.p + n > len(self.d):
            raise YxmlError('%s: needs %d bytes at %d, the leaf is %d bytes' % (what, n, self.p, len(self.d)))

    def take(self, n, what):
        self.need(n, what)
        b = self.d[self.p:self.p + n]
        self.p += n
        return b


def c_string(data, at):
    """The C string at data[at:] (the reader's "%s" source is not bounded by the field: it runs to the NUL)."""
    end = data.find(b'\0', at)
    if end < 0:
        end = len(data)
    return data[at:end]


def name16(data, at, what):
    """sub_82C34EF8(dst, 16, "%s", src): written only when the string fits 15 characters + NUL; else the field is left
    unwritten (the call returns -1). An unwritten name is a reader condition this decoder refuses."""
    s = c_string(data, at)
    if len(s) >= 16:
        raise YxmlError('%s: name of %d bytes does not fit the 16-byte field (the reader leaves it unwritten)' % (what, len(s)))
    return s.decode('latin-1')


def version_of(head):
    """strtol(header + 11, base 10): leading spaces, a sign, digits. A digit in byte 15 would read on past the 16-byte copy."""
    s = head[11:16]
    i = 0
    while i < len(s) and s[i:i + 1] in b' \t\n\r\v\f':
        i += 1
    sign = 1
    if i < len(s) and s[i:i + 1] in b'+-':
        sign = -1 if s[i:i + 1] == b'-' else 1
        i += 1
    j = i
    while j < len(s) and 48 <= s[j] <= 57:
        j += 1
    if j == len(s):
        raise YxmlError('header byte 15 is a digit: strtol would read past the 16-byte header copy')
    return sign * int(s[i:j]) if j > i else 0


def cut_substring(full32, part):
    """v2: sub_82C3E518 finds `part` in the 32-byte string (from 0); sub_82C3E2D0 removes len(part) bytes at that index."""
    s = full32
    idx = s.find(part) if part else -1
    if idx < 0 or not part:
        return s
    return s[:idx] + s[idx + len(part):]


def parse(data, rule=GAME):
    """The layout as sub_823C59E8 builds it: {'version', 'counts', 'records', 'groups', 'elements', 'tail', 'consumed'}."""
    if data[:11] != MAGIC:
        raise YxmlError('not a YXMLBIN leaf: the first 11 bytes are %r (sub_823C6870 parses only "YXMLBIN VER")' % data[:11])
    r = Reader(data)
    head = r.take(16, 'header')
    version = version_of(head)
    nrec, nel, ngrp, ntail = struct.unpack('<4I', r.take(16, 'counts'))
    for n, lim, what in ((nrec, LIMIT_RECORDS, 'records'), (nel, LIMIT_ELEMENTS, 'elements'), (ngrp, LIMIT_GROUPS, 'groups'),
                         (ntail, LIMIT_TAIL, 'tail words')):
        if n > lim:
            raise YxmlError('%s count %d over the allocation limit %d' % (what, n, lim))

    records = []
    for k in range(nrec):
        rid, kind, grp = struct.unpack('<3I', r.take(rule.record_head, 'record %d' % k)[:12])
        if version >= 2:
            raw = r.take(rule.record_name, 'record %d name' % k)
            name = raw.split(b'\0', 1)[0].decode('latin-1')
        else:
            name = ''
        records.append({'index': k, 'id': rid, 'kind': kind, 'group': grp, 'name': name})

    elements, groups = [], []
    first = 0
    if nel > 0:
        while True:
            g = len(groups)
            if g >= ngrp:
                break
            raw = r.take(rule.count_width, 'group %d count' % g)
            n = int.from_bytes(raw, 'little')
            if n & 0x80000000:
                raise YxmlError('group %d count %d is negative as the reader compares it (signed)' % (g, n))
            if first + n > nel:
                raise YxmlError('group %d: elements %d..%d run past the element count %d (the reader would write past its array)'
                                % (g, first, first + n - 1, nel))
            for e in range(first, first + n):
                elements.append(parse_element(r, e, g, version, records, rule))
            rec = next((rr['index'] for rr in records if rr['group'] == g), -1)
            groups.append({'index': g, 'count': n, 'first': first, 'record': rec})
            first += n
            if first >= nel:
                break
    if len(elements) != nel:
        raise YxmlError('the groups hold %d elements, the header says %d (the reader leaves the rest unfilled)' % (len(elements), nel))
    if len(groups) != ngrp:
        raise YxmlError('%d groups read, the header says %d (the reader leaves the rest unfilled)' % (len(groups), ngrp))
    tail = list(struct.unpack('<%dI' % ntail, r.take(4 * ntail, 'tail words')))
    return {'version': version, 'counts': {'records': nrec, 'elements': nel, 'groups': ngrp, 'tail': ntail},
            'records': records, 'groups': groups, 'elements': elements, 'tail': tail,
            'consumed': r.p, 'size': len(data)}


def parse_element(r, e, g, version, records, rule):
    at = r.p
    base = r.take(rule.elem_base, 'element %d' % e)
    w = struct.unpack('<19I', base[:76])
    f = struct.unpack('<19f', base[:76])
    el = {
        'index': e, 'group': g,
        '+0': w[0],                                             # file word 0
        'x': f[1], 'y': f[2],                                   # +4 / +8 local position
        'sx': f[3], 'sy': f[4],                                 # +12 / +16 scale
        'w': f[5], 'h': f[6],                                   # +20 / +24 native size
        'anchor': w[7] & 0xFF,                                  # +52 (u8 of file word 7)
        '+28': f[8], '+32': f[9],                               # file words 8 / 9
        'colour': [f[10], f[11], f[12], f[13]],                 # +36..+48 base colour (file words 10..13)
        '+53': w[14] & 0xFF,                                    # u8 of file word 14
        '+56': f[15],                                           # file word 15
        'link': struct.unpack('<i', base[64:68])[0],            # +60: a record index (>= 0) or minus a parent record id
        'skipped_word17': w[17],                                # file word 17: the reader stores it nowhere
        '+64': w[18],                                           # file word 18
        'name': name16(r.d, at + rule.elem_name_at, 'element %d name' % e),   # +72
    }
    if version == 2:
        ext = r.take(rule.ext2, 'element %d v2 extension' % e)
        full = ext[:32].split(b'\0', 1)[0][:31]
        second = cut_substring(full, el['name'].encode('latin-1'))
        if len(second) >= 16:
            raise YxmlError('element %d second name of %d bytes does not fit 16' % (e, len(second)))
        el['name2'] = second.decode('latin-1')                 # +88
        el['+104'] = [v & 0xFFFF for v in struct.unpack('<4I', ext[32:48])]   # +104 / +106 / +108 / +110 (u16)
        el['+112'] = list(ext[48:52])                           # +112..+115 bytes (+113 the group filter)
        el['+116'] = -1
    elif version >= 3:
        ext_at = r.p
        ext = r.take(rule.ext3, 'element %d v3 extension' % e)
        el['name2'] = name16(r.d, ext_at, 'element %d second name' % e)   # +88
        el['+104'] = [v & 0xFFFF for v in struct.unpack('<4I', ext[16:32])]
        el['+112'] = list(ext[32:36])
        el['+116'] = struct.unpack('<i', ext[36:40])[0]
    else:
        el['name2'] = ''
        el['+104'] = [0, 0, 0, 0]
        el['+112'] = [0, 0, 0, 0]
        el['+116'] = -1
    owner = next((rr for rr in records if rr['group'] == g), None)
    el['+188'] = owner['id'] if owner else -1                  # the id of the first record whose group word is this group
    return el


def check(layout):
    """Consistency the files show beyond the reader's own conditions (reported, not refused): links in range, file read whole."""
    nrec = layout['counts']['records']
    bad_links = [el['index'] for el in layout['elements'] if el['link'] >= nrec]
    return {'trailing_bytes': layout['size'] - layout['consumed'], 'links_out_of_range': len(bad_links),
            'parent_links': sum(1 for el in layout['elements'] if el['link'] < 0)}


def kind_of(layout, el):
    link = el['link']
    if 0 <= link < len(layout['records']):
        return layout['records'][link]['kind']
    return None


def print_tree(layout, out=sys.stdout):
    c = layout['counts']
    extra = check(layout)
    out.write('YXMLBIN version %d: %d records, %d elements, %d groups, %d tail words; %d of %d bytes read, %d parent links\n'
              % (layout['version'], c['records'], c['elements'], c['groups'], c['tail'], layout['consumed'], layout['size'],
                 extra['parent_links']))
    for rr in layout['records']:
        out.write('  record %3d id %d kind %d group %d %s\n' % (rr['index'], rr['id'], rr['kind'], rr['group'], rr['name']))
    for g in layout['groups']:
        rec = layout['records'][g['record']]['name'] if g['record'] >= 0 else '-'
        out.write('  group %d (record %d %s): elements %d..%d\n' % (g['index'], g['record'], rec, g['first'], g['first'] + g['count'] - 1))
        for el in layout['elements'][g['first']:g['first'] + g['count']]:
            link = el['link']
            target = layout['records'][link]['name'] if 0 <= link < len(layout['records']) else ('parent id %d' % -link if link < 0 else '?')
            out.write('    #%d %s%s  kind %s -> %s  xy %.4g %.4g  scale %.4g %.4g  size %.4g %.4g  anchor %d  rgba %s  +113 %d\n'
                      % (el['index'], el['name'], ('|' + el['name2']) if el['name2'] else '', kind_of(layout, el), target,
                         el['x'], el['y'], el['sx'], el['sy'], el['w'], el['h'], el['anchor'],
                         '/'.join('%.3g' % v for v in el['colour']), el['+112'][1]))
    if layout['tail']:
        out.write('  tail words: %s%s\n' % (' '.join(str(v) for v in layout['tail'][:16]), ' ...' if len(layout['tail']) > 16 else ''))


def pac_module():
    sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
    import pac  # noqa: E402  (tools\wm30\pac.py: the pac reader)
    return pac


def pac_path(pac, name):
    if os.path.exists(name):
        return name
    for sub in ('', 'menu'):
        p = os.path.join(pac.GAME, 'pac', sub, name)
        if os.path.exists(p):
            return p
    raise SystemExit('no pac %s (looked in %s)' % (name, os.path.join(pac.GAME, 'pac')))


def pac_leaves(name):
    """[(leaf path, bytes)] of every YXMLBIN leaf of a pac."""
    pac = pac_module()
    data = pac.load(pac_path(pac, name))
    return [(leaf, dec) for leaf, dec in pac.entries(data) if dec[:11] == MAGIC]


def census(names, out=sys.stdout):
    """One line per YXMLBIN leaf; returns (leaves, failures)."""
    total = fails = 0
    for name in names:
        for leaf, dec in pac_leaves(name):
            total += 1
            try:
                lay = parse(dec)
                x = check(lay)
                c = lay['counts']
                out.write('%s %s v%d: %d records, %d elements, %d groups, %d tail; trailing %d, links out of range %d, parent links %d\n'
                          % (os.path.basename(name), leaf, lay['version'], c['records'], c['elements'], c['groups'], c['tail'],
                             x['trailing_bytes'], x['links_out_of_range'], x['parent_links']))
            except YxmlError as ex:
                fails += 1
                out.write('%s %s FAIL: %s\n' % (os.path.basename(name), leaf, ex))
    out.write('census: %d YXMLBIN leaves, %d failed\n' % (total, fails))
    return total, fails


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('file', nargs='?')
    ap.add_argument('--pac')
    ap.add_argument('--leaf')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--census', nargs='+')
    a = ap.parse_args(argv)
    if a.census:
        _, fails = census(a.census)
        return 1 if fails else 0
    if a.pac:
        items = [(leaf, dec) for leaf, dec in pac_leaves(a.pac) if not a.leaf or leaf == a.leaf]
        if not items:
            raise SystemExit('no YXMLBIN leaf%s in %s' % (' ' + a.leaf if a.leaf else '', a.pac))
    elif a.file:
        items = [(a.file, open(a.file, 'rb').read())]
    else:
        ap.error('FILE, --pac or --census')
    rc = 0
    for leaf, dec in items:
        try:
            lay = parse(dec)
        except YxmlError as ex:
            print('%s FAIL: %s' % (leaf, ex))
            rc = 1
            continue
        if a.json:
            lay['leaf'] = leaf
            lay['check'] = check(lay)
            print(json.dumps(lay))
        else:
            print('== %s' % leaf)
            print_tree(lay)
    return rc


if __name__ == '__main__':
    sys.exit(main())
