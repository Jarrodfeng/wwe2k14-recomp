#!/usr/bin/env python3
r"""S103 T7: --wwe2k14_dump_targets (the stage mirror) is OFF by default and costs nothing off (text lint of the files that carry it; no build).
  py -I tools\scene\stage_mirror_lint.py
Checks: the cvar defaults to ""; reader.h reads it once (into the frame list); the uses in live.cpp (NoteScene, the split decision, the dump call) all sit behind the empty-list test or the per-message flag; the drawer's DumpMaps runs only behind a non-empty map_dump_dir_ (never set =
nothing runs); ReadImage appears in the mirror code only inside StageMirror / DumpMaps. Exit 1 on any FAIL."""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))


def rd(*p):
    return open(os.path.join(ROOT, *p), encoding='utf-8').read()


live = rd('src', 'renderer_adapter', 'live.cpp')
reader = rd('src', 'renderer_adapter', 'reader.h')
sd = rd('src', 'renderer_scene', 'live_scene_draw.cpp')
sdh = rd('src', 'renderer_scene', 'live_scene_draw.h')
hdr = rd('src', 'renderer_adapter', 'stage_mirror.h')
fails = []


def need(cond, msg):
    print(('ok    ' if cond else 'FAIL  ') + msg)
    if not cond:
        fails.append(msg)


need(re.search(r'REXCVAR_DEFINE_STRING\(wwe2k14_dump_targets,\s*""', reader) is not None, 'cvar wwe2k14_dump_targets defaults to ""')
need(len(re.findall(r'REXCVAR_GET[(]wwe2k14_dump_targets[)]', reader)) == 1, 'reader.h reads the cvar once (the list)')
need('if (!options.dump_targets_frames.empty()) StageMirrorNote(m, guest_frame);' in live, 'StageMirrorNote is called only behind the empty-list test (NoteScene)')
need(len(re.findall(r'StageMirrorNote\(', live)) == 2, 'StageMirrorNote: the definition and the one call')
need(re.search(r'if \(mirror_pending\.empty\(\) \|\| guest_frame < mirror_pending\.front\(\)\) return;\s*if \(m\.scene_mode == 2 && !scene_frame::IsSentinel\(scene_frame::HookFrameOf\(guest_frame\)\)\) return;', live) is not None,
     'mode 2 dumps only on a sentinel walk, at or after the listed frame')
need('StagePageDue(message.guest_frame) || message.stage_mirror || AsyncDue(' in live, 'the split decision adds only the one per-message flag')
need(len(re.findall(r'if \(!split_frame && message\.stage_mirror\) StageMirror\(frame, binder, message\);', live)) == 1 and len(re.findall(r'StageMirror\(frame, binder, message\)', live)) == 1, 'StageMirror is called once, behind message.stage_mirror')
need(len(re.findall(r'if \(message\.stage_mirror\) scene_draw->SetMapDump\(', live)) == 1 and len(re.findall(r'SetMapDump\(', live)) == 1, 'SetMapDump is called once, behind message.stage_mirror')
need('listed frame " + std::to_string(message.stage_mirror_want) + " not drawn' in live, 'F5: a skipped listed frame is logged "not drawn"')
need('std::exchange(map_dump_dir_, std::filesystem::path())' in sd and sd.index('std::exchange(map_dump_dir_') < sd.index('if (!source_->ready())'), 'F3: DrawToPng takes the dump tag before any early return')
need(len(re.findall(r'if \(!dump_dir\.empty\(\)\) DumpMaps\(', sd)) == 1 and len(re.findall(r'\bDumpMaps\(', sd)) == 2, 'DumpMaps: the definition and the one call, behind a non-empty dir')
need('std::filesystem::path map_dump_dir_;' in sdh, 'map_dump_dir_ defaults empty (a path)')
need('RimMap' in hdr and hdr.index('return "rim_blur"') < hdr.index('for (const TechRole& r : kTechRoles)'), 'F1: a draw into a rim_map target is rim_blur before the technique table')
need('"T_GaussianBlur5x5", "rim_node_quad"' in hdr, 'F1: T_GaussianBlur5x5 is rim_node_quad')
need('last_writer' in hdr and 'dropped past' in live, 'F2: last writer tracked and the drop count in the manifest')
# ReadImage only inside the mirror functions
sm = re.search(r'void StageMirror\(const LoadedFrame& frame.*?\n  void PollAsync\(\)', live, re.S)
dm = re.search(r'void LiveSceneDraw::DumpMaps\(.*?\nstd::string LiveSceneDraw::Census', sd, re.S)
need(sm is not None and 'renderer->ReadImage(' in sm.group(0), 'StageMirror holds its ReadImage')
need(dm is not None and 'renderer_.ReadImage(' in dm.group(0), 'DumpMaps holds its ReadImage')
print('stage_mirror_lint: %s' % ('FAIL' if fails else 'PASS'))
sys.exit(1 if fails else 0)
