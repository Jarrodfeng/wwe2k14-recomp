"""S106 C2: the check runner. A step's check is a RECIPE (tools/check/recipes/<ROW>.toml): a BEFORE arm that must show the fault, an AFTER arm,
preconditions refused before any run, one verdict, one sheet. Design: docs/research/s106/C2.md.

  py -I tools\\check\\check.py lint [RECIPE|all]
  py -I tools\\check\\check.py preflight RECIPE [--arm before|after] [--exe PATH --commit SHA]
  py -I tools\\check\\check.py run RECIPE --arm before|after --exe PATH --commit SHA   [--dry: print the launch.ps1 per arm, write nothing to the log]
  py -I tools\\check\\check.py verdict RECIPE        compare both arms: verdict.txt + sheet.jpg
  py -I tools\\check\\check.py eyes RECIPE --arm before|after --answer yes|no
  py -I tools\\check\\check.py list
RECIPE = a row id (tools\\check\\recipes\\<ROW>.toml) or a path. Options: --session s106  --main DIR  --runs-log FILE  --bench DIR (test hooks).
Exit: 0 ok / PASS / FLAG; 1 lint error / FAIL; 2 precondition unreadable or mirror missing (refused, not run); 3 refused (a precondition is false,
exe stamp wrong, alone unsupported, a passing AFTER or a faulting BEFORE already stands, the AFTER's recipe / route / flags / mods differ from BEFORE's, a real
run without --exe / --commit or with a test hook, a run_list ABORT); 4 any other verdict (VOID, BEFORE-BLIND, EYES, PENDING). Refusals write nothing
to check_runs.log: that file holds runs only (line: `YYYY-MM-DD HH:MM:SS row=ID arm=BEFORE|AFTER run=<folder:NNN|pending> verdict=... question=...`).
0 game runs here: `run` (without --dry) is what the main session runs.
"""
# entry: main
import argparse
import contextlib
import datetime
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import recipe as R  # noqa: E402
import readout as RO  # noqa: E402

DEFAULT_MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'


def find_main(tree=TREE):
    try:
        p = subprocess.run(['git', '-C', tree, 'rev-parse', '--git-common-dir'], capture_output=True, text=True, timeout=30)
        c = p.stdout.strip()
        if p.returncode == 0 and c:
            c = os.path.normpath(os.path.join(tree, c))
            return os.path.dirname(c)
    except Exception:  # noqa: BLE001
        pass
    return os.environ.get('RDR_MAIN_CHECKOUT', DEFAULT_MAIN)


class Ctx:
    def __init__(self, a):
        self.tree = TREE
        self.main = a.main or find_main()
        self.session = a.session
        self.bench = a.bench or os.path.join(self.main, 'out', 'bench')
        self.sdir = os.path.join(self.main, 'out', self.session)
        self.runs_log = a.runs_log or os.path.join(self.sdir, 'check_runs.log')

    def arm_dir(self, row, arm):
        return os.path.join(self.sdir, 'check', row, arm)

    def row_dir(self, row):
        return os.path.join(self.sdir, 'check', row)


def recipe_path(name):
    if os.path.isfile(name):
        return name
    return os.path.join(HERE, 'recipes', name + '.toml')


# ---------------------------------------------------------------- the log line
def clean_q(q):
    q = re.sub(r'[\r\n]+', ' ', q)
    return q.replace(' run=', ' run:').replace(' verdict=', ' verdict:').strip()


def runs_for(path, row):
    """Lines of a check_runs.log for ROW: the row= token is matched exactly (row=A1 is not row=A12), `pending` lines are not a check."""
    out = []
    if not os.path.isfile(path):
        return out
    for l in open(path, encoding='utf-8').read().splitlines():
        m = re.search(r'\brow=(\S+)', l)
        v = re.search(r'\bverdict=(\S+)', l)
        if m and m.group(1) == row and v and v.group(1) not in ('pending', 'REFUSED'):     # S106 C2F F13: a REFUSED line (run_list ABORT) is not a run
            out.append(l)
    return out


def log_line(ctx, row, arm, run, verdict, question):
    os.makedirs(os.path.dirname(ctx.runs_log), exist_ok=True)
    line = '%s row=%s arm=%s run=%s verdict=%s question=%s' % (
        datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'), row, arm.upper(), run, verdict, clean_q(question))
    with open(ctx.runs_log, 'a', encoding='utf-8', newline='') as f:
        f.write(line + '\r\n')
    return line


# ---------------------------------------------------------------- exe checks
def exe_info(exe):
    """-> (dict(commit, dirty, preset, options), why). exe_info.json (C4 / G2L) first, else the stamp header (exe_vs_head.exe_stamp)."""
    d = os.path.dirname(exe)
    if not os.path.isfile(exe):
        return None, '%s does not exist' % exe
    j = os.path.join(d, 'exe_info.json')
    if os.path.isfile(j):
        try:
            with open(j, encoding='utf-8') as f:
                x = json.load(f)
            return dict(commit=str(x.get('commit', '')), dirty=bool(x.get('dirty', False)), preset=x.get('preset') or os.path.basename(d),
                        options=x.get('options') or {}, source='exe_info.json'), None
        except (OSError, ValueError) as e:
            return None, 'exe_info.json unreadable (%s)' % e
    sys.path.insert(0, os.path.join(TREE, 'tools', 'bench'))
    try:
        import exe_vs_head as EV
        c, dirty, why = EV.exe_stamp(exe)
    except Exception as e:  # noqa: BLE001
        return None, 'no exe_info.json and the stamp reader failed (%s)' % e
    if why:
        return None, why
    return dict(commit=c, dirty=bool(dirty), preset=os.path.basename(d), options=cmake_options(d), source='stamp header + CMakeCache.txt'), None


def cmake_options(build_dir):
    """S106 C2F B3b: a stamp-only exe (out\\build\\...) has no exe_info.json: its WWE2K14_* options come from the CMakeCache.txt beside it. -> dict (empty = unknown)."""
    p = os.path.join(build_dir, 'CMakeCache.txt')
    out = {}
    if os.path.isfile(p):
        with open(p, encoding='utf-8', errors='replace') as f:
            for l in f:
                m = re.match(r'(WWE2K14_[A-Z0-9_]+):[A-Z]+=(.*?)\s*$', l)
                if m:
                    out[m.group(1)] = m.group(2)
    return out


def exe_refusal(rec, exe, commit, other=None):
    info, why = exe_info(exe)
    if why:
        return 'exe %s: %s' % (exe, why), None
    if not commit:
        return 'no --commit: the arm must name the commit its exe was built at', info
    if not (info['commit'].startswith(commit) or commit.startswith(info['commit'])) or not info['commit']:
        return 'exe stamp commit %s is not the arm\'s commit %s' % (info['commit'], commit), info
    if info['dirty']:
        return 'exe %s was built dirty (its uncommitted edits cannot be seen)' % exe, info
    pre = rec['recipe']['preset']
    if info['preset'] != pre:
        return 'exe preset %s is not the recipe\'s %s' % (info['preset'], pre), info
    if other is not None:      # S106 C2F B3b: both arms' options must be KNOWN and equal (an unknown side is not "equal")
        if not info['options'] or not other.get('options'):
            return 'the build options of %s are unknown (no exe_info.json options, no CMakeCache.txt beside the exe): cannot show both arms were built alike' % (
                'this arm' if not info['options'] else 'the other arm'), info
        if other['options'] != info['options']:
            return 'exe_info options differ from the other arm\'s (%s)' % ', '.join(
                sorted(k for k in set(info['options']) | set(other['options']) if info['options'].get(k) != other['options'].get(k))), info
    return None, info


# ---------------------------------------------------------------- the mirror
def mirror_path(ctx, rec):
    d = rec['destination'].get('data', 'userdata_frozen')
    name = 'userdata_chk_' + rec['recipe']['row'] if d == 'plant' else d
    return name, os.path.join(ctx.bench, name)


def sha_of_save(mirror):
    for root, _, files in os.walk(mirror):
        for f in files:
            if f.lower() == 'savedata.dat':
                with open(os.path.join(root, f), 'rb') as fh:
                    return hashlib.sha256(fh.read()).hexdigest()
    return None


def plant_cmd(ctx, rec):
    pl = rec['plant']
    name, dst = mirror_path(ctx, rec)
    c = [sys.executable, '-I', os.path.join(TREE, 'tools', 'save', 'make_test_save.py'), '--src', os.path.join(ctx.main, 'out', 'bench', 'userdata_frozen'),
         '--dst', dst, '--threads', str(pl.get('threads', 'none'))]
    for k, fl in (('mask', '--mask'), ('caption', '--caption'), ('blood', '--blood'), ('palette', '--palette'), ('holder', '--holder')):
        if k in pl:
            c += [fl, str(pl[k])]
    return c + ['--force']


def q_save_check(ctx, exprs, mirror):
    sys.path.insert(0, os.path.join(TREE, 'tools', 'q'))
    import qsave
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = qsave.main(['check'] + list(exprs) + ['--data', mirror])
    return code, buf.getvalue()


# ---------------------------------------------------------------- preflight
def preflight(ctx, rec, arm, exe=None, commit=None, other=None, real=False, make_mirror=True):
    """-> (code, [messages], info). 0 ok. Writes nothing to check_runs.log."""
    msgs = []
    err, warn = R.lint(rec, TREE)
    msgs += ['WARN ' + w for w in warn]
    if err:
        return 1, ['LINT ' + e for e in err] + msgs, None
    row = rec['recipe']['row']
    if rec.get('run', {}).get('alone'):
        rl = os.path.join(ctx.main, 'tools', 'bench', 'run_list.ps1')
        txt = open(rl, encoding='utf-8', errors='replace').read() if os.path.isfile(rl) else ''
        if not re.search(r'\[switch\]\s*\$Alone\b', txt):
            return 3, ['REFUSED: alone = true but run_list has no -Alone yet (alone not supported yet; lane C5)'], None
    name, mirror = mirror_path(ctx, rec)
    would_plant = False
    if rec['destination'].get('data') == 'plant' and not os.path.isdir(mirror):
        if not make_mirror:      # S106 C2F F5: preflight / --dry write nothing: they say what a real run would plant
            would_plant = True
            msgs.append('would plant %s: %s (a real run plants it first; the save preconditions are read after that)' % (mirror, ' '.join(plant_cmd(ctx, rec)[3:])))
        else:
            if not os.path.isdir(os.path.join(ctx.main, 'out', 'bench', 'userdata_frozen')):
                return 2, ['REFUSED: no frozen mirror %s to plant from' % os.path.join(ctx.main, 'out', 'bench', 'userdata_frozen')], None
            p = subprocess.run(plant_cmd(ctx, rec), capture_output=True, text=True, timeout=300)
            if p.returncode:
                return 2, ['REFUSED: make_test_save failed: ' + (p.stdout + p.stderr).strip()[-300:]], None
    if not would_plant:
        if not os.path.isdir(mirror):
            return 2, ['REFUSED: no user-data mirror %s' % mirror], None
        sha = sha_of_save(mirror)
        if not sha:      # S106 C2F B3c: no SaveData.Dat = the "same mirror for both arms" check could not run
            return 3, ['REFUSED: no SaveData.Dat in the mirror %s (the mirror-hash check between the arms cannot run)' % mirror], None
        stored = os.path.join(ctx.row_dir(row), 'mirror.sha256')
        if os.path.isfile(stored):
            old = open(stored).read().strip()
            if old != sha:
                return 3, ['REFUSED: the mirror changed since the other arm ran (sha %s vs %s)' % (sha[:12], old[:12])], None
        exprs = R.all_pre_exprs(rec)
        if exprs:
            code, out = q_save_check(ctx, exprs, mirror)
            msgs += ['  ' + l for l in out.strip().splitlines()]
            if code == 2:
                return 2, ['REFUSED: a precondition could not be read (q save check exit 2)'] + msgs, None
            if code != 0:
                hint = ' (plant it: [plant] caption / mask / holder)' if 'plant' not in rec else ''
                return 3, ['REFUSED: a save precondition is false (q save check exit %d)%s' % (code, hint)] + msgs, None
    for p in rec.get('pre', []):
        if 'file' in p:
            cands = [os.path.join(ctx.main, p['file'].replace('\\', os.sep)), os.path.join(TREE, p['file'].replace('\\', os.sep))]
            if not any(os.path.exists(c) for c in cands):
                return 3, ['REFUSED: required file / folder %s does not exist' % p['file']], None
    ch = rec['destination'].get('champion')
    if ch:
        ids, src = R.participants(rec, ctx.main, TREE)
        if ids is None:
            return 3, ['REFUSED: champion set but cannot see the participants (%s)' % src], None
        if ch['char'] not in ids:
            return 3, ['REFUSED: champion %d is not in the destination\'s participants %s (%s)' % (ch['char'], sorted(ids), src)], None
    info = None
    if exe:
        why, info = exe_refusal(rec, exe, commit, other)
        if why:
            return 3, ['REFUSED: ' + why], info
    if real:
        rl = os.path.join(ctx.main, 'tools', 'bench', 'run_list.ps1')
        txt = open(rl, encoding='utf-8', errors='replace').read() if os.path.isfile(rl) else ''
        if not re.search(r'\[string\]\s*\$ForCommit\b', txt):
            return 3, ['REFUSED: run_list.ps1 has no -ForCommit parameter yet (lane G2L): a real run would not check the exe stamp'], info
    return 0, msgs, info


# ---------------------------------------------------------------- the launch
def build_flags(ctx, rec, arm):
    d = rec['destination']
    ad = ctx.arm_dir(rec['recipe']['row'], arm)
    f = [d.get('flags', '').strip(), '--wwe2k14_settings_file=%s' % os.path.join(ad, 'settings.json')]
    if d.get('settings', 'blank') == 'new_player':
        f.append('--wwe2k14_use_launcher_settings=true')
    f += ['--wwe2k14_replay_dir=%s' % os.path.join(ctx.main, 'out', 'replays'),
          '--wwe2k14_scenario_dir=%s' % os.path.join(ctx.main, 'out', 'scenarios'),
          '--game_data_root=%s' % os.path.join(ctx.main, 'game_data', 'extracted')]
    if d.get('mods'):
        f.append('--wwe2k14_mods_dir=%s' % os.path.join(ad, 'mods'))
    return ' '.join(x for x in f if x)


def launch_text(ctx, rec, arm, exe, commit):
    d = rec['destination']
    name, _ = mirror_path(ctx, rec)
    q = lambda s: str(s).replace("'", "''")  # noqa: E731
    route = os.path.join(TREE, d['route'].replace('\\', os.sep))
    return ("# check.py: row %s arm %s (generated; one run_list call, in-process)\n"
            "& '%s' -Scripts @('%s') -Extra '%s' -Data '%s' -Exe '%s' -ForCommit '%s' -WaitSeconds %d%s\n"
            % (rec['recipe']['row'], arm.upper(), q(os.path.join(ctx.main, 'tools', 'bench', 'run_list.ps1')), q(route), q(build_flags(ctx, rec, arm)),
               q(name), q(exe or '<exe>'), q(commit or '<commit>'), int(d.get('timeout_s', 600)),
               ' -Alone' if rec.get('run', {}).get('alone') else ''))      # S106 C2F F1: alone = true reaches run_list (preflight refuses a run_list without it)


def tree_sha(path):
    """sha256 over a folder's relative paths and bytes (sorted); None for a missing folder."""
    if not os.path.isdir(path):
        return None
    h = hashlib.sha256()
    for root, dirs, files in os.walk(path):
        dirs.sort()
        for fn in sorted(files):
            fp = os.path.join(root, fn)
            h.update(os.path.relpath(fp, path).replace('\\', '/').encode() + b'\0')
            with open(fp, 'rb') as f:
                h.update(f.read())
    return h.hexdigest()


def file_sha(path):
    try:
        with open(path, 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return None


def inputs_hash(ctx, rec, arm):
    """S106 C2F B3a: what an arm RAN against, besides the exe: the recipe file, the route, the final -Extra string (the arm folder masked) with -Alone, the mods tree.
    Both arms must store the same dict; an AFTER whose dict differs from BEFORE's is refused."""
    d = rec['destination']
    flags = build_flags(ctx, rec, 'ARM') + (' -Alone' if rec.get('run', {}).get('alone') else '')
    mods = d.get('mods')
    return dict(recipe=file_sha(rec['_path']), route=file_sha(os.path.join(TREE, d['route'].replace('\\', os.sep))),
                flags=hashlib.sha256(flags.encode()).hexdigest(),
                mods=tree_sha(os.path.join(ctx.main, mods.replace('\\', os.sep))) if mods else None)


def inputs_diff(a, b):
    return sorted(k for k in set(a or {}) | set(b or {}) if (a or {}).get(k) != (b or {}).get(k))


def resolve_commit(tree, c):
    """S106 C2F F6: --commit is 7-40 hex and a real commit of this repo. -> (full sha, None) or (None, why)."""
    if not c or not re.fullmatch(r'[0-9a-fA-F]{7,40}', c):
        return None, '--commit %r must be 7 to 40 hex digits (a commit, never a name)' % c
    p = subprocess.run(['git', '-C', tree, 'rev-parse', '--verify', '--quiet', c + '^{commit}'], capture_output=True, text=True, timeout=30)
    full = p.stdout.strip()
    if p.returncode or not full:
        return None, '--commit %s is not a commit of this repository' % c
    return full, None


def run_cmd(cmd, tmo):
    """S106 C2F F3: run a command; on the timeout kill the whole process tree (the game is a child of run_list's pwsh). -> (output, timed_out, return code)."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    try:
        out, _ = p.communicate(timeout=tmo)
        return out, False, p.returncode
    except subprocess.TimeoutExpired:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], capture_output=True, timeout=60)
        try:
            out, _ = p.communicate(timeout=30)
        except Exception:  # noqa: BLE001
            out = ''
        return (out or '') + '\nTIMEOUT after %ss (process tree killed)' % tmo, True, None


def parse_run_list(text):
    """-> (log_name or None, void or None). void: 'abort' (run_list refused: not a run), 'tool', 'route'."""
    m = re.search(r'->\s+(wwe2k14_\d+)\b', text)
    log = m.group(1) if m else None
    if re.search(r'(^|ABORT:\s*)alone: (broken|NO|unverified)\b', text, re.M):    # S106 C5F B1: the PC was not alone (or not proven alone): readout not judged -> BLOCKED-ALONE
        return log, 'alone'
    if re.search(r'^ABORT:|->\s+ABORT:|\bABORT:', text):
        return log, 'abort'
    if re.search(r'script FAILED|KILLED|STILL RUNNING', text):
        return log, 'route'
    if re.search(r'\]\[crash\]|device lost', text):
        return log, 'tool'
    if not log:
        return None, 'tool'
    if not re.search(r'EXITED after [\d.,]+s, exit code 0\b', text):    # S106 C2F B1: the rule of run_pass.ps1: only a clean exit 0 is a finished run
        return log, 'route'
    return log, None


def run_arm(ctx, rec, arm, exe, commit, other):
    row = rec['recipe']['row']
    q = rec['recipe']['question']
    ad = ctx.arm_dir(row, arm)
    os.makedirs(ad, exist_ok=True)
    name, mirror = mirror_path(ctx, rec)
    sha = sha_of_save(mirror)
    if sha:
        os.makedirs(ctx.row_dir(row), exist_ok=True)
        with open(os.path.join(ctx.row_dir(row), 'mirror.sha256'), 'w') as f:
            f.write(sha)
    with open(os.path.join(ad, 'settings.json'), 'w') as f:
        f.write('{}')
    mods = rec['destination'].get('mods')
    if mods:
        dst = os.path.join(ad, 'mods')
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        shutil.copytree(os.path.join(ctx.main, mods.replace('\\', os.sep)), dst)
    with open(os.path.join(ad, 'launch.ps1'), 'w', encoding='utf-8') as f:
        f.write(launch_text(ctx, rec, arm, exe, commit))
    log_line(ctx, row, arm, 'pending', 'pending', q)
    tmo = int(rec['destination'].get('timeout_s', 600)) + 600
    void = None
    out, timed_out, _ = run_cmd(['pwsh', '-NoProfile', '-File', os.path.join(ad, 'launch.ps1')], tmo)     # S106 C2F F3: a timeout kills the tree (the game too)
    with open(os.path.join(ad, 'run_list.txt'), 'w', encoding='utf-8') as f:
        f.write(out)
    log, v2 = parse_run_list(out)
    void = 'tool' if timed_out else v2
    leaf = os.path.basename(os.path.dirname(exe))
    run = '%s:%s' % (leaf, re.sub(r'\D', '', log or '') or 'none')
    ev, new, logp, err = dict(guards=[], readouts=[], shots=[]), 0, None, None
    if log and not void:
        logp = os.path.join(os.path.dirname(exe), 'logs', log + '.log')
        if not os.path.isfile(logp):
            void = 'tool'
        else:
            try:
                ev = RO.evaluate(rec, RO.read_log(logp))
            except Exception as e:  # noqa: BLE001      S106 C2F F10: nothing after the pending line may raise: it becomes a VOID with the error kept
                void, err = 'tool', 'readout evaluation raised %s: %s' % (type(e).__name__, e)
            if not void:
                tj = os.path.join(ad, 'triage.json')
                cmd = [sys.executable, '-I', os.path.join(TREE, 'tools', 'report', 'triage.py'), logp, '--json', '--out', tj]
                bj = os.path.join(ctx.arm_dir(row, 'before'), 'triage.json')
                if arm == 'after' and os.path.isfile(bj):
                    cmd += ['--baseline', bj]
                tout, t_to, trc = run_cmd(cmd, 600)
                if t_to:
                    void, err = 'tool', 'triage timed out'
                elif trc == 2:
                    void, err = 'tool', 'triage could not read the log (exit 2)'
                elif os.path.isfile(tj):
                    try:
                        new = sum(len(lg.get('findings', [])) for lg in json.load(open(tj, encoding='utf-8')).get('logs', []))
                    except (OSError, ValueError) as e:
                        void, err = 'tool', 'triage.json unreadable (%s)' % e
    load = re.findall(r'\[load\][^\n]*', out)
    verdict = RO.arm_verdict(rec, arm, ev, None, new, void)
    res = dict(row=row, arm=arm, run=run, verdict=verdict, commit=commit, exe=exe, exe_info=exe_info(exe)[0], ev=ev, tripwire_new=new, void=void,
               answer=None, log=logp, load=load[:2], mirror_sha=sha, inputs=inputs_hash(ctx, rec, arm), error=err)
    save_result(ctx, row, arm, res)
    log_line(ctx, row, arm, run, verdict, q)      # S106 C2F F13: verdict REFUSED when run_list ABORTed (the board skips it as a run)
    return res


def save_result(ctx, row, arm, res):
    os.makedirs(ctx.arm_dir(row, arm), exist_ok=True)
    with open(os.path.join(ctx.arm_dir(row, arm), 'result.json'), 'w', encoding='utf-8') as f:
        json.dump(res, f, indent=1)


def load_result(ctx, row, arm):
    p = os.path.join(ctx.arm_dir(row, arm), 'result.json')
    if os.path.isfile(p):
        with open(p, encoding='utf-8') as f:
            return json.load(f)
    return None


# ---------------------------------------------------------------- commands
def cmd_lint(ctx, a):
    names = [os.path.splitext(f)[0] for f in sorted(os.listdir(os.path.join(HERE, 'recipes'))) if f.endswith('.toml')] \
        if a.recipe in (None, 'all') else [a.recipe]
    bad = 0
    for n in names:
        try:
            rec = R.load(recipe_path(n))
            err, warn = R.lint(rec, TREE)
        except Exception as e:  # noqa: BLE001
            err, warn = ['unreadable: %s' % e], []
        for w in warn:
            print('WARN  %s: %s' % (n, w))
        for e in err:
            print('ERROR %s: %s' % (n, e))
        print('%s %s' % ('FAIL' if err else 'ok  ', n))
        bad += bool(err)
    return 1 if bad else 0


def cmd_preflight(ctx, a):
    rec = R.load(recipe_path(a.recipe))
    code, msgs, _ = preflight(ctx, rec, a.arm or 'before', a.exe, a.commit, make_mirror=False)      # S106 C2F F5: preflight plants nothing
    print('\n'.join(msgs) or 'preflight ok')
    return code


def cmd_run(ctx, a):
    rec = R.load(recipe_path(a.recipe))
    row = rec['recipe']['row']
    arms = [a.arm] if a.arm else ['before', 'after']
    if not a.dry:
        if not a.arm:
            print('run needs --arm before|after')
            return 3
        if not a.exe or not a.commit:      # S106 C2F F2: a real run names its exe and its commit (no exe = no stamp check, then a crash)
            print('REFUSED: a real run needs both --exe and --commit (the exe stamp is checked against the commit)')
            return 3
    exe = a.exe
    if exe and os.path.isdir(exe):
        exe = os.path.join(exe, 'wwe2k14.exe')
    commit = a.commit
    if commit or not a.dry:
        commit, why = resolve_commit(TREE, commit)      # S106 C2F F6: full sha, a real commit, 7+ hex
        if why:
            print('REFUSED: ' + why)
            return 3
    for arm in arms:
        other = None
        o = load_result(ctx, row, 'before' if arm == 'after' else 'after')
        if o:
            other = o.get('exe_info')
        code, msgs, info = preflight(ctx, rec, arm, exe, commit if exe else None, other, real=not a.dry, make_mirror=not a.dry)
        print('\n'.join(msgs))
        if code:
            return code
        if a.dry:
            print('--- dry: %s arm %s would run launch.ps1:' % (row, arm.upper()))
            print(launch_text(ctx, rec, arm, exe, commit))
            continue
        cur = inputs_hash(ctx, rec, arm)
        same_commit = lambda r: bool(r and r.get('commit')) and (commit.startswith(r['commit']) or r['commit'].startswith(commit))  # noqa: E731  full shas compared
        if arm == 'before':
            o = load_result(ctx, row, 'before')
            if o and o['verdict'] == 'FAULT' and same_commit(o) and not inputs_diff(o.get('inputs'), cur):
                print('REFUSED: a BEFORE that shows the fault already stands for commit %s and this recipe / route / flags (never re-run)' % commit[:12])
                return 3
        if arm == 'after':
            b = load_result(ctx, row, 'before')
            if not b or b['verdict'] != 'FAULT':
                print('REFUSED: AFTER waits for a BEFORE that shows the fault (BEFORE verdict: %s)' % (b['verdict'] if b else 'none'))
                return 3
            diff = inputs_diff(b.get('inputs'), cur)      # S106 C2F B3a: both arms are the same except the exe
            if not b.get('inputs') or diff:
                print('REFUSED: this AFTER would run different inputs than the BEFORE (%s differ: the recipe, the route, the final flags or the mods tree changed since BEFORE)'
                      % (', '.join(diff) or 'BEFORE stored no inputs hash'))
                return 3
            old = load_result(ctx, row, 'after')
            if old and old['verdict'] == 'PASS' and same_commit(old):
                print('REFUSED: a passing AFTER for commit %s already stands (never re-run)' % commit[:12])
                return 3
        res = run_arm(ctx, rec, arm, exe, commit, other)
        print('%s %s -> %s (%s)' % (row, arm.upper(), res['verdict'], res['run']))
        return 3 if res['verdict'] == 'REFUSED' else (0 if res['verdict'] in ('FAULT', 'PASS', 'FLAG') else 4)
    return 0


def reeval(rec, arm, res):
    """S106 C2F B3a: a stored result is judged again with the CURRENT recipe from its saved log (when the log is still there), so an edited bar cannot hide in an old verdict."""
    if res and res.get('log') and os.path.isfile(res['log']) and not res.get('void'):
        ev = RO.evaluate(rec, RO.read_log(res['log']))
        res = dict(res, ev=ev, verdict=RO.arm_verdict(rec, arm, ev, res.get('answer'), res.get('tripwire_new', 0), res.get('void')))
    return res


def cmd_verdict(ctx, a):
    rec = R.load(recipe_path(a.recipe))
    row = rec['recipe']['row']
    b, af = reeval(rec, 'before', load_result(ctx, row, 'before')), reeval(rec, 'after', load_result(ctx, row, 'after'))
    v = RO.compare(b and b['verdict'], af and af['verdict'])
    mismatch = inputs_diff(b.get('inputs'), af.get('inputs')) if (b and af and (b.get('inputs') or af.get('inputs'))) else []
    if mismatch:
        v = 'VOID'
    lines = ['row %s: %s' % (row, v)] + (['VOID: the two arms did not run the same inputs (%s differ)' % ', '.join(mismatch)] if mismatch else []) + ['question: %s' % rec['recipe']['question'],
             'BEFORE: %s commit %s  %s' % (b['run'] if b else '-', b['commit'] if b else '-', b['verdict'] if b else '-'),
             'AFTER : %s commit %s  %s' % (af['run'] if af else '-', af['commit'] if af else '-', af['verdict'] if af else '-'),
             '%-44s %-10s %-10s %-10s %s' % ('readout', 'bar', 'BEFORE', 'AFTER', 'met')]
    for it in rec.get('readout', []):
        def val(r):
            return next((x['value'] for x in r['ev']['readouts'] if x['name'] == it['name']), '-') if r else '-'
        bar = it.get('fixed')
        lines.append('%-44s %-10s %-10s %-10s %s' % (it['name'][:44], bar, 'UNREAD' if val(b) is None else val(b),
                                                       'UNREAD' if val(af) is None else val(af),
                                                       next((x['meets_fixed'] for x in af['ev']['readouts'] if x['name'] == it['name']), '-') if af else '-'))
    os.makedirs(ctx.row_dir(row), exist_ok=True)
    with open(os.path.join(ctx.row_dir(row), 'verdict.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    if b and af and not a.no_sheet:
        import sheet
        sheet.make_sheet(os.path.join(ctx.row_dir(row), 'sheet.jpg'), lines, b['ev']['shots'], af['ev']['shots'])
    return {'PASS': 0, 'FLAG': 0, 'FAIL': 1}.get(v, 4)


def cmd_eyes(ctx, a):
    rec = R.load(recipe_path(a.recipe))
    row = rec['recipe']['row']
    res = load_result(ctx, row, a.arm)
    if not res or a.answer not in ('yes', 'no'):
        print('eyes: no result for %s %s (or answer not yes / no)' % (row, a.arm))
        return 3
    res['answer'] = a.answer
    res['verdict'] = RO.arm_verdict(rec, a.arm, res['ev'], a.answer, res.get('tripwire_new', 0), res.get('void'))
    save_result(ctx, row, a.arm, res)
    print(log_line(ctx, row, a.arm, res['run'], res['verdict'], rec['recipe']['question']))
    return 0


def cmd_list(ctx, a):
    for f in sorted(os.listdir(os.path.join(HERE, 'recipes'))):
        if f.endswith('.toml'):
            rec = R.load(os.path.join(HERE, 'recipes', f))
            row = rec['recipe']['row']
            b, af = load_result(ctx, row, 'before'), load_result(ctx, row, 'after')
            print('%-6s before %-12s after %-8s %s' % (row, b['verdict'] if b else '-', af['verdict'] if af else '-', rec['recipe']['question'][:90]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['lint', 'preflight', 'run', 'verdict', 'eyes', 'list'])
    ap.add_argument('recipe', nargs='?')
    ap.add_argument('--arm', choices=['before', 'after'])
    ap.add_argument('--exe')
    ap.add_argument('--commit')
    ap.add_argument('--session', default='s106')
    ap.add_argument('--main')
    ap.add_argument('--runs-log')
    ap.add_argument('--bench')
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--answer')
    ap.add_argument('--no-sheet', action='store_true')
    a = ap.parse_args(argv)
    if a.cmd == 'run' and not a.dry and (a.main or a.runs_log or a.bench):      # S106 C2F F7: the test hooks never steer a real run (a real run reads and writes the real folders)
        print('REFUSED: --main / --runs-log / --bench are test hooks: only with --dry (or the selftest)')
        return 3
    ctx = Ctx(a)
    if a.cmd not in ('lint', 'list') and not a.recipe:
        ap.error('RECIPE is required')
    if a.cmd == 'eyes' and not a.arm:
        ap.error('eyes needs --arm')
    return {'lint': cmd_lint, 'preflight': cmd_preflight, 'run': cmd_run, 'verdict': cmd_verdict, 'eyes': cmd_eyes, 'list': cmd_list}[a.cmd](ctx, a)


if __name__ == '__main__':
    sys.exit(main())
