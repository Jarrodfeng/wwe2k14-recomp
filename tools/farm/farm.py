r"""The replay farm (Session 101, lane 22, RENDERER_ARC.md section 4 tool 7): every capture we keep, replayed by rplay with no game.
Hidden window, BELOW_NORMAL priority, one rplay at a time, a hard timeout per run. Per capture: frame times (median / p99), pictures of fixed frames.
  py -I tools\farm\farm.py list
  py -I tools\farm\farm.py run      [--build RPLAY.EXE] [--only GLOB[,GLOB]] [--runs N] [--timeout S] [--host local] [--out DIR] [--rplay-args "..."]
  py -I tools\farm\farm.py baseline [--from RUNDIR]        # latest run -> out\farm\baseline\ (with a KEEP file)
  py -I tools\farm\farm.py compare  [--run RUNDIR] [--base DIR]   # exit 1 on SLOWER / DIFFERS / FAIL
  py -I tools\farm\farm.py check    [--only ...]           # run + compare against the baseline in one go
  py -I tools\farm\farm.py promo    [--only GLOB] [--arms crowd,arena,wrestlers,all] [--game DIR] [--runs N]   # (S101 lane 47) paired A/B: promotion off vs on, GPU time per frame
Docs: docs\renderer\FARM.md. Corpus = out\captures\* (folders), out\s101\runs\*\case_*.zip, out\corpus\* (folders or zips) of the MAIN checkout.
Pictures are game imagery: they stay under out\farm\ (local), never committed.
"""
import argparse, csv, fnmatch, json, os, re, shutil, statistics, subprocess, sys, time, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]


def main_checkout():
    # The main checkout (where out\ and game_data\ live), also from a worktree: git's common dir is the main .git.
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True,
                                timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return HERE


MAIN = Path(os.environ.get('RDR_MAIN_CHECKOUT', main_checkout()))
FARM = MAIN / 'out' / 'farm'
DEFAULT_RPLAY = HERE / 'out' / 'renderer' / 'dev' / 'rplay.exe'

# Thresholds (docs\renderer\FARM.md "the noise band"): chosen from the measured A/A band of the same build, see the page.
THR_MEDIAN = 0.12      # frame time median this much slower than the reference AND at least MEDIAN_FLOOR_MS slower = SLOWER (paired A/B band, 5 runs ABBA: -8.3 % .. +6.8 %)
MEDIAN_FLOOR_MS = 0.25 # a 0.8 ms capture moved +26 % (0.2 ms) in a same-build A/A: under the floor a ratio means nothing
THR_P99 = 0.30         # p99 this much slower AND at least P99_FLOOR_MS (captures with at least MIN_FRAMES_P99 frames; band -8 % .. +16 %)
P99_FLOOR_MS = 0.5
MIN_FRAMES_TIMING = 12  # fewer frames (under 11 steady ones after frame 0) are replayed for pictures only; their timing is printed, not judged
MIN_FRAMES_P99 = 60
THR_GPU = 0.10         # the GPU's own time (timestamps around each frame's command buffer): same-build A/A moved at most 1.1 %; SLOWER = this much AND GPU_FLOOR_MS in ms
GPU_FLOOR_MS = 0.05
# owned_check's picture tolerance (tools\native\owned_verdict.py, Session 82): p99.9 of the per-pixel largest channel difference <= 8,
# no 16x16 block mean over 2, share of pixels over 4 <= noise + 0.3 %.
TOL_P999, TOL_BLOCK, TOL_SMALL, TOL_SHARE = 8.0, 2.0, 4, 0.003
# S101 lane 47: the promotion switches rplay takes (src\renderer_scene\promotion_rig.h). A run whose --rplay-args holds one is an ARM (labelled in run.json, timing printed against
# the reference, pictures expected to differ), not a "test-only" planted-fault run; it is still never a baseline.
PROMO_SWITCHES = ('--crowd-ours', '--arena-ours', '--wrestlers-ours')
PROMO_ARMS = {'crowd': ['--crowd-ours'], 'arena': ['--arena-ours'], 'wrestlers': ['--wrestlers-ours'], 'all': ['--crowd-ours', '--arena-ours', '--wrestlers-ours']}


# ---------------------------------------------------------------- hosts: where rplay runs
class LocalHost:
    """This PC: rplay hidden, below normal priority, one at a time (the GPU is shared)."""
    name = 'local'

    def run_rplay(self, exe, args, timeout_s, log_path):
        flags = 0
        if os.name == 'nt':
            flags = subprocess.BELOW_NORMAL_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW
        for attempt in range(6):
            with open(log_path, 'w', encoding='utf-8', errors='replace') as log:
                proc = subprocess.Popen([str(exe)] + args, stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
                try:
                    code = proc.wait(timeout=timeout_s)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    return 'timeout'
            # Other lanes' rplay / fview share this card: when theirs hold the host-visible memory ours cannot start (exit 2, no frame ring). Wait, try again.
            if code == 2 and 'Not enough graphics memory' in Path(log_path).read_text(errors='replace'):
                time.sleep(10 + 10 * attempt)
                continue
            return code
        return code


class SurfaceHost:
    """The seam, not built: rplay on the Surface Pro 6 over SSH (tools\\surface\\: push the capture and rplay.exe, run, pull timing.csv + pictures)."""
    name = 'surface'

    def run_rplay(self, *a, **k):
        raise NotImplementedError('farm host "surface" is designed (docs\\renderer\\FARM.md) but not built')


class PhoneHost:
    """The seam, not built: rplay as an Android binary over adb (tools\\android\\: adb push, adb shell, adb pull)."""
    name = 'phone'

    def run_rplay(self, *a, **k):
        raise NotImplementedError('farm host "phone" is designed (docs\\renderer\\FARM.md) but not built')


HOSTS = {'local': LocalHost, 'surface': SurfaceHost, 'phone': PhoneHost}


# ---------------------------------------------------------------- corpus
class Item:
    def __init__(self, name, path, kind):
        self.name, self.path, self.kind = name, Path(path), kind

    def __repr__(self):
        return f'{self.name} ({self.kind})'


def corpus():
    items = []
    caps = MAIN / 'out' / 'captures'
    if caps.is_dir():
        for p in sorted(caps.iterdir()):
            if (p / 'images.bin').exists() and (p / 'frames').is_dir():
                items.append(Item(p.name, p, 'dir'))
    runs = MAIN / 'out' / 's101' / 'runs'
    if runs.is_dir():
        for run in sorted(runs.iterdir()):
            zips = sorted(run.glob('case_*.zip'))
            for k, z in enumerate(zips):
                items.append(Item(run.name + (f'_{k + 1}' if len(zips) > 1 else ''), z, 'zip'))
    extra = MAIN / 'out' / 'corpus'
    if extra.is_dir():
        for p in sorted(extra.iterdir()):
            if p.suffix == '.zip':
                items.append(Item('corpus_' + p.stem, p, 'zip'))
            elif (p / 'images.bin').exists() and (p / 'frames').is_dir():
                items.append(Item('corpus_' + p.name, p, 'dir'))
    seen = {}
    for it in items:  # names must be unique
        seen[it.name] = seen.get(it.name, 0) + 1
        if seen[it.name] > 1:
            it.name += f'_{seen[it.name]}'
    return items


def select(items, only):
    if not only:
        return items
    pats = [x for x in only.split(',') if x]
    return [i for i in items if any(fnmatch.fnmatch(i.name, p) for p in pats)]


def open_capture(item, cache):
    """A folder rplay can read: the capture folder itself, or the zip's capture\\ extracted under cache (removed by the caller)."""
    if item.kind == 'dir':
        return item.path, False
    dest = cache / item.name
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(item.path) as z:
        members = [m for m in z.namelist() if m.startswith('capture/') and not m.endswith('/')]
        if not members:
            raise RuntimeError(f'{item.path.name}: no capture\\ folder in the zip')
        for m in members:
            target = dest / m[len('capture/'):]
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(m) as src, open(target, 'wb') as out:
                shutil.copyfileobj(src, out)
    return dest, True


def frame_count(folder):
    return len(list((folder / 'frames').glob('frame_*.bin')))


# ---------------------------------------------------------------- measuring
def gpu_state():
    try:
        out = subprocess.run(['nvidia-smi', '--query-gpu=utilization.gpu,clocks.gr,clocks.max.gr,pstate', '--format=csv,noheader,nounits'],
                             capture_output=True, text=True, timeout=15).stdout.strip().split('\n')[0].split(',')
        return {'util_pct': int(out[0]), 'clock_mhz': int(out[1]), 'clock_max_mhz': int(out[2]), 'pstate': out[3].strip()}
    except Exception:
        return None


CLOCK_LOW = 0.90   # S101 lane 47: a run whose median graphics clock is under 90 % of the best run's is flagged and re-run (the GPU's clock state moved the same code 0.57 -> 2.06 ms: PROMO_GPU_COST.md)
CLOCK_REDO = 3     # at most this many re-run rounds before the capture FAILs as "mixed clocks"


class ClockSampler:
    """`nvidia-smi --query-gpu=clocks.gr -lms 100` to a pipe while a replay runs (stdout, not -f: a killed writer leaves a file empty); a reader thread keeps the samples. median() is None when
    nvidia-smi is missing or printed nothing (then the clock check is skipped and said so)."""

    def __init__(self):
        self.samples, self.proc, self.thread = [], None, None

    def __enter__(self):
        try:
            self.proc = subprocess.Popen(['nvidia-smi', '--query-gpu=clocks.gr', '--format=csv,noheader,nounits', '-lms', '100'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                                         creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        except OSError:
            self.proc = None
            return self
        import threading

        def pump():
            for line in self.proc.stdout:
                line = line.strip().split()[0] if line.strip() else ''
                if line.isdigit():
                    self.samples.append(int(line))
        self.thread = threading.Thread(target=pump, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *a):
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            if self.thread:
                self.thread.join(5)

    def median(self):
        return statistics.median(self.samples) if len(self.samples) >= 3 else None


def flag_low_clocks(clocks, thr=CLOCK_LOW):
    """clocks: key -> median MHz (or None). The keys whose clock is under thr x the best run's. None entries are not judged (no sampler)."""
    known = {k: v for k, v in clocks.items() if v}
    if not known:
        return []
    best = max(known.values())
    return sorted(k for k, v in known.items() if v < thr * best)


def systematic_tags(clocks, low):
    """Tags (the first element of the key) ALL of whose runs are low: the arm itself runs at another clock (the driver lowers the clock when a replay keeps the GPU less busy, e.g. an arm with a
    heavy CPU side), so a re-run cannot fix it. Its GPU time is not comparable with the arms that ran at the best clock."""
    tags = {k[0] for k in clocks if clocks[k]}
    return sorted(t for t in tags if all(k in low for k in clocks if k[0] == t and clocks[k]))


def settle_clocks(clocks, redo, max_rounds=CLOCK_REDO):
    """Re-run (redo(key) -> new median MHz) the runs whose clock is low, up to max_rounds rounds; an arm that is low on EVERY run is systematic and is not re-run.
    Returns (still-low keys of non-systematic arms, systematic tags). A non-empty first list = mixed clocks inside one arm: the capture FAILs (a verdict never rests on mixed clocks)."""
    for _ in range(max_rounds):
        low = flag_low_clocks(clocks)
        sys_tags = systematic_tags(clocks, low)
        todo = [k for k in low if k[0] not in sys_tags]
        if not todo:
            break
        for k in todo:
            clocks[k] = redo(k)
    low = flag_low_clocks(clocks)
    sys_tags = systematic_tags(clocks, low)
    return [k for k in low if k[0] not in sys_tags], sys_tags


def measured(host, exe, cmd, timeout_s, log_path):
    """One measured replay with the clock sampled beside it: (exit code, median graphics clock MHz or None)."""
    with ClockSampler() as clk:
        code = host.run_rplay(exe, cmd, timeout_s, log_path)
    return code, clk.median()


def split_args(text):
    """--rplay-args: shell-style split with quotes honoured and backslashes kept (Windows paths); a path with spaces works in "double quotes"."""
    import shlex
    return [t[1:-1] if len(t) > 1 and t[0] == t[-1] and t[0] in '"\'' else t for t in shlex.split(text or '', posix=False)]


def read_timing(path):
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            rows.append({k: float(v) for k, v in r.items()})
    return rows


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(p * len(s)))] if s else 0.0


def summarise(run_csvs):
    """Per frame: the median over the runs (a spike in one run does not count); then median / p99 of those, warm-up frames dropped."""
    tables = [read_timing(p) for p in run_csvs]
    n = min(len(t) for t in tables)
    totals, cpus, gpus, stamps, draws = [], [], [], [], []
    for i in range(n):
        tot = [t[i]['load_us'] + t[i]['bind_us'] + t[i]['render_us'] + t[i]['gpu_us'] for t in tables]
        cpu = [t[i]['load_us'] + t[i]['bind_us'] + t[i]['render_us'] for t in tables]
        totals.append(statistics.median(tot))
        cpus.append(statistics.median(cpu))
        gpus.append(statistics.median(t[i]['gpu_us'] for t in tables))
        stamps.append(statistics.median(t[i].get('gpu_ns', 0.0) for t in tables) / 1000.0)  # us; 0 when the build or the queue has no timestamps
        draws.append(tables[0][i]['draws'])
    skip = (1 if n >= 2 else 0) + (n // 20 if n >= 20 else 0)  # frame 0 builds the pipelines on first sight (its own number: first_ms); a few more on long captures
    body = slice(skip, None)
    t, c, g, k = totals[body], cpus[body], gpus[body], stamps[body]
    return {
        'frames': n, 'skipped_warmup': skip, 'first_ms': totals[0] / 1000 if totals else 0, 'draws_per_frame': statistics.mean(draws) if draws else 0,
        'median_ms': statistics.median(t) / 1000, 'p99_ms': pct(t, 0.99) / 1000, 'mean_ms': statistics.mean(t) / 1000,
        'cpu_median_ms': statistics.median(c) / 1000,
        'gpu_median_ms': statistics.median(g) / 1000,                    # WaitIdle after the frame: GPU execution + submit / wake latency
        'gpu_ts_ms': statistics.median(k) / 1000 if any(k) else None,    # the GPU's own time between two timestamps in the frame's command buffer
        'gpu_ts_p99_ms': pct(k, 0.99) / 1000 if any(k) else None,        # S101 lane 47: its p99 over the frames
        'gpu_ts_mean_ms': statistics.mean(k) / 1000 if any(k) else None,
        'total_s': sum(totals) / 1e6,
        # run-to-run spread of the whole-capture median, from the individual runs (the noise the thresholds must clear)
        'run_medians_ms': [statistics.median([r['load_us'] + r['bind_us'] + r['render_us'] + r['gpu_us'] for r in tb[skip:]]) / 1000 for tb in tables],
    }


def shot_frames(n):
    return sorted({0, n // 2, max(0, n - 1)})


LOCK = FARM / 'lock'


class FarmLock:
    """One farm (one rplay) at a time across processes: a lock directory, stale after 3 h. Waits up to 2 h."""

    def __enter__(self):
        FARM.mkdir(parents=True, exist_ok=True)
        waited = 0
        while True:
            try:
                os.mkdir(LOCK)
                (LOCK / 'owner.txt').write_text(f'pid {os.getpid()} {time.strftime("%Y-%m-%d %H:%M:%S")}\n')
                return self
            except FileExistsError:
                try:
                    if time.time() - LOCK.stat().st_mtime > 3 * 3600:
                        shutil.rmtree(LOCK, ignore_errors=True)
                        continue
                except OSError:
                    pass
                if waited == 0:
                    print(f'farm: another farm run holds {LOCK}; waiting', flush=True)
                if waited > 7200:
                    raise RuntimeError(f'farm lock {LOCK} still held after 2 h')
                time.sleep(5)
                waited += 5

    def __exit__(self, *a):
        shutil.rmtree(LOCK, ignore_errors=True)


def cmd_run(args):
    with FarmLock():
        return run_locked(args)


def run_locked(args):
    exe = Path(args.build) if args.build else DEFAULT_RPLAY
    if not exe.exists():
        print(f'FAIL no rplay at {exe} (build: pwsh -NoProfile -File tools\\renderer\\build.ps1)')
        return 1
    ref = Path(args.ref) if args.ref else None
    if ref and not ref.exists():
        print(f'FAIL no reference rplay at {ref}')
        return 1
    host = HOSTS[args.host]()
    items = select(corpus(), args.only)
    if not items:
        print('FAIL no captures selected (py -I tools\\farm\\farm.py list)')
        return 1
    stamp = time.strftime('%Y%m%d_%H%M%S')
    out = Path(args.out) if args.out else FARM / 'runs' / stamp
    out.mkdir(parents=True, exist_ok=True)
    args.out_resolved = str(out)
    cache = out / '_cache'  # under this run's folder: another run's cleanup cannot delete it
    used = out / 'rplay_used.exe'  # the exact exe this run measured (a renderer build replaces out\renderer\dev\rplay.exe): a baseline takes this copy
    shutil.copy2(exe, used)
    extra = split_args(args.rplay_args)
    meta = {'stamp': stamp, 'rplay': str(exe), 'rplay_copy': str(used), 'rplay_mtime': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(exe.stat().st_mtime)),
            'host': host.name, 'ref': str(ref) if ref else None, 'runs_each': args.runs, 'extra_rplay_args': extra, 'gpu_before': gpu_state(), 'captures': {}}
    arm = [x for x in extra if x in PROMO_SWITCHES]
    if arm:
        meta['arm'] = arm  # S101 lane 47: a promotion arm (rplay_args holds the switches; --game DIR is among them)
    try:
        meta['git'] = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    except Exception:
        pass
    began = time.time()
    failed = 0
    for item in items:
        t0 = time.time()
        entry = {'kind': item.kind, 'status': 'ok'}
        meta['captures'][item.name] = entry
        folder, temp = None, False
        try:
            cdir = out / item.name
            if cdir.exists():
                shutil.rmtree(cdir)  # a reused --out must not keep a previous run's shots: a build that stops writing one would pass on the old PNG
            cdir.mkdir(parents=True)
            folder, temp = open_capture(item, cache)
            n = frame_count(folder)
            entry['frames'] = n
            shots = shot_frames(n)
            entry['shot_frames'] = shots
            entry['gpu_before'] = gpu_state()
            arms = [('', exe, extra)] + ([('ref', ref, [])] if ref else [])  # ref = the reference build, run in turn with the new one (paired: drift hits both)
            for tag, e_exe, e_extra in arms:  # warm-up replays, not kept: the card leaves its idle clock (LESSONS: 30-60 % from clock state alone), driver and file caches fill
                for w in range(args.warm):
                    host.run_rplay(e_exe, ['--capture', str(folder), '--validation', '0', '--frames', str(n)] + e_extra, args.timeout, cdir / f'warm{tag}.log')
            entry['gpu_warm'] = gpu_state()
            csvs = {tag: {} for tag, _, _ in arms}
            clocks = {}   # (tag, run) -> median graphics clock MHz during that replay
            arm_of = {tag: (e_exe, e_extra) for tag, e_exe, e_extra in arms}

            def one_run(tag, r):
                e_exe, e_extra = arm_of[tag]
                rdir = cdir / tag / f'run{r}'
                rdir.mkdir(parents=True, exist_ok=True)
                csv_path = rdir / 'timing.csv'
                if csv_path.exists():
                    csv_path.unlink()
                cmd = ['--capture', str(folder), '--validation', '0', '--frames', str(n), '--timing', str(csv_path),
                       '--shot-dir', str(rdir / 'shots'), '--shots', ','.join(map(str, shots))] + e_extra
                code, mhz = measured(host, e_exe, cmd, args.timeout, rdir / 'rplay.log')
                if code != 0 or not csv_path.exists():
                    entry['status'] = f'FAIL rplay {code} ({tag or "new"} run {r}, {rdir / "rplay.log"})'
                    return None
                csvs[tag][r] = csv_path
                clocks[(tag, r)] = mhz
                return mhz
            for r in range(args.runs):
                for tag, e_exe, e_extra in (arms if r % 2 == 0 else arms[::-1]):  # ABBA: which arm goes first alternates, so "second in line" favours neither
                    one_run(tag, r)
                    if entry['status'] != 'ok':
                        break
                if entry['status'] != 'ok':
                    break
            if entry['status'] == 'ok':
                def redo(key):
                    return one_run(*key) if entry['status'] == 'ok' else None
                still, systematic = settle_clocks(clocks, redo)
                entry['clocks_mhz'] = {f'{t or "new"}/run{r}': v for (t, r), v in sorted(clocks.items())}
                # `run` / `check` (is this build slower?) record the clocks and re-run low runs, but a light capture (a 0.14 ms GPU frame) idles its GPU by itself, so a mixed clock here is a NOTE
                # printed by compare, never a FAIL (the planted-delay control also lowers its own clock). The GPU-cost question (`promo`) is where mixed clocks FAIL / are NOT COMPARABLE.
                entry['clock_note'] = [f'{t or "new"}/run{r}' for t, r in still] + [f'{t or "new"} (every run)' for t in systematic]
            csvs = {tag: [csvs[tag][r] for r in sorted(csvs[tag])] for tag in csvs}
            entry['gpu_after'] = gpu_state()
            if entry['status'] == 'ok':
                entry.update(summarise(csvs['']))
                entry['noise'] = noise_of(cdir / '', args.runs, shots)
                if ref:
                    entry['ref'] = summarise(csvs['ref'])
                    entry['ref']['noise'] = noise_of(cdir / 'ref', args.runs, shots)  # the picture noise is the REFERENCE's own (a new build that jitters must not widen its tolerance)
        except Exception as e:
            entry['status'] = f'FAIL {type(e).__name__}: {e}'
        finally:
            if temp and folder and not args.keep_cache:
                shutil.rmtree(folder, ignore_errors=True)
        entry['wall_s'] = round(time.time() - t0, 1)
        if entry['status'] != 'ok':
            failed += 1
        line = f"{item.name:52} {entry.get('frames', '?'):>4} frames  "
        if entry['status'] == 'ok':
            gts = f" gpu_ts {entry['gpu_ts_ms']:.2f}" if entry.get('gpu_ts_ms') is not None else ''
            line += f"median {entry['median_ms']:7.2f} ms  p99 {entry['p99_ms']:7.2f}  (cpu {entry['cpu_median_ms']:.2f} gpu tail {entry['gpu_median_ms']:.2f}{gts})  {entry['wall_s']:.0f}s"
        else:
            line += entry['status']
        print(line, flush=True)
    shutil.rmtree(cache, ignore_errors=True)
    meta['gpu_after'] = gpu_state()
    meta['wall_s'] = round(time.time() - began, 1)
    (out / 'run.json').write_text(json.dumps(meta, indent=1))
    print(f'farm run {out}: {len(items)} captures, {failed} failed, {meta["wall_s"]:.0f} s')
    return 1 if failed else 0


def noise_of(rundir, runs, shots):
    """Same build twice: the picture noise (run0 vs run1 of one arm) per shot, in the same three numbers the compare uses."""
    if runs < 2:
        return None
    res = {}
    for f in shots:
        a, b = rundir / 'run0' / 'shots' / f'frame_{f:04d}.png', rundir / 'run1' / 'shots' / f'frame_{f:04d}.png'
        if a.exists() and b.exists():
            res[str(f)] = picture_numbers(a, b)
    return res


# ---------------------------------------------------------------- pictures
def picture_numbers(a_path, b_path):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(a_path).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(b_path).convert('RGB'), dtype=np.int16)
    if a.shape != b.shape:
        return {'size_differs': True, 'p999': 255.0, 'block': 255.0, 'share': 1.0}
    d = np.abs(a - b).max(axis=2)
    h, w = (d.shape[0] // 16) * 16, (d.shape[1] // 16) * 16
    block = d[:h, :w].reshape(h // 16, 16, w // 16, 16).mean(axis=(1, 3)).max() if h and w else d.mean()
    return {'p999': float(np.percentile(d, 99.9)), 'block': float(block), 'share': float((d > TOL_SMALL).mean())}


def picture_verdict(nums, noise):
    if nums.get('size_differs'):
        return False, 'size differs'
    noise_share = noise['share'] if noise else 0.0
    bad = []
    if nums['p999'] > TOL_P999:
        bad.append(f"p99.9 {nums['p999']:.1f} > {TOL_P999:g}")
    if nums['block'] > TOL_BLOCK:
        bad.append(f"block mean {nums['block']:.2f} > {TOL_BLOCK:g}")
    if nums['share'] > noise_share + TOL_SHARE:
        bad.append(f"{100 * nums['share']:.2f}% of pixels over {TOL_SMALL}")
    return (not bad), '; '.join(bad)


# ---------------------------------------------------------------- baseline and compare
def latest_run():
    runs = sorted((FARM / 'runs').glob('*/run.json')) if (FARM / 'runs').is_dir() else []
    return runs[-1].parent if runs else None


def cmd_baseline(args):
    src = Path(args.src) if args.src else latest_run()
    if not src or not (src / 'run.json').exists():
        print('FAIL no farm run to store (run one first)')
        return 1
    meta = json.loads((src / 'run.json').read_text())
    bad = [n for n, e in meta['captures'].items() if e['status'] != 'ok']
    if bad:
        print(f'FAIL the run has failed captures, not a baseline: {", ".join(bad)}')
        return 1
    if meta.get('arm'):
        print(f'FAIL the run was a promotion arm {meta["arm"]}: an A/B, not a baseline')
        return 1
    if meta.get('extra_rplay_args'):
        print(f'FAIL the run used test-only rplay args {meta["extra_rplay_args"]}: not a baseline')
        return 1
    exe = src / 'rplay_used.exe'  # the copy the run measured; never whatever rplay.exe is now
    if not exe.exists():
        print(f'FAIL {exe} missing: the run predates the exe copy, or it was cleaned; run the farm again')
        return 1
    dest = FARM / 'baseline'
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns('rplay.log', 'rplay_used.exe', '_cache'))
    shutil.copy2(exe, dest / 'rplay_ref.exe')  # a later check replays it in turn with the new build (paired), so drift cancels
    (dest / 'KEEP').write_text('The replay farm baseline (tools\\farm\\farm.py baseline). Kept by hand: replace it with a new baseline, do not delete.\n')
    print(f'baseline stored: {dest} (from {src.name}, {len(meta["captures"])} captures, rplay {meta["rplay_mtime"]})')
    return 0


def judge_timing(e, o, args):
    """(verdict 'ok' | 'SLOWER', notes, text) for one capture: total median / p99, and the GPU's own time separately."""
    verdict, notes = 'ok', []
    rm, rp = e['median_ms'] / o['median_ms'], e['p99_ms'] / o['p99_ms']
    text = f'median {o["median_ms"]:.2f} -> {e["median_ms"]:.2f} ms ({(rm - 1) * 100:+.1f}%), p99 {o["p99_ms"]:.2f} -> {e["p99_ms"]:.2f} ({(rp - 1) * 100:+.1f}%)'
    g_new, g_old = e.get('gpu_ts_ms'), o.get('gpu_ts_ms')
    kind = 'gpu' if g_new is not None and g_old is not None else 'gpu tail'
    if kind == 'gpu tail':
        g_new, g_old = e['gpu_median_ms'], o['gpu_median_ms']  # no timestamps in one build: the tail (execution + wake latency) stands in
    if g_old:
        text += f', {kind} {g_old:.2f} -> {g_new:.2f} ms ({(g_new / g_old - 1) * 100:+.1f}%)'
    if o.get('first_ms') and e.get('first_ms') and e['first_ms'] > 1.5 * o['first_ms']:
        notes.append(f'first frame (pipelines) {o["first_ms"]:.0f} -> {e["first_ms"]:.0f} ms')
    if e['frames'] < MIN_FRAMES_TIMING:
        notes.append(f'{e["frames"]} frames: timing not judged')
        return verdict, notes, text
    if (rm > 1 + args.thr_median and e['median_ms'] - o['median_ms'] >= MEDIAN_FLOOR_MS) or (
            e['frames'] >= MIN_FRAMES_P99 and rp > 1 + args.thr_p99 and e['p99_ms'] - o['p99_ms'] >= P99_FLOOR_MS):
        verdict = 'SLOWER'
    if g_old and g_new > g_old * (1 + args.thr_gpu) and g_new - g_old >= GPU_FLOOR_MS:
        verdict = 'SLOWER'
        notes.append(f'GPU time up {g_new - g_old:.2f} ms')
    if rm < 1 - args.thr_median:
        notes.append('faster')
    return verdict, notes, text


def cmd_compare(args):
    run = Path(args.run) if args.run else latest_run()
    base = Path(args.base) if args.base else FARM / 'baseline'
    if not run or not (run / 'run.json').exists():
        print(f'FAIL cannot read {run}')
        return 1
    new = json.loads((run / 'run.json').read_text())
    paired_run = bool(new.get('ref'))  # a paired run carries its reference numbers: no baseline folder needed
    if not paired_run and not (base / 'run.json').exists():
        print(f'FAIL cannot read {base}')
        return 1
    old = json.loads((base / 'run.json').read_text()) if (base / 'run.json').exists() else {'stamp': '-', 'rplay_mtime': '-', 'captures': {}}
    print(f'compare {run.name} (rplay {new["rplay_mtime"]}, git {new.get("git", "?")}) against ' + (f'its paired reference {new["ref"]}' if paired_run else f'baseline {old["stamp"]} (rplay {old["rplay_mtime"]}), UNPAIRED: speed is not judged reliably'))
    gnew, gold = new.get('gpu_before'), old.get('gpu_before')
    if not paired_run and gnew and gold and abs(gnew['clock_mhz'] - gold['clock_mhz']) > 0.15 * max(gold['clock_mhz'], 1):
        print(f'  NOTE GPU clock at the start differs: {gold["clock_mhz"]} MHz then, {gnew["clock_mhz"]} now (LESSONS: clock state moves GPU ms)')
    if gnew and gnew['util_pct'] > 30:
        print(f'  NOTE GPU was {gnew["util_pct"]}% busy before the run (other lanes share it): timing is noisy')
    problems, n_new, n_first, n_nopic = 0, 0, 0, 0
    arm = new.get('arm')
    if arm:
        print(f'  ARM {" ".join(arm)}: pictures are expected to differ from the reference and are not judged; the timing is the promotion cost (`promo` gives the GPU numbers)')
    for name, e in sorted(new['captures'].items()):
        o = e.get('ref') or old['captures'].get(name)  # a paired run carries its own reference numbers
        paired = bool(e.get('ref'))
        if e['status'] != 'ok':
            print(f'  FAIL     {name}: {e["status"]}')
            problems += 1
            continue
        if o is None:
            print(f'  NEW      {name}: not in the baseline ({e["frames"]} frames, median {e["median_ms"]:.2f} ms)')
            n_new += 1
            continue
        verdict, notes, timing = judge_timing(e, o, args)
        if arm:
            print(f'  ARM      {name}: {timing}')
            continue
        n_first += sum(1 for x in notes if x.startswith('first frame'))
        noise_src = (o.get('noise') or {})  # the reference's own picture noise (paired: its arm; unpaired: the baseline's stored one)
        differ, nopic = [], []
        for f in e.get('shot_frames', []):
            a = (run / name / 'ref' if paired else base / name) / 'run0' / 'shots' / f'frame_{f:04d}.png'
            b = run / name / 'run0' / 'shots' / f'frame_{f:04d}.png'
            if not a.exists() and not b.exists():
                nopic.append(str(f))  # neither build presented a picture there (a part frame, say): nothing to compare, not a difference
                continue
            if not a.exists() or not b.exists():
                differ.append(f'{f} (picture missing in the {"reference" if not a.exists() else "new build"})')
                continue
            ok, why = picture_verdict(picture_numbers(a, b), noise_src.get(str(f)))
            if not ok:
                differ.append(f'{f} ({why})')
        if nopic:
            n_nopic += 1
            notes.append('no picture in either build at frames ' + ', '.join(nopic))
        if differ:
            verdict = 'DIFFERS' if verdict == 'ok' else verdict + '+DIFFERS'
            notes.append('pictures differ on frames ' + ', '.join(differ))
        if e.get('clock_note'):  # S101 lane 47: runs under 90 % of the best run's graphics clock (after re-runs): read the GPU ms with that in mind
            notes.append('low graphics clock: ' + ', '.join(e['clock_note']))
        if verdict != 'ok':
            problems += 1
        print(f'  {verdict:8} {name}: {timing}' + (' [' + '; '.join(notes) + ']' if notes else ''))
    missing = [] if paired_run else sorted(set(old["captures"]) - set(new["captures"]))
    for name in missing:
        print(f'  MISSING  {name}: in the baseline, not in this run')
    print(f'compare: {problems} problem(s) over {len(new["captures"])} captures; notes: {n_new} new, {len(missing)} missing, {n_first} with a slower first frame (pipelines), '
          f'{n_nopic} with a shot neither build presented (thresholds: median +{100 * args.thr_median:.0f}%, p99 +{100 * args.thr_p99:.0f}%, gpu +{100 * args.thr_gpu:.0f}% and {GPU_FLOOR_MS} ms, pictures: owned_check tolerance)')
    return 1 if problems else 0


CENSUS_CROWD = re.compile(r'\[rdr\]\[crowd\] ours frames (\d+), fallback (\d+)')
CENSUS_HOOK = re.compile(r'\[rdr\]\[(arena|wrestlers)\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+)')


def census_of(log_path):
    """The promotion census lines of an rplay log: what the arm really did (ours_frames 0 = it drew nothing)."""
    text = Path(log_path).read_text(errors='replace') if Path(log_path).exists() else ''
    res = {}
    m = CENSUS_CROWD.search(text)
    if m:
        res['crowd'] = {'ours_frames': int(m.group(1)), 'fallback_frames': int(m.group(2))}
    for m in CENSUS_HOOK.finditer(text):
        res[m.group(1)] = {'ours_frames': int(m.group(2)), 'fallback_frames': int(m.group(3)), 'replaced': int(m.group(4)), 'kept': int(m.group(5))}
    return res


def dead_classes(tag, cen):
    """The classes of arm `tag` whose census says ours drew no frame (or printed no census): the arm measured the stream, not the promotion."""
    keys = [k2 for k2, sw in (('crowd', '--crowd-ours'), ('arena', '--arena-ours'), ('wrestlers', '--wrestlers-ours')) if sw in PROMO_ARMS[tag]]
    return [k2 for k2 in keys if not cen.get(k2) or cen[k2]['ours_frames'] == 0]


def cmd_promo(args):
    with FarmLock():
        return promo_locked(args)


def promo_locked(args):
    """Paired A/B of the promotions (S101 lane 47): per capture, the OFF arm and each chosen ON arm replayed in turn (the order rotates per run), GPU timestamps on, warm-up replays first.
    Per arm: the GPU time per frame (median / p99 / mean over the frames of the per-frame median over the runs) and its difference from OFF. An arm whose census shows ours drew
    nothing is a FAIL (it measured the stream)."""
    exe = Path(args.build) if args.build else DEFAULT_RPLAY
    if not exe.exists():
        print(f'FAIL no rplay at {exe} (build: pwsh -NoProfile -File tools\\renderer\\build.ps1)')
        return 1
    game = Path(args.game) if args.game else MAIN / 'game_data' / 'extracted'
    if not game.is_dir():
        print(f'FAIL no game files at {game} (--game DIR)')
        return 1
    names = [x for x in args.arms.split(',') if x]
    bad = [x for x in names if x not in PROMO_ARMS]
    if bad or not names:
        print(f'FAIL unknown arm(s) {bad or names}: choose from {", ".join(PROMO_ARMS)}')
        return 1
    items = select(corpus(), args.only)
    if not items:
        print('FAIL no captures selected (py -I tools\\farm\\farm.py list)')
        return 1
    host = HOSTS[args.host]()
    stamp = time.strftime('%Y%m%d_%H%M%S')
    out = Path(args.out) if args.out else FARM / 'promo' / stamp
    out.mkdir(parents=True, exist_ok=True)
    cache = out / '_cache'
    shutil.copy2(exe, out / 'rplay_used.exe')
    arms = [('off', [])] + [(n, PROMO_ARMS[n] + ['--game', str(game)]) for n in names]
    meta = {'stamp': stamp, 'kind': 'promo', 'rplay': str(exe), 'rplay_mtime': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(exe.stat().st_mtime)), 'host': host.name, 'runs_each': args.runs,
            'arms': {n: a for n, a in arms}, 'gpu_before': gpu_state(), 'captures': {}}
    try:
        meta['git'] = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip()
    except Exception:
        pass
    failed = 0
    for item in items:
        t0 = time.time()
        entry = {'kind': item.kind, 'status': 'ok'}
        meta['captures'][item.name] = entry
        folder, temp = None, False
        try:
            cdir = out / item.name
            if cdir.exists():
                shutil.rmtree(cdir)
            cdir.mkdir(parents=True)
            folder, temp = open_capture(item, cache)
            n = frame_count(folder)
            if args.frames:
                n = min(n, args.frames)
            entry['frames'] = n
            entry['gpu_before'] = gpu_state()
            for tag, a in arms:  # unmeasured warm-ups per arm: clock state, driver and file caches (an arm also reads the game files)
                for w in range(args.warm):
                    host.run_rplay(exe, ['--capture', str(folder), '--validation', '0', '--frames', str(n)] + a, args.timeout, cdir / f'warm_{tag}.log')
            csvs = {tag: {} for tag, _ in arms}
            census = {tag: {} for tag, _ in arms}   # tag -> run -> that run's census (D3: the dead-arm guard judges every run)
            clocks = {}                              # (tag, run) -> median graphics clock MHz during that replay
            arm_args = dict(arms)

            def one_run(tag, r):
                rdir = cdir / tag / f'run{r}'
                rdir.mkdir(parents=True, exist_ok=True)
                csv_path = rdir / 'timing.csv'
                if csv_path.exists():
                    csv_path.unlink()
                cmd = ['--capture', str(folder), '--validation', '0', '--frames', str(n), '--timing', str(csv_path), '--warm-frames', str(args.warm_frames)] + arm_args[tag]
                code, mhz = measured(host, exe, cmd, args.timeout, rdir / 'rplay.log')
                if code != 0 or not csv_path.exists():
                    entry['status'] = f'FAIL rplay {code} ({tag} run {r}, {rdir / "rplay.log"})'
                    return None
                csvs[tag][r] = csv_path
                clocks[(tag, r)] = mhz
                if tag != 'off':
                    census[tag][r] = census_of(rdir / 'rplay.log')
                return mhz
            for r in range(args.runs):
                k = r % len(arms)
                order = arms[k:] + arms[:k]  # the arm that goes first rotates: "first in line" favours none
                if r % 2:
                    order = order[::-1]
                for tag, a in order:
                    one_run(tag, r)
                    if entry['status'] != 'ok':
                        break
                if entry['status'] != 'ok':
                    break
            if entry['status'] == 'ok':
                still, systematic = settle_clocks(clocks, lambda key: one_run(*key) if entry['status'] == 'ok' else None)
                entry['clocks_mhz'] = {f'{t}/run{r}': v for (t, r), v in sorted(clocks.items())}
                entry['clock_not_comparable'] = systematic   # arms that ran at another clock on every run: raw numbers printed, no verdict
                if still:
                    entry['status'] = f'FAIL mixed clocks: runs {still} stayed under {int(CLOCK_LOW * 100)}% of the best run\'s graphics clock after {CLOCK_REDO} re-run rounds ({entry["clocks_mhz"]})'
            entry['gpu_after'] = gpu_state()
            if entry['status'] == 'ok':
                entry['arms'] = {tag: summarise([csvs[tag][r] for r in sorted(csvs[tag])]) for tag, _ in arms}
                entry['census_runs'] = {t: {str(r): c for r, c in runs.items()} for t, runs in census.items() if runs}
                entry['census'] = {t: runs[max(runs)] for t, runs in census.items() if runs}   # (the last run's, for the printout)
                for tag, _ in arms:
                    if tag == 'off':
                        continue
                    for r in sorted(census[tag]):
                        dead = dead_classes(tag, census[tag][r])
                        if dead:
                            entry['status'] = f'FAIL arm {tag} run {r}: ours drew nothing for {", ".join(dead)} (census {census[tag][r]}): it measured the stream'
                            break
                    if entry['status'] != 'ok':
                        break
        except Exception as e:
            entry['status'] = f'FAIL {type(e).__name__}: {e}'
        finally:
            if temp and folder and not args.keep_cache:
                shutil.rmtree(folder, ignore_errors=True)
        entry['wall_s'] = round(time.time() - t0, 1)
        if entry['status'] != 'ok':
            failed += 1
            print(f"{item.name:52} {entry['status']}", flush=True)
            continue
        off = entry['arms']['off']
        print(f"{item.name:52} {entry['frames']:>4} frames  draws/frame {off['draws_per_frame']:.0f}", flush=True)
        for tag, _ in arms:
            cl = [entry['clocks_mhz'].get(f'{tag}/run{r}') for r in range(args.runs)]
            print(f"   {tag:10} graphics clock per run (median MHz): {', '.join(str(int(c)) if c else '?' for c in cl)}", flush=True)
        for tag, _ in arms:
            sm = entry['arms'][tag]
            if sm.get('gpu_ts_ms') is None:
                print(f'   {tag:10} no GPU timestamps on this queue')
                continue
            d = sm['gpu_ts_ms'] - off['gpu_ts_ms']
            d99 = sm['gpu_ts_p99_ms'] - off['gpu_ts_p99_ms']
            if tag in entry.get('clock_not_comparable', []):
                print(f"   {tag:10} gpu median {sm['gpu_ts_ms']:7.3f} ms  p99 {sm['gpu_ts_p99_ms']:7.3f}  NOT COMPARABLE: this arm ran at another graphics clock on every run (see above); its GPU ms are the clock's, not the promotion's", flush=True)
                continue
            c = ', '.join(f"{k2} ours {v['ours_frames']} fb {v['fallback_frames']}" + (f" repl {v['replaced']} kept {v['kept']}" if 'replaced' in v else '') for k2, v in entry['census'].get(tag, {}).items())
            print(f"   {tag:10} gpu median {sm['gpu_ts_ms']:7.3f} ms  p99 {sm['gpu_ts_p99_ms']:7.3f}  mean {sm['gpu_ts_mean_ms']:7.3f}   vs off {d:+7.3f} ms ({(sm['gpu_ts_ms'] / off['gpu_ts_ms'] - 1) * 100:+6.1f}%), p99 {d99:+7.3f} ms   [{c}]", flush=True)
    shutil.rmtree(cache, ignore_errors=True)
    meta['gpu_after'] = gpu_state()
    (out / 'promo.json').write_text(json.dumps(meta, indent=1))
    print(f'farm promo {out}: {len(items)} captures, {failed} failed')
    return 1 if failed else 0


def cmd_list(args):
    for it in select(corpus(), args.only):
        print(f'{it.name:56} {it.kind:4} {it.path}')
    return 0


def cmd_check(args):
    if not args.ref:
        args.ref = str(FARM / 'baseline' / 'rplay_ref.exe')
    rc = cmd_run(args)
    if rc:
        return rc
    return cmd_compare(argparse.Namespace(run=args.out_resolved, base=None, thr_median=args.thr_median, thr_p99=args.thr_p99, thr_gpu=args.thr_gpu))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)

    def common(p):
        p.add_argument('--build')
        p.add_argument('--ref', help='reference rplay.exe, replayed in turn with --build on every capture (paired: the machine drifts, both arms drift together)')
        p.add_argument('--only', default='')
        p.add_argument('--runs', type=int, default=3)
        p.add_argument('--warm', type=int, default=2, help='unmeasured replays before the timed ones')
        p.add_argument('--timeout', type=int, default=900)
        p.add_argument('--host', default='local', choices=sorted(HOSTS))
        p.add_argument('--out')
        p.add_argument('--rplay-args', default='', help='extra rplay args for the NEW arm: planted faults (test-only), or the promotion switches "--crowd-ours --arena-ours --wrestlers-ours --game DIR" (an arm, S101 lane 47). Never a baseline')
        p.add_argument('--keep-cache', action='store_true')
        p.add_argument('--thr-median', type=float, default=THR_MEDIAN)
        p.add_argument('--thr-p99', type=float, default=THR_P99)
        p.add_argument('--thr-gpu', type=float, default=THR_GPU)
    common(sub.add_parser('run'))
    common(sub.add_parser('check'))
    b = sub.add_parser('baseline')
    b.add_argument('--from', dest='src')
    c = sub.add_parser('compare')
    c.add_argument('--run')
    c.add_argument('--base')
    c.add_argument('--thr-median', type=float, default=THR_MEDIAN)
    c.add_argument('--thr-p99', type=float, default=THR_P99)
    c.add_argument('--thr-gpu', type=float, default=THR_GPU)
    pr = sub.add_parser('promo')
    pr.add_argument('--build')
    pr.add_argument('--only', default='')
    pr.add_argument('--arms', default='crowd,arena,wrestlers', help='crowd,arena,wrestlers,all (each is one ON arm against the OFF arm)')
    pr.add_argument('--game', default='', help="the game files (default: the main checkout's game_data\\extracted)")
    pr.add_argument('--runs', type=int, default=5)
    pr.add_argument('--warm', type=int, default=2)
    pr.add_argument('--warm-frames', type=int, default=60, help='untimed frames each replay renders in-process before timing (loads the arena / wrestlers, builds pipelines, lifts the GPU clock)')
    pr.add_argument('--frames', type=int, default=0, help='replay only the first N frames of each capture (0 = all)')
    pr.add_argument('--timeout', type=int, default=900)
    pr.add_argument('--host', default='local', choices=sorted(HOSTS))
    pr.add_argument('--out')
    pr.add_argument('--keep-cache', action='store_true')
    l = sub.add_parser('list')
    l.add_argument('--only', default='')
    args = ap.parse_args()
    return {'run': cmd_run, 'baseline': cmd_baseline, 'compare': cmd_compare, 'list': cmd_list, 'check': cmd_check, 'promo': cmd_promo}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
