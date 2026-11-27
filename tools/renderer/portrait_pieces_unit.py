#!/usr/bin/env python3
"""S111 PORTRAITHAIR: builds and runs tools\\renderer\\portrait_pieces_unit.cpp (src\\renderer_scene\\piece_owner.h: a piece's owner slot, a body's slot, the roster portrait's
kept set; SYNTHETIC rows, no game, no GPU, 0 game runs), then its planted twins (each, built with -DWWE2K14_PIECE_OWNER_PLANT=N, must exit 1), then the one-rule check:
  - live_scene_draw.cpp (LiveSceneDraw::DrawPortrait) builds its kept set with KeptWithPieces;
  - rf_wrestlers.cpp (ApplyWrestlers) builds its owner map with OwnerBodySlot and finds a guest-routed piece's owner with PieceOwnerSlot;
  - neither file parses "actor:slot:" itself any more (the rule lives in piece_owner.h only).
    py -I tools\\renderer\\portrait_pieces_unit.py [--root DIR]
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {1: 'bodies_only', 2: 'any_owner_slot', 3: 'layer_not_minus_one'}


def build_run(root, out, plant):
    exe = os.path.join(out, 'portrait_pieces_unit%s.exe' % ('_p%d' % plant if plant else ''))
    cmd = [CLANG, '-std=c++20', '-O1', '-Wall', '-Wextra'] + (['-DWWE2K14_PIECE_OWNER_PLANT=%d' % plant] if plant else []) + ['-o', exe, os.path.join(root, 'tools', 'renderer', 'portrait_pieces_unit.cpp')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        return None, 'build failed'
    return subprocess.run([exe], capture_output=True, text=True, timeout=120), None


def text_of(root, rel):
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 'portrait_pieces_unit')
    os.makedirs(out, exist_ok=True)
    bad = []
    p, err = build_run(root, out, 0)
    if err:
        print('FAIL ' + err)
        return 1
    sys.stdout.write(p.stdout)
    if p.returncode != 0:
        bad.append('unit arms')
    for n, name in PLANTS.items():
        p, err = build_run(root, out, n)
        caught = err is None and p.returncode == 1 and 'FAIL' in p.stdout
        print(('ok   ' if caught else 'FAIL ') + 'planted twin %d (%s) is caught' % (n, name))
        if not caught:
            bad.append('plant %d' % n)
    lsd = text_of(root, 'src/renderer_scene/live_scene_draw.cpp')
    rfw = text_of(root, 'src/renderer_scene/rf_wrestlers.cpp')
    sh = text_of(root, 'src/renderer_host/scene_host.cpp')
    checks = [
        ('KeptWithPieces(' in lsd, 'live_scene_draw.cpp builds the portrait kept set with KeptWithPieces'),
        ('OwnerBodySlot(' in rfw, 'rf_wrestlers.cpp builds its owner map with OwnerBodySlot'),
        (rfw.count('PieceOwnerSlot(') >= 2, 'rf_wrestlers.cpp finds a guest-routed piece owner with PieceOwnerSlot (light and draw place)'),
        ('"actor:slot:"' not in rfw, 'rf_wrestlers.cpp parses no "actor:slot:" id itself'),
        ('"actor:slot:"' not in lsd, 'live_scene_draw.cpp parses no "actor:slot:" id itself'),
        # S111 SOCKPIECE review: the hidden piece's owner row takes its owner slot by the same rule (no "layer - 1" of its own)
        ('PieceOwnerSlot(' in text_of(root, 'src/renderer_scene/piece_hidden.h'), 'piece_hidden.h HiddenOwnerRow finds the owner slot with PieceOwnerSlot'),
        # S111 PORTRAITHAIR review: D1's body count (no runtime unit reaches DrawPortrait): a guest-routed piece is counted where it is drawn, and D1 judges drawn - pieces_drawn
        (re.search(r'\+\+rep\.drawn;\s*\n\s*if \(piece_from_guest\) \+\+rep\.pieces_drawn;', rfw) is not None, 'rf_wrestlers.cpp counts a drawn guest-routed piece in ClassReport::pieces_drawn, right after rep.drawn'),
        ('pieces += r.pieces_drawn' in lsd and 'DrawnCountOk(drawn - pieces)' in lsd, 'live_scene_draw.cpp D1 judges the bodies (drawn - pieces_drawn)'),
        # S111 PORTRAITCENSUS: the host census line prints the pieces the draw drew, from the real count (no runtime unit reaches DrawPortrait / ServePortrait)
        (re.search(r'if \(piece_from_guest\) \+\+rep\.pieces_drawn;[^\n]*\n\s*if \(piece_from_guest\) rep\.piece_ids\.push_back\(uint32_t\(GuestPieceKey\(o\.flags, o\.pad\)\)\);', rfw) is not None,
         'rf_wrestlers.cpp records each counted piece id (GuestPieceKey) right after pieces_drawn'),
        ('*pieces_census = wwe2k14::caw_portrait::PiecesText(pieces, piece_ids.data(), piece_ids.size());' in lsd, 'live_scene_draw.cpp DrawPortrait builds the census field from the summed pieces_drawn'),
        ('why, &loading, &pieces_text);' in sh and '" " + pieces_text) + " " + std::to_string(req.width)' in sh, 'scene_host.cpp ServePortrait passes pieces_text and prints it after drawn'),
    ]
    for ok, what in checks:
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            bad.append(what)
    print('portrait_pieces_unit.py: ' + ('FAIL ' + ', '.join(bad) if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
