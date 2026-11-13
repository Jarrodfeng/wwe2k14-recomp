r"""S101 lane 196 (docs\renderer\SKIP_GAME_WORK_MAP.md sections 17 and 18): the temporal + source ledger for candidates E (shadow / rim / plane / circle CASTER passes)
and H (wrestlers D3). Offline, 0 game runs, read-only, numbers only (nothing from the game is written; --json is for under out\).

  py -I tools\skip\caster_ledger.py CAPTURE --class casters|wrestlers [--frames A:B:STEP] [--record-offset N] [--light-state] [--json OUT] [--expect NAME] [--plant NAME] [-q]

Classes (a measurement rule, like tools\skip\ledger.py; the hooks join by file content):
  casters    map (origin pass site 0x82C04634: props map, map A, map B), plane (0x82BF4D38), rim (pass site 0x82BFBD58, class model_part), circle (call site 0x828131E4)
  wrestlers  main-pass model draws (pass 0x82C0A950 / 0x82C0B94C) whose VS variant is yCh_*
Key = {sub-class, object, part, ordinal among the frame's draws of that (site, object, part)}: map A and map B draw the same (object, part), so ordinal 0 / 1 are the two maps.
For EVERY constant register a draw's programs read (the fx registry's ranges of the variant) the tool tests, independently (a read can have several):
  trivial  all of 0 / 1 / -1 (carries no information)                         pass   the value >= 90 % of the draws of the same (pass site / call site) share this frame
  record   equals a scene-record field of record (frame - offset)              colour  equals the same stage / register of a MAIN-pass colour draw of the SAME object, same frame
  lstate   equals a register built from a light state the record carries (--light-state; J1 only)
  held     equals the same key's value in the previous frame (consecutive frames)    first  equals the key's first-appearance value (template upper bound)
and a draw's verdicts: ready (every read trivial / pass / record / colour / lstate), ready+held (held also counts), ready+held+first. The tool also prints the unsourced registers by
fx field name, the cross-tab of the palette rows (c76..c138) against the colour pass and the record, per-slot texture-image holds, and per-frame draw counts.
--plant (tests only): colour_off (the colour source off: the map B / rim palette rows become unsourced), offset0 (records read at offset 0), shift_prev (held against two frames back),
no_ordinal (the ordinal dropped: map A / map B merge), no_trivial (trivial vectors need a source).
"""
import argparse
import collections
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import ledger as L  # noqa: E402  (Capture, Registry, RecordIndex, LightFrameIndex, parse_frames, LedgerError, reg_name, TRIVIAL, SITE_A, SITE_B)
from ledger_io import CLASS_NAMES  # noqa: E402

SITE_MAP, SITE_PLANE, SITE_RIM, CALL_CIRCLE = 0x82C04634, 0x82BF4D38, 0x82BFBD58, 0x828131E4
SEC_TEXSLOTS = 10
PLANTS = ('colour_off', 'offset0', 'shift_prev', 'no_ordinal', 'no_trivial')
SOURCES = ('trivial', 'pass', 'record', 'colour', 'lstate')


def sub_class(klass_name, o, vn, mode):
    """The sub-class of a draw, or None when the draw is not in the studied class."""
    if mode == 'casters':
        if o['pass_site'] == SITE_MAP and klass_name == 'shadow':
            return 'map'
        if o['pass_site'] == SITE_PLANE and klass_name == 'shadow':
            return 'plane'
        if o['pass_site'] == SITE_RIM and klass_name == 'model_part':
            return 'rim'
        if o['call_site'] == CALL_CIRCLE and klass_name == 'shadow':
            return 'circle'
        return None
    if klass_name == 'model_part' and o['pass_site'] in (L.SITE_A, L.SITE_B) and vn and vn.startswith('yCh_'):
        return 'wrestler_mirror' if o['pass_site'] == L.SITE_B else 'wrestler'
    return None


def group_of(stage, reg):
    if stage == 'v' and 76 <= reg <= 138:
        return 'world' if reg <= 78 else 'palette'
    if reg <= 12 or reg in (181, 182, 197, 199):
        return 'light'
    if 68 <= reg <= 71 or 164 <= reg <= 166 or 168 <= reg <= 170:
        return 'shadow'
    if reg == 186:
        return 'clock'
    return 'other'


def tex_images(f, d):
    if SEC_TEXSLOTS not in f.S:
        return {}
    es, cnt, so = f.S[SEC_TEXSLOTS]
    out = {}
    for k in range(d['tex_first'], min(d['tex_first'] + d['tex_count'], cnt)):
        image, _sampler, slot = struct.unpack_from('<IIB', f.b, so + es * k)
        out[slot] = image
    return out


def run(path, mode, frames_spec='', rec_off=1, plants=(), light_state=False):
    t0 = time.time()
    cap = L.Capture(path)
    reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    frames = L.parse_frames(frames_spec, cap.n)
    if not frames:
        raise L.LedgerError('--frames selects no frame')
    if 'offset0' in plants:
        rec_off = 0
    lfi, light_off = None, 0
    if light_state and cap.has_scene:
        lfi = L.LightFrameIndex(cap.record(i) for i in range(cap.n))
        if lfi.records == 0:
            raise L.LedgerError('--light-state: no record of this capture carries light tables')
        light_off, _ = L.detect_light_offset(cap, frames, reg, lfi)
    first, prev, prev2 = {}, {}, {}
    segprev = {}
    tfirst, tprev = {}, {}
    sc = collections.defaultdict(lambda: dict(
        draws=0, nodraw_comp=0, ready=0, ready_held=0, ready_held_first=0, new_key=0, reads=0, g=collections.defaultdict(collections.Counter),
        pal_obj=collections.defaultdict(collections.Counter), unsrc=collections.Counter(), unsrc_held=collections.Counter(), unsrc_sets=collections.Counter(), pal=collections.Counter(), tex=collections.defaultdict(collections.Counter),
        per_frame=[], full_held=0, comparable=0, segs=[], seg_total=0, seg_cmp=0, seg_held=0, variants=collections.Counter(), objects=set(), calls_prev={}, calls_changed=0, calls_frames=0))
    nrec = 0
    skipped = 0
    last_fi = None
    for fi in frames:
        f = cap.frame(fi)
        if not f.has_origins:
            raise L.LedgerError('frame %d has no draw origins (a version-1 capture)' % fi)
        contiguous = last_fi is not None and fi == last_fi + 1
        last_fi = fi
        ri = fi - rec_off
        rec = cap.record(ri) if (cap.has_scene and 0 <= ri < cap.n) else None
        if cap.has_scene and rec is None:
            skipped += 1
            continue
        nrec += 1 if rec else 0
        rindex = L.RecordIndex(rec) if rec else None
        draws, colour_by_obj = [], collections.defaultdict(lambda: collections.defaultdict(set))
        groups = collections.defaultdict(list)
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o:
                continue
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            kn = CLASS_NAMES[o['klass']]
            sub = sub_class(kn, o, vn, mode)
            if sub:
                draws.append((i, o, d, vn, sub))
                groups[(o['pass_site'], o['call_site'])].append(len(draws) - 1)
            if mode == 'casters' and kn == 'model_part' and o['pass_site'] in (L.SITE_A, L.SITE_B) and vn:
                vb = f.bank(d['vs_bank'])
                cobj = colour_by_obj[o['object']]
                for r in reg.read_regs(vn):
                    cobj[('v', r)].add(tuple(vb[4 * r:4 * r + 4]))
        if not draws:
            continue
        # segments: runs of consecutive caster draws of one (sub-class, site, call site) with one camera (map: VS c56-c59; plane c13-c16; circle c4-c7; wrestlers: c56-c59)
        segs, segof, cur = [], [], None
        for (i, o, d, vn, sub) in draws:
            vb_ = f.bank(d['vs_bank'])
            base = {'map': 56, 'rim': 56, 'plane': 13, 'circle': 4}.get(sub, 56)
            camv = tuple(vb_[4 * base:4 * base + 16])
            ck = (sub, o['pass_site'], o['call_site'], camv)
            if cur is None or segs[-1] != ck:
                segs.append(ck)
            segof.append(len(segs) - 1)
            cur = ck
        seg_n = collections.Counter(segof)
        # pass bank: per (segment, stage, reg) the value >= 90 % of the segment's readers share (segments of one draw prove nothing: not a source)
        votes, reads_ = collections.defaultdict(collections.Counter), collections.Counter()
        meta = []
        for (i, o, d, vn, sub) in draws:
            pn = reg.variant_of(d['ps']) if d['ps'] else None
            vb = f.bank(d['vs_bank'])
            pb = f.bank(d['ps_bank']) if d['ps'] else None
            meta.append((pn, vb, pb))
            for stage, name, bank in (('v', vn, vb), ('p', pn, pb)):
                if not name:
                    continue
                for r in reg.read_regs(name):
                    k = (segof[len(meta) - 1], stage, r)
                    votes[k][tuple(bank[4 * r:4 * r + 4])] += 1
                    reads_[k] += 1
        lcam = None
        if lfi is not None and rec is not None:
            lcam = set()
            for s in rec.light_states():
                for m in s['shadow_m']:
                    for c in range(4):
                        lcam.add(tuple(struct.unpack('<4I', struct.pack('<4f', *m[c::4]))))
                        lcam.add(tuple(struct.unpack('<4I', struct.pack('<4f', *m[4 * c:4 * c + 4]))))
                for c in range(4):
                    lcam.add(tuple(struct.unpack('<4I', struct.pack('<4f', *s['shadow_f'][4 * c:4 * c + 4]))))
                for xk in s['shadow_x']:
                    for c in range(2):
                        lcam.add(tuple(struct.unpack('<4I', struct.pack('<4f', *xk[4 * c:4 * c + 4]))))
        passbank = {k: c.most_common(1)[0][0] for k, c in votes.items() if c.most_common(1)[0][1] >= 2 and c.most_common(1)[0][1] >= 0.9 * reads_[k]}
        seen = collections.Counter()
        frame_calls = collections.Counter()
        segs_by_sub = collections.Counter(s[0] for s in segs)
        for sname, cnt in segs_by_sub.items():
            sc[sname]['segs'].append(cnt)
        # a segment's camera held against the previous frame's segment of the same ordinal (sub-class, k-th segment)
        ordc = collections.Counter()
        for sidx, ck in enumerate(segs):
            kq = (ck[0], ordc[ck[0]])
            ordc[ck[0]] += 1
            pv = segprev.get(kq)
            S0 = sc[ck[0]]
            S0['seg_total'] += 1
            if pv is not None and contiguous:
                S0['seg_cmp'] += 1
                if pv == ck[3]:
                    S0['seg_held'] += 1
            segprev[kq] = ck[3]
        for n, (i, o, d, vn, sub) in enumerate(draws):
            pn, vb, pb = meta[n]
            S = sc[sub]
            k0 = (sub, o['pass_site'], o['call_site'], o['object'], o['part'])
            ordinal = 0 if 'no_ordinal' in plants else seen[k0]
            seen[k0] += 1
            frame_calls[(sub, o['object'])] += 1
            key = k0 + (ordinal,)
            S['draws'] += 1
            S['objects'].add(o['object'])
            S['variants'][vn or '?'] += 1
            if d['ps'] and not pn:
                S['nodraw_comp'] += 1   # a pixel shader the registry does not know: not counted as ready
            new_key = False
            all_ready = all_ready_held = all_ready_held_first = True
            held_all = True
            unsrc_here = []
            for stage, name, bank in (('v', vn, vb), ('p', pn, pb)):
                if not name:
                    continue
                for r in sorted(reg.read_regs(name)):
                    v = tuple(bank[4 * r:4 * r + 4])
                    g = group_of(stage, r)
                    S['reads'] += 1
                    src = set()
                    if set(v) <= L.TRIVIAL and 'no_trivial' not in plants:
                        src.add('trivial')
                    if seg_n[segof[n]] >= 2 and passbank.get((segof[n], stage, r)) == v:
                        src.add('pass')
                    if rindex is not None and not (set(v) <= L.TRIVIAL) and rindex.lookup(v):
                        src.add('record')
                    if 'colour_off' not in plants and stage == 'v' and v in colour_by_obj.get(o['object'], {}).get((stage, r), ()) and not (set(v) <= L.TRIVIAL):
                        src.add('colour')
                    if lfi is not None and group_of(stage, r) in ('light', 'shadow') and lfi.match(f.guest_frame - light_off, o['object'], stage, r, v):
                        src.add('lstate')
                    if lcam is not None and mode == 'casters' and ((stage == 'v' and r in (54, 56, 57, 58, 59, 64, 65, 66, 67)) or (stage == 'p' and r in (168, 169, 170)))                             and v in lcam and not (set(v) <= L.TRIVIAL):
                        src.add('lstate')   # the map cameras against the shadow matrices of the record's light states (casters only; the wrestlers' shadow registers are NOT matched this way: 0 of 1920 reads on J1, lane 196 probe)
                    kk = (key, stage, r)
                    held = first_eq = False
                    if kk not in first:
                        first[kk] = v
                        new_key = True
                    else:
                        ref = prev2.get(kk) if 'shift_prev' in plants else prev.get(kk)
                        want = fi - (2 if 'shift_prev' in plants else 1)
                        held = bool(ref) and contiguous and ref[0] == want and ref[1] == v
                        first_eq = first[kk] == v
                    if kk in prev:
                        prev2[kk] = prev[kk]
                    prev[kk] = (fi, v)
                    if not held:
                        held_all = False
                    ready = bool(src)
                    for s in src:
                        S['g'][g][s] += 1
                    S['g'][g]['reads'] += 1
                    if held:
                        S['g'][g]['held'] += 1
                    if first_eq:
                        S['g'][g]['first'] += 1
                    if ready:
                        S['g'][g]['ready'] += 1
                    elif held:
                        S['g'][g]['held_only'] += 1
                    elif first_eq:
                        S['g'][g]['first_only'] += 1
                    else:
                        S['g'][g]['none'] += 1
                    if not ready:
                        all_ready = False
                    if not (ready or held):
                        all_ready_held = False
                    if not (ready or held or first_eq):
                        all_ready_held_first = False
                        S['unsrc'][(stage, r)] += 1
                        unsrc_here.append((stage, r))
                    elif not ready:
                        S['unsrc_held'][(stage, r)] += 1
                    if stage == 'v' and 76 <= r <= 138 and name and 'skinned' in name:
                        lab = 'colour' if 'colour' in src else 'record' if 'record' in src else 'other_src' if src else 'held_only' if held else 'NONE'
                        S['pal'][lab] += 1
                        S['pal_obj'][o['object']][lab] += 1
            if not new_key:
                S['comparable'] += 1
                if held_all:
                    S['full_held'] += 1
            else:
                S['new_key'] += 1
            if all_ready:
                S['ready'] += 1
            if all_ready_held:
                S['ready_held'] += 1
            if all_ready_held_first:
                S['ready_held_first'] += 1
            S['unsrc_sets'][tuple(sorted(unsrc_here))] += 1
            for slot, image in sorted(tex_images(f, d).items()):
                kk = (key, slot)
                T = S['tex'][slot]
                T['reads'] += 1
                if kk not in tfirst:
                    tfirst[kk] = image
                    T['first'] += 1
                else:
                    if tprev.get(kk) == image and contiguous:
                        T['held_prev'] += 1
                tprev[kk] = image
        for sub in {k[0] for k in seen}:
            S = sc[sub]
            n = sum(c for (s, _o), c in frame_calls.items() if s == sub)
            S['per_frame'].append(n)
            for (s, obj), c in frame_calls.items():
                if s != sub:
                    continue
                if obj in S['calls_prev'] and contiguous:
                    S['calls_frames'] += 1
                    if S['calls_prev'][obj] != c:
                        S['calls_changed'] += 1
                S['calls_prev'][obj] = c
    res = dict(path=cap.dir, mode=mode, frames=len(frames), frames_without_record=skipped, rec_off=rec_off, records_read=nrec,
               light_offset=light_off if lfi is not None else None, seconds=round(time.time() - t0, 1), sub={})
    for sub, S in sorted(sc.items()):
        pf = S['per_frame']
        un = S['unsrc']
        order = [k for k, _ in un.most_common()]
        curve = []
        for kk in (1, 2, 3, 5, 8):
            top = set(order[:kk])
            ok = sum(c for s, c in S['unsrc_sets'].items() if set(s) <= top)
            curve.append((kk, 100.0 * ok / S['draws'] if S['draws'] else 0.0))
        res['sub'][sub] = dict(
            draws=S['draws'], per_frame=(sum(pf) / len(pf)) if pf else 0.0, per_frame_min=min(pf) if pf else 0, per_frame_max=max(pf) if pf else 0,
            objects=len(S['objects']), variants=dict(S['variants'].most_common(6)), reads=S['reads'], unknown_ps=S['nodraw_comp'],
            ready=S['ready'], ready_held=S['ready_held'], ready_held_first=S['ready_held_first'], new_key=S['new_key'], comparable=S['comparable'], full_held=S['full_held'],
            groups={g: dict(c) for g, c in S['g'].items()}, palette=dict(S['pal']), palette_obj={'%08X' % k: dict(v) for k, v in S['pal_obj'].items()},
            unsrc_top=[('%s c%d %s' % ('VS' if st == 'v' else 'PS', r, L.reg_name(st, r)), n) for (st, r), n in un.most_common(10)],
            unsrc_held_top=[('%s c%d %s' % ('VS' if st == 'v' else 'PS', r, L.reg_name(st, r)), n) for (st, r), n in S['unsrc_held'].most_common(6)],
            segs_per_frame=(sum(S['segs']) / len(S['segs'])) if S['segs'] else 0.0, seg_cmp=S['seg_cmp'], seg_held=S['seg_held'], curve=curve, calls_changed=S['calls_changed'], calls_frames=S['calls_frames'],
            tex={str(slot): dict(reads=c['reads'], first=c['first'], held_prev=c['held_prev']) for slot, c in sorted(S['tex'].items())})
    return res


def pct(a, b):
    return '%6.1f%%' % (100.0 * a / b) if b else '     - '


def report(res):
    print('capture %s  class %s  frames %d  records read %d (offset %+d, %d frames without a record)  light offset %s  %.0f s' % (
        res['path'], res['mode'], res['frames'], res['records_read'], res['rec_off'], res['frames_without_record'], res['light_offset'], res['seconds']))
    for sub, S in res['sub'].items():
        n = S['draws']
        print('== %-16s draws %7d (%.1f a frame, min %d max %d), objects %d, reads %d' % (sub, n, S['per_frame'], S['per_frame_min'], S['per_frame_max'], S['objects'], S['reads']))
        print('   variants: ' + ', '.join('%s %d' % kv for kv in S['variants'].items()))
        print('   draws 100%% sourced (trivial/pass/record/colour/lstate): %s | +held: %s | +held +first-value (template upper bound): %s | fully held (all reads = last frame): %s of %d comparable | call-count changes %d of %d' % (
            pct(S['ready'], n), pct(S['ready_held'], n), pct(S['ready_held_first'], n), pct(S['full_held'], S['comparable']), S['comparable'], S['calls_changed'], S['calls_frames']))
        print('   camera segments a frame %.1f; a segment camera equal to the previous frame same-ordinal segment: %s of %d' % (S['segs_per_frame'], pct(S['seg_held'], S['seg_cmp']).strip(), S['seg_cmp']))
        print('   %-8s %9s %8s %8s %8s %8s %8s %8s | %8s %8s %8s' % ('group', 'reads', 'trivial', 'pass', 'record', 'colour', 'lstate', 'ANY src', '=prev', '=first', 'NONE'))
        for g, c in sorted(S['groups'].items()):
            rd = c.get('reads', 0)
            print('   %-8s %9d %8s %8s %8s %8s %8s %8s | %8s %8s %8s' % (g, rd, pct(c.get('trivial', 0), rd), pct(c.get('pass', 0), rd), pct(c.get('record', 0), rd), pct(c.get('colour', 0), rd),
                  pct(c.get('lstate', 0), rd), pct(c.get('ready', 0), rd), pct(c.get('held', 0), rd), pct(c.get('first', 0), rd), pct(c.get('none', 0), rd)))
        if S['palette']:
            tot = sum(S['palette'].values())
            print('   palette rows (skinned VS c76..c138) by source: ' + ', '.join('%s %s' % (k or 'none', pct(v, tot).strip()) for k, v in sorted(S['palette'].items(), key=lambda kv: -kv[1])))
        for ob, c in sorted(S['palette_obj'].items()):
            tot = sum(c.values())
            if c.get('NONE', 0) > 0.02 * tot:
                print('   palette object %s: rows %d, NONE %s, record %s, colour %s' % (ob, tot, pct(c.get('NONE', 0), tot).strip(), pct(c.get('record', 0), tot).strip(), pct(c.get('colour', 0), tot).strip()))
        print('   unsourced (not any source, not held, not first-value) top: ' + '; '.join('%s x%d' % kv for kv in S['unsrc_top']))
        if S['unsrc_held_top']:
            print('   no source but held/first-equal top: ' + '; '.join('%s x%d' % kv for kv in S['unsrc_held_top']))
        print('   draws that would be 100%% (sources + held + first) if the top K unsourced registers were closed: ' + ', '.join('K=%d %.1f%%' % kv for kv in S['curve']))
        if S['tex']:
            print('   texture slots (reads, =prev frame): ' + ', '.join('s%s %d %s' % (k, v['reads'], pct(v['held_prev'], v['reads'] - v['first']).strip()) for k, v in S['tex'].items()))


def main(argv=None):
    ap = argparse.ArgumentParser(description='caster / wrestler ledger (S101 lane 196)')
    ap.add_argument('capture')
    ap.add_argument('--class', dest='mode', choices=('casters', 'wrestlers'), required=True)
    ap.add_argument('--frames', default='')
    ap.add_argument('--record-offset', type=int, default=1)
    ap.add_argument('--light-state', action='store_true')
    ap.add_argument('--json')
    ap.add_argument('--plant', choices=PLANTS, action='append', default=[])
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args(argv)
    try:
        res = run(a.capture, a.mode, a.frames, a.record_offset, tuple(a.plant), a.light_state)
    except (L.LedgerError, OSError, ValueError, KeyError) as e:
        print('FAIL  %s: %s' % (type(e).__name__, e))
        return 1
    if not a.q:
        report(res)
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as fh:
            json.dump(res, fh, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
