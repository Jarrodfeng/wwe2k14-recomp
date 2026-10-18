"""Generate the coverage routes (Session 8 task 6) from existing routes into tools/coverage/routes/.

Each coverage route is an existing route plus `mark` lines (a mark takes no frames, so the route's timing is
unchanged), optionally a split `wait_frames` (60 + rest, the same total) so a window covers just the first 60 frames
after a scenario starts, and for the match route a pause-menu + replay tail instead of its last lines.

Mark text names the window that ENDS at the mark (src/coverage.h writes NN_<text>.txt). Scenario names use
"scenario.part": tools/coverage/cov.py groups windows by the text before the first '.'.

Where a press moves to another scenario's screen, the press and the frames until the new screen is up get their own
transition window "to_<scenario>" (no scenario in cov.py), checked by a screenshot taken at that mark, so the new
scenario's windows (e.g. pause_menu.first60) start once it is on screen and hold no tail of the screen before it.
The transition marks only split existing waits: every screenshot keeps its frame offset from the press.

The source routes are never edited. Run:  py tools/coverage/make_routes.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "tools" / "coverage" / "routes"

# (rule, anchor regex, payload). Rules apply in order; each anchor matches the first line at or after the previous
# rule's position (so the rules also check the route still has the expected shape).
#   after   -> insert payload lines after the anchor line
#   before  -> insert payload lines before the anchor line
#   split   -> the anchor must be `wait_frames N` (N > 60): replace with wait_frames 60, payload, wait_frames N-60
#   tail    -> drop the anchor line and everything after it, append payload
ROUTES = {
    "cov_title_menu.txt": ("tools/input_scripts/selftest_title.txt", [
        ("after", r"^wait_file SaveData\.Dat", ["mark 1 boot"]),
        ("split", r"^wait_frames 180", ["mark 2 title.first60"]),
        ("after", r"^screenshot title", ["mark 3 title.rest"]),
        ("after", r"^wait_file MNUIHD\.PAC", ["mark 4 storage_notice"]),
        ("split", r"^wait_frames 300", ["mark 5 main_menu.first60"]),
        ("after", r"^screenshot main_menu", ["mark 6 main_menu.rest"]),
    ]),
    "cov_logo.txt": ("tools/input_scripts/repro_logo_lettering.txt", [
        ("after", r"^wait_file SaveData\.Dat", ["mark 1 boot"]),
        ("after", r"^wait_file MNUIHD\.PAC", ["mark 2 title_to_menu"]),
        ("before", r"^press DOWN\b", ["mark 3 main_menu"]),
        ("after", r"^wait_file CASTHUMB\.PAC", ["mark 4 creations_menu"]),
        ("after", r"^screenshot logo_grid", ["mark 5 logo.grid"]),
        ("after", r"^screenshot editor", ["mark 6 logo.editor"]),
        ("after", r"^log repro: Lettering opened", ["mark 7 logo.stamps"]),
        ("after", r"^log repro: PASS", ["mark 8 logo.lettering"]),
    ]),
    "cov_caw.txt": ("tools/review/routes/review_caw.txt", [
        ("after", r"^wait_file SaveData\.Dat", ["mark 1 boot"]),
        ("after", r"^wait_file MNUIHD\.PAC", ["mark 2 title_to_menu"]),
        ("after", r"^screenshot rv_menu_main$", ["mark 3 main_menu"]),
        ("after", r"^screenshot rv_menu_superstar_new_edit", ["mark 4 creations_menu"]),
        ("after", r"^wait_file CREATEMODE\.PAC", ["mark 5 caw.loading"]),
        ("split", r"^wait_frames 80", ["mark 6 caw.first60"]),
        ("after", r"^screenshot rv_caw_t25", ["mark 7 caw.templates"]),
        ("after", r"^screenshot rv_caw_t33", ["mark 8 caw.editor"]),
    ]),
    # Create a Story, built from three screenshot-checked probes (coverage runs 004, 006, 009): CREATIONS -> STORY
    # (6th item) -> NEW opens CUSTOM STORY with an ATTENTION box "Welcome to Create-A-Story!" (SKIP TUTORIAL
    # highlighted) -> A = SKIP TUTORIAL -> calendar (RAW / SUPERSTARS / SMACKDOWN shows) -> A on RAW week 1 -> show
    # page (ADD MOMENT) -> A -> CHOOSE A MOMENT TYPE (SCENE / MATCH) -> A = SCENE -> scene editor (The Miz in the RAW
    # arena, ANIMATION tab) -> A = animation categories (ATTACK highlighted) -> A -> A = first ATTACK animation
    # (BACKSTAGE ASSAULT: backstage set, the animation plays in the editor) -> RIGHT x3 + A = ADVANCED EDITOR ->
    # ADJUST LENGTH tool. Never saves (no START / save prompt; user data mirrored per run). Run 009 recorded the
    # last three windows as story_scene.* (tools/coverage/cov.py ALIASES maps them to story_cutscene).
    "cov_story.txt": ("tools/review/routes/review_caw.txt", [
        ("after", r"^wait_file SaveData\.Dat", ["mark 1 boot"]),
        ("after", r"^wait_file MNUIHD\.PAC", ["mark 2 title_to_menu"]),
        ("after", r"^screenshot rv_menu_main$", ["mark 3 main_menu"]),
        ("after", r"^screenshot rv_menu_creations", []),
        ("tail", r"^press A", [
            "press DOWN 6 20",
            "press DOWN 6 20",
            "press DOWN 6 20",
            "press DOWN 6 20",
            "press DOWN 6 20",
            "wait_frames 30",
            "mark 4 creations_menu",
            "press A",
            "wait_frames 150",
            "mark 5 story.menu",
            "press A",
            "wait_frames 600",
            "mark 6 story.open",
            "press A",
            "wait_frames 240",
            "mark 7 story.calendar",
            "press A",
            "wait_frames 240",
            "mark 8 story.show_page",
            "press A",
            "wait_frames 240",
            "screenshot cov_story3_moment_type",
            "mark 9 story.moment_type",
            "press A",
            "wait_frames 300",
            "screenshot cov_story3_scene_editor",
            "mark 10 story_scene.editor_load",
            "press A",
            "wait_frames 240",
            "screenshot cov_story3_anim_list",
            "mark 11 story_scene.anim_list",
            "press A",
            "wait_frames 120",
            "screenshot cov_story3_s1",
            "mark 12 story_scene.anim_category",
            "press A",
            "wait_frames 300",
            "screenshot cov_story3_s2_300",
            # Transition: A on BACKSTAGE ASSAULT closes the category list and loads the backstage set (run 010 file
            # opens after the press: BG78.PAC +107 frames, the last ones MISC_MW.PAC / WSD04.PAC +198 / +216), then
            # the animation starts; the screenshot at this mark shows it playing.
            "mark 13 to_story_cutscene",
            "wait_frames 300",
            "screenshot cov_story3_s2_600",
            "mark 14 story_cutscene.anim_chosen",
            "press RIGHT 6 20",
            "press RIGHT 6 20",
            "press RIGHT 6 20",
            "wait_frames 30",
            "screenshot cov_story3_icons",
            "mark 15 story_cutscene.icons",
            "press A",
            "wait_frames 300",
            "screenshot cov_story3_p_300",
            "wait_frames 600",
            "screenshot cov_story3_p_900",
            "mark 16 story_cutscene.after_icon_a",
            "wait_frames 60",
            "log coverage: done",
            "quit",
        ]),
    ]),
    "cov_match.txt": ("tools/input_scripts/bench_lesnar_bryan.txt", [
        ("after", r"^wait_file SaveData\.Dat", ["mark 1 boot"]),
        ("after", r"^wait_file MNUIHD\.PAC", ["mark 2 title_to_menu"]),
        ("after", r"^log bench: match launched", ["mark 3 match_setup_menus"]),
        ("after", r"^wait_file 343\.bik2", ["mark 4 match_load"]),
        ("split", r"^wait_frames 100", ["mark 5 entrances.first60"]),
        ("after", r"^wait_file LOADHD\.PAC 9000", ["mark 6 entrances.rest"]),
        ("after", r"^log scene=match", ["mark 7 entrance_to_match_load"]),
        ("split", r"^wait_frames 600", ["mark 8 in_ring.first60"]),
        ("after", r"^screenshot b_match3600", ["mark 9 in_ring.rest"]),
        # Pause menu (docs/session6_notes.md run 087: START in the match opens "1P PAUSED": RETURN TO GAME /
        # RESTART MATCH / REPLAY / ...), then REPLAY (third item). Screenshots check what each step really did.
        # Transition windows (to_pause_menu, to_replay): the press plus 60 frames, checked by the screenshot at the
        # mark; every other screenshot keeps its frame offset from the press (60 + 60 + 60 = the old 60 + 120).
        ("tail", r"^wait_frames 30$", [
            "press START",
            "wait_frames 60",
            "screenshot cov_pause_enter",
            "mark 10 to_pause_menu",
            "wait_frames 60",
            "mark 11 pause_menu.first60",
            "wait_frames 60",
            "screenshot cov_pause",
            "press DOWN 6 20",
            "press DOWN 6 20",
            "wait_frames 20",
            "screenshot cov_pause_replay_selected",
            "mark 12 pause_menu.rest",
            # Run 005 (coverage build): A on REPLAY opens the replay viewer PAUSED on the start of the buffer (HUD:
            # CHANGE TARGET Y, < LB RB >, ANGLE X, ALTITUDE, MOVE LS, SWING/ZOOM RS, RETURN B, SAVE A, AUTO TRACK;
            # frames +180 and +600 identical). No play button on the HUD: try holding RT (full pull) to play.
            # Never press A in the viewer (SAVE).
            "press A",
            "wait_frames 60",
            "screenshot cov_replay_enter",
            "mark 13 to_replay",
            "wait_frames 60",
            "mark 14 replay.first60",
            "wait_frames 60",
            "screenshot cov_replay180",
            "mark 15 replay.paused",
            "set 0x0000 0 255 0 0 0 0",
            "wait_frames 120",
            "screenshot cov_replay_rt120",
            "wait_frames 240",
            "screenshot cov_replay_rt360",
            "mark 16 replay_play.rt_hold",
            "set 0x0000 0 0 0 0 0 0",
            "wait_frames 300",
            "screenshot cov_replay_after300",
            "mark 17 replay_play.after_release",
            "wait_frames 60",
            "log coverage: done",
            "quit",
        ]),
    ]),
}

HEADER = ("# GENERATED by tools/coverage/make_routes.py from {src} (do not edit; edit the generator).\n"
          "# Coverage route: the source route + `mark` lines (window edges for src/coverage.h){extra}.\n"
          "# Run with the coverage build: tools/coverage/run_coverage.ps1\n")


def apply(lines: list[str], rules) -> list[str]:
    out = list(lines)
    pos = 0
    for rule, pattern, payload in rules:
        rx = re.compile(pattern)
        idx = next((i for i in range(pos, len(out)) if rx.search(out[i].split("#", 1)[0].strip())), None)
        if idx is None:
            raise SystemExit(f"anchor not found (rule {rule} {pattern!r}) after line {pos + 1}")
        if rule == "after":
            out[idx + 1:idx + 1] = payload
            pos = idx + 1 + len(payload)
        elif rule == "before":
            out[idx:idx] = payload
            pos = idx + len(payload) + 1
        elif rule == "split":
            n = int(out[idx].split()[1])
            if n <= 60:
                raise SystemExit(f"split: {out[idx]!r} is not longer than 60 frames")
            out[idx:idx + 1] = ["wait_frames 60"] + payload + [f"wait_frames {n - 60}"]
            pos = idx + 2 + len(payload)
        elif rule == "tail":
            out[idx:] = payload
            pos = len(out)
        else:
            raise SystemExit(f"unknown rule {rule}")
    return out


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (src, rules) in ROUTES.items():
        lines = (REPO / src).read_text(encoding="utf-8").splitlines()
        new = apply(lines, rules)
        extra = "; its last lines are replaced by a pause menu + replay tail" if name == "cov_match.txt" else ""
        text = HEADER.format(src=src, extra=extra) + "\n".join(new) + "\n"
        (OUT / name).write_text(text, encoding="utf-8", newline="\n")
        marks = sum(1 for line in new if line.startswith("mark "))
        print(f"{name}: {len(new)} lines, {marks} marks  <- {src}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
