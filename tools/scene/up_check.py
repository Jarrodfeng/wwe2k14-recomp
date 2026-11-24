#!/usr/bin/env python3
r"""Lane UPS's check (Session 103): the stage light-map page is ONE image per arena set, uploaded once per key change. 0 game runs: fview headless + frame_rig.exe on brock_losswin_rt (an entrance).

  py -I tools\scene\up_check.py [--out DIR] [--frames 20,60,100,140,180]

Arms on the same frames, each on its own frame_rig --serve process (the arena set stays built between frames, like the live game: the pools key changes frame to frame):
  after   : the shared page image (FrontEnd::AdoptPage), the shipped behaviour.
  before  : the RIG-ONLY planted twin WWE_RDR_PLANT_PAGE_PER_MODEL=1 (rdr_scene::g_plant_page_per_model, set by frame_rig.cpp only): every model keeps its private page copy and every key change is
            re-sent to each = the old fan-out. This arm IS the "before" number (copies and MB a frame).
  percaller: WWE_RDR_PAGE_PER_CALLER=1 = the rig's stand-in for the +dev switch wwe2k14_rdr_page_per_caller: every caller sends into the shared image (the old byte cost with ONE image).
  sigplant : sigchange + WWE_RDR_PLANT_PAGE_NO_REJOIN=1 (a crowd model stays in the retired group): the check must read >= 2 page groups.
  sigchange: WWE_RDR_SIG_CHANGE_SLOT=777: ONE job whose frame sequence changes the arena set's signature mid-run (a synthetic kFxSlotWorld row; the set is REBUILT after the 8-frame debounce into a
            new page group, the old one retired): review F1 / F2's twin.
Reads frame_rig's `up_counter` / `up_seq` lines (FrontEnd::texture_update_count / _bytes by caller: page / screen / other; the live page groups and the page-carrying models).
PASS needs:
  - after: every frame: at most 1 page-sized upload, < 1.5 MB of live texture uploads in all, ONE page group alive, every page-carrying model of the arena set on its group (carriers == on_group);
  - before (the planted twin): at least 2 page copies and 2 MB on its worst frame, 0 models on a shared image; the shipped arm has >= 2 models on one image;
  - percaller: the bytes of the per-model fan-out come back (>= the planted twin's page copies on every frame the key changed): the in-run switch restores the old byte cost;
  - sigchange: ONE page group on every frame of the rebuild sequence, carriers == on_group, and exactly ONE page upload for the rebuild (a new group, a new key);
  - the whole picture of every frame is byte-identical between after and before (max |diff| 0): only the binding is shared, never the pixels.
Exit 0 = all PASS. Writes up_numbers.json under --out (default out\s103\UP\gate).
"""
import argparse, contextlib, io, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
import frame_rig  # noqa: E402

PAGE_BYTES = 512 * 512 * 4
ENVS = ('WWE_RDR_PLANT_PAGE_PER_MODEL', 'WWE_RDR_PAGE_PER_CALLER', 'WWE_RDR_SIG_CHANGE_SLOT', 'WWE_RDR_PLANT_PAGE_NO_REJOIN')
UP_RE = re.compile(r'up_counter \w+ page_copies (\d+) page_bytes (\d+) screen_copies (\d+) screen_bytes (\d+) other_copies (\d+) other_bytes (\d+) page_skip_key (\d+) page_skip_hash (\d+) page_models (\d+) '
                   r'page_groups (\d+) page_carriers (\d+) page_on_group (\d+) ring_rest_page (\d+)')
UP_KEYS = ['page_copies', 'page_bytes', 'screen_copies', 'screen_bytes', 'other_copies', 'other_bytes', 'skip_key', 'skip_hash', 'models', 'groups', 'carriers', 'on_group', 'ring_rest']
SEQ_RE = re.compile(r'up_seq (\d+) page_copies (\d+) page_bytes (\d+) page_skip_key (\d+) page_skip_hash (\d+) page_models (\d+) page_groups (\d+) page_carriers (\d+) page_on_group (\d+) ring_rest_page (\d+)')
SEQ_KEYS = ['i', 'page_copies', 'page_bytes', 'skip_key', 'skip_hash', 'models', 'groups', 'carriers', 'on_group', 'ring_rest']


def main_checkout():
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    return env if env else (os.path.dirname(common) if common else ROOT)


def run_arm(capture, frames, out, env, repeat=1):
    """one arm on its own rig server: returns {frame: counters}, [up_seq rows], the stdout text, the exit code and the work dir"""
    for k in ENVS:
        os.environ.pop(k, None)
    for k, v in env.items():
        os.environ[k] = v
    frame_rig.SESSION = frame_rig.RigSession(serve=True, prep_root=os.path.join(out, 'prep'))
    argv = ['--capture', capture, '--frames', ','.join(map(str, frames)), '--out', out, '--look-from-stream', '--look-pools', '--mask', '0x1FF', '--repeat', str(repeat), '--no-sheet', '--no-family-png']
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        try:
            rc = frame_rig.main(argv)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 1
    for srv in frame_rig.SESSION.servers.values():   # the next arm must not meet this process
        try:
            srv.stdin.close()
            srv.wait(timeout=30)
        except Exception:
            srv.kill()
    for k in ENVS:
        os.environ.pop(k, None)
    text = buf.getvalue()
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'rig_text.txt'), 'w', encoding='utf8').write(text)
    work = os.path.join(out, 'work_' + os.path.basename(capture.rstrip('\\/')))
    res, seq = {}, []
    for f in frames:
        rp = os.path.join(work, 'ours_%d_report.txt' % f)
        body = open(rp, encoding='utf8', errors='replace').read() if os.path.exists(rp) else ''   # the job's own report only (never the whole stdout: a line from another job must not stand in)
        m = UP_RE.search(body)
        if m:
            res[f] = dict(zip(UP_KEYS, [int(x) for x in m.groups()]))
        seq += [dict(zip(SEQ_KEYS, [int(x) for x in sm.groups()]), frame=f) for sm in SEQ_RE.finditer(body)]
    return res, seq, text, rc, work


def load_png(path):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGBA'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    ap.add_argument('--frames', default='20,60,100,140,180')
    ap.add_argument('--capture', default='brock_losswin_rt')
    ap.add_argument('--sig-frame', type=int, default=100, help='the one frame the mid-run signature change job runs on')
    args = ap.parse_args()
    main_root = main_checkout()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('UP_gate', args.out)
    os.makedirs(out, exist_ok=True)
    frames = [int(x) for x in args.frames.split(',')]
    capture = os.path.join(main_root, 'out', 'captures', args.capture)
    fails = []

    def check(ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        if not ok:
            fails.append(text)

    arms = {}
    plan = (('after', frames, {}), ('before', frames, {'WWE_RDR_PLANT_PAGE_PER_MODEL': '1'}), ('percaller', frames, {'WWE_RDR_PAGE_PER_CALLER': '1'}),
            ('sigchange', [args.sig_frame], {'WWE_RDR_SIG_CHANGE_SLOT': '777'}),
            ('sigplant', [args.sig_frame], {'WWE_RDR_SIG_CHANGE_SLOT': '777', 'WWE_RDR_PLANT_PAGE_NO_REJOIN': '1'}))
    for tag, fr, env in plan:
        res, seq, text, rc, work = run_arm(capture, fr, os.path.join(out, tag), env)
        arms[tag] = (res, work, seq)
        for line in text.splitlines():
            if line.startswith('FAIL') and 'textur' not in line:
                check(False, '%s: %s' % (tag, line[:160]))
        check(rc == 0, '%s arm: frame_rig exit %s' % (tag, rc))
        check(len(res) == len(fr), '%s arm: an up_counter line for every frame (%d of %d)' % (tag, len(res), len(fr)))
        for f in fr:
            r = res.get(f)
            if r:
                print('INFO  %s frame %d: page %d copies %.2f MB, screen %d copies %.2f MB, other %d copies %.2f MB, page skips key %d same-bytes %d, groups %d, carriers %d on the group %d (ring-at-rest page carriers off it: %d)' % (
                    tag, f, r['page_copies'], r['page_bytes'] / 1048576.0, r['screen_copies'], r['screen_bytes'] / 1048576.0, r['other_copies'], r['other_bytes'] / 1048576.0, r['skip_key'], r['skip_hash'], r['groups'], r['carriers'], r['on_group'], r['ring_rest']))
    after, before, percaller = arms['after'][0], arms['before'][0], arms['percaller'][0]
    for f in frames:
        a = after.get(f)
        if not a:
            continue
        total = a['page_bytes'] + a['screen_bytes'] + a['other_bytes']
        check(a['page_copies'] <= 1, 'after frame %d: at most 1 page-sized UpdateTexture (%d)' % (f, a['page_copies']))
        check(total < 1.5 * 1048576, 'after frame %d: live texture uploads %.2f MB < 1.5 MB' % (f, total / 1048576.0))
        check(a['groups'] == 1, 'after frame %d: ONE page group alive (%d)' % (f, a['groups']))
        check(a['carriers'] >= 2 and a['carriers'] == a['on_group'], 'after frame %d: every page-carrying arena model is on the set\'s group (%d carriers, %d on it)' % (f, a['carriers'], a['on_group']))
    peak = max((v['page_bytes'] for v in before.values()), default=0)
    peak_n = max((v['page_copies'] for v in before.values()), default=0)
    a_models = min((v['models'] for v in after.values()), default=0)
    b_models = max((v['models'] for v in before.values()), default=0)
    a_n = max((v['page_copies'] for v in after.values()), default=0)
    check(peak_n >= 2 and peak_n >= 2 * max(1, a_n) - 1 and peak >= 2 * PAGE_BYTES and b_models == 0 and a_models >= 2, 'before (planted per-model twin): the fan-out reads %d page copies, %.1f MB on its worst frame, 0 models on a shared image (shipped: %d copies, %d models on it); the rig scene holds few page models: at the ~40 the Surface entrance may carry it is %d MB' % (peak_n, peak / 1048576.0, a_n, a_models, 40))
    # the +dev switch: the old byte cost returns (every caller sends) on every frame whose key changed
    for f in frames:
        b, p = before.get(f), percaller.get(f)
        if b and p and b['page_copies'] >= 2:
            check(p['page_copies'] >= b['page_copies'] - 1 and p['groups'] == 1, 'percaller frame %d: the in-run switch sends once per caller into the shared image (%d copies vs the old fan-out %d), one group' % (f, p['page_copies'], b['page_copies']))
    # the mid-run signature change (review F1 / F2)
    seq = arms['sigchange'][2]
    check(len(seq) == 14, 'sigchange: 14 frame lines (%d)' % len(seq))
    if seq:
        check(all(s['groups'] == 1 for s in seq), 'sigchange: ONE page group on every frame of the rebuild sequence (%s)' % ','.join(str(s['groups']) for s in seq))
        check(all(s['carriers'] >= 2 and s['carriers'] == s['on_group'] for s in seq), 'sigchange: every page-carrying arena model is on the set\'s group on every frame (carriers %s)' % ','.join('%d/%d' % (s['on_group'], s['carriers']) for s in seq))
        ups = [s['page_copies'] for s in seq]
        check(sum(ups) == 1 and all(u <= 1 for u in ups), 'sigchange: ONE page upload for the rebuild (a new group, a new key), none otherwise (%s)' % ','.join(map(str, ups)))
        rebuilt = [s['i'] for s in seq if s['page_copies'] == 1]
        check(bool(rebuilt) and rebuilt[0] >= 3, 'sigchange: the rebuild happened after the synthetic row (frame %s of 14)' % (rebuilt[0] if rebuilt else '-'))
    # the twin of F1: the crowd models (the rig carries ~60) stay in the retired group: the check must SEE two page groups
    pseq = arms['sigplant'][2]
    check(bool(pseq) and pseq[-1]['groups'] >= 2 and bool(seq) and seq[-1]['groups'] == 1, 'sigplant (planted: a model stays in the group it joined first): the check ends on >= 2 page groups where the shipped rebuild ends on 1 (%s)' % ','.join(str(x['groups']) for x in pseq))
    worst = 0
    for f in frames:
        pa = os.path.join(arms['after'][1], 'ours_%d_mask511.png' % f)
        pb = os.path.join(arms['before'][1], 'ours_%d_mask511.png' % f)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            check(False, 'frame %d: the whole picture is missing in an arm (%s / %s)' % (f, os.path.exists(pa), os.path.exists(pb)))
            continue
        ia, ib = load_png(pa), load_png(pb)
        d = 255 if ia.shape != ib.shape else int(abs(ia.astype('int16') - ib.astype('int16')).max())
        worst = max(worst, d)
        check(d == 0, 'frame %d: the whole picture is identical between the shared page and the per-model copies (max |diff| %d)' % (f, d))
    json.dump({'after': after, 'before': before, 'percaller': percaller, 'sigchange': seq, 'sigplant': pseq, 'max_pixel_diff': worst}, open(os.path.join(out, 'up_numbers.json'), 'w'), indent=1)
    print('up_check: %s (%d FAIL)' % ('PASS' if not fails else 'FAIL', len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
