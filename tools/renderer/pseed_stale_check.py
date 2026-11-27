#!/usr/bin/env python3
"""S110 PIPECENSUS: the release-time stale-seed test (0 game runs, no GPU). A pipeline seed is hashes of OUR compiled-in SPIR-V, so any shader change makes some records stale: they
resolve to nothing and the player's boot prepares less than the list promises. This tool says so BEFORE a release ships it. It is a thin, release-callable wrapper over
pipeline_seed.py's own checker (no second copy of the key logic):
  py -I tools\\renderer\\pseed_stale_check.py --list db\\shaders\\shader_list.txt            # builds a catalog from THIS tree's SPIR-V (clang++, ~10 s), then checks
  py -I tools\\renderer\\pseed_stale_check.py --list out\\pipeseed\\candidate_seed.txt --catalog out\\tools\\pseed_catalog.txt   # a catalog already built from this tree
  py -I tools\\renderer\\pseed_stale_check.py --selftest
Exit 0 = every R line resolves against the catalog and the list passes pipeline_seed.check_text (count, checksum, version, depth token, no duplicate); 1 = a stale record or any other
problem (the stale ones are counted by program-name family); 2 = no catalog could be built. The minimum count (pseed_min.txt beside the list) is held when that file exists, else not
(a candidate under out\\ has none; the shipped list's own pseed_min.txt applies to it). It does not touch release.ps1 and writes nothing but the catalog when it builds one
(S111 PSEEDSTALE: in a temp folder it removes again). check.py's row pipeline_seed runs it as `--root TREE` (gated in check_gates.py on the list and the compiled SPIR-V);
a stale record whose shader changed its words but not its interface is moved by tools\\renderer\\pseed_rekey.py (0 game runs)."""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import pipeline_seed as P   # noqa: E402

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
FLAGS = ['-std=c++20', '-Wall', '-Wextra', '-Wno-deprecated-declarations', '-D_CRT_SECURE_NO_WARNINGS']


def build_catalog(root, out_dir):
    """Builds tools\\renderer\\pseed_catalog.cpp over the tree's compiled-in SPIR-V (src\\renderer\\shaders\\spv\\fx_spv_data.cpp, the effects, and fx_own_shaders.cpp, kOwnShaders) and prints its catalog to out_dir\\pseed_catalog.txt. Returns the path or None."""
    if not os.path.exists(CLANG):
        print('no compiler at ' + CLANG)
        return None
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, 'pseed_catalog.exe')
    b = subprocess.run([CLANG] + FLAGS + ['-o', exe, os.path.join(root, 'tools', 'renderer', 'pseed_catalog.cpp'), os.path.join(root, 'src', 'renderer', 'shaders', 'spv', 'fx_spv_data.cpp'), os.path.join(root, 'src', 'renderer', 'shaders', 'spv', 'fx_own_shaders.cpp')],   # S111 TOOLLINK-D: kOwnShaders is its own unit
                       capture_output=True, text=True, timeout=900)
    if b.returncode != 0:
        print((b.stdout + b.stderr)[-1500:])
        return None
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or r.stdout.count('\n') < 100:
        print('pseed_catalog printed %d lines (exit %d)' % (r.stdout.count('\n'), r.returncode))
        return None
    path = os.path.join(out_dir, 'pseed_catalog.txt')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(r.stdout)
    return path


def family(name):
    """'yBG_Default_vertex_x + yBG_Default_fragment_y' -> 'yBG_Default' (the program's effect, for the report only)."""
    m = re.match(r'([A-Za-z0-9]+(?:_[A-Za-z0-9]+)?)', name.split(' + ')[0])
    return m.group(1) if m else (name or '?')


def stale_records(text, catalog):
    """The R lines whose vertex or fragment hash the catalog lacks: [(line number, name, which)]."""
    out = []
    for no, ln in enumerate(text.split('\n'), 1):
        ln = ln.rstrip(' \r')
        if not ln.startswith('R '):
            continue
        hx = ln[2:].lstrip(' ')
        if len(hx) != P.RECORD * 2 or not re.fullmatch(r'[0-9a-fA-F]+', hx):
            continue
        r = bytes.fromhex(hx)
        which = [w for w, h in (('vertex', P.vhash(r)), ('fragment', P.fhash(r))) if h != 0 and h not in catalog]
        if which:
            out.append((no, r[P.NAME:].split(b'\0')[0].decode('latin1'), '+'.join(which)))
    return out


def verdict(text, catalog, version, stages=None, minimum=0):
    """(problems, stale): problems = pipeline_seed.check_text's list (empty = good), stale = stale_records. A stale record is always also a problem."""
    return P.check_text(text, catalog, version, stages, minimum), stale_records(text, catalog)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=ROOT, help='the tree whose SPIR-V the catalog is built from and whose list is checked (check.py passes the checked tree)')
    ap.add_argument('--list', default='', help='default: ROOT\\db\\shaders\\shader_list.txt')
    ap.add_argument('--catalog', default='', help='a pseed_catalog.exe output built from this tree; default: build one now into a temp folder')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    root = os.path.abspath(a.root)
    lst = a.list or os.path.join(root, 'db', 'shaders', 'shader_list.txt')
    cat_path, tmp = a.catalog, None
    if not cat_path:
        tmp = tempfile.mkdtemp(prefix='pseed_stale_')
        cat_path = build_catalog(root, tmp)
        if not cat_path:
            shutil.rmtree(tmp, ignore_errors=True)
            print('pseed_stale_check: no catalog (build failed)')
            return 2
    stages = {}
    catalog = P.load_catalog(cat_path, stages)
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)   # S111 PSEEDSTALE: check.py runs this on every seed / shader diff; the exe and catalog are not kept
    with open(lst, encoding='utf-8', errors='replace') as f:
        text = f.read()
    minimum = P.read_min(os.path.join(os.path.dirname(os.path.abspath(lst)), 'pseed_min.txt'))
    if minimum is None and not os.path.exists(os.path.join(os.path.dirname(os.path.abspath(lst)), 'pseed_min.txt')):
        minimum = 0   # a list outside db\shaders carries no committed minimum
    bad, stale = verdict(text, catalog, P.list_version(root), stages, minimum)
    for p in bad[:40]:
        print('FAIL ' + p)
    fam = collections.Counter(family(n) for _, n, _ in stale)
    if stale:
        print('stale: %d R lines name a shader the current catalog lacks; by family: %s' % (len(stale), ', '.join('%s %d' % kv for kv in sorted(fam.items()))))
        print('fix (0 game runs): py -I tools\\renderer\\pseed_rekey.py moves a record whose shader changed its words but not its interface onto the current hash; '
              'a record it leaves needs a harvest (tools\\renderer\\pipeline_census.py + pipeline_seed.py merge)')
    n = sum(1 for ln in text.splitlines() if ln.startswith('R '))
    print('pseed_stale_check: %s (%d R lines, %d stale, %d problems)' % ('FAILED' if bad or stale else 'ok', n, len(stale), len(bad)))
    return 1 if bad or stale else 0


def selftest():
    fails = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            fails.append(what)

    cat = {0xAAAA, 0xBBBB, 0xCCCC}
    good = [P.synth(0xAAAA, 0xBBBB, name=b'yBG_Default_v + yBG_Default_f'), P.synth(0xCCCC, 0, name=b'rdr_ui')]
    text = '\n'.join(P.seed_lines(good, 2)) + '\n'
    bad, stale = verdict(text, cat, 2)
    need(bad == [] and stale == [], 'a current seed passes with no stale record')
    # the planted stale key: one record's fragment hash is one no catalog blob has (a shader changed since the seed was made), the list otherwise valid (its count and checksum rebuilt)
    old = good + [P.synth(0xAAAA, 0xDEAD, name=b'yCh_skin_v + yCh_skin_f')]
    text2 = '\n'.join(P.seed_lines(old, 2)) + '\n'
    bad2, stale2 = verdict(text2, cat, 2)
    need(len(stale2) == 1 and stale2[0][1].startswith('yCh_skin') and stale2[0][2] == 'fragment', 'a planted stale key (a fragment hash no blob has) is found, by name and stage')
    need(any('not in the shader catalog' in p for p in bad2), 'planted stale key: the pipeline_seed checker fails the list too (exit 1 path)')
    # the same list against a catalog that lost a shader: a real shader change
    bad3, stale3 = verdict(text, {0xAAAA, 0xBBBB}, 2)
    need(len(stale3) == 1 and stale3[0][1] == 'rdr_ui', 'a catalog that lost a shader makes the record naming it stale')
    # a zero fragment hash (vertex-only program) is never stale
    need(stale_records('\n'.join(P.seed_lines([P.synth(0xCCCC, 0)], 2)), cat) == [], 'a vertex-only record (fragment hash 0) is not stale')
    need(family('yBG_Default_vertex_3 + yBG_Default_fragment_3') == 'yBG_Default' and family('rdr_ui') == 'rdr_ui', 'family names the effect for the report')
    # the CLI path end to end on a temp list + temp catalog file: exit 1 with the stale key, 0 without
    with tempfile.TemporaryDirectory() as d:
        cp = os.path.join(d, 'cat.txt')
        with open(cp, 'w') as f:
            f.write('\n'.join('%016x 8 %s' % (h, s) for h, s in ((0xAAAA, 'V'), (0xBBBB, 'F'), (0xCCCC, 'V'))) + '\n' + '\n'.join('%016x 8 V' % (0x1000 + i) for i in range(120)) + '\n')
        lp = os.path.join(d, 'l.txt')
        for body, want in ((text, 0), (text2, 1)):
            with open(lp, 'w') as f:
                f.write(body)
            r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--list', lp, '--catalog', cp], capture_output=True, text=True, timeout=60)
            ver = P.list_version(ROOT)
            if ver != 2:   # the synthetic lists carry version 2: a build whose list version moved gets the H line rewritten
                with open(lp, 'w') as f:
                    f.write('\n'.join(P.seed_lines(good if want == 0 else old, ver)) + '\n')
                r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--list', lp, '--catalog', cp], capture_output=True, text=True, timeout=60)
            need(r.returncode == want, 'CLI: %s list exits %d (got %d)' % ('a current' if want == 0 else 'a stale', want, r.returncode))
    print('pseed_stale_check selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'every check passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
