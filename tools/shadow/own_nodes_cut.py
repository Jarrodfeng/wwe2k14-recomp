r"""S102 lane P4P5: the shadow module's and the rim map's draw nodes as groups of a captured frame: census, and the cut. 0 game runs, offline.

  py -I tools\shadow\own_nodes_cut.py census CAPTURE [--json OUT] [--frames A:B]
  py -I tools\shadow\own_nodes_cut.py cut CAPTURE OUT.zip --which module|rim|both [--only-frames A:B]
  py -I tools\shadow\own_nodes_cut.py oracle CAPTURE        (rule-independent: what of the two nodes is LEFT in a capture; exit 1 when anything is)

CAPTURE = a case zip (the Bryan entrance / the 15-type match of the standard captures; relative paths are read from the main checkout).

What a "group" is: the passes (SetTargets up to the next SetTargets, with their clears, draws and copies) the game's two draw nodes make in one frame.
  module  sub_827E9740 -> sub_827E95D8: props map + circle blobs, map A, map B, the two coverage reductions, the min blurs   (pass kinds props, mapA, mapB, reduce, min_*)
  rim     sub_827EA7B8 -> sub_827EA098: the rim light map, its two reductions, its blur                                      (pass kinds rim, rim_reduce, rim_t2)
The pass kinds are shadow_census.py's classification (draw origins: pass_site / the recorder's queue site / the effect), the same rule the C++ planner
(src\renderer_adapter\own_group.h) applies; the plane chain (category 0, lane P3) is NOT in either group.

`census` prints, per capture and group: frames carrying it, passes / ops / draws / copies per frame (min..max), whether the passes of the group are ONE run of the frame's
ops (no foreign pass between the first and last: "contiguous"), what stands directly before / after, the images the group writes and which later draws read them (slot, effect
family), and the image ids the check compares. `cut` writes a copy of the case zip in which every op of the chosen group(s) is removed (the node's walk is not run: the emulation of
"the node returned at once"); nothing else in the frame is touched. The cut is the planted twin of the lane: with nothing drawing the pass, the picture must differ.
Counts only; the output zip is scratch for under out\ (delete it after the comparison)."""
import argparse
import collections
import json
import os
import struct
import subprocess
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shadow_census as sc  # noqa: E402

pcap = sc.pcap

MODULE_KINDS = ('props', 'mapA', 'mapB', 'reduce', 'min_pair', 'min_rgba', 'min_r', 'min_rg')
RIM_KINDS = ('rim', 'rim_reduce', 'rim_t2')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def resolve(path):
    if os.path.isabs(path) or os.path.exists(path):
        return path
    return os.path.join(main_checkout(HERE), path)


class PassX:
    """shadow_census.Pass (slotted) plus the span end and the kind."""

    def __init__(self, q):
        self.target, self.depth, self.pos, self.draws, self.copies, self.clear = q.target, q.depth, q.pos, q.draws, q.copies, q.clear
        self.end, self.kind = 0, None


def classify(cap, f):
    """-> (passes with .kind and .end set, copies table). The kind rule of shadow_census.do_frame (origins first; never a hash alone), plane kinds kept apart."""
    d = f.draws
    o = f.origins
    ps = o['pass_site'].astype(np.int64)
    cs = pcap.queue_site(o)
    ps_eff = cap.effs(d['ps_hash'])
    copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
    passes = [PassX(q) for q in sc.walk(f, copies)]
    n = len(f.ops)
    for i, p in enumerate(passes):
        p.end = passes[i + 1].pos if i + 1 < len(passes) else n
        p.kind = None
        if not p.draws:
            continue
        ix = np.array(p.draws)
        sites, calls, effs = ps[ix], cs[ix], ps_eff[ix]
        tr = cap.imgs[p.target]
        if (sites == sc.SITE_PLANE).any():
            p.kind = 'plane'
        elif (sites == sc.SITE_MAP).any():
            p.kind = 'props' if int(tr['format']) == sc.FMT_RGBA8 else ('mapA' if int(tr['gf']) == 4 else 'mapB' if int(tr['gf']) == 5 else 'map?')
        elif (sites == sc.SITE_RIM).any():
            p.kind = 'rim'
        elif (calls == sc.CALL_PBLUR).any():
            p.kind = 'plane_blur'
        elif (calls == sc.CALL_DOWN).any():
            p.kind = 'plane_down'
        elif (calls == sc.CALL_REDUCE).any():
            p.kind = 'reduce'
        elif ((calls == sc.CALL_RIMRED) & (effs == 'yBlur/flag_any')).any():
            p.kind = 'rim_reduce'
        elif (calls == sc.CALL_MINBLUR).any():
            e = effs[calls == sc.CALL_MINBLUR][0]
            p.kind = {'yBlur/min_pair': 'min_pair', 'yBlur/min_rgba': 'min_rgba', 'yBlur/min_r': 'min_r', 'yBlur/min_rg': 'min_rg', 'yBlur/t2': 'rim_t2'}.get(e, 'min?')
        elif (calls == sc.CALL_CIRCLE).any():
            p.kind = 'props'
    return passes, copies


def group_of(kind):
    return 'module' if kind in MODULE_KINDS else 'rim' if kind in RIM_KINDS else None


def frame_groups(cap, fi):
    """-> dict group -> list of passes, the frame's passes, copies, f"""
    f = cap.frame(fi)
    passes, copies = classify(cap, f)
    g = {'module': [], 'rim': []}
    for p in passes:
        k = group_of(p.kind)
        if k:
            g[k].append(p)
    return g, passes, copies, f


def frame_range(spec, n):
    if not spec:
        return range(n)
    a, b = spec.split(':')
    return range(int(a or 0), min(int(b or n), n))


def census(cap, rng):
    S = {g: dict(frames=0, passes=[], ops=[], draws=[], copies=[], noncontig=0, foreign=collections.Counter(), before=collections.Counter(), after=collections.Counter(),
                 kinds=collections.Counter(), dests=set(), targets=set(), readers=collections.Counter(), orderviol=0) for g in ('module', 'rim')}
    both_adjacent = both_frames = 0
    reads_inside = 0
    for fi in rng:
        g, passes, copies, f = frame_groups(cap, fi)
        d = f.draws
        tf, tc = d['tex_first'].astype(np.int64), d['tex_count'].astype(np.int64)
        sl_img = f.slots['image'].astype(np.int64)
        nd = len(d)
        didx, dpos = f.draws_ops() if hasattr(f, 'draws_ops') else f.draw_ops()
        pos_of = np.full(nd, -1, np.int64)
        pos_of[didx] = dpos
        spans = {}
        for name, ps in g.items():
            if not ps:
                continue
            s = S[name]
            s['frames'] += 1
            first, last = ps[0].pos, ps[-1].end
            spans[name] = (first, last)
            s['passes'].append(len(ps))
            s['ops'].append(sum(p.end - p.pos for p in ps))
            s['draws'].append(sum(len(p.draws) for p in ps))
            s['copies'].append(sum(len(p.copies) for p in ps))
            mem = {p.pos for p in ps}
            foreign = [q for q in passes if first <= q.pos < last and q.pos not in mem]
            if foreign:
                s['noncontig'] += 1
                for q in foreign:
                    s['foreign'][q.kind or ('role%d' % int(cap.imgs[q.target]['role']))] += 1
            for p in ps:
                s['kinds'][p.kind] += 1
                s['targets'].add(p.target)
                for ci in p.copies:
                    s['dests'].add(int(copies[ci]['d']['image']))
            i0 = passes.index(ps[0])
            i1 = passes.index(ps[-1])
            if i0 > 0:
                q = passes[i0 - 1]
                s['before'][q.kind or ('role%d_%dx%d' % (int(cap.imgs[q.target]['role']), int(cap.imgs[q.target]['w']), int(cap.imgs[q.target]['h'])))] += 1
            if i1 + 1 < len(passes):
                q = passes[i1 + 1]
                s['after'][q.kind or ('role%d_%dx%d' % (int(cap.imgs[q.target]['role']), int(cap.imgs[q.target]['w']), int(cap.imgs[q.target]['h'])))] += 1
            # who reads what the group wrote: texture slots of draws OUTSIDE the group's own passes (a read of a group image inside the group is a chain step)
            written = {p.target for p in ps} | {int(copies[ci]['d']['image']) for p in ps for ci in p.copies}
            own_draws = {dd for p in ps for dd in p.draws}
            for dd in range(nd):
                if dd in own_draws:
                    continue
                imgs = sl_img[int(tf[dd]):int(tf[dd]) + int(tc[dd])]
                hit = [int(x) for x in imgs if int(x) in written]
                if hit:
                    inside = first <= int(pos_of[dd]) < last
                    if inside:
                        reads_inside += 1
                    for h in hit:
                        s['readers']['image %d by effect %s%s' % (h, cap.eff(int(d['ps_hash'][dd])).split('/')[0], ' (INSIDE the span)' if inside else '')] += 1
        if 'module' in spans and 'rim' in spans:
            both_frames += 1
            both_adjacent += int(spans['module'][1] == spans['rim'][0])
    return S, dict(both_frames=both_frames, both_adjacent=both_adjacent, reads_inside=reads_inside)


def mm(v):
    return '%d..%d' % (min(v), max(v)) if v else '-'


def print_census(label, S, extra, nframes):
    print('== %s: %d frames' % (label, nframes))
    for name in ('module', 'rim'):
        s = S[name]
        print('  %-6s frames %d | passes %s ops %s draws %s copies %s per frame | kinds %s' % (name, s['frames'], mm(s['passes']), mm(s['ops']), mm(s['draws']), mm(s['copies']), dict(s['kinds'])))
        print('         contiguous: %s (frames with a foreign pass inside: %d %s)' % ('YES, ONE run of the frame\'s ops' if not s['noncontig'] else 'NO', s['noncontig'], dict(s['foreign'])))
        print('         directly before: %s | directly after: %s' % (dict(s['before']), dict(s['after'])))
        print('         images written: targets %s, copy destinations %s' % (sorted(s['targets']), sorted(s['dests'])))
        print('         read by later draws: %s' % dict(s['readers']))
    print('  module directly followed by rim in %d of %d frames that carry both; draws of OTHER passes inside a group span reading its images: %d' % (extra['both_adjacent'], extra['both_frames'], extra['reads_inside']))


def cmd_census(a):
    path = resolve(a.capture)
    cap = sc.Cap(path)
    rng = frame_range(a.frames, len(cap.frames))
    S, extra = census(cap, rng)
    print_census(os.path.basename(os.path.dirname(path)) or path, S, extra, len(rng))
    if a.json:
        out = {k: dict(v, foreign=dict(v['foreign']), before=dict(v['before']), after=dict(v['after']), kinds=dict(v['kinds']), dests=sorted(v['dests']), targets=sorted(v['targets']),
                       readers=dict(v['readers'])) for k, v in S.items()}
        out['extra'] = extra
        json.dump(out, open(a.json, 'w'), indent=1)
    # exit 1 when a group is empty (the capture is not what the lane needs)
    return 0 if S['module']['frames'] and S['rim']['frames'] else 1


def cmd_cut(a):
    path = resolve(a.capture)
    cap = sc.Cap(path)
    zin = cap.c.zf
    if zin is None:
        print('FAIL  the input must be a case zip')
        return 1
    rng = frame_range(a.only_frames, len(cap.frames))
    which = ('module', 'rim') if a.which == 'both' else (a.which,)
    new_frames = {}
    cut_passes = cut_ops = frames_cut = 0
    for fi in rng:
        g, passes, copies, f = frame_groups(cap, fi)
        n = len(f.ops)
        drop = np.zeros(n, bool)
        for name in which:
            for p in g[name]:
                drop[p.pos:p.end] = True
                cut_passes += 1
        if not drop.any():
            continue
        buf = bytearray(f.buf)
        es, cnt, off = f.secs[pcap.SEC['ops']]
        keep = np.frombuffer(bytes(f.buf), np.dtype([('kind', '<u4'), ('index', '<u4')]), cnt, off)[~drop]
        buf[off:off + 8 * len(keep)] = keep.tobytes()
        nsec = struct.unpack_from('<I', buf, 12)[0]
        for k in range(nsec):
            sid = struct.unpack_from('<I', buf, 32 + 24 * k)[0]
            if sid == pcap.SEC['ops']:
                struct.pack_into('<Q', buf, 32 + 24 * k + 8, len(keep))
        new_frames[cap.frames[fi]] = bytes(buf)
        cut_ops += int(drop.sum())
        frames_cut += 1
    with zipfile.ZipFile(a.out, 'w', zipfile.ZIP_STORED, allowZip64=True) as zout:
        for info in zin.infolist():
            zout.writestr(info.filename, new_frames.get(info.filename) or zin.read(info.filename))
    print('cut %d passes (%d ops) of %s in %d of %d frames -> %s' % (cut_passes, cut_ops, '+'.join(which), frames_cut, len(cap.frames), a.out))
    return 0



# the images the readers sample: the module's six copy destinations and the rim map's four (214-219, 222-225; 220 / 221 are the PLANE chain's, category 0, not these nodes')
FINAL_IMAGES = set(range(214, 220)) | set(range(222, 226))


def cmd_oracle(a):
    """What is left of the two nodes in a capture, counted WITHOUT the pass classification the planner and the cut share: a draw whose ORIGIN is the node's (the map / rim pass sites, the circle call,
    the coverage-reduction queue site, a min blur or the rim's t2 blur), a SetTargets whose target is one of the images 1180-1193 the nodes render into, a Copy whose destination is one of 214-225 (the final
    images the readers sample). On a cut capture every one must be 0; on the uncut capture they must be many (the oracle is not vacuous)."""
    path = resolve(a.capture)
    cap = sc.Cap(path)
    draws_left = targets_left = copies_left = 0
    for fi in range(len(cap.frames)):
        f = cap.frame(fi)
        d, o = f.draws, f.origins
        if len(d):
            ps = o['pass_site'].astype(np.int64)
            cs = pcap.queue_site(o)
            effs = cap.effs(d['ps_hash'])
            hit = (ps == sc.SITE_MAP) | (ps == sc.SITE_RIM) | (cs == sc.CALL_CIRCLE) | (cs == sc.CALL_REDUCE)
            hit |= (cs == sc.CALL_MINBLUR) & np.isin(effs, ['yBlur/min_pair', 'yBlur/min_rgba', 'yBlur/min_r', 'yBlur/min_rg', 'yBlur/t2'])
            # draws are listed per frame, but only those still referenced by an op are in the walk: count the ones the ops reach
            didx, _ = f.draw_ops()
            reach = np.zeros(len(d), bool)
            reach[didx] = True
            draws_left += int((hit & reach).sum())
        copies = sc.table(f, sc.SEC_COPIES, sc.COPY_DT)
        for kind, idx in f.ops:
            kind, idx = int(kind), int(idx)
            if kind == 0:
                t = f.targets[idx]
                img = int(t['color'][0]['image']) if t['count'] else int(t['depth']['image'])
                targets_left += 1180 <= img <= 1193
            elif kind == 3:
                copies_left += int(copies[idx]['d']['image']) in FINAL_IMAGES
    total = draws_left + targets_left + copies_left
    print('ORACLE remaining %d (draws with a node origin %d, SetTargets into 1180-1193 %d, copies into the final images %d) in %s' % (total, draws_left, targets_left, copies_left, os.path.basename(os.path.dirname(path)) or path))
    return 1 if total else 0

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('census')
    c.add_argument('capture')
    c.add_argument('--json', default='')
    c.add_argument('--frames', default='')
    r = sub.add_parser('oracle')
    r.add_argument('capture')
    k = sub.add_parser('cut')
    k.add_argument('capture')
    k.add_argument('out')
    k.add_argument('--which', choices=['module', 'rim', 'both'], required=True)
    k.add_argument('--only-frames', default='')
    a = ap.parse_args()
    return cmd_census(a) if a.cmd == 'census' else cmd_oracle(a) if a.cmd == 'oracle' else cmd_cut(a)


if __name__ == '__main__':
    sys.exit(main())
