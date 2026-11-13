r"""S101 lane 92 (KL, docs\renderer\SKIP_GAME_WORK_MAP.md "Ledger measured (lane KL)"): the values ledger census. Offline, 0 game runs.

For rung D3 (the game's work for an object is skipped, so no stream draw carries its constants) every value our renderer would read must come from a
NAMED source. Over a capture (frames + scene records) this tool measures, per promoted class (arena, wrestlers, ring, flash) and per draw, where each
constant register the draw's programs read would come from:
  record    the value equals a field of the scene record for this frame (world, palette row, light, camera, eye, param, tint), bit for bit
  pass      shared by the pass: the value every draw of the same pass site in the frame reads (camera, fog, fades; "pass bank")
  mirror    pass 0x82C0B94C (the mirrored arena copy): the pass bank of 0x82C0A950 with x negated (draw_submit.md 150-151)
  template  constant over every frame the key {pass site, object, part, ordinal} is drawn in (>= 2 frames): file data or our own constant
  role      a function of the draw's own target / viewport size (1/w, 1/h ...): comes from the surface role
  light_state  (S101 lane 109, PER_DRAW_LIGHTS.md) the value is a register row built from a light state the scene record carries for this draw's engine
            object (kScLightStates / kScLightRefs: the render camera record R read at the model call; rows built as src\renderer_scene\light_state_rows.h).
            DORMANT until a record carries the tables (the sources list never shows it); --light-state asks for it and FAILS when no record has them.
  derived   (S101 lane 109) the value is a rule of other registers of the SAME draw, proven bit-exact by lane 101 on Check B and run 7079: PS c197
            DifLghMax = the float32 sum of the PS's DifLghCol rows c0-c3 (added in order 0+1, +2, +3); PS c199 = (m, 1/m, 0, 0), m = the largest component
            of that sum; VS c12 / c4 KeyLight = the PS's c12 / c4 (light 4). Tried after every other source; the draw's other registers still need their own.
  unsourced none of the above
The registers a draw reads come from the fx registry (src\renderer\shaders\spv\fx_registry.h: the constant ranges of the variant, found by the
capture's shader hash). A draw whose hash is not in the registry has "no variant" and is never counted as sourced.

  py -I tools\skip\ledger.py CAPTURE_DIR [--frames A:B:STEP] [--record-offset N|auto] [--plant NAME] [--json OUT] [--expect NAME] [-q]
CAPTURE_DIR = a capture folder (frames\ and scene\ inside) or a case folder holding capture\. --record-offset N: frame i reads record i-N (default
auto: the offset with the most exact palette-row matches over a sample; the mapping is +1, WRESTLER_PROMOTION_MAP.md 47).
--expect: pass/fail assertions on the numbers: checkB (run 7051: wrestler palette rows NOT all sourced, the record held the update-hook palette for
nodes 59-108), r7079 (run 7079: every wrestler palette row sourced, lane 43), mirror (the 0x82C0B94C camera registers are sourced by the mirror
rule). Exit 1 when an assertion fails or an input cannot be read.
--plant (tests only): offset0 (records read at offset 0: record rows must become unsourced), no_mirror_rule (mirrored cameras unsourced),
no_record (the record side disabled), no_derived (the derived column off), derived_drop_w (the c197 / c199 rules computed with colour w zeroed: today's record
shape; --expect derived must FAIL).
--expect derived: every wrestler draw that the rules apply to matches (c197, c199, KeyLight vec and colour), and the rules applied at all.
Class rule (a measurement class, NOT the hooks' claim; the hooks join by file content): main-pass model draws (pass 0x82C0A950 / 0x82C0B94C) whose VS
variant is yCh_* = wrestlers; yBG_* = arena; yProp_* = arena when its world rows (c76-c78) equal an arena object's world in the record, else ring;
pass 0x82C139FC = flash. Numbers only. Nothing from the game is written; --json is for under out\."""
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
from ledger_io import (Frame, Record, Registry, CLASS_NAMES, SC_OBJECTS, SC_BONES, SC_LIGHTS, SC_PARAMS)  # noqa: E402
import light_rows  # noqa: E402  (S101 lane 109)

SITE_A, SITE_B, SITE_FLASH = 0x82C0A950, 0x82C0B94C, 0x82C139FC
CLASSES = ('arena', 'wrestlers', 'ring', 'flash')
PLANTS = ('offset0', 'no_mirror_rule', 'no_record', 'no_derived', 'derived_drop_w')
DERIVED_RULES = ('c197', 'c199', 'key_vec', 'key_color')
ONE, NEG1 = 0x3F800000, 0xBF800000
TRIVIAL = {0, ONE, NEG1, 0x80000000}
SOURCES = ('record', 'pass', 'mirror', 'template', 'role', 'light_state', 'derived', 'world', 'unsourced')  # lane 130: 'world' = the kScArenaWorlds field (arena_ready --worlds)
PALETTE_REG0, PALETTE_REGS = 76, 63  # c76..c138: 21 bone slots of 3 rows (fx ranges {76, 63})


class LedgerError(Exception):
    pass


def main_checkout():
    """The main checkout (where out\\ and game_data\\ live), also from a worktree."""
    try:
        import subprocess
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


def find_capture(path):
    """A capture folder, a case folder (capture\\ inside) or a case zip (unpacked once to <main>/out/s101/lane92/<zip name>: game data, local only)."""
    if path.lower().endswith('.zip'):
        if not os.path.isfile(path):
            alt = os.path.join(main_checkout(), path)
            if os.path.isfile(alt):
                path = alt
            else:
                raise LedgerError('case zip not found: ' + path)
        dest = os.path.join(main_checkout(), 'out', 's101', 'lane92', os.path.splitext(os.path.basename(path))[0])
        if not os.path.isdir(os.path.join(dest, 'capture', 'frames')):
            import zipfile
            with zipfile.ZipFile(path) as z:
                z.extractall(dest)
        path = dest
    for cand in (path, os.path.join(path, 'capture')):
        if os.path.isdir(os.path.join(cand, 'frames')):
            return cand
    raise LedgerError('no frames folder under ' + path)


def f2b(f):
    return struct.unpack('<I', struct.pack('<f', f))[0]


def mat_vectors(m):
    """The 4 columns and 4 rows of a 16-float matrix as bit tuples (a palette register is a column of the 4x4: (m00, m10, m20, tx))."""
    bits = struct.unpack('<16I', struct.pack('<16f', *m))
    for k in range(4):
        yield bits[k::4], 'col'
        yield bits[4 * k:4 * k + 4], 'row'


def dir_signature(a, b, c):
    dot = lambda x, y: x[0] * y[0] + x[1] * y[1] + x[2] * y[2]
    return tuple(sorted(round(dot(x, y), 2) for x, y in ((a, b), (a, c), (b, c))))


def f32vec(v):
    return struct.unpack('<4f', struct.pack('<4I', *v))


class RecordIndex:
    """Bit-exact index of one scene record: vec4 -> kind, vec3 prefix -> kind (lights, eye), plus the arena worlds."""

    def __init__(self, rec):
        self.v4 = {}
        self.v3 = {}
        self.arena_world = set()
        self.rot_sigs = set()  # rotation-invariant signature (sorted pairwise dots) of every 3 directional lights of a set
        objs = rec.objects()
        for o in objs:
            if o['klass'] == 1 and o['id'].startswith('arena'):
                for v, _ in mat_vectors(o['world']):
                    self.arena_world.add(v)
            for v, _ in mat_vectors(o['world']):
                self.v4.setdefault(v, 'world')
            t = struct.unpack('<4I', struct.pack('<4f', *o['tint']))
            self.v4.setdefault(t, 'tint')
        for m in rec.bones():
            for v, _ in mat_vectors(m):
                self.v4.setdefault(v, 'palette')
        for name, val in rec.params():
            self.v4.setdefault(struct.unpack('<4I', struct.pack('<4f', *val)), 'param')
        for l in rec.lights():
            for k, vec in (('light_dir', l['dir']), ('light_color', l['color'])):
                self.v3.setdefault(struct.unpack('<3I', struct.pack('<3f', *vec)), k)
        sets = collections.defaultdict(list)
        for l in rec.lights():
            if l['kind'] == 0 and any(l['dir']):
                sets[l['set']].append(l['dir'])
        for dirs in sets.values():
            for a in range(len(dirs)):
                for b in range(a + 1, len(dirs)):
                    for c in range(b + 1, len(dirs)):
                        self.rot_sigs.add(dir_signature(dirs[a], dirs[b], dirs[c]))
        cam = rec.camera()
        if cam:
            for v, _ in mat_vectors(cam['view']):
                self.v4.setdefault(v, 'camera')
            for v, _ in mat_vectors(cam['proj']):
                self.v4.setdefault(v, 'camera')
            self.v3.setdefault(struct.unpack('<3I', struct.pack('<3f', *cam['eye'])), 'eye')

    def lookup(self, v):
        k = self.v4.get(v)
        return k or self.v3.get(v[:3])


class Capture:
    def __init__(self, path):
        self.dir = find_capture(path)
        fr = os.path.join(self.dir, 'frames')
        self.n = len([x for x in os.listdir(fr) if x.startswith('frame_') and x.endswith('.bin')])
        self.has_scene = os.path.isdir(os.path.join(self.dir, 'scene'))
        if not self.n:
            raise LedgerError('no frame files in ' + fr)

    def frame(self, i):
        return Frame(os.path.join(self.dir, 'frames', 'frame_%06d.bin' % i))

    def record(self, i):
        p = os.path.join(self.dir, 'scene', 'frame_%06d.rsc' % i)
        return Record(p) if os.path.isfile(p) else None


_FIELDS = None


def field_names():
    """{(stage, register): field names} from the fx descriptions (src/renderer/shaders/fx/*.fx.json: constants {field, register, count})."""
    global _FIELDS
    if _FIELDS is None:
        _FIELDS = collections.defaultdict(set)
        d = os.path.join(ROOT, 'src', 'renderer', 'shaders', 'fx')
        for fn in os.listdir(d) if os.path.isdir(d) else []:
            if not fn.endswith('.fx.json'):
                continue
            try:
                with open(os.path.join(d, fn), encoding='utf-8') as fh:
                    j = json.load(fh)
            except (OSError, ValueError):
                continue
            for stage, key in (('v', 'vertex'), ('p', 'fragment')):
                for var in j.get(key, []) or []:
                    for c in var.get('constants', []):
                        for r in range(c['register'], c['register'] + max(1, c['count'])):
                            _FIELDS[(stage, r)].add(c['field'] if c['count'] <= 1 else '%s[%d]' % (c['field'], r - c['register']))
    return _FIELDS


def reg_name(stage, r):
    names = sorted(field_names().get((stage, r), ()))
    return '/'.join(n.split('[')[0] for n in names[:3]) if names else '?'


def closure_curve(R, ks=(1, 2, 3, 5, 8, 12, 20)):
    """Share of the class's draws that would be 100 % sourced if the K most common unsourced registers were closed (greedy by draw count)."""
    n = R['draws']
    if not n:
        return []
    order = [k for k, _ in R['unsourced'].most_common()]
    out = []
    for k in ks:
        top = set(order[:k])
        ok = sum(c for s, c in R['unsrc_sets'].items() if set(s) <= top)
        out.append((k, 100.0 * ok / n))
    return out


def parse_frames(spec, n):
    if not spec:
        return list(range(n))
    a = spec.split(':')
    lo = int(a[0]) if a[0] else 0
    hi = int(a[1]) if len(a) > 1 and a[1] else n
    step = int(a[2]) if len(a) > 2 and a[2] else 1
    return list(range(lo, min(hi, n), step))


def detect_offset(cap, frames):
    """Record offset by voting: for 6 sample frames, how many palette-column vectors of the frame's draws equal a record palette vector at each offset."""
    sample = frames[len(frames) // 4::max(1, len(frames) // 6)][:6]
    votes = collections.Counter()
    for fi in sample:
        f = cap.frame(fi)
        vecs = set()
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] != SITE_A:
                continue
            b = f.bank(f.draw(i)['vs_bank'])
            for r in range(PALETTE_REG0, PALETTE_REG0 + PALETTE_REGS):
                v = tuple(b[4 * r:4 * r + 4])
                if not set(v) <= TRIVIAL:
                    vecs.add(v)
        for off in (-1, 0, 1, 2):
            ri = fi - off
            rec = cap.record(ri) if 0 <= ri < cap.n else None
            if not rec:
                continue
            idx = set()
            for m in rec.bones():
                for v, _ in mat_vectors(m):
                    idx.add(v)
            votes[off] += len(vecs & idx)
    if not votes:
        raise LedgerError('cannot detect the record offset: no scene records')
    return votes.most_common(1)[0][0], dict(votes)


def classify_draw(vn, o, vb, rindex):
    """The measurement class of a model-part draw (see the module doc), or None when it is none of the four."""
    site = o['pass_site']
    if site == SITE_FLASH:
        return 'flash'
    if site not in (SITE_A, SITE_B):
        return None
    if vn is None:
        return None
    if vn.startswith('yCh_'):
        return 'wrestlers'
    if vn.startswith('yBG_'):
        return 'arena'
    if vn.startswith('yProp_'):
        w = [tuple(vb[4 * r:4 * r + 4]) for r in (76, 77, 78)]
        if rindex is not None and all(x in rindex.arena_world for x in w):
            return 'arena'
        return 'ring'
    return None


def viewport_candidates(f, d):
    dy = f.dynamic(d['dyn'])
    vp = dy['viewport']
    out = set()
    for w in (vp[2], vp[3], dy['scissor'][2], dy['scissor'][3], abs(dy['guest_scale'][0]) * 2, abs(dy['guest_scale'][1]) * 2):
        if w > 0:
            for x in (w, 1.0 / w, 2.0 / w, 0.5 / w, 0.5 * w):
                out.add(x)
    return out


def is_role(v, cands):
    nz = [struct.unpack('<f', struct.pack('<I', c))[0] for c in v if c not in TRIVIAL]
    if not nz:
        return False
    for x in nz:
        if not any(abs(x - c) <= 1e-5 * max(1.0, abs(c)) for c in cands):
            return False
    return True


def collect(cap, frames, reg, rec_off, plants, sites=(SITE_A, SITE_B, SITE_FLASH), classify=None):
    """Pass 1: per key {pass, object, part, ordinal} and (stage, reg): the first value, whether it ever changed, how many frames it was seen in."""
    stats = {}
    for fi in frames:
        f = cap.frame(fi)
        if not f.has_origins:
            raise LedgerError('frame %d has no draw origins (a version-1 capture): the ledger needs the names' % fi)
        ri = fi - rec_off
        rec = cap.record(ri) if (cap.has_scene and 0 <= ri < cap.n) else None
        if cap.has_scene and rec is None:
            continue  # no record for this frame (the first frames at offset +1): nothing to source from, not counted
        rindex = RecordIndex(rec) if rec else None
        seen = collections.Counter()
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] not in sites:
                continue
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            vb = f.bank(d['vs_bank'])
            cls = (classify or classify_draw)(vn, o, vb, rindex)
            if not cls:
                continue
            k0 = (o['pass_site'], o['object'], o['part'])
            key = k0 + (seen[k0],)
            seen[k0] += 1
            pn = reg.variant_of(d['ps']) if d['ps'] else None
            pb = f.bank(d['ps_bank']) if d['ps'] else None
            for stage, name, bank in (('v', vn, vb), ('p', pn, pb)):
                if not name:
                    continue
                for r in reg.read_regs(name):
                    v = bank[4 * r:4 * r + 4]
                    kk = (key, stage, r)
                    s = stats.get(kk)
                    if s is None:
                        stats[kk] = [v, False, 1]
                    else:
                        if s[0] != v:
                            s[1] = True
                        s[2] += 1
    return stats


def mirror_equal(v, a):
    return v[0] == (a[0] ^ 0x80000000) and v[1:] == a[1:]


def derived_source(stage, r, v, cls, vs_regs, ps_regs, psb, plants):
    """S101 lane 109: the derived rule that applies to register r (stage 'v' / 'p') of this draw, as (rule, expected bits), or None when no rule applies.
    The caller counts applicable / matched and tags the source only when v == expected."""
    if 'no_derived' in plants:
        return None
    if stage == 'p' and r in (197, 199) and psb is not None and {0, 1, 2, 3} <= ps_regs:
        cols = [tuple(psb[4 * k:4 * k + 4]) for k in range(4)]
        if 'derived_drop_w' in plants:
            cols = [c[:3] + (0,) for c in cols]
        sm = light_rows.sum_rows(cols)
        return ('c197', tuple(light_rows.b32(x) for x in sm)) if r == 197 else ('c199', light_rows.derived_c199(sm))
    if stage == 'v' and r in (4, 12) and cls == 'wrestlers' and psb is not None and r in ps_regs:
        return ('key_color' if r == 4 else 'key_vec', tuple(psb[4 * r:4 * r + 4]))
    return None


class LightStateIndex:
    """One record's light tables: engine object -> the register-row tables of the states its model calls read (lazy)."""

    def __init__(self, rec):
        self.states = rec.light_states()
        self.by_object = collections.defaultdict(set)
        for obj, ordinal, state, frame in rec.light_refs():
            if state < len(self.states):
                self.by_object[obj].add(state)
        self._rows = {}

    def rows(self, i):
        t = self._rows.get(i)
        if t is None:
            t = self._rows[i] = light_rows.rows_from_state(self.states[i])
        return t

    def match(self, obj, stage, r, v):
        """True when v equals register r built from one of the states the object's calls read. All-0 / 1 / -1 vectors never match (they carry no information)."""
        if set(v) <= TRIVIAL:
            return False
        for i in self.by_object.get(obj, ()):
            e = self.rows(i).get(r)
            if e and (v in e.get(stage, ()) or v in e.get('*', ())):
                return True
        return False


class LightFrameIndex:
    """S101 lane 109 (review F3): the light tables of every record of a capture, indexed by the HOOK's frame (LightRef.frame) then engine object, so a stream
    frame is joined through a measured hook-to-stream offset, not by record index (a record can carry several closed frames, or none)."""

    def __init__(self, recs):
        self.records = 0
        self.by_frame = collections.defaultdict(lambda: collections.defaultdict(list))
        for rec in recs:
            if rec is None or not rec.count(13):
                continue
            self.records += 1
            li = LightStateIndex(rec)
            for obj, ordinal, state, frame in rec.light_refs():
                if state < len(li.states):
                    self.by_frame[frame][obj].append((li, state))

    def match(self, hook_frame, obj, stage, r, v):
        if set(v) <= TRIVIAL:
            return False
        for li, i in self.by_frame.get(hook_frame, {}).get(obj, ()):
            e = li.rows(i).get(r)
            if e and (v in e.get(stage, ()) or v in e.get('*', ())):
                return True
        return False


def detect_light_offset(cap, frames, reg, lfi):
    """The hook-to-stream frame offset, measured: stream guest_frame - offset = the hook frame whose states match the stream's DifLghCol c0 rows
    (best of -4..+4 over up to 8 sample frames). LedgerError when nothing matches at any offset."""
    sample = frames[::max(1, len(frames) // 8)][:8]
    votes = collections.Counter()
    for fi in sample:
        f = cap.frame(fi)
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] not in (SITE_A, SITE_B) or not f.draw(i)['ps']:
                continue
            d = f.draw(i)
            pn = reg.variant_of(d['ps'])
            if not pn or 0 not in reg.read_regs(pn):
                continue
            v = tuple(f.bank(d['ps_bank'])[0:4])
            for off in range(-4, 5):
                if lfi.match(f.guest_frame - off, o['object'], 'p', 0, v):
                    votes[off] += 1
    if not votes:
        raise LedgerError('cannot measure the hook-to-stream frame offset: no stream DifLghCol row matches any light state within +-4 frames')
    best = max(votes.values())
    return min((o for o, n in votes.items() if n == best), key=abs), dict(votes)


def run(path, frames_spec='', rec_off='auto', plants=(), reg_path=None, quiet=True, light_state=False, extra_sites=(), classify=None, family_fn=None, world=None):
    t0 = time.time()
    cap = Capture(path)
    reg = Registry(reg_path or os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    frames = parse_frames(frames_spec, cap.n)
    if not frames:
        raise LedgerError('--frames selects no frame')
    sites = (SITE_A, SITE_B, SITE_FLASH) + tuple(extra_sites)  # S101 lane 128: extra pass sites + a classify / family override (tools\skip\arena_ready.py)
    votes = None
    if 'offset0' in plants:
        rec_off = 0
    elif rec_off == 'auto':
        rec_off, votes = detect_offset(cap, frames)
    rec_off = int(rec_off)
    stats = collect(cap, frames, reg, rec_off, plants, sites, classify)
    # pass 2: classify
    res = {c: dict(draws=0, no_variant=0, full=0, regs=collections.Counter(), unsourced=collections.Counter(), unsourced_variants={},
                   palette=collections.Counter(), palette_obj=collections.defaultdict(collections.Counter), rot=collections.Counter(), rec_kinds=collections.Counter(), unsrc_sets=collections.Counter(), sources_by_reg=collections.defaultdict(collections.Counter),
                   derived_rules={k: [0, 0] for k in DERIVED_RULES}, light_state_regs=collections.Counter(),
                   fam=collections.defaultdict(lambda: dict(draws=0, full=0, regs=collections.Counter(), sbr=collections.defaultdict(collections.Counter), unsrc_sets=collections.Counter(), unsrc_sets_nw=collections.Counter(), objects=set(), uvals=collections.defaultdict(set)))) for c in CLASSES}
    nrec = 0
    skipped = 0
    # S101 lane 109 (F3): the light tables of all records, joined by the hook's frame through a measured offset; none = the light_state column is dormant
    lfi, light_off, light_votes = None, 0, None
    if cap.has_scene and 'no_record' not in plants:
        lfi = LightFrameIndex(cap.record(i) for i in range(cap.n))
        if lfi.records == 0:
            lfi = None
        else:
            light_off, light_votes = detect_light_offset(cap, frames, reg, lfi)
    light_records = lfi.records if lfi else 0
    for fi in frames:
        f = cap.frame(fi)
        ri = fi - rec_off
        rec = cap.record(ri) if (cap.has_scene and 0 <= ri < cap.n and 'no_record' not in plants) else None
        if rec:
            nrec += 1
        rindex = RecordIndex(rec) if rec else None
        crec = cap.record(fi - rec_off) if cap.has_scene and 0 <= fi - rec_off < cap.n else None
        if cap.has_scene and crec is None:
            skipped += 1
            continue
        arena_idx = RecordIndex(crec) if crec else None  # class rule always sees the record (even when a plant blinds the source side)
        draws = []
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or o['klass'] != 1 or o['pass_site'] not in sites:
                continue
            d = f.draw(i)
            vn = reg.variant_of(d['vs'])
            vb = f.bank(d['vs_bank'])
            cls = (classify or classify_draw)(vn, o, vb, arena_idx)
            if cls:
                draws.append((i, o, d, vn, cls))
        # pass bank: per (pass site, stage, reg) the value read by the most draws, kept when at least 90 % of the draws that read it agree
        votes_ = collections.defaultdict(collections.Counter)
        reads_ = collections.Counter()
        for i, o, d, vn, cls in draws:
            pn = reg.variant_of(d['ps']) if d['ps'] else None
            for stage, name, bank in (('v', vn, f.bank(d['vs_bank'])), ('p', pn, f.bank(d['ps_bank']) if d['ps'] else None)):
                if not name:
                    continue
                for r in reg.read_regs(name):
                    votes_[(o['pass_site'], stage, r)][tuple(bank[4 * r:4 * r + 4])] += 1
                    reads_[(o['pass_site'], stage, r)] += 1
        passbank = {}
        for k, c in votes_.items():
            v, n = c.most_common(1)[0]
            if n >= 2 and n >= 0.9 * reads_[k]:
                passbank[k] = v
        seen = collections.Counter()
        for i, o, d, vn, cls in draws:
            R = res[cls]
            R['draws'] += 1
            fam = family_fn(o, vn, f.bank(d['vs_bank']), arena_idx) if family_fn else None
            FR = R['fam'][fam] if fam else None
            if FR is not None:
                FR['draws'] += 1
                FR['objects'].add(o['object'])
            k0 = (o['pass_site'], o['object'], o['part'])
            key = k0 + (seen[k0],)
            seen[k0] += 1
            pn = reg.variant_of(d['ps']) if d['ps'] else None
            if d['ps'] and not pn:
                R['no_variant'] += 1
                continue
            cands = None
            bad = False
            vs_regs = reg.read_regs(vn)
            ps_regs = reg.read_regs(pn) if pn else set()
            psb = f.bank(d['ps_bank']) if d['ps'] else None
            rotcache = {}
            unsrc_here = []
            world_here = []  # lane 130: registers sourced only by the fitted world field (reported apart, never as proof)
            for stage, name, bank in (('v', vn, f.bank(d['vs_bank'])), ('p', pn, f.bank(d['ps_bank']) if d['ps'] else None)):
                if not name:
                    continue
                for r in sorted(reg.read_regs(name)):
                    v = tuple(bank[4 * r:4 * r + 4])
                    src = None
                    wclaim = world is not None and stage == 'v' and r in world.get((fi, i), ())  # lane 130: the (fitted) kScArenaWorlds field's value equals this register
                    if src is None and rindex is not None and not (set(v) <= TRIVIAL):
                        kind = rindex.lookup(v)
                        if kind:
                            src = 'record'
                            R['rec_kinds'][kind] += 1
                    if src is None:
                        # the pass bank is the main pass's (0x82C0A950); the mirrored copy 0x82C0B94C has no bank of its own, it gets the main pass's by the mirror rule
                        pbv = passbank.get((SITE_A if o['pass_site'] == SITE_B else o['pass_site'], stage, r))
                        if pbv is not None and pbv == v:
                            src = 'pass'
                        elif pbv is not None and o['pass_site'] == SITE_B and 'no_mirror_rule' not in plants and mirror_equal(v, pbv):
                            src = 'mirror'
                    if src is None:
                        s = stats.get((key, stage, r))
                        if s is not None and not s[1] and s[2] >= 2:
                            src = 'template'
                    if src is None:
                        if cands is None:
                            cands = viewport_candidates(f, d)
                        if is_role(v, cands):
                            src = 'role'
                    if src is None and lfi is not None and lfi.match(f.guest_frame - light_off, o['object'], stage, r, v):
                        src = 'light_state'
                        R['light_state_regs'][(stage, r)] += 1
                    dr = derived_source(stage, r, v, cls, vs_regs, ps_regs, psb, plants)
                    if dr is not None:
                        R['derived_rules'][dr[0]][0] += 1
                        if v == dr[1]:
                            R['derived_rules'][dr[0]][1] += 1
                            if src is None:
                                src = 'derived'
                    if wclaim and src in (None, 'template'):  # lane 130: shown in its own column; a register that was unsourced stays unsourced in the NO-FIELD sets (world_here)
                        if src is None:
                            world_here.append((stage, r))
                        src = 'world'
                    if src is None:
                        src = 'unsourced'
                        bad = True
                        if r in (8, 9, 10) and arena_idx is not None:
                            if (stage, 'rot') not in rotcache:
                                x = [f32vec(bank[4 * q:4 * q + 4])[:3] for q in (8, 9, 10)]
                                rotcache[(stage, 'rot')] = dir_signature(*x) in arena_idx.rot_sigs
                            if rotcache[(stage, 'rot')]:
                                R['rot'][(stage, r)] += 1
                        R['unsourced'][(stage, r)] += 1
                        unsrc_here.append((stage, r))
                        R['unsourced_variants'].setdefault((stage, r), collections.Counter())[vn if stage == 'v' else pn] += 1
                    R['regs'][src] += 1
                    R['sources_by_reg'][(stage, r)][src] += 1
                    if FR is not None:
                        FR['regs'][src] += 1
                        if src == 'unsourced' and len(FR['uvals'][(stage, r)]) < 4:
                            FR['uvals'][(stage, r)].add(v)
                        FR['sbr'][(stage, r)][src] += 1
                    if stage == 'v' and PALETTE_REG0 <= r < PALETTE_REG0 + PALETTE_REGS and vn and 'skinned' in vn:
                        R['palette'][src] += 1
                        R['palette_obj'][o['object']][src] += 1
            R['unsrc_sets'][tuple(sorted(x for x in unsrc_here))] += 1
            if FR is not None:
                FR['unsrc_sets'][tuple(sorted(x for x in unsrc_here))] += 1
                FR['unsrc_sets_nw'][tuple(sorted(x for x in unsrc_here + world_here))] += 1  # lane 130: without the fitted field
            if not bad:
                R['full'] += 1
                if FR is not None:
                    FR['full'] += 1
        if not quiet:
            print('frame %d done' % fi, file=sys.stderr)
    if light_state and not light_records:
        raise LedgerError('--light-state: none of the %d records read carries light tables (kScLightStates / kScLightRefs): a capture taken before lane 109\'s hook, or without it' % nrec)
    return dict(light_records=light_records, light_offset=light_off, light_votes=light_votes, path=cap.dir, frames=len(frames), rec_off=rec_off, offset_votes=votes, records_read=nrec, frames_without_record=skipped, plants=list(plants), res=res,
                secs=time.time() - t0, registry_variants=len(reg.variants))


def summarize(out, top=8):
    lines = []
    lines.append('light_state column: %s' % ('active (%d of %d records carry light tables; hook-to-stream frame offset %+d, votes %s)' % (out['light_records'], out['records_read'], out['light_offset'], out['light_votes']) if out['light_records'] else
                                             'dormant (no record carries kScLightStates / kScLightRefs)'))
    lines.append('ledger: %s  %d frames  record offset %+d%s  records read %d  %.1f s' % (
        os.path.basename(os.path.dirname(out['path'])) or out['path'], out['frames'], out['rec_off'],
        '  votes %s' % out['offset_votes'] if out['offset_votes'] else '', out['records_read'], out['secs']))
    for c in CLASSES:
        R = out['res'][c]
        n = R['draws']
        if not n:
            lines.append('  %-9s no draws in the sampled frames' % c)
            continue
        tot = sum(R['regs'].values())
        pct = 100.0 * R['full'] / n
        brk = ' '.join('%s %.1f%%' % (s, 100.0 * R['regs'][s] / tot) for s in SOURCES if tot and R['regs'][s])
        lines.append('  %-9s draws %6d  100%%-sourced %6.2f%%  (no-variant %d)  register reads %d: %s' % (c, n, pct, R['no_variant'], tot, brk))
        dr = ', '.join('%s %d/%d' % (k, v[1], v[0]) for k, v in R['derived_rules'].items() if v[0])
        if dr:
            lines.append('            derived rules matched/applicable: %s' % dr)
        if R['light_state_regs']:
            lines.append('            light_state-sourced register reads: %s' % ', '.join('%s c%d %d' % ('VS' if s == 'v' else 'PS', r, n) for (s, r), n in sorted(R['light_state_regs'].items())))
        if R['rec_kinds']:
            lines.append('            record-sourced register reads by kind: %s' % ', '.join('%s %d' % kv for kv in R['rec_kinds'].most_common()))
        if R['palette']:
            pt = sum(R['palette'].values())
            lines.append('            palette rows read %d: record %.1f%%, template/pass/other %.1f%%, unsourced %.1f%%' % (
                pt, 100.0 * R['palette']['record'] / pt, 100.0 * (pt - R['palette']['record'] - R['palette']['unsourced']) / pt,
                100.0 * R['palette']['unsourced'] / pt))
        for (stage, r), cnt in R['unsourced'].most_common(top):
            vs = ', '.join('%s x%d' % (k, v) for k, v in R['unsourced_variants'][(stage, r)].most_common(2))
            rot = R['rot'].get((stage, r), 0)
            lines.append('            unsourced %s c%-3d %-22s in %6d draws (%5.1f%%)%s  e.g. %s' % (
                'VS' if stage == 'v' else 'PS', r, '(' + reg_name(stage, r) + ')', cnt, 100.0 * cnt / n,
                '  [matches a record light set up to rotation in %d]' % rot if rot else '', vs))
        cc = closure_curve(R)
        if cc:
            lines.append('            draws 100%% sourced if the top K unsourced registers were closed: %s' % '  '.join('K=%d %.1f%%' % x for x in cc))
    return lines


def expect_checks(out, name):
    """(ok, message) pairs for one --expect name."""
    res = out['res']
    chk = []
    if name in ('checkB', 'r7079'):
        w = res['wrestlers']
        objs = w['palette_obj']
        if not objs:
            return [(False, 'no wrestler palette reads: nothing to judge')]
        # the main wrestler = the object with the most record-sourced palette rows (the other actors' palettes are never in the record)
        main = max(objs, key=lambda k: objs[k]['record'])
        c = objs[main]
        pt, un = sum(c.values()), c['unsourced']
        if name == 'checkB':
            chk.append((un > 0, 'Check B (run 7051): the main wrestler (object 0x%08X) has palette rows unsourced > 0 (the record held the old palette for nodes 59-108): %d of %d rows (%.2f%% sourced)' % (main, un, pt, 100.0 * (pt - un) / pt)))
        else:
            chk.append((un == 0, 'run 7079 (lane 43): every palette row of the main wrestler (object 0x%08X) sourced: unsourced %d of %d rows (record %.1f%%)' % (main, un, pt, 100.0 * c['record'] / pt)))
    if name == 'derived':
        w = res['wrestlers']['derived_rules']
        for rule in DERIVED_RULES:
            app, ok = w[rule]
            chk.append((app > 0 and ok == app, 'derived rule %s on the wrestlers: matched %d of %d applicable draw reads (bit-exact; want all, and some)' % (rule, ok, app)))
        others = {c: {k: v for k, v in res[c]['derived_rules'].items() if v[0]} for c in ('arena', 'ring', 'flash')}
        chk.append((True, 'info, other classes (not asserted): ' + '; '.join('%s %s' % (c, ' '.join('%s %d/%d' % (k, v[1], v[0]) for k, v in o.items()) or 'none') for c, o in others.items())))
    if name == 'mirror':
        a = res['arena']
        mir = sum(c['mirror'] for k, c in a['sources_by_reg'].items())
        cam = {(s, r): c for (s, r), c in a['sources_by_reg'].items() if s == 'v' and 56 <= r <= 59}
        un = sum(c['unsourced'] for c in cam.values())
        chk.append((mir > 0 and un == 0, 'mirrored arena: mirror-rule sourced register reads %d, unsourced camera (VS c56-c59) reads %d' % (mir, un)))
    if not chk:
        chk.append((False, 'unknown --expect ' + name))
    return chk


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--record-offset', default='auto')
    ap.add_argument('--plant', default='', choices=('',) + PLANTS)
    ap.add_argument('--json', default='')
    ap.add_argument('--light-state', action='store_true', help='require the light_state column: fail when no record carries light tables')
    ap.add_argument('--expect', action='append', default=[])
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args(argv)
    try:
        out = run(a.capture, a.frames, a.record_offset, (a.plant,) if a.plant else (), quiet=a.q, light_state=a.light_state)
    except (LedgerError, OSError, KeyError, struct.error, ValueError) as e:  # ValueError: a record reader refusing a layout (F6): a FAIL line, not a traceback
        print('FAIL  %s: %s' % (a.capture, e))
        return 1
    for l in summarize(out):
        print(l)
    bad = 0
    for e in a.expect:
        for ok, msg in expect_checks(out, e):
            print('%s  expect %-8s %s' % ('PASS' if ok else 'FAIL', e, msg))
            bad += 0 if ok else 1
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        j = {k: v for k, v in out.items() if k != 'res'}
        j['classes'] = {c: dict(draws=r['draws'], full=r['full'], no_variant=r['no_variant'], regs=dict(r['regs']), palette=dict(r['palette']), palette_by_object={hex(k): dict(v) for k, v in r['palette_obj'].items()},
                                derived_rules=r['derived_rules'], light_state_regs={'%s%d' % k: v for k, v in r['light_state_regs'].items()},
                                unsourced=[[s, reg_, n, reg_name(s, reg_)] for (s, reg_), n in r['unsourced'].most_common(40)],
                                unsourced_reads=sum(r['unsourced'].values()), closure=closure_curve(r), record_kinds=dict(r['rec_kinds']), rot={'%s%d' % k: v for k, v in r['rot'].items()}) for c, r in out['res'].items()}
        json.dump(j, open(a.json, 'w'), indent=1, default=str)
    print('ledger: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
