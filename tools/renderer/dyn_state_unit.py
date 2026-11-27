#!/usr/bin/env python3
# entry: main
"""S111 DYNSTATE: the proof of the per-draw (dynamic) pipeline state (0 game runs, no GPU, no SDK).
  1. builds and runs tools\\renderer\\dyn_state_unit.cpp (the key, the record rule, the format-caps rule, the device rule, the stored list, the seed),
     then its planted twins: each must exit 1 and name its check;
  2. the wiring lints on the real tree, each with a planted twin of its own text that must be caught:
     - every graphics vkCmdBindPipeline of src\\renderer\\renderer_parts is followed by a DynApply (a bind site that sets no state draws with the
       state of the draw before it);
     - GetPipeline and PrebuildOne key by dyn::KeyState, and GetPipeline builds and records that same state;
     - BuildPipeline lists the eight per-draw states under the device's mask;
     - StoreInit reads and writes the list under pstore::LimitsWord and makes the seed's records keys (dyn::KeyState) before MergeSeed;
     - vk_api.h loads the eight commands; Device::Create fills Caps::dyn_mask and logs the device's answer.
  py -I tools\\renderer\\dyn_state_unit.py [--root DIR]      Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'key.blend_twins_are_one_pipeline_where_supported', 2: 'record.sets_what_the_pipeline_no_longer_bakes', 3: 'list.an_old_full_state_list_becomes_this_devices_keys',
          4: 'key.twins_stay_two_pipelines_where_not_supported', 5: 'seed.records_differing_only_in_dynamic_state_merge_to_one'}
PARTS = ('record_pass.inc', 'half_cubes_copy.inc', 'present_dlss_overlay.inc')
STATES = ('VK_DYNAMIC_STATE_CULL_MODE_EXT', 'VK_DYNAMIC_STATE_FRONT_FACE_EXT', 'VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT', 'VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT',
          'VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT', 'VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT', 'VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT', 'VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT')
COMMANDS = ('vkCmdSetCullModeEXT', 'vkCmdSetFrontFaceEXT', 'vkCmdSetDepthTestEnableEXT', 'vkCmdSetDepthWriteEnableEXT', 'vkCmdSetDepthCompareOpEXT',
            'vkCmdSetColorBlendEnableEXT', 'vkCmdSetColorBlendEquationEXT', 'vkCmdSetColorWriteMaskEXT')
BIND = 'vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS'
WINDOW = 12   # lines after a bind in which its DynApply must stand


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def body(text, head):
    """The text of the function whose definition line starts with `head`, to its closing brace at column 0."""
    i = text.find(head)
    if i < 0:
        return ''
    j = text.find('\n}', i)
    return text[i:j] if j > 0 else ''


def unset_binds(text):
    """Line numbers of graphics binds with no DynApply in the WINDOW lines after them."""
    lines = strip_comments(text).split('\n')
    return [no + 1 for no, ln in enumerate(lines) if BIND in ln and not any('DynApply(' in x for x in lines[no:no + WINDOW + 1])]


def unnoted_counts(text):
    """Names of dyn::Counts that never reach a DynNote (S111 DYNSTATE review: a blend the record switched off for a format that cannot blend is counted
    and logged by DynNote, as BuildPipeline counts a baked one). A Counts made and used on one line (a one-draw site) must reach DynNote on that line."""
    s = strip_comments(text)
    bad = []
    for ln in s.split('\n'):
        for name in re.findall(r'dyn::Counts\s+(\w+)\s*;', ln):
            if ('DynApply(' in ln and ('DynNote(' + name) not in ln) or ('DynApply(' not in ln and ('DynNote(' + name) not in s):
                bad.append(name)
    return bad


def lint_key(pp):
    g = body(pp, 'VkPipeline Renderer::Impl::GetPipeline(')
    return ('dyn::KeyState(state, device.caps().dyn_mask)' in g and 'std::memcpy(&key.state, &ks, sizeof(DrawState))' in g and 'BuildPipeline(ks, pass_class)' in g
            and 'StoreNoteBuilt(ks, pass_class' in g)


def lint_build(pp):
    b = body(pp, 'VkPipeline Renderer::Impl::BuildPipeline(')
    return all(s in b for s in STATES) and 'dyn::kRaster' in b and 'dyn::kBlend' in b and 'dynamic.pDynamicStates = dynamic_all' in b


def lint_store(ps):
    i = body(ps, 'void Renderer::Impl::StoreInit(')
    p = body(ps, 'bool Renderer::Impl::PrebuildOne(')
    k, m = i.find('dyn::KeyState('), i.find('MergeSeed(')
    return ('pstore::LimitsWord(opts.format_caps_mask, device.caps().dyn_mask)' in i and re.search(r'ParseList\([^;]*store_caps_mask', i) is not None and -1 < k < m
            and 'dyn::KeyState(r.state, device.caps().dyn_mask)' in p)


def lint_device(dc, va):
    return (all(('X(%s)' % c) in va for c in COMMANDS) and 'caps_.dyn_mask = dyn_mask' in dc and 'dyn::MaskOf(dyn_report, options.dynamic_state)' in dc
            and '[rdr] dynamic state:' in dc and all(('vk::' + c) in dc for c in COMMANDS))


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
    exe = os.path.join(outdir, 'dyn_state_unit.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-deprecated-declarations', '-D_CRT_SECURE_NO_WARNINGS']
    b = subprocess.run([CLANG] + flags + ['-o', exe, os.path.join(root, 'tools', 'renderer', 'dyn_state_unit.cpp')], capture_output=True, text=True, timeout=900)
    if b.returncode != 0:
        print(b.stdout + b.stderr)
        print('FAIL: build dyn_state_unit')
        return 1
    r = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    print(r.stdout.rstrip())
    need(r.returncode == 0, 'dyn_state_unit: every check passed')
    for n, check in PLANTS.items():
        p = subprocess.run([exe, 'plant', str(n)], capture_output=True, text=True, timeout=300)
        fails = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL ')]
        need(p.returncode == 1 and any(check in ln for ln in fails), 'plant %d caught by %s' % (n, check))

    parts = os.path.join('src', 'renderer', 'renderer_parts')
    binds = 0
    for name in PARTS:
        text = read(root, os.path.join(parts, name))
        binds += strip_comments(text).count(BIND)
        need(unset_binds(text) == [], '%s: every graphics pipeline bind is followed by a DynApply (lines without: %s)' % (name, unset_binds(text)))
        need(unnoted_counts(text) == [], '%s: every dyn::Counts reaches DynNote (the format-caps counts of a blend set per draw; not noted: %s)' % (name, unnoted_counts(text)))
    need(binds == 8, 'the eight graphics bind sites are the ones linted (found %d): a new one joins PARTS in this file' % binds)
    others = [n for n in sorted(os.listdir(os.path.join(root, parts))) if n not in PARTS and BIND in strip_comments(read(root, os.path.join(parts, n)))]
    need(others == [], 'no other part binds a graphics pipeline: %s' % others)
    twin = read(root, os.path.join(parts, 'present_dlss_overlay.inc')).replace('DynApply(cmd, dl, state, present_class, dn);', '', 1)
    need(unset_binds(twin) != [], 'wiring twin: a bind site that sets no per-draw state IS caught')
    twin_note = read(root, os.path.join(parts, 'record_pass.inc')).replace('DynNote(dyn_n, pass_class)', 'DynNoteSkipped()', 1)
    need(unnoted_counts(twin_note) != [], 'wiring twin: a pass whose format-caps counts never reach DynNote IS caught')

    pp = strip_comments(read(root, os.path.join(parts, 'passes_pipelines.inc')))
    need(lint_key(pp), 'GetPipeline keys, builds and records by dyn::KeyState')
    need(not lint_key(pp.replace('BuildPipeline(ks, pass_class)', 'BuildPipeline(state, pass_class)')), 'wiring twin: a GetPipeline that builds from the whole state IS caught')
    need(lint_build(pp), 'BuildPipeline lists the eight per-draw states under the device\'s mask')
    need(not lint_build(pp.replace('VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT', 'VK_DYNAMIC_STATE_SCISSOR')), 'wiring twin: a BuildPipeline without the write mask state IS caught')
    ps = strip_comments(read(root, os.path.join(parts, 'pipeline_store.inc')))
    need(lint_store(ps), 'StoreInit reads the list under LimitsWord and makes the seed keys before MergeSeed; PrebuildOne keys by dyn::KeyState')
    need(not lint_store(ps.replace('sr.state = dyn::KeyState(', 'sr.state = Whole(')), 'wiring twin: a StoreInit that merges the seed\'s full states IS caught')
    need(not lint_store(ps.replace('pstore::LimitsWord(opts.format_caps_mask, device.caps().dyn_mask)', 'opts.format_caps_mask')), 'wiring twin: a list header without the dynamic set IS caught')
    dc = strip_comments(read(root, os.path.join('src', 'renderer', 'device.cpp')))
    va = read(root, os.path.join('src', 'renderer', 'vk_api.h'))
    need(lint_device(dc, va), 'vk_api.h loads the eight commands; Device::Create checks them, fills Caps::dyn_mask and logs the device\'s answer')
    need(not lint_device(dc.replace('caps_.dyn_mask = dyn_mask', 'caps_.dyn_mask = 0'), va), 'wiring twin: a device that never reports its mask IS caught')
    print('dyn_state_unit.py: ' + ('FAILED: ' + '; '.join(bad) if bad else 'every check passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
