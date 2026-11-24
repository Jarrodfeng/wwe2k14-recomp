# S106 0a: builds + runs script_fail_test.cpp (the shipped src/script_fail_policy.h), then checks input_script.h is wired to it.
# (the unit's entry line is in script_fail_test.cpp; the wiring rows here read src/input_script.h as text)
import re, shutil, subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
exe = root / 'out' / 'tools' / 'script_fail_test.exe'
exe.parent.mkdir(parents=True, exist_ok=True)
cxx = shutil.which('clang++') or r'C:\Program Files\LLVM\bin\clang++.exe'
if not Path(cxx).exists():
    print('FAIL: no clang++ found'); sys.exit(2)
r = subprocess.run([cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', str(root / 'tools/script_fail/script_fail_test.cpp'), '-o', str(exe)], capture_output=True, text=True)
if r.returncode:
    print(r.stdout + r.stderr); sys.exit(2)
r = subprocess.run([str(exe)], capture_output=True, text=True)
print(r.stdout.strip())
if r.returncode:
    sys.exit(3)
src = (root / 'src/input_script.h').read_text(encoding='utf-8', errors='replace')
def body(name):
    i = src.index('void ' + name + '(')
    j = src.index('\n  }\n', i)
    return src[i:j]
bad = 0
for fn in ('FailAndFinish', 'FailWithScreenshot'):
    if 'QuitOnScriptFail(ScriptFailState{quit_on_fail_, REXCVAR_GET(wwe2k14_input_fail_quit)' not in body(fn) or 'REXCVAR_GET(wwe2k14_live)' not in body(fn):
        print('FAIL: %s does not call QuitOnScriptFail with the switch' % fn); bad += 1
if 'REXCVAR_GET(wwe2k14_input_fail_quit)' not in body('Init'):
    print('FAIL: Init (the load failure) does not read wwe2k14_input_fail_quit'); bad += 1
if 'fail_quit_pending_ = true' not in body('Init'):
    print('FAIL: Init (the load failure) does not set fail_quit_pending_ (the quit)'); bad += 1
if 'QuitOnScriptFail(' not in body('Init'):
    print('FAIL: Init (the load failure) does not go through the policy (diag hold, live session)'); bad += 1
if '#include "script_fail_policy.h"' not in src:
    print('FAIL: input_script.h does not include script_fail_policy.h'); bad += 1
if bad:
    sys.exit(4)
print('script_fail selftest: PASS')
