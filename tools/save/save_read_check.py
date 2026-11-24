"""S106 C1 proof for tools/save/save_read.py + tools/q/qsave.py (`q save`). 0 game runs; reads the main checkout's mirrors read-only.

  py -I tools/save/save_read_check.py      exit 0 = every arm is as expected

Past faults re-found on the real mirrors (the S105 chain: run 185, the caption runs 104 / 122 / 126 / 131 / 133, the importer TEST mirror, T4):
  title 1 World Heavyweight held by 322 with a plated belt, title 0 (Cena) plateless; caption word 0 on userdata_frozen; DLC mask 0x7C on
  userdata_mask7c_s105 and 0 on frozen; CAW slot 0 on userdata_caw_s105; Cena Threads set 0 on userdata_threads_s105.
Planted twins in the UNSAFE direction (each must flip the verdict): a TEST mirror with the caption word 1 / blood 1 (made by make_test_save.py),
  mask 0x40, title 1's holder changed to 139, a reader that skips +0x1C, a reader that ignores the roster-row rule (an empty id's word 0 would count).
`q save check` must exit 1 on a false precondition (the S105 run 185 case: "the champion whose belt has a plate" = Cena is refused).
Scratch: out\\c1\\ of this worktree (test mirrors made by make_test_save.py from the frozen mirror, which is only read).
"""
# entry: save_read.Save / qsave.eval_expr / qsave.main (q save check)
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'q'))
import save_read as S  # noqa: E402
import qsave as Q  # noqa: E402
import make_test_save as M  # noqa: E402
import unlock_all as U  # noqa: E402

BENCH = os.path.join(S.MAIN, 'out', 'bench')
FROZEN = os.path.join(BENCH, 'userdata_frozen')
SCRATCH = os.path.join(S.REPO, 'out', 'c1')
FAILS = []


def arm(name, ok, detail=''):
    print('%s  %s  %s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        FAILS.append(name)


def ev(sv, expr):
    try:
        return Q.eval_expr(sv, expr)[0]
    except Q.ExprError as e:
        return 'ERR %s' % e


def sanity(sv):
    """The reader's own sanity test (the t0 record): kind 1, belt 2004, holder 139. A reader off by the 0x1C header cannot pass it."""
    try:
        t = {x['title']: x for x in sv.titles()}
        return t[0]['kind'] == 1 and t[0]['belt'] == 2004 and t[0]['holders'][0] == 139 and sv.title_count() == 91
    except (IndexError, KeyError, struct.error):
        return False


def make(name, *args):
    dst = os.path.join(SCRATCH, name)
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'make_test_save.py'), '--src', FROZEN, '--dst', dst, '--force', '--threads', 'none', *args],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-400:], r.stderr[-400:])
    return dst, r.returncode


def main():
    if os.path.isdir(SCRATCH):
        shutil.rmtree(SCRATCH)
    os.makedirs(SCRATCH)
    fz = S.Save('userdata_frozen')
    arm('frozen sanity (t0 kind 1 belt 2004 holder 139, 91 titles)', sanity(fz))
    # ---- the five re-finds on the real mirrors
    arm('t1 held by 322 (Del Rio)', ev(fz, 'holder:title=1:char=322') is True)
    arm('t1 belt 2009 has a plate', ev(fz, 'plate:title=1') is True and S.has_plate(2009) is True)
    arm('t0 (Cena) belt has no plate', ev(fz, 'noplate:title=0') is True and ev(fz, 'plate:title=0') is False)
    # S106 C1F F4: t17's belt has a plate, a holder, and a save-byte lock (id 115 off): the DLC manager may still unlock it -> unreadable, not "no plate"
    arm('locked belt t17 (unlock id 115 off, plate in the index): plate / noplate are unreadable (F4)',
        str(ev(fz, 'noplate:title=17')).startswith('ERR') and str(ev(fz, 'plate:title=17')).startswith('ERR'))
    arm('caption word 0 on frozen', fz.options()['caption'] == 0 and ev(fz, 'option:caption=0') is True and ev(fz, 'option:caption=1') is False)
    arm('blood 0 on frozen', fz.options()['blood'] == 0)
    arm('dlc mask 0 on frozen', fz.dlc_mask()['mask'] == 0 and ev(fz, 'dlc:mask==0') is True)
    m7 = S.Save('userdata_mask7c_s105')
    arm('dlc mask 0x7C on mask7c mirror', m7.dlc_mask()['mask'] == 0x7C and ev(m7, 'dlc:mask&0x40') is True and ev(fz, 'dlc:mask&0x40') is False)
    cw = S.Save('userdata_caw_s105')
    cs = cw.caws()
    arm('CAW slot 0 THE SUPERSTAR on caw mirror', len(cs) == 1 and cs[0]['slot'] == 0 and cs[0]['name'] == 'THE SUPERSTAR' and cs[0]['package'] is True
        and ev(cw, 'caw:count>=1') is True and ev(fz, 'caw:count>=1') is False)
    th = S.Save('userdata_threads_s105')
    arm('Cena set 0 on threads mirror, none on frozen', ev(th, 'threads:char=139:set=0') is True and ev(fz, 'threads:char=139') is False and ev(fz, 'threads:count=0') is True)
    # ---- planted twins, unsafe direction: each must flip the verdict
    d, rc = make('cap1', '--caption', '1', '--blood', '1')
    c1 = S.Save(d) if rc == 0 else None
    arm('twin caption 1 / blood 1 (make_test_save --caption --blood): reader says ON, check flips',
        c1 is not None and c1.options()['caption'] == 1 and c1.options()['blood'] == 1 and ev(c1, 'option:caption=1') is True and ev(c1, 'option:blood=1') is True
        and ev(fz, 'option:caption=1') is False and c1.options()['s_block'] != fz.options()['s_block'])
    d, rc = make('cap0', '--caption', '0', '--blood', '0')
    arm('twin caption 0 / blood 0 round trip', rc == 0 and S.Save(d).options()['caption'] == 0 and ev(S.Save(d), 'option:blood=0') is True)
    d, rc = make('mask40', '--mask', '40')
    mk = S.Save(d) if rc == 0 else None
    arm('twin mask 0x40: the bit check flips from false to true', mk is not None and mk.dlc_mask()['mask'] == 0x40 and ev(mk, 'dlc:mask&0x40') is True
        and ev(mk, 'dlc:mask&0x20') is False)
    # holder changed: rewrite title 1's +156 holder to 139 (offset formula written out again, not the reader's)
    data, word, payload, hdr = U.read_payload(U.find_save(FROZEN))
    p = bytearray(payload)
    struct.pack_into('>I', p, 0x1C + 0x112828 + 676 * 1 + 156, 139)
    M.finish(p)
    path = U.find_save(os.path.join(SCRATCH, 'cap0'))
    with open(path, 'wb') as f:
        f.write(M.pack_save(data, word, p))
    hv = S.Save(os.path.join(SCRATCH, 'cap0'))
    arm('twin holder 139 on title 1: holder:title=1:char=322 FAILS, char=139 passes, plate:title=1 still true (belt unchanged)',
        ev(hv, 'holder:title=1:char=322') is False and ev(hv, 'holder:title=1:char=139') is True and ev(hv, 'holder:title=1:char!=322') is True)
    # reader skipping +0x1C: the sanity test must fail
    bad = S.Save('userdata_frozen', blob_skip=0x1C)
    arm('twin reader skips +0x1C: sanity test fails', sanity(bad) is False and sanity(fz) is True)
    # reader ignoring the roster-row rule: empty ids (word 0, not -1) would count as sets
    real = S.Save.real_ids
    try:
        S.Save.real_ids = lambda self: list(S.CHARS)
        naive = S.Save('userdata_frozen')
        n_sets = len(naive.threads())
        verdict = ev(naive, 'threads:count=0')
    finally:
        S.Save.real_ids = real
    arm('twin reader without the roster rule: empty ids read 0 -> phantom sets, threads:count=0 flips to false', n_sets > 0 and verdict is False
        and ev(fz, 'threads:count=0') is True, 'phantom chars %d' % n_sets)
    # ---- q save check exit codes (the precondition check)
    q = os.path.join(os.path.dirname(HERE), 'q', 'q.py')

    def run(*exprs, data=None):
        a = [sys.executable, '-I', q, 'save', 'check', *exprs] + (['--data', data] if data else [])
        return subprocess.run(a, capture_output=True, text=True).returncode
    arm('q save check exit 0 when all true', run('holder:title=1:char=322', 'plate:title=1', 'option:caption=0', 'dlc:mask==0') == 0)
    arm('q save check exit 1: "the champion whose belt has a plate" = Cena (S105 run 185)', run('plate:title=0') == 1 and run('holder:title=0:char=139', 'plate:title=0') == 1)
    arm('q save check exit 1 when one of several is false', run('plate:title=1', 'option:caption=1') == 1)
    arm('q save check exit 1 on the unsafe twin mirror (mask 0x40 asked of the frozen)', run('dlc:mask&0x40') == 1 and run('dlc:mask&0x40', data=os.path.join(SCRATCH, 'mask40')) == 0)
    arm('q save check exit 2 on a malformed expression', run('bogus:x=1') == 2)
    exit_rows(q)  # S106 C1F: the review's inputs; every one an exit 2 (or the named 0 / 1)
    shutil.rmtree(SCRATCH, ignore_errors=True)
    print('save_read_check: %s' % ('ALL PASS' if not FAILS else 'FAILED: %s' % '; '.join(FAILS)))
    return 1 if FAILS else 0


def exit_rows(q):
    """S106 C1F: `q save check` never exits 0 on an input it did not fully understand (review_C1.md B1-B4, F1-F3)."""
    env_nodb = dict(os.environ, WWE_QASSET_DB=os.path.join(SCRATCH, 'no_such_asset.db'))

    def rc(exprs, data=None, env=None, raw=None):
        a = [sys.executable, '-I', q, 'save', 'check', *exprs] + (['--data', data] if data is not None else []) + (raw or [])
        return subprocess.run(a, capture_output=True, text=True, env=env).returncode
    thr, caw = os.path.join(BENCH, 'userdata_threads_s105'), os.path.join(BENCH, 'userdata_caw_s105')

    def rows(name, want, exprs_list, data=None, env=None):
        got = {e: rc([e], data, env) for e in exprs_list}
        good = all(v == want for v in got.values())
        arm(name, good, '' if good else str(got))
    # B1: a title that does not exist / has kind 0 (also with the asset index missing)
    rows('B1 title outside the table or kind 0 -> exit 2', 2, ['holder:title=91:char!=139', 'holder:title=50:char!=139', 'noplate:title=91',
         'noplate:title=999', 'plate:title=999', 'holder:title=46:char=1', 'holder:title=-1:char=1'])
    rows('B1 same with the asset index missing -> exit 2', 2, ['noplate:title=91', 'noplate:title=999'], env=env_nodb)
    # B2: operators / keys parsed then ignored
    rows('B2 threads char!= / set!= -> exit 2 (threads mirror)', 2, ['threads:char!=139', 'threads:char=139:set!=0', 'threads:counts>=0', 'threads:char>=139'], data=thr)
    rows('B2 plate:title!=1, caw:slot>=5, repeated / unknown keys -> exit 2', 2, ['plate:title!=1', 'caw:slot>=5', 'holder:title=1:char=139:char=322',
         'holder:title=1:char=322:bogus=1', 'holder:title=1:title=2:char=1', 'caw:counts>=1', 'option:captions=0', 'dlc:masks==0', 'holder:title!=1:char=1'])
    arm('B2 the right forms still work: threads:char=139:set=0 -> 0 on the threads mirror, caw:slot!=5 -> 0',
        rc(['threads:char=139:set=0', 'threads:count>=1'], thr) == 0 and rc(['caw:slot!=5', 'caw:slot=0'], caw) == 0)
    # B3: char=512 is not "vacant"
    rows('B3 holder:title=5:char=512 -> exit 2', 2, ['holder:title=5:char=512', 'holder:title=5:char!=512'])
    arm('B3 holder:title=T:vacant: title 2 vacant -> 0, title 5 (held by 367) -> 1', rc(['holder:title=2:vacant']) == 0 and rc(['holder:title=5:vacant']) == 1)
    # B4: a used slot whose package is missing is wiped at boot: not counted
    d = os.path.join(SCRATCH, 'caw_nopkg')
    shutil.copytree(caw, d)
    for root, dirs, _ in os.walk(d):
        for x in list(dirs):
            if x.endswith('CustomSuperStar.cas'):
                shutil.rmtree(os.path.join(root, x))
    arm('B4 CAW slot used but its .cas package missing: caw:count>=1 is false (exit 1), caw:count=0 true; caw:slot=0 false',
        rc(['caw:count>=1'], d) == 1 and rc(['caw:count=0'], d) == 0 and rc(['caw:slot=0'], d) == 1 and rc(['caw:count>=1'], caw) == 0)
    # F1 / F2: crashes and an empty --data are exit 2
    arm('F1 --data with no value / option:s_block>=1 -> exit 2', rc(['option:caption=0'], raw=['--data']) == 2 and rc(['option:s_block>=1']) == 2)
    arm('F2 --data "" -> exit 2 (never the frozen mirror)', rc(['option:caption=0'], data='') == 2)
    bad = os.path.join(SCRATCH, 'corrupt')
    shutil.copytree(FROZEN, bad)
    sp = U.find_save(bad)
    raw = open(sp, 'rb').read()
    with open(sp, 'wb') as f:
        k = U.PREFIX + 100
        f.write(raw[:k] + bytes(b ^ 0xFF for b in raw[k:k + 200]) + raw[k + 200:])
    arm('F1 a corrupt save file -> exit 2', rc(['option:caption=0'], bad) == 2)
    # F3: a save with another layout
    syn = bytearray(0x2000)
    struct.pack_into('>II', syn, 0, 0x1000, 0x800)
    try:
        S.Save(payload=bytes(syn))
        lay = 'no refusal'
    except SystemExit:
        lay = True
    arm('F3 a payload with another layout (sizes 0x1000 / 0x800) is refused', lay is True, str(lay))
    # C3 sites 6-7: --holder writes title T's holders; refuses T outside 0..90 and kind-0 titles
    mk = os.path.join(HERE, 'make_test_save.py')
    hd = os.path.join(SCRATCH, 'holder0')
    r = subprocess.run([sys.executable, '-I', mk, '--src', FROZEN, '--dst', hd, '--holder', '0:322'], capture_output=True, text=True)
    hs = S.Save(hd) if r.returncode == 0 else None
    arm('--holder 0:322 TEST mirror: title 0 reads holder 322 (check true, 139 false), frozen still 139',
        hs is not None and ev(hs, 'holder:title=0:char=322') is True and ev(hs, 'holder:title=0:char=139') is False and hs.threads() == []
        and ev(S.Save('userdata_frozen'), 'holder:title=0:char=139') is True and rc(['holder:title=0:char=322'], hd) == 0, 'rc %d' % r.returncode)
    bad = [subprocess.run([sys.executable, '-I', mk, '--src', FROZEN, '--dst', os.path.join(SCRATCH, 'hbad'), '--force', '--holder', t],
                          capture_output=True, text=True).returncode for t in ('91:1', '46:1', '-1:1')]
    arm('--holder refuses title 91 / kind-0 title 46 / -1', all(x != 0 for x in bad), str(bad))
    # F5: --caption with no --threads plants no Threads set; with no --dst it refuses (never the keep-listed userdata_threads_s105)
    mk = os.path.join(HERE, 'make_test_save.py')
    r = subprocess.run([sys.executable, '-I', mk, '--src', FROZEN, '--caption', '1'], capture_output=True, text=True)
    arm('F5 make_test_save --caption 1 with no --dst refuses (exit != 0, and the keep-listed s105 mirror is not the target)',
        r.returncode != 0 and not os.path.isdir(os.path.join(S.REPO, 'out', 'bench', 'userdata_threads_s105')), 'rc %d' % r.returncode)
    dd = os.path.join(SCRATCH, 'cap_nothreads')
    r = subprocess.run([sys.executable, '-I', mk, '--src', FROZEN, '--dst', dd, '--caption', '1'], capture_output=True, text=True)
    arm('F5 make_test_save --caption 1 --dst X (no --threads): no Threads set planted', r.returncode == 0 and S.Save(dd).threads() == []
        and S.Save(dd).options()['caption'] == 1, 'rc %d' % r.returncode)


if __name__ == '__main__':
    sys.exit(main())
