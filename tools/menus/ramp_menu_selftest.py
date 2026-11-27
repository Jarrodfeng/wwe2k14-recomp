"""S111 RAMPMENU: offline checks of the RAMP CAMERA row (docs/research/s111/RAMPMENU.md). No game runs.

    py -I tools/menus/ramp_menu_selftest.py

1. tools/menus/ramp_menu_test.cpp COMPILED with clang++ against src/ramp_menu_core.h and run (the row after the seven stock rows, refusals,
   the question's answer, the settings key).
2. PLANTED TWINS: four faults written into copies of the header (a text replacement that must hit exactly once), each of which the same
   unit must FAIL: the row written over CAMERA CUTS (a stock row's index), the count not raised, the answer swapped, the value not persisted.
3. Wiring pins: the hook lines in src/photo_mode.h, src/ramp_menu.h, src/gfx_menu.h, src/wwe2k14_app.h and the launcher's key.
Exit 0 = all passed.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

problems = []


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('  FAIL: ' + msg)


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError("no clang++ found (tools\\env.ps1 puts the project's LLVM on PATH): the unit cannot run")


sys.path.insert(0, os.path.join(REPO, 'tools', 'split'))
from read_unit import read_text  # a split file is read with its parts expanded (tools/split/read_unit.py; text mode: '\n' line ends)


def read(*parts):
    return read_text(os.path.join(REPO, *parts), errors='strict')


def build_and_run(header_text, tmp, tag):
    """Compiles ramp_menu_test.cpp against `header_text` as ramp_menu_core.h (a copy under tmp: a planted twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'ramp_menu_core.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'ramp_menu_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'ramp_menu_test.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('ramp_menu_test did not compile: %s' % (r.stdout + r.stderr)[-800:])
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.strip()


TWINS = {
    'the row written over CAMERA CUTS (index 0)': ('uint8_t* row = m + kMenuRows + kRowSize * kRampAction;',
                                                  'uint8_t* row = m + kMenuRows + kRowSize * 0;'),
    'the count not raised': ('PutBe32(m + kMenuCount, kStockRows + 1);', 'PutBe32(m + kMenuCount, kStockRows);'),
    'the answer swapped': ('answer == 0 ? true : answer == 1 ? false : current', 'answer == 0 ? false : answer == 1 ? true : current'),
    'the value not persisted (always OFF)': ('const char* word = value ? "true" : "false";', 'const char* word = "false";'),
    'the game handed an answer it writes CAMERA CUTS for': ('constexpr uint32_t kAnswerNoWrite = 2;', 'constexpr uint32_t kAnswerNoWrite = 0;'),
    'the file read in replay playback': ('  if (s.replay_playback) return StartFrom::kRecording;\n', ''),
    'the file read over a command-line value': ('  if (s.command_line) return StartFrom::kCommandLine;\n', ''),
    'a +dev build reads the player\'s file by itself': (
        '  if (s.dev_build && !s.settings_file_given && !s.use_launcher_settings) return StartFrom::kNotAllowed;\n', ''),
    'the file read but its value ignored (always ON)': ('  on = m[1].str() == "true";\n', '  on = true;\n'),
}


def start_after_replay(app_text):
    """The start's file read comes after the replay's settings (replay::Want ran first, so it never sees our value as given) and after
    reshoot / director (which can set --wwe2k14_replay_play)."""
    a = app_text.find('wwe2k14::ramp_menu::ApplyAtStart();')
    r = app_text.find('wwe2k14::replay::g_replay.PreSetup();')
    return a >= 0 and r >= 0 and r < a

WIRING = [
    ('src/photo_mode.h', '#include "ramp_menu.h"'),
    ('src/photo_mode.h', 'if (uint32_t(ctx.lr) == wwe2k14::ramp_menu::kVpFillerRet) wwe2k14::ramp_menu::OnVpRows(ctx, base);'),
    ('src/ramp_menu.h', 'REX_HOOK_RAW(sub_8235C218)'),
    ('src/ramp_menu.h', 'REX_HOOK_RAW(sub_8235CBE0)'),
    ('src/ramp_menu.h', 'REX_HOOK_RAW(sub_8235BC90)'),
    ('src/ramp_menu.h', '#include "ramp_menu_core.h"'),
    ('src/gfx_menu.h', '{kTextRamp, "RAMP CAMERA", 0},'),
    ('src/gfx_menu.h', '|| g_photo_pause_item || g_ramp_pause_item) {'),
    ('src/wwe2k14_app.h', 'wwe2k14::ramp_menu::ApplyAtStart();'),
    ('src/launcher/launcher_core.h', '{"ramp_camera", s.ramp_camera}'),
    ('src/launcher/launcher_core.h', 'Get(j, "ramp_camera", s.ramp_camera)'),
    ('src/launcher/launcher_core.h', 'if (!replay_playback) f.push_back(std::string("--wwe2k14_ramp_camera=") + (s.ramp_camera ? "true" : "false"));'),
    ('src/launcher/launcher_core.h', 'const bool replay_playback = s.preset == kReplay && !s.replay_record;'),
    ('src/launcher/launcher_main.cpp', 'launch_ramp_camera_ = s.ramp_camera;'),
    ('src/launcher/launcher_main.cpp', '"ramp_camera", ramp) && ramp != launch_ramp_camera_)'),
    ('src/launcher/launcher_main.cpp', 'OptionToggle("RAMP CAMERA", s.ramp_camera, true,'),
    ('tools/launcher/settings_map.txt', 'RAMP CAMERA (PC) | pc | wwe2k14_ramp_camera | live |'),
]


def hook_body(text, name):
    """The text of REX_HOOK_RAW(name) up to the next hook (or the end of the file)."""
    at = text.find('REX_HOOK_RAW(%s)' % name)
    if at < 0:
        return ''
    end = text.find('REX_HOOK_RAW(', at + 1)
    return text[at:end if end >= 0 else len(text)]


def main():
    header = read('src', 'ramp_menu_core.h')
    with tempfile.TemporaryDirectory() as tmp:
        rc, out = build_and_run(header, tmp, 'real')
        print('  ' + out.replace('\n', '\n  '))
        check(rc == 0, 'the unit fails on the real header')
        caught = 0
        for i, (what, (old, new)) in enumerate(TWINS.items()):
            if header.count(old) != 1:
                check(False, 'planted twin "%s": its anchor is not in the header exactly once (%d)' % (what, header.count(old)))
                continue
            rc, out = build_and_run(header.replace(old, new), tmp, 'twin%d' % i)
            check(rc != 0, 'planted twin (%s) was not caught' % what)
            if rc != 0:
                caught += 1
        print('  planted twins caught: %d of %d' % (caught, len(TWINS)))
    for path, needle in WIRING:
        check(needle in read(*path.split('/')), 'wiring missing in %s: %s' % (path, needle))
    photo = read('src', 'photo_mode.h')
    hook = photo[photo.index('REX_HOOK_RAW(sub_82391928)'):photo.index('REX_HOOK_RAW(sub_823839D0)')]
    check('OnVpRows(ctx, base);' in hook and hook.index('OnVpRows(ctx, base);') < hook.index('__imp__sub_82391928(ctx, base);'),
          'the row is added before the game copies the rows (sub_82391928)')
    ramp = read('src', 'ramp_menu.h')
    ask = ramp[ramp.index('REX_HOOK_RAW(sub_8235C218)'):ramp.index('REX_HOOK_RAW(sub_8235CBE0)')]
    a_set = ask.index('*cuts = QuestionByte(RampOn());')
    a_call = ask.index('__imp__sub_8235C218(ctx, base);', a_set)
    check(a_call < ask.index('*cuts = saved;', a_call), 'the question hook: CAMERA CUTS byte set, the game call, the byte put back (in that order)')
    # S111 RAMPMENU review F1: the answer hook hands the game its own no-write answer (sub_8235CBE0 writes CAMERA CUTS' byte for 0 / 1
    # only) and never touches the byte itself; then our value is saved.
    ans = hook_body(ramp, 'sub_8235CBE0')
    b_set = ans.find('ctx.r4.u64 = kAnswerNoWrite;')
    b_call = ans.find('__imp__sub_8235CBE0(ctx, base);', max(b_set, 0))
    check(0 <= b_set < b_call and b_call < ans.find('Save(now);', b_call),
          'the answer hook: the game is handed kAnswerNoWrite, the game call, then our value saved')
    check('*cuts' not in ans and 'kCameraCutsByte' not in ans, 'the answer hook never writes CAMERA CUTS\' byte')
    # S111 RAMPMENU review F2: B in our question (the flow's cancel step sub_8235BC90 rebuilds page 3 on CAMERA CUTS) puts the cursor
    # back on RAMP CAMERA, as the game does for every stock row; the new page is the flow's +104 (sub_8235BC90 returns no menu in r3).
    can = hook_body(ramp, 'sub_8235BC90')
    c_flow = can.find('const uint32_t flow = ctx.r3.u32;')
    c_call = can.find('__imp__sub_8235BC90(ctx, base);', max(c_flow, 0))
    c_page = can.find('W(base, flow + 104)', max(c_call, 0))
    check(0 <= c_flow < c_call < c_page and c_page < can.find('sub_82391100, page, kRampAction', c_page),
          'the cancel hook: the flow kept, the game call, the new page read from the flow, then the cursor on our row')
    # S111 RAMPMENU review F3: the start reads the key whoever started the game (not behind wwe2k14_use_launcher_settings); the command
    # line wins through the switch's source.
    start = ramp[ramp.index('inline void ApplyAtStart()'):ramp.index('inline void OnVpRows(')]
    check('StartDecisionRead(' in start and 'rex::cvar::GetFlagSource(kFlag) != rex::cvar::Source::kDefault' in start,
          'ApplyAtStart: one rule (StartDecision) fed with whether something already set the switch')
    check('wwe2k14::IsDevBuild()' in start and 'wwe2k14_replay_play' in start,
          'ApplyAtStart: the dev build and the replay playback reach the rule')
    # S111 RAMPMENU review F4: the order in OnPreSetup (planted twin: the call moved before the replay's settings must fail the pin).
    app = read('src', 'wwe2k14_app.h')
    check(start_after_replay(app), 'wwe2k14_app.h: ApplyAtStart runs after replay::g_replay.PreSetup()')
    twin = app.replace('    wwe2k14::ramp_menu::ApplyAtStart();', '', 1)
    twin = twin.replace('    wwe2k14::gfx_menu::ApplyLauncherSettings();',
                        '    wwe2k14::ramp_menu::ApplyAtStart();\n    wwe2k14::gfx_menu::ApplyLauncherSettings();', 1)
    check(not start_after_replay(twin), 'planted twin (ApplyAtStart before the replay\'s settings) was not caught by the order pin')
    print('  wiring: %d pins' % (len(WIRING) + 9))
    if problems:
        print('ramp_menu_selftest: FAIL (%d)' % len(problems))
        return 1
    print('ramp_menu_selftest: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
