#!/usr/bin/env python3
"""S105 B3: builds and runs tools\\renderer\\b3_store_unit.cpp (the stream-free host's pipeline store wiring: catalog registration, the 3D-scene slice gate, the owner-mode
precompile rule, the store-write failure reason; 0 game runs, no GPU, no SDK), then its planted twins (each must exit 1 and name its check), then the wiring lints on the real
tree, each with a planted twin of its own text that must be caught:
  - scene_host.cpp Start calls RegisterShaderCatalogFrom (kVariants AND kOwnShaders) after Renderer::Create (a stream-free host that never registers the catalog is the v0.4.0 fault: "0 of 0");
  - scene_host.cpp sets SetPipelinePrebuildAllowed(store_progress::PrebuildAllowed(... Source::Draw3D)) before its RenderFrame calls;
  - precompile.h has no unconditional "owner and frame >= 120 = done" rule and asks OwnerPrecompileDone;
  - the launcher no longer hides PREPARE SHADERS or the precompile start behind kStreamFreeTarget.
  py -I tools\\renderer\\b3_store_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {6: 'catalog.own_blobs_in_the_same_call', 1: 'catalog.registered_once_with_alpha_twins', 2: 'rule.not_done_with_records_waiting', 3: 'gate.no_slice_in_a_3d_scene', 4: 'write.failure_names_the_step_and_the_os_error', 5: 'flush.saved_before_the_ack'}


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
from read_unit import read_text  # a split file is read with its parts expanded (tools/split/read_unit.py; text mode: '\n' line ends)


def read(root, rel):
    return read_text(os.path.join(root, rel))


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def lint_catalog(sc):
    m = re.search(r'Renderer::Create\(', sc)
    c = re.search(r'RegisterShaderCatalogFrom\(\s*\*renderer_[^;]*;', sc[m.end():]) if m else None
    return c is not None and 'kVariants' in c.group(0) and 'kOwnShaders' in c.group(0)   # S110 RDRPREP: our own blobs go in the same call


def lint_gate(sc):
    m = re.search(r'SetPipelinePrebuildAllowed\(\s*store_progress::PrebuildAllowed\([^;]*Source::Black', sc)
    r = sc.find('renderer_->RenderFrame(built)')
    return m is not None and r != -1 and m.start() < r


def lint_precompile(pc):
    old = re.search(r'wwe2k14_rdr_owner"\)\s*==\s*"true"\s*&&\s*frame\s*>=\s*120', pc)
    ok_gpu = re.search(r'!owner\s*&&[^;]*gpu_native[^;]*frame\s*>=\s*120', pc) is not None
    return old is None and 'OwnerPrecompileDone(' in pc and ok_gpu


def lint_flush(pc):
    f = pc.find('RequestFlushAndWait(')
    w = pc.find('Write(done ?')
    return f != -1 and w != -1 and f < w


def lint_left(rd):
    m = re.search(r'store_pub_left\.store\(([^;]*)\);', rd)
    return m is not None and 'store_unresolved' not in m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    bad = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)

    if not os.path.exists(CLANG):
        print('FAIL: no compiler at ' + CLANG)
        return 1
    outdir = os.path.join(root, 'out', 'tools')
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, 'b3_store_unit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-Wall', '-Wextra', '-o', exe, os.path.join(root, 'tools', 'renderer', 'b3_store_unit.cpp')], capture_output=True, text=True)
    if b.returncode != 0:
        print(b.stdout + b.stderr)
        print('FAIL: build')
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    print(r.stdout.rstrip())
    need(r.returncode == 0, 'b3_store_unit: every check passed')
    for n, check in PLANTS.items():
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=120)
        fail_lines = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL ')]
        need(p.returncode == 1 and any(check in ln for ln in fail_lines), 'plant %d caught by %s' % (n, check))

    sc = strip_comments(read(root, 'src/renderer_host/scene_host.cpp'))
    need(lint_catalog(sc), 'scene_host.cpp: Start registers the catalog after Renderer::Create')
    need(not lint_catalog(re.sub(r'RegisterShaderCatalogFrom\(', 'RegisterNothing(', sc)), 'wiring twin: a Start that never registers the catalog IS caught')
    need(not lint_catalog(sc.replace('kOwnShaders', 'kNothing')), 'wiring twin: a Start that registers kVariants only IS caught')
    need(lint_gate(sc), 'scene_host.cpp: the slice gate is set from PrebuildAllowed(... Black) before RenderFrame')
    need(not lint_gate(re.sub(r'SetPipelinePrebuildAllowed\(', 'SetNothing(', sc)), 'wiring twin: a frame with no slice gate IS caught')
    pc = strip_comments(read(root, 'src/precompile.h'))
    need(lint_precompile(pc), 'precompile.h: owner mode asks OwnerPrecompileDone, no unconditional frame-120 rule')
    twin = pc.replace('if (owner) {', 'if (rex::cvar::GetFlagByName("wwe2k14_rdr_owner") == "true" && frame >= 120) done = true, have = true;\n  if (owner) {', 1)
    need(not lint_precompile(twin), 'wiring twin: precompile done at frame 120 with records waiting IS caught')
    need(lint_flush(pc), 'precompile.h: owner mode flushes the store (RequestFlushAndWait) before it writes done')
    need(not lint_flush(pc.replace('RequestFlushAndWait(', 'Nothing(')), 'wiring twin: a done written before the store flush IS caught')
    rd = strip_comments(read(root, 'src/renderer/renderer.cpp'))
    need(lint_left(rd), 'renderer.cpp: StorePublish counts only resolvable records as left (unresolved do not block done)')
    need(not lint_left(rd.replace('store_pending.size() - store_cursor', 'store_pending.size() - store_cursor + store_unresolved.size()')), 'wiring twin: unresolved records counted as left IS caught')
    hs = strip_comments(read(root, 'src/renderer_host/host_start.cpp'))
    need('o.precompile_mode' in hs and 'GetFlagByName("wwe2k14_precompile")' in hs, 'host_start.cpp: the precompile boot reaches the host (precompile_mode)')
    lm = strip_comments(read(root, 'src/launcher/launcher_main.cpp'))
    need(not re.search(r'if\s*\(\s*!kStreamFreeTarget\s*\)\s*\{\s*OptionToggle\("PREPARE SHADERS"', lm) and not re.search(r'if\s*\(\s*!kStreamFreeTarget\s*&&\s*cmd\.exe', lm), 'launcher_main.cpp: PREPARE SHADERS and the precompile start are not hidden in the stream-free build')
    mm = strip_comments(read(root, 'src/launcher_mobile/mobile_main.cpp'))
    need('if (kStreamFreeTarget) return false;' not in mm.split('NeedPrecompile')[1][:200] if 'NeedPrecompile' in mm else False, 'mobile_main.cpp: NeedPrecompile is not switched off in the stream-free build')
    print('b3_store_unit.py: ' + ('FAILED: ' + '; '.join(bad) if bad else 'every check passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
