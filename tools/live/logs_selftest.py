"""GTB (B0 / B4): offline test that the live tools and goto read the log folder of the exe being driven. 0 game runs.

  py -I tools/live/logs_selftest.py

1. a fake stream-free (OFF) build folder with a run log and an event bus file; WWE2K14_EXE_DIR points at it;
2. resolve a goto plan (wm30:1) and confirm its expect_log step reads that OFF folder (live.run_log, the one path the executor opens);
   live.read_batches, newest_run_with_batches and events.Events read it too;
3. the planted twin: the stream folder hard-coded (the old behaviour) must FAIL the same check;
4. a missing log raises an error naming the path (run log, event bus folder, event bus file), never a silent wait;
5. the exe folder rules (exe_folder_of, the out/live/exe_dir.txt memory, WWE2K14_EXE_DIR first) and B4: exe_defines finds the
   stage-view cvar name in a stream exe and not in an OFF one, so live.start leaves the flag out there.
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'goto'))
import live  # noqa: E402
import events  # noqa: E402
import address  # noqa: E402
import resolve  # noqa: E402

fails = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)
    return ok


def raises(fn, cls, needle):
    try:
        fn()
    except cls as x:
        return needle in str(x)
    except Exception:
        return False
    return False


def same_dir(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def plan_expect_text():
    plan = resolve.resolve(address.parse('wm30:1'), resolve.Context())
    steps = [s for s in plan.steps if s['op'] == 'expect_log']
    return steps[0]['text'] if steps else None


def reads_off(off_logs, run):
    """What the executor's expect_log step does: resolve the plan, take its text, open live.run_log(run) and find the text there."""
    text = plan_expect_text()
    if not text:
        return False
    path = live.run_log(run)
    if not same_dir(os.path.dirname(path), off_logs):
        return False
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return any('[wm30]' in ln and text in ln for ln in f)
    except OSError:
        return False


tmp = tempfile.mkdtemp(prefix='gtb_logs_')
off = os.path.join(tmp, 'win-amd64-release-off-v3')
logs = os.path.join(off, 'logs')
os.makedirs(logs)
RUN = 42
with open(os.path.join(logs, 'wwe2k14_%03d.log' % RUN), 'w', encoding='utf-8') as f:
    f.write('[2026-11-19 12:00:00.000] [i] [wm30] [t1] %s7 (fake)\n' % plan_expect_text())
    f.write('[2026-11-19 12:00:01.000] [i] [wwe2k14][live] batch 1> screenshot x\n')
with open(os.path.join(logs, 'events_%03d.jsonl' % RUN), 'w', encoding='utf-8') as f:
    f.write('{"f": 1, "kind": "seq", "v": 9}\n')
with open(os.path.join(off, 'wwe2k14.exe'), 'wb') as f:
    f.write(b'MZ fake OFF exe, no stage cvar here')
stream = os.path.join(tmp, 'win-amd64-release')
os.makedirs(stream)
with open(os.path.join(stream, 'wwe2k14.exe'), 'wb') as f:
    f.write(b'MZ ... wwe2k14_stage_view ...')

off_sv = os.path.join(tmp, 'win-amd64-release-off-sv')   # S103 SVO: an OFF exe that holds the host's stage-view cvar (and not the stream build's name)
os.makedirs(off_sv)
with open(os.path.join(off_sv, 'wwe2k14.exe'), 'wb') as f:
    f.write(b'MZ ... wwe2k14_rdr_stage_view ...')

saved_env = os.environ.get('WWE2K14_EXE_DIR')
saved_file = live.EXE_DIR_FILE
saved_exe_dir = live.exe_dir
try:
    os.environ['WWE2K14_EXE_DIR'] = off
    # 1-2. the OFF folder is read
    check(same_dir(live.logs_dir(), logs), 'logs_dir() is the OFF folder given by WWE2K14_EXE_DIR')
    check(reads_off(logs, RUN), "the wm30:1 plan's expect_log reads the OFF folder (text found in the OFF run log)")
    b, _, _ = live.read_batches(RUN)
    check(1 in b, 'read_batches reads the OFF run log')
    check(live.newest_run_with_batches() == RUN, 'newest_run_with_batches lists the OFF folder')
    ev = events.Events(RUN)
    check(same_dir(os.path.dirname(ev.path), logs) and [e['kind'] for e in ev.read(True)] == ['seq'], 'Events reads the OFF event bus file')
    check(events.newest_run() == RUN, 'events.newest_run looks in the OFF folder')

    # 3. planted twin: the stream folder hard-coded must fail the same check
    live.exe_dir = lambda: live.EXE_DIR
    check(not reads_off(logs, RUN), 'TWIN (stream folder hard-coded) fails the OFF check')
    live.exe_dir = saved_exe_dir

    # 4. a missing log fails loudly, with the path
    check(raises(lambda: live.require_log(999), FileNotFoundError, 'wwe2k14_999.log'), 'a missing run log raises, naming the path')
    check(raises(lambda: live.read_batches(999), FileNotFoundError, 'wwe2k14_999.log'), 'read_batches of a missing log raises')
    saved_wait = events.MISSING_FILE_S
    events.MISSING_FILE_S = 0.2
    check(raises(lambda: events.Events(999).wait({'kind': 'x'}, 5.0), events.MissingLog, 'events_999.jsonl'),
          'Events.wait on a missing event bus file raises (naming it) instead of waiting out the timeout')
    events.MISSING_FILE_S = saved_wait
    os.environ['WWE2K14_EXE_DIR'] = os.path.join(tmp, 'no_such_build')
    check(raises(lambda: events.Events(RUN).read(), events.MissingLog, 'no_such_build'), 'Events.read in a missing folder raises, naming it')
    check(raises(lambda: live.require_log(RUN), FileNotFoundError, 'no_such_build'), 'run log in a missing folder raises, naming it')

    # 5. folder rules and B4
    del os.environ['WWE2K14_EXE_DIR']
    live.EXE_DIR_FILE = os.path.join(tmp, 'exe_dir.txt')
    check(same_dir(live.exe_dir(), live.EXE_DIR), 'no env, no memory file: the stream build folder')
    with open(live.EXE_DIR_FILE, 'w') as f:
        f.write(off)
    check(same_dir(live.exe_dir(), off), 'the folder live.start remembered is used')
    os.environ['WWE2K14_EXE_DIR'] = stream
    check(same_dir(live.exe_dir(), stream), 'WWE2K14_EXE_DIR wins over the memory file')
    del os.environ['WWE2K14_EXE_DIR']
    check(same_dir(live.exe_folder_of(os.path.join(off, 'wwe2k14.exe')), off), 'exe_folder_of(path) is the exe folder')
    check(same_dir(live.exe_folder_of(''), live.EXE_DIR), "exe_folder_of('') is the stream build")
    check(live.exe_folder_of('coverage').endswith('win-amd64-coverage-off'), "exe_folder_of('coverage') is the trace build")
    check(live.exe_defines(os.path.join(stream, 'wwe2k14.exe'), 'wwe2k14_stage_view'), 'B4: a stream exe holding the cvar gets the stage-view flag')
    check(not live.exe_defines(os.path.join(off, 'wwe2k14.exe'), 'wwe2k14_stage_view'), 'B4: an OFF exe without it does not')
    check(live.exe_defines(os.path.join(off_sv, 'wwe2k14.exe'), 'wwe2k14_rdr_stage_view') and not live.exe_defines(os.path.join(off_sv, 'wwe2k14.exe'), 'wwe2k14_stage_view') and not live.exe_defines(os.path.join(stream, 'wwe2k14.exe'), 'wwe2k14_rdr_stage_view'), 'SVO: the OFF host cvar is told apart from the stream build one (neither name is a substring of the other)')
finally:
    live.EXE_DIR_FILE = saved_file
    live.exe_dir = saved_exe_dir
    if saved_env is None:
        os.environ.pop('WWE2K14_EXE_DIR', None)
    else:
        os.environ['WWE2K14_EXE_DIR'] = saved_env

print('%d failed' % len(fails) if fails else 'all passed')
sys.exit(1 if fails else 0)
