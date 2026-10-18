"""Self-tests for the triage recipes that the historical logs cannot exercise (Session 8, task 13).

    py tools\\triage\\selftest.py [--log LOG_WITH_SCAN_LINES] [--tsv classify_candidates_output.tsv]

1. classifier: every [scan] candidate/lone target in a run log is classified by triage's missing-function recipe
   and, with --tsv, compared with tools\\analysis\\classify_candidates.py's classes for the same log (same method;
   triage measures function extents with the code index, which re-syncs at labels, so functions with an inline
   jump table may differ: listed).
2. switch recipe, historical replay: the three decoder switches of PLAYBOOK Session 2 had 8 / 8 / 4 labels before
   config\\switch_tables.toml fixed them to 11. The recipe is run on each function with its labels truncated to the
   old count; it must report TOO SHORT and propose exactly the 11 labels that are in switch_tables.toml today.
3. fresh-map paths (synthetic logs, clearly not game runs): a 0xC000001D crash at a real ud2 inside a decoder
   function's host code, and a 0xC0000005 crash inside a guest function's host code, both with today's map and
   exe; triage must name the function and, for the ud2, find the switch and print the dump command.
4. event identity: event-1000 entries belong to a run by exe path + process start time (synthetic entries, and
   every real wwe2k14 entry in this machine's event log maps to exactly one release run).
5. faults outside the exe (synthetic DLL crashes: huge and wrapped offsets): DLL named, map not used.
6. guest AV correlation (synthetic): a probe AV long before a host crash / in a killed run is not the cause.
7. edge cases: empty log -> no_log; a map without an exe -> no exception. 8. the dump run 012: 66 tables matched.
Synthetic logs go to out\\harness\\triage\\selftest\\ (never a game log folder). Exit 0 = all passed.
"""
import argparse
import datetime as dt
import os
import re
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage as T  # noqa: E402

OUT = os.path.join(T.REPO, 'out', 'harness', 'triage', 'selftest')
# (function, its bctr from config\switch_tables.toml, ReXGlue's label count before the fix: PLAYBOOK Session 2)
DECODERS = [('sub_835A9728', 0x835A97E0, 8), ('sub_835ABFE8', 0x835AC0A0, 8), ('sub_835AD638', None, 4)]


def check(cond, msg, fails):
    print(('PASS  ' if cond else 'FAIL  ') + msg)
    if not cond:
        fails.append(msg)


def test_classifier(log, tsv, idx, fails):
    num, parts, _ = T.find_parts(log, T.EXE_DIR)
    run = T.parse_log(num, parts)
    ours = {}
    for t in sorted(run.scan):
        lines = []
        _, cls = T.recipe_missing_function(t, run, idx, lines)
        ours[t] = cls
    counts = {}
    for c in ours.values():
        counts[c] = counts.get(c, 0) + 1
    print(f'classifier on {len(ours)} [scan] targets of run {num:03d}: {counts}')
    check(len(ours) > 0, f'run {num:03d} has [scan] targets to classify', fails)
    if tsv:
        theirs = {}
        for line in open(tsv, encoding='utf-8').read().splitlines()[1:]:
            p = line.split('\t')
            theirs[int(p[0], 16)] = p[2]
        diff = [(t, theirs.get(t), ours.get(t)) for t in sorted(set(theirs) | set(ours)) if theirs.get(t) != ours.get(t)]
        for t, a, b in diff[:10]:
            f = idx.containing(t)
            print(f'      0x{t:08X}: classify_candidates {a}, triage {b} ({f[1] if f else "?"}, index size '
                  f'0x{f[2]:X}, {f[3]} instructions = 0x{4 * f[3]:X})' if f else f'      0x{t:08X}: {a} vs {b}')
        explained = [d for d in diff if (lambda f: f and f[2] != 4 * f[3])(idx.containing(d[0]))]
        check(len(diff) == len(explained), f'triage agrees with classify_candidates.py on {len(ours) - len(diff)}/'
              f'{len(ours)} targets; the {len(diff)} differences are all functions whose index extent (label '
              f're-sync) is longer than its instruction count (inline data)', fails)


def test_switch_replay(idx, image, fails):
    fixes = tomllib.load(open(os.path.join(T.REPO, 'config', 'switch_tables.toml'), 'rb'))['switch_tables']
    by_bctr = {e['address']: e for e in fixes}
    for fn, bctr, old_n in DECODERS:
        f = next(x for x in idx.funcs if x[1] == fn)
        real_walk = idx.walk

        def truncated(func, _real=real_walk, _n=old_n):
            ins, labels, sws = _real(func)
            for sw in sws:
                if sw['bctr'] in by_bctr:
                    sw['labels'] = sw['labels'][:_n]
            return ins, labels, sws
        idx.walk = truncated
        lines = []
        run = T.Run()
        try:
            cmds = T.recipe_switch(fn, idx, image, run, lines)
        finally:
            idx.walk = real_walk
        text = '\n'.join(lines)
        target = [e for e in fixes if f[0] <= e['address'] < f[0] + f[2]]
        check(len(target) == 1, f'{fn}: exactly one [[switch_tables]] fix inside it', fails)
        if not target:
            continue
        e = target[0]
        want = ', '.join(f'0x{x:08X}' for x in e['labels'])
        check('TOO SHORT' in text and f'labels = [{want}]' in text and f'register = {e["register"]}' in text,
              f'{fn} replayed with {old_n} labels: TOO SHORT, proposes the 11 labels of switch_tables.toml '
              f'(bctr 0x{e["address"]:08X}, r{e["register"]})', fails)
        check(any(c == f'--wwe2k14_dump_words=0x{e["address"]:08X}:13' for c in cmds),
              f'{fn}: dump command --wwe2k14_dump_words=0x{e["address"]:08X}:13 printed', fails)
        check(f'switch r{e["register"]} bctr 0x{e["address"]:08X}' in text,
              f'{fn}: the walk found the switch at bctr 0x{e["address"]:08X} on r{e["register"]}'
              + (f' (PLAYBOOK: 0x{bctr:08X})' if bctr else ''), fails)


def test_switch_audit(idx, image, fails):
    """Every generated switch through the recipe's walker + measurement (a re-audit of PLAYBOOK Session 2).
    Absolute tables (lwzx): the image must give exactly ReXGlue's label count. Relative tables (lbzx/lhzx offsets +
    base): the index bound check (cmplwi on the switch register) must equal the label count."""
    rows = idx.db.execute('select addr, name, size, insns, file, line, has_body from funcs where n_bctr > 0 '
                          'order by addr').fetchall()
    n_sw, kinds, bad = 0, {}, []
    for f in rows:
        _, _, sws = idx.walk(f)
        if not sws:
            continue
        n_sw += len(sws)
        lines = []
        T.recipe_switch(f[1], idx, image, T.Run(), lines)
        for l in lines:
            if not l.startswith('switch r'):
                continue
            rel = 'RELATIVE' in l
            st = l.split('->', 1)[1].strip()
            key = ('relative ' if rel else 'absolute ') + ('ok' if st.startswith('ok') else st.split(';')[0].split(' (')[0])
            kinds[key] = kinds.get(key, 0) + 1
            if 'BOUND CHECK SAYS' in st:
                kinds['absolute: bound check differs from the image'] = kinds.get(
                    'absolute: bound check differs from the image', 0) + 1
            if (not rel and not st.startswith('ok')) or (rel and 'BOUND != LABELS' in st):
                bad.append(f'{f[1]}: {l[:170]}')
            elif 'BOUND CHECK SAYS' in st:
                print(f'INFO  {f[1]}: {l[:200]}')
    print(f'switch audit: {n_sw} switches: {kinds}')
    for b in bad[:10]:
        print('      ' + b)
    check(n_sw == 1159, f'walker finds {n_sw} switches (PLAYBOOK Session 2: 1,159)', fails)
    check(not bad, f'every absolute table measures exactly its ReXGlue labels and every relative table\'s bound '
                   f'check equals its labels ({len(bad)} exceptions)', fails)


def synthetic_log(name, body_lines):
    os.makedirs(os.path.join(OUT, 'logs'), exist_ok=True)
    path = os.path.join(OUT, 'logs', name)
    now = dt.datetime.now()
    ts = lambda k: (now + dt.timedelta(milliseconds=k)).strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]  # noqa: E731
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'[{ts(0)}] [info] [core] [t1] SYNTHETIC triage self-test log (not a game run)\n')
        f.write(f"[{ts(1)}] [info] [core] [t1] Vulkan device 'selftest'\n")
        for k, l in enumerate(body_lines):
            f.write((l if l.startswith(' ') else f'[{ts(2 + k)}] {l}') + '\n')
    return path


def host_range(hm, sym):
    i = hm.names.index(sym)
    j = i + 1
    while j < len(hm.rvas) and hm.rvas[j] <= hm.rvas[i]:  # aliases at the same address
        j += 1
    return hm.rvas[i], hm.rvas[j]


def test_fresh_map(idx, image, fails):
    mp, fresh, reason = T.map_status(T.EXE_DIR, dt.datetime.now())
    check(fresh, f'today\'s map is fresh for a run starting now ({reason})', fails)
    if not fresh:
        return
    hm = T.HostMap(mp)
    exe = os.path.join(T.EXE_DIR, 'wwe2k14.exe')
    # ud2 inside a decoder's host code
    lo, hi = host_range(hm, '__imp__sub_835A9728')
    code = T.exe_bytes_at(exe, lo, hi - lo) or b''
    k = code.find(b'\x0f\x0b')  # first ud2 (a real one: the generated switch default traps)
    ud2 = lo + k if k >= 0 else None
    check(ud2 is not None, f'found a ud2 (0F 0B) in __imp__sub_835A9728 host code (rva 0x{lo:X}-0x{hi:X})', fails)
    if ud2 is not None:
        log = synthetic_log('wwe2k14_901.log', [
            f'[critical] [core] [t1] [wwe2k14][crash] unhandled exception 0xC000001D at wwe2k14.exe+0x{ud2:X} '
            f'(if inside the exe). Backtrace:',
            f'    #00 wwe2k14.exe+0x{ud2:X}  (look up in wwe2k14.map)'])
        v = T.triage(log, T.EXE_DIR, False, None, idx, image)
        text = '\n'.join(v['details'] + v['next'])
        check(v['class'] == 'switch_out_of_range' and v['culprit'] == 'sub_835A9728' and '= ud2' in text
              and '--wwe2k14_dump_words=0x835A97E0:13' in text,
              f'synthetic ud2 at +0x{ud2:X}: {v["class"]}, culprit {v["culprit"]}, ud2 bytes confirmed, '
              f'dump command for bctr 0x835A97E0', fails)
        for d in v['details']:
            if 'switch r' in d or 'map:' in d:
                print('      ' + d[:200])
    # access violation inside a guest function's host code
    lo, hi = host_range(hm, '__imp__sub_82CE3868')
    off = lo + (hi - lo) // 2
    caller = next(x for x in idx.funcs if x[1] == 'sub_82CE9A60')  # PLAYBOOK S3: the Lettering crash's caller
    ret1, ret2 = caller[0] + 8, idx.next_after(caller[0])[0] + 8
    log = synthetic_log('wwe2k14_902.log', [
        '[error] [sys] [t1] Unhandled guest access violation: read of guest 0x000001A4 (host 0x00000001000001A4) '
        'on thread 0xF8000708',
        f'[critical] [core] [t1] [wwe2k14][crash] unhandled exception 0xC0000005 at wwe2k14.exe+0x{off:X} '
        f'(if inside the exe). Backtrace:',
        f'    #00 wwe2k14.exe+0x{off:X}  (look up in wwe2k14.map)',
        '[critical] [core] [t1] [wwe2k14][crash] guest context:',
        '    guest thread id 44 registers (may lag inside the faulting function):',
        '      r28=BCBDBEBF  r29=000000C9  r30=00000000  r31=00000000',
        '      lr=00000000  ctr=82A2CB80  last_indirect_target=00000000',
        f'    guest stack 0x71460000-0x71480000, return addresses: {ret1:08X} {ret2:08X}'])
    v = T.triage(log, T.EXE_DIR, False, None, idx, image)
    text = '\n'.join(v['details'])
    check(v['class'] == 'guest_access_violation' and v['culprit'] == 'sub_82CE3868' and 'r31 = 0' in text
          and 'sub_82CE9A60' in text, f'synthetic AV inside __imp__sub_82CE3868 host code: {v["class"]}, culprit '
          f'{v["culprit"]}, r31 hint, guest return addresses resolved', fails)


def test_missing_replay(idx, fails):
    """Historical replay: each Session 2 FATAL address is registered today (by a hint). Drop it from the index and
    classify it again. Approximate: dropping the entry does not undo what codegen did around the hint. Example:
    PLAYBOOK says 0x8331EA70 sat in an uncovered region 0x8331E9E8-0x8331EA80 (IN_GAP then), but today's codegen
    extends sub_8331E6C8 up to 0x8331EA70, so the replay says GAP_START. Only 0x823F16E0's geometry (one missing
    16-byte slot between registered thunks) is unchanged by its hint, so only that one is asserted; a faithful replay
    of the others needs a scratch codegen without the hint (tools\\codegen_repro)."""
    expect = {0x823F16E0: 'GAP_START'}
    for addr in (0x834C5610, 0x82D22E30, 0x82501EA0, 0x82BB6020, 0x8331EA70, 0x823F16E0):
        i = idx.starts.index(addr) if addr in idx.starts else None
        if i is None:
            check(False, f'0x{addr:08X} registered today', fails)
            continue
        saved = idx.funcs[i]
        del idx.funcs[i]
        del idx.starts[i]
        try:
            lines = []
            _, cls = T.recipe_missing_function(addr, T.Run(), idx, lines)
        finally:
            idx.funcs.insert(i, saved)
            idx.starts.insert(i, addr)
        verdict = next((l.strip() for l in lines if l.startswith('  ACCEPT') or l.startswith('  REJECT')
                        or l.startswith('  IN_GAP')), '')
        msg = f'replay 0x{addr:08X} unregistered: {cls}; {lines[0][len(cls) + 2:][:90]} | {verdict[:80]}'
        if addr in expect:
            check(cls == expect[addr], msg + f' (PLAYBOOK geometry: {expect[addr]})', fails)
        else:
            print('INFO  ' + msg)


def test_hang(idx, image, fails):
    """Synthetic: frames stop at t=10 s while the log goes on for 50 s more, no shutdown -> hang."""
    os.makedirs(os.path.join(OUT, 'logs'), exist_ok=True)
    t0 = dt.datetime.now().replace(microsecond=0)
    ts = lambda s: (t0 + dt.timedelta(seconds=s)).strftime('%Y-%m-%d %H:%M:%S.000')  # noqa: E731
    lines = [f'[{ts(0)}] [info] [core] [t1] SYNTHETIC triage self-test log (not a game run)',
             f"[{ts(0)}] [info] [core] [t1] Vulkan device 'selftest'",
             f'[{ts(10)}] [info] [core] [t2] [wwe2k14][frames] t=10 s, frames 2-600: 60.0 fps, avg 16.7 ms',
             f"[{ts(11)}] [info] [core] [t3] [wwe2k14][file] NtCreateFile 'D:\\PAC\\SELFTEST.PAC' open -> 0x00000000"]
    lines += [f'[{ts(12 + k)}] [info] [apu] [t4] audio tick {k}' for k in range(48)]
    with open(os.path.join(OUT, 'logs', 'wwe2k14_903.log'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    with open(os.path.join(OUT, 'logs', 'frames_903.csv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('frame,t_seconds,frame_ms\n')
        f.write(''.join(f'{n},{(n - 1) / 60:.3f},16.667\n' for n in range(2, 602)))
    v = T.triage(os.path.join(OUT, 'logs', 'wwe2k14_903.log'), T.EXE_DIR, False, None, idx, image)
    check(v['class'] == 'hang' and v['confidence'] == 'high',
          f'synthetic hang (frames stop 50 s before the log ends): {v["class"]} ({v["confidence"]}): '
          f'{(v["details"] or [""])[0][:110]}', fails)


def test_event_identity(fails):
    """Event-1000 entries belong to a run only by process identity (exe path + process start time), never by a time
    window: (a) synthetic entries, (b) every real wwe2k14 entry in this machine's event log maps to exactly one
    release run, the one whose log starts 0-3 s after that process started."""
    exe = os.path.join(T.EXE_DIR, 'wwe2k14.exe')
    t0 = dt.datetime(2026, 1, 1, 12, 0, 0)
    ev = lambda t, ps, path=exe: {'time': t.isoformat(), 't': t, 'pstart': ps.isoformat(timespec='milliseconds') if ps  # noqa: E731
                                  else '', 'path': path, 'module': 'wwe2k14.exe', 'code': '0xc0000005'}
    own = ev(t0 + dt.timedelta(seconds=60), t0 - dt.timedelta(seconds=0.2))
    prev = ev(t0 - dt.timedelta(seconds=2), t0 - dt.timedelta(seconds=70))           # previous run's crash, 2 s before
    cov = ev(t0 + dt.timedelta(seconds=30), t0 - dt.timedelta(seconds=0.1),
             exe.replace('win-amd64-release', 'win-amd64-coverage'))                    # coverage build, same moment
    other = ev(t0 + dt.timedelta(seconds=20), t0 + dt.timedelta(seconds=5), r'C:\x\iruka.exe')
    mine, rest = T.match_app_events([own, prev, cov, other], exe, t0, t0 + dt.timedelta(seconds=59))
    check(mine == [own] and len(rest) == 3, 'synthetic: only the entry of this exe whose process started 0.2 s '
          'before the log is this run\'s (previous run\'s crash 2 s before, coverage build and another exe rejected)',
          fails)
    nostart = ev(t0 + dt.timedelta(seconds=61), None)
    mine, _ = T.match_app_events([nostart], exe, t0, t0 + dt.timedelta(seconds=59), t_next=t0 + dt.timedelta(
        seconds=60.5))
    check(not mine, 'synthetic: an entry without a start time falls back to the window, which ends at the next '
          'run\'s first line', fails)
    allev = T.all_app_events()
    logs = os.path.join(T.EXE_DIR, 'logs')
    real = [a for a in allev['app'] if os.path.normcase(a.get('path') or '') == os.path.normcase(exe)]
    if not real:
        print('INFO  no wwe2k14 entries in this machine\'s event log: real-history check skipped')
        return
    starts = {}
    for p in os.listdir(logs):
        m = re.fullmatch(r'wwe2k14_(\d+)\.log', p)
        if m:
            num, parts, _ = T.find_parts(os.path.join(logs, p), T.EXE_DIR)
            with open(parts[0], encoding='utf-8', errors='replace') as f:
                mm = T.TS_RE.match(f.readline())
            if mm:
                starts[num] = T.parse_ts(mm.group(1))
    owners = {}
    for a in real:
        owners[a['time']] = [n for n, s in starts.items() if T.match_app_events([a], exe, s, s)[0]]
    bad = {t: o for t, o in owners.items() if len(o) != 1}
    check(not bad, f'real event log: each of the {len(real)} release-exe crash entries belongs to exactly one run '
          f'({", ".join(f"{t[11:19]}->{o[0]:03d}" for t, o in sorted(owners.items())[-4:] if len(o) == 1)} ...)'
          + (f'; ambiguous/unmatched: {bad}' if bad else ''), fails)


def test_dll_fault(idx, image, fails):
    """Synthetic: the crash handler prints DLL faults as huge or wrapped 'wwe2k14.exe+' offsets; they must name the
    DLL from the backtrace and never resolve against the exe map."""
    for num, off, mod, code, want in ((904, '1F5A3C2B10', 'rexgpu-xenos.dll', 'C0000005', 'host_access_violation'),
                                      (905, 'FFFFFFF8E1234567', 'rexruntime.dll', 'C0000005', 'host_access_violation'),
                                      (906, '1F5A3C2B10', 'rexgpu-xenos.dll', 'C000001D', 'host_exception')):
        log = synthetic_log(f'wwe2k14_{num}.log', [
            f'[critical] [core] [t7] [wwe2k14][crash] unhandled exception 0x{code} at wwe2k14.exe+0x{off} '
            f'(if inside the exe). Backtrace:',
            '    #00 wwe2k14.exe+0x1000  (look up in wwe2k14.map)',
            '    #01 KERNELBASE.dll+0x10EF83',
            '    #02 ntdll.dll+0x169BA0',
            f'    #03 {mod}+0x123456',
            '    #04 wwe2k14.exe+0x2000  (look up in wwe2k14.map)'])
        v = T.triage(log, T.EXE_DIR, False, None, idx, image)
        text = '\n'.join(v['details'] + v['next'])
        check(v['class'] == want and v['confidence'] == 'medium' and (v['culprit'] or '').startswith(f'{mod}+0x123456')
              and 'outside wwe2k14.exe' in text and 'llvm-objdump' not in text,
              f'synthetic 0x{code} in {mod} (exe offset 0x{off}): {v["class"]} ({v["confidence"]}), culprit '
              f'{(v["culprit"] or "")[:70]}', fails)


def test_guest_av_correlation(idx, image, fails):
    """Synthetic: a guest AV logged by a probe 8 s before a host crash is not the cause; a probe run killed while
    frames kept coming is not a guest AV crash."""
    os.makedirs(os.path.join(OUT, 'logs'), exist_ok=True)
    t0 = dt.datetime.now().replace(microsecond=0)
    ts = lambda s: (t0 + dt.timedelta(seconds=s)).strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]  # noqa: E731
    head = [f'[{ts(0)}] [info] [core] [t1] SYNTHETIC triage self-test log (not a game run)',
            f"[{ts(0)}] [info] [core] [t1] Vulkan device 'selftest'",
            f"[{ts(1)}] [info] [core] [t3] [wwe2k14][file] NtCreateFile 'D:\\PAC\\SELFTEST.PAC' open -> 0x00000000",
            f'[{ts(2)}] [error] [sys] [t9] Unhandled guest access violation: read of guest 0x9F000000 (host '
            f'0x000000019F000000) on thread 0xF8000010',
            f'[{ts(20)}] [info] [core] [t2] [wwe2k14][frames] t=20 s, frames 2-1200: 60.0 fps, avg 16.7 ms']
    lines = head + [f'[{ts(21 + k)}] [info] [apu] [t4] audio tick {k}' for k in range(8)]
    crash = [f'[{ts(29.5)}] [critical] [core] [t1] [wwe2k14][crash] unhandled exception 0xC0000005 at '
             f'wwe2k14.exe+0x1000 (if inside the exe). Backtrace:', '    #00 wwe2k14.exe+0x1000  (look up in wwe2k14.map)']
    for num, body in ((907, lines + crash), (908, lines)):
        with open(os.path.join(OUT, 'logs', f'wwe2k14_{num}.log'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(body) + '\n')
        with open(os.path.join(OUT, 'logs', f'frames_{num}.csv'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('frame,t_seconds,frame_ms\n' + ''.join(f'{n},{(n - 1) / 60:.3f},16.667\n' for n in range(2, 1740)))
    v = T.triage(os.path.join(OUT, 'logs', 'wwe2k14_907.log'), T.EXE_DIR, False, None, idx, image)
    check(v['class'] == 'host_access_violation' and any('not tied to the end of the run' in s for s in v['secondary']),
          f'synthetic host crash 27 s after a probe guest AV: {v["class"]} ({v["confidence"]}); the AV is a '
          f'secondary note', fails)
    v = T.triage(os.path.join(OUT, 'logs', 'wwe2k14_908.log'), T.EXE_DIR, False, None, idx, image)
    check(v['class'] not in ('guest_access_violation', 'host_access_violation'),
          f'synthetic probe run killed while frames kept coming: {v["class"]} ({v["confidence"]}), not a guest AV',
          fails)


def test_edge_cases(idx, image, fails):
    """Empty log -> no_log; a folder with a map but no exe -> no exception, map not trusted."""
    p = os.path.join(OUT, 'logs', 'wwe2k14_909.log')
    open(p, 'w').close()
    v = T.triage(p, T.EXE_DIR, False, None, idx, image)
    check(v['class'] == 'no_log', f'empty log: {v["class"]}', fails)
    d = os.path.join(OUT, 'noexe')
    os.makedirs(os.path.join(d, 'logs'), exist_ok=True)
    for header in (' wwe2k14\n\n Timestamp is 7ac80aa5 (synthetic)\n', ' wwe2k14\n'):
        with open(os.path.join(d, 'wwe2k14.map'), 'w') as f:
            f.write(header)
        with open(os.path.join(d, 'logs', 'wwe2k14_200.log'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('[2026-01-01 12:00:00.000] [info] [core] [t1] SYNTHETIC (not a game run)\n'
                    '[2026-01-01 12:01:00.000] [info] [core] [t1] Execution complete\n')
        try:
            v = T.triage('200', d, False, None, idx, image)
            ok = v['class'] == 'clean_exit' and not v['map']['fresh']
            msg = v['map']['reason'][:90]
        except Exception as e:  # noqa: BLE001
            ok, msg = False, f'{type(e).__name__}: {e}'
        check(ok, f'map without an exe ({"no timestamp" if "Timestamp" not in header else "newer than the run"}): '
              f'{msg}', fails)


def test_dump_run(idx, image, fails):
    """The Session 2 table-dump run 012 (66 tables, PLAYBOOK): every dumped range is matched to its switch and
    re-measured from the live words."""
    log = os.path.join(T.EXE_DIR, 'logs', 'wwe2k14_012.log')
    if not os.path.exists(log):
        print('INFO  run 012 log not present: dump-run check skipped')
        return
    v = T.triage(log, T.EXE_DIR, False, None, idx, image)
    summary = next((d for d in v['details'] if 'dumped tables by result' in d), '')
    check(v['class'] == 'clean_exit' and "{'ok': 66}" in summary,
          f'run 012 (dump run): {v["class"]}, {summary.strip()}', fails)


def test_cross_build_signature(idx, image, fails):
    """Session 3: runs 050/051 hit the Lettering crash at +0x33031BD after a relink, 030-049 at +0x3303185. The
    guest-side signature (guest AV address, lr/ctr) must link 050 to them; the host-offset match cannot."""
    logs = os.path.join(T.EXE_DIR, 'logs')
    if not all(os.path.exists(os.path.join(logs, f'wwe2k14_{n:03d}.log')) for n in (49, 50, 51)):
        print('INFO  runs 049-051 not present: cross-build signature check skipped')
        return
    v = T.triage(os.path.join(logs, 'wwe2k14_050.log'), T.EXE_DIR, False, None, idx, image)
    line = next((d for d in v['details'] if 'guest-side crash signature' in d), '')
    check(all(f'{n:03d}' in line.split('folder')[-1] for n in (30, 45, 48, 49, 51)),
          f'run 050 (relinked, other offset) linked by guest-side signature to 030, 045, 048, 049, 051: '
          f'{line.split("in ")[-1][:80]}', fails)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--log', default=None, help='a run log with [scan] lines (default: the newest release log)')
    ap.add_argument('--tsv', default=None, help='classify_candidates.py output for the same log')
    a = ap.parse_args()
    idx, image = T.CodeIndex(), T.Image()
    fails = []
    log = a.log
    if not log:
        logs = os.path.join(T.EXE_DIR, 'logs')
        cands = sorted((p for p in os.listdir(logs) if re.fullmatch(r'wwe2k14_\d+\.log', p)),
                       key=lambda p: int(p[8:-4]))
        log = os.path.join(logs, cands[-1])
    test_classifier(log, a.tsv, idx, fails)
    test_missing_replay(idx, fails)
    test_switch_replay(idx, image, fails)
    test_switch_audit(idx, image, fails)
    test_fresh_map(idx, image, fails)
    test_hang(idx, image, fails)
    test_event_identity(fails)
    test_dll_fault(idx, image, fails)
    test_guest_av_correlation(idx, image, fails)
    test_edge_cases(idx, image, fails)
    test_dump_run(idx, image, fails)
    test_cross_build_signature(idx, image, fails)
    print(f'\n{"ALL PASSED" if not fails else f"{len(fails)} FAILED"}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
