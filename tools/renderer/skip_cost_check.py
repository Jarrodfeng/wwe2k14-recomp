#!/usr/bin/env python3
"""S102 lane K1 acceptance test, offline, 0 game runs, no window: a frame the render thread handles but does not draw costs almost nothing (R16 pieces A + B).

  py -I tools\\renderer\\skip_cost_check.py [--root DIR] [--exe RPLAY] [--only brock|bryan] [--keep]

rplay replays a capture with every 2nd frame handled as the live sink handles a skipped one (--skip-every 2: classify, pruned persist-only bind), banks lazy + blocks on (T1 / T1b), in arms:
  OFF    the shipped skip path (guard insert-only, maps).                       ON     --guard-decay on --skip-lean on (K1).
  TRUTH  --skip-prune truth: every skipped frame fully bound (the lasting state a skip must reproduce).
  twins  (each must differ and is reported as CAUGHT):
         NONE  --skip-prune none (a skipped frame drops the passes of the lasting copies it needed)          vs OFF, by the lasting-destination hashes;
         BANK  --bank-block-plant 4 (a bank resolved without its patches, on frames that are then drawn)     vs OFF, by the drawn frames' pictures.
         (The guard decaying too fast and the flat walk forgetting a write are twins of guard_decay_unit.py: the real captures never make the guard hold a moving copy, see docs\\renderer\\SKIP_COST.md.)
Bars (all must hold, per capture):
  1. exit 0 (= validation errors 0) for OFF, ON and TRUTH.
  2. the drawn frames' pictures (--shots on every 20th drawn frame) are byte-equal OFF vs ON.
  3. the lasting destinations (--skip-hash) are equal OFF vs ON, and ON differs from TRUTH in exactly the places OFF does (the shipped prune is not a full bind; K1 adds no difference).
  4. the guard state after the run (the census line's state hash) is equal OFF vs ON (the flat walk is the map walk), and the lasting-bind count ON <= OFF.
  5. the census line is read (a missing line fails loudly); the ms a skipped / drawn frame and the Note cost are printed OFF against ON (not a bar: the PC is shared; the Surface is the judge).
Exit 0 all PASS, 1 any FAIL. A check that cannot read its input fails loudly."""
import argparse, glob, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = {
    'brock': ('folder', 'out/s101/thin/case_brock/capture', 341),
    'bryan': ('zip', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip', 237),
}
LINE = re.compile(r'\[rplay\]\[skip_cost\] frames (\d+), skipped (\d+), lasting binds (\d+), skipped-frame ms mean ([\d.]+), drawn-frame ms mean ([\d.]+).*?per 600 frames: lasting binds ([\d.]+); guard decay (on|off) \(window (\d+)\), lean (on|off): guard held (\d+), would have been held by the old guard (\d+), per-frame by image (\d+); state guard images (\d+), state hash ([0-9a-f]+); streaks.Note ([\d.]+) us a frame handled; split a skipped frame: contents (\d+) us, bind (\d+) us, record \+ submit (\d+) us; a drawn frame: (\d+) / (\d+) / (\d+) us; stale reads (\d+) \(img(\d+) (\d+), spliced img (\d+): (\d+)\), would decay \(switch off\) (\d+), rule (\w+), top image-rule dest img (\d+) \((\d+) copies left out\)')
HASH = re.compile(r'\[rplay\]\[skiphash\] f=(\d+) n=(\d+) dest=(\d+) mip=(\d+) rect=(\S+) hash=([0-9a-f]+)')


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def run(exe, cap, frames, name, extra, work, shots, nshots=True):
    shot_dir = os.path.join(work, name + '_shots')
    shutil.rmtree(shot_dir, ignore_errors=True)
    os.makedirs(shot_dir)
    cmd = [exe, '--capture', cap, '--frames', str(frames), '--validation', '1', '--skip-every', '2', '--lazy-banks', 'on', '--bank-blocks', 'on', '--skip-hash', '80',
           '--shot-dir', shot_dir, '--shots', ','.join(str(s) for s in (shots if nshots else shots[:1])), '--out', os.path.join(work, name + '.png')] + extra
    for attempt in range(12):   # the GPU is shared: retry only the logged start-up refusal
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if 'Not enough graphics memory for the frame ring' not in text:
            break
        print('NOTE  arm %s: not enough graphics memory, attempt %d of 12' % (name, attempt + 1), flush=True)
        import time
        time.sleep(20)
    m = LINE.search(text)
    if not m:
        print(text[-2500:])
        raise RuntimeError('arm %s printed no [rplay][skip_cost] census line' % name)
    g = m.groups()
    census = dict(frames=int(g[0]), skipped=int(g[1]), lasting=int(g[2]), skip_ms=float(g[3]), drawn_ms=float(g[4]), per600=float(g[5]), decay=g[6], lean=g[8], held=int(g[9]), old_held=int(g[10]),
                  by_image=int(g[11]), state_images=int(g[12]), state_hash=g[13], note_us=float(g[14]), split=[int(x) for x in g[15:21]], stale=int(g[21]), stale_img=int(g[22]), stale_img_n=int(g[23]), stale_spliced=int(g[25]), would=int(g[26]), top_img=int(g[28]))
    hashes = {(int(a), int(b)): (c, d, e, f) for a, b, c, d, e, f in HASH.findall(text)}
    pics = {}
    for s in shots:
        fp = os.path.join(shot_dir, 'frame_%04d.png' % s)
        if os.path.isfile(fp):
            with open(fp, 'rb') as f:
                pics[s] = f.read()
    return census, hashes, pics, p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--exe', default='')
    ap.add_argument('--only', default='')
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    exe = args.exe or next((e for e in (os.path.join(root, 'out', 'renderer', 'release', 'rplay.exe'), os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')) if os.path.isfile(e)), '')
    if not exe or not os.path.isfile(exe):
        print('FAIL  rplay.exe is not built (tools\\renderer\\build.ps1 -Release)')
        return 1
    work = os.path.join(root, 'out', 'k1')
    os.makedirs(work, exist_ok=True)
    fails = 0

    def line(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        fails += 0 if ok else 1

    for name, (kind, rel, frames) in CAPS.items():
        if args.only and name != args.only:
            continue
        src = os.path.join(main_co, rel)
        if not os.path.exists(src):
            print('FAIL  input missing: ' + src)
            return 1
        unpacked = None
        if kind == 'zip':   # rplay reads a capture FOLDER: the case zip is unpacked once (tar) and removed at the end
            unpacked = os.path.join(work, name + '_unpack')
            shutil.rmtree(unpacked, ignore_errors=True)
            os.makedirs(unpacked)
            tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
            subprocess.run([tar, '-xf', src, '-C', unpacked], capture_output=True, timeout=900)
            cap = os.path.join(unpacked, 'capture')
        else:
            cap = src
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            print('FAIL  not a capture folder: ' + cap)
            return 1
        shots = list(range(20, frames, 20))
        shots = [s for s in shots if s % 2 == 0]   # --skip-every 2 skips the odd frames: the even ones are drawn
        off, h_off, p_off, rc_off = run(exe, cap, frames, name + '_off', [], work, shots)
        on, h_on, p_on, rc_on = run(exe, cap, frames, name + '_on', ['--guard-decay', 'on', '--skip-lean', 'on'], work, shots)
        tr, h_tr, p_tr, rc_tr = run(exe, cap, frames, name + '_truth', ['--skip-prune', 'truth'], work, shots)
        nn, h_nn, p_nn, rc_nn = run(exe, cap, frames, name + '_none', ['--skip-prune', 'none'], work, shots)
        bk, h_bk, p_bk, rc_bk = run(exe, cap, frames, name + '_bank', ['--bank-block-plant', '4'], work, shots)
        # review F6: the splice. A reader gap (frames 40..99, longer than the 16 frame window) is spliced into the capture's reads of one of the shadow copies' images; the stale-read ledger must pass the positive-evidence
        # rule (no more stale reads of that image than the shipped path has) and flag the time-based draft rule on at least one image.
        sp_a, sp_b = (40, 100) if name == 'brock' else (30, 80)
        flagged = []
        for im in (215, 216, 214):
            sp = ['--splice-reader', '%d,%d,%d' % (im, sp_a, sp_b)]
            s_off, _, _, _ = run(exe, cap, frames, '%s_sp%d_off' % (name, im), sp, work, shots, False)
            s_pos, _, _, _ = run(exe, cap, frames, '%s_sp%d_pos' % (name, im), ['--guard-decay', 'on'] + sp, work, shots, False)
            s_old, _, _, _ = run(exe, cap, frames, '%s_sp%d_old' % (name, im), ['--guard-decay', 'on', '--guard-rule', 'old'] + sp, work, shots, False)
            line(s_pos['stale_spliced'] <= s_off['stale_spliced'], '%s: splice on img%d (reader gap %d..%d): positive-evidence stale reads of it %d <= shipped (switch off) %d; old rule %d' % (name, im, sp_a, sp_b - 1, s_pos['stale_spliced'], s_off['stale_spliced'], s_old['stale_spliced']))
            if s_old['stale_spliced'] > s_off['stale_spliced']:
                flagged.append(im)
        line(len(flagged) > 0, '%s: the stale-read ledger flags the time-based draft rule on at least one spliced image (flagged: %s): CAUGHT' % (name, flagged))
        line(on['stale'] <= off['stale'], '%s: stale reads with K1 on %d <= shipped path %d (the shipped path itself reads %d stale of img%d: the per-frame state image the catch-up redoes, not a K1 effect)' % (name, on['stale'], off['stale'], off['stale'], off['stale_img']))
        line((rc_off, rc_on, rc_tr) == (0, 0, 0), '%s: exit codes (= validation errors) OFF %d, ON %d, TRUTH %d (all must be 0)' % (name, rc_off, rc_on, rc_tr))
        bad = [s for s in shots if p_off.get(s) is None or p_off.get(s) != p_on.get(s)]
        line(len(p_off) == len(shots) and not bad, '%s: %d drawn frames, pictures ON byte-equal with OFF (first differing %s)' % (name, len(shots), bad[:6]))
        line(len(h_off) > 0 and h_off == h_on, '%s: %d lasting destinations hashed, ON equal with OFF' % (name, len(h_off)))
        diff_truth_off = [k for k in h_off if h_tr.get(k) != h_off[k]]
        diff_truth_on = [k for k in h_on if h_tr.get(k) != h_on[k]]
        line(len(h_tr) > 0 and diff_truth_off == diff_truth_on and len(h_tr) == len(h_off),
             '%s: lasting destinations against TRUTH: ON differs in exactly the places OFF does (%d of %d; the shipped prune already differs from a full bind on the early frames, K1 adds none)' % (name, len(diff_truth_on), len(h_off)))
        line(off['state_hash'] == on['state_hash'] and off['state_images'] == on['state_images'], '%s: guard state after the run equal OFF vs ON (flat walk == map walk): %d images, hash %s' % (name, off['state_images'], off['state_hash']))
        line(on['lasting'] <= off['lasting'] and on['skipped'] == off['skipped'], '%s: lasting binds ON %d <= OFF %d (of %d skipped)' % (name, on['lasting'], off['lasting'], off['skipped']))
        d_none = [k for k in h_off if h_nn.get(k) != h_off[k]]
        line(len(d_none) > 0, 'planted twin NONE (a skipped frame drops the passes its lasting copies need), %s: %d of %d lasting destinations differ from OFF (must be > 0): CAUGHT' % (name, len(d_none), len(h_off)))
        d_bank = [s for s in shots if p_off.get(s) != p_bk.get(s)]
        line(len(d_bank) > 0, 'planted twin BANK (a bank resolved without its patches on drawn frames), %s: %d of %d drawn pictures differ from OFF (must be > 0): CAUGHT' % (name, len(d_bank), len(shots)))
        print('INFO  %s per 600 frames: lasting binds OFF %.1f ON %.1f; guard held %d (old guard would have held %d more); per-frame by the image rule %d' % (name, off['per600'], on['per600'], off['held'], on['old_held'], off['by_image']), flush=True)
        print('INFO  %s render-side ms (capture file read left out; the PC is shared, not a bar): skipped frame OFF %.3f ON %.3f; drawn frame OFF %.3f ON %.3f; streaks.Note OFF %.1f us ON %.1f us a frame handled' % (
            name, off['skip_ms'], on['skip_ms'], off['drawn_ms'], on['drawn_ms'], off['note_us'], on['note_us']), flush=True)
        print('INFO  %s split of a skipped frame OFF / ON (us): contents %d / %d, bind %d / %d, record + submit %d / %d; a drawn frame OFF: %d / %d / %d' % (
            name, off['split'][0], on['split'][0], off['split'][1], on['split'][1], off['split'][2], on['split'][2], off['split'][3], off['split'][4], off['split'][5]), flush=True)
        if unpacked and not args.keep:
            shutil.rmtree(unpacked, ignore_errors=True)
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
