#!/usr/bin/env python3
"""S102 lane G1 offline check (0 game runs, no window): LOW's levers (docs\\renderer\\LOW_LEVERS.md).

  py -I tools\\g1\\g1_check.py [--root DIR]

1. unit: tools\\g1\\low_levers_test.cpp built against the real headers passes.
2. planted twins of the headers (a lever that ignores its switch, a clamp that raises, a bar off by one ...): each must make the test FAIL.
3. wiring: every switch is READ in reader.h and its value reaches the code that uses it (the release-day fault class: a switch that exists and nothing reads);
   each link is also removed in a copy and the wiring check must then FAIL.
Exit 0 only when everything holds."""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def read(root, rel):
    with open(os.path.join(root, rel), newline='') as f:
        return f.read()


def build_test(root, work, tag, headers):
    """headers: {name: text} written next to a copy of the test; returns the exe path or None."""
    d = os.path.join(work, tag)
    os.makedirs(d, exist_ok=True)
    for name, text in headers.items():
        with open(os.path.join(d, name), 'w', newline='') as f:
            f.write(text)
    exe = os.path.join(d, 'test.exe')
    src = os.path.join(root, 'tools', 'g1', 'low_levers_test.cpp')
    p = subprocess.run([CLANG, '-std=c++20', '-O1', '-Wall', '-I' + d, src, '-o', exe, '-fuse-ld=lld'], capture_output=True, text=True, timeout=300)
    if p.returncode:
        print(p.stdout + p.stderr)
        return None
    return exe


def run_test(exe):
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout


# ---- wiring links: (what, file, regex that must match). The chain: switch read -> options field -> renderer / binder use. ----
LINKS = [
    ('lever 1 switch is read', 'src/renderer_adapter/reader.h', r'REXCVAR_GET\(wwe2k14_rdr_low_shadow_small\)'),
    ('lever 1 goes through ShadowMapSize', 'src/renderer_adapter/reader.h', r'options\.shadow_map_size\s*=\s*(?:rdr_adapter::)?low_levers::ShadowMapSize\('),
    ('lever 1 size reaches the shadow code', 'src/renderer_adapter/live.cpp', r'g_shadow_map_size\s*=\s*options\.shadow_map_size'),
    ('lever 2 switch is read', 'src/start_settings.cpp', r'REXCVAR_GET\(wwe2k14_rdr_low_present_noclear\)'),   # S103 SPB: ApplyStartSettings reads it for both builds
    ('lever 2 goes through PresentNoClear', 'src/start_settings.cpp', r'a\.present_noclear\s*=\s*(?:rdr_adapter::)?low_levers::PresentNoClear\('),
    ('lever 2 reaches the start options from Applied', 'src/renderer_adapter/reader.h', r'options\.present_noclear\s*=\s*applied\.present_noclear'),   # S103 SPB review L6: the link the move to start_settings.cpp cut
    ('lever 2 reaches the renderer options', 'src/renderer_adapter/live.cpp', r'ro\.present_noclear\s*=\s*options\.present_noclear'),
    ('lever 2 is read where the present is recorded', 'src/renderer/renderer.cpp', r'const bool noclear\s*=\s*options\.present_noclear\s*&&'),
    ('lever 2 branches on the local, not a handle', 'src/renderer/renderer.cpp', r'if \(noclear\) \{'),
    ('lever 2 clears the viewport too without a pipeline', 'src/renderer/renderer.cpp', r'pipeline \? PresentBars\('),
    ('lever 2 clears the bars', 'src/renderer/renderer.cpp', r'PresentBars\('),
    ('lever 2 pass has no clear', 'src/renderer/swapchain.cpp', r'render_pass_noclear_\s*=\s*MakePass\(VK_ATTACHMENT_LOAD_OP_DONT_CARE[,)]'),
    ('lever 2 pass is made only when asked (not in Create)', 'src/renderer/swapchain.cpp', r'if \(!render_pass_noclear_\) render_pass_noclear_'),
    ('lever 3 switch is read', 'src/renderer_adapter/reader.h', r'REXCVAR_GET\(wwe2k14_rdr_low_no_fullsize_copies\)'),
    ('lever 3 goes through CopyAliasFor', 'src/renderer_adapter/reader.h', r'(?:rdr_adapter::)?low_levers::CopyAliasFor\('),
    ('lever 3 mode reaches copy_alias', 'src/renderer_adapter/live.cpp', r'copy_alias::g_mode\s*=\s*options\.copy_alias'),
    ('LOW: the direct settings-file start sets the quality note', 'src/gfx_menu.h', r'SetFlagByName\("wwe2k14_quality_note"'),
    ('LOW is read from the resolved tier (the note)', 'src/start_settings.cpp', r'(?:rdr_adapter::)?low_levers::IsLow\(flag\)'),
]


def wiring(texts):
    bad = []
    for what, rel, rx in LINKS:
        if not re.search(rx, texts[rel]):
            bad.append(what)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    work = os.path.join(root, 'out', 'g1_check')
    ll = read(root, 'src/renderer_adapter/low_levers.h')
    pc = read(root, 'src/renderer/present_clear.h')
    base = {'low_levers.h': ll, 'present_clear.h': pc}

    exe = build_test(root, work, 'real', base)
    if not exe:
        line(False, 'unit test did not build')
        return 1
    rc, out = run_test(exe)
    line(rc == 0, 'unit test on the real headers: ' + out.strip().splitlines()[-1] if out.strip() else 'unit test printed nothing')

    plants = [
        ('lever 1 ignores its switch', 'low_levers.h', 'if (lever && low && shadows_ours && asked > kLowShadowMapSize)', 'if (low && shadows_ours && asked > kLowShadowMapSize)'),
        ('lever 1 ignores LOW', 'low_levers.h', 'if (lever && low && shadows_ours && asked > kLowShadowMapSize)', 'if (lever && shadows_ours && asked > kLowShadowMapSize)'),
        ('lever 1 raises a smaller ask', 'low_levers.h', 'asked > kLowShadowMapSize', 'asked != kLowShadowMapSize'),
        ('lever 1 resizes the stream\'s maps', 'low_levers.h', 'lever && low && shadows_ours', 'lever && low'),
        ('lever 2 ignores its switch', 'low_levers.h', 'return lever && low; }', 'return low; }'),
        ('lever 3 ignores its switch', 'low_levers.h', 'if (!(lever && low)) return', 'if (!low) return'),
        ('lever 3 drops scene colour and other too', 'low_levers.h', '1u | 2u | 8u;', '63u;'),
        ('lever 3 drops fewer classes', 'low_levers.h', 'kLowCopyClasses | (asked_mode', '0u | (asked_mode'),
        ('LOW: the note is ignored', 'low_levers.h', 'if (!note.empty()) return note.rfind("LOW_", 0) == 0;', 'if (!note.empty()) return true;'),
        ('LOW: a note match anywhere', 'low_levers.h', 'note.rfind("LOW_", 0) == 0', 'note.find("LOW_") != std::string::npos'),
        ('LOW: the shadow flag means LOW (the old detection)', 'low_levers.h', 'return flag("gpu_native_crowd_pose_half") == "true";', 'return flag("gpu_native_crowd_pose_half") == "true" || flag("gpu_shadow_half_rate") == "true";'),
        ('LOW: no flag fallback', 'low_levers.h', 'return flag("gpu_native_crowd_pose_half") == "true";', 'return false;'),
        ('bars: bottom bar one row short', 'present_clear.h', 'if (y1 < fb_h)', 'if (y1 < fb_h - 1)'),
        ('bars: top bar missing', 'present_clear.h', 'if (y0 > 0)', 'if (y0 > 1)'),
        ('bars: left bar double-covers the corners', 'present_clear.h', 'out[n++] = {0, y0, uint32_t(x0), uint32_t(y1 - y0)};', 'out[n++] = {0, 0, uint32_t(x0), uint32_t(fb_h)};'),
        ('bars: covered panel still clears', 'present_clear.h', 'int n = 0;', 'int n = 0; if (x0 == 0 && y0 == 0 && x1 == fb_w && y1 == fb_h) { out[n++] = {0, 0, uint32_t(fb_w), uint32_t(fb_h)}; return n; }'),
    ]
    for i, (name, hdr, old, new) in enumerate(plants):
        if old not in base[hdr]:
            line(False, 'plant "%s": anchor text not found in %s (the check is blind)' % (name, hdr))
            continue
        h = dict(base)
        h[hdr] = base[hdr].replace(old, new, 1)
        pe = build_test(root, work, 'plant%d' % i, h)
        if not pe:
            line(False, 'plant "%s": did not build' % name)
            continue
        rc, out = run_test(pe)
        line(rc != 0, 'planted fault "%s" is caught by the unit test' % name)

    texts = {rel: read(root, rel) for rel in {l[1] for l in LINKS}}
    bad = wiring(texts)
    line(not bad, 'wiring: %d links present%s' % (len(LINKS) - len(bad), ('; MISSING: ' + '; '.join(bad)) if bad else ''))
    for what, rel, rx in LINKS:
        t = dict(texts)
        t[rel] = re.sub(rx, 'REMOVED', t[rel])  # every match: a switch may be read in two places
        line(bool(wiring(t)), 'planted unwired link "%s" is caught' % what)
    # the no-clear pass is never made at Create (every player would pay for it): nothing before MakePass names it
    def eager(sw):
        return 'render_pass_noclear' in sw[:sw.index("// The swapchain's pass:")]
    sw = texts['src/renderer/swapchain.cpp']
    line(not eager(sw), 'the no-clear pass is not made in Create')
    # S102 main (05:20): G2 gave MakePass a layout argument; plant after the first CLEAR pass's statement, whatever its arguments, and fail loudly if it does not apply
    planted = re.sub(r'(render_pass_ = MakePass\(VK_ATTACHMENT_LOAD_OP_CLEAR[^;]*;)', r'\1 render_pass_noclear();', sw, count=1)
    if planted == sw:
        line(False, 'planted eager creation DID NOT APPLY (the Create text changed: update the plant)')
    else:
        line(eager(planted), 'planted eager creation is caught')
    print('G1 CHECK: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
