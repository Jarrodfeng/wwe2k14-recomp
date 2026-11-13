r"""Lane 267 (S101), piece M0 of docs\renderer\SKIP_GAME_WORK_MAP.md section 20 (Candidate M): the crowd members' uniformity census. Offline, 0 game runs, nothing written but numbers.

The contract's premise [U]: every member draw of a RUN (the stands block member loop sub_82518340, op 15 `bl 0x82BB10C0` at 0x82518574, lr 0x82518578; the seated-floor loop sub_8276EAA0,
0x8276ED60, lr 0x8276ED64) is identical to the run's FIRST draw except c76-c78 (the member's world, the op 21). If it holds, the hook can keep the first op-15 of each run as the carrier of
state, bank and texture and drop the others. This tool reads the captures (case zips / capture folders, v2 with draw origins) and, per run, per member draw, compares EVERYTHING the draw binds
with the run's first draw: the vertex constant bank (c76-c78 = dwords 304..315 masked), the fragment bank (all), the texture slots (images, and everything else about them: sampler, view format,
swizzle, levels), the vertex and index buffers (content), the layout, the shaders (vs / ps hashes + the patched vs), the fixed and dynamic state, the bool loop table, the draw record's own
shape (counts, index width, source primitive, flags).

  py -I tools\crowd\member_ledger.py [LABEL=CAPTURE ...] [--frames A:B:STEP] [--plant NAME] [--json OUT] [--selftest] [-q]
No CAPTURE = the standard set (DEFAULTS: Check B 7051, Brock 7079, J1 7086, Brock 7171, Bryan's entrance, a match 131008, the long match 7174), read from the main checkout's out\.
Exit 0 = "contract stands" (0 non-uniform members outside c76-c78 on every capture, RL1 holds, every capture readable); 1 = "contract must change" (the fields are listed) or an RL1 / sanity
failure; 2 = an input cannot be read.

Run boundary. The game's boundary is the op 14 (stream source) of a group (stands, lr 0x82518450) or of an object (floor, lr 0x8276EC14). The captured stream carries no op 14 as such, only its
effect: the next draw has another mesh. So a run here = consecutive member draws of one call site, no other op between them, ONE MESH (vertex offset + index offset) per run. Honest limits of that proxy:
(a) two real runs with the same mesh and the same everything adjacent are counted as one (the run count is a LOWER bound, calls / run an upper bound; uniformity is unaffected); (b) the mesh
fields (vertex / index offset and bytes) are equal by the definition of the run, so the vertex / index buffer check is trivially clean in the default mode: the plants `boundary_merge` shows the
buffers ARE compared when the boundary is wrong. The textures, banks, state and shaders are the real check.

RL1 (SKIP_GAME_WORK_MAP 16.9, 20.7): a hook counts REAL calls. Here every op-15 of a call site is counted one at a time, from the op list AND independently from the draw table's call-site
column; the two must agree, and `calls > runs` must hold wherever a run has more than one member (`seen != runs`: a collapsed count fails). The per-frame list of frames with calls == runs is
printed.

Plants (--plant NAME and --selftest; each must change the verdict): perturb_bank (one non-c76-c78 word of a non-first member's vertex bank changed), perturb_ps_bank, perturb_slot (a non-first
member's texture image changed), perturb_state (the fixed state), perturb_buffer (a non-first member's index buffer content), boundary_merge (no run boundary: all consecutive member draws of a site
one run), boundary_split (every member its own run: calls == runs), calls_collapsed (calls counted as runs). Control: perturb_world (a c76-c78 word changed: must stay clean)."""
import argparse
import collections
import hashlib
import json
import os
import sys
import time
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402  (Cap, the shared reader; reads through pcap / capread)

pcap = cc.pcap

SITES = {0x82518574: 'stands', 0x8276ED60: 'floor'}   # the `bl 0x82BB10C0` addresses (lr = +4: 0x82518578, 0x8276ED64)
WORLD = slice(4 * 76, 4 * 79)                          # c76-c78 = dwords 304..315 of the vertex bank (cards_census R_WORLD)
RECORD_PREFIX = {'stands': b'crowd:block:', 'floor': b'crowd:floor:'}
US_PER_DROP = 1.75                                     # SKIP_GAME_WORK_MAP 20.5: 1.17 us replay + ~0.59 us reader per op 15
PLANTS = ('perturb_bank', 'perturb_ps_bank', 'perturb_slot', 'perturb_state', 'perturb_buffer', 'perturb_world', 'boundary_merge', 'boundary_split', 'calls_collapsed')
DEFAULTS = [
    ('checkB7051', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
    ('brock7079', 'out/s101/runs/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip'),
    ('J1_7086', 'out/s101/runs/20261112_172304_s100_owner_s89_brock/case_20261112_172341_872_0_pc_run_exit.zip'),
    ('brock7171', 'out/s101/runs/20261113_134448_s100_owner_s89_brock/case_20261113_134611_700_0_pc_run_exit.zip'),
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('match131008', 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip'),
    ('match7174', 'out/s101/runs/20261113_135405_s101_owner_match_long/case_20261113_135453_130_0_pc_run_exit.zip'),
]
SCALARS = ('vs_hash', 'ps_hash', 'vs_patched', 'layout', 'vertex_count', 'index_count', 'index_32bit', 'source_prim', 'flags',
           'vertex_data', 'index_data', 'fixed', 'dynamic', 'bool_loop', 'slot_images', 'slot_state')


class LedgerError(Exception):
    """An input that cannot be read. Never a pass."""


class Draw:
    """One member draw (one op 15 call): its call site, its mesh (the run key), the compared scalars and the two banks (u4[1024], read-only references)."""
    __slots__ = ('site', 'mesh', 'f', 'vs', 'ps')

    def __init__(self, site, mesh, f, vs, ps):
        self.site, self.mesh, self.f, self.vs, self.ps = site, mesh, f, vs, ps


# ------------------------------------------------------------------- extraction (capture -> op-ordered items) ---------------------------------
class Reader:
    def __init__(self, cap):
        self.cap = cap
        self._blob = {}

    def blob(self, off, n):
        k = (off, n)
        h = self._blob.get(k)
        if h is None:
            h = self._blob[k] = hashlib.blake2b(bytes(self.cap.geo[off:off + n]), digest_size=8).digest()
        return h

    def frame_items(self, f):
        """-> (items in op order: Draw for a member draw, None for any other op (a run barrier, consecutive barriers collapsed), calls_by_table {site: n}), or raises LedgerError."""
        if not f.has_origins:
            raise LedgerError('frame %d carries no draw origins (a version-1 capture cannot name the call sites)' % f.idx)
        D = f.draws
        site_col = f.origins['call_site']
        items, hist = [], collections.Counter()
        nb = len(f.raw[pcap.SEC['bool_loops']])
        for kind, idx in f.ops:
            if int(kind) != 2:
                if items and items[-1] is not None:
                    items.append(None)
                continue
            idx = int(idx)
            site = int(site_col[idx])
            if site not in SITES:
                if items and items[-1] is not None:
                    items.append(None)
                continue
            d = D[idx]
            hist[SITES[site]] += 1
            tf, tc = int(d['tex_first']), int(d['tex_count'])
            imgs, state = [], []
            for s in f.slots[tf:tf + tc]:
                imgs.append((int(s['slot']), int(s['image'])))
                r = s.copy()
                r['image'] = 0
                r['sampler'] = 0
                state.append(r.tobytes() + f.raw[pcap.SEC['samplers']][int(s['sampler'])])
            bl = int(d['bool_loop'])
            vo, vb, io, ib = int(d['vertex_offset']), int(d['vertex_bytes']), int(d['index_offset']), int(d['index_bytes'])
            fields = dict(
                vs_hash=int(d['vs_hash']), ps_hash=int(d['ps_hash']), vs_patched=int(d['vs_patched']), layout=f.layout_bytes(int(d['layout'])),
                vertex_count=int(d['vertex_count']), index_count=int(d['index_count']), index_32bit=int(d['index_32bit']), source_prim=int(d['source_prim']), flags=int(d['flags']),
                vertex_data=self.blob(vo, vb), index_data=self.blob(io, ib), fixed=f.raw[pcap.SEC['fixeds']][int(d['fixed'])], dynamic=f.raw[pcap.SEC['dynamics']][int(d['dynamic'])],
                bool_loop=f.raw[pcap.SEC['bool_loops']][bl] if bl < nb else bl, slot_images=tuple(imgs), slot_state=b''.join(state))
            items.append(Draw(SITES[site], (vo, vb, io, ib), fields, f.bank(int(d['vs_bank'])), f.bank(int(d['ps_bank']))))
        return items, hist


# ------------------------------------------------------------------- the analysis (pure: items in, numbers out) --------------------------------
def split_runs(items, plant=None):
    """-> list of runs, each a list of Draw (consecutive draws of one site, no barrier, one mesh)."""
    runs, cur = [], []
    for it in items:
        if it is None:
            if cur:
                runs.append(cur)
            cur = []
            continue
        if cur:
            same = it.site == cur[-1].site and (plant == 'boundary_merge' or (it.mesh == cur[-1].mesh and plant != 'boundary_split'))
            if not same:
                runs.append(cur)
                cur = []
        cur.append(it)
    if cur:
        runs.append(cur)
    return runs


def apply_plant(runs, plant):
    """Mutates copies of a non-first member of the first run with >= 2 members (so a real capture's frame is perturbed once per frame)."""
    if plant not in ('perturb_bank', 'perturb_ps_bank', 'perturb_slot', 'perturb_state', 'perturb_buffer', 'perturb_world'):
        return
    done = set()
    for r in runs:
        if len(r) < 2 or r[0].site in done:
            continue
        done.add(r[0].site)
        d = r[1]
        if plant == 'perturb_bank':
            d.vs = d.vs.copy()
            d.vs[0] ^= 1
        elif plant == 'perturb_world':
            d.vs = d.vs.copy()
            d.vs[304] ^= 1
        elif plant == 'perturb_ps_bank':
            d.ps = d.ps.copy()
            d.ps[5] ^= 1
        else:
            d.f = dict(d.f)
            if plant == 'perturb_slot':
                d.f['slot_images'] = tuple((s, i + 1) for s, i in d.f['slot_images'])
            elif plant == 'perturb_state':
                d.f['fixed'] = bytes([d.f['fixed'][0] ^ 1]) + d.f['fixed'][1:]
            elif plant == 'perturb_buffer':
                d.f['index_data'] = b'\0' * 8


class Stats:
    def __init__(self):
        self.frames = 0                  # frames with >= 1 member draw at the site
        self.runs = 0
        self.members = 0                 # = op-15 calls
        self.calls_table = 0             # the same count read from the draw table's call-site column
        self.runs_ge2 = 0
        self.max_run = 0
        self.fields = collections.Counter()      # non-uniform members by field
        self.regs = {'vs_bank': collections.Counter(), 'ps_bank': collections.Counter()}
        self.bad_members = 0             # members with any field outside c76-c78 differing from the run's first
        self.bad_runs = 0
        self.frames_calls_eq_runs = []   # RL1: frames where calls == runs although members exist
        self.world_same_as_first = 0     # sanity: members whose c76-c78 equals the first's (a run of identical worlds would make the pairing meaningless)
        self.calls_min = None
        self.calls_max = 0
        self.rec_match = 0
        self.rec_frames = 0
        self.rec_calls = 0
        self.rec_members = 0

    def jsonable(self):
        d = {k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in self.__dict__.items() if k != 'regs'}
        d['regs'] = {k: dict(sorted(v.items())) for k, v in self.regs.items()}
        d['frames_calls_eq_runs'] = self.frames_calls_eq_runs[:20]
        return d


def analyse_frame(items, hist, S, plant=None, num=None):
    """Adds one frame to S = {site name: Stats}. Returns nothing; faults are in the numbers."""
    runs = split_runs(items, plant)
    apply_plant(runs, plant)
    by_site = collections.defaultdict(list)
    for r in runs:
        by_site[r[0].site].append(r)
    for site, rs in by_site.items():
        st = S[site]
        calls_walk = sum(len(r) for r in rs)             # RL1: every op 15 counted one at a time, never collapsed
        calls = len(rs) if plant == 'calls_collapsed' else calls_walk
        st.frames += 1
        st.runs += len(rs)
        st.members += calls
        st.calls_table += hist.get(site, 0)
        st.calls_min = calls if st.calls_min is None else min(st.calls_min, calls)
        st.calls_max = max(st.calls_max, calls)
        if calls == len(rs) and calls_walk > 0:
            st.frames_calls_eq_runs.append(num)
        for r in rs:
            st.max_run = max(st.max_run, len(r))
            if len(r) > 1:
                st.runs_ge2 += 1
            first = r[0]
            run_bad = False
            for d in r[1:]:
                bad = False
                for k in SCALARS:
                    if d.f[k] != first.f[k]:
                        st.fields[k] += 1
                        bad = True
                dv = d.vs != first.vs
                dv[WORLD] = False
                if dv.any():
                    st.fields['vs_bank'] += 1
                    st.regs['vs_bank'].update(int(x) // 4 for x in np.unique(np.nonzero(dv)[0] // 4 * 4))
                    bad = True
                dp = d.ps != first.ps
                if dp.any():
                    st.fields['ps_bank'] += 1
                    st.regs['ps_bank'].update(int(x) // 4 for x in np.unique(np.nonzero(dp)[0] // 4 * 4))
                    bad = True
                if d.mesh != first.mesh:
                    st.fields['mesh'] += 1
                    bad = True
                st.world_same_as_first += int(not (d.vs[WORLD] != first.vs[WORLD]).any())
                st.bad_members += int(bad)
                run_bad |= bad
            st.bad_runs += int(run_bad)


def record_counts(raw):
    """-> {site name: number of crowd member objects in the scene record} (names crowd:block: / crowd:floor:)."""
    import struct
    magic, ver, nsec = struct.unpack_from('<III', raw, 0)
    if magic != 0x4E435352 or ver != 1:
        raise LedgerError('not a scene record v1')
    secs = {}
    for i in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', raw, 40 + 24 * i)
        secs[sid] = (es, cnt, off)
    if 1 not in secs or 4 not in secs:
        raise LedgerError('scene record without strings / objects')
    st = raw[secs[1][2]:secs[1][2] + secs[1][1]]
    es, cnt, off = secs[4]
    if es != cc.OBJECT_SIZE:
        raise LedgerError('scene record Object element size %d, this reader reads %d' % (es, cc.OBJECT_SIZE))
    out = collections.Counter()
    for i in range(cnt):
        o = struct.unpack_from('<I', raw, off + es * i + 12)[0]
        if not o:
            continue
        nm = st[o - 1:st.index(b'\0', o - 1)]
        for site, pre in RECORD_PREFIX.items():
            if nm.startswith(pre):
                out[site] += 1
    return out


def measure(cap, frames, plant=None):
    rd = Reader(cap)
    S = {n: Stats() for n in SITES.values()}
    t0 = time.time()
    for fi in frames:
        f = cap.frame(fi)
        items, hist = rd.frame_items(f)
        before = {n: S[n].members for n in S}
        analyse_frame(items, hist, S, plant, cap.nums[fi])
        raw = cap.record(cap.nums[fi] - 1) if cap.nums[fi] > 0 else None
        if raw is not None:
            try:
                rc = record_counts(raw)
            except LedgerError:
                rc = None
            if rc is not None:
                for n in S:
                    calls = hist.get(n, 0)
                    if calls or rc.get(n, 0):
                        S[n].rec_frames += 1
                        S[n].rec_calls += calls
                        S[n].rec_members += rc.get(n, 0)
                        S[n].rec_match += int(calls == rc.get(n, 0))
    return S, len(frames), time.time() - t0


# ------------------------------------------------------------------- verdict and report ---------------------------------------------------------
def judge(label, S, nframes):
    """-> list of (level, text): level 'FAIL' breaks the verdict, 'INFO' does not."""
    out = []
    any_members = False
    for n, st in S.items():
        if not st.members:
            out.append(('INFO', '%s %s: no member draws in this capture' % (label, n)))
            continue
        any_members = True
        if st.bad_members:
            out.append(('FAIL', '%s %s: %d of %d members (%d of %d runs) differ from their run\'s first outside c76-c78: %s%s' % (
                label, n, st.bad_members, st.members, st.bad_runs, st.runs, dict(st.fields),
                (' vs-bank registers %s' % dict(st.regs['vs_bank'])) if st.regs['vs_bank'] else '') + ((' ps-bank registers %s' % dict(st.regs['ps_bank'])) if st.regs['ps_bank'] else '')))
        if st.calls_table != st.members:
            out.append(('FAIL', '%s %s: RL1: %d calls counted one at a time in the op list, %d in the draw table (a collapsed count)' % (label, n, st.members, st.calls_table)))
        if st.runs_ge2 and st.members <= st.runs:
            out.append(('FAIL', '%s %s: RL1: calls (%d) == runs (%d) although %d runs have more than one member: calls were merged' % (label, n, st.members, st.runs, st.runs_ge2)))
        if not st.runs_ge2:
            out.append(('FAIL', '%s %s: RL1: calls == runs (%d == %d), no run has more than one member: every call was taken for a run (a split boundary) or the site is not a loop' % (label, n, st.members, st.runs)))
        if st.members > st.runs and st.world_same_as_first == st.members - st.runs:
            out.append(('FAIL', '%s %s: every non-first member has the first one\'s world (c76-c78): the members are not members' % (label, n)))
    if not any_members:
        out.append(('INFO', '%s: no member draw at either site (not a case for the contract)' % label))
    return out


def report(label, S, nframes, secs):
    print('%-12s %d frames read in %.1f s' % (label, nframes, secs))
    for n, st in S.items():
        if not st.members:
            print('  %-7s no member draws' % n)
            continue
        drop = st.members - st.runs
        per_frame_drop_ms = drop / max(1, st.frames) * US_PER_DROP / 1000.0
        print('  %-7s frames %d | runs/f %.1f | members(calls)/f %.1f (min %d, max %d) | members/run %.2f (max %d, %d runs have >1) | RL1 calls %d == table %d, runs %d, frames with calls==runs %d' % (
            n, st.frames, st.runs / st.frames, st.members / st.frames, st.calls_min, st.calls_max, st.members / max(1, st.runs), st.max_run, st.runs_ge2, st.members, st.calls_table, st.runs,
            len(st.frames_calls_eq_runs)))
        print('          non-uniform members %d (runs %d) %s | kept %d / dropped %d (%.1f %% dropped), %.1f dropped/f x %.2f us = %.2f ms/f' % (
            st.bad_members, st.bad_runs, dict(st.fields) or 'by field: none', st.runs, drop, 100.0 * drop / st.members, drop / st.frames, US_PER_DROP, per_frame_drop_ms))
        if st.rec_frames:
            print('          scene record cross-check (informational): record members == stream calls in %d of %d frames (record %d, stream %d)' % (st.rec_match, st.rec_frames, st.rec_members, st.rec_calls))


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    lo, hi, step = (int(x) if x else None for x in (spec.split(':') + ['', ''])[:3])
    return list(range(lo or 0, min(hi if hi is not None else total, total), step or 1))


def run(captures, frames_spec='', plant=None, quiet=False, json_out=None):
    fails, bad_input, totals, allj = [], False, {n: dict(members=0, runs=0, bad=0, captures=0, multi=0) for n in SITES.values()}, {}
    for spec in captures:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        try:
            cap = cc.Cap(path)
            S, nf, secs = measure(cap, parse_frames(frames_spec, len(cap.frames)), plant)
        except (pcap.PError, cc.CensusError, LedgerError, OSError, KeyError, IndexError, zipfile.BadZipFile) as e:
            print('FAIL  %-12s cannot read %s: %s' % (label, path, e))
            bad_input = True
            continue
        if not quiet:
            report(label, S, nf, secs)
        for lv, tx in judge(label, S, nf):
            if lv == 'FAIL':
                fails.append(tx)
                print('FAIL  ' + tx)
            elif not quiet:
                print('      ' + tx)
        for n, st in S.items():
            t = totals[n]
            t['members'] += st.members
            t['runs'] += st.runs
            t['bad'] += st.bad_members
            t['captures'] += int(st.members > 0)
            t['multi'] += st.runs_ge2
        allj[label] = {n: st.jsonable() for n, st in S.items()}
    print('--- totals over %d capture(s)%s' % (len(captures), ' (plant %s)' % plant if plant else ''))
    for n, t in totals.items():
        print('  %-7s captures with members %d | runs %d | members %d | runs with >1 member %d | non-uniform members %d' % (n, t['captures'], t['runs'], t['members'], t['multi'], t['bad']))
        if not t['members'] or not t['multi']:
            fails.append('%s: no capture exercises the site (0 runs with more than one member): the census proves nothing for it' % n)
            print('FAIL  ' + fails[-1])
    if json_out:
        json.dump(allj, open(json_out, 'w'), indent=1, sort_keys=True, default=str)
    if bad_input:
        print('VERDICT  unreadable input: no verdict')
        return 2
    if fails:
        print('VERDICT  contract must change / census failed: %d finding(s)' % len(fails))
        return 1
    print('VERDICT  contract stands: 0 non-uniform members outside c76-c78 on every capture, RL1 holds (calls > runs, counts agree)')
    return 0


# ------------------------------------------------------------------- selftest (synthetic frames: no capture needed) -----------------------------
def synth(rng, runs_per_site=3, members=4):
    """One synthetic frame: both sites, runs of `members` identical draws except c76-c78; a barrier between sites."""
    items, hist = [], collections.Counter()
    base_f = {k: 0 for k in SCALARS}
    base_f.update(layout=b'L', vertex_data=b'v', index_data=b'i', fixed=b'\x01\x02', dynamic=b'd', bool_loop=b'b', slot_images=((0, 7), (13, 9)), slot_state=b's')
    for site in SITES.values():
        for r in range(runs_per_site):
            vs = rng.integers(0, 2 ** 32, 1024, dtype=np.uint32)
            ps = rng.integers(0, 2 ** 32, 1024, dtype=np.uint32)
            mesh = (1000 * (1 + r), 64, 5000 * (1 + r), 32)
            f = dict(base_f, vs_hash=11, ps_hash=12, vertex_count=40 + r, slot_images=((0, 7 + r), (13, 9)))
            for m in range(members):
                v = vs.copy()
                v[WORLD] = rng.integers(0, 2 ** 32, 12, dtype=np.uint32)
                items.append(Draw(site, mesh, dict(f), v, ps))
                hist[site] += 1
        items.append(None)
    return items, hist


def selftest():
    rng = np.random.default_rng(267)
    ok = True

    def verdict(plant):
        S = {n: Stats() for n in SITES.values()}
        for k in range(5):
            items, hist = synth(rng)
            # a fresh deep-enough copy per call: apply_plant copies what it mutates
            analyse_frame(items, hist, S, plant, k)
        return [t for lv, t in judge('synth', S, 5) if lv == 'FAIL'], S

    def expect(name, plant, want_fail, text=None):
        nonlocal ok
        f, S = verdict(plant)
        good = bool(f) == want_fail and (text is None or any(text in x for x in f))
        ok &= good
        print('%s  selftest %-18s %s%s' % ('PASS' if good else 'FAIL', name, 'reported: %s' % f[0][:150] if f else 'clean', '' if good else '   <-- WRONG'))
        return S
    S = expect('clean', None, False)
    st = S['stands']
    assert st.runs == 15 and st.members == 60 and st.calls_table == 60, (st.runs, st.members)
    expect('control perturb_world', 'perturb_world', False)
    expect('perturb_bank', 'perturb_bank', True, 'vs-bank registers')
    expect('perturb_ps_bank', 'perturb_ps_bank', True, 'ps-bank registers')
    expect('perturb_slot', 'perturb_slot', True, 'slot_images')
    expect('perturb_state', 'perturb_state', True, 'fixed')
    expect('perturb_buffer', 'perturb_buffer', True, 'index_data')
    expect('calls_collapsed', 'calls_collapsed', True, 'RL1')
    expect('boundary_split', 'boundary_split', True, 'RL1')
    expect('boundary_merge', 'boundary_merge', True, 'differ from their run')
    S2 = expect('boundary_merge fields', 'boundary_merge', True, 'mesh')
    # a perturbed non-first member must be named by its register
    f, S = verdict('perturb_bank')
    ok &= S['stands'].regs['vs_bank'] == {0: 5}
    print('%s  selftest perturb_bank register named: %s' % ('PASS' if S['stands'].regs['vs_bank'] == {0: 5} else 'FAIL', dict(S['stands'].regs['vs_bank'])))
    # a barrier splits a run: two halves of a run with a non-draw op between are two runs
    items, hist = synth(rng, 1, 4)
    items.insert(2, None)
    S3 = {n: Stats() for n in SITES.values()}
    analyse_frame(items, hist, S3, None, 0)
    good = S3['stands'].runs == 2 and S3['stands'].members == 4
    ok &= good
    print('%s  selftest barrier splits a run: stands runs %d members %d' % ('PASS' if good else 'FAIL', S3['stands'].runs, S3['stands'].members))
    # RL1 through the draw table: a table count that disagrees is reported
    items, hist = synth(rng)
    hist['stands'] -= 1
    S4 = {n: Stats() for n in SITES.values()}
    analyse_frame(items, hist, S4, None, 0)
    good = any('RL1' in t for lv, t in judge('synth', S4, 1) if lv == 'FAIL')
    ok &= good
    print('%s  selftest table/walk disagreement reported' % ('PASS' if good else 'FAIL'))
    print('selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='*')
    ap.add_argument('--frames', default='')
    ap.add_argument('--plant', choices=PLANTS)
    ap.add_argument('--json')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    caps = a.captures or ['%s=%s' % c for c in DEFAULTS]
    return run(caps, a.frames, a.plant, a.q, a.json)


if __name__ == '__main__':
    sys.exit(main())
