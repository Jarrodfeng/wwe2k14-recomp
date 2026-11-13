r"""Lane 183 (S101): the resolve-copy census. Offline, 0 game runs. Candidate C of docs\renderer\SKIP_GAME_WORK_MAP.md section 13.2 / 14.

The 360 resolves a render target into a texture (43-94 copies a frame, an OP_COPY of the capture). On a tile-based GPU each copy is a whole read + write of the image. For every copy of a
capture this tool answers: WHICH later draws read its destination, until the destination is rewritten, and what fate each reader had (ours = a promotion class draws it from our own images;
stream = the game's draw replayed). A copy whose readers are ALL ours AND whose source stays untouched until the last reader is an elision candidate; a copy with one game-replayed
reader is blocked by that reader's class.

  py -I tools\skip\copy_readers.py CAPTURE.zip [--frames A:B] [--run-fview] [--json OUT] [--list] [-q]
  py -I tools\skip\copy_readers.py --selftest

Readers come from the capture alone (a draw's texture slot images, a later copy's source, the Present), joined by IMAGE ID (the capture's own identity of a texture: no address
guessing). A reader is attributed to every still-open copy of that image (a partial-rect copy keeps the older one open). A copy ends when a later copy writes a rect that covers its
destination rect, or when a draw / clear renders to the destination image. Fates need a replay: `--run-fview` runs fview headless once per frame of the window
(`--frame F --ops --json`, every promotion the capture allows, tools\census\leftovers.py\'s list; each run replays F frames first) into out\s101\copy_readers\NAME.
(The `--thin-run` trace.csv is NOT used: it keeps only the main pass's model parts.) Without fates every reader is "unknown" and no copy is judged; the class of the reader
(DrawClass + effect) is still printed.

A copy's CLASS is read from its source: the last pass that drew into the source image (size, pass site, top vertex effect) and the destination size, mapped by `name_copy` below (a [L] naming
by sizes and effects, printed beside the raw label so the mapping can be judged).
Numbers only; nothing from the game is written. --json is for under out\."""
import argparse
import collections
import csv
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'crowd'))
import cards_census as cc  # noqa: E402
import dead_passes as dp  # noqa: E402

FATE_NAMES = ["stream", "replaced", "crowd_ours", "arena_ours", "wrestlers_ours", "crowd_injected", "ring_ours", "ui_ours", "flash_ours", "particle_run", "props_ours",
              "shadow_casters_ours", "ui_text_ours", "screens_ours", "post_ours", "rim_ours", "circles_ours", "movie_ours", "small_passes_ours", "plane_shadow_ours", "ui_quads_ours",
              "rim_chain_ours", "caw_ours"]  # = src\fview\main.cpp kFateNames (asserted there against the highest fate constant)
DRAW_CLASS = ['unnamed', 'model_part', 'crowd', 'sprite', 'text', 'movie', 'particle', 'shadow', 'post', 'clear', 'other']  # src\renderer_adapter\names.h DrawClass
PLANTS = ('extra_reader', 'drop_reader', 'fate_all_stream', 'src_dirty', 'node_reader')
UNKNOWN = '(no fates)'
ANY_READER = False   # --any-reader: [L] upper bound, a game-replayed reader no longer blocks (it binds the aliased source through Map() like any draw); the Present, blind and unjudged readers still do
LAST_BLIND = collections.Counter()   # effect -> draws with an image-0 slot in the last scan


class Fates:
    """{(frame, op): fate name} from `fview --frame F --ops --json` files (every draw of that frame with its fate; particle run heads relabelled by leftovers.py's rule). A frame with no file has
    UNKNOWN fates: a reader in it is 'unknown' and no copy that has one is judged."""

    def __init__(self):
        self.m, self.frames = {}, set()

    def add_ops_json(self, frame, path):
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'census'))
        import leftovers as lo
        draws, _n, _k = lo.load_draws(path)
        draws = lo.mark_particle_heads(draws)
        for d in draws:
            fate = d['fate']
            if fate == 'stream' and str(d.get('outcome', '')).startswith('left out'):   # the stream's draw is not replayed and no named class owns it: a hook dropped it (shadows_ours, the settings): its work is ours
                fate = 'left_out'
            self.m[(frame, int(d['op']))] = fate
        self.frames.add(frame)

    def get(self, frame, op):
        if frame not in self.frames:
            return None
        return self.m.get((frame, op), 'stream')


def full_rect(img_wh, r):
    return img_wh is not None and r[0] <= 0 and r[1] <= 0 and r[2] >= img_wh[0] and r[3] >= img_wh[1]


def covers(a, b):
    """rect a covers rect b."""
    return a[0] <= b[0] and a[1] <= b[1] and a[2] >= b[2] and a[3] >= b[3]


def name_copy(src_label, dst_size, src_size):
    """[L] the destination's purpose, from the source pass (size, pass site, effects) as the captures show them (lane 183, section 14.2: Check B, wm30, Bryan, the match). A name is a reading of the
    sizes and vertex effects, not a proof: anything else is 'other:WxH'. The raw label is printed with --list so the naming can be judged."""
    size, _img, site, effs = src_label
    eff = effs.lower()
    wh = '%dx%d' % tuple(src_size) if src_size else '?'
    if 'yplaneshadowmap' in eff or 'yblur/plane_' in eff:
        return 'plane shadow chain'
    if 'yauddefault' in eff and src_size == (64, 64):
        return 'crowd bake'
    if 'yprelight' in eff:
        return 'yPreLight bake'
    if 'yliquidmap' in eff:
        return 'sweat map'
    if 'yrimlightmap' in eff or (src_size == (512, 256) and 'yblur' in eff):
        return 'rim chain'
    if 'yshadowmap' in eff or 'ycircleshadow' in eff:
        return 'shadow map'
    if 'yblur/masked' in eff and src_size == (512, 512):
        return 'shadow blur'
    if 'ysprite/tint_constant' in eff and src_size == (512, 512):
        return 'crowd light page'
    if 'ytonemap' in eff and src_size and src_size[0] <= 320:
        return 'post chain'
    if src_size == (1280, 720) or (src_size and src_size[0] >= 1280):
        return 'scene colour resolve'
    return 'other:' + wh


def scan(cap, frames, fates, plant=None):
    """-> list of copy records (closed or open at the end)."""
    records, open_by_img = [], collections.defaultdict(list)
    writer_label = {}   # image -> (size, site, effects) of the last pass that drew into it
    seq = 0
    state = {'planted': False, 'last_copy': None}
    LAST_BLIND.clear()

    def do_draw(n, fi, f, labels, origins, seq, force_fate=None):
        d = n['d']
        o = origins[d] if origins is not None else None
        klass = DRAW_CLASS[int(o['klass'])] if (o is not None and int(o['op']) and int(o['klass']) < len(DRAW_CLASS)) else 'untagged'
        site = '%08X' % int(o['pass_site']) if o is not None else '0'
        call = '%08X' % int(o['call_site']) if o is not None else '0'
        eff = str(cap.eff(int(f.draws['vs_hash'][d])))
        fate = force_fate or fates.get(fi, n['k'])   # None unknown / 'stream' / an ours name
        if any(img == 0 for img in n['r']):   # a slot with no image (the S100 nulled-stage fault): it could read ANY copy, so every open copy gets a blind reader and is never judged
            LAST_BLIND[eff] += 1
            for recs in open_by_img.values():
                for rec in recs:
                    rec['readers'].append((seq, fi, n['k'], 'blind', eff, site, fate, int(call, 16)))
        for img in n['r']:
            for rec in open_by_img.get(img, []):
                rec['readers'].append((seq, fi, n['k'], klass, eff, site, fate, int(call, 16)))
        for w in n['w']:
            writer_label[w] = labels[n['pass']] if 0 <= n['pass'] < len(labels) else writer_label.get(w)

    for fi in frames:
        f = cap.frame(fi)
        nodes, passes = dp.frame_nodes(cap, f)
        labels = dp.classify(cap, f, passes)
        copies_t = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
        origins = f.origins if f.has_origins else None
        for n in nodes:
            seq += 1
            kind = n['kind']
            if kind == 'copy':
                c = copies_t[int(f.ops[n['k']][1])]
                src, dst = int(c['s']['image']), int(c['d']['image'])
                dr = [int(x) for x in c['dr']]
                for rec in list(open_by_img[dst]):   # an older copy to the same image whose rect this one covers ends here
                    if covers(dr, rec['dr']):
                        rec['end'] = 'rewritten by copy'
                        open_by_img[dst].remove(rec)
                for rec in open_by_img.get(src, []):   # a copy of a copy reads it
                    rec['readers'].append((seq, fi, n['k'], 'copy', 'copy', 'copy', None, 0))
                for recs in open_by_img.values():   # the source of an older copy that this copy overwrites: the source is no longer clean
                    for rec in recs:
                        if rec['src'] == dst and rec['src_dirty'] is None:
                            rec['src_dirty'] = seq
                pl = None
                lab = writer_label.get(src, (cap.wh(src), src, '00000000', ''))
                rec = {'id': len(records), 'frame': fi, 'k': n['k'], 'src': src, 'dst': dst, 'dr': dr, 'src_wh': cap.wh(src), 'dst_wh': cap.wh(dst), 'label': lab,
                       'src_fmt': int(cap.imgs[src]['format']) if src in cap.imgs else -1, 'dst_fmt': int(cap.imgs[dst]['format']) if dst in cap.imgs else -1,
                       'src_kind': int(cap.imgs[src]['kind']) if src in cap.imgs else -1, 'same': bool(cap.wh(src) == cap.wh(dst) and [int(x) for x in c['sr']] == dr and full_rect(cap.wh(dst), dr) and (int(cap.imgs[src]['format']) == int(cap.imgs[dst]['format']) if src in cap.imgs and dst in cap.imgs else False)), 'readers': [], 'src_dirty': None, 'end': None, 'seq': seq, 'pass_label': pl, 'area': max(0, dr[2] - dr[0]) * max(0, dr[3] - dr[1])}
                records.append(rec)
                state['last_copy'] = rec
                open_by_img[dst].append(rec)
                continue
            if kind == 'present':
                for img in n['r']:
                    for rec in open_by_img.get(img, []):
                        rec['readers'].append((seq, fi, n['k'], 'present', 'present', 'present', None, 0))
                continue
            # draws and clears: they write images (the pass targets) and a draw reads its slot images
            for w in n['w']:
                for rec in open_by_img.get(w, []):   # rendered into: the copy's content is gone
                    rec['end'] = 'rendered into'
                open_by_img[w] = []
                for recs in open_by_img.values():
                    for rec in recs:
                        if rec['src'] == w and rec['src_dirty'] is None:
                            rec['src_dirty'] = seq
            if kind == 'draw':
                do_draw(n, fi, f, labels, origins, seq)
                if plant == 'node_reader' and not state['planted'] and state['last_copy'] is not None and state['last_copy']['dst_wh'] == (64, 64) and state['last_copy']['src_wh'] == (64, 64):
                    state['planted'] = True   # PLANT: one extra game-replayed draw (fate stream) that samples the first 64x64 copy's destination; the same draw path as a real one
                    syn = dict(n, r=[state['last_copy']['dst']], w=[0xFFFFFFF0], k=n['k'])
                    do_draw(syn, fi, f, labels, origins, seq, force_fate='stream')
            elif kind == 'clear':
                for w in n['w']:
                    writer_label.setdefault(w, labels[n['pass']])
    for lst in open_by_img.values():
        for rec in lst:
            rec['end'] = rec['end'] or 'open at the end'
    return records


def apply_plant(records, plant):
    """Selftest plants: mangle the records after the scan; each must flip the verdict of a named check."""
    if plant == 'extra_reader':   # an elidable copy gets one game-replayed reader
        for r in records:
            if verdict(r)[0] == 'elidable':
                r['readers'].append((10 ** 9, r['frame'], 0, 'model_part', 'planted', '00000000', 'stream', 0))
                return r['id']
    if plant == 'drop_reader':   # a blocked copy loses its stream reader(s)
        for r in records:
            if verdict(r)[0] == 'blocked':
                r['readers'] = [x for x in r['readers'] if x[6] not in ('stream', None)]
                return r['id']
    if plant == 'fate_all_stream':
        for r in records:
            r['readers'] = [(a, b, c, d, e, f, 'stream' if g else g, h) for (a, b, c, d, e, f, g, h) in r['readers']]
        return -1
    if plant == 'src_dirty':   # the source is written before the last reader
        for r in records:
            if verdict(r)[0] == 'elidable':
                r['src_dirty'] = r['seq'] + 1
                return r['id']
    return None


def reader_kind(rd):
    """-> 'ours:<fate>' / 'stream:<class>:<effect>' / 'copy' / 'present' / 'unknown:<class>'."""
    seq, fi, k, klass, eff, site, fate, call = rd
    if klass in ('copy', 'present'):
        return klass
    if klass == 'blind':
        return 'blind:' + eff
    if fate is None:
        return 'unknown:' + klass
    if fate != 'stream':
        return 'ours:' + str(fate)
    return 'stream:%s:%s' % (klass, eff)


def verdict(rec):
    """-> (state, why). state in {'no reader', 'elidable', 'blocked', 'src dirty', 'unknown', 'unresolved'}."""
    kinds = [reader_kind(r) for r in rec['readers']]
    if not kinds:
        return 'no reader', ''
    if any(k.startswith('blind') for k in kinds):
        return 'unresolved', 'a draw with an image-0 slot in the window'
    if any(k.startswith('unknown') for k in kinds):
        return 'unknown', ''
    bad = sorted({k for k in kinds if not k.startswith('ours:') and not (ANY_READER and (k.startswith('stream:') or k == 'copy'))})
    if bad:
        return 'blocked', bad[0]
    last = max(r[0] for r in rec['readers'])
    if rec['src_dirty'] is not None and rec['src_dirty'] <= last:   # a reader that renders INTO the source is a feedback loop, not a clean read
        return 'src dirty', 'source rewritten before the last reader'
    return 'elidable', ''


def run_fview(cap_path, frames, out_dir, quiet=False):
    """Headless fview, one `--frame F --ops --json` run per frame of the window (each replays F frames first), with every promotion the capture allows (leftovers.py's list) -> (Fates, promote)."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'census'))
    import leftovers as lo
    from pathlib import Path
    main_co = lo.main_checkout()
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))   # lane 218: the shared locator; refuses an exe older than the renderer sources
    import fview_exe as fe
    fview = fe.find_or_exit(root=ROOT, kind=None)
    os.makedirs(out_dir, exist_ok=True)
    capdir = lo.open_input(Path(cap_path), Path(out_dir))
    has_records = (capdir / 'scene').is_dir() and any((capdir / 'scene').glob('frame_*.rsc'))
    promote = [p for p in lo.ALL_PROMOTIONS if (has_records or not lo.PROMOTIONS[p][1]) and p not in ('caw', 'floor')]
    flags = [lo.PROMOTIONS[p][0] for p in promote] + ['--shadows-ours', '--game', os.path.join(str(main_co), 'game_data', 'extracted')]
    thin = lo.thin_frames(capdir)
    if thin:
        flags += ['--thin', ','.join(lo.thin_arg(promote))]
    fates, log = Fates(), []
    for f in frames:
        oj = os.path.join(out_dir, 'ops_f%d.json' % f)
        if not os.path.isfile(oj):
            cmd = [fview, '--capture', str(capdir), '--frame', str(f), '--root', ROOT, '--ops', '--json', oj] + flags
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, errors='replace')
            log.append(p.stdout + p.stderr)
            if p.returncode != 0 or not os.path.isfile(oj):
                sys.exit('FAIL fview exit %d frame %d\n%s' % (p.returncode, f, (p.stdout + p.stderr)[-1500:]))
        fates.add_ops_json(f, oj)
    with open(os.path.join(out_dir, 'fview.log'), 'a') as fh:
        fh.write('\n'.join(log))
    return fates, promote


def report(cap, records, frames, quiet=False, list_all=False, out=None):
    """Counts only the copies that started in the window with at least one frame of room after them (the last frame's lifetimes are cut)."""
    nfr = max(1, len(frames) - 1)
    counted = [r for r in records if r['frame'] != frames[-1]]
    by = collections.OrderedDict()
    for r in counted:
        nm = name_copy(r['label'], r['dst_wh'], r['src_wh'])
        c = by.setdefault(nm, {'n': 0, 'no reader': 0, 'elidable': 0, 'blocked': 0, 'src dirty': 0, 'unknown': 0, 'unresolved': 0, 'same': 0, 'area': 0, 'area_el': 0, 'area_dead': 0, 'block_by': collections.Counter(), 'readers': collections.Counter(), 'labels': collections.Counter()})
        v, why = verdict(r)
        c['n'] += 1
        c[v] += 1
        c['area'] += r['area']
        c['area_el'] += r['area'] if v == 'elidable' else 0
        c['area_dead'] += r['area'] if v == 'no reader' else 0
        if v == 'elidable' and r['same']:
            c['same'] += 1
        if v == 'blocked':
            c['block_by'][why] += 1
        for rd in {reader_kind(x) for x in r['readers']}:
            c['readers'][rd] += 1
        c['labels'][(str(r['src_wh']), str(r['dst_wh']), r['label'][2], r['label'][3][:40])] += 1
    tot = collections.Counter()
    solo = collections.Counter()   # blocker kind -> copies that ONLY this kind blocks (they become elidable if that class were ours)
    ours_by = collections.Counter()   # reader kind -> elidable copies it reads
    tot_same = collections.Counter()   # elidable copies: True = full-rect, same size, same format (the destination IS the source's pixels); False = a sub-rect / resize / reformat
    for r in counted:
        v, why = verdict(r)
        kinds = {reader_kind(x) for x in r['readers']}
        bad = {k for k in kinds if not k.startswith('ours:')}
        if v == 'blocked' and len(bad) == 1:
            solo[next(iter(bad))] += 1
        if v == 'elidable':
            tot_same[r['same']] += 1
            for k in kinds:
                ours_by[k] += 1
    print('copy readers: %s, frames %d..%d (%d counted frames), %d copies (%.1f / frame)' % (os.path.basename(cap.path), frames[0], frames[-1], nfr, len(counted), len(counted) / nfr))
    print('%-16s %7s %7s %8s %9s %8s %9s %8s  %s' % ('class', 'per/frm', 'copies', 'noreader', 'elidable', 'blocked', 'src dirty', 'unjudged', 'top blocker (copies)'))
    out_rows = []
    for nm, c in sorted(by.items(), key=lambda kv: -kv[1]['n']):
        top = ', '.join('%s (%d)' % kv for kv in c['block_by'].most_common(2))
        print('%-16s %7.1f %7d %8d %9d %8d %9d %8d  %s' % (nm, c['n'] / nfr, c['n'], c['no reader'], c['elidable'], c['blocked'], c['src dirty'], c['unknown'] + c['unresolved'], top[:110]))
        for k in ('n', 'no reader', 'elidable', 'blocked', 'src dirty', 'unknown', 'unresolved'):
            tot[k] += c[k]
        out_rows.append({'class': nm, 'copies': c['n'], 'per_frame': round(c['n'] / nfr, 2), 'no_reader': c['no reader'], 'elidable': c['elidable'], 'elidable_same': c['same'], 'mpix_per_frame': round(c['area'] / 1e6 / nfr, 3), 'mpix_elidable': round(c['area_el'] / 1e6 / nfr, 3), 'mpix_dead': round(c['area_dead'] / 1e6 / nfr, 3), 'blocked': c['blocked'], 'src_dirty': c['src dirty'],
                         'unknown': c['unknown'], 'unresolved': c['unresolved'], 'block_by': dict(c['block_by'].most_common(6)), 'readers': dict(c['readers'].most_common(8)), 'labels': [list(k) + [v] for k, v in c['labels'].most_common(3)]})
    ar = sum(c['area'] for c in by.values()) / 1e6 / nfr
    ae = sum(c['area_el'] for c in by.values()) / 1e6 / nfr
    ad = sum(c['area_dead'] for c in by.values()) / 1e6 / nfr
    print('pixels copied a frame (destination rects): %.2f MPix, of them elidable %.2f, unread (dead) %.2f' % (ar, ae, ad))
    for nm, c in sorted(by.items(), key=lambda kv: -kv[1]['area']):
        print('  %-20s %6.3f MPix/frame, elidable %6.3f, dead %6.3f' % (nm, c['area'] / 1e6 / nfr, c['area_el'] / 1e6 / nfr, c['area_dead'] / 1e6 / nfr))
    print('%-16s %7.1f %7d %8d %9d %8d %9d %8d' % ('ALL', tot['n'] / nfr, tot['n'], tot['no reader'], tot['elidable'], tot['blocked'], tot['src dirty'], tot['unknown'] + tot['unresolved']))
    if LAST_BLIND:
        print('BLIND READERS: %d draws with an image-0 texture slot in the window (%s): every copy open at them is unresolved, not judged' % (sum(LAST_BLIND.values()), ', '.join('%s %d' % kv for kv in LAST_BLIND.most_common(5))))
    print('unblock (copies that become elidable if this reader class were ours): ' + '; '.join('%s %d' % kv for kv in solo.most_common(12)))
    print('elidable copies with source == destination pixels (same size, full rect, same format): %d of %d' % (tot_same[True], tot_same[True] + tot_same[False]))
    print('elidable copies are read by: ' + '; '.join('%s %d' % kv for kv in ours_by.most_common(12)))
    if list_all:
        for nm, c in by.items():
            print('  %s readers: %s' % (nm, dict(c['readers'].most_common(10))))
            print('  %s labels (src wh, dst wh, site, effects): %s' % (nm, c['labels'].most_common(3)))
    res = {'capture': os.path.basename(cap.path), 'frames': [frames[0], frames[-1]], 'copies_per_frame': round(len(counted) / nfr, 2), 'classes': out_rows, 'total': dict(tot), 'unblock': dict(solo.most_common()), 'elidable_same': tot_same[True], 'elidable_readers': dict(ours_by.most_common()), 'blind': dict(LAST_BLIND)}
    if out:
        json.dump(res, open(out, 'w'), indent=1)
    return res


def parse_frames(spec, total):
    if not spec:
        return list(range(total))
    s, e = (spec.split(':') + [''])[:2]
    return list(range(int(s or 0), min(total, int(e or total))))


def selftest():
    """Synthetic records (no capture): the verdict logic, the plants, the covers rule. Exit 1 on any failure."""
    bad = 0

    def check(name, got, want):
        nonlocal bad
        ok = got == want
        bad += 0 if ok else 1
        print(('PASS  ' if ok else 'FAIL  ') + '%s: got %r want %r' % (name, got, want))

    def rec(readers, dirty=None):
        return {'id': 1, 'frame': 0, 'seq': 1, 'readers': readers, 'src_dirty': dirty}
    ours = (5, 0, 1, 'crowd', 'e', '0', 'crowd_ours', 0)
    stream = (6, 0, 2, 'model_part', 'yWrestler/x', '0', 'stream', 0)
    unk = (7, 0, 3, 'model_part', 'e', '0', None, 0)
    check('no readers', verdict(rec([]))[0], 'no reader')
    check('all ours', verdict(rec([ours, ours]))[0], 'elidable')
    check('one stream reader blocks', verdict(rec([ours, stream])), ('blocked', 'stream:model_part:yWrestler/x'))
    check('a copy reader blocks', verdict(rec([ours, (8, 0, 4, 'copy', 'copy', 'copy', None, 0)]))[0], 'blocked')
    check('present blocks', verdict(rec([ours, (8, 0, 4, 'present', 'present', 'present', None, 0)]))[0], 'blocked')
    check('unknown fate', verdict(rec([ours, unk]))[0], 'unknown')
    check('source dirty before the last reader', verdict(rec([ours], dirty=3))[0], 'src dirty')
    check('source dirty after the last reader', verdict(rec([ours], dirty=9))[0], 'elidable')
    check('a reader that renders into the source (dirty == its seq) is a feedback loop', verdict(rec([ours], dirty=5))[0], 'src dirty')
    check('covers', (covers([0, 0, 64, 64], [0, 0, 64, 64]), covers([0, 0, 32, 64], [0, 0, 64, 64])), (True, False))
    rs = [dict(rec([ours]), id=0), dict(rec([ours, stream]), id=1)]
    check('plant extra_reader flips elidable', (apply_plant(rs, 'extra_reader'), verdict(rs[0])[0]), (0, 'blocked'))
    rs = [dict(rec([ours, stream]), id=0)]
    check('plant drop_reader flips blocked', (apply_plant(rs, 'drop_reader'), verdict(rs[0])[0]), (0, 'elidable'))
    rs = [dict(rec([ours]), id=0, seq=1)]
    check('plant src_dirty', (apply_plant(rs, 'src_dirty'), verdict(rs[0])[0]), (0, 'src dirty'))
    rs = [dict(rec([ours]), id=0)]
    apply_plant(rs, 'fate_all_stream')
    check('plant fate_all_stream', verdict(rs[0])[0], 'blocked')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--frames', default='')
    ap.add_argument('--run-fview', action='store_true', help='run fview --thin-run for the fates (headless), into out\\s101\\copy_readers\\NAME')
    ap.add_argument('--json')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--plant', default='')
    ap.add_argument('--any-reader', action='store_true', help='[L] upper bound: game-replayed readers do not block (alias by Map()); only the Present, blind and unjudged ones do')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    global ANY_READER
    ANY_READER = a.any_reader
    if a.selftest:
        return selftest()
    if not a.capture:
        ap.error('capture required')
    cap = cc.Cap(a.capture)
    frames = parse_frames(a.frames, len(cap.frames))
    if a.run_fview:
        name = os.path.basename(os.path.dirname(os.path.abspath(a.capture)))
        fates, promote = run_fview(a.capture, frames, os.path.join(ROOT, 'out', 's101', 'copy_readers', name))
        print('[copy_readers] promotions on in the replay: %s' % ','.join(promote))
    else:
        fates = Fates()
    recs = scan(cap, frames, fates, a.plant)
    if a.plant and a.plant != 'node_reader':
        r = apply_plant(recs, a.plant)
        print('[copy_readers] plant %s applied to copy %s' % (a.plant, r))
    report(cap, recs, frames, list_all=a.list, out=a.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())
