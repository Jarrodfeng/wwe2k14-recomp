"""S110 GOTO / HOMESTEP: compiles tools/goto/menu_goto_test.cpp (src/menu_goto_core.h) and tools/goto/menu_home_test.cpp
(src/menu_home_core.h) and runs them. Also pins the wiring (hook lines present) so a lost hook fails here.
Run: py -I tools\\goto\\menu_goto_selftest.py (selftest.py runs it too)"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)


def main():
    import selftest
    src = os.path.join(REPO, 'src')
    with tempfile.TemporaryDirectory() as tmp:
        for name in ('menu_goto_test', 'menu_home_test'):
            exe = os.path.join(tmp, name + '.exe')
            r = subprocess.run([selftest.find_clang(), '-std=c++17', '-I', src, os.path.join(HERE, name + '.cpp'), '-o', exe],
                               capture_output=True, text=True, timeout=180)
            if r.returncode != 0:
                print('FAIL: %s did not compile:' % name, (r.stdout + r.stderr)[-800:])
                return 1
            r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
            print(r.stdout.strip())
            if r.returncode != 0:
                return 1

    def read(p):
        with open(os.path.join(REPO, p), encoding='utf-8') as f:
            return f.read()
    wiring = [('src/scenario.h', 'g_page_tick(ctx, base)'), ('src/live_control.h', 'directive == "page"'),
              ('src/live_control.h', 'g_page_live(page_args)'), ('src/wwe2k14_app.h', '#include "menu_goto.h"'),
              ('src/menu_goto.h', 'g_page_live = &LiveCommand'), ('src/menu_goto.h', 'REX_HOOK_RAW(sub_828426D0)'),
              ('src/menu_goto.h', 'ServesOurCall(true, uint32_t(ctx.lr)'), ('src/input_script.h', 'case Op::kPage:'),
              ('src/input_script.h', 'menu_goto::ParseScriptLine('),
              ('src/input_script.h', 'case Op::kHome:'), ('src/input_script.h', 'cmd == "home"'), ('src/input_script.h', '#include "menu_home.h"'),
              ('src/menu_home.h', 'menu_ready')]
    for path, needle in wiring:
        if needle not in read(path):
            print('FAIL: wiring missing in %s: %s' % (path, needle))
            return 1
    print('wiring ok (%d hook lines)' % len(wiring))
    return 0


if __name__ == '__main__':
    sys.exit(main())
