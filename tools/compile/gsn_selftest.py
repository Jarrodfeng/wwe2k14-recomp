#!/usr/bin/env python3
"""gsn selftest (S106 lane GSN): the follow-ups of GSMAIN, each row with a planted twin (docs/research/s106/review_GSMAIN.md N7, N14 + the re-review notes 1-6).

  py -I tools\\compile\\gsn_selftest.py [--root .] [--mutant]      --mutant = the same rows against the OLD behaviour (each must be caught)

Rows (`ok` / `FAIL`; exit 0 = all as expected, 1 = a row wrong, 3 = BLOCKED, main's configured build missing):
  N7a  a CMakeLists.txt that adds / removes a define (the tree vs its base): game_syntax --changed adds the units whose flags moved (never NOTHING CHECKED), capped
  N7b  an include dir only in the base's CMakeLists: the units it moves are picked, and a probe that needs the dir FAILS with the tree's flags (passes with the base's)
  N7c  a comment-only CMakeLists.txt change: no unit is added (NOTHING CHECKED rc 0 stays the truth)
  N14  keep_stream_off_link.py --syntax reads THIS tree's flags (a define only this tree's CMakeLists gives reaches the compile of its --extra probe)
  note1 P1's reference -D list = every non-INTERNAL / STATIC entry of main's cache (a planted cache variable reaches it)
  note2 P1 with an empty reference = NOT JUDGED, with an empty syntax_cfg side = FAIL
  note3 cmake_state: build.ninja older than ANY real input (VERSION, a keep_stream list, rexglue.cmake) gives the reason
  note4 prune sweeps `<key>.del.<pid>` / `<key>.lock.dead.<pid>` older than 1 h (nothing else, nothing younger)
  note5 an OSError on os.replace(<key>.lines) is Unavailable (a loud fallback), never a traceback
  N5 latent  scan allowlists: kinds per OUTSIDE_VARS entry, ${CMAKE_CURRENT_LIST_DIR} per file, execute_process / file(SIZE|SHA*|COPY|REAL_PATH|...) / IS_DIRECTORY / IS_NEWER_THAN fail closed
"""
# entry: commands
import argparse
import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault('GAME_SYNTAX_NO_CACHE', '1')   # twins are scratch units: no store writes
import game_syntax as gs   # noqa: E402
import syntax_cfg as cfg   # noqa: E402
import proc as _proc   # noqa: E402

CRT_BLOCK = '    target_compile_definitions(wwe2k14 PRIVATE _CRT_SECURE_NO_WARNINGS)\n'
MUTANT = False


def _cm(root):
    return (cfg.WorkTree(root).read('CMakeLists.txt') or b'').decode('utf-8', 'replace').replace('\r\n', '\n')


def _run_changed(root, main, cm_bytes, scratch, name, build='off'):
    """game_syntax.run_changed in-process, base HEAD, changed set = [CMakeLists.txt], the tree's CMakeLists.txt replaced by `cm_bytes` (the override env): (rc, output)."""
    bp = os.path.join(scratch, name + '.cmake.txt')
    open(bp, 'wb').write(cm_bytes)
    js = os.path.join(scratch, name + '.json')
    json.dump({'CMakeLists.txt': bp}, open(js, 'w'))
    ns = argparse.Namespace(changed='HEAD', build=build, files='', no_twin=False)
    old_env, old_ch = os.environ.get(cfg.OVERRIDE_ENV), gs.changed_files
    os.environ[cfg.OVERRIDE_ENV] = js
    gs.changed_files = lambda _r, _b: ['CMakeLists.txt']
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            rc = gs.run_changed(ns, root, main)
    finally:
        gs.changed_files = old_ch
        if old_env is None:
            os.environ.pop(cfg.OVERRIDE_ENV, None)
        else:
            os.environ[cfg.OVERRIDE_ENV] = old_env
    return rc, buf.getvalue()


def n7a(root, main, scratch):
    base = _cm(root)
    if CRT_BLOCK not in base:
        return ['N7a cannot be built: CMakeLists.txt has no `%s` block' % CRT_BLOCK.strip()]
    rc, out = _run_changed(root, main, base.replace(CRT_BLOCK, '').encode(), scratch, 'n7a')
    p = []
    m = re.search(r'note: (\d+) unit\(s\) added because their flags changed', out)
    if not m:
        p.append('N7a a removed define moved no unit into the check (the old behaviour): rc %s, %s' % (rc, out.strip().splitlines()[-1:]))
    elif int(m.group(1)) > gs.CMAKE_UNITS_CAP:
        p.append('N7a %s units added: over the cap %d' % (m.group(1), gs.CMAKE_UNITS_CAP))
    if 'NOTHING CHECKED' in out:
        p.append('N7a still NOTHING CHECKED')
    if not re.search(r'^(ok  |FAIL) src/', out, re.M):
        p.append('N7a no unit was parsed')
    return p


def _probe_ok(root, main, lines, path):
    fl = gs.flags_for(lines, main.replace('\\', '/'), 'src/start_settings.cpp')
    return gs.compile_unit(root, main, fl, path, 'probe')[1] == 0


def n7b(root, main, scratch):
    inc = os.path.join(scratch, 'n7b_inc').replace('\\', '/')
    os.makedirs(inc, exist_ok=True)
    open(os.path.join(inc, 'gsn_planted_hdr.h'), 'w').write('#define GSN_PLANTED_HDR 1\n')
    probe = os.path.join(scratch, 'n7b_probe.cpp')
    open(probe, 'w').write('#include "gsn_planted_hdr.h"\nint gsn_probe;\n')
    base = _cm(root)
    anchor = CRT_BLOCK
    if anchor not in base:
        return ['N7b cannot be built: anchor block missing']
    with_dir = base.replace(anchor, anchor + '    target_include_directories(wwe2k14 PRIVATE "%s")\n' % inc)
    t_with = cfg.WorkTree(root, {'CMakeLists.txt': with_dir.encode()})
    t_without = cfg.WorkTree(root, {'CMakeLists.txt': base.encode()})
    lw = cfg.commands(t_with, main, 'off')    # the "base": has the dir
    lwo = cfg.commands(t_without, main, 'off')    # the checked tree: dropped it
    pick, total, note = gs.cmake_flag_units(lwo, lw, main)
    p = []
    if not pick:
        return ['N7b a dropped include dir moved no unit (%s)' % note]
    if len(pick) > gs.CMAKE_UNITS_CAP or len(pick) > gs.CMAKE_UNITS_PER_SIG * 3:
        p.append('N7b picked %d units (cap %d per signature)' % (len(pick), gs.CMAKE_UNITS_PER_SIG))
    if not _probe_ok(root, main, lw, probe):
        p.append('N7b control: a probe that needs the dir fails even with the dir in the flags')
    if _probe_ok(root, main, lwo, probe):
        p.append('N7b a probe that needs the dropped dir PASSES with the tree\'s flags')
    if total < len(pick):
        p.append('N7b total < picked')
    return p


def n7c(root, main, scratch):
    rc, out = _run_changed(root, main, (_cm(root) + '\n# gsn planted: a comment moves no flag\n').encode(), scratch, 'n7c')
    p = []
    if 'unit(s) added' in out:
        p.append('N7c a comment-only CMakeLists change added units: %s' % [l for l in out.splitlines() if 'added' in l][:1])
    if rc != 0 or 'NOTHING CHECKED' not in out:
        p.append('N7c expected NOTHING CHECKED rc 0, got rc %s: %s' % (rc, out.strip().splitlines()[-1:]))
    return p


def n14(root, main, scratch):
    """The script from this tree gets the tree's define (--extra probe #errors without it); the OLD script (HEAD's) does not."""
    base = _cm(root)
    if CRT_BLOCK not in base:
        return ['N14 cannot be built']
    cm = os.path.join(scratch, 'n14_cm.txt')
    open(cm, 'w', newline='').write(base.replace(CRT_BLOCK, CRT_BLOCK + '    target_compile_definitions(wwe2k14 PRIVATE GSN_PLANTED_N14=1)\n'))
    js = os.path.join(scratch, 'n14.json')
    json.dump({'CMakeLists.txt': cm}, open(js, 'w'))
    probe = os.path.join(scratch, 'n14_probe.cpp')
    open(probe, 'w').write('#ifndef GSN_PLANTED_N14\n#error planted: this tree\'s own define did not reach the compile\n#endif\nint gsn_n14;\n')
    env = dict(os.environ, **{cfg.OVERRIDE_ENV: js})
    script = os.path.join(root, 'tools', 'scene', 'keep_stream_off_link.py')
    if MUTANT:   # the old script: main's out\build flags
        old = subprocess.run(['git', '-C', root, 'show', 'ada7fe72c^:tools/scene/keep_stream_off_link.py'], capture_output=True).stdout   # the script before N14 (ada7fe72c, an ancestor of s106; the branch name s106 carries N14 once merged)
        script = os.path.join(root, 'tools', 'scene', '_gsn_old_link.py')
        open(script, 'wb').write(old)
    try:
        r = _proc.run([sys.executable, '-I', script, '--root', root, '--syntax', '--keep', '0', '--only', 'src/start_settings.cpp', '--extra', probe,
                       '--out', os.path.join(scratch, 'n14_out'), '-j', '2'], timeout=600, env=env)
    finally:
        if MUTANT:
            os.remove(script)
    out = (r.stdout or '') + (r.stderr or '')
    p = []
    if r.returncode != 0 or 'planted: this tree' in out:
        p.append('N14 the probe needing the tree\'s own define did not compile (rc %s): %s' % (r.returncode, [l for l in out.splitlines() if 'error' in l.lower() or 'FAIL' in l][:2]))
    return p


def note1(root, main, scratch):
    """P1's reference -D list comes from every non-INTERNAL cache entry, not plan()'s CARRY / WWE2K14_* filter."""
    pl = cfg.plan(cfg.WorkTree(root), main, 'off')[1]
    pl['cache']['GSN_PLANTED_CACHE_VAR'] = ('BOOL', 'ON')
    ra = cfg.reference_args(pl)
    p = []
    if 'GSN_PLANTED_CACHE_VAR' in pl['args']:
        p.append('note1 control: plan()\'s own filter carries the planted variable (the twin cannot tell the lists apart)')
    if 'GSN_PLANTED_CACHE_VAR' not in ra:
        p.append('note1 the reference -D list misses a cache variable plan()\'s filter does not carry')
    return p


def note2(root, main, scratch):
    sst = _sst()
    p = []
    orig_ref, orig_cmd = cfg.reference_lines, cfg.commands
    try:
        cfg.reference_lines = lambda *a, **k: []
        probs, _notes, judged = sst.p1_parity(main, scratch)
        st = sst.p1_status(probs, judged)
        if st != 'NOT JUDGED':
            p.append('note2 an EMPTY reference gave P1 %s (want NOT JUDGED)' % st)
        cfg.reference_lines = orig_ref
        cfg.commands = lambda *a, **k: []
        probs, _notes, judged = sst.p1_parity(main, scratch)
        st = sst.p1_status(probs, judged)
        if st != 'FAIL':
            p.append('note2 an EMPTY syntax_cfg side gave P1 %s (want FAIL)' % st)
    finally:
        cfg.reference_lines, cfg.commands = orig_ref, orig_cmd
    return p


def note3(root, main, scratch):
    """cmake_state tests EVERY real input's mtime, not CMakeLists.txt's alone."""
    d = os.path.join(scratch, 'n3main')
    shutil.rmtree(d, ignore_errors=True)
    bd = os.path.join(d, gs.BUILD_DIRS['on'])
    os.makedirs(bd)
    for f in cfg.REAL_INPUTS:
        os.makedirs(os.path.dirname(os.path.join(d, f)) or d, exist_ok=True)
        open(os.path.join(d, f), 'w').write('x')
    ninja = os.path.join(bd, 'build.ninja')
    open(ninja, 'w').write('n')
    t = time.time()
    for f in cfg.REAL_INPUTS:
        os.utime(os.path.join(d, f), (t - 100, t - 100))
    os.utime(ninja, (t - 50, t - 50))
    gs.cmake_content_unchanged_since = lambda *a, **k: False   # the scratch folder sits inside a git repo: its reflog would call every file unchanged
    p = []
    if gs.cmake_state(d, d, ['on']):
        p.append('note3 control: nothing newer than build.ninja but a reason was given: %s' % gs.cmake_state(d, d, ['on']))
    for f in ('VERSION', 'tools/scene/keep_stream_off_sources.txt', 'generated/rexglue.cmake'):
        os.utime(os.path.join(d, f), (t, t))
        why = gs.cmake_state(d, d, ['on'])
        if not any(('older than ' + f) in w for w in why):
            p.append('note3 a %s newer than build.ninja gave no "older than" reason: %s' % (f, why))
        os.utime(os.path.join(d, f), (t - 100, t - 100))
    return p


def note4(root, main, scratch):
    store = os.path.join(scratch, 'n4store')
    shutil.rmtree(store, ignore_errors=True)
    os.makedirs(store)
    k = 'a' * 24
    old, new = time.time() - 7200, time.time() - 60
    mk = {k + '.del.11': (True, old), k + '.lock.dead.12': (False, old), 'b' * 24 + '.del.13': (True, new), 'b' * 24 + '.lock.dead.14': (False, new), 'c' * 24 + '.lockx': (False, old),
          'notakey.del.15': (True, old)}
    for n, (isdir, t) in mk.items():
        pth = os.path.join(store, n)
        if isdir:
            os.makedirs(pth)
            open(os.path.join(pth, 'f'), 'w').write('x')
        else:
            open(pth, 'w').write('x')
        os.utime(pth, (t, t))
    cfg.prune(store)
    left = set(os.listdir(store))
    p = []
    for n in (k + '.del.11', k + '.lock.dead.12'):
        if n in left:
            p.append('note4 %s (2 h old) was not swept' % n)
    for n in ('b' * 24 + '.del.13', 'b' * 24 + '.lock.dead.14', 'c' * 24 + '.lockx', 'notakey.del.15'):
        if n not in left:
            p.append('note4 %s should have stayed' % n)
    return p


def note5(root, main, scratch):
    """An OSError from os.replace(tmp, <key>.lines) is Unavailable, never a traceback."""
    cm = (_cm(root) + '\n# gsn note5 %f\n' % time.time()).encode()   # a key nobody has built: forces the configure path
    real_replace = os.replace

    def bad(a, b, *x, **k):
        if str(b).endswith('.lines'):
            raise PermissionError('planted: access denied')
        return real_replace(a, b, *x, **k)
    os.replace = bad
    try:
        cfg.commands(cfg.WorkTree(root, {'CMakeLists.txt': cm}), main, 'off')
        return ['note5 a failing os.replace(.lines) produced lines']
    except cfg.Unavailable:
        return []
    except Exception as e:   # noqa: BLE001
        return ['note5 wrong failure kind: %s: %s' % (type(e).__name__, str(e)[:120])]
    finally:
        os.replace = real_replace


def note_n5(root, main, scratch):
    """The latent N5 allowlists: kinds per OUTSIDE_VARS entry, ${CMAKE_CURRENT_LIST_DIR} per file, execute_process / file(SIZE|SHA*|...) / IS_DIRECTORY / IS_NEWER_THAN fail closed."""
    p = []
    cs = '${CMAKE_CURRENT_SOURCE_DIR}'
    ld = '${CMAKE_CURRENT_LIST_DIR}'
    cases = [
        ('CMakeLists.txt', 'execute_process(COMMAND python zz.py OUTPUT_VARIABLE X)\n', True, 'execute_process of an unknown command'),
        ('CMakeLists.txt', 'execute_process(COMMAND git -C "%s" rev-parse HEAD)\n' % cs, False, 'execute_process git (known)'),
        ('CMakeLists.txt', 'file(SHA256 "%s/tools/zz/a.txt" H)\n' % cs, True, 'file(SHA256) of an unlisted file'),
        ('CMakeLists.txt', 'file(SIZE "%s/tools/zz/a.txt" H)\n' % cs, True, 'file(SIZE)'),
        ('CMakeLists.txt', 'file(TIMESTAMP "%s/tools/zz/a.txt" H)\n' % cs, True, 'file(TIMESTAMP)'),
        ('CMakeLists.txt', 'file(COPY "%s/tools/zz/a.txt" DESTINATION "x")\n' % cs, True, 'file(COPY)'),
        ('CMakeLists.txt', 'file(REAL_PATH "%s/tools/zz/a.txt" H)\n' % cs, True, 'file(REAL_PATH)'),
        ('CMakeLists.txt', 'if(IS_DIRECTORY "%s/zz_unlisted")\nendif()\n' % cs, True, 'IS_DIRECTORY'),
        ('CMakeLists.txt', 'if(a IS_NEWER_THAN b)\nendif()\n', True, 'IS_NEWER_THAN'),
        ('CMakeLists.txt', 'file(READ "${WWE2K14_FFX_DLL}" X)\n', True, 'a READ through a var classified for EXISTS only'),
        ('CMakeLists.txt', 'if(EXISTS "${WWE2K14_FFX_DLL}")\nendif()\n', False, 'EXISTS through the same var'),
        ('CMakeLists.txt', 'file(STRINGS "%s/VERSION" V)\n' % ld, False, 'LIST_DIR at the root = the root'),
        ('tools/x.cmake', 'file(STRINGS "%s/VERSION" V)\n' % ld, True, 'LIST_DIR in tools/ = tools/VERSION, not the real VERSION'),
        ('third_party/audio/opus/opus_sources.cmake', 'file(STRINGS "%s/sources.txt" V)\n' % ld, False, 'LIST_DIR next to the real opus list'),
        ('third_party/audio/opus/opus_sources.cmake', 'file(STRINGS "%s/zz.txt" V)\n' % ld, True, 'LIST_DIR next to an unlisted file'),
    ]
    for rel, text, want_problem, name in cases:
        got = cfg.scan_texts({rel: text})
        if bool(got) != want_problem:
            p.append('N5 latent: %s: %s (%s)' % (name, 'not reported' if want_problem else 'reported though fine', got[:1]))
    return p


def _sst():
    import syntax_cfg_selftest as sst
    return sst


def rows(root, main, scratch):
    res = {}
    for name, fn in (('N7a define moves units', n7a), ('N7b include dir moves units + probe fails', n7b), ('N7c comment adds none', n7c), ('N14 off_link reads the tree', n14),
                     ('note1 reference -D list from the whole cache', note1), ('note2 P1 empty side', note2), ('note3 cmake_state every input mtime', note3),
                     ('note4 prune sweeps .del / .lock.dead', note4), ('note5 OSError is Unavailable', note5), ('N5 latent allowlists', note_n5)):
        try:
            res[name] = fn(root, main, scratch)
        except Exception as e:   # noqa: BLE001
            res[name] = ['%s raised %s: %s' % (name, type(e).__name__, str(e)[:300])]
    return res


def main():
    global MUTANT
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--mutant', action='store_true', help='run the rows against the OLD behaviour: every N7a / N7b / N14 row must fail')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = gs.main_checkout(root)
    if not main_dir or not os.path.isfile(os.path.join(main_dir, gs.BUILD_DIRS['on'], 'CMakeCache.txt')):
        print('BLOCKED: the main checkout / its configured out\\build was not found (%s): nothing judged' % main_dir)
        return 3
    scratch = os.path.join(root, 'out', 'gsn_selftest')
    os.makedirs(scratch, exist_ok=True)
    t0 = time.time()
    if a.mutant:
        MUTANT = True
        gs.cmake_flag_units = lambda *x, **k: ([], 0, 'mutant: the old behaviour adds nothing')
    res = rows(root, main_dir, scratch)
    bad = 0
    for name, ps in res.items():
        must_fail = a.mutant and name.split()[0] in ('N7a', 'N7b', 'N14')
        okrow = bool(ps) if must_fail else not ps
        print('%s %s%s' % ('ok  ' if okrow else 'FAIL', name, ' (mutant caught)' if must_fail and ps else ''))
        for q in ps[:4]:
            print('       ' + q)
        bad += not okrow
    print('gsn selftest%s: %s (%.0f s)' % (' --mutant' if a.mutant else '', 'FAIL' if bad else 'ok', time.time() - t0))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
