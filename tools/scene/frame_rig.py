#!/usr/bin/env python3
r"""The offline rig (Session 103, lane R): a captured frame drawn from its scene record ALONE, beside the game's own picture of that frame.

  py -I tools\scene\frame_rig.py --capture C:\...\out\captures\brock_losswin --frames 20,110,200 [--out DIR] [--pair-offset 5] [--no-textures] [--mask 0x0]

Per frame N (0 game runs; fview headless + frame_rig.exe, both offline):
  game  = fview --replay --dump-frames N: the capture's stream frame N through the real binder (the presented surface; NO display curve on this side: fview applies none
          unless --display-curve-s100 is given).
  ours  = out\tools\frame_rig.exe on scene\frame_NNNNNN.rsc (library record_frame.h + FrontEnd::RenderToPixels), whole and once per family; no display curve either
          (the post / curve is not in the record -> frame path yet), so both sides are LINEAR-as-presented-by-the-tools and the same curve (none) is on both.
  camera = a live record carries NO camera section yet (FRAME_CONTRACT / lane A). The rig takes the game-space world -> clip matrix of the stream's own first arena draw
          (fview --save-case: constant ViewProj c56-c59; row-vector form = its transpose) and the eye solved from it. OFFLINE ONLY: the live path needs a reader at the source.
  pairing = the record is paired with the stream frame of the SAME capture frame number, and checked: the stream frame's header guest_frame must equal the record's
          header guest_frame (both are the present's own number: OWN_THE_PASS 9a item 5). --pair-offset K pairs record N+K with stream N: the planted pairing fault.
Writes DIR\first_sheet_fNNN.png (game | ours | difference | per-family panels, labelled) and DIR\frame_NNN_report.txt; exit 1 on any FAIL line.

Round 2 (R2): the whole panel is made as the live frame is made: --see-order game (default; the game's see-through order), lane E's maps hook and lane F's particle hook
attached (--no-hooks leaves them off), the wrestlers' models TOLD with --link ID=CH:AT when the record names none (labelled TOLD on the sheet; brock_losswin's records were taken before
lane B's writer: its default links are DEFAULT_LINKS), the maps as panels, and per family a "what is missing" line (the distance from the game's picture over the family's own pixels
+ what the record / the hooks say is not there). `--assets offline` draws through tools\scene\offline_assets.h (the viewer's loader seam) instead of GameFilesSource: the pictures must be equal.
Plants (each must FAIL with its token; `--expect-fail TOKEN`): --no-hooks (HOOKS MISSING), --see-order distance (SEE ORDER), a --link of a character that does not exist (WRESTLER HOLE).
"""
import argparse, glob, hashlib, json, os, re, shutil, struct, subprocess, sys, threading, time
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
sys.path.insert(0, HERE)
import e3_floor  # S103 E3
import scene_ref  # S103 PS1: CASE_LOCK
FAMILIES = ['arena', 'ring', 'props', 'wrestlers', 'crowd', 'screens', 'flashes', 'particles', 'other']
CLEAR = (31, 36, 46)  # the front-end's clear colour (0.12, 0.14, 0.18)
# The wrestlers' models, TOLD: a record taken before lane B's writer names none (its objects carry "asset unknown"). slot 0 = Brock (343), slot 6 = Bryan (324): z7147's silhouettes match
# [LIVE_MAPS.md gap 1]; UNCONFIRMED for brock_losswin itself (the sheet shows whether the figure fits). Attire 0 assumed.
DEFAULT_LINKS = {'brock_losswin': ['actor:slot:0=343:0'], 'brock_losswin_rt': ['actor:slot:0=343:0']}  # slot 6's palette has 99 nodes, Bryan's model 109: not linked (measured on the first sheet)
# S103 W2: TOLD body scales for a pre-lane-B capture whose stream draws cannot be joined to the told model offline (its stream holds partial vertex ranges: the fit finds no mesh). Like DEFAULT_LINKS this is
# told, not measured here: Brock's one body scale 1.0104980 (= 4139 / 4096) is lane 42's fit on run 7051 (docs\formats\wrestler_host_vertex.md) [U that this capture's Brock is the same body: the picture is what
# shows it]. A stream measurement (w2_skin_path_check --scales) wins over it; with neither, the wrestler stays a hole, as live.
DEFAULT_SCALES = {'brock_losswin': {'actor:slot:0': 1.0104980}, 'brock_losswin_rt': {'actor:slot:0': 1.0104980}}
# The holes a pre-lane-B capture is KNOWN to have with its default links (S103 main session, 2026-11-15, rig run on frame 110: 7 holes, all
# "the record does not name the model of actor:slot:6"). Only with the default links; any other count (a plant link, a new hole) still FAILs.
KNOWN_HOLES = {'brock_losswin': (7, 'actor:slot:6 (Bryan) unnamed in a pre-lane-B record and not linkable (palette 99 nodes, model 109)'),
               'brock_losswin_rt': (7, 'actor:slot:6 (Bryan) unnamed in a pre-lane-B record and not linkable (palette 99 nodes, model 109)')}
# What the docs say a family still lacks (the page named); the measured part (skips, EMPTY, hook counters) is added from the run itself.
DOC_GAPS = {
    'wrestlers': 'no model name in a pre-lane-B record: TOLD via --link (WRESTLERS_RECORD.md)',
    'particles': 'LiveParticles skips a viewer-space frame (live_particles.cpp); every BuildFrame frame is viewer-space, so none is drawn (see census viewer_space_skipped)',
    'screens': 'tron / movie media is not in the record (STAGE_FROM_RECORD.md)',
}


_FVIEW = {}


def find_fview_once(root=None):
    """fview_exe.find_fview (the exe and its freshness against every source it compiles: ~1.6 s of path walking a call), once per process: a session asks for it from every arm and every prefetch thread"""
    from fview_exe import find_fview
    k = root or ROOT
    if k not in _FVIEW:
        _FVIEW[k] = find_fview(root=k)
    return _FVIEW[k]


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.normpath(os.path.dirname(common)) if common else ROOT


def canon(p):
    """one spelling of a path (SLF2: '/' vs '\\' made the memo keys of one capture differ, and a step ran twice at once on the same file)"""
    return os.path.normcase(os.path.abspath(p)) if isinstance(p, str) else p


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


class _Done:
    """what subprocess.run returns, for a job a rig server ran"""
    def __init__(self, rc, text):
        self.returncode, self.stdout, self.stderr = rc, text, ''


class RigSession:
    r"""S103 PS1: LOAD ONCE, DRAW MANY for the rig's callers. One session = a set of `frame_rig.exe --serve` processes (one per distinct process-level environment, normally one) that keep the loaded game
    (arena, models, textures, crowd) between jobs, plus a memo of the per-(capture, frame) preparation every arm of a proof repeats (the game's replay pictures, body scales, the stream's camera, the look
    rows, the pools). A job is the one-shot command line unchanged; the env vars a one-shot run takes per job (WWE_RIG_BODY_SCALES, WWE_RDR_L2_DUMP) are job flags here; the other WWE_RDR_* / RDR_* vars are
    process level and so part of the server's key. A job the server refuses (exit 5: it needs a fresh process) runs as an ordinary one-shot. Use: frame_rig.SESSION = frame_rig.RigSession() before run_main().
    """
    PER_JOB_ENV = ('WWE_RIG_BODY_SCALES', 'WWE_RDR_L2_DUMP')

    def __init__(self, serve=True, prep_root=None):
        self.serve = serve and os.environ.get('WWE_RIG_NO_SERVE') != '1'
        self.servers = {}
        self.memo_ = {}
        self.lock_ = threading.Lock()
        self.prep_root = prep_root   # where the shared preparation (the game's pictures, the stream's ops list, the page fits) is written: one folder per capture; arms copy what they need
        self.stats = {'jobs': 0, 'served': 0, 'one_shot': 0, 'job_s': 0.0}

    def memo(self, key, fn):
        """fn() once per key, from any thread: a second caller of a key that is being made waits for it (an exception is not kept)"""
        for k_ in key:   # SLF2: a path in a key is in its one spelling, or two spellings of a capture make two entries
            assert not (isinstance(k_, str) and ('/' in k_ or '\\' in k_)) or k_ == canon(k_), 'memo key path not canonical: %r' % (k_,)
        with self.lock_:
            e = self.memo_.get(key)
            if e is None:
                e = self.memo_[key] = [threading.Lock(), False, None]
        with e[0]:
            if not e[1]:
                e[2] = fn()
                e[1] = True
        return e[2]

    def prep_dir(self, name):
        d = os.path.join(self.prep_root, name)
        os.makedirs(d, exist_ok=True)
        return d

    @staticmethod
    def _quote(a):
        return '"%s"' % a if (' ' in a or not a) else a

    def _server(self, env):
        key = tuple(sorted((k, v) for k, v in env.items() if k.startswith(('WWE_RDR_', 'RDR_', 'WWE_FVIEW', 'WWE_RIG_', 'WWE_STEPS')) and k not in self.PER_JOB_ENV))
        srv = self.servers.get(key)
        if srv is None or srv.poll() is not None:
            e = {k: v for k, v in env.items() if k not in self.PER_JOB_ENV}
            srv = subprocess.Popen([os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe'), '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e, text=True, errors='replace', bufsize=1)
            self.servers[key] = srv
        return srv

    def rig(self, cmd, env=None, timeout=1800):
        """cmd = [frame_rig.exe, args...] exactly as a one-shot call; env = the environment of that call. Returns something with .returncode / .stdout / .stderr."""
        env = dict(env if env is not None else os.environ)
        self.stats['jobs'] += 1
        t0 = time.time()
        args = list(cmd[1:])
        if self.serve:
            if env.get('WWE_RIG_BODY_SCALES') is not None and '--body-scales' not in args:
                args += ['--body-scales', env['WWE_RIG_BODY_SCALES']]
            if env.get('WWE_RDR_L2_DUMP') and '--l2-dump' not in args:
                args += ['--l2-dump', env['WWE_RDR_L2_DUMP']]
            srv = self._server(env)
            timer = threading.Timer(timeout, srv.kill)
            timer.start()
            lines, rc = [], 3
            try:
                srv.stdin.write(' '.join(self._quote(a) for a in args) + '\n')
                srv.stdin.flush()
                for ln in srv.stdout:
                    if ln.startswith('@@JOB_BEGIN'):
                        continue
                    if ln.startswith('@@JOB_END'):
                        rc = int(ln.split()[2])
                        break
                    lines.append(ln)
                else:
                    lines.append('frame_rig serve process ended without finishing the job\n')
            except (OSError, ValueError) as ex:
                lines.append('frame_rig serve process failed: %s\n' % ex)
            finally:
                timer.cancel()
            text = ''.join(lines)
            if rc != 5:
                self.stats['served'] += 1
                self.stats['job_s'] += time.time() - t0
                return _Done(rc, text)
        self.stats['one_shot'] += 1
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env, timeout=timeout)
        self.stats['job_s'] += time.time() - t0
        return p

    def close(self):
        for srv in self.servers.values():
            try:
                srv.stdin.write('quit\n')
                srv.stdin.close()
                srv.wait(timeout=60)
            except Exception:
                srv.kill()
        self.servers = {}


SESSION = None   # a RigSession: run_main then serves its rig jobs from it and memoises the per-frame preparation; None = every call is its own process, as before


def _shared(key, fn):
    return SESSION.memo(key, fn) if SESSION is not None else fn()


# ---- S103 PS1: the preparation every arm of a proof repeats, made once per (capture, frame) and shared (a session only; without one every call below runs its own, as before) ----
def _prep_dir(capture):
    return SESSION.prep_dir(os.path.basename(capture.rstrip('\\/'))) if (SESSION is not None and SESSION.prep_root) else None


def prep_replay(fview, capture, game, frames, gdir):
    """-> (rc, text, folder holding the game's pictures of `frames`): fview --replay through the last frame with the chosen frames dumped. In a session the folder is the shared one."""
    capture = canon(capture)
    P = _prep_dir(capture)
    gd = gdir if P is None else os.path.join(P, 'game_' + hashlib.md5(','.join(map(str, frames)).encode()).hexdigest()[:8])

    def go():
        r_, t_ = run([fview, '--capture', capture, '--replay', '--frames', str(max(frames) + 1), '--dump-frames', ','.join(map(str, frames)), '--dump-dir', gd, '--root', ROOT, '--game', game, '--validation', '1'], timeout=1800)
        return r_, t_, gd
    return go() if P is None else SESSION.memo(('replay', capture, tuple(frames)), go)


def prep_body_scales(capture, game, n, rec_n, links):
    capture = canon(capture)
    return stream_body_scales(capture, game, n, rec_n, links) if SESSION is None else SESSION.memo(('bodyscales', capture, n, rec_n, tuple(links)), lambda: stream_body_scales(capture, game, n, rec_n, links))


def prep_camera(fview, capture, n, game, work):
    """stream_camera; in a session made in the shared folder once, its ops list copied into `work` (the arm's folder; l_check's scene reference reads it from there)"""
    capture = canon(capture)
    P = _prep_dir(capture)
    if P is None:
        return stream_camera(fview, capture, n, game, work)
    try:   # SLF2: a failure is raised inside the memo (so it keeps nothing) and returned here as (None, why)
        res = SESSION.memo(('camera', capture, n), lambda: _stream_camera_raise(fview, capture, n, game, P))
    except CameraError as e_:
        return None, str(e_)
    src, dst = os.path.join(P, 'ops_%d.json' % n), os.path.join(work or P, 'ops_%d.json' % n)
    if work and os.path.exists(src) and not os.path.exists(dst):
        os.makedirs(work, exist_ok=True)
        shutil.copyfile(src, dst)
    return res


def prep_look(capture, n, work):
    capture = canon(capture)
    P = _prep_dir(capture)
    return stream_look(capture, n, work) if P is None else SESSION.memo(('look', capture, n), lambda: stream_look(capture, n, P))


def entrant_of(rsc):
    bones_exe = os.path.join(ROOT, 'out', 'tools', 'l2_bones.exe')
    if os.path.exists(bones_exe):
        rcb, tb = run([bones_exe, rsc])
        mb = re.match(r'node2 (\S+) (\S+)', tb or '')
        return (float(mb.group(1)), float(mb.group(2))) if mb else None
    return None


def prep_pools(fview, capture, n, game, work, ent):
    capture = canon(capture)
    P = _prep_dir(capture)
    if P is None:
        return stream_pools(fview, capture, n, game, work, ent)

    def go():
        prep_camera(fview, capture, n, game, P)   # (the ops list the page fit reads is the camera step's)
        return stream_pools(fview, capture, n, game, P, ent)
    res = SESSION.memo(('pools', capture, n, ent), go)
    for f_ in ('page_%d.png' % n, 'spot_%d.json' % n):   # the files the fit wrote: this arm's folder gets them too (the PAGE rms line reads page_N.png there)
        if work and os.path.exists(os.path.join(P, f_)) and not os.path.exists(os.path.join(work, f_)):
            os.makedirs(work, exist_ok=True)
            shutil.copyfile(os.path.join(P, f_), os.path.join(work, f_))
    return res


def prefetch(capture, frames, look=False, pools=False, extra=None, workers=4, links=None, pair_offset=0):
    """Starts the shared preparation of (capture, frames) on threads: the replay, and per frame the body scales, the camera, the look rows, the pools and an `extra(n)` of the caller's (l_check's scene reference).
    Returns the futures; a task that fails keeps nothing (the arm that needs it makes it again and reports the failure). Session only."""
    import concurrent.futures
    if SESSION is None or not SESSION.prep_root:
        return []
    main_root = main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = canon(capture)
    name = os.path.basename(capture.rstrip('\\/'))
    fview = find_fview_once(ROOT)
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=workers)
    lk = links if links is not None else DEFAULT_LINKS.get(name, [])
    futs = []

    def safe(fn, *a):
        try:
            return fn(*a)
        except Exception:
            return None

    def task_frame(n):
        rec_n = n + pair_offset
        safe(prep_body_scales, capture, game, n, rec_n, lk)
        safe(prep_camera, fview, capture, n, game, None)
        if look:
            safe(prep_look, capture, n, None)
        if pools:
            safe(prep_pools, fview, capture, n, game, None, entrant_of(os.path.join(capture, 'scene', 'frame_%06d.rsc' % rec_n)))
        if extra:
            safe(extra, n)
    futs.append(pool.submit(safe, prep_replay, fview, capture, game, frames, None))
    for n in frames:
        futs.append(pool.submit(task_frame, n))
    pool.shutdown(wait=False)
    return futs


def _rig_run(cmd, env=None, timeout=1800):
    if SESSION is not None:
        return SESSION.rig(cmd, env, timeout)
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env, timeout=timeout)


def stream_body_scales(capture, game, n, rec_n, links=()):
    """S103 W2: the body scale of each wrestler object, measured from the capture's own stream draws of frame n (what the live WrestlerCore measures; w2_skin_path_check --scales).
    Returns 'ADDR=s,ADDR=s' ('' when the stream holds none: the wrestlers then stay holes, as live)."""
    exe = os.path.join(ROOT, 'out', 'tools', 'w2_skin_path_check.exe')
    if not os.path.exists(exe):
        run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'wrestler', 'build_w2_check.ps1')], timeout=1800)
    if not os.path.exists(exe):
        text = ''
    else:
        text = None
    if text is None:
        rc, text = run([exe, '--capture', capture, '--game', game, '--frames', '%d-%d' % (n, n), '--step', '1', '--pair', str(rec_n - n + 1), '--scales'] + [x for l in links if l != 'none' for x in ('--link', l)], timeout=900)
    got = {m.group(1): m.group(2) for m in re.finditer(r'^BODYSCALE (\S+) (\S+)$', text, re.M)}
    for k, v in DEFAULT_SCALES.get(os.path.basename(capture.rstrip('\\/')), {}).items():
        got.setdefault(k, repr(v))
    return ','.join('%s=%s' % kv for kv in got.items())


def stream_guest_frame(capture, n):
    with open(os.path.join(capture, 'frames', 'frame_%06d.bin' % n), 'rb') as f:
        b = f.read(32)
    return struct.unpack_from('<Q', b, 16)[0]


def record_guest_frame(rsc_dump, rsc):
    rc, text = run([rsc_dump, rsc])
    m = re.search(r'^\s*version \d+\s+source \d+\s+sections \d+\s+guest_frame (\d+)', text, re.M)  # the header line, not any later mention
    return int(m.group(1)) if m else None, text


class CameraError(Exception):
    pass


def stream_camera(fview, capture, n, game, work):
    """-> (camera, why) as before; a failure is (None, why)"""
    try:
        return _stream_camera_raise(fview, capture, n, game, work)
    except CameraError as e:
        return None, str(e)


def _case_dirs(root):
    return set(d for d in glob.glob(os.path.join(root, 'out', 'cases', 'fview_*')) if not os.path.basename(d).startswith('fview_unpack_'))


def _bank_camera(capture, n):
    """S103 PMS: the camera of the first arena colour draw (yBG_*) read from its VS constant bank in the capture (shadow_census.py's reader), for a capture whose colour draws fview leaves out (z7147: D3 caster capture).
    Same values and layout as the --save-case route: rows c56..c59 of ViewProj, clip = M * [p, 1]. OFFLINE ONLY."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'shadow'))
    import shadow_census as sc
    cap = sc.Cap(capture)
    f = cap.frame(n)
    d = f.draws
    ps = f.origins['pass_site'].astype(np.int64)
    eff = cap.effs(d['vs_hash'])
    for i in np.nonzero((ps == sc.SITE_COLOUR) | (ps == sc.SITE_MIRROR))[0]:
        e = str(eff[i])
        if not e.startswith('yBG_'):
            continue
        r = sc.FXR.get((e.split('/')[0], 'vertex', 'ViewProj'))
        if r is None:
            continue
        M = sc.regs(f.bank(int(d[i]['vs_bank'])), r, 4).astype(np.float64)
        A, b = M[[0, 1, 3], :3], -M[[0, 1, 3], 3]
        eye = np.linalg.solve(A, b)
        return (M.T.astype(np.float32).reshape(-1), eye.astype(np.float32), -1, M), 'ViewProj of the stream bank of draw %d (%s): fview leaves the colour draws out of this capture' % (int(i), e)
    raise CameraError('no arena colour draw (yBG_*) with a ViewProj in the stream banks of frame %d' % n)


def _stream_camera_raise(fview, capture, n, game, work):
    """The game-space row-vector world -> clip matrix + eye of the stream frame's first arena draw (yBG_*): OFFLINE ONLY (see the module doc)."""
    ops = os.path.join(work, 'ops_%d.json' % n)
    rc, text = run([fview, '--capture', capture, '--frame', str(n), '--ops', '--json', ops, '--root', ROOT, '--game', game])
    if rc != 0 or not os.path.exists(ops):
        raise CameraError('fview --ops failed: ' + text[-300:])
    try:
        d = json.load(open(ops))
    except ValueError as e:
        raise CameraError('fview --ops wrote an unreadable op list: %s' % e)
    cand = [o for o in d['ops'] if o['kind'] == 'draw' and str(o.get('effect', '')).startswith('yBG_') and o.get('outcome') == 'drawn']
    if not cand:
        return _bank_camera(capture, n)   # S103 PMS: a D3 caster capture (z7147): fview leaves the colour draws out, the stream's banks still hold them
    op = cand[0]['op']
    with scene_ref.CASE_LOCK:
        before = _case_dirs(ROOT)
        rc, text = run([fview, '--capture', capture, '--frame', str(n), '--save-case', '--op', str(op), '--root', ROOT, '--game', game])
        new = sorted(_case_dirs(ROOT) - before)
    if not new:
        raise CameraError('fview --save-case wrote no case: ' + text[-300:])
    case = json.load(open(os.path.join(new[-1], 'case.json')))
    for c in case['draw'].get('constants', []):
        if c.get('field') == 'ViewProj' and c.get('count') == 4 and len(c['values']) == 16:
            M = np.array(c['values'], dtype=np.float64).reshape(4, 4)  # rows c56..c59, clip = M * [p, 1]
            A, b = M[[0, 1, 3], :3], -M[[0, 1, 3], 3]
            eye = np.linalg.solve(A, b)
            return (M.T.astype(np.float32).reshape(-1), eye.astype(np.float32), op, M), 'ViewProj of op %d (%s)' % (op, cand[0]['effect'])
    raise CameraError('op %d has no ViewProj constant' % op)


def stream_pools(fview, capture, n, game, work, entrant_xz=None):
    """S103 L2 (class B): the entrance page's pool + spot rows, fitted from the game's OWN page of this stream frame (the 512x512 render target the floor draws sample as slot 13; read with fview --image-png).
    OFFLINE ONLY: the live rows come from the rig (STAGE_LIGHTING.md class B); this is what they must reproduce. Pools = the 8 brightest blobs (centroid -> world x / z by the span 1/5600, 1/5750; tint = peak minus the
    local background), the spot = the bluish blob left over (tint = (peak - bg) / (spot rgb x 0.63)); the spot row is written AT THE ENTRANT (the record's actor root), and the fitted centre is reported against it.
    -> (rig command-line args, a line for the report)."""
    ops = os.path.join(work, 'ops_%d.json' % n)
    d = json.load(open(ops))
    cand = [o for o in d['ops'] if o['kind'] == 'draw' and str(o.get('effect', '')).startswith('yBG_DefaultLM') and o.get('outcome') == 'drawn']
    if not cand:
        return [], 'no yBG_DefaultLM draw to find the page from'
    op = cand[0]['op']
    with scene_ref.CASE_LOCK:
        before = _case_dirs(ROOT)
        run([fview, '--capture', capture, '--frame', str(n), '--save-case', '--op', str(op), '--root', ROOT, '--game', game])
        new = sorted(_case_dirs(ROOT) - before)
    if not new:
        return [], 'fview --save-case wrote no case'
    case = json.load(open(os.path.join(new[-1], 'case.json')))
    page_id = next((t['image'] for t in case['draw'].get('textures', []) if t.get('slot') == 13), None)
    if page_id is None:
        return [], 'the floor draw has no slot-13 page'
    png = os.path.join(work, 'page_%d.png' % n)
    run([fview, '--capture', capture, '--frame', str(n), '--through-op', str(op), '--image-png', '%d,%s' % (page_id, png), '--root', ROOT, '--game', game])
    if not os.path.exists(png):
        return [], 'fview --image-png wrote no page'
    P = np.asarray(Image.open(png).convert('RGB'), dtype=np.float64) / 255.0
    H, W = P.shape[:2]
    lum = P.mean(axis=2)
    lm = (1.0 / 5600.0, 1.0 / 5750.0)
    yy, xx = np.mgrid[0:H, 0:W]
    def world(cx, cy):
        return ((cx + 0.5) / W - 0.5) * 2.0 / lm[0], ((cy + 0.5) / H - 0.5) * 2.0 / lm[1]
    def bg_at(cx, cy):
        r = np.hypot(xx - cx, yy - cy)
        m = (r > 22) & (r < 27)
        return np.array([np.median(P[..., k][m]) for k in range(3)])
    work_l = lum.copy()
    args, found = [], []
    for _ in range(8):
        y, x = np.unravel_index(work_l.argmax(), work_l.shape)
        if work_l[y, x] < 0.55:
            break
        m = (np.hypot(xx - x, yy - y) < 9) & (lum > 0.5 * lum[y, x])
        cx, cy = float((xx * lum)[m].sum() / lum[m].sum()), float((yy * lum)[m].sum() / lum[m].sum())
        bg = bg_at(cx, cy)
        rgb = np.clip(P[y, x] - bg, 0, 1.5)
        wx, wz = world(cx, cy)
        args += ['--look-pool', '%.6g,%.6g,375,%.6g,%.6g,%.6g' % (wx, wz, rgb[0], rgb[1], rgb[2])]
        found.append((wx, wz))
        work_l[np.hypot(xx - cx, yy - cy) < 24] = 0.0
    # the spot: the most blue-over-red blob left
    bl = P[..., 2] - P[..., 0]
    for wx, wz in found:
        px, py = int((wx * lm[0] * 0.5 + 0.5) * W), int((wz * lm[1] * 0.5 + 0.5) * H)
        bl[np.hypot(xx - px, yy - py) < 24] = -1
    # smooth the blue-over-red map with a 15 x 15 box (a faint disk, not one noisy texel: the unsmoothed argmax picked the wrong blob, ~130 units off), then the centroid of the pixels above half the peak
    k = 15
    cs = np.cumsum(np.cumsum(np.pad(bl, k // 2, mode='edge'), axis=0), axis=1)
    cs = np.pad(cs, ((1, 0), (1, 0)))
    sm = (cs[k:, k:] - cs[:-k, k:] - cs[k:, :-k] + cs[:-k, :-k]) / float(k * k)
    y, x = np.unravel_index(sm.argmax(), sm.shape)
    if sm[y, x] > 0.03:
        mm = (np.hypot(xx - x, yy - y) < 14) & (sm > 0.5 * sm[y, x])
        x, y = int(round(float((xx * sm)[mm].sum() / sm[mm].sum()))), int(round(float((yy * sm)[mm].sum() / sm[mm].sum())))
    bl = sm
    msg = '%d pools fitted' % len(found)
    if bl[y, x] > 0.03:
        bg = bg_at(x, y)
        rgb = np.clip((P[y, x] - bg) / (np.array([0.71, 0.78, 1.0]) * 0.63), 0, 1.5)
        sx, sz = world(x, y)
        # the fitted centre is written as an ABSOLUTE spot: the record's node 2 is ~130 units off the game's spot on every frame ([U] what the game centres it on, STAGE_LIGHTING.md), so the page is proved with the fitted centre
        args += ['--look-spot-entrant', '%.6g,%.6g,%.6g' % tuple(rgb)]   # the rule under test: the spot is centred on the record's entrant root (skeleton node 2)
        json.dump({'x': float(sx), 'z': float(sz)}, open(os.path.join(work, 'spot_%d.json' % n), 'w'))
        msg += ', spot fitted at (%.0f, %.0f) tint %.3f' % (sx, sz, rgb.mean())
        if entrant_xz:
            msg += ' vs the entrant root (%.0f, %.0f): %.0f units apart' % (entrant_xz[0], entrant_xz[1], float(np.hypot(sx - entrant_xz[0], sz - entrant_xz[1])))
    else:
        msg += ', no spot found'
    return args, msg


def stream_look(capture, n, work):
    """S103 L: the stage look rows from the stream frame's own light constants (lm_extract groups): rig slot 1 = the most-drawn yBG_DefaultLM group (the arena's sets), rig slot 2 =
    the crowd's yAudience group; plus the entrance flag. -> (rig command-line args, a line for the report). OFFLINE ONLY: the live writer reads the rig's light objects (STAGE_LIGHTING.md)."""
    out = os.path.join(work, 'lm')
    os.makedirs(out, exist_ok=True)
    rc, text = run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'lightmatch', 'lm_extract.py'), capture, '--frames', str(n), '--out', out])
    path = os.path.join(out, 'frame_%d.json' % n)
    if rc != 0 or not os.path.exists(path):
        return [], 'lm_extract failed: ' + text[-200:]
    groups = json.load(open(path))['groups']

    def vals(g, k, count):
        v = g['vals'].get(k)
        if not v:
            return None
        a = v.get('v') or v.get('f')
        return a[:4 * count] if a else None

    def tuple_of(g):
        d, c, s, gr = vals(g, 'DifLghVec', 4), vals(g, 'DifLghCol', 4), vals(g, 'SkyCol', 1), vals(g, 'GroundCol', 1)
        if not (d and c and s and gr):
            return None
        return [d[4 * k + i] for k in range(4) for i in range(3)] + [c[4 * k + i] for k in range(4) for i in range(3)] + s[:3] + gr[:3]

    args, parts = ['--look-entrance'], ['entrance']
    for slot, effect in ((1, 'yBG_DefaultLM'), (2, 'yAudience')):
        cand = [g for g in groups if g['effect'] == effect and tuple_of(g) and any(x > 0 for x in tuple_of(g)[12:24])]
        if not cand:
            continue
        g = max(cand, key=lambda x: x['draws'])
        args += ['--look-rig', str(slot), ','.join('%.9g' % x for x in tuple_of(g))]
        parts.append('rig slot %d from %s group (%d draws)' % (slot, effect, g['draws']))
    # S103 RIG3: rig slot 3 (sets 30 / 31 / 32: the ring, ropes, posts, turnbuckles; SlotForSet) = the props' own tuple: the tuple the yProp_* groups carry that is NOT the arena's (slot 1), the crowd's
    # (slot 2) or a wrestler's (the yCh_* groups: slot 0), the most-drawn first (a04_entrance f60: the yProp_BumpMap / yProp_Default groups, 0.502 0.502 0.541 / sky 0.753 / ground 0.627, 90+ draws).
    # Slots 4-7 (sets 14 / 11 / 12 / 13) are NOT injected: the groups carry no set id and no group in the entrance captures holds a fifth tuple to name them by [C a04_entrance f60], and RigFor
    # joins only slots 1-3 offline; the live route reads them from the game's own remap (PoolFor).
    def key(g):
        t = tuple_of(g)
        return tuple(round(x, 4) for x in t) if t else None
    taken = set(k for k in (key(g) for g in groups if g['effect'] in ('yAudience', 'yAudDefault') or g['effect'].startswith(('yCh_', 'yBG_'))) if k)
    tot = {}
    for g in groups:
        k = key(g)
        if g['effect'].startswith('yProp_') and k and k not in taken and any(x > 0 for x in tuple_of(g)[12:24]):
            tot.setdefault(k, [0, g])[0] += g['draws']
    if tot:
        n3, g3 = max(tot.values(), key=lambda v: v[0])
        args += ['--look-rig', '3', ','.join('%.9g' % x for x in tuple_of(g3))]
        parts.append('rig slot 3 from the yProp_* tuple not taken by slots 0-2 (%d draws)' % n3)
    return args, ', '.join(parts)
def camera_factor(M):
    """Factors the stream's ViewProj (rows c56..c59, clip = M * [p, 1], D3D depth 0..1) into a rigid view and a perspective projection, and reports what the projection is:
    row 3 = the view's z row (unit when the view is rigid); |row 0| = x scale, |row 1| = |y scale|; z row = q * (view z row) with q = f / (f - n), and its translation gives n = -b / q.
    Returns (dict, ok): fov_y, aspect (x scale / y scale), q, near, far, ysign (the sign the y scale must carry for the view rows (right, up-or-down, forward) to be a PROPER rotation, det +1),
    det of the normalized rows, rigid error (row norms and orthogonality). Pure numpy: no game data written."""
    M = np.asarray(M, dtype=np.float64)
    rz, t_z = M[3, :3], M[3, 3]
    nz = np.linalg.norm(rz)
    rx0, ry0 = M[0, :3], M[1, :3]
    xs, ys = np.linalg.norm(rx0), np.linalg.norm(ry0)
    rxn, ryn, rzn = rx0 / xs, ry0 / ys, rz / nz
    det = float(np.linalg.det(np.stack([rxn, ryn, rzn])))
    q = float(np.dot(M[2, :3], rzn) / nz) if nz else 0.0   # z row = q * view-z row (unit)
    q = float(np.dot(M[2, :3], rz) / (nz * nz))
    b = float(M[2, 3] - q * t_z)
    near = -b / q if q else float('nan')
    far = q * near / (q - 1.0) if q != 1.0 else float('inf')
    rigid_err = max(abs(nz - 1.0), abs(float(np.dot(rxn, ryn))), abs(float(np.dot(rxn, rzn))), abs(float(np.dot(ryn, rzn))),
                    float(np.linalg.norm(M[2, :3] - q * rz) / max(1e-9, abs(q))))
    info = dict(fov_y_deg=float(np.degrees(2.0 * np.arctan(1.0 / ys))), aspect=float(ys / xs), xs=float(xs), ys=float(ys), q=q, near=near, far=far,
                ysign=('NEGATIVE' if det < 0 else 'positive'), det=det, rigid_err=rigid_err)
    return info, rigid_err < 5e-3


MAPB_IOU = 0.80   # S103 MBC (RMP.md 2.3 site 7): the bar of the prop footprint
MAPB_ELEV = 0.5150   # shadow_fit.h: the light's travel elevation (a constant on 7 captures)


def _inside_hull(pts, q):
    """Points of q inside the convex hull of pts (Andrew's monotone chain, counter-clockwise; numpy only)."""
    P = sorted(map(tuple, pts))
    def half(seq):
        h = []
        for p in seq:
            while len(h) >= 2 and (h[-1][0] - h[-2][0]) * (p[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (p[0] - h[-2][0]) <= 0:
                h.pop()
            h.append(p)
        return h
    H = np.array(half(P)[:-1] + half(P[::-1])[:-1])
    ok = np.ones(len(q), bool)
    for i in range(len(H)):
        a, b = H[i], H[(i + 1) % len(H)]
        ok &= (b[0] - a[0]) * (q[:, 1] - a[1]) - (b[1] - a[1]) * (q[:, 0] - a[0]) >= 0
    return ok


def mapb_run(cmd, env, prefix, extra):
    """One more rig run of the same frame with extra args (a different --prefix); -> (report text or None, rc)."""
    c2 = list(cmd)
    c2[c2.index('--prefix') + 1] = prefix
    if '--maps-fault' in extra:   # merge with a fault list the caller already passed
        k = extra.index('--maps-fault')
        if '--maps-fault' in c2:
            i = c2.index('--maps-fault') + 1
            c2[i] = c2[i] + ',' + extra[k + 1]
            extra = extra[:k] + extra[k + 2:]
    c2 += extra
    p2 = _rig_run(c2, env, 1800)
    rp = prefix + '_report.txt'
    return (open(rp).read() if p2.returncode == 0 and os.path.exists(rp) else None), p2.returncode


def mapb_census(text):
    m = re.search(r'\[scene\]\[maps\]\[mapb\] mode (.+?), props cast (\d+) objects / (\d+) meshes \(weapons (\d+), event props (\d+), structure pieces (\d+), ids 59/68/71/96 (\d+)\), skipped: mode 1 (\d+), gate unknown (\d+), no model (\d+), categories 33 / 43 at scene word 1 (\d+); out of the fit (\d+)', text)
    if not m:
        return None
    return dict(mode=m.group(1), cast=int(m.group(2)), meshes=int(m.group(3)), weapons=int(m.group(4)), event=int(m.group(5)), stage=int(m.group(6)), unsure=int(m.group(7)),
                sk1=int(m.group(8)), skh=int(m.group(9)), skn=int(m.group(10)), p43=int(m.group(11)), oof=int(m.group(12)))


def mapb_check(rep_text, gs, gs_why, n, args, cmd, env, prefix):
    """S103 MBC: the props in the wrestlers' shadow map (map B). NO offline record carries weapon rows (sections 59-61 are written by the live hooks), so the check runs on a FIXTURE (frame_rig.cpp
    InjectMapBFixture, labelled): a box given to the front end the way ApplyWeaponRows gives a weapon, tagged by the SAME filter (weapons::CastsMapB), plus a second box that is a gm-set item (must NOT cast).
    The wrestlers-only run (this frame's normal run: no fixture) is the baseline. Footprint = the floor points (the grid e3_floor reads) our map shadows only with the fixture; the EXPECTED footprint is the
    box's shadow computed analytically (corners carried along the light's travel direction to the floor, convex hull), independent of our shader. IoU >= MAPB_IOU. Modes: scene word 4 (entrance / cutscene)
    = mode 0 = the box casts; scene word 1 + 3 participants = mode 1 = nothing is cast (`--mapb-expect-mode1`). Tokens: MAPB CENSUS, MAPB MODE1, MAPB GM, MAPB PROPS NONE, MAPB PROPS IOU.
    NOT proved here: the footprint of the GAME's own props (no offline record holds them): that is the first live prop-bearing frame's check."""
    ours_a, why = e3_floor.parse_ours(rep_text)
    if ours_a is None or 'center' not in ours_a:
        return ['FAIL  frame %d: MAPB PROPS NONE: no shadow map / fit in the baseline report: %s' % (n, why)]
    cx, cy, cz = ours_a['center']
    R = max(1.0, float(np.linalg.norm((np.array(ours_a['hi']) - np.array(ours_a['lo'])) / 2)))
    floor_y = ours_a['hi'][1]
    half = 0.12 * R
    fx = [cx + 0.6 * R, cy - 0.3 * R, cz, half]
    gx = [cx - 0.6 * R, cy - 0.3 * R, cz, half]
    fa = ['--prop-fixture', ','.join('%.6g' % v for v in fx), '--gm-fixture', ','.join('%.6g' % v for v in gx)]
    if args.mapb_expect_mode1:
        txt, rc = mapb_run(cmd, env, prefix + '_m1', fa + ['--scene-word', '1', '--participants', '3'])
        cen = mapb_census(txt) if txt else None
        if cen is None:
            return ['FAIL  frame %d: MAPB CENSUS: the mode-1 run has no [scene][maps][mapb] line (exit %d)' % (n, rc)]
        ok = cen['mode'] == '1' and cen['cast'] == 0 and cen['sk1'] > 0
        return ['%s  frame %d: MAPB MODE1: scene word 1 with 3 participants: mode %s, props cast %d, skipped for mode 1 %d%s' % ('PASS' if ok else 'FAIL', n, cen['mode'], cen['cast'], cen['sk1'], '' if ok else '  -> map B must be the game\'s mode 1 (wrestlers only)')]
    pa = ['--scene-word', str(args.mapb_scene_word)] + (['--participants', str(args.mapb_participants)] if args.mapb_participants is not None else [])   # review 1: scene word 1 + 2 participants is mode 0 and MUST cast; word 1 with no count is the old loop_path reader's answer (a hole: nothing casts)
    txt, rc = mapb_run(cmd, env, prefix + '_pf', fa + pa)
    if txt is None:
        return ['FAIL  frame %d: MAPB PROPS NONE: the fixture run failed (exit %d)' % (n, rc)]
    cen = mapb_census(txt)
    if cen is None:
        return ['FAIL  frame %d: MAPB CENSUS: the fixture run has no [scene][maps][mapb] line' % n]
    fxl = re.findall(r'mapb_fixture slot (\d) (weapon|gm-set) item .* tagged (\d)', txt)
    tag = {int(sl): int(t) for sl, _, t in fxl}
    lines = []
    if tag.get(1, 0) != 0:
        lines.append('FAIL  frame %d: MAPB GM: the gm-set fixture item was tagged as a map B caster (gm sets and blood never cast: categories 23 / 36)' % n)
    if tag.get(0, 0) != 1 or cen['cast'] == 0:
        lines.append('FAIL  frame %d: MAPB PROPS NONE: the weapon fixture was not cast (tagged %s, cast %d; %s)' % (n, tag.get(0), cen['cast'], 'mode ' + cen['mode']))
        return lines
    if gs is None:
        return lines + ['FAIL  frame %d: MAPB GAME: the game\'s map could not be read from the stream (%s)' % (n, gs_why)]
    ours_p, why2 = e3_floor.parse_ours(txt)
    if ours_p is None or 'center' not in ours_p:
        return lines + ['FAIL  frame %d: MAPB PROPS NONE: the fixture run made no map: %s' % (n, why2)]
    ag_a = e3_floor.floor_agreement(gs, prefix + '_map_shadow.png', ours_a)
    ag_p = e3_floor.floor_agreement(gs, prefix + '_pf_map_shadow.png', ours_p)
    oa, op = ag_a['masks'][1], ag_p['masks'][1]
    grid = 120
    span = 2.4
    xs = np.linspace(cx - span * R, cx + span * R, grid)
    zs = np.linspace(cz - span * R, cz + span * R, grid)
    X, Z = np.meshgrid(xs, zs)
    # the light's travel direction (shadow_fit.h header): (-key.x, +0.5150, -key.z), horizontal part scaled to sqrt(1 - 0.515^2); the key is the one TOLD from the stream
    kx, kz = gs['key'][0], gs['key'][2]
    hn = float(np.hypot(kx, kz)) or 1.0
    s_h = float(np.sqrt(1 - MAPB_ELEV ** 2))
    fwd = np.array([-kx / hn * s_h, MAPB_ELEV, -kz / hn * s_h])
    corners = np.array([[fx[0] + sx * half, fx[1] + sy * half, fx[2] + sz * half] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)])
    t = (floor_y - corners[:, 1]) / fwd[1]
    proj = corners[:, [0, 2]] + t[:, None] * fwd[[0, 2]]
    exp = _inside_hull(proj, np.stack([X.ravel(), Z.ravel()], axis=1)).reshape(X.shape)
    po, pe = op & ~oa, exp & ~oa
    inter, union = int((po & pe).sum()), int((po | pe).sum())
    if int(pe.sum()) == 0:
        return lines + ['FAIL  frame %d: MAPB PROPS NONE: the analytic footprint of the fixture is empty on the grid (place it elsewhere)' % n]
    iou = inter / union if union else 0.0
    ok = iou >= MAPB_IOU
    lines.append('%s  frame %d: MAPB PROPS %s: fixture prop footprint on the floor grid: expected (analytic) %d, ours-added %d, both %d, IoU %.3f (bar %.2f); mode %s, props cast %d objects / %d meshes (weapons %d, event props %d, structure pieces %d, ids 59/68/71/96 %d), skipped mode 1 %d / gate unknown %d / no model %d, out of the fit %d'
                 % ('PASS' if ok else 'FAIL', n, 'COVERED' if ok else 'IOU', int(pe.sum()), int(po.sum()), inter, iou, MAPB_IOU, cen['mode'], cen['cast'], cen['meshes'], cen['weapons'], cen['event'], cen['stage'], cen['unsure'], cen['sk1'], cen['skh'], cen['skn'], cen['oof']))
    try:
        S = 384
        fl = np.zeros(pe.shape + (3,), np.uint8)
        fl[pe] = (255, 40, 40)
        fl[po] = (40, 255, 40)
        fl[pe & po] = (255, 255, 40)
        tiles = [label(np.asarray(Image.open(prefix + '_map_shadow.png').convert('RGB').resize((S, S))), 'OURS wrestlers only f%d' % n, 18),
                 label(np.asarray(Image.open(prefix + '_pf_map_shadow.png').convert('RGB').resize((S, S))), 'OURS with the fixture props (mode 0)', 18),
                 label(np.asarray(Image.fromarray(fl).resize((S, S), Image.NEAREST)), 'prop footprint: expected red / ours green / both yellow, IoU %.2f' % iou, 18)]
        Image.fromarray(np.concatenate(tiles, axis=1)).save(prefix + '_mapb_props.png')
        lines.append('      sheet: ' + prefix + '_mapb_props.png')
    except Exception as ex:   # the sheet is a courtesy
        lines.append('      (no sheet: %s)' % ex)
    return lines


def e3_check_maps(rep_text, gs, gs_why, n, args, work, out, prefix, game_img):
    """S103 E3: the map lines of one frame. MAP SHADOW: MADE or a counted HOLE (FAIL token 'MAP SHADOW HOLE'), and the floor agreement with the game's map B read the same way
    (FAIL token 'MAP SHADOW DISTANCE' below the bar). MAP circles / plane / rim / pre-light / sweat: made, or a HOLE line with the reason (counted, named; not a failure of this check)."""
    lines = []
    ours, why = e3_floor.parse_ours(rep_text)
    if ours is None:
        lines.append('FAIL  frame %d: MAP SHADOW HOLE: no shadow map was made: %s' % (n, why))
    elif gs is None:
        lines.append('FAIL  frame %d: MAP SHADOW GAME: the game\'s map could not be read from the stream (%s): no comparison' % (n, gs_why))
    elif 'center' not in ours:
        lines.append('FAIL  frame %d: MAP SHADOW BOUNDS: the report has no fit bounds' % n)
    else:
        ag = e3_floor.floor_agreement(gs, prefix + '_map_shadow.png', ours)
        ok = ag['iou'] >= args.maps_iou and ag['ours_px'] > 0 and ag['game_px'] > 0
        src = 'FITTED from the main camera (shadow_light_rule.h)' if ours.get('fitted') else 'from the game\'s caster state'
        lines.append('%s  frame %d: MAP SHADOW %s (%s; casters sampled %d): floor read like the colour pass, %d points: shadowed ours %d / game %d / both %d, IoU %.3f (bar %.2f), ours-in-game %.2f, game-in-ours %.2f'
                     % ('PASS' if ok else 'FAIL', n, 'MADE' if ok else 'DISTANCE', src, ours.get('samples', 0), ag['points'], ag['ours_px'], ag['game_px'], ag['inter'], ag['iou'], args.maps_iou, ag['ours_in_game'], ag['game_in_ours']))
        ci, cu, cv = e3_floor.casters_inside(ours)
        cok = ci == 8
        lines.append('%s  frame %d: MAP SHADOW %s: the casters box (8 corners, %d samples) read through our readers matrix: %d of 8 inside the map, u %.3f..%.3f v %.3f..%.3f'
                     % ('PASS' if cok else 'FAIL', n, 'COVERS the casters' if cok else 'BOUNDS (the casters are clipped by the map)', ours.get('samples', 0), ci, cu[0], cu[1], cv[0], cv[1]))
        try:   # the sheet: game map B | ours | the floor masks (game red, ours green, both yellow)
            gm = (np.clip(gs['map'], 0, 1) * 255).astype(np.uint8)
            gimg = np.stack([gm] * 3, axis=2)
            oimg = load(prefix + '_map_shadow.png')
            g_sh, o_sh = ag['masks']
            fl = np.zeros(g_sh.shape + (3,), np.uint8)
            fl[g_sh] = (255, 40, 40)
            fl[o_sh] = (40, 255, 40)
            fl[g_sh & o_sh] = (255, 255, 40)
            S = 384
            tiles = [label(np.asarray(Image.fromarray(gimg).resize((S, S))), 'GAME map B (stream) f%d' % n, 18), label(np.asarray(Image.fromarray(oimg).resize((S, S))), 'OURS shadow map (fitted)', 18),
                     label(np.asarray(Image.fromarray(fl).resize((S, S), Image.NEAREST)), 'floor shadow: game red / ours green / both yellow, IoU %.2f' % ag['iou'], 18)]
            sd = out if os.path.basename(out.rstrip(os.sep + '/')) == 'E3' else os.path.join(out, 'E3')
            os.makedirs(sd, exist_ok=True)
            Image.fromarray(np.concatenate(tiles, axis=1)).save(os.path.join(sd, 'sheet_shadow_f%03d%s.png' % (n, ('_' + args.maps_fault.replace(',', '+')) if args.maps_fault else '')))
        except Exception as e:   # the sheet is evidence, not a verdict
            lines.append('NOTE  frame %d: shadow sheet not written: %s' % (n, e))
    # the other maps: made, or a named, counted hole
    cs = re.search(r'maps_made (\d+) shadow_skip "([^"]*)" circles_skip "([^"]*)"', rep_text)
    if cs:
        lines.append('%s  frame %d: MAP circles: %s' % ('PASS' if not cs.group(3) else 'HOLE', n, 'made' if not cs.group(3) else 'NOT MADE (counted hole): ' + cs.group(3)))
    e2 = re.search(r'\[scene\]\[maps\]\[e2\] rim made \d+ \(objects \d+, meshes \d+; (.*?)\), plane made \d+ \(draws \d+; (.*?)\), pre-light made \d+ \(slots \d+; (.*?)\), sweat made \d+ \(items \d+; (.*)\)\s*$', rep_text, re.M)
    if e2:
        for nm, r in zip(('rim', 'plane', 'pre-light', 'sweat'), e2.groups()):
            made = r == 'ok' or r.startswith('FITTED')
            if nm == 'rim' and made:
                rp = re.search(r'map_rim_params \S+ \S+ (\S+) (\S+)', rep_text)
                if rp and gs is not None and gs.get('rim', (0, 0))[1] > 0:
                    on, of = float(rp.group(1)), float(rp.group(2))
                    gn, gf = gs['rim']
                    ok = abs(on - gn) <= 0.25 * (gf - gn) and abs(of - gf) <= 0.25 * (gf - gn)
                    lines.append('%s  frame %d: MAP RIM %s: depth range ours (%.1f, %.1f) vs the stream RimLightDepth (%.1f, %.1f): |dnear| %.1f, |dfar| %.1f (bar 25 %% of the game width %.1f)'
                                 % ('PASS' if ok else 'FAIL', n, 'MADE' if ok else 'DISTANCE', on, of, gn, gf, abs(on - gn), abs(of - gf), gf - gn))
                    continue
            lines.append('%s  frame %d: MAP %s: %s' % ('PASS' if made else 'HOLE', n, nm, 'made (%s)' % r if made else 'NOT MADE (counted hole): ' + r))
    return lines


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.uint8)


def heat(a, b):
    d = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2).astype(np.float32) / 255.0
    v = np.clip(d * 4.0, 0, 1)  # 25 % difference = full heat
    r = np.clip(v * 2, 0, 1)
    g = np.clip(v * 2 - 0.5, 0, 1)
    bl = np.clip(1 - v * 2, 0, 1) * 0.35
    return (np.stack([r, g, bl], axis=2) * 255).astype(np.uint8), float(d.mean()), float((d > 0.1).mean())


def label(img, text, h=30):
    out = Image.new('RGB', (img.shape[1], img.shape[0] + h), (24, 24, 28))
    out.paste(Image.fromarray(img), (0, h))
    dr = ImageDraw.Draw(out)
    try:
        font = ImageFont.truetype('arial.ttf', 15)
    except Exception:
        font = ImageFont.load_default()
    dr.text((6, 6), text, fill=(235, 235, 235), font=font)
    return np.asarray(out)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', required=True)
    ap.add_argument('--frames', default='20,110,200')
    ap.add_argument('--out', default=None)
    ap.add_argument('--pair-offset', type=int, default=0, help='plant: pair record N+K with stream frame N (a pairing fault must be reported)')
    ap.add_argument('--no-textures', action='store_true', help='plant: WWE_RDR_NO_TEXTURES=1 on our side (must show in the per-family numbers)')
    ap.add_argument('--mask', default=None, help='a family mask for the whole panel (0x0 drops everything: the empty-panel plant)')
    ap.add_argument('--size', default=None, help='WxH of our picture (default: the game picture\'s size)')
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN', help='a planted-fault run: exit 0 only when the run failed AND its output holds TOKEN (the twins are run this way)')
    ap.add_argument('--repeat', type=int, default=3, help='build the same record N times on one cache: no frame after the first may add a model (the live path builds every frame)')
    ap.add_argument('--particles-probe', action='store_true', help='S103 F: also draw a synthetic particle record through BuildFrame (viewer space) and check where the particles land')
    ap.add_argument('--particles-probe-fault', default=None, metavar='no_mirror', help='S103 F plant: the hook skips the viewer-space mirror: the probe must FAIL')
    ap.add_argument('--plant-leak', action='store_true', help='plant: a model added per repeat (the growth check must fail)')
    ap.add_argument('--look-from-stream', action='store_true', help='S103 L: inject the stage look rows (entrance flag + the rig slots) read from the game\'s own draw constants of the stream frame (tools\\lightmatch\\lm_extract.py): the rows a live writer will write (STAGE_LIGHTING.md)')
    ap.add_argument('--look-pools', action='store_true', help='S103 L2: with --look-from-stream, also inject the entrance page\'s pool + spot rows fitted from the game\'s own page (class B)')
    ap.add_argument('--plant-look', action='append', choices=['entrance', 'rig', 'wrong_slot', 'pools', 'spot', 'spot150', 'pool_swap'], help='S103 L plant: the entrance flag / the rig rows ignored, or the arena sets joined to the crowd slot')
    ap.add_argument('--camera-roundtrip', action='store_true', help='draw the record with the camera given on the command line, and again from a file whose camera section holds the same camera (record_io.h CAMERA LAYOUT): must be identical')
    ap.add_argument('--link', action='append', default=None, metavar='ID=CH:AT', help='TOLD wrestler models (repeatable); default per capture: DEFAULT_LINKS; --link none = no links')
    ap.add_argument('--no-body-scale', action='store_true', help='plant: no body scale is fed (neither measured nor TOLD): every Live wrestler must stay a HOLE (body scale ... is not known)')
    ap.add_argument('--no-hooks', action='store_true', help='plant: the maps and particle hooks are not attached (the HOOKS MISSING check must fail)')
    ap.add_argument('--see-order', default='game', choices=['game', 'distance'], help='the see-through order of our picture (distance = the plant: SEE ORDER must fail)')
    ap.add_argument('--assets', default='files', choices=['files', 'offline'], help="files = GameFilesSource (the game exe's loader); offline = tools\\scene\\offline_assets.h (the viewer seam): the pictures must be equal")
    ap.add_argument('--camera', default='stream', choices=['stream', 'record'], help="stream = the stream's first arena ViewProj (borrowed; every capture so far); record = the record's own camera (kCamProjFromFov) whenever it has one, compared with the stream's")
    ap.add_argument('--camera-factor-plant', action='store_true', help='plant: the factored view keeps det < 0 (an improper view): the VIEWPROJ compare must fail')
    ap.add_argument('--env-order', default=None, choices=['game', 'distance'], help='plant: sets WWE_RDR_ORDER for our draw (SEE ORDER must fail for distance although --see-order says game)')
    ap.add_argument('--order-check', action='store_true', help='also draw the other see-through order: at least one frame must differ between Game and Distance (ORDER PICTURE)')
    ap.add_argument('--assets-compare', action='store_true', help='also draw with --assets offline and require identical pictures (ASSET SOURCES)')
    ap.add_argument('--body-scale-one', action='store_true', help='S103 L9: rig-only fixture: every wrestler is told body scale 1 ([U]; a case from a live run has no body-scale table)')
    ap.add_argument('--plane-told', default=None, metavar='FILE', help='S103 L9: the plane inputs a record of an OURS frame does not carry yet (EyePos, PlanePos, fade, fit, camera: the game\'s own draw values), TOLD to the rig (a fixture)')
    ap.add_argument('--liquid-told', default=None, metavar='FILE', help='S103 SWF: the liquid-map fixture (frame_rig.cpp RigInjectLiquid)')
    ap.add_argument('--wrk-told', default=None, metavar='LIST', help='S103 WRK: the six g_fWklLv0..5 (then eight g_fIdMapLv0..7) every wrestler body is told, comma list (frame_rig.cpp)')
    ap.add_argument('--wrk-off', action='store_true', help='S103 WRK: the switch off (the picture before the lane)')
    ap.add_argument('--liquid-plant', default=None, metavar='LIST', help='S103 SWF: off | unbound | no_prev | mask_white (liquid_switch.h Plants)')
    ap.add_argument('--game-clear', action='store_true', help="S103 CLRB: the game's scene clear colour on a live record (black without a row) instead of the viewer's slate: WWE_RDR_GAME_CLEAR=1 for our draw")
    ap.add_argument('--rig-args', default='', metavar='ARGS', help='S103 PM2: extra arguments handed to the rig exe as they are (e.g. --prop-fixture x,y,z,half --scene-word 4: pm2_check.py)')
    ap.add_argument('--maps-check', action='store_true', help='S103 E3: the MAP SHADOW / MAP RIM checks (3 extra fview runs a frame); s103_E3 passes it, other lanes do not')
    ap.add_argument('--mapb-check', action='store_true', help='S103 MBC: the props in map B (needs --maps-check for the game\'s map): the prop footprint IoU >= 0.80 against the game\'s map B (a second rig run with mapb_wrestlers_only)')
    ap.add_argument('--mapb-scene-word', type=int, default=4, help='S103 MBC: the scene word the fixture run is given (default 4: entrance / cutscene)')
    ap.add_argument('--mapb-participants', type=int, default=None, help='S103 MBC: the participant count the fixture run is given (default: unknown)')
    ap.add_argument('--mapb-expect-mode1', action='store_true', help='S103 MBC (wm30): map B must be the game\'s mode 1 (wrestlers only): mode 1 in the census, no prop cast')
    ap.add_argument('--maps-fault', default=None, metavar='LIST', help='S103 E3 plant: live_maps.h MapsFaults (fit_view_not_light, fit_key_not_negated, fit_no_bounds, ...); the MAP SHADOW check must fail')
    ap.add_argument('--maps-iou', type=float, default=0.55, help='S103 E3: the bar of the MAP SHADOW floor agreement (IoU of the shadowed floor points, ours vs the game\'s map read the same way)')
    ap.add_argument('--camera-roundtrip-plant', action='store_true', help='plant: the file gets the TRANSPOSED camera (the layout fault): the pictures must differ')
    ap.add_argument('--no-sheet', action='store_true', help='S103 PS1: no first_sheet PNG and no per-family "what is missing" panel work (the pass / fail lines and the report file are the same); the proof checks that read numbers ask for this')
    ap.add_argument('--no-family-png', action='store_true', help='S103 PS1: the rig does not write the per-family panel PNGs (needs --no-sheet; the whole picture is still written)')
    ap.add_argument('--metrics-ref', action='append', default=[], metavar='LABEL=PNG', help='S103 PS1: the rig computes l_metrics\' numbers of this job against the reference picture PNG (repeatable; {n} = the frame, {game} = this run\'s game picture folder, {work} = the work folder) and writes them to --metrics-out')
    ap.add_argument('--metrics-device', default=None, choices=['gpu', 'cpu', 'both'], help='S103 PS1: where the rig measures: gpu (default: rig_metrics.comp on the rig\'s device), cpu, or both (a FAIL when they disagree)')
    ap.add_argument('--metrics-out', default=None, metavar='JSON', help='S103 PS1: where the metrics go ({n}, {work} placeholders)')
    ap.add_argument('--metrics-cov-store', default=None, metavar='KEY', help='S103 PS1: keep this job\'s covered-pixel masks under KEY (+ _<frame>) for the later arms (the `before` arm)')
    ap.add_argument('--metrics-cov-from', default=None, metavar='KEY', help='S103 PS1: measure every family over the pixels the stored KEY (+ _<frame>) masks cover')
    args = ap.parse_args(argv)
    if args.expect_fail is None:
        return run_main(args)
    import contextlib, io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = run_main(args)
    text = buf.getvalue()
    sys.stdout.write(text)
    caught = rc != 0 and args.expect_fail in text
    print(('PASS' if caught else 'FAIL') + '  planted fault %s: run exit %d, token "%s" %s' % ('caught' if caught else 'NOT caught', rc, args.expect_fail, 'found' if args.expect_fail in text else 'missing'))
    return 0 if caught else 1


def run_main(args):
    main_root = main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    capture = os.path.abspath(args.capture)
    name = os.path.basename(capture.rstrip('\\/'))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('R2', args.out)
    work = os.path.join(out, 'work_' + name)
    os.makedirs(work, exist_ok=True)
    fview = find_fview_once(ROOT)
    rig = os.path.join(ROOT, 'out', 'tools', 'frame_rig.exe')
    rsc_dump = os.path.join(ROOT, 'out', 'tools', 'rsc_dump.exe')
    for need in (rig, rsc_dump, game, capture):
        if not os.path.exists(need):
            print('FAIL  cannot read its input: ' + need)
            return 1
    frames = [int(x) for x in args.frames.split(',')]
    fails = []
    order_diffs = []

    # the game's pictures: one replay through the last frame, the chosen frames dumped
    gdir = os.path.join(work, 'game')
    rc, text, gdir0 = prep_replay(fview, capture, game, frames, gdir)   # S103 PS1: in a session the game's pictures of a (capture, frames) are made once; the other arms copy them
    if gdir0 != gdir and rc == 0:
        os.makedirs(gdir, exist_ok=True)
        for n_ in frames:
            if os.path.exists(os.path.join(gdir0, 'frame_%05d.png' % n_)):
                shutil.copyfile(os.path.join(gdir0, 'frame_%05d.png' % n_), os.path.join(gdir, 'frame_%05d.png' % n_))
    m = re.search(r'validation errors (\d+)', text)
    print('fview replay: exit %d, validation errors %s' % (rc, m.group(1) if m else '?'))
    if rc != 0:
        print('FAIL  fview replay: ' + text[-400:])
        return 1

    for n in frames:
        gpath = os.path.join(gdir, 'frame_%05d.png' % n)
        if not os.path.exists(gpath):
            print('FAIL  frame %d: fview wrote no game picture' % n)
            fails.append('game %d' % n)
            continue
        game_img = load(gpath)
        H, W = game_img.shape[:2]
        if args.size:
            W, H = (int(x) for x in args.size.split('x'))
        rec_n = n + args.pair_offset
        _bl = args.link if args.link is not None else DEFAULT_LINKS.get(name, [])
        os.environ['WWE_RIG_BODY_SCALES'] = '' if args.no_body_scale else prep_body_scales(capture, game, n, rec_n, _bl)   # S103 W2 (inherited by every frame_rig.exe call below)
        rsc = os.path.join(capture, 'scene', 'frame_%06d.rsc' % rec_n)
        report = []
        # ---- pairing ----
        sg = stream_guest_frame(capture, n)
        rg, dump_text = record_guest_frame(rsc_dump, rsc)
        paired = rg == sg
        line = ('PASS' if paired else 'FAIL') + '  pairing: stream frame %d guest_frame %d vs record frame_%06d guest_frame %s' % (n, sg, rec_n, rg)
        if not paired:
            line += '  -> PAIRING FAULT: the record is not this frame\'s own'
            fails.append('pairing %d' % n)
        print(line)
        report.append(line)
        rec_has_cam = 'camera: none' not in dump_text
        cam, why = prep_camera(fview, capture, n, game, work)
        print('  camera: %s' % why)
        report.append('camera: ' + why + (' (the record has its own camera: unused here)' if rec_has_cam else ' (the record has none)'))
        if cam is None:
            print('FAIL  frame %d: no camera' % n)
            fails.append('camera %d' % n)
            continue
        vp, eye, _, Mvp = cam
        json.dump({'vp': [float(x) for x in vp]}, open(os.path.join(work, 'camera_%d.json' % n), 'w'))  # S103 L2: the stream's row-vector ViewProj, for l2_check's projection of the entrant's root into the GAME's picture
        cinfo, crigid = camera_factor(Mvp)
        cfl = ('PASS' if crigid else 'FAIL') + ('  frame %d: CAMERA FACTOR (the stream ViewProj = rigid view x perspective): fov_y %.3f deg (y scale %.5f), aspect x/y %.4f, q %.6f, near %.4f, far %.1f, y scale sign for a proper-rotation view: %s (det %.4f), rigid error %.2e'
                                              % (n, cinfo['fov_y_deg'], cinfo['ys'], cinfo['aspect'], cinfo['q'], cinfo['near'], cinfo['far'], cinfo['ysign'], cinfo['det'], cinfo['rigid_err']))
        print(cfl)
        report.append(cfl)
        if not crigid:
            fails.append('camera factor %d' % n)
        # the planes and the sign the record-camera path assumes (record_frame.cpp: near 10, far 300000, y scale negative for a proper-rotation view). [U: the sign claim holds if the game's view has det +1]
        planes_ok = 9.0 < cinfo['near'] < 11.0 and 2.5e5 < cinfo['far'] < 3.5e5 and cinfo['det'] < 0
        pl = ('PASS' if planes_ok else 'FAIL') + '  frame %d: CAMERA PLANES: near %.3f (want ~10), far %.0f (want ~3e5), det %.3f (want < 0, i.e. a negative y scale) [U: the sign holds if the game view has det +1]' % (n, cinfo['near'], cinfo['far'], cinfo['det'])
        print(pl)
        report.append(pl)
        if not planes_ok:
            fails.append('camera planes %d' % n)
        env = dict(os.environ)
        if args.no_textures:
            env['WWE_RDR_NO_TEXTURES'] = '1'
        if args.game_clear:
            env['WWE_RDR_GAME_CLEAR'] = '1'   # S103 CLRB: a live record clears to the game's colour (black), as the game build does (the rig's default is the slate)
        if args.env_order:
            env['WWE_RDR_ORDER'] = args.env_order   # plant: the front-end's env override beats the frame's own choice: the check must read what was USED
        prefix =os.path.join(work, 'ours_%d' % n)
        for stale in glob.glob(prefix + '_*'):   # no stale pictures from an earlier run (a map PNG the hooks did not make this time must not survive)
            os.remove(stale)
        # the factored camera as a record's own would be (rigid game-space view + FOV, NO near / far: BuildFrame's defaults), drawn at 320x180 with an empty mask, and its view x projection
        # compared with the stream's ViewProj: both signs and the planes in one number. --camera-factor-plant keeps the improper (det < 0) view: the compare must fail.
        Mm = np.asarray(Mvp, dtype=np.float64)
        xs_, ys_ = np.linalg.norm(Mm[0, :3]), np.linalg.norm(Mm[1, :3])
        rxn, ryn = Mm[0, :3] / xs_, Mm[1, :3] / ys_
        ty_n = Mm[1, 3] / ys_
        sgn = 1.0 if args.camera_factor_plant else (-1.0 if cinfo['det'] < 0 else 1.0)
        Vcol = np.array([list(rxn) + [Mm[0, 3] / xs_], list(sgn * ryn) + [sgn * ty_n], list(Mm[3, :3]) + [Mm[3, 3]], [0, 0, 0, 1]])
        Vrow = Vcol.T.reshape(-1)
        fcmd = [rig, '--record', rsc, '--game', game, '--size', '320x180', '--prefix', prefix + '_fov', '--camera-view', ','.join('%.9g' % x for x in Vrow), '--fov', '%.9g' % cinfo['fov_y_deg'],
                '--eye', ','.join('%.9g' % x for x in eye), '--stream-viewproj', ','.join('%.9g' % x for x in vp), '--families', '0x0', '--no-hooks']
        pf = _rig_run(fcmd, None, 600)
        fm = re.search(r'viewproj_compare .*relative ([0-9.eE+-]+)', pf.stdout or '')
        vok = pf.returncode == 0 and fm is not None and float(fm.group(1)) < 2e-3
        vl = ('PASS' if vok else 'FAIL') + '  frame %d: VIEWPROJ: record-style camera (factored view + fov %.3f, default near / far) x BuildFrame\'s projection vs the stream ViewProj: relative error %s%s' % (n, cinfo['fov_y_deg'], fm.group(1) if fm else '?', '' if vok else '  -> the projection BuildFrame builds is not the game\'s')
        print(vl)
        report.append(vl)
        if not vok:
            fails.append('viewproj %d' % n)
        use_rec_cam = args.camera == 'record' and rec_has_cam
        if args.camera == 'record' and not rec_has_cam:
            report.append('camera mode record: this record has no camera, the stream\'s borrowed ViewProj is used (labelled)')
        cmd = [rig, '--record', rsc, '--game', game, '--size', '%dx%d' % (W, H), '--prefix', prefix, '--stream-viewproj', ','.join('%.9g' % x for x in vp)]
        if not use_rec_cam:
            cmd += ['--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye)]
        links = args.link if args.link is not None else DEFAULT_LINKS.get(name, [])
        links = [l for l in links if l != 'none']
        for l in links:
            cmd += ['--link', l]
        cmd += ['--see-order', args.see_order, '--assets', args.assets]
        if args.no_hooks:
            cmd += ['--no-hooks']
        if args.look_from_stream:  # S103 L: the stage look rows a live writer does not write yet, from the game's own draw constants of this stream frame (offline only)
            lk, lwhy = prep_look(capture, n, work)
            print('  look: ' + lwhy)
            report.append('look: ' + lwhy)
            cmd += lk
            if args.look_pools:  # S103 L2
                ent = entrant_of(rsc)
                pk, pwhy = prep_pools(fview, capture, n, game, work, ent)
                print('  pools: ' + pwhy)
                report.append('pools: ' + pwhy)
                cmd += pk
        for p in (args.plant_look or []):
            cmd += ['--plant-look', p]
        if args.mask is not None:
            cmd += ['--families', args.mask]
        else:
            cmd += ['--per-family']
        cmd += ['--repeat', str(args.repeat)]
        if args.no_family_png:
            cmd += ['--no-family-png']
        mkey = '%s_%d' % (name, n)   # S103 PS1: the rig's own metrics (rig_metrics.h): the reference pictures and where the numbers go, per frame
        fmt = lambda t_: t_.format(n=n, game=gdir, work=work)
        for mr in args.metrics_ref:
            cmd += ['--metrics-ref', fmt(mr)]
        if args.metrics_ref or args.metrics_cov_store:
            cmd += ['--metrics-out', fmt(args.metrics_out) if args.metrics_out else prefix + '_metrics.json']
        if args.metrics_device:
            cmd += ['--metrics-device', args.metrics_device]
        if args.metrics_cov_store:
            cmd += ['--metrics-cov-store', args.metrics_cov_store + ':' + mkey]
        if args.metrics_cov_from:
            cmd += ['--metrics-cov-from', args.metrics_cov_from + ':' + mkey]
        # S103 E3: the game's wrestler shadow map and its readers' constants from the stream (the comparison side). (S111 SHADOWKEY review: no key light is told to the rig any more: the fit reads none.)
        gs, gs_why = (None, 'hooks off') if (args.no_hooks or not args.maps_check) else e3_floor.game_shadow(fview, ROOT, game, capture, n, work)
        if gs is None and args.maps_check and not args.no_hooks:
            print('  E3: no game shadow data for frame %d: %s' % (n, gs_why))
        if args.maps_fault:
            cmd += ['--maps-fault', args.maps_fault]
        if args.rig_args:
            cmd += args.rig_args.split()   # S103 PM2
        if args.body_scale_one:
            cmd += ['--body-scale-one']   # S103 L9
        if args.plane_told:
            cmd += ['--plane-told', args.plane_told]   # S103 L9
        if args.liquid_told:
            cmd += ['--liquid-told', args.liquid_told]   # S103 SWF
        if args.liquid_plant:
            cmd += ['--liquid-plant', args.liquid_plant]   # S103 SWF
        if args.wrk_told:
            cmd += ['--wrk-told', args.wrk_told]   # S103 WRK
        if args.wrk_off:
            cmd += ['--wrk-off']   # S103 WRK
        if args.particles_probe_fault:
            cmd += ['--particles-probe-fault', args.particles_probe_fault]
        elif args.particles_probe:
            cmd += ['--particles-probe']
        if args.plant_leak:
            cmd += ['--plant-leak']
        if args.look_pools:
            env['WWE_RDR_L2_DUMP'] = prefix + '_page.raw'   # S103 L2 (B3): the composed page, written once per key by rf_arena_live.cpp
        p = _rig_run(cmd, env, 1800)
        if os.environ.get('WWE_RDR_L2_LOG'):  # S103 L2 (dev): the rig's stderr next to its pictures
            open(prefix + '_stderr.txt', 'w', encoding='utf8').write(p.stderr or '')
        if args.look_pools and os.path.exists(prefix + '_page.raw') and os.path.exists(os.path.join(work, 'page_%d.png' % n)):  # S103 L2 (B3): the composed page against the GAME's own page of this frame
            comp = np.fromfile(prefix + '_page.raw', dtype=np.uint8).reshape(512, 512, 4)[..., :3].astype(np.float64) / 255.0
            gpage = np.asarray(Image.open(os.path.join(work, 'page_%d.png' % n)).convert('RGB'), dtype=np.float64) / 255.0
            prms = float(np.sqrt(((comp - gpage) ** 2).mean()))
            pl = 'PAGE rms: frame %d composed page vs the game\'s page: %.4f' % (n, prms)
            print(pl)
            report.append(pl)
        if p.returncode != 0:
            print('FAIL  frame %d: frame_rig exit %d: %s' % (n, p.returncode, ((p.stdout or '')[-700:] + ' | ' + (p.stderr or '')[-300:])))
            fails.append('rig %d' % n)
            continue
        rep_text = open(prefix + '_report.txt').read()
        for pl in rep_text.splitlines():
            if pl.startswith('particle_probe'):
                print(pl.replace('particle_probe PASS', 'PASS  frame %d: particle_probe' % n, 1))
        if args.maps_check and not args.no_hooks:   # S103 E3 (review 1: only behind --maps-check): the wrestlers' shadow map MADE (from the record + the fit), and read like the colour pass reads the game's
            for ml in e3_check_maps(rep_text, gs, gs_why, n, args, work, out, prefix, game_img):
                print(ml)
                report.append(ml)
                if ml.startswith('FAIL'):
                    fails.append('map shadow %d' % n)
        if args.mapb_check and not args.no_hooks:   # S103 MBC
            for ml in mapb_check(rep_text, gs, gs_why, n, args, cmd, env, prefix):
                print(ml)
                report.append(ml)
                if ml.startswith('FAIL'):
                    fails.append('mapb %d' % n)
        verr = [int(x) for x in re.findall(r'validation_errors (\d+)', rep_text)]
        vline =('PASS' if verr and max(verr) == 0 else 'FAIL') + '  frame %d: validation errors %s' % (n, verr)
        print(vline)
        report.append(vline)
        if not (verr and max(verr) == 0):
            fails.append('validation %d' % n)
        gm = re.search(r'repeat (\d+): models made after the first build (\d+), after the last (\d+)', rep_text)
        if gm:
            grew = int(gm.group(3)) - int(gm.group(2))
            gl = ('PASS' if grew == 0 else 'FAIL') + '  frame %d: %s builds of the same record, models made after the first build %s, after the last %s (per-frame growth: %d)' % (n, gm.group(1), gm.group(2), gm.group(3), grew)
            print(gl)
            report.append(gl)
            if grew:
                fails.append('per-frame growth %d' % n)
        if args.assets_compare and args.mask is None:
            # offline_assets.h (the viewer seam, with lane B's model_key rule) must give the same pictures as GameFilesSource (the game exe's loader)
            c2 = [x for x in cmd]
            c2[c2.index('--assets') + 1] = 'offline'
            c2[c2.index('--prefix') + 1] = prefix + '_offl'
            c2 = [x for x in c2 if x != '--per-family']
            p3 = subprocess.run(c2, capture_output=True, text=True, errors='replace', env=env, timeout=1800)
            same_a = p3.returncode == 0 and open(prefix + '_offl_whole.png', 'rb').read() == open(prefix + '_whole.png', 'rb').read()
            al = ('PASS' if same_a else 'FAIL') + '  frame %d: ASSET SOURCES: offline_assets.h and GameFilesSource draw %s pictures%s' % (n, 'identical' if same_a else 'DIFFERENT', '' if p3.returncode == 0 else ' (offline run exit %d: %s)' % (p3.returncode, (p3.stderr or p3.stdout)[-200:]))
            print(al)
            report.append(al)
            if not same_a:
                fails.append('asset sources %d' % n)
        # ---- R2 checks: the live frame's terms ----
        hook_ok = '  hook [scene][maps]' in rep_text and '  hook [scene][particles]' in rep_text
        hl = ('PASS' if hook_ok else 'FAIL') + '  frame %d: the maps hook and the particle hook are attached%s' % (n, '' if hook_ok else '  -> HOOKS MISSING: the picture is not made as the live frame is')
        print(hl)
        report.append(hl)
        if not hook_ok:
            fails.append('hooks %d' % n)
        om = re.search(r'see_order requested (\w+) used (\w+)', rep_text)
        order_ok = om is not None and om.group(2) == 'game'
        ol = ('PASS' if order_ok else 'FAIL') + '  frame %d: the see-through order the front-end USED is the game\'s (requested %s, used %s, after the WWE_RDR_ORDER override)%s' % (
            n, om.group(1) if om else '?', om.group(2) if om else '?', '' if order_ok else '  -> SEE ORDER: the picture was drawn far to near, not in the game\'s order')
        print(ol)
        report.append(ol)
        if not order_ok:
            fails.append('see order %d' % n)
        if use_rec_cam:
            rm = re.search(r'viewproj_compare .*relative ([0-9.eE+-]+)', rep_text)
            rok = rm is not None and float(rm.group(1)) < 2e-3
            rl = ('PASS' if rok else 'FAIL') + '  frame %d: VIEWPROJ (the record\'s own camera): view x BuildFrame\'s projection vs the stream ViewProj, relative error %s' % (n, rm.group(1) if rm else '?')
            print(rl)
            report.append(rl)
            if not rok:
                fails.append('viewproj record %d' % n)
        if args.order_check and args.mask is None:
            # the other order, drawn: Game and Distance pictures must differ on at least one frame with see-through items (else the order switch tests nothing)
            alt = 'distance' if args.see_order == 'game' else 'game'
            c3 = [x for x in cmd if x != '--per-family']
            c3[c3.index('--see-order') + 1] = alt
            c3[c3.index('--prefix') + 1] = prefix + '_alt'
            p4 = subprocess.run(c3, capture_output=True, text=True, errors='replace', env=env, timeout=1800)
            if p4.returncode == 0:
                a_img, b_img = load(prefix + '_whole.png'), load(prefix + '_alt_whole.png')
                ndiff = int((np.abs(a_img.astype(np.int16) - b_img.astype(np.int16)).max(axis=2) > 0).sum())
                order_diffs.append(ndiff)
                ql = 'PASS  frame %d: ORDER PICTURE: %s order vs %s order differ in %d pixels' % (n, args.see_order, alt, ndiff)
            else:
                ql = 'FAIL  frame %d: ORDER PICTURE: the %s-order run failed (exit %d)' % (n, alt, p4.returncode)
                fails.append('order picture %d' % n)
            print(ql)
            report.append(ql)
        wr = re.findall(r'  family wrestlers objects (\d+) drawn (\d+) skipped (\d+) holes (\d+) why (.*)', rep_text.split('coverage:')[0].split('panel ', 1)[1].split('\npanel ')[0] + '\n')  # the first panel's block (the whole / mask panel)
        if links and wr:
            wobj, wdrawn, wskip, whole_, wwhy = wr[0]
            lm = re.search(r'links_total filled (\d+) kept (\d+)', rep_text)
            filled = int(lm.group(1)) if lm else 0
            known = KNOWN_HOLES.get(name) if (args.link is None and name in KNOWN_HOLES) else None
            allowed = known[0] if known else 0
            w_ok = filled > 0 and int(wdrawn) >= filled and int(whole_) == allowed   # every object a link filled must be drawn (not "at least one wrestler")
            kn = ('  [known holes %d: %s]' % known) if known else ''
            wl = ('PASS' if w_ok else 'FAIL') + '  frame %d: the TOLD wrestlers (%s) are drawn: %s of the %d object(s) the links filled (%s wrestler objects, %s holes)%s%s' % (
                n, ' '.join(links), wdrawn, filled, wobj, whole_, kn, '' if w_ok else '  -> WRESTLER HOLE: %s' % wwhy[:120])
            print(wl)
            report.append(wl)
            if not w_ok:
                fails.append('wrestlers %d' % n)
        holes = sum(int(x) for x in re.findall(r'  family \S+ objects \d+ drawn \d+ skipped \d+ holes (\d+)', rep_text.split('coverage:')[0].split('panel ', 1)[1].split('\npanel ')[0] + '\n'))
        print('  frame %d: holes (skips that leave a hole: asset unknown / load failed): %d' % (n, holes))
        report.append('holes: %d' % holes)
        if args.camera_roundtrip:
            rec_file = os.path.join(work, 'cam_%d.rsc' % n)
            vpw = vp.reshape(4, 4).T.reshape(-1) if args.camera_roundtrip_plant else vp  # the plant writes the transposed matrix
            w = subprocess.run([rig, '--record', rsc, '--game', game, '--prefix', prefix + '_cam', '--camera', ','.join('%.9g' % x for x in vpw), '--eye', ','.join('%.9g' % x for x in eye), '--write-record', rec_file] + [x for l in links for x in ('--link', l)],
                               capture_output=True, text=True, errors='replace', timeout=600)
            p2 = subprocess.run([rig, '--record', rec_file, '--game', game, '--size', '%dx%d' % (W, H), '--prefix', prefix + '_camfile'] + [x for l in links for x in ('--link', l)], capture_output=True, text=True, errors='replace', timeout=1800)
            same = w.returncode == 0 and p2.returncode == 0 and open(prefix + '_whole.png', 'rb').read() == open(prefix + '_camfile_whole.png', 'rb').read()
            cl = ('PASS' if same else 'FAIL') + '  frame %d: CAMERA LAYOUT: the picture from the command-line camera and from a record file holding it are %s' % (n, 'identical' if same else 'DIFFERENT')
            print(cl)
            report.append(cl)
            if not same:
                fails.append('camera layout %d' % n)
        # the texture check (per family): distinct 15-bit colours of a family's own panel; textured arena ~1000, crowd ~300 on brock_losswin; textures off 502 / 13.
        # S103 SW (2026-11-17): the arena bar 700 -> 600. Bisected: frame 20 read 710 at the E3 merge (0d0deb11f), 1672 at the L9 merge (f5347faca), 691 from the L10 merge (f32d3dfaf: the vertex-LM rule limited to
        # what the stream shows, a deliberate lighting fix that makes the entrance frame dark like the game's picture) and 695 now; the picture is textured (sheet), and --no-textures gives 439 (frame 20) / 475 (frame 110) here,
        # so 600 still separates textured from untextured on the frames the plant runs (110) and on 20. (Frame 200 with textures off reads 907: only the frame-110 plant discriminates there, as before.)
        for fam, need in (('arena', 600), ('crowd', 100)):
            cm = re.search(r'panel %s families \S+ objects_drawn (\d+) front_end_objects \d+ covered_pixels (\d+) of \d+ colours (\d+)' % fam, rep_text)
            if cm and int(cm.group(1)) and int(cm.group(2)):
                cnt = int(cm.group(3))
                ok_t = cnt >= need
                if args.look_from_stream and fam == 'arena':
                    # S103 L: a correctly (dark) lit arena has fewer 15-bit colours than a washed one, so the count cannot say "textured" here; the default run (no look rows) keeps the check
                    ok_t = True
                    tl = 'PASS  frame %d: arena panel has %s distinct colours (the count check is the default run\'s: the look rows darken the arena)' % (n, cnt)
                else:
                    tl = ('PASS' if ok_t else 'FAIL') + '  frame %d: %s panel has %s distinct colours (textured needs >= %d)' % (n, fam, cnt, need)
                print(tl)
                report.append(tl)
                if not ok_t:
                    fails.append('textures %s %d' % (fam, n))
        whole_png = prefix + ('_mask%d.png' % int(args.mask, 0) if args.mask is not None else '_whole.png')
        ours = load(whole_png)
        cov0 = re.search(r'panel (?:whole|mask\d+) families \S+ objects_drawn \d+ front_end_objects \d+ covered_pixels (\d+)', rep_text)
        empty = cov0 is not None and int(cov0.group(1)) == 0
        if empty:
            print('  frame %d: OUR whole panel is EMPTY (nothing drawn)' % n)
            fails.append('empty panel %d' % n)
        report.append(rep_text)
        if ours.shape != game_img.shape:
            ours = np.asarray(Image.fromarray(ours).resize((game_img.shape[1], game_img.shape[0])))
        hm, mean_d, frac = heat(game_img, ours)
        stat = 'mean |diff| %.1f %%, pixels off by >10 %%: %.1f %%' % (mean_d * 100, frac * 100)
        print('  frame %d: %s' % (n, stat))
        report.append('difference: ' + stat)
        suffix = ''
        if args.pair_offset:
            suffix += '_pair%d' % args.pair_offset
        if args.no_textures:
            suffix += '_notex'
        if args.mask is not None:
            suffix += '_mask'
        if not args.no_sheet:   # S103 PS1: --no-sheet skips the picture sheet and the per-family distance text (nothing a pass / fail line reads)
            # ---- the sheet ----
            told = (' + TOLD: ' + ' '.join(links)) if links else ''
            whole_label = 'OURS (record alone), whole: see-order %s%s%s' % (args.see_order, '' if args.no_hooks else ' + maps/particle hooks', told)
            if args.mask is not None:
                whole_label += ' [families mask %s]' % args.mask
            top = [label(game_img, 'GAME: %s stream frame %d (fview, no display curve)' % (name, n)),
                   label(ours, '%s: record frame %d (no display curve)%s' % (whole_label, rec_n, '  EMPTY' if empty else '')),
                   label(hm, 'DIFFERENCE heat (25%% = full); %s' % stat)]
            tw = 427
            def small(a, text):
                im = Image.fromarray(a).resize((tw, int(a.shape[0] * tw / a.shape[1])))
                return label(np.asarray(im), text, 28)
            panels = []
            whole_block = rep_text.split('coverage:')[0]
            for fam in FAMILIES:
                pp = prefix + '_%s.png' % fam
                fr = re.findall(r'  family %s objects (\d+) drawn (\d+) skipped (\d+) holes (\d+) why (.*)' % fam, whole_block)
                cov = re.findall(r'panel %s families.*?covered_pixels (\d+)' % fam, rep_text)
                if os.path.exists(pp) and fr:
                    o, d, s_, hl, why = fr[0]
                    c = int(cov[0]) if cov else 0
                    panels.append(small(load(pp), '%s%s: %s objs, drawn %s, skipped %s%s%s' % (fam, (' + TOLD' if (fam == 'wrestlers' and links) else ''), o, d, s_, ' (%s)' % why[:30] if s_ != '0' else '', '  EMPTY' if c == 0 else '')))
            # the maps (lane E's hook made them; the game's own maps are the stream's, not drawn here) as panels, and "what is missing" per family
            for mname, mlabel in (('map_shadow', 'MAP shadow (ours, from the record)'), ('map_circles', 'MAP circles (ours, from the record)')):
                mp = prefix + '_%s.png' % mname
                if os.path.exists(mp) and not args.no_hooks:
                    mi = load(mp)
                    made = int((np.abs(mi.astype(np.int16) - 255).max(axis=2) > 0).sum())
                    panels.append(small(mi, '%s: %d px differ from clear%s' % (mlabel, made, '  EMPTY' if made == 0 else '')))
                elif not args.no_hooks:
                    panels.append(small(np.zeros((game_img.shape[0] // 2, game_img.shape[1] // 2, 3), np.uint8), '%s: NOT MADE (inputs missing in this record)' % mlabel))
            missing = []
            for fam in FAMILIES:
                fr = re.findall(r'  family %s objects (\d+) drawn (\d+) skipped (\d+) holes (\d+) why (.*)' % fam, whole_block)
                pp = prefix + '_%s.png' % fam
                if not fr:
                    continue
                o, d, s_, hl_, why = fr[0]
                dist = ''
                if os.path.exists(pp):
                    fi = load(pp)
                    if fi.shape != game_img.shape:
                        fi = np.asarray(Image.fromarray(fi).resize((game_img.shape[1], game_img.shape[0])))
                    cov_m = (np.abs(fi.astype(np.int16) - np.array(CLEAR, np.int16)).max(axis=2) > 2)
                    if cov_m.any():
                        dd = np.abs(game_img.astype(np.int16) - fi.astype(np.int16)).max(axis=2)[cov_m].astype(np.float32) / 255.0
                        dist = 'covers %.1f %% of the frame; DISTANCE to the game over its own pixels: mean |diff| %.1f %%, off by >10 %%: %.1f %%' % (100.0 * cov_m.mean(), 100.0 * dd.mean(), 100.0 * (dd > 0.1).mean())
                    else:
                        dist = 'covers 0 % of the frame: EMPTY (off screen or hidden in this view: unconfirmed which)'
                gaps = []
                if s_ != '0':
                    gaps.append('%s skipped (%s)' % (s_, why[:90]))
                if int(hl_):
                    gaps.append('%s HOLES' % hl_)
                if DOC_GAPS.get(fam):
                    gaps.append('docs: ' + DOC_GAPS[fam])
                missing.append('%-9s %s of %s drawn. %s%s' % (fam, d, o, dist, ('  MISSING: ' + '; '.join(gaps)) if gaps else '  missing: nothing the record or the hooks name'))
            mm = re.search(r'  maps_made (\d+) shadow_skip "([^"]*)" circles_skip "([^"]*)"[^;]*; light: ([^;]*); record has caster calls (\d) circle rows (\d); particle batches (\d+)', rep_text)
            if mm:
                missing.append('maps      made %s (shadow skip "%s", circles skip "%s"); light %s; the record carries caster calls %s, circle rows %s. Plane / rim / pre-light / sweat maps are not built (LIVE_MAPS.md 4).' % (mm.group(1), mm.group(2) or '-', mm.group(3) or '-', mm.group(4), 'yes' if mm.group(5) == '1' else 'NO', 'yes' if mm.group(6) == '1' else 'NO'))
                missing.append('particles %s particle batches in the record' % mm.group(7))
            pc = re.search(r'\[scene\]\[particles\] (.*)', rep_text)
            if pc:
                missing.append('particles hook: ' + pc.group(1)[:200] + '  (no particle texture lookup in the rig: white stand-in)')
            if links:
                missing.append('TOLD (not in the record): ' + ', '.join(links) + ' -- the models come from --link; unconfirmed that they are the match\'s wrestlers')
            # first row: the three big panels scaled to a common width
            bw = 640
            def fit(a, w=bw):
                im = Image.fromarray(a)
                return np.asarray(im.resize((w, int(a.shape[0] * w / a.shape[1]))))
            big = [fit(a) for a in top]
            mh = max(a.shape[0] for a in big)
            big = [np.pad(a, ((0, mh - a.shape[0]), (0, 0), (0, 0))) for a in big]
            row1 = np.concatenate(big, axis=1)
            rows = [row1]
            if panels:
                per = row1.shape[1] // tw
                ph = max(a.shape[0] for a in panels)
                panels = [np.pad(a, ((0, ph - a.shape[0]), (0, 0), (0, 0))) for a in panels]
                for i in range(0, len(panels), per):
                    chunk = panels[i:i + per]
                    while len(chunk) < per:
                        chunk.append(np.zeros_like(panels[0]))
                    r = np.concatenate(chunk, axis=1)
                    r = np.pad(r, ((0, 0), (0, row1.shape[1] - r.shape[1]), (0, 0)))
                    rows.append(r)
            # the "what is missing" block, one line per family (the distance line), as text under the panels
            tl_h = 20 * (len(missing) + 1) + 10
            timg = Image.new('RGB', (row1.shape[1], tl_h), (24, 24, 28))
            td = ImageDraw.Draw(timg)
            try:
                tfont = ImageFont.truetype('arial.ttf', 13)
            except Exception:
                tfont = ImageFont.load_default()
            td.text((6, 4), 'WHAT IS MISSING per family (measured from this run; "docs:" = a page names it). The per-family panels above EXCLUDE the maps and the particles (whole panel only):', fill=(255, 220, 120), font=tfont)
            for k, ln in enumerate(missing):
                td.text((6, 24 + 20 * k), ln[:230], fill=(225, 225, 225), font=tfont)
            rows.append(np.asarray(timg))
            report.extend(missing)
            sheet = np.concatenate(rows, axis=0)
            suffix = ''
            if args.pair_offset:
                suffix += '_pair%d' % args.pair_offset
            if args.no_textures:
                suffix += '_notex'
            if args.mask is not None:
                suffix += '_mask'
            spath = os.path.join(out, 'first_sheet_f%03d%s.png' % (n, suffix))
            Image.fromarray(sheet).save(spath)
            print('  sheet: ' + spath)
        with open(os.path.join(out, 'frame_%03d%s_report.txt' % (n, suffix)), 'w') as f:
            f.write('\n'.join(report) + '\n')
    if args.order_check and order_diffs and max(order_diffs) == 0:
        print('FAIL  ORDER PICTURE: the Game and Distance see-through orders drew identical pictures on every frame (%d frames): the order switch is not tested by these frames' % len(order_diffs))
        fails.append('order picture (all frames equal)')
    if fails:
        print('FAIL  ' + '; '.join(fails))
        return 1
    print('PASS  frame_rig')
    return 0


if __name__ == '__main__':
    sys.exit(main())
