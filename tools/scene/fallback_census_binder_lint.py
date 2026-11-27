#!/usr/bin/env python3
"""Seam lint for the fallback census in the binder (S103 L2b review F1 / F3): the remnant_extra note must NOT depend on the post chain (after_post_ is never set under OURS: scene_skip_nodes.h skips it),
must go through NoteRemnantOnce with the once-per-Bind flag reset in Bind, and an unfound frame image must be a census reason. Exit 1 on a break.
  py -I tools\\scene\\fallback_census_binder_lint.py"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "split"))
from read_unit import read_path  # binder.h is split: read it with its parts expanded (tools/split/read_unit.py)
text = read_path(Path(__file__).resolve().parents[2] / "src" / "renderer_adapter" / "binder.h")
bad = []
calls = [l.split("//")[0] for l in text.splitlines() if "NoteRemnantOnce(" in l]   # code part only: the comment may name after_post_
if len(calls) != 1:
    bad.append(f"expected exactly one NoteRemnantOnce call in binder.h, found {len(calls)}")
elif "after_post_" in calls[0]:
    bad.append("the remnant_extra note is gated on after_post_, which the OURS path never sets (review F1)")
elif "scene_frame_id_" not in calls[0]:
    bad.append("the remnant_extra note is not gated on scene_frame_id_")
if not re.search(r"fc_remnant_noted_\s*=\s*false", text):
    bad.append("fc_remnant_noted_ is never reset (it must be reset at the start of Bind)")
if not re.search(r'NoteRender\([^;]*"frame_image_unfound"', text):
    bad.append('an unfound frame image is not noted as "frame_image_unfound" (review F3)')
for b in bad:
    print("FAIL: " + b)
print("fallback_census_binder_lint: " + ("FAILED" if bad else "ok"))
sys.exit(1 if bad else 0)
