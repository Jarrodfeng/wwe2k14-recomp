"""qasset: `q asset` = an index of the game's FILES (S103 QAS; Jarrod: "always be building q indexes ... an index of the whole game").

Probes kept writing scratch pac readers (a texture's alpha histogram, a bgEtc group, the evtobj / bgEtc ids). This indexes every pac of the player's own game
files ONCE, read-only, with our existing readers (tools\\wm30\\pac.py: XCTD / BPE / ZLIB / PACH walk; tools\\arena\\arena_files.py: the JBOY mesh + material
parameter reader and the stage-slot flag text), into out\\harness\\index\\asset.db (names, ids, counts and alpha statistics ONLY; never a byte of game data, never
committed; the player's machine builds it from the player's files, like sdk.db / index.db). It is built and refreshed by itself on use (a stat walk of the
pacs; only changed pacs are re-read); `q asset update` forces it.

    py tools/q/q.py asset find WORDS...            names across all levels: pac paths, PACH virtual paths (/STG/CAGE), model node names, material effects /
                                                   parameter names, texture names (all words must match one name)
    py tools/q/q.py asset pac PATH [PACH [ENTRY]]  browse: PATH = a pac (ch139, bgEtc, pac\\bg\\bgEtc.pac) -> its PACHs; PACH = index or virtual-path word
                                                   (3, CELL, STG/CELL) -> its entries; ENTRY = entry path or key (1/0, 10) -> a model's nodes / meshes /
                                                   materials, a texture directory's textures
    py tools/q/q.py asset tex NAME                 a texture: format, size, mips, alpha max / share 0 / share >= 224 / mean, where it lives (NAME = exact, or
                                                   a substring with --like)
    py tools/q/q.py asset mat NAME [PAC] [--pac P] a material class (effect, e.g. yCh_Reflect) or the material of a texture name: parameters with their
                                                   values, sampler -> texture; PAC (a pac stem like ch139) or --pac narrows it
    py tools/q/q.py asset id evtobj|bgetc|weapon N an id -> its PACH and entries (evtobj SOBJ / BOBJ / SPMT ids; bgEtc PACH number; stage weapon key N in
                                                   bgEtc WPON / WPBS / WPO2)
    py tools/q/q.py asset slot ARENA [N]           the arena slot flags of bgNN.pac (the 50001 text)
    py tools/q/q.py asset info                     age, counts, pacs that could not be read
    py tools/q/q.py asset update [-j N] [--full]   force a refresh
Every answer's first line says how old the index is and how many pacs were re-read just now. At most 4 workers (fewer when free RAM is low), one pac per worker at a
time. In a worktree the game files and the db are the MAIN checkout's.
"""
import json
import os
import re
import sqlite3
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for _d in ('wm30', 'arena', 'filecensus'):
    sys.path.insert(0, os.path.join(ROOT, 'tools', _d))

SCHEMA = 3
MAX_JOBS = 4
# pacs whose entries are listed (PACH / key / size) but not decoded: motion / sound / movie data and parameter tables (m3), no models or textures (measured: decoding them is
# >40 % of the cold build for nothing a probe asks). Their entries show as kind 'raw'. m2.pac IS decoded: it holds the weapon models (p0 key 1000 / id, LoadM2Weapon).
RAW_PACS = re.compile(r'(^|/)(mot\d+|dlc_mot\d+|sound|evtmov\d*(_\d+)?|m3|mvmot|string|wunidata)\.pac$', re.I)
PAC_MAGICS = (b'EPAC', b'EPK8')
MAX_DEPTH = 6


def game_dir():
    r"""<checkout with the game>\game_data (WWE_QASSET_GAME = another folder with extracted\pac under it: the selftest's fixture)"""
    if os.environ.get('WWE_QASSET_GAME'):
        return os.environ['WWE_QASSET_GAME']
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
    import pac
    return os.path.dirname(pac.GAME)


def main_root():
    return os.path.dirname(game_dir())


def db_path():
    r"""out\harness\index\asset.db of the checkout that holds the game (WWE_QASSET_DB = another file: the selftest's)"""
    if os.environ.get('WWE_QASSET_DB'):
        return os.environ['WWE_QASSET_DB']
    return os.path.join(main_root(), 'out', 'harness', 'index', 'asset.db')


# ------------------------------------------------------------------------------------------------------------------------------- reading (worker side)

def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def cstr(b):
    return bytes(b).split(b'\0')[0].decode('latin1')


def read_dir(data):
    """The EPAC header's own directory (at 0x800, little-endian; the same rows plist360.arc holds big-endian): [(tag, name, sector, size256)].
    Section = tag[4], LE u32 w = (u16 entry bytes / 4) | (u16 kind << 16; kind 1 = 16-byte rows with 8-character names, else 12-byte rows), 4 zero bytes, then rows
    {name, LE sector (2 KB units from 0x4000), LE size (256 B units)}. Matches plist360.arc (tools/goto/scene_ids.read_arc) on every listed pac. EPAC and EPK8 alike."""
    out = []
    p = 0x800
    while p + 12 <= 0x4000:
        tag = data[p:p + 4]
        if not tag or tag[0] == 0 or not all(32 <= c < 127 for c in tag):
            break
        w = struct.unpack_from('<I', data, p + 4)[0]
        p += 12
        rs = 16 if (w >> 16) & 1 else 12
        n = (w & 0xFFFF) * 4 // rs
        for _ in range(n):
            nm = data[p:p + rs - 8].split(b'\0')[0].decode('latin1').strip()
            sec, sz = struct.unpack_from('<II', data, p + rs - 8)
            out.append((tag.decode('latin1').strip(), nm, sec, sz))
            p += rs
    return out


def dxt_alpha(kind, blob, w, h):
    """-> (max, zero share, >=224 share, mean 0..255) of the top mip, or None for a format without alpha"""
    import numpy as np
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if kind == 'DXT5':
        raw = np.frombuffer(blob, np.uint8, count=bw * bh * 16, offset=128).reshape(-1, 16)
        a0, a1 = raw[:, 0].astype(np.int32), raw[:, 1].astype(np.int32)
        bits = np.zeros(len(raw), np.uint64)
        for i in range(6):
            bits |= raw[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        idx = ((bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)).astype(np.int64)
        tab = np.zeros((len(raw), 8), np.int32)
        tab[:, 0], tab[:, 1] = a0, a1
        hi = a0 > a1
        for i in range(2, 8):
            v8 = ((8 - i) * a0 + (i - 1) * a1) // 7
            if i < 6:
                v6 = ((6 - i) * a0 + (i - 1) * a1) // 5
            else:
                v6 = np.zeros_like(a0) if i == 6 else np.full_like(a0, 255)
            tab[:, i] = np.where(hi, v8, v6)
        a = np.take_along_axis(tab, idx, 1)
    elif kind == 'DXT1':
        # 1-bit alpha: a block with c0 <= c1 makes index 3 transparent (alpha 0); every other texel is opaque (255)
        raw = np.frombuffer(blob, np.uint8, count=bw * bh * 8, offset=128).reshape(-1, 8)
        c0 = raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8)
        c1 = raw[:, 2].astype(np.int32) | (raw[:, 3].astype(np.int32) << 8)
        bits = raw[:, 4].astype(np.uint32) | (raw[:, 5].astype(np.uint32) << 8) | (raw[:, 6].astype(np.uint32) << 16) | (raw[:, 7].astype(np.uint32) << 24)
        idx = (bits[:, None] >> (np.uint32(2) * np.arange(16, dtype=np.uint32))[None, :]) & np.uint32(3)
        a = np.where((c0 <= c1)[:, None] & (idx == 3), 0, 255).astype(np.int32)
    elif kind == 'DXT3':
        raw = np.frombuffer(blob, np.uint8, count=bw * bh * 16, offset=128).reshape(-1, 16)[:, :8]
        a = np.empty((len(raw), 16), np.int32)
        a[:, 0::2] = (raw & 15).astype(np.int32) * 17
        a[:, 1::2] = (raw >> 4).astype(np.int32) * 17
    elif kind in ('BGRA8', 'RGBA8'):
        raw = np.frombuffer(blob, np.uint8, count=w * h * 4, offset=128)
        a = raw[3::4].astype(np.int32)
    elif kind == 'BGRA4':
        raw = np.frombuffer(blob, '<u2', count=w * h, offset=128)
        a = (raw >> 12).astype(np.int32) * 17
    else:
        return None
    return int(a.max()), float((a == 0).mean()), float((a >= 224).mean()), float(a.mean())


def dds_info(blob):
    """-> (fmt, w, h, mips) of a DDS blob"""
    if blob[:4] != b'DDS ' or len(blob) < 128:
        raise ValueError('not a DDS')
    h, w = struct.unpack_from('<II', blob, 12)
    flags = struct.unpack_from('<I', blob, 8)[0]
    mips = struct.unpack_from('<I', blob, 28)[0] if flags & 0x20000 else 1
    fc = bytes(blob[84:88])
    bits, rm, gm, bm, am = struct.unpack_from('<5I', blob, 88)
    if fc in (b'DXT1', b'DXT3', b'DXT5'):
        fmt = fc.decode()
    elif fc == b'\0\0\0\0' and bits == 32 and am:
        fmt = 'BGRA8' if rm == 0x00FF0000 else 'RGBA8'
    elif fc == b'\0\0\0\0' and bits == 32:
        fmt = 'BGRX8'
    elif fc == b'\0\0\0\0' and bits == 16 and am == 0xF000:
        fmt = 'BGRA4'
    else:
        fmt = 'fourcc ' + fc.hex() if fc != b'\0\0\0\0' else 'raw%d' % bits
    return fmt, w, h, mips


def parse_texdir(dec):
    """-> [(name, fmt, w, h, mips, bytes, alpha stats | None, note)]; a bad or truncated record is a row with a note (counted as an error by the caller), never a lost directory"""
    cnt = struct.unpack_from('<I', dec, 0)[0]
    out = []
    for i in range(cnt):
        r = 16 + 32 * i
        if r + 32 > len(dec):
            out.append(('?record %d' % i, '?', 0, 0, 0, 0, None, 'directory truncated before record %d of %d' % (i, cnt)))
            break
        name = cstr(dec[r:r + 16])
        size, off = struct.unpack_from('<II', dec, r + 20)
        try:
            blob = dec[off:off + size]
            if len(blob) < size:
                raise ValueError('blob past the end')
            fmt, w, h, mips = dds_info(blob)
            st = None
            if fmt in ('DXT1', 'DXT3', 'DXT5', 'BGRA8', 'RGBA8', 'BGRA4'):
                st = dxt_alpha(fmt, blob, w, h)
            out.append((name, fmt, w, h, mips, size, st, ''))
        except Exception as e:      # noqa: BLE001
            out.append((name, '?', 0, 0, 0, size, None, str(e)[:80]))
    return out


def is_texdir(dec):
    if len(dec) < 48:
        return False
    cnt = struct.unpack_from('<I', dec, 0)[0]
    return 1 <= cnt <= 8192 and dec[32:36] == b'dds\0'


EFFECT_OK = re.compile(r'^[A-Za-z_][A-Za-z0-9_.]{1,15}$')


def parse_mesh_rec(body, texs, rec, shift):
    """one mesh record -> (verts, effect, params, samplers) or None when it does not read as a mesh record. shift 0 = the standard layout (effect +0x7C, parameter count +0x94,
    list +0x98); shift 4 = the variant of bgEtc /STG3/WPO2 and two p62 models (every field from the effect on sits 4 bytes earlier: effect +0x78, count +0x90, list +0x94).
    Plausibility: n <= 2^20, a printable effect name, a count <= 512 and a list inside the body."""
    n = be32(body, rec)
    eo = 0x7C - shift
    raw = bytes(body[rec + eo:rec + eo + 16]).split(b'\0')[0]
    if n > (1 << 20) or not EFFECT_OK.match(raw.decode('latin1')):
        return None
    eff = raw.decode('latin1')
    nparam, plist = be32(body, rec + 0x94 - shift), be32(body, rec + 0x98 - shift)
    if nparam > 512 or plist + 4 * nparam > len(body):
        return None
    params, samp = {}, {}
    for k in range(nparam):
        po = be32(body, plist + 4 * k)
        pname = cstr(body[po:po + 16])
        tw = be32(body, po + 16) >> 16
        if tw == 0x0F:
            ti = struct.unpack_from('>i', body, po + 20)[0]
            samp[pname] = texs[ti] if 0 <= ti < len(texs) else ''
        elif tw == 0x0D:
            params[pname] = [round(x, 6) for x in struct.unpack_from('>4f', body, po + 20)]
        elif tw == 0x0A:
            params[pname] = [round(struct.unpack_from('>f', body, po + 20)[0], 6)]
    return n, eff, params, samp


def parse_model(dec):
    """-> dict(nnode, nmesh, nvert (None = suspect), ntex, nodes, textures, mats [(mesh, verts, effect, params, samplers)], bad (records that read as neither layout), shifted (records read as the variant)).
    Layout: src/assets/jboy.h header comment, tools/arena/arena_files.py parse_jboy, without the index / vertex streams. A record that fits neither layout is NOT turned into a
    row: it is counted in `bad`, and the caller marks the model suspect (nvert NULL) and the entry."""
    body = dec[8:8 + be32(dec, 4)]
    nmesh, mtab, nnode, ntex, ntab, ttab = (be32(body, o) for o in (0x10, 0x14, 0x18, 0x1C, 0x20, 0x24))
    nodes = [cstr(body[ntab + 80 * i:ntab + 80 * i + 16]) for i in range(min(nnode, 4096))]
    texs = [cstr(body[ttab + 16 * i:ttab + 16 * i + 16]) for i in range(min(ntex, 256))]
    # two layouts: the standard one (record 0xB8, effect +0x7C) and the shifted one of bgEtc /STG3/WPO2 + two p62 models (record 0xB4, every field from the effect on 4 bytes
    # earlier). The layout is chosen per MODEL: the one that reads every record (standard wins a tie); records that read as neither count as bad.
    best = None
    for shift, stride in ((0, 0xB8), (4, 0xB4)):
        got, nbad = [], 0
        for mi in range(min(nmesh, 512)):
            try:
                g = parse_mesh_rec(body, texs, mtab + mi * stride, shift)
            except (struct.error, IndexError):
                g = None
            if g is None:
                nbad += 1
            else:
                got.append((mi,) + g)
        if best is None or nbad < best[0]:
            best = (nbad, got, shift)
        if nbad == 0:
            break
    bad, mats, shifted = best[0], best[1], (len(best[1]) if best[2] else 0)
    nvert = None if (bad or nnode > 4096 or ntex > 256) else sum(m[1] for m in mats)
    return dict(nnode=nnode, nmesh=nmesh, nvert=nvert, ntex=ntex, nodes=nodes, textures=texs, mats=mats, bad=bad, shifted=shifted)


def pach_positions(data):
    """[(file offset, count, end)] of the PACH archives after the EPAC header (or one at 0), as pac.entries finds them"""
    import pac
    pos = 0x4000 if data[:4] in PAC_MAGICS else 0
    out = []
    while pos + 8 <= len(data):
        if data[pos:pos + 4] != b'PACH':
            pos += 0x800
            continue
        count = struct.unpack_from('<I', data, pos + 4)[0]
        first = pos + 8 + 12 * count
        end = first
        for i in range(count):
            _, off, size = struct.unpack_from('<III', data, pos + 8 + 12 * i)
            end = max(end, first + off + size)
        out.append((pos, count, end))
        pos = (end + 0x7FF) & ~0x7FF
    return out


def read_pac(path, rel):
    """Everything the index keeps about one pac -> dict of row lists. Raises only for a pac that cannot be opened at all."""
    import pac
    t0 = time.time()
    data = pac.load(path)
    raw_only = bool(RAW_PACS.search(rel))
    res = dict(pach=[], entry=[], model=[], mat=[], tex=[], flags=[], errs=0)
    arc = read_dir(data) if data[:4] in PAC_MAGICS else []
    by_pos = {0x4000 + sec * 0x800: (tag, nm) for tag, nm, sec, sz in arc}
    pos_list = pach_positions(data)
    for k, (pos, count, end) in enumerate(pos_list):
        tag, nm = by_pos.get(pos, ('', ''))
        vpath = '/%s/%s' % (tag, nm) if tag else ''
        res['pach'].append((k, pos, count, vpath, tag, nm))
        for eid, raw in pac.pach(data, pos):
            _walk(res, k, str(eid), eid, raw, 0, raw_only)
    # directory rows that are not a PACH (chHeads CASD, evt EVP / EVZ, evtmov bink): one row per entry with its tag and size, never "0 PACH, empty".
    # They number after the real PACHs (idx = nach + j); the size is the directory's own (256-byte units, so an upper bound).
    have = {pos for pos, _, _ in pos_list}
    j = len(pos_list)
    for tag, nm, sec, sz in arc:
        pos = 0x4000 + sec * 0x800
        if pos in have:
            continue
        head = bytes(data[pos:pos + 4]) if pos + 4 <= len(data) else b''
        t = head.decode('latin1') if head and all(32 <= c < 127 for c in head) else head.hex()
        res['pach'].append((j, pos, 1, '/%s/%s' % (tag, nm), tag, nm))
        res['entry'].append((j, '0', 0, 0, 'blob', t, sz * 256, 'directory row, not a PACH' + ('' if pos < len(data) else ' (offset past the end of the file)')))
        j += 1
    res['secs'] = time.time() - t0
    res['nach'] = len(pos_list)
    res['nblob'] = j - len(pos_list)
    res['size'] = len(data)
    return res


def _walk(res, k, epath, eid, raw, depth, raw_only):
    import pac
    tag = bytes(raw[:4])
    if raw_only:
        res['entry'].append((k, epath, eid, depth, 'raw', tag.decode('latin1', 'replace'), len(raw), ''))
        return
    try:
        dec = pac.decode(raw)
    except Exception as e:      # noqa: BLE001
        res['errs'] += 1
        res['entry'].append((k, epath, eid, depth, 'bad', tag.decode('latin1', 'replace'), len(raw), 'decode: ' + str(e)[:80]))
        return
    t = bytes(dec[:4])
    if t == b'PACH' and depth < MAX_DEPTH:
        n = struct.unpack_from('<I', dec, 4)[0]
        res['entry'].append((k, epath, eid, depth, 'pach', 'PACH', len(dec), str(n)))
        for ceid, craw in pac.pach(dec):
            _walk(res, k, epath + '/' + str(ceid), ceid, craw, depth + 1, False)
    elif t == b'JBOY':
        note = ''
        try:
            m = parse_model(dec)
            res['model'].append((k, epath, m['nnode'], m['nmesh'], m['nvert'], m['ntex'], ' '.join(m['nodes']), ' '.join(m['textures'])))
            for mesh, verts, eff, params, samp in m['mats']:
                res['mat'].append((k, epath, mesh, verts, eff, json.dumps(params, sort_keys=True), json.dumps(samp, sort_keys=True)))
            if m['bad']:
                res['errs'] += 1
                note = 'SUSPECT: %d of %d mesh records read as neither layout (vertex count unknown)' % (m['bad'], m['nmesh'])
            elif m['shifted']:
                note = '%d mesh record(s) in the shifted layout (effect +0x78)' % m['shifted']
        except Exception as e:      # noqa: BLE001
            res['errs'] += 1
            note = 'SUSPECT jboy: ' + str(e)[:80]
        res['entry'].append((k, epath, eid, depth, 'model', 'JBOY', len(dec), note))
    elif is_texdir(dec):
        note = ''
        try:
            tx = parse_texdir(dec)
            for name, fmt, w, h, mips, size, st, nt in tx:
                res['tex'].append((k, epath, name, name.lower(), fmt, w, h, mips, size) + (st if st else (None, None, None, None)) + (nt,))
            nbad = sum(1 for t in tx if t[7])
            note = str(len(tx)) + (' (%d bad records)' % nbad if nbad else '')
            res['errs'] += 1 if nbad else 0
        except Exception as e:      # noqa: BLE001
            res['errs'] += 1
            note = 'texdir: ' + str(e)[:80]
        res['entry'].append((k, epath, eid, depth, 'texdir', 'dds', len(dec), note))
    else:
        if eid == 50001 and depth == 0:
            try:
                import filecensus_flags
                fl = filecensus_flags.parse_slot_flags(bytes(dec).decode('latin1'))
                for slot, toks in fl.items():
                    res['flags'].append((k, int(slot), ' '.join(sorted(toks))))
            except Exception:       # noqa: BLE001
                res['errs'] += 1
        res['entry'].append((k, epath, eid, depth, 'other', t.decode('latin1', 'replace') if all(32 <= c < 127 for c in t) else t.hex(), len(dec), ''))


def _worker(job):
    rel, path = job
    try:
        return rel, read_pac(path, rel), None
    except Exception as e:      # noqa: BLE001
        return rel, None, '%s: %s' % (type(e).__name__, str(e)[:200])


# ------------------------------------------------------------------------------------------------------------------------------- the db

DDL = '''
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE pacs(id INTEGER PRIMARY KEY, path TEXT UNIQUE, size INTEGER, mtime_ns INTEGER, nach INTEGER, err TEXT, raw INTEGER, secs REAL, nerr INTEGER);
CREATE TABLE pach(id INTEGER PRIMARY KEY, pac_id INTEGER, idx INTEGER, off INTEGER, nent INTEGER, vpath TEXT, arc_tag TEXT, arc_name TEXT);
CREATE TABLE entry(id INTEGER PRIMARY KEY, pach_id INTEGER, path TEXT, eid INTEGER, depth INTEGER, kind TEXT, tag TEXT, size INTEGER, note TEXT);
CREATE TABLE model(entry_id INTEGER PRIMARY KEY, nnode INTEGER, nmesh INTEGER, nvert INTEGER, ntex INTEGER, nodes TEXT, textures TEXT);
CREATE TABLE material(id INTEGER PRIMARY KEY, entry_id INTEGER, mesh INTEGER, verts INTEGER, effect TEXT, effect_lc TEXT, params TEXT, samplers TEXT);
CREATE TABLE texture(id INTEGER PRIMARY KEY, entry_id INTEGER, name TEXT, name_lc TEXT, fmt TEXT, w INTEGER, h INTEGER, mips INTEGER, bytes INTEGER,
                     a_max INTEGER, a_zero REAL, a_ge224 REAL, a_mean REAL, note TEXT);
CREATE TABLE flags(pac_id INTEGER, slot INTEGER, tokens TEXT);
CREATE INDEX i_pach ON pach(pac_id, idx);
CREATE INDEX i_entry ON entry(pach_id, path);
CREATE INDEX i_entry_eid ON entry(eid);
CREATE INDEX i_mat ON material(effect_lc);
CREATE INDEX i_mat_e ON material(entry_id);
CREATE INDEX i_tex ON texture(name_lc);
CREATE INDEX i_tex_e ON texture(entry_id);
CREATE INDEX i_pach_v ON pach(vpath);
'''


def scan():
    """{rel path (forward slashes): (abs path, size, mtime_ns)} of every pac under game_data (extracted\\pac + dlc)"""
    g = game_dir()
    out = {}
    for top in ('extracted', 'dlc'):
        base = os.path.join(g, top)
        for dp, dn, fn in os.walk(base):
            for f in fn:
                if f.lower().endswith('.pac'):
                    fp = os.path.join(dp, f)
                    st = os.stat(fp)
                    rel = os.path.relpath(fp, base).replace('\\', '/')
                    if top == 'dlc':
                        rel = 'dlc/' + rel
                    out[rel] = (fp, st.st_size, st.st_mtime_ns)
    return out


def safe_jobs(want):
    """at most MAX_JOBS workers; fewer when free RAM is low (a worker holds one decoded pac, up to ~0.5 GB, plus numpy: 1.5 GB each with 4 GB left over)"""
    try:
        import build_ours
        free = build_ours._free_ram_gb()
    except Exception:       # noqa: BLE001
        free = None
    by_ram = MAX_JOBS if free is None else int((free - 4) // 1.5)
    return max(1, min(want or MAX_JOBS, MAX_JOBS, by_ram, (os.cpu_count() or 4)))


def pid_alive(pid):
    """True when a process with this pid exists (Windows: OpenProcess + exit code; elsewhere signal 0)"""
    if pid <= 0:
        return False
    try:
        if os.name == 'nt':
            import ctypes
            h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)       # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                return False
            code = ctypes.c_ulong()
            ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
            ctypes.windll.kernel32.CloseHandle(h)
            return bool(ok) and code.value == 259                              # STILL_ACTIVE
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def lock_is_stale(lk):
    """a lock is a dead writer's when its owner pid is gone; a live owner that has not touched it for an hour is a hung one"""
    try:
        pid = int(open(lk).read().strip() or 0)
    except (OSError, ValueError):
        pid = 0
    try:
        age = time.time() - os.path.getmtime(lk)
    except OSError:
        return False
    return (not pid_alive(pid)) or age > 3600


def acquire_lock(path, wait_s=None):
    """one writer at a time: a lock file (owner pid) next to the db. -> the lock path, or None when a live writer still holds it after wait_s (queries wait <= 10 s)"""
    lk = path + '.lock'
    t0 = time.time()
    if wait_s is None:
        wait_s = float(os.environ.get('WWE_QASSET_LOCK_WAIT', '10'))
    while True:
        try:
            fd = os.open(lk, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            return lk
        except FileExistsError:
            try:
                if lock_is_stale(lk):
                    os.remove(lk)
                    continue
            except OSError:
                pass
            if time.time() - t0 > wait_s:
                return None
            time.sleep(0.3)
        except OSError:
            return None


def touch_lock(lk):
    if lk:
        try:
            os.utime(lk, None)
        except OSError:
            pass


def _insert(con, rel, st, res, err):
    """replace everything of one pac by its new rows (one transaction)"""
    row = con.execute('SELECT id FROM pacs WHERE path=?', (rel,)).fetchone()
    if row:
        pid = row[0]
        pids = [r[0] for r in con.execute('SELECT id FROM pach WHERE pac_id=?', (pid,))]
        for ph in pids:
            eids = [r[0] for r in con.execute('SELECT id FROM entry WHERE pach_id=?', (ph,))]
            for chunk in range(0, len(eids), 500):
                c = eids[chunk:chunk + 500]
                q = ','.join('?' * len(c))
                for tb in ('model', 'material', 'texture'):
                    con.execute('DELETE FROM %s WHERE entry_id IN (%s)' % (tb, q), c)
            con.execute('DELETE FROM entry WHERE pach_id=?', (ph,))
        con.execute('DELETE FROM pach WHERE pac_id=?', (pid,))
        con.execute('DELETE FROM flags WHERE pac_id=?', (pid,))
        con.execute('DELETE FROM pacs WHERE id=?', (pid,))
    cur = con.execute('INSERT INTO pacs(path,size,mtime_ns,nach,err,raw,secs,nerr) VALUES(?,?,?,?,?,?,?,?)',
                      (rel, st[1], st[2], res['nach'] if res else 0, err, 1 if RAW_PACS.search(rel) else 0, res['secs'] if res else 0, res['errs'] if res else 0))
    pid = cur.lastrowid
    if not res:
        return
    pach_id = {}
    nent = {}
    for e in res['entry']:
        if e[3] == 0:
            nent[e[0]] = nent.get(e[0], 0) + 1
    for k, pos, count, vpath, tag, nm in res['pach']:
        c = con.execute('INSERT INTO pach(pac_id,idx,off,nent,vpath,arc_tag,arc_name) VALUES(?,?,?,?,?,?,?)', (pid, k, pos, count, vpath, tag, nm))
        pach_id[k] = c.lastrowid
    emap = {}
    for k, epath, eid, depth, kind, tag, size, note in res['entry']:
        c = con.execute('INSERT INTO entry(pach_id,path,eid,depth,kind,tag,size,note) VALUES(?,?,?,?,?,?,?,?)', (pach_id[k], epath, eid, depth, kind, tag, size, note))
        emap[(k, epath)] = c.lastrowid
    con.executemany('INSERT INTO model(entry_id,nnode,nmesh,nvert,ntex,nodes,textures) VALUES(?,?,?,?,?,?,?)',
                    [(emap[(k, ep)], a, b, c_, d, e, f) for k, ep, a, b, c_, d, e, f in res['model']])
    con.executemany('INSERT INTO material(entry_id,mesh,verts,effect,effect_lc,params,samplers) VALUES(?,?,?,?,?,?,?)',
                    [(emap[(k, ep)], m, v, eff, eff.lower(), p, s) for k, ep, m, v, eff, p, s in res['mat']])
    con.executemany('INSERT INTO texture(entry_id,name,name_lc,fmt,w,h,mips,bytes,a_max,a_zero,a_ge224,a_mean,note) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)',
                    [(emap[(k, ep)],) + tuple(t) for k, ep, *t in res['tex']])
    con.executemany('INSERT INTO flags(pac_id,slot,tokens) VALUES(?,?,?)', [(pid, s, t) for k, s, t in res['flags']])


def build(jobs=None, full=False, only=None, budget_s=None, out=sys.stderr, lock=None, redo=False):
    """(re)index the pacs that are new or changed; -> (pacs read, pacs left, seconds). A budget stops the run between pacs (the next call resumes)."""
    t0 = time.time()
    path = db_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    now = scan()
    con = sqlite3.connect(path)
    have = False
    try:
        have = con.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0] == str(SCHEMA)
    except sqlite3.Error:
        have = False
    if not have or full:
        con.close()
        for s in ('', '-journal', '-wal', '-shm'):
            try:
                os.remove(path + s)
            except OSError:
                pass
        con = sqlite3.connect(path)
        con.executescript(DDL)
        con.execute("INSERT INTO meta VALUES('schema', ?)", (str(SCHEMA),))
        con.commit()
    old = {r[0]: (r[1], r[2]) for r in con.execute('SELECT path,size,mtime_ns FROM pacs')}
    todo = [r for r, (fp, sz, mt) in now.items() if (old.get(r) != (sz, mt) or (redo and only)) and (not only or only.lower() in r.lower())]
    gone = [r for r in old if r not in now]
    for r in gone:
        _insert(con, r, (0, 0, 0), None, 'gone')
        con.execute('DELETE FROM pacs WHERE path=?', (r,))
    todo.sort(key=lambda r: -now[r][1])
    n = safe_jobs(jobs)
    done = 0
    print('q asset: indexing %d pacs (%d workers)' % (len(todo), n), file=out)
    if todo:
        import multiprocessing as mp
        ctx = mp.get_context('spawn')
        deadline = t0 + budget_s if budget_s else None
        with ctx.Pool(n, maxtasksperchild=6) as pool:
            it = pool.imap_unordered(_worker, [(r, now[r][0]) for r in todo])
            while done < len(todo):
                try:
                    # the budget is a wall-clock deadline, checked while waiting for a result (one huge pac cannot overrun it)
                    rel, res, err = it.next(timeout=None if deadline is None else max(0.2, deadline - time.time()))
                except mp.TimeoutError:
                    pool.terminate()
                    break
                _insert(con, rel, now[rel], res, err)
                con.commit()
                touch_lock(lock)
                done += 1
                if err:
                    print('q asset: %s: %s' % (rel, err), file=out)
                if done % 25 == 0:
                    print('q asset: %d / %d pacs (%.0f s)' % (done, len(todo), time.time() - t0), file=out)
                if deadline and time.time() > deadline:
                    pool.terminate()
                    break
    con.execute("INSERT OR REPLACE INTO meta VALUES('built', ?)", (time.strftime('%Y-%m-%d %H:%M:%S'),))
    con.execute("INSERT OR REPLACE INTO meta VALUES('built_ts', ?)", (str(int(time.time())),))
    con.execute("INSERT OR REPLACE INTO meta VALUES('game_dir', ?)", (game_dir(),))
    con.commit()
    con.close()
    return done, len(todo) - done, time.time() - t0


def query_budget():
    """a query that has to build may spend min(90 s, half of q's own watchdog) (WWE_QASSET_BUDGET overrides)"""
    if os.environ.get('WWE_QASSET_BUDGET'):
        return float(os.environ['WWE_QASSET_BUDGET'])
    try:
        import qtimeout
        t = qtimeout.timeout_secs()
    except Exception:       # noqa: BLE001
        t = 120
    return min(90.0, 0.5 * t) if t else 90.0


def ensure_fresh():
    """-> (state, changed, left, seconds): 'fresh' | 'refreshed' | 'built' | 'PARTIAL' | 'STALE'"""
    path = db_path()
    t0 = time.time()
    now = scan()
    if not now:
        return 'fresh', 0, 0, 0.0
    have = os.path.exists(path)
    old = None
    if have:
        try:
            c0 = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
            if c0.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0] == str(SCHEMA):
                old = {r[0]: (r[1], r[2]) for r in c0.execute('SELECT path,size,mtime_ns FROM pacs')}
            c0.close()
        except (sqlite3.Error, TypeError):
            old = None
    stale = [r for r, (fp, sz, mt) in now.items() if (old or {}).get(r) != (sz, mt)] + [r for r in (old or {}) if r not in now]
    if old is not None and not stale:
        return 'fresh', 0, 0, time.time() - t0
    lk = acquire_lock(path)
    if lk is None:
        return 'STALE', len(stale), len(stale), time.time() - t0
    try:
        done, left, secs = build(budget_s=query_budget(), lock=lk)
    except (SystemExit, Exception) as e:      # noqa: BLE001
        print('q asset: the refresh failed: %s' % e, file=sys.stderr)
        return 'STALE', len(stale), len(stale), time.time() - t0
    finally:
        try:
            os.remove(lk)
        except OSError:
            pass
    if left:
        return 'PARTIAL', len(stale), left, time.time() - t0
    return ('refreshed' if old is not None else 'built'), len(stale), 0, time.time() - t0


HEADER = ''


def age_text(secs):
    if secs < 90:
        return '%d s' % secs
    if secs < 5400:
        return '%d min' % (secs // 60)
    if secs < 172800:
        return '%.1f h' % (secs / 3600)
    return '%.1f days' % (secs / 86400)


def connect():
    global HEADER
    if os.environ.get('WWE_QASSET_NOREFRESH') and os.path.exists(db_path()):      # tests / debugging: answer from what is there
        state, nchg, left, secs = 'fresh', 0, 0, 0.0
    else:
        state, nchg, left, secs = ensure_fresh()
    p = db_path()
    if not os.path.exists(p):
        raise SystemExit('q asset: no index and no game files were found (game_data\\extracted\\pac in this checkout or the main one)')
    con = sqlite3.connect('file:%s?mode=ro' % p, uri=True)
    con.row_factory = sqlite3.Row
    m = dict(con.execute('SELECT key, value FROM meta').fetchall())
    if m.get('schema') != str(SCHEMA):
        raise SystemExit('q asset: the index has schema %s, this q needs %d: run  py tools/q/q.py asset update --full' % (m.get('schema'), SCHEMA))
    try:
        age = age_text(time.time() - int(m.get('built_ts', '0')))
    except ValueError:
        age = '?'
    npacs = con.execute('SELECT COUNT(*) FROM pacs').fetchone()[0]
    if state == 'fresh':
        what = '0 pacs changed since (%.1f s check)' % secs
    elif state == 'STALE':
        what = 'WARNING STALE: %d pacs changed and the refresh did not run (another writer, or it failed): py tools/q/q.py asset update' % nchg
    elif state == 'PARTIAL':
        what = 'WARNING PARTIAL: %d pacs still to read after the %d s budget; answers miss them: run  py tools/q/q.py asset update' % (left, query_budget())
    else:
        what = '%s just now: %d pacs re-read in %.1f s' % (state, nchg, secs)
    HEADER = '[asset index built %s (%s ago), %d pacs; %s]' % (m.get('built', '?'), age, npacs, what)
    return con


# ------------------------------------------------------------------------------------------------------------------------------- queries

def pct(x):
    return '-' if x is None else ('%.1f%%' % (100 * x))


def pac_rows(con, frag):
    """pacs matching FRAG: an exact stem (ch139, bgEtc) first, then an exact path tail, then a substring"""
    f = frag.lower().replace('\\', '/').strip('/')
    rows = con.execute('SELECT * FROM pacs ORDER BY path').fetchall()
    stem = [r for r in rows if os.path.basename(r['path'])[:-4].lower() == f]
    if stem:
        return stem
    base = f[:-4] if f.endswith('.pac') else f
    tail = [r for r in rows if r['path'][:-4].lower().endswith(base) and (r['path'][:-4].lower() == base or r['path'][:-4].lower().endswith('/' + base))]
    if tail:
        return tail
    return [r for r in rows if base in r['path'].lower()]


def loc(r):
    """a one-line location for a joined row (pac, pach idx, vpath, entry path)"""
    return '%s PACH %d%s entry %s' % (r['pac'], r['pidx'], (' ' + r['vpath']) if r['vpath'] else '', r['epath'])


LOC_SQL = ('SELECT pacs.path AS pac, pach.idx AS pidx, pach.vpath AS vpath, entry.path AS epath, entry.id AS eid_row, entry.kind AS kind FROM entry '
           'JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id')


def cmd_find(con, words, limit):
    ws = [w.lower() for w in words]
    like = lambda col: ' AND '.join('lower(%s) LIKE ?' % col for _ in ws)      # noqa: E731
    args = ['%' + w + '%' for w in ws]
    out = {'words': words, 'pacs': [], 'pach': [], 'nodes': [], 'effects': [], 'params': [], 'textures': []}
    out['pacs'] = [r['path'] for r in con.execute('SELECT path FROM pacs WHERE %s LIMIT 200' % like('path'), args)]
    for r in con.execute('SELECT pacs.path AS pac, pach.idx AS idx, pach.vpath AS vpath, pach.nent AS nent FROM pach JOIN pacs ON pach.pac_id=pacs.id WHERE pach.vpath != "" AND %s LIMIT 200' % like('pach.vpath'), args):
        out['pach'].append(dict(r))
    # nodes: the whole name must hold every word
    for r in con.execute(LOC_SQL + ' JOIN model ON model.entry_id = entry.id WHERE %s LIMIT 2000' % like('model.nodes'), args):
        nodes = con.execute('SELECT nodes FROM model WHERE entry_id=?', (r['eid_row'],)).fetchone()[0].split(' ')
        hit = sorted({n for n in nodes if all(w in n.lower() for w in ws)})
        if hit:
            out['nodes'].append(dict(loc=loc(r), names=hit[:8], n=len(hit)))
    for r in con.execute('SELECT effect, COUNT(*) AS n, COUNT(DISTINCT entry_id) AS ents FROM material WHERE %s GROUP BY effect_lc ORDER BY n DESC LIMIT 100' % like('effect'), args):
        out['effects'].append(dict(r))
    pn = {}
    for r in con.execute('SELECT params, samplers, effect FROM material WHERE ' + ' AND '.join('(lower(params) LIKE ? OR lower(samplers) LIKE ?)' for _ in ws), [a for w in args for a in (w, w)]):
        try:
            names = list(json.loads(r['params']).keys()) + list(json.loads(r['samplers']).keys())
        except ValueError:
            continue
        for n in names:
            if all(w in n.lower() for w in ws):
                pn[n] = pn.get(n, 0) + 1
    out['params'] = [dict(name=k, materials=v) for k, v in sorted(pn.items(), key=lambda x: -x[1])[:30]]
    for r in con.execute('SELECT name, COUNT(*) AS n, MIN(fmt) AS fmt, MIN(w) AS w, MIN(h) AS h FROM texture WHERE %s GROUP BY name_lc ORDER BY n DESC, name LIMIT 100' % like('name_lc'), args):
        out['textures'].append(dict(r))
    return out


def print_find(r, a):
    print('asset find %s  %s' % (' '.join(r['words']), HEADER))
    L = a.limit
    for key, title in (('pacs', 'pac files'), ('pach', 'PACH virtual paths'), ('nodes', 'model node names'), ('effects', 'material effects'), ('params', 'material parameter / sampler names'), ('textures', 'textures')):
        rows = r[key]
        print('%s: %d' % (title, len(rows)))
        for x in rows[:L]:
            if key == 'pacs':
                print('  ' + x)
            elif key == 'pach':
                print('  %s PACH %d %s (%d entries)' % (x['pac'], x['idx'], x['vpath'], x['nent']))
            elif key == 'nodes':
                print('  %s: %s%s' % (x['loc'], ' '.join(x['names']), ' (+%d)' % (x['n'] - len(x['names'])) if x['n'] > len(x['names']) else ''))
            elif key == 'effects':
                print('  %s: %d meshes in %d models' % (x['effect'], x['n'], x['ents']))
            elif key == 'params':
                print('  %s: %d materials' % (x['name'], x['materials']))
            else:
                print('  %s: %s %dx%d, %d occurrence(s)' % (x['name'], x['fmt'], x['w'], x['h'], x['n']))
        if len(rows) > L:
            print('  ... %d more (--limit)' % (len(rows) - L))


def pach_rows(con, pac_id, sel):
    if sel.isdigit():
        return con.execute('SELECT * FROM pach WHERE pac_id=? AND idx=?', (pac_id, int(sel))).fetchall()
    s = sel.lower().strip('/')
    rows = con.execute('SELECT * FROM pach WHERE pac_id=? AND vpath != "" ORDER BY idx', (pac_id,)).fetchall()
    ex = [r for r in rows if r['vpath'].lower().strip('/') == s or r['arc_name'].lower() == s]
    return ex or [r for r in rows if s in r['vpath'].lower()]


def entry_summary(con, e):
    """one line about an entry row"""
    if e['kind'] == 'model':
        m = con.execute('SELECT * FROM model WHERE entry_id=?', (e['id'],)).fetchone()
        if not m:
            return 'model'
        return 'model: %d nodes, %d meshes, %s, %d texture names%s' % (m['nnode'], m['nmesh'], '%d vertices' % m['nvert'] if m['nvert'] is not None else 'VERTEX COUNT UNKNOWN (suspect)', m['ntex'], '  [%s]' % e['note'] if e['note'] else '')
    if e['kind'] == 'texdir':
        n = con.execute('SELECT COUNT(*) FROM texture WHERE entry_id=?', (e['id'],)).fetchone()[0]
        return 'texture directory: %d textures' % n
    if e['kind'] == 'pach':
        return 'nested PACH: %s entries' % e['note']
    if e['kind'] == 'raw':
        return 'not decoded (%s, motion / sound data)' % e['tag']
    return '%s %s' % (e['kind'], e['tag'] + (' ' + e['note'] if e['note'] else ''))


def cmd_pac(con, args):
    prs = pac_rows(con, args[0])
    if not prs:
        return {'error': 'no pac matches "%s"' % args[0]}
    if len(prs) > 1 and len(args) > 1:
        return {'error': 'several pacs match "%s": %s' % (args[0], ', '.join(r['path'] for r in prs[:12]))}
    out = {'pacs': []}
    for p in prs[:25]:
        d = dict(pac=p['path'], size=p['size'], nach=p['nach'], err=p['err'], raw=bool(p['raw']), nerr=p['nerr'],
                 nblob=con.execute('SELECT COUNT(*) FROM entry JOIN pach ON entry.pach_id=pach.id WHERE pach.pac_id=? AND entry.kind="blob"', (p['id'],)).fetchone()[0])
        if len(args) == 1:
            d['pach'] = []
            for ph in con.execute('SELECT * FROM pach WHERE pac_id=? ORDER BY idx', (p['id'],)):
                c = con.execute('SELECT SUM(kind="model"), SUM(kind="texdir"), COUNT(*) FROM entry WHERE pach_id=?', (ph['id'],)).fetchone()
                d['pach'].append(dict(idx=ph['idx'], vpath=ph['vpath'], nent=ph['nent'], models=c[0] or 0, texdirs=c[1] or 0, entries=c[2] or 0))
        else:
            phs = pach_rows(con, p['id'], args[1])
            if not phs:
                return {'error': 'no PACH "%s" in %s' % (args[1], p['path'])}
            d['pachs'] = []
            for ph in phs[:4]:
                x = dict(idx=ph['idx'], vpath=ph['vpath'], nent=ph['nent'])
                ents = con.execute('SELECT * FROM entry WHERE pach_id=? ORDER BY id', (ph['id'],)).fetchall()
                if len(args) == 2:
                    x['entries'] = [dict(path=e['path'], eid=e['eid'], depth=e['depth'], kind=e['kind'], size=e['size'], summary=entry_summary(con, e)) for e in ents]
                else:
                    sel = [e for e in ents if e['path'] == args[2] or (args[2].isdigit() and e['eid'] == int(args[2]) and e['depth'] == 0) or e['path'].endswith('/' + args[2])]
                    x['detail'] = []
                    for e in sel[:6]:
                        dd = dict(path=e['path'], kind=e['kind'], size=e['size'], summary=entry_summary(con, e))
                        if e['kind'] == 'model':
                            m = con.execute('SELECT * FROM model WHERE entry_id=?', (e['id'],)).fetchone()
                            dd['nodes'] = m['nodes'].split(' ')
                            dd['textures'] = m['textures'].split(' ') if m['textures'] else []
                            dd['materials'] = [dict(mesh=r['mesh'], verts=r['verts'], effect=r['effect'], params=json.loads(r['params']), samplers=json.loads(r['samplers']))
                                               for r in con.execute('SELECT * FROM material WHERE entry_id=? ORDER BY mesh', (e['id'],))]
                        elif e['kind'] == 'texdir':
                            dd['textures'] = [dict(r) for r in con.execute('SELECT name,fmt,w,h,mips,bytes,a_max,a_zero,a_ge224,a_mean,note FROM texture WHERE entry_id=? ORDER BY id', (e['id'],))]
                        elif e['kind'] == 'pach':
                            dd['children'] = [dict(path=c['path'], kind=c['kind'], size=c['size'], summary=entry_summary(con, c)) for c in ents if c['path'].startswith(e['path'] + '/') and c['depth'] == e['depth'] + 1]
                        x['detail'].append(dd)
                    if not sel:
                        return {'error': 'no entry "%s" in %s PACH %d (entries: %s)' % (args[2], p['path'], ph['idx'], ', '.join(e['path'] for e in ents[:30]))}
                d['pachs'].append(x)
        out['pacs'].append(d)
    return out


def fmt_mat(m):
    ps = ['%s %s' % (k, ' '.join('%g' % x for x in v)) for k, v in m['params'].items()]
    ss = ['%s -> %s' % (k, v or '(none)') for k, v in m['samplers'].items()]
    return '%s [%d verts]  params: %s  samplers: %s' % (m['effect'], m['verts'], '; '.join(ps) or '-', '; '.join(ss) or '-')


def print_pac(r, a):
    if 'error' in r:
        print('asset pac: ' + r['error'])
        return
    L = a.limit * 4
    for d in r['pacs']:
        print('asset pac %s  (%s)  %s' % (d['pac'], '%.1f MB, %d PACH%s%s' % (d['size'] / 1e6, d['nach'], ' + %d non-PACH directory entries' % d['nblob'] if d['nblob'] else '', ', ENTRIES NOT DECODED (motion / sound)' if d['raw'] else '') + ('; READ ERROR ' + d['err'] if d['err'] else '') + ('; %d entry parse errors' % d['nerr'] if d['nerr'] else ''), HEADER))
        if 'pach' in d:
            for x in d['pach'][:L]:
                print('  PACH %-4d %-14s %4d entries  %s%s' % (x['idx'], x['vpath'] or '-', x['nent'], '%d models ' % x['models'] if x['models'] else '', '%d texture dirs' % x['texdirs'] if x['texdirs'] else ''))
            if len(d['pach']) > L:
                print('  ... %d more PACHs (--limit)' % (len(d['pach']) - L))
        for x in d.get('pachs', []):
            print('  PACH %d %s (%d entries)' % (x['idx'], x['vpath'] or '-', x['nent']))
            for e in x.get('entries', [])[:L]:
                print('    %s%-12s %9d B  %s' % ('  ' * e['depth'], e['path'], e['size'], e['summary']))
            if len(x.get('entries', [])) > L:
                print('    ... %d more entries (--limit)' % (len(x['entries']) - L))
            for dd in x.get('detail', []):
                print('    entry %s: %s, %d B' % (dd['path'], dd['summary'], dd['size']))
                if 'nodes' in dd:
                    print('      nodes (%d): %s%s' % (len(dd['nodes']), ' '.join(dd['nodes'][:60]), ' ...' if len(dd['nodes']) > 60 else ''))
                    print('      texture names: %s' % (' '.join(dd['textures']) or '-'))
                    for m in dd['materials'][:L]:
                        print('      mesh %d: %s' % (m['mesh'], fmt_mat(m)))
                for t in dd.get('textures', [])[:L] if 'nodes' not in dd else []:
                    print('      %s' % fmt_tex(t))
                for c in dd.get('children', [])[:L]:
                    print('      %s  %d B  %s' % (c['path'], c['size'], c['summary']))


def fmt_tex(t):
    if t.get('a_max') is None:
        al = 'alpha: none / not measured (%s)' % t['fmt']
    else:
        al = 'alpha max %d, share 0 %s, share >= 224 %s, mean %.1f' % (t['a_max'], pct(t['a_zero']), pct(t['a_ge224']), t['a_mean'])
    return '%s  %s %dx%d  %d mips  %d B  %s%s' % (t['name'], t['fmt'], t['w'], t['h'], t['mips'], t['bytes'], al, '  NOTE ' + t['note'] if t.get('note') else '')


def cmd_tex(con, name, like):
    n = name.lower()
    where, arg = ('name_lc LIKE ?', '%' + n + '%') if like else ('name_lc = ?', n)
    rows = con.execute('SELECT texture.*, pacs.path AS pac, pach.idx AS pidx, pach.vpath AS vpath, entry.path AS epath FROM texture JOIN entry ON texture.entry_id = entry.id JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id WHERE texture.' + where + ' ORDER BY pacs.path, pach.idx', (arg,)).fetchall()
    groups = {}
    for r in rows:
        k = (r['name_lc'], r['fmt'], r['w'], r['h'], r['mips'], r['a_max'], None if r['a_zero'] is None else round(r['a_zero'], 4), None if r['a_ge224'] is None else round(r['a_ge224'], 4))
        groups.setdefault(k, []).append(r)
    out = []
    for k, rs in groups.items():
        t = dict(rs[0])
        out.append(dict(tex={x: t[x] for x in ('name', 'fmt', 'w', 'h', 'mips', 'bytes', 'a_max', 'a_zero', 'a_ge224', 'a_mean', 'note')}, count=len(rs), where=[loc(r) for r in rs]))
    out.sort(key=lambda g: (-g['count'], g['tex']['name']))
    return {'name': name, 'like': like, 'groups': out}


def print_tex(r, a):
    print('asset tex %s%s: %d distinct texture(s)  %s' % (r['name'], ' (substring)' if r['like'] else '', len(r['groups']), HEADER))
    for g in r['groups'][:a.limit]:
        print('  ' + fmt_tex(g['tex']))
        print('    in %d place(s): %s%s' % (g['count'], '; '.join(g['where'][:4]), ' ...' if g['count'] > 4 else ''))
    if len(r['groups']) > a.limit:
        print('  ... %d more (--limit)' % (len(r['groups']) - a.limit))
    if not r['groups']:
        print('  none (names are matched case-insensitively; --like for a substring)')


def cmd_mat(con, name, pacs, like):
    n = name.lower()
    cond = 'material.effect_lc ' + ('LIKE ?' if like else '= ?')
    arg = ('%' + n + '%') if like else n
    sql = ('SELECT material.*, pacs.path AS pac, pach.idx AS pidx, pach.vpath AS vpath, entry.path AS epath FROM material JOIN entry ON material.entry_id = entry.id '
           'JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id WHERE (%s OR lower(material.samplers) LIKE ?)' % cond)
    args = [arg, '%"' + n + '"%' if not like else '%' + n + '%']
    if pacs:
        ids = [p['id'] for f in pacs for p in pac_rows(con, f)]
        if not ids:
            return {'error': 'no pac matches %s' % pacs}
        sql += ' AND pacs.id IN (%s)' % ','.join('?' * len(ids))
        args += ids
    rows = con.execute(sql + ' ORDER BY pacs.path, pach.idx, entry.id, material.mesh', args).fetchall()
    groups = {}
    for r in rows:
        k = (r['effect'], r['params'], r['samplers'])
        groups.setdefault(k, []).append(r)
    out = []
    for k, rs in groups.items():
        out.append(dict(effect=k[0], params=json.loads(k[1]), samplers=json.loads(k[2]), verts=rs[0]['verts'], count=len(rs), where=['%s PACH %d%s entry %s mesh %d' % (r['pac'], r['pidx'], (' ' + r['vpath']) if r['vpath'] else '', r['epath'], r['mesh']) for r in rs[:6]]))
    out.sort(key=lambda g: -g['count'])
    return {'name': name, 'groups': out, 'total': len(rows)}


def print_mat(r, a):
    if 'error' in r:
        print('asset mat: ' + r['error'])
        return
    print('asset mat %s: %d mesh materials in %d distinct (effect, parameters, samplers) groups  %s' % (r['name'], r['total'], len(r['groups']), HEADER))
    for g in r['groups'][:a.limit]:
        print('  x%d  %s' % (g['count'], fmt_mat(dict(g, verts=g['verts']))))
        print('       e.g. %s%s' % ('; '.join(g['where'][:3]), ' ...' if g['count'] > 3 else ''))
    if len(r['groups']) > a.limit:
        print('  ... %d more groups (--limit, or narrow with a pac)' % (len(r['groups']) - a.limit))


def cmd_id(con, kind, n):
    out = dict(kind=kind, id=n, hits=[])
    if kind == 'evtobj':
        name = '%04d' % n
        for r in con.execute('SELECT pach.*, pacs.path AS pac FROM pach JOIN pacs ON pach.pac_id=pacs.id WHERE pacs.path LIKE "%evtobj%" AND pach.arc_name = ? ORDER BY pacs.path', (name,)):
            out['hits'].append(('evtobj prop id %d: %s in %s -> PACH %d (%s)' % (n, r['arc_tag'], r['pac'], r['idx'], r['vpath']), r['pac_id'], r['idx'], None))
    elif kind == 'bgetc':
        for r in con.execute('SELECT pach.*, pacs.path AS pac FROM pach JOIN pacs ON pach.pac_id=pacs.id WHERE pacs.path LIKE "%bgetc.pac" AND pach.idx = ?', (n,)):
            out['hits'].append(('bgEtc PACH %d: %s in %s' % (n, r['vpath'] or '(no directory row)', r['pac']), r['pac_id'], r['idx'], None))
    elif kind == 'weapon':
        # (1) m2.pac: PACH 0 (/MOT/WEAP) nested key 1000, nested key N = the path LoadM2Weapon reads (src/assets/asset_load.h); (2) bgEtc WPON / WPBS / WPO2 key N = the
        # game's stage-weapon sets (NOT what LoadM2Weapon reads)
        sql = ('SELECT pach.*, pacs.path AS pac, entry.path AS epath, entry.kind AS kind, entry.depth AS depth FROM entry JOIN pach ON entry.pach_id=pach.id JOIN pacs ON pach.pac_id=pacs.id '
               'WHERE pacs.path LIKE "%/m2.pac" AND pach.idx = 0 AND entry.path = ? ORDER BY entry.id')
        for r in con.execute(sql, ('1000/%d' % n,)):
            out['hits'].append(('weapon %d: m2.pac PACH 0 %s key 1000/%d (%s): READ BY our loader (LoadM2Weapon)' % (n, r['vpath'], n, r['kind']), r['pac_id'], r['idx'], r['epath']))
        sql = ('SELECT pach.*, pacs.path AS pac, entry.path AS epath, entry.kind AS kind, entry.depth AS depth FROM entry JOIN pach ON entry.pach_id=pach.id JOIN pacs ON pach.pac_id=pacs.id '
               'WHERE pacs.path LIKE "%bgetc.pac" AND pach.arc_name LIKE "WP%" AND entry.eid = ? ORDER BY entry.depth, pach.idx, entry.id')
        rows = con.execute(sql, (n,)).fetchall()
        top = [r for r in rows if r['depth'] == 0]
        for r in (top or rows):
            out['hits'].append(('weapon %d: bgEtc PACH %d (%s), entry %s (%s): the game\'s stage-weapon set, not read by LoadM2Weapon' % (n, r['idx'], r['vpath'], r['epath'], r['kind']), r['pac_id'], r['idx'], r['epath']))
    else:
        return {'error': 'id kind must be evtobj, bgetc or weapon'}
    out['detail'] = []
    for text, pid, idx, sub in out['hits'][:6]:
        ents = con.execute('SELECT entry.* FROM entry JOIN pach ON entry.pach_id=pach.id WHERE pach.pac_id=? AND pach.idx=? ORDER BY entry.id', (pid, idx)).fetchall()
        if sub:         # a weapon key: that entry and what is under it
            ents = [e for e in ents if e['path'] == sub or e['path'].startswith(sub + '/')]
        nm = sum(1 for e in ents if e['kind'] == 'model')
        out['detail'].append(dict(models=nm, entries=[dict(path=e['path'], kind=e['kind'], size=e['size'], summary=entry_summary(con, e)) for e in ents]))
    return out


def print_id(r, a):
    if 'error' in r:
        print('asset id: ' + r['error'])
        return
    print('asset id %s %d: %d hit(s)  %s' % (r['kind'], r['id'], len(r['hits']), HEADER))
    for (text, pid, idx, sub), d in zip(r['hits'], r['detail']):
        print('  ' + text)
        print('    %d entries, %d models' % (len(d['entries']), d['models']))
        for e in d['entries'][:a.limit]:
            print('    %-10s %9d B  %s' % (e['path'], e['size'], e['summary']))
        if len(d['entries']) > a.limit:
            print('    ... %d more entries (--limit)' % (len(d['entries']) - a.limit))
    if not r['hits']:
        print('  none (evtobj: the SOBJ / BOBJ / SPMT id rows of evtobj*.pac; bgetc: a PACH number of bgEtc.pac; weapon: key 1000/N of m2.pac, or a key inside bgEtc WPON / WPBS / WPO2)')


def cmd_slot(con, arena, slot):
    rows = pac_rows(con, 'bg%02d' % arena)
    if not rows:
        return {'error': 'no bg%02d.pac' % arena}
    pid = rows[0]['id']
    q = 'SELECT slot, tokens FROM flags WHERE pac_id=?' + (' AND slot=?' if slot is not None else '') + ' ORDER BY slot'
    return dict(pac=rows[0]['path'], flags=[dict(r) for r in con.execute(q, (pid,) + ((slot,) if slot is not None else ()))])


def print_slot(r, a):
    if 'error' in r:
        print('asset slot: ' + r['error'])
        return
    print('asset slot %s: %d slot record(s)  %s' % (r['pac'], len(r['flags']), HEADER))
    for f in r['flags'][:a.limit * 4]:
        print('  slot %-4d %s' % (f['slot'], f['tokens']))


def cmd_info(con):
    m = dict(con.execute('SELECT key, value FROM meta').fetchall())
    for tb in ('pacs', 'pach', 'entry', 'model', 'material', 'texture', 'flags'):
        m[tb] = con.execute('SELECT COUNT(*) FROM %s' % tb).fetchone()[0]
    m['raw_pacs'] = con.execute('SELECT COUNT(*) FROM pacs WHERE raw=1').fetchone()[0]
    m['read_seconds_total'] = round(con.execute('SELECT SUM(secs) FROM pacs').fetchone()[0] or 0, 1)
    m['pacs_with_errors'] = [dict(r) for r in con.execute('SELECT path, err, nerr FROM pacs WHERE err IS NOT NULL OR nerr > 0 ORDER BY path LIMIT 30')]
    m['suspect_models'] = con.execute('SELECT COUNT(*) FROM model WHERE nvert IS NULL').fetchone()[0]
    m['shifted_layout_entries'] = con.execute("SELECT COUNT(*) FROM entry WHERE note LIKE '%shifted layout%'").fetchone()[0]
    m['kinds'] = {r[0]: r[1] for r in con.execute('SELECT kind, COUNT(*) FROM entry GROUP BY kind')}
    m['status'] = HEADER
    return m


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='q asset', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['update', 'find', 'pac', 'tex', 'mat', 'id', 'slot', 'info'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--limit', type=int, default=25)
    ap.add_argument('--like', action='store_true', help='tex / mat: a substring instead of the exact name')
    ap.add_argument('--pac', action='append', default=[], help='mat: only this pac (stem or path); repeatable')
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--redo', action='store_true', help='update FRAG --redo: re-read the pacs matching FRAG even when unchanged')
    ap.add_argument('-j', type=int, default=MAX_JOBS)
    a = ap.parse_args(argv)
    if a.cmd == 'update':
        path = db_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        lk = acquire_lock(path, 600)
        if lk is None:
            print('q asset: another writer holds the lock', file=sys.stderr)
            return 2
        try:
            done, left, secs = build(a.j, a.full, a.args[0] if a.args else None, lock=lk, redo=a.redo)
            print('q asset: %d pacs read, %d left, %.1f s' % (done, left, secs))
        finally:
            try:
                os.remove(lk)
            except OSError:
                pass
        return 0
    con = connect()
    if a.cmd == 'info':
        m = cmd_info(con)
        print(json.dumps(m, indent=1) if a.json else '\n'.join('%s: %s' % (k, v) for k, v in m.items()))
        return 0
    if not a.args:
        print('q asset %s: give an argument (q asset -h)' % a.cmd, file=sys.stderr)
        return 2
    if a.cmd == 'find':
        r, pr = cmd_find(con, a.args, a.limit), print_find
    elif a.cmd == 'pac':
        r, pr = cmd_pac(con, a.args), print_pac
    elif a.cmd == 'tex':
        r, pr = cmd_tex(con, a.args[0], a.like), print_tex
    elif a.cmd == 'mat':
        pacs = list(a.pac)
        names = []
        for w in a.args:
            if not names and not pacs and pac_rows(con, w) and any(os.path.basename(p['path'])[:-4].lower() == w.lower() for p in pac_rows(con, w)):
                pacs.append(w)
            elif names and any(os.path.basename(p['path'])[:-4].lower() == w.lower() for p in pac_rows(con, w)):
                pacs.append(w)
            else:
                names.append(w)
        if not names:
            print('q asset mat: give a material class or a texture name', file=sys.stderr)
            return 2
        r, pr = cmd_mat(con, names[0], pacs, a.like), print_mat
    elif a.cmd == 'id':
        if len(a.args) != 2 or not a.args[1].lstrip('-').isdigit():
            print('q asset id: q asset id evtobj|bgetc|weapon N', file=sys.stderr)
            return 2
        r, pr = cmd_id(con, a.args[0].lower(), int(a.args[1])), print_id
    else:
        r, pr = cmd_slot(con, int(a.args[0]), int(a.args[1]) if len(a.args) > 1 else None), print_slot
    if a.json:
        r['index'] = HEADER
        print(json.dumps(r, indent=1, default=str))
    else:
        pr(r, a)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
