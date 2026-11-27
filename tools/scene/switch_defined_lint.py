#!/usr/bin/env python3
"""switch_defined_lint (S103 SMF, BUG2 finding 3): a `wwe2k14_rdr_*` switch of the table src\\renderer_adapter\\rdr_switches.h (WWE2K14_RDR_SWITCHES) that code of the stream-free player build reads
must be DEFINED in BOTH builds.

The stream build (ON, WWE2K14_STREAM() = 1) compiles reader.h / live.cpp; the stream-free player build (OFF) does not: a switch defined only there is invisible in the player build
(rdr_switches_cvar.h finds no flag, keeps the table default, and the SDK ignores the command-line flag silently: BUG2 found rdr_fx_log / rdr_slot_look / rdr_stage_vis like that).
"Read by the player build" = `rdr_sw::k<Id>` appears in a file outside src\\renderer_adapter, src\\renderer and src\\scene_live (those are stream levers: the OFF build has no such lever by design).
A definition counts for OFF when it sits in a `#if !WWE2K14_STREAM()` region (or the `#else` of `#if WWE2K14_STREAM()`), or in a file outside src\\renderer_adapter\\ ; for ON when it is not in a
`#if !WWE2K14_STREAM()` region.   Text scan, no compile, well under a second.

  py -I tools\\scene\\switch_defined_lint.py --root .     scan the tree (exit 1 on an error)
  py -I tools\\scene\\switch_defined_lint.py --selftest   the planted twins: the bad table + defines MUST be flagged, the good one must pass
"""
import argparse
import os
import re
import sys

TABLE = os.path.join('src', 'renderer_adapter', 'rdr_switches.h')
ENTRY = re.compile(r'^\s*X\(\s*(\w+)\s*,\s*"(wwe2k14_\w+)"')
DEFINE = re.compile(r'REXCVAR_DEFINE_\w+\s*\(\s*(\w+)\s*,')
IF = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)')
USE = re.compile(r'rdr_sw::k(\w+)')
# Bundle-3 levers (src\bundle3.h) player code reads but the stream-free build must NOT define: bundle3::Apply sets every defined lever on, and these never ran stream-free (SMF review A1;
# tools\bundle3\lint.py fails a bundle lever defined stream-free that is not in WWE2K14_BUNDLE3_OFF_LEVERS). The table default is what the player build sees.
BUNDLE_EXEMPT = {'wwe2k14_rdr_scene_strings_once', 'wwe2k14_rdr_swap_once'}
STREAM_ONLY_DIRS =('src/renderer_adapter/', 'src/renderer/', 'src/scene_live/')


def table_names(text):
    names = []
    for line in text.splitlines():
        m = ENTRY.match(line)
        if m:
            names.append((m.group(1), m.group(2)))
    return names


def defs_of(path, text, out):
    """out[name] = set of 'ON' / 'OFF' this definition serves."""
    stack = []   # each: 'ON' / 'OFF' / None (an unrelated #if), flipped by #else
    in_adapter = path.replace('\\', '/').startswith('src/renderer_adapter/')
    for line in text.splitlines():
        m = IF.match(line)
        if m:
            kw, rest = m.group(1), m.group(2)
            if kw in ('if', 'ifdef', 'ifndef'):
                r = rest.strip()
                if re.fullmatch(r'!\s*WWE2K14_STREAM\s*\(\s*\)', r): stack.append('OFF')
                elif re.fullmatch(r'WWE2K14_STREAM\s*\(\s*\)', r): stack.append('ON')
                else: stack.append(None)
            elif kw == 'else' and stack:
                stack[-1] = {'ON': 'OFF', 'OFF': 'ON', None: None}[stack[-1]]
            elif kw == 'endif' and stack:
                stack.pop()
            continue
        d = DEFINE.search(line)
        if not d:
            continue
        region = next((s for s in reversed(stack) if s), None)
        serves = out.setdefault(d.group(1), set())
        if region == 'ON': serves.add('ON')
        elif region == 'OFF': serves.add('OFF')
        else:
            serves.add('ON')
            if not in_adapter: serves.add('OFF')


def stream_only_sources(root):
    """The files the stream-free build leaves out (tools\\scene\\keep_stream_sources.txt, the CMake list): a switch read only there is a stream lever."""
    try:
        return {l.strip() for l in open(os.path.join(root, 'tools', 'scene', 'keep_stream_sources.txt'), encoding='utf-8') if l.startswith('src/')}
    except OSError:
        return set()


def scan(files, stream_only=frozenset()):
    """files: list of (relative path, text). Returns (errors, table_count)."""
    table_text = next((t for p, t in files if p.replace('\\', '/').endswith('renderer_adapter/rdr_switches.h')), None)
    if table_text is None:
        return ['rdr_switches.h not found'], 0
    names = table_names(table_text)
    used = set()   # ids read by code the stream-free player build compiles
    defs = {}
    for p, t in files:
        pp = p.replace('\\', '/')
        if pp.endswith('renderer_adapter/rdr_switches.h'): continue
        defs_of(pp, t, defs)
        if pp.startswith(STREAM_ONLY_DIRS) or pp.endswith("_test.cpp") or pp in stream_only: continue
        used.update(m.group(1) for m in USE.finditer(t))
    errs = []
    for ident, n in names:
        if ident not in used or n in BUNDLE_EXEMPT: continue   # a stream-only lever / a bundle lever kept out of the stream-free build on purpose
        s = defs.get(n, set())
        for b in ('ON', 'OFF'):
            if b not in s:
                errs.append(f'{TABLE}: switch "{n}" (read by the player build as rdr_sw::k{ident}) has no definition in the {b} build; define it under `#if !WWE2K14_STREAM()` in src\\start_settings.cpp (same type and default as reader.h)')
    return errs, len(names)


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)


def load(root):
    out = []
    for dp, dn, fn in os.walk(os.path.join(root, 'src')):
        for f in fn:
            if f.endswith(('.h', '.cpp', '.inl')):
                p = os.path.join(dp, f)
                try:
                    out.append((os.path.relpath(p, root), read_path(p)))
                except OSError:
                    pass
    return out


def selftest():
    table = 'X(A, "wwe2k14_rdr_a", PerFrame, 0) \\\n  X(B, "wwe2k14_rdr_b", PerFrame, 0)\n'
    reader = ('src/renderer_scene/x.cpp', 'rdr_sw::BoolSeen(rdr_sw::kA, 0, f); rdr_sw::BoolSeen(rdr_sw::kB, 0, f);')
    tb = ('src/renderer_adapter/rdr_switches.h', table)
    rh = ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_a, false, "w", "x")\nREXCVAR_DEFINE_BOOL(wwe2k14_rdr_b, false, "w", "x")\n')
    both = ('src/start_settings.cpp', '#if !WWE2K14_STREAM()\nREXCVAR_DEFINE_BOOL(wwe2k14_rdr_a, false, "w", "x")\nREXCVAR_DEFINE_BOOL(wwe2k14_rdr_b, false, "w", "x")\n#endif\n')
    only_a = ('src/start_settings.cpp', '#if !WWE2K14_STREAM()\nREXCVAR_DEFINE_BOOL(wwe2k14_rdr_a, false, "w", "x")\n#endif\n')
    stream_lever = ('src/renderer_adapter/live.cpp', 'rdr_sw::Bool(rdr_sw::kB);')   # B read by the stream build only
    ok = True
    e, _ = scan([reader, tb, rh, both])
    print('good:', 'PASS' if not e else 'FAIL ' + str(e)); ok &= not e
    e, _ = scan([reader, tb, rh, only_a])   # the BUG2 shape: b only in reader.h but read by the player build
    hit = len(e) == 1 and 'wwe2k14_rdr_b' in e[0] and 'OFF build' in e[0]
    print('planted (read by the player build, defined in reader.h only):', 'CAUGHT' if hit else 'MISSED'); ok &= hit
    e, _ = scan([reader, tb, both])   # OFF-only: ON lacks them
    hit = len(e) == 2 and all('ON build' in x for x in e)
    print('planted (defined in the OFF block only):', 'CAUGHT' if hit else 'MISSED'); ok &= hit
    e, _ = scan([stream_lever, tb, rh, only_a])   # a stream lever (not read by player-build code): allowed to be stream-only
    print('stream lever (not read by the player build):', 'PASS' if not e else 'FAIL ' + str(e)); ok &= not e
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest: return selftest()
    errs, n = scan(load(a.root), stream_only_sources(a.root))
    for e in errs: print(e)
    print(f'switch_defined_lint: {n} table switches, {len(errs)} error(s)')
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
