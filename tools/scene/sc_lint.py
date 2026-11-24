# Lane SCF (Session 103; the Opus review of lane SC, F2 / F5 / F7 and the tap-entry rule). Four source lints for the 2D layer's scissor / target state, each with a planted twin (--selftest).
#   py -I tools\scene\sc_lint.py [--root DIR] [--selftest]
#   1 reset_taps      every recorder whose executor reaches sub_82CABC68 (the device scissor rewrite from the default table) has a REX_HOOK_RAW in src\draw_origin.h whose tap entry (src\scene_live\quads2d_tap.h)
#                     invalidates the thread's scissor (calls q::OnTargetReset). The table is the documented set (op 45 sub_82BB3978, op 47 sub_82BB3FC0, op 48 sub_82BB3B08; executors sub_82BB4348 / sub_82BB3910 /
#                     sub_82BB43C8, from `q callers 82CABC68` -> sub_82CABCF8 / sub_82CAC088); a recorder added to the table without a resetting tap fails.
#   2 continue_parity every `continue;` in q2d::Append (src\renderer_scene\quads2d_draw.cpp) that is not driven by Decide has a twin in q2d::Check (a skip Append makes that Check does not count makes the live frame check lie).
#                     Both bodies must carry the same number of `continue;`.
#   3 layout_text     tools\scene\*_unit.cpp: a `layout N (M bytes` / `layout N, M bytes` phrase whose M is the current Quad2D size must name the current kQuad2DLayout; a phrase with another M must say `was`;
#                     every `kQuad2DLayout == K` must be the current layout; every `sizeof(Quad2D) == M` the current size (the static_assert in scene_record.h).
#   4 tap_active      every q2d_tap entry (an inline function of quads2d_tap.h taking a PPCContext) called from a REX_HOOK_RAW starts with `if (!Active()) return;` or a `t_scope.open` test; the one entry that must run
#                     first for another reason (OnOp19: the fallback census) carries `// q2d-lint: census-first`.
import argparse
import re
import sys
from pathlib import Path

RESET_TABLE = [   # recorder, hook entry in quads2d_tap.h, what it is (executor in the comment)
    ('sub_82BB3978', 'OnSetRenderTarget', 'op 45 SetRenderTarget (executor sub_82BB4348 -> sub_82CAC9A8 -> sub_82CABCF8 -> sub_82CABC68)'),
    ('sub_82BB3FC0', 'OnResetRtDs', 'op 47 reset_rt_ds_default (executor sub_82BB3910 -> sub_82CAC9A8 / sub_82CAC088)'),
    ('sub_82BB3B08', 'OnSetDepthStencil', 'op 48 SetDepthStencil (executor sub_82BB43C8 -> sub_82CAC088 -> sub_82CABC68)'),
]


def rd(root, rel):
    return (Path(root) / rel).read_text(encoding='utf-8', errors='replace')


def func_body(text, header_re):
    m = re.search(header_re, text)
    if not m:
        return None
    i = text.index('{', m.end() - 1) if text[m.end() - 1] != '{' else m.end() - 1
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def hook_body(draw_origin, fn):
    return func_body(draw_origin, r'REX_HOOK_RAW\(' + fn + r'\)\s*\{')


def lint_reset_taps(draw_origin, tap):
    errs = []
    for fn, entry, what in RESET_TABLE:
        hb = hook_body(draw_origin, fn)
        if hb is None:
            errs.append(f'reset_taps: no REX_HOOK_RAW({fn}) in draw_origin.h for {what}')
            continue
        if f'q2d_tap::{entry}(' not in hb:
            errs.append(f'reset_taps: the hook on {fn} does not call q2d_tap::{entry}')
            continue
        tb = func_body(tap, r'inline void ' + entry + r'\s*\([^)]*\)\s*\{')
        if tb is None:
            errs.append(f'reset_taps: quads2d_tap.h has no {entry}')
        elif 'OnTargetReset(' not in tb and 'OnTargetResetTo(' not in tb:   # S103 PFR: the tap reads the default scissor table (OnTargetResetTo); the no-memory form stays accepted
            errs.append(f'reset_taps: {entry} (the tap of {fn}, {what}) does not call OnTargetReset: its executor rewrites the device scissor')
    return errs


def lint_continue_parity(draw_cpp):
    a = func_body(draw_cpp, r'Report Append\([^)]*\)\s*\{')
    c = func_body(draw_cpp, r'Report Check\([^)]*\)\s*\{')
    if a is None or c is None:
        return ['continue_parity: Append or Check not found in quads2d_draw.cpp']
    na, nc = len(re.findall(r'\bcontinue;', a)), len(re.findall(r'\bcontinue;', c))
    if na != nc:
        return [f'continue_parity: Append has {na} `continue;` but Check has {nc}: every skip Append makes must be counted by Check (the live frame check decides on Check)']
    return []


def lint_layout_text(scene_record, units):
    errs = []
    m = re.search(r'kQuad2DLayout\s*=\s*(\d+)', scene_record)
    s = re.search(r'static_assert\(sizeof\(Quad2D\)\s*==\s*(\d+)', scene_record)
    if not m or not s:
        return ['layout_text: kQuad2DLayout or the sizeof static_assert not found in scene_record.h / rows_quad2d.h']
    layout, size = int(m.group(1)), int(s.group(1))
    for name, text in units.items():
        for ln, line in enumerate(text.splitlines(), 1):
            for lm in re.finditer(r'layout (\d+)[ ,(]+(\d+) bytes', line):
                n, b = int(lm.group(1)), int(lm.group(2))
                if b == size and n != layout:
                    errs.append(f'layout_text: {name}:{ln}: "layout {n} ({b} bytes" but the current layout is {layout}')
                elif b != size and 'was' not in line:
                    errs.append(f'layout_text: {name}:{ln}: "layout {n} ({b} bytes" is not the current size {size} and the line does not say "was"')
            for km in re.finditer(r'kQuad2DLayout\s*==\s*(\d+)', line):
                if int(km.group(1)) != layout:
                    errs.append(f'layout_text: {name}:{ln}: kQuad2DLayout == {km.group(1)} but the current layout is {layout}')
            for sm in re.finditer(r'sizeof\(Quad2D\)\s*==\s*(\d+)', line):
                if int(sm.group(1)) != size:
                    errs.append(f'layout_text: {name}:{ln}: sizeof(Quad2D) == {sm.group(1)} but the current size is {size}')
    return errs


def lint_tap_active(hooks, tap):
    errs = []
    called = set(re.findall(r'q2d_tap::(\w+)\(', hooks))
    for entry in sorted(called):
        if entry in ('LeafScope',):   # a class: its constructor tests Active() first (checked below)
            continue
        tb = func_body(tap, r'inline \w+(?:\s*\*)?\s+' + entry + r'\s*\([^)]*PPCContext[^)]*\)\s*\{')
        if tb is None:
            continue   # not a context-taking entry (e.g. RowsThisFrame): not a hook entry
        first = next((l.strip() for l in tb[1:].splitlines() if l.strip() and not l.strip().startswith('//')), '')   # the first CODE line (a comment after the brace is skipped)
        ok = first.startswith('if (!Active()) return') or 'if (!t_scope.open) return' in first
        census = 'q2d-lint: census-first' in tb.splitlines()[0] or any('q2d-lint: census-first' in l for l in tap.splitlines() if entry in l)
        if not ok and not census:
            errs.append(f'tap_active: q2d_tap::{entry} (called from a REX_HOOK_RAW) does not start with `if (!Active()) return;` or a t_scope.open test (first line: {first.strip()[:80]!r})')
    ls = func_body(tap, r'LeafScope\(const PPCContext& ctx, uint8_t kind\)\s*\{')
    if ls is None or 'if (!Active()' not in ls.splitlines()[0] + ls[:120]:
        errs.append('tap_active: the LeafScope constructor does not test Active() first')
    return errs


def load(root):
    do = rd(root, 'src/draw_origin.h')
    tap = rd(root, 'src/scene_live/quads2d_tap.h')
    hooks = do + rd(root, 'src/scene_crowd.h') + rd(root, 'src/d3d_textures.h')   # every file that calls a tap entry from a REX_HOOK_RAW
    draw = rd(root, 'src/renderer_scene/quads2d_draw.cpp')
    sr = rd(root, 'src/renderer_scene/scene_record.h') + '\n' + rd(root, 'src/renderer_scene/rows_quad2d.h')   # S106 G4-1AF: the Quad2D layout constants moved to rows_quad2d.h
    units = {p.name: p.read_text(encoding='utf-8', errors='replace') for p in sorted((Path(root) / 'tools' / 'scene').glob('*_unit.cpp'))}
    return do, tap, draw, sr, units, hooks


def run_all(root):
    do, tap, draw, sr, units, hooks = load(root)
    return lint_reset_taps(do, tap) + lint_continue_parity(draw) + lint_layout_text(sr, units) + lint_tap_active(hooks, tap)


def selftest(root):
    do, tap, draw, sr, units, hooks = load(root)
    fails = 0

    def expect(name, errs, want_fail):
        nonlocal fails
        ok = bool(errs) == want_fail
        print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ' -> ' + (str(errs) if errs else 'no error raised')), flush=True)
        fails += 0 if ok else 1

    expect('clean tree: reset_taps', lint_reset_taps(do, tap), False)
    expect('clean tree: continue_parity', lint_continue_parity(draw), False)
    expect('clean tree: layout_text', lint_layout_text(sr, units), False)
    expect('clean tree: tap_active', lint_tap_active(hooks, tap), False)
    # twin 1: the op-48 recorder's tap loses its reset
    tap1 = re.sub(r'(inline void OnSetDepthStencil[^{]*\{)(.*?)\n\}', lambda m: m.group(1) + m.group(2).replace('q::OnTargetResetTo(t_scissor, GuestMem{}, &g_counters);', '').replace('q::OnTargetReset(t_scissor, &g_counters);', '') + '\n}', tap, count=1, flags=re.S)
    expect('twin: the op-48 tap has no OnTargetReset', lint_reset_taps(do, tap1), True)
    # twin 2: a recorder with no hook at all
    do2 = do.replace('REX_HOOK_RAW(sub_82BB3FC0)', 'REX_HOOK_RAW(sub_82BB3FC1)')
    expect('twin: no hook on the op-47 recorder', lint_reset_taps(do2, tap), True)
    # twin 3: Append gains a continue Check lacks
    # S103 (merge with P7 H2): anchor on the FIRST `continue;` inside Append's body (the refused branch's text changed when H2 added first_refused), never on exact neighbour text
    _ai = draw.find('Report Append(')
    _ci = draw.find('continue;', _ai) if _ai >= 0 else -1
    draw3 = (draw[:_ci] + 'continue;\n    }\n    if (rows[0].seq == 0xFFFFFFFFu) {\n      ' + draw[_ci:]) if _ci >= 0 else draw
    expect('twin planted (anchor found)', [] if _ci >= 0 else ['no anchor'], False)
    expect('twin: an uncounted continue in Append', lint_continue_parity(draw3), True)
    # twin 4: a stale layout phrase in a unit
    units4 = dict(units)
    units4['p3c_unit.cpp'] = units['p3c_unit.cpp'] + '\n//   [P3C RECORD]  layout 3 (320 bytes a row)\n'
    expect('twin: a unit says layout 3 (320 bytes', lint_layout_text(sr, units4), True)
    units5 = dict(units)
    units5['i2_unit.cpp'] = units['i2_unit.cpp'] + '\nCheck(sizeof(Quad2D) == 320 && kQuad2DLayout == 3, "x", "y");\n'
    expect('twin: kQuad2DLayout == 3', lint_layout_text(sr, units5), True)
    # twin 6: a hook entry that does not test Active() first
    tap6 = tap.replace('inline void OnResetRtDs(const PPCContext& ctx, const uint8_t* base) {   // op 47, sub_82BB3FC0\n  if (!Active()) return;', 'inline void OnResetRtDs(const PPCContext& ctx, const uint8_t* base) {   // op 47, sub_82BB3FC0\n  q::OnTargetReset(t_scissor, &g_counters);\n  if (!Active()) return;', 1)
    if tap6 == tap:
        tap6 = tap.replace('if (!Active()) return;\n  q::OnResetRtDs', 'q::OnResetRtDs', 1)
    expect('twin: OnResetRtDs does not start with Active()', lint_tap_active(hooks, tap6), True)
    # twin 7: the census-first exemption is required for OnOp19
    tap7 = tap.replace('q2d-lint: census-first', 'q2d-lint: nothing')
    expect('twin: OnOp19 loses its census-first marker', lint_tap_active(hooks, tap7), True)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest(a.root)
    errs = run_all(a.root)
    for e in errs:
        print('FAIL ' + e)
    print('PASS sc_lint: reset_taps, continue_parity, layout_text, tap_active' if not errs else f'FAIL sc_lint: {len(errs)} problem(s)')
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
