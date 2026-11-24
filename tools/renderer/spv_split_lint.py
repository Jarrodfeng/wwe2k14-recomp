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
  7. with --base REF (optional one-off identity proof, not in the default run): every array of REF has the same words now, and none is missing;
  8. --plant: structural twins that need no base: the unit minus one `#include`, one stale sha line, a script without the unit, a /force:unresolved script without it (with --base also one
     changed word). Each MUST be reported.
Exit 0 = clean."""
import argparse
import glob
import hashlib
import io
import os
import re
import subprocess
import sys
import tokenize

REL = 'src/renderer/shaders/spv/'
DEF = re.compile(r'(?:inline constexpr|extern const) uint32_t (kFx_\w+)\[(\d*)\] = \{(.*?)\};', re.S)
DECL = re.compile(r'^extern const uint32_t (kFx_\w+)\[(\d+)\];', re.M)
TABLE = re.compile(r'\bkVariants\b|\bkHashes\b|\bkFx_|\bkOwnShaders\b')   # the data unit's symbols (S110 RDRPREP: + kOwnShaders): a unit that names one in CODE odr-uses the unit
FAILS = 0


def say(ok, msg):
    global FAILS
    FAILS += 0 if ok else 1
    print(('ok   ' if ok else 'FAIL ') + msg)


def words(body):
    return [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body)]


def parse(text):
    return {m.group(1): (m.group(2), words(m.group(3))) for m in DEF.finditer(text.replace('\r\n', '\n'))}


def cpp_code(text):
    """S110 RDRPREP review: the C / C++ text with its comments blanked (line breaks and string literals kept): a table named in a comment is prose, not an odr-use
    (pipeline_catalog.h's comment naming kVariants made every includer a "user" and its comment had to be reworded to dodge the scan)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
            continue
        if c == '"' and i and text[i - 1] == 'R':   # a raw string R"d( ... )d"
            m = re.match(r'"([^()\\\s]{0,16})\(', text[i:])
            if m:
                j = text.find(')' + m.group(1) + '"', i)
                j = n if j < 0 else j + len(m.group(1)) + 2
                out.append(text[i:j])
                i = j
                continue
        if c in '"\'':   # a string or character literal, to its closing quote (never past the line: a digit separator 1'000 stops there)
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def users_of(files):
    """files: {path: C / C++ text}. The .cpp basenames that odr-use the tables (TABLE named in code, not in a comment: a unit that only includes a header that declares types,
    or names a table in prose, links without the words) or include, directly, a file that does (deeper is over-approximation: thin_draw.h is everywhere). A name shared by
    two .cpp files (main.cpp) cannot be told apart in a script and is left out."""
    inc, hit = {}, set()
    for p, t in files.items():
        inc[p] = {os.path.basename(x) for x in re.findall(r'#\s*include\s+"([^"]+)"', t)}
        if TABLE.search(t) and TABLE.search(cpp_code(t)):
            hit.add(os.path.basename(p))
    allc = [os.path.basename(p) for p in inc if p.endswith(('.cpp', '.cc'))]
    return sorted({os.path.basename(p) for p, i in inc.items() if (i & hit or os.path.basename(p) in hit) and p.endswith(('.cpp', '.cc')) and allc.count(os.path.basename(p)) == 1})


def registry_users(root):
    """The .cpp files (src and tools) that include fx_registry.h directly or through any header, by include basename (an over-approximation only asks for the unit)."""
    files = {}
    for top in ('src', 'tools'):
        for r, ds, fs in os.walk(os.path.join(root, top)):
            ds[:] = [x for x in ds if x not in ('out', 'node_modules', '.git')]
            if r.replace('\\', '/').endswith('src/renderer/shaders/spv'):
                continue
            for f in fs:
                if f.endswith(('.cpp', '.cc', '.h', '.hpp', '.inc')):
                    try:
                        files[os.path.join(r, f)] = open(os.path.join(r, f), encoding='utf-8', errors='replace').read()
                    except OSError:
                        continue
    return users_of(files)


COMPILE_WORD = re.compile(r'clang|cl\.exe|rsc_obj|lld|-std=|-[cIoD]\b|-D\w')   # a string with a space that is a command line, not a sentence


def py_code(text):
    """S110 RDRPREP review: a Python script's text with its comments, docstrings and prose strings blanked (line breaks kept), for the "names a user" scan. A source named
    in a docstring, a comment or a message (a string with a space and no compiler word) is prose, not a compile; a path or an argv piece (no space) stays. Text that does
    not tokenize is returned whole (the old line scan)."""
    try:
        toks = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, SyntaxError):
        return text
    starts = [0] + [m.end() for m in re.finditer('\n', text)]
    fmid = getattr(tokenize, 'FSTRING_MIDDLE', None)
    out = list(text)
    prev = tokenize.NEWLINE
    for k, tok in enumerate(toks):
        kind = tok.type
        drop = kind == tokenize.COMMENT
        if kind == tokenize.STRING or (fmid is not None and kind == fmid):
            nxt = next((t.type for t in toks[k + 1:] if t.type not in (tokenize.NL, tokenize.COMMENT)), tokenize.ENDMARKER)
            doc = kind == tokenize.STRING and prev in (tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT) and nxt in (tokenize.NEWLINE, tokenize.ENDMARKER)
            drop = doc or (re.search(r'\s', tok.string) is not None and not COMPILE_WORD.search(tok.string))
        if drop and tok.start[0] <= len(starts) and tok.end[0] <= len(starts):
            for i in range(starts[tok.start[0] - 1] + tok.start[1], min(starts[tok.end[0] - 1] + tok.end[1], len(out))):
                if out[i] != '\n':
                    out[i] = ' '
        if kind not in (tokenize.NL, tokenize.COMMENT):
            prev = kind
    return ''.join(out)


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
        def names_a_user(text):
            for ln in text.splitlines():
                if READ_LINE.search(ln):
                    continue
                if any(re.search(r"""[\\/'"\s(,]%s['"\s)\],]""" % re.escape(u), ln) for u in users):
                    return True
            return False
        if not names_a_user(t) or (n.endswith('.py') and not names_a_user(py_code(t))):   # S110 RDRPREP review: a .py names a user only in code (a docstring, a comment or a message is prose)
            continue
        has = 'fx_spv_data' in t or re.search(r"src.renderer['\\)\s]*-Recurse[^\n]*\*\.cpp", t) or 'rsc_obj' in t
        if not has or ('force:unresolved' in t and 'fx_spv_data' not in t):
            bad.append(n)
    return bad


def own_bad(lines, d):
    """S110 RDRPREP: fx_spv_data.cpp's own-shader block: each `#include "rdr_*.h"` has a sha line (of that header's text) right above it and a kOwnShaders row of its array; each row's header is included.
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
    ob = own_bad(unit_lines, d)
    own_n = len([l for l in unit_lines if re.match(r'#include "rdr_\w+\.h"', l)])
    say(own_n > 0 and not ob, 'fx_spv_data.cpp: the kOwnShaders block holds %d own shaders, each with its include, a current sha line and a row (bad: %s)' % (own_n, ob[:3] or 'none'))
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
        own_inc = next((l for l in unit_lines if re.match(r'#include "rdr_\w+\.h"', l)), None)
        say(own_inc is not None and own_bad([l for l in unit_lines if l != own_inc], d) != [], 'planted twin (the unit minus %s) is caught' % own_inc)
        own_sha = next((l for l in unit_lines if re.match(r'// rdr_\w+ words sha256: [0-9a-f]{64}$', l)), None)   # S110 RDRPREP review: a stale own sha line (the header changed, the unit did not)
        stale_own = [l[:-1] + ('0' if l[-1] != '0' else '1') if l == own_sha else l for l in unit_lines]
        say(own_sha is not None and own_bad(stale_own, d) == [re.match(r'// (rdr_\w+) ', own_sha).group(1)], 'planted twin (one stale own sha line) is caught')
        victim = next((n for n in sorted(scripts) if 'fx_spv_data' in scripts[n] and n in script_misses({n: scripts[n].replace('fx_spv_data', 'fx_spv_dat_')}, users) or False), None)
        say(victim is not None, 'planted twin (a script with the unit removed: %s) is caught' % victim)
        say(script_misses({'tools/planted_force.ps1': 'clang++ x.cpp %s -Wl,/force:unresolved' % users[0]}, users) == ['tools/planted_force.ps1'], 'planted twin (a /force:unresolved script without the unit) is caught')
        # S110 RDRPREP review: the scan reads code, not prose (a table named in a C++ comment, a source named in a .py docstring / comment / message)
        tw = {'src/planted/a.h': '// the kVariants table, named here in prose only\nstruct A {};\n', 'src/planted/planted_unit.cpp': '#include "a.h"\nint main() { return 0; }\n'}
        say(users_of(tw) == [], 'planted twin (a header naming kVariants only in a comment, included by a .cpp) is NOT a registry user')
        tw['src/planted/a.h'] = 'inline const void* p = rdr::fx::kOwnShaders;   // the table in code\n'
        say(users_of(tw) == ['planted_unit.cpp'], 'planted twin (a header using kOwnShaders in code) makes its includer a registry user')
        u = users[0]
        prose = '"""Builds and runs tools/planted/%s (prose)."""\nimport subprocess\nCLANG = "clang++"\n# tools/planted/%s is named in this comment\nprint("the unit %s was not built")\n' % (u, u, u)
        say(script_misses({'tools/planted_prose.py': prose}, users) == [], 'planted twin (a .py naming %s only in its docstring, a comment and a message) is NOT a miss' % u)
        built = prose + 'subprocess.run([CLANG, "-o", "x.exe", "src/%s"])\n' % u
        say(script_misses({'tools/planted_build.py': built}, users) == ['tools/planted_build.py'], 'planted twin (the same .py compiling src/%s without the unit) IS a miss' % u)
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
