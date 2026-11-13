r"""Lane 62 (S101, lane UJ of docs\renderer\UI_PROMOTION_MAP.md section 9): the pac side of the UI join census. Offline, 0 game runs.

Every texture the player's own pacs hold, as hashes (nothing from the game is written except the cache of hashes under out\):
  index_all()           a hash index of every DDS in every pac under game_data\extracted (multiprocessing, cached per pac in out\s101\ui_join\)
  scan_needles(...)     a raw byte search of every decoded leaf of every pac for given needles (the match HUD images that no DDS hash found)
  read_dds(pac, leaf, off)   re-read one texture's bytes through the pac reader (the join check's independent bytes)
  load_fonts()          fontHD.pac: the sheets + the .yfb tables (glyph records, kerning pair list)
Built on tools\wm30\pac.py (the reader the other lanes use). A pac that cannot be read is recorded as an error, never skipped silently."""
import hashlib
import json
import os
import struct
import sys
import time
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
import pac as pacmod  # noqa: E402

GAME = pacmod.GAME
MAIN = os.path.normpath(os.path.join(GAME, '..', '..'))
CACHE_DIR = os.path.join(MAIN, 'out', 's101', 'ui_join')
INDEX_VERSION = 2
BLOCK_BYTES = {b'DXT1': 8, b'DXT2': 16, b'DXT3': 16, b'DXT4': 16, b'DXT5': 16, b'ATI1': 8, b'ATI2': 16, b'BC4U': 8, b'BC5U': 16}


def h8(b):
    return hashlib.blake2b(bytes(b), digest_size=8).hexdigest()


def dds_layout(buf, off=0):
    """A DDS at buf[off:]: dict(w, h, mips, fourcc, bits, data (offset of the texel data), size (bytes of all mips), mip0 (bytes of mip 0)) or None.
    Block formats by fourcc, plain formats by the header's bit count; anything else is None (never guessed)."""
    if buf[off:off + 4] != b'DDS ' or off + 128 > len(buf) or struct.unpack_from('<I', buf, off + 4)[0] != 124:
        return None
    h, w, _, _, mips = struct.unpack_from('<5I', buf, off + 12)
    flags, fourcc, bits = struct.unpack_from('<I4sI', buf, off + 80)
    mips = max(mips, 1)
    data = off + 128 + (20 if fourcc == b'DX10' else 0)
    if w == 0 or h == 0 or w > 16384 or h > 16384:
        return None
    bb = BLOCK_BYTES.get(fourcc)
    sizes = []
    for k in range(mips):
        mw, mh = max(w >> k, 1), max(h >> k, 1)
        if bb:
            sizes.append(((mw + 3) // 4) * ((mh + 3) // 4) * bb)
        elif bits and not (flags & 4):
            sizes.append(mw * mh * bits // 8)
        else:
            return None
    return dict(w=w, h=h, mips=mips, fourcc=fourcc.decode('latin1').replace('\0', ''), bits=bits, data=data, size=sum(sizes), mip0=sizes[0])


def texture_directory(leaf):
    """The texture directory leaf (p*/7001 of the menu pacs): header u32 count, 0x100, 0, 0x10; then per texture 32 bytes: name[16], ext[4], u32 size, u32 offset,
    u32 0 (offset = the DDS in this leaf). {dds offset: name}, or {} when the leaf is not one."""
    if len(leaf) < 16 or struct.unpack_from('<4I', leaf, 0)[1:] != (0x100, 0, 0x10):
        return {}
    n = struct.unpack_from('<I', leaf, 0)[0]
    if n == 0 or 16 + 32 * n > len(leaf):
        return {}
    out = {}
    for i in range(n):
        name = leaf[16 + 32 * i:32 + 32 * i].split(b'\0')[0].decode('latin1')
        size, off, _ = struct.unpack_from('<3I', leaf, 36 + 32 * i)
        if off + size > len(leaf):
            return {}
        out[off] = name
    return out


def leaves_of(path):
    """[(leaf name, decoded bytes)] of one pac file (an EPAC / PACH pac); a file with no PACH structure is one 'raw' leaf."""
    data = pacmod.load(path)
    ls = list(pacmod.entries(data))
    return ls if ls else [('raw', data)]


def scan_leaf(leaf):
    """Every DDS in a decoded leaf: [(offset, layout)]."""
    out, p = [], leaf.find(b'DDS ')
    while p >= 0:
        lay = dds_layout(leaf, p)
        if lay:
            out.append((p, lay))
        p = leaf.find(b'DDS ', p + 4)
    return out


def _index_one(args):
    rel, path = args
    st = os.stat(path)
    ent = dict(mtime=int(st.st_mtime), size=st.st_size, leaves=0, bytes=0, textures=[], error=None)
    try:
        for name, leaf in leaves_of(path):
            ent['leaves'] += 1
            ent['bytes'] += len(leaf)
            names = texture_directory(leaf)
            for off, lay in scan_leaf(leaf):
                payload = leaf[lay['data']:lay['data'] + lay['size']]
                ent['textures'].append([name, off, names.get(off, ''), lay['w'], lay['h'], lay['fourcc'], lay['bits'], lay['mips'], len(payload), lay['mip0'],
                                        h8(payload), h8(payload[:lay['mip0']])])
    except Exception as e:  # recorded, and the census fails on it (an index with a hole proves nothing)
        ent['error'] = '%s: %s' % (type(e).__name__, e)
    return rel, ent


def _needle_one(args):
    rel, path, needles = args
    hits, errors = [], None
    try:
        for name, leaf in leaves_of(path):
            for nid, nb in needles:
                p = leaf.find(nb)
                if p >= 0:
                    hits.append([nid, rel, name, p])
    except Exception as e:
        errors = '%s: %s' % (type(e).__name__, e)
    return rel, hits, errors


def all_pacs():
    out = []
    pacdir = os.path.join(GAME, 'pac')
    for dp, dn, fn in os.walk(pacdir):
        for f in sorted(fn):
            if f.lower().endswith('.pac'):
                p = os.path.join(dp, f)
                out.append((os.path.relpath(p, pacdir).replace(os.sep, '/'), p))
    return sorted(out)


def _pool_size():
    return max(2, min(10, (os.cpu_count() or 4) // 2))


def index_all(reindex=False, log=print):
    """{pac rel path: entry} for every pac under game_data\\extracted\\pac. Cached per pac by (mtime, size)."""
    os.makedirs(CACHE_DIR, exist_ok=True)
    cpath = os.path.join(CACHE_DIR, 'pac_index.json')
    cache = {}
    if not reindex and os.path.exists(cpath):
        try:
            c = json.load(open(cpath))
            if c.get('version') == INDEX_VERSION:
                cache = c['pacs']
        except Exception:
            cache = {}
    pacs = all_pacs()
    if not pacs:
        raise SystemExit('no pacs under %s (a census that reads nothing must fail)' % GAME)
    todo = []
    for rel, path in pacs:
        st = os.stat(path)
        c = cache.get(rel)
        if not (c and c['mtime'] == int(st.st_mtime) and c['size'] == st.st_size and not c.get('error')):
            todo.append((rel, path))
    if todo:
        log('  indexing %d of %d pacs (%d cached) ...' % (len(todo), len(pacs), len(pacs) - len(todo)))
        t = time.time()
        with Pool(_pool_size()) as pool:
            for k, (rel, ent) in enumerate(pool.imap_unordered(_index_one, todo, chunksize=1)):
                cache[rel] = ent
        log('  indexed in %.0fs' % (time.time() - t))
        tmp = cpath + '.%d.tmp' % os.getpid()
        json.dump(dict(version=INDEX_VERSION, pacs=cache), open(tmp, 'w'))
        os.replace(tmp, cpath)
    return {rel: cache[rel] for rel, _ in pacs}


def scan_needles(needles, log=print):
    """needles = [(id, bytes)]. Returns ([[id, pac, leaf, offset]], errors, pacs_scanned). A raw search of every decoded leaf of every pac."""
    pacs = all_pacs()
    key = h8(b''.join(nb for _, nb in needles))
    cpath = os.path.join(CACHE_DIR, 'needles_%s.json' % key)
    sig = [[rel, int(os.stat(p).st_mtime), os.stat(p).st_size] for rel, p in pacs]
    if os.path.exists(cpath):
        try:
            c = json.load(open(cpath))
            if c['sig'] == sig:
                return c['hits'], c['errors'], len(pacs)
        except Exception:
            pass
    os.makedirs(CACHE_DIR, exist_ok=True)
    log('  raw search of %d pacs for %d needles ...' % (len(pacs), len(needles)))
    t = time.time()
    hits, errors = [], []
    with Pool(_pool_size()) as pool:
        for rel, h, e in pool.imap_unordered(_needle_one, [(rel, p, needles) for rel, p in pacs], chunksize=1):
            hits += h
            if e:
                errors.append([rel, e])
    log('  searched in %.0fs' % (time.time() - t))
    json.dump(dict(sig=sig, hits=hits, errors=errors), open(cpath + '.tmp', 'w'))
    os.replace(cpath + '.tmp', cpath)
    return hits, errors, len(pacs)


_pac_cache = {}


def read_dds(rel, leaf_name, off):
    """Independent re-read: the texel data (all mips) of the DDS at `off` in leaf `leaf_name` of pac `rel`, straight from the pac."""
    if rel not in _pac_cache:
        _pac_cache.clear()
        _pac_cache[rel] = dict(leaves_of(os.path.join(GAME, 'pac', rel.replace('/', os.sep))))
    leaf = _pac_cache[rel].get(leaf_name)
    if leaf is None:
        return None
    lay = dds_layout(leaf, off)
    return None if lay is None else leaf[lay['data']:lay['data'] + lay['size']]


# ---------------------------------------------------------------- fonts -----------------------------------------------------------------------------
def parse_yfb(t):
    """{name, codes, rects {code: (x, y, w, h)}, pairs {(left, right): value}, tail_records, tail_other}. Layout: docs\\formats\\ui_font.md; the tail after the
    glyph records is read as 8-byte records (u16 left, u16 right, u32 value); a record whose two codes are in the font and whose value is 1..8 is a kerning pair."""
    name = t[0x10:0x30].split(b'\0')[0].decode('latin1')
    name = name[5:] if name.startswith('name=') else name
    n = struct.unpack_from('<I', t, 0xA4)[0]
    codes = list(struct.unpack_from('<%dH' % n, t, 0xE8))
    recs = (0xE8 + 2 * n + 7) & ~7
    rects = {c: struct.unpack_from('<4H', t, recs + 16 * k) for k, c in enumerate(codes)}
    tail = t[recs + 16 * n:]
    cs = set(codes)
    pairs, other, total = {}, 0, len(tail) // 8
    for i in range(total):
        l, r, v = struct.unpack_from('<HHI', tail, 8 * i)
        if l in cs and r in cs and 1 <= v <= 8:
            pairs[(l, r)] = v
        else:
            other += 1
    return dict(name=name, count=n, codes=codes, rects=rects, pairs=pairs, tail_records=total, tail_other=other, tail_bytes=len(tail))


def load_fonts():
    """fontHD.pac -> {font name: dict(yfb=parsed, sheets=[(leaf, full payload hash, layout)])}; the sheets of a font are the DDS leaves under p0/<yfb id - 1>/."""
    path = os.path.join(GAME, 'pac', 'menu', 'fontHD.pac')
    leaves = dict(leaves_of(path))
    out = {}
    for name, leaf in leaves.items():
        if name.startswith('p0/') and name.count('/') == 1 and b'name=' in leaf[:0x30] and b'.yfb' in leaf[:0x40]:
            eid = int(name.split('/')[1])
            y = parse_yfb(leaf)
            sheets = []
            for ln, lb in sorted(leaves.items()):
                if ln.startswith('p0/%d/' % (eid - 1)):
                    lay = dds_layout(lb, 0)
                    if lay:
                        sheets.append((ln, h8(lb[lay['data']:lay['data'] + lay['size']]), lay))
            out[y['name']] = dict(leaf=name, yfb=y, sheets=sheets)
    if not out:
        raise SystemExit('fontHD.pac: no .yfb tables found (a census that reads nothing must fail)')
    return out
