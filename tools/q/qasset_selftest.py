"""qasset_selftest: proves `q asset` on a FIXTURE game folder in a TEMP dir (0 game runs, nothing from the real game), with PLANTED misses; then, when the real
index exists, the S103 QAS proofs against it (read-only).

Fixture: pac\\evt\\evtobj.pac (3 PACHs; directory rows SOBJ 2036 / 2037, BOBJ 3009; each PACH = a model + a texture directory), pac\\ch\\ch900.pac (a nested PACH holding a model
with a g_fDifAmbLevel parameter and a texture directory with a DXT5 and a BGRA8 texture of known alpha), pac\\mot99.pac (a motion pac: entries listed, not decoded).
Checks: the build counts; `tex` alpha stats equal the planted truth; `mat` shows the parameter and the sampler; `id evtobj` finds the planted row and INVENTS nothing for an
absent id; `find` reaches a parameter name; `pac` shows the nested entry path; a raw pac is not decoded; then PLANT A CHANGE (the DXT5 texture rewritten with another alpha):
the next query must refresh by itself and answer the NEW value (the old one must be gone).
Real tree (skipped when out\\harness\\index\\asset.s<SCHEMA>.db is absent): `tex 13904Cap` DXT5 512x512 alpha max 40; `id bgetc 3` /STG/CELL with 17 models; `id evtobj 2038` PACH 4;
`mat yCh_Reflect ch139` shows g_fDifAmbLevel 1.
Run:  py -I tools/q/qasset_selftest.py
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# entry: db_name (the index file the q asset sub-command opens; the rest runs through q.py as a subprocess)
HERE = os.path.dirname(os.path.abspath(__file__))
Q = os.path.join(HERE, 'q.py')


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail('%s\n--- output ---\n%s' % (what, str(out)[-1800:]))
    print('PASS  %s' % what)


# ----------------------------------------------------------------------------------------------------------------------------------------------- fixture builders

def be(v):
    return struct.pack('>I', v)


def name16(s):
    return s.encode().ljust(16, b'\0')


def jboy(effect, with_amb=True, tex='fixtex', nodes=('root',), variant='std', nmesh=1, words=None, tech=0x1, extra=(), vc=None):
    """a minimal JBOY: 1 mesh (3 vertices), the nodes, 2 texture names, params g_fDifAmbLevel 1.0 and sampDiffusePrm -> texture 0. words = the cull word of each mesh (record +0x8C; the
    shifted layout +0x88; default 0x4 = two-sided), tech = the technique word (+0x90; shifted +0x8C)"""
    nn = len(nodes)
    stride = 0xB4 if variant == 'shifted' else 0xB8          # the shifted layout's record is 4 bytes shorter
    mtab, ntab = 0x40, 0x40 + stride * nmesh
    ttab = ntab + 80 * nn
    plist = ttab + 32
    # S111 ASSETCOV: extra = [(parameter name, float)] more parameters; vc = per mesh None or 3 vertices' 4 colour bytes (file bytes 24..27 of each 28-byte S0 vertex)
    names = [('sampDiffusePrm', None)] + ([('g_fDifAmbLevel', 1.0)] if with_amb else []) + list(extra)
    prec = plist + 4 * len(names)
    sbase = prec + 24 * len(names)
    body = bytearray(sbase + sum(4 + 28 * len(v) for v in (vc or []) if v))
    body[0x10:0x14] = be(nmesh)
    body[0x14:0x18] = be(mtab)
    body[0x18:0x1C] = be(nn)
    body[0x1C:0x20] = be(2)
    body[0x20:0x24] = be(ntab)
    body[0x24:0x28] = be(ttab)
    npar = len(names)
    for mi in range(nmesh):
        rec = mtab + mi * stride
        body[rec:rec + 4] = be(3)
        if vc and vc[mi]:
            s0 = sbase + sum(4 + 28 * len(v) for v in vc[:mi] if v)
            body[rec + 0x68:rec + 0x6C] = be(s0)
            body[rec + 0x6C:rec + 0x70] = be(s0 + 4 + 28 * len(vc[mi]))
            for vi, c4 in enumerate(vc[mi]):
                body[s0 + 4 + 28 * vi + 24:s0 + 4 + 28 * vi + 28] = bytes(c4)
        if variant == 'std':
            body[rec + 0x7C:rec + 0x8C] = name16(effect)
            body[rec + 0x8C:rec + 0x90] = be(words[mi] if words else 0x4)
            body[rec + 0x90:rec + 0x94] = be(tech)
            body[rec + 0x94:rec + 0x98] = be(npar)
            body[rec + 0x98:rec + 0x9C] = be(plist)
        elif variant == 'shifted':          # every field from the effect on sits 4 bytes earlier (bgEtc /STG3/WPO2)
            body[rec + 0x78:rec + 0x88] = name16(effect)
            body[rec + 0x88:rec + 0x8C] = be(words[mi] if words else 0x4)
            body[rec + 0x8C:rec + 0x90] = be(tech)
            body[rec + 0x90:rec + 0x94] = be(npar)
            body[rec + 0x94:rec + 0x98] = be(plist)
        else:                               # garbage: fits neither layout
            body[rec + 0x78:rec + 0x8C] = b'\xff' * 20
    for i, n in enumerate(nodes):
        body[ntab + 80 * i:ntab + 80 * i + 16] = name16(n)
    body[ttab:ttab + 16] = name16(tex)
    body[ttab + 16:ttab + 32] = name16('unused')
    for i, (pn, pv) in enumerate(names):
        po = prec + 24 * i
        body[plist + 4 * i:plist + 4 * i + 4] = be(po)
        body[po:po + 16] = name16(pn)
        if pv is None:
            body[po + 16:po + 20] = be(0x000F0000 | 4)
            body[po + 20:po + 24] = be(0)
        else:
            body[po + 16:po + 20] = be(0x000A0000 | 4)
            body[po + 20:po + 24] = struct.pack('>f', pv)
    return b'JBOY' + be(len(body)) + bytes(body)


def dds(fourcc, w, h, payload, bgra=False):
    hdr = bytearray(128)
    hdr[0:4] = b'DDS '
    struct.pack_into('<I', hdr, 4, 124)
    struct.pack_into('<I', hdr, 8, 0x1007)
    struct.pack_into('<II', hdr, 12, h, w)
    struct.pack_into('<I', hdr, 76, 32)
    if bgra:
        struct.pack_into('<I', hdr, 80, 0x41)
        struct.pack_into('<5I', hdr, 88, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    else:
        struct.pack_into('<I', hdr, 80, 4)
        hdr[84:88] = fourcc
    return bytes(hdr) + payload


def dxt5_block(a0, a1, idx):
    bits = 0
    for k in range(16):
        bits |= idx << (3 * k)
    return bytes([a0, a1]) + bits.to_bytes(6, 'little') + bytes(8)


def texdir(items):
    """items [(name, dds bytes)] -> a texture directory entry"""
    out = bytearray(16 + 32 * len(items))
    struct.pack_into('<I', out, 0, len(items))
    blobs = b''
    for i, (nm, d) in enumerate(items):
        r = 16 + 32 * i
        out[r:r + 16] = name16(nm)
        out[r + 16:r + 20] = b'dds\0'
        struct.pack_into('<II', out, r + 20, len(d), len(out) + len(blobs))
        blobs += d
    return bytes(out) + blobs


def pach(entries):
    """[(id, bytes)] -> a PACH archive (LE count, id / offset / size)"""
    out = bytearray(b'PACH' + struct.pack('<I', len(entries)))
    data = b''
    for eid, b in entries:
        out += struct.pack('<III', eid, len(data), len(b))
        data += b
    return bytes(out) + data


def epac(pachs, rows, magic=b'EPAC', wide=False):
    """pachs [bytes]; rows [(tag, name)] one per PACH in order -> a pac whose 0x800 directory names each PACH (wide = kind 1: 16-byte rows, 8-character names)"""
    head = bytearray(0x4000)
    head[0:4] = magic
    body = b''
    secs = []
    for pb in pachs:
        secs.append(len(body) // 0x800)
        body += pb + bytes((-len(pb)) % 0x800)
    p = 0x800
    i = 0
    tags = []
    for tag, nm in rows:
        if tags and tags[-1][0] == tag:
            tags[-1][1].append((nm, secs[i]))
        else:
            tags.append((tag, [(nm, secs[i])]))
        i += 1
    for tag, rs in tags:
        head[p:p + 4] = tag.encode().ljust(4)
        rsz = 16 if wide else 12
        struct.pack_into('<I', head, p + 4, (len(rs) * rsz // 4) | ((1 << 16) if wide else 0))
        p += 12
        for nm, sec in rs:
            head[p:p + rsz - 8] = nm.encode().ljust(rsz - 8, b'\0')
            struct.pack_into('<II', head, p + rsz - 8, sec, 8)
            p += rsz
    return bytes(head) + body


def write_fixture(game, dxt_a0=40):
    pac_dir = os.path.join(game, 'extracted', 'pac')
    os.makedirs(os.path.join(pac_dir, 'evt'), exist_ok=True)
    os.makedirs(os.path.join(pac_dir, 'ch'), exist_ok=True)
    tex = texdir([('FixCap', dds(b'DXT5', 8, 8, dxt5_block(dxt_a0, 0, 0) * 2 + dxt5_block(dxt_a0, 0, 1) * 2)),
                  ('FixBgra', dds(b'', 4, 4, bytes([10, 20, 30, 255]) * 8 + bytes([10, 20, 30, 0]) * 8, bgra=True))])
    ev = [pach([(0, jboy('yProp_Fix', nodes=('root', 'hat'))), (10, tex)]),
          pach([(0, jboy('yProp_Fix2')), (10, texdir([('FixOther', dds(b'DXT1', 4, 4, bytes([0, 0, 1, 0, 0xFF, 0xFF, 0, 0])))]))]),
          pach([(0, jboy('yProp_Belt', with_amb=False)), (10, tex)])]
    with open(os.path.join(pac_dir, 'evt', 'evtobj.pac'), 'wb') as f:
        f.write(epac(ev, [('SOBJ', '2036'), ('SOBJ', '2037'), ('BOBJ', '3009')]))
    inner = pach([(5, jboy('yCh_Reflect', nodes=('Bip', 'l_mabuta'))), (10, tex)])
    with open(os.path.join(pac_dir, 'ch', 'ch900.pac'), 'wb') as f:
        f.write(epac([pach([(0, inner)])], [('EMD', '0000')]))
    inner2 = pach([(5, jboy('yShift', variant='shifted', nmesh=2, words=[0x6, 0x4], tech=0x82C05A80)), (6, jboy('yBadRec', variant='garbage')), (10, tex)])
    with open(os.path.join(pac_dir, 'ch', 'ch901.pac'), 'wb') as f:
        f.write(epac([pach([(0, inner2)])], [('EMD', 'LONGNAME')], wide=True))
    with open(os.path.join(pac_dir, 'zz_epk.pac'), 'wb') as f:
        f.write(epac([pach([(0, jboy('yEpk'))])], [('EMD', '0000')], magic=b'EPK8'))
    with open(os.path.join(pac_dir, 'evt', 'Entrance99.pac'), 'wb') as f:
        f.write(epac([b'EVPX' + bytes(300), b'EVZ0' + bytes(200)], [('EVP', '0000'), ('EVZ', '0000')]))
    os.makedirs(os.path.join(pac_dir, 'bg'), exist_ok=True)
    bgtex = texdir([('BgTex', dds(b'DXT1', 4, 4, bytes([0, 0, 1, 0, 0xFF, 0xFF, 0, 0])))])
    bg = pach([(0, jboy('yBG_Fix', nmesh=2, words=[0x6, 0x4], tech=0x82C05A80)), (1, jboy('yBG_Fix', words=[0x116])), (500, jboy('yBG_Fix', words=[0x6])), (400, bgtex)])
    with open(os.path.join(pac_dir, 'bg', 'bg90.pac'), 'wb') as f:
        f.write(epac([bg], [('STG', '0000')]))
    w1000 = pach([(29, pach([(0, jboy('yWeapon29', nodes=('root', 'blade'))), (10, tex)]))])
    with open(os.path.join(pac_dir, 'm2.pac'), 'wb') as f:
        f.write(epac([pach([(1000, w1000)])], [('MOT', 'WEAP')]))
    with open(os.path.join(pac_dir, 'mot99.pac'), 'wb') as f:
        f.write(epac([pach([(0, b'ZMOT' + bytes(64))])], [('ZMOT', '0000')]))


# ----------------------------------------------------------------------------------------------------------------------------------------------- running q

def run(args, env_extra=None, timeout=240):
    # (S111 ASSETCOV review) never the caller's WWE_QASSET_* (the README asks a builder to set WWE_QASSET_DB on every call): a fixture call that drops WWE_QASSET_DB to test the
    # default path (the seed blocks' e3) inherited it, built that REAL index against the fixture game folder and deleted every real pac as gone (measured: 473 pacs -> 3)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('WWE_QASSET_')}
    env.update(env_extra or {})
    r = subprocess.run([sys.executable, '-I', Q, 'asset'] + args, capture_output=True, text=True, timeout=timeout, env=env)
    return r.returncode, (r.stdout or '') + (r.stderr or '')


def main():
    tmp = tempfile.mkdtemp(prefix='qasset_st_')
    try:
        game = os.path.join(tmp, 'game_data')
        env = {'WWE_QASSET_GAME': game, 'WWE_QASSET_DB': os.path.join(tmp, 'asset.db'), 'WWE_QASSET_BUDGET': '120'}
        write_fixture(game)
        rc, out = run(['info'], env)
        check(rc == 0 and 'pacs: 8' in out, 'the first query builds the fixture index by itself (8 pacs)', out)
        check('built just now' in out, 'the header says built just now', out)
        check('model: 11' in out and 'texture: 12' in out, 'counts: 11 models, 12 textures (evtobj 3 / 5, ch900 1 / 2, ch901 2 / 2, zz_epk 1, m2 1 / 2, bg90 3 / 1)', out)
        rc, out = run(['tex', 'FixCap'], env)
        check('DXT5 8x8' in out and 'alpha max 40' in out and 'share 0 50.0%' in out, 'tex FixCap: DXT5 8x8, alpha max 40, share 0 = 50 % (planted: 2 of 4 blocks index 1 = a1 = 0)', out)
        check('mean 20.0' in out, 'tex FixCap: mean 20.0 (planted)', out)
        check('5 place(s)' in out, 'tex FixCap: found in the evtobj PACH(s) and ch900 (one distinct texture, several places)', out)
        rc, out = run(['tex', 'FixBgra'], env)
        check('BGRA8 4x4' in out and 'alpha max 255' in out and 'share 0 50.0%' in out and 'share >= 224 50.0%' in out, 'tex FixBgra: BGRA8, alpha max 255, share 0 and >= 224 both 50 % (planted)', out)
        rc, out = run(['tex', 'NoSuchTex'], env)
        check('0 distinct' in out, 'PLANTED MISS: a texture that does not exist is not invented', out)
        rc, out = run(['mat', 'yCh_Reflect'], env)
        check('g_fDifAmbLevel 1' in out and 'sampDiffusePrm -> fixtex' in out and 'ch900.pac' in out, 'mat yCh_Reflect: g_fDifAmbLevel 1, sampler -> fixtex, located in ch900.pac', out)
        rc, out = run(['mat', 'yCh_Reflect', 'ch900'], env)
        check('1 mesh materials' in out, 'mat with a pac stem (ch900) narrows to it', out)
        rc, out = run(['mat', 'yProp_Fix', 'ch900'], env)
        check('0 mesh materials' in out, 'PLANTED MISS: a material of another pac is not shown for ch900', out)
        rc, out = run(['id', 'evtobj', '3009'], env)
        check('PACH 2' in out and '/BOBJ/3009' in out, 'id evtobj 3009 -> PACH 2 (/BOBJ/3009)', out)
        rc, out = run(['id', 'evtobj', '2037'], env)
        check('PACH 1' in out and '/SOBJ/2037' in out, 'id evtobj 2037 -> PACH 1 (/SOBJ/2037)', out)
        rc, out = run(['id', 'evtobj', '9999'], env)
        check('0 hit(s)' in out, 'PLANTED MISS: an absent evtobj id answers no hit', out)
        rc, out = run(['find', 'ambLevel'], env)
        check('g_fDifAmbLevel' in out, 'find reaches a parameter name (ambLevel -> g_fDifAmbLevel)', out)
        rc, out = run(['find', 'mabuta'], env)
        check('l_mabuta' in out and 'ch900.pac' in out, 'find reaches a model node name (l_mabuta) in ch900.pac', out)
        rc, out = run(['pac', 'ch900', '0'], env)
        check('0/5' in out and 'model: 2 nodes, 1 meshes, 3 vertices' in out, 'pac ch900 0 lists the nested entry path 0/5 with its model summary', out)
        rc, out = run(['pac', 'ch900', '0', '0/5'], env)
        check('mesh 0: yCh_Reflect' in out and 'l_mabuta' in out, 'pac ch900 0 0/5 shows the model: nodes and the mesh material', out)
        rc, out = run(['pac', 'mot99', '0'], env)
        check('not decoded' in out, 'a motion pac (mot99) lists its entries without decoding them', out)
        # review fixes 1-3, 6, 7
        rc, out = run(['pac', 'ch901'], env)
        check('/EMD/LONGNAME' in out, 'a kind-1 directory (16-byte rows, 8-character names) names its PACH /EMD/LONGNAME (the old parser read it as 12-byte rows)', out)
        rc, out = run(['pac', 'zz_epk'], env)
        check('/EMD/0000' in out and '1 models' in out, 'an EPK8 pac has its directory read too (/EMD/0000)', out)
        rc, out = run(['pac', 'Entrance99'], env)
        check('2 non-PACH directory entries' in out and '/EVP/0000' in out and '/EVZ/0000' in out and 'EVPX' in out.replace('1 entries', '') or ('2 non-PACH directory entries' in out and '/EVP/0000' in out), 'non-PACH directory entries (EVP / EVZ) are listed, not "0 PACH, empty"', out)
        rc, out = run(['pac', 'Entrance99', '/EVP/0000'], env)
        check('blob EVPX' in out, 'the blob entry shows its tag (EVPX)', out)
        rc, out = run(['mat', 'yShift'], env)
        check('g_fDifAmbLevel 1' in out and 'ch901.pac' in out and 'x2' in out, 'the shifted layout (record 0xB4, effect at +0x78) is decoded for BOTH meshes of the model: yShift x2 with its parameter', out)
        rc, out = run(['pac', 'ch901', '0', '0'], env)
        check('shifted layout' in out and 'VERTEX COUNT UNKNOWN' in out, 'pac shows the shifted-layout note and the suspect model (garbage record: vertex count unknown)', out)
        rc, out = run(['mat', 'yBadRec'], env)
        check('0 mesh materials' in out, 'PLANTED MISS: a garbage mesh record is not turned into a material row', out)
        rc, out = run(['info'], env)
        check('suspect_models: 1' in out and 'ch901.pac' in out, 'info reports the suspect model and the pac with parse errors (ch901)', out)
        rc, out = run(['tex', 'FixOther'], env)
        check('DXT1 4x4' in out and 'alpha max 255' in out and 'share 0 50.0%' in out and 'share >= 224 50.0%' in out, 'DXT1 1-bit alpha measured (planted: 8 of 16 texels transparent)', out)
        rc, out = run(['id', 'weapon', '29'], env)
        check('m2.pac PACH 0 /MOT/WEAP key 1000/29' in out and 'READ BY our loader' in out, 'id weapon 29 finds m2.pac key 1000/29 and says our loader reads it', out)
        rc, out = run(['id', 'weapon', '30'], env)
        check('0 hit(s)' in out, 'PLANTED MISS: an absent weapon id answers no hit', out)
        cull_proofs(game, env, tmp)
        # locks: a dead writer's lock is taken over at once; a live writer's lock is respected (STALE after the wait), then released
        p = os.path.join(game, 'extracted', 'pac', 'ch', 'ch900.pac')
        st = os.stat(p)
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns + 3_000_000_000))
        lk = env['WWE_QASSET_DB'] + '.lock'
        dead = subprocess.Popen([sys.executable, '-c', 'pass'])
        dead.wait()
        with open(lk, 'w') as f:
            f.write(str(dead.pid))
        rc, out = run(['tex', 'FixCap'], env)
        check('refreshed just now' in out and 'STALE' not in out and not os.path.exists(lk), 'a FRESH lock whose owner pid is dead is taken over at once (not waited out by age)', out)
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns + 4_000_000_000))
        with open(lk, 'w') as f:
            f.write(str(os.getpid()))
        t0 = __import__('time').time()
        rc, out = run(['tex', 'FixCap'], dict(env, WWE_QASSET_LOCK_WAIT='2'))
        check('WARNING STALE' in out and os.path.exists(lk) and __import__('time').time() - t0 < 60, 'a live writer holds the lock: the query waits its short bound and answers STALE', out)
        os.remove(lk)
        rc, out = run(['tex', 'FixCap'], env)
        check('refreshed just now' in out, 'once the lock is gone the next query refreshes', out)
        # a planted change: the DXT5 texture rewritten with another alpha; the next query must refresh by itself
        write_fixture(game, dxt_a0=200)
        p = os.path.join(game, 'extracted', 'pac', 'ch', 'ch900.pac')
        st = os.stat(p)
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
        rc, out = run(['tex', 'FixCap'], env)
        check('alpha max 200' in out and 'alpha max 40' not in out, 'PLANTED CHANGE: the next tex query refreshed by itself and answers the new alpha max 200', out)
        check('refreshed just now' in out, 'the header says it refreshed just now', out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    real_proofs()
    print('ALL PASS')
    return 0


def jout(out):
    """the JSON object of a `--json` answer (build / header lines may precede it)"""
    import json
    return json.JSONDecoder().raw_decode(out[out.index('{'):])[0]


def cull_proofs(game, env, tmp):
    """S103 QAC: the cull word (+0x8C, bit 0x2 = one-sided) and the technique word (+0x90) of every mesh record, `asset cull`, and the schema bump. Fixture bg90.pac: key 0 = 2 meshes (words
    0x6, 0x4), key 1 = 1 mesh (0x116), key 500 = 1 mesh (0x6, NOT a stage slot); ch901's yShift = the shifted layout (words at +0x88, 0x6 / 0x4)."""
    import sqlite3
    rc, out = run(['pac', 'bg90', '0', '0'], env)
    check('cull 0x6 (one-sided), technique 0x82C05A80' in out and 'cull 0x4 (two-sided), technique 0x82C05A80' in out, 'pac bg90 0 0: a mesh with word 0x6 is one-sided, 0x4 two-sided, both with the technique word', out)
    rc, out = run(['pac', 'bg90', '0', '1'], env)
    check('cull 0x116 (one-sided)' in out, 'pac bg90 0 1: word 0x116 is one-sided (bit 0x2 set)', out)
    rc, out = run(['pac', 'ch901', '0', '0/5'], env)
    check('cull 0x6 (one-sided), technique 0x82C05A80' in out and 'cull 0x4 (two-sided)' in out, 'the shifted layout reads its words 4 bytes earlier (ch901 yShift: 0x6 one-sided, 0x4 two-sided)', out)
    rc, out = run(['cull', '--json'], env)
    rows = {x['label']: x for x in jout(out)['rows']}
    bg = rows.get('bg arena pacs (bgNN)', {})
    check(bg.get('meshes') == 4 and bg.get('one_sided') == 3 and bg.get('two_sided') == 1 and bg.get('words') == {'0x6': 2, '0x4': 1, '0x116': 1}, 'asset cull: the bg family row = 4 meshes, 3 one-sided, 1 two-sided, words 0x6 x2 / 0x4 / 0x116', out)
    st = rows.get('bg arena pacs (bgNN): stage slots (keys 0..399)', {})
    ot = rows.get('bg arena pacs (bgNN): other keys', {})
    check((st.get('meshes'), st.get('one_sided'), st.get('two_sided')) == (3, 2, 1) and (ot.get('meshes'), ot.get('one_sided'), ot.get('two_sided')) == (1, 1, 0), 'asset cull: the arena row splits stage slots (keys 0..399: 3 meshes, 2 set, 1 clear) from other keys (key 500: 1 mesh, set)', out)
    fam = [x for k, x in rows.items() if 'stage slots' not in k and not k.endswith(': other keys')]
    rc, info = run(['info'], env)
    nmat = int(info.split('material: ')[1].split()[0])
    check(sum(x['meshes'] for x in fam) == nmat and all(x['one_sided'] + x['two_sided'] == x['meshes'] for x in fam), 'asset cull: the family rows add up to every mesh row of the index (%d) and set + clear = meshes in each' % nmat, out)
    rc, out = run(['cull', 'bg90', '--set', '--list', '--json'], env)
    j = jout(out)
    check(j['lines']['total'] == 3 and all(x['word'] & 2 for x in j['lines']['rows']) and rc == 0, 'asset cull bg90 --set --list: exactly the 3 one-sided meshes', out)
    rc, out = run(['cull', 'bg90', '--clear', '--list', '--json'], env)
    j = jout(out)
    check(j['lines']['total'] == 1 and j['lines']['rows'][0]['word'] == 4, 'asset cull bg90 --clear --list: exactly the 1 two-sided mesh (word 0x4)', out)
    rc, out = run(['cull', 'bg90'], env)
    check('PACH 0 /STG/0000' in out and 'stage slots (keys 0..399)' in out, 'asset cull bg90: one row per PACH, plus the stage-slot split for an arena pac', out)
    rc, out = run(['cull', 'bg90', '0', '0'], env)
    check('mesh 0 yBG_Fix  cull 0x6 (one-sided)' in out and 'mesh 1 yBG_Fix  cull 0x4 (two-sided)' in out, 'asset cull bg90 0 0: one line per mesh with its word', out)
    rc, out = run(['cull', '--set', '--clear'], env)
    check('select nothing' in out, 'PLANTED MISS: --set with --clear selects nothing and says so', out)
    # the schema in the file name: a q never opens, wipes or rebuilds a file of another schema
    import hashlib
    old = os.path.join(tmp, 'old_schema.db')
    c = sqlite3.connect(old)
    c.executescript("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT); INSERT INTO meta VALUES('schema','3'); INSERT INTO meta VALUES('built_ts','1');")
    c.commit()
    c.close()

    def sha(f):
        return hashlib.sha256(open(f, 'rb').read()).hexdigest()

    h0 = sha(old)
    e2 = dict(env, WWE_QASSET_DB=old)
    rc, out = run(['cull'], dict(e2, WWE_QASSET_NOREFRESH='1'))
    check(rc != 0 and 'schema 3' in out and 'never opens, wipes or rebuilds' in out and sha(old) == h0, 'a schema-3 file named as the index, refresh off: refused with a message, file byte-identical', out)
    rc, out = run(['cull'], e2)
    check(rc != 0 and 'never opens, wipes or rebuilds' in out and sha(old) == h0 and not os.path.exists(old + '.lock'), 'a schema-3 file, refresh on: refused, NOT rebuilt, file byte-identical', out)
    rc, out = run(['update'], e2)
    check(rc != 0 and sha(old) == h0, '`update` on a schema-3 file: refused, file byte-identical', out)
    rc, out = run(['update', '--full'], e2)
    check(rc != 0 and sha(old) == h0, '`update --full` on a schema-3 file: refused, file byte-identical', out)
    # the default name: <main>\out\harness\index\asset.s4.db; the old asset.db beside it is left alone (and named in the header)
    idx = os.path.join(tmp, 'out', 'harness', 'index')
    os.makedirs(idx, exist_ok=True)
    shutil.copyfile(old, os.path.join(idx, 'asset.db'))
    h1 = sha(os.path.join(idx, 'asset.db'))
    e3 = {k: v for k, v in env.items() if k != 'WWE_QASSET_DB'}
    rc, out = run(['cull', '--json'], e3)
    rows = {x['label']: x for x in jout(out)['rows']}
    sys.path.insert(0, HERE)
    import qasset
    new = os.path.join(idx, qasset.db_name())
    check(rc == 0 and qasset.db_name() == 'asset.s%d.db' % qasset.SCHEMA and os.path.exists(new), 'default path: the index is built as %s beside the old file' % qasset.db_name(), out)
    check(sha(os.path.join(idx, 'asset.db')) == h1, 'the schema-3 asset.db was left byte-identical by a schema-%d q call' % qasset.SCHEMA, out)
    check(('writing ' + new) in out and rows.get('bg arena pacs (bgNN)', {}).get('meshes') == 4, 'the rebuild printed which file it writes, and answered (bg row 4 meshes)', out)
    check('asset.db' in jout(out)['index'] and 'left alone' in jout(out)['index'], 'the header names the older index file it left alone', out)
    c = sqlite3.connect(new)
    sv = c.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0]
    nn = c.execute('SELECT COUNT(*) FROM material WHERE cull_word IS NULL').fetchone()[0]
    c.close()
    check(sv == str(qasset.SCHEMA) and nn == 0, 'the new db is schema %d and no mesh row lacks its cull word' % qasset.SCHEMA)
    rc, out = run(['cull'], e3)
    check('TOTAL' in out and 'bg arena pacs (bgNN)' in out, 'asset cull prints the family table with a TOTAL row', out)


def real_proofs():
    here = os.path.dirname(os.path.dirname(HERE))
    sys.path.insert(0, HERE)
    import qasset
    cands = [os.path.join(here, 'out', 'harness', 'index', qasset.db_name()),
             os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(here))), 'out', 'harness', 'index', qasset.db_name())]
    db = next((c for c in cands if os.path.exists(c)), None)
    if not db:
        print('SKIP  real-tree proofs: no out\\harness\\index\\asset.db (run: py tools\\q\\q.py asset update)')
        return
    env = {'WWE_QASSET_NOREFRESH': '1', 'WWE_QASSET_DB': db}      # the db this check looked at (a worktree's own, or the main checkout's)
    real_dir_proof()
    import sqlite3
    c0 = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    try:
        sv = c0.execute("SELECT value FROM meta WHERE key='schema'").fetchone()[0]
    except sqlite3.Error:
        sv = None
    c0.close()
    if sv != str(qasset.SCHEMA):
        print('SKIP  real-tree proofs: the real index is schema %s, this q writes %d (the first q asset call after the merge rebuilds it)' % (sv, qasset.SCHEMA))
        return
    rc, out = run(['info'], env)
    if 'pacs: ' not in out or 'ch139' not in run(['pac', 'ch139'], env)[1]:
        print('SKIP  real-tree proofs: the real index does not hold the full game yet')
        return
    rc, out = run(['tex', '13904Cap'], env)
    check('DXT5 512x512' in out and 'alpha max 40' in out, 'REAL tex 13904Cap: DXT5 512x512, alpha max 40 (CAP)', out)
    rc, out = run(['id', 'bgetc', '3'], env)
    check('/STG/CELL' in out and '17 models' in out, 'REAL id bgetc 3: /STG/CELL, 17 models (CAGE)', out)
    rc, out = run(['id', 'evtobj', '2038'], env)
    check('PACH 4' in out, 'REAL id evtobj 2038: PACH 4 (the announcer mic)', out)
    rc, out = run(['mat', 'yCh_Reflect', 'ch139'], env)
    check('g_fDifAmbLevel 1' in out, 'REAL mat yCh_Reflect ch139: g_fDifAmbLevel 1 (EYA)', out)
    rc, out = run(['id', 'weapon', '29'], env)
    check('m2.pac PACH 0 /MOT/WEAP key 1000/29' in out and 'READ BY our loader' in out, 'REAL id weapon 29: m2.pac key 1000/29 (read by LoadM2Weapon) and the bgEtc sets', out)
    c = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    st = c.execute("SELECT SUM((cull_word & 2) != 0), SUM((cull_word & 2) = 0) FROM material JOIN entry ON material.entry_id = entry.id JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id "
                   "WHERE pacs.path GLOB 'pac/bg/bg[0-9]*.pac' AND entry.depth = 0 AND entry.eid BETWEEN 0 AND 399").fetchone()
    check(tuple(st) == (11377, 7064), 'REAL cull: all arena bg pacs, stage slots (keys 0..399): bit 0x2 set 11,377, clear 7,064 (CUL_ADV.md section G; got %s)' % (st,))
    n = c.execute("SELECT COUNT(*) FROM texture WHERE fmt='DXT1' AND a_max IS NOT NULL").fetchone()[0]
    check(n > 100, 'REAL DXT1 textures carry measured 1-bit alpha (%d rows)' % n)
    n = c.execute("SELECT COUNT(*) FROM material WHERE effect LIKE '?%' OR effect = ''").fetchone()[0]
    check(n == 0, 'REAL: no placeholder material rows ("?bad mesh record") remain (%d)' % n)
    n = c.execute("SELECT COUNT(*) FROM pach JOIN pacs ON pach.pac_id=pacs.id WHERE pacs.path LIKE '%Entrance00.pac' AND pach.nent > 0").fetchone()[0]
    check(n > 100, 'REAL Entrance00.pac: its EVP / EVZ directory entries are rows (%d), not "0 PACH, empty"' % n)
    sh = c.execute("SELECT cull_word, COUNT(*) FROM material JOIN entry ON material.entry_id = entry.id WHERE entry.note LIKE '%shifted layout%' GROUP BY cull_word").fetchall()
    check(len(sh) > 0 and sum(n for w, n in sh) > 50 and all(w & 4 for w, n in sh) and {w for w, n in sh} <= {4, 6}, 'REAL shifted layout: the word at +0x88 has bit 0x4 set on every mesh and is 4 / 6 only, like the standard cull word (%s)' % (sh,))
    n = c.execute("SELECT COUNT(*) FROM entry WHERE note LIKE '%shifted layout%'").fetchone()[0]
    check(n > 20, 'REAL: the shifted bgEtc mesh records are decoded (%d entries carry the shifted-layout note)' % n)


def real_dir_proof():
    """read_dir against plist360.arc (tools/goto/scene_ids.read_arc) on a sample of real pacs: plain, wide (kind 1), EPK8 and XCTD-packed (the full 354-pac comparison is
    run by hand when read_dir changes)"""
    sys.path.insert(0, HERE)
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'goto'))
    import qasset
    import scene_ids
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'wm30'))
    import pac
    g = os.path.join(os.path.dirname(pac.GAME), 'extracted')
    defs = scene_ids.read_def(os.path.join(g, 'plist360.def'))
    arc = scene_ids.read_arc(os.path.join(g, 'plist360.arc'))
    want = ('bgEtc', 'evtobj', 'ch139', 'Entrance00', 'mot01', 'chHeads', 'MnLayHD', 'menuHD', 'CAE', 'cab00', 'prtcls')
    done = 0
    for i, rel in enumerate(defs):
        if os.path.basename(rel)[:-4] not in want:
            continue
        data = pac.load(os.path.join(g, rel))
        got = {}
        for tag, nm, sec, sz in qasset.read_dir(data):
            got.setdefault(tag, []).append(nm)
        exp = {t.strip(): v for t, v in arc.get(i, {}).items() if v}
        check(got == exp, 'REAL read_dir == plist360.arc rows for %s (%d section(s))' % (rel, len(exp)), '%s != %s' % (list(got.items())[:3], list(exp.items())[:3]))
        done += 1
    check(done >= 8, 'REAL read_dir sample covered %d pacs' % done)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
