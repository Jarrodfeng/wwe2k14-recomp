"""S110 GOTO / HOMESTEP / CASNAV: compiles tools/goto/menu_goto_test.cpp (src/menu_goto_core.h), tools/goto/menu_home_test.cpp
(src/menu_home_core.h) and tools/goto/cas_goto_test.cpp (src/cas_goto_core.h) and runs them. Also pins the wiring (hook lines present)
so a lost hook fails here.
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
        for name in ('menu_goto_test', 'menu_home_test', 'cas_goto_test', 'mode_table_test', 'mode_pick_test', 'cas_pick_test', 'cas_row_test'):
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
              ('src/menu_home.h', 'menu_ready'),
              ('src/cas_launch.h', '#include "cas_goto.h"'), ('src/cas_launch.h', 'cas_goto::Step(ctx, base, frame);'),
              ('src/cas_goto.h', 'if (!ArrivalReady(call, h, p, frame)) return;'),
              ('src/cas_goto.h', 'const std::string preview = PreviewNotReady(call);'),
              ('src/cas_goto.h', 'if (!ArrivalReady(call, h, p, frame)) return;  // CASREADY review: a row arrival'),
              ('src/cas_launch.h', 'mode_launch::Step(ctx, base, frame);'), ('src/scenario.h', 'name.rfind("@mode:", 0) == 0'),
              ('src/mode_launch.h', 'call(sub_8283CFF0, {uint64_t(sm), uint64_t(g_mode->exit)});'),
              ('src/mode_launch.h', '#include "mode_pick.h"'), ('src/mode_launch.h', 'mode_pick::Watch(frame);'),
              ('src/mode_launch.h', 'mode_pick::Prepare(ctx, base, *g_mode, g_name, frame);'),
              ('src/mode_pick.h', 'REX_HOOK_RAW(sub_8226B730)'), ('src/mode_pick.h', 'wwe2k14::mode_pick::EntranceTake(ctx, base);'),
              ('src/cas_launch.h', '#include "cas_pick.h"'), ('src/cas_launch.h', 'cas_pick::Step(ctx, base, frame);'),
              # CASPICK review: a step is the mixin's own dispatch (the pad's path), and the pick yields to a go-to armed mid-pick
                ('src/cas_pick.h', 'call(sub_823BC8C0, {uint64_t(m)});'),
                # S110 CASROW: one owner of the editor's presses (src/cas_driver.h), the verb's hook and game calls, the pick on the logo list
                ('src/cas_goto.h', 'if (!cas_driver::Claim(cas_driver::kGoto)) {'), ('src/cas_pick.h', 'if (!cas_driver::Claim(cas_driver::kPick)) {'),
                ('src/cas_row.h', 'if (!cas_driver::Claim(cas_driver::kRow)) {'), ('src/cas_launch.h', '#include "cas_row.h"'),
                ('src/cas_launch.h', 'cas_row::Step(ctx, base, frame);'),
                ('src/cas_row.h', 'call(sub_823BF6B8, {uint64_t(table), uint64_t(base + i)})'), ('src/cas_row.h', 'call(sub_823BC838, {uint64_t(m)});'),
                ('src/cas_pick.h', 'NestedLogoCommit(nested, mode, cat, g_edit)'),
                # CASROW review: a box on top stops the pick (it waited, or fell through to the panel words of object 0)
                ('src/cas_pick.h', 'if (top.kind == cas_row::TopKind::kBox && Rd(top.obj + cas_row::kBoxState) == 2) {'),
              # CASPICK fix 3 (rule 6, runs 281 / 294): the keep is the panel's own commit once the panel is idle; the end is the overlay gone
              ('src/cas_pick.h', 'call(sub_833FF868, {uint64_t(pn)});'),
              ('src/cas_pick.h', 'if (!KeepReady(Rd(pn + kPPending), Rd(pn + kPStep), Rd(pn + kPSubMode), Rd(pn + kPResult), preview.empty())) {'),
              ('src/cas_pick.h', 'p = CloseSettledPage(a, Rd(h + cas_goto::kHPage))')]
    for path, needle in wiring:
        if needle not in read(path):
            print('FAIL: wiring missing in %s: %s' % (path, needle))
            return 1
    print('wiring ok (%d hook lines)' % len(wiring))
    return 0


if __name__ == '__main__':
    sys.exit(main())
