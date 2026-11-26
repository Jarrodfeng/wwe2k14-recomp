#!/usr/bin/env python3
r"""CAWH lint (S103): no `fprintf(stderr` / `std::cerr` in src\renderer_scene or src\scene_live outside the allow-list. Census and diagnostic lines of the game go through rdr::Log (src\renderer\problem.h):
stderr never reaches the game's run log (CAWG row 10, CAWB, and the namer's miss diagnostic in particle_glue.h: runs 014-019 never said which picture was missing).
  py -I tools\scene\stderr_census_lint.py [--root DIR] [--selftest]
The allow-list is per file with a CAP (the number of such lines today): existing rig / dev-only prints stay, a NEW line in any file (or any line in a file not listed) fails. Lowering a cap is always fine; raising one needs a reason.
--selftest: the lint must FIRE on the old particle_glue.h line (the real past bug) and on a synthetic std::cerr file, and must stay quiet on the fixed line.
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import sys

PAT = re.compile(r'\bfprintf\s*\(\s*stderr\b|\bstd::cerr\b|\bfputs\s*\([^;]*,\s*stderr\s*\)')
DIRS = ('src/renderer_scene', 'src/scene_live')
# file -> (cap, reason). Every entry is an existing print that is rig-, dev- or env-gated, or a boot line that has not been moved yet (a candidate: it needs problem.cpp linked into the tools that build the file).
ALLOW = {
    'src/renderer_scene/front_end.cpp': (12, 'dev dumps behind WWE_RDR_DUMP_MATS / reload / order prints (pre-existing)'),

    'src/renderer_scene/live_map_rope.cpp': (8, '[B2F] rope dump, env-gated debugging'),
    'src/renderer_scene/live_scene_selftest.cpp': (1, 'the selftest tool\'s usage line'),
    'src/renderer_scene/promotion_rig.h': (2, 'the replay rig only'),
    'src/renderer_scene/record_io.h': (1, 'CPU1 lights-key guard: RDR_SWAP_PLANTS test builds only, abort() follows at once (a log line would never flush)'),
    'src/renderer_scene/rf_arena_live.cpp': (2, '[L2] arena dumps (one env-gated)'),
    'src/renderer_scene/rf_c_core.cpp': (9, '[L2] / [C4] dumps, debugging'),
    'src/renderer_scene/rf_ring.cpp': (1, '[C4] ring dump, debugging'),
    'src/renderer_scene/shader_reload.cpp': (10, 'the hot-reload watcher (dev tool, runs only with a shader folder)'),
}


def strip_comments(t):
    return re.sub(r'//[^\n]*', '', t)


def lint(files):
    """files: {relative path with /: text} -> [problems]"""
    bad = []
    for p, t in sorted(files.items()):
        if not (p.endswith(('.h', '.cpp')) and (p.startswith(DIRS) or p.startswith('synthetic/'))):
            continue
        n = sum(1 for line in strip_comments(t).split('\n') if PAT.search(line))
        cap = ALLOW.get(p, (0, ''))[0]
        if n > cap:
            bad.append('%s: %d line(s) print to stderr (allowed %d): use rdr::Log("%%s", ...) (src\\renderer\\problem.h), stderr never reaches the run log' % (p, n, cap))
    return bad


def read_tree(root):
    out = {}
    for d in DIRS:
        for dp, _, fn in os.walk(os.path.join(root, d)):
            for f in fn:
                if f.endswith(('.h', '.cpp')):
                    full = os.path.join(dp, f)
                    with open(full, encoding='utf-8', errors='replace') as fh:
                        out[os.path.relpath(full, root).replace('\\', '/')] = fh.read()
    return out


OLD_LINE = '    if (wwe2k14::IsDevBuild()) for (const std::string& l : inner_->namer.TakeDiag()) std::fprintf(stderr, "%s\\n", l.c_str());   // S103 PF: the first 8 misses, dev builds only\n'
NEW_LINE = '    if (wwe2k14::IsDevBuild()) for (const std::string& l : inner_->namer.TakeDiag()) rdr::Log("%s", l.c_str());\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    files = read_tree(os.path.abspath(a.root))
    bad = lint(files)
    for b in bad:
        print('FAIL ' + b)
    print('%s the tree: %d problem(s) in %d files' % ('ok  ' if not bad else 'FAIL', len(bad), len(files)))
    rc = 1 if bad else 0
    if a.selftest:
        def need(ok, what):
            nonlocal rc
            print(('ok   ' if ok else 'FAIL ') + what)
            rc |= 0 if ok else 1
        need(bool(PAT.search(OLD_LINE)) and not PAT.search(NEW_LINE), 'the pattern matches the old particle_glue.h line (the real past bug) and not the rdr::Log one')
        pg = 'src/scene_live/particle_glue.h'
        tw = dict(files)
        tw[pg] = OLD_LINE
        need(bool(lint(tw)), 'the lint FIRES on particle_glue.h with the old stderr line put back')
        tw[pg] = NEW_LINE
        need(not lint(tw), 'and is quiet on the fixed line')
        tw['synthetic/new.cpp'] = 'void F() { std::cerr << "census"; }\n'
        need(bool(lint(tw)), 'the lint fires on a new std::cerr file (twin)')
        tw2 = dict(files)
        tw2['src/renderer_scene/rf_ring.cpp'] = files.get('src/renderer_scene/rf_ring.cpp', '') + '\nvoid G() { std::fprintf(stderr, "x\\n"); }\n'
        need(bool(lint(tw2)), 'the lint fires on one MORE line in an allow-listed file (the cap)')
    return rc


if __name__ == '__main__':
    sys.exit(main())
