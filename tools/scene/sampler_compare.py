#!/usr/bin/env python3
r"""S103 lane B1: the game's sampler state per (effect, stage), from the player's own effect files, against what the stream's draws actually bound. 0 game runs, offline.

  py -I tools\scene\sampler_compare.py [--root .] [--captures a04_match,a04_entrance,...] [--frames 3] [--json OUT.json] [--selftest] [--cpp EXE]

For every draw of K frames of each kept capture (read in place under the main checkout's out\captures, never written): the draw's effect (fx_names.py's rule: the VS / PS hash through the
fx.json files), and for each bound texture slot the stream's SamplerDesc (capture section 9) against SamplerTable[effect][stage] = {u, v, w, mag, min, mip}
(tools\shaders\effect_table.py sampler_table: the pass+16 sampler block of the player's own pac\_startup.pac, documented in docs\formats\effect_techniques.md). Compared: address U / V / W,
mag, min, mip mode. Ignored: anisotropy, LOD (they carry the player's setting and the jitter bias). Report: matches / mismatches per (effect, stage); the mismatch set IS the model draw's
per-parameter address override (descriptor +28, sub_82C04DF0) and any state the pass does not own: listed as (effect, stage, observed).
--selftest: the planted twins (each must be CAUGHT): yCh_skin stage 5 flipped (clamp <-> wrap), the stage-12 point mip dropped.
--cpp EXE: also runs the C++ parser (tools\scene\sampler_table_dump.cpp, src\renderer_scene\sampler_table.h) on the same effect leaves and requires its table equal to the Python one.
Nothing from the game is written but our own counts (--json under out\)."""
import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shaders'))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import capread  # noqa: E402
import effect_table  # noqa: E402
import fx_names  # noqa: E402
from harness.dataroot import data_root  # noqa: E402

DEFAULT_CAPS = ['a00_entrance', 'a01_entrance', 'a04_entrance', 'a04_match', 'a08_entrance', 'a08_match', 'a09_entrance', 'a09_match', 'a16_entrance', 'a16_match',
                'a30_entrance', 'a30_match', 'a41_entrance', 'a41_match', 'brock_losswin']
# VkSamplerAddressMode of the Xenos clamp mode (src\renderer_adapter\reader.h AddressMode, S4; the C++ twin: sampler_table.h VkAddress)
VK_ADDR = {0: 0, 1: 1, 2: 2, 4: 2, 3: 4, 5: 4, 7: 4, 6: 3}
SAMPLER_SEC = 9
SAMPLER_DT = np.dtype([('mag', 'u1'), ('min', 'u1'), ('mip', 'u1'), ('u', 'u1'), ('v', 'u1'), ('w', 'u1'), ('aniso', 'u1'), ('border', 'u1'),
                       ('bias', '<f4'), ('min_lod', '<f4'), ('max_lod', '<f4')])   # rdr::SamplerDesc (src\renderer\api.h)


def samplers_of(buf):
    nsec = struct.unpack_from('<I', buf, 12)[0]
    for i in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * i)
        if sid == SAMPLER_SEC:
            if es != SAMPLER_DT.itemsize:
                raise SystemExit('FAIL sampler element size %d, expected %d' % (es, SAMPLER_DT.itemsize))
            return np.frombuffer(buf, SAMPLER_DT, cnt, off)
    return np.zeros(0, SAMPLER_DT)


def expected(tab, effect, stage):
    e = tab.get(effect.lower())
    if not e or stage not in e or 'u' not in e[stage]:
        return None
    s = e[stage]
    return dict(u=VK_ADDR.get(s.get('u', 0), 2), v=VK_ADDR.get(s.get('v', 0), 2), w=VK_ADDR.get(s.get('w', 0), 2), mag=int(s.get('mag', 0) == 1),
                min=int(s.get('min', 0) == 1), mip=int(s.get('mip', 2) == 1))


def observed(sd):
    return dict(u=int(sd['u']), v=int(sd['v']), w=int(sd['w']), mag=int(sd['mag']), min=int(sd['min']), mip=int(sd['mip']))


def collect(caps_dir, names, nframes, fx):
    """-> list of (effect, stage, observed dict, capture) for every bound slot of every draw of the sampled frames."""
    rows = []
    for name in names:
        path = os.path.join(caps_dir, name)
        if not os.path.isdir(path):
            print('  skip (not found):', name)
            continue
        src = capread.Source(path)
        fr_files = src.frames
        pick = sorted(set(int(round(i * (len(fr_files) - 1) / max(1, nframes - 1))) for i in range(nframes))) if len(fr_files) > 1 else [0]
        for fi in pick:
            buf = src.read(fr_files[fi])
            fr = capread.Frame(buf)
            sam = samplers_of(buf)
            for i in range(len(fr.draws)):
                d = fr.draws[i]
                lab, _per, _why = fx.eff.resolve(int(d['vs_hash']), int(d['ps_hash']))
                pe = lab.split('|')[0] if lab else None
                vs = fx.name_of('vertex', int(d['vs_hash']), pe)
                ps = fx.name_of('fragment', int(d['ps_hash']), pe) if int(d['ps_hash']) else None
                eff = pe or fx.effect_of(ps or vs)   # the effect the hashes resolve to (a variant's own name is not an effect name)
                if not eff:
                    continue
                for s in fr.slots[int(d['tex_first']):int(d['tex_first']) + int(d['tex_count'])]:
                    if int(s['sampler']) >= len(sam) or int(s['image']) == 0:
                        continue
                    rows.append((eff, int(s['slot']), observed(sam[int(s['sampler'])]), name))
    return rows


def compare(rows, tab):
    """-> (stats, mism) : per (effect, stage) [match, mismatch, no_entry]; mismatch cells {(effect, stage, observed-tuple): count}."""
    st = collections.defaultdict(lambda: [0, 0, 0])
    mism = collections.Counter()
    fields = collections.Counter()
    for eff, stage, obs, cap in rows:
        exp = expected(tab, eff, stage)
        if exp is None:
            st[(eff, stage)][2] += 1
            continue
        bad = [k for k in ('u', 'v', 'w', 'mag', 'min', 'mip') if exp[k] != obs[k]]
        if bad:
            st[(eff, stage)][1] += 1
            mism[(eff, stage, tuple(obs[k] for k in ('u', 'v', 'w', 'mag', 'min', 'mip')), tuple(exp[k] for k in ('u', 'v', 'w', 'mag', 'min', 'mip')))] += 1
            for k in bad:
                fields[k] += 1
        else:
            st[(eff, stage)][0] += 1
    return st, mism, fields


def twin_table(tab, which):
    import copy
    t = copy.deepcopy(tab)
    if which == 'ych_skin_stage5':
        for k in ('u', 'v'):
            t['ych_skin'][5][k] = 0 if t['ych_skin'][5][k] else 2
    elif which == 'stage12_mip':
        for e in t.values():
            if 12 in e and 'mip' in e[12]:
                e[12]['mip'] = 1
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--captures', default=','.join(DEFAULT_CAPS))
    ap.add_argument('--frames', type=int, default=3)
    ap.add_argument('--json')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--cpp')
    a = ap.parse_args()
    caps_dir = os.path.join(data_root(ROOT), 'out', 'captures')
    res = effect_table.load_all()
    tab, conflicts = effect_table.sampler_table(res)
    print('table: %d effects with a sampler block, %d pass disagreements' % (sum(1 for v in tab.values() if v), conflicts))
    fx = fx_names.Fx()
    rows = collect(caps_dir, a.captures.split(','), a.frames, fx)
    print('%d draw-slot rows' % len(rows))
    st, mism, fields = compare(rows, tab)
    m = sum(v[0] for v in st.values())
    x = sum(v[1] for v in st.values())
    n = sum(v[2] for v in st.values())
    print('MATCH %d  MISMATCH %d  NO TABLE ENTRY %d   (%.2f%% of the compared)' % (m, x, n, 100.0 * m / max(1, m + x)))
    print('mismatching fields:', dict(fields))
    print('mismatch set (effect, stage, observed u,v,w,mag,min,mip  vs  table):')
    for (eff, stage, obs, exp), c in sorted(mism.items(), key=lambda kv: -kv[1])[:60]:
        print('  %-28s stage %-2d observed %s  table %s   x%d' % (eff, stage, obs, exp, c))
    noent = collections.Counter()
    for (eff, stage), v in st.items():
        if v[2]:
            noent[(eff, stage)] += v[2]
    print('no table entry (effect, stage) top:', noent.most_common(12))
    rc = 0
    if not rows or m + x == 0:
        print('FAIL: nothing compared (a check that reads nothing must fail)')
        rc = 1
    if a.selftest:
        for twin, cell in (('ych_skin_stage5', ('ych_skin', 5)), ('stage12_mip', None)):
            _s, mm, _f = compare(rows, twin_table(tab, twin))
            if cell:
                bad = sum(c for (e, s, _o, _x), c in mm.items() if (e.lower(), s) == cell)
                base = sum(c for (e, s, _o, _x), c in mism.items() if (e.lower(), s) == cell)
            else:
                bad = sum(c for (e, s, _o, _x), c in mm.items() if s == 12)
                base = sum(c for (e, s, _o, _x), c in mism.items() if s == 12)
            caught = bad > base
            print('twin %-16s %s (cell mismatches %d vs %d untouched)' % (twin, 'CAUGHT' if caught else 'NOT CAUGHT', bad, base))
            if not caught:
                rc = 1
    if a.cpp:
        rc |= cpp_twin(a.cpp, tab)
        rc |= forged_test(a.cpp)
        if a.selftest:   # the planted twin on the C++ side's comparison: a table with the stage-12 point mip dropped must differ from what the C++ parser reads
            bad = cpp_twin(a.cpp, twin_table(tab, 'stage12_mip'), quiet=True)
            print('twin cpp_stage12_mip  %s' % ('CAUGHT' if bad else 'NOT CAUGHT'))
            rc |= 0 if bad else 1
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump(dict(match=m, mismatch=x, no_entry=n, fields=dict(fields),
                       mismatches=[dict(effect=e, stage=s, observed=list(o), table=list(t), count=c) for (e, s, o, t), c in mism.items()]), open(a.json, 'w'), indent=1)
    print('RESULT', 'PASS' if rc == 0 else 'FAIL')
    return rc


def synth_effect(ca, cb, cc, stage=3, state=0, val=2):
    """A minimal effect blob (header + body) with one technique, one pass and a sampler block whose three counts are given as they are (a forged block uses counts that wrap a 32-bit sum)."""
    body = bytearray(0x400)
    struct.pack_into('>I', body, 0x200, 0x220)      # technique table
    struct.pack_into('>I', body, 0x20C, 1)          # one technique
    struct.pack_into('>I', body, 0x220, 0x230)      # technique 0
    struct.pack_into('>I', body, 0x234, 1)          # one pass
    struct.pack_into('>I', body, 0x230 + 16, 0x250)  # pass struct
    struct.pack_into('>I', body, 0x250 + 8, 0x270)   # pass block (id)
    struct.pack_into('>I', body, 0x250 + 16, 0x340)  # sampler block
    struct.pack_into('>III', body, 0x340 + 128, ca, cb, cc)
    struct.pack_into('>HHI', body, 0x340 + 140, stage, state, val)
    return struct.pack('>III', 0, len(body), 0) + bytes(body)


def leaf_of(effects):
    """A decoded effect leaf (LE count, 16-byte header, 32-byte entries name16 'obj\0' size offset, then the blobs)."""
    head = 16 + 32 * len(effects)
    out, blobs = bytearray(struct.pack('<I', len(effects)) + bytes(12)), bytearray()
    for name, blob in effects:
        out += name.encode().ljust(16, bytes(1)) + b'obj' + bytes(1) + struct.pack('<II', len(blob), head + len(blobs)) + bytes(4)
        blobs += blob
    return bytes(out + blobs)


def forged_test(exe):
    """Item 4 of the review: a block whose counts wrap a 32-bit sum (0xFFFFFFFF + 1 + 1) must be rejected by the Python parse AND the C++ one; the control (0, 0, 1) is read by both."""
    ctl, forged = synth_effect(0, 0, 1), synth_effect(0xFFFFFFFF, 1, 1)
    ok = True
    for name, blob, want in (('control', ctl, 1), ('forged', forged, 0)):
        t = effect_table.parse_effect(blob)
        n = sum(len(p['samplers']) for tech in t['techniques'] for p in tech['passes'])
        print('  python %-8s samplers %d (want %d)' % (name, n, want))
        ok &= n == want
    outdir = os.path.join(data_root(ROOT), 'out', 'b1')
    os.makedirs(outdir, exist_ok=True)
    fn = os.path.join(outdir, 'forged_leaf.bin')
    open(fn, 'wb').write(leaf_of([('ctl', ctl), ('forge', forged)]))
    out = subprocess.run([os.path.abspath(exe), fn], capture_output=True, text=True, timeout=120).stdout
    cells = [ln.split()[0] for ln in out.splitlines() if len(ln.split()) == 8 and not ln.startswith('conflicts')]
    print('  C++ cells from the forged leaf:', cells, '(want [ctl])')
    ok &= cells == ['ctl']
    return 0 if ok else 1


def cpp_twin(exe, tab, quiet=False):
    """The C++ parser (sampler_table.h) on the same effect leaves must give the same table: the exe reads leaf files and prints `effect stage u v w mag min mip`."""
    import glob
    import pac
    import shaderlib
    outdir = os.path.join(data_root(ROOT), 'out', 'b1')
    os.makedirs(outdir, exist_ok=True)
    leaves = []
    for rel in effect_table.SHADER_FILES:
        data = pac.load(os.path.join(shaderlib.GAME, rel.replace('/', os.sep)))
        for leaf, dec in pac.entries(data):
            if b'\x10\x2a\x11' in dec and shaderlib.effect_table(dec):
                fn = os.path.join(outdir, 'leaf_%d.bin' % len(leaves))
                open(fn, 'wb').write(dec)
                leaves.append(fn)
    out = subprocess.run([os.path.abspath(exe)] + leaves, capture_output=True, text=True, timeout=300).stdout
    got = {}
    if 'codes FAIL' in out and not quiet:
        print(out)
        return 1
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 8 and p[0] != 'conflicts':
            got.setdefault(p[0].lower(), {})[int(p[1])] = dict(zip(('u', 'v', 'w', 'mag', 'min', 'mip'), map(int, p[2:])))
    bad = 0
    for eff, stages in tab.items():
        for stage, s in stages.items():
            if 'u' not in s:
                continue
            want = dict(u=s.get('u', 0), v=s.get('v', 0), w=s.get('w', 0), mag=s.get('mag', 0), min=s.get('min', 0), mip=s.get('mip', 2))
            if got.get(eff, {}).get(stage) != want:
                bad += 1
                if bad < 6 and not quiet:
                    print('  C++ differs:', eff, stage, got.get(eff, {}).get(stage), want)
    ncells = sum(len(v) for v in got.values())
    if quiet:
        return 1 if bad else 0
    print('C++ twin: %d cells read, %d differ from the Python table' % (ncells, bad))
    return 1 if bad or not ncells else 0


if __name__ == '__main__':
    sys.exit(main())
