r"""S101 lane 189 (docs\renderer\SKIP_GAME_WORK_MAP.md section 16, "Ring D3"): the ring's temporal ledger. Offline, 0 game runs, read-only.

Question: of the values a ring draw reads (every constant register its two programs read, plus the image bound to each texture slot), how many are the SAME
as the key's value one frame earlier, and the same as the value at the key's first appearance ("match start")? That is the share of the ring's per-frame
game-thread recording that a "held value" or a "record once" route could skip without a new per-frame contributor, and the share that needs one
(the palette rows of the ropes, covers, turnbuckles, skirts).

  py -I tools\ring\ring_ledger.py CAPTURE --arena N [--frames A:B:STEP] [--json OUT] [-q] [--expect NAME] [--plant NAME]
CAPTURE = a capture folder or a case folder (capture\ inside), the same inputs as tools\skip\ledger.py. N = the bg arena id whose pac holds the ring (4 for Check B /
run 7079 / J1, 29 for wm30). Ring draws are found by CONTENT, as the ring core joins them: main-pass model draws (pass site 0x82C0A950) of an object whose every drawn
part's (vertex count, index count) equals mesh `part` of one ring entry of that arena's pac (tools\ring\ring_files.py reads the player's own file; counts only leave it).
Key = {object, part, ordinal among the object's calls of this frame}: an INSTANCE of a ring piece (the D3 key of SKIP_GAME_WORK_MAP.md 10.3).
Groups of registers (VS/PS, by the fx registry's ranges of the draw's variant):
  palette   VS c79..c138 (rows of the instance's bones beyond the first: rope simulation, skirt skeletons)    world  VS c76-c78 (bone 0: the instance's own transform)
  light     c0-c12, c181, c182, c197, c199      shadow  c68-c71, c164-c166, c168-c170      pass  the value >= 90 % of the ring draws share in this frame (camera, fog)
  clock c186   matcol c185 / c189      other  the rest (material constants, UV scroll, fades)
Per piece and group: reads that have an earlier frame to compare with, the share equal to the previous frame of the same key (consecutive frames only), the share equal to the key's
first-appearance value. Per piece: draws whose EVERY non-pass register equals last frame ("fully held"), the same for the palette+world alone, how many of an instance's
bones change per frame (histogram) and by how much (max |delta| of a changed row's translation). Texture slots: the image id per slot against the previous frame / first appearance.
--expect checkB|j1|wm30: assertions the contract of section 16 quotes. --plant (tests only): shift_prev (compare against the frame TWO back), no_instance (key without the ordinal:
instances merge and the instance count check fails), no_join (every object taken as a ring object: the draws-a-frame check fails),
no_npal_cut (the bone slots past a mesh's palette counted: the stale rows the bank keeps from the previous draw make the never-moving posts look moving: the past bug of the wrestlers' palette, WRESTLER_PROMOTION_MAP.md Caution).
Numbers only. Nothing from the game is written; --json is for under out\."""
import argparse
import collections
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import ledger as L  # noqa: E402  (Capture, Registry, parse_frames, LedgerError)
import ring_files as rf  # noqa: E402

SITE_A = L.SITE_A
SEC_DRAWS, SEC_TEXSLOTS = 4, 10
OWN = ('palette', 'world', 'matcol', 'clock', 'other')
GROUPS = ('palette', 'world', 'light', 'shadow', 'pass', 'clock', 'matcol', 'other', 'stale')
PLANTS = ('shift_prev', 'no_instance', 'no_join', 'no_npal_cut')
PIECE = {951: 'post', 981: 'post_low', 952: 'turnbuckle', 953: 'canvas', 956: 'proxy', 960: 'rope_high', 980: 'rope_low', 964: 'cover', 982: 'cover_low', 973: 'skirt', 974: 'skirt'}
INSTANCES = {'post': 4, 'turnbuckle': 12, 'canvas': 1, 'cover': 12}   # per frame, RING_PROMOTION_MAP.md 1.1 / ring.md B2.1 (ropes: 12 over both LODs; skirts: 3 drawn)


def group_of(stage, reg, npal=21):
    if stage == 'v' and 76 <= reg <= 138:
        if reg >= 76 + 3 * npal:
            return 'stale'   # a bone slot beyond the mesh's own palette: the bank holds whatever the previous draw left there
        return 'world' if reg <= 78 else 'palette'
    if reg <= 12 or reg in (181, 182, 197, 199):
        return 'light'
    if 68 <= reg <= 71 or 164 <= reg <= 166 or 168 <= reg <= 170:
        return 'shadow'
    if reg == 186:
        return 'clock'
    if reg in (185, 189):
        return 'matcol'
    return 'other'


def draw_counts(f, i):
    """(vertex_count, index_count) of draw i (adapter_frame.h Draw: ... vertex_bytes 72, index_bytes 76, vertex_count 80, index_count 84)."""
    es, _, so = f.S[SEC_DRAWS]
    return struct.unpack_from('<II', f.b, so + es * i + 80)


def tex_images(f, d):
    """{slot: image id} of the draw's texture slots (TextureSlot: image u32, sampler u32, slot u8 at +8; the section's element size is read, not assumed)."""
    if SEC_TEXSLOTS not in f.S:
        return {}
    es, cnt, so = f.S[SEC_TEXSLOTS]
    out = {}
    for k in range(d['tex_first'], min(d['tex_first'] + d['tex_count'], cnt)):
        image, _sampler, slot = struct.unpack_from('<IIB', f.b, so + es * k)
        out[slot] = image
    return out


class Joiner:
    """object -> ring entries, by the (vertex count, index count) of every part the object draws (the ring core's intersection, counts only)."""

    def __init__(self, arena):
        ring = rf.load_ring(arena, textures=False)
        self.by_entry = {e: [(m.n, m.stream_index_count) for m in sorted(ms, key=lambda m: m.index)] for e, ms in ring.entries.items()}
        self.npal = {e: [max(len(m.palette), 1) for m in sorted(ms, key=lambda m: m.index)] for e, ms in ring.entries.items()}   # bone slots (3 rows each) of a mesh
        self.cache = {}

    def join(self, parts):
        """parts = {part: (vc, ic)} -> sorted tuple of the entries every part fits, () when none."""
        key = tuple(sorted(parts.items()))
        r = self.cache.get(key)
        if r is None:
            r = tuple(sorted(e for e, ms in self.by_entry.items() if all(p < len(ms) and ms[p] == c for p, c in parts.items())))
            self.cache[key] = r
        return r


def npal_of(joiner, entries, part):
    return max((joiner.npal[e][part] for e in entries if e in joiner.npal and part < len(joiner.npal[e])), default=21)


def piece_name(entries):
    names = sorted({PIECE.get(e, str(e)) for e in entries})
    return '/'.join(names) if names else '?'


def run(path, arena, frames_spec='', plants=(), quiet=True, light_state=False):
    t0 = time.time()
    cap = L.Capture(path)
    reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    frames = L.parse_frames(frames_spec, cap.n)
    if not frames:
        raise L.LedgerError('--frames selects no frame')
    joiner = Joiner(arena)
    # pre-pass: the join of every object over the WHOLE capture (a thin-recorded capture such as J1 carries geometry counts only in an object's proof frames; its lite draws have none)
    joined_obj = {}
    for fi in frames:
        f = cap.frame(fi)
        by = collections.defaultdict(dict)
        for i in range(f.ndraws):
            o = f.origin(i)
            if o and o['klass'] == 1 and o['pass_site'] == SITE_A:
                vc, ic = draw_counts(f, i)
                if vc:
                    by[o['object']].setdefault(o['part'], (vc, ic))
        for obj, parts in by.items():
            ents = ('plant',) if 'no_join' in plants else joiner.join(parts)
            if ents:
                joined_obj[obj] = ents
            elif obj in joined_obj and len(parts) >= 1 and not joiner.join(parts):
                pass
    lfi, light_off = None, 0
    if light_state and cap.has_scene:
        lfi = L.LightFrameIndex(cap.record(i) for i in range(cap.n))
        if lfi.records == 0:
            raise L.LedgerError('--light-state: no record of this capture carries light tables')
        light_off, _votes = L.detect_light_offset(cap, frames, reg, lfi)
    lstat = collections.defaultdict(collections.Counter)    # object -> light-state matching counters
    calls_prev = {}                                         # object -> calls (instances) of the previous frame
    churn = collections.defaultdict(collections.Counter)    # object -> frames, frames whose call count differs from the previous frame
    first, prev, prev2, tfirst, tprev = {}, {}, {}, {}, {}
    gstat = collections.defaultdict(collections.Counter)     # (object, group) -> counters
    pstat = collections.defaultdict(collections.Counter)     # object -> counters
    tstat = collections.defaultdict(collections.Counter)
    bones_hist = collections.defaultdict(collections.Counter)  # object -> changed-bones-per-instance-frame buckets
    delta_vals = collections.defaultdict(list)                 # object -> max |delta| of a changed row, one per changed row group (sampled)
    moving = collections.Counter()                              # (stage, reg) -> reads of the ring's own non-palette registers that differ from last frame
    objinfo = {}                                               # object -> dict(entries, variants, parts)
    per_frame = []
    inst_max = collections.defaultdict(int)
    last_fi = None
    frames_with = 0
    for fi in frames:
        f = cap.frame(fi)
        contiguous = last_fi is not None and fi == last_fi + 1
        last_fi = fi
        by_obj = collections.defaultdict(list)
        all_a = []
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] != SITE_A:
                continue
            by_obj[o['object']].append((i, o))
            all_a.append(i)
        draws = []
        for obj, lst in by_obj.items():
            ents = joined_obj.get(obj)
            if not ents:
                continue
            info = objinfo.setdefault(obj, dict(entries=ents, parts=set(), variants=set()))
            info['entries'] = ents
            for i, o in lst:
                draws.append((i, o))
        if not draws:
            continue
        frames_with += 1
        per_frame.append(len(draws))
        # the pass bank, the ledger's rule, over EVERY main-pass model draw of the frame (arena, wrestlers, ring): the value >= 90 % of the draws reading a register share
        votes = collections.defaultdict(collections.Counter)
        reads = collections.Counter()
        meta = {}
        for i in all_a:
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            pn = reg.variant_of(d['ps']) if d['ps'] else None
            vb = f.bank(d['vs_bank'])
            pb = f.bank(d['ps_bank']) if d['ps'] else None
            meta[i] = (d, vn, pn, vb, pb)
            for stage, name, bank in (('v', vn, vb), ('p', pn, pb)):
                if not name:
                    continue
                for r in reg.read_regs(name):
                    votes[(stage, r)][tuple(bank[4 * r:4 * r + 4])] += 1
                    reads[(stage, r)] += 1
        passbank = {k: c.most_common(1)[0][0] for k, c in votes.items() if c.most_common(1)[0][1] >= 2 and c.most_common(1)[0][1] >= 0.9 * reads[k]}
        seen = collections.Counter()
        for i, o in draws:
            d, vn, pn, vb, pb = meta[i]
            obj = o['object']
            k0 = (obj, o['part'])
            ordinal = 0 if 'no_instance' in plants else seen[k0]
            seen[k0] += 1
            key = (obj, o['part'], ordinal)
            npal = 21 if 'no_npal_cut' in plants else npal_of(joiner, objinfo[obj]['entries'], o['part'])
            info = objinfo[obj]
            info['parts'].add(o['part'])
            info['variants'].add(vn)
            new_key, held_all, held_env, hold_pal, pal_n = False, True, True, True, 0
            changed_bones = set()
            bones_read = set()
            for stage, name, bank in (('v', vn, vb), ('p', pn, pb)):
                if not name:
                    continue
                for r in sorted(reg.read_regs(name)):
                    v = tuple(bank[4 * r:4 * r + 4])
                    g = group_of(stage, r, npal)
                    shared = passbank.get((stage, r)) == v and g not in ('palette', 'world', 'stale')
                    if shared:
                        g = 'pass'   # a value the whole pass shares (camera, fog, fades, and the lights where the pass shares them): sourced by the pass bank at the marker
                    if g == 'stale':
                        continue
                    kk = (key, stage, r)
                    gs = gstat[(obj, g)]
                    gs['reads'] += 1
                    if lfi is not None and group_of(stage, r, npal) in ('light', 'shadow'):
                        ls = lstat[obj]
                        ls['reads'] += 1
                        if lfi.match(f.guest_frame - light_off, obj, stage, r, v):
                            ls['state_exact'] += 1
                        elif set(v) <= L.TRIVIAL:
                            ls['trivial'] += 1
                    pal = stage == 'v' and 76 <= r <= 138
                    if pal:
                        pal_n += 1
                        bones_read.add((r - 76) // 3)
                    if kk not in first:
                        first[kk] = v
                        gs['first_seen'] += 1
                        new_key = True
                    else:
                        p = prev.get(kk)
                        ref = prev2.get(kk) if 'shift_prev' in plants else p
                        want = fi - (2 if 'shift_prev' in plants else 1)
                        held = bool(ref) and contiguous and ref[0] == want and ref[1] == v
                        if held:
                            gs['held_prev'] += 1
                        else:
                            if g in OWN:
                                held_all = False
                                if not pal:
                                    moving[(obj, stage, r)] += 1
                            elif g in ('light', 'shadow'):
                                held_env = False
                            if pal:
                                hold_pal = False
                                changed_bones.add((r - 76) // 3)
                                if ref and ref[0] == want:
                                    a = struct.unpack('<4f', struct.pack('<4I', *ref[1]))
                                    b = struct.unpack('<4f', struct.pack('<4I', *v))
                                    delta_vals[obj].append(max(abs(x - y) for x, y in zip(a, b)))
                        if first[kk] == v:
                            gs['held_first'] += 1
                    if kk in prev:
                        prev2[kk] = prev[kk]
                    prev[kk] = (fi, v)
            ps = pstat[obj]
            ps['draws'] += 1
            if new_key:
                ps['new_key'] += 1
            else:
                if held_all:
                    ps['fully_held'] += 1   # every register of the ring's OWN groups (palette, world, matcol, clock, other) equals last frame
                if held_env:
                    ps['env_held'] += 1     # every light / shadow / pass register equals last frame
                if held_all and held_env:
                    ps['all_held'] += 1
            if pal_n and not new_key:
                ps['pal_draws'] += 1
                if hold_pal:
                    ps['pal_held'] += 1
                n = len(changed_bones)
                bones_hist[obj]['0' if n == 0 else '1-3' if n <= 3 else '4-10' if n <= 10 else '11+'] += 1
                bones_hist[obj]['_bones_changed'] += n
                bones_hist[obj]['_bones_read'] += len(bones_read)
            for slot, image in sorted(tex_images(f, d).items()):
                kk = (key, slot)
                ts = tstat[obj]
                ts['reads'] += 1
                if kk not in tfirst:
                    tfirst[kk] = image
                    ts['first'] += 1
                else:
                    if tprev.get(kk) == image and contiguous:
                        ts['held_prev'] += 1
                    if tfirst[kk] == image:
                        ts['held_first'] += 1
                tprev[kk] = image
        for obj in {o['object'] for _, o in draws} | set(calls_prev):
            n_now = seen[(obj, 0)] if (obj, 0) in seen else 0
            if obj in calls_prev and contiguous:
                churn[obj]['frames'] += 1
                if calls_prev[obj] != n_now:
                    churn[obj]['changed'] += 1
            calls_prev[obj] = n_now
        for obj in {o['object'] for _, o in draws}:
            inst_max[obj] = max(inst_max[obj], max((seen[(obj, p)] for p in range(0, 8)), default=0))
    res = dict(path=str(path), arena=arena, frames=len(frames), frames_with_ring=frames_with,
               draws_per_frame=(sum(per_frame) / len(per_frame)) if per_frame else 0.0,
               draws_min=min(per_frame) if per_frame else 0, draws_max=max(per_frame) if per_frame else 0, seconds=round(time.time() - t0, 1))
    res['objects'] = {}
    for obj, info in sorted(objinfo.items()):
        pn = piece_name(info['entries'])
        res['objects']['%08X' % obj] = dict(piece=pn, entries=list(info['entries']), parts=len(info['parts']), instances_max=inst_max.get(obj, 0), variants=sorted(v or '?' for v in info['variants']))
    # aggregate per piece (objects of one piece, e.g. the four skirts, are summed)
    piece_of = {o: v['piece'] for o, v in res['objects'].items()}
    gp = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    pp = collections.defaultdict(collections.Counter)
    tp = collections.defaultdict(collections.Counter)
    bh = collections.defaultdict(collections.Counter)
    dv = collections.defaultdict(list)
    for (obj, g), c in gstat.items():
        gp[piece_of['%08X' % obj]][g].update(c)
    for obj, c in pstat.items():
        pp[piece_of['%08X' % obj]].update(c)
    for obj, c in tstat.items():
        tp[piece_of['%08X' % obj]].update(c)
    for obj, c in bones_hist.items():
        bh[piece_of['%08X' % obj]].update(c)
    for obj, v in delta_vals.items():
        dv[piece_of['%08X' % obj]].extend(v)
    mv = collections.Counter()
    for (obj, st, r), n in moving.items():
        mv[('%s %s c%d' % (piece_of['%08X' % obj], 'VS' if st == 'v' else 'PS', r))] += n
    cp = collections.defaultdict(collections.Counter)
    for obj, c in churn.items():
        cp[piece_of['%08X' % obj]].update(c)
    res['call_count_changes'] = {p: dict(c) for p, c in sorted(cp.items())}
    res['moving_registers'] = dict(mv.most_common(20))
    lp = collections.defaultdict(collections.Counter)
    for obj, c in lstat.items():
        lp[piece_of['%08X' % obj]].update(c)
    res['light_state'] = {p: dict(c) for p, c in sorted(lp.items())}
    res['light_offset'] = light_off if lfi is not None else None
    res['groups'] = {p: {g: dict(c) for g, c in gs.items()} for p, gs in sorted(gp.items())}
    tot = collections.defaultdict(collections.Counter)
    for p, gs in gp.items():
        for g, c in gs.items():
            tot[g].update(c)
    res['groups_total'] = {g: dict(c) for g, c in tot.items()}
    res['pieces'] = {p: dict(c) for p, c in sorted(pp.items())}
    res['textures'] = {p: dict(c) for p, c in sorted(tp.items())}
    res['bones_changed'] = {p: dict(c) for p, c in sorted(bh.items())}
    res['delta'] = {}
    for p, v in dv.items():
        v.sort()
        res['delta'][p] = dict(n=len(v), p50=v[len(v) // 2], p90=v[int(len(v) * 0.9)], max=v[-1])
    return res


def pct(a, b):
    return '%5.1f%%' % (100.0 * a / b) if b else '    - '


def report(res):
    print('capture %s  arena %s' % (res['path'], res['arena']))
    print('frames %d (with ring draws %d), ring draws a frame %.1f (min %d max %d), %.0f s' % (res['frames'], res['frames_with_ring'], res['draws_per_frame'], res['draws_min'], res['draws_max'], res['seconds']))
    for k, v in res['objects'].items():
        print('  object %s  %-12s entries %s  parts %d  instances (max ordinal+1) %d  %s' % (k, v['piece'], v['entries'], v['parts'], v['instances_max'], ','.join(x.replace('_vertex', '') for x in v['variants'])))
    print('%-14s %-8s %9s %7s %7s' % ('piece', 'group', 'reads', '=prev', '=first'))
    for piece, gs in res['groups'].items():
        for g in GROUPS:
            c = gs.get(g)
            if not c:
                continue
            rd = c.get('reads', 0) - c.get('first_seen', 0)
            print('%-14s %-8s %9d %7s %7s' % (piece[:14], g, c.get('reads', 0), pct(c.get('held_prev', 0), rd), pct(c.get('held_first', 0), rd)))
    print('-- frames whose per-object call count (instances drawn) differs from the previous frame (LOD flips move instances between the two rope objects): ' + ', '.join('%s %d of %d' % (p, c.get('changed', 0), c.get('frames', 0)) for p, c in res['call_count_changes'].items()))
    print('-- the ring own non-palette registers that moved (reads that differ from last frame): ' + ', '.join('%s x%d' % kv for kv in res['moving_registers'].items()))
    if res.get('light_state'):
        print('-- light / shadow registers against the light-state record (hook-to-stream offset %+d): reads, bit-exact from the states, trivial (0 / 1 / -1), other' % res['light_offset'])
        for p, c in res['light_state'].items():
            print('   %-14s reads %7d  state-exact %s  trivial %s  other %s' % (p, c.get('reads', 0), pct(c.get('state_exact', 0), c.get('reads', 0)), pct(c.get('trivial', 0), c.get('reads', 0)),
                  pct(c.get('reads', 0) - c.get('state_exact', 0) - c.get('trivial', 0), c.get('reads', 0))))
    print('-- totals over the ring (reads that have an earlier frame to compare with)')
    for g in GROUPS:
        c = res['groups_total'].get(g)
        if c:
            rd = c.get('reads', 0) - c.get('first_seen', 0)
            print('%-8s reads %9d  =prev %s  =first %s' % (g, c['reads'], pct(c.get('held_prev', 0), rd), pct(c.get('held_first', 0), rd)))
    print('-- per piece: draws | own held (palette, world, matcol, clock, other = last frame) | env held (light, shadow) | all held | palette+world rows held | bones changed per frame | texture slots =prev')
    for p, c in res['pieces'].items():
        t = res['textures'].get(p, {})
        b = res['bones_changed'].get(p, {})
        dl = res['delta'].get(p)
        comparable = c['draws'] - c.get('new_key', 0)
        trd = t.get('reads', 0) - t.get('first', 0)
        print('%-14s draws %6d  new keys %3d  own held %s  env held %s  all held %s  palette held %s  bones changed %s of %s read (inst-frames: 0:%d 1-3:%d 4-10:%d 11+:%d)  delta p50/p90/max %s  tex =prev %s' % (
            p[:14], c['draws'], c.get('new_key', 0), pct(c.get('fully_held', 0), comparable), pct(c.get('env_held', 0), comparable), pct(c.get('all_held', 0), comparable), pct(c.get('pal_held', 0), c.get('pal_draws', 0)), b.get('_bones_changed', 0), b.get('_bones_read', 0),
            b.get('0', 0), b.get('1-3', 0), b.get('4-10', 0), b.get('11+', 0), ('%.4f/%.4f/%.3f' % (dl['p50'], dl['p90'], dl['max'])) if dl else '-', pct(t.get('held_prev', 0), trd)))


def expect(res, name):
    bad = []
    d = res['draws_per_frame']
    if not (95 <= d <= 125):
        bad.append('ring draws a frame %.1f outside 95..125 (the map: 108 colour draws on Check B)' % d)
    pieces = {v['piece']: v['instances_max'] for v in res['objects'].values()}
    for name_, want in INSTANCES.items():
        got = max((n for p, n in pieces.items() if p.startswith(name_) and 'low' not in p), default=0)
        if got and got != want:
            bad.append('instances of %s: %d, expected %d' % (name_, got, want))
    post = res['pieces'].get('post')
    if post and post.get('pal_draws') and post.get('pal_held', 0) < post['pal_draws']:
        bad.append('the post palette rows moved on a capture where they never do (held %d of %d draws): stale bone slots counted?' % (post.get('pal_held', 0), post['pal_draws']))
    pw = res['groups_total'].get('palette', {})
    rd = pw.get('reads', 0) - pw.get('first_seen', 0)
    held = pw.get('held_prev', 0)
    if name in ('checkB', 'j1') and rd and held >= rd:
        bad.append('palette rows never move on a capture where the ropes move (held %d of %d)' % (held, rd))
    if name == 'wm30' and rd and held < rd * 0.9:
        bad.append('wm30 is the ring at rest: palette held %d of %d, expected ~all' % (held, rd))
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description='ring temporal ledger (S101 lane 189)')
    ap.add_argument('capture')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--frames', default='')
    ap.add_argument('--json')
    ap.add_argument('--expect', choices=('checkB', 'j1', 'wm30'))
    ap.add_argument('--plant', choices=PLANTS, action='append', default=[])
    ap.add_argument('--light-state', action='store_true', help='also match the light / shadow registers against the kScLightStates of the capture (J1 only)')
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args(argv)
    try:
        res = run(a.capture, a.arena, a.frames, tuple(a.plant), a.q, a.light_state)
    except (L.LedgerError, OSError, ValueError, KeyError) as e:
        print('FAIL  %s: %s' % (type(e).__name__, e))
        return 1
    if not a.q:
        report(res)
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as fh:
            json.dump(res, fh, indent=1)
    bad = expect(res, a.expect) if a.expect else []
    for b in bad:
        print('FAIL  ' + b)
    if a.expect and not bad:
        print('PASS  expectations (%s)' % a.expect)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
