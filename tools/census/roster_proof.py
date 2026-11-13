r"""Lane 158 (S101): the per-wrestler proof. For every wrestler OBJECT of one captured frame: did it come out OURS (drawn by the wrestler hook), or is it still
the stream's, and if the hook looked at it and kept it, why?

The wrestler hook's census line (`[rdr][wrestlers] objects seen N joined J ... replaced R | by slot ...`, wrestler_core.h Report) is aggregate: counts by
skeleton slot, never by object or character. fview's `--ops --json` is per draw: fate (wrestlers_ours / stream / ...), `keep_why` (the reason the LAST hook that
claimed the draw kept it: not ready / unjoined / mismatch / variant / texture / palette / not ours), object, part, pass site, technique. This tool reads those.

  py -I tools\census\roster_proof.py CASE_ZIP_OR_CAPTURE [--frame N|last] [--arm ours|off] [--label 0xADDR=NAME ...] [--expect-ours 0xADDR,...] [--expect-stream 0xADDR,...]
                                      [--fview EXE] [--out DIR] [--json FILE] [--plant NAME] [--timeout S]
  Several frames and the roster bar:  --frames 60,150,240,300 (the verdict per object = its WORST frame; a wrestler off screen in one frame is judged on the frames it is in),
  --arm both (the ours arm AND the plant arm with the hook off: every wrestler must read stream there), --picks 343,324,... (character ids: each must be found as an OURS object of its
  atlas body size, default attire entry p0/0; the size is a lead [L], not an identification), --caw (the body no pick explains, not the referee, must exist and be ours).
  py -I tools\census\roster_proof.py --ops OPS.json [--ref-ops OPS.json] [...]       (an ops json made earlier: leftovers.py writes ops_fN.json)
  py -I tools\census\roster_proof.py --selftest                                       (synthetic tables + planted twins; no fview, no game files)

WHAT IS A WRESTLER OBJECT. An engine object (the `object` of a model-part draw) that either (a) has a draw with fate wrestlers_ours, or (b) is drawn into the rim
light map (technique T_RimLightMap: only the wrestler bodies are rim-light casters; the ring, the props and the arena are not). (b) is what makes the OFF arm
readable: with `--wrestlers-ours` off nothing is ours and nothing is claimed, but the same objects are still the rim casters. `--ref-ops` gives the object set from
another arm of the same frame (the plant uses it).
The verdict reads the object's MAIN-PASS colour draws (pass site 0x82C0A950 / 0x82C0B94C, the sites the hook claims; no technique):
  ours      every main-pass draw of the object is wrestlers_ours.
  partial   some are ours, some still the stream's (the parts and the reasons are listed).
  refused   none ours, and the hook (or another) claimed the draw and kept it: keep_why says why (unjoined / mismatch / palette / texture / variant / not ready).
  stream    none ours, no hook claimed it (the class was not promoted for this capture, `not ours`, or the flag was off).
The OWNERSHIP of the other passes (rim map, shadow maps) is printed as counts for information; it is not in the verdict.

NAMES. The ops carry no character. `--label 0xADDR=NAME` names an object (the bar uses the addresses the evidence names: Brock 0xA90BC480, lane 79 UNCLAIMED_MODEL_PARTS.md 1.1).
Without a label the row shows `fits:` = characters whose default attire's body model has exactly that many parts in the drawables atlas (out\atlas\atlas.json, via
roster_materials.py): a lead [L], not an identification; several names can fit.

Plants (--plant, acts on the table before the verdict; each must break the bar):
  fate_swap     every wrestlers_ours draw is read as stream (the hook "off"): all wrestler rows must read stream / refused.
  drop_rim      the rim-map draws are removed: an off-arm table then has no wrestler objects at all, and the tool must say so (exit 3), not pass.
Exit: 0 ok, 1 an --expect-* failed, 2 usage / tool error, 3 the table holds no wrestler objects or the frame is incomplete (a verdict is never printed as a pass then).
"""
import argparse
import collections
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

MAIN_SITES = (0x82C0A950, 0x82C0B94C)
RIM = 'T_RimLightMap'
SHADOW = 'T_ShadowMap'
OURS = 'wrestlers_ours'
REFUSAL = ('not ready', 'unjoined', 'mismatch', 'variant', 'texture', 'palette')   # draw_replace_hook.h KeepWhy names (NotOurs is "not ours")


def fail(msg, code=2):
    print('FAIL ' + msg, flush=True)
    sys.exit(code)


def load_draws(ops):
    """-> list of model-part draws of an ops json (dict or path)."""
    d = ops if isinstance(ops, dict) else json.loads(Path(ops).read_text(encoding='utf-8'))
    return [o for o in d['ops'] if o.get('kind') == 'draw' and o.get('class') == 'model_part']


def plant(draws, name):
    if not name:
        return draws
    if name == 'fate_swap':
        return [dict(d, fate='stream', keep_why='') if d['fate'] == OURS else d for d in draws]
    if name == 'drop_rim':
        return [d for d in draws if d.get('technique') != RIM and d['fate'] != OURS]
    fail(f'unknown --plant {name}')


def wrestler_objects(draws, ref=None):
    """The set of wrestler objects: wrestlers_ours draws and the rim-map casters, of this table and (if given) of the reference table."""
    s = set()
    for t in (draws, ref or []):
        for d in t:
            if d['object'] and (d['fate'] == OURS or d.get('technique') == RIM):
                s.add(d['object'])
    return s


def judge(draws, objs, labels=None, fits=None):
    """-> rows, one per wrestler object, sorted by object. fits: {part_count: [names]} (a lead)."""
    labels = labels or {}
    rows = []
    for o in sorted(objs):
        mine = [d for d in draws if d['object'] == o]
        main = [d for d in mine if d['pass_site'] in MAIN_SITES and not d.get('technique')]
        ours = [d for d in main if d['fate'] == OURS]
        rest = [d for d in main if d['fate'] != OURS]
        why = collections.Counter((d.get('keep_why') or '-') for d in rest)
        parts_all = sorted({d['part'] for d in main})
        parts_ours = sorted({d['part'] for d in ours})
        parts_rest = sorted({d['part'] for d in rest} - set(parts_ours))
        if not main:
            # lane 158 review S4: nothing to judge in THIS frame (the object is only in its rim / shadow passes here): skipped in the union, never a pass
            verdict, reason = 'absent', 'no main-pass colour draw of this object in the frame (only its rim / shadow passes)'
        elif not rest:
            verdict, reason = 'ours', ''
        elif ours:
            verdict = 'partial'
            reason = ', '.join(f'{k} x{n}' for k, n in why.most_common())
        else:
            ref = [k for k in why if k in REFUSAL]
            verdict = 'refused' if ref else 'stream'
            reason = ', '.join(f'{k} x{n}' for k, n in why.most_common()) if why else ''
            if verdict == 'stream' and not reason.strip('- x0123456789,'):
                reason = 'no hook claimed it (class not promoted for this capture, or off)'
        rim = [d for d in mine if d.get('technique') == RIM]
        sh = [d for d in mine if d.get('technique') == SHADOW]
        rows.append({'object': '0x%08X' % o, 'label': labels.get(o, ''), 'fits': (fits or {}).get(len(parts_all), [])[:6] if not labels.get(o) else [],
                     'parts': len(parts_all), 'main_draws': len(main), 'ours': len(ours), 'stream': len(rest), 'verdict': verdict, 'reason': reason,
                     'parts_stream': parts_rest, 'rim_draws': len(rim), 'rim_ours': sum(1 for d in rim if d['fate'] != 'stream'),
                     'shadow_draws': len(sh), 'shadow_ours': sum(1 for d in sh if d['fate'] != 'stream')})
    return rows


def render(rows, title):
    L = [f'### {title}', '', '| object | name | parts | main draws | ours | stream | verdict | why / parts still stream | rim (ours/all) | shadow (ours/all) |', '|---|---|---|---|---|---|---|---|---|---|']
    for r in rows:
        nm = r['label'] or ('fits: ' + ' / '.join(r['fits']) if r['fits'] else '?')
        ps = r['parts_stream']
        extra = r['reason'] + ((' parts ' + (','.join(str(p) for p in ps) if len(ps) <= 8 else f'{len(ps)} of {r["parts"]}')) if ps and r['verdict'] != 'ours' else '')
        L.append(f"| {r['object']} | {nm} | {r['parts']} | {r['main_draws']} | {r['ours']} | {r['stream']} | **{r['verdict'].upper()}** | {extra or '-'} | {r['rim_ours']}/{r['rim_draws']} | {r['shadow_ours']}/{r['shadow_draws']} |")
    c = collections.Counter(r['verdict'] for r in rows)
    L += ['', f"{len(rows)} wrestler objects: " + ', '.join(f'{c.get(k, 0)} {k}' for k in ('ours', 'partial', 'refused', 'stream'))]
    return '\n'.join(L)


def parse_addr(s):
    return int(s, 16) if s.lower().startswith('0x') else int(s, 16)


def check_expect(rows, ours_list, stream_list):
    """-> list of failure strings. An address that is not a wrestler object in the table fails too (an expectation on an object nobody drew is not a pass)."""
    by = {int(r['object'], 16): r for r in rows}
    bad = []
    for a in ours_list:
        r = by.get(a)
        if r is None:
            bad.append(f'0x{a:08X}: expected OURS, but it is not a wrestler object in this frame')
        elif r['verdict'] != 'ours':
            bad.append(f"0x{a:08X} ({r['label'] or '?'}): expected OURS, read {r['verdict'].upper()} ({r['reason'] or 'parts ' + ','.join(map(str, r['parts_stream']))})")
    for a in stream_list:
        r = by.get(a)
        if r is None:
            bad.append(f'0x{a:08X}: expected STREAM, but it is not a wrestler object in this frame')
        elif r['verdict'] == 'ours':
            bad.append(f"0x{a:08X}: expected STREAM (the plant arm), read OURS")
    return bad


def atlas_fits():
    """{body part count: [names]} from the drawables atlas (a lead, not an identification). {} when the atlas is not there."""
    try:
        import roster_materials as RM
        atlas = RM.main_checkout() / 'out' / 'atlas' / 'atlas.json'
        if not atlas.exists():
            return {}
        names = {e['id']: e.get('name') for e in json.loads((HERE / 'db' / 'ids' / 'characters.json').read_text(encoding='utf-8'))['entries']}
        out = collections.defaultdict(list)
        with open(atlas, encoding='utf-8', errors='replace') as fh:
            t = RM.scan(fh)
        for cid, v in t.items():
            body = sum(v['entries'].get('p0/0', {}).values())
            if body:
                out[body].append(f"{names.get(cid, cid)}")
        return dict(out)
    except Exception:
        return {}


# ---------------------------------------------------------------- fview
def run_fview(fview, cap, frame, flags, out_json, timeout):
    cmd = [str(fview), '--capture', str(cap), '--frame', str(frame), '--root', str(HERE)] + flags + ['--ops', '--json', str(out_json)]
    env = dict(os.environ)
    Path(out_json).unlink(missing_ok=True)   # review S2: a stale json from an earlier run must never be read as this run's
    for attempt in range(2):   # the GPU is shared: one 0xC0000409 crash on start was seen (2026-11-12) and a plain retry ran clean
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace', env=env)
        except subprocess.TimeoutExpired:
            fail(f'fview timeout after {timeout}s: {" ".join(cmd[:6])} ...')
        if p.returncode == 0 or Path(out_json).exists():
            break
    text = p.stdout + p.stderr
    if p.returncode != 0 and not Path(out_json).exists():
        fail(f'fview exit {p.returncode}: ' + text[-1200:])
    return text


def main_capture(a):
    import leftovers as LO
    main_co = LO.main_checkout()
    # lane 214: the NEWEST fview.exe of the candidates, not the first that exists (K1 run 7137: a worktree without its own build fell to the main checkout's dev exe from 02:26,
    # built before lane 206's long map: the pads read unjoined / "4 not learned" although the current code joins them). The exe used is printed with its build time.
    # lane 218: the shared locator (tools\fview\fview_exe.py): the newest own build, else the main checkout's, and it REFUSES an exe older than the renderer sources.
    sys.path.insert(0, str(HERE / 'tools' / 'fview'))
    import fview_exe as fe
    try:
        fview = Path(fe.find_fview(prefer=a.fview, allow_stale=a.allow_stale, root=HERE, kind=None))
    except fe.FviewError as e:
        fail(str(e))
    if not fview or not fview.exists():
        fail('no fview.exe: build it with tools\\fview\\build.ps1 or pass --fview')
    import datetime as _dt
    print(f'[roster_proof] fview: {fview} (built {_dt.datetime.fromtimestamp(fview.stat().st_mtime):%Y-%m-%d %H:%M}); an exe older than the code under test reads pieces the code now joins as unjoined', flush=True)
    # review S2: the work folder is named after the zip (its stem), not its parent: two zips in one run folder must not share one
    out = Path(a.out) if a.out else main_co / 'out' / 's101' / 'roster_proof' / (Path(a.input).stem if Path(a.input).suffix.lower() == '.zip' else Path(a.input).name)
    out.mkdir(parents=True, exist_ok=True)
    cap = LO.open_input(Path(a.input), out)
    if not (cap / 'images.bin').exists():
        fail(f'{cap}: not a capture folder (no images.bin)')
    nframes = len(list((cap / 'frames').glob('*'))) if (cap / 'frames').is_dir() else 0
    if a.frames:
        frames = [int(x) for x in a.frames.split(',')]
    else:
        frames = [max(nframes - 1, 0) if a.frame == 'last' else int(a.frame)]
    for f in frames:
        if not 0 <= f < max(nframes, 1):
            fail(f'frame {f}: the capture has {nframes} frames')
    has_records = (cap / 'scene').is_dir() and any((cap / 'scene').glob('frame_*.rsc'))
    game = str(main_co / 'game_data' / 'extracted')
    thin_set = LO.thin_frames(cap)
    flags_ours = ['--wrestlers-ours', '--game', game]
    thin_cls = []
    if thin_set:
        # lane 143: a thin capture is replayed with --thin for the promoted classes (the lite draws carry no geometry); the wrestler hook alone is promoted here, plus
        # the ring and the arena so that the lite draws of the other classes are not dropped (they are not read by the verdict)
        flags_ours += ['--arena-ours', '--ring-ours']
        thin_cls = ['arena', 'ring', 'wrestlers']
        flags_ours += ['--thin', ','.join(thin_cls)]
    arms = {}
    if a.arm in ('ours', 'both'):
        if not has_records:
            print(f'[roster_proof] {cap}: no scene records (capture/scene/frame_*.rsc): the wrestler hook cannot run on it; the ours arm will read STREAM for every wrestler (reason: no records)', flush=True)
        arms['ours'] = flags_ours if has_records else []
    if a.arm in ('off', 'both'):
        # the plant arm: everything the ours arm has EXCEPT --wrestlers-ours (a thin capture keeps arena / ring on so their lite draws are drawn; the wrestlers' are not learned: expected)
        arms['off'] = (['--arena-ours', '--ring-ours', '--game', game, '--thin', 'arena,ring'] if (thin_cls and has_records) else [])
    ops = {}
    incomplete = set()
    for name, fl in arms.items():
        for frame in (frames if name == 'ours' else frames[:1]):   # the plant arm needs one frame only
            oj = out / f'ops_{name}_f{frame}.json'
            text = run_fview(fview, cap, frame, fl, oj, a.timeout)
            for ln in text.splitlines():
                m = LO.THIN_LINE.search(ln)
                if m and int(m.group(3)) > 0 and name == 'ours':   # the plant arm (hook off) does not learn the wrestlers' lite draws by design: only the ours arm decides completeness
                    incomplete.add(frame)
            ops[(name, frame)] = oj
    return ops, frames, incomplete, has_records, bool(thin_set), cap, game


def selftest():
    def d(obj, part, fate='stream', why='', site=0x82C0A950, tech=''):
        return {'kind': 'draw', 'class': 'model_part', 'object': obj, 'part': part, 'fate': fate, 'keep_why': why, 'pass_site': site, 'technique': tech}
    B, Y, R, P, WALL = 0xA90BC480, 0xA8F78BE0, 0xA89AA220, 0xA8FB30B0, 0xAB000000
    t = []
    t += [d(B, i, OURS) for i in range(26)] + [d(B, i, site=0x82BFBD58, tech=RIM) for i in range(26)]   # Brock: all ours
    t += [d(Y, i, OURS) for i in range(14)] + [d(Y, i, site=0x82BFBD58, tech=RIM) for i in range(14)]  # Bryan: all ours
    t += [d(R, i, OURS) for i in range(10)] + [d(R, i, site=0x82BFBD58, tech=RIM) for i in range(10)]  # referee
    t += [d(P, 0, 'stream', 'palette'), d(P, 1, OURS), d(P, 0, site=0x82BFBD58, tech=RIM)]            # a piece: partial
    t += [d(0xA90BC999, 0, 'stream', 'unjoined'), d(0xA90BC999, 0, site=0x82BFBD58, tech=RIM)]         # a refused wrestler
    t += [d(0xA90BCAAA, 0), d(0xA90BCAAA, 0, site=0x82BFBD58, tech=RIM)]                                # a stream wrestler (unclaimed)
    t += [d(WALL, i, 'arena_ours') for i in range(5)] + [d(0xAA141900, 0, 'stream', 'not ours')]       # not wrestlers
    ops = {'ops': [dict(x, op=i) for i, x in enumerate(t)]}
    draws = load_draws(ops)
    rows = judge(draws, wrestler_objects(draws), {B: 'Brock', Y: 'Bryan', R: 'Referee'})
    by = {r['object']: r for r in rows}
    ok = []

    def chk(c, w):
        ok.append(c)
        print(('ok   ' if c else 'FAIL ') + w)
    chk(len(rows) == 6, 'six wrestler objects found (props and arena objects are not wrestlers)')
    chk(by['0xA90BC480']['verdict'] == 'ours' and by['0xA8F78BE0']['verdict'] == 'ours' and by['0xA89AA220']['verdict'] == 'ours', 'Brock, Bryan and the referee read ours')
    chk(by['0xA8FB30B0']['verdict'] == 'partial' and 'palette' in by['0xA8FB30B0']['reason'], 'a half-ours piece reads partial with its reason')
    chk(by['0xA90BC999']['verdict'] == 'refused' and 'unjoined' in by['0xA90BC999']['reason'], 'a claimed-and-kept wrestler reads refused (unjoined)')
    chk(by['0xA90BCAAA']['verdict'] == 'stream', 'an unclaimed wrestler reads stream')
    chk(check_expect(rows, [B, Y, R], []) == [], 'expect-ours passes for the three')
    chk(len(check_expect(rows, [P], [])) == 1 and len(check_expect(rows, [0xDEAD0000], [])) == 1, 'expect-ours fails for a partial piece and for an address that is not a wrestler')
    # plant fate_swap: the same table with the hook "off": every wrestler must read stream / refused, none ours
    pd = plant(draws, 'fate_swap')
    prow = judge(pd, wrestler_objects(pd), {})
    chk(all(r['verdict'] in ('stream', 'refused') for r in prow) and len(prow) == 6, 'plant fate_swap: all six wrestler rows read stream / refused (the rim casters still name them)')
    chk(len(check_expect(prow, [B, Y, R], [])) == 3, 'plant fate_swap: the expect-ours bar FAILS for Brock, Bryan and the referee')
    chk(check_expect(prow, [], [B, Y, R]) == [], 'plant fate_swap: the expect-stream bar passes (the off arm)')
    # plant drop_rim on an off table: no wrestler objects at all -> the tool must refuse to pass (exit 3 path)
    off = plant(plant(draws, 'fate_swap'), 'drop_rim')
    chk(len(wrestler_objects(off)) == 0, 'plant drop_rim: an off table without rim casters has no wrestler objects (the tool exits 3, never a silent pass)')
    # --ref-ops: the object set from the ours arm names the objects of the off arm
    refobjs = wrestler_objects(off, draws)
    chk(len(refobjs) == 6, '--ref-ops: the reference arm supplies the object set')
    # a table where only the doubled alpha draws are stream: partial, parts listed
    t2 = [d(B, i, OURS) for i in range(3)] + [d(B, 1, 'stream', 'palette')] + [d(B, 0, site=0x82BFBD58, tech=RIM)]
    r2 = judge(load_draws({'ops': t2}), {B}, {})
    chk(r2[0]['verdict'] == 'partial' and r2[0]['parts_stream'] == [] , 'a part drawn twice (ours and stream) is partial with no part that is ONLY stream')
    # several frames: the worst verdict wins, a wrestler off screen in one frame is judged on the frames it is in
    f1 = judge(load_draws({'ops': [d(B, i, OURS) for i in range(26)] + [d(B, 0, site=0x82BFBD58, tech=RIM)]}), {B}, {})
    f2 = judge(load_draws({'ops': [d(B, i, OURS) for i in range(25)] + [d(B, 25, 'stream', 'palette')] + [d(B, 0, site=0x82BFBD58, tech=RIM)]}), {B}, {})
    f3 = judge(load_draws({'ops': [d(Y, i, OURS) for i in range(14)] + [d(Y, 0, site=0x82BFBD58, tech=RIM)]}), {Y}, {})
    u = union({10: f1, 20: f2, 30: f3})
    ub = {r['object']: r for r in u}
    chk(ub['0xA90BC480']['worst'] == 'partial' and len(ub['0xA90BC480']['frames']) == 2 and ub['0xA8F78BE0']['worst'] == 'ours', 'union over frames: the worst verdict wins (Brock partial in one of two), Bryan seen once is ours')
    lines, bad, _used = check_picks(u, {343: ('Brock Lesnar', 26), 324: ('Daniel Bryan', 14), 342: ('Sin Cara', 10)})
    chk(len(bad) == 2 and 'Sin Cara: not found' in bad and any('Brock' in b for b in bad), 'picks: Brock partial and Sin Cara missing both fail; Bryan (14 parts, ours) passes')
    lines, bad, _used = check_picks(union({10: f1, 30: f3}), {343: ('Brock Lesnar', 26), 324: ('Daniel Bryan', 14)})
    chk(bad == [], 'picks: all found and ours -> no failure')
    # --caw: the unexplained body (not a pick's object, not the referee's size) must exist and be ours
    CAW = 0xA95C0000
    t4 = [d(B, i, OURS) for i in range(26)] + [d(B, 0, site=0x82BFBD58, tech=RIM)] + [d(R, i, OURS) for i in range(10)] + [d(R, 0, site=0x82BFBD58, tech=RIM)]
    t4 += [d(CAW, i, 'stream', 'unjoined') for i in range(31)] + [d(CAW, 0, site=0x82BFBD58, tech=RIM)]
    u4 = union({1: judge(load_draws({'ops': t4}), wrestler_objects(load_draws({'ops': t4})), {}, {})})
    _l, _b, used4 = check_picks(u4, {343: ('Brock Lesnar', 26)})
    cl, cb = check_caw(u4, used4, 12)
    chk(len(cb) == 1 and 'unjoined' in cl[0] and CAW and '0xA95C0000' in cl[0], '--caw: the 31-part unexplained body reads REFUSED (unjoined); Brock and the referee are not CAW candidates')
    t5 = [x if x['object'] != CAW else dict(x, fate=OURS, keep_why='') for x in t4]
    u5 = union({1: judge(load_draws({'ops': t5}), wrestler_objects(load_draws({'ops': t5})), {}, {})})
    cl, cb = check_caw(u5, used4, 12)
    chk(cb == [], '--caw: the same body ours -> no failure')
    cl, cb = check_caw(union({1: judge(load_draws({'ops': t4[:28]}), wrestler_objects(load_draws({'ops': t4[:28]})), {}, {})}), used4, 12)
    chk(cb == ['caw: not found'], '--caw: no unexplained body in the frames -> FAIL "not found", never a pass')
    # ---- lane 158 review plants
    import io, contextlib, tempfile
    # B1 / S5: an unpicked piece that is not ours must fail the bar; --allow excuses it; an object with only rim draws never passes
    PIECE, RIMONLY = 0xA8FB8810, 0xA9100000
    tb = [d(B, i, OURS) for i in range(26)] + [d(B, 0, site=0x82BFBD58, tech=RIM)] + [d(PIECE, 0, 'stream', 'palette'), d(PIECE, 0, site=0x82BFBD58, tech=RIM)] + [d(RIMONLY, 0, site=0x82BFBD58, tech=RIM)]
    ub1 = union({1: judge(load_draws({'ops': tb}), wrestler_objects(load_draws({'ops': tb})), {}, {})})
    chk({r['object']: r['worst'] for r in ub1}['0xA9100000'] == 'stream', 'plant rim-only: an object with only rim draws reads STREAM (absent in every frame), never ours')
    ev = check_everyone(ub1, set())
    chk(len(ev) == 2, 'B1: the not-ours piece AND the rim-only object both fail "every wrestler object is ours"')
    chk(len(check_everyone(ub1, {PIECE, RIMONLY})) == 0, 'B1: --allow excuses exactly the named objects')
    # S4: an absent frame is skipped, not a pass and not a fail for an object that is ours elsewhere
    fa = judge(load_draws({'ops': [d(B, 0, site=0x82BFBD58, tech=RIM)]}), {B}, {})
    ua = union({1: fa, 2: f1})
    chk(ua[0]['worst'] == 'ours' and ua[0]['frames'] == [2] and ua[0]['absent_frames'] == [1], 'S4: an absent frame is skipped in the union (judged on the frame it is in)')
    # S1: a pick id the atlas has no body for fails, it is not dropped
    ln, bd, _u = check_picks(ub1, {343: ('Brock Lesnar', 26)}, missing=[999])
    chk(bd == ['pick 999: no atlas body'], 'S1: a dropped pick id (no atlas body) is a failure')
    # B2 / S5: same-size collision. Sin Cara (10 parts) is "found" by size on the referee's object; the BODY list is what refuses it
    ur = union({1: judge(load_draws({'ops': [d(R, i, OURS) for i in range(10)] + [d(R, 0, site=0x82BFBD58, tech=RIM)]}), {R}, {})})
    ln, bd, _u = check_picks(ur, {342: ('Sin Cara', 10)})
    chk(bd == [], 'B2: the size check alone accepts the referee as Sin Cara (the collision is real, said as a lead [L])')
    bl, bb = check_bodies({(255, 0), (343, 0)}, [342, 343], [])
    chk(bb == ['ch342 attire 0 not joined'], 'B2: the BODY list refuses Sin Cara (ch342 attire 0 not joined) although a 10-part object is ours')
    # B3: Bryan left in seat 0 / participant skipped / a stock body in the match
    bl, bb = check_bodies({(324, 0), (343, 0)}, [343], [324])
    chk(bb == ['ch324 is in the match'], 'B3: forbid-bodies 324: Bryan still in the match fails')
    with tempfile.TemporaryDirectory() as td:
        lg = Path(td) / 'run.log'
        lg.write_text("[x] launch 'lesnar_bryan': participant 0 = id 5 (CPU)\n[x] launch 'lesnar_bryan': participant 1 = id 343 (CPU)\n", encoding='utf-8')
        ll, lb = check_launch_log(lg, {0: 5})
        chk(lb == [], 'B3: the log line "participant 0 = id 5" satisfies --expect-seat 0=5')
        lg.write_text("[x] launch participant '999' skipped (id 0-499, pad -1..3)\n[x] launch 'lesnar_bryan': participant 1 = id 343 (CPU)\n", encoding='utf-8')
        ll, lb = check_launch_log(lg, {0: 999})
        chk(len(lb) == 2, 'B3: a skipped participant and a missing seat line both fail')
        # S5: the exit paths of main(), through real files
        def ops_file(name, rows):
            p = Path(td) / name
            p.write_text(json.dumps({'ops': [dict(x, op=i) for i, x in enumerate(rows)]}), encoding='utf-8')
            return str(p)
        ours_t = [d(B, i, OURS) for i in range(26)] + [d(B, 0, site=0x82BFBD58, tech=RIM)] + [d(R, i, OURS) for i in range(10)] + [d(R, 0, site=0x82BFBD58, tech=RIM)]
        off_t = [dict(x, fate='stream') for x in ours_t]
        off_bad = [x if x['object'] != R else dict(x, fate=OURS) for x in off_t]
        po, pf, pbad, pown = ops_file('o.json', ours_t), ops_file('f.json', off_t), ops_file('fb.json', off_bad), ops_file('o2.json', ours_t + [d(PIECE, 0, 'stream', 'palette'), d(PIECE, 0, site=0x82BFBD58, tech=RIM)])
        empty = ops_file('e.json', [d(WALL, 0, 'arena_ours')])

        def run_main(*argv):
            old = sys.argv
            sys.argv = ['roster_proof'] + list(argv)
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    return main()
            finally:
                sys.argv = old
        chk(run_main('--ops', po, '--off-ops', pf, '--expect-ours', '0xA90BC480,0xA89AA220') == 0, 'main: ours table all ours + off table all stream, expect-ours -> exit 0')
        chk(run_main('--ops', po, '--off-ops', pbad) == 1, 'S5 off-arm branch: an object reading OURS with the hook off -> exit 1')
        chk(run_main('--ops', pown) == 1, 'B1 through main: an unpicked not-ours piece -> exit 1')
        chk(run_main('--ops', pown, '--allow', '0xA8FB8810') == 0, 'B1 through main: the same piece with --allow -> exit 0')
        chk(run_main('--ops', empty) == 3, 'a table with no wrestler objects -> exit 3, never a pass')
        chk(run_main('--ops', po, '--expect-seat', '0=5') == 1, 'B3 through main: --expect-seat without --run-log -> exit 1')
    tabs, stop = complete_tables({('ours', 300): 'p'}, {300}, 'both')
    chk(stop == 3 and tabs == [], 'S5 incomplete frame: the only ours frame incomplete -> exit 3')
    tabs, stop = complete_tables({('ours', 200): 'a', ('ours', 300): 'b', ('off', 200): 'c'}, {300}, 'both')
    chk(stop == 0 and [t[1] for t in tabs] == [200, 200], 'S5 incomplete frame: frame 300 left out, 200 kept')
    print('roster_proof selftest: ' + ('PASS' if all(ok) else 'FAIL'))
    return 0 if all(ok) else 1


RANK = {'ours': 0, 'partial': 1, 'stream': 2, 'refused': 3}   # 'absent' rows are not in the union's verdicts (review S4)


def union(per_frame):
    """per_frame: {frame: rows} -> rows over the frames: one row per object, the WORST verdict seen (refused > stream > partial > ours), the frames it was JUDGED in and the verdict counts.
    A frame where the object has no main-pass draw ('absent') is skipped; an object absent in every frame reads STREAM (nothing of it was ever shown to be ours)."""
    by = {}
    for fr, rows in sorted(per_frame.items(), key=lambda kv: str(kv[0])):
        for r in rows:
            u = by.setdefault(r['object'], {'object': r['object'], 'label': r['label'], 'fits': r['fits'], 'parts': 0, 'frames': [], 'absent_frames': [], 'verdicts': collections.Counter(), 'reason': '', 'worst': None})
            if r['verdict'] == 'absent':
                u['absent_frames'].append(fr)
                continue
            u['frames'].append(fr)
            u['verdicts'][r['verdict']] += 1
            u['parts'] = max(u['parts'], r['parts'])
            if u['worst'] is None or RANK[r['verdict']] > RANK[u['worst']]:
                u['worst'] = r['verdict']
                u['reason'] = r['reason']
    for u in by.values():
        if u['worst'] is None:
            u['worst'], u['reason'] = 'stream', 'no main-pass colour draw in any analysed frame (only rim / shadow passes): never shown to be ours'
    return [by[k] for k in sorted(by)]


def render_union(rows, title):
    L = [f'### {title}', '', '| object | name | parts | frames judged | verdicts per frame | worst | why |', '|---|---|---|---|---|---|---|']
    for r in rows:
        nm = r['label'] or ('fits: ' + ' / '.join(r['fits']) if r['fits'] else '?')
        L.append(f"| {r['object']} | {nm} | {r['parts']} | {len(r['frames'])} | " + (', '.join(f'{k} {n}' for k, n in sorted(r['verdicts'].items())) or '-') + f" | **{r['worst'].upper()}** | {r['reason'] or '-'} |")
    c = collections.Counter(r['worst'] for r in rows)
    L += ['', f"{len(rows)} wrestler objects over the frames: " + ', '.join(f'{c.get(k, 0)} {k}' for k in ('ours', 'partial', 'refused', 'stream'))]
    return '\n'.join(L)


def pick_bodies(ids):
    """({character id: (name, body part count)}, [ids with no atlas body]) from the drawables atlas: the default attire's body model (entry p0/0) mesh count.
    Review S1: an id the atlas does not know, or with body 0, is returned in the second list and FAILS the run (it used to be dropped silently)."""
    import roster_materials as RM
    atlas = RM.main_checkout() / 'out' / 'atlas' / 'atlas.json'
    if not atlas.exists():
        return {}, list(ids)
    names = {e['id']: e.get('name') for e in json.loads((HERE / 'db' / 'ids' / 'characters.json').read_text(encoding='utf-8'))['entries']}
    with open(atlas, encoding='utf-8', errors='replace') as fh:
        t = RM.scan(fh, set(ids))
    out, missing = {}, []
    for cid in ids:
        body = sum(t[cid]['entries'].get('p0/0', {}).values()) if cid in t else 0
        if body:
            out[cid] = (names.get(cid, str(cid)), body)
        else:
            missing.append(cid)
    return out, missing


def check_picks(urows, bodies, missing=()):
    """Review B2. A pick is only 'an object of its body size is present and ours' [L]: sizes collide (Sin Cara 10 = the referee 10, Andre 14 = other 14-part bodies, Cena 27 = Rock 27, Cole 12). Two
    picks with the same count need two distinct objects. Identification is the BODY list (bodies_of / check_bodies), not this. -> (lines, failures, used objects)."""
    used, lines, bad = set(), [], []
    for cid in missing:
        lines.append(f'PICK {cid}: no body in the drawables atlas (unknown id or entry p0/0 empty): cannot be checked')
        bad.append(f'pick {cid}: no atlas body')
    for cid, (name, n) in sorted(bodies.items()):
        cand = [r for r in urows if r['parts'] == n and r['object'] not in used]
        if not cand:
            lines.append(f'PICK {cid} {name} ({n} parts): NOT FOUND in any analysed frame (off screen in all of them, or a different attire / size)')
            bad.append(f'{name}: not found')
            continue
        r = sorted(cand, key=lambda x: (RANK[x['worst']], -len(x['frames'])))[0]
        used.add(r['object'])
        ok = r['worst'] == 'ours'
        lines.append(f"PICK {cid} {name} ({n} parts): an object of that size {r['object']} {'OURS' if ok else r['worst'].upper()} in {r['verdicts'].get('ours', 0)} of {len(r['frames'])} frames [L]" + ('' if ok else f" ({r['reason']})"))
        if not ok:
            bad.append(f'{name}: {r["worst"]}')
    return lines, bad, used


def check_everyone(urows, allow):
    """Review B1. THE BAR is: every wrestler object in the analysed frames is OURS (pieces, unpicked bodies, the referee), not only the picks. -> failures. `allow` = addresses excused by --allow."""
    return [f"{r['object']} ({r['label'] or str(r['parts']) + ' parts'}): {r['worst'].upper()} ({r['reason'] or 'no reason given'}) - a wrestler object that is not ours" for r in urows
            if r['worst'] != 'ours' and int(r['object'], 16) not in allow]


REFEREE_PARTS = 10   # the referee's default body model (ch255 p0/0 = 10 meshes, drawables atlas): not a CAW candidate


def check_caw(urows, used, min_parts):
    """--caw (leg 3b): the created wrestler = the BODY object no pick explains (>= min_parts parts, not the referee's size, not a pick's object). At least one must exist and EVERY such object must be ours.
    -> (lines, failures). A lead [L]: 'the unexplained body', not an identified CAW. Review B3: it is only meaningful together with check_seat / forbid-bodies (a stock wrestler left in seat 0 is also an unexplained body)."""
    rest = [r for r in urows if r['object'] not in used and r['parts'] >= min_parts and r['parts'] != REFEREE_PARTS and not r['label']]
    if not rest:
        return ['CAW: no unexplained body object (>= %d parts) in any analysed frame: the created wrestler was not drawn, or is smaller than --caw-min-parts' % min_parts], ['caw: not found']
    lines, bad = [], []
    for r in rest:
        ok = r['worst'] == 'ours'
        lines.append(f"CAW? {r['object']} ({r['parts']} parts): {'OURS' if ok else r['worst'].upper()} in {r['verdicts'].get('ours', 0)} of {len(r['frames'])} frames" + ('' if ok else f" ({r['reason']})"))
        if not ok:
            bad.append(f"caw {r['object']}: {r['worst']}")
    return lines, bad


# ---------------------------------------------------------------- identification: the hook's own BODY list (review B2 / B3)
import re
BODY_RE = re.compile(r'^BODY ch(\d+) attire (\d+) entry (\d+) nodes (\d+)', re.M)
SEEN_RE = re.compile(r'objects seen (\d+) joined (\d+)')


def bodies_of(cap, game, frames, timeout, exe=None):
    """Runs tools\\wrestler\\wrestler_ours_check.exe (the SHIPPED decision core, CPU) over the frames and returns ({(character, attire)}, objects_seen, objects_joined, text).
    Its BODY ch%03d attire %d lines are the roster entries the core JOINED to stream objects: that is the identification (which character's model), not a size."""
    import leftovers as LO
    main_co = LO.main_checkout()
    exe = Path(exe) if exe else next((c for c in (HERE / 'out' / 'tools' / 'wrestler_ours_check.exe', main_co / 'out' / 'tools' / 'wrestler_ours_check.exe') if c.exists()), None)
    if not exe:
        fail('no wrestler_ours_check.exe (out\\tools): build it (tools\\wrestler) or pass --no-bodies; picks cannot be identified without it')
    lo, hi = min(frames), max(frames)
    p = subprocess.run([str(exe), '--capture', str(cap), '--game', str(game), '--frames', f'{max(lo - 2, 0)}-{hi}'], capture_output=True, text=True, timeout=timeout, errors='replace')
    text = p.stdout + p.stderr
    if p.returncode != 0:
        fail(f'wrestler_ours_check exit {p.returncode}: ' + text[-600:])
    m = SEEN_RE.search(text)
    return {(int(c), int(a)) for c, a, _e, _n in BODY_RE.findall(text)}, int(m.group(1)) if m else -1, int(m.group(2)) if m else -1, text


def check_bodies(bodies, pick_ids, forbid):
    """-> (lines, failures). Every pick must have `chNNN attire 0` in the BODY list of the same capture; every forbidden character must have no body at all."""
    lines, bad = [], []
    for cid in pick_ids:
        have = sorted(a for c, a in bodies if c == cid)
        if 0 in have:
            lines.append(f'BODY ch{cid:03d} attire 0: joined (identified)')
        else:
            lines.append(f'BODY ch{cid:03d} attire 0: NOT in the hook\'s BODY list' + (f' (attires joined: {have})' if have else ''))
            bad.append(f'ch{cid:03d} attire 0 not joined')
    for cid in forbid:
        if any(c == cid for c, _a in bodies):
            lines.append(f'BODY ch{cid:03d}: joined, but forbidden here (the stock wrestler was not replaced)')
            bad.append(f'ch{cid:03d} is in the match')
        else:
            lines.append(f'BODY ch{cid:03d}: absent, as required')
    return lines, bad


def check_launch_log(path, seat_expect):
    """Review B3. The run log must say `launch '<name>': participant SEAT = id ID` for every expected seat and no `launch participant '...' skipped` (scenario.h skips an id outside 0-499 and
    leaves the captured wrestler in the seat). seat_expect: {seat: id}. -> (lines, failures)."""
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    seated = {int(s): int(i) for s, i in re.findall(r"launch '[^']*': participant (\d+) = id (\d+)", text)}
    skipped = re.findall(r"launch participant '([^']*)' skipped", text)
    lines, bad = [], []
    for seat, cid in sorted(seat_expect.items()):
        if seated.get(seat) == cid:
            lines.append(f'LOG participant {seat} = id {cid}: seated')
        else:
            lines.append(f'LOG participant {seat} = id {cid}: NOT seated (log says {seated.get(seat, "no such line")})')
            bad.append(f'seat {seat}: id {cid} not seated')
    for s in skipped:
        lines.append(f"LOG participant '{s}' SKIPPED")
        bad.append(f"participant '{s}' skipped")
    return lines, bad


def complete_tables(ops, incomplete, arm_mode):
    """ops: {(arm, frame): path}. Thin frames of the ours arm with unlearned lite draws are left out (said); none left in the ours arm = exit 3. -> (tables, stop code or 0)."""
    tables = [(arm, fr, str(p)) for (arm, fr), p in ops.items() if not (arm == 'ours' and fr in incomplete)]
    if incomplete:
        print(f'[roster_proof] frames {sorted(incomplete)} are INCOMPLETE (thin lite draws not learned): left out of the verdict', flush=True)
    if not any(t[0] == 'ours' for t in tables) and arm_mode in ('ours', 'both'):
        print('[roster_proof] no complete frame in the ours arm: not a measurement (exit 3)', flush=True)
        return tables, 3
    return tables, 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input', nargs='?')
    ap.add_argument('--ops', action='append', help='an ops json (fview --ops --json) of the OURS arm instead of a capture; repeat for several')
    ap.add_argument('--off-ops', action='append', help='an ops json of the hook-OFF arm (the plant); repeat for several. Review N3: the arm is named here, never guessed from a file name')
    ap.add_argument('--ref-ops', help='an ops json of another arm of the same frame, for the wrestler object set')
    ap.add_argument('--frame', default='last')
    ap.add_argument('--frames', help='capture mode: several frames, comma separated (a wrestler off screen in one is on in another); the verdict per object is its WORST over them')
    ap.add_argument('--arm', default='ours', choices=('ours', 'off', 'both'), help='capture mode: ours = --wrestlers-ours; off = no flag (the plant arm); both')
    ap.add_argument('--label', action='append', default=[], help='0xADDR=NAME')
    ap.add_argument('--expect-ours', default='')
    ap.add_argument('--expect-stream', default='')
    ap.add_argument('--allow', default='', help='0xADDR,... wrestler objects excused from "every wrestler object is ours" (say why in the report)')
    ap.add_argument('--picks', default='', help='character ids (db\\ids): an object of each body size must be present and ours [L]; each must also be in the hook\'s BODY list as chNNN attire 0 (identification)')
    ap.add_argument('--forbid-bodies', default='', help='character ids that must NOT be in the BODY list (leg 3b: 324 = Bryan, the stock wrestler the CAW replaces)')
    ap.add_argument('--no-bodies', action='store_true', help='skip the BODY-list identification (sizes only: NOT an identification)')
    ap.add_argument('--run-log', help='the run\'s full log (out\\build\\win-amd64-release\\logs\\wwe2k14_N.log): for --expect-seat')
    ap.add_argument('--expect-seat', default='', help='SEAT=ID,...: the log must say "participant SEAT = id ID" and no "skipped" (leg 3b: 0=<CAW id>)')
    ap.add_argument('--caw', action='store_true', help='leg 3b: the body object no --picks explains (not the referee) must exist and be ours')
    ap.add_argument('--caw-min-parts', type=int, default=12, help='--caw: the smallest part count of a body (pieces are smaller; Michael Cole has 12: raise it if he is in the match)')
    ap.add_argument('--fview')
    ap.add_argument('--allow-stale', action='store_true', help='run although fview.exe is older than the renderer / fview sources (tools\\fview\\fview_exe.py)')
    ap.add_argument('--out')
    ap.add_argument('--json')
    ap.add_argument('--plant', default='')
    ap.add_argument('--per-frame', action='store_true', help="also print every frame's table (several frames)")
    ap.add_argument('--timeout', type=int, default=1500)
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.input and not a.ops and not a.off_ops:
        ap.error('give a case zip / capture folder, or --ops / --off-ops')
    labels = {}
    for s in a.label:
        k, _, v = s.partition('=')
        labels[parse_addr(k)] = v
    incomplete = set()
    notes = []
    cap = game = None
    frames = []
    if a.ops or a.off_ops:
        tables = [('ours', Path(p).stem, p) for p in (a.ops or [])] + [('off', Path(p).stem, p) for p in (a.off_ops or [])]
    else:
        ops, frames, incomplete, has_rec, thin, cap, game = main_capture(a)
        # a thin frame with lite draws nobody learned is NOT a measurement (lane 143): it is left out of the verdict and said; no complete frame left = exit 3
        tables, stop = complete_tables(ops, incomplete, a.arm)
        if incomplete:
            notes.append(f'incomplete frames left out: {sorted(incomplete)}')
        if stop:
            return stop
    fits = atlas_fits()
    ref = load_draws(a.ref_ops) if a.ref_ops else None
    code = 0
    exp_o = [parse_addr(x) for x in a.expect_ours.split(',') if x]
    exp_s = [parse_addr(x) for x in a.expect_stream.split(',') if x]
    allow = {parse_addr(x) for x in a.allow.split(',') if x}
    # review N1: the off arm takes the ours arm's first frame as the object-set reference, so every ours object must be there as a row
    ours_first = None
    for name, frame, path in tables:
        if name == 'ours' and ours_first is None:
            ours_first = load_draws(path)
    per = collections.defaultdict(dict)   # arm -> {frame: rows}
    for name, frame, path in tables:
        draws = plant(load_draws(path), a.plant)
        r_ref = ref if ref is not None else (ours_first if name == 'off' else None)
        rows = judge(draws, wrestler_objects(draws, r_ref), labels, fits)
        per[name][frame] = rows
        if a.per_frame or sum(1 for t in tables if t[0] == name) == 1:
            print(render(rows, f'{name} (frame {frame}{", plant " + a.plant if a.plant else ""})'))
            print()
        if not rows:
            print(f'[roster_proof] {name} frame {frame}: NO wrestler objects in this table (no wrestlers_ours draw, no rim-map caster): not a measurement, not a pass', flush=True)
            code = max(code, 3)
    result = {}
    ours_union = None
    for name, fr_rows in per.items():
        u = union(fr_rows)
        result[name] = u
        if len(fr_rows) > 1:
            print(render_union(u, f'{name}: worst verdict over frames {",".join(str(f) for f in sorted(fr_rows, key=str))}'))
            print()
        urows = [{'object': r['object'], 'label': r['label'], 'verdict': r['worst'], 'reason': r['reason'], 'parts_stream': []} for r in u]
        if name == 'off':
            # the plant arm (--wrestlers-ours off): every wrestler must read stream; the same addresses the ours arm is expected to own are the ones checked
            bad = check_expect(urows, [], exp_s or exp_o) + [f"{r['object']}: read {r['worst'].upper()} with the hook off" for r in u if r['worst'] in ('ours', 'partial')]
            if 'ours' in result:   # N1: every object of the ours arm's first frame must be a row here, reading STREAM
                have = {r['object'] for r in u}
                bad += [f"{r['object']}: in the ours arm but missing from the off arm" for r in result['ours'] if r['object'] not in have and r['frames']]
        else:
            ours_union = u
            bad = check_expect(urows, exp_o, [])
            bad += check_everyone(u, allow)   # review B1
            pick_ids = [int(x) for x in a.picks.split(',') if x]
            used = set()
            if pick_ids:
                bodies, missing = pick_bodies(pick_ids)
                lines, pbad, used = check_picks(u, bodies, missing)
                for ln in lines:
                    print(ln)
                bad += pbad
            if a.caw:
                clines, cbad = check_caw(u, used, a.caw_min_parts)
                for ln in clines:
                    print(ln)
                bad += cbad
            forbid = [int(x) for x in a.forbid_bodies.split(',') if x]
            if (pick_ids or forbid) and not a.no_bodies:
                if cap is None:
                    print('[roster_proof] --picks / --forbid-bodies need a capture (the BODY list is read from it); --ops cannot identify', flush=True)
                    bad.append('no capture for the BODY list')
                else:
                    bset, seen, joined, _t = bodies_of(cap, game, frames, a.timeout)
                    blines, bbad = check_bodies(bset, pick_ids, forbid)
                    for ln in blines:
                        print(ln)
                    bad += bbad
                    n_ours = sum(1 for r in u if r['worst'] == 'ours')
                    print(f'[roster_proof] hook census (wrestler_ours_check over the frames): objects seen {seen}, joined {joined}; this tool: {len(u)} wrestler objects, {n_ours} ours')
                    if seen >= 0 and seen < n_ours:   # review N2: the core cannot have replaced more objects than it saw (pieces are linked, not joined: joined is printed, not compared)
                        bad.append(f'census disagrees: {n_ours} ours objects but the core saw only {seen}')
            if a.expect_seat:
                if not a.run_log:
                    bad.append('--expect-seat needs --run-log')
                else:
                    seat = {int(s): int(i) for s, i in (x.split('=') for x in a.expect_seat.split(',') if x)}
                    llines, lbad = check_launch_log(a.run_log, seat)
                    for ln in llines:
                        print(ln)
                    bad += lbad
        for b in bad:
            print('[roster_proof] EXPECT FAILED: ' + b, flush=True)
        if bad:
            code = max(code, 1)
    if a.json:
        Path(a.json).write_text(json.dumps({'notes': notes, 'incomplete': sorted(incomplete), 'frames': {k: sorted(map(str, v)) for k, v in per.items()}, 'union': {k: [dict(r, verdicts=dict(r['verdicts'])) for r in v] for k, v in result.items()}}, indent=1), encoding='utf-8')
    print(f'[roster_proof] exit {code}')
    return code


if __name__ == '__main__':
    sys.exit(main())
