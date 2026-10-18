"""tools/bisect: which flag(s) or which commit changes a route's outcome (Session 8, task 14). Docs: tools/bisect/README.md

    py tools\\bisect\\autobisect.py flags --route determinism_lesnar.txt --until d_match0900 ^
        --base="--wwe2k14_fixed_seed=1234 --wwe2k14_s7_defaults=false" --preset 4k60 --check shot:d_match0900 --aa
    py tools\\bisect\\autobisect.py flags --route selftest_title.txt --flags="--a=1 --b=2 --c=3" --check "log:some regex"
    py tools\\bisect\\autobisect.py commits --repo ours --good f5808a9 --bad 5d93fb2 --route selftest_title.txt ^
        --base="--wwe2k14_fixed_seed=1234" --check "log:modified: wwe2k14_fixed_seed = '1234'" --hold-lock
    py tools\\bisect\\autobisect.py presets            the presets --preset can expand (parsed from src\\wwe2k14_app.h)
    add --dry-run to print the plan and launch nothing; flag strings are ONE argument written with "=" (--base="...")

Checks (--check, one per bisect; the outcome of a probe is a yes/no, compared with the reference probe's):
    crash                   the run did not end with "EXITED ..., exit code 0", or its log has a hard-fail line
                            (bench_report.HARD_FAILS: crash backtrace, device lost, script FAILED ...)
    log:REGEX               REGEX (Python re) matches a line of the run's log
    shot:LABEL              captures\\NNN_LABEL.bmp differs from the reference probe's: after resizing the larger
                            image to the smaller one (area average, as for the run-175 logo pre-check), changed if
                            MAD > --shot-mad (2.0) or the share of pixels off by > 32 in any channel > --shot-off
                            (0.5 %). Run 175 (4K logo screens vs 720p): MAD 0.8-1.1, <= 0.11 % = resampling only.
    perf:SCENE.METRIC<X     regress.py's scene numbers (brock / second / match; fps, n45, gpu_ms, pct20,
    perf:SCENE.METRIC>X     min_full_window_fps, wall_s): "bad" when the comparison holds (bench routes only)
    perf:bar60              7b/7c's 60-fps bar fails on any scene (window >= 59.5 fps, < 0.5 % of frames > 20 ms)

Flags mode: probe(S) = the route with --base plus the flag subset S. The reference is probe({}); the full set must
change the outcome. 'split' (default) is adaptive group testing: test both halves of a changing set, recurse into
each half that changes; a set that changes while neither half does is reported as an INTERACTION (only the
combination matters). 'each' tests every flag alone. Afterwards the flags not found are tested together on top of
the base (from the cache when that set was already run): if they change the outcome too, that is reported.
Commits mode: first-parent commits after --good up to --bad; the reference is --good, --bad must differ; binary
search for the first commit whose outcome differs. Each commit is built in a scratch git worktree with its own
build folder (tools\\bisect\\wt_build.ps1); the main tree and exe folder are never touched. Details: README.md.
Delete a bisect worktree ONLY with: pwsh -NoProfile -File tools\\bisect\\wt_build.ps1 -Clean -Repo ours|sdk
(never "git worktree remove": it follows the junctions into game_data\\extracted / the SDK submodules).

Every game launch goes through tools\\bench\\run_list.ps1 (run lock, frozen user data mirrored, timeline on, a run
still going after --wait seconds is killed). Default: the lock is taken per probe (others can run in between);
--hold-lock holds it for the whole bisect. Results: out\\harness\\bisect\\<stamp>\\ (bisect.log, result.json).
"""
import argparse
import collections
import datetime
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
DEFAULT_EXE = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'wwe2k14.exe')
RUN_LIST = os.path.join(REPO, 'tools', 'bench', 'run_list.ps1')
RUNLOCK_PS1 = os.path.join(REPO, 'tools', 'runlock.ps1')
SCRIPTS_DIR = os.path.join(REPO, 'tools', 'input_scripts')
APP_H = os.path.join(REPO, 'src', 'wwe2k14_app.h')
OUT = os.path.join(REPO, 'out', 'harness', 'bisect')
HERE = os.path.dirname(os.path.abspath(__file__))
WT_BUILD = os.path.join(HERE, 'wt_build.ps1')
SDK_SRC = os.path.join(REPO, 'thirdparty', 'rexglue-sdk-src')

sys.path.insert(0, os.path.join(REPO, 'tools', 'bench'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'regress'))
import bench_report  # noqa: E402  HARD_FAILS, log_parts
import regress  # noqa: E402  bench_metrics, BAR60, bmp_problem (its main() only runs as a script)

SHOT_MAD = 2.0
SHOT_OFF = 0.5      # % of pixels off by > OFF_LEVEL
OFF_LEVEL = 32


# --------------------------------------------------------------------------------------------------- presets
# Parsed from src/wwe2k14_app.h (OnPreSetup), so they follow the code. base = flags that make every component a real
# change (the component flags are otherwise already on by default: S6/S7 defaults).
PRESET_BLOCKS = collections.OrderedDict([
    ('s6', dict(marker='if (REXCVAR_GET(wwe2k14_s6_defaults))', base='--wwe2k14_s6_defaults=false')),
    ('s7', dict(marker='if (REXCVAR_GET(wwe2k14_s7_defaults))', base='--wwe2k14_s7_defaults=false')),
    ('1440p60', dict(marker='if (REXCVAR_GET(wwe2k14_preset_1440p60) || REXCVAR_GET(wwe2k14_preset_4k60))',
                     base='')),
])
PAIR = re.compile(r'std::pair\{"([^"]+)",\s*"([^"]*)"\}')


def parse_presets():
    with open(APP_H, encoding='utf-8') as f:
        text = f.read()
    out = collections.OrderedDict()
    for name, spec in PRESET_BLOCKS.items():
        i = text.find(spec['marker'])
        if i < 0:
            continue
        j = text.find('SetFlagByName(name, value)', i)
        pairs = PAIR.findall(text[i:j if j > 0 else i + 4000])
        out[name] = dict(flags=['--%s=%s' % kv for kv in pairs], base=spec['base'],
                         source='src/wwe2k14_app.h block %r' % spec['marker'])
    if '1440p60' in out:
        # The 4K preset sets resolution_scale=3 first, then the 1440p60 block leaves it alone (only-if-default).
        m = re.search(r'wwe2k14_preset_4k60\)\s*&&[^{]*\{\s*rex::cvar::SetFlagByName\("resolution_scale",\s*"(\d+)"\)',
                      text)
        scale = m.group(1) if m else None
        if scale:
            flags = ['--resolution_scale=%s' % scale if f.startswith('--resolution_scale=') else f
                     for f in out['1440p60']['flags']]
            out['4k60'] = dict(flags=flags, base='', source=out['1440p60']['source'] +
                               ' with resolution_scale=%s (the wwe2k14_preset_4k60 block)' % scale)
    return out


# ------------------------------------------------------------------------------------------------ flag strings
def split_flags(s):
    """'--a=1 --b' -> ['--a=1', '--b'] (whitespace-separated; values must not contain spaces)."""
    out = []
    for tok in (s or '').split():
        if not tok.startswith('--'):
            raise ValueError('not a --flag: %r' % tok)
        out.append(tok)
    return out


def flag_name(f):
    return f[2:].split('=', 1)[0]


def merge_flags(base, extra):
    """base + extra; a flag in extra replaces the same flag in base."""
    names = {flag_name(f) for f in extra}
    return [f for f in base if flag_name(f) not in names] + list(extra)


# ---------------------------------------------------------------------------------------------------- routes
def resolve_route(route):
    for p in (route, os.path.join(SCRIPTS_DIR, route)):
        if os.path.isfile(p):
            return os.path.abspath(p)
    raise SystemExit('route not found: %s (looked in . and %s)' % (route, SCRIPTS_DIR))


def truncate_route(path, label, outdir):
    """A copy of the route that stops after 'screenshot LABEL': waits 60 frames (the async save must finish before
    quit, PLAYBOOK) and quits. Written under out\\harness\\bisect\\routes (generated, never in the repo)."""
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    in_repeat = False
    for i, line in enumerate(lines):
        word = (line.split() or [''])[0]
        if word == 'repeat':
            in_repeat = True
        elif word == 'end_repeat':
            in_repeat = False
        if line.strip() == 'screenshot %s' % label:
            if in_repeat:
                # The cut copy would have "repeat" without "end_repeat": the script would not load (review nit).
                raise SystemExit('route %s: "screenshot %s" (line %d) is inside a repeat block; --until cannot cut '
                                 'there (use a label outside the block)' % (path, label, i + 1))
            break
    else:
        raise SystemExit('route %s has no "screenshot %s" line' % (path, label))
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(path))[0]
    dst = os.path.join(outdir, '%s_until_%s.txt' % (stem, label))
    body = ['# Generated by tools/bisect/autobisect.py: %s up to "screenshot %s", then wait 60 frames and quit.' %
            (os.path.basename(path), label)] + lines[:i + 1] + ['wait_frames 60', 'log bisect: route end', 'quit']
    text = '\n'.join(body) + '\n'
    if not os.path.exists(dst) or open(dst, encoding='utf-8').read() != text:
        with open(dst, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    return dst


# ---------------------------------------------------------------------------------------------------- checks
class Check:
    def __init__(self, spec, shot_mad, shot_off):
        self.spec = spec
        self.shot_mad, self.shot_off = shot_mad, shot_off
        self.kind, _, self.arg = spec.partition(':')
        if self.kind == 'log':
            try:
                self.rx = re.compile(self.arg)
            except re.error as e:
                raise SystemExit('log:REGEX: invalid regular expression %r: %s' % (self.arg, e))
        elif self.kind == 'shot':
            if not self.arg:
                raise SystemExit('shot:LABEL needs a label')
        elif self.kind == 'perf':
            if self.arg != 'bar60':
                m = re.match(r'^(brock|second|match)\.(\w+)\s*([<>])\s*([-\d.]+)$', self.arg)
                if not m:
                    raise SystemExit('perf check wants SCENE.METRIC<X, SCENE.METRIC>X or bar60, got %r' % self.arg)
                self.scene, self.metric, self.op, self.value = m.group(1), m.group(2), m.group(3), float(m.group(4))
        elif self.kind != 'crash':
            raise SystemExit('unknown check %r (crash | log:REGEX | shot:LABEL | perf:...)' % spec)

    def needs_reference_image(self):
        return self.kind == 'shot'

    def evaluate(self, probe, ref):
        """-> (value, detail). value: bool (the outcome), or None = could not be judged (error)."""
        if probe['run'] is None:
            line = probe['line'] or ''
            # The game ran but died before it made a log (e.g. a DLL loader failure, 0xC0000139 / 0xC0000135):
            # run_list still prints the EXITED / KILLED line. For "crash" that IS the outcome (review finding 6).
            if self.kind == 'crash' and NOLOG_DEATH.search(line):
                return True, 'no log, run_list: %s' % line[:160]
            return None, 'no run: %s' % line
        lines = probe['lines']
        crashed = not re.search(r'EXITED after [\d.,]+s, exit code 0\b', probe['line'] or '')
        hard = [x for x in lines if any(p.search(x) for _, p in bench_report.HARD_FAILS)]
        if self.kind == 'crash':
            bad = crashed or bool(hard)
            return bad, ('run_list: %s' % (probe['line'] or '')[:160]) + (
                '; hard-fail: %s' % bench_report.LOG_PREFIX.sub('', hard[0].rstrip())[:160] if hard else '')
        if crashed or hard:
            # Not what this check measures, but the data it needs may be missing: say so in the detail.
            note = ' (NOTE: run did not exit cleanly%s)' % ('; hard-fail line' if hard else '')
        else:
            note = ''
        if self.kind == 'log':
            hit = next((x for x in lines if self.rx.search(x)), None)
            return (hit is not None), ('match: %s' % bench_report.LOG_PREFIX.sub('', hit.rstrip())[:160]
                                       if hit else 'no line matches') + note
        if self.kind == 'shot':
            path = probe['shot']
            prob = regress.bmp_problem(path) if path else 'no capture path'
            if prob:
                return None, 'screenshot %s: %s%s' % (self.arg, prob, note)
            if ref is None:
                return False, 'reference image %s' % os.path.basename(path)
            m = shot_metrics(ref['shot'], path)
            if 'error' in m:
                return None, m['error']
            probe['shot_metrics'] = m
            changed = m['mad'] > self.shot_mad or m['off32'] > self.shot_off
            return changed, ('vs %s: MAD %.3f, off>32 %.3f%%, off>16 %.3f%%, max %d (%s)%s' % (
                os.path.basename(ref['shot']), m['mad'], m['off32'], m['off16'], m['max'], m['how'], note))
        if self.kind == 'perf':
            b = regress.bench_metrics(probe['logs'], probe['run'], lines)
            if self.arg == 'bar60':
                fails = []
                for s in regress.SCENES:
                    sc = b['scenes'].get(s)
                    if sc is None:
                        return None, 'scene %s not found (anchors %s)%s' % (s, b['anchors'], note)
                    mw = sc['min_full_window_fps']
                    if mw is None or mw < regress.BAR60['min_window_fps'] or sc['pct20'] >= regress.BAR60['max_pct20']:
                        fails.append('%s min window %s, >20 ms %.2f%%' % (s, mw, sc['pct20']))
                return bool(fails), ('bar60 fails: ' + '; '.join(fails) if fails else 'bar60 passes') + note
            sc = b['scenes'].get(self.scene)
            if sc is None or sc.get(self.metric) is None:
                return None, '%s.%s not found (anchors %s)%s' % (self.scene, self.metric, b['anchors'], note)
            v = sc[self.metric]
            bad = v < self.value if self.op == '<' else v > self.value
            return bad, '%s.%s = %s (bad if %s %s)%s' % (self.scene, self.metric, v, self.op, self.value, note)
        raise AssertionError(self.kind)


def shot_metrics(a_path, b_path):
    """MAD / share of pixels off by > 16 and > 32 (max over R, G, B). Different sizes: the larger image is resized
    to the smaller one with an area average (Image.BOX), as for the run-175 cross-resolution pre-check."""
    import numpy as np
    from PIL import Image
    with Image.open(a_path) as ia, Image.open(b_path) as ib:
        a, b = ia.convert('RGB'), ib.convert('RGB')
        how = '%dx%d' % a.size
        if a.size != b.size:
            ra, rb = a.size[0] / a.size[1], b.size[0] / b.size[1]
            if abs(ra - rb) > 0.01:
                return dict(error='aspect differs: %dx%d vs %dx%d' % (a.size + b.size))
            if a.size[0] > b.size[0]:
                how = '%dx%d -> %dx%d' % (a.size + b.size)
                a = a.resize(b.size, Image.BOX)
            else:
                how = '%dx%d -> %dx%d' % (b.size + a.size)
                b = b.resize(a.size, Image.BOX)
        x = np.asarray(a, dtype=np.int16)
        y = np.asarray(b, dtype=np.int16)
    d = np.abs(x - y)
    dm = d.max(axis=2)
    return dict(mad=float(d.mean()), off16=float((dm > 16).mean() * 100), off32=float((dm > OFF_LEVEL).mean() * 100),
                max=int(d.max()), how=how)


# --------------------------------------------------------------------------------------------------- launching
def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


RUNLIST_LINE = re.compile(r'^(\S+\.txt) -> (?:wwe2k14_(\d+))?')
# run_and_triage's end-of-run lines (EXITED with a non-zero code, still running, killed, aborted).
NOLOG_DEATH = re.compile(r'EXITED after [\d.,]+s, exit code (?!0\b)-?\d+|STILL RUNNING|KILLED|ABORT')


class Prober:
    """Runs probes (one game run each) and caches them by (exe, flag set)."""

    def __init__(self, args, route_path, check, log, dry_run=False):
        self.args, self.route_path, self.check, self.log = args, route_path, check, log
        self.dry_run = dry_run
        self.cache = collections.OrderedDict()
        self.ref = None
        self.n_launched = 0
        self.history = []   # every probe evaluated in this call, in order (the cache can drop entries)

    def script_arg(self, exe):
        exe_dir = os.path.dirname(os.path.abspath(exe))
        try:
            return os.path.relpath(self.route_path, exe_dir)
        except ValueError:
            return self.route_path

    def launch(self, exe, flags):
        script = self.script_arg(exe)
        cmd = '& %s -Scripts @(%s) -Extra %s -Exe %s -WaitSeconds %d' % (
            ps_quote(RUN_LIST), ps_quote(script), ps_quote(' '.join(flags)), ps_quote(exe), self.args.wait)
        self.log('$ pwsh -NoProfile -Command "%s"' % cmd)
        if self.dry_run:
            return None, 'dry run'
        p = subprocess.run(['pwsh', '-NoProfile', '-Command', cmd], capture_output=True, text=True, errors='replace')
        out = (p.stdout or '') + (p.stderr or '')
        for line in out.splitlines():
            self.log('  | ' + line)
        leaf = os.path.basename(script)
        for line in out.splitlines():
            m = RUNLIST_LINE.match(line.strip())
            if m and m.group(1).lower() == leaf.lower():
                return m.group(2), line.strip()
        return None, (out.strip().splitlines()[-1] if out.strip() else 'no output from run_list.ps1')

    def disk_key(self, exe, flags):
        """Identity of a probe across bisect calls: exe + staged rex*.dll (path, size, mtime), the route file's bytes,
        the flag set. A later call (resume, other thresholds) reuses the run instead of launching it again."""
        exe_dir = os.path.dirname(exe)
        ids = []
        for f in [exe] + sorted(glob.glob(os.path.join(exe_dir, 'rex*.dll'))):
            st = os.stat(f)
            ids.append('%s|%d|%d' % (os.path.normcase(f), st.st_size, int(st.st_mtime)))
        with open(self.route_path, 'rb') as f:
            route_sha = hashlib.sha256(f.read()).hexdigest()[:16]
        return json.dumps([ids, route_sha, sorted(flags), self.args.wait])

    def probe(self, flags, tag, exe=None, reuse=True):
        exe = os.path.abspath(exe or self.args.exe)
        key = (os.path.normcase(exe), tuple(sorted(flags)))
        if reuse and key in self.cache:
            p = self.cache[key]
            self.log('probe %-28s cached (run %s)' % (tag, p['run']))
            return p
        exe_dir = os.path.dirname(exe)
        logs, captures = os.path.join(exe_dir, 'logs'), os.path.join(exe_dir, 'captures')
        print('probe %-28s %s ...' % (tag, ' '.join(flags) or '(base only)'), flush=True)
        self.log('')
        self.log('=== probe %s: %s' % (tag, ' '.join(flags)))
        t0 = datetime.datetime.now()
        run_flags = list(flags)
        if os.path.normcase(exe) != os.path.normcase(DEFAULT_EXE) and not any(
                flag_name(f) == 'game_data_root' for f in flags):
            # Snapshot / worktree exes: do not rely on the upward search for game_data/extracted (6 levels).
            run_flags.append('--game_data_root=%s' % os.path.join(REPO, 'game_data', 'extracted'))
        dkey = None if self.dry_run else self.disk_key(exe, flags)
        disk = load_disk_cache()
        hit = disk.get(dkey) if (reuse and dkey and not self.args.no_reuse) else None
        if hit and os.path.exists(os.path.join(logs, 'wwe2k14_%s.log' % hit['run'])):
            run, line = hit['run'], hit['line']
            self.log('  reusing run %s (made %s; same exe + DLL path/size/mtime, route bytes, flags)' %
                     (run, hit.get('when')))
        elif getattr(self.args, 'cached_only', False):
            raise ProbeError('--cached-only: probe %s (%s) has no earlier run; stopping here' % (tag, ' '.join(flags)))
        else:
            run, line = self.launch(exe, run_flags)
            self.n_launched += 0 if self.dry_run else 1
            if run and dkey and reuse:
                disk = load_disk_cache()
                disk[dkey] = dict(run=run, line=line, when=datetime.datetime.now().isoformat(timespec='seconds'))
                save_disk_cache(disk)
        lines = []
        if run:
            for part in bench_report.log_parts(logs, run):
                with open(part, encoding='utf-8', errors='replace') as f:
                    lines.extend(f.readlines())
        p = dict(tag=tag, flags=list(flags), exe=exe, run=run, line=line, logs=logs, lines=lines,
                 seconds=round((datetime.datetime.now() - t0).total_seconds(), 1),
                 shot=regress.shot_path(captures, run, self.check.arg) if (run and
                                                                                      self.check.kind == 'shot') else None)
        if self.dry_run:
            p['value'], p['detail'] = None, 'dry run'
        else:
            p['value'], p['detail'] = self.check.evaluate(p, self.ref)
        p['flags_seen'] = cmdline_flags(lines)
        self.cache[key if reuse else key + (tag,)] = p
        self.history.append(p)
        verdict = {True: 'YES', False: 'no', None: 'ERROR'}[p['value']]
        msg = 'probe %-28s run %s  %ss  outcome %s  %s' % (tag, run or '---', p['seconds'], verdict, p['detail'])
        print(msg, flush=True)
        self.log(msg)
        missing = [f for f in flags if flag_name(f) not in p['flags_seen']] if run else []
        if missing:
            self.log('  WARNING: not on the run\'s "modified ... (source: command line)" lines: %s' % ' '.join(missing))
        return p


CACHE_FILE = os.path.join(OUT, 'probe_cache.json')


def load_disk_cache():
    try:
        with open(CACHE_FILE, encoding='utf-8') as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def save_disk_cache(d):
    os.makedirs(OUT, exist_ok=True)
    tmp = CACHE_FILE + '.%d.tmp' % os.getpid()
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(d, f, indent=0)
    os.replace(tmp, CACHE_FILE)


CMDLINE_FLAG = re.compile(r"\] modified: (\w+) = '([^']*)' \(source: ([^)]*)\)")


def cmdline_flags(lines):
    out = {}
    for x in lines[:600]:
        m = CMDLINE_FLAG.search(x)
        if m:
            out[m.group(1)] = (m.group(2), m.group(3))
    return out


def outcome_changed(p, ref):
    """True / False against the reference outcome; None = the probe could not be judged."""
    if p['value'] is None or ref['value'] is None:
        return None
    return p['value'] != ref['value']


# ------------------------------------------------------------------------------------------------- flags mode
def bisect_flags(args, prober, log, result):
    base = split_flags(args.base)
    cands = []
    if args.preset:
        presets = parse_presets()
        if args.preset not in presets:
            raise SystemExit('unknown preset %r (have %s)' % (args.preset, ', '.join(presets)))
        pr = presets[args.preset]
        cands += pr['flags']
        log('preset %s (%s): %s' % (args.preset, pr['source'], ' '.join(pr['flags'])))
        if pr['base'] and not args.no_preset_base:
            base = merge_flags(base, split_flags(pr['base']))
            log('preset base added (makes every component a real change): %s' % pr['base'])
    user = split_flags(args.flags)
    # --flags overrides a preset component of the same name (review finding 7), and says so.
    over = [f for f in cands if flag_name(f) in {flag_name(u) for u in user} and f not in user]
    if over:
        msg = 'NOTE: --flags overrides preset value(s): %s -> %s' % (
            ' '.join(over), ' '.join(u for u in user if flag_name(u) in {flag_name(o) for o in over}))
        print(msg)
        log(msg)
    cands = merge_flags(cands, user)
    seen = set()
    cands = [f for f in cands if not (flag_name(f) in seen or seen.add(flag_name(f)))]
    if not cands:
        raise SystemExit('no candidate flags (--flags or --preset)')
    clash = [f for f in cands if flag_name(f) in {flag_name(b) for b in base}]
    if clash:
        log('NOTE: candidates override base flags: %s' % ' '.join(clash))
    log('base: %s' % (' '.join(base) or '(none)'))
    log('candidates (%d): %s' % (len(cands), ' '.join(cands)))
    print('base: %s' % (' '.join(base) or '(none)'))
    print('candidates (%d): %s' % (len(cands), ' '.join(cands)))

    def with_base(subset):
        return merge_flags(base, subset)

    result.update(mode='flags', base=base, candidates=cands, strategy=args.strategy)
    ref = prober.probe(with_base([]), 'reference')
    prober.ref = ref
    if args.dry_run:
        n = len(cands)
        print('dry run: reference, %sfull set, then up to %d probes (%s)' % (
            'A/A, ' if args.aa else '', 2 * n - 2 if args.strategy == 'split' else n, args.strategy))
        return result
    if ref['value'] is None:
        result['error'] = 'reference probe could not be judged: %s' % ref['detail']
        return result
    # Candidates whose value the reference run already had (its "modified:" lines, e.g. an S6/S7 default): no-ops.
    noops = []
    for f in cands:
        name, _, val = f[2:].partition('=')
        have = ref.get('flags_seen', {}).get(name)
        if have and have[0].lower() == (val or 'true').lower():
            noops.append('%s (reference has it, source: %s)' % (f, have[1]))
    if noops:
        result['noops'] = noops
        msg = 'NOTE: no-op candidates (already set in the reference run): %s' % '; '.join(noops)
        print(msg)
        log(msg)
    if args.aa:
        # A second reference run (cache bypassed): the noise floor of this route + check.
        aa = prober.probe(with_base([]), 'A/A (reference again)', reuse=False)
        ch = outcome_changed(aa, ref)
        result['aa'] = dict(run=aa['run'], value=aa['value'], detail=aa['detail'], metrics=aa.get('shot_metrics'))
        if ch is None or ch:
            result['error'] = 'A/A: the reference run twice gives %s (%s): noise above the threshold, no bisect' % (
                'an error' if ch is None else 'a different outcome', aa['detail'])
            return result
    full = prober.probe(with_base(cands), 'all candidates')
    ch = outcome_changed(full, ref)
    if not ch:
        result['error'] = ('all candidates together %s the outcome (%s): nothing to bisect' %
                           ('could not be judged for' if ch is None else 'do not change', full['detail']))
        return result

    tests = []   # (subset, changed)

    def changed(subset, tag):
        p = prober.probe(with_base(subset), tag)
        c = outcome_changed(p, ref)
        tests.append(dict(flags=list(subset), run=p['run'], changed=c, detail=p['detail']))
        if c is None:
            raise ProbeError('probe %s could not be judged: %s' % (tag, p['detail']))
        return c

    found, combos = [], []

    def search(S, depth=0):
        if len(S) == 1:
            found.append(S[0])
            return
        h = len(S) // 2
        L, R = S[:h], S[h:]
        cL = changed(L, 'split d%d %s' % (depth, short(L)))
        cR = changed(R, 'split d%d %s' % (depth, short(R)))
        if cL:
            search(L, depth + 1)
        if cR:
            search(R, depth + 1)
        if not cL and not cR:
            combos.append(list(S))

    try:
        if args.strategy == 'each':
            for f in cands:
                if changed([f], 'alone %s' % flag_name(f)):
                    found.append(f)
            if not found:
                combos.append(list(cands))   # the full set changes it, no single flag does
        else:
            search(cands)
        def minimize(S):
            # One-at-a-time removal (1-minimal, as in delta debugging): drop a flag when the set still changes the
            # outcome without it. |S| probes; the result needs every flag it keeps.
            keep = list(S)
            for f in list(S):
                if len(keep) > 1 and changed([x for x in keep if x != f], 'minimize without %s' % flag_name(f)):
                    keep.remove(f)
            return keep

        if getattr(args, 'minimize', False):
            combos[:] = [minimize(S) for S in combos]
        rest = [f for f in cands if f not in found and not any(f in c for c in combos)]
        if rest and (found or combos):
            c = changed(rest, 'the rest together')
            result['rest'] = dict(flags=rest, changed=c)
            if c and getattr(args, 'minimize', False):
                combos.append(minimize(rest))   # an effect hidden by the split (needs flags from both halves)
    except ProbeError as e:
        result['error'] = str(e)
    result['tests'] = tests
    result['found'] = found
    result['interactions'] = combos
    return result


class ProbeError(Exception):
    pass


def short(flags):
    return ','.join(flag_name(f) for f in flags)[:60]


# ----------------------------------------------------------------------------------------------- commits mode
def git(repo, *a, check=True):
    p = subprocess.run(['git', '-C', repo] + list(a), capture_output=True, text=True, errors='replace')
    if check and p.returncode:
        raise SystemExit('git %s failed: %s' % (' '.join(a), (p.stderr or p.stdout).strip()))
    return p.stdout.strip()


def commit_search(n, changed_at):
    """Binary search over commits[0..n-1] (the first-parent commits after --good; commits[n-1] = --bad is known to
    differ). changed_at(i) -> True / False / None (None = could not be judged). Returns (first changed index, None)
    or (None, index that could not be judged). Shared with test_autobisect.py (review nit: test the tool's loop)."""
    lo, hi = -1, n - 1     # commits[hi] differs; lo = last known same (-1 = good)
    while hi - lo > 1:
        mid = (lo + hi) // 2
        c = changed_at(mid)
        if c is None:
            return None, mid
        if c:
            hi = mid
        else:
            lo = mid
    return hi, None


def owner_file(wt):
    return wt + '.owner'


def release_owner(wt):
    """Delete <worktree>.owner if this process wrote it (wt_build.ps1 -Owner <pid>)."""
    f = owner_file(wt)
    try:
        with open(f, encoding='utf-8') as fh:
            text = fh.read()
        if re.search(r'pid=%d\b' % os.getpid(), text):
            os.remove(f)
    except OSError:
        pass


def pin_check(folder):
    """build.ps1 -CheckOnly on a snapshot folder (its rex*.dll must be pinned, from one pin entry)."""
    cmd = ['pwsh', '-NoProfile', '-File', os.path.join(REPO, 'tools', 'build.ps1'), '-CheckOnly', '-ExeDir', folder,
           '-SdkBin', folder]
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    return p.returncode, ((p.stdout or '') + (p.stderr or '')).strip()


def bisect_commits(args, prober, log, result):
    repo = REPO if args.repo == 'ours' else SDK_SRC
    good = git(repo, 'rev-parse', '--verify', args.good + '^{commit}')
    bad = git(repo, 'rev-parse', '--verify', args.bad + '^{commit}')
    if subprocess.run(['git', '-C', repo, 'merge-base', '--is-ancestor', good, bad]).returncode:
        raise SystemExit('--good %s is not an ancestor of --bad %s' % (args.good, args.bad))
    if good not in set(git(repo, 'rev-list', '--first-parent', bad).split()):
        # A --good on a merged side branch: the mainline commits after it lack its changes (not monotonic).
        raise SystemExit('--good %s is not on the first-parent (mainline) history of --bad %s: pick a mainline '
                         'commit (git log --first-parent %s)' % (args.good, args.bad, args.bad))
    commits = git(repo, 'rev-list', '--reverse', '--first-parent', '%s..%s' % (good, bad)).split()
    log('repo %s (%s): %d first-parent commits after %s up to %s' % (args.repo, repo, len(commits), good[:9], bad[:9]))
    for c in commits:
        log('  %s' % git(repo, 'log', '-1', '--format=%h %ad %s', '--date=format:%m-%d %H:%M', c))
    print('%d commits in (%s, %s]' % (len(commits), good[:9], bad[:9]))
    base = split_flags(args.base)
    # SDK worktrees live in thirdparty\sdk-wt\<name> (Session 6 rule); ours under out\harness (gitignored).
    wt = os.path.join(OUT, 'wt', 'ours') if args.repo == 'ours' else os.path.join(REPO, 'thirdparty', 'sdk-wt', 'bisect')
    result.update(mode='commits', repo=args.repo, good=good, bad=bad, commits=commits, base=base, worktree=wt)
    if args.dry_run:
        print('dry run: worktree %s (%s); build + probe %s (reference) and %s, then about %d of the commits '
              'between' % (wt, WT_BUILD, good[:9], bad[:9], max(0, len(commits) - 1).bit_length()))
        for c in [good] + commits:
            print('  %s' % git(repo, 'log', '-1', '--format=%h %ad %s', '--date=format:%m-%d %H:%M', c)[:110])
        prober.probe(base, 'plan (dry run)')
        return result

    # Build inputs fixed at the start (review finding 4): one snapshot of --exe's folder taken under the run lock.
    # 'ours' stages its (pin-checked) rex*.dll into every commit's build; 'sdk' copies its exe for every commit.
    snap_dir = os.path.dirname(snapshot_exe(args.exe))
    log('exe snapshot (build inputs for every commit): %s' % snap_dir)
    print('exe snapshot: %s' % snap_dir, flush=True)
    result['snapshot'] = snap_dir
    if args.repo == 'ours':
        rc, out = pin_check(snap_dir)
        for line in out.splitlines():
            log('  pin | ' + line)
        if rc:
            raise SystemExit('the SDK DLLs in %s are not pinned (build.ps1 -CheckOnly exit %d): pin them '
                             '(tools\\build_sdk.ps1 -Pin) or pass --exe of a pinned build.\n%s' % (snap_dir, rc, out[-600:]))

    built = {}   # {commit: exe} of the build now on disk ('ours': one worktree exe, rebuilt per commit)

    def exe_for(commit):
        if commit in built:
            return built[commit]
        if args.repo == 'ours':
            built.clear()
        cmd = ['pwsh', '-NoProfile', '-File', RUNLOCK_PS1, 'run', '-Name', 'bisect build %s %s' % (args.repo, commit[:9]),
               '--', 'pwsh', '-NoProfile', '-File', WT_BUILD, '-Repo', args.repo, '-Commit', commit, '-Worktree', wt,
               '-Log', os.path.join(prober.outdir, 'build_%s.log' % commit[:9]), '-Owner', str(os.getpid())]
        cmd += ['-DllSource' if args.repo == 'ours' else '-ExeSource', snap_dir]
        log('$ ' + subprocess.list2cmdline(cmd))
        print('build %s ...' % commit[:9], flush=True)
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        out = (p.stdout or '') + (p.stderr or '')
        for line in out.splitlines():
            log('  | ' + line)
        m = re.search(r'^BISECT_EXE (.+)$', out, re.M)
        if p.returncode or not m:
            raise ProbeError('build of %s failed (exit %s): %s' % (commit[:9], p.returncode,
                                                                 ' / '.join(out.strip().splitlines()[-3:])))
        exe = m.group(1).strip()
        print('build %s ok: %s' % (commit[:9], exe), flush=True)
        built[commit] = exe
        return exe

    def run_commit(commit, tag):
        # The probe adds --game_data_root (a worktree exe is too deep for the app's upward search).
        exe = exe_for(commit)
        p = prober.probe(list(base), '%s %s' % (tag, commit[:9]), exe=exe)
        # One exe path for all commits: drop it from the cache so the next commit is really run.
        for k in [k for k, v in prober.cache.items() if v is p]:
            del prober.cache[k]
        p['commit'] = commit
        return p

    try:
        ref = run_commit(good, 'good (reference)')
        prober.ref = ref
        if ref['value'] is None:
            result['error'] = 'reference (good) could not be judged: %s' % ref['detail']
            return result
        if args.check.startswith('shot:'):
            # Keep the reference image: the scratch exe folder's captures are reused by the next commit's run.
            keep = os.path.join(prober.outdir, 'ref_' + os.path.basename(ref['shot']))
            shutil.copyfile(ref['shot'], keep)
            ref['shot'] = keep
        tests = [dict(commit=good, run=ref['run'], value=ref['value'], detail=ref['detail'])]
        pb = run_commit(bad, 'bad')
        tests.append(dict(commit=bad, run=pb['run'], value=pb['value'], detail=pb['detail']))
        cb = outcome_changed(pb, ref)
        if cb is not True:
            result['error'] = ('--bad %s could not be judged (%s)' if cb is None else
                               '--bad %s does not differ from --good (%s)') % (bad[:9], pb['detail'])
            result['tests'] = tests
            return result

        def changed_at(i):
            pm = run_commit(commits[i], 'mid')
            tests.append(dict(commit=commits[i], run=pm['run'], value=pm['value'], detail=pm['detail']))
            c = outcome_changed(pm, ref)
            if c is None:
                result['error'] = 'commit %s could not be judged: %s' % (commits[i][:9], pm['detail'])
            return c

        first, _ = commit_search(len(commits), changed_at)
        if first is not None:
            result['first_changed'] = commits[first]
            result['first_changed_subject'] = git(repo, 'log', '-1', '--format=%h %s', commits[first])
        result['tests'] = tests
    except ProbeError as e:
        result['error'] = str(e)
    finally:
        release_owner(wt)
    return result


# --------------------------------------------------------------------------------------------------- snapshot
SNAP_FILES = ('wwe2k14.exe', 'wwe2k14.map', 'wwe2k14.pdb', 'rexgpu-xenos.dll', 'rexgpu-xenos.pdb', 'rexruntime.dll',
              'rexruntime.pdb')


def snap_hash(folder):
    h = hashlib.sha256()
    for name in ('wwe2k14.exe', 'rexgpu-xenos.dll', 'rexruntime.dll'):
        p = os.path.join(folder, name)
        if os.path.exists(p):
            with open(p, 'rb') as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    h.update(chunk)
    return h.hexdigest()[:12]


def snapshot_exe(exe):
    """A copy of the exe + staged SDK DLLs (+ map/PDBs) in out/harness/bisect/exe/snap_<sha12 of exe+DLLs>/, made
    under the run lock (no build can relink or restage them mid-copy); the name is the hash of the COPIED bytes, so an
    existing folder of that name is reused as is. Its runs log to that folder's logs/ and captures/ (own run numbers).
    Returns the copy's exe."""
    src = os.path.dirname(exe)
    dst = os.path.join(OUT, 'exe', 'snap_' + snap_hash(src))
    if os.path.exists(os.path.join(dst, 'wwe2k14.exe')):
        return os.path.join(dst, 'wwe2k14.exe')
    cmd = ['pwsh', '-NoProfile', '-File', RUNLOCK_PS1, 'run', '-Name', 'bisect exe snapshot', '--',
           sys.executable, os.path.abspath(__file__), '_copy', src, os.path.join(OUT, 'exe')]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
    m = re.search(r'^SNAPSHOT (.+)$', r.stdout or '', re.M)
    if r.returncode or not m or not os.path.exists(os.path.join(m.group(1).strip(), 'wwe2k14.exe')):
        raise SystemExit('exe snapshot failed (exit %s): %s' % (r.returncode, ((r.stdout or '') + (r.stderr or '')).strip()[-400:]))
    return os.path.join(m.group(1).strip(), 'wwe2k14.exe')


def copy_snapshot(src, parent):
    """Run under the lock by snapshot_exe: copy into a temp folder, name it after the hash of the copied files."""
    tmp = os.path.join(parent, 'tmp_%d' % os.getpid())
    os.makedirs(tmp, exist_ok=True)
    for name in SNAP_FILES:
        p = os.path.join(src, name)
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(tmp, name))
    dst = os.path.join(parent, 'snap_' + snap_hash(tmp))
    if os.path.exists(os.path.join(dst, 'wwe2k14.exe')):
        shutil.rmtree(tmp)
    else:
        os.replace(tmp, dst)
    print('SNAPSHOT %s' % dst)
    return 0


# ------------------------------------------------------------------------------------------------------- main
def main():
    if len(sys.argv) == 4 and sys.argv[1] == '_copy':
        return copy_snapshot(sys.argv[2], sys.argv[3])
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='mode', required=True)
    sub.add_parser('presets', help='list the presets --preset can expand')
    for name in ('flags', 'commits'):
        p = sub.add_parser(name)
        p.add_argument('--route', required=True, help='input script (a name in tools\\input_scripts or a path)')
        p.add_argument('--until', help='stop the route after "screenshot LABEL" (+60 frames, quit): shorter probes')
        p.add_argument('--check', required=True, help='crash | log:REGEX | shot:LABEL | perf:SCENE.METRIC<X | perf:bar60')
        p.add_argument('--base', default='', help='flags every probe gets, ONE string: --base="--a=1 --b=2"')
        p.add_argument('--shot-mad', type=float, default=SHOT_MAD, help='shot: changed if MAD > this (%(default)s)')
        p.add_argument('--shot-off', type=float, default=SHOT_OFF,
                       help='shot: changed if %% of pixels off by > 32 > this (%(default)s)')
        p.add_argument('--exe', default=DEFAULT_EXE, help='flags mode: the exe to run (default the main build); '
                                                           'commits --repo sdk: the exe folder copied for the DLL swap')
        p.add_argument('--wait', type=int, default=720, help='run_list -WaitSeconds (kill after; default 720)')
        p.add_argument('--hold-lock', action='store_true', help='hold the run lock for the whole bisect')
        p.add_argument('--cached-only', action='store_true',
                       help='launch nothing: judge only probes an earlier call already ran (e.g. other thresholds); '
                            'stops at the first probe that would need a run')
        p.add_argument('--no-reuse', action='store_true',
                       help='launch every probe (default: reuse a run an earlier call made with the same exe, DLLs, '
                            'route bytes and flags; out/harness/bisect/probe_cache.json)')
        p.add_argument('--dry-run', action='store_true', help='print the plan, launch nothing')
        p.add_argument('--no-snapshot', action='store_true',
                       help='flags mode: run --exe in place (default: a copy of its folder taken under the run lock, '
                            'so another agent\'s build cannot change the exe between probes)')
        if name == 'flags':
            p.add_argument('--flags', default='', help='candidate flags, ONE string: --flags="--a=1 --b=2"')
            p.add_argument('--preset', help='expand a preset into candidates (see "presets")')
            p.add_argument('--no-preset-base', action='store_true', help='do not add the preset\'s base flag')
            p.add_argument('--strategy', choices=('split', 'each'), default='split')
            p.add_argument('--aa', action='store_true', help='run the reference twice first (noise check)')
            p.add_argument('--minimize', action='store_true',
                           help='shrink each INTERACTION to a 1-minimal set (one probe per flag in it)')
        else:
            p.add_argument('--repo', choices=('ours', 'sdk'), required=True,
                           help='ours = this repo; sdk = thirdparty\\rexglue-sdk-src (wwe2k14-local)')
            p.add_argument('--good', required=True, help='old commit (the reference outcome)')
            p.add_argument('--bad', required=True, help='new commit (must differ from --good)')
            p.add_argument('--aa', action='store_true', help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.mode == 'presets':
        for n, pr in parse_presets().items():
            print('%-8s base %-32s %s' % (n, pr['base'] or '(none)', ' '.join(pr['flags'])))
            print('         from %s' % pr['source'])
        return 0

    if args.mode == 'commits' and args.cached_only:
        raise SystemExit('--cached-only is for flags mode (commits mode must build each commit)')
    check = Check(args.check, args.shot_mad, args.shot_off)
    route = resolve_route(args.route)
    if args.until:
        route = truncate_route(route, args.until, os.path.join(OUT, 'routes'))
    if args.hold_lock and not args.dry_run and not args.cached_only:
        sys.argv[0] = os.path.abspath(sys.argv[0])
        from runlock import ensure_lock
        ensure_lock('bisect %s %s' % (args.mode, args.check))
    args.exe = os.path.abspath(args.exe)
    if args.mode == 'flags' and not args.no_snapshot and not args.dry_run:
        if args.cached_only:   # no copy (it would wait for the lock): the snapshot of this build must exist
            snap = os.path.join(OUT, 'exe', 'snap_' + snap_hash(os.path.dirname(args.exe)), 'wwe2k14.exe')
            if not os.path.exists(snap):
                raise SystemExit('--cached-only: no snapshot of %s yet (pass --exe <snapshot exe> --no-snapshot)'
                                 % args.exe)
            args.exe = snap
        else:
            args.exe = snapshot_exe(args.exe)
        print('exe snapshot: %s' % args.exe, flush=True)

    stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S') + '_%d' % os.getpid()
    outdir = os.path.join(OUT, stamp)
    os.makedirs(outdir, exist_ok=True)
    flog = open(os.path.join(outdir, 'bisect.log'), 'w', encoding='utf-8')

    def log(s=''):
        flog.write(s + '\n')
        flog.flush()

    log('bisect %s  argv %s' % (stamp, sys.argv[1:]))
    log('route %s' % route)
    log('check %s (shot thresholds: MAD > %s or off>32 > %s%%)' % (args.check, args.shot_mad, args.shot_off))
    print('route %s, check %s, results %s' % (os.path.basename(route), args.check, outdir), flush=True)
    prober = Prober(args, route, check, log, dry_run=args.dry_run)
    prober.outdir = outdir
    t0 = datetime.datetime.now()
    result = {}
    try:
        if args.mode == 'flags':
            bisect_flags(args, prober, log, result)
        else:
            bisect_commits(args, prober, log, result)
    except ProbeError as e:
        result['error'] = str(e)
    result.update(route=route, check=args.check, shot_mad=args.shot_mad, shot_off=args.shot_off,
                  launched=prober.n_launched, minutes=round((datetime.datetime.now() - t0).total_seconds() / 60, 1),
                  probes=[{k: v for k, v in p.items() if k != 'lines'} for p in prober.history])
    for p in result['probes']:
        p.pop('flags_seen', None)

    # Verdict: one line per probe (flags on top of the base / commit), then the answer.
    lines = []
    if not args.dry_run:
        base_set = set(result.get('base') or [])
        lines.append('probes (outcome = %s; "changed" = differs from the reference):' % args.check)
        for p in result['probes']:
            what = p.get('commit', '')[:9] or ' '.join(f for f in p['flags'] if f not in base_set) or '(base)'
            lines.append('  run %-4s %-5s %-24s %s | %s' % (
                p['run'] or '---', {True: 'YES', False: 'no', None: 'ERROR'}[p['value']], p['tag'][:24], what[:90],
                (p['detail'] or '')[:110]))
    if result.get('noops'):
        lines.append('NOTE: no-op candidates: %s' % '; '.join(result['noops']))
    if result.get('error'):
        lines.append('BISECT INCONCLUSIVE: %s' % result['error'])
    if args.mode == 'flags' and not args.dry_run and 'found' in result:
        if result['found']:
            lines.append('BISECT: changes the outcome on its own: %s' % ' '.join(result['found']))
        if result['interactions']:
            lines.append('BISECT: only in combination (neither half alone): %s' %
                         ' | '.join(' '.join(c) for c in result['interactions']))
        rest = result.get('rest')
        if rest:
            lines.append('BISECT: the other %d flag(s) together (%s): %s' % (
                len(rest['flags']), short(rest['flags']), 'ALSO change it' if rest['changed'] else 'no change'))
        if not result['found'] and not result['interactions'] and not result.get('error'):
            lines.append('BISECT: nothing found')
    if args.mode == 'commits' and result.get('first_changed'):
        lines.append('BISECT: first commit that changes the outcome: %s' % result['first_changed_subject'])
    lines.append('%d game runs, %.1f min; details %s' % (prober.n_launched, result['minutes'], outdir))
    for x in lines:
        print(x)
        log(x)
    with open(os.path.join(outdir, 'result.json'), 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=1, default=str)
    flog.close()
    if result.get('error'):
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
