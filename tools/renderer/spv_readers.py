"""S111 TOOLLINK-D: which build scripts may list the effects' data unit (src\\renderer\\shaders\\spv\\fx_spv_data.cpp), and which only READ shader metadata.

A unit reads metadata (kVariants names, sizes, constants, kHashes) with no link reference to the words. Only a unit that calls rdr::fx::Words (the pipeline builders, the catalog
registrars) needs fx_spv_data.cpp; the kOwnShaders table is the small fx_own_shaders.cpp. A tool build script that lists fx_spv_data.cpp by name while the sources it names
(with every file they #include, transitively) hold no `rdr::fx::Words(` call links and compiles the unit for nothing.

  needs_files(root)                 the src / tools files whose code (comments out) calls rdr::fx::Words or names kOwnShaders
  script_findings(root, text, rel)  None when the script is fine or not a unit build; else the reason it lists the unit for nothing
A script that enumerates its sources (a `*.cpp` filter, glob, os.walk) is not judged: its source set is not in its text."""
import os
import re

NEED = re.compile(r'\bfx::Words\b|\bkOwnShaders\b')
SKIP_DIRS = ('src/renderer/shaders/spv/',)
INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
TOKEN = re.compile(r"""['"]([^'"\n]*?\.cpp)['"]""")
JOIN = re.compile(r"""os\.path\.join\(([^()]*)\)""")
ENUM = re.compile(r'\*\.cpp|\bglob\s*\(|os\.walk|\brglob\b')   # a script that lists its sources by a pattern (build_rig's `-Filter '*.cpp'`); `-Recurse -Include *.h` (a staleness scan) is not one
UNIT = re.compile(r"""fx_spv_data\.cpp['"]""")


# Scripts that also compile an OLD tree out of git history (`git archive <commit>^ src`): that tree's registry still holds kFx_ addresses, so its own fx_spv_data.cpp is linked for
# it (the script skips the unit for the current tree).
OLD_TREE = ('tools/scene/build_material_samplers_check.ps1', 'tools/scene/unify5_stock_check.ps1')


def strip_comments(s):
    """Line comments only: a `/*` inside a string literal (front_end.cpp has some) would make a block-comment strip eat real code. A name left in a block comment keeps the
    unit listed, the safe side."""
    return re.sub(r'//[^\n]*', '', s)


def read(root, rel):
    with open(os.path.join(root, *rel.split('/')), encoding='utf-8', errors='replace') as f:
        return f.read()


def needs_files(root):
    out = set()
    for top in ('src', 'tools'):
        for r, ds, fs in os.walk(os.path.join(root, top)):
            ds[:] = [d for d in ds if d not in ('out', 'node_modules', '.git')]
            for f in fs:
                if not f.endswith(('.cpp', '.h', '.inc', '.hpp')):
                    continue
                rel = os.path.relpath(os.path.join(r, f), root).replace('\\', '/')
                if rel.startswith(SKIP_DIRS) or rel.startswith('tools/renderer/spv_'):
                    continue
                if NEED.search(strip_comments(read(root, rel))):
                    out.add(rel)
    return out


def includes(root, rel, cache):
    if rel not in cache:
        found = []
        if os.path.isfile(os.path.join(root, *rel.split('/'))):
            for m in INC.finditer(strip_comments(read(root, rel))):
                for base in (os.path.dirname(rel), 'src', 'tools', 'src/renderer/shaders/spv', ''):
                    c = os.path.normpath(os.path.join(base, m.group(1))).replace('\\', '/')
                    if os.path.isfile(os.path.join(root, *c.split('/'))):
                        found.append(c)
                        break
        cache[rel] = found
    return cache[rel]


def named_sources(root, text, rel):
    """The .cpp files the script names (a bare name = beside the script), that exist, the unit itself left out."""
    names = [m.group(1) for m in TOKEN.finditer(text)]
    for m in JOIN.finditer(text):   # os.path.join(root, 'tools', 'scene', 'x.cpp')
        parts = re.findall(r"""['"]([^'"]+)['"]""", m.group(1))
        if parts and parts[-1].endswith('.cpp'):
            names.append('/'.join(parts))
    out = []
    for n in names:
        n = n.replace('\\', '/')
        if n.endswith('fx_spv_data.cpp'):
            continue
        n = os.path.dirname(rel) + '/' + n if '/' not in n else n
        if os.path.isfile(os.path.join(root, *n.split('/'))) and n not in out:
            out.append(n)
    return out


def script_findings(root, text, rel, needs, cache=None):
    """None = fine / not judged; a string = the script lists fx_spv_data.cpp while no source it names reaches a Words call."""
    if not UNIT.search(text) or ENUM.search(text) or rel in OLD_TREE:
        return None
    srcs = named_sources(root, text, rel)
    if not srcs:
        return None
    cache = {} if cache is None else cache
    seen, st = set(), list(srcs)
    while st:
        f = st.pop()
        if f in seen:
            continue
        seen.add(f)
        st += includes(root, f, cache)
    if seen & needs:
        return None
    return 'lists fx_spv_data.cpp but none of its %d named sources (%d files with their includes) calls rdr::fx::Words or reads kOwnShaders: it reads metadata only' % (len(srcs), len(seen))


def scan(root, scripts=None):
    """{script rel: reason} over every tools\\**\\*.ps1 / *.py (or the given {rel: text})."""
    needs = needs_files(root)
    cache = {}
    if scripts is None:
        scripts = {}
        for r, ds, fs in os.walk(os.path.join(root, 'tools')):
            ds[:] = [d for d in ds if d not in ('out', 'node_modules', '.git', 'q')]
            for f in fs:
                if f.endswith(('.ps1', '.py')):
                    rel = os.path.relpath(os.path.join(r, f), root).replace('\\', '/')
                    scripts[rel] = read(root, rel)
    bad = {}
    for rel, text in sorted(scripts.items()):
        why = script_findings(root, text, rel, needs, cache)
        if why:
            bad[rel] = why
    return bad, needs
