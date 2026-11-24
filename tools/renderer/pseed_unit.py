#!/usr/bin/env python3
# entry: lint_init (the wiring lints read StoreInit, where ParseSeedLines + MergeSeed are called; the loader itself is entered through pseed_unit.cpp, which this driver builds and runs)
# (a driver: the loader unit tools/renderer/pseed_unit.cpp; MergeSeed is entered as StoreInit calls it, wiring-linted)
"""S106 PSEED: the pipeline seed's own proof (0 game runs, no GPU, no SDK). Runs in the lane's cheap proof:
  1. builds and runs tools\\renderer\\pseed_unit.cpp (the loader half: R-line parse, merge, save rule), then its five planted twins (each must exit 1 and name its check);
  2. pipeline_seed.py selftest (the merger / checker's planted faults: stale hash, flipped digit, duplicate, multiview, wrong count, wrong version, short line);
  3. the committed list: every R line parses (the unit's --list) and `pipeline_seed.py check` against the shaders' own hashes (pseed_catalog.exe, hashed by the renderer's HashWords)
     passes (docs\\research\\s106\\PSEED.md adversary C5: a stale seed never ships; no seed yet = pass);
  4. the wiring lints on the real tree, each with a planted twin of its own text that must be caught:
     - StoreInit reads opts.pipeline_seed_path with ParseSeedLines + MergeSeed before `store_session = list_session + 1`;
     - StoreSave keeps records through SaveKeeps (an undrawn seed record never joins the player's list);
     - host_start.cpp sets ro.pipeline_seed_path from ListPath inside the store-on block and does NOT include shader_list.h (R1: the cvar's non-inline definition would be linked twice);
     - shader_list.h Import takes the path from ListPath (one path rule for the seed and the importer).
  py -I tools\\renderer\\pseed_unit.py [--root DIR]      Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'parse.bad_lines_never_loaded', 2: 'merge.known_keys_and_seed_duplicates_not_added_twice', 3: 'save.an_undrawn_seed_record_is_not_saved',
          4: 'parse.multiview_dropped_without_the_device_feature', 5: 'parse.names_kept',
          6: 'depth.token_becomes_the_devices_depth_format', 7: 'header.a_wrong_version_refuses_the_whole_seed',
          8: 'depth.a_colour_format_the_device_cannot_render_to_drops_the_record',
          9: 'depth.a_device_depth_format_of_0_refuses_the_seed'}


def read(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def lint_init(rd):
    m = re.search(r'void Renderer::Impl::StoreInit\(.*?\n\}', rd, flags=re.S)
    if not m:
        return False
    b = m.group(0)
    i, j, k, s = b.find('opts.pipeline_seed_path'), b.find('ParseSeedLines('), b.find('MergeSeed('), b.find('store_session = list_session + 1')
    return -1 not in (i, j, k, s) and i < j < k < s


def lint_harvest(rl):
    return bool(re.search(r'harvest\s*=\s*\[bool\]\$HarvestPipelines', rl)) and bool(re.search(r'\$HarvestPipelines\s*=\s*\[switch\]\[bool\]\$job\.harvest', rl))


def lint_merge(ms):
    return "startswith('H rdr ')" in ms and 'for ln in seed_lines:' in ms and 'o.write(ln' in ms


def lint_loader(rd):
    m = re.search(r'void Renderer::Impl::StoreInit\(.*?\n\}', rd, flags=re.S)
    if not m:
        return False
    c = re.search(r'ParseSeedLines\((.*?)\);', m.group(0), flags=re.S)
    return c is not None and 'caps().depth_format' in c.group(1) and 'VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT' in c.group(1)


def lint_save(rd):
    m = re.search(r'void Renderer::Impl::StoreSave\(.*?\n\}', rd, flags=re.S)
    return m is not None and 'SaveKeeps(r)' in m.group(0) and 'if (r.dead) continue' not in m.group(0)


def lint_host(hs, raw):
    inc = re.search(r'#include\s+"[^"]*shader_list\.h"', raw) is not None
    blk = re.search(r'if \(REXCVAR_GET\(wwe2k14_rdr_pipeline_store\)\) \{(.*?)\n  \}', hs, flags=re.S)
    if blk is None:
        return False
    b = blk.group(1)
    g, p = b.find('REXCVAR_GET(wwe2k14_rdr_pipeline_seed)'), b.find('ro.pipeline_seed_path')
    # S106 PSEED fix B2: the path is set ONLY under the seed cvar (a statement of its own: the GET precedes the assignment, no `;` or brace between); SHADERSEED (2026-11-24): the cvar defaults to true
    guarded = -1 < g < p and ';' not in b[g:p] and '{' not in b[g:p] and '}' not in b[g:p]
    default_on = re.search(r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_pipeline_seed,\s*true\b', hs) is not None
    return (not inc) and p != -1 and 'ListPath(' in b and guarded and default_on


def lint_cap(rd):
    """S106 PSEEDF2 N9: StoreNoteBuilt's 'far past the cap' test must not count the pending seed records (the seed adds up to ~4000 records at boot): the cap is kMaxRecords * 2 + store_seed_added."""
    m = re.search(r'void Renderer::Impl::StoreNoteBuilt\(.*?\n\}', rd, flags=re.S)
    i = re.search(r'void Renderer::Impl::StoreInit\(.*?\n\}', rd, flags=re.S)
    return (m is not None and i is not None and re.search(r'store_records\.size\(\)\s*>=\s*pstore::kMaxRecords\s*\*\s*2\s*\+\s*store_seed_added', m.group(0)) is not None
            and re.search(r'store_seed_added\s*=\s*sm\.added', i.group(0)) is not None)


def lint_inc_last(raw):
    """S106 PSEEDF2 N1: shader_list_path.h (platform/compat.h -> <windows.h>: near / far macros) is the LAST #include of host_start.cpp, so no header after it sees windows.h."""
    incs = re.findall(r'^\s*#include\s+[<"]([^>"]+)[>"]', raw, flags=re.M)
    return bool(incs) and incs[-1].endswith('shader_list_path.h')


ABS_USER = re.compile(r'[A-Za-z]:[\\/]+Users[\\/]', re.I)


def lint_no_user_paths(root):
    """S106 PSEEDF2 N9: no absolute user path (a drive + Users\\name) in the lane's tools: the repo is public. Returns the offending 'file:line' list."""
    out = []
    for name in sorted(os.listdir(os.path.join(root, 'tools', 'renderer'))):
        if not (name.startswith('pseed_') or name.startswith('pipeline_')) or not name.endswith(('.py', '.cpp', '.h')):
            continue
        with open(os.path.join(root, 'tools', 'renderer', name), encoding='utf-8', errors='replace') as f:
            for no, ln in enumerate(f, 1):
                if ABS_USER.search(ln):
                    out.append('%s:%d' % (name, no))
    return out


def lint_import(sl):
    m = re.search(r'inline void Import\(.*?\n\}', sl, flags=re.S)
    return m is not None and 'ListPath(setting)' in m.group(0) and 'GetModuleFileNameW' not in m.group(0)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=900, **kw)


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
    exe = os.path.join(outdir, 'pseed_unit.exe')
    flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-deprecated-declarations', '-D_CRT_SECURE_NO_WARNINGS']
    b = run([CLANG] + flags + ['-o', exe, os.path.join(root, 'tools', 'renderer', 'pseed_unit.cpp')])
    if b.returncode != 0:
        print(b.stdout + b.stderr)
        print('FAIL: build pseed_unit')
        return 1
    r = run([exe])
    print(r.stdout.rstrip())
    need(r.returncode == 0, 'pseed_unit: every check passed')
    for n, check in PLANTS.items():
        p = run([exe, 'plant', str(n)])
        fails = [ln for ln in p.stdout.splitlines() if ln.startswith('FAIL ')]
        need(p.returncode == 1 and any(check in ln for ln in fails), 'plant %d caught by %s' % (n, check))

    ps = run([sys.executable, '-I', os.path.join(root, 'tools', 'renderer', 'pipeline_seed.py'), 'selftest'])
    print(ps.stdout.rstrip())
    need(ps.returncode == 0, 'pipeline_seed.py selftest: the merger / checker catch every planted fault')

    cat_exe = os.path.join(outdir, 'pseed_catalog.exe')
    cb = run([CLANG] + flags + ['-o', cat_exe, os.path.join(root, 'tools', 'renderer', 'pseed_catalog.cpp'), os.path.join(root, 'src', 'renderer', 'shaders', 'spv', 'fx_spv_data.cpp')])
    if cb.returncode != 0:
        print(cb.stdout + cb.stderr)
        need(False, 'build pseed_catalog')
    else:
        catalog = os.path.join(outdir, 'pseed_catalog.txt')
        cr = run([cat_exe])
        with open(catalog, 'w', encoding='utf-8') as f:
            f.write(cr.stdout)
        need(cr.returncode == 0 and cr.stdout.count('\n') > 100, 'pseed_catalog: the shaders\' own hashes printed (%d blobs)' % cr.stdout.count('\n'))
        lst = os.path.join(root, 'db', 'shaders', 'shader_list.txt')
        ck = run([sys.executable, '-I', os.path.join(root, 'tools', 'renderer', 'pipeline_seed.py'), 'check', '--list', lst, '--catalog', catalog])
        print(ck.stdout.rstrip()[-1500:])
        need(ck.returncode == 0, 'the committed shader_list.txt seed is current (no stale hash, no duplicate, count and version right)')
        ul = run([exe, '--list', lst])
        need(ul.returncode == 0, 'pseed_unit --list: every committed R line parses in the loader')

    rd = strip_comments(read(root, 'src/renderer/renderer.cpp'))
    need(lint_init(rd), 'renderer.cpp: StoreInit reads the seed (ParseSeedLines + MergeSeed) before store_session is set')
    need(not lint_init(rd.replace('MergeSeed(', 'MergeNothing(')), 'wiring twin: a StoreInit that never merges the seed IS caught')
    need(lint_save(rd), 'renderer.cpp: StoreSave keeps records through SaveKeeps (an undrawn seed record is not saved)')
    need(not lint_save(rd.replace('SaveKeeps(r)', '!r.dead')), 'wiring twin: a save that keeps undrawn seed records IS caught')
    raw_hs = read(root, 'src/renderer_host/host_start.cpp')
    hs = strip_comments(raw_hs)
    need(lint_host(hs, raw_hs), 'host_start.cpp: the seed path is set from ListPath in the store-on block, shader_list.h is not included here')
    need(not lint_host(hs, raw_hs + '\n#include "../shader_list.h"\n'), 'wiring twin: host_start.cpp including shader_list.h (the duplicate cvar definition) IS caught')
    need(not lint_host(hs.replace('ro.pipeline_seed_path', 'ro.nothing'), raw_hs), 'wiring twin: a host that never sets the seed path IS caught')
    need(not lint_host(hs.replace('if (REXCVAR_GET(wwe2k14_rdr_pipeline_seed))', ''), raw_hs), 'wiring twin: a host that sets the seed path with no cvar test (the seed on for every player) IS caught')
    need(not lint_host(hs.replace('REXCVAR_DEFINE_BOOL(wwe2k14_rdr_pipeline_seed, true', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_pipeline_seed, false'), raw_hs), 'wiring twin: a seed cvar that defaults to off (a shipped seed nobody reads) IS caught')
    need(lint_loader(rd), 'renderer.cpp: StoreInit hands the device\'s depth format and colour-attachment test to the seed parser')
    need(not lint_loader(rd.replace('caps().depth_format', 'kLiteral')), 'wiring twin: a StoreInit that does not pass the device\'s depth format IS caught')
    need(not lint_loader(rd.replace('VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT', 'VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT')), 'wiring twin: a StoreInit that never asks for colour-attachment support IS caught')
    need(lint_cap(rd), 'renderer.cpp: StoreNoteBuilt\'s record cap leaves room for the pending seed records (kMaxRecords * 2 + store_seed_added)')
    need(not lint_cap(rd.replace(' + store_seed_added', '')), 'wiring twin: a cap that counts the seed records IS caught')
    need(lint_inc_last(raw_hs), 'host_start.cpp: shader_list_path.h (windows.h) is the last #include')
    need(not lint_inc_last(raw_hs + '\n#include "wide_frame.h"\n'), 'wiring twin: an include after shader_list_path.h IS caught')
    need(lint_no_user_paths(root) == [], 'no absolute user path in tools\\renderer\\pseed_* / pipeline_* (public repo): %s' % lint_no_user_paths(root))
    need(bool(ABS_USER.search('GAME_LIST = r\'C:' + chr(92) + 'Users' + chr(92) + 'x' + chr(92) + 'out\'')), 'wiring twin: an absolute user path IS caught')
    sl = strip_comments(read(root, 'src/shader_list.h'))
    need(lint_import(sl), 'shader_list.h: Import takes the list path from ListPath (one rule for the seed and the importer)')
    need(not lint_import(sl.replace('ListPath(setting)', 'Other(setting)')), 'wiring twin: an Import with its own path rule IS caught')
    # S106 PSEED fix round: the tools' own selftests, and the wiring of F6 / F7
    for tool, what in (('pseed_proof.py', 'the proof: known misses by full key, the 780 cold names, P2 per key'), ('pipeline_census.py', 'the census: every job starts on an empty store')):
        pr = run([sys.executable, '-I', os.path.join(root, 'tools', 'renderer', tool), '--selftest'])
        print(pr.stdout.rstrip())
        need(pr.returncode == 0, tool + ' selftest: ' + what)
    rl = read(root, 'tools/bench/run_list.ps1')
    need(lint_harvest(rl), 'run_list.ps1: -HarvestPipelines is carried into the parallel worker (job.json harvest, read back like step30)')
    need(not lint_harvest(rl.replace('harvest = [bool]$HarvestPipelines;', '')), 'wiring twin: a job.json without harvest IS caught')
    ms = read(root, 'tools/shaders/merge_storage.py')
    need(lint_merge(ms), 'merge_storage.py --list carries the R / H rdr seed lines over (a regenerated list keeps its seed)')
    need(not lint_merge(ms.replace("for ln in seed_lines:", "for ln in []:")), 'wiring twin: a merge_storage that reads the seed lines but never writes them IS caught')
    print('pseed_unit.py: ' + ('FAILED: ' + '; '.join(bad) if bad else 'every check passed'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
