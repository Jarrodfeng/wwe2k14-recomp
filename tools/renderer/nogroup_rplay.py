r"""S102 lane K2 (R16 piece C): rplay replays of Bryan's entrance and the Brock match with `--skip-every 2`, the group rule (OFF, the shipped lasting bind) against `--lasting-nogroup on` (ON). 0 game runs.

  py -I tools\renderer\nogroup_rplay.py [--root DIR] [--exe RPLAY] [--only brock|bryan] [--reps N]

Reports per capture (a table in the output, copied to docs\renderer\SKIP_COST.md): the lasting binds, ms per lasting bind OFF vs ON (render-thread cost of a skipped frame that carries a lasting bind: contents + classify + bind +
record + submit, the capture file read left out; N alternating repetitions, the median is the number; the PC is shared with other lanes: a trend, not a bar), draws kept of draws, passes kept, object passes kept / left out,
the stale-read ledger (K1: reads of an owed image before a drawn frame's own write; the shipped path reads 281 Brock / 172 Bryan, almost all img388).
Bars (FAIL = exit 1):
  - exit 0 (rplay's exit code is its validation error count) in every arm; the census lines are printed (an arm with no [rplay][k2] line fails by name)
  - STRESS arms (--skip-stress on, TEST ONLY: every skipped frame carries one forced lasting copy, so the lasting bind runs on every skipped frame): the same bars, and the lever must take (draws kept ON < OFF, object passes left out > 0)
  - the pictures of the drawn frames are byte-equal OFF vs ON
  - the lasting destinations' hashes (--skip-hash) are equal OFF vs ON
  - the lasting-bind COUNT is equal OFF vs ON (the switch changes what a lasting bind keeps, never whether a frame has one)
  - ON keeps no more draws than OFF; ON's stale reads <= OFF's (the ledger must not rise), reported per top image
  - the plants: --skip-prune none (a skipped frame drops the passes its lasting copies need: the destinations change) is CAUGHT as in K1's check
Whether the lever has anything to take on a capture is reported (object passes left out > 0); Brock and Bryan both carry object passes."""
import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = {
    'brock': ('folder', 'out/s101/thin/case_brock/capture', 341),
    'bryan': ('zip', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip', 237),
}
K2 = re.compile(r'\[rplay\]\[k2\] nogroup (on|off): lasting binds (\d+) of (\d+) skipped frames, ms per lasting bind ([\d.]+) \(Bind\(\) alone ([\d.]+)\), kept (\d+) of (\d+) draws, (\d+) of (\d+) passes \((\d+) only by the model-part group\); object passes (\d+), kept (\d+), left out although another was kept (\d+), kept for a lasting render target (\d+)')
STALE = re.compile(r'stale reads (\d+) \(img(\d+) (\d+),')
HASH = re.compile(r'\[rplay\]\[skiphash\] f=(\d+) n=(\d+) dest=(\d+) mip=(\d+) rect=(\S+) hash=([0-9a-f]+) forced=(\d)')
VAL = re.compile(r'validation errors (\d+)')
FAILS = []


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    if not ok:
        FAILS.append(text)


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def run(exe, cap, frames, name, extra, work, shots):
    shot_dir = os.path.join(work, name + '_shots')
    shutil.rmtree(shot_dir, ignore_errors=True)
    os.makedirs(shot_dir)
    cmd = [exe, '--capture', cap, '--frames', str(frames), '--validation', '1', '--skip-every', '2', '--lazy-banks', 'on', '--bank-blocks', 'on', '--skip-hash', '80',
           '--shot-dir', shot_dir, '--shots', ','.join(str(s) for s in shots), '--out', os.path.join(work, name + '.png')] + extra
    text, rc = '', 1
    for attempt in range(12):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, errors='replace')
        text, rc = (p.stdout or '') + (p.stderr or ''), p.returncode
        if 'Not enough graphics memory for the frame ring' not in text:
            break
        print('NOTE  arm %s: not enough graphics memory, attempt %d of 12' % (name, attempt + 1), flush=True)
        time.sleep(20)
    m = K2.search(text)
    if not m:
        print(text[-2000:])
        return None
    g = m.groups()
    k2 = dict(nogroup=g[0], lasting=int(g[1]), skipped=int(g[2]), ms=float(g[3]), bind_ms=float(g[4]), draws_kept=int(g[5]), draws_in=int(g[6]), passes_kept=int(g[7]), passes_in=int(g[8]), grouped=int(g[9]), obj=int(g[10]), obj_kept=int(g[11]), obj_dropped=int(g[12]), lt=int(g[13]))
    sm = STALE.search(text)
    k2.update(stale=int(sm.group(1)) if sm else -1, stale_img=int(sm.group(2)) if sm else 0, stale_img_n=int(sm.group(3)) if sm else 0, rc=rc)   # (rplay's exit code is its validation error count: 4 = errors)
    hashes = {(int(a), int(b)): (c, d, e, f, g) for a, b, c, d, e, f, g in HASH.findall(text)}   # (dest, mip, rect, hash, forced)
    pics = {}
    for s in shots:
        fp = os.path.join(shot_dir, 'frame_%04d.png' % s)
        if os.path.isfile(fp):
            with open(fp, 'rb') as f:
                pics[s] = f.read()
    return k2, hashes, pics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--exe', default='')
    ap.add_argument('--only', default='')
    ap.add_argument('--reps', type=int, default=3)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = os.environ.get('RDR_MAIN_CHECKOUT') or main_checkout(root)
    cands_exe = [e for e in (os.path.join(root, 'out', 'renderer', 'release', 'rplay.exe'), os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')) if os.path.isfile(e)]
    exe = args.exe or (max(cands_exe, key=os.path.getmtime) if cands_exe else '')   # the NEWEST build (a stale release exe once shadowed the dev build)
    if not exe:
        print('FAIL  rplay.exe is not built (tools\\renderer\\build.ps1)')
        return 1
    work = os.path.join(root, 'out', 'k2')
    os.makedirs(work, exist_ok=True)
    for name, (kind, rel, frames) in CAPS.items():
        if args.only and name != args.only:
            continue
        src = os.path.join(main_co, rel)
        if not os.path.exists(src):
            print('FAIL  input missing: ' + src)
            return 1
        if kind == 'zip':
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
        shots = [s for s in range(20, frames, 20) if s % 2 == 0]
        offs, ons = [], []
        for rep in range(max(1, args.reps)):   # alternate, so heat and the other lanes' load fall on both arms alike
            a = run(exe, cap, frames, '%s_off%d' % (name, rep), [], work, shots)
            b = run(exe, cap, frames, '%s_on%d' % (name, rep), ['--lasting-nogroup', 'on'], work, shots)
            if a is None or b is None:
                say(False, '%s: an arm printed no [rplay][k2] census line' % name)
                return 1
            offs.append(a), ons.append(b)
        off, h_off, p_off = offs[0]
        on, h_on, p_on = ons[0]
        say(all(r[0]['rc'] == 0 for r in offs + ons), '%s: every arm exit 0 (= validation errors 0; %d arms)' % (name, len(offs) + len(ons)))
        diff = [s for s in shots if p_off.get(s) != p_on.get(s) or s not in p_off]
        say(not diff and len(p_off) == len(shots), '%s: the pictures of the %d drawn frames are byte-equal OFF vs ON%s' % (name, len(shots), (' (differ: %s)' % diff[:10]) if diff else ''))
        hd = [k for k in set(h_off) | set(h_on) if h_off.get(k) != h_on.get(k)]
        say(len(h_off) > 0 and not hd, '%s: the lasting destinations (%d hashed copies) are equal OFF vs ON%s' % (name, len(h_off), (' (differ: %d, first %s)' % (len(hd), sorted(hd)[:3])) if hd else ''))
        say(off['lasting'] == on['lasting'], '%s: lasting binds OFF %d of %d skipped frames, ON %d (equal: the switch changes what a bind keeps, not whether there is one)' % (name, off['lasting'], off['skipped'], on['lasting']))
        # ON may keep a little MORE than OFF with natural seeds: the lasting-target rule keeps an object pass that draws to a lasting render target in every lasting bind, the group rule only when another object pass was kept.
        # The bar: at most 5 % of the frames' draws above OFF (the lever must not grow a bind); the stress arm below must show a real fall.
        say(on['draws_kept'] <= off['draws_kept'] + 0.05 * off['draws_in'], '%s: draws kept by the lasting binds OFF %d of %d (%.1f %%), ON %d (%.1f %%; %d object passes kept for a lasting render target); passes kept OFF %d of %d, ON %d' % (
            name, off['draws_kept'], off['draws_in'], 100.0 * off['draws_kept'] / max(1, off['draws_in']), on['draws_kept'], 100.0 * on['draws_kept'] / max(1, on['draws_in']), on['lt'], off['passes_kept'], off['passes_in'], on['passes_kept']))
        say(on['stale'] <= off['stale'], '%s: stale reads (K1 ledger) ON %d <= OFF %d (top image img%d: OFF %d, ON img%d %d)' % (name, on['stale'], off['stale'], off['stale_img'], off['stale_img_n'], on['stale_img'], on['stale_img_n']))
        ms_off = [r[0]['ms'] for r in offs]
        ms_on = [r[0]['ms'] for r in ons]
        bo_n, bn_n = [r[0]['bind_ms'] for r in offs], [r[0]['bind_ms'] for r in ons]
        print('INFO  %s natural seeds: ms per lasting bind (median of %d alternating reps; every rep: OFF %s | ON %s): whole OFF %.3f ON %.3f; Bind() alone OFF %.3f ON %.3f; object passes %d, kept OFF %d / ON %d, left out by ON although another was kept %d' % (
            name, len(offs), ' '.join('%.2f' % x for x in ms_off), ' '.join('%.2f' % x for x in ms_on), statistics.median(ms_off), statistics.median(ms_on), statistics.median(bo_n), statistics.median(bn_n), off['obj'], off['obj_kept'], on['obj_kept'], on['obj_dropped']), flush=True)
        # STRESS (TEST ONLY): every skipped frame carries one forced lasting copy, so the lasting bind runs on every skipped frame (the Surface's shape when a guard holds a moving copy) and the group rule has real work to pull in
        so, sn = [], []
        for rep_i in range(max(1, args.reps)):
            a = run(exe, cap, frames, '%s_soff%d' % (name, rep_i), ['--skip-stress', 'lasting'], work, shots)
            b = run(exe, cap, frames, '%s_son%d' % (name, rep_i), ['--skip-stress', 'lasting', '--lasting-nogroup', 'on'], work, shots)
            if a is None or b is None:
                say(False, '%s: a stress arm printed no [rplay][k2] census line' % name)
                return 1
            so.append(a), sn.append(b)
        (soff, hs_off, ps_off), (son, hs_on, ps_on) = so[0], sn[0]
        say(all(r[0]['rc'] == 0 for r in so + sn), '%s stress: every arm exit 0 (%d arms)' % (name, len(so) + len(sn)))
        sd = [s_ for s_ in shots if ps_off.get(s_) != ps_on.get(s_) or s_ not in ps_off]
        say(not sd and len(ps_off) == len(shots), '%s stress: the pictures of the %d drawn frames are byte-equal OFF vs ON%s' % (name, len(shots), (' (differ: %s)' % sd[:10]) if sd else ''))
        shd = [k for k in set(hs_off) | set(hs_on) if hs_off.get(k) != hs_on.get(k)]
        # A forced seed is a per-frame copy the classifier would never keep (forced=1 in the hash line). Its source pass can accumulate into a target that persists across frames (no colour clear), so the passes the group rule kept
        # in the EARLIER skipped frames shaped it (docs/renderer/SKIP_COST.md K2, finding 3): INFO, with the count. The bar: every NATURAL lasting copy (forced=0) is equal OFF vs ON, even under stress.
        nat_keys = [k for k in hs_off if hs_off[k][4] == '0']
        nat_bad = [k for k in nat_keys if hs_off.get(k) != hs_on.get(k)]
        forced_bad = [k for k in hs_off if hs_off[k][4] == '1' and hs_off.get(k) != hs_on.get(k)]
        say(len(nat_keys) > 0 and not nat_bad, '%s stress: the %d natural lasting copies are equal OFF vs ON%s; forced-seed copies differing: %d of %d (dest images %s: INFO, see SKIP_COST.md K2)' % (
            name, len(nat_keys), (' (DIFFER: %d, first %s)' % (len(nat_bad), sorted(nat_bad)[:3])) if nat_bad else '', len(forced_bad), len(hs_off) - len(nat_keys), sorted(set(hs_off[k][0] for k in forced_bad))[:8]))
        say(soff['lasting'] == son['lasting'] and son['lasting'] > 0, '%s stress: lasting binds OFF %d of %d skipped frames, ON %d' % (name, soff['lasting'], soff['skipped'], son['lasting']))
        say(son['draws_kept'] < soff['draws_kept'] and son['obj_dropped'] > 0, '%s stress: the lever took: draws kept OFF %d of %d (%.1f %%), ON %d (%.1f %%); passes kept OFF %d, ON %d; object passes %d, kept OFF %d / ON %d, left out %d' % (
            name, soff['draws_kept'], soff['draws_in'], 100.0 * soff['draws_kept'] / max(1, soff['draws_in']), son['draws_kept'], 100.0 * son['draws_kept'] / max(1, son['draws_in']), soff['passes_kept'], son['passes_kept'], soff['obj'], soff['obj_kept'], son['obj_kept'], son['obj_dropped']))
        say(son['stale'] <= soff['stale'], '%s stress: stale reads (K1 ledger) ON %d <= OFF %d' % (name, son['stale'], soff['stale']))
        # F2: the forced seeds copy into a kUsageLasting image (a real lasting seed's shape). Against the TRUTH bind (every op bound, the renderer's PersistentOnly filter as for the others) the forced copies must be equal in ON
        # wherever they are in OFF; the planted twin (--skip-prune none: the passes a seed needs are not bound) must differ on them. (An any-copy forced seed is NOT a bar: into an image nothing carries, the renderer's own
        # PersistentOnly filter drops the whole chain unless the extra passes of the group rule happened to flag it: SKIP_COST.md K2 finding 3.)
        st_truth = run(exe, cap, frames, name + '_struth', ['--skip-stress', 'lasting', '--skip-prune', 'truth'], work, shots)
        st_none = run(exe, cap, frames, name + '_snone', ['--skip-stress', 'lasting', '--lasting-nogroup', 'on', '--skip-prune', 'none'], work, shots)
        if st_truth is None or st_none is None:
            say(False, '%s stress: the truth / none arm printed no census line' % name)
            return 1
        h_tr, h_nn = st_truth[1], st_none[1]
        forced = [k for k in hs_off if hs_off[k][4] == '1']
        f_on_bad = [k for k in forced if hs_on.get(k, ('',) * 5)[3] != h_tr.get(k, ('',) * 5)[3] and hs_off.get(k, ('',) * 5)[3] == h_tr.get(k, ('',) * 5)[3]]   # ON must lose nothing OFF had right
        f_off_bad = [k for k in forced if hs_off.get(k, ('',) * 5)[3] != h_tr.get(k, ('',) * 5)[3]]
        f_nn_bad = [k for k in forced if h_nn.get(k, ('',) * 5)[3] != h_tr.get(k, ('',) * 5)[3]]
        say(len(forced) > 0 and not f_on_bad, '%s stress: the %d forced lasting-destination copies are equal to the TRUTH bind in ON wherever OFF equals it (OFF differs from truth on %d of them itself, ON loses %d that OFF had)' % (name, len(forced), len(f_off_bad), len(f_on_bad)))
        say(len(forced) > 0 and len(f_nn_bad) > 0, '%s stress: plant --skip-prune none: CAUGHT, %d of %d forced copies differ from truth' % (name, len(f_nn_bad), len(forced)))
        mo, mn = [r[0]['ms'] for r in so], [r[0]['ms'] for r in sn]
        bo, bn = [r[0]['bind_ms'] for r in so], [r[0]['bind_ms'] for r in sn]
        print('INFO  %s stress: ms per lasting bind (median of %d alternating reps): whole (bind + record + submit, GPU waits on a shared 4090 included) OFF %.3f ON %.3f [reps OFF %s | ON %s]; Bind() alone (CPU only) OFF %.3f ON %.3f [reps OFF %s | ON %s]' % (
            name, len(so), statistics.median(mo), statistics.median(mn), ' '.join('%.2f' % x for x in mo), ' '.join('%.2f' % x for x in mn), statistics.median(bo), statistics.median(bn), ' '.join('%.2f' % x for x in bo), ' '.join('%.2f' % x for x in bn)), flush=True)
        # ANY-COPY stress (TEST ONLY): the forced seed is any copy of the frame (shadow maps, post chain, ...), which is what a guard holding a moving per-frame copy makes of a lasting bind: its chain pulls in many object passes, so the group rule
        # keeps most of the frame (Surface: 85-96 % of the draws). This is the arm for the kept share and the ms. The destinations are NOT a bar: the renderer's own PersistentOnly filter drops a chain into an image nothing carries
        # unless the extra passes of the group rule happened to flag it (SKIP_COST.md K2 finding 3, F2).
        ao, an = [], []
        for rep_i in range(max(1, args.reps)):
            ra = run(exe, cap, frames, '%s_aoff%d' % (name, rep_i), ['--skip-stress', 'on'], work, shots)
            rb = run(exe, cap, frames, '%s_aon%d' % (name, rep_i), ['--skip-stress', 'on', '--lasting-nogroup', 'on'], work, shots)
            if ra is None or rb is None:
                say(False, '%s: an any-copy stress arm printed no census line' % name)
                return 1
            ao.append(ra), an.append(rb)
        (aoff, ha_off, pa_off), (aon, ha_on, pa_on) = ao[0], an[0]
        ad = [s_ for s_ in shots if pa_off.get(s_) != pa_on.get(s_) or s_ not in pa_off]
        say(all(r[0]['rc'] == 0 for r in ao + an) and not ad and len(pa_off) == len(shots), '%s any-copy stress: every arm exit 0, the pictures of the %d drawn frames byte-equal OFF vs ON%s' % (name, len(shots), (' (differ: %s)' % ad[:10]) if ad else ''))
        a_nat = [k for k in ha_off if ha_off[k][4] == '0']
        a_nat_bad = [k for k in a_nat if ha_off.get(k) != ha_on.get(k)]
        a_forced_bad = [k for k in ha_off if ha_off[k][4] == '1' and ha_off.get(k) != ha_on.get(k)]
        say(len(a_nat) > 0 and not a_nat_bad, '%s any-copy stress: the %d natural lasting copies equal OFF vs ON; forced any-copy destinations differing OFF vs ON: %d of %d (INFO: finding 3)' % (name, len(a_nat), len(a_forced_bad), len(ha_off) - len(a_nat)))
        say(aon['draws_kept'] < aoff['draws_kept'] and aon['obj_dropped'] > 0 and aon['stale'] <= aoff['stale'], '%s any-copy stress: the lever took: draws kept OFF %d of %d (%.1f %%), ON %d (%.1f %%); passes kept OFF %d, ON %d; object passes %d, kept OFF %d / ON %d; stale reads ON %d <= OFF %d' % (
            name, aoff['draws_kept'], aoff['draws_in'], 100.0 * aoff['draws_kept'] / max(1, aoff['draws_in']), aon['draws_kept'], 100.0 * aon['draws_kept'] / max(1, aon['draws_in']), aoff['passes_kept'], aon['passes_kept'], aoff['obj'], aoff['obj_kept'], aon['obj_kept'], aon['stale'], aoff['stale']))
        amo, amn = [r[0]['ms'] for r in ao], [r[0]['ms'] for r in an]
        abo, abn = [r[0]['bind_ms'] for r in ao], [r[0]['bind_ms'] for r in an]
        print('INFO  %s any-copy stress: ms per lasting bind (median of %d alternating reps): whole OFF %.3f ON %.3f [reps OFF %s | ON %s]; Bind() alone OFF %.3f ON %.3f [reps OFF %s | ON %s]' % (
            name, len(ao), statistics.median(amo), statistics.median(amn), ' '.join('%.2f' % x for x in amo), ' '.join('%.2f' % x for x in amn), statistics.median(abo), statistics.median(abn), ' '.join('%.2f' % x for x in abo), ' '.join('%.2f' % x for x in abn)), flush=True)
        # F6: the owned card bake (--own-card-bake: every bake pass taken out of the walk and re-issued as a group) with lasting-destination stress seeds: the lasting destinations and the drawn pictures equal OFF vs ON
        bo = run(exe, cap, frames, name + '_bakeoff', ['--skip-stress', 'lasting', '--own-card-bake'], work, shots)
        bn = run(exe, cap, frames, name + '_bakeon', ['--skip-stress', 'lasting', '--own-card-bake', '--lasting-nogroup', 'on'], work, shots)
        if bo is None or bn is None:
            say(False, '%s: an --own-card-bake arm printed no census line' % name)
            return 1
        (bk_off, hb_off, pb_off), (bk_on, hb_on, pb_on) = bo, bn
        bd = [k for k in set(hb_off) | set(hb_on) if hb_off.get(k, ('',) * 5)[3] != hb_on.get(k, ('',) * 5)[3]]
        bpd = [s_ for s_ in shots if pb_off.get(s_) != pb_on.get(s_) or s_ not in pb_off]
        say(bk_off['rc'] == 0 and bk_on['rc'] == 0 and len(hb_off) > 0 and not bd and not bpd, '%s --own-card-bake stress: exit %s / %s, %d lasting-destination hashes, %d differ OFF vs ON, %d of %d drawn pictures differ; draws kept OFF %d of %d, ON %d' % (
            name, bk_off['rc'], bk_on['rc'], len(hb_off), len(bd), len(bpd), len(shots), bk_off['draws_kept'], bk_off['draws_in'], bk_on['draws_kept']))
        # the plant: a skipped frame that drops the passes its lasting copies need
        nn = run(exe, cap, frames, name + '_none', ['--skip-prune', 'none'], work, shots)
        if nn is None:
            say(False, '%s: plant none printed no census line' % name)
            continue
        nd = [k for k in set(h_off) | set(nn[1]) if h_off.get(k) != nn[1].get(k)]
        say(len(nd) > 0, '%s: plant --skip-prune none (the passes a lasting copy needs are not bound): CAUGHT, %d of %d lasting destinations differ from the shipped bind' % (name, len(nd), len(h_off)))
    print('nogroup_rplay: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
