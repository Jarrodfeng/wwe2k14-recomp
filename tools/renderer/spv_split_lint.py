#!/usr/bin/env python3
"""S105 SPV lint: the effects' SPIR-V words live in ONE data unit, no header carries them.

  py -I tools\\renderer\\spv_split_lint.py [--root DIR] [--tools-ref REF] [--base REF] [--plant]

Checks (text only, nothing is run):
  1. no header under src\\renderer\\shaders\\spv\\ holds a word array (`= {`): fx_<effect>.h and fx_registry.h only DECLARE (`extern const uint32_t kFx_x[N];`), so no unit that includes the
     registry parses the SPIR-V text (13 s per unit before S105 SPV);
  2. every declared array is defined exactly once, in fx_<effect>_data.inc, with the same size, and fx_spv_data.cpp includes every effect's header and data file;
  3. no .cpp / .h / .inc outside fx_spv_data.cpp includes a *_data.inc;
  4. every effect has one `// fx_<effect> words sha256: H` line right before its `#include "fx_<effect>_data.inc"`, and H is the sha256 of that file (a hand-resolved merge or a stale line FAILs);
  5. every tool build script (tools\\**\\*.ps1 / *.py) that names a .cpp which (transitively) includes fx_registry.h also names fx_spv_data.cpp, or globs src\\renderer -Recurse, or links
     rsc_obj; a script with /force:unresolved must name the unit (it would link every kFx_ pointer as null). --tools-ref REF reads the scripts from git REF (proves the check on old scripts);
  6. tools\\renderer\\spv_unit_exempt.txt lists the scripts that only name a user as text (each with its reason);
  7. with --base REF (optional one-off identity proof, not in the gate): every array of REF has the same words now, and none is missing;
  8. --plant: structural twins that need no base: the unit minus one `#include`, one stale sha line, a script without the unit, a /force:unresolved script without it (with --base also one
     changed word). Each MUST be reported.
Exit 0 = clean."""
import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys

REL = 'src/renderer/shaders/spv/'
DEF = re.compile(r'(?:inline constexpr|extern const) uint32_t (kFx_\w+)\[(\d*)\] = \{(.*?)\};', re.S)
DECL = re.compile(r'^extern const uint32_t (kFx_\w+)\[(\d+)\];', re.M)
FAILS = 0


def say(ok, msg):
    global FAILS
    FAILS += 0 if ok else 1
    print(('ok   ' if ok else 'FAIL ') + msg)


def words(body):
    return [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body)]


def parse(text):
    return {m.group(1): (m.group(2), words(m.group(3))) for m in DEF.finditer(text.replace('\r\n', '\n'))}


def registry_users(root):
    """The .cpp files (src and tools) that include fx_registry.h directly or through any header, by include basename (an over-approximation only asks for the unit)."""
    inc, refs = {}, set()
    for top in ('src', 'tools'):
        for r, ds, fs in os.walk(os.path.join(root, top)):
            ds[:] = [x for x in ds if x not in ('out', 'node_modules', '.git')]
            if r.replace('\\', '/').endswith('src/renderer/shaders/spv'):
                continue
            for f in fs:
                if f.endswith(('.cpp', '.cc', '.h', '.hpp', '.inc')):
                    try:
                        t = open(os.path.join(r, f), encoding='utf-8', errors='replace').read()
                    except OSError:
                        continue
                    inc[os.path.join(r, f)] = {os.path.basename(x) for x in re.findall(r'#\s*include\s+"([^"]+)"', t)}
                    if re.search(r'\bkVariants\b|\bkHashes\b|\bkFx_', t):   # odr-uses the tables (a unit that only includes a header that declares types links without the words)
                        refs.add(f)
    hit = set(refs)   # the files that odr-use the tables; users = those .cpp plus the .cpp that include one of them directly (deeper is over-approximation: thin_draw.h is everywhere)
    allc = [os.path.basename(p) for p in inc if p.endswith(('.cpp', '.cc'))]
    return sorted({os.path.basename(p) for p, i in inc.items() if (i & hit or os.path.basename(p) in hit) and p.endswith(('.cpp', '.cc')) and allc.count(os.path.basename(p)) == 1})   # a name shared by two files (main.cpp) cannot be told apart in a script


def tool_scripts(root, ref):
    out = {}
    if ref:
        g = subprocess.run(['git', '-C', root, 'ls-tree', '-r', '--name-only', ref, 'tools'], capture_output=True, text=True)
        for n in g.stdout.split():
            if n.endswith(('.ps1', '.py')):
                out[n] = subprocess.run(['git', '-C', root, 'show', '%s:%s' % (ref, n)], capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    else:
        for r, _, fs in os.walk(os.path.join(root, 'tools')):
            for f in fs:
                if f.endswith(('.ps1', '.py')):
                    try:
                        out[os.path.relpath(os.path.join(r, f), root).replace('\\', '/')] = open(os.path.join(r, f), encoding='utf-8', errors='replace').read()
                    except OSError:
                        pass
    return out


READ_LINE = re.compile(r"""read|open\(|Get-Content|Select-String|-match|re\.(search|match|findall|compile)|\br['"]|\bin t\b|\.text|assert|PASS|FAIL""")


def script_misses(scripts, users):
    """Scripts that compile or link a registry user without the data unit. A user counts as compiled when a line names its .cpp (quoted, a path piece) and is not a text read of it
    (read / open / regex / Get-Content lines are the tests that slice a source as text). A script is fine when it names the unit, globs src\\renderer -Recurse, or links rsc_obj."""
    bad = []
    for n, t in scripts.items():
        if n.endswith('tools/renderer/spv_split_lint.py') or not re.search(r'clang|rsc_obj|cl\.exe|force:unresolved', t):
            continue
        if 'fsyntax-only' in t and 'fuse-ld' not in t and 'force:unresolved' not in t:
            continue   # a syntax pass never links
        named = False
        for ln in t.splitlines():
            if READ_LINE.search(ln):
                continue
            if any(re.search(r"""[\\/'"\s(,]%s['"\s)\],]""" % re.escape(u), ln) for u in users):
                named = True
                break
        if not named:
            continue
        has = 'fx_spv_data' in t or re.search(r"src.renderer['\\)\s]*-Recurse[^\n]*\*\.cpp", t) or 'rsc_obj' in t
        if not has or ('force:unresolved' in t and 'fx_spv_data' not in t):
            bad.append(n)
    return bad


def exempt(root):
    """tools/renderer/spv_unit_exempt.txt: `script | reason` for a script that names a registry user's .cpp without linking it (a text slice, a doc line, or measured: llvm-nm shows no undefined kFx_)."""
    out = set()
    try:
        for ln in open(os.path.join(root, 'tools', 'renderer', 'spv_unit_exempt.txt'), encoding='utf-8'):
            if '|' in ln and not ln.lstrip().startswith('#'):
                out.add(ln.split('|')[0].strip())
    except OSError:
        pass
    return out


def sha_bad(lines, d):
    out = []
    for i, l in enumerate(lines):
        m = re.match(r'#include "(fx_\w+)_data\.inc"', l)
        if m:
            cur = open(os.path.join(d, m.group(1) + '_data.inc'), encoding='utf-8', errors='replace', newline='').read().replace('\r\n', '\n')
            prev = re.match(r'// %s words sha256: ([0-9a-f]{64})$' % m.group(1), lines[i - 1]) if i else None
            if not prev or prev.group(1) != hashlib.sha256(cur.encode()).hexdigest():
                out.append(m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--base')
    ap.add_argument('--tools-ref')
    ap.add_argument('--plant', action='store_true')
    a = ap.parse_args()
    d = os.path.join(a.root, *REL.strip('/').split('/'))
    headers = sorted(glob.glob(os.path.join(d, 'fx_*.h')))
    incs = sorted(glob.glob(os.path.join(d, 'fx_*_data.inc')))
    fat = [os.path.basename(p) for p in headers if '= {' in open(p, encoding='utf-8', errors='replace').read() and not p.endswith('fx_registry.h')]
    reg = open(os.path.join(d, 'fx_registry.h'), encoding='utf-8', errors='replace').read()
    fat += ['fx_registry.h'] if 'uint32_t kFx_' in reg else []
    say(not fat, 'no spv header holds a word array (%d headers; offenders: %s)' % (len(headers), fat[:4] or 'none'))
    declared, defined = {}, {}
    for p in headers:
        for m in DECL.finditer(open(p, encoding='utf-8', errors='replace').read()):
            declared[m.group(1)] = int(m.group(2))
    now = {}
    for p in incs:
        for k, (n, w) in parse(open(p, encoding='utf-8', errors='replace').read()).items():
            defined[k] = defined.get(k, 0) + 1
            now[k] = (n, w)
    bad = [k for k in declared if defined.get(k) != 1 or int(now[k][0] or -1) != declared[k] or len(now[k][1]) != declared[k]]
    bad += [k for k in defined if k not in declared]
    say(declared and not bad, '%d arrays declared, each defined once in a data .inc with its size (bad: %s)' % (len(declared), bad[:3] or 'none'))
    unit = open(os.path.join(d, 'fx_spv_data.cpp'), encoding='utf-8', errors='replace').read()
    effects = re.findall(r'#include "(fx_\w+\.h)"', reg)   # the effects the registry knows (a stale fx_BumpMap.h with no array is not one)

    def missing_of(text):
        return [n for n in effects + [e.replace('.h', '_data.inc') for e in effects] if '"%s"' % n not in text]
    missing = missing_of(unit)
    say(bool(effects) and not missing, 'fx_spv_data.cpp includes every effect header and data file (missing %s)' % (missing[:3] or 'none'))
    stray = []
    for root, _, fs in os.walk(os.path.join(a.root, 'src')):
        for f in fs:
            if f.endswith(('.cpp', '.h', '.inc')) and f != 'fx_spv_data.cpp':
                if re.search(r'#\s*include\s+"[^"]*fx_\w+_data\.inc"', open(os.path.join(root, f), encoding='utf-8', errors='replace').read()):
                    stray.append(f)
    say(not stray, 'only fx_spv_data.cpp includes a data .inc (strays: %s)' % (stray[:3] or 'none'))
    unit_lines = unit.replace('\r\n', '\n').split('\n')
    sb = sha_bad(unit_lines, d)
    say(not sb, 'fx_spv_data.cpp: one current sha line per effect, %d effects (stale or missing: %s)' % (len(effects), sb[:3] or 'none'))
    scripts = tool_scripts(a.root, a.tools_ref)
    users = registry_users(a.root)
    ex = exempt(a.root)
    miss = [m for m in script_misses(scripts, users) if m not in ex]
    stale = [e for e in ex if e not in scripts]
    say(not stale, 'spv_unit_exempt.txt names only existing scripts (stale: %s)' % (stale[:3] or 'none'))
    say(bool(users) and bool(scripts) and not miss, 'every tool build naming a registry user names fx_spv_data.cpp (%d users, %d scripts; missing: %s)' % (len(users), len(scripts), miss[:12] or 'none'))
    if a.plant:
        say(missing_of('\n'.join(l for l in unit_lines if l != '#include "fx_yBlur.h"')) == ['fx_yBlur.h'], 'planted twin (the unit minus #include "fx_yBlur.h") is caught')
        stale = [re.sub(r'sha256: [0-9a-f]', 'sha256: %s' % ('0' if '0' not in l.split('sha256: ')[-1][:1] else '1'), l, 1) if l.startswith('// fx_yBlur words') else l for l in unit_lines]
        say(sha_bad(stale, d) == ['fx_yBlur'], 'planted twin (one stale sha line) is caught')
        victim = next((n for n in sorted(scripts) if 'fx_spv_data' in scripts[n] and n in script_misses({n: scripts[n].replace('fx_spv_data', 'fx_spv_dat_')}, users) or False), None)
        say(victim is not None, 'planted twin (a script with the unit removed: %s) is caught' % victim)
        say(script_misses({'tools/planted_force.ps1': 'clang++ x.cpp %s -Wl,/force:unresolved' % users[0]}, users) == ['tools/planted_force.ps1'], 'planted twin (a /force:unresolved script without the unit) is caught')
    if a.base:
        base = {}
        for p in headers + incs:
            g = subprocess.run(['git', '-C', a.root, 'show', '%s:%s%s' % (a.base, REL, os.path.basename(p))], capture_output=True, text=True, encoding='utf-8', errors='replace')
            if g.returncode == 0:
                for k, (n, w) in parse(g.stdout).items():
                    base[k] = w

        def compare(cur):
            return [k for k, w in base.items() if k not in cur or cur[k][1] != w]
        diff = compare(now)
        say(bool(base) and not diff, 'byte-identical to %s: %d arrays compared, changed or missing: %s' % (a.base, len(base), diff[:3] or 'none'))
        if a.plant and base:
            k = sorted(now)[len(now) // 2]
            twin = dict(now)
            w = list(now[k][1])
            w[len(w) // 2] ^= 1
            twin[k] = (now[k][0], w)
            say(compare(twin) == [k], 'planted twin (one word of %s changed) is caught' % k)
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
