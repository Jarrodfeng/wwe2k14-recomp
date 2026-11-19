#!/usr/bin/env python3
"""S103 CAE lint (review F5): the arena id's two numbers and the stage object have ONE home.
  py -I tools/scene/cae_literals_lint.py [--root DIR]
Fails when, in src/scene_live (comments stripped; a line may carry `lint:cae-ok` with its reason):
  - a literal 1216 or a `< 400` appears outside arena_id_core.h (kStageArenaIdOffset, kMaxArenaId);
  - the stage object is read raw (`StageFx().Stage()`) outside stage_fx_contrib.h (use StageLive() / StageIdRead() / CurrentStage()).
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    root = os.path.abspath(ap.parse_args().root)
    d = os.path.join(root, 'src', 'scene_live')
    bad = 0
    n = 0
    lit = re.compile(r'\b1216\b|<\s*400u?\b')
    raw = re.compile(r'StageFx\(\)\s*\.\s*Stage\s*\(')
    for f in sorted(os.listdir(d)):
        if not f.endswith(('.h', '.cpp')):
            continue
        for i, ln in enumerate(open(os.path.join(d, f), encoding='utf-8', errors='replace'), 1):
            if 'lint:cae-ok' in ln:
                continue
            code = ln.split('//')[0]
            n += 1
            if f != 'arena_id_core.h' and lit.search(code):
                print('FAIL: src/scene_live/%s:%d literal 1216 / `< 400` (use arena_id_core.h kStageArenaIdOffset / kMaxArenaId): %s' % (f, i, ln.strip()[:110]))
                bad += 1
            if f != 'stage_fx_contrib.h' and raw.search(code):
                print('FAIL: src/scene_live/%s:%d raw StageFx().Stage() read (use StageLive / StageIdRead / CurrentStage): %s' % (f, i, ln.strip()[:110]))
                bad += 1
    # the plant: the lint must see a literal
    probe = 'x < 400; y = 1216;  StageFx().Stage()'
    assert lit.search(probe) and raw.search(probe), 'the lint patterns are blind'
    print('cae_literals_lint: %s (%d lines)' % ('FAIL' if bad else 'PASS', n))
    return 1 if bad else 0


sys.exit(main())
