r"""S102 lane K2 (stale reads with the guard decay; docs/renderer/SKIP_COST.md "stale reads with the decay"). 0 game runs.

The Surface's ab11 arms (guard_decay + skip_lean on) read 198-275 stale reads (`[rdr][skip_cost] stale reads (must be 0)`) against 1-2 in the A arm: an image read BEFORE its copy only every N handled frames (> the 16-frame guard
window), with per-frame reads after the copy in between, was judged by those per-frame reads and its guard dropped, so the skipped frames in between left its copy out and the next carry read found it owed.
The rule now also holds the guard while the reads after the copy stay within twice the image's own observed carry cadence (CopyStreaks::GuardRec::gap).

  py -I tools\renderer\cadence_check.py [--root DIR] [--exe RPLAY]

1. guard_decay_test (S9: cadence 40, window 16, 400 handled frames, the live order with the ledger): the cadence rule reads <= 1 stale (the first cycle) and the blind rule (cadence_blind, the faulty rule) >= 5: `--plant-blind` must FAIL.
2. rplay on the Brock capture (`--skip-every 2`, `--splice-cadence IMG,40`: every read of IMG removed, a state reader before its copy every 40th frame, a per-frame reader after the copy in the others), IMG 215 and 216:
   A (decay off), FIX (--guard-decay on), BLIND (--guard-decay on --guard-rule blind, the planted twin).
   Bars: exit 0 in all three; the spliced image's stale reads: FIX == A (the A arm stays at its own count); BLIND > A (the twin trips); FIX's total stale reads <= A's; the decay still works in FIX (copies it let go > 0)."""
import argparse
import shutil
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
STALE = re.compile(r'stale reads (\d+) \(img(\d+) (\d+), spliced img (\d+): (\d+)\)')
HELD = re.compile(r'guard held (\d+), would have been held by the old guard (\d+)')
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--exe', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = os.environ.get('RDR_MAIN_CHECKOUT') or main_checkout(root)
    out = os.path.join(root, 'out', 'k2c')
    os.makedirs(out, exist_ok=True)
    exe_t = os.path.join(out, 'gdt.exe')
    p = subprocess.run([CLANG, '-std=c++20', '-Wno-deprecated-declarations', '-I', os.path.join(root, 'src'), os.path.join(root, 'src', 'renderer_adapter', 'guard_decay_test.cpp'), '-o', exe_t], capture_output=True, text=True, timeout=600)
    if p.returncode:
        print(p.stderr[-1500:])
        say(False, 'guard_decay_test did not compile')
        return 1
    r = subprocess.run([exe_t], capture_output=True, text=True, timeout=120)
    say(r.returncode == 0 and 'all passed' in r.stdout, 'guard_decay_test S9-S15 (cadence, jitter, run end, kept copy, draw-instead): %s' % ' | '.join(l for l in r.stdout.splitlines() if l.startswith(('cadence', 'alternating', 'growing', 'jitter', 'run end', 'skipped frame', 'draw-instead'))))
    r = subprocess.run([exe_t, '--plant-peek'], capture_output=True, text=True, timeout=120)
    say(r.returncode != 0 and 'FAIL' in r.stdout, 'twin --plant-peek (the run-end recovery ignores the next frame): CAUGHT (exit %s)' % r.returncode)
    r = subprocess.run([exe_t, '--plant-blind'], capture_output=True, text=True, timeout=120)
    say(r.returncode != 0 and 'FAIL' in r.stdout, 'twin --plant-blind (the faulty rule restored): CAUGHT (exit %s)' % r.returncode)
    # text plants (each must make guard_decay_test FAIL): F3 (a skipped frame that keeps the copy does not clear the debt), F4 (the owed set is filled by Classify itself, also on a T3 draw-instead frame), the once-per-episode count,
    # the monotone gap (the last interval only), the pin on the first stale read
    src_path = os.path.join(root, 'src', 'renderer_adapter', 'lasting_prune.h')
    with open(src_path, encoding='utf-8') as fh:
        src = fh.read()
    test_src = open(os.path.join(root, 'src', 'renderer_adapter', 'guard_decay_test.cpp'), encoding='utf-8').read()
    for name, a, b in (('f3_no_erase', 'if (ledger) pend_owed_.push_back({dest, kKept, DestRectKey(c)});', ''),
                       ('keep_run_ends_keeps_nothing', 'if (goes_on) continue;', 'continue;'),
                       ('f1_keep_clears_at_once', 'if (ledger) pend_owed_.push_back({dest, kKept, rk});', 'if (ledger) ClearRect(dest, rk);'),
                       ('f2_image_level_next_test', 'if (x1 <= x0 || y1 <= y0) return false;', 'if (x1 <= x0 || y1 <= y0) return true;'),
                       ('kept_applied_immediately', 'if (pr.reason == kKept) {', 'if (false) {'),
                       ('f4_immediate_owed', 'if (!defer_owed) CommitOwed();', 'CommitOwed();'),
                       ('once_per_episode_off', 'settled_.count(id)) return;', 'false) return;'),
                       ('gap_last_only_and_no_pin', ('r->gap = std::max(r->gap, uint32_t(', 'Pin(id);   // review F2'), ('r->gap = std::max(0u, uint32_t(', '/* Pin(id); */   // review F2')),
                       ('pin_on_nocopy_read_off', 'if (!(f & 1)) r->pinned = true;', ''),
                       ('pin_on_stale_off', 'Pin(id);   // review F2', '/* Pin(id); */   // review F2')):
        planted = src
        for aa, bb in (zip(a, b) if isinstance(a, tuple) else [(a, b)]):
            planted = planted.replace(aa, bb) if planted.count(aa) == 1 else src
        if planted == src:
            say(False, 'text plant %s DID NOT APPLY (%r is gone)' % (name, a[:40]))
            continue
        scratch = os.path.join(out, 'plant_' + name, 'renderer_adapter')
        os.makedirs(scratch, exist_ok=True)
        with open(os.path.join(scratch, 'lasting_prune.h'), 'w', encoding='utf-8') as fh:
            fh.write(planted)
        with open(os.path.join(scratch, 'guard_decay_test.cpp'), 'w', encoding='utf-8') as fh:
            fh.write(test_src)
        exe_p = os.path.join(out, 'plant_' + name + '.exe')
        pc = subprocess.run([CLANG, '-std=c++20', '-Wno-deprecated-declarations', '-I', os.path.dirname(scratch), '-I', os.path.join(root, 'src', 'renderer_adapter'), '-I', os.path.join(root, 'src'),
                             os.path.join(scratch, 'guard_decay_test.cpp'), '-o', exe_p], capture_output=True, text=True, timeout=600)
        if pc.returncode:
            say(False, 'text plant %s did not compile: %s' % (name, pc.stderr[-300:]))
            continue
        pr = subprocess.run([exe_p], capture_output=True, text=True, timeout=120)
        say(pr.returncode != 0 and 'FAIL' in pr.stdout, 'text plant %s: CAUGHT (exit %s, %d FAIL lines)' % (name, pr.returncode, pr.stdout.count('FAIL ')))
    cands = [e for e in (os.path.join(root, 'out', 'renderer', 'release', 'rplay.exe'), os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')) if os.path.isfile(e)]
    exe = args.exe or (max(cands, key=os.path.getmtime) if cands else '')
    cap = os.path.join(main_co, 'out', 's101', 'thin', 'case_brock', 'capture')
    if not exe or not os.path.isfile(os.path.join(cap, 'images.bin')):
        say(False, 'input missing: rplay.exe %r or the Brock capture %s' % (exe, cap))
        return 1
    for img in (215, 216):
        res = {}
        for tag, extra in (('A', []), ('FIX', ['--guard-decay', 'on']), ('BLIND', ['--guard-decay', 'on', '--guard-rule', 'blind'])):
            q = subprocess.run([exe, '--capture', cap, '--frames', '341', '--validation', '1', '--skip-every', '2', '--lazy-banks', 'on', '--bank-blocks', 'on', '--splice-cadence', '%d,40' % img] + extra,
                               capture_output=True, text=True, timeout=1500, errors='replace')
            text = (q.stdout or '') + (q.stderr or '')
            m, h = STALE.search(text), HELD.search(text)
            res[tag] = dict(rc=q.returncode, total=int(m.group(1)) if m else -1, spliced=int(m.group(5)) if m else -1, decayed=int(h.group(2)) if h else -1)
        a, f, b = res['A'], res['FIX'], res['BLIND']
        say(all(x['rc'] == 0 and x['total'] >= 0 for x in res.values()), 'img%d cadence 40: every arm exit 0 and printed the ledger' % img)
        say(f['spliced'] == a['spliced'], 'img%d: stale reads of the spliced image: A (decay off) %d, FIX %d (equal: the A arm stays at its own count)' % (img, a['spliced'], f['spliced']))
        say(b['spliced'] > a['spliced'], 'img%d: planted twin BLIND (the faulty rule): %d stale reads of the image against A\'s %d: CAUGHT' % (img, b['spliced'], a['spliced']))
        say(f['total'] <= a['total'], 'img%d: total stale reads FIX %d <= A %d (BLIND %d)' % (img, f['total'], a['total'], b['total']))
        say(f['decayed'] > 0, 'img%d: the decay still works in FIX: %d copies let go that the insert-only guard held (BLIND %d)' % (img, f['decayed'], b['decayed']))
    # THE RECOVERY (wwe2k14_rdr_run_end_keep): Brock, --skip-every 2, img1216 (1024x512, sampled + lasting) has a copy run that ends on skipped frame 297 and is never written again. OFF: a run-end episode, the image
    # stays one frame old (owed and read at the end). ON: the copy is kept (the next frame has no copy into the image): 0 run-end episodes, nothing owed at the end, the image is written at 297 and holds. PLANT (the next frame's copy table
    # ignored): the shipped behaviour again. NOT a bar: img1216 equal to the full bind byte for byte: its SOURCE img1287 (a lasting render target) is not equal to the full bind in any skipped frame (295 included), the drift is the
    # source's per-frame passes, not the copy.
    arms = {}
    for tag, extra in (('OFF', []), ('ON', ['--run-end-keep', 'on']), ('PLANT', ['--run-end-keep', 'on', '--run-end-peek-ignored'])):
        q = subprocess.run([exe, '--capture', cap, '--frames', '341', '--validation', '1', '--skip-every', '2', '--skip-hash', '341', '--hash-img', '1216'] + extra, capture_output=True, text=True, timeout=1500, errors='replace')
        text = (q.stdout or '') + (q.stderr or '')
        m = re.search(r'\[rplay\]\[stale\] run-end keep [^,]*\(?[^)]*\)?, stale frames \(per read\) (\d+), owed and read at the end (\d+); total (\d+):.*?copy run ended on a skipped frame (\d+)', text)
        kept = re.search(r'copies kept (\d+)', text)
        hashes = dict((int(a_), h_) for a_, h_ in re.findall(r'imghash\] f=(\d+) img=1216 hash=([0-9a-f]+)', text))
        arms[tag] = dict(rc=q.returncode, frames=int(m.group(1)) if m else -1, open=int(m.group(2)) if m else -1, run_end=int(m.group(4)) if m else -1, kept=int(kept.group(1)) if kept else -1, hashes=hashes)
    o, n, pl = arms['OFF'], arms['ON'], arms['PLANT']
    say(all(x['rc'] == 0 and x['run_end'] >= 0 for x in arms.values()), 'run end on Brock: every arm exit 0 and printed the split')
    say(o['run_end'] > 0 and o['open'] >= 1, 'run end: OFF (shipped): %d run-end episodes, %d image owed and read at the end: the shipped stale picture re-found' % (o['run_end'], o['open']))
    say(n['run_end'] == 0 and n['open'] == 0 and n['kept'] > 0, 'run end: ON: %d run-end episodes, %d owed at the end, %d copies kept' % (n['run_end'], n['open'], n['kept']))
    say(pl['run_end'] == o['run_end'] and pl['open'] == o['open'] and pl['kept'] == 0, 'run end: planted twin (next frame ignored): %d episodes, %d open, %d kept: the shipped behaviour again: CAUGHT' % (pl['run_end'], pl['open'], pl['kept']))
    late = [f for f in sorted(o['hashes']) if f >= 299]
    say(len(late) > 0 and all(n['hashes'].get(f) != o['hashes'].get(f) for f in late) and len(set(n['hashes'][f] for f in late)) == 1, 'run end: img1216 after the run end (%d skipped frames from 299): ON holds the kept copy (differs from OFF in all, constant), PLANT equals OFF: %s' % (
        len(late), all(pl['hashes'].get(f) == o['hashes'].get(f) for f in late)))
    # ROUND 3 BARS. (1) --skip-every 298: only frame 297 (the run end of img1216 and img1287) is skipped. With the recovery ON the two images are BYTE-EQUAL to the full bind at 297 (group and nogroup), OFF is not.
    # The earlier "separate shipped divergence" was an rplay artefact (rplay has no A1b catch-up; live catches up img1216's level-0 copy); the real shipped hole is the run end itself.
    h298 = {}
    for tag, extra in (('truth', ['--skip-prune', 'truth']), ('OFF', []), ('ON', ['--run-end-keep', 'on']), ('ON_nogroup', ['--run-end-keep', 'on', '--lasting-nogroup', 'on'])):
        q = subprocess.run([exe, '--capture', cap, '--frames', '300', '--validation', '1', '--skip-every', '298', '--skip-hash', '300', '--hash-img', '1216,1287'] + extra, capture_output=True, text=True, timeout=900, errors='replace')
        text = (q.stdout or '') + (q.stderr or '')
        h298[tag] = dict((int(i_), h_) for f_, i_, h_ in re.findall(r'imghash\] f=(\d+) img=(\d+) hash=([0-9a-f]+)', text) if int(f_) == 297)
    t = h298['truth']
    say(len(t) == 2 and h298['ON'] == t and h298['ON_nogroup'] == t, 'skip-every 298: img1216 / img1287 at the run end 297 with the recovery ON (group and nogroup) are byte-equal to the full bind: %s' % ({k: v for k, v in h298['ON'].items()}))
    say(h298['OFF'].get(1216) != t.get(1216), 'skip-every 298: OFF img1216 %s != the full bind %s: the shipped wrong picture re-found' % (h298['OFF'].get(1216), t.get(1216)))
    # (2) the drawn frames' pictures 118-340 under skip-every 2: ON differs from the full bind only where OFF also differs (and in fewer frames)
    shots = ','.join(str(f_) for f_ in range(118, 341, 2))
    pics = {}
    for tag, extra in (('truth', ['--skip-prune', 'truth']), ('OFF', []), ('ON', ['--run-end-keep', 'on'])):
        sd = os.path.join(out, 'shots_' + tag)
        shutil.rmtree(sd, ignore_errors=True)
        os.makedirs(sd)
        q = subprocess.run([exe, '--capture', cap, '--frames', '341', '--validation', '1', '--skip-every', '2', '--shot-dir', sd, '--shots', shots, '--out', os.path.join(sd, 'last.png')] + extra, capture_output=True, text=True, timeout=1800, errors='replace')
        pics[tag] = {}
        for f_ in range(118, 341, 2):
            fp = os.path.join(sd, 'frame_%04d.png' % f_)
            if os.path.isfile(fp):
                with open(fp, 'rb') as fh:
                    pics[tag][f_] = fh.read()
    frames = sorted(pics['truth'])
    off_d = [f_ for f_ in frames if pics['OFF'].get(f_) != pics['truth'][f_]]
    on_d = [f_ for f_ in frames if pics['ON'].get(f_) != pics['truth'][f_]]
    say(len(frames) > 100 and len(off_d) > 0 and set(on_d) <= set(off_d) and len(on_d) < len(off_d), 'drawn pictures 118-340 (%d frames) against the full bind: OFF differs at %s, ON at %s (ON only where OFF also differs, and in fewer frames)' % (len(frames), off_d, on_d))
    print('cadence_check: %d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
