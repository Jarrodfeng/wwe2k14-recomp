r"""qasset_cov (S111 ASSETCOV): three more answers (`params` moved to qasset_census.py, S111 QCENSUS) of the asset index (`q asset`), each one a scratch script somebody had to write in S111.

  q asset params EFFECT [--has NAME] [--lacks NAME] [--values NAME] [--like] [--source S]
      The materials (= meshes: one material row per mesh) of one effect (EFFECT exact, case-insensitive; --like = substring; `*` = every effect), counted by which parameter /
      sampler names they carry: how many carry each name, and the value histogram of the --has / --values names. BODYSPEC's questions, one line each:
        q asset params yCh_skin --has g_fSpecLev --lacks g_fSpecPow       yCh_skin meshes that name the strength without the power
        q asset params '*' --has g_fSpecularLev                           any material naming g_fSpecularLev
  q asset vcolor PAC [ENTRY] [--mesh N]
      The vertex colour of the meshes of a pac, per channel: min / max / the number of distinct values and the top values. The colour word is S0 file bytes 24..27 of each 28-byte vertex
      (src\assets\jboy.h LoadJboy, the colour word block (s0d + 28 v, p0[24..27]); docs\research\s111\VERTEXSEAM.md: for a yCh_skin mesh the skin shader reads it as (byte 25, 26, 27, 24) = inExtra x y z w: byte 25 = the first lobe's
      gain, byte 24 = the packed ID-map / wrinkle slot). Stored at build time (table vcolor, schema 6) for every mesh whose streams tile (S1 = S0 + 4 + 28 n).
  REPLACED?  `q asset pac` / `info` mark a game-folder pac\ch\*.pac whose first four bytes are not XCTD (0F F5 12 ED): every stock character pac is XCTD, a community pac
      (the Superstar Importer's, or a hand-replaced file) is EPAC / EPK8 (S111 MODINDEX review). The magic is stored per pac at build time (pacs.magic).
Schema 6 = schema 5 + the vcolor table + pacs.magic. A missing asset.s6.db of the default name is seeded from the asset.s5.db (or s4) beside it: the old rows answer at once, and every
decoded pac is marked for a re-read (the colour and the magic exist only in the file), which the next query / `q asset update` does.
"""
import json
import os
import re
import struct

import qasset_mods

XCTD = b'\x0f\xf5\x12\xed'
CH_PAC = re.compile(r'^pac/ch/[^/]+\.pac$', re.I)
CHANNELS = (24, 25, 26, 27)         # S0 file byte of each colour channel
CH_ROLE = {24: 'w: ID-map / wrinkle slot (skin)', 25: 'x: first-lobe gain (skin)', 26: 'y', 27: 'z'}
TOP = 8

VCOLOR_DDL = '''
CREATE TABLE vcolor(entry_id INTEGER, mesh INTEGER, nvert INTEGER, ch INTEGER, vmin INTEGER, vmax INTEGER, ndist INTEGER, top TEXT);
CREATE INDEX i_vcolor ON vcolor(entry_id, mesh);
'''


# ------------------------------------------------------------------------------------------------------------------------------- the magic (REPLACED?)

def magic_text(head):
    """the first four bytes of a pac file -> 'XCTD' | the ASCII tag (EPAC, EPK8) | hex"""
    head = bytes(head[:4])
    if head == XCTD:
        return 'XCTD'
    if len(head) == 4 and all(32 <= c < 127 for c in head):
        return head.decode('ascii')
    return head.hex().upper()


def file_magic(path):
    try:
        with open(path, 'rb') as f:
            return magic_text(f.read(4))
    except OSError:
        return None


def is_replaced(rel, magic):
    """a GAME-folder pac\\ch\\*.pac (not a mod's, not dlc) whose magic is known and is not XCTD"""
    return bool(magic) and magic != 'XCTD' and bool(CH_PAC.match(rel))


def replaced_summary(con, limit=12):
    rows = [r[0] for r in con.execute("SELECT path FROM pacs WHERE magic IS NOT NULL AND magic != 'XCTD' ORDER BY path") if CH_PAC.match(r[0])]
    unknown = sum(1 for r in con.execute('SELECT path FROM pacs WHERE magic IS NULL') if CH_PAC.match(r[0]))
    return dict(count=len(rows), pacs=rows[:limit], more=max(0, len(rows) - limit), magic_unknown=unknown)


def replaced_text(rel, magic):
    return 'REPLACED? magic %s (every stock pac\\ch pac is XCTD: a community pac is EPAC / EPK8)' % magic if is_replaced(rel, magic) else ''


# ------------------------------------------------------------------------------------------------------------------------------- vertex colour (worker side)

def mesh_vcolor(body, rec, n):
    """one mesh record -> [(file byte, min, max, distinct, top json)] x4, or None when the streams do not tile (S1 = S0 + 4 + 28 n) or the stream leaves the body"""
    import numpy as np
    if not n:
        return None
    s0, s1 = struct.unpack_from('>II', body, rec + 0x68)[0], struct.unpack_from('>I', body, rec + 0x6C)[0]
    off = s0 + 4
    if s0 + 4 + 28 * n != s1 or off + 28 * n > len(body):
        return None
    a = np.frombuffer(body, np.uint8, count=28 * n, offset=off).reshape(n, 28)
    out = []
    for b in CHANNELS:
        cnt = np.bincount(a[:, b], minlength=256)
        nz = np.nonzero(cnt)[0]
        top = sorted(((int(v), int(cnt[v])) for v in nz), key=lambda x: (-x[1], x[0]))[:TOP]
        out.append((b, int(nz[0]), int(nz[-1]), int(len(nz)), json.dumps(top)))
    return out


def model_vcolor(body, mtab, stride, mats):
    """[(mesh, nvert, channels)] of the meshes that read as a record (mats = parse_model's rows: mesh, verts, ...); never raises (a model keeps its materials without it)"""
    out = []
    try:
        for m in mats:
            ch = mesh_vcolor(body, mtab + m[0] * stride, m[1])
            if ch:
                out.append((m[0], m[1], ch))
    except Exception:       # noqa: BLE001
        return []
    return out


# ------------------------------------------------------------------------------------------------------------------------------- the seed (schema 5 / 4 -> 6)

def seed(path, schema, out):
    """A missing asset.s6.db of the default name starts as a copy of the asset.s5.db (else s4) beside it (sqlite backup: consistent, the old file only opened read-only), gets the vcolor
    table and the pacs.magic column, and every decoded (not raw) pac is marked for a re-read (mtime -1): the colour and the magic are in the pac files only. -> the file it was seeded
    from, or ''."""
    import sqlite3
    if schema != 6 or os.path.exists(path) or os.path.basename(path) != 'asset.s6.db':
        return ''
    d = os.path.dirname(path)
    tmp = '%s.seed%d' % (path, os.getpid())
    for name, sv in (('asset.s5.db', '5'), ('asset.s4.db', '4')):
        prev = os.path.join(d, name)
        if not os.path.isfile(prev):
            continue
        try:
            src = sqlite3.connect('file:%s?mode=ro' % prev, uri=True)
            try:
                row = src.execute("SELECT value FROM meta WHERE key='schema'").fetchone()
                if not row or str(row[0]) != sv:
                    continue
                dst = sqlite3.connect(tmp)
                try:
                    src.backup(dst)
                    have_vc = dst.execute("SELECT COUNT(*) FROM sqlite_master WHERE name='vcolor'").fetchone()[0]
                    if not have_vc:
                        dst.executescript(VCOLOR_DDL)
                    if 'magic' not in [r[1] for r in dst.execute('PRAGMA table_info(pacs)')]:
                        dst.execute('ALTER TABLE pacs ADD COLUMN magic TEXT')
                    nre = 0
                    if not have_vc:
                        nre = dst.execute('UPDATE pacs SET mtime_ns = -1 WHERE raw = 0').rowcount
                    dst.execute("UPDATE meta SET value='6' WHERE key='schema'")
                    dst.commit()
                finally:
                    dst.close()
            finally:
                src.close()
            os.replace(tmp, path)
        except (sqlite3.Error, OSError) as e:
            print('q asset: seeding %s from %s failed (%s): a full build instead' % (path, prev, e), file=out)
            try:
                os.remove(tmp)
            except OSError:
                pass
            return ''
        print('q asset: %s seeded from %s (the old rows answer meanwhile; %d decoded pacs are marked for a re-read: the vertex colour and the pac magic exist only in the files)' % (path, prev, nre), file=out)
        return prev
    return ''


# ------------------------------------------------------------------------------------------------------------------------------- q asset vcolor

def cmd_vcolor(con, args, a, pac_rows):
    prs = pac_rows(con, args[0])
    if not prs:
        return {'error': 'no pac matches "%s"' % args[0]}
    if len(prs) > 1:
        return {'error': 'several pacs match "%s": %s' % (args[0], ', '.join(r['path'] for r in prs[:12]))}
    p = prs[0]
    sql = ('SELECT pach.idx AS pidx, pach.vpath AS vpath, entry.path AS epath, material.mesh AS mesh, material.effect AS effect, material.verts AS verts, '
           'vcolor.nvert AS nvert, vcolor.ch AS ch, vcolor.vmin AS vmin, vcolor.vmax AS vmax, vcolor.ndist AS ndist, vcolor.top AS top '
           'FROM material JOIN entry ON material.entry_id = entry.id JOIN pach ON entry.pach_id = pach.id '
           'LEFT JOIN vcolor ON vcolor.entry_id = material.entry_id AND vcolor.mesh = material.mesh WHERE pach.pac_id = ?')
    sargs = [p['id']]
    if a.mesh is not None:
        sql += ' AND material.mesh = ?'
        sargs.append(a.mesh)
    if len(args) > 1:
        sql += ' AND (entry.path = ? OR entry.path LIKE ?)'
        sargs += [args[1], '%/' + args[1]]
    sql += ' ORDER BY pach.idx, entry.id, material.mesh, vcolor.ch'
    meshes = {}
    for r in con.execute(sql, sargs):
        k = (r['pidx'], r['epath'], r['mesh'])
        m = meshes.setdefault(k, dict(pach='%d%s' % (r['pidx'], ' ' + r['vpath'] if r['vpath'] else ''), entry=r['epath'], mesh=r['mesh'], effect=r['effect'], nvert=r['nvert'] if r['nvert'] is not None else r['verts'], channels=[]))
        if r['ch'] is not None:
            m['channels'].append(dict(ch=r['ch'], min=r['vmin'], max=r['vmax'], distinct=r['ndist'], top=json.loads(r['top'])))
    ml = list(meshes.values())
    nvc = con.execute('SELECT COUNT(*) FROM vcolor JOIN entry ON vcolor.entry_id = entry.id JOIN pach ON entry.pach_id = pach.id WHERE pach.pac_id = ?', (p['id'],)).fetchone()[0]
    return dict(pac=p['path'], total=len(ml), with_colour=sum(1 for m in ml if m['channels']), pac_vcolor_rows=nvc, meshes=ml)


def print_vcolor(r, a, header):
    if 'error' in r:
        print('asset vcolor: ' + r['error'])
        return
    print('asset vcolor %s: %d mesh(es), %d with a colour stream  %s' % (r['pac'], r['total'], r['with_colour'], header))
    if r['total'] and not r['pac_vcolor_rows']:
        print('  NOTE no vertex colour rows for this pac at all: not re-read since schema 6 (py tools/q/q.py asset update), or none of its meshes tile')
    for m in r['meshes'][:a.limit]:
        print('  PACH %s entry %s mesh %d %s [%s verts]%s' % (m['pach'], m['entry'], m['mesh'], m['effect'], m['nvert'] if m['nvert'] is not None else '?', '' if m['channels'] else '  no colour rows (streams do not tile, or not read yet)'))
        for c in m['channels']:
            print('    byte %d %-32s min %d max %d distinct %d  top %s' % (c['ch'], '(' + CH_ROLE[c['ch']] + ')', c['min'], c['max'], c['distinct'], ' '.join('%d x%d' % (v, n) for v, n in c['top'])))
    if r['total'] > a.limit:
        print('  ... %d more meshes (--limit, --mesh N, or an ENTRY path)' % (r['total'] - a.limit))
