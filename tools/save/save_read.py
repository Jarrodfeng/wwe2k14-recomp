"""S106 C1: the SaveData.Dat reader (decoder library behind `q save`). Read-only: it never writes a mirror.

  import save_read as S
  sv = S.Save('userdata_frozen')            # a folder name under <main>\\out\\bench, or a path to a user-data folder (or to the SaveData.Dat)
  sv.titles(); sv.options(); sv.dlc_mask(); sv.caws(); sv.threads(); sv.plate(belt_bobj_id)

Layout, functions and confidence: docs\\formats\\save_data.md (S106 C1, docs\\research\\s106\\C1.md). Rules applied here:
  * blob + X (block A, `[0x8376F3C0] + X`) = payload 0x1C + X; P + Y (block B, 0x83891878 + Y) = payload 0x1C + sizeA + Y.
  * an empty character id's Threads record reads 0 words, not -1: a set counts only for a roster row whose +0 equals the id.
  * holders: record +156 (tag titles fill both slots; 512 = none). The alternate set +164 is read by the game only when [0x836F2DA4] == 1,
    which no route from boot reaches: reported as `alt`, never used by a check.
  * a holder shows on a belt's nameplate only when the title's unlock id (+184, s32, < 0 = always) is set (flag byte P + 0x80394 + id).
Our test copies and the frozen mirror only; the decoded values are game-derived data: print them, never commit them.
"""
import os
import sqlite3
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import unlock_all as U  # noqa: E402  (read_payload: zlib + header + sum check)

REPO = U.REPO
MAIN = REPO
_up = os.path.abspath(os.path.join(REPO, '..', '..', '..'))
if os.path.basename(os.path.dirname(REPO)) == 'worktrees' and os.path.isdir(os.path.join(_up, 'out')):
    MAIN = _up  # a worktree reads the main checkout's out\ (mirrors, asset index) by absolute path
else:
    # S106 TIPFIX: out\wt\<id> (the train, the trials) is not under `worktrees`: the main checkout is the parent of git's common dir
    try:
        import subprocess as _sp
        _gc = _sp.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        _m = os.path.dirname(os.path.normpath(_gc)) if _gc else ''
        if _m and os.path.basename(_gc.rstrip('/\\')) == '.git' and os.path.isdir(os.path.join(_m, 'out')):
            MAIN = _m
    except Exception:
        pass

SIZE_A, SIZE_B, TITLE_N = 0x1224AC, 0x92DC0, 91   # S106 C1F F3: the retail layout (header sizes + title count)
BLOB = 0x1C                    # payload offset of block A (blob + X = payload 0x1C + X)
TITLE_COUNT = 0x112824           # blob + : word, 91 [L that it is a count]
TITLE_TABLE = 0x112828           # blob + : 676-byte records
TITLE_STRIDE = 676
HOLDER_OFF = 156                 # record +156 + 4k
ALT_OFF = 164                    # record +164 + 4k (only when [0x836F2DA4] == 1)
UNLOCK_OFF = 184                 # record +184 s32
NO_HOLDER = 512
ROSTER = 204                     # blob + 204 (c - 100): roster row stride; row +0 = id
CAPTION = 0x75E44 + 16           # blob + : B + 20, u32
OPT30_WORDS = {'opt30_B16': 0x75E44 + 12, 'opt30_B28': 0x75E44 + 24, 'opt30_B32': 0x75E44 + 28, 'opt30_B40': 0x75E44 + 36}
S_BLOCK = 0x80458                # P + : 92 bytes
S_LEN = 92
BLOOD = 0x804A9                  # P + : S + 81, u8
DLC_MASK = 0x81168               # P + : BE u32 (copy at +4)
UNLOCK_FLAGS = 0x80394           # P + : bytes, one per unlock id
CAW_BASE = 0x1E500               # blob + : records
CAW_STRIDE = 2924
CAW_SLOTS = 100
CAW_NAME, CAW_SHORT, CAW_LONG, CAW_STAMP = 204, 272, 312, 2040
THR_BASE = 4 + 860               # P + : + 1272 (c - 100) + 132 n
THR_REC = 1272
THR_SET = 132
THR_DEF_ATTIRE, THR_DEF_COLOUR = 1258, 1260
CHARS = range(100, 500)


def _resolve(spec):
    """A mirror: an existing path, or a name under <main>\\out\\bench. -> the SaveData.Dat path."""
    cands = [spec] if spec else []
    cands.append(os.path.join(MAIN, 'out', 'bench', spec or 'userdata_frozen'))
    for c in cands:
        if os.path.isfile(c):
            return c
        if os.path.isdir(c):
            try:
                return U.find_save(c)
            except SystemExit:
                continue
    raise SystemExit('no user-data mirror %r (a path, or a folder name under %s)' % (spec, os.path.join(MAIN, 'out', 'bench')))


class Save:
    """One decoded SaveData.Dat. Pass `payload=` (bytes) to decode a synthetic payload (units)."""

    def __init__(self, spec=None, payload=None, blob_skip=0):
        if payload is None:
            if spec is not None and not str(spec).strip():
                raise SystemExit('empty --data (a folder name or a path; omit it for userdata_frozen)')  # S106 C1F F2
            self.path = _resolve('userdata_frozen' if spec is None else spec)
            _, _, payload, hdr = U.read_payload(self.path)  # read-only open; sum / sizes checked
            self.size_a, self.size_b = hdr[0], hdr[1]
        else:
            self.path = None
            self.size_a, self.size_b = struct.unpack_from('>II', payload, 0)
        self.p = bytes(payload)
        self.base = BLOB - blob_skip
        # S106 C1F F3: every offset here is for the retail layout; another one is unreadable, never read with these offsets
        if blob_skip == 0:
            if (self.size_a, self.size_b) != (SIZE_A, SIZE_B):
                raise SystemExit('save layout differs: blocks 0x%X / 0x%X, expected 0x%X / 0x%X' % (self.size_a, self.size_b, SIZE_A, SIZE_B))
            if self.title_count() != TITLE_N:
                raise SystemExit('save layout differs: title count %d, expected %d' % (self.title_count(), TITLE_N))
        # blob_skip: planted-twin only (a reader that forgets the 0x1C header): shifts every blob / P offset down
        self.base = BLOB - blob_skip

    # ---- raw helpers (payload offsets) ----
    def u32(self, o):
        return struct.unpack_from('>I', self.p, o)[0]

    def s32(self, o):
        return struct.unpack_from('>i', self.p, o)[0]

    def blob(self, x):
        return self.base + x

    def pb(self, y):
        return self.base + self.size_a + y

    def cstr(self, o, n):
        return self.p[o:o + n].split(b'\0')[0].decode('latin-1')

    # ---- the five things ----
    def title_count(self):
        return self.u32(self.blob(TITLE_COUNT))

    def titles(self):
        out = []
        n = min(self.title_count(), 91)
        for t in range(n):
            r = self.blob(TITLE_TABLE) + TITLE_STRIDE * t
            kind = self.u32(r)
            if kind == 0:
                continue
            uid = self.s32(r + UNLOCK_OFF)
            unlocked = uid < 0 or self.p[self.pb(UNLOCK_FLAGS) + uid] != 0
            out.append({
                'title': t, 'kind': kind, 'belt': self.u32(r + 4), 'key': self.u32(r + 8),
                'name_id': self.u32(r + 12), 'short_id': self.u32(r + 16),
                'holders': [self.u32(r + HOLDER_OFF + 4 * k) for k in (0, 1)],
                'alt': [self.u32(r + ALT_OFF + 4 * k) for k in (0, 1)],   # reported only; the game reads it when [0x836F2DA4] == 1
                'unlock_id': uid, 'unlocked': unlocked,
            })
        return out

    def options(self):
        o = {'caption': self.u32(self.blob(CAPTION)), 'blood': self.p[self.pb(BLOOD)]}
        for k, x in OPT30_WORDS.items():
            o[k] = self.u32(self.blob(x))
        o['s_block'] = self.p[self.pb(S_BLOCK):self.pb(S_BLOCK) + S_LEN].hex()
        return o

    def dlc_mask(self):
        return {'mask': self.u32(self.pb(DLC_MASK)), 'copy': self.u32(self.pb(DLC_MASK) + 4)}

    def caws(self):
        out = []
        pkg = os.path.dirname(os.path.dirname(self.path)) if self.path else None  # .../00000001 (SaveData.dat\SaveData.Dat is two down)
        for i in range(CAW_SLOTS):
            r = self.blob(CAW_BASE) + CAW_STRIDE * i
            if self.p[r + CAW_NAME] == 0:
                continue
            has_pkg = None
            if pkg:
                has_pkg = os.path.isfile(os.path.join(pkg, '%02dCustomSuperStar.cas' % i, 'SaveData.Dat'))
            out.append({'slot': i, 'name': self.cstr(r + CAW_NAME, 68), 'short': self.cstr(r + CAW_SHORT, 40),
                        'long': self.cstr(r + CAW_LONG, 68), 'stamp': self.u32(r + CAW_STAMP), 'package': has_pkg})
        return out

    def real_ids(self):
        """Roster ids whose row +0 (u16) equals the id (empty ids carry zeros: their Threads words read 0, never -1)."""
        return [c for c in CHARS if struct.unpack_from('>H', self.p, self.blob(ROSTER * (c - 100)))[0] == c]

    def threads(self):
        out = []
        for c in self.real_ids():
            r = self.pb(THR_REC * (c - 100)) + 4  # P + 4 + 1272 (c - 100)
            sets = []
            for n in range(3):
                w = self.s32(r + 860 + THR_SET * n)
                if w != -1:
                    sets.append({'set': n, 'slot': w, 'name': self.cstr(r + 860 + THR_SET * n + 64, 68)})
            if sets:
                out.append({'char': c, 'sets': sets,
                            'default_attire': struct.unpack_from('>h', self.p, r + THR_DEF_ATTIRE)[0],
                            'default_colour': struct.unpack_from('>h', self.p, r + THR_DEF_COLOUR)[0]})
        return out

    # ---- the belt's plate (asset index, read-only) ----
    def plate(self, bobj):
        return has_plate(bobj)


_PLATES = {}


def asset_db():
    return os.environ.get('WWE_QASSET_DB') or os.path.join(MAIN, 'out', 'harness', 'index', 'asset.s4.db')


def has_plate(bobj):
    """True when BOBJ `bobj` (a belt id, e.g. 2009) is a PACH with entry 30 AND an entry-4 material sampling `nameplate` (PLATE2, 60 ids).
    None when the asset index is missing (the caller must treat that as unknown, never as 'no plate')."""
    if bobj in _PLATES:
        return _PLATES[bobj]
    db = asset_db()
    if not os.path.isfile(db):
        return None
    con = sqlite3.connect('file:%s?mode=ro' % db.replace('\\', '/'), uri=True)
    try:
        r = con.execute(
            "select count(*) from pach p where p.vpath=? and exists(select 1 from entry e where e.pach_id=p.id and e.eid=30) "
            "and exists(select 1 from entry e join material m on m.entry_id=e.id where e.pach_id=p.id and e.eid=4 and m.samplers like '%nameplate%')",
            ('/BOBJ/%d' % bobj,)).fetchone()[0]
    finally:
        con.close()
    _PLATES[bobj] = bool(r)
    return _PLATES[bobj]


def title_plated(tr):
    """A title record (from titles()): the belt has a plate AND a holder is set AND the unlock gate passes (sub_822D02C0(t, 0, 1))."""
    p = has_plate(tr['belt'])
    if p is None:
        return None
    return bool(p and tr['unlocked'] and tr['holders'][0] != NO_HOLDER)
