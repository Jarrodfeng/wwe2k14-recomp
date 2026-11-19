#!/usr/bin/env python3
r"""Lane UPL's check (Session 103): the per-upload overhead of the stream-free build. 0 game runs: frame_rig.exe --serve on one capture (default a04_entrance, an entrance with stage screens), run the way up_check.py runs it.

  py -I tools\scene\upl_check.py [--out DIR] [--frames 60] [--repeat 8] [--capture NAME] [--arms after,before,...]

What it answers (docs\research\s103\GCOL.md section 4: 4.16 ms of uploads for 2.2 MB on the Surface, ~3.9 ms of it fixed):
  U3 (batch)  the frame's image uploads in three groups: 3 barriers in all instead of 2 per image upload. Switch wwe2k14_rdr_upload_batch, ON by default in the stream-free build.
  U2 (share)  the screens that show the same picture upload it ONCE and share one image (FrontEnd::UpdateScreens). Switch wwe2k14_rdr_screen_share, ON by default in the stream-free build.
The captures carry no live screen rows, so the rig (WWE_RDR_RIG_SCREEN_ROWS=1) adds one row per stage screen key to every build of the job and WWE_RDR_RIG_SCREEN_FEED gives the pictures: `same` = every screen shows the
same picture of a serial (what the live YuvScreenFeed hands screens of one decoded movie frame), `distinct` = a picture per screen. The job builds the frame once and --repeat - 1 more times with a new serial each: the steady
state of a playing movie. Per repeat the rig prints `up_rep` (image / buffer copies, barriers and bytes ApplyUploads recorded, screen uploads, pictures asked / served with no upload / rebinds, a hash of the drawn picture).
Arms (each on its own frame_rig --serve process):
  after    : shipped (batch ON, share ON)                          before : batch off + share off (the old per-upload cost and the old per-screen fan-out)
  after2   : the shipped arm again: the determinism control        batchonly: batch ON, share off (U3 alone)         shareonly: share ON, batch off (U2 alone)
  distinct / distinct_before : a different picture per screen (nothing to share): the honest control, the bytes must equal the old fan-out
  plant    : WWE_RDR_PLANT_SCREEN_REUPLOAD = every screen re-sent every frame to a private image: the BYTES bar must FAIL on it
PASS needs (per steady repeat):
  - after: screen uploads <= 1 and <= one picture's bytes; before: one upload per screen (>= 2); pictures served with no upload = screens - 1; no rebind in a steady repeat;
  - U3: barriers recorded a frame fall from before to after (batchonly vs before shows U3 alone); image copies a frame fall from before to after (the share);
  - the drawn picture (hash of every repeat + the first render's PNG) is identical in every arm (max |diff| 0): only the binding is shared, never the pixels;
  - distinct: screen bytes equal the old fan-out; after2 draws what after drew; the plant arm is CAUGHT by the bytes bar.
Exit 0 = all PASS. Writes upl_numbers.json under --out.
"""
import argparse, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import up_check  # noqa: E402  (frame_rig session + run_arm + load_png)

REP_RE = re.compile(r'up_rep (\d+) images (\d+) buffers (\d+) barriers (\d+) bytes (\d+) screen_copies (\d+) screen_bytes (\d+) pics (\d+) shared (\d+) moved (\d+) hash ([0-9a-f]+)')
REP_KEYS = ['k', 'images', 'buffers', 'barriers', 'bytes', 'screen_copies', 'screen_bytes', 'pics', 'shared', 'moved', 'hash']
FIRST_RE = re.compile(r'up_screens (\w+) pics (\d+) shared (\d+) moved (\d+) images (\d+) rows (\d+) sized (\d+)')
PIC = 256 * 128 * 4


def read_arm(work, frame):
    rp = os.path.join(work, 'ours_%d_report.txt' % frame)
    body = open(rp, encoding='utf8', errors='replace').read() if os.path.exists(rp) else ''
    reps = []
    for m in REP_RE.finditer(body):
        reps.append({k: (x if k == 'hash' else int(x)) for k, x in zip(REP_KEYS, m.groups())})
    first = FIRST_RE.search(body)
    return reps, (dict(zip(['pics', 'shared', 'moved', 'screen_images', 'rows', 'sized'], [int(x) for x in first.groups()[1:]])) if first else {})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    ap.add_argument('--frames', default='60')
    ap.add_argument('--repeat', type=int, default=8)
    ap.add_argument('--capture', default='a04_entrance')
    ap.add_argument('--arms', default='', help='comma list of arms to run (default all)')
    args = ap.parse_args()
    main_root = up_check.main_checkout()
    import run_dirs
    out = run_dirs.run_out('UPL_gate', args.out)
    os.makedirs(out, exist_ok=True)
    frame = int(args.frames.split(',')[0])
    capture = os.path.join(main_root, 'out', 'captures', args.capture)
    fails = []

    def check(ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        if not ok:
            fails.append(text)

    up_check.ENVS = up_check.ENVS + ('WWE_RDR_RIG_SCREEN_STAGGER', 'WWE_RDR_RIG_SCREEN_DROP', 'WWE_RDR_RIG_FEED_CACHE', 'WWE_RDR_SCREEN_SHARE', 'WWE_RDR_UPLOAD_BATCH', 'WWE_RDR_RIG_SCREEN_FEED', 'WWE_RDR_RIG_SCREEN_ROWS', 'WWE_RDR_PLANT_SCREEN_REUPLOAD')
    base = {'WWE_RDR_RIG_SCREEN_ROWS': '1', 'WWE_RDR_RIG_SCREEN_FEED': 'same'}
    plan = (('after', dict(base)),
            ('before', dict(base, WWE_RDR_SCREEN_SHARE='0', WWE_RDR_UPLOAD_BATCH='0')),
            ('after2', dict(base)),
            ('batchonly', dict(base, WWE_RDR_SCREEN_SHARE='0')),
            ('shareonly', dict(base, WWE_RDR_UPLOAD_BATCH='0')),
            ('distinct', dict(base, WWE_RDR_RIG_SCREEN_FEED='distinct')),
            ('distinct_before', dict(base, WWE_RDR_RIG_SCREEN_FEED='distinct', WWE_RDR_SCREEN_SHARE='0', WWE_RDR_UPLOAD_BATCH='0')),
            ('plant', dict(base, WWE_RDR_PLANT_SCREEN_REUPLOAD='1')),
            # the live shapes (review): serials that stagger (half the screens lag a serial on every other build), a row that drops one build and returns with an old serial, and a cache-backed feed that recycles
            # (2 entries for 4 pictures: a hit kept until the next call is overwritten by the next conversion). Each against its own share-off / batch-off twin: the picture must not move.
            ('stagger', dict(base, WWE_RDR_RIG_SCREEN_STAGGER='1')),
            ('stagger_before', dict(base, WWE_RDR_RIG_SCREEN_STAGGER='1', WWE_RDR_SCREEN_SHARE='0', WWE_RDR_UPLOAD_BATCH='0')),
            ('drop', dict(base, WWE_RDR_RIG_SCREEN_DROP='1')),
            ('drop_before', dict(base, WWE_RDR_RIG_SCREEN_DROP='1', WWE_RDR_SCREEN_SHARE='0', WWE_RDR_UPLOAD_BATCH='0')),
            ('cache2', dict(base, WWE_RDR_RIG_SCREEN_FEED='distinct', WWE_RDR_RIG_FEED_CACHE='2')),
            ('cache2_before', dict(base, WWE_RDR_RIG_SCREEN_FEED='distinct', WWE_RDR_RIG_FEED_CACHE='2', WWE_RDR_SCREEN_SHARE='0', WWE_RDR_UPLOAD_BATCH='0')),
            ('cache2same', dict(base, WWE_RDR_RIG_FEED_CACHE='2')),
            ('cache2same_before', dict(base, WWE_RDR_RIG_FEED_CACHE='2', WWE_RDR_SCREEN_SHARE='0', WWE_RDR_UPLOAD_BATCH='0')))
    if args.arms:
        plan = tuple(p for p in plan if p[0] in args.arms.split(','))
    arms, works, firsts = {}, {}, {}
    for tag, env in plan:
        reps_n = max(args.repeat, 12) if tag.startswith(('stagger', 'drop')) else args.repeat
        res, seq, text, rc, work = up_check.run_arm(capture, [frame], os.path.join(out, tag), env, repeat=reps_n)
        reps, first = read_arm(work, frame)
        arms[tag], works[tag], firsts[tag] = reps, work, first
        check(rc == 0, '%s arm: frame_rig exit %s' % (tag, rc))
        check(len(reps) == reps_n - 1, '%s arm: %d repeat lines (%d wanted)' % (tag, len(reps), reps_n - 1))
        check(first.get('rows', 0) >= 2, '%s arm: the record carries %d screen rows (the rig added them)' % (tag, first.get('rows', 0)))
        for r in reps:
            print('INFO  %-15s repeat %d: screen uploads %d (%.2f MB), pictures asked %d, served with no upload %d, rebinds %d | image copies %d, buffer copies %d, barriers %d, %.2f MB | picture %s' % (
                tag, r['k'], r['screen_copies'], r['screen_bytes'] / 1048576.0, r['pics'], r['shared'], r['moved'], r['images'], r['buffers'], r['barriers'], r['bytes'] / 1048576.0, r['hash']))
    have = lambda *n: all(a in arms and arms[a] for a in n)  # noqa: E731
    if have('after', 'before'):
        # the steady repeats: skip the first repeat (the screens' images are being made: the rebind from the textures' own images to the shared one happens there)
        for ra, rb in zip(arms['after'][1:], arms['before'][1:]):
            k = ra['k']
            check(rb['screen_copies'] >= 2, 'before repeat %d: the old fan-out uploads one picture per screen (%d uploads, %d bytes)' % (k, rb['screen_copies'], rb['screen_bytes']))
            check(ra['screen_copies'] <= 1 and ra['screen_bytes'] <= PIC, 'after repeat %d: ONE screen upload of at most one picture (%d uploads, %d bytes; the old fan-out %d, %d)' % (k, ra['screen_copies'], ra['screen_bytes'], rb['screen_copies'], rb['screen_bytes']))
            check(ra['shared'] == ra['pics'] - ra['screen_copies'] and ra['moved'] == 0, 'after repeat %d: %d pictures asked, %d served with no upload, %d rebinds (steady: none)' % (k, ra['pics'], ra['shared'], ra['moved']))
            check(ra['images'] < rb['images'], 'after repeat %d: image copies a frame fall (%d -> %d)' % (k, rb['images'], ra['images']))
            check(ra['barriers'] < rb['barriers'], 'after repeat %d: barriers recorded a frame fall (%d -> %d)' % (k, rb['barriers'], ra['barriers']))
    if have('batchonly', 'before'):
        for rb, rt in zip(arms['before'][1:], arms['batchonly'][1:]):
            if rb['images'] > 1:
                check(rt['images'] == rb['images'] and rt['barriers'] < rb['barriers'], 'U3 alone repeat %d: the same %d image copies, barriers recorded fall (%d -> %d)' % (rb['k'], rb['images'], rb['barriers'], rt['barriers']))
    if have('shareonly', 'before'):
        for rb, rs in zip(arms['before'][1:], arms['shareonly'][1:]):
            check(rs['screen_copies'] <= 1 and rs['screen_bytes'] < rb['screen_bytes'], 'U2 alone repeat %d: screen uploads fall (%d -> %d, %d -> %d bytes)' % (rb['k'], rb['screen_copies'], rs['screen_copies'], rb['screen_bytes'], rs['screen_bytes']))
    if have('distinct', 'distinct_before'):
        for rd, rb in zip(arms['distinct'][1:], arms['distinct_before'][1:]):
            check(rd['screen_copies'] == rb['screen_copies'] and rd['screen_bytes'] == rb['screen_bytes'], 'distinct repeat %d: a different picture per screen leaves nothing to dedupe (%d uploads, %d bytes; old %d, %d): the saving comes only from equal pictures' % (rd['k'], rd['screen_copies'], rd['screen_bytes'], rb['screen_copies'], rb['screen_bytes']))
    if have('after', 'plant'):
        for ra, rp in zip(arms['after'][1:], arms['plant'][1:]):
            caught = not (rp['screen_copies'] <= 1 and rp['screen_bytes'] <= PIC)
            check(caught, 'plant (every screen re-sent every frame) repeat %d is CAUGHT by the bytes bar (%d uploads, %d bytes vs shipped %d, %d)' % (ra['k'], rp['screen_copies'], rp['screen_bytes'], ra['screen_copies'], ra['screen_bytes']))
    # the picture: the hash of every repeat and the first render's PNG are identical in every arm of one feed
    worst = 0
    groups = (('after', 'before', 'after2', 'batchonly', 'shareonly', 'plant'), ('distinct', 'distinct_before'), ('stagger', 'stagger_before'), ('drop', 'drop_before'), ('cache2', 'cache2_before', 'distinct'), ('cache2same', 'cache2same_before', 'after'))
    for grp in groups:
        ref = next((a for a in grp if a in arms and arms[a]), None)
        for a in grp:
            if a == ref or a not in arms or not arms[a]:
                continue
            check([r['hash'] for r in arms[a]] == [r['hash'] for r in arms[ref]], '%s: the picture of every repeat is identical to %s (hashes)' % (a, ref))
            pa, pb = os.path.join(works[ref], 'ours_%d_mask511.png' % frame), os.path.join(works[a], 'ours_%d_mask511.png' % frame)
            if os.path.exists(pa) and os.path.exists(pb):
                ia, ib = up_check.load_png(pa), up_check.load_png(pb)
                d = 255 if ia.shape != ib.shape else int(abs(ia.astype('int16') - ib.astype('int16')).max())
                worst = max(worst, d)
                check(d == 0, '%s: the first render is identical to %s (max |diff| %d)' % (a, ref, d))
    if have('distinct', 'after'):   # the one-buffer feed with a different picture per key: the screens must NOT all show the last picture (the old pointer-kept-until-flush fault)
        check([r['hash'] for r in arms['distinct']] != [r['hash'] for r in arms['after']], 'distinct (one shared buffer, a picture per key): the drawn pictures differ from the same-picture arm (the screens keep their own pictures)')
    if have('stagger', 'stagger_before'):
        moved = sum(r['moved'] for r in arms['stagger'][2:])
        n = len(arms['stagger'][2:])
        check(moved <= (n + 1) // 2, 'stagger: re-binds in %d steady repeats = %d (a screen that just moved is held for 8 builds: at most one per two repeats; per-repeat %s)' % (n, moved, ','.join(str(r['moved']) for r in arms['stagger'][2:])))
        check(sum(r['screen_bytes'] for r in arms['stagger'][2:]) <= sum(r['screen_bytes'] for r in arms['stagger_before'][2:]), 'stagger: screen bytes never above the old fan-out')
    json.dump({'arms': arms, 'first': firsts, 'max_pixel_diff': worst}, open(os.path.join(out, 'upl_numbers.json'), 'w'), indent=1)
    print('upl_check: %s (%d FAIL)' % ('PASS' if not fails else 'FAIL', len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
