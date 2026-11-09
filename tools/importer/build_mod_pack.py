"""Session 98, Superstar Importer diagnostic run 2: build a minimal fake DLC pack ("pack 6") from the PLAYER'S OWN DLC
files, then read it back the way the game does.

    py tools\\importer\\build_mod_pack.py [--dlc-dir DIR] [--out DIR] [--pack-id 6] [--char 397] [--source 385]
                                          [--revision 32] [--name-text 299] [--pack-code 6] [--validate-only]
                                          [--selftest]

NEVER commit or ship the output (CLAUDE.md rule 1): it is built from the player's files into out\\ (gitignored). The
mods-folder home comes later; for now the game sees it through --wwe2k14_dlc_extra_dir=OUT (src\\dlc.h) with the pack id
gate opened by --wwe2k14_dlc_gate_extra=6.

What it writes (formats: docs\\formats\\catalog_dlc.md, chetc_roster_record.md, roster_images.md):
  info\\catalog.dlc        DLCC v2, pack PACK, revision REV, one type-1 entry (entry id 13, licence bit 0, char CHAR),
                           no attribute / bundle records, the file list strcmp-sorted with each pac's arc index (a, b)
  info\\catalog_hd.arc / catalog_sd.arc
                           the plist360.arc-format index of every pac (one group per pac index, rows taken from each
                           pac's own EPAC / EPK8 directory)
  pac\\ch\\chCHAR_dlcPP.pac  = the source's chSRC_dlcNN.pac with its EMD names 000SRCxx -> 000CHARxx (in place)
  pac\\ch\\chEtc_dlcPP.pac   = the highest pack's chEtc with rows CHAR of CHAR/DAT, CHAR/DSBD, CHAR/PRO cloned from SRC:
                           ids SRC -> CHAR (DAT +0 / +4 / +200, PRO self ids), attire ids SRC*100+n -> CHAR*100+n (DAT
                           +116.., DSBD), name text ids -> 60000 / 62000 / 64000 + NAME_TEXT (299 = "TEST CENA", an
                           existing test name: no string pac edit), pack code (+17) -> PACK_CODE, rank (+2) -> max + 1.
                           New leaves are stored as 'BPE ' blocks with an identity pair table (valid Gage BPE: the guest
                           decoder sub_82C41918 and src\\native_bpe.h both expand it to the raw bytes)
  pac\\string / sound / audience / DLC / ExclusiveAI _dlcPP.pac = byte copies of the highest pack's (cumulative files:
                           the highest pack id's copy is the one read)
  pac\\DLC_HD_dlcPP.pac / DLC_SD_dlcPP.pac
                           SSFA / SSFB / SSFC entry %04d CHAR = the source's packed renders (copied, not re-encoded) and
                           IGST / TEX = the highest pack's banner PACH plus a leaf for entry 13 (a copy of leaf 0)

--selftest rebuilds the highest real pack's arcs and an unchanged chEtc through the same writers and checks they are
byte-identical to the originals (proves the writers). Validation (always run after a build) parses the pack back.
"""

import argparse
import datetime
import os
import shutil
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import pac  # noqa: E402  (bpe_decode / decode / pach: the readers the other tools use)


def main_root():
    """The main checkout (game_data and out live there; a worktree has neither)."""
    marker = os.sep + os.path.join('.claude', 'worktrees') + os.sep
    return REPO.split(marker)[0] if marker in REPO else REPO


SECTOR = 0x800
HEADER = 0x4000
ENTRY_ID = 13           # the next free catalog entry id (packs 2-5 use 1-12)
GATE_TABLE = [0x00010001, 0x00020001, 0x00030008, 0x00040008, 0x00050008]  # 0x82035E9C, read from the image (run 072)


# ---------------------------------------------------------------- pacs (EPAC / EPK8)

class Pac:
    """A pac's directory: [(tag, kind, [(name, blob)])], blob = the entry's bytes (sector-aligned area cut to size)."""

    def __init__(self, magic, dirs, version=7, trailer=b''):
        # trailer: the bytes after the data area (the real pacs end with one sector holding the packing tool's name /
        # version, a host name, an IP and a time: not data the game reads; kept as found, else one zero sector)
        self.magic, self.dirs, self.version, self.trailer = magic, dirs, version, trailer

    @staticmethod
    def read(path):
        d = open(path, 'rb').read()
        magic = d[:4]
        if magic not in (b'EPAC', b'EPK8'):
            raise SystemExit('%s: not a plain EPAC / EPK8 pac (XCTD pacs are not supported here)' % path)
        dir_bytes, data_size, version = struct.unpack_from('<III', d, 4)
        p, end, dirs = SECTOR, SECTOR + dir_bytes, []
        while p < end:
            tag = d[p:p + 4]
            words, kind = struct.unpack_from('<HH', d, p + 4)
            p += 12
            rs = 16 if kind == 1 else 12
            ents = []
            for _ in range(words * 4 // rs):
                name = d[p:p + rs - 8]
                sec, sz = struct.unpack_from('<II', d, p + rs - 8)
                off = HEADER + sec * SECTOR
                ents.append([name, d[off:off + sz * 256], sec, sz])
                p += rs
            dirs.append([tag, kind, ents])
        return Pac(magic, dirs, version, d[HEADER + data_size:])

    def layout(self):
        """Sectors in directory order, each entry at the next 2 KB boundary (the real pacs' layout)."""
        sec = 0
        for _, _, ents in self.dirs:
            for e in ents:
                e[2] = sec
                e[3] = (len(e[1]) + 255) // 256
                sec += (e[3] * 256 + SECTOR - 1) // SECTOR

    def bytes(self, relayout=True):
        if relayout:
            self.layout()
        dir_rows = b''
        for tag, kind, ents in self.dirs:
            rs = 16 if kind == 1 else 12
            dir_rows += tag + struct.pack('<HHI', len(ents) * rs // 4, kind, 0)
            for name, _, sec, sz in ents:
                dir_rows += name + struct.pack('<II', sec, sz)
        end = 0
        for _, _, ents in self.dirs:
            for _, _, sec, sz in ents:
                end = max(end, sec * SECTOR + sz * 256)
        data_size = (end + SECTOR - 1) // SECTOR * SECTOR
        trailer = self.trailer or bytes(SECTOR)
        out = bytearray(HEADER + data_size + len(trailer))
        out[HEADER + data_size:] = trailer
        out[0:16] = self.magic + struct.pack('<III', len(dir_rows), data_size, self.version)
        out[SECTOR:SECTOR + len(dir_rows)] = dir_rows
        for _, _, ents in self.dirs:
            for _, blob, sec, sz in ents:
                off = HEADER + sec * SECTOR
                out[off:off + len(blob)] = blob
        return bytes(out)

    def find(self, tag, name):
        for t, _, ents in self.dirs:
            if t == tag:
                for e in ents:
                    if e[0] == name:
                        return e
        return None

    def arc_rows(self):
        """This pac's rows in a catalog arc (big-endian; directory flags = words << 20, | 0x1000 for 8-char names)."""
        out = b''
        for tag, kind, ents in self.dirs:
            rs = 16 if kind == 1 else 12
            out += tag + struct.pack('>II', (len(ents) * rs // 4) << 20 | (0x1000 if kind == 1 else 0), 0)
            for name, _, sec, sz in ents:
                out += name + struct.pack('>II', sec, sz)
        return out


def pach_read(blob):
    """[(id, raw bytes)] of a PACH (the entry's area may run past the PACH's end: cut by the table)."""
    return pac.pach(blob)


def pach_write(leaves):
    """A PACH from [(id, raw)]: the table, then each leaf at a 4-byte boundary (the real PACHs' layout)."""
    table, data = b'', bytearray()
    for eid, raw in leaves:
        table += struct.pack('<III', eid, len(data), len(raw))
        data += raw
        data += b'\0' * (-len(data) % 4)
    return b'PACH' + struct.pack('<I', len(leaves)) + table + bytes(data)


def bpe_identity(raw, header_words=(0x00000100, 0)):
    """A 'BPE ' block that expands to `raw`: per Gage block an identity pair table (0xFF -> skip 128 codes, one
    literal pair for code 128 (0x80 = itself), 0xFE -> skip 127 codes = 256: table end), a u16 length, the bytes."""
    out = b'BPE ' + struct.pack('<II', *header_words) + struct.pack('<I', len(raw))
    for i in range(0, max(len(raw), 1), 0xFFFF):
        chunk = raw[i:i + 0xFFFF]
        out += b'\xff\x80\xfe' + struct.pack('<H', len(chunk)) + chunk
    return out


# ---------------------------------------------------------------- catalog.dlc / arcs

def read_catalog(path):
    d = open(path, 'rb').read()
    magic, ver, hs, pid, rev, ne, na, nb, nf = struct.unpack_from('<4sHHHHHHHH', d, 0)
    return {'magic': magic, 'version': ver, 'header': hs, 'pack': pid, 'revision': rev, 'entries': ne, 'attr': na,
            'bundle': nb, 'files': nf, 'data': d}


def parse_catalog_like_game(d, gate):
    """The game's parser order (sub_8281F150): header, gate, entries (64 B), attribute records (u16 size each),
    bundle records (u16 size each), file records (u16 size, s16 a, s16 b, u8 len, name). Returns (ok, why, info)."""
    if d[:4] != b'DLCC':
        return False, 'magic', None
    ver, hs, pid, rev, ne, na, nb, nf = struct.unpack_from('<HHHHHHHH', d, 4)
    if ver != 2:
        return False, 'version %d != 2' % ver, None
    if not any((w >> 16) == pid and rev >= (w & 0xFFFF) for w in gate):
        return False, 'pack id %d / revision %d not in the gate table' % (pid, rev), None
    p = hs
    entries = []
    for _ in range(ne):
        e = d[p:p + 64]
        size, eid, typ, epack, bit, eid2 = struct.unpack_from('<HHHHHH', e, 0)
        if size != 64:
            return False, 'entry size %d' % size, None
        entries.append({'id': eid, 'type': typ, 'pack': epack, 'bit': bit, 'id2': eid2,
                        'char': struct.unpack_from('<i', e, 16)[0], 'name': e[32:64].split(b'\0')[0].decode('latin-1')})
        p += 64
    for _ in range(na + nb):
        p += struct.unpack_from('<H', d, p)[0]
    files = []
    for _ in range(nf):
        size, a, b, n = struct.unpack_from('<HhhB', d, p)
        if size != 7 + n:
            return False, 'file record size', None
        files.append((d[p + 7:p + 7 + n].decode('ascii'), a, b))
        p += size
    if any(d[p:]):
        return False, 'non-zero bytes after the file list', None
    names = [f[0].encode() for f in files]
    if names != sorted(names):
        return False, 'file list not strcmp-sorted (the game binary-searches it)', None
    return True, '', {'pack': pid, 'revision': rev, 'entries': entries, 'files': files}


def write_catalog(pack_id, revision, char_id, name, files):
    """files: [(name, a, b)] in any order; written strcmp-sorted."""
    files = sorted(files, key=lambda f: f[0].encode())
    filetime = int((datetime.datetime.now(datetime.timezone.utc) -
                    datetime.datetime(1601, 1, 1, tzinfo=datetime.timezone.utc)).total_seconds() * 10_000_000)
    out = b'DLCC' + struct.pack('<HHHHHHHHQ', 2, 28, pack_id, revision, 1, 0, 0, len(files), filetime)
    entry = struct.pack('<HHHHHH', 64, ENTRY_ID, 1, pack_id, 0, ENTRY_ID) + b'\0' * 4 + struct.pack('<i', char_id)
    entry += b'\0' * 12 + name.encode('latin-1')[:31].ljust(32, b'\0')
    out += entry
    for fname, a, b in files:
        n = fname.encode('ascii')
        out += struct.pack('<HhhB', 7 + len(n), a, b, len(n)) + n
    return out + b'\0' * (-len(out) % 16)


def write_arc(pacs_by_index):
    """pacs_by_index: [Pac] for index 0, 1, ...: group markers 0xFF000000 (first), 0xFFFF0000 | k, end 0xFFFFFFFF."""
    out = b''
    for k, p in enumerate(pacs_by_index):
        out += (b'\xff\x00\x00\x00' if k == 0 else struct.pack('>I', 0xFFFF0000 | k)) + p.arc_rows()
    return out + b'\xff\xff\xff\xff'


def read_arc(d):
    """{group index: [(tag, flags, [(name, sector, size256)])]}."""
    groups, p, cur = {}, 0, None
    while p + 4 <= len(d):
        w = struct.unpack_from('>I', d, p)[0]
        if w == 0xFFFFFFFF:
            break
        if p == 0 and d[:4] == b'\xff\x00\x00\x00':
            cur, p = 0, p + 4
            groups[cur] = []
            continue
        if (w >> 16) == 0xFFFF:
            cur, p = w & 0xFFFF, p + 4
            groups[cur] = []
            continue
        tag = d[p:p + 4]
        flags = struct.unpack_from('>I', d, p + 4)[0]
        p += 12
        rs = 16 if flags & 0x1000 else 12
        ents = []
        for _ in range((flags >> 20) * 4 // rs):
            ents.append((d[p:p + rs - 8], *struct.unpack_from('>II', d, p + rs - 8)))
            p += rs
        groups[cur].append((tag, flags, ents))
    return groups


# ---------------------------------------------------------------- the build

def find_packs(dlc_dir):
    """{pack id: folder} for every info\\catalog.dlc under dlc_dir."""
    out = {}
    for root, _, files in os.walk(dlc_dir):
        if os.path.basename(root).lower() == 'info' and 'catalog.dlc' in [f.lower() for f in files]:
            c = read_catalog(os.path.join(root, 'catalog.dlc'))
            if c['magic'] == b'DLCC':
                out[c['pack']] = os.path.dirname(root)
    return out


def pac_path(pack_dir, rel):
    p = os.path.join(pack_dir, rel)
    if os.path.exists(p):
        return p
    # case-insensitive fallback (the catalogs' case differs from the folder's on some files)
    d, base = os.path.split(p)
    for f in os.listdir(d):
        if f.lower() == base.lower():
            return os.path.join(d, f)
    raise SystemExit('missing %s' % p)


def patch_rows(etc, src, char, name_text, pack_code, log):
    """chEtc: rows `char` of CHAR/DAT, CHAR/DSBD, CHAR/PRO cloned from `src`."""
    att_lo, att_hi, att_delta = src * 100, src * 100 + 99, (char - src) * 100
    # DAT: 501 leaves (id = character id), each 'BPE ' -> 208 bytes = {u16 2, u16 208} + the 204-byte record.
    dat = etc.find(b'CHAR', b'DAT ')
    leaves = pach_read(dat[1])
    rows = {eid: pac.decode(raw) for eid, raw in leaves}
    rec = bytearray(rows[src])
    if len(rec) != 208 or struct.unpack_from('<H', rec, 4)[0] != src:
        raise SystemExit('CHAR/DAT row %d is not the expected 208-byte record' % src)
    R = 4  # the file row's header
    # rank (+2): unique among the active rows (1..140 in pack 5's chEtc; CAS ids 0-99 hold 1000+, 500 holds 2000)
    ranks = [struct.unpack_from('<H', r, R + 2)[0] for eid, r in rows.items() if len(r) == 208 and 100 <= eid < 500 and r[R + 16] == 1]
    for off in (0, 4, 200):
        struct.pack_into('<H', rec, R + off, char)
    struct.pack_into('<H', rec, R + 2, max(ranks) + 1)
    for k, base in enumerate((60000, 62000, 64000)):
        struct.pack_into('<H', rec, R + 6 + 2 * k, base + name_text)
    rec[R + 17] = pack_code
    att = []
    for off in range(R + 116, R + 200, 2):  # attire / label ids (u16 run from +116)
        v = struct.unpack_from('<H', rec, off)[0]
        if att_lo <= v <= att_hi:
            struct.pack_into('<H', rec, off, v + att_delta)
            att.append((off - R, v, v + att_delta))
    hdr = struct.unpack_from('<II', leaves[src][1], 4)
    new_dat = [(eid, bpe_identity(bytes(rec), hdr) if eid == char else raw) for eid, raw in leaves]
    dat[1] = pach_write(new_dat)
    log.append('CHAR/DAT row %d: ids %d, rank %d, name texts %d/%d/%d, pack code %d, attire %s' % (
        char, char, max(ranks) + 1, 60000 + name_text, 62000 + name_text, 64000 + name_text, pack_code,
        ', '.join('+%d %d->%d' % a for a in att)))
    # DSBD: 501 raw 80-byte leaves (40 u16: the attire id list first)
    dsbd = etc.find(b'CHAR', b'DSBD')
    leaves = pach_read(dsbd[1])
    row = bytearray(leaves[src][1])
    changed = []
    for off in range(0, len(row), 2):
        v = struct.unpack_from('<H', row, off)[0]
        if att_lo <= v <= att_hi:
            struct.pack_into('<H', row, off, v + att_delta)
            changed.append('+%d %d->%d' % (off, v, v + att_delta))
    dsbd[1] = pach_write([(eid, bytes(row) if eid == char else raw) for eid, raw in leaves])
    log.append('CHAR/DSBD row %d (raw, 80 B): %s' % (char, ', '.join(changed) or 'no attire ids'))
    # PRO: 501 'BPE ' leaves -> 1276 bytes; the character's own id appears in the slots from +576 (seen for 384, 385,
    # 393 and the blank 397): src -> char there.
    pro = etc.find(b'CHAR', b'PRO ')
    leaves = pach_read(pro[1])
    row = bytearray(pac.decode(leaves[src][1]))
    changed = []
    for off in range(4, len(row), 2):
        if struct.unpack_from('<H', row, off)[0] == src:
            struct.pack_into('<H', row, off, char)
            changed.append(off)
    hdr = struct.unpack_from('<II', leaves[src][1], 4)
    pro[1] = pach_write([(eid, bpe_identity(bytes(row), hdr) if eid == char else raw) for eid, raw in leaves])
    log.append('CHAR/PRO row %d (%d B): id %d -> %d at %s' % (char, len(row), src, char, changed))


def build(a):
    packs = find_packs(a.dlc_dir)
    if not packs:
        raise SystemExit('no DLC packs under %s' % a.dlc_dir)
    top = max(p for p in packs if p < a.pack_id)
    src_pack = None
    for pid, folder in packs.items():
        cat = read_catalog(os.path.join(folder, 'info', 'catalog.dlc'))
        ok, _, info = parse_catalog_like_game(cat['data'], GATE_TABLE)
        if ok and any(f[0].lower() == ('pac\\ch\\ch%d_dlc%02d.pac' % (a.source, pid)) for f in info['files']):
            src_pack = pid
    if src_pack is None:
        raise SystemExit('no pack holds character %d' % a.source)
    T, S, P = packs[top], packs[src_pack], a.pack_id
    print('packs found: %s; cumulative files from pack %d (%s); character %d from pack %d (%s)' % (
        sorted(packs), top, T, a.source, src_pack, S))
    out = a.out
    if os.path.exists(out):
        shutil.rmtree(out)
    for sub in ('info', 'pac', os.path.join('pac', 'ch')):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    log = []
    pp, tt, ss = '%02d' % P, '%02d' % top, '%02d' % src_pack

    # 1. byte copies of the cumulative pacs
    for base in ('DLC', 'ExclusiveAI', 'audience', 'sound', 'string'):
        shutil.copyfile(pac_path(T, 'pac\\%s_dlc%s.pac' % (base, tt)), os.path.join(out, 'pac', '%s_dlc%s.pac' % (base, pp)))
        log.append('pac\\%s_dlc%s.pac = copy of pack %d\'s' % (base, pp, top))
    # 2. the character pac: EMD names 000SRCxx -> 000CHARxx in the directory (in place: same size, same layout)
    d = bytearray(open(pac_path(S, 'pac\\ch\\ch%d_dlc%s.pac' % (a.source, ss)), 'rb').read())
    chp = Pac.read(pac_path(S, 'pac\\ch\\ch%d_dlc%s.pac' % (a.source, ss)))
    renamed = []
    old_prefix, new_prefix = b'000%03d' % a.source, b'000%03d' % a.char
    dir_bytes = struct.unpack_from('<I', d, 4)[0]
    p = SECTOR
    while p < SECTOR + dir_bytes:
        words, kind = struct.unpack_from('<HH', d, p + 4)
        rs = 16 if kind == 1 else 12
        q = p + 12
        for _ in range(words * 4 // rs):
            name = bytes(d[q:q + rs - 8])
            if name.startswith(old_prefix):
                d[q:q + 6] = new_prefix
                renamed.append('%s -> %s' % (name.decode(), bytes(d[q:q + rs - 8]).decode()))
            q += rs
        p = q
    if not renamed:
        raise SystemExit('ch%d: no %s* entries to rename' % (a.source, old_prefix.decode()))
    open(os.path.join(out, 'pac', 'ch', 'ch%d_dlc%s.pac' % (a.char, pp)), 'wb').write(bytes(d))
    log.append('pac\\ch\\ch%d_dlc%s.pac = pack %d\'s ch%d with %s (%s)' % (a.char, pp, src_pack, a.source,
                                                                        ', '.join(renamed), chp.magic.decode()))
    # 3. chEtc
    etc = Pac.read(pac_path(T, 'pac\\ch\\chEtc_dlc%s.pac' % tt))
    patch_rows(etc, a.source, a.char, a.name_text, a.pack_code, log)
    open(os.path.join(out, 'pac', 'ch', 'chEtc_dlc%s.pac' % pp), 'wb').write(etc.bytes())
    # 4. DLC_HD / DLC_SD: the source's renders as CHAR + the top pack's banners + a leaf for our entry
    key_src, key_new = b'%04d' % a.source, b'%04d' % a.char
    for res in ('HD', 'SD'):
        srcp = Pac.read(pac_path(S, 'pac\\DLC_%s_dlc%s.pac' % (res, ss)))
        topp = Pac.read(pac_path(T, 'pac\\DLC_%s_dlc%s.pac' % (res, tt)))
        dirs = []
        for tag in (b'SSFA', b'SSFB', b'SSFC'):
            e = srcp.find(tag, key_src)
            if not e:
                raise SystemExit('DLC_%s_dlc%s: no %s/%s' % (res, ss, tag.decode(), key_src.decode()))
            dirs.append([tag, 0, [[key_new, e[1], 0, 0]]])
        tex = topp.find(b'IGST', b'TEX ')
        leaves = pach_read(tex[1])
        ids = [eid for eid, _ in leaves]
        if ENTRY_ID not in ids:
            leaves = sorted(leaves + [(ENTRY_ID, dict(leaves)[0])], key=lambda l: l[0])  # ids ascending, as the real PACHs
        dirs.append([b'IGST', 0, [[b'TEX ', pach_write(leaves), 0, 0]]])
        newp = Pac(b'EPAC', dirs, topp.version, topp.trailer)
        open(os.path.join(out, 'pac', 'DLC_%s_dlc%s.pac' % (res, pp)), 'wb').write(newp.bytes())
        log.append('pac\\DLC_%s_dlc%s.pac: SSFA/SSFB/SSFC %s (pack %d\'s %s renders, packed as they were) + IGST/TEX '
                   '(pack %d\'s %d banner leaves %s + leaf %d = copy of leaf 0)' % (
                       res, pp, key_new.decode(), src_pack, key_src.decode(), top, len(ids), ids, ENTRY_ID))
    # 5. file list with arc indexes: DLC_HD (0, -1), DLC_SD (-1, 0), every other pac a == b running in sorted order
    names = ['pac\\DLC_HD_dlc%s.pac' % pp, 'pac\\DLC_SD_dlc%s.pac' % pp] + [
        'pac\\%s_dlc%s.pac' % (b, pp) for b in ('DLC', 'ExclusiveAI', 'audience', 'sound', 'string')] + [
        'pac\\ch\\ch%d_dlc%s.pac' % (a.char, pp), 'pac\\ch\\chEtc_dlc%s.pac' % pp]
    names.sort(key=lambda s: s.encode())
    files, hd, sd, k = [], [], [], 1
    for n in names:
        pobj = Pac.read(os.path.join(out, n))
        if 'DLC_HD_' in n:
            files.append((n, 0, -1))
            hd.insert(0, pobj)
        elif 'DLC_SD_' in n:
            files.append((n, -1, 0))
            sd.insert(0, pobj)
        else:
            files.append((n, k, k))
            hd.append(pobj)
            sd.append(pobj)
            k += 1
    open(os.path.join(out, 'info', 'catalog.dlc'), 'wb').write(
        write_catalog(P, a.revision, a.char, a.entry_name, files))
    open(os.path.join(out, 'info', 'catalog_hd.arc'), 'wb').write(write_arc(hd))
    open(os.path.join(out, 'info', 'catalog_sd.arc'), 'wb').write(write_arc(sd))
    log.append('info\\catalog.dlc: pack %d revision %d, entry %d type 1 bit 0 char %d "%s", %d files' % (
        P, a.revision, ENTRY_ID, a.char, a.entry_name, len(files)))
    for line in log:
        print('  ' + line)


# ---------------------------------------------------------------- validation

def validate(a):
    """Read the pack back the way the game does; returns the number of failures."""
    out, fails, P = a.out, [], a.pack_id
    say = lambda ok, msg: (print('  %s %s' % ('PASS' if ok else 'FAIL', msg)), ok or fails.append(msg))
    gate = list(GATE_TABLE)
    gate[0] = (P << 16) | 1  # what --wwe2k14_dlc_gate_extra writes
    cat = open(os.path.join(out, 'info', 'catalog.dlc'), 'rb').read()
    ok, why, info = parse_catalog_like_game(cat, GATE_TABLE)
    say(not ok and 'gate' in why, 'catalog refused by the UNPATCHED gate (%s): the switch is needed' % (why or 'accepted'))
    ok, why, info = parse_catalog_like_game(cat, gate)
    say(ok, 'catalog parses like sub_8281F150 with word 0 = %08X%s' % (gate[0], '' if ok else ': ' + why))
    if not ok:
        return len(fails)
    say(info['entries'][0]['char'] == a.char and info['entries'][0]['type'] == 1,
        'entry: %s' % info['entries'][0])
    # our provider's own reader (src/dlc_catalog.h FindFileList: the list must chain to the last non-zero byte)
    sys.path.insert(0, os.path.join(REPO, 'tools', 'dlc'))
    hd = read_arc(open(os.path.join(out, 'info', 'catalog_hd.arc'), 'rb').read())
    sd = read_arc(open(os.path.join(out, 'info', 'catalog_sd.arc'), 'rb').read())
    by_a = {f[1]: f[0] for f in info['files'] if f[1] >= 0}
    by_b = {f[2]: f[0] for f in info['files'] if f[2] >= 0}
    for label, arc, idx in (('HD', hd, by_a), ('SD', sd, by_b)):
        say(sorted(arc) == sorted(idx), '%s arc groups %s = the file records\' %s indexes %s' % (
            label, sorted(arc), 'a' if label == 'HD' else 'b', sorted(idx)))
        for g, rows in arc.items():
            pobj = Pac.read(os.path.join(out, idx[g]))
            mine = [(t, [(e[0], e[2], e[3]) for e in ents]) for t, _, ents in pobj.dirs]
            theirs = [(t, list(ents)) for t, _, ents in rows]
            say(mine == theirs, '%s arc group %d = %s\'s own directory (%d dirs)' % (label, g, idx[g], len(mine)))

    def resolve(arc, idx, tag, name):
        """The highest-numbered match within this pack (one pack: at most one)."""
        for g, rows in arc.items():
            for t, _, ents in rows:
                if t == tag:
                    for e in ents:
                        if e[0] == name:
                            path = os.path.join(out, idx[g])
                            d = open(path, 'rb').read()
                            off = HEADER + e[1] * SECTOR
                            return idx[g], d[off:off + e[2] * 256]
        return None, None

    char_key = b'%04d' % a.char
    emd = [b'000%03d%02d' % (a.char, n) for n in (2, 4)]
    wanted = [(b'CHAR', b'DAT '), (b'CHAR', b'DSBD'), (b'CHAR', b'PRO '), (b'CHAR', b'WCM '), (b'SSFA', char_key),
              (b'SSFB', char_key), (b'SSFC', char_key), (b'IGST', b'TEX ')] + [(b'EMD ', n) for n in emd]
    for tag, name in wanted:
        for label, arc, idx in (('HD', hd, by_a), ('SD', sd, by_b)):
            where, blob = resolve(arc, idx, tag, name)
            say(where is not None, '%s /%s/%s -> %s' % (label, tag.decode().strip(), name.decode().strip(), where))
            if where is None:
                continue
            if (tag, name) == (b'CHAR', b'DAT '):
                rec = pac.decode(dict(pach_read(blob))[a.char])
                ids = [struct.unpack_from('<H', rec, 4 + o)[0] for o in (0, 4, 200)]
                txt = [struct.unpack_from('<H', rec, 4 + o)[0] for o in (6, 8, 10)]
                say(len(rec) == 208 and ids == [a.char] * 3 and rec[4 + 16] == 1 and rec[4 + 17] == a.pack_code,
                    '  row %d decodes (pac.py BPE): ids %s, active %d, pack code %d, name texts %s, attire %s' % (
                        a.char, ids, rec[4 + 16], rec[4 + 17], txt,
                        [struct.unpack_from('<H', rec, 4 + o)[0] for o in range(116, 124, 2)]))
                src = pac.decode(dict(pach_read(blob))[a.source])
                diff = [o - 4 for o in range(4, 208) if src[o] != rec[o]]
                say(True, '  row %d differs from row %d only at record bytes %s' % (a.char, a.source, diff))
            elif tag == b'SSFA':
                dds = pac.decode(blob)
                say(dds[:4] == b'DDS ' and dds[84:88] == b'DXT5', '  SSFA %s decodes to DDS %s %dx%d' % (
                    name.decode(), dds[84:88], struct.unpack_from('<I', dds, 16)[0], struct.unpack_from('<I', dds, 12)[0]))
            elif tag == b'IGST':
                ids = [e for e, _ in pach_read(blob)]
                say(ENTRY_ID in ids, '  banner leaves %s' % ids)
            elif tag == b'EMD ':
                say(blob[:4] == b'PACH', '  %s is a PACH of %d leaves' % (name.decode(), struct.unpack_from('<I', blob, 4)[0]))
    # the identity BPE through a literal port of the guest loop (native_bpe.h DecodeReference)
    sample = bytes(range(256)) * 5
    say(pac.bpe_decode(bpe_identity(sample)) == sample, 'identity BPE round-trips (1280 bytes, every byte value)')
    big = bytes((i * 7) & 0xFF for i in range(70000))
    say(pac.bpe_decode(bpe_identity(big)) == big, 'identity BPE round-trips across Gage blocks (70000 bytes)')
    # the provider's file check: every listed file present
    missing = [f[0] for f in info['files'] if not os.path.exists(os.path.join(out, f[0]))]
    say(not missing, 'every listed file present%s' % (': missing %s' % missing if missing else ''))
    return len(fails)


def selftest(a):
    """The writers against the real files: the top pack's arcs and an unchanged chEtc rebuilt byte for byte."""
    packs = find_packs(a.dlc_dir)
    fails = 0
    for pid in sorted(packs):
        folder = packs[pid]
        cat = read_catalog(os.path.join(folder, 'info', 'catalog.dlc'))
        ok, why, info = parse_catalog_like_game(cat['data'], GATE_TABLE)
        print('pack %d: catalog %s%s' % (pid, 'parses' if ok else 'REFUSED', '' if ok else ' (' + why + ')'))
        if not ok or not info['files']:
            continue
        for label, col in (('hd', 1), ('sd', 2)):
            arc_path = os.path.join(folder, 'info', 'catalog_%s.arc' % label)
            idx = {f[col]: f[0] for f in info['files'] if f[col] >= 0}
            pacs = [Pac.read(pac_path(folder, idx[k])) for k in range(len(idx))]
            same = write_arc(pacs) == open(arc_path, 'rb').read()
            fails += not same
            print('  catalog_%s.arc rebuilt from the pacs: %s' % (label, 'IDENTICAL' if same else 'DIFFERENT'))
        for f in info['files']:
            if f[0].lower().startswith('pac\\ch\\chetc_dlc') or f[0].lower().startswith('pac\\dlc_hd_dlc'):
                path = pac_path(folder, f[0])
                orig = open(path, 'rb').read()
                p = Pac.read(path)
                for _, _, ents in p.dirs:  # re-pack every PACH through pach_write (same bytes expected)
                    for e in ents:
                        if e[1][:4] == b'PACH':
                            leaves = pach_read(e[1])
                            e[1] = pach_write(leaves) + b'\0' * (len(e[1]) - len(pach_write(leaves)))
                same = p.bytes() == orig
                fails += not same
                print('  %s rebuilt: %s' % (f[0], 'IDENTICAL' if same else 'DIFFERENT'))
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    root = main_root()
    ap.add_argument('--dlc-dir', default=os.path.join(root, 'game_data', 'dlc'))
    ap.add_argument('--out', default=os.path.join(root, 'out', 's98', 'importer', 'run2', 'pack6'))
    ap.add_argument('--pack-id', type=int, default=6)
    ap.add_argument('--revision', type=int, default=32,
                    help='32: above pack 5\'s 31, so pack 6 is the highest by id AND revision (which one decides is '
                         'unconfirmed)')
    ap.add_argument('--char', type=int, default=397)
    ap.add_argument('--source', type=int, default=385)
    ap.add_argument('--name-text', type=int, default=299, help='name text ids 60000/62000/64000 + N (299 = TEST CENA)')
    ap.add_argument('--pack-code', type=int, default=6)
    ap.add_argument('--entry-name', default='Importer Test')
    ap.add_argument('--validate-only', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if not os.path.abspath(a.out).startswith(os.path.join(root, 'out') + os.sep) and \
            not os.path.abspath(a.out).startswith(os.path.join(REPO, 'out') + os.sep):
        raise SystemExit('--out must be under out\\ (gitignored): the pack is built from game files')
    if a.selftest:
        n = selftest(a)
        print('selftest: %d difference(s)' % n)
        return 1 if n else 0
    if not a.validate_only:
        build(a)
    print('validation of %s:' % a.out)
    n = validate(a)
    print('validation: %d failure(s)' % n)
    return 1 if n else 0


if __name__ == '__main__':
    sys.exit(main())
