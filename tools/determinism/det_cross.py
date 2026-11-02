"""Session 77 (XDET_PLAN.md): the strict cross-device verdict, run as `det_compare.py --cross A B`. Read-only.

    py tools/determinism/det_compare.py --cross A B [--exe-dir PRESET_OR_DIR] [--out FILE]

A and B: a run number of this PC (out/harness/determinism/det_NNN.csv + <exe dir>/logs/wwe2k14_NNN.log), or a folder
or .zip with det_*.csv and wwe2k14_*.log (+ det_serial*.txt scheduler trace, det_*.bin raw dumps): e.g. a phone run
pulled by tools/android/run.ps1, or a tester's SHARE zip. Unlike the plain compare there is no alignment and no slack:
under replay mode every frame must match frame against frame.

Prints, first, one verdict line, then the evidence:
  INVALID     the pair cannot say anything (the runs' settings differ, a det timeout fired, the replay slipped or
              missed, the render-list guard tripped, a test poke, an unreadable region in the match)... stamped with
              the first frame each applies from; a divergence before that frame is still reported.
  DIVERGED    the earliest frame where any layer differs, with its layer, regions and class.
  MATCH       every compared frame equal in every layer (with the coverage line: what is and is not hashed).
Layers: SCHED (scheduler: the per-frame det vblank 'vb' and the trace's per-vblank pick hashes), EVENTS (script marks
and file opens on the same frame), SIM (game state values), LAYOUT (objects with heap pointers), AUDIO (the audio
LCG). Classes: value, nan-only (both NaN, bits differ; from raw dumps when present), layout, timing.
Text also goes to out/harness/determinism/compare_cross_<A>_<B>.txt (game-derived: stays under out/).
"""
import glob
import os
import re
import struct
import sys
import tempfile
import zipfile

import det_compare as dc

LAYER_OF = {'alcg': 'AUDIO', 'glob': 'LAYOUT', 'c0': 'LAYOUT', 'c1': 'LAYOUT', 'ref': 'LAYOUT', 'cam': 'LAYOUT',
            'vb': 'SCHED'}
LAYERS = ['SCHED', 'EVENTS', 'SIM', 'LAYOUT', 'AUDIO']
NOT_HASHED = ('physics (Havok) state, animation / move state, AI brain state, crowd and props, audio output '
              '(XMA PCM), GPU-written memory (resolves), render commands')

# Log lines that make a run non-deterministic or not comparable. (regex, reason, how to read a frame from it)
FRAME_RE = re.compile(r'frame[ =](\d+)')
INVALID_RES = [
    (re.compile(r'\[det\] serial: vblank (\d+) fired while a guest thread still runs'), 'det_serial vblank timeout',
     'vblank'),
    (re.compile(r'\[det\] vblank (\d+) fired without quiescence'), 'det_frames quiescence timeout', 'vblank'),
    (re.compile(r'\[det\] serial: a wait handle without a readiness peek'), 'det_serial: a wait without a peek', None),
    (re.compile(r'\[replay\] SLIP at frame (\d+)'), 'replay slip (controller call count)', 'frame'),
    (re.compile(r'\[replay\] no recorded answer .* at frame (\d+)'), 'replay miss', 'frame'),
    (re.compile(r'\[replay\] FAILED'), 'replay file not read', None),
    (re.compile(r'given explicitly; replay mode wants'), 'a setting conflicts with the replay', None),
    # (Session 77, gap 23: only the reports that edit guest memory or come before a hang; a list end passed inside the
    # buffer is a report-only note since the guard change. Older logs' "went past its end" lines did edit memory.)
    (re.compile(r"\[replay_watch\] (?:a render-command list's walk (?:went past its end|left the record buffer)|"
                r'a render-command replay ran past|TEST: the replay guard)'),
     'render-list guard tripped (guest memory edited) or a runaway walk', 'frame'),
    (re.compile(r'\[dethash\] poke at frame (\d+)'), 'TEST poke (deliberate change)', 'frame'),
]
# Session 77: slow-host lines that change nothing (reported as notes, not INVALID). Both waits have no time limit in
# the SDK (det_serial.cpp WaitCpDrained logs every 3 s and keeps waiting; SerialWaitIdle logs a long baton hold and
# keeps waiting): the one escape is the vblank timeout above, INVALID on its own. Seen: run 4865 (an older Adreno's
# limits emulated: slow pipeline builds), scheduler trace equal to the reference.
NOTE_RES = [
    (re.compile(r'\[det\] serial: slot \d+ .* has held the baton for'), 'det_serial: a thread held the baton over the '
     'stall time (log only)'),
    (re.compile(r'\[det\] serial: still waiting for a drain'), 'det_serial: CP / XMA drain wait over 3 s (log only)'),
    (re.compile(r"\[replay_watch\] a render-command list's walk passed its end inside the buffer"),
     'render-list walk passed a list end inside its buffer (report only, src\\replay_watch.h)'),
]
# Settings that differ between machines without changing the game (paths, windows, logs, this tool's own outputs).
HOST_ONLY = re.compile(r'^(user_data_root|cache_root|game_data_root|log_|wwe2k14_det_hash_dir|wwe2k14_replay_dir|'
                       r'wwe2k14_det_hash_raw_(?:every|from|to)|det_serial_trace_detail|'
                       r'det_serial_trace|wwe2k14_input_script|wwe2k14_timeline|wwe2k14_settings_file|gpu_anatomy_path|'
                       # Session 77 (comparer skeptics, wf_9dc5ff67-b15): not the vulkan_ readback switches (they copy
                       # GPU output into guest memory: vulkan/command_processor.cpp IssueCopy_ReadbackResolvePath
                       # memcpy), not gpu_native_detached (writes a guest D3D device byte, half2_hooks.h) and not
                       # gpu_extents_ (the extents the CP writes into guest memory): those are compared.
                       r'window_|fullscreen|vsync|wwe2k14_check_|wwe2k14_live|wwe2k14_gpu$|'
                       # (Session 77: gpu_det_skip_host_draws is compared too: copies the game reads back are not
                       # written with it on.)
                       r'vulkan_(?!readback_)|gpu_(?!native_detached|extents_|det_skip_host_draws)|'
                       r'present_|wwe2k14_screenshot|wwe2k14_clip|wwe2k14_bookmark|wwe2k14_replay_record$|'
                       r'wwe2k14_replay_play$|wwe2k14_det_poke$|'
                       # Session 77 (gap 23): the render-list guard (on by default on Android only) changes guest
                       # memory only when it trips, and every trip is INVALID by its own log line (INVALID_RES), so
                       # the setting itself is compared through those lines.
                       r'wwe2k14_replay_guard$|'
                       # Session 77: the det wait limits (src\replay.h does not carry them either): a limit that fires
                       # is INVALID by its own log line, one that does not fire changes nothing.
                       r'det_serial_vblank_timeout_ms|det_serial_stall_ms|det_frames_timeout_ms|det_frames_settle_us|'
                       # Session 77 (phone run 120): the app's own folders on Android (src\android_driver.h), and the
                       # Windows power-throttling opt-out run_list sets (src\background_speed.h: nothing off Windows).
                       r'wwe2k14_android_lib_dir$|wwe2k14_android_tmp_dir$|wwe2k14_android_driver_dir$|'
                       r'wwe2k14_scenario_dir$|'  # the folder of the scenario files (their name is carried)
                       # Session 79: the HD 520 arc switch (src\native_frame.h) only sets gpu_ settings, none of them
                       # the three compared above (native_detached, extents_, det_skip_host_draws).
                       r'wwe2k14_native_frame$|'
                       # Session 82: run_list mutes fast-forwarded runs; audio_mute only zeroes the samples handed to
                       # SDL (sdl_audio_driver.cpp), the game's audio work is unchanged.
                       r'audio_mute$|'
                       r'wwe2k14_background_full_speed$)')
# Session 77: settings whose compiled-in default differs by platform (tools: a scan of every REXCVAR_DEFINE inside an
# #if or with a macro default). A '# xdet modified' line lists only settings changed from the default, so a side left
# at default is read as this platform's default, not as "unknown" (else PC 'off' vs phone default 'off' looked
# different, and PC default 'on' vs phone default 'off' looked the same). Keyed by the '# xdet' arch value.
# Since Session 77's later builds the '# xdet platform' line states these settings' effective values (effective()
# uses that first); this table is for the CSVs written before (nf_skin_3bone was off by default on ARM until Jarrod
# turned it on everywhere, 2026-10-31 ~12:20).
PLATFORM_DEFAULTS = {
    'wwe2k14_nf_skin_3bone': {'x86-64': 'on', 'arm64': 'off'},  # src\nf\skin_3bone.h, src\native_fn.h (before)
    # (default true on every platform since Session 66, src\native_skin.h: an older CSV that does not state it, next
    # to a newer one that does, read 4917 vs 4919 as "settings differ")
    'wwe2k14_native_skin': {'x86-64': 'true', 'arm64': 'true'},
    # (Session 77, gap 23: on by default on Android only, src\replay_watch.h; host-only above, listed here so the
    # pair's effective values are shown. Keyed 'platform/arch': the default follows __ANDROID__, not the arch.)
    'wwe2k14_replay_guard': {'android/arm64': 'true', 'windows/x86-64': 'false'},
}


def bundle(spec, exe_dir):
    """{label, csv, log, bin, trace} for a run number, a folder or a zip."""
    if spec.isdigit():
        run = int(spec)
        csv = os.path.join(dc.DET, dc.det_stem(run, exe_dir) + '.csv')
        b = {'label': 'run %d' % run, 'csv': csv, 'log': os.path.join(exe_dir, 'logs', 'wwe2k14_%03d.log' % run),
             'bin': csv[:-4] + '.bin', 'trace': None}
    else:
        path = os.path.abspath(spec)
        if path.lower().endswith('.zip'):
            # Session 77 (skeptics): a fresh folder per extraction - a reused one kept an older zip's trace / dump,
            # which then stood in for files this zip does not have.
            base = os.path.join(dc.DET, 'cross')
            os.makedirs(base, exist_ok=True)
            dest = tempfile.mkdtemp(prefix=os.path.splitext(os.path.basename(path))[0] + '_', dir=base)
            with zipfile.ZipFile(path) as z:
                z.extractall(dest)
            path = dest
        if not os.path.isdir(path):
            dc.fail('not a run number, folder or zip: %s' % spec)

        def newest(pattern, rx=None):
            hits = sorted((h for h in glob.glob(os.path.join(path, '**', pattern), recursive=True)
                           if not rx or re.match(rx, os.path.basename(h))), key=os.path.getmtime)
            return hits[-1] if hits else None
        # (Session 77: the current log part, wwe2k14_NNN.log, never a rotated wwe2k14_NNN.K.log: a zip keeps no mtimes.)
        b = {'label': os.path.basename(path), 'csv': newest('det_*.csv'),
             'log': newest('wwe2k14_*.log', r'^wwe2k14_\d+\.log$'),
             'bin': newest('det_*.bin'), 'trace': newest('det_serial*.txt')}
        # Session 77 (phone run 120): the log with the CSV's own run number (det_files_148.csv -> wwe2k14_148.log), not
        # the newest: a pulled phone logs folder holds every older log too, all with the pull's time.
        m = re.search(r'_(\d+)\.csv$', b['csv'] or '')
        if m:
            own = newest('wwe2k14_%s.log' % m.group(1))
            if not own:  # Session 77 (skeptics): never another run's log (its INVALID lines and events are not ours)
                dc.fail('%s: no wwe2k14_%s.log for %s (the CSV\'s own log is needed)' % (spec, m.group(1), b['csv']))
            b['log'] = own
            if b['bin'] and not b['bin'].endswith('_%s.bin' % m.group(1)):
                own_bin = newest('det_*_%s.bin' % m.group(1)) or newest('det_%s.bin' % m.group(1))
                b['bin'] = own_bin
    for key in ('csv', 'log'):
        if not b[key] or not os.path.exists(b[key]):
            dc.fail('%s: no %s found (%s)' % (spec, key, b[key]))
    # Session 77 (comparer skeptic): every part of the run's log (spdlog rotates wwe2k14_NNN.log into wwe2k14_NNN.K.log,
    # K = 1 the newest), oldest first: an INVALID line (a vblank timeout, logged for the first 20 only) may sit in one.
    stem = os.path.splitext(b['log'])[0]
    parts = sorted(glob.glob(glob.escape(stem) + '.*.log'),
                   key=lambda p: -int(re.search(r'\.(\d+)\.log$', p).group(1)) if re.search(r'\.(\d+)\.log$', p) else 0)
    b['log_parts'] = parts + [b['log']]
    if not b['trace']:
        with open(b['log'], encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.search(r'\[det\] serial trace -> (\S+) \(open\)', line)
                if m:
                    b['trace'] = m.group(1)
    if b['trace'] and not os.path.exists(b['trace']):
        b['trace'] = None
    return b


def xdet_lines(csv):
    """The '# xdet' header lines -> (platform dict, modified-settings dict). The identity line (Session 77, P7:
    src\\xdet_check.h) goes into the platform dict as 'identity.<key>'."""
    plat, mods = {}, {}
    with open(csv, encoding='utf-8', errors='replace') as f:
        for line in f:
            if not line.startswith('#'):
                break
            if line.startswith('# xdet modified:'):
                for kv in line[len('# xdet modified:'):].split():
                    k, _, v = kv.partition('=')
                    mods[k] = v
            elif line.startswith('# xdet renderer:') or line.startswith('# xdet track:'):
                continue  # (read by track_lines)
            elif line.startswith('# xdet identity:'):
                for kv in line[len('# xdet identity:'):].split():
                    k, _, v = kv.partition('=')
                    plat['identity.' + k] = v
            elif line.startswith('# xdet '):
                for kv in line[len('# xdet '):].split():
                    k, _, v = kv.partition('=')
                    plat[k] = v
    return plat, mods


AT_FRAME_RE = re.compile(r'^# xdet (changed|timeouts|renderer) at frame (\d+): (.*)$')


def track_lines(csv):
    """Session 77: what src/frame_hash.h marks besides the first-frame snapshot (SDK + game, phone session):
    {'track': bool (the writer marks mid-run changes), 'renderer': the effective renderer at the start,
     'changed': [(frame, name, value)], 'timeouts': [(frame, text)], 'renderers': [(frame, text)]}."""
    out = {'track': False, 'renderer': None, 'changed': [], 'timeouts': [], 'renderers': []}
    with open(csv, encoding='utf-8', errors='replace') as f:
        for line in f:
            if not line.startswith('# xdet '):
                continue
            line = line.rstrip('\r\n')
            if line.startswith('# xdet track:'):
                out['track'] = 'settings' in line
            elif line.startswith('# xdet renderer:'):
                out['renderer'] = line[len('# xdet renderer:'):].strip()
            else:
                m = AT_FRAME_RE.match(line)
                if not m:
                    continue
                what, frame, text = m.group(1), int(m.group(2)), m.group(3)
                if what == 'changed':
                    for kv in text.split():
                        k, _, v = kv.partition('=')
                        out['changed'].append((frame, k, v))
                elif what == 'timeouts':
                    out['timeouts'].append((frame, text))
                else:
                    out['renderers'].append((frame, text))
    return out


CVAR_RE = re.compile(r'\[wwe2k14\]\[input\] cvar (\S+) = (.*?) \(frame (\d+)\) -> ok')
RUN_NO_RE = re.compile(r'\[wwe2k14\] run log number: (\d+)')


def scan_log(paths, vb_to_frame, expected_run=None, cvar_rule=True):
    """[(frame or None, reason, line)] for every INVALID line, {note reason: count} (NOTE_RES), and the recording's
    end frame (or None), over all the log parts (oldest first). A log without its first line ('run log number') is
    cut: INVALID (a lost part could hold the one line that marks a timeout); one with another run's number (not the
    CSV's) is INVALID too."""
    found, notes, end = [], {}, None
    started = False
    for path in paths:
        with open(path, encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
        for line in lines[:400]:
            m = RUN_NO_RE.search(line)
            if m:
                started = True
                if expected_run is not None and int(m.group(1)) != expected_run:
                    found.append((None, 'log of run %s, not the CSV\'s run %d' % (m.group(1), expected_run),
                                  line.strip()[-120:]))
                break
        for line in lines:
            # Session 77 (comparer skeptics): a route's `cvar` step changes a setting mid-run; the '# xdet modified'
            # line is a snapshot from the first hashed frame and cannot show it. Host-only settings excepted.
            # (When both CSVs mark mid-run setting changes themselves, the comparison of those lines replaces this.)
            m = CVAR_RE.search(line)
            if m:
                if cvar_rule and not HOST_ONLY.match(m.group(1)):
                    found.append((int(m.group(3)), 'a setting changed mid-run by the route: %s = %s' % m.groups()[:2],
                                  line.strip()[-200:]))
                continue
            if '[replay] recording controller calls to' in line:
                end = 'recording'  # the run that made the recording: complete by definition
                continue
            m = re.search(r'\[replay\] recording ended at frame (\d+)', line)
            if m:
                end = int(m.group(1))
                s = re.search(r'misses (\d+), slips (\d+)', line)
                if s and (int(s.group(1)) or int(s.group(2))):
                    found.append((end, 'replay summary: misses %s, slips %s' % s.groups(), line.strip()))
                continue
            for rx, reason, kind in INVALID_RES:
                m = rx.search(line)
                if not m:
                    continue
                frame = None
                if kind == 'vblank' and m.groups():
                    frame = vb_to_frame(int(m.group(1)))
                elif kind == 'frame':
                    g = m.group(1) if m.groups() else None
                    fm = FRAME_RE.search(line)
                    frame = int(g) if g else (int(fm.group(1)) if fm else None)
                found.append((frame, reason, line.strip()[-200:]))
                break
            for rx, reason in NOTE_RES:
                if rx.search(line):
                    notes[reason] = notes.get(reason, 0) + 1
                    break
    if not started:
        found.append((None, 'log cut: no "run log number" line in %d part(s) (a lost part could hold an INVALID '
                            'line)' % len(paths), ''))
    return found, notes, end


def read_trace(path):
    """{det vblank: (decision count, pick hash, picks)}; picks = slot numbers, 'x' external request, '-' idle."""
    out = {}
    if not path:
        return out
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.rstrip('\n').split(' ', 3)
            if len(p) >= 3 and p[0].isdigit():
                out[int(p[0])] = (p[1], p[2], p[3].split() if len(p) > 3 else [])
    return out


def trace_diff(tr_a, tr_b):
    """(differing vblanks, of them the ones whose thread picks differ (not only the 'x' externals), comparable count).
    The pick list is cut at 240 characters in the trace: a cut list counts as thread-order different (unknown)."""
    vbs = sorted(set(tr_a) & set(tr_b))
    diff = [v for v in vbs if tr_a[v][:2] != tr_b[v][:2]]
    order = []

    def picks(tokens):  # det_serial_trace_detail adds '[CP wait]' before a pick and 'iPOS.N' interrupt tags
        return [re.sub(r'^\[[^\]]*\]', '', t) for t in tokens if not re.match(r'^i[0-9A-F]+\.\d$', t)]
    for v in diff:
        pa, pb = picks(tr_a[v][2]), picks(tr_b[v][2])
        full = len(pa) == int(tr_a[v][0]) and len(pb) == int(tr_b[v][0])  # else cut at 240 characters: unknown
        if not full or [p for p in pa if p != 'x'] != [p for p in pb if p != 'x']:
            order.append(v)
    return diff, order, len(vbs)


def is_nan_word(w):
    (u,) = struct.unpack('>I', w)
    return (u & 0x7F800000) == 0x7F800000 and (u & 0x007FFFFF) != 0


def is_heap_pointer(w):
    """A guest physical-heap address (0xA0000000-0xBFFFFFFF: where the game's objects live, e.g. the slot table's
    0xA8F77710). As a float that is a tiny negative number (about -1e-19), so value words rarely look like this.
    A heuristic: the class says so."""
    (u,) = struct.unpack('>I', w)
    return 0xA0000000 <= u < 0xC0000000


def classify(raw_a, raw_b, frame, k, layout=False):
    """'nan-only' / 'value' (/ 'layout (pointer words)' for object regions) from raw dumps at this frame, or None when
    no dump pair exists (the first differing frame may fall between dumps: then the next dump pair that differs)."""
    frames = [f for f in sorted(set(raw_a) & set(raw_b)) if f >= frame]
    for f in frames:
        a, b = raw_a[f][k], raw_b[f][k]
        if a is None or b is None:
            continue
        a, b = bytes(a), bytes(b)
        diffs = [o for o in range(0, min(len(a), len(b)) & ~3, 4) if a[o:o + 4] != b[o:o + 4]]
        if not diffs:
            continue
        at = '' if f == frame else ' (raw dump at frame %d)' % f
        if all(is_nan_word(a[o:o + 4]) and is_nan_word(b[o:o + 4]) for o in diffs):
            return 'nan-only (%d words)%s' % (len(diffs), at)
        if layout and all(is_heap_pointer(a[o:o + 4]) and is_heap_pointer(b[o:o + 4]) for o in diffs):
            return 'layout (%d pointer words, first +%d)%s' % (len(diffs), diffs[0], at)
        values = [o for o in diffs if not (is_heap_pointer(a[o:o + 4]) and is_heap_pointer(b[o:o + 4]))]
        return 'value (%d words differ, first value word +%d%s)%s' % (
            len(diffs), values[0] if values else diffs[0],
            ', %d pointer words too' % (len(diffs) - len(values)) if len(values) < len(diffs) else '', at)
    return None


def main(args, exe_dir):
    lines = []

    def say(s=''):
        lines.append(s)

    A, B = bundle(args.a, exe_dir), bundle(args.b, exe_dir)
    names_a, rows_a, meta_a, drop_a = dc.read_csv_path(A['csv'])
    names_b, rows_b, meta_b, drop_b = dc.read_csv_path(B['csv'])
    if names_a != names_b:
        dc.fail('region lists differ: %s vs %s' % (names_a, names_b))
    names = names_a
    common = sorted(set(rows_a) & set(rows_b))
    if not common:
        dc.fail('no common frames')
    vb_k = names.index('vb') if 'vb' in names else None

    def vb_to_frame_of(rows):
        if vb_k is None:
            return lambda vb: None
        pairs = sorted((int(r[vb_k], 16), f) for f, r in rows.items() if r[vb_k] not in ('-', '0', '!'))

        def conv(vb):
            for v, f in pairs:
                if v >= vb:
                    return f
            return None
        return conv

    plat_a, mods_a = xdet_lines(A['csv'])
    plat_b, mods_b = xdet_lines(B['csv'])
    run_no = lambda b: int(re.search(r'(\d+)\.csv$', b['csv']).group(1)) if re.search(r'(\d+)\.csv$', b['csv']) else None
    trk_a, trk_b = track_lines(A['csv']), track_lines(B['csv'])
    both_track = trk_a['track'] and trk_b['track']
    inv_a, notes_a, end_a = scan_log(A['log_parts'], vb_to_frame_of(rows_a), run_no(A), cvar_rule=not both_track)
    inv_b, notes_b, end_b = scan_log(B['log_parts'], vb_to_frame_of(rows_b), run_no(B), cvar_rule=not both_track)
    # Session 77: the CSV's own marks. A det vblank that fired on a timeout (the SDK's counter) is INVALID from its
    # frame, log line or not; a setting changed mid-run must change the same way at the same frame on both sides.
    for side, trk, inv in (('A', trk_a, inv_a), ('B', trk_b, inv_b)):
        for frame, text in trk['timeouts']:
            inv.append((frame, 'det vblank timeout (SDK counter in the CSV): %s' % text, ''))
    if both_track:
        ch_a = {(f, k, v) for f, k, v in trk_a['changed'] if not HOST_ONLY.match(k)}
        ch_b = {(f, k, v) for f, k, v in trk_b['changed'] if not HOST_ONLY.match(k)}
        for side, mine, other, inv in (('A', ch_a, ch_b, inv_a), ('B', ch_b, ch_a, inv_b)):
            for f, k, v in sorted(mine - other):
                inv.append((f, 'a setting changed mid-run on this side only (or to another value): %s=%s' % (k, v),
                            ''))
    # The recording run itself: it ends where the other side's playback ended (or at its last row).
    if end_a == 'recording':
        end_a = end_b if isinstance(end_b, int) else max(rows_a)
    if end_b == 'recording':
        end_b = end_a if isinstance(end_a, int) else max(rows_b)
    ev_a, files_a = dc.read_log_path(A['log_parts'])
    ev_b, files_b = dc.read_log_path(B['log_parts'])

    # Fingerprint: every non-host setting must be the same.
    fp, allowed, host_diff = [], [], []
    allow = re.compile(args.allow) if getattr(args, 'allow', '') else None

    def effective(mods, plat, k):
        """(value or None, how to show it): a setting left at default is its platform's default when known."""
        if k in mods:
            return mods[k], mods[k]
        if k in plat:  # stated by the CSV writer (newer builds): this build's own effective value
            return plat[k], '(default: %s)' % plat[k]
        table = PLATFORM_DEFAULTS.get(k, {})
        where = '%s/%s' % (plat.get('platform'), plat.get('arch'))
        d = table.get(where, table.get(plat.get('arch')))
        return (d, '(default: %s on %s)' % (d, where)) if d is not None else (None, '(default)')
    stated = {'wwe2k14_native_skin', 'wwe2k14_nf_skin_3bone', 'wwe2k14_replay_guard'} & (set(plat_a) | set(plat_b))
    for k in sorted(set(mods_a) | set(mods_b) | set(PLATFORM_DEFAULTS) | stated):
        (va, sa), (vb, sb) = effective(mods_a, plat_a, k), effective(mods_b, plat_b, k)
        if va == vb:
            continue
        d = '%s: A %s, B %s' % (k, sa, sb)
        if HOST_ONLY.match(k):
            host_diff.append(d)
            continue
        (allowed if allow and allow.search(k) else fp).append(d)
    if not mods_a or not mods_b:
        fp.append('no "# xdet modified" line on %s (run without --wwe2k14_det_hash_v2): settings not compared'
                  % ('A' if not mods_a else 'B'))
    # Session 77 (P7): the identity line. Other game files (default.xex), another build or the check profile on one
    # side only: not comparable. A side written before the line existed is not checked (old CSVs).
    ident_a = {k[9:]: v for k, v in plat_a.items() if k.startswith('identity.')}
    ident_b = {k[9:]: v for k, v in plat_b.items() if k.startswith('identity.')}
    if ident_a and ident_b:
        # (Review: the release number only - every APK says '+dev' and test builds '-NAME-playtest', so a PC release
        # exe vs a phone of the same release would read "different builds". Which code ran is not proven by it.)
        release = lambda v: (re.match(r'v?\d+(?:\.\d+)*', v or '') or [v])[0]
        for key, what, norm in (('xex', 'different game files (default.xex digest/size)', None),
                                ('version', 'different releases', release),
                                ('profile', 'the tester check profile on one side only (or given too late)', None)):
            va, vb = ident_a.get(key), ident_b.get(key)
            if (norm(va) if norm else va) != (norm(vb) if norm else vb):
                fp.append('%s: A %s, B %s (%s)' % (key, va, vb, what))

    # The compared window: from the first common frame to the recording's end (after it the route's own pad and quit
    # take over: not a replay any more).
    last = min(max(common), end_a or max(common), end_b or max(common))

    # First difference per layer. Session 77: a difference after the window is noted, never a verdict (run 4897's
    # scheduler traces differed at det vblank 5850, during the quit, and the verdict read "DIVERGED at frame 1").
    first = {}  # layer -> (frame, detail)
    late = {}   # layer -> (frame, detail) after the window

    def note(layer, frame, detail):
        if frame is None:
            return
        target = late if frame > last else first
        if layer not in target or frame < target[layer][0]:
            target[layer] = (frame, detail)

    raw_a = dc.read_bin_path(A['bin']) if A['bin'] and os.path.exists(A['bin']) else {}
    raw_b = dc.read_bin_path(B['bin']) if B['bin'] and os.path.exists(B['bin']) else {}
    # The raw dumps hold the regions only: the v2 'vb' column (first) has no bytes there.
    raw_index = lambda k: k - 1 if vb_k is not None and k > vb_k else k
    per_region = []
    unreadable = []
    for k, name in enumerate(names):
        layer = LAYER_OF.get(name, 'SIM')
        firstf, ndiff, changes, prev, readable = None, 0, 0, None, 0
        for f in common:
            va, vb_ = rows_a[f][k], rows_b[f][k]
            if va not in ('-', '0', '!'):
                readable += 1
            if prev is not None and va != prev:
                changes += 1
            prev = va
            if (va == '!' or vb_ == '!') and end_a and f <= end_a and name != 'vb':
                unreadable.append((f, name))
            if va != vb_:
                ndiff += 1
                if firstf is None:
                    firstf = f
        cls = None
        if firstf is not None:
            if layer == 'SCHED':
                cls = 'schedule'
            elif layer == 'LAYOUT':
                # Whole objects hold values as well as heap pointers: only pointer words are a layout difference.
                cls = classify(raw_a, raw_b, firstf, raw_index(k), layout=True) or \
                    'layout or value (no raw dump pair differs)'
            else:
                cls = classify(raw_a, raw_b, firstf, raw_index(k)) or 'value (no raw dump pair differs)'
            note(layer, firstf, '%s: %s' % (name, cls))
        per_region.append((name, layer, firstf, ndiff, changes, readable, cls))

    # Events (script marks, file opens) must be on the same frame.
    for (la, fa), (lb, fb) in zip(ev_a, ev_b):
        if la != lb or fa != fb:
            note('EVENTS', min(fa, fb), 'event %s at A %d / B %d%s' % (la, fa, fb, '' if la == lb else ' (B: %s)' % lb))
            break
    for i, ((pa, fa), (pb, fb)) in enumerate(zip(files_a, files_b)):
        if pa != pb or fa != fb:
            note('EVENTS', min(fa, fb), 'file open #%d %s at A %d / B %s %d' % (i, pa, fa, pb, fb))
            break
    # Session 77 (phone run 160: no route at all, 0 marks to the PC's 5, read as "none"): one side's extra events or
    # file opens inside the compared window are a difference too (zip stops at the shorter list).
    for what, xa, xb in (('event', ev_a, ev_b), ('file open', files_a, files_b)):
        wa, wb = [e for e in xa if e[1] <= last], [e for e in xb if e[1] <= last]
        if len(wa) != len(wb):
            n = min(len(wa), len(wb))
            side, extra = ('A', wa[n]) if len(wa) > len(wb) else ('B', wb[n])
            note('EVENTS', extra[1], '%s #%d %s at frame %d on %s only (%d in A, %d in B up to frame %d)' %
                 (what, n, extra[0], extra[1], side, len(wa), len(wb), last))

    # Scheduler trace: first det vblank whose pick count / hash differ.
    tr_a, tr_b = read_trace(A['trace']), read_trace(B['trace'])
    sched_note = 'no scheduler trace on %s' % ('both' if not tr_a and not tr_b else ('A' if not tr_a else 'B'))
    sched_x = None  # (count, first vblank, first frame) when only the externals' timing differs
    if tr_a and tr_b:
        diff, order, nvb = trace_diff(tr_a, tr_b)
        conv = vb_to_frame_of(rows_a)
        # Only the det vblanks up to the window's last frame count (its 'vb' cell); later ones are noted.
        vb_last = None
        if vb_k is not None and last in rows_a and rows_a[last][vb_k] not in ('-', '0', '!'):
            vb_last = int(rows_a[last][vb_k], 16)
        if vb_last is not None:
            late_diff = [v for v in diff if v > vb_last]
            if late_diff:
                late_order = [v for v in order if v > vb_last]
                late['SCHED'] = (last + 1, 'scheduler traces differ in %d det vblanks after the window (first %d%s)'
                                 % (len(late_diff), late_diff[0], ', thread order too' if late_order else ''))
            diff = [v for v in diff if v <= vb_last]
            order = [v for v in order if v <= vb_last]
        if not diff:
            sched_note = 'scheduler traces equal over %d det vblanks' % nvb
        else:
            sched_note = ('scheduler traces differ in %d of %d det vblanks (%.2f%%), first %d (frame %s); thread order '
                          'differs in %d of them%s' % (len(diff), nvb, 100.0 * len(diff) / nvb, diff[0], conv(diff[0]),
                                                       len(order), ', first %d (frame %s)' % (order[0], conv(order[0]))
                                                       if order else ' (only where the external requests - GPU '
                                                       'interrupt callbacks, x - ran, or how many)'))
            if order:
                fr = conv(order[0])
                note('SCHED', fr if fr is not None else common[0], 'thread order at det vblank %d' % order[0])
            else:
                sched_x = (len(diff), diff[0], conv(diff[0]))

    # INVALID: every reason with the first frame it applies from.
    invalid = []
    for side, inv in (('A', inv_a), ('B', inv_b)):
        for frame, reason, line in inv:
            invalid.append((frame, side, reason, line))
    if unreadable:
        f, n = unreadable[0]
        invalid.append((f, '-', 'unreadable region in the recording (%s; %d cells)' % (n, len(unreadable)), ''))
    for d in fp:
        invalid.append((common[0], '-', 'settings differ: ' + d, ''))
    if end_a is None or end_b is None:
        invalid.append((None, '-', 'no "[replay] recording ended" line on %s: not a full replay play' %
                        ('A' if end_a is None else 'B'), ''))
    # Session 77: a mark after the compared window (the route's own quit, not the replay) is a note, like differences.
    late_invalid = [x for x in invalid if x[0] is not None and x[0] > last]
    invalid = [x for x in invalid if not (x[0] is not None and x[0] > last)]
    inv_from = min([f for f, *_ in invalid if f is not None], default=None)
    if invalid and inv_from is None:
        inv_from = common[0]

    # Verdict.
    div = min(((f, layer, d) for layer, (f, d) in first.items()), default=None)
    say('cross-device compare: %s vs %s' % (A['label'], B['label']))
    if div and (inv_from is None or div[0] < inv_from):
        verdict = 'DIVERGED at frame %d: layer %s, %s%s' % (
            div[0], div[1], div[2], '' if inv_from is None else ' (valid up to frame %d)' % (inv_from - 1))
    elif invalid:
        first_inv = min(invalid, key=lambda x: (x[0] is None, x[0] or 0))
        verdict = 'INVALID from frame %s (%s)%s' % (inv_from, first_inv[2],
                                                   '' if not div else '; first difference at frame %d (%s, %s), not '
                                                   'before the invalid point' % (div[0], div[1], div[2]))
    elif div:
        verdict = 'DIVERGED at frame %d: layer %s, %s' % div
    elif sched_x:
        verdict = ('MATCH-EXCEPT-INTERRUPT-TIMING: frames %d-%d equal in SIM, LAYOUT, EVENTS, AUDIO and thread order; '
                   'GPU interrupt callbacks ran at another point or in another number in %d det vblanks (first at '
                   'frame %s): the game state did not change here, but the schedule is not identical' %
                   (common[0], last, sched_x[0], sched_x[2]))
    else:
        verdict = 'MATCH: frames %d-%d equal in every layer (%s); serial scheduling' % (common[0], last,
                                                                                      ', '.join(LAYERS))
    say('VERDICT: ' + verdict)
    say('covered: ' + ', '.join(n for n in names if n != 'vb'))
    say('not hashed: ' + NOT_HASHED)
    say('')
    say('A: %s | %s' % (A['csv'], ' '.join('%s=%s' % kv for kv in plat_a.items()) or meta_a[:150]))
    say('B: %s | %s' % (B['csv'], ' '.join('%s=%s' % kv for kv in plat_b.items()) or meta_b[:150]))
    for side, b in (('A', A), ('B', B)):  # Session 77 (skeptics): which log parts, trace and dump were read
        say('%s files: log %s; trace %s; raw dump %s' % (side, ' + '.join(b['log_parts']), b['trace'], b['bin']))
    for side, trk in (('A', trk_a), ('B', trk_b)):  # Session 77: the effective renderer (render-only: not compared)
        ident = {k[9:]: v for k, v in (plat_a if side == 'A' else plat_b).items() if k.startswith('identity.')}
        if ident:
            say('%s identity: %s' % (side, ' '.join('%s=%s' % kv for kv in ident.items())))
        say('%s renderer: %s%s%s' % (side, trk['renderer'] or '(not written by this build)',
                                    '; changed at frames ' + ', '.join('%d (%s)' % r for r in trk['renderers'][:5])
                                    if trk['renderers'] else '',
                                    '' if trk['track'] else '; mid-run changes not marked by this build'))
    say('frames: A %d rows, B %d rows, %d in common (%d-%d); recording ends A %s, B %s; dropped lines A %d, B %d' % (
        len(rows_a), len(rows_b), len(common), common[0], common[-1], end_a, end_b, drop_a, drop_b))
    if allowed:
        say('settings allowed to differ (--allow %s): %s' % (args.allow, '; '.join(allowed)))
    if host_diff:  # Session 77: shown, so a reader can judge the host-only list too
        say('host-only settings that differ (not compared): %s' % '; '.join(
            re.sub(r'(/data/app/|/data/user/)\S+', r'\1...', d) for d in host_diff))
    for side, notes in (('A', notes_a), ('B', notes_b)):
        if notes:
            say('notes %s (log only, not invalid): %s' % (side, '; '.join('%s x%d' % kv for kv in notes.items())))
    say('')
    say('first difference per layer:')
    for layer in LAYERS:
        say('  %-7s %s' % (layer, '%d (%s)' % first[layer] if layer in first else 'none'))
    say('  sched trace: ' + sched_note)
    if late:
        say('after the compared window (frame > %d: the route, not the replay; not part of the verdict): %s' % (
            last, '; '.join('%s %d (%s)' % (layer, f, d) for layer, (f, d) in sorted(late.items()))))
    if late_invalid:
        say('invalid marks after the compared window (not part of the verdict): %s' % '; '.join(
            'frame %s %s %s' % (f, s, r) for f, s, r, _ in sorted(late_invalid, key=lambda x: x[0])[:10]))
    say('')
    say('INVALID reasons (%d):' % len(invalid))
    for frame, side, reason, line in sorted(invalid, key=lambda x: (x[0] is None, x[0] or 0))[:40]:
        say('  frame %-7s %s %s%s' % (frame, side, reason, ('  | ' + line) if line else ''))
    say('')
    say('per region (layer, first differing frame, frames differing, changes in A, readable frames in A, class):')
    inert = []
    for name, layer, firstf, ndiff, changes, readable, cls in per_region:
        say('  %-7s %-6s %-8s %-6d %-6d %-6d %s' % (name, layer, firstf if firstf is not None else '-', ndiff,
                                                    changes, readable, cls or ''))
        if layer == 'SIM' and (readable == 0 or changes == 0):
            inert.append(name)
    say('inert or never readable SIM regions (they check nothing in this run): %s' % (', '.join(inert) or 'none'))
    say('events: %d marks / %d file opens in A, %d / %d in B' % (len(ev_a), len(files_a), len(ev_b), len(files_b)))

    for s in lines:
        print(s)
    tag = lambda b: re.sub(r'[^A-Za-z0-9_.-]', '_', b['label'])
    out = args.out or os.path.join(dc.DET, 'compare_cross_%s_%s.txt' % (tag(A), tag(B)))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('\nwrote %s' % out)
    return 0 if verdict.startswith('MATCH') else 1
