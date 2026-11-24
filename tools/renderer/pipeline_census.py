#!/usr/bin/env python3
"""S106 PSEED: the pipeline census driver. Draws the player's own files through the offline rig with the renderer's own pipeline store ON, so the stored list IS the census (the real
keys: no second copy of Record's key logic). 0 game runs: arena_rec.exe + frame_rig.exe, headless 160x90, at most 2 rig processes alive (the rig is the shared PC's: never more).
Arms (docs\\research\\s106\\PSEED.md "What to build"):
  a  every match arena x {entrance, match} looks (arena_rec --from-provider --scene-word 4 / 1: the look AND phase1 the game's own rows give)
  b  the same with the map arms the game has at run time but a stored record may lack: frame_rig --census-maps N (bit 1 shadow, 2 props, 4 plane; each separate: C7)
  d  roster characters told into a template capture record (--link actor:slot:N=CH:AT --body-scale-one) with the pass mode word 0 and 2 (--census-char-mode) x the map arms
  (c, the GPU-skinned ring pairs, needs ring rows at rest in the record: NOT built, see the note; e, the fixed families, come from harvested run lists.)
Each job writes its own store folder DIR\\jobs\\<tag>\\pipelines.bin (a store folder is one process's); merge them with
  py -I tools\\renderer\\pipeline_seed.py merge --out db\\shaders\\shader_list.txt --catalog CATALOG.txt DIR\\jobs\\*\\pipelines.bin
  py -I tools\\renderer\\pipeline_census.py --out out\\pipeseed\\census [--arenas 4,5] [--looks entrance,match] [--maps 0,1,2,4,7] [--chars 343:0] [--template RSC] [--dry]
A non-dry run first empties DIR\\jobs (N2: no folder of an earlier run joins the merge), and each job's store folder is recreated empty (F2). --selftest checks that. --dry prints the jobs and exits. Exit 0 = every job ran, 1 = a job failed (named). Fails loudly when a job's store holds 4096 records (StoreSave would trim it)."""
import argparse
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'harvest'))
sys.path.insert(0, HERE)
import choice_ours   # noqa: E402  (MATCH_ARENAS, SCENE_WORD, main_checkout: the rig's own arena list and look words)
import pipeline_seed   # noqa: E402  (decode_list: the stored list's reader)

CAMERA = '0.001,0,0,0,0,0.001,0,0,0,0,0.0001,0,0,0,0.5,1'   # the choice rig's: no chunk is culled, a small fixed one keeps the draw cheap


def jobs(arenas, looks, maps, chars, template):
    out = []
    for ar in arenas:
        for lk in looks:
            for m in maps:
                out.append(dict(tag='a%02d_%s_m%d' % (ar, lk, m), arena=ar, look=lk, maps=m, chars=None, mode=-1))
    if chars and template:
        for ch in chars:
            for mode in (0, 2):
                for m in (0, 1):
                    out.append(dict(tag='c%s_mode%d_m%d' % (ch.replace(':', '_'), mode, m), arena=None, look='match', maps=m, chars=ch, mode=mode, template=template))
    return out


def fresh_store(out_dir, tag):
    """S106 PSEED fix F2: a census job starts on an EMPTY store folder. A reused folder makes frame_rig's StoreInit load the old pipelines.bin, whose records age out only after 12
    substantial sessions: a census re-run after a cull or state change would keep the old keys and the merger would ship keys no current draw makes. Only DIR\\jobs\\<tag> is ever
    deleted. Returns the folder; raises when it is not empty afterwards."""
    jobs_dir = os.path.abspath(os.path.join(out_dir, 'jobs'))
    store = os.path.abspath(os.path.join(jobs_dir, tag))
    if os.path.dirname(store) != jobs_dir or not tag or tag in ('.', '..'):
        raise ValueError('census store %r is not a folder directly under %s' % (store, jobs_dir))
    if os.path.isdir(store):
        shutil.rmtree(store)
    os.makedirs(store)
    if os.listdir(store):
        raise RuntimeError('census store %s is not empty' % store)
    return store


def clear_jobs(out_dir):
    """S106 PSEEDF2 N2: a non-dry census starts with DIR\\jobs empty. Job folders of an earlier run (another --arenas / --maps) would otherwise stay: the merge glob (DIR\\jobs\\*\\pipelines.bin)
    and pseed_proof.load_census read every folder there, so old keys would join this census. Only DIR\\jobs is deleted."""
    jobs_dir = os.path.abspath(os.path.join(out_dir, 'jobs'))
    if os.path.basename(jobs_dir) != 'jobs' or os.path.dirname(jobs_dir) != os.path.abspath(out_dir):
        raise ValueError('census jobs folder %r is not DIR\\jobs' % jobs_dir)
    if os.path.isdir(jobs_dir):
        shutil.rmtree(jobs_dir)


def selftest():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        store = os.path.join(d, 'jobs', 'a04_match_m0')
        os.makedirs(store)
        for n in ('pipelines.bin', 'pipeline_cache.bin'):
            with open(os.path.join(store, n), 'wb') as f:
                f.write(b'old keys')
        got = fresh_store(d, 'a04_match_m0')
        ok = got == os.path.abspath(store) and os.listdir(store) == []
        print(('ok   ' if ok else 'FAIL ') + 'fresh_store: a store folder holding an old pipelines.bin / pipeline_cache.bin starts empty')
        try:
            fresh_store(d, '..')
            refused = False
        except ValueError:
            refused = True
        print(('ok   ' if refused else 'FAIL ') + 'fresh_store: a tag that leaves DIR\\jobs is refused (nothing outside it is ever deleted)')
        os.makedirs(os.path.join(d, 'jobs', 'old_arena_tag'))
        with open(os.path.join(d, 'jobs', 'old_arena_tag', 'pipelines.bin'), 'wb') as f:
            f.write(b'keys of an earlier --arenas run')
        clear_jobs(d)
        gone = not glob.glob(os.path.join(d, 'jobs', '*', 'pipelines.bin'))
        print(('ok   ' if gone else 'FAIL ') + 'N2: clear_jobs removes the job folders of an earlier run (the merge glob and pseed_proof.load_census would read them)')
        try:
            clear_jobs(os.path.join(d, 'jobs', 'x'))
            os.makedirs(os.path.join(d, 'other'))
            clear_jobs(os.path.join(d, 'other'))
            ok2 = True
        except Exception:
            ok2 = False
        print(('ok   ' if ok2 else 'FAIL ') + 'N2: clear_jobs on a folder with no jobs\\ is a no-op')
        return 0 if ok and refused and gone and ok2 else 1


def run_job(j, out_dir, game, tools):
    store = fresh_store(out_dir, j['tag'])   # F2: never reuse a store (the old keys would join the census)
    work = os.path.join(out_dir, 'work')
    os.makedirs(work, exist_ok=True)
    rsc = j.get('template')
    if j['arena'] is not None:
        rsc = os.path.join(work, j['tag'] + '.rsc')
        p = subprocess.run([os.path.join(tools, 'arena_rec.exe'), '--game', game, '--arena', str(j['arena']), '--from-provider', '--scene-word', str(choice_ours.SCENE_WORD[j['look']]),
                            '--camera-from', 'none', '--out', rsc], capture_output=True, text=True, errors='replace', timeout=300)
        if p.returncode != 0:
            return j['tag'], 'arena_rec exit %d: %s' % (p.returncode, (p.stdout + p.stderr).strip()[-300:])
    cmd = [os.path.join(tools, 'frame_rig.exe'), '--record', rsc, '--game', game, '--size', '160x90', '--prefix', os.path.join(work, j['tag']), '--camera', CAMERA,
           '--pipeline-store', store, '--census-maps', str(j['maps'])]
    if j['look'] == 'entrance':
        cmd.append('--look-entrance')
    if j['chars']:
        cmd += ['--link', 'actor:slot:0=' + j['chars'], '--body-scale-one', '--census-char-mode', str(j['mode'])]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=600)
    for f in os.listdir(work):
        if f.startswith(j['tag'] + '_') and f.endswith('.png') or f.endswith(j['tag'] + '.rsc'):
            os.remove(os.path.join(work, f))
    if r.returncode != 0:
        return j['tag'], 'frame_rig exit %d: %s' % (r.returncode, (r.stdout + r.stderr).strip()[-300:])
    lst = os.path.join(store, 'pipelines.bin')
    if not os.path.exists(lst):
        return j['tag'], 'no pipelines.bin was written (the renderer store was not on: is frame_rig.exe built from this tree?)'
    n = len(pipeline_seed.decode_list(lst))
    if n >= 4096:
        return j['tag'], 'the store holds %d records: StoreSave would trim the oldest (kMaxRecords 4096)' % n
    return j['tag'], None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'pipeseed', 'census'))
    ap.add_argument('--arenas', default='all')
    ap.add_argument('--looks', default='entrance,match')
    ap.add_argument('--maps', default='0,1,2,4,7')
    ap.add_argument('--chars', default='343:0')
    ap.add_argument('--template', default='')
    ap.add_argument('--game')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    arenas = choice_ours.MATCH_ARENAS if a.arenas == 'all' else [int(x) for x in a.arenas.split(',')]
    chars = [c for c in a.chars.split(',') if c]
    template = a.template or os.path.join(choice_ours.main_checkout(), 'out', 'captures', 'brock_losswin', 'scene', 'frame_109.rsc')
    js = jobs(arenas, a.looks.split(','), [int(x) for x in a.maps.split(',')], chars, template if os.path.exists(template) else '')
    if chars and not os.path.exists(template):
        print('note: no template record at %s: the roster arm (d) is skipped (pass --template RSC)' % template)
    print('pipeline_census: %d jobs, 2 rig processes at most' % len(js))
    if a.dry:
        for j in js:
            print('  ' + j['tag'])
        return 0
    game = a.game or os.path.join(choice_ours.main_checkout(), 'game_data', 'extracted')
    tools = os.path.join(ROOT, 'out', 'tools')
    clear_jobs(a.out)   # N2
    bad = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
        for tag, why in ex.map(lambda j: run_job(j, a.out, game, tools), js):
            if why:
                print('FAIL %s: %s' % (tag, why))
                bad.append(tag)
    print('pipeline_census: %d jobs, %d failed -> %s' % (len(js), len(bad), os.path.join(a.out, 'jobs')))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
