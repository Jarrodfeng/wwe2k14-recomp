"""S110 FIRELINE: offline checks of the `wm30_fire` / `wm30_step` input-script lines. No game runs.

    py -I tools/wm30/script_selftest.py     src/wm30_script_core.h COMPILED with clang++ (tools/wm30/script_test.cpp) and driven: the load rule is the live
                                            directive's own parser, the request carries every word, the verdict decides the line, the bound; wiring pins;
                                            planted twins (a malformed bank accepted, `force` dropped, a refused verdict reported as success, no bound),
                                            each compiled and seen to fail

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


def read(*p):
    with open(os.path.join(REPO, *p), encoding='utf-8') as f:
        return f.read()


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError("no clang++ found (tools\\env.ps1 puts the project's LLVM on PATH)")


def run_unit(header_text, tmp, tag):
    """Compiles + runs script_test.cpp against `header_text` as src/wm30_script_core.h (a copy under tmp: a planted twin never touches the tree). Returns (exit, output)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'wm30_script_core.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'script_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, '-I', os.path.join(REPO, 'src'), os.path.join(HERE, 'script_test.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('script_test did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-600:]))
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


PLANTS = {
    'a malformed bank is accepted at load': ('e = wm30_fire::ParseFireArgs(std::string("fire ") + rest, a);',
                                             'e = wm30_fire::ParseFireArgs(std::string("fire ") + rest, a); if (e.find("bank") != std::string::npos) e.clear();'),
    'force is dropped from the request': ('p.submit(std::string(VerbWord(v)) + " " + rest)', 'p.submit(std::string(VerbWord(v)) + " " + rest.substr(0, rest.find(" force")))'),
    'a refused verdict is reported as success': ('if (answer.rfind(ok, 0) == 0) return "";', 'if (true) return "";'),
    'the wait has no bound': ('if (frames_waited < kScriptFrames) return 0;', 'return 0;'),
    'a step line is judged by the fire word': ('const std::string ok = v == Verb::kFire ? "fired" : "stepped";', 'const std::string ok = "fired";'),
    'the failure text is replaced': ('why = bad.empty() ? answer : bad;', 'why = bad.empty() ? answer : std::string("failed");'),
}


def main():
    header = read('src', 'wm30_script_core.h')
    tmp = tempfile.mkdtemp(prefix='wm30_script_')
    try:
        code, out = run_unit(header, tmp, 'real')
        check(code == 0 and 'all passed' in out, 'the unit fails on the real header:\n' + out)
        for name, (old, new) in PLANTS.items():
            check(header.count(old) == 1, 'plant "%s": its anchor text is not in the header exactly once (the header changed: re-aim the plant)' % name)
            if header.count(old) != 1:
                continue
            tag = 'plant%d' % (list(PLANTS).index(name))
            code, out = run_unit(header.replace(old, new), tmp, tag)
            check(code != 0 and 'FAIL' in out, 'planted twin "%s" was NOT caught by the unit' % name)

        # wiring: the line shares the live directive's parser, verdict and request code (no second implementation)
        fire, step, inp = read('src', 'wm30_fire.h'), read('src', 'wm30_step.h'), read('src', 'input_script.h')
        for nm, src, verb in (('fire', fire, 'Fire'), ('step', step, 'Step')):
            check('SubmitLocked(a)' in src and src.count('SubmitLocked(') == 3, '%s: the live command and the script submit must share SubmitLocked (def + 2 calls)' % nm)
            check('Parse%sArgs(args, a)' % verb in src, '%s: the script submit parses with the live directive\'s Parse%sArgs' % (nm, verb))
            check('wm30_script::g_%s_port = {&ScriptSubmit, &ScriptPoll, &ScriptWithdraw}' % nm in src, '%s: the port is not registered' % nm)
        core = read('src', 'wm30_script_core.h')
        check('ParseFireArgs' in core and 'ParseStepArgs' in core, 'the core must check a line with the live parsers')
        check('wm30_script::CheckLine(verb, step.text)' in inp, 'input_script.h does not check the line at load')
        check('wm30_script::Request(verb, step.text)' in inp and 'wm30_script::Poll(verb' in inp, 'input_script.h does not request + poll')
        check('FailAndFinish();' in inp[inp.index('case Op::kWm30Fire'):inp.index('case Op::kHome')], 'a failed verdict must FailAndFinish the script')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('script_selftest: %s' % ('FAILED (%d)' % len(problems) if problems else 'all passed'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
