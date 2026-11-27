r"""RENDER-CLOCK LINT (S103 lane RCL, spec docs/research/s103/TC_render_clocks.md section 3).

The renderer reads the simulation's own time state; it keeps no private clock. A guest frame number is LATCHED and later SUBTRACTED, a frame number is turned into a time, a wall clock feeds a
picture value: each is one source line, and each is a finding unless it is declared.

  py -I tools\lint\render_clock_lint.py --root .            exit 1 on a violation (a new unmarked site, a bad / stale marker)
  py -I tools\lint\render_clock_lint.py --root . --list     every site with its class and state
  py -I tools\lint\render_clock_lint.py --root . --selftest planted twins + the past-bug re-find (the fade code of commit 57a51129e)

Finding kinds: W wall clock read, F frame difference / latch / modulus / age window / frames-to-seconds, A time-like accumulator, P a clock-making call fed by an F(a) (reported as part of the F), S our own
shader time uniform.
Declared with one comment on the line or the line above the statement (or a render-clock-begin: ... render-clock-end block):
    // render-clock: <class> - <reason, at least 3 words>
classes (closed): game(<field>, <where>) present ctl lookup perf dev.  A marker on a line with no finding is itself a finding (markers must not rot), except game(...), which is checked the other way:
its statement must read a record field and must not take a frame difference.
Zero tolerance: every unmarked site is a violation (no baseline, no ratchet)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SRC_DIRS = ['src/renderer', 'src/renderer_adapter', 'src/renderer_host', 'src/renderer_scene', 'src/renderer_game', 'src/scene_live']
CODE_EXT = ('.h', '.cpp', '.inc')
SHADER_EXT = ('.glsl', '.frag', '.vert', '.comp')
SHADER_DIRS = ['src/post']
SHADER_PREFIX_DIRS = [('src/renderer/shaders', 'rdr_')]
# translated game shaders: their g_Time / noise strips are the game's constants, fed from the stream or a record (not scanned)
EXCLUDED = [('src/renderer/shaders/fx/', 'translated game shaders: g_Time is the game constant'), ('src/renderer/shaders/spv/', 'compiled SPIR-V'), ('src/scene/', 'game-shader twins')]
# whole files that only declare perf counters (class perf): path -> reason (empty on purpose, justify each line)
BASELINE_FILES = {}

CLASSES = ('present', 'ctl', 'lookup', 'perf', 'dev')
MARK_RX = re.compile(r'//\s*render-clock:\s*(.*)$')
BEGIN_RX = re.compile(r'//\s*render-clock-begin:\s*(.*)$')
END_RX = re.compile(r'//\s*render-clock-end\b')
GAME_RX = re.compile(r'^game\(([^)]*)\)\s*(?:-\s*(.*))?$')
CLS_RX = re.compile(r'^(present|ctl|lookup|perf|dev)\s*-\s*(.*)$')

FRAME = r'(?:guest_frame|hook_frame|record_frame|capture_frame|frame_number_?|frame_index|g_guest_frame|gf_now|gf|fno)'
FRAME_RX = re.compile(r'\b' + FRAME + r'\b')
LATCH_NAME = r'\w*(?:t0|origin|start|first|born|since|begin)\w*'
W_RX = re.compile(r'\b(?:steady_clock|high_resolution_clock|system_clock)\s*::\s*now\b|\bQueryPerformanceCounter\b|\btimeGetTime\b|\bGetTickCount(?:64)?\b|\bSDL_GetTicks(?:64)?\b|'
                  r'\bSDL_GetPerformanceCounter\b|\bclock_gettime\b|\b__rdtsc\b')
MSNAME = r'(?:_ms|_us|_ns|\bms|\bus|\bns|Ms|Us|Ns|census|stats|upload_\w*|_time\b|_t\b)'
USE_OK_RX = re.compile(MSNAME + r'|\bLog|printf|\bformat\b|\blog\b|Stat|stat')
ACC_RX = re.compile(r'\b(\w+)\s*(?:\+=|=\s*\1\s*\+)')
TIMEY = {'time', 'clock', 'phase', 'age', 'elapsed', 'tick', 'ticks', 'progress', 'alpha', 'fade', 'anim', 'scroll'}


def timey(name):
    words = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name).lower().split('_')
    return any(w in TIMEY for w in words) and not any(w in ('ms', 'us', 'ns', 'count', 'bytes', 'stats') for w in words)
ACC_DT_RX = re.compile(r'\+=\s*(?:dt\b|kDt\b|1\.0?f?\s*/\s*60|1\.f\s*/\s*fps)')
SHADER_UNI_RX = re.compile(r'\b(?:float|int|uint|vec[234])\s+(time|u_time|frame|frame_no|seed|phase|progress|age)\s*[;\[]|layout[^;]*\b(time|u_time)\b')
REC_READ_RX = re.compile(r'ctx\.t\.|\bframe\.\w+|\bPartClock\w*|\w+Row\b|\brec\.|\brecord\.')


def strip_code(text):
    """Remove block comments, line comments and string / char literal bodies, keep line numbers and keep the original lines for marker parsing."""
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    out = []
    for ln in text.split('\n'):
        ln = re.sub(r'"(?:\\.|[^"\\])*"', '""', ln)
        i = ln.find('//')
        out.append(ln if i < 0 else ln[:i])
    return out


def _site(ln, kind, msg):
    return {'line': ln, 'kind': kind, 'msg': msg}


def raw_findings(code, path, shader=False):
    """[{line, kind, msg, perf_auto}] for the stripped lines of one file (no markers applied)."""
    out = []
    if shader:
        for i, ln in enumerate(code):
            m = SHADER_UNI_RX.search(ln)
            if m:
                out.append(_site(i + 1, 'S', f'shader member "{m.group(1) or m.group(2)}" looks like a time / frame input: declare it game(...)'))
        return out
    n = len(code)
    assigned_from_frame = set()
    for ln in code:
        for m in re.finditer(r'\b(\w+)\s*=\s*[^=;]*\b' + FRAME + r'\b', ln):
            assigned_from_frame.add(m.group(1))
    latch_vars = {v for v in assigned_from_frame if re.fullmatch(LATCH_NAME, v)}
    for i, ln in enumerate(code):
        no = i + 1
        if not ln.strip():
            continue
        # F(b): the latch
        m = re.search(r'\b(' + LATCH_NAME + r')\s*=[^=][^;]*\b' + FRAME + r'\b', ln)
        if m and not re.search(r'[<>=!]=|\bconst\b.*\bcolor', ln):
            out.append(_site(no, 'F', f'latch: "{m.group(1)}" takes a guest frame number (a private clock origin)'))
        # F(a): FRAME - X / X - FRAME with X a latched name
        for lv in set(re.findall(r'\b(' + LATCH_NAME + r')\b', ln)) | (latch_vars & set(re.findall(r'\b\w+\b', ln))):
            if re.search(r'\b' + FRAME + r'\b[^;]*?(?<![-+*/<>=!])-(?!=|-|>)\s*[\w\.\->]*\b' + re.escape(lv) + r'\b|\b[\w\.\->]*\b' + re.escape(lv) + r'\b\s*-(?!=|-|>)[^;]*\b' + FRAME + r'\b', ln):
                call = re.search(r'\b(\w+)\s*\([^()]*\b' + FRAME + r'\b[^()]*-', ln)
                out.append(_site(no, 'F', f'difference of a guest frame and "{lv}"' + (f' fed to {call.group(1)}() (P)' if call else '')))
                break
        # F(a'): a frame-named parameter minus an origin-named one (arena_world.h ClockAt: frame - origin)
        m = re.search(r'\b(\w*frame\w*)\b\s*-\s*\b(' + LATCH_NAME + r')\b', ln)
        if m and not any(f[0] == no and f[1] == 'F' for f in [(x['line'], x['kind']) for x in out]):
            out.append(_site(no, 'F', f'difference "{m.group(1)} - {m.group(2)}": a frame minus a latched origin'))
        # F(c): phase / parity
        if re.search(r'\b' + FRAME + r'\b\s*%|\([^()]*\b' + FRAME + r'\b[^()]*\)\s*%|\b' + FRAME + r'\b\s*&\s*(?!&)(?!1\b)\w', ln) and not re.search(r'\bprintf\b', ln):
            out.append(_site(no, 'F', 'frame modulus / mask (phase or parity from a frame number)'))
        # F(d): age windows (a frame number compared with another frame number plus a k-constant)
        OP = r'(?:<=|>=|<|>)'
        KC = r'(?:\w+::)*k[A-Z]\w*'
        if re.search(r'\b' + FRAME + r'\b\s*' + OP + r'\s*[^;,()?]*?\+\s*' + KC + r'|\+\s*' + KC + r'\s*' + OP + r'\s*[\w\.\->]*\b' + FRAME + r'\b|\b' + FRAME + r'\b\s*-\s*[\w\.\->]+\s*' + OP + r'\s*\(?[^;,]*?\b' + KC, ln):
            out.append(_site(no, 'F', 'age window on a guest frame number (a frame count used as time)'))
        # F(e): frames to seconds
        if re.search(r'\b' + FRAME + r'\b\s*(?:/|\*)\s*60(?:\.0?f?)?\b|\b' + FRAME + r'\b[^;]*\*\s*\(\s*1\.0?f?\s*/\s*60', ln):
            out.append(_site(no, 'F', 'guest frames turned into seconds'))
        # A: accumulators
        m = ACC_RX.search(ln)
        if m and timey(m.group(1)) and not re.search(r'(?:stats|census|counters?|periodic)\w*\s*(?:\.|->)\s*$', ln[:m.start()]):
            out.append(_site(no, 'A', f'accumulator "{m.group(1)}": a private clock'))
        elif ACC_DT_RX.search(ln):
            out.append(_site(no, 'A', 'accumulator of a fixed time step'))
        # T: the between-ticks blend factor (class present: the game has only ticks)
        if re.search(r'\bkTweenAlpha\s*=[^=]|\.Blend\([^;]*?,\s*(?:0\.5f|0\.0f|seam_\.tween_alpha)\b', ln):
            out.append(_site(no, 'T', 'between-ticks blend factor (a presentation value: declare it present)'))
        # W: wall clocks
        if W_RX.search(ln):
            f = _site(no, 'W', 'wall clock read')
            f['perf_auto'] = _perf_auto(code, i)
            out.append(f)
    return out


def _perf_auto(code, i):
    """A W read whose value only reaches a duration / *_ms counter / log is measurement (class perf, no marker)."""
    ln = code[i]
    if re.search(r'duration', ln) and re.search(MSNAME, ln):
        return True
    m = re.search(r'\b(\w+)\s*=\s*[^;=]*(?:' + W_RX.pattern + r')', ln)
    if not m:
        # an expression statement / return of a clock read inside a duration expression on its own line
        return bool(re.search(r'duration|\.count\(\)', ln) and (re.search(r'\bLog|printf', ln) or re.search(MSNAME, ln)))
    var = m.group(1)
    uses = 0
    for j in range(i + 1, min(len(code), i + 160)):
        if re.search(r'\b' + re.escape(var) + r'\b', code[j]):
            if re.search(r'\b' + re.escape(var) + r'\s*=[^=]', code[j]) and not re.search(r'-\s*' + re.escape(var), code[j]):
                break  # reassigned: a new life starts
            uses += 1
            if not USE_OK_RX.search(code[j]):
                # the same line may continue on the next one (a wrapped statement): allow if the next line is ok
                if j + 1 < len(code) and USE_OK_RX.search(code[j + 1]):
                    continue
                return False
    return uses > 0


def parse_markers(raw_lines):
    """[{line, cls, field, reason, begin, end, cover:(a, b)}] and [{line, msg}] marker findings."""
    markers, bad = [], []
    n = len(raw_lines)
    open_block = None
    for i, ln in enumerate(raw_lines):
        mb = BEGIN_RX.search(ln)
        if mb:
            if open_block:
                bad.append({'line': i + 1, 'msg': 'render-clock-begin inside another block'})
            open_block = (i + 1, mb.group(1).strip())
            continue
        if END_RX.search(ln):
            if not open_block:
                bad.append({'line': i + 1, 'msg': 'render-clock-end without a begin'})
                continue
            spec = open_block[1]
            _add_marker(markers, bad, open_block[0], spec, (open_block[0] + 1, i))
            open_block = None
            continue
        m = MARK_RX.search(ln)
        if not m:
            continue
        spec = m.group(1).strip()
        # the statement: this line when it carries code, else from the next code line to the first line ending in ; { or }
        code_here = ln[:ln.find('//')].strip() if '//' in ln else ''
        if code_here:
            cover = (i + 1, i + 1)
            if not re.search(r'[;{}]\s*$', code_here):
                end = i
                while end + 1 < n and end - i < 8 and not re.search(r'[;{}]\s*$', _nocomment(raw_lines[end])):
                    end += 1
                cover = (i + 1, end + 1)
        else:
            j = i + 1
            while j < n and (not raw_lines[j].strip() or raw_lines[j].strip().startswith('//')):
                j += 1
            end = j
            while end + 1 < n and end - j < 8 and not re.search(r'[;{}]\s*$', _nocomment(raw_lines[end])):
                end += 1
            cover = (j + 1, end + 1)
        _add_marker(markers, bad, i + 1, spec, cover)
    if open_block:
        bad.append({'line': open_block[0], 'msg': 'render-clock-begin never closed'})
    return markers, bad


def _nocomment(ln):
    i = ln.find('//')
    return (ln if i < 0 else ln[:i]).rstrip()


def _add_marker(markers, bad, line, spec, cover):
    g = GAME_RX.match(spec)
    if g:
        if len(g.group(1).split()) < 1 or not g.group(1).strip():
            bad.append({'line': line, 'msg': 'game(...) marker names no field'})
            return
        markers.append({'line': line, 'cls': 'game', 'field': g.group(1).strip(), 'reason': g.group(2) or '', 'cover': cover})
        return
    c = CLS_RX.match(spec)
    if not c:
        word = spec.split(' ')[0].split('(')[0]
        if word in CLASSES:
            bad.append({'line': line, 'msg': f'marker "{word}" has no reason (write: render-clock: {word} - why, at least 3 words)'})
        else:
            bad.append({'line': line, 'msg': f'unknown render-clock class "{word}" (closed list: game(...) {" ".join(CLASSES)})'})
        return
    if len(c.group(2).split()) < 3:
        bad.append({'line': line, 'msg': f'marker "{c.group(1)}" reason is shorter than 3 words'})
        return
    markers.append({'line': line, 'cls': c.group(1), 'reason': c.group(2), 'cover': cover})


def lint_text(path, text, shader=False):
    """Returns dict: sites [{line, kind, msg, state, cls}], marker_bad [{line,msg}], unmarked (int)."""
    raw = text.split('\n')
    code = strip_code(text)
    finds = raw_findings(code, path, shader)
    markers, bad = parse_markers(raw)
    used = set()
    for f in finds:
        f['state'] = 'unmarked'
        for k, mk in enumerate(markers):
            a, b = mk['cover']
            if a <= f['line'] <= b:
                if mk['cls'] == 'game':
                    stmt = ' '.join(code[a - 1:b])
                    if f['kind'] == 'F' and not REC_READ_RX.search(stmt):
                        continue  # a game marker over a frame difference that reads no record field does not excuse it
                    if f['kind'] in ('F', 'W') and re.search(r'\b' + FRAME + r'\b\s*-|-\s*\b' + FRAME + r'\b', stmt):
                        continue
                f['state'] = 'marked'
                f['cls'] = mk['cls']
                used.add(k)
                break
        if f['state'] == 'unmarked' and f.get('perf_auto'):
            f['state'] = 'perf_auto'
            f['cls'] = 'perf'
    for k, mk in enumerate(markers):
        a, b = mk['cover']
        if mk['cls'] == 'game':
            stmt = ' '.join(code[a - 1:b])
            if not REC_READ_RX.search(stmt):
                bad.append({'line': mk['line'], 'msg': f'game({mk["field"]}) marker: its statement reads no record field (ctx.t.*, *Row, PartClock, frame.*)'})
            elif re.search(r'\b' + FRAME + r'\b\s*-|-\s*\b' + FRAME + r'\b', stmt):
                bad.append({'line': mk['line'], 'msg': f'game({mk["field"]}) marker over a guest-frame difference'})
        elif k not in used:
            bad.append({'line': mk['line'], 'msg': f'stale "{mk["cls"]}" marker: it covers no render-clock site (markers must not rot)'})
    return {'sites': finds, 'marker_bad': bad, 'unmarked': sum(1 for f in finds if f['state'] == 'unmarked')}


def scan_files(root):
    files = []
    for d in SRC_DIRS:
        for dp, _dn, fn in os.walk(os.path.join(root, d)):
            for f in fn:
                if f.endswith(CODE_EXT) and not f.endswith('_test.cpp'):
                    files.append((os.path.relpath(os.path.join(dp, f), root).replace('\\', '/'), False))
    for d in SHADER_DIRS:
        for dp, _dn, fn in os.walk(os.path.join(root, d)):
            for f in fn:
                if f.endswith(SHADER_EXT):
                    files.append((os.path.relpath(os.path.join(dp, f), root).replace('\\', '/'), True))
    for d, pre in SHADER_PREFIX_DIRS:
        for dp, _dn, fn in os.walk(os.path.join(root, d)):
            for f in fn:
                if f.endswith(SHADER_EXT) and f.startswith(pre):
                    files.append((os.path.relpath(os.path.join(dp, f), root).replace('\\', '/'), True))
    out = []
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import part_files   # S111 SPLIT-WIRE review: a split parent is read with its parts expanded (lint_tree); a part is not read again
    parts = {os.path.realpath(str(x)) for d in SRC_DIRS if os.path.isdir(os.path.join(root, d)) for x in part_files(os.path.join(root, d))}
    for p, sh in sorted(set(files)):
        if any(p.startswith(x) for x, _ in EXCLUDED) or p in BASELINE_FILES or os.path.realpath(os.path.join(root, p)) in parts:
            continue
        out.append((p, sh))
    return out


def lint_tree(root):
    res = {}
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
    for p, sh in scan_files(root):
        res[p] = lint_text(p, read_path(os.path.join(root, p)), sh)
    return res


def where(root, p):
    """line of p's (expanded) text -> (file, line): a split parent's line in a part is named by the part and its own line"""
    full = os.path.join(root, p)
    try:
        if b'split:' not in open(full, 'rb').read():
            return lambda ln: (p, ln)
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
        from read_unit import line_map
        m = line_map(full)
    except OSError:
        return lambda ln: (p, ln)

    def at(ln):
        if not 1 <= ln <= len(m):
            return p, ln
        f, n = m[ln - 1]
        return os.path.relpath(str(f), root).replace('\\', '/'), n
    return at


def check(root, res):
    bad = []
    allowed = 0
    for p, r in sorted(res.items()):
        at = where(root, p) if r['marker_bad'] or r['unmarked'] > allowed else None
        for mb in r['marker_bad']:
            f, n = at(mb['line'])
            bad.append(f'{f}:{n}: {mb["msg"]}')
        if r['unmarked'] > allowed:
            by_file = {}
            for s in r['sites']:
                if s['state'] == 'unmarked':
                    f, n = at(s['line'])
                    by_file.setdefault(f, []).append((n, s))
            for f, sites in sorted(by_file.items()):
                bad.append(f'{f}: {len(sites)} unmarked render-clock site(s): ' + '; '.join(f'{n} {s["kind"]} {s["msg"]}' for n, s in sites[:6]) +
                           '  -> carry the game\'s value, or declare it: // render-clock: <class> - <reason>')
    return bad


def list_sites(res):
    cnt = {}
    for p, r in sorted(res.items()):
        for s in r['sites']:
            cls = s.get('cls', '-')
            cnt[(s['state'], cls)] = cnt.get((s['state'], cls), 0) + 1
            print(f'{p}:{s["line"]}: [{s["kind"]}] {s["state"]}{"/" + cls if cls != "-" else ""}: {s["msg"]}')
    print('counts: ' + ', '.join(f'{k[0]}/{k[1]}={v}' for k, v in sorted(cnt.items())))


# ---------------------------------------------------------------- selftest
BAD_LATCH = '''  int fade_a = 255;
  if (entrance) as.match_clock_on = false;
  else if (stage_lmpage::On()) {
    if (!as.match_clock_on) as.match_clock_on = true, as.match_t0 = ctx.t.header.guest_frame;
    fade_a = assets::stage_pools::FadeAlpha255(ctx.t.header.guest_frame - as.match_t0);
  }
'''
TWIN_GOOD = '''  int fade_a = 255;
  if (entrance) as.match_clock_on = false;
  else if (stage_lmpage::On()) {
    // render-clock: game(stage_page_clock_ticks, obj+108, docs/research/s103/CF_crowd_fade.md)
    const float t = ctx.t.stage_page_clock_ticks; fade_a = assets::stage_pools::FadeAlpha255(uint64_t(t));
  }
'''
WALL_PROGRESS = '''  auto now = std::chrono::steady_clock::now();
  const double dt = std::chrono::duration<double>(now - last).count();
  phase += dt * 60.f;
  uv_offset = phase;
'''
OK_PERF = '''  const auto t0 = std::chrono::steady_clock::now();
  work();
  upload_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
'''
BAD_MARKER = '''  // render-clock: present
  const float kA = 0.5f;
  // render-clock: fast - a typo class here
  const int kB = 1;
  // render-clock: ctl - nothing below needs this marker
  const int kC = 3;
'''
OK_PRESENT = '''  // render-clock: present - the game has only ticks, no halfway value exists
  constexpr float kTweenAlpha = 0.5f; phase += kTweenAlpha;
'''
ARENA_CLOCK = '''inline bool ClockAt(uint32_t frame, uint32_t origin, bool origin_known, float& out) {
  if (!origin_known) return false;
  out = float(int32_t(frame - origin));
  return true;
}
'''
ARENA_CLOCK_DEV = '''// render-clock: dev - the J1 disproved clock, kept only for the offline proof plant
inline bool ClockAt(uint32_t frame, uint32_t origin, bool origin_known, float& out) {
  if (!origin_known) return false;
  // render-clock: dev - the J1 disproved clock, kept only for the offline proof plant
  out = float(int32_t(frame - origin));
  return true;
}
'''
GAME_BAD_FRAME = '''  // render-clock: game(stage_page_clock_ticks, obj+108)
  fade_a = FadeAlpha255(ctx.t.header.guest_frame - as.match_t0);
'''
SHADER_BAD = 'layout(push_constant) uniform P { vec4 a; };\nuniform float u_time;\nvoid main() {}\n'
SHADER_OK = '// render-clock: game(g_Time, the game constant fed from the record) \nuniform float u_time;\nvoid main() {}\n'


def _git_show(root, rev, path):
    try:
        r = subprocess.run(['git', '-C', root, 'show', f'{rev}:{path}'], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        return r.stdout if r.returncode == 0 and r.stdout else None
    except Exception:
        return None


def selftest(root):
    ok = True

    def expect(label, cond, extra=''):
        nonlocal ok
        print(f"  {'ok  ' if cond else 'FAIL'} {label} {extra}")
        ok &= bool(cond)

    r = lint_text('x.cpp', BAD_LATCH)
    kinds = [s['msg'] for s in r['sites'] if s['state'] == 'unmarked']
    expect('bad_latch: the old fade code is flagged at the latch AND the subtraction (callee named)', len(kinds) >= 2 and any('latch' in k for k in kinds) and any('FadeAlpha255' in k for k in kinds), f'({len(kinds)} finding(s))')
    r = lint_text('x.cpp', TWIN_GOOD)
    expect('twin_good: the same function reading a record field with a game(...) marker passes', r['unmarked'] == 0 and not r['marker_bad'], f'({r["unmarked"]} finding(s), {len(r["marker_bad"])} marker problem(s))')
    r = lint_text('x.cpp', GAME_BAD_FRAME)
    expect('twin_bad_game: a game(...) marker does not excuse a guest-frame difference', r['unmarked'] >= 1 and r['marker_bad'])
    r = lint_text('x.cpp', WALL_PROGRESS)
    k = {s['kind'] for s in r['sites'] if s['state'] == 'unmarked'}
    expect('bad_wall_progress: W and A findings', {'W', 'A'} <= k, str(sorted(k)))
    r = lint_text('x.cpp', OK_PERF)
    expect('ok_perf: t0 = now(); upload_ms_ += duration(now() - t0) is auto-exempt', r['unmarked'] == 0 and any(s['state'] == 'perf_auto' for s in r['sites']))
    r = lint_text('x.cpp', BAD_MARKER)
    expect('bad_marker: no reason / unknown class / stale marker = three marker findings', len(r['marker_bad']) == 3, f'({len(r["marker_bad"])})')
    r = lint_text('x.cpp', OK_PRESENT)
    expect('ok_present: a present marker with a reason excuses the accumulator', r['unmarked'] == 0 and not r['marker_bad'])
    r = lint_text('x.glsl', SHADER_BAD, True)
    expect('bad_shader_time: an unmarked u_time uniform is an S finding', any(s['kind'] == 'S' and s['state'] == 'unmarked' for s in r['sites']))
    r = lint_text('x.glsl', SHADER_OK, True)
    expect('ok_shader_time: a game(...) marker over it passes', r['unmarked'] == 0)
    # the time-translation sibling: the J1-disproved clock must be flagged unless declared dev
    r = lint_text('arena_world.h', ARENA_CLOCK)
    expect('sibling: ClockAt (frame - origin, the J1-disproved clock) is flagged', r['unmarked'] >= 1)
    r = lint_text('arena_world.h', ARENA_CLOCK_DEV)
    expect('sibling: ClockAt declared dev passes (the first marker, over the signature, goes stale: expected 1 marker problem)', r['unmarked'] == 0 and len(r['marker_bad']) == 1)
    # PAST-BUG RE-FIND: the fade code of commit 57a51129e from git (embedded copy when git cannot show it)
    src = _git_show(root, '57a51129e', 'src/renderer_scene/rf_arena_live.cpp')
    how = 'git show 57a51129e'
    if not src or 'match_t0' not in src:
        src, how = BAD_LATCH, 'embedded copy'
    r = lint_text('src/renderer_scene/rf_arena_live.cpp', src)
    hit = [s for s in r['sites'] if s['state'] == 'unmarked' and ('match_t0' in s['msg'] or 'FadeAlpha255' in s['msg'] or 'latch' in s['msg'])]
    expect(f'PAST-BUG RE-FIND ({how}): the fade latch and subtraction of rf_arena_live.cpp are flagged', len(hit) >= 2, f'(lines {[s["line"] for s in hit]})')
    # the planted unmarked site must FAIL check() (zero tolerance)
    expect('check(): a tree holding the old fade code fails', bool(check(root, {'x.cpp': lint_text('x.cpp', BAD_LATCH)})))
    # S111 SPLIT-WIRE review: a split parent is read with its parts expanded and a part is not read again: a site in a part is ONE site, named at the part's line
    import shutil
    import tempfile
    tmp = tempfile.mkdtemp(prefix='render_clock_split_')
    try:
        os.makedirs(os.path.join(tmp, 'src', 'renderer', 'p'))
        with open(os.path.join(tmp, 'src', 'renderer', 'p.h'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('#pragma once\nnamespace q {\n#include "p/part.inc"  // split: part\n}  // namespace q\n')
        body = 'inline void F(Ctx& ctx, As& as) {\n' + BAD_LATCH + '}\n'
        with open(os.path.join(tmp, 'src', 'renderer', 'p', 'part.inc'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(body)
        one = lint_text('x.h', body)['unmarked']
        rr = lint_tree(tmp)
        n = sum(r['unmarked'] for r in rr.values())
        msgs = check(tmp, rr)
        expect('split part: its sites are counted once (parent read with the part, the part not read again) and named at the part', one >= 2 and n == one
               and msgs and all('src/renderer/p/part.inc:' in m for m in msgs), f'(one {one}, tree {n}: {msgs[:2]})')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # the tree as it stands has no unmarked site and no bad or stale marker
    res = lint_tree(root)
    bad = check(root, res)
    expect('the current tree: no unmarked site, no bad or stale marker', not bad, f'({len(bad)} problem(s)) {bad[:2]}')
    print('first-run site counts by class (state/class): ', end='')
    cnt = {}
    for rr in res.values():
        for s in rr['sites']:
            cnt[(s['state'], s.get('cls', '-'))] = cnt.get((s['state'], s.get('cls', '-')), 0) + 1
    print(', '.join(f'{a}/{b}={n}' for (a, b), n in sorted(cnt.items())))
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    root = os.path.abspath(a[a.index('--root') + 1]) if '--root' in a else ROOT
    if '--selftest' in a:
        sys.exit(0 if selftest(root) else 1)
    res = lint_tree(root)
    if '--list' in a:
        list_sites(res)
        return
    bad = check(root, res)
    for b in bad:
        print('ERROR ' + b)
    total = sum(r['unmarked'] for r in res.values())
    print(f'render_clock_lint: {len(res)} file(s), {total} unmarked site(s), {len(bad)} violation(s)')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
