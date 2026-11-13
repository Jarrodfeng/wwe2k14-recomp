r"""Lane 274 (docs\renderer\SKIP_GAME_WORK_MAP.md section 10 "Lane 274"): `calls = runs` for EVERY D3 chain (arena, ring, props, wrestlers, other, casters), fed from the capture's real calls, not from the emulated hook.

  py -I tools\d3\calls_runs_check.py CAPTURE [--frames A:B:STEP] [--rec-off 1] [--unsplit] [--expect-clean arena,ring,wrestlers,casters] [--list N]

Why. fview's EmulateHook (the offline `calls = runs` guard of every d3 check) makes ONE emulated call per binder run, so the hook tally and the runs agree by construction. The ring's first live verify (RL1, run 7171) failed
because the real hook counted 44 calls and ScanRuns merged a model's back-to-back instance calls into 8 runs. This tool counts REAL calls independently: every sub_82C04DF0 call writes one LightRef (object, ordinal, state, hook
frame) into the scene record (lane 109), so the LightRefs of an object in a hook frame are the real colour calls. It replicates ScanRuns (d3_binder.h) op for op over the frame's op list:
  R  runs as ScanRuns makes them today (a run closes on another object, a non-draw op, a non-ModelPart draw; only runs with a keyed draw count)
  S  runs with `split_on_part_restart` (lane 258 / 274): a run also closes when the next KEYED draw of the same object has a part index <= the last keyed part OF THE SAME PASS SITE in the open run (the arena draws every part twice in one call, once at site A 0x82C0A950 and once at site B 0x82C0B94C) (the real calls as the draws show them)
  C  real calls of the object in the hook frame (LightRefs)
and tells, per chain, how many object-frames have C == R (the live guard holds), R < C == S (back-to-back instances merged: the ring's RL1 class), or something else (named). A hook call that drew no keyed part is
counted by LightRefs but not by the live hook tally (EndGuestCall counts a call only when it drew keyed parts or was skipped); `C > S` objects are listed as `extra calls` (no keyed draws: not a defect by itself).
Chains (a MEASUREMENT class, ledger.py classify_draw): wrestlers yCh_*, arena yBG_* and yProp_* whose world rows equal an arena world, ring the other yProp_*. Casters: the three caster pass sites against the CasterCall
records (kScCasterCalls) per (model, pass), against ScanCasterRuns' rule. 0 game runs, read only, numbers only. Exit 1 with --expect-clean CHAINS when a named chain has an object-frame that is not clean.

The SHIPPED scans split (arena and ring routes: ScanRuns split_on_site_restart; wrestlers are exempt, casters have their own rule), so an object-frame is `ok` when C == S (the runs the shipped scan makes). --unsplit judges against R instead (the pre-fix scan: ring and arena models read `merged`); the summary prints R and S side by side either way."""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L  # noqa: E402
from ledger_io import CLASS_NAMES, SEC_OPS  # noqa: E402
import struct  # noqa: E402

CHAINS = ('arena', 'ring', 'wrestlers', 'other')
CASTER_SITES = {0x82C04634: 1, 0x82BF4D38: 2, 0x82BFBD58: 3}
OP_DRAW = 2


def frame_ops(f):
    es, cnt, so = f.S[SEC_OPS]
    return [struct.unpack_from('<II', f.b, so + es * i) for i in range(cnt)]


def scan(f, reg, rindex, split_chains=()):
    """ScanRuns' replica over one frame. Returns [(object, chain, keyed, ndraws, [parts of keyed draws], has_split)] in op order, closing exactly as d3_binder.h does.
    With split_chains the run of an object of those chains also closes on a part restart (R == S for them). Returns (runs_R, runs_S) as lists of (object, chain, keyed_draws)."""
    out = {}
    for mode in ('R', 'S'):
        runs = []
        cur = None
        last_part = {}

        def close():
            nonlocal cur, last_part
            if cur is not None and cur[3] > 0 and cur[4] == 0:   # Run::Keyed(): keyed draws and no other-site draw of the object in the run
                runs.append(cur)
            cur = None
            last_part = {}
        for kind, idx in frame_ops(f):
            if kind != OP_DRAW:
                close()
                continue
            o = f.origin(idx)
            if not o or o['klass'] != 1 or not o['object']:
                close()
                continue
            chain = None
            wr = False
            d = f.draw(idx)
            keyed = o['pass_site'] in (L.SITE_A, L.SITE_B)
            if keyed:
                vn = reg.variant_of(d['vs'])
                vb = f.bank(d['vs_bank']) if vn else None
                chain = L.classify_draw(vn, o, vb, rindex) if vn else None
                wr = bool(vn and vn.startswith('yCh_'))   # the split exempts wrestler colour draws (d3_binder.h ScanRuns variant_of)
            if cur is not None and cur[0] != o['object']:
                close()
            if cur is not None and mode == 'S' and keyed and not wr and o['part'] <= last_part.get(o['pass_site'], -1):
                close()
            if cur is None:
                cur = [o['object'], None, [], 0, 0]
            if not keyed:
                cur[4] += 1
            else:
                cur[3] += 1
                cur[2].append(o['part'])
                last_part[o['pass_site']] = o['part']
                if chain and not cur[1]:
                    cur[1] = chain
        close()
        out[mode] = runs
    return out['R'], out['S']


def caster_runs(f):
    """ScanCasterRuns' replica: runs per (object, pass), a run closes on another object / pass, a non-increasing part, a non-draw op or a non-caster draw."""
    runs = []
    cur = None
    last_part = -1
    for kind, idx in frame_ops(f):
        if kind != OP_DRAW:
            cur, last_part = None, -1
            continue
        o = f.origin(idx)
        pas = CASTER_SITES.get(o['pass_site'], 0) if o else 0
        if not o or o['klass'] not in (1, 7) or not o['object'] or not pas:
            cur, last_part = None, -1
            continue
        if cur and (cur[0] != o['object'] or cur[1] != pas or o['part'] <= last_part):
            cur, last_part = None, -1
        if cur is None:
            cur = [o['object'], pas]
            runs.append(cur)
        last_part = o['part']
    return runs


def analyse(path, frames_spec='', rec_off=1, ring_objects=(), unsplit=False):
    cap = L.Capture(path)
    if not cap.has_scene:
        raise L.LedgerError('this capture has no scene records (the real calls are the point of the tool)')
    reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    frames = L.parse_frames(frames_spec, cap.n)
    recs, calls_of, ccalls_of = {}, collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    for i in range(cap.n):
        rec = cap.record(i)
        if rec is None:
            continue
        recs[i] = rec
        for obj, ordinal, state, frame in rec.light_refs():
            calls_of[frame][obj] += 1
        for c in rec.caster_calls():
            ccalls_of[c['frame']][(c['object'], c['pass_'])] += 1
    ring_objects = set(ring_objects)
    for rec in recs.values():
        for e in rec.ring_rows():
            ring_objects.add(e['object'])
    res = {c: collections.Counter() for c in CHAINS + ('casters',)}
    detail = collections.defaultdict(list)   # chain -> [(frame, object, C, R, S, tag)]
    n_frames = 0
    for fi in frames:
        f = cap.frame(fi)
        rec = recs.get(fi)
        if rec is None or not f.has_origins:
            continue
        n_frames += 1
        hook = rec.guest_frame - rec_off
        rindex = L.RecordIndex(rec)
        R, S = scan(f, reg, rindex)
        r_by, s_by, chain_of = collections.Counter(), collections.Counter(), {}
        for obj, ch, parts, nk, no in R:
            r_by[obj] += 1
            chain_of.setdefault(obj, 'ring' if obj in ring_objects else (ch or 'other'))
        for obj, ch, parts, nk, no in S:
            s_by[obj] += 1
        for obj in r_by:
            ch = chain_of[obj]
            c, r, s = calls_of.get(hook, {}).get(obj, 0), r_by[obj], s_by[obj]
            e = r if unsplit else s   # the runs the shipped scan makes (S) unless --unsplit
            if c == e:
                tag = 'ok'
            elif unsplit and c == s and r < c:
                tag = 'merged'
            elif c == 0:
                tag = 'norecord'
            elif c < e:
                tag = 'fewer_calls'
            elif c > e:
                tag = 'extra_calls'
            else:
                tag = 'other'
            res[ch][tag] += 1
            res[ch]['objects'] += 1
            res[ch]['calls'] += c
            res[ch]['runs'] += r
            res[ch]['splitruns'] += s
            if tag != 'ok':
                detail[ch].append((fi, obj, c, r, s, tag))
        # casters
        cr = collections.Counter((o_, p_) for o_, p_ in caster_runs(f))
        for k, r in cr.items():
            c = ccalls_of.get(hook, {}).get(k, 0)
            tag = 'ok' if c == r else ('norecord' if c == 0 else ('fewer_calls' if c < r else 'extra_calls'))
            res['casters'][tag] += 1
            res['casters']['objects'] += 1
            res['casters']['calls'] += c
            res['casters']['runs'] += r
            if tag != 'ok':
                detail['casters'].append((fi, k[0], c, r, r, tag + ' pass %d' % k[1]))
    res['_refs'] = sum(len(v) for v in calls_of.values())
    return res, detail, n_frames


def main():
    ap = argparse.ArgumentParser(description='calls = runs for every D3 chain from the capture real calls (lane 274)')
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--rec-off', type=int, default=1)
    ap.add_argument('--expect-clean', default='')
    ap.add_argument('--list', type=int, default=6)
    ap.add_argument('--unsplit', action='store_true', help='judge against the pre-fix scan (R) instead of the shipped one (S)')
    ap.add_argument('--ring-from', default='', help='comma list of other captures whose ring-rows objects are also the ring class (the heap addresses repeat between runs of one route; a capture without ring-rows records, J1 / 7147, needs them)')
    a = ap.parse_args()
    ro = set()
    for p in [x for x in a.ring_from.split(',') if x]:
        c2 = L.Capture(p)
        for i in range(c2.n):
            r2 = c2.record(i)
            if r2 is not None:
                ro.update(e['object'] for e in r2.ring_rows())
    res, detail, n = analyse(a.capture, a.frames, a.rec_off, ro, a.unsplit)
    if not res['_refs']:
        print('NOTE  this capture holds no LightRefs (it predates lane 109: Check B / run 7079): real calls are unknown, every object reads `no record`. Compare R and S per chain instead: S > R is the merged-instance signature.')
    print('frames %d (object-frame = an object with a keyed run in a frame; C real calls, R ScanRuns runs today, S runs split at a part restart)' % n)
    for ch in CHAINS + ('casters',):
        r = res[ch]
        print('%-9s object-frames %6d: ok %6d, merged (R<C==S) %5d, fewer calls than runs %4d, extra calls (C>S) %4d, no record %4d, other %3d | sum C %7d R %7d S %7d' %
              (ch, r['objects'], r['ok'], r['merged'], r['fewer_calls'], r['extra_calls'], r['norecord'], r['other'], r['calls'], r['runs'], r['splitruns']))
    for ch in CHAINS + ('casters',):
        for fi, obj, c, r, s, tag in detail[ch][:a.list]:
            print('  %-9s frame %d object %08X: real calls %d, runs today %d, runs split %d  [%s]' % (ch, fi, obj, c, r, s, tag))
    bad = [x for x in a.expect_clean.split(',') if x and (res[x]['objects'] - res[x]['ok'])]
    if bad:
        print('FAIL  not clean: ' + ', '.join(bad))
        return 1
    print('PASS' if a.expect_clean else 'INFO')
    return 0


if __name__ == '__main__':
    sys.exit(main())
