r"""S111 CHECKFAST: which check.py lint rows may be SKIPped on a diff that holds nothing the lint reads.

check.py asks GATES[row name](change) for a row that has a gate: a string (the first changed path that is an input) = RUN the row; None = no input in the diff = SKIP
with the reason "no input of this lint in the diff". A row with no gate runs on every invocation. `check.py --all-rows` ignores every gate (the forced full run).

Why (LANETIME audit, S111): `frame_tick` (median 72 s) and `kind_last` (59 s) were two long poles that ran on every check, 0 FAIL in 60+ runs, and set check.py's
wall (82 s). A gate here is the lint's own input list, written once, never narrower than what the lint reads (the S108 lesson: a trigger narrower than the lint's input
was a false PASS):
  frame_tick  COMPILES tools\renderer\frame_tick_unit.cpp, so every file in its quoted-include closure is an input (src\renderer_game\frame_tick.h and what it
              includes: frame_split.h, renderer_adapter\tail_release.h, renderer_scene\live_scene.h -> record_io.h, ...; walked on each call, so a new include is
              seen; frame_tick.h's game part below `#if !defined(WWE2K14_FRAME_TICK_CORE_ONLY)` is not compiled by the unit and not walked); it READS by text
              src\half2_hooks.h, src\tail_release_hook.h, src\renderer_adapter\reader.h with their split parts (tools\split\read_unit.py), the unit's .py and
              read_unit.py, and (its last arm) EVERY src file for a call of the scene writer's boundary (EndFrame / LatchFrame / EmitLatched): a src file is an input
              when it ADDS such a call (a declaration change of one of the three in live_scene.h adds the name too).
  kind_last   reads every src file for `constexpr kXxLast = kName;` and `enum { }` bodies: a changed src file is an input when it (now, or in the lines it removed) holds
              an `enum` or a `kXxLast` name, or is deleted (a file without either cannot change the verdict); plus the lint itself.
  cvar_link / one_definition / cvar_shard / build_list read the whole src tree, the build lists and the tools build scripts (through clang -E, the SDK headers):
              any changed path outside docs\, .claude\, db\ and *.md is an input (those four rows only skip on a docs / notes / claims / agent-file diff).
  pipeline_seed (S111 PSEEDSTALE) compiles tools\renderer\pseed_catalog.cpp with src\renderer\shaders\spv\fx_spv_data.cpp and fx_own_shaders.cpp and checks db\shaders\shader_list.txt's R lines
              against it: the list, pseed_min.txt, pseed_stale_check.py, pipeline_seed.py, any path under src\renderer\shaders\spv\ and the three units' quoted-include closure.
A change to check.py or this file runs every gated row (the gate itself changed). 0 game runs."""
import os
import posixpath
import re
import sys

SELF = ('tools/handback/check.py', 'tools/handback/check_gates.py')
CODE_EXT = ('.h', '.hpp', '.cpp', '.cc', '.inc')
QUIET = ('docs/', '.claude/', 'db/', 'out/')   # a path under these (or any .md) is read by none of the coarse rows

FRAME_TICK_FILES = ('src/renderer_game/frame_tick.h', 'src/half2_hooks.h', 'src/tail_release_hook.h', 'src/renderer_adapter/reader.h',
                    'tools/renderer/frame_tick_unit.py', 'tools/renderer/frame_tick_unit.cpp', 'tools/split/read_unit.py')
FRAME_TICK_DIRS = ('src/renderer_adapter/reader/', 'src/half2_hooks/', 'src/tail_release_hook/', 'src/renderer_game/frame_tick/')   # the parts of a split parent (tools\split\read_unit.py)
FRAME_TICK_UNIT = 'tools/renderer/frame_tick_unit.cpp'   # built by the lint: its quoted-include closure is compiled
FRAME_TICK_TEXT = ('src/renderer_adapter/reader.h', 'src/half2_hooks.h', 'src/tail_release_hook.h')   # read by text with their split parts
QUOTED = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"([^\n]*)', re.M)
CORE_ONLY_CUT = re.compile(r'^[ \t]*#[ \t]*if[ \t]+![ \t]*defined[ \t]*\(?[ \t]*WWE2K14_FRAME_TICK_CORE_ONLY\b', re.M)   # the unit defines it: below is not compiled
BOUNDARY_CALL = re.compile(r'\b(?:EndFrame|LatchFrame|EmitLatched)\s*\(')
ENUM_OR_LAST = re.compile(r'\benum\b|\bk\w*Last\b')


def _self(ch):
    return next((p for p in SELF if p in ch.status), None)


def _closure(ch, starts, split_only):
    """Every path reached from `starts` through quoted #include lines (resolved against the including file's folder, as clang does with no -I), named or not
    present (a deleted header is still an input); split_only = only `// split: NAME` part lines (tools\\split\\read_unit.py's MARK)."""
    seen, todo = set(), list(starts)
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        txt = ch.read(p)
        if not txt:
            continue
        cut = CORE_ONLY_CUT.search(txt)
        if cut and not split_only:
            txt = txt[:cut.start()]
        for inc, rest in QUOTED.findall(txt):
            if split_only and 'split:' not in rest:
                continue
            todo.append(posixpath.normpath(posixpath.join(posixpath.dirname(p), inc.replace('\\', '/'))))
    return seen


def frame_tick_inputs(ch):
    """The files frame_tick reads by name (its last arm, the boundary-call scan of every src file, is separate)."""
    return set(FRAME_TICK_FILES) | _closure(ch, [FRAME_TICK_UNIT], False) | _closure(ch, FRAME_TICK_TEXT, True)


def frame_tick(ch):
    hit = _self(ch)
    if hit:
        return hit
    named = frame_tick_inputs(ch)
    for p in sorted(ch.status):
        if p in named or p.startswith(FRAME_TICK_DIRS):
            return p
    for p in sorted(ch.status):
        if p.startswith('src/') and p.endswith(CODE_EXT) and any(BOUNDARY_CALL.search(t) for _n, t in ch.added(p)):
            return p + ' (adds a call of the scene writer boundary)'
    return None


def kind_last(ch):
    hit = _self(ch)
    if hit:
        return hit
    if 'tools/lint/kind_last_lint.py' in ch.status:
        return 'tools/lint/kind_last_lint.py'
    for p in sorted(ch.status):
        if not (p.startswith('src/') and p.endswith(CODE_EXT)):
            continue
        if ch.status[p] == 'D':
            return p + ' (deleted)'
        txt = ch.read(p)
        if txt is None or ENUM_OR_LAST.search(txt) or any(ENUM_OR_LAST.search(t) for _n, t in ch.removed(p)):
            return p
    return None


def _coarse(ch):
    hit = _self(ch)
    if hit:
        return hit
    for p in sorted(ch.status):
        if not (p.startswith(QUIET) or p.lower().endswith('.md')):
            return p
    return None


def _cdf(name, any_src):
    """S111 CDCHECKALL: a code-diff extractor row (check_cdfields.py): its own tool files, the files it reads (check_cdfields.inputs: the tools' own constants, so a new
    role / file is seen), and (any_src: levels reads constexpr names / rule functions / comments anywhere in src) every src code file."""
    def gate(ch):
        hit = _self(ch)
        if hit:
            return hit
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import check_cdfields
            files, prefixes = check_cdfields.inputs(name)
        except Exception as e:   # the list cannot be read: run the row (it says why itself), never a silent skip
            return 'input list unreadable: %s' % e
        for p in sorted(ch.status):
            if p in files or (prefixes and p.startswith(prefixes)) or (any_src and p.startswith('src/') and p.endswith(CODE_EXT)):
                return p
        return None
    return gate


PSEED_FILES = ('db/shaders/shader_list.txt', 'db/shaders/pseed_min.txt', 'tools/renderer/pseed_stale_check.py', 'tools/renderer/pipeline_seed.py', 'tools/renderer/pseed_catalog.cpp')
PSEED_UNITS = ['tools/renderer/pseed_catalog.cpp', 'src/renderer/shaders/spv/fx_spv_data.cpp', 'src/renderer/shaders/spv/fx_own_shaders.cpp']   # what pseed_stale_check.build_catalog compiles (S111 TOOLLINK-D: kOwnShaders is its own unit)
PSEED_DIRS = ('src/renderer/shaders/spv/',)   # the compiled words and the registry


def pipeline_seed(ch):
    """S111 PSEEDSTALE: the stale-seed row's inputs (a shader regenerated under spv\\ changes a hash the list may name)."""
    hit = _self(ch)
    if hit:
        return hit
    named = set(PSEED_FILES) | _closure(ch, PSEED_UNITS, False)
    for p in sorted(ch.status):
        if p in named or p.startswith(PSEED_DIRS):
            return p
    return None


GATES = {'frame_tick': frame_tick, 'kind_last': kind_last, 'pipeline_seed': pipeline_seed,
         'cd_levels': _cdf('levels', True), 'cd_targetfmt': _cdf('targetfmt', False), 'cd_draworder': _cdf('draworder', False),
         'cvar_link': _coarse, 'one_definition': _coarse, 'cvar_shard': _coarse, 'build_list': _coarse}


def decide(ch, name):
    """(run?, why): the first input in the diff, or the SKIP reason. A row with no gate always runs."""
    g = GATES.get(name)
    if g is None:
        return True, 'no gate'
    hit = g(ch)
    if hit:
        return True, hit
    return False, 'no input of this lint in the diff (gate: tools\\handback\\check_gates.py)'
