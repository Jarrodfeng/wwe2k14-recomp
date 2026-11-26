#!/usr/bin/env python3
"""S105 A5a lint (text, 0 game runs): kObjOwnRowsPiece must never enter the EYF pre-pass of rf_wrestlers.cpp.
  py -I tools\\scene\\own_rows_prepass_lint.py --root .        the tree
  py -I tools\\scene\\own_rows_prepass_lint.py --selftest      the planted twin must fire
Why: the pre-pass pushes a (owner slot, piece id, alpha) row for every flagged piece onto its OWNER, so the owner's merged mesh fades or skips that piece. An own-rows piece is never drawn by its owner (a
guest-row body, whose meshes carry src_key 0 since S111 GUESTKEY): it is drawn as its own model and its fade rides on its own tint (rf_wrestlers.cpp guest_alpha), so it has no business in that mask (before
S111 GUESTKEY a guest part's src_key was its part index, and such a row hid or faded the body's guest part with the same number). The pre-pass condition is the line that tests `kObjDrawnByOwner |
kObjDetachedPiece` right after `kObjVisible`; the new flag may appear in the file (the route branch) but never on that line. Also: the flag is defined once (scene_record.h) with its own bit.
Exit 0 = clean, 1 = violation."""
import argparse
import os
import re
import sys

PREPASS = re.compile(r'^[^\n]*kObjVisible\)\s*\|\|\s*!\(o\.flags\s*&\s*\(kObjDrawnByOwner[^\n]*$', re.M)


def check(files):
    bad = []
    rf = files.get('src/renderer_scene/rf_wrestlers.cpp')
    if rf is None:
        return ['src/renderer_scene/rf_wrestlers.cpp not found']
    lines = PREPASS.findall(rf)
    if len(lines) != 1:
        bad.append('rf_wrestlers.cpp: the EYF pre-pass condition line was not found exactly once (%d): the lint must be updated with the code' % len(lines))
    for ln in lines:
        if 'kObjOwnRowsPiece' in ln:
            bad.append('rf_wrestlers.cpp: kObjOwnRowsPiece is in the EYF pre-pass condition (its owner never draws it: the row would fade nothing of ours): %s' % ln.strip()[:120])
    sr = files.get('src/renderer_scene/scene_record.h', '')
    m = re.findall(r'kObjOwnRowsPiece\s*=\s*(\d+)', sr)
    if len(m) != 1:
        bad.append('scene_record.h: kObjOwnRowsPiece must be defined exactly once (found %d)' % len(m))
    else:
        bits = [int(v) for v in re.findall(r'\bkObj\w+\s*=\s*(\d+)\s*[,;]', sr)]
        if bits.count(int(m[0])) != 1:
            bad.append('scene_record.h: the bit of kObjOwnRowsPiece (%s) is shared with another ObjectFlags member' % m[0])
    fe = files.get('src/renderer_scene/front_end.cpp', '')   # S105 A5b F5: the piece-row lookup of a model's meshes is guarded by MeshAnswersPieceRow, decided once at AddModelImpl (S111 GUESTKEY: src_kind 2, guest meshes included)
    if 'answers_piece_row = MeshAnswersPieceRow(' not in fe or 'if (me.answers_piece_row) {' not in fe:
        bad.append('front_end.cpp: the EYF piece-row loop is not guarded by MeshAnswersPieceRow (answers_piece_row)')
    if re.search(r'if \(me\.src_kind == 2\)\s*\{', fe):
        bad.append('front_end.cpp: an unguarded `if (me.src_kind == 2) {` piece-row loop is back')
    return bad


def read(root):
    out = {}
    for rel in ('src/renderer_scene/rf_wrestlers.cpp', 'src/renderer_scene/scene_record.h', 'src/renderer_scene/front_end.cpp'):
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
            out[rel] = f.read()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    files = read(os.path.abspath(a.root))
    bad = check(files)
    if a.selftest:
        if bad:
            print('FAIL: the clean tree is not clean: ' + '; '.join(bad))
            return 1
        tw = dict(files)
        tw['src/renderer_scene/rf_wrestlers.cpp'] = files['src/renderer_scene/rf_wrestlers.cpp'].replace('kObjDrawnByOwner | kObjDetachedPiece))', 'kObjDrawnByOwner | kObjDetachedPiece | kObjOwnRowsPiece))', 1)
        t1 = check(tw)
        tw2 = dict(files)
        tw2['src/renderer_scene/scene_record.h'] = files['src/renderer_scene/scene_record.h'].replace('kObjOwnRowsPiece = 16384', 'kObjOwnRowsPiece = 128', 1)
        t2 = check(tw2)
        tw3 = dict(files)
        tw3['src/renderer_scene/front_end.cpp'] = files['src/renderer_scene/front_end.cpp'].replace('if (me.answers_piece_row) {', 'if (me.src_kind == 2) {', 1)
        t3 = check(tw3)
        if not t1 or not t2 or not t3:
            print('FAIL: a planted twin did not fire (pre-pass %s, shared bit %s, front-end guard %s)' % (bool(t1), bool(t2), bool(t3)))
            return 1
        print('selftest: clean tree clean; all three twins fire')
        return 0
    if bad:
        print('FAIL: ' + '; '.join(bad))
        return 1
    print('own_rows_prepass_lint: clean')
    return 0


if __name__ == '__main__':
    sys.exit(main())
