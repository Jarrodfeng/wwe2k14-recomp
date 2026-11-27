#!/usr/bin/env python3
"""S105 SPV lint: the effects' SPIR-V words live in ONE data unit, no header carries them.

  py -I tools\\renderer\\spv_split_lint.py [--root DIR] [--base REF] [--plant]

Checks (text only, nothing is run):
  1. no header under src\\renderer\\shaders\\spv\\ holds a word array (`= {`): fx_<effect>.h and fx_registry.h only DECLARE (`extern const uint32_t kFx_x[N];`), so no unit that includes the
     registry parses the SPIR-V text (13 s per unit before S105 SPV);
  2. every declared array is defined exactly once, in fx_<effect>_data.inc, with the same size, and fx_spv_data.cpp includes every effect's header and data file;
  3. no .cpp / .h / .inc outside fx_spv_data.cpp includes a *_data.inc;
  4. every effect has one `// fx_<effect> words sha256: H` line right before its `#include "fx_<effect>_data.inc"`, and H is the sha256 of that file (a hand-resolved merge or a stale line FAILs);
  5. S111 TOOLLINK: fx_registry.h names no kFx_ array outside `sizeof(` (sizeof's operand is unevaluated: no link reference; the whole header, not only the kVariants rows). The words are reached only through
     rdr::fx::Words (defined in fx_spv_data.cpp from its kWords table), so a unit that reads kVariants for names, sizes or constants links without the 86 MB data unit, and a unit
     that uploads words and lacks it fails at its own link with `undefined symbol: rdr::fx::Words`. Its planted twins (a row holding the address again; a new table outside kVariants
     holding one) run every time;
  5b. S111 TOOLLINK-D: kOwnShaders is its own small unit (fx_own_shaders.cpp, the rdr_* words); fx_spv_data.cpp holds only the effects. A tool build script that lists
     fx_spv_data.cpp while the .cpp files it names (with their includes) never call rdr::fx::Words only reads metadata and must not list it (tools\\renderer\\spv_readers.py;
     a script that enumerates its sources is not judged). Planted twins: a metadata-only script is caught, the same naming front_end.cpp passes;
  6. with --base REF (optional one-off identity proof, not in the default run): every array of REF has the same words now, and none is missing;
  7. --plant: structural twins that need no base: the unit minus one `#include`, one stale sha line, one own shader's include or sha line (with --base also one changed word).
     Each MUST be reported.
Exit 0 = clean."""
import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv_readers   # S111 TOOLLINK-D: which build scripts list the data unit for nothing

REL = 'src/renderer/shaders/spv/'
DEF = re.compile(r'(?:inline constexpr|extern const) uint32_t (kFx_\w+)\[(\d*)\] = \{(.*?)\};', re.S)
DECL = re.compile(r'^extern const uint32_t (kFx_\w+)\[(\d+)\];', re.M)
ROWS = re.compile(r'^inline constexpr Variant kVariants\[\] = \{\n(.*?)\n\};', re.S | re.M)
FAILS = 0


def say(ok, msg):
    global FAILS
    FAILS += 0 if ok else 1
    print(('ok   ' if ok else 'FAIL ') + msg)


def words(body):
    return [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body)]


def parse(text):
    return {m.group(1): (m.group(2), words(m.group(3))) for m in DEF.finditer(text.replace('\r\n', '\n'))}


def row_refs(reg):
    """S111 TOOLLINK: (kVariants rows, the kFx_ names fx_registry.h holds outside `sizeof(...)`, comments and #include lines), or (0, None) when the table is not found.
    The whole header is read, not only the kVariants rows (review): any table of it that names a data-unit array odr-uses it."""
    text = reg.replace('\r\n', '\n')
    m = ROWS.search(text)
    if not m:
        return 0, None
    rows = len(re.findall(r'^\s*\{"', m.group(1), re.M))
    code = re.sub(r'//[^\n]*', ' ', re.sub(r'^\s*#\s*include[^\n]*', ' ', text, flags=re.M))
    return rows, sorted(set(re.findall(r'\bkFx_\w+', re.sub(r'\bsizeof\s*\(\s*(?:::)?\w+\s*\)', ' ', code))))


def own_bad(lines, d):
    """S110 RDRPREP: the own-shader block (fx_own_shaders.cpp since S111 TOOLLINK-D): each `#include "rdr_*.h"` has a sha line (of that header's text) right above it and a kOwnShaders row of its array; each row's header is included.
    Returns the names that are stale, unlisted or missing."""
    text = '\n'.join(lines)
    out = []
    names = [m.group(1) for l in lines for m in [re.match(r'#include "(rdr_\w+)\.h"', l)] if m]
    for i, l in enumerate(lines):
        m = re.match(r'#include "(rdr_\w+)\.h"', l)
        if m:
            cur = open(os.path.join(d, m.group(1) + '.h'), encoding='utf-8', errors='replace', newline='').read().replace('\r\n', '\n')
            prev = re.match(r'// %s words sha256: ([0-9a-f]{64})$' % m.group(1), lines[i - 1]) if i else None
            if not prev or prev.group(1) != hashlib.sha256(cur.encode()).hexdigest():
                out.append(m.group(1))
            if not re.search(r'\{"%s", ::kSpv_%s, sizeof\(::kSpv_%s\)\}' % ((m.group(1),) * 3), text):
                out.append(m.group(1) + ' (no kOwnShaders row)')
    for n in re.findall(r'\{"(\w+)", ::kSpv_\1, sizeof', text):
        if n not in names:
            out.append(n + ' (row without its include)')
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
    own_unit = open(os.path.join(d, 'fx_own_shaders.cpp'), encoding='utf-8', errors='replace').read()   # S111 TOOLLINK-D: the kOwnShaders table's own unit
    own_lines = own_unit.replace('\r\n', '\n').split('\n')
    effects = re.findall(r'#include "(fx_\w+\.h)"', reg)   # the effects the registry knows (a stale fx_BumpMap.h with no array is not one)

    def missing_of(text):
        return [n for n in effects + [e.replace('.h', '_data.inc') for e in effects] if '"%s"' % n not in text]
    missing = missing_of(unit)
    say(bool(effects) and not missing, 'fx_spv_data.cpp includes every effect header and data file (missing %s)' % (missing[:3] or 'none'))
    stray = []
    for root, _, fs in os.walk(os.path.join(a.root, 'src')):
        for f in fs:
            if f.endswith(('.cpp', '.h', '.inc')) and f != 'fx_spv_data.cpp' and not re.fullmatch(r'(scene|post)_spirv_data(_\d+)?\.cpp', f):   # the S110 SPIRVDATA data units (tools\shaders\spv_data_identity.py checks each .inc is in exactly one)
                if re.search(r'#\s*include\s+"[^"]*fx_\w+_data\.inc"', open(os.path.join(root, f), encoding='utf-8', errors='replace').read()):
                    stray.append(f)
    say(not stray, 'only fx_spv_data.cpp includes a data .inc (strays: %s)' % (stray[:3] or 'none'))
    unit_lines = unit.replace('\r\n', '\n').split('\n')
    sb = sha_bad(unit_lines, d)
    say(not sb, 'fx_spv_data.cpp: one current sha line per effect, %d effects (stale or missing: %s)' % (len(effects), sb[:3] or 'none'))
    ob = own_bad(own_lines, d)
    own_n = len([l for l in own_lines if re.match(r'#include "rdr_\w+\.h"', l)])
    say(own_n > 0 and not ob, 'fx_own_shaders.cpp: the kOwnShaders table holds %d own shaders, each with its include, a current sha line and a row (bad: %s)' % (own_n, ob[:3] or 'none'))
    say(not [l for l in unit_lines if re.match(r'#include "rdr_\w+\.h"', l)] and 'kOwnShaders' not in re.sub(r'//[^\n]*', '', unit), 'fx_spv_data.cpp holds no own shader and no kOwnShaders (they are fx_own_shaders.cpp)')
    # S111 TOOLLINK-D: a build script that only READS metadata does not list the data unit (spv_readers.py)
    readers_bad, needs = spv_readers.scan(a.root)
    say(bool(needs) and not readers_bad, 'no tool build script lists fx_spv_data.cpp for sources that never call rdr::fx::Words (%d files call it or read kOwnShaders; offenders: %s)' % (len(needs), sorted(readers_bad)[:4] or 'none'))
    rd_twin = {'tools/scene/build_twin_unit.ps1': "$srcs = @((Join-Path $repo 'src\\renderer_scene\\char_effects.cpp'), (Join-Path $repo 'src\\renderer\\shaders\\spv\\fx_spv_data.cpp'))"}
    say(bool(spv_readers.scan(a.root, rd_twin)[0]) and os.path.isfile(os.path.join(a.root, 'src', 'renderer_scene', 'char_effects.cpp')), 'planted twin (a script listing fx_spv_data.cpp for char_effects.cpp, metadata only) is caught')
    rd_ok = {'tools/scene/build_twin_unit.ps1': rd_twin['tools/scene/build_twin_unit.ps1'].replace('char_effects.cpp', 'front_end.cpp')}
    say(not spv_readers.scan(a.root, rd_ok)[0], 'planted twin (the same script naming front_end.cpp, a Words caller) passes')
    rows, refs = row_refs(reg)
    say(rows > 0 and refs == [], 'fx_registry.h names no kFx_ outside sizeof( (kVariants %d rows; offenders: %s)' % (rows, 'table not found' if refs is None else refs[:3] or 'none'))
    # S111 TOOLLINK planted twin (every run): the first row holding its words' address again, as the rows did before (`{"x", kFx_x, sizeof(kFx_x), ...}`)
    twin = re.sub(r'(\{"\w+", )sizeof\((kFx_\w+)\)', r'\1\2, sizeof(\2)', reg.replace('\r\n', '\n'), count=1)
    t_rows, t_refs = row_refs(twin)
    say(twin != reg.replace('\r\n', '\n') and bool(t_refs) and t_rows == rows, 'planted twin (a kVariants row holding a kFx_ address: %s) is caught' % ((t_refs or ['none'])[0]))
    # S111 TOOLLINK review: the same address in a NEW generated table outside kVariants (the class is any odr-use of a data-unit array in the header, not one table's rows)
    twin2 = reg.replace('\r\n', '\n').replace('inline constexpr HashEntry kHashes[] = {', 'inline constexpr const uint32_t* kPick[] = {kFx_yBlur_base_fragment_twin};\ninline constexpr HashEntry kHashes[] = {', 1)
    t2_rows, t2_refs = row_refs(twin2)
    say(twin2 != reg.replace('\r\n', '\n') and bool(t2_refs) and t2_rows == rows, 'planted twin (a kFx_ address in a table outside kVariants: %s) is caught' % ((t2_refs or ['none'])[0]))
    if a.plant:
        say(missing_of('\n'.join(l for l in unit_lines if l != '#include "fx_yBlur.h"')) == ['fx_yBlur.h'], 'planted twin (the unit minus #include "fx_yBlur.h") is caught')
        stale = [re.sub(r'sha256: [0-9a-f]', 'sha256: %s' % ('0' if '0' not in l.split('sha256: ')[-1][:1] else '1'), l, 1) if l.startswith('// fx_yBlur words') else l for l in unit_lines]
        say(sha_bad(stale, d) == ['fx_yBlur'], 'planted twin (one stale sha line) is caught')
        own_inc = next((l for l in own_lines if re.match(r'#include "rdr_\w+\.h"', l)), None)
        say(own_inc is not None and own_bad([l for l in own_lines if l != own_inc], d) != [], 'planted twin (the own unit minus %s) is caught' % own_inc)
        own_sha = next((l for l in own_lines if re.match(r'// rdr_\w+ words sha256: [0-9a-f]{64}$', l)), None)   # S110 RDRPREP review: a stale own sha line (the header changed, the unit did not)
        stale_own = [l[:-1] + ('0' if l[-1] != '0' else '1') if l == own_sha else l for l in own_lines]
        say(own_sha is not None and own_bad(stale_own, d) == [re.match(r'// (rdr_\w+) ', own_sha).group(1)], 'planted twin (one stale own sha line) is caught')
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
