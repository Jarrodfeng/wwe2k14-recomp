"""Upstream-style benchmark report for one SDK setting (Session 8, task 18).

    py tools/bench/upstream_report.py [--only NAME] [--logs DIR] [--out DIR] [--stdout]

Turns our own run outputs (game log, timeline, frames CSV in out/build/win-amd64-release/logs) into Markdown tables
in docs/upstream/<setting>.md: correctness (verify-mode counts from the log), the in-run A/B (bench_report.py's
paired-segment analysis, with ab_paired.py's independent CP-busy numbers as a cross-check) and end-to-end runs with
the setting on (fps60.py). Every number is recomputed from the raw run outputs when they exist; the Provenance
section compares each recomputed number with the value the session notes recorded, and says which numbers come from
the notes only.

Numbers only: no game imagery, no game code, no game strings, no file paths of the user's machine. Neutral wording
so a table can be quoted in an upstream ReXGlue issue later; whether anything is sent upstream is Jarrod's call.
Reads files only (no game run, no run lock); runs below normal priority so a game run being measured is not
disturbed. About 2 s per A/B run and 1-2 s per other run.

Studies are defined in STUDIES below (runs, notes values). Add one by copying an entry; write notes values with
decimals as strings exactly as the notes print them ('-3.40'), so the check runs at the notes' precision.

Exit code: 0 = every notes value recomputed and the same, every table row has numbers; 1 = some notes value differs
or could not be checked, or a row could not be recomputed (the docs are still written, and say so in Provenance);
2 = no such study. With --logs other than fps60.py's fixed folder the end-to-end rows say "not recomputed".
"""
import argparse
import datetime
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import bench_report as br  # noqa: E402
import ab_paired  # noqa: E402

DEFAULT_LOGS = br.DEFAULT_LOGS
DEFAULT_OUT = os.path.join(REPO, 'docs', 'upstream')

TEST_SYSTEM = [
    ('CPU', 'AMD Ryzen 9 5950X (16 cores)'),
    ('GPU', 'NVIDIA GeForce RTX 4090, driver 591.74 (nvidia-smi, Session 7b); no Resizable BAR'),
    ('RAM', '32 GB'),
    ('OS', 'Windows 11 IoT Enterprise LTSC 2024'),
    ('Backend', 'ReXGlue SDK Vulkan backend, render_target_path_vulkan = fsi'),
    ('Title', 'a statically recompiled Xbox 360 title (3D sports game: two entrance cutscenes then 1v1 gameplay)'),
]

# Neutral scene names (our scene keys come from file-open anchors in the log; see tools/bench/README.md).
SCENE_LABEL = {
    'tron_343': 'entrance 1',
    'tron_324': 'entrance 2 (route B)',
    'tron_103': 'entrance 2 (route A)',
    'match': 'gameplay (1v1 match)',
    'tron_324_pan': 'entrance 2, heaviest shot (route B, +380..+500)',
    'tron_103_heavy': 'entrance 2, heaviest stretch (route A, +0..+700)',
}
FPS60_LABEL = {'Brock': 'entrance 1', 'second entrance': 'entrance 2', 'match': 'gameplay'}
AB_PAIRED_LABEL = {'tron_343': 'entrance 1', 'tron_103 heavy': 'entrance 2 heaviest (+0..+700)',
                   'tron_103 rest': 'entrance 2 rest', 'tron_324 early': 'entrance 2 (+0..+380)',
                   'tron_324 pan': 'entrance 2 heaviest (+380..+500)', 'tron_324 rest': 'entrance 2 rest',
                   'match': 'gameplay'}
UNLABELLED = 'unlabelled scene'   # never print a raw scene key (they come from game file names) into the docs
EXTRA_WINDOWS = ['tron_103_heavy=tron_103:0:700']
RES = {1: '1280x720', 2: '2560x1440', 3: '3840x2160'}

STUDIES = [
    dict(
        name='vulkan_texture_load_direct',
        title='Resolution-scaled texture loads written directly into the image',
        patch='patches/rexglue-sdk/0005-session7c-attribution-texture-loads.patch (local patch, not upstream)',
        what=[
            'Resolution-scaled textures (render targets resolved at the internal scale and then sampled) are '
            'reloaded often. The existing path runs the untile/convert compute shader into a shared scratch buffer, '
            'then records vkCmdCopyBufferToImage and a barrier for every load, whole level each time.',
            'With the setting on, single-level 32bpb / 64bpb scaled loads are written by the compute shader straight '
            'into the VkImage through a UINT storage view (textures created while the setting is on get the '
            'STORAGE usage). Other loads (mip chains, array layers, formats the direct shader does not cover) stay on '
            'the old path.',
            'Verify mode (vulkan_texture_load_direct_verify): for up to 4 direct loads per frame the old shader also '
            'runs into a readback buffer and the directly written image is read back; both are compared byte for '
            'byte after the submission completes. A positive control (deliberately wrong reference) exists to show '
            'the comparison can fail.',
            'Why it was measured: GPU timestamps in a 4K frame attributed about 92% of the GPU time spent before '
            'draws to these loads (about 74 loads and 400 MB per frame) (Session 7c attribution capture, run 164; '
            'taken from the notes).',
        ],
        from_notes=['the attribution figure in "What the setting changes" (92% of pre-draw GPU time, 74 loads, '
                    '~400 MB per frame: anatomy capture of run 164, whose per-row CSV was not re-analysed here)'],
        verify_kind='texdirect',
        verify=[166, 172],
        ab=[(167, 'vulkan_texture_load_direct')],
        hold=[168, 169, 170, 171],
        context=[(168, 151), (169, 149)],
        notes={
            ('verify', 166, 'run_direct'): 427872, ('verify', 166, 'run_compared'): 31400,
            ('verify', 166, 'run_mismatches'): 0,
            ('verify', 172, 'run_direct'): 431971, ('verify', 172, 'run_compared'): 45626,
            ('verify', 172, 'run_mismatches'): 0,
            ('ab', 167, 'gpu_ms', 'mean'): '-3.40', ('ab', 167, 'gpu_ms', 'ci_lo'): '-3.75',
            ('ab', 167, 'gpu_ms', 'ci_hi'): '-2.95', ('ab', 167, 'gpu_ms', 'neg'): 60, ('ab', 167, 'pairs'): 62,
            ('ab', 167, 'arm', 'A', 'gpu_ms'): '14.54', ('ab', 167, 'arm', 'B', 'gpu_ms'): '11.18',
            ('ab', 167, 'busy', 'mean'): '0.10',
            ('hold', 168, 'second entrance', 'fps'): '60.00', ('hold', 168, 'second entrance', 'gpu'): '13.30',
            ('hold', 168, 'second entrance', 'over20'): 7,
            ('hold', 169, 'second entrance', 'fps'): '59.97', ('hold', 169, 'second entrance', 'gpu'): '11.27',
            ('hold', 169, 'second entrance', 'over20'): 6,
            ('hold', 170, 'second entrance', 'fps'): '59.98', ('hold', 170, 'second entrance', 'gpu'): '11.42',
            ('hold', 171, 'second entrance', 'fps'): '60.00', ('hold', 171, 'second entrance', 'gpu'): '12.70',
            ('hold', 151, 'second entrance', 'fps'): '57.99', ('hold', 151, 'second entrance', 'gpu'): '16.9',
            ('hold', 149, 'second entrance', 'fps'): '59.88', ('hold', 149, 'second entrance', 'gpu'): '14.55',
        },
    ),
    dict(
        name='gpu_upload_skip_unchanged',
        title='Skip shared-memory re-uploads of pages whose bytes did not change',
        patch='patches/rexglue-sdk/0004-session7b-upload-skip.patch (local patch, not upstream)',
        what=[
            'With clear_memory_page_state = true (the SDK invalidates every page of the shared-memory buffer at '
            'the end of each frame), this title re-uploads about 650-1,100 page ranges per frame. Each upload ends '
            'the render pass, records a whole-buffer barrier from every read stage (fragment included) to TRANSFER, '
            'the copy, and a barrier back; the render pass then restarts.',
            'With the setting on, the guest bytes of a page about to be uploaded are hashed (XXH3-64) and compared '
            'with the hash of the bytes last uploaded to that page; when they match and nothing else has written that '
            'page of the GPU buffer since, the upload is skipped and the page is made valid and watched as by an '
            'upload. Rendering input is the same by construction.',
            'Verify mode (gpu_upload_skip_unchanged_verify): every page is uploaded as with the setting off, and for '
            'up to 256 would-be-skipped pages per frame (round robin) the page is first copied out of the GPU buffer '
            'at that point of the command stream and compared with the hashed guest bytes after the submission '
            'completes. The same log line also counts pages that changed while only the frame-end clear had '
            'invalidated them (what clear_memory_page_state = false would have rendered stale).',
        ],
        from_notes=['the "about 650-1,100 page ranges per frame" range in "What the setting changes" (per-scene '
                    'upload counts of Session 7b; the verify table here gives whole-run averages instead, and the '
                    'notes\' "~87% unchanged" is the last 300-frame window, which the cross-check below confirms '
                    'from the matched / uploaded counts)'],
        verify_kind='upskip',
        verify=[152, 159],
        ab=[(158, 'gpu_upload_skip_unchanged')],
        hold=[153, 154, 155, 156, 160],
        context=[(153, 139), (154, 138)],
        notes={
            ('verify', 152, 'mismatches_total'): 0, ('verify', 152, 'stale_total'): 0,
            ('verify', 152, 'last_recorded'): 65776, ('verify', 152, 'last_compared'): 65864,
            ('verify', 152, 'last_matched'): 328967, ('verify', 152, 'last_uploaded'): 380050,
            ('verify', 159, 'mismatches_total'): 0, ('verify', 159, 'stale_total'): 0,
            ('verify', 159, 'last_recorded'): 67633, ('verify', 159, 'last_compared'): 67889,
            ('ab', 158, 'gpu_ms', 'mean'): '-4.60', ('ab', 158, 'gpu_ms', 'ci_lo'): '-5.16',
            ('ab', 158, 'gpu_ms', 'ci_hi'): '-4.10', ('ab', 158, 'gpu_ms', 'neg'): 50, ('ab', 158, 'pairs'): 50,
            ('ab', 158, 'arm', 'A', 'gpu_ms'): '12.48', ('ab', 158, 'arm', 'B', 'gpu_ms'): '8.10',
            ('paired', 158, 'tron_343'): '0.13', ('paired', 158, 'tron_103 heavy'): '0.09',
            ('paired', 158, 'tron_103 rest'): '-0.10', ('paired', 158, 'match'): '0.10',
            ('paired_w', 158, 'tron_343'): '1.32', ('paired_w', 158, 'tron_103 heavy'): '2.37',
            ('paired_w', 158, 'tron_103 rest'): '0.66', ('paired_w', 158, 'match'): '0.17',
            ('hold', 153, 'second entrance', 'fps'): '59.98', ('hold', 153, 'second entrance', 'gpu'): '8.64',
            ('hold', 153, 'second entrance', 'over20'): 6,
            ('hold', 154, 'second entrance', 'fps'): '60.00', ('hold', 154, 'second entrance', 'gpu'): '10.08',
            ('hold', 155, 'second entrance', 'fps'): '59.97', ('hold', 155, 'second entrance', 'gpu'): '8.56',
            ('hold', 156, 'second entrance', 'fps'): '60.00', ('hold', 156, 'second entrance', 'gpu'): '10.15',
            ('hold', 160, 'second entrance', 'fps'): '30.08', ('hold', 160, 'second entrance', 'gpu'): '10.5',
            ('hold', 139, 'second entrance', 'fps'): '59.06', ('hold', 138, 'second entrance', 'fps'): '58.5',
        },
    ),
]

AB_RUNS = {int(n) for st in STUDIES for n, _ in st['ab']}


def fps60_logs():
    """The folder fps60.py reads (fixed in that script as L = r'...'); DEFAULT_LOGS if it cannot be read."""
    try:
        with open(os.path.join(HERE, 'fps60.py'), encoding='utf-8') as f:
            m = re.search(r"^L = r?'([^']+)'", f.read(), re.M)
        if m:
            return m.group(1)
    except OSError:
        pass
    return DEFAULT_LOGS


def same_dir(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))

# --------------------------------------------------------------------------------------------- helpers


def lower_priority():
    if sys.platform == 'win32':
        try:
            import ctypes
            k32 = ctypes.WinDLL('kernel32', use_last_error=True)
            k32.GetCurrentProcess.restype = ctypes.c_void_p
            k32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            k32.SetPriorityClass(k32.GetCurrentProcess(), 0x00004000)  # BELOW_NORMAL (children inherit it)
        except (OSError, AttributeError):
            pass


def nnn(n):
    return '%03d' % int(n)


def notes_key_text(k):
    """A notes key in neutral words (scene keys translated through the label maps, never printed raw)."""
    kind, n, rest = k[0], k[1], list(k[2:])
    if kind in ('paired', 'paired_w'):
        what = 'ab_paired CP busy %s, %s' % ('B - A' if kind == 'paired' else 'CI half-width',
                                             AB_PAIRED_LABEL.get(rest[0], UNLABELLED))
    elif kind == 'hold':
        what = '%s %s' % (FPS60_LABEL.get(rest[0], UNLABELLED), ' '.join(str(x) for x in rest[1:]))
    else:
        what = '%s %s' % (kind, ' '.join(str(x) for x in rest))
    return 'run %s %s' % (nnn(n), what.strip())


def f2(x, nd=2, sign=False):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return 'n/a'
    return ('%+.*f' if sign else '%.*f') % (nd, x)


def thousands(x):
    return 'n/a' if x is None else '{:,}'.format(int(x))


def log_exists(logs, n):
    return os.path.exists(os.path.join(logs, 'wwe2k14_%s.log' % nnn(n)))


TRUE = ('true', '1')


def run_config(logs, n):
    """Configuration of a run from its log's "[wwe2k14] modified:" lines (bench_report.load_log)."""
    log = br.load_log(logs, nnn(n))
    s = log['settings']
    scale = int(s.get('resolution_scale', '1') or 1)
    script = os.path.basename((s.get('wwe2k14_input_script') or log.get('script') or '').replace('\\', '/'))
    low = script.lower()
    route = 'B' if 'bryan' in low else ('A' if 'taker' in low else '?')
    # Our script file names carry content names; the report only says which route and what kind of script:
    # bench_* = the plain replay, a run listed as an A/B run of a study = the A/B toggle variant (made by
    # make_ab.py), anything else = 'other script' (never guessed from the name).
    if low.startswith('bench_'):
        kind = 'replay'
    elif int(n) in AB_RUNS:
        kind = 'A/B toggle variant'
    else:
        kind = 'other script'
    label = 'route %s %s' % (route, kind)
    return dict(n=nnn(n), settings=s, scale=scale, res=RES.get(scale, '%dx' % scale), route=route, script=label,
                msaa='off' if s.get('wwe2k14_force_no_msaa', '').lower() in TRUE else '4x (title default)',
                entr_fps=60 if s.get('wwe2k14_entrance_60fps', '').lower() in TRUE else 30,
                hard={k: len(v) for k, v in log['hard'].items() if v}, start=log['start_ts'])


def public_settings(cfg):
    """Settings worth listing (no paths of this machine, no log plumbing)."""
    out = []
    for k, v in cfg['settings'].items():
        if k in ('user_data_root', 'cache_root', 'log_max_file_size_mb') or k.endswith('_path') or ':' in v \
                or '\\' in v or '/' in v:
            if k == 'wwe2k14_input_script':
                out.append((k, cfg['script']))
            continue
        out.append((k, v))
    return out


# --------------------------------------------------------------------------------------------- verify parsers

TEXDIRECT_RE = re.compile(
    r'\[s7\]\[TEXDIRECT\] frames (\d+)-(\d+) \(vulkan_texture_load_direct (\w+), verify (\w+)\): (\d+) direct loads '
    r'\((\d+) 32bpb, (\d+) 64bpb, ([\d.]+) MB written\), (\d+) scaled loads on the buffer \+ copy path \(([^)]*)\);'
    r'.*?verify: recorded (\d+), compared (\d+), mismatches (\d+), dropped (\d+), compare time ([\d.]+) ms; '
    r'run: (\d+) direct loads, compared (\d+), mismatches (\d+)')
UPSKIP_RE = re.compile(
    r'\[s7\]\[UPSKIP\] frames (\d+)-(\d+) \(skip (\w+), stats (\w+), verify (\w+)\): (\d+) upload calls '
    r'\(([\d.]+)/frame\), (\d+) pages uploaded \(([\d.]+)/frame\), (\d+) pages skipped \(([\d.]+)/frame\) of (\d+) '
    r'not-valid pages with a hash checked, (\d+) matched but uploaded for verify; (\d+) uploaded pages changed while '
    r'only the frame-end clear had invalidated them \(stale with clear_memory_page_state=false; (\d+) in total\); '
    r'hashed (\d+) MB in ([\d.]+) ms \(([\d.]+) ms/frame\);.*?verify: (\d+) readbacks recorded, (\d+) dropped, '
    r'(\d+) compared, (\d+) mismatches \((\d+) in total\)')


def log_lines(logs, n):
    for part in br.log_parts(logs, nnn(n)):
        with open(part, encoding='utf-8', errors='replace') as f:
            for line in f:
                yield line


def parse_texdirect(logs, n):
    w = []
    for line in log_lines(logs, n):
        m = TEXDIRECT_RE.search(line)
        if m:
            g = m.groups()
            reasons = dict((k.strip(), int(v)) for v, k in
                           [(x.strip().rsplit(' ', 1)[1], x.strip().rsplit(' ', 1)[0]) for x in g[9].split(',')])
            w.append(dict(f0=int(g[0]), f1=int(g[1]), on=g[2], verify=g[3], direct=int(g[4]), d32=int(g[5]),
                          d64=int(g[6]), mb=float(g[7]), old=int(g[8]), reasons=reasons, recorded=int(g[10]),
                          compared=int(g[11]), mismatches=int(g[12]), dropped=int(g[13]), cmp_ms=float(g[14]),
                          run_direct=int(g[15]), run_compared=int(g[16]), run_mismatches=int(g[17])))
    if not w:
        return None
    last = w[-1]
    reasons = {}
    for x in w:
        for k, v in x['reasons'].items():
            reasons[k] = reasons.get(k, 0) + v
    frames = last['f1'] - w[0]['f0']
    return dict(windows=len(w), frames=frames, verify=last['verify'], run_direct=last['run_direct'],
                run_compared=last['run_compared'], run_mismatches=last['run_mismatches'],
                sum_direct=sum(x['direct'] for x in w), sum_old=sum(x['old'] for x in w),
                sum_recorded=sum(x['recorded'] for x in w), sum_compared=sum(x['compared'] for x in w),
                sum_mismatches=sum(x['mismatches'] for x in w), sum_dropped=sum(x['dropped'] for x in w),
                sum_mb=sum(x['mb'] for x in w), reasons=reasons, d32=sum(x['d32'] for x in w),
                d64=sum(x['d64'] for x in w), last=last)


def parse_upskip(logs, n):
    w = []
    for line in log_lines(logs, n):
        m = UPSKIP_RE.search(line)
        if m:
            g = m.groups()
            w.append(dict(f0=int(g[0]), f1=int(g[1]), skip=g[2], stats=g[3], verify=g[4], calls=int(g[5]),
                          calls_pf=float(g[6]), uploaded=int(g[7]), skipped=int(g[9]), hashed=int(g[11]),
                          matched=int(g[12]), stale=int(g[13]), stale_total=int(g[14]), hash_mb=int(g[15]),
                          hash_ms=float(g[16]), hash_ms_pf=float(g[17]), recorded=int(g[18]), dropped=int(g[19]),
                          compared=int(g[20]), mismatches=int(g[21]), mismatches_total=int(g[22])))
    if not w:
        return None
    last = w[-1]
    frames = last['f1'] - w[0]['f0']
    hashed = sum(x['hashed'] for x in w)
    matched = sum(x['matched'] for x in w) + sum(x['skipped'] for x in w)
    return dict(windows=len(w), frames=frames, verify=last['verify'], skip=last['skip'],
                sum_recorded=sum(x['recorded'] for x in w), sum_dropped=sum(x['dropped'] for x in w),
                sum_compared=sum(x['compared'] for x in w), sum_mismatches=sum(x['mismatches'] for x in w),
                mismatches_total=last['mismatches_total'], stale_total=last['stale_total'],
                sum_stale=sum(x['stale'] for x in w), hashed=hashed, matched_or_skipped=matched,
                skippable_pct=100.0 * matched / hashed if hashed else float('nan'),
                calls_pf=sum(x['calls'] for x in w) / frames if frames else float('nan'),
                hash_ms_pf=sum(x['hash_ms'] for x in w) / frames if frames else float('nan'),
                last_recorded=last['recorded'], last_compared=last['compared'], last_matched=last['matched'],
                last_uploaded=last['uploaded'], last=last)


# --------------------------------------------------------------------------------------------- A/B and hold runs


def scene_label(name):
    """Neutral label of a bench_report scene/window name, None for ones not reported. A second occurrence in a run
    gets a one-digit suffix (tron_343_2, match_2); window names like tron_324_690 are not suffixes."""
    if name in SCENE_LABEL:
        return SCENE_LABEL[name]
    m = re.match(r'^(.*)_(\d)$', name)
    if m and m.group(1) in SCENE_LABEL:
        return '%s (occurrence %s)' % (SCENE_LABEL[m.group(1)], m.group(2))
    return None


def ab_run(logs, n, cvar):
    windows = br.DEFAULT_WINDOWS + [br.parse_window(w) for w in EXTRA_WINDOWS]
    run = br.Run(nnn(n), logs, 3600, windows, quiet=True)
    whole = br.ab_analysis(run, cvar, br.DEFAULT_SETTLE)
    per = []
    names = [s[0] for s in run.scenes if not s[0].startswith('mark:')] + [w[0] for w in run.windows]
    for name in names:
        if scene_label(name) is None:
            continue
        r = br.ab_analysis(run, cvar, br.DEFAULT_SETTLE, scene=name)
        if 'error' not in r and r['pairs']:
            per.append((name, r))
    try:
        paired = ab_paired.load_run(nnn(n), logs, br.DEFAULT_SETTLE)
    except Exception as e:  # cross-check only
        paired = {'error': str(e)}
    return dict(run=run, whole=whole, per=per, paired=paired, validity=run.validity())


FPS_SCENE_RE = re.compile(r'^run (\d+) (Brock|second entrance|match) \(frames (\d+)-(\d+)\): (\d+) frames, wall '
                          r'([\d.]+) s, ([\d.]+) fps, >20 ms (\d+) \(([\d.]+)%\), >25 (\d+), >35 (\d+), GPU '
                          r'([\d.]+|nan) ms/frame')
FPS_WIN_RE = re.compile(r'per (\d+) frames fps/GPUms: (.*)$')


def fps60(n):
    p = subprocess.run([sys.executable, os.path.join(HERE, 'fps60.py'), nnn(n)], capture_output=True, text=True,
                       timeout=600)
    scenes, cur = [], None
    for line in p.stdout.splitlines():
        m = FPS_SCENE_RE.match(line.strip())
        if m:
            g = m.groups()
            cur = dict(scene=g[1], f0=int(g[2]), f1=int(g[3]), frames=int(g[4]), wall=float(g[5]), fps=float(g[6]),
                       over20=int(g[7]), over20_pct=float(g[8]), over25=int(g[9]), over35=int(g[10]),
                       gpu=float(g[11]), windows=[])
            scenes.append(cur)
            continue
        m = FPS_WIN_RE.search(line)
        if m and cur is not None:
            size = int(m.group(1))
            vals = [tuple(float(v) for v in x.split('/')) for x in m.group(2).split()]
            for k, (fps, gpu) in enumerate(vals):
                n_in = min(size, cur['frames'] - k * size)
                cur['windows'].append(dict(fps=fps, gpu=gpu, frames=n_in, full=n_in == size))
    return dict(scenes=scenes, rc=p.returncode, err=p.stderr.strip()[-300:])


def window_summary(sc):
    full = [w for w in sc['windows'] if w['full']]
    if not full:
        return None, None, None
    return (min(w['fps'] for w in full), min(w['gpu'] for w in full), max(w['gpu'] for w in full))


# --------------------------------------------------------------------------------------------- provenance


class Prov:
    """Recomputed values next to the notes' values. A notes value with decimals is a string exactly as the notes
    wrote it ('-3.40', '16.9'): it is compared at that many decimals and printed as written. Integers compare
    exactly. A float notes value is refused (repr(-3.40) is '-3.4', which would loosen the check tenfold)."""

    def __init__(self, notes):
        for k, v in notes.items():
            if isinstance(v, float):
                raise ValueError('notes value %r = %r: write decimals as a string as in the notes (e.g. \'-3.40\')'
                                 % (k, v))
        self.notes = notes
        self.rows = []
        self.used = set()

    def check(self, key, value, label, nd=2):
        if key not in self.notes:
            return
        self.used.add(key)
        note = self.notes[key]
        if value is None or (isinstance(value, float) and math.isnan(value)):
            verdict = 'not recomputable'
        elif isinstance(note, str):
            # The notes round to their own precision: compare at the notes' number of decimals.
            dec = len(note.split('.')[1]) if '.' in note else 0
            verdict = 'same' if abs(value - float(note)) <= 0.5 * 10 ** (-dec) + 1e-9 else 'DIFFERS'
            nd = max(nd, dec)
        else:
            verdict = 'same' if value == note else 'DIFFERS'
        self.rows.append((label, note, value, verdict, nd))

    def unchecked(self):
        return [k for k in self.notes if k not in self.used]

    def not_same(self):
        return [r for r in self.rows if r[3] != 'same']


# --------------------------------------------------------------------------------------------- markdown


def ci_text(s):
    if s.get('ci_lo') is None or math.isnan(s['ci_lo']):
        return 'no CI: %d pairs' % (s['pos'] + s['neg'])
    return '%s .. %s' % (f2(s['ci_lo'], sign=True), f2(s['ci_hi'], sign=True))


def md_table(header, rows):
    out = ['| ' + ' | '.join(header) + ' |', '|' + '|'.join('---' for _ in header) + '|']
    for r in rows:
        out.append('| ' + ' | '.join(str(x) for x in r) + ' |')
    return out


def config_cell(cfg):
    return '%s, MSAA %s, entrances %d fps, route %s' % (cfg['res'], cfg['msaa'], cfg['entr_fps'], cfg['route'])


def build_study(st, logs, prov_out):
    prov = Prov(st['notes'])
    problems = []   # numbers that could not be recomputed (shown in Provenance, counted on the console)
    L = []
    gen = datetime.datetime.now().strftime('%Y-%m-%d %H:%M')
    L += ['# %s: %s' % (st['name'], st['title']), '',
          'Generated by `py tools/bench/upstream_report.py --only %s` on %s from our own run outputs (game log, '
          'timeline, frame CSV). Numbers only: no images, no game code. Draft material for a possible upstream '
          'ReXGlue issue; nothing has been sent upstream.' % (st['name'], gen), '',
          'Setting: `%s` (default off). Patch: %s.' % (st['name'], st['patch']), '', '## What the setting changes', '']
    for p in st['what']:
        L += ['- ' + p]
    L += ['', '## Test system', '']
    L += md_table(['', ''], TEST_SYSTEM)
    L += ['', 'Scenes: "entrance 1" and "entrance 2" are pre-match cutscenes (heavy GPU work: crowd, lighting, '
          'post-processing), "gameplay" is the 1v1 match. Route A and route B differ in the second entrance and '
          'the arena. Runs replay a recorded input script with a frozen save, so every run shows the same content '
          '(camera shots can vary between runs).', '']
    cfgs = {}

    def cfg(n):
        if n not in cfgs:
            cfgs[n] = run_config(logs, n)
        return cfgs[n]

    # ---- correctness
    L += ['## Correctness: verify runs', '']
    rows = []
    for n in st['verify']:
        if not log_exists(logs, n):
            problems.append('run %s: verify log missing' % nnn(n))
            rows.append((nnn(n), 'log missing', '', '', '', '', ''))
            continue
        c = cfg(n)
        if st['verify_kind'] == 'texdirect':
            v = parse_texdirect(logs, n)
            if not v:
                problems.append('run %s: no [TEXDIRECT] lines in the log' % nnn(n))
                rows.append((nnn(n), config_cell(c), 'no [TEXDIRECT] lines', '', '', '', ''))
                continue
            prov.check(('verify', n, 'run_direct'), v['run_direct'], 'run %s direct loads (run total)' % nnn(n), 0)
            prov.check(('verify', n, 'run_compared'), v['run_compared'], 'run %s loads compared' % nnn(n), 0)
            prov.check(('verify', n, 'run_mismatches'), v['run_mismatches'], 'run %s mismatches' % nnn(n), 0)
            old = v['sum_old']
            rows.append((nnn(n), config_cell(c), '%s frames' % thousands(v['frames']),
                         '%s direct (%s 32bpb, %s 64bpb); %s on the old path' % (
                             thousands(v['run_direct']), thousands(v['d32']), thousands(v['d64']), thousands(old)),
                         thousands(v['run_compared']), '**%d**' % v['run_mismatches'],
                         '%s (verify slots full)' % thousands(v['sum_dropped'])))
            v['cfg'] = c
            st.setdefault('_tex', {})[n] = v
        else:
            v = parse_upskip(logs, n)
            if not v:
                problems.append('run %s: no [UPSKIP] lines in the log' % nnn(n))
                rows.append((nnn(n), config_cell(c), 'no [UPSKIP] lines', '', '', '', ''))
                continue
            prov.check(('verify', n, 'mismatches_total'), v['mismatches_total'], 'run %s mismatches (total)' % nnn(n), 0)
            prov.check(('verify', n, 'stale_total'), v['stale_total'], 'run %s pages stale under clear=false' % nnn(n), 0)
            prov.check(('verify', n, 'last_recorded'), v['last_recorded'], 'run %s last window readbacks' % nnn(n), 0)
            prov.check(('verify', n, 'last_compared'), v['last_compared'], 'run %s last window compared' % nnn(n), 0)
            prov.check(('verify', n, 'last_matched'), v['last_matched'], 'run %s last window pages matched' % nnn(n), 0)
            prov.check(('verify', n, 'last_uploaded'), v['last_uploaded'], 'run %s last window pages uploaded' % nnn(n), 0)
            rows.append((nnn(n), config_cell(c), '%s frames' % thousands(v['frames']),
                         '%.0f%% of %s hashed re-uploads unchanged; %.0f upload calls/frame' % (
                             v['skippable_pct'], thousands(v['hashed']), v['calls_pf']),
                         thousands(v['sum_compared']), '**%d**' % v['mismatches_total'],
                         '%s; stale under clear=false: %d' % (thousands(v['sum_dropped']), v['stale_total'])))
    if st['verify_kind'] == 'texdirect':
        L += md_table(['run', 'configuration', 'length', 'loads', 'compared byte for byte', 'mismatches',
                       'not compared'], rows)
        tex = st.get('_tex', {})
        if tex:
            L += ['', 'Old-path loads by reason (whole run; a load can be counted under one reason):', '']
            keys = sorted({k for v in tex.values() for k in v['reasons']})
            keys = [k for k in keys if any(v['reasons'].get(k) for v in tex.values())]
            L += md_table(['run'] + keys, [[nnn(n)] + [thousands(v['reasons'].get(k, 0)) for k in keys]
                                           for n, v in sorted(tex.items())])
            L += ['', 'Direct loads per window are summed and checked against the run total the SDK prints: %s.' %
                  ', '.join('run %s %s' % (nnn(n), 'equal' if v['sum_direct'] == v['run_direct'] else
                                           'DIFFERENT (%d vs %d)' % (v['sum_direct'], v['run_direct']))
                            for n, v in sorted(tex.items()))]
    else:
        L += md_table(['run', 'configuration', 'length', 'pages', 'pages compared', 'mismatches',
                       'not compared (dropped); stale pages'], rows)
        L += ['', '"Unchanged" = hashed pages whose bytes matched the last upload (uploaded anyway in verify mode). '
              '"Stale under clear=false" = pages that changed while only the frame-end clear had invalidated them.']
    L += ['']

    # ---- A/B
    L += ['## Performance: in-run A/B', '',
          'One run, one build: the input script switches the setting on and off at a fixed guest-frame period through '
          'both entrances and the gameplay, so both arms see the same content. CP frames are split by the value in '
          'force; the frame containing a toggle and the next %d are dropped. Consecutive segments are paired without '
          'overlap (off/on and on/off orders both occur, listed per run); each pair gives one difference B - A '
          '(on minus off). The tables show the mean, median, a bootstrap 95%% CI over pairs (10,000 resamples), and '
          'how many pairs were faster with the setting on. GPU ms/frame is the sum of the per-submission GPU '
          'timestamp durations (TOP_OF_PIPE..BOTTOM_OF_PIPE around each submission) of the submissions carrying '
          'that frame; CP busy is the command-processor thread\'s busy time per frame; submit is the GPU submit '
          'thread\'s time per frame.' % br.DEFAULT_SETTLE, '']
    for n, cvar in st['ab']:
        if not log_exists(logs, n):
            problems.append('run %s: A/B outputs missing' % nnn(n))
            L += ['Run %s: outputs missing; no A/B numbers recomputed (see Provenance).' % nnn(n), '']
            continue
        c = cfg(n)
        ab = ab_run(logs, n, cvar)
        w = ab['whole']
        if 'error' in w:
            problems.append('run %s: A/B analysis failed' % nnn(n))
            L += ['Run %s: %s' % (nnn(n), w['error']), '']
            continue
        A, B = w['arms']['A'], w['arms']['B']
        L += ['Run %s (%s; %d toggles, median %.1f s apart; %d pairs, orders %s%s):' % (
            nnn(n), config_cell(c), w['flips'], w['typical_ms'] / 1000.0, w['pairs'],
            ', '.join('%s %d' % kv for kv in sorted((w.get('orders') or {}).items())).replace(
                'AB', 'off-on').replace('BA', 'on-off'),
            '' if not ab['validity'] else '; validity notes: ' + '; '.join(map(str, ab['validity']))), '']
        rows = []
        for m, label in (('gpu_ms', 'GPU ms/frame'), ('busy', 'CP busy ms/frame'), ('asub', 'submit thread ms/frame')):
            s = w['metrics'].get(m)
            if not s:
                continue
            rows.append((label, f2(A['means'].get(m)), f2(B['means'].get(m)), '**%s**' % f2(s['mean'], sign=True),
                         f2(s['median'], sign=True), ci_text(s), '%d of %d' % (s['neg'], s['pos'] + s['neg'])))
        L += md_table(['whole run', 'off (A)', 'on (B)', 'B - A mean', 'median', '95% CI', 'pairs faster'], rows)
        s = w['metrics'].get('gpu_ms', {})
        prov.check(('ab', n, 'gpu_ms', 'mean'), s.get('mean'), 'run %s GPU B - A mean' % nnn(n))
        prov.check(('ab', n, 'gpu_ms', 'ci_lo'), s.get('ci_lo'), 'run %s GPU 95%% CI low' % nnn(n))
        prov.check(('ab', n, 'gpu_ms', 'ci_hi'), s.get('ci_hi'), 'run %s GPU 95%% CI high' % nnn(n))
        prov.check(('ab', n, 'gpu_ms', 'neg'), s.get('neg'), 'run %s pairs faster (GPU)' % nnn(n), 0)
        prov.check(('ab', n, 'pairs'), w['pairs'], 'run %s pairs' % nnn(n), 0)
        prov.check(('ab', n, 'arm', 'A', 'gpu_ms'), A['means'].get('gpu_ms'), 'run %s GPU arm off' % nnn(n))
        prov.check(('ab', n, 'arm', 'B', 'gpu_ms'), B['means'].get('gpu_ms'), 'run %s GPU arm on' % nnn(n))
        prov.check(('ab', n, 'busy', 'mean'), w['metrics'].get('busy', {}).get('mean'),
                   'run %s CP busy B - A mean' % nnn(n))
        rows = []
        for name, r in ab['per']:
            g = r['metrics'].get('gpu_ms')
            b = r['metrics'].get('busy')
            label = scene_label(name)
            rows.append((label, r['pairs'], f2(r['arms']['A']['means'].get('gpu_ms')),
                         f2(r['arms']['B']['means'].get('gpu_ms')),
                         '%s (%s)' % (f2(g['mean'], sign=True), ci_text(g)) if g else 'n/a',
                         '%d of %d' % (g['neg'], g['pos'] + g['neg']) if g else '',
                         '%s (%s)' % (f2(b['mean'], sign=True), ci_text(b)) if b else 'n/a'))
        if rows:
            L += ['', 'Per scene (same method, pairs inside the scene; no CI below %d pairs):' % br.AB_MIN_PAIRS_CI, '']
            L += md_table(['scene', 'pairs', 'GPU off', 'GPU on', 'GPU B - A (95% CI)', 'pairs faster',
                           'CP busy B - A (95% CI)'], rows)
        pr = ab['paired']
        if isinstance(pr, dict) and 'error' not in pr and pr:
            rows = []
            for name, r in pr.items():
                d, wdt = ab_paired.ci(r['diffs'])
                A0, B1 = r['arm'][0], r['arm'][1]
                if not A0 or not B1:
                    continue
                label = AB_PAIRED_LABEL.get(name, UNLABELLED)
                prov.check(('paired', n, name), d, 'run %s ab_paired CP busy B - A, %s' % (nnn(n), label))
                prov.check(('paired_w', n, name), wdt, 'run %s ab_paired CP busy CI half-width, %s' % (nnn(n), label))
                rows.append((label, len(r['diffs']),
                             '%s +- %s' % (f2(d, sign=True), f2(wdt)),
                             '%d of %d' % (sum(x < 0 for x in r['diffs']), len(r['diffs']))))
            if rows:
                L += ['', 'Cross-check with an independent implementation (tools/bench/ab_paired.py: CP busy only, '
                      'own scene cuts and segmenting, normal-approximation 95% CI):', '']
                L += md_table(['scene', 'pairs', 'CP busy B - A ms', 'pairs faster'], rows)
        L += ['']

    # ---- hold runs
    L += ['## End to end: runs with the setting on', '',
          'Whole scenes, frame times from the game\'s frame log (presented frames), GPU ms/frame as above. '
          '"Lowest 600-frame window" excludes a scene\'s last partial window (it contains the load into the next '
          'scene).', '']
    rows = []
    fps_cache = {}

    fps_dir_ok = same_dir(logs, fps60_logs())

    def fps_of(n):
        if n not in fps_cache:
            if not log_exists(logs, n):
                fps_cache[n] = None
            elif not fps_dir_ok:
                fps_cache[n] = dict(scenes=[], rc=None, err='fps60.py reads only its own fixed logs folder, not '
                                                            'the --logs folder given')
            else:
                fps_cache[n] = fps60(n)
        return fps_cache[n]

    def hold_rows(n, tag=''):
        out = []
        r = fps_of(n)
        if r is None:
            problems.append('run %s: outputs missing' % nnn(n))
            return [(nnn(n) + tag, 'outputs missing', '', '', '', '', '', '')]
        if r['rc'] != 0 or not r['scenes']:
            err = (r.get('err') or '').strip().splitlines()
            if r['rc'] is None:
                why = r['err']
            elif err:
                # Only the exception type goes into the docs: messages can hold paths or game file names.
                m = re.match(r'^([A-Za-z_][\w.]*)', err[-1])
                why = 'fps60.py failed (%s)' % (m.group(1) if m else 'exit code %s' % r['rc'])
                print('upstream_report: fps60.py %s failed (exit %s): %s' % (nnn(n), r['rc'], err[-1]),
                      file=sys.stderr)
            else:
                why = 'fps60.py exit code %s, no scenes found' % r['rc']
            problems.append('run %s: end-to-end numbers not recomputed (%s)' % (nnn(n), why))
            return [(nnn(n) + tag, 'not recomputed: %s' % why, '', '', '', '', '', '')]
        c = cfg(n)
        for sc in r['scenes']:
            lo_fps, lo_gpu, hi_gpu = window_summary(sc)
            label = FPS60_LABEL.get(sc['scene'], UNLABELLED)
            prov.check(('hold', n, sc['scene'], 'fps'), sc['fps'], 'run %s %s fps' % (nnn(n), label))
            prov.check(('hold', n, sc['scene'], 'gpu'), sc['gpu'], 'run %s %s GPU ms/frame' % (nnn(n), label))
            prov.check(('hold', n, sc['scene'], 'over20'), sc['over20'], 'run %s %s frames > 20 ms' % (nnn(n), label), 0)
            if c['entr_fps'] == 30 and sc['scene'] != 'match':
                slow = '%d > 35 ms (%.2f%%)' % (sc['over35'], 100.0 * sc['over35'] / max(1, sc['frames']))
            else:
                slow = '%d > 20 ms (%.2f%%)' % (sc['over20'], sc['over20_pct'])
            out.append((nnn(n) + tag, config_cell(c), label, f2(sc['fps']), f2(lo_fps, 1), slow, f2(sc['gpu']),
                        '%s .. %s' % (f2(lo_gpu, 1), f2(hi_gpu, 1))))
        return out
    for n in st['hold']:
        rows += hold_rows(n)
    L += md_table(['run', 'configuration', 'scene', 'fps', 'lowest 600-frame window', 'slow frames',
                   'GPU ms/frame', 'GPU per window'], rows)
    L += ['']
    if st.get('context'):
        L += ['### Context: earlier runs without the setting (cross-run, not a controlled comparison)', '',
              'Same route and resolution, but another build and other settings (listed below). Cross-run numbers '
              'vary by about 20% in the heaviest stretches in this project; the in-run A/B above is the controlled '
              'number.', '']
        rows = []
        diffs = []
        for with_n, without_n in st['context']:
            rows += hold_rows(without_n, ' (without)')
            if log_exists(logs, with_n) and log_exists(logs, without_n):
                a, b = dict(public_settings(cfg(without_n))), dict(public_settings(cfg(with_n)))
                keys = sorted(set(a) | set(b))
                d = ['`%s`: %s -> %s' % (k, a.get(k, '(default)'), b.get(k, '(default)')) for k in keys
                     if a.get(k) != b.get(k) and k != 'wwe2k14_input_script']
                diffs.append('- run %s (without) vs run %s (with): %s' % (nnn(without_n), nnn(with_n),
                                                                         '; '.join(d) if d else 'no setting differs'))
        L += md_table(['run', 'configuration', 'scene', 'fps', 'lowest 600-frame window', 'slow frames',
                       'GPU ms/frame', 'GPU per window'], rows)
        L += ['', 'Settings that differ (from the logs):', ''] + diffs + ['']

    # ---- provenance
    L += ['## Provenance', '',
          'Recomputed: every number in the tables above, from the raw run outputs (logs, timelines, frame CSVs of '
          'the runs named), with the same tools the sessions used (bench_report.py A/B analysis, ab_paired.py, '
          'fps60.py, the SDK\'s own verify log lines). %s' % (
              'Every table row has its numbers.' if not problems else
              'Exceptions: rows marked "outputs missing", "log missing" or "not recomputed" carry no numbers:'), '']
    if problems:
        L += ['- ' + p for p in problems] + ['']
    L += [
          'Taken from the notes only (PLAYBOOK, docs/session7b_1440p60_notes.md, docs/session7c_4k60_notes.md), not '
          'recomputed here: ' + '; '.join(['the GPU driver version and "no Resizable BAR" in Test system'] +
                                          st.get('from_notes', [])) + '.', '',
          'Cross-check: the values the notes recorded next to the same values recomputed now (compared at the '
          'notes\' rounding).', '']
    rows = [(label, note, f2(val, nd) if isinstance(val, float) else val, verdict) for label, note, val, verdict, nd
            in prov.rows]
    missing = prov.unchecked()
    L += md_table(['value', 'notes', 'recomputed', ''], rows)
    if missing:
        L += ['', '**%d notes values not checked** (outputs missing or not produced by these tools): ' % len(missing) +
              '; '.join(notes_key_text(k) + ' = ' + str(st['notes'][k]) for k in missing) + '.']
    L += ['']

    # ---- configuration
    L += ['## Run configuration', '', 'Non-default settings of every run, from the log (paths left out). Settings '
          'that are not in upstream ReXGlue come from our local patches (patches/rexglue-sdk/0001-0006) or from the '
          'title project (wwe2k14_*). In the A/B runs the setting under test is also toggled by the input script.', '']
    runs = sorted(cfgs)
    per = {n: dict(public_settings(cfgs[n])) for n in runs}
    keys = []
    for n in runs:
        keys += [k for k in per[n] if k not in keys]
    common = [k for k in keys if all(per[n].get(k) == per[runs[0]].get(k) for n in runs)]
    differ = [k for k in keys if k not in common]
    L += md_table(['run', 'started', 'script', 'configuration', 'hard fails in the log'],
                  [(cfgs[n]['n'], cfgs[n]['start'] or '?', cfgs[n]['script'] or '?', config_cell(cfgs[n]),
                    ', '.join('%s %d' % kv for kv in cfgs[n]['hard'].items()) or 'none') for n in runs])
    L += ['', 'Same in every run listed: ' + ', '.join('`%s = %s`' % (k, per[runs[0]][k]) for k in common) + '.', '']
    if differ:
        L += ['Settings that differ between the runs listed ("-" = not set: the SDK or title default):', '']
        L += md_table(['setting'] + [cfgs[n]['n'] for n in runs],
                      [[k] + [per[n].get(k, '-') for n in runs] for k in differ])
    prov.problems = problems
    prov_out.append((st['name'], prov))
    return '\n'.join(L).rstrip() + '\n'


def write_index(out_dir, studies, provs):
    L = ['# Upstream benchmark tables', '',
         'Draft tables for possible upstream ReXGlue issues, one file per SDK setting. Generated; do not edit by '
         'hand. Regenerate with:', '', '```', 'py tools/bench/upstream_report.py', '```', '',
         'Rules: numbers only (no game imagery, no game code, no game strings, no paths of this machine). '
         'Whether anything here is sent upstream is Jarrod\'s decision; nothing has been sent.', '']
    rows = []
    for st in studies:
        prov = dict(provs).get(st['name'])
        n_diff = len(prov.not_same()) if prov else 0
        rows.append(('[%s](%s.md)' % (st['name'], st['name']), st['title'],
                     ', '.join(nnn(n) for n in st['verify']), ', '.join(nnn(n) for n, _ in st['ab']),
                     ', '.join(nnn(n) for n in st['hold']),
                     '%d checked, %d not the same, %d not checked' % (len(prov.rows), n_diff, len(prov.unchecked()))
                     if prov else ''))
    L += md_table(['setting', 'what', 'verify runs', 'A/B runs', 'runs with it on', 'notes cross-check'], rows)
    with open(os.path.join(out_dir, 'README.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--only', action='append', help='setting name (repeatable); default: all studies')
    ap.add_argument('--logs', default=DEFAULT_LOGS,
                    help='run outputs folder (default: %(default)s). fps60.py (end-to-end tables) only reads its own '
                         'fixed folder: with another --logs those rows say "not recomputed" and the exit code is 1')
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--stdout', action='store_true', help='print the Markdown instead of writing files')
    args = ap.parse_args()
    lower_priority()
    studies = [s for s in STUDIES if not args.only or s['name'] in args.only]
    if not studies:
        print('upstream_report: no study named %s (have: %s)' % (args.only, ', '.join(s['name'] for s in STUDIES)),
              file=sys.stderr)
        return 2
    os.makedirs(args.out, exist_ok=True)
    provs = []
    bad = 0
    for st in studies:
        text = build_study(st, args.logs, provs)
        prov = provs[-1][1]
        n_diff = prov.not_same()
        unchecked = prov.unchecked()
        bad += len(n_diff) + len(unchecked) + len(prov.problems)
        out = sys.stderr if args.stdout else sys.stdout
        if args.stdout:
            sys.stdout.write(text)
            where = st['name']
        else:
            path = os.path.join(args.out, st['name'] + '.md')
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
            where = 'wrote ' + os.path.relpath(path, REPO)
        print('%s (%d notes values checked, %d not the same, %d not checked, %d rows not recomputed%s)' % (
            where, len(prov.rows), len(n_diff), len(unchecked), len(prov.problems),
            ': ' + '; '.join('%s notes %s recomputed %s' % (r[0], r[1], r[2]) for r in n_diff) if n_diff else ''),
            file=out)
        for p in prov.problems:
            print('  WARNING %s' % p, file=out)
        if unchecked:
            print('  WARNING notes values not checked: %s' % '; '.join(notes_key_text(k) for k in unchecked), file=out)
    if not args.stdout and not args.only:
        write_index(args.out, studies, provs)
        print('wrote %s' % os.path.relpath(os.path.join(args.out, 'README.md'), REPO))
    # exit 1: some notes value differs or could not be checked, or a table row has no numbers (docs still written)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
