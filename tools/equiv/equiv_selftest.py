"""S111 EQUIV piece B: the driver's selftest (tools\\equiv\\equiv.py; docs\\research\\s111\\EQUIV0.md).

Unit checks on fixtures (no q, no build): the closure and its refusals (a kernel import, a hooked callee, q's "seen above" elision), the
hook scan, the branch rows and their steering pins, the branch-count mapping (generated lines -> guest addresses -> unhit arms), the verdict
and the steering loop. End-to-end checks on the REAL game code (S111 EQUIVTIER: the fake host's hand transcriptions are retired): the three
tests and their planted twins through `equiv.py run` on the one exe with the stub seam and branch counts (`equiv.py cov-build
splat_life,mip_row,placement_table`; default: the newest such exe under the main checkout's out\\s95\\cg), each a headless tester run (no
window, no game thread); build the DLL first (tools\\equiv\\build_equiv.ps1 -Standalone). The static tier's refusal of a stub test needs an
exe without the seam (--static-exe, a lane exe of the default preset); without it that one check is listed as not run.
Plants (EQUIV_PLANT, each run in a child process on the unit checks): every plant must fail exactly its own checks, so each check is seen to
fail on wrong code.
    py -I tools\\equiv\\equiv_selftest.py [--exe COVEXE] [--static-exe EXE]   all checks + the plants
    py -I tools\\equiv\\equiv_selftest.py --units    unit checks only (what a plant run executes)
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import equiv as E  # noqa: E402

PLANTS = {
    'closure_ignores_hooks': {'closure.hooked'},
    'closure_ignores_imports': {'closure.import', 'closure.seen_above'},
    'proven_ignores_coverage': {'verdict.uncovered', 'verdict.no_counts', 'steer.loop'},
    'coverage_ignores_false_arm': {'coverage.arms'},
    'passthrough_ignores_flags': {'passthrough.dropped'},
}

RESULTS = []
NOT_RUN = []
COV_EXES = os.path.join(E.MAIN, 'out', 's95', 'cg', 'equiv_splat_life_mip_row_placement_table_*', 'exe', 'wwe2k14.exe')


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok), detail))


def node(f, *kids):
    return {'func': f, 'sites': [{'site': '0x0', 'kind': 'call', 'target': None}], 'callees': list(kids)}


def fixture_test():
    fl = [E.Field('[in_r3+24]', 'int', 0, 7, 1), E.Field('[0x837689D4]', 'int', 0, 1, 1), E.Field('[in_r3+64]', 'f32', -0.1, 1, 0.5),
          E.Field('ret#sub_82C2E1E0', 'int', 0, 1, 1)]
    return E.TestDef('fx', 0x82000000, None, [], fl, 'fixture')


def units():
    # ---- closure
    clean = {'func': 'sub_A', 'indirect_calls': 0, 'callees': [node('sub_B'), node('__savegprlr_17')]}
    funcs, _, ref = E.closure(clean, [], {})
    check('closure.clean', funcs == ['sub_A', 'sub_B'] and not ref, f'{funcs} {ref}')

    imp = {'func': 'sub_A', 'callees': [node('sub_E', node('__imp__RtlEnterCriticalSection'))]}
    _, _, r1 = E.closure(imp, [], {})
    _, _, r2 = E.closure({'func': 'sub_A', 'callees': [node('sub_0000000E', node('__imp__X'))]}, [0xE], {})   # the same, the callee stubbed
    check('closure.import', len(r1) == 1 and '__imp__RtlEnterCriticalSection' in r1[0] and 'sub_A -> sub_E' in r1[0] and not r2, f'{r1} / {r2}')

    # q callees lists sub_C's callees under its first parent (stubbed here) and says "seen above" (no callees) under sub_D
    seen = {'func': 'sub_A', 'callees': [node('sub_0000000B', node('sub_C', node('__imp__RtlLeaveCriticalSection'))), node('sub_D', node('sub_C'))]}
    _, st, r3 = E.closure(seen, [0xB], {})
    _, _, r4 = E.closure({'func': 'sub_A', 'callees': [node('sub_0000000B', node('sub_C', node('__imp__Y'))), node('sub_0000000D', node('sub_C'))]},
                         [0xB, 0xD], {})   # both parents stubbed: sub_C is never entered
    check('closure.seen_above', len(r3) == 1 and 'sub_D -> sub_C' in r3[0] and st == ['sub_0000000B'] and not r4, f'{r3} {st} / {r4}')

    hk = {'func': 'sub_A', 'callees': [node('sub_82C79000', node('sub_K'))]}
    f5, _, r5 = E.closure(hk, [], {'sub_82C79000': 'src\\nf\\memcpy_fwd.h'})
    f6, s6, r6 = E.closure(hk, [0x82C79000], {'sub_82C79000': 'src\\nf\\memcpy_fwd.h'})
    check('closure.hooked', len(r5) == 1 and 'sub_82C79000 is hooked' in r5[0] and 'memcpy_fwd.h' in r5[0] and not r6 and 'sub_K' not in f6
          and s6 == ['sub_82C79000'], f'{r5} / {r6} {f6}')

    # ---- hook scan
    texts = {'a.h': 'REX_HOOK_RAW(sub_825D99C8) {\n}', 'b.h': '#define MYHOOK(fn) \\\n  REX_HOOK_RAW(fn) { x(); }\nMYHOOK(sub_82000010)\n',
             'c.h': 'REX_HOOK(sub_82000020, Over)', 'd.h': '// calls sub_82000030 (not a hook)\n'}
    h = E.hooked_functions(texts)
    check('hooks.scan', set(h) == {'sub_825D99C8', 'sub_82000010', 'sub_82000020'} and h['sub_82000010'] == 'b.h', f'{h}')

    # ---- pass-through hooks (S111 EQUIVHOOK): a listed hook whose entry holds is allowed with its flags; the planted twin drops one flag
    # (the hook reads two cvars before the original, the entry lists one) and must be refused; an unlisted hook stays refused
    hp = {'f.h': 'REX_HOOK_RAW(sub_82000040) {\n  if (REXCVAR_GET(a_on) || REXCVAR_GET(b_on)) Patch(base);  // {\n  __imp__sub_82000040(ctx, base);\n  x();\n}\n'
                 'REX_HOOK_RAW(sub_82000050) {\n  if (REXCVAR_GET(a_on)) { ctx.r3.u64 = 1; return; }\n  __imp__sub_82000050(ctx, base);\n}\n'}
    good = {'sub_82000040': {'file': 'f.h', 'flags': {'a_on': 'false', 'b_on': 'false'}, 'early_returns': 0, 'state': {}}}
    twin = {'sub_82000040': {'file': 'f.h', 'flags': {'a_on': 'false'}, 'early_returns': 0, 'state': {}}}
    ret = {'sub_82000050': {'file': 'f.h', 'flags': {'a_on': 'false'}, 'early_returns': 0, 'state': {}}}   # a new early answer nobody listed
    hk2 = {'func': 'sub_A', 'callees': [node('sub_82000040'), node('sub_82000050')]}
    hooked2 = {'sub_82000040': 'f.h', 'sub_82000050': 'f.h'}
    ok_g, bad_g = E.hook_passthrough.allowed(hooked2, hp, good)
    ok_t, bad_t = E.hook_passthrough.allowed(hooked2, hp, twin)
    ok_r, bad_r = E.hook_passthrough.allowed(hooked2, hp, ret)
    fg, _, rg = E.closure(hk2, [], {k: v for k, v in hooked2.items() if k not in ok_g})
    ft, _, rt = E.closure(hk2, [], {k: v for k, v in hooked2.items() if k not in ok_t})
    check('passthrough.allowed', ok_g == {'sub_82000040': {'a_on': 'false', 'b_on': 'false'}} and not bad_g and 'sub_82000040' in fg
          and len(rg) == 1 and 'sub_82000050 is hooked' in rg[0], f'{ok_g} {bad_g} {rg}')
    check('passthrough.dropped', not ok_t and 'sub_82000040' in bad_t and any('sub_82000040 is hooked' in r for r in rt) and 'sub_82000040' not in ft
          and not ok_r and 'sub_82000050' in bad_r and 'return before the original' in bad_r['sub_82000050'][0], f'{ok_t} {bad_t} {rt} {bad_r}')
    check('passthrough.conflict', E.hook_passthrough.merge_flags({'x': {'a': 'false'}, 'y': {'a': 'true'}})[1] != ''
          and E.hook_passthrough.merge_flags({'x': {'a': 'false'}, 'y': {'a': 'false', 'b': 'false'}}) == ({'a': 'false', 'b': 'false'}, ''), '')
    # the real table against the real hooks in src
    real = E.src_texts()
    ok_real, bad_real = E.hook_passthrough.allowed(E.hooked_functions(real), real)
    check('passthrough.table', set(ok_real) == set(E.hook_passthrough.TABLE) and not bad_real, f'{sorted(ok_real)} {bad_real}')

    # ---- branch rows and steering
    t = fixture_test()
    lines = ['if ([in_r3+24] == 5) {   // +1C, skips to @82697F18',
             '    +3C  if ([0x837689D4] == 0) goto @8269810C',
             'if (([in_r3+36] + (([0x8392DC74] * 0.79999995[0x820232D4]) + [in_r3+48])) > -12[0x8200B67C]) {   // +B4, skips to @82697F7C',
             '  +70  call#70 sub_82C2E1E0(r3=in_r5)',
             'if (ret#70 != 0) {   // +78, else @8258F710',
             '} while (extsh([in_r3+28]) > r26@8258FDD4+1);   // +750',
             'if ([in_r3+64] >= 0.45[0x8200F2E8]) {   // +1D0, skips to @8269810C']
    br, calls = E.branches(lines)
    got = {b.off: E.steer(b, t, calls) for b in br}
    p1, p3c, pb4, p78, p750, p1d0 = got.get(0x1C), got.get(0x3C), got.get(0xB4), got.get(0x78), got.get(0x750), got.get(0x1D0)
    import struct
    f45 = struct.unpack('<f', struct.pack('<f', 0.45))[0]
    ok = (sorted(br_.off for br_ in br) == [0x1C, 0x3C, 0x78, 0xB4, 0x1D0, 0x750]
          and p1[0] == [{'[in_r3+24]': 4.0}, {'[in_r3+24]': 5.0}, {'[in_r3+24]': 6.0}]
          and p3c[0] == [{'[0x837689D4]': 0.0}, {'[0x837689D4]': 1.0}]
          and pb4[0] is None and 'several' in pb4[1]
          and p78[0] == [{'ret#sub_82C2E1E0': 0.0}, {'ret#sub_82C2E1E0': 1.0}]
          and p750[0] is None and 'loop-carried' in p750[1]
          and p1d0[0] is not None and len(p1d0[0]) == 3 and p1d0[0][1] == {'[in_r3+64]': f45} and p1d0[0][0]['[in_r3+64]'] < f45 < p1d0[0][2]['[in_r3+64]'])
    check('steer.pins', ok, f'{got}')

    # ---- branch counts -> unhit arms
    gen = ['DEFINE_REX_FUNC(sub_82000000) {', '\tREX_FUNC_PROLOGUE();', '\t// mflr r12', '\tctx.r12.u64 = ctx.lr;', '\t// cmpwi cr6,r3,0',
           '\tcr6.compare<int32_t>(ctx.r3.s32, 0, xer);', '\t// beq cr6,0x82000018', '\tif (cr6.eq) goto loc_82000018;', '\t// li r3,1',
           '\tctx.r3.s64 = 1;', '\t// cmpwi cr6,r4,0', '\tcr6.compare<int32_t>(ctx.r4.s32, 0, xer);', '\t// bne cr6,0x82000018',
           '\tif (!cr6.eq) goto loc_82000018;', 'loc_82000018:', '\t// blr', '\treturn;', '}']
    gp = os.path.join(tempfile.gettempdir(), 'equiv_selftest_gen.cpp')
    export = {'data': [{'functions': [{'name': '?__imp__sub_82000000@@YAXAEAUPPCContext@@PEAE@Z', 'filenames': [gp],
                                        'branches': [[8, 6, 8, 14, 5, 0, 0, 0, 4], [14, 6, 14, 15, 3, 2, 0, 0, 4]]}]}]}
    arms = E.unhit_arms(export, 0x82000000, gp, gen, 1)
    addr = E.body_addresses(gen, 1, 0x82000000)
    check('coverage.arms', arms == ([(0x82000008, 'not taken')], 2) and addr[8] == 0x82000008 and addr[14] == 0x82000014, f'{arms}')
    check('coverage.absent', E.unhit_arms({'data': [{'functions': []}]}, 0x82000000, gp, gen, 1) is None)
    # S111 EQUIVTIER: the coverage preset's own macros (REX_FUNC_PROLOGUE, REX_STORE_*: the stub seam / store checks) and codegen expressions
    # (cntlzw's `== 0 ?`) give llvm-cov branches too; only a PowerPC conditional branch's line is a branch of the game function
    seam = ['DEFINE_REX_FUNC(sub_82000000) {', '\tREX_FUNC_PROLOGUE();', '\t// stw r3,0(r4)', '\tREX_STORE_U32(ctx.r4.u32, ctx.r3.u32);',
            '\t// cntlzw r11,r3', '\tctx.r11.u64 = ctx.r3.u32 == 0 ? 32 : __builtin_clz(ctx.r3.u32);', '\t// bdnz 0x82000000',
            '\tif (--ctr.u64 != 0) goto loc_82000000;', '\t// blr', '\treturn;', '}']
    exp2 = {'data': [{'functions': [{'name': '__imp__sub_82000000', 'filenames': [gp],
                                     'branches': [[2, 2, 2, 2, 0, 9, 0, 0, 4], [4, 2, 4, 2, 0, 9, 0, 0, 4], [6, 16, 6, 31, 0, 9, 0, 0, 4],
                                                  [8, 6, 8, 18, 4, 5, 0, 0, 4]]}]}]}
    arms2 = E.unhit_arms(exp2, 0x82000000, gp, seam, 1)
    check('coverage.seam', arms2 == ([], 1), f'{arms2}')

    # ---- verdict
    eq = {'result': 'EQUAL', 'equal': 200, 'why': '', 'shrunk': '', 'outs': [], 'pins': {}}
    v1, _ = E.verdict('fx', [eq], ([], 3), None)
    check('verdict.proven', v1 == 'PROVEN', v1)
    v2, l2 = E.verdict('fx', [eq], ([(0x82000008, 'not taken', 'not steerable: x')], 3), None)
    check('verdict.uncovered', v2 == 'AGREES-UNCOVERED' and '0x82000008 not taken' in l2[0], f'{v2} {l2}')
    v3, _ = E.verdict('fx', [eq], None, 'no counts')
    check('verdict.no_counts', v3 == 'AGREES-UNCOVERED', v3)
    diff = dict(eq, result='DIFFER', shrunk='[in_r3+24]=3', outs=['[in_r3+64] game=1 ours=2'])
    v4, l4 = E.verdict('fx', [eq, diff], ([], 3), None)
    check('verdict.refuted', v4 == 'REFUTED' and '[in_r3+24]=3' in l4[0] and '[in_r3+64] game=1 ours=2' in l4[1], f'{l4}')
    v5, l5 = E.verdict('fx', [eq], ([], 3), None, ['kernel import __imp__X reached through sub_A'])
    check('verdict.not_isolable', v5 == 'NOT-ISOLABLE' and '__imp__X' in l5[0], f'{l5}')

    # ---- the steering loop on a simulated runner: +1C is hit only with state 5 pinned; +B4's other arm never (not steerable)
    sim_lines = lines[:3]

    def runner_two(pins):
        un = [] if pins.get('[in_r3+24]') == 5.0 else [(0x8200001C, 'taken')]
        return dict(eq), (un + [(0x820000B4, 'not taken')], 3), None

    reps, arms2, _ = E.steer_loop(t, sim_lines, 0x82000000, runner_two)
    v6, l6 = E.verdict('fx', reps, arms2, None)
    ok6 = (v6 == 'AGREES-UNCOVERED' and len(arms2[0]) == 1 and arms2[0][0][0] == 0x820000B4 and 'not steerable' in arms2[0][0][2]
           and any(r['pins'] == {'[in_r3+24]': 5.0} for r in reps))

    def runner_one(pins):
        return dict(eq), ([] if pins.get('[in_r3+24]') == 5.0 else [(0x8200001C, 'taken')], 2), None

    reps7, arms7, _ = E.steer_loop(t, sim_lines, 0x82000000, runner_one)
    v7, _ = E.verdict('fx', reps7, arms7, None)
    check('steer.loop', ok6 and v7 == 'PROVEN', f'{v6} {l6} / {v7}')

    # no steering round run (--rounds 0): a steerable arm must not be reported as "steered but never ran"
    _, arms8, _ = E.steer_loop(t, sim_lines, 0x82000000, runner_one, rounds=0)
    why8 = arms8[0][0][2] if arms8 and arms8[0] else ''
    check('steer.untried', 'not run' in why8 and 'never ran' not in why8, why8)


def real_run(name, exe, work):
    """`equiv.py run NAME --exe EXE --cases 300` in its own report folder: (exit code, verdict word, its output lines)."""
    r = subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'equiv.py'), 'run', name, '--exe', exe,
                        '--cases', '300', '--equiv-dir', work], capture_output=True, text=True, errors='replace', timeout=900)
    lines = (r.stdout + r.stderr).splitlines()
    first = next((ln for ln in lines if ln.split(' ', 1)[0] in ('PROVEN', 'AGREES-UNCOVERED', 'REFUTED', 'NOT-ISOLABLE')), '')
    return r.returncode, first.split(' ', 1)[0], lines


def unhit_list(line):
    """The arms after "never ran: " of an AGREES-UNCOVERED line."""
    return [s.strip() for s in line.split('never ran: ', 1)[1].split(',')] if 'never ran: ' in line else []


def end_to_end(exe, static_exe):
    cov = os.path.join(os.path.dirname(E.DLL), 'equiv_cov_unit.exe')   # the coverage tier's entry order (src\equiv\equiv_cov.h)
    if os.path.exists(cov):
        r = subprocess.run([cov], capture_output=True, text=True)
        check('cov.entry', r.returncode == 0, r.stdout[-600:])
    else:
        check('cov.entry', False, f'{cov} missing: pwsh -NoProfile -File tools\\equiv\\build_equiv.ps1 -Standalone')
    if not os.path.exists(E.DLL):
        check('real.built', False, f'{E.DLL} missing: pwsh -NoProfile -File tools\\equiv\\build_equiv.ps1 -Standalone')
        return
    if not exe:
        found = sorted(glob.glob(COV_EXES), key=os.path.getmtime)
        exe = found[-1] if found else ''
    if not exe or not os.path.exists(os.path.join(os.path.dirname(exe), 'variant.json')):
        check('real.exe', False, f'no covmap exe with the stub seam ({exe or COV_EXES}): py -I tools\\equiv\\equiv.py cov-build splat_life,mip_row,placement_table')
        return
    print(f'end-to-end exe: {exe}')
    work = tempfile.mkdtemp(prefix='equiv_selftest_')
    try:
        rc, v, l = real_run('splat_life', exe, work)
        check('real.splat_life', rc == 0 and v == 'PROVEN', '\n'.join(l[-6:]))
        rc, v, l = real_run('splat_life.twin_fade', exe, work)
        check('real.splat_twin', v == 'REFUTED' and 'shrunk [in_r3+24]=3' in ' '.join(l) and any(x.strip().startswith('[in_r3+64] ') for x in l),
              '\n'.join(l[-6:]))
        # the one arm no input reaches: +28's skip (a found row index at or past the row count; the index is sub_827FDB10's return)
        rc, v, l = real_run('mip_row', exe, work)
        first = next((x for x in l if x.startswith('AGREES-UNCOVERED')), '')
        check('real.mip_row', v == 'AGREES-UNCOVERED' and unhit_list(first) == ['0x827FDBA0 taken'], '\n'.join(l[-6:]))
        rc, v, l = real_run('mip_row.twin_unsigned', exe, work)
        check('real.mip_twin', v == 'REFUTED' and 'byte31=128' in ' '.join(l), '\n'.join(l[-6:]))
        # PLACECOUNT0 (fd832a342): ours follows the loader's block-1 gate (b1.count=0 agrees now). S111 EQUIVTIER: with the file's counts read
        # as the signed halfwords they are, the loader reads a count of -1 (0xFFFF) as none. S111 PLACESIGNED: ours reads them signed too, and the
        # test's b3.count starts at 0 (the game wipes memory on a negative arena count), so the table now AGREES.
        rc, v, l = real_run('placement_table', exe, work)
        check('real.placement', v == 'AGREES-UNCOVERED', '\n'.join(l[-6:]))
        rc, v, l = real_run('placement_table.twin32', exe, work)
        # the twin is ours + 32-byte records: its first shrunk difference is its own .n=1
        check('real.placement_twin', v == 'REFUTED' and '.n=' in ' '.join(l) and 'table game=built ours=none' in ' '.join(l), '\n'.join(l[-6:]))
        if static_exe:
            rc, v, l = real_run('placement_table', static_exe, work)
            check('real.placement_static', v == 'NOT-ISOLABLE' and 'coverage tier' in ' '.join(l), '\n'.join(l[-6:]))
        else:
            NOT_RUN.append('real.placement_static (no --static-exe)')
    finally:
        shutil.rmtree(work, ignore_errors=True)


def report():
    bad = [r for r in RESULTS if not r[1]]
    for name, ok, detail in RESULTS:
        print(f'{"PASS" if ok else "FAIL"} {name}' + ('' if ok else f': {detail}'))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--units', action='store_true')
    ap.add_argument('--exe', default='', help='the covmap exe with the stub seam (equiv.py cov-build); default: the newest one')
    ap.add_argument('--static-exe', default='', help='an exe of the tree without the seam (a default-preset lane exe)')
    a = ap.parse_args()
    units()
    if a.units:
        bad = report()
        print(f'units: {len(RESULTS)} checks, {len(bad)} failed: {" ".join(n for n, _, _ in bad)}')
        return 1 if bad else 0
    end_to_end(a.exe, a.static_exe)
    bad = report()
    plant_bad = []
    for plant, want in PLANTS.items():
        env = dict(os.environ, EQUIV_PLANT=plant)
        r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--units'], capture_output=True, text=True, env=env)
        failed = {ln.split()[1].rstrip(':') for ln in r.stdout.splitlines() if ln.startswith('FAIL ')}
        ok = failed == want
        print(f'plant {plant}: {"fails exactly" if ok else "WRONG: fails"} {sorted(failed)}' + ('' if ok else f' (wanted {sorted(want)})'))
        if not ok:
            plant_bad.append(plant)
    print(f'equiv_selftest: {len(RESULTS)} checks, {len(bad)} failed; {len(PLANTS)} plants, {len(plant_bad)} wrong'
          + (f' ({" ".join(n for n, _, _ in bad)} {" ".join(plant_bad)})' if bad or plant_bad else '')
          + (f'; not run: {", ".join(NOT_RUN)}' if NOT_RUN else ''))
    return 1 if bad or plant_bad else 0


if __name__ == '__main__':
    sys.exit(main())
