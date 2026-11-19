"""qx6_selftest: proves the QX3 verbs (q asm, q ours hooks / text / affects, shader fallback) on a fixture in a TEMP folder, with PLANTED misses (no clang, no game, 0 runs):
  asm      a generated listing with a loop, a switch and an outside branch: the window marks branch targets and their sources; `--into` finds the planted branch
           that enters a range from outside, says whether the fall-through enters, and reports ZERO for a range that only the fall-through enters;
  hooks    a REX_HOOK_RAW, a macro that expands to REX_HOOK_RAW invoked with sub_X, a [[midasm_hook]] row + its callback definition + the generated `extern void` line;
           a function with no hook answers none (no false positive); a mid-asm hook inside a function is listed when the function is asked;
  text     an address written in a comment (any spelling) and a word in a string / code line, with the kind;
  affects  a lane with a per-check glob: a covered file lists the check, a file outside it lists none (the gate's `*` does not cross `/`);
  shader   a block field declared in a .glsl and used in a .fx.json is found when it is no C++ symbol.
Then the real tree (skipped when generated\\ is absent): the BELT adversary's window of sub_8272D710 and the branches into 0x8272E030..0x8272E4EC.
Run:  py -I tools/q/qx6_selftest.py
"""
import contextlib
import io
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qx6  # noqa: E402

Q = os.path.join(HERE, 'q.py')


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{str(out)[-1500:]}')
    print(f'PASS  {what}')


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


LISTING = '''DEFINE_REX_FUNC(sub_82000100) {
	// stwu r1,-96(r1)
	// cmpwi cr6,r3,0
	// beq cr6,0x82000114
	// li r4,1
	// addi r3,r3,-1
loc_82000114:
	// lwz r11,0(r3)
	// cmpwi cr6,r11,5
	// bne cr6,0x8200012C
	// li r5,7
	// b 0x82000134
	// nop
loc_8200012C:
	// li r5,9
	// bdnz 0x82000114
loc_82000134:
	// blr
}
DEFINE_REX_FUNC(sub_82000200) {
	// cmpwi cr6,r3,0
	// b 0x82000124
	// blr
}
'''


def fixture(root):
    write(root, 'generated/default/wwe2k14_recomp.0.cpp', LISTING + '\nextern void wwe2k14_fx_cb(PPCRegister& r3);\n\twwe2k14_fx_cb(ctx.r3);\nextern bool wwe2k14_gate_cb(PPCRegister& r3);\n\tif (wwe2k14_gate_cb(ctx.r3)) {\n')
    # sub_82000100 = 0x82000100 .. 0x82000144 (17 instructions: 5 + 5 + 2 + ... counted by the listing); the index row carries file / line
    write(root, 'src/hooks.h', '''#pragma once
#define WWE_FX_HOOK(sub) REX_HOOK_RAW(sub) { Run<__imp__##sub>(ctx, base); }
REX_HOOK_RAW(sub_82000100) { __imp__sub_82000100(ctx, base); }
WWE_FX_HOOK(sub_82009990)
// the 82C1E840 post node is not hooked here; PartsBlur is a note
void wwe2k14_fx_cb(PPCRegister& r3) { printf("PartsBlur %d", 1); }
bool wwe2k14_gate_cb(PPCRegister& r3) { return true; }
// REX_HOOK_RAW(sub_82008888) { dead: commented out }
const char* k = "REX_HOOK_RAW(sub_82007777) { dead: quoted }";
#define WWE_PASTE_HOOK(name, n) REX_HOOK_RAW(name##n) { Run<__imp__##name##n>(ctx, base); }
#define WWE_PASTE_HOOKS(n) WWE_PASTE_HOOK(__savegprlr_, n) WWE_PASTE_HOOK(__restgprlr_, n)
WWE_PASTE_HOOKS(14)
''')
    write(root, 'config/x_hooks.toml', '''# a note
[[midasm_hook]]
address = 0x82000114
name = "wwe2k14_fx_cb"
registers = ["r3"]

[[midasm_hook]]
address = 0x8200012C
name = "wwe2k14_gate_cb"
registers = ["r3"]
''')
    write(root, 'tools/gate/lanes/fx_lane.json', json.dumps({'affects': ['src/other/*.h'], 'checks': [
        {'name': 'fx.one', 'cmd': ['py', 'tools/x.py'], 'affects': ['src/fx/*.h', 'src/hooks.h']},
        {'name': 'fx.two', 'cmd': ['py', 'tools/x.py']}]}))
    write(root, 'tools/gate/lanes/fx_always.json', json.dumps({'checks': [{'name': 'fa.never_declared', 'cmd': ['some_unknown_tool', '--all']}]}))
    write(root, 'tools/x.py', 'print(1)\n')
    write(root, 'tools/y.py', 'print(2)\n')
    write(root, 'src/other/o.h', '// o\n')
    write(root, 'src/fx/a.h', '// x\n')
    write(root, 'src/fx/deep/b.h', '// y\n')
    write(root, 'src/shaders/blk.glsl', 'layout(std140) uniform B {\n  vec4 Param1;   // z: ambient\n};\nvoid main() { float a = Param1.z; }\n')
    write(root, 'src/shaders/blk.fx.json', '{\n "registers": [ {"field": "Param1"} ]\n}\n')


def index_con(root):
    os.makedirs(os.path.join(root, 'out', 'harness', 'index'), exist_ok=True)
    con = sqlite3.connect(os.path.join(root, 'out', 'harness', 'index', 'index.db'))
    con.row_factory = sqlite3.Row
    con.execute('CREATE TABLE funcs(addr INTEGER PRIMARY KEY, name TEXT, size INTEGER, insns INTEGER, file TEXT, line INTEGER, n_bctrl INTEGER, n_bctr INTEGER, resync INTEGER, has_body INTEGER)')
    con.execute("INSERT INTO funcs VALUES (?,?,?,?,?,?,0,0,0,1)", (0x82000100, 'sub_82000100', 0x38, 14, 'wwe2k14_recomp.0.cpp', 1))
    con.execute("INSERT INTO funcs VALUES (?,?,?,?,?,?,0,0,0,1)", (0x82000200, 'sub_82000200', 0xC, 3, 'wwe2k14_recomp.0.cpp', 21))
    con.commit()
    return con


def resolve(con, s):
    s = s.strip()
    r = con.execute('SELECT addr FROM funcs WHERE name = ?', (s,)).fetchone()
    if r:
        return r['addr'], None
    a = int(s.replace('0x', ''), 16)
    r = con.execute('SELECT addr FROM funcs WHERE addr <= ? ORDER BY addr DESC LIMIT 1', (a,)).fetchone()
    return r['addr'], f'inside {r["addr"]:08X}'


def run_asm(root, con, *args):
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        qx6.asm_main(list(args), con, resolve, root)
    return buf.getvalue()


def main():
    with tempfile.TemporaryDirectory(prefix='qx6_') as root:
        fixture(root)
        con = index_con(root)
        # ---- asm
        o = run_asm(root, con, 'sub_82000100', '0x82000114', '0x82000120')
        check('82000114  lwz r11,0(r3)   <= 82000108,82000130' in o, 'asm: the branch target 82000114 lists its sources (beq at 82000108, bdnz at 82000130)', o)
        check('8200011C  bne cr6,0x8200012C   => 8200012C' in o and '82000124' not in o, 'asm: a branch shows its target; the window stops at TO', o)
        o = run_asm(root, con, 'sub_82000100', '--into', '0x82000114', '0x8200012C')
        check('beq cr6,0x82000114' in o and 'bdnz 0x82000114' in o and '(2)' in o and '[cond]' in o, 'asm --into: both PLANTED outside branches (beq 82000108, bdnz 82000130) into 82000114 found', o)
        check('fall-through: YES' in o, 'asm --into: the fall-through from the instruction before FROM is reported as entering', o)
        o = run_asm(root, con, 'sub_82000100', '--into', '0x82000134', '0x82000138')
        check('b 0x82000134' in o and '[uncond]' in o and 'YES' in o, 'asm --into: an unconditional branch and the fall-through of a conditional (bdnz) both enter 82000134', o)
        o = run_asm(root, con, 'sub_82000100', '--into', '0x82000128', '0x8200012C')
        check('(0)' in o and 'fall-through: no' in o, 'asm --into: a range after an unconditional `b` that nothing branches to: ZERO branches, no fall-through (no false positive)', o)
        o = run_asm(root, con, 'sub_82000100', '--into', '0x8200012C', '0x82000134')
        check('bne cr6,0x8200012C' in o and '(1)' in o and 'YES' in o, 'asm --into: bne at 8200011C enters 8200012C; the nop before it falls through', o)
        o = run_asm(root, con, 'sub_82000100', '--into', '0x82000138', '0x82000140')
        check('(0)' in o and 'fall-through: no' in o, 'asm --into: after a blr nothing is claimed to enter', o)
        o = run_asm(root, con, 'sub_82000100', '--into', '0x82000114', '0x8200012C')
        check('NOT LISTED' in o and '0 bctr' in o and 'OTHER functions' in o, 'asm --into: always says what it cannot see (other functions, unresolved bctr count K, hook jumps)', o)
        o = run_asm(root, con, 'sub_82000100', '0x82009000', '0x82009010')
        check('WARNING' in o and 'not inside sub_82000100' in o, 'asm: a window outside the function warns', o)
        # ---- hooks
        r = qx6.cmd_hooks(root, 'sub_82000100', main=root)
        check(len(r['src']) == 1 and r['src'][0]['file'] == 'src/hooks.h' and r['src'][0]['line'] == 3, 'hooks: REX_HOOK_RAW(sub_82000100) found with file:line', r)
        check([x['name'] for x in r['midasm']] == ['wwe2k14_fx_cb', 'wwe2k14_gate_cb'], 'hooks: the mid-asm hooks INSIDE the asked function are listed', r)
        m = r['midasm'][0]
        check(m['toml'] == 'config/x_hooks.toml:4' and m['defs'] == ['src/hooks.h:6'] and 'declares it' in m['generated'] and '1 call' in m['generated'],
              'hooks: toml row, callback definition and generated extern / call site', m)
        r = qx6.cmd_hooks(root, 'sub_82009990', main=root)
        check(len(r['src']) == 1 and 'WWE_FX_HOOK' in r['src'][0]['kind'], 'hooks: a macro that expands to REX_HOOK_RAW, invoked with the function, is found', r)
        r = qx6.cmd_hooks(root, 'sub_82000200', main=root)
        check(not r['src'] and not r['midasm'], 'hooks: a function with no hook answers none', r)
        r = qx6.cmd_hooks(root, 'wwe2k14_fx_cb', main=root)
        check(len(r['midasm']) == 1, 'hooks: by callback name', r)
        r = qx6.cmd_hooks(root, '0x82000114', main=root)
        check(len(r['midasm']) == 1, 'hooks: by exact mid-asm address', r)
        # ---- text
        r = qx6.cmd_text(root, ['82C1E840'])
        check(r['n'] == 1 and r['hits'][0]['kind'] == 'comment', 'text: an address in a comment', r)
        r = qx6.cmd_text(root, ['sub_82C1E840'])
        check(r['n'] == 1, 'text: any spelling of an address (sub_ / 0x / bare)', r)
        r = qx6.cmd_text(root, ['partsblur'])
        kinds = sorted(h['kind'] for h in r['hits'])
        check(kinds == ['comment', 'string'], 'text: a word, case-insensitive, with its kind (comment and string)', r)
        r = qx6.cmd_text(root, ['PartsBlur', 'ambient'])
        check(r['n'] == 0, 'text: ALL words must be on the line (no partial match)', r)
        # ---- affects (the gate's own selection: fastgate.selected)
        r = qx6.cmd_affects(root, 'src/fx/a.h')
        check('fx.one' in [a['check'] for a in r['selected']], 'affects: a file under the per-check glob selects the check', r)
        check('fx.two' not in [a['check'] for a in r['selected']] and 'fx.one' in [a['check'] for a in r['selected']], 'affects: the lane-wide list does NOT merge into a check that has its own (src/fx/a.h is not src/other/*.h)', r)
        r = qx6.cmd_affects(root, 'src/other/o.h')
        names = [a['check'] for a in r['selected']]
        check('fx.two' in names and 'fx.one' not in names, 'affects: a check with NO list of its own uses the lane-wide one; one with its own list does not', r)
        check(r['always'] >= 1, 'affects: a check with no affects anywhere is reported as running for ANY change (never "gated by no lane")', r)
        r = qx6.cmd_affects(root, 'src/fx/deep/b.h')
        check('fx.one' not in [a['check'] for a in r['selected']], 'affects: `*` does not cross a `/` (the gate rule): src/fx/deep/b.h is NOT covered by src/fx/*.h', r)
        r = qx6.cmd_affects(root, 'tools/gate/lanes/fx_lane.json')
        check({'fx.one', 'fx.two'} <= {a['check'] for a in r['selected']}, 'affects: a changed lane json selects its checks (the gate rule)', r)
        for bad in ('../outside.h', 'C:/Windows/notepad.exe'):
            try:
                qx6.cmd_affects(root, bad)
                check(False, f'affects: {bad} must be refused (outside the repository)')
            except qx6.XError as e:
                check('outside' in str(e) or 'not under' in str(e), f'affects: {bad} is refused, not answered with 0', str(e))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            qx6.print_affects(qx6.cmd_affects(root, 'src/zzz/none.h'))
        check('no lane' not in out.getvalue().lower() and 'selects' in out.getvalue(), 'affects: the printed answer never says "no lane covers"', out.getvalue())
        # a wrong-case spelling is matched to the git spelling (a git repo fixture)
        subprocess.run(['git', '-C', root, 'init', '-q'], check=True)
        subprocess.run(['git', '-C', root, 'add', '-A'], check=True)
        r = qx6.cmd_affects(root, 'SRC/Fx/A.H')
        check(r['file'] == 'src/fx/a.h' and 'fx.one' in [a['check'] for a in r['selected']], 'affects: a wrong-case path is mapped to the git spelling', r)
        # ---- hooks, review fixes
        r = qx6.cmd_hooks(root, 'sub_82008888', main=root)
        check(not r['src'], 'hooks: a hook inside a COMMENT is not a hook', r)
        r = qx6.cmd_hooks(root, 'sub_82007777', main=root)
        check(not r['src'], 'hooks: a hook inside a STRING is not a hook', r)
        r = qx6.cmd_hooks(root, '__savegprlr_14', main=root)
        check(len(r['src']) == 1 and r['src'][0]['line'] == 12, 'hooks: a nested macro with ## pastes expands (__savegprlr_14 from WWE_PASTE_HOOKS(14))', r)
        r = qx6.cmd_hooks(root, '__restgprlr_14', main=root)
        check(len(r['src']) == 1, 'hooks: the second paste of the same invocation', r)
        r = qx6.cmd_hooks(root, 'wwe2k14_gate_cb', main=root)
        check(len(r['midasm']) == 1 and 'declares it; 1 call' in r['midasm'][0]['generated'], 'hooks: `extern bool NAME(` declared and an `if (NAME())` call is counted', r)
        h = qx6.cmd_hooks(root, '', main=root)
        check(not any(x['target'] in ('sub_82008888', 'sub_82007777') for x in h['src']), 'hooks: the all-hooks listing holds no dead (commented / quoted) hook', h)
        con.close()
        # ---- shader fallback
        r = qx6.shader_hits(root, 'Param1')
        check(any(h['kind'] == 'decl' and h['file'].endswith('blk.glsl') for h in r['hits']) and any(h['file'].endswith('blk.fx.json') for h in r['hits']),
              'shader: a block field declared in glsl and named in fx.json', r)
        r = qx6.shader_hits(root, 'Param10')
        check(r['n'] == 0, 'shader: word-bounded (Param10 is not Param1)', r)
    # ---- the real tree
    main_root = qx6.main_root(os.path.dirname(os.path.dirname(HERE)))
    if os.path.exists(os.path.join(main_root, 'generated', 'default', 'wwe2k14_recomp.223.cpp')) and os.path.exists(os.path.join(main_root, 'out', 'harness', 'index', 'index.db')):
        env = dict(os.environ, PYTHONIOENCODING='utf-8')

        def q(*a):
            return subprocess.run([sys.executable, Q] + list(a), capture_output=True, text=True, env=env, timeout=120).stdout
        o = q('asm', 'sub_8272D710', '0x8272E02C', '0x8272E040')
        check('8272E030  lis r11,-31853' in o and '8272E040  bl 0x82ba7f08' in o, 'real: BELT window of sub_8272D710 0x8272E02C..0x8272E040', o)
        o = q('asm', 'sub_8272D710', '--into', '0x8272E030', '0x8272E4EC')
        check('from outside it (0)' in o and 'falls through into 8272E030' in o, 'real: no branch enters 0x8272E030..0x8272E4EC from outside; only the fall-through from 8272E02C', o)
        o = q('ours', 'hooks', 'sub_8270ECE8')
        check('src/scene_live/replay_pose_hook.h:20' in o, 'real: q ours hooks sub_8270ECE8 finds the RPF hook', o)
        o = q('ours', 'hooks', '0x82C04E1C')
        check('wwe2k14_model_entry' in o and 'config/fp_probe_hooks.toml:72' in o and re.search(r'src/fp_probe.h:[0-9]+', o), 'real: q ours hooks 0x82C04E1C finds wwe2k14_model_entry (toml row, callback, generated)', o)
        o = q('ours', 'hooks', 'wwe2k14_model_entry')
        check('0x82C04E1C' in o, 'real: q ours hooks by callback name', o)
        o = q('ours', 'hooks', '__savegprlr_14')
        check('src/nf/reg_save.h:98' in o, 'real: q ours hooks __savegprlr_14 finds the nested ## macro hook (src/nf/reg_save.h)', o)
        o = q('ours', 'hooks', 'wwe2k14_pacing_halve_82331778')
        check('declares it' in o and 'not declared' not in o, 'real: a `bool` / `void` callback declared in generated is found (pacing_halve)', o)
        o = q('ours', 'def', 'Param1')
        check(('not in the C++ index' in o or 'Param1' in o) and 'no C++ symbol' not in o, 'real: shader fallback wording (the symbol is not in the index, it is not claimed absent)', o)
        e = subprocess.run([sys.executable, Q, 'ours', '--branch', 'main', 'text', 'foo'], capture_output=True, text=True, env=env, timeout=120)
        check(e.returncode == 2 and '--branch is not supported' in e.stderr, 'real: --branch on a text verb is refused with a clear error', e.stderr)
        wt = os.path.dirname(os.path.dirname(HERE))
        r = qx6.cmd_affects(wt, 'tools/q/qx6.py', derived=False)      # derived=False: the selection rule is proven by the fixture above; the derived-deps table is a cold ~2 min computation under load
        check(isinstance(r, dict) and 'selected' in r, 'real: cmd_affects on this checkout answers (derived deps not consulted)', r)
        o = q('ours', 'text', '82C1E840', 'post')
        check('scene_skip_core.h' in o or 'post_settings.h' in o, 'real: q ours text finds addresses written in comments', o)
    else:
        print('SKIP  real-tree proofs (generated\\ or the q index is absent)')
    print('qx6_selftest: all passed')


if __name__ == '__main__':
    try:
        main()
    except Fail as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
