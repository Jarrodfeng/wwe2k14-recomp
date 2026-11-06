"""Session 92 census: the sweep runner - one game per item, 3 lanes, resumable, never stopped by one bad item.

What it answers: "for each item (a six-man, an arena, a match type, a 30 Years match, a screen tour ...) is the whole
thing drawn by TRUE NATIVE (0 fallback passes, 0 lost clears), did it crash or hang, and what do its pictures look
like?" Each finished item leaves out\\census\\<pass>\\results\\<id>.json and its shots (jpg) under shots\\<id>\\.

  py tools\\census\\items.py                                  (once; writes out\\census\\items.json)
  py tools\\census\\run.py --pass pass1 [--lanes 3] [--only six_03,type_12 | --family roster] [--dry] [--redo]
                         [--redo-failed] [--exe PATH] [--keep-png]
  --dry          print what would run and what would be skipped (no game)
  --redo         run the chosen items again even if they have a result; --redo-failed: only error/stuck/crash ones
  --exe PATH     another build's wwe2k14.exe (logs, events and captures are read next to that exe; its flags get
                 --game_data_root)
Every game: TRUE NATIVE (--wwe2k14_d3d_owned=true), fast-forwarded (the recording item: normal speed), frozen unlocked
save, a fresh copy of the NATIVE 1440p settings file. Every wait has a timeout; the game is always closed or killed.
Follow-up 1v1 items (fu_<id>) are added when a six-man shows no entrance for a wrestler.
"""
import argparse
import glob
import json
import os
import queue
import random
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as C  # noqa: E402
from common import Crashed, Stuck, Unbuildable  # noqa: E402

PLOCK = threading.Lock()


def say_for(pdir, lane):
    def say(msg):
        line = '%s [%s] %s' % (C.now(), lane, msg)
        with PLOCK:
            print(line, flush=True)
            with open(os.path.join(pdir, 'progress.log'), 'a', encoding='utf-8') as f:
                f.write(line + '\n')
    return say


# ---------------------------------------------------------------- handlers (the game is started inside)

def match_spec(item):
    random.seed(item['id'])  # rumble=N random is drawn from the seeded generator
    return C.match.build_spec(item['spec'])[0]


def do_match(g, item, extra):
    spec = match_spec(item)
    extra['spec_numeric'] = spec
    g.start()  # start() adds the item's own flags (passing them again here would give CLI11 a flag twice)
    g.build_match(spec)
    hold = item.get('hold_frames', 1300)
    if item.get('entrances') == 'full':
        if not g.entrances_to_bell(900):
            raise Stuck('no bell within 15 min of entrances')
        shot_at = (180, 600, 1200)
    else:
        if not g.skip_to_bell():
            raise Stuck('START presses never reached the bell')
        shot_at = (180, 600, 1200)
    g.hold_match(hold, shot_at=shot_at, pair_at=600)


# Session 92: the game's own 30 Years drive (--wwe2k14_wm30_auto, src\wm30_jump.h) is an input script that waits for
# any running script - and a live game always has the live channel's script running, so it never started (the item sat
# at the main menu until its timeout). The census sends the same five lines itself over the live channel; the jump hook
# (--wwe2k14_wm30=N) still turns the era load into match N.
# Session 92 extension (10:2x, read from the final pass's logs, 0 runs): wm30_e1-e4 stuck because EVTDSC0 / EVTDSC1 open
# ~250-270 frames after the FIRST A on the era screen (a preview load, runs 6185-6187) - before any match is picked - so
# press_until_file ended after one press and nothing picked the match (no "[wm30] jump" line); wm30_e0 (run 6184) needed 3
# presses, its EVTDSC0 opened at the pick and the sequence went 31 -> 9 17 frames later. So the drive now presses A until
# the game is in the match (sequence 9), one press per 300 frames, with no press once it is there.
WM30_DRIVE = ['press_until_state DOWN cursor >=1 24 30 5 if_needed', 'press_until_state UP cursor 1 24 30 5 if_needed',
              'press_until_state A menu !=1 24 30 5', 'wait_frames 180', 'press_until_file A EVTDSC 6 300 4',
              'wait_frames 60', 'press_until_state A seq 9 6 300 4 if_needed']


def do_wm30(g, item, extra):
    g.start(extra_flags=['--wwe2k14_wm30_auto=false'])  # start() adds the item's flags (never pass a flag twice)
    g.batch(WM30_DRIVE, timeout=300)
    if not g.wait_for({'kind': 'seq', 'seq': 9}, 840):
        raise Stuck('the 30 Years match never loaded (seq 9)')
    # Session 92 (m1 run 5968): after the match archives load, 30 Years waits on a card for A (Session 90's Heenan
    # route: one A ~884 frames after the entrance archive) - 15 min with no entrance otherwise. Press A every 25 s
    # until an entrance step or the bell (A does not skip an entrance; START does).
    started = lambda e: ((e.get('kind') == 'entrance' and (e.get('step') or 0) > 0) or
                         (e.get('kind') == 'match' and e.get('what') == 'clock_started'))
    for _ in range(12):
        if g.tr.clock or g.wait_for(started, 25):
            break
        g.batch(['press A'], timeout=60)
        g.shot()
    if not g.entrances_to_bell(900):
        raise Stuck('no bell within 15 min of entrances')
    g.hold_match(item.get('hold_frames', 1300), shot_at=(180, 600, 1200), pair_at=600)


def do_tour(g, item, extra):
    from nav_check import TOUR
    g.start()
    legs = extra['legs'] = []
    for t in TOUR:
        if g.left() < 60:
            raise Stuck('item deadline during the tour')
        t0 = time.time()
        try:
            g.nav.go(t)
            ok = g.nav.where().get('node') == g.nav.atlas.resolve(t)[0]
            note = ''
        except C.nav.Lost as x:
            ok, note = False, str(x)[:150]
        g.batch(['wait_frames 90'], timeout=60)
        s = g.shot()
        s['label'] = t
        legs.append({'target': t, 'ok': ok, 'note': note, 'seconds': round(time.time() - t0, 1)})


def do_nodes(g, item, extra):
    g.start()
    out = extra['nodes'] = []
    for node in item['nodes']:
        if g.left() < 60:
            raise Stuck('item deadline during the node list')
        try:
            g.nav.go(node)
            g.batch(['wait_frames 90'], timeout=60)
            s = g.shot()
            s['label'] = '%s %s' % (node, g.nav.atlas.names.get(node, ''))
            out.append({'node': node, 'ok': True})
        except C.nav.Lost as x:
            out.append({'node': node, 'ok': False, 'note': str(x)[:150]})


def do_recording(g, item, extra):
    src = os.path.join(C.REPO, 'out', 's91', 'jarrod_block.txt')
    if not os.path.exists(src):
        raise Unbuildable('the recording %s is missing' % src)
    # Session 92 extension (10:2x, 0 runs): the raw recording replayed from boot never left the title (runs 6204 / 6350:
    # sequence 3 for 12 minutes) - it starts at the main menu (the recorded session had booted through its live start),
    # so no START was ever pressed. Now: boot to the menu (`include boot_to_menu`), then the recording as a
    # state-driven route (tools\input_scripts\recording_to_route.py: each press waits for the state / file it changed in
    # the recording; 31 of 1048 presses keep the recorded timing, so it still runs at normal speed). The route's own
    # opening wait for the main menu (wait_file MNUIHD.PAC + wait_state menu 1) is dropped: boot_to_menu ends there.
    route = os.path.join(C.CEN, 'jarrod_block_route.txt')
    subprocess.run([sys.executable, os.path.join(C.REPO, 'tools', 'input_scripts', 'recording_to_route.py'), src,
                    '-o', route], check=True, stdout=subprocess.DEVNULL, timeout=120)
    with open(route, encoding='utf-8', errors='replace') as f:
        lines = f.read().rstrip('\n').split('\n')
    dropped, body = 0, []
    for ln in lines:
        s = ln.strip()
        if dropped < 2 and (s.startswith('wait_file MNUIHD.PAC') or s.startswith('wait_state menu 1 ')):
            dropped += 1
            continue
        body.append(ln)
    while body and body[-1].strip() in ('quit', ''):
        body.pop()
    dst = os.path.join(C.CEN, 'jarrod_block_run.txt')
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write("# harness: realtime (Jarrod's Session 91 recording as a state-driven route, recorded at normal speed)\n")
        f.write('on_fail quit\ninclude boot_to_menu\n')
        f.write('screenshot_every 600 cen_%s\n' % item['id'])
        f.write('\n'.join(body) + '\nwait_frames 120\nscreenshot_every 0\nquit\n')
    g.start(script=dst, fast=False, wait_menu=False)
    end = min(time.time() + 2400, g.deadline)
    while time.time() < end:
        g.pump()
        if not C.game_procs(g.name):
            break
        time.sleep(3)
    else:
        raise Stuck('the recording did not finish in time (last events kept)')
    extra['recording_exited'] = True
    for p in sorted(glob.glob(os.path.join(g.caps, '%03d_cen_%s_*.png' % (g.run, item['id'])))):
        base = os.path.splitext(os.path.basename(p))[0][len('%03d_' % g.run):]
        g.shots.append({'label': base, 'file': base, 'phase': 'other', 'path': p})


def do_cpu_finish(g, item, extra):
    spec = C.match.build_spec('Daniel Bryan vs Brock Lesnar; players=0; pace=quick')[0]
    extra['spec_numeric'] = spec
    g.start()
    g.build_match(spec)
    if not g.skip_to_bell():
        raise Stuck('START presses never reached the bell')
    g.batch(['wait_frames 300'], timeout=300)
    g.shot()
    g.set_pause(False)                                              # never start from a paused match (S92 race)
    g.batch(['press START 5 10', 'wait_frames 90'], timeout=120)   # the pause menu, once
    g.set_pause(True)                                               # verified, retried (max 2 more presses)
    s = g.shot()
    s['label'] = 'pause menu'
    g.batch(['press START 5 10', 'wait_frames 60'], timeout=120)
    g.set_pause(False)                                              # verified, retried
    end = min(time.time() + 1500, g.deadline)
    shots = 0
    while time.time() < end and not any(e.get('what') == 'finish' for e in g.events if e.get('kind') == 'match'):
        g.pump()
        if shots < 40:
            g.shot()
            shots += 1
        g.batch(['wait_frames 600'], timeout=300)
        if not g.pump() and g.paused():                             # a stray toggle: no events and paused -> resume
            g.set_pause(False)
    if not any(e.get('what') == 'finish' for e in g.events if e.get('kind') == 'match'):
        raise Stuck('the CPU match did not finish in time')
    for _ in range(12):   # the finish, post-match screens, highlights
        g.batch(['wait_frames 240'], timeout=120)
        g.shot()


HANDLERS = {'roster': do_match, 'arena': do_match, 'type': do_match, 'wm30': do_wm30}
MODE_HANDLERS = {'tour': do_tour, 'nodes': do_nodes, 'recording': do_recording, 'cpu_finish': do_cpu_finish}


def run_item(item, lane, exe, pdir, atlas, say, keep_png=False):
    t0 = time.time()
    started = C.now()
    g = C.Game(lane, item, exe, say, atlas=atlas)
    status, note, extra = 'ok', '', {}
    handler = MODE_HANDLERS[item['mode']] if item['family'] == 'mode' else HANDLERS[item['family']]
    try:
        try:
            if item['family'] in ('roster', 'arena', 'type'):
                match_spec(item)  # a spec the builder refuses is 'unbuildable' without a launch
            handler(g, item, extra)
        except C.match.SpecError as x:
            status, note = 'unbuildable', 'spec: %s' % x
        except Unbuildable as x:
            status, note = 'unbuildable', str(x)
        except Stuck as x:
            status, note = 'stuck', str(x)
        except (Crashed, C.live.NoGame) as x:
            status, note = 'crash', 'game gone: %s' % str(x)[:200]
        except Exception as x:
            status, note = 'error', '%s: %s %s' % (type(x).__name__, x, C.tb())
    finally:
        try:
            clean = g.close()
        except Exception:
            clean = False
        if not clean and status == 'ok':
            note = 'the game had to be killed to close'
        if g.watchdog_fired and status != 'stuck':
            status, note = 'stuck', 'watchdog: ' + note
    if g.run is None:
        res = {'id': item['id'], 'family': item['family'], 'spec': item.get('spec'), 'status': status, 'note': note,
               'verdict': status if status != 'ok' else 'error', 'started': started, 'finished': C.now(),
               'elapsed_s': round(time.time() - t0), 'lane': lane, 'shots': [], 'signatures': [], 'windows': [],
               'totals': {'windows': 0, 'passes': 0, 'owned': 0, 'fallback': 0, 'no_pipeline': 0, 'lost_clears': 0}}
    else:
        try:
            res = C.finish_result(item, g, status, note, started, time.time() - t0,
                                  os.path.join(pdir, 'shots', item['id']), keep_png)
        except Exception as x:
            res = {'id': item['id'], 'family': item['family'], 'status': 'error', 'verdict': 'error',
                   'note': 'result building failed: %s %s' % (x, C.tb()), 'run': g.run, 'started': started,
                   'elapsed_s': round(time.time() - t0), 'shots': [], 'signatures': [], 'windows': [],
                   'totals': {'windows': 0, 'passes': 0, 'owned': 0, 'fallback': 0, 'no_pipeline': 0,
                              'lost_clears': 0}}
    res.update(extra)
    if item['family'] == 'roster' and item['id'].startswith('six_') and res.get('status') == 'ok':
        got = set(int(k) for k in res.get('entrance_slots', {}))
        res['no_entrance'] = [item['wrestlers'][s] for s in range(len(item['wrestlers'])) if s not in got]
    return res


def followup_item(wid, names):
    opp = 324 if wid != 324 else 343
    return {'id': 'fu_%d' % wid, 'family': 'roster', 'spec': '#%d, #%d; type=0; pace=quick; players=0' % (wid, opp),
            'wrestlers': [wid, opp], 'names': [names.get(wid, str(wid)), names.get(opp, str(opp))],
            'entrances': 'full', 'hold_frames': 1300, 'flags': [], 'timeout_s': 1800,
            'note': 'follow-up: no entrance event in his six-man'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pass', dest='pass_name', required=True)
    ap.add_argument('--lanes', type=int, default=3)
    ap.add_argument('--only', default='')
    ap.add_argument('--family', default='')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--redo', action='store_true')
    ap.add_argument('--redo-failed', action='store_true')
    ap.add_argument('--exe', default='')
    ap.add_argument('--keep-png', action='store_true')
    ap.add_argument('--flags', default='', help='extra game flags for every item ("--a=1 --b=2"); each replaces a '
                    'built-in flag of the same name (e.g. --wwe2k14_native_frame=false) - a switch A/B (Session 92)')
    ap.add_argument('--keep-shaders', action='store_true',
                    help='copy every game\'s shader storage into <pass>\\shaders\\ when it ends; merge with '
                    'py tools\\shaders\\merge_storage.py <pass>\\shaders\\* --list OUT (Session 92: the final census)')
    a = ap.parse_args()
    C.FLAG_OVERRIDES[:] = [f for f in a.flags.split() if f.startswith('--')]
    pdir = os.path.join(C.CEN, a.pass_name)
    if a.keep_shaders:
        C.KEEP_SHADERS = os.path.join(pdir, 'shaders')
    rdir = os.path.join(pdir, 'results')
    os.makedirs(rdir, exist_ok=True)
    ipath = os.path.join(C.CEN, 'items.json')
    if not os.path.exists(ipath):
        import items
        items.main()
    with open(ipath, encoding='utf-8') as f:
        items = json.load(f)
    # follow-ups already known from earlier results of this pass
    names = {}
    for e in C.match.load('characters.json') + C.match.load('dlc_characters.json'):
        names[e['id']] = e['name']
    have = {it['id'] for it in items}
    for p in glob.glob(os.path.join(rdir, 'six_*.json')):
        try:
            for wid in json.load(open(p, encoding='utf-8')).get('no_entrance', []):
                if 'fu_%d' % wid not in have:
                    items.append(followup_item(wid, names))
                    have.add('fu_%d' % wid)
        except (OSError, ValueError):
            pass
    only = set(x.strip() for x in a.only.split(',') if x.strip())
    todo, skipped = [], []
    for it in items:
        if only and it['id'] not in only:
            continue
        if a.family and it['family'] != a.family:
            continue
        rp = os.path.join(rdir, it['id'] + '.json')
        if os.path.exists(rp) and not a.redo:
            try:
                bad = json.load(open(rp, encoding='utf-8')).get('status') in ('error', 'stuck', 'crash')
            except (OSError, ValueError):
                bad = True
            if not (a.redo_failed and bad):
                skipped.append(it['id'])
                continue
        todo.append(it)
    print('pass %s: %d to run, %d already have a result%s' % (a.pass_name, len(todo), len(skipped),
                                                            (' (%s)' % ', '.join(skipped[:8])) if skipped else ''))
    for it in todo:
        print('  %-18s %-7s timeout %5d s  %s' % (it['id'], it['family'], it.get('timeout_s', 0), (it.get('spec') or '')[:70]))
    if a.dry or not todo:
        return 0
    q = queue.Queue()
    for it in todo:
        q.put(it)
    atlas = C.nav.Atlas()
    active = [0]
    stop = threading.Event()
    exe = a.exe or C.MAIN_EXE

    def worker(lane):
        say = say_for(pdir, lane)
        while not stop.is_set():
            try:
                it = q.get_nowait()
            except queue.Empty:
                with PLOCK:
                    busy = active[0]
                if busy == 0:
                    return
                time.sleep(5)
                continue
            with PLOCK:
                active[0] += 1
            say('START %s (%s) %s' % (it['id'], it['family'], (it.get('spec') or '')[:80]))
            try:
                res = run_item(it, lane, exe, pdir, atlas, say, a.keep_png)
            except BaseException as x:   # nothing may stop the sweep
                res = {'id': it['id'], 'family': it['family'], 'status': 'error', 'verdict': 'error',
                       'note': 'runner failure: %s %s' % (x, C.tb()), 'shots': [], 'signatures': [], 'windows': [],
                       'totals': {'windows': 0, 'passes': 0, 'owned': 0, 'fallback': 0, 'no_pipeline': 0,
                                  'lost_clears': 0}}
                try:
                    C.kill_game(lane)
                except Exception:
                    pass
            try:
                C.write_json(os.path.join(rdir, it['id'] + '.json'), res)
            except Exception as x:
                say('could not write the result: %s' % x)
            say('DONE %s: %s (%s) windows %s fallback %s lost %s, %d shots, %ss, run %s' % (
                it['id'], res.get('verdict'), res.get('status'), res.get('totals', {}).get('windows'),
                res.get('totals', {}).get('fallback'), res.get('totals', {}).get('lost_clears'),
                len(res.get('shots', [])), res.get('elapsed_s'), res.get('run')))
            for wid in res.get('no_entrance', []):
                if 'fu_%d' % wid not in have:
                    have.add('fu_%d' % wid)
                    fu = followup_item(wid, names)
                    if not only or fu['id'] in only or not a.only:
                        q.put(fu)
                        say('follow-up queued: %s (%s)' % (fu['id'], names.get(wid)))
            with PLOCK:
                active[0] -= 1
            time.sleep(3)

    # Session 92: live names are unique per pass. With 'cen1..3' for every pass, two sweeps at once (pass 1 + a fix
    # check) shared live pipes and kill_game() (by `--wwe2k14_live=NAME` on the command line) killed the other sweep's
    # games. Pipe names allow letters, digits and '_'.
    tag = re.sub(r'[^A-Za-z0-9_]', '_', a.pass_name)[:24]
    lane_names = ['cen_%s_%d' % (tag, i + 1) for i in range(a.lanes)]
    threads = [threading.Thread(target=worker, args=(lane_names[i],), daemon=True) for i in range(a.lanes)]
    for i, t in enumerate(threads):
        t.start()
        time.sleep(20)   # staggered starts: run_list serialises launches anyway
    try:
        while any(t.is_alive() for t in threads):
            time.sleep(2)
    except KeyboardInterrupt:
        stop.set()
        for nm in lane_names:
            C.kill_game(nm)
    print('pass %s: finished %s' % (a.pass_name, C.now()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
