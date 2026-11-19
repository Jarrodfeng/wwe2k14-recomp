r"""Gate scheduler library (S103 lane PS2): what a check touches, who may run beside whom, and the GPU slots.
Used by tools\gate\gate.py; selftest: tools\gate\sched_selftest.py.

WHY (docs\research\s103\GS_gate_speed.md finding b; measured): fastgate.serial_needed() put 282 of 342 lanes on ONE serial worker (any check without
`affects`, or a .ps1 build), so --jobs did nothing. Whether two checks may run side by side is a question about PATHS, not about "has affects":
two checks conflict only when one WRITES a path the other reads or writes.

FOOTPRINT of a check (footprint()): reads / writes = repo paths (posix, relative to the checkout root, under out\ mostly), gpu = takes a GPU slot,
unknown = the writes could not be enumerated. Sources, strongest first:
  1. the check says so:  "writes": ["out/tools/x.exe", ...]  (and "reads": [...]); "writes": [] = declared pure; "gpu": true|false overrides the inference.
  2. its command line: the argument of -o / --out / --output / --out-dir / -OutFile is a write; every other out\ / game_data\ / generated\ token is a touch.
  3. the scripts it runs (a .py / .ps1 in the command, found by a static scan of the file): literal out\ paths ('out', 'tools', 'x.exe' joins and
     "out/tools/x.exe" strings, a .ps1's quoted *.exe / *_obj names under its $outDir). A script that writes (open(..., 'w'), write_text, mkdir, -o ...)
     makes every path it names a write; a script with no write call only reads them.
  4. nothing derivable and the script writes somewhere unnamed (or is not a file we can read): unknown. An unknown check conflicts with EVERY other check
     (a barrier): never assumed safe. `gate.py --footprints` prints every check's footprint and why, so a lane can declare "writes" for it.
A touched path under the MAIN checkout's out\ (a captures folder, the player's game_data) that the check does not write is a shared read.

PathLocks: a readers-writers table over path prefixes inside one gate run. A check takes all of its paths at once (all or nothing under one condition,
so no deadlock); two paths conflict when one is a prefix of the other and at least one side writes. GpuSlots: N slots as lock files in
<main checkout>\out\gate\gpu_slots, held with an OS byte-range lock (msvcrt.locking / fcntl.flock): a process that dies, however it dies, frees its slot, so
a lock can never go stale, and gates running from different worktrees share the same N slots."""
import json, os, re, sys, threading, time
from pathlib import Path

WRITE_ARGS = ('-o', '--out', '--output', '--out-dir', '--outdir', '--dir', '-outfile', '-out', '-outputpath', '--dump', '--png', '--csv', '--json')
PATH_TOKEN = re.compile(r"""(?<![\w./\\-])((?:[A-Za-z]:)?[\\/]*(?:\.\.?[\\/])*(?:out|game_data|generated)[\\/][^\s'"`;|)(,<>]+)""")
EXE_LIT = re.compile(r"""['"]([\w.+-]+\.(?:exe|pdb|lib|dll|ilk))['"]""")
OBJ_LIT = re.compile(r"""['"]([\w.+-]*_obj)['"]""")
OUT_STR = re.compile(r"""['"]((?:[A-Za-z]:)?[\\/]*(?:\.\.?[\\/])*out(?:[\\/][\w.+*$ {}%-]+)+)['"]""")
OUT_JOIN = re.compile(r"""['"]out['"]\s*(?:,|/)\s*((?:['"][\w.+*-]+['"]\s*(?:,|/)?\s*)+)""")
JOIN_PART = re.compile(r"""['"]([\w.+*-]+)['"]""")
PS_OUTDIR = re.compile(r"""\$outDir\s*=\s*Join-Path\s+\$repo\s+['"]([^'"]+)['"]""", re.I)
WRITE_PY = re.compile(r"open\([^)]*['\"][wa]b?\+?['\"]|write_text|write_bytes|makedirs|mkdir|shutil\.(?:copy|copyfile|copytree|move|rmtree)|os\.(?:replace|remove|rename|unlink)"
                      r"|\.unlink\(|--out\b|'-o'|\"-o\"|savefig|imwrite|\.save\(")
WRITE_PS = re.compile(r"Set-Content|Out-File|New-Item|Remove-Item|Copy-Item|Move-Item|\s-o\s|clang|cl\.exe|link\b|Add-Content|\bcmake\b|\bninja\b|Compress-Archive|Expand-Archive", re.I)
READ_ONLY = ('out/captures', 'out/build', 'out/toolcache', 'out/s101', 'game_data', 'generated', 'thirdparty', 'out/cases_in')
RIG_GPU = re.compile(r"\b(?:frame_rig|post_rig|host_rig|maps_draw|rsc_draw|crowd_live_rig|rf_b_draw|arena_rec|crowd_pair)\.exe\b|\b(?:fview|rplay)\.exe\b|\b(?:fview|rplay)_exe\b|\bfind_fview\b")
# S103 21:48: the bare words `fview` / `rplay` were dropped: any code that only NAMED them (getenv_lint.py's allowlist of src\fview, a
# path filter, a size table) took a GPU slot, and pure lints waited up to 950 s behind rig renders. A launch names the exe, an exe variable
# or the finder; anything else that renders marks its check "gpu": true.
GPU_PLANT_SKIP = ()   # a lane marks a check "gpu": false to opt out of the inference
EXE_SIBLINGS = ('.pdb', '.ilk', '.exp', '.lib', '.exe.deps.txt')   # what a link writes beside <stem>.exe (S103 GST review 1)


def owns(p, writes):
    """True when output path `p` belongs to a build whose write footprint is `writes`: inside / around one of them, or a `<stem>.*` file of a written .exe (S103 GST review 1: 202 of 219
    lane builds missed the tool cache because the link's .pdb / .ilk / .exp counted as foreign)."""
    p = norm(p)
    for w in writes:
        w = norm(w)
        if p == w or p.startswith(w + '/') or w.startswith(p + '/'):
            return True
        if w.lower().endswith('.exe') and (p == w[:-4] or p.startswith(w[:-4] + '.')):
            return True
    return False


def norm(p):
    p = str(p).replace('\\', '/')
    while p.startswith('./'):
        p = p[2:]
    return p.rstrip('/') or p


def rel_to(root, main, p):
    """A path token -> a posix path relative to root (inside root or main: relative to that checkout); None for a path elsewhere."""
    p = str(p).strip().strip('\'"')
    q = norm(p)
    if re.match(r'^[A-Za-z]:/', q) or q.startswith('/'):
        for base, tag in ((Path(root), ''), (Path(main), '@main/')):
            b = norm(str(base)) + '/'
            if q.lower().startswith(b.lower()):
                return tag + q[len(b):]
        return None
    while q.startswith('../'):
        q = q[3:]
    return q


def _read(root, a):
    p = Path(root) / a
    try:
        if a.lower().endswith(('.py', '.ps1')) and p.is_file():
            return p.read_text(encoding='utf-8', errors='replace')
    except OSError:
        pass
    return None


def _globish(p):
    """A path with a wildcard or variable part: keep the fixed prefix (a prefix lock covers everything under it)."""
    p = re.split(r'[*$%{]', p)[0]
    return norm(p.rsplit('/', 1)[0] if p and not p.endswith('/') and re.search(r'[*$%{]', p) else p)


_SP_CACHE = {}


def script_paths(text, kind, cmd=(), top=True, no_sub=False):
    """Memoised front of _script_paths: hundreds of checks share the same modules."""
    k = (text, kind, tuple(cmd) if top else (), top, no_sub)
    r = _SP_CACHE.get(k)
    if r is None:
        t, w, ro = _script_paths(text, kind, cmd, top, no_sub)
        r = _SP_CACHE[k] = (frozenset(t), w, frozenset(ro))
    return set(r[0]), r[1], set(r[2])


def _script_paths(text, kind, cmd=(), top=True, no_sub=False):
    """-> (touched paths, writes_flag, read-only paths) from one script's text. top=False: a module / child script the command line does not reach
    directly: none of the "this flag is not passed" relaxations apply to it (its arguments come from its caller). no_sub: no file of the closure starts a
    subprocess (only then does `--check` mean read-only)."""
    touched = set()
    savecase = '--save-case' in text   # fview writes a case folder under out/cases (frame_rig.py globs it before / after): a write whatever else the script says
    if savecase:
        touched.add('out/cases')
    for m in OUT_STR.finditer(text):
        touched.add(_globish(norm(m.group(1))))
    for m in OUT_JOIN.finditer(text):
        parts = JOIN_PART.findall(m.group(1))
        touched.add(_globish(norm('out/' + '/'.join(parts))))
    if kind == 'ps1':
        outs = [norm(m.group(1)) for m in PS_OUTDIR.finditer(text)] or ['out/tools']
        named = 0
        for m in EXE_LIT.finditer(text):
            named += 1
            for o in outs:
                touched.add(o + '/' + m.group(1))
                if m.group(1).lower().endswith('.exe'):   # S103 GST review 1: the link also writes <exe>'s .pdb / .ilk / .exp / .lib / .exe.deps.txt: they are this build's, not "outside every footprint"
                    for sx in EXE_SIBLINGS:
                        touched.add(o + '/' + m.group(1)[:-4] + sx)
        for m in OBJ_LIT.finditer(text):
            named += 1
            for o in outs:
                touched.add(o + '/' + m.group(1))
        if named:
            # S103 GST: a build script that names its output exe(s) / object folder writes THOSE, not the whole folder it quotes (out\tools as the $outDir): every rig
            # that reads a different exe under out/tools no longer waits for the build (66 build rows waited 8,596 s in one gate). No exe named = the folder stays (the fallback).
            for o in outs:
                touched.discard(o)
    touched = {t for t in touched if t and t != 'out' and t.startswith(('out/', '../', '@main/')) or t.startswith(('game_data', 'generated'))}
    # inputs no check writes (the captures, the ninja build folder, the player's files) and programs a script only RUNS are reads; a script that compiles
    # (-o / clang in its text) writes its exes
    compiles = kind == 'py' and bool(re.search(r"""['"]-o['"]|\bclang""", text))
    ro = {t for t in touched if t.startswith(READ_ONLY) or any(t.startswith('@main/' + r) for r in READ_ONLY)
          or (kind == 'py' and not compiles and t.lower().endswith(('.exe', '.dll', '.pdb', '.lib', '_obj')))}
    touched -= ro
    writes = bool((WRITE_PS if kind == 'ps1' else WRITE_PY).search(text)) or savecase
    if writes and kind == 'py' and top and not savecase:
        # the script's writes are behind a flag the command does not pass (--check: compare only), or go only to its own tempfile folder
        if no_sub and '--check' in cmd and "'--check'" in text.replace('"', "'"):
            writes = False
        elif 'tempfile.' in text and not touched and not _ARG_WRITE_IN_TEXT.search(text):
            writes = False
        elif not touched:
            # every output option the script offers (--json, --emit-expect, --write-expect, --out ...) is absent from this command line: its write calls do not run
            opts = {m.group(1) for m in _ARG_OPT.finditer(text)}
            if opts and not any(c.split('=')[0] in opts for c in cmd) and not re.search(r"open\((?!\s*(?:a|args|ns|opts)\.)[^)]*['\"][wa]b?['\"]", text):
                writes = False
    return touched, writes, ro


_ARG_OPT = re.compile(r"""add_argument\(\s*['\"](--(?:out|output|json|emit-expect|write-expect|emit|write|dir|csv)[\w-]*)['\"]""")
_ARG_WRITE_IN_TEXT = re.compile(r"""add_argument\(\s*['\"](?:--out|-o|--output|--json|--dir)\b""")


_SCRIPT_LIT = re.compile(r"[\w./\\-]+\.(?:py|ps1)\b")


_PS1_WHOLE = re.compile(r"^\s*[\w./\\-]+\.ps1\s*$")
_PS1_CMD = re.compile(r"^\s*(?:pwsh|powershell)(?:\.exe)?\s.*?([\w./\\-]+\.ps1)\b")


def _py_ps1_literals(text):
    """The .ps1 files a python source can run: a string that IS a path ending .ps1 (an argv word) or a command string that starts with pwsh / powershell. A .ps1
    named inside a message string ('rebuild with tools\\fview\\build.ps1') is never run, so it is not followed (S103 DIB review bug 1: fview_exe.py's messages
    dragged tools\\fview\\build.ps1 into ~70 checks' closures). A file that does not parse yields nothing."""
    import ast
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return []
    out = []
    for n in ast.walk(tree):
        if isinstance(n, ast.Constant) and isinstance(n.value, str) and '.ps1' in n.value:
            v = n.value
            if _PS1_WHOLE.match(v):
                out.append(v.strip())
            else:
                m = _PS1_CMD.match(v)
                if m:
                    out.append(m.group(1))
    return out


def closure_files(root, a):
    """Every script a command argument reaches: the script itself, its local .py imports (proofkey.script_closure) and every .py / .ps1 named by a literal (a .ps1
    dot-sources or runs others). -> [(abs path, kind, text, is_top)]; the first is the argument's own file. Shared with the proof key (fastgate.cache_key_ex).
    Memoised in proofkey._CLOSURE (the selftests clear it when they edit scripts)."""
    import proofkey as pk
    key = ('cf', str(root), a)
    if key in pk._CLOSURE:
        return pk._CLOSURE[key]
    root = Path(root)
    out, seen = [], set()
    todo = [(root / a, True)]
    while todo and len(out) < 400:
        p, top = todo.pop(0)
        nk = os.path.normcase(os.path.abspath(str(p)))
        if nk in seen or not os.path.isfile(nk):
            continue
        seen.add(nk)
        kind = 'py' if nk.endswith('.py') else 'ps1' if nk.endswith('.ps1') else None
        if kind is None:
            continue
        members = [(str(p), top)]
        if kind == 'py':
            fl, _ = pk.script_closure(root, str(p))   # the whole import / literal closure of this script, already parsed once per file
            members += [(f, False) for f in fl]
        for mp, mtop in members:
            mk = os.path.normcase(os.path.abspath(mp))
            if mk != nk and mk in seen:
                continue
            seen.add(mk)
            mkind = 'py' if mk.endswith('.py') else 'ps1'
            info = pk._file_info(mp) if mkind == 'py' else None
            try:
                text = info[0] if info else Path(mp).read_text(encoding='utf-8', errors='replace')
            except OSError:
                continue
            out.append((mp, mkind, text, mtop))
            if mkind == 'ps1' or '.ps1' in text:
                # S103 DIB: a .ps1's # comments name scripts without running them (tools\cxx_cache.ps1's header names half the tools folder: every build script's
                # closure then reached run_list.ps1, the release scripts ... and was uncacheable). The helpers every build dot-sources are not expanded at all.
                if mkind == 'ps1' and os.path.basename(mp).lower() in ('cxx_cache.ps1', 'cxx_cc.ps1', 'env.ps1'):
                    continue
                for lit in (_py_ps1_literals(text) if mkind == 'py' else [m.group(0) for m in _SCRIPT_LIT.finditer(_code_only(text))]):
                    lit = lit.replace('\\', '/')   # (a python file only names a .ps1 it can RUN: S103 DIB review bug 1)
                    for cand in (root / lit, Path(mp).parent / lit):
                        if cand.is_file():
                            todo.append((cand, False))
                            break
    pk._CLOSURE[key] = out
    return out


def footprint(check, root, main, exes=()):
    """-> {'reads': set, 'writes': set, 'unknown': bool, 'gpu': bool, 'why': str}"""
    cmd = [a for a in check.get('cmd', []) if isinstance(a, str)]
    reads, writes, why = set(), set(), []
    unknown = False
    declared_w = check.get('writes')
    for r in check.get('reads', []) or []:
        reads.add(norm(r))
    text_all = ''
    # command line
    i = 0
    while i < len(cmd):
        a = cmd[i]
        low = a.lower()
        val = None
        if low in WRITE_ARGS and i + 1 < len(cmd):
            val, i = cmd[i + 1], i + 1
        elif '=' in a and a.split('=', 1)[0].lower() in WRITE_ARGS:
            val = a.split('=', 1)[1]
        if val is not None:
            r = rel_to(root, main, val)
            if r:
                writes.add(_globish(r))
        else:
            for m in PATH_TOKEN.finditer(a):
                r = rel_to(root, main, m.group(1))
                if r:
                    reads.add(_globish(r))
            # -o <path> / -Command bodies: "-o out/tools/x.exe"
            for m in re.finditer(r"\s-o\s+['\"]?([^\s'\";]+)", ' ' + a):
                r = rel_to(root, main, m.group(1))
                if r:
                    writes.add(_globish(r))
        i += 1
    # scripts: the file the command names AND everything it reaches (a wrapper's frame_rig.py globs out/cases and runs fview: the wrapper's own text says neither)
    script_writes = False
    scripts_ok = False
    scanned = set()
    for a in cmd:
        # a script named as an argument, or inside a -Command body
        names = [a] if a.lower().endswith(('.py', '.ps1')) else [m.group(0) for m in _SCRIPT_LIT.finditer(a)] if len(a) > 24 else []
        for nm in names:
            nm = nm.replace('\\', '/')
            if nm in scanned:
                continue
            scanned.add(nm)
            files = closure_files(root, nm)
            if not files:
                continue
            scripts_ok = True
            no_sub = not any(re.search(r'subprocess|os\.system|Popen|os\.exec|os\.spawn', f[2]) for f in files if f[1] == 'py')
            for _p, kind, t, top in files:
                touched, w, ro = script_paths(t, kind, cmd, top=top, no_sub=no_sub)
                if kind == 'ps1' and top and w and not (EXE_LIT.search(t) or OBJ_LIT.search(t)):
                    folders = sorted(x for x in touched if x.count('/') == 1 and not re.search(r'\.\w+$', x))
                    if folders:
                        why.append(f'{Path(_p).name}: writes but names no output exe: whole-folder footprint ({", ".join(folders)})')   # S103 GST: the fallback is said, not silent
                script_writes = script_writes or w
                (writes if w else reads).update(touched)
                reads.update(ro)
                if kind == 'py':
                    text_all += t   # the GPU inference reads python scripts only: a .ps1 build names the rig exes it COMPILES
    # a pwsh -Command body or an exe run directly: its own text
    body = ' '.join(cmd)
    compiled = bool(re.search(r'\bclang(\+\+)?\b|\bcl\.exe\b|\blink\b', body)) and ' -o ' in body
    for e in exes:
        reads.add(norm(e))
    if declared_w is not None:
        writes = {norm(w) for w in declared_w}
        why.append('writes declared')
        unknown = False
    else:
        first = cmd[0].lower() if cmd else ''
        interp = Path(first).stem in ('py', 'python', 'python3', 'pythonw')   # a python launcher runs a script (scanned below), it is not a built exe
        direct_exe = not interp and (first.endswith('.exe') or first.startswith(('out/', './out')))
        if direct_exe:
            why.append('runs a built exe (no scripts to scan): writes only what its args name')
        elif first in ('pwsh', 'powershell'):
            body_writes = bool(re.search(r"New-Item|Set-Content|Out-File|Remove-Item|Copy-Item|Move-Item|Add-Content|Compress-Archive|Expand-Archive", body)) or compiled
            has_script = any(a.lower().endswith('.ps1') for a in cmd)
            if has_script and not scripts_ok:
                unknown = True
                why.append('a .ps1 we cannot read')
            elif has_script and script_writes and not writes:
                unknown = True
                why.append('a .ps1 that writes but names no output path')
            elif body_writes and not writes:
                unknown = True
                why.append('a -Command that writes but names no output path')
            else:
                why.append('derived from the .ps1 / command text')
        elif any(a.lower().endswith('.py') for a in cmd):
            if not scripts_ok:
                unknown = True
                why.append('a .py we cannot read')
            elif script_writes and not writes:
                # the script writes (or runs something that does) and names no out\ path at all: its output lands somewhere we cannot see
                unknown = True
                why.append('a script that writes but names no output path')
            else:
                why.append('derived from the script text')
        else:
            unknown = True
            why.append('unrecognised command')
    # a path in main's out\ the check does not write is a shared read; paths in writes drop out of reads
    reads = {r for r in reads if not any(r == w or r.startswith(w + '/') for w in writes)}
    gpu = check.get('gpu')
    if gpu is None:
        gpu = bool(RIG_GPU.search(_code_only(text_all) + ' ' + body))   # comments and docstrings name tools without running them (game_syntax.py names fview in its docstring)
    return {'reads': reads, 'writes': writes, 'unknown': unknown, 'gpu': bool(gpu), 'why': '; '.join(why)}


def _code_only(text):
    """A python source without its docstrings and # comments (what the GPU inference reads: only code that can run a renderer counts)."""
    text = re.sub(r'(?s)"""(.*?)"""', '', text)   # docstrings (a code string that long is not a command line)
    return re.sub(r'(?m)^\s*#.*$|\s#\s.*$', '', text)


def _overlap(a, b):
    return a == b or a.startswith(b + '/') or b.startswith(a + '/')


class WaitDeadline(Exception):
    """A lock / slot wait ran past the gate's --deadline (S103 GST): the check is NOT RUN, never killed blind."""


def hms(t=None):
    return time.strftime('%H:%M:%S', time.localtime(t))


class PathsBusy(Exception):
    """acquire(nowait=True) found a conflicting holder / queued writer (S103 GST review 8): the caller lets go of what it holds and waits with nothing held."""


class PathLocks:
    """Readers-writers locks over path prefixes, all-or-nothing, inside one process. `unknown` = a barrier that conflicts with everything.
    S103 GST: WRITER PREFERENCE. A check that waits and writes (or is a barrier) holds back every LATER check that conflicts with it, so a stream of readers can no
    longer starve a build (66 build rows waited 8,596 s). Only a waiting writer holds back later arrivals, in arrival order: the blocked-by relation points
    only to held or earlier entries, so there is no cycle. A wait past `warn_s` (GATE_PATH_WARN_S, default 30) logs once, naming the holders."""

    def __init__(self, warn_s=None):
        self._cv = threading.Condition()
        self._held = []   # (token, reads, writes, unknown, label)
        self._queue = []  # waiting entries in arrival order: (token, reads, writes, unknown, label)
        self._n = 0
        self.waits = 0
        self.blocked_by = {}   # label -> text, while that check waits (the stall watchdog reads it)
        try:
            self.warn_s = float(os.environ['GATE_PATH_WARN_S']) if warn_s is None else float(warn_s)
        except (KeyError, ValueError):
            self.warn_s = 30.0

    @staticmethod
    def _conflict(r1, w1, u1, r2, w2, u2):
        if u1 or u2:
            return True
        for w in w1:
            for x in list(r2) + list(w2):
                if _overlap(w, x):
                    return True
        for w in w2:
            for x in r1:
                if _overlap(w, x):
                    return True
        return False

    def _blockers(self, me):
        """The labels of what keeps `me` out: held entries that conflict, and earlier WAITING writers that conflict."""
        out = []
        for h in self._held:
            if self._conflict(me[1], me[2], me[3], h[1], h[2], h[3]):
                out.append(h[4] or '?')
        for w in self._queue:
            if w[0] == me[0]:
                break
            if (w[2] or w[3]) and self._conflict(me[1], me[2], me[3], w[1], w[2], w[3]):
                out.append((w[4] or '?') + ' (queued writer)')
        return out

    def acquire(self, fp, label='', log=None, until=None, nowait=False):
        reads, writes, unknown = fp['reads'], fp['writes'], fp['unknown']
        with self._cv:
            self._n += 1
            me = (self._n, set(reads), set(writes), bool(unknown), label)
            self._queue.append(me)
            t0 = time.time()
            waited = said = False
            try:
                while True:
                    bl = self._blockers(me)
                    if not bl:
                        break
                    if nowait:
                        raise PathsBusy(', '.join(dict.fromkeys(bl)))
                    waited = True
                    self.blocked_by[label] = ', '.join(dict.fromkeys(bl))
                    if until is not None and time.time() >= until:
                        raise WaitDeadline(label)
                    if not said and log and time.time() - t0 >= self.warn_s:
                        said = True
                        what = ('writes ' + ','.join(sorted(writes))[:80]) if writes or unknown else ('reads ' + ','.join(sorted(reads))[:80])
                        log(f"[gate] {hms()} {label}: waiting {time.time() - t0:.0f} s for paths ({what}), blocked by: {self.blocked_by[label][:300]}")
                    self._cv.wait(0.5)
            except BaseException:
                self._queue.remove(me)
                self.blocked_by.pop(label, None)
                self._cv.notify_all()   # a leaving waiter may unblock the ones queued behind it
                raise
            self._queue.remove(me)
            self.blocked_by.pop(label, None)
            if waited:
                self.waits += 1
            self._held.append(me)
            return me[0]

    def release(self, tok):
        with self._cv:
            self._held = [h for h in self._held if h[0] != tok]
            self._cv.notify_all()


class GpuSlots:
    """N GPU slots shared by every gate on this PC: gpu.0 .. gpu.N-1 under <dir>, each held with an OS byte-range lock on the open file (released by the OS
    when the process ends, so no stale lock exists); the holder writes its pid / check name into the file for `gate status`."""

    def __init__(self, folder, n=2, poll=0.25, log=None, who=None):
        self.who = dict(who or {})   # S103 GST: {'root': checkout, 'gate': RUN_ID} written into every slot this gate takes, so a holder list says WHOSE slot it is
        self.who.setdefault('gate', 'pid%d' % os.getpid())
        self.dir = Path(folder)
        self.n = max(1, int(n))
        self.poll = poll
        self.log = log
        self.dir.mkdir(parents=True, exist_ok=True)
        self.waited = 0.0
        self._mu = threading.Lock()

    @staticmethod
    def _try_lock(fd):
        try:
            if os.name == 'nt':
                import msvcrt
                os.lseek(fd, 0, 0)
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False

    @staticmethod
    def _unlock(fd):
        try:
            if os.name == 'nt':
                import msvcrt
                os.lseek(fd, 0, 0)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError:
            pass

    def _ticket_try(self):
        """The ticket is held by whoever is GATHERING several slots (acquire n > 1): one gatherer at a time, and single-slot askers wait while it is held, so a gatherer is never starved by a stream of
        single askers and two gatherers never each hold part of what the other needs (no hold-and-wait cycle: slot holders never wait for anything while they hold a slot)."""
        try:
            fd = os.open(str(self.dir / 'gpu.ticket'), os.O_RDWR | os.O_CREAT)
        except OSError:
            return None
        if self._try_lock(fd):
            return fd
        os.close(fd)
        return None

    def _ticket_free(self):
        fd = self._ticket_try()
        if fd is None:
            return False
        self._unlock(fd)
        os.close(fd)
        return True

    def _grab_one(self, label, skip=()):
        for i in range(self.n):
            if i in skip:
                continue
            p = self.dir / f'gpu.{i}'
            try:
                fd = os.open(str(p), os.O_RDWR | os.O_CREAT)
            except OSError:
                continue
            if self._try_lock(fd):
                try:
                    os.lseek(fd, 1, 0)
                    os.ftruncate(fd, 1)
                    os.write(fd, json.dumps({'pid': os.getpid(), 'check': label, 'when': time.strftime('%H:%M:%S'), 'root': self.who.get('root', ''), 'gate': self.who['gate']}).encode())
                except OSError:
                    pass
                return (fd, i)
            os.close(fd)
        return None

    def acquire(self, label='', n=1, until=None):
        """n = 1: one slot (the token is (fd, index)). n > 1 (S103 GSP2: a check that runs n GPU processes at once holds n slots): n slots at once (a list token), gathered under the ticket."""
        n = max(1, min(int(n), self.n))
        t0 = time.time()
        said = False
        got = []
        tfd = None
        done = False
        try:
            while True:
                if n > 1:
                    if tfd is None:
                        tfd = self._ticket_try()
                    if tfd is not None:
                        while len(got) < n:
                            one = self._grab_one(label + ' (%d slots)' % n, skip={i for _, i in got})
                            if one is None:
                                break
                            got.append(one)
                        if len(got) == n:
                            with self._mu:
                                self.waited += time.time() - t0
                            done = True
                            return list(got)
                elif self._ticket_free():
                    one = self._grab_one(label)
                    if one is not None:
                        with self._mu:
                            self.waited += time.time() - t0
                        return one
                if not said and self.log and time.time() - t0 > 2:
                    said = True
                    self.log(f'[gate] {hms()} {label}: waiting for {n} GPU slot(s) ({self.n} slots, {self.holders()})')
                if until is not None and time.time() >= until:
                    raise WaitDeadline(label)
                time.sleep(self.poll)
        finally:
            if tfd is not None:
                self._unlock(tfd)
                os.close(tfd)
            if n > 1 and not done:   # gave up (an exception): hand back what was gathered
                for one in got:
                    self.release(one)

    def release(self, tok):
        if isinstance(tok, list):
            for one in tok:
                self.release(one)
            return
        fd, _ = tok
        self._unlock(fd)
        try:
            os.close(fd)
        except OSError:
            pass

    def holders(self):
        """Honest holder list (S103 GST): a slot whose OS lock can be taken is FREE (a released slot keeps its last label in the file: `free (last: X)`); a held one
        names its check, pid, and 'this gate' or 'other gate <root>'."""
        out = []
        for i in range(self.n):
            p = self.dir / f'gpu.{i}'
            try:
                with open(p, 'rb') as fh:   # byte 0 is the OS lock: read from byte 1 (a read over a locked range fails on Windows)
                    fh.seek(1)
                    t = fh.read().decode('utf-8', 'replace')
                d = json.loads(t) if t.strip() else {}
            except Exception:
                d = {}
            free = False
            try:
                fd = os.open(str(p), os.O_RDWR)
                try:
                    free = self._try_lock(fd)   # a second handle: success = nobody holds it
                    if free:
                        self._unlock(fd)
                finally:
                    os.close(fd)
            except OSError:
                free = not d
            if free:
                if d:
                    out.append(f"{i}:free (last: {d.get('check', '?')})")
            elif d:
                mine = d.get('gate') == self.who['gate']
                out.append(f"{i}:{d.get('check', '?')} (pid {d.get('pid', '?')}, " + ('this gate' if mine else f"other gate {d.get('root') or d.get('gate') or '?'}") + ')')
            else:
                out.append(f'{i}:held (no label)')
        return ', '.join(out) or 'all free'


def default_jobs(cpu=None):
    """Lanes in parallel by default: a quarter of the logical CPUs, 2..8 (a 5950X's 32 threads give 8; the C++ builds use their own 16 threads, so more workers
    only add contention). GATE_JOBS overrides."""
    try:
        return max(1, int(os.environ['GATE_JOBS']))
    except (KeyError, ValueError):
        pass
    cpu = cpu or os.cpu_count() or 4
    return max(2, min(8, cpu // 4))


def default_gpu_slots():
    try:
        return max(1, int(os.environ['GATE_GPU_SLOTS']))
    except (KeyError, ValueError):
        return 6   # S103 2026-11-18 09:10 back to 6 (Jarrod: "maybe we need to step back down to 6 ... gpu usage will never hit that high"; RAM / CPU contention, clang OOMs). Was 10 at 21:50 (Jarrod: "could probably go from like 6 to 10"; 4090 at 8 %), was 6 at 21:42: 2 slots machine-wide queued every parallel gate (PW-1's RL_lint check timed out at 1800 s waiting) while the 4090 sat at 8 % / 1.4 of 24 GB; a headless rig job is small
