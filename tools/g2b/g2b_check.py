#!/usr/bin/env python3
"""S102 lane G2b offline check (0 game runs, 0 Surface runs, no window): wwe2k14_rdr_exclusive_fullscreen (docs\\renderer\\OUTPUT_PASS.md "Lane G2b").

  py -I tools\\g2b\\g2b_check.py [--root DIR]

1. unit: tools\\g2b\\exclusive_test.cpp against the real src\\renderer\\exclusive_policy.h (switch off = no structs; extension missing = "unsupported" and the normal path;
   a lost-exclusive event takes the fallback once; a refused create / refused acquire / repeat LOST rebuild; a surface that says no). Planted twin headers must each FAIL it.
2. wiring (regex on the real sources, planted twins that must be caught): the exclusive structs exist in ONE place behind excl_.ChainStructs(); Present and
   Acquire rebuild on every lost result and log on the transition only; a refused create is retried once without the structs; the instance extension is requested
   under the same switch as the device extension; the table line (StartOnly), cvar, option are all there; Windows only.
The renderer and rplay compile through tools\\renderer\\build.ps1 (the hand-back says it was run); the game exe through tools\\build.ps1.
Sources are read with CRLF turned into LF, so the plants work on either checkout. Exit 0 only when everything holds."""
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
        return f.read().replace('\r\n', '\n')  # F7: an LF and a CRLF checkout read the same


def unit(root, work, tag, header_text):
    d = os.path.join(work, tag)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'exclusive_policy.h'), 'w', newline='') as f:
        f.write(header_text)
    exe = os.path.join(d, 't.exe')
    c = subprocess.run([CLANG, '-std=c++20', '-I', d, os.path.join(root, 'tools/g2b/exclusive_test.cpp'), '-o', exe], capture_output=True, text=True)
    if c.returncode:
        print(c.stdout, c.stderr)
        return None
    return subprocess.run([exe], capture_output=True, text=True).returncode == 0


def plant(text, old, new):
    out = text.replace(old, new)
    assert out != text, 'plant did not apply: ' + old
    return out


def wiring_ok(sc, dev, ren, rd, tab):
    """All wiring facts as one predicate so a planted copy is judged by the same code."""
    f = []
    gate = re.search(r'if \(excl_\.ChainStructs\(\)\) \{(.*?)\n  \}\n#endif', sc, re.S)
    f.append(('exclusive info struct made in Build and the support query only', len(re.findall(r'ExclusiveInfo exclusive_info\{', sc)) == 2))
    f.append(('structs attached to the swapchain only inside the ChainStructs() gate', bool(gate) and 'info.pNext = &exclusive_info' in gate.group(1) and len(re.findall(r'(?<![\w.])info\.pNext = &exclusive_info', sc)) == 1))
    f.append(('F2: Present and Acquire rebuild on EVERY lost result (NeedsRebuild) and log on the transition only',
              len(re.findall(r'excl_\.NeedsRebuild\(int\(result\)\)\) \{[^}]*?stale_ = true;[^}]*?if \(excl_\.NoteResult\(int\(result\)\)\) LogExclusive\(\);', sc)) == 2))
    f.append(('F2: a refused acquire sets stale_', 'if (excl_.state() == excl::State::Lost) stale_ = true;' in sc))
    f.append(('F1: a refused exclusive create is noted, then retried once without the structs and without oldSwapchain',
              re.search(r'if \(created != VK_SUCCESS && chained\) \{[^}]*?NoteCreateFailed\(\)[^}]*?info\.pNext = nullptr;[^}]*?info\.oldSwapchain = VK_NULL_HANDLE;[^}]*?created = vkCreateSwapchainKHR\(dev, &info', sc, re.S) is not None
              and sc.count('vkCreateSwapchainKHR(dev, &info') == 2))
    f.append(('F4: every chained create is followed by an acquire (a rebuild while On included)', 'if (excl_.AcquireAfterCreate(chained)) AcquireExclusive();' in sc and 'was_exclusive' not in sc))
    i_rel, i_new = sc.find('ReleaseExclusive(old);'), sc.find('VkResult created = vkCreateSwapchainKHR(dev, &info')
    f.append(('F4: ReleaseExclusive(old) comes before vkCreateSwapchainKHR in Build', 0 < i_rel < i_new))
    f.append(('F3: the surface is asked (fullScreenExclusiveSupported) before the first swapchain', 'caps2(device_->physical(), &surface_info, &caps)' in sc and re.search(r'MakeSurface\(first, error\)\) return false;\n#if defined\(_WIN32\)\n  QueryExclusiveSupport\(\);', sc) is not None))
    f.append(('F5: pointers are members; no null-monitor branch; no exclusive_state() getter', 'acquire_fn_' in sc and 'static ExclusiveFn' not in sc and 'if (exclusive_win32.hmonitor)' not in sc and 'exclusive_state' not in sc))
    f.append(('O1: the display is read only while on or lost', 'excl_.state() == excl::State::On || excl_.state() == excl::State::Lost' in sc))
    f.append(('the start line is printed after the first Build', re.search(r'Build\(\);\n\s*LogExclusive\(\);', sc) is not None))
    f.append(('released when the swapchain goes (ReleaseSurface, Destroy) and once in Build for the old one', sc.count('ReleaseExclusive(swapchain_);') == 2 and sc.count('ReleaseExclusive(old);') == 1))
    f.append(('no windows.h include in the renderer file', re.search(r'#\s*include\s*<windows\.h>', sc) is None))
    f.append(('F3 lint: the device extension is gated on the instance extension under the same switch',
              re.search(r'options\.exclusive_fullscreen && InstanceHas\("VK_KHR_get_surface_capabilities2"\)\) \{\s*instance_extensions\.push_back\("VK_KHR_get_surface_capabilities2"\);\s*has_caps2 = true;', dev) is not None
              and re.search(r'if \(options\.exclusive_fullscreen && has_caps2\)', dev) is not None))
    f.append(('Windows-only extension probe', re.search(r'#if defined\(_WIN32\)\n\s*//[^\n]*\n\s*if \(options\.exclusive_fullscreen && has_caps2\)', dev) is not None))
    f.append(('device gets the switch only for a windowed device', 'device_options.exclusive_fullscreen = opts.exclusive_fullscreen && windowed;' in ren))
    f.append(('swapchain is configured before Create', re.search(r'swapchain\.ConfigureExclusive\(opts\.exclusive_fullscreen && windowed\);[^\n]*\n\s*if \(windowed && !swapchain\.Create', ren) is not None))
    f.append(('table: StartOnly, default 0', 'X(ExclusiveFullscreen, "wwe2k14_rdr_exclusive_fullscreen", StartOnly, 0)' in tab))
    f.append(('cvar defined, default off', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_exclusive_fullscreen, false' in rd))
    f.append(('reader sets the option from the cvar on Windows only', re.search(r'#if defined\(_WIN32\)\n\s*options\.exclusive_fullscreen = REXCVAR_GET\(wwe2k14_rdr_exclusive_fullscreen\);', rd) is not None))
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    root = os.path.abspath(ap.parse_args().root)
    work = os.path.join(root, 'out', 'g2b_check')
    header = read(root, 'src/renderer/exclusive_policy.h')

    line(unit(root, work, 'real', header) is True, 'unit: the real policy header passes exclusive_test.cpp')
    plants = [
        ('a lost event does not stop the structs (no fallback)', 'return armed_ && state_ != State::Lost;', 'return armed_;'),
        ('a lost event is never noticed', 'if (result != kResultExclusiveLost || !armed_ || state_ == State::Lost) return false;', 'return false;'),
        ('extension missing still chains the structs', 'armed_ = wanted && extension_present;', 'armed_ = wanted;'),
        ('switch off still chains the structs', 'return armed_ && state_ != State::Lost;', 'return state_ != State::Lost;'),
        ('F2: rebuild only on the transition', 'bool NeedsRebuild(int result) const { return armed_ && result == kResultExclusiveLost; }',
         'bool NeedsRebuild(int result) const { return armed_ && state_ != State::Lost && result == kResultExclusiveLost; }'),
        ('F1: a failed create leaves the structs on', 'if (!armed_ || state_ == State::Lost) return false;\n    state_ = State::Lost;\n    held_ = false;\n    return true;', 'return false;'),
        ('F4: a rebuild in state On does not acquire again', 'bool AcquireAfterCreate(bool chained) const { return chained; }', 'bool AcquireAfterCreate(bool chained) const { return chained && state_ != State::On; }'),
        ('F4: a release is not recorded (Destroy would release a swapchain that holds nothing)', 'void NoteReleased() { held_ = false; }', 'void NoteReleased() {}'),
        ('F3: a surface that says no still chains', '    armed_ = false;\n    held_ = false;\n    state_ = State::Unsupported;', '    armed_ = armed_;'),
    ]
    for i, (name, old, new) in enumerate(plants):
        line(unit(root, work, 'plant%d' % i, plant(header, old, new)) is False, 'unit planted (%s): caught' % name)

    sc = read(root, 'src/renderer/swapchain.cpp')
    dev = read(root, 'src/renderer/device.cpp')
    ren = read(root, 'src/renderer/renderer.cpp')
    rd = read(root, 'src/renderer_adapter/reader.h')
    tab = read(root, 'src/renderer_adapter/rdr_switches.h')
    for name, ok in wiring_ok(sc, dev, ren, rd, tab):
        line(ok, 'wiring: ' + name)
    twins = [
        ('Present forgets stale_ after a loss', 'sc', plant(sc, 'one line on the transition, never a crash\n    stale_ = true;\n', 'one line on the transition, never a crash\n')),
        ('structs attached without the gate', 'sc', plant(sc, 'if (excl_.ChainStructs()) {\n    exclusive_win32', 'if (true) {\n    exclusive_win32')),
        ('structs attached outside the gate too', 'sc', plant(sc, '  VkResult created = vkCreateSwapchainKHR(dev, &info', '  info.pNext = &exclusive_info;\n  VkResult created = vkCreateSwapchainKHR(dev, &info')),
        ('F1: the retry is deleted', 'sc', plant(sc, '    created = vkCreateSwapchainKHR(dev, &info, nullptr, &swapchain_);\n  }', '  }')),
        ('F2: Acquire rebuilds only on the transition', 'sc', plant(sc, 'if (excl_.NeedsRebuild(int(result))) {  // S102 G2b: exclusivity lost (alt-tab', 'if (excl_.NoteResult(int(result))) {  // S102 G2b: exclusivity lost (alt-tab')),
        ('F4: the old swapchain is released after the create instead of before', 'sc', plant(plant(sc, '  ReleaseExclusive(old);  // S102 G2b F4', '  // S102 G2b F4'), '  if (excl_.AcquireAfterCreate(chained)) AcquireExclusive();', '  ReleaseExclusive(old);\n  if (excl_.AcquireAfterCreate(chained)) AcquireExclusive();')),
        ('F3: the instance extension push is dropped', 'dev', plant(dev, '      instance_extensions.push_back("VK_KHR_get_surface_capabilities2");\n', '')),
    ]
    for name, which, tmp in twins:
        args = {'sc': sc, 'dev': dev}
        args[which] = tmp
        bad = [n for n, ok in wiring_ok(args['sc'], args['dev'], ren, rd, tab) if not ok]
        line(bool(bad), 'wiring planted (%s): caught by "%s"' % (name, bad[0] if bad else 'nothing'))
    print('g2b_check:', 'FAILED %d' % fails if fails else 'ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
