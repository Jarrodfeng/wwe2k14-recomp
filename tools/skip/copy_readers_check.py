r"""Lane 183 (S101) acceptance test of tools\skip\copy_readers.py. Offline, 0 game runs, headless fview only (the fates). docs\renderer\SKIP_GAME_WORK_MAP.md section 14.

  py -I tools\skip\copy_readers_check.py [--quick]

Bars (each prints PASS / FAIL; a check that cannot read its input fails loudly):
  1. selftest        copy_readers.py --selftest (verdict logic and the record plants).
  2. census          Check B frames 200..204 (run 7051): >= 80 copies a frame; the 20 crowd bake copies a frame are ALL read by ours only (crowd_ours / crowd_injected, the cards) and are
                     elidable; the yPreLight copies are mostly unread (the dead bakes dead_passes.py found); no draw has an image-0 slot.
  3. null self-test  the SAME capture with one extra game-replayed draw planted on the first bake copy's destination (copy_readers.py --plant node_reader, the scan's own draw path):
                     exactly one elidable copy flips to blocked, nothing else changes.
  4. past-bug re-find (the S100 07:15 / 11:39 nulled-stage fault, docs\renderer\TEXTURE_COHERENCE.md 6 + POST_PLAN.md 19): the old capture brock_tron holds draws whose texture slot is
                     image 0 (the post / shadow chain reads a copy destination through a nulled stage). The tool counts them as BLIND readers and judges NO copy there (0 elidable); Check B has none.
  5. cross-check        every yPreLight copy lane 175's dead_passes.py calls dead (same capture, same frames) has no reader here (this tool is the finer one: it ends a copy at the next
                     copy over its rect).
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import copy_readers as cr  # noqa: E402

CHECK_B = 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'
BROCK_TRON = 'out/captures/brock_tron'


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


class R:
    fails = 0

    @staticmethod
    def line(ok, text):
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        R.fails += 0 if ok else 1


def counts(cap, recs, frames, name):
    by = {}
    for r in recs:
        if r['frame'] == frames[-1]:
            continue
        if cr.name_copy(r['label'], r['dst_wh'], r['src_wh']) == name:
            v = cr.verdict(r)[0]
            by[v] = by.get(v, 0) + 1
    return by


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--quick', action='store_true', help='skip the brock_tron arm')
    a = ap.parse_args()
    main_co = main_checkout()
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'copy_readers.py'), '--selftest'], capture_output=True, text=True, timeout=120)
    R.line(p.returncode == 0 and p.stdout.count('PASS') >= 10 and 'FAIL' not in p.stdout, 'selftest (%d PASS)' % p.stdout.count('PASS'))

    z = os.path.join(main_co, CHECK_B)
    if not os.path.isfile(z):
        print('FAIL  input missing: ' + z)
        return 1
    frames = list(range(200, 204))
    cap = cr.cc.Cap(z)
    fates, promote = cr.run_fview(z, frames, os.path.join(ROOT, 'out', 's101', 'copy_readers', 'check_' + os.path.basename(os.path.dirname(z))))
    recs = cr.scan(cap, frames, fates)
    blind = sum(cr.LAST_BLIND.values())
    counted = [r for r in recs if r['frame'] != frames[-1]]
    nfr = len(frames) - 1
    R.line(len(counted) / nfr >= 80, 'Check B f200-203: %.1f copies a frame (>= 80)' % (len(counted) / nfr))
    bake = counts(cap, recs, frames, 'crowd bake')
    R.line(bake.get('elidable', 0) >= 18 * nfr and sum(bake.values()) == bake.get('elidable', 0), 'crowd bake copies all elidable (readers ours only): %s' % bake)
    prel = counts(cap, recs, frames, 'yPreLight bake')
    R.line(prel.get('no reader', 0) >= 0.8 * sum(prel.values()) > 0, 'yPreLight copies mostly unread: %s' % prel)
    R.line(blind == 0, 'Check B: draws with an image-0 slot: %d (expect 0)' % blind)

    n_el = sum(1 for r in counted if cr.verdict(r)[0] == 'elidable')
    cr.scan(cap, frames, fates, 'node_reader')   # (prime LAST_BLIND reset path)
    recs2 = cr.scan(cap, frames, fates, 'node_reader')
    counted2 = [r for r in recs2 if r['frame'] != frames[-1]]
    n_el2 = sum(1 for r in counted2 if cr.verdict(r)[0] == 'elidable')
    v1 = {r['id']: cr.verdict(r)[0] for r in counted}
    v2 = {r['id']: cr.verdict(r)[0] for r in counted2}
    changed = [i for i in v1 if v1[i] != v2.get(i)]
    R.line(n_el2 == n_el - 1 and len(changed) == 1 and v1[changed[0]] == 'elidable' and v2[changed[0]] == 'blocked',
           'null self-test: planted reader flips exactly one elidable copy to blocked (elidable %d -> %d, copies that changed: %d)' % (n_el, n_el2, len(changed)))

    # 5: dead_passes cross-check (same capture, same frames)
    import dead_passes as dp
    per = dp.run(cap, frames)
    dead_prelight = set()
    for fi, f, nodes, passes in per[:-1]:
        for n in nodes:
            if n['kind'] == 'copy' and not n['alive']:
                c = cr.cc.table(f, cr.cc.SEC_COPIES, cr.cc.COPY_DT)[int(f.ops[n['k']][1])]
                if cap.wh(int(c['s']['image'])) == (384, 64):
                    dead_prelight.add((fi, n['k']))
    mine = {(r['frame'], r['k']) for r in counted if cr.name_copy(r['label'], r['dst_wh'], r['src_wh']) == 'yPreLight bake' and cr.verdict(r)[0] == 'no reader'}
    R.line(len(dead_prelight) > 0 and dead_prelight <= mine,
           'dead yPreLight copies: every one of the %d that dead_passes.py calls dead is unread in copy_readers (which finds %d: it knows a copy is overwritten, dead_passes only a full clear)' % (len(dead_prelight), len(mine)))

    if not a.quick:
        bt = os.path.join(main_co, BROCK_TRON)
        if not os.path.isdir(bt):
            R.line(False, 'brock_tron capture missing: ' + bt)
        else:
            capb = cr.cc.Cap(bt)
            fr = [0, 1, 2]
            fates_b, _ = cr.run_fview(bt, fr, os.path.join(ROOT, 'out', 's101', 'copy_readers', 'check_brock_tron'))
            recb = cr.scan(capb, fr, fates_b)
            blind_b = sum(cr.LAST_BLIND.values())
            cb = [r for r in recb if r['frame'] != fr[-1]]
            el_b = sum(1 for r in cb if cr.verdict(r)[0] == 'elidable')
            R.line(blind_b >= 30 * len(fr) and el_b == 0, 'past bug (nulled stage), old capture brock_tron: %d image-0 draws (%s), %d copies judged elidable (expect 0)'
                   % (blind_b, ', '.join('%s %d' % kv for kv in cr.LAST_BLIND.most_common(3)), el_b))
    import shutil   # disk rule (S101 night): the unpacked captures (~600 MB each) are removed as soon as the check has read them
    shutil.rmtree(os.path.join(ROOT, 'out', 's101', 'copy_readers'), ignore_errors=True)
    print('%d failure(s)' % R.fails)
    return 1 if R.fails else 0


if __name__ == '__main__':
    sys.exit(main())
