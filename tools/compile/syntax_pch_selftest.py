# Selftest for game_syntax's precompiled header (S105 PCH). py -I tools\compile\syntax_pch_selftest.py
# Scratch trees + real clang on tiny units (seconds), a scratch store (GAME_SYNTAX_CACHE_DIR); no game, no build tree.
# The probe unit uses std::vector WITHOUT including it: it compiles only when the PCH really is in the parse (a pass proves use; a failure proves a skip / fallback).
#   1 built once, reused by a second unit (builds == 1), both units pass and the verdicts are stored under the PCH key (a new run hits)
#   2 a changed header (check_pch.h or one it includes) changes the key: new .pch, the unit sees the new header; unchanged header = same .pch, no build
#   3 a touched header (same bytes, new mtime) is rejected by clang and rebuilt once, the unit still passes
#   4 a unit with another flag, a unit that #defines before its first #include, and a unit with its own -include skip the PCH (probe unit FAILS there: no PCH in the parse)
#   5 a PCH build failure falls back to a plain parse, loudly; a unit needing the PCH fails (never a false pass); a self-contained unit passes
#   6 real errors under the PCH are reported: a narrowing error and a missing symbol
#   7 --no-cache (GAME_SYNTAX_NO_CACHE) = no PCH and no pch folder
#   8 prune_pch caps (entries, LRU, keep)
# Planted twins (each must be caught): stale PCH after a header edit (row 2), PCH used for other flags (row 4), a swallowed build failure (row 5).
import os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, os.environ.get('GS_DIR', str(HERE)))
TMP = Path(tempfile.mkdtemp(prefix='gs_pch_selftest_'))
os.environ['GAME_SYNTAX_CACHE_DIR'] = str(TMP / 'store')
os.environ.pop('GAME_SYNTAX_NO_CACHE', None)
import game_syntax as gs
import syntax_pch as sp
import syntax_store as st

FAILS = []


def ok(cond, text):
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


def tree(name, pch_body='#include <vector>\n#include "h.h"\n'):
    d = TMP / name
    (d / 'src').mkdir(parents=True)
    (d / 'src' / 'h.h').write_text('#pragma once\ninline int pch_val() { return 1; }\n')
    (d / 'src' / 'pch_units.h').write_text('#pragma once\n' + pch_body)   # S106 G4-4: the check's PCH is src/pch_units.h, the file the real build force-includes
    return d


def unit(d, name, body):
    p = d / 'src' / name
    p.write_text(body)
    return str(p).replace('\\', '/')


PROBE = 'int use() { std::vector<int> v; return pch_val() + (int)v.size(); }\n'   # needs <vector> and h.h from the PCH
ARGS = lambda d: ['-std=c++17', '-I' + str(d / 'src').replace('\\', '/')]


def ref_for(d, args=None):
    return sp.make_ref(d, args or ARGS(d))


def run(d, up, ref, args=None):
    wt = str(d).replace('\\', '/')
    return gs.syntax_check(args or ARGS(d), up, roots=((wt, '<ROOT>'),), pch=ref)


def pchs():
    return sorted((TMP / 'store' / 'pch').glob('*.pch')) if (TMP / 'store' / 'pch').is_dir() else []


def main():
    if not os.path.isfile(gs.CLANG):
        print('SKIP: clang not found')
        return 0
    try:
        # S106 G4-4 row 0: the UNSAFE twin under fix 1A. The PCH is the STL-only src/pch_units.h the real build force-includes; a unit that forgot a NON-PCH header (h.h) must FAIL.
        # (Before 1A the PCH header was an old check_pch.h with h.h / fx_registry.h inside: this row passed the unit wrongly on the S105 code; seen before the fix.)
        T = tree('wtT', pch_body='#include <vector>\n')
        ut = unit(T, 'ut.cpp', 'int useT() { std::vector<int> v; return pch_val() + (int)v.size(); }\n')   # forgot #include "h.h"; <vector> comes from the PCH, as in the build
        t0 = run(T, ut, ref_for(T))
        ok(t0[0] != 0 and 'pch_val' in t0[1], '0 unsafe twin: a unit that forgot a NON-PCH header (h.h) FAILS under the PCH (the build would reject it too)')
        ut2 = unit(T, 'ut2.cpp', '#include "h.h"\nint useT2() { std::vector<int> v; return pch_val() + (int)v.size(); }\n')   # forgot only <vector>: the force-include supplies it, like the build
        t1 = run(T, ut2, ref_for(T))
        ok(t1[0] == 0, '0 a unit that forgot only an STL header passes (the build force-includes the same STL header)')
        shutil.rmtree(TMP / 'store' / 'pch', ignore_errors=True)   # the rows below count .pch files from zero
        A = tree('wtA')
        u1, u2 = unit(A, 'u1.cpp', PROBE), unit(A, 'u2.cpp', PROBE.replace('use', 'use2'))
        r = ref_for(A)
        a = run(A, u1, r)
        b = run(A, u2, r)
        ok(a[0] == 0 and b[0] == 0, '1 both units pass with the PCH (the probe unit includes nothing: the PCH is in the parse)')
        ok(r['builds'] == 1 and len(pchs()) == 1, '1 the PCH was built once and reused by the second unit (builds %d, files %d)' % (r['builds'], len(pchs())))
        r2 = ref_for(A)
        a2 = run(A, u1, r2)
        ok(a2[3] and a2[0] == 0 and r2['builds'] == 0, '1 a second run is a store hit under the PCH key and builds no PCH')
        # plain parse of the same unit really fails (the probe is a valid probe)
        pl = gs.syntax_check(ARGS(A), u1, roots=(), pch=None)
        ok(pl[0] != 0, '1 control: the same unit without the PCH fails (so a pass above proves the PCH was used)')
        # 2 header edits
        p1 = pchs()[0]
        (A / 'src' / 'h.h').write_text('#pragma once\ninline int pch_val2() { return 1; }\n')   # the PCH's closure changes: pch_val is gone
        r3 = ref_for(A)
        c = run(A, u1, r3)
        ok(c[0] != 0 and 'pch_val' in c[1], '2 planted twin: after a header edit the stale PCH is NOT reused; the unit sees the new header and fails on the removed name')
        ok(r3['builds'] == 1 and len(pchs()) == 2 and r3['path'] != str(p1).replace('\\', '/'), '2 the changed closure built a new .pch (old one kept for LRU)')
        (A / 'src' / 'pch_units.h').write_text('#pragma once\n#include <vector>\n#include <string>\n#include "h.h"\n')
        r4 = ref_for(A)
        d_ = run(A, unit(A, 'u3.cpp', 'int use3() { std::string s; std::vector<int> v; return pch_val2() + (int)(s.size() + v.size()); }\n'), r4)
        ok(d_[0] == 0 and r4['builds'] == 1, '2 an edit of pch_units.h itself rebuilds (the new <string> is visible to the unit)')
        # 3 touched header
        h = A / 'src' / 'h.h'
        txt = h.read_text()
        time.sleep(0.05)
        h.write_text(txt)
        os.utime(h, (time.time() + 5, time.time() + 5))
        r5 = ref_for(A)
        u4 = unit(A, 'u4.cpp', 'int use4() { std::vector<int> v; return pch_val2() + (int)v.size(); }\n')
        before = gs.PCH_STATS['rebuilt']
        e = run(A, u4, r5)
        ok(e[0] == 0 and 'PCH FALLBACK' not in e[1], '3 a touched header (same bytes, new mtime) still passes (rebuilt: %d)' % (gs.PCH_STATS['rebuilt'] - before))
        # 4 skips
        os.environ['GAME_SYNTAX_PCH_ENTRIES'] = '8'
        ref = ref_for(A)
        other = ARGS(A) + ['-DOTHER=1']
        okf, why = sp.unit_ok(other, ref['args'], u1)
        f = run(A, u1, ref, args=other)
        ok(not okf and f[0] != 0, '4 planted twin: a unit with other flags does NOT use the PCH (unit_ok False: %s; the probe unit fails without it)' % why[:40])
        u5 = unit(A, 'u5.cpp', '#define EARLY 1\n#include <cstdint>\n' + 'int use5() { std::vector<int> v; return (int)v.size(); }\n')
        okd, why = sp.unit_ok(ARGS(A), ref['args'], u5)
        g = run(A, u5, ref)
        ok(not okd and 'before its first #include' in why and g[0] != 0, '4 a unit that #defines before its first #include skips the PCH (%s)' % why[:50])
        okm, why = sp.unit_ok(ARGS(A) + ['-include', 'x.h'], ref['args'], u1)
        ok(not okm, '4 a unit with its own -include (main.cpp) skips the PCH')
        u6 = unit(A, 'u6.cpp', '// a comment\n#pragma once\n#include <cstdint>\n' + PROBE.replace('use', 'use6'))
        ok(sp.unit_ok(ARGS(A), ref['args'], u6)[0], '4 positive control: comments / #pragma before the first #include do not skip')
        # 5 build failure
        B = tree('wtB', pch_body='#include <vector>\nint broken = ;\n')
        rb = ref_for(B)
        u7 = unit(B, 'u7.cpp', PROBE.replace('pch_val()', '1'))   # needs <vector> from the (broken) PCH
        u8 = unit(B, 'u8.cpp', '#include <vector>\nint use8() { std::vector<int> v; return (int)v.size(); }\n')
        n_before = gs.PCH_STATS['fallback']
        x = run(B, u7, rb)
        ok(x[0] != 0 and rb['note'].startswith('PCH build failed') and gs.PCH_STATS['fallback'] == n_before + 1, '5 planted twin: a PCH build failure falls back loudly and the unit that needs the PCH FAILS (no false pass; note: %s)' % rb['note'][:50])
        y = run(B, u8, rb)
        ok(y[0] == 0, '5 a self-contained unit passes after the fallback (plain parse)')
        ok(not pchs() or all(p.name != '' for p in pchs()) and not list((TMP / 'store' / 'pch').glob('*.tmp')), '5 no temp file left by the failed build')
        # 6 real errors under the PCH
        C = tree('wtC')
        un = unit(C, 'un.cpp', 'int narrow(double d) { int x{d}; return x; }\n')
        rc_ = ref_for(C)
        n = run(C, un, rc_)
        ok(n[0] != 0 and 'narrow' in n[1].lower() and rc_['builds'] == 1, '6 a narrowing error is reported under the PCH')
        us = unit(C, 'us.cpp', 'int missing() { return no_such_symbol(); }\n')
        m = run(C, us, rc_)
        ok(m[0] != 0 and 'no_such_symbol' in m[1], '6 a missing symbol is reported under the PCH')
        m2 = run(C, us, ref_for(C))
        ok(m2[3] and m2[0] != 0, '6 the failure is replayed as a failure from the store')
        # 9 S106 G4-4: a unit that skips the PCH still parses the build's force-include as plain text; _clean strips the build.ninja copy of it
        gs.FORCE_INC = str(A / 'src' / 'pch_units.h').replace('\\', '/')
        try:
            args_ninja = ARGS(A) + ['-include', gs.FORCE_INC]
            ok(gs._clean(args_ninja) == ARGS(A), "9 _clean strips the build's `-include src/pch_units.h` pair (the check re-adds its own)")
            u9 = unit(A, 'u9.cpp', '#define EARLY9 1\n#include <cstdint>\nint use9() { std::vector<int> v; return (int)v.size(); }\n')   # skips the PCH (defines first), needs <vector>: the force-include supplies it
            gs.FORCE_UNITS = {gs._norm(u9)}   # S106 G4-4F: the force-include is per unit
            k9 = gs.syntax_check(ARGS(A), u9, roots=(), pch=None)
            ok(k9[0] == 0, '9 a unit that skips the PCH still gets the force-include as plain text (like the build)')
        finally:
            gs.FORCE_INC = None
            gs.FORCE_UNITS = set()
        # 11 S106 G4-4F (review B1 / F1 / F4): the force-include is per unit. The launcher target (src/launcher, src/importer, audio_import) has none in the real build.
        gs.FORCE_INC = str(A / 'src' / 'pch_units.h').replace('\\', '/')
        try:
            ul = unit(A, 'ul.cpp', 'int uselaunch() { std::vector<int> v; return (int)v.size(); }\n')   # a launcher-style unit that forgot <vector>
            ug = unit(A, 'ug.cpp', 'int usegame() { std::vector<int> v; return (int)v.size(); }\n')     # a game unit: the force-include supplies <vector>
            fu = gs.force_units(str(A), ['src/ug.cpp', 'src/launcher/l.cpp', 'src/importer/i.cpp', 'src/audio_import/audio_import.cpp', 'src/audio_import/lookup.cpp', 'src/main.cpp']) if hasattr(gs, 'force_units') else None
            ok(fu is not None and sorted(os.path.basename(x) for x in fu) == ['lookup.cpp', 'ug.cpp'], '11 force_units: game units only (not launcher / importer / audio_import.cpp / main.cpp): %s' % (sorted(os.path.basename(x) for x in fu) if fu else fu))
            gs.FORCE_UNITS = fu if fu is not None else set()
            kl = gs.syntax_check(ARGS(A), ul, roots=(), pch=None)
            kg = gs.syntax_check(ARGS(A), ug, roots=(), pch=None)
            ok(kl[0] != 0 and 'vector' in kl[1], "11 B1 unsafe twin: a launcher-style unit that forgot <vector> FAILS (the launcher build has no force-include)")
            ok(kg[0] == 0, '11 B1 control: the same text as a game unit passes (force-include)')
            ok(sp.unit_ok(ARGS(A), ARGS(A), u5, force=True)[0] if 'force' in sp.unit_ok.__code__.co_varnames else False, '12 F4: a force-included game unit that #defines before its first #include uses the PCH (the build puts the STL first anyway)')
            ok(not sp.unit_ok(ARGS(A), ARGS(A), u5)[0], '12 F4 control: outside the force-include set the define-first skip stays')
        finally:
            gs.FORCE_INC = None
            gs.FORCE_UNITS = set() if hasattr(gs, 'FORCE_UNITS') else None
        # 13 S106 G4-4F (review F2): a unit a tools\**\build*.ps1 also compiles (no force-include there) must pass WITHOUT the force-include
        Tt = tree('wtTool', pch_body='#include <vector>\n')
        (Tt / 'tools').mkdir()
        (Tt / 'tools' / 'build_t.ps1').write_text('clang src/ut3.cpp src/ut4.cpp\n')
        ut3 = unit(Tt, 'ut3.cpp', 'int useT3() { std::vector<int> v; return (int)v.size(); }\n')   # relies on the force-include for <vector>
        ut4 = unit(Tt, 'ut4.cpp', '#include <vector>\nint useT4() { std::vector<int> v; return (int)v.size(); }\n')
        gs.FORCE_INC = str(Tt / 'src' / 'pch_units.h').replace('\\', '/')
        try:
            fl = {'src/ut3.cpp': ARGS(Tt), 'src/ut4.cpp': ARGS(Tt)}
            bad3 = gs.check_tool_builds(str(Tt), str(Tt), ['src/ut3.cpp', 'src/ut4.cpp'], fl, gs.tool_build_notes(str(Tt), ['src/ut3.cpp', 'src/ut4.cpp'])) if hasattr(gs, 'check_tool_builds') else None
            ok(bad3 is not None and [b[0] for b in bad3] == ['src/ut3.cpp'] and 'build_t.ps1' in bad3[0][1] and 'without pch_units.h' in bad3[0][1], '13 F2: a unit a tool build also compiles FAILS when it relies on the force-include (%s)' % (bad3,))
        finally:
            gs.FORCE_INC = None
        # 14 S106 G4-4F (m1): ensure returns (path, gen) read under the lock
        EE = tree('wtEns')
        er = ref_for(EE)
        et = sp.ensure(er, str(TMP / 'store'), gs._proc.run, gs.CLANG, gs.CLANG_ID, gs.norm_args, (), 120)
        ok(isinstance(et, tuple) and len(et) == 2 and et[0] and et[1] == er['gen'], '14 m1: ensure returns (path, gen)')
        # 10 S106 G4-4 (review_PCH 4 / 5 / 6): the generation check, an evicted file, a macro-only header edit, two processes on one key
        sd = str(TMP / 'store')

        def ens(r_, **kw):
            res_ = sp.ensure(r_, sd, gs._proc.run, gs.CLANG, gs.CLANG_ID, gs.norm_args, (), 120, **kw)
            return res_[0] if isinstance(res_, tuple) else res_
        E = tree('wtE')
        re_ = ref_for(E)
        pe = ens(re_)
        b0, g0 = re_['builds'], re_['gen']
        pe2 = ens(re_, force=True, seen_gen=g0 - 1)
        ok(pe2 == pe and re_['builds'] == b0, '10 a forced rebuild whose parse used an OLDER build of the .pch does not rebuild (generation check)')
        os.remove(pe)
        pe3 = ens(re_)
        ok(pe3 and os.path.isfile(pe3) and re_['builds'] == b0 + 1 and 'evicted' in re_['note'], '10 an evicted .pch is rebuilt once and the note says evicted (not "unknown")')
        os.remove(pe3)
        pe4 = ens(re_)
        ok(pe4 is None and 'vanished twice' in re_['note'], '10 a second eviction in one run gives up with a note (no endless rebuilds)')
        M = tree('wtM', pch_body='#include <vector>\n#include "m.h"\n')
        (M / 'src' / 'm.h').write_text('#pragma once\n#define PCH_MACRO 1\n')
        rm1 = ref_for(M)
        ens(rm1)
        (M / 'src' / 'm.h').write_text('#pragma once\n#define PCH_MACRO 2\n')
        rm2 = ref_for(M)
        ens(rm2)
        ok(rm1['path'] and rm2['path'] and rm1['path'] != rm2['path'], '10 a macro-only header edit (a #define value) changes the PCH key (-dD)')
        # two processes build the same key at once (the S105 review: os.replace over a .pch another clang has open failed and left a temp file)
        W = tree('wtW')
        helper = TMP / 'two_proc.py'
        helper.write_text('import os, sys\nsys.path.insert(0, %r)\nimport game_syntax as gs, syntax_pch as sp\n'
                          'r = sp.make_ref(sys.argv[1], ["-std=c++17", "-I" + sys.argv[1] + "/src"])\n'
                          'p = sp.ensure(r, os.environ["GAME_SYNTAX_CACHE_DIR"], gs._proc.run, gs.CLANG, gs.CLANG_ID, gs.norm_args, (), 120)\n'
                          'p = p[0] if isinstance(p, tuple) else p\nprint(p or "NONE " + r["note"])\n' % os.environ.get('GS_DIR', str(HERE)))
        procs = [subprocess.Popen([sys.executable, '-I', str(helper), str(W)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) for _ in range(2)]
        outs = [pr.communicate(timeout=300)[0].strip() for pr in procs]
        ok(all(o and not o.startswith('NONE') for o in outs) and outs[0] == outs[1] and not list((TMP / 'store' / 'pch').glob('*.tmp')),
           '10 two processes building the same key at once both get the one .pch and leave no temp file (%s)' % ' / '.join(o[-40:] for o in outs))
        # 15 S110 SYNTAXPCH: main.cpp through a PCH of src/pch_main.h (job flags carry the plain `-include pch_main.h` as with_pch adds it)
        MM = tree('wtMain')
        (MM / 'src' / 'pch_main.h').write_text('#pragma once\n#include <vector>\n#include "h.h"\n')
        (MM / 'src' / 'only_main.h').write_text('#pragma once\ninline int only_main_val() { return 3; }\n')
        mroot = str(MM).replace('\\', '/')
        mflags = ARGS(MM) + ['-include', mroot + '/src/pch_main.h']
        ref_m = lambda: gs.main_pch_ref(mroot, mroot, ARGS(MM))

        def run_main(body, refs, flags=None):
            up = unit(MM, 'main.cpp', body)
            r_ = gs.run_jobs([('src/main.cpp [on]', flags or mflags, up)], mroot, mroot, workers=1, pch_refs=refs)[0]
            return r_[1], r_[2]
        body = '#include "only_main.h"\nint m1() { std::vector<int> v; return pch_val() + only_main_val() + (int)v.size(); }\n'
        for k_ in (st.store_dir(),):
            for f_ in Path(k_).glob('*.json'):
                f_.unlink()
        used0, rm = gs.PCH_STATS['used'], ref_m()
        rc1, tx1 = run_main(body, [rm])
        ok(rc1 == 0 and rm['builds'] == 1 and gs.PCH_STATS['used'] == used0 + 1, '15 main.cpp parses through the PCH, built on the first run (builds %d, rc %d)' % (rm['builds'], rc1))
        rm2 = ref_m()
        rc2, _t = run_main(body + 'int m2() { return 2; }\n', [rm2])
        ok(rc2 == 0 and rm2['builds'] == 0 and rm2['path'], '15 the second run (new main.cpp text) reuses the .pch: no build')
        rc3, _t = run_main(body + 'int m3() { return 3; }\n', [])
        ok(rc3 == rc1 == 0, '15 --no-pch twin (no refs, plain-text pch_main.h): the same verdict')
        bad_body = body + 'int m4() { return no_such_main_symbol(); }\n'
        rc4, tx4 = run_main(bad_body, [ref_m()])
        rc5, tx5 = run_main(bad_body + '\n', [])
        ok(rc4 != 0 and 'no_such_main_symbol' in tx4 and rc5 != 0, '15 an error in main.cpp is reported through the PCH path and by the plain parse')
        (MM / 'src' / 'only_main.h').write_text('#pragma once\ninline int only_main_val() { return nope_planted; }\n')
        rc6, tx6 = run_main(body + 'int m5() { return 5; }\n', [ref_m()])
        ok(rc6 != 0 and 'nope_planted' in tx6, '15 planted syntax error in a header only main.cpp includes is REPORTED through the PCH path')
        (MM / 'src' / 'only_main.h').write_text('#pragma once\ninline int only_main_val() { return 3; }\n')
        (MM / 'src' / 'pch_main.h').write_text('#pragma once\n#include <vector>\n#include "h.h"\ninline int extra_v() { return 2; }\n')
        rme = ref_m()
        rc7, tx7 = run_main(body + 'int m6() { return extra_v(); }\n', [rme])
        ok(rc7 == 0 and rme['builds'] == 1 and rme['path'] != rm['path'] and os.path.isfile(rme['path']), '15 a change of pch_main.h itself rebuilds the PCH (the unit sees extra_v; builds %d)' % rme['builds'])
        # 7 no cache
        shutil.rmtree(TMP / 'store' / 'pch', ignore_errors=True)
        os.environ['GAME_SYNTAX_NO_CACHE'] = '1'
        D = tree('wtD')
        ud = unit(D, 'ud.cpp', PROBE)
        z = run(D, ud, ref_for(D))
        ok(z[0] != 0 and not pchs() and not (TMP / 'store' / 'pch').exists(), '7 --no-cache: no PCH is built or used (the probe unit fails) and no pch folder exists')
        os.environ.pop('GAME_SYNTAX_NO_CACHE', None)
        # 8 prune
        pd = TMP / 'store' / 'pch'
        pd.mkdir(parents=True, exist_ok=True)
        for i in range(5):
            (pd / ('k%d.pch' % i)).write_bytes(b'x' * 10)
            os.utime(pd / ('k%d.pch' % i), (time.time() - 100 + i,) * 2)
        n = sp.prune(str(TMP / 'store'), keep=str(pd / 'k0.pch'), max_entries=3)
        left = sorted(p.name for p in pd.glob('*.pch'))
        ok(n == 2 and left == ['k0.pch', 'k3.pch', 'k4.pch'], '8 the entry cap removes the oldest .pch files but never `keep` (left %s)' % left)
        ok(st.prune(cap_mb=0) >= 0 and sorted(p.name for p in pd.glob('*.pch')) == left, '8 the json store prune leaves the pch folder alone')
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    print('FAIL: %d' % len(FAILS) if FAILS else 'PASS: syntax_pch selftest')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
