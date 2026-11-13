#!/usr/bin/env python3
"""Phase D coverage board (S101 lane 93; RENDERER_ARC.md section 5 "Phase D: the gate"; extended by lane 156).

Reads every case zip under out\\s101\\runs (game data, local only: this tool reads ONLY manifest.json and log.txt from each zip, plus the
run folder's plan.json, and never copies content out; the board holds names, counts and dates) and builds the board:
core-loop item x device x draw class, with the newest evidence for each cell and the PATH that draws the class.

  py -I tools\\board\\coverage.py                     print the board (markdown) to stdout
  py -I tools\\board\\coverage.py --write             write docs\\renderer\\COVERAGE_BOARD.md (generated; never edit by hand)
  py -I tools\\board\\coverage.py --runs DIR          read zips under DIR instead of <main checkout>\\out\\s101\\runs (and no live channels)
  py -I tools\\board\\coverage.py --check             parse every zip; exit 1 if any zip cannot be read (a gate check)
  py -I tools\\board\\coverage.py --selftest          planted-fault selftest (see tools\\board\\selftest.py)

Cell states (a cell is one core-loop item x device x draw class):
  ours      the newest zip with evidence has a census line for the class with replaced > 0 and mismatch 0 (the class draws it)
  stream    the class's switch is off (known from the zip's logged cvars or the run's plan.json) or its census line shows
            replaced 0: the game's own draws are on screen for it
  unproven  the switch is on (or a census line exists with mismatches) but the census line is missing or not clean: NEVER "ours"
  none      no zip for this item on this device has any evidence about the class
  n/a       the class does not occur in the item (an assumption, not evidence: APPLIES below) and nothing says otherwise

lane 156 (S101) added: 23 classes (every merged promotion: post, rim, circles, movie, small passes, plane, ui quads, ui text, shadow casters,
shadow maps, far cards, signs / foam hands, floor crowd, pieces, props, screens); a PATH per class (what draws it when it is ours, the lane /
fate that made it); the thin-draw path (arena / ring / wrestlers: `thin` when the zip's thin census line is clean, `unproven` when it shows
holes or proof mismatches, and a class drawn through thin draws still reads `ours` when its OWN census line says so); a share (ours frames /
ours + fallback frames: "ours" at a low share is mostly the game's); the live J3 channels (out\\live\\j3*). The rule is unchanged: `ours`
only from a clean census line in a zip; a switch on with a missing or mismatching line reads `unproven`; nothing is inferred.
"""
import argparse, glob, json, os, re, subprocess, sys, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return REPO


# The core loop, exactly RENDERER_ARC.md decision 2 and section 5: menus, exhibition matches of every type, entrances and victory
# scenes block the release; create modes, Universe and 30 Years do not block (but must not be clearly wrong).
ITEMS = [
    ('menus', 'Menus', True),
    ('matches', 'Exhibition matches (every type)', True),
    ('entrances', 'Entrances', True),
    ('victory', 'Victory scenes', True),
    ('create', 'Create modes', False),
    ('universe', 'Universe', False),
    ('wm30', '30 Years of WrestleMania', False),
    # lane 252 (S101): the items the arc's core-loop list left out, so "own everything" can be seen. Not release gates.
    ('replays', 'Match replays (post-match highlights)', False),
    ('cutscenes', 'Cutscenes (30 Years / Universe / story scenes)', False),
    ('viewer', 'Arena Viewer (file census, rdr backend)', False),
]
ITEM_NAME = {k: n for k, n, _ in ITEMS}
DEVICES = [('pc', 'PC'), ('phone', 'Phone')]

# Draw classes (lane 156: one per merged promotion). (key, label, census tag, the switch: wwe2k14_rdr_<switch>[_ours]).
# "stream" is what every other draw is.
CLASSES = [
    ('ui', 'UI pictures', 'ui', 'ui'),
    ('ui_text', 'UI text', 'ui_text', 'ui_text'),
    ('ui_quads', 'UI sprites + rects', 'ui_quads', 'ui_quads'),
    ('movie', 'Movie planes', 'movie', 'movie'),
    ('crowd', 'Crowd members', 'crowd', 'crowd'),
    ('far_cards', 'Far crowd cards', 'crowd_cards', 'crowd_cards'),
    ('crowd_props', 'Signs / foam hands', 'crowd_props', 'crowd_props'),
    ('floor_crowd', 'Seated floor crowd', 'floor_crowd', 'crowd_floor'),
    ('arena', 'Arena', 'arena', 'arena'),
    ('ring', 'Ring', 'ring', 'ring'),
    ('wrestlers', 'Wrestlers', 'wrestlers', 'wrestlers'),
    ('caw', 'Editor wrestler (CAW)', 'caw', 'caw'),     # lane 213: the Create a Superstar editor's wrestler is the caw hook's (lane 153), not the wrestlers hook's
    ('pieces', 'Wrestler pieces', 'wrestlers_pieces', 'wrestlers'),
    ('props', 'Props (table, steps, mic)', 'props', 'props'),
    ('screens', 'Stage screens', 'screens', 'screens'),
    ('flash', 'Crowd flashes', 'flash', 'flash'),
    ('particles', 'Particle runs', 'particles', 'particles'),
    ('shadow_casters', 'Shadow casters', 'shadow_casters', 'shadow_casters'),
    ('shadow_maps', 'Shadow maps + passes', 'shadows_ours', 'shadows'),
    ('circles', 'Circle shadows', 'circles', 'circles'),
    ('plane', 'Plane shadow chain', 'plane_shadow', 'plane_shadow'),
    ('rim', 'Rim light map', 'rim_map', 'rim'),
    ('small_passes', 'Sweat + skin pre-light', 'small_passes', 'small_passes'),
    ('post', 'Post chain', 'post', 'post'),
]
CLASS_LABEL = {k: l for k, l, _, _ in CLASSES}
# INFO[key] = (parent class: when its switch is off this class is the stream's, thin switch or None, the path text, lane / fate that made it).
# The census line shape of each class is parse_line(); the lanes are read from git log / the merge messages.
INFO = {
    'ui': (None, None, 'the game\'s quads, pictures bound from our uploads (the stream keeps quad, blend, target)', 'S100, UI_OURS'),
    'ui_text': (None, None, 'text laid out by us, each glyph quad checked against the game\'s', 'UI_TEXT_OURS'),
    'ui_quads': ('ui', None, 'sprites + flat rects drawn by our own quad and 2D program (fate 20)', 'lane 148, fate 20'),
    'movie': (None, None, 'our quad + YUV shader from the planes the decoder wrote (titantron, menu backdrop; fate 17)', 'lane 131, fate 17'),
    'crowd': (None, None, 'members drawn by us from the scene record (the stream\'s member draws dropped)', 'S100'),
    'far_cards': ('crowd', None, 'far audience as cards drawn by us (the stream\'s card draws dropped)', 'lane 110 (CD / C1)'),
    'crowd_props': (None, None, 'signs / foam hands drawn in place from the arena file', 'lane 133 (N4)'),
    'floor_crowd': ('crowd', None, 'seated floor members as ordinary LOD 0 crowd members (draw-side line is fview-only; live logs the record side)', 'lane 82'),
    'arena': (None, 'arena_thin', 'in place from the scene record; thin = the draw is a pinned record', 'S100; thin lane 122'),
    'ring': (None, 'ring_thin', 'in place from the scene record; thin as arena', 'S100, lane 51; thin lane 122'),
    'wrestlers': (None, 'wrestlers_thin', 'in place from the scene record (bodies, attires, palettes); thin as arena', 'S100; thin lane 122'),
    'caw': (None, None, 'the Create a Superstar editor wrestler drawn in place, part by part from the edit pacs (no scene record needed)', 'lane 153 (N3)'),
    'pieces': ('wrestlers', None, 'the referee\'s sleeve / arm piece and alpha pieces, inside the wrestlers hook', 'lanes 48, 134'),
    'props': (None, None, 'commentary table, ring steps, announcer\'s microphone in place from the game files', 'lane 84'),
    'screens': (None, None, 'stage screens\' geometry (ribbon band, wall, mini-tron) in place from the game files', 'lane 115, fate 13'),
    'flash': (None, None, 'the crowd\'s camera flashes drawn in place from the arena file', 'S100'),
    'particles': (None, None, 'runs of particle draws merged into one draw each by our vertex stage', 'S101 particles'),
    'shadow_casters': (None, None, 'our caster geometry in the game\'s shadow passes (buffers swapped; constants and targets stay the stream\'s)', 'lane 99 (SH1), SHADOW_CASTERS_OURS'),
    'shadow_maps': (None, None, 'our shadow passes: maps + plane drawn by us (retargeted draws)', 'lane 123 (SH2), SHADOWS_OURS'),
    'circles': (None, None, 'circle blob shadows from the scene record, constants checked (fate 16)', 'lane 136, fate 16'),
    'plane': (None, None, 'the plane shadow chain\'s constants from the scene record (fate 19)', 'lane 139, fate 19'),
    'rim': (None, None, 'our caster geometry drawn in the rim light map (fate 15)', 'lane 124, fate 15'),
    'small_passes': (None, None, 'sweat map + skin pre-light passes with meshes of our own (fate 18)', 'lane 135, fate 18'),
    'post': (None, None, 'the post chain (DoF, bloom, glare, combine, film) recorded by our own pass (fate 14)', 'lanes 114, 140, fate 14'),
}
PARENT = {k: v[0] for k, v in INFO.items()}
THIN = {k: v[1] for k, v in INFO.items() if v[1]}
SWITCHES = [c[3] for c in CLASSES] + list(THIN.values())
SW_OF = {k: sw for k, _, _, sw in CLASSES}
TAG_OF = {k: tag for k, _, tag, _ in CLASSES}

# Which classes occur in which item. [U] unconfirmed: an assumption from what each scene shows, used only to tell "n/a" from "none".
_UI = {'ui', 'ui_text', 'ui_quads', 'movie'}
_SCENE = {'crowd', 'far_cards', 'crowd_props', 'floor_crowd', 'arena', 'ring', 'wrestlers', 'pieces', 'props', 'screens', 'flash', 'particles',
          'shadow_casters', 'shadow_maps', 'circles', 'plane', 'rim', 'small_passes', 'post'}
_MATCH = _UI | _SCENE
CROWD_FAMILY = {'crowd', 'far_cards', 'crowd_props', 'floor_crowd'}   # lane 259
APPLIES = {'menus': set(_UI), 'create': {'ui', 'ui_text', 'ui_quads', 'caw', 'post', 'shadow_casters', 'rim'}, 'universe': set(_UI), 'matches': set(_MATCH),
           'entrances': set(_MATCH), 'victory': set(_MATCH), 'wm30': set(_MATCH),
           # lane 252: a replay and a cutscene draw the match scene (+ UI); the Arena Viewer is not the game and has no census classes (its own file census: see viewer_board)
           'replays': set(_MATCH), 'cutscenes': set(_MATCH), 'viewer': set()}

# Live-driver channels read besides the run folders: the J3 check's out\live\j3*\cases (no plan.json, the route is the driver's menu.txt, and the other
# live folders hold old builds and cumulative counters under a menu's words). A live zip joins an item only by the manifest's words: "FORCED scene" (the
# 30 Years step the goto forced) = wm30; anything else stays ungrouped. Never from the folder name.
LIVE_CHANNELS = ('j3', 'j3b', 'j3c', 'j3d')

# Route / where.words -> core-loop item. Order matters (first hit). The route name is the run folder's plan.json route file, else the
# run folder name with its date prefix removed.
ITEM_RULES = [
    # lane 252: the new items name their route on purpose (a dedicated route's file name keeps the words). 'replay' alone is NOT a rule: replay_play / dlc_replay /
    # s21_replay_time are the harness's input recordings, not the match replay. 'cutscene' alone is not either (s53_cutscene_hitch is an entrance hitch route).
    ('replays', r'match_?replay|instant_?replay|sweep_replay'),
    ('cutscenes', r'sweep_cutscene|cutscene_play|story_scene'),
    ('viewer', r'arena_?viewer|sweep_viewer'),
    ('victory', r'victory'),
    ('universe', r'universe'),
    ('wm30', r'wm30|30.?years'),
    ('create', r'caw|create|editor'),
    ('entrances', r'entrance|brock|in a match.*(match load|entrances)'),
    ('matches', r'match|in the ring'),
    ('menus', r'menu'),
]

# The gate faults (RENDERER_ARC 5 Phase D + STATUS "Open (carried)"); the full audit is docs\renderer\GATE_FAULTS.md (lane 156) and this table
# repeats its STATE column. `case_words`: a zip whose manifest note or run name matches is that fault's case; `related`: a run that looks at the
# same scene but is not a bug case. A zip is a case only when it carries the fault's words in its note (the bug button's note field).
FAULTS = [
    ('Title-logo glitch', r'\b(title.?logo|logo)\b', r'boot_menu', 'open (GATE_FAULTS row 1: Jarrod saw it on APK 14 and 16)'),
    ('Carry / pin darkening', r'\b(carry|pin|darken(ing)?)\b', r'brock|match', 'open (GATE_FAULTS row 2: reproducible, lane 160 reading it)'),
    ("The Undertaker's blocks", r'\b(undertaker|taker|blocks?)\b', r'undertaker|taker', 'open (GATE_FAULTS row 3: run 7030, ultrawide 2x entrance, post chain)'),
    ('120 on a 60 Hz panel', r'\b(120|60 ?hz)\b', r'', 'open (GATE_FAULTS row 4)'),
    ('Crowd flashes', r'\bcrowd.?flash(es)?\b', r'brock', 'unknown, probably closed (GATE_FAULTS row 5)'),
    ("Bryan's entrance hang", r'\bbryan\b.*\bhang\b|\bhang\b.*\bbryan\b', r'bryan_entrance', 'unknown: not reproduced in 3 runs, cause unconfirmed (GATE_FAULTS row 6)'),
]

BUG_TRIGGERS = {'key', 'chord', 'tap', 'auto_hitch'}   # CASE_ZIP.md: key / chord / tap are the bug button; auto_hitch is the hitch watcher
BUG_BUTTON = {'key', 'chord', 'tap'}

# Census lines (the LAST matching line of a zip is its state: the counters are cumulative). One regex per shape.
RE_GEN = re.compile(r'\[rdr\]\[(\w+)\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(.*?mismatch (\d+)')
RE_NOTOURS = re.compile(r'not ours (\d+)')
RE_NOTREADY = re.compile(r'\(not ready (\d+)')
# lane 252: the same proof as the props' and the wrestlers' for the classes whose OWN detail line says how many objects it joined: arena "objects joined N", ring "ring B: objects joined N",
# screens "objects joined to keys [..]", crowd props "joined entries {..}". 0 / [] / {} = the class found nothing of its kind in the scene (the CAW editor has no arena or ring; a match with no
# stage screens). The line is the class's own window / stop line (it carries "not ready N," after "ours frames N,"), never the census line. Classes with no such count: see CLASS_PROOF.
RE_JOINED_N = re.compile(r'\[rdr\]\[(arena|ring)\] ours frames \d+, not ready \d+,.*?objects joined (\d+) ')
RE_JOINED_SCREENS = re.compile(r'\[rdr\]\[screens\] ours frames \d+, not ready \d+,.*?objects joined to keys \[([^\]]*)\]')
RE_JOINED_CPROPS = re.compile(r'\[rdr\]\[crowd_props\] ours frames \d+, not ready \d+,.*?joined entries \{([^}]*)\}')
# class -> census key of its "joined" proof; the classes NOT here have no joined count in any detail line: crowd (members drawn / stream draws dropped), far_cards (cards drawn),
# floor_crowd (members drawn), flash ("objects N" with decisions N: what an object is, unconfirmed), circles / plane / rim / shadow_casters / caster (claimed N), small_passes (decisions N),
# caw (claimed N), particles (draws N), post (frames), shadow_maps (frames), ui* / movie (decisions): their zero is "the game drew none" = the generic saw-nothing rule, not a proof.
CLASS_PROOF = {'arena': 'arena_joined', 'ring': 'ring_joined', 'screens': 'screens_joined', 'crowd_props': 'crowd_props_joined', 'props': 'props_seen',
               'wrestlers': 'wrestlers_seen', 'pieces': 'wrestlers_seen'}
RE_PROPSJOINED = re.compile(r'\[rdr\]\[props\].*?objects joined \[([^\]]*)\]')   # lane 247: the props hook's own detail line: which prop objects it joined
RE_OBJSEEN = re.compile(r'\[rdr\]\[wrestlers\] wrestlers: objects seen (\d+)')
RE_QUADS = re.compile(r'quads: decisions (\d+), sprites (\d+), rects (\d+)')
RE_CROWD = re.compile(r'\[rdr\]\[crowd\] ours frames (\d+), fallback (\d+), stream draws dropped (\d+), members drawn (\d+)')
RE_CARDS = re.compile(r'\[rdr\]\[crowd\] cards: frames ours (\d+), fallback (\d+), card draws dropped (\d+), cards drawn (\d+), kept (\d+)')
RE_CARDS_WARN = re.compile(r'\[rdr\]\[crowd\] WARN cards: .*?picture mismatches (\d+)')
RE_FLOOR = re.compile(r'floor members drawn (\d+), floor draws kept (\d+) of (\d+) floor stream draws, floor \(class, LOD 0\) checks: (\d+) of (\d+)')
RE_CPROPS = re.compile(r'\[rdr\]\[crowd_props\] ours frames (\d+), not ready (\d+),.*?decisions (\d+), replaced (\d+); mismatch: counts (\d+) vertices (\d+) indices (\d+) layout (\d+) no bytes (\d+)')
RE_PART = re.compile(r'\[rdr\]\[particles\] frames (\d+), draws (\d+), runs (\d+) \(merged draws (\d+)')
RE_POST = re.compile(r'\[rdr\]\[post\] ours frames (\d+), fallback (\d+), shapes')
RE_SHADOWS = re.compile(r'\[rdr\]\[shadows_ours\] frames (\d+) .*?draws retargeted (\d+).*?refused (\d+)')
RE_SHADOW_TARGETS = re.compile(r'\[rdr\]\[shadows_ours\] frames (\d+) .*?images made \d+ \(map (\d+), plane (\d+)\)')   # lane 251: our map / plane-mask images made = the game's shadow passes seen
RE_CIRCLES = re.compile(r'\[rdr\]\[circles\] circles: frames (\d+) \(not ready (\d+)\) claimed (\d+) replaced (\d+) \| kept: unjoined (\d+), mismatch (\d+).*?mesh checked (\d+) bad (\d+)')
RE_PLANE = re.compile(r'\[rdr\]\[plane_shadow\] plane_shadow: frames (\d+) \(not ready (\d+)\) claimed (\d+) \(.*?\) replaced (\d+) \(.*?\) \| kept: unjoined (\d+), mismatch (\d+)')
RE_SMALL = re.compile(r'\[rdr\]\[small_passes\] decisions (\d+), replaced (\d+) \(sweat (\d+), prelight (\d+)\) \| verify (\d+); read failed (\d+); bad: vertices (\d+) indices (\d+) shape (\d+) layout (\d+) clip (\d+) variant (\d+)')
RE_PIECES = re.compile(r'\[rdr\]\[wrestlers\] wrestlers: objects seen.*?verify (\d+) bad (\d+).*?vertex elements compared (\d+): exact (\d+) near (\d+) bad (\d+).*?pieces \(draws unless noted\): replaced (\d+) palette-refused (\d+) no owner (\d+)')
RE_THIN = re.compile(r'\[rdr\]\[thin\] (arena|ring|wrestlers) thin (\d+), proof (\d+) \(ok (\d+)\).*?holes (\d+), proof mismatches (\d+).*?slot missing (\d+)')
RE_UIQ = re.compile(r'\[rdr\]\[ui\] .*?quads: decisions (\d+), sprites (\d+), rects (\d+)')
RE_PAIRWARN = re.compile(r'\[rdr\]\[crowd\] WARN the record does not pair.*?bad frames so far (\d+)')
RE_CVAR = re.compile(r"modified: wwe2k14_rdr_(\w+?)(?:_ours)? = '(\w+)'")
RE_FLAG = re.compile(r'--wwe2k14_rdr_(\w+?)(?:_ours)?=(\w+)')
# lane 252: log markers that say a run REACHED a replay / cutscene (reach evidence only; class cells still come from a dedicated route's zip)
RE_REPLAY_OPEN = re.compile(r"NtCreateFile 'cache:\\Spectacular\.rps' \w+ access=0x([0-9A-Fa-f]+) .*?frame=(\d+)")
RE_BANK_OPEN = re.compile(r"NtCreateFile '[^']*\\(?:SEASON|UNIVERSE|WSD)0\d\.PAC'.*?frame=(\d+)", re.I)
RE_PROBLEM = re.compile(r'\[rdr\]\[PROBLEM\]')
RE_VUID = re.compile(r'VUID-|Validation Error|validation error', re.I)
RE_VALSTART = re.compile(r'\(validation (\d+)')

GENERIC_TAGS = {'ui', 'ui_text', 'movie', 'arena', 'ring', 'wrestlers', 'caw', 'props', 'screens', 'flash', 'shadow_casters', 'rim_map'}


class ZipRead(Exception):
    pass


def _row(frames=0, fallback=0, replaced=0, kept=0, mismatch=0, nofb=False, **extra):
    # nofb: the census line has no fallback count (forced 0), so a share would read 100% for nothing: no share is shown
    d = dict(frames=frames, fallback=fallback, replaced=replaced, kept=kept, mismatch=mismatch, nofb=nofb)
    d.update(extra)
    return d


def parse_line(x, census, thin, warn):
    """One log line -> the census / thin tables (the last line of a shape wins). Returns True when the line was a census line."""
    m = RE_UIQ.search(x)       # lane 156: the quads counters on any [rdr][ui] line (the window line from UiOurs::WindowDetail, or the stop-time report)
    if m:
        census['ui_quads'] = _row(0, 0, int(m.group(2)) + int(m.group(3)), 0, 0, nofb=True)
        if not RE_GEN.search(x):
            return True
    m = RE_GEN.search(x)
    if m and m.group(1) in GENERIC_TAGS:
        mo = RE_NOTOURS.search(x)   # lane 225: "not ours N" = claimed draws the class does not recognise as its own kind (every claim not ours = the class found nothing of its kind in the scene)
        mr = RE_NOTREADY.search(x)
        census[m.group(1)] = _row(int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)), not_ours=int(mo.group(1)) if mo else None, not_ready=int(mr.group(1)) if mr else 0)
        return True
    m = RE_CARDS.search(x)
    if m:
        census['crowd_cards'] = _row(int(m.group(1)), int(m.group(2)), int(m.group(4)), int(m.group(5)), warn.get('cards', 0))
        return True
    m = RE_CARDS_WARN.search(x)
    if m:
        warn['cards'] = warn.get('cards', 0) + int(m.group(1))
        if 'crowd_cards' in census:
            census['crowd_cards']['mismatch'] = warn['cards']
        return True
    m = RE_PAIRWARN.search(x)
    if m:
        warn['crowd'] = max(warn.get('crowd', 0), int(m.group(1)))
        if 'crowd' in census:
            census['crowd']['mismatch'] = warn['crowd']
        return True
    m = RE_FLOOR.search(x)       # the floor text: the window line '[rdr][crowd] floor: ours frames N, floor members drawn ...' (lane 156) or the stop-time Report
    fl = _row(0, 0, int(m.group(1)), int(m.group(2)), max(0, int(m.group(5)) - int(m.group(4))), nofb=True) if m else None
    if fl:
        census['floor_crowd'] = fl
    m = RE_CROWD.search(x)
    if m:
        census['crowd'] = _row(int(m.group(1)), int(m.group(2)), int(m.group(4)), 0, warn.get('crowd', 0))
        return True
    if fl:
        return True
    mj = RE_JOINED_CPROPS.search(x)   # lane 252: this detail line is also the crowd props' census line (returns below)
    if mj:
        census['crowd_props_joined'] = _row(0, 0, 0, 0, 0, nofb=True, seen=0 if not mj.group(1).strip() else 1)
    m = RE_CPROPS.search(x)
    if m:
        census['crowd_props'] = _row(int(m.group(1)), int(m.group(2)), int(m.group(4)), 0, sum(int(m.group(i)) for i in range(5, 10)))
        return True
    m = RE_PART.search(x)
    if m:
        census['particles'] = _row(int(m.group(1)), 0, int(m.group(4)), nofb=True)
        return True
    m = RE_POST.search(x)
    if m:
        census['post'] = _row(int(m.group(1)), int(m.group(2)), int(m.group(1)))   # replaced = frames the post chain was ours
        return True
    m = RE_SHADOWS.search(x)
    if m:
        census['shadows_ours'] = _row(int(m.group(1)), 0, int(m.group(2)), int(m.group(3)), nofb=True)   # kept = "refused" (declined: the stream's draws stay)
        mt = RE_SHADOW_TARGETS.search(x)
        if mt:
            census['shadows_ours']['maps_made'], census['shadows_ours']['plane_made'] = int(mt.group(2)), int(mt.group(3))
        return True
    m = RE_CIRCLES.search(x)
    if m:
        census['circles'] = _row(int(m.group(1)) - int(m.group(2)), int(m.group(2)), int(m.group(4)), int(m.group(5)), int(m.group(6)) + int(m.group(8)))
        return True
    m = RE_PLANE.search(x)
    if m:
        census['plane_shadow'] = _row(int(m.group(1)) - int(m.group(2)), int(m.group(2)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
        return True
    m = RE_SMALL.search(x)
    if m:
        bad = sum(int(m.group(i)) for i in (7, 8, 10, 11))      # vertices, indices, layout, clip: content that differs; shape / variant are refusals
        census['small_passes'] = _row(0, 0, int(m.group(2)), int(m.group(9)) + int(m.group(12)), bad, nofb=True)
        return True
    mj = RE_JOINED_N.search(x)       # lane 252: arena / ring joined counts
    if mj:
        census[mj.group(1) + '_joined'] = _row(0, 0, 0, 0, 0, nofb=True, seen=int(mj.group(2)))
    mj = RE_JOINED_SCREENS.search(x)
    if mj:
        census['screens_joined'] = _row(0, 0, 0, 0, 0, nofb=True, seen=0 if not mj.group(1).strip() else 1)
    mj = RE_JOINED_CPROPS.search(x)
    if mj:
        census['crowd_props_joined'] = _row(0, 0, 0, 0, 0, nofb=True, seen=0 if not mj.group(1).strip() else 1)
    mp = RE_PROPSJOINED.search(x)   # lane 247: props joined none = the class found no table / steps / mic (the positive proof of absence, like the wrestlers' objects seen 0)
    if mp:
        census['props_seen'] = _row(0, 0, 0, 0, 0, nofb=True, seen=0 if not mp.group(1).strip() else 1)
    mo = RE_OBJSEEN.search(x)   # lane 225 (review SF): the wrestlers' own count of wrestler objects it saw: the one positive proof that the class does not occur
    if mo:
        census['wrestlers_seen'] = _row(0, 0, 0, 0, 0, nofb=True, seen=int(mo.group(1)))
    m = RE_PIECES.search(x)
    if m:
        bad = int(m.group(2)) + int(m.group(6))
        census['wrestlers_pieces'] = _row(0, 0, int(m.group(7)), int(m.group(8)), bad, nofb=True)
        return True
    m = RE_THIN.search(x)
    if m:
        thin[m.group(1)] = dict(thin=int(m.group(2)), proof=int(m.group(3)), ok=int(m.group(4)), holes=int(m.group(5)), mismatch=int(m.group(6)), slot_missing=int(m.group(7)))
        return True
    return False


RE_RUNLOG = re.compile(r'->\s+(wwe2k14_\d+)')
RE_LOGTS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)')


def _ts(x):
    m = RE_LOGTS.match(x or '')
    return m.group(1) if m else None


_LOG_INDEX = {}


def _log_index(ld):
    """{log path: (mtime, first timestamp)} of every game log in the folder, read once per process."""
    key = str(ld)
    if key not in _LOG_INDEX:
        idx = {}
        for lp in glob.glob(str(Path(ld) / 'wwe2k14_*.log')):
            try:
                with open(lp, 'rb') as f:
                    head = f.read(400).decode('utf-8', 'replace')
                idx[lp] = (os.path.getmtime(lp), _ts(head.lstrip()))
            except Exception:
                pass
        _LOG_INDEX[key] = idx
    return _LOG_INDEX[key]


def full_log(run_dir, created, zip_log, logs_dir=None):
    r"""Lane 213: the zip's log.txt is only the window around the case (the last ~40-400 lines), so the census lines printed earlier in the run (the
    600-frame UI / small-pass / flash / screen windows) are not in it. The whole game log is outuild\win-amd64-release\logs\wwe2k14_NNNN.log of the main
    checkout. Candidates: the number the run folder's run.log names ("<route> -> wwe2k14_NNNN"), else (a run with no run.log) every log whose first
    timestamp is before the zip's created time and whose mtime is within 5 minutes after it. A candidate is accepted only when the zip's last lines (up to
    4) are all lines of it, so a reused number or a concurrent run never lends the wrong log; none accepted = the zip's own window. Returns (lines, name)."""
    if not created:
        return None, None
    ld = Path(logs_dir) if logs_dir else main_checkout() / 'out' / 'build' / 'win-amd64-release' / 'logs'
    if not ld.exists():
        return None, None
    cands = []
    rl = Path(run_dir) / 'run.log'
    if rl.exists():
        try:
            cands = [ld / (nm + '.log') for nm in RE_RUNLOG.findall(rl.read_text(encoding='utf-8', errors='replace'))]
        except Exception:
            cands = []
    if not cands:
        import datetime as _dt
        try:
            ct = _dt.datetime.strptime(created, '%Y-%m-%d %H:%M:%S').timestamp()
        except Exception:
            return None, None
        cands = [Path(lp) for lp, (mt, first) in sorted(_log_index(ld).items()) if first and first <= created and ct - 5 <= mt <= ct + 300]
    tail = [x for x in (zip_log or []) if x.strip()][-4:]
    for lp in cands:
        if not lp.exists():
            continue
        try:
            lines = lp.read_text(encoding='utf-8', errors='replace').splitlines()
        except Exception:
            continue
        if tail:
            have = set(lines)
            if all(x in have for x in tail):
                return lines, lp.stem
        else:
            first = next((t for t in map(_ts, lines[:50]) if t), None)
            last = next((t for t in map(_ts, reversed(lines[-50:])) if t), None)
            if first and last and first <= created <= last[:-2] + '99':
                return lines, lp.stem
    return None, None


def read_zip(path, live=False, logs_dir=None):
    """One zip -> a dict of everything the board needs. Reads manifest.json and log.txt only. Raises ZipRead when unreadable."""
    path = Path(path)
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            if 'manifest.json' not in names:
                raise ZipRead(f'{path.name}: no manifest.json')
            man = json.loads(z.read('manifest.json').decode('utf-8'))
            log = z.read('log.txt').decode('utf-8', 'replace').splitlines() if 'log.txt' in names else None
    except ZipRead:
        raise
    except Exception as e:
        raise ZipRead(f'{path.name}: unreadable ({e})')
    if man.get('format') != 'wwe2k14-case':
        raise ZipRead(f'{path.name}: manifest format is {man.get("format")!r}')
    return _record(path, man, log, live, logs_dir)


RE_SEQ = re.compile(r'\[wwe2k14\]\[scene\] sequence (\d+) -> (\d+)')
# Scene sequence ids the game logs on every scene change (session_log.h; ids seen in real PC logs). Only ids seen named are used. Universe's id is not
# known (unconfirmed). A match's entrances and victory scenes are the same sequence (9, CGameMainSequence) as the bout, so a plain log cannot split them.
SEQ_FAMILY = {75: 'menus', 3: 'menus', 9: 'matches', 31: 'wm30', 76: 'create', 77: 'create', 78: 'create', 25: 'create', 20: 'create'}


def scene_item(log):
    """The core-loop item a plain log (no manifest) can honestly be placed in, from its scene-change lines, or None. Menus only when the run never left
    the title / menu sequences; matches / wm30 / create when exactly ONE of those is reached; a run that reached more than one is None (its census is one
    block for the whole run and cannot be split between items: unplaced, never guessed)."""
    fams = set()
    for x in (log or []):
        m = RE_SEQ.search(x)
        if m:
            f = SEQ_FAMILY.get(int(m.group(2)))
            if f:
                fams.add(f)
    rest = fams - {'menus'}
    if len(rest) == 1:
        return next(iter(rest))
    if not rest and 'menus' in fams:
        return 'menus'
    return None


def read_log(path):
    """A plain phone game log (pulled by tools\\android\\pull_cases.ps1 into out\\android\\phone_logs) read as a pseudo-zip: device phone, item from its
    scene lines (scene_item). Returns None when the log holds no census line at all (no evidence: the cells stay 'none')."""
    path = Path(path)
    try:
        log = path.read_text(encoding='utf-8', errors='replace').splitlines()
    except Exception as e:
        raise ZipRead(f'{path.name}: unreadable ({e})')
    probe = {}
    if not any(parse_line(x, probe, {}, {}) for x in log):
        return None
    created = next((t for t in map(_ts, log[:50]) if t), '')
    man = {'format': 'wwe2k14-case', 'created': created, 'trigger': 'phone_log', 'note': '', 'build': {'kind': 'phone log'},
           'device': {'platform': 'android'}, 'where': {'words': '', 'match': {}}}
    z = _record(path, man, log, False, None)
    z['item'] = scene_item(log)
    return z


def _record(path, man, log, live, logs_dir):
    path = Path(path)
    run_dir = path.parent
    zip_log = log
    phone = ((man.get('device') or {}).get('platform') or '').lower() == 'android'
    # a phone source never borrows a PC game log (the phone's full log is the pulled phone log itself)
    flog, flog_name = (None, None) if (live or phone) else full_log(run_dir, man.get('created') or '', zip_log, logs_dir)
    if flog is not None:
        log = flog
    plan = None
    pj = run_dir / 'plan.json'
    if pj.exists():
        try:
            plan = json.loads(pj.read_text(encoding='utf-8'))
        except Exception:
            plan = None
    route = None
    if plan and plan.get('route'):
        route = Path(plan['route']).stem
    if live:
        route = ''      # a live-driver channel: its route is the driver's menu.txt, so the item comes from the manifest's words only
    elif not route:
        route = re.sub(r'^\d{8}_\d{6}_', '', run_dir.name)
    # promotion: the zip's own logged cvars first, the run's plan.json flags second; None = not known
    promoted = {}
    for x in (log or []):
        m = RE_CVAR.search(x)
        if m:
            promoted[m.group(1)] = (m.group(2).lower() == 'true')
    explicit = set(promoted)
    if plan:
        for fl in plan.get('flags', []):
            m = RE_FLAG.match(fl)
            if m and m.group(1) not in promoted:
                promoted[m.group(1)] = (m.group(2).lower() == 'true')
        # a plan with the owner on but no switch for a class means the class was left at its default (off)
        explicit = set(promoted)   # lane 213: switches the run SET (a logged cvar or a plan flag); the owner-default fill below is an inference, not a setting
        if any(re.match(r'--wwe2k14_rdr_owner=', fl) for fl in plan.get('flags', [])):
            for sw in SWITCHES:
                promoted.setdefault(sw, False)
    zcensus = {}   # the same, from the zip's own window only (lane 213: what the old board saw; the n/a rule below keeps using it)
    for x in (zip_log or []):
        parse_line(x, zcensus, {}, {})
    census = {}   # census tag -> dict of the last census line
    thin = {}     # arena / ring / wrestlers -> the last thin census line
    warn = {}
    problems = {'rdr_problem': 0, 'validation_errors': 0, 'validation_level': None, 'mismatch': 0}
    for x in (log or []):
        if parse_line(x, census, thin, warn):
            continue
        if RE_PROBLEM.search(x):
            problems['rdr_problem'] += 1
        if RE_VUID.search(x):
            problems['validation_errors'] += 1
        m = RE_VALSTART.search(x)
        if m:
            problems['validation_level'] = int(m.group(1))
    problems['mismatch'] = sum(c['mismatch'] for c in census.values())
    markers = reach_markers(log)
    words = (man.get('where') or {}).get('words') or ''
    dev = ((man.get('device') or {}).get('platform') or '').lower()
    device = 'phone' if dev == 'android' else 'pc' if dev == 'windows' else (dev or 'unknown')
    mt = ((man.get('where') or {}).get('match') or {}).get('type')
    return dict(path=str(path), name=path.name, run=run_dir.name, route=route, words=words, created=man.get('created') or '',
                trigger=man.get('trigger') or '', note=man.get('note') or '', device=device,
                kind=(man.get('build') or {}).get('kind') or '', git=(man.get('build') or {}).get('git') or '',
                has_log=log is not None, log_src=flog_name or 'zip window', zip_lines=len(zip_log or []), log_lines=len(log or []), promoted=promoted, explicit=explicit, markers=markers, census=census, zcensus=zcensus, thin=thin, problems=problems, match_type=mt,
                item=(('wm30' if re.search(r'FORCED scene', words) else None) if live else (item_of(route, words) or (scene_item(log) if phone else None))), live=live)


def reach_markers(log):
    """Lane 252: what a log says about replays and cutscenes.
    replay_write / replay_read = opens of the Spectacular replay clip file (cache:/Spectacular.rps) after guest frame 0 (frame 0 is the cache being made at boot),
    with write access (bit 0x40000000) or read-only: the post-match highlight clip is recorded, then played back [L: seen ~580 frames apart in the victory runs;
    that it is the Spectacular highlights is a reading of the file name, not a trace].
    bank_open = opens of the story scene packs SEASONnn (30 Years) / UNIVERSEnn / WSDnn after frame 0: a bank OPEN (the script's 1002 preload also opens it at the
    bell, docs/systems/wm30.md), so reach, not 'a cutscene played'.
    director_play = a scene the scene director pushed (`!scene play`, line 'play: pushed scene'): played for certain."""
    m = dict(replay_write=0, replay_read=0, bank_open=0, director_play=0)
    for x in (log or []):
        if 'Spectacular' in x:
            r = RE_REPLAY_OPEN.search(x)
            if r and int(r.group(2)) > 0:
                m['replay_write' if int(r.group(1), 16) & 0x40000000 else 'replay_read'] += 1
        elif 'NtCreateFile' in x and '.PAC' in x.upper():
            r = RE_BANK_OPEN.search(x)
            if r and int(r.group(1)) > 0:
                m['bank_open'] += 1
        elif '[scene][director] play: pushed scene' in x:
            m['director_play'] += 1
    return m


def also_items(z):
    """Items a zip REACHED by a log marker besides the one its route names (lane 252). Reach only: its census is the whole run's, so it never fills a class cell of these items."""
    m = z.get('markers') or {}
    a = set()
    if m.get('replay_write') or m.get('replay_read'):
        a.add('replays')
    if m.get('bank_open') or m.get('director_play'):
        a.add('cutscenes')
    a.discard(z.get('item'))
    return a


def item_of(route, words):
    for key, rx in ITEM_RULES:
        if re.search(rx, route or '', re.I):
            return key
    for key, rx in ITEM_RULES:
        if re.search(rx, words or '', re.I):
            return key
    return None


def cls_tag(cls):
    return TAG_OF[cls]


def _share(line):
    if not line or line.get('nofb'):
        return None      # no fallback count in this census line (forced 0): a share would read 100% for nothing
    t = line['frames'] + line['fallback']
    return None if (t == 0 or line['frames'] == 0 and line['fallback'] == 0) else round(100.0 * line['frames'] / t)


def class_info(z, cls):
    """What one zip says about one class: dict(state, share, path) with state ours / mixed / stream / unproven, or None (the zip says nothing).
    mixed = ours, but under half of the window's frames (share < 50): not counted as ours. A census line with mismatches reads unproven even when the
    plan says the switch is off. Thin draws never reach the replace seam, so only the `[rdr][thin]` line speaks for them: a thin switch that is on with
    no thin line is unproven, and a thin line is dirty when holes > 0, proof mismatches > 0, ok < proof or slot missing > 0 (thin_draw.h requires 0)."""
    sw = SW_OF[cls]
    line = z['census'].get(TAG_OF[cls])
    prom = z['promoted'].get(sw)           # True / False / None
    parent = PARENT.get(cls)
    if line is not None and line['mismatch'] > 0:
        return dict(state='unproven', share=_share(line), path='')   # drawing but not matching the stream: not proven, whatever the plan says
    if prom is False or (parent and z['promoted'].get(SW_OF[parent]) is False):
        # the switch (or the class it rides on) is off: the game's draws, even if a stale line exists. Lane 213: off by the owner-default fill (the run never
        # mentioned the switch) is an inference, so weak: an older zip that proved the class still speaks (the plane arm / casters arm order of the sweep)
        said = (prom is False and sw in z['explicit']) or bool(parent and z['promoted'].get(SW_OF[parent]) is False and SW_OF[parent] in z['explicit'])
        return dict(state='stream', share=None, path='', weak=not said)
    if line is None:
        if prom is True:
            return dict(state='unproven', share=None, path='', weak=True)   # promoted, census line missing: never "ours" (weak: says nothing about an older clean zip)
        return None
    if line['replaced'] <= 0:
        # a clean census line that replaced nothing: the game's draws. Lane 213: a line that also saw nothing (no fallback frames, nothing kept) means the
        # class did not draw in this run at all: that is an absence (weak), not a finding, and an older clean 'ours' still speaks.
        # lane 225: every kept draw "not ours" (the hook claimed draws and found none of its own kind: the Create a Superstar editor's wrestlers class, objects seen 0) is an absence too
        # Review SF: an absence is accepted only with POSITIVE proof. The wrestlers / pieces: the class's own count "objects seen 0" (claims all "not ours" prove nothing: that is exactly the
        # shadow casters' bug, a refusal). Every other class: the line saw nothing at all (no fallback frame, nothing kept).
        saw_nothing = line['fallback'] == 0 and line['kept'] == 0
        proof = False
        if cls in CLASS_PROOF:
            # lane 225 (wrestlers / pieces: objects seen 0), lane 247 (props: the Create editor loads the props set but joins none), lane 252 (arena, ring, screens, crowd props: their own
            # joined count is 0 / [] / {}). A joined object with nothing replaced (joined > 0) is NOT a proof: that stays the game's draw.
            seen = z['census'].get(CLASS_PROOF[cls])
            proof = seen is not None and seen.get('seen') == 0 and line['replaced'] <= 0 and line['mismatch'] == 0
            absent = proof
        else:
            absent = saw_nothing
        # lane 252: a proven absence is weak too (it does not override an older zip that proved the class ours); the board shows n/a when EVERY zip of the item that speaks is a proof
        return dict(state='stream', share=None, path='', weak=saw_nothing or proof, absent=absent, proof=proof)
    info = dict(state='ours', share=_share(line), path='')
    ts = THIN.get(cls)
    if ts:
        tl = z['thin'].get(cls)
        thin_on = z['promoted'].get(ts)
        if thin_on is False:
            info['path'] = 'full'
        elif tl is not None:
            if tl['holes'] > 0 or tl['mismatch'] > 0 or tl['ok'] < tl['proof'] or tl['slot_missing'] > 0:
                return dict(state='unproven', share=info['share'], path='thin (proof not clean)')   # the thin path itself is not clean
            info['path'] = 'thin'
        elif thin_on is True:
            return dict(state='unproven', share=info['share'], path='thin (no thin line)', weak=True)          # thin on, nothing speaks for the thin draws
    if info['share'] is not None and info['share'] < 50:
        info['state'] = 'mixed'
    return info


PLANE_ABSENT_MIN_FRAMES = 600   # lane 251: a shadow census window shorter than this proves nothing about a pass that may come later


def plane_pass_absent(z):
    """Lane 251: POSITIVE proof that the plane shadow pass never ran in this zip: the shadow reader's own census made shadow-map images (maps_made > 0, so it saw the game's
    shadow passes) over at least PLANE_ABSENT_MIN_FRAMES frames and made no plane-mask image (plane_made == 0: the plane pass allocates one on its first draw), and the plane
    chain's own line (if any) replaced nothing. A zip without the shadows_ours line, or with the plane chain replacing draws, is never absent."""
    sh = z['census'].get('shadows_ours')
    if not sh or sh.get('plane_made') is None or sh['frames'] < PLANE_ABSENT_MIN_FRAMES or sh.get('maps_made', 0) <= 0 or sh['plane_made'] != 0:
        return False
    pl = z['census'].get('plane_shadow')
    return pl is None or (pl['replaced'] <= 0 and pl['kept'] <= 0 and pl['mismatch'] <= 0)


def class_state(z, cls):
    """The state one zip gives one class: ours / stream / unproven, or None (the zip says nothing about it)."""
    i = class_info(z, cls)
    return i['state'] if i else None


def zip_state(z):
    p = z['problems']
    bad = []
    if z['trigger'] == 'run_failure':
        bad.append('run failure')
    if p['rdr_problem']:
        bad.append(f"{p['rdr_problem']} PROBLEM line(s)")
    if p['validation_errors']:
        bad.append(f"{p['validation_errors']} validation error(s)")
    if p['mismatch']:
        bad.append(f"mismatch {p['mismatch']}")
    return bad


def build(zips):
    """The board: cells[(item, device, class)] = dict(state, zip, share, path), and per item/device summaries."""
    cells = {}
    meta = {}
    for item, _, _ in ITEMS:
        for dev, _ in DEVICES:
            group = sorted([z for z in zips if z['item'] == item and z['device'] == dev], key=lambda z: (z['created'], z['name']), reverse=True)
            meta[(item, dev)] = dict(zips=group, also=sorted([z for z in zips if item in also_items(z) and z['device'] == dev], key=lambda z: (z['created'], z['name']), reverse=True))
            for cls, *_ in CLASSES:
                found = None
                weak = None
                says = []                             # lane 252: every zip that says something (after the drops below), for the all-proof rule
                for z in group:                       # newest first: the first zip with a say wins
                    inf = class_info(z, cls)
                    if inf is not None and cls not in APPLIES[item] and cls_tag(cls) not in z['zcensus']:
                        inf = None                    # a class that cannot occur here says nothing unless its own census line is there
                    if inf is not None and cls in CROWD_FAMILY and cls not in APPLIES[item] and inf['state'] in ('ours', 'mixed'):
                        inf = None                    # lane 259: a menu-filed zip of a run that played a match carries the match's crowd census (counters are whole-run); a scene that draws no audience never reads ours / mixed from it (a mismatch still shows)
                    if inf is not None and cls not in APPLIES[item] and inf['state'] == 'stream' and inf.get('absent'):
                        inf = None                    # lane 225: ... and a line that only shows the class found nothing of its kind here (frames 0 / every claim "not ours") is the class not occurring, not the game's draw
                    if inf is not None and (not inf.get('weak') or inf.get('proof')):
                        says.append(inf)         # a zip that only lacks the line / has the switch off says nothing; a finding (ours, mixed, mismatch, a clean stream) or a proof does
                    if inf is not None and inf.get('weak'):
                        weak = weak or (inf, z)       # lane 213: "switch on, census line absent" (or a line that saw no draw at all) is an absence, not a finding
                        continue
                    if inf is not None:
                        # an older zip that proves the class (ours / mixed) or shows a mismatch speaks past a newer absence; an older 'stream' does not
                        # (switch off back then): the newer zip's switch-on-without-a-line is the better news
                        found = weak if (weak is not None and inf['state'] == 'stream') else (inf, z)
                        break
                if found is None and weak is not None:
                    found = weak
                # n/a (the class never occurred): every zip that says something is a proof, and at least 2 of them (one window may simply be before the scene loaded; lane 251's rule)
                proof_na = bool(found and found[0].get('proof') and len(says) >= 2 and all(i.get('proof') for i in says))
                absent_zips = []
                if cls == 'plane' and (found is None or found[0].get('weak')):
                    # lane 251: a class whose pass the game never ran in ANY zip of this item that speaks about shadows (>= 2 zips, all absent by the shadow reader's own census)
                    # does not occur here: n/a with the evidence named, not "unproven" (the 30 Years plane cell: plane images made 0 in every wm30 zip). A zip that proved or refused it is a finding and wins.
                    sp = [z for z in group if z['census'].get('shadows_ours') and z['census']['shadows_ours'].get('plane_made') is not None]
                    if len(sp) >= 2 and all(plane_pass_absent(z) for z in sp):
                        absent_zips = sp
                if proof_na:
                    cells[(item, dev, cls)] = dict(state='n/a', zip=found[1], share=None, path='', note=f'{CLASS_LABEL[cls]} joined nothing in {len(says)} zip(s) (its own joined count is 0)')
                elif absent_zips:
                    cells[(item, dev, cls)] = dict(state='n/a', zip=absent_zips[0], share=None, path='', note=f'plane pass absent in {len(absent_zips)} zips (shadow census: plane images made 0)')
                elif found:
                    cells[(item, dev, cls)] = dict(state=found[0]['state'], zip=found[1], share=found[0]['share'], path=found[0]['path'])
                elif group and cls in APPLIES[item]:
                    cells[(item, dev, cls)] = dict(state='unproven', zip=group[0], share=None, path='')          # zips exist, none says anything: not proven
                elif cls in APPLIES[item]:
                    cells[(item, dev, cls)] = dict(state='none', zip=None, share=None, path='')
                else:
                    cells[(item, dev, cls)] = dict(state='n/a', zip=None, share=None, path='')
    return cells, meta


def headline(cells):
    counts = {'ours': 0, 'mixed': 0, 'stream': 0, 'unproven': 0, 'none': 0, 'n/a': 0}
    for c in cells.values():
        counts[c['state']] += 1
    counted = sum(v for k, v in counts.items() if k != 'n/a')
    proven = counts['ours'] + counts['stream']
    return counts, counted, proven


def extras(cells):
    """Counts the headline does not carry: ours cells drawn through thin draws, ours cells with a share under 50%."""
    thin = sum(1 for c in cells.values() if c['state'] == 'ours' and c['path'] == 'thin')
    low = sum(1 for c in cells.values() if c['state'] == 'mixed')
    return thin, low


def load_all(runs_dir, live_root=None, logs_dir=None, phone_roots=None, skipped=None):
    zips, errors = [], []
    skipped = skipped if skipped is not None else []
    for p in sorted(glob.glob(str(Path(runs_dir) / '**' / '*.zip'), recursive=True)):
        try:
            zips.append(read_zip(p, logs_dir=logs_dir))
        except ZipRead as e:
            errors.append(str(e))
    for root in (phone_roots or []):
        # the phone column's sources (tools\\android\\pull_cases.ps1): case zips in out\\cases (only those whose manifest says android: a PC zip there stays
        # out of the board) and plain game logs in out\\android\\phone_logs (read_log)
        for p in sorted(glob.glob(str(Path(root) / 'out' / 'cases' / '*.zip'))):
            try:
                z = read_zip(p)
            except ZipRead as e:
                errors.append(str(e))
                continue
            if z['device'] == 'phone':
                zips.append(z)
            else:
                skipped.append(f'{Path(p).name}: platform {z["device"]}, not a phone zip')
        for p in sorted(glob.glob(str(Path(root) / 'out' / 'android' / 'phone_logs' / '*.log'))):
            try:
                z = read_log(p)
            except ZipRead as e:
                errors.append(str(e))
                continue
            if z is None:
                skipped.append(f'{Path(p).name}: no census line (no evidence)')
            else:
                zips.append(z)
    if live_root:
        for p in sorted(q for ch in LIVE_CHANNELS for q in glob.glob(str(Path(live_root) / ch / 'cases' / '*.zip'))):
            try:
                zips.append(read_zip(p, live=True))
            except ZipRead as e:
                errors.append(str(e))
    return zips, errors


def when(z):
    return z['created'][:16] if z else ''


def cell_text(c):
    if c['zip'] is None:
        return c['state']
    t = f"{c['state']} ({when(c['zip'])[5:]})"
    extra = []
    if c['state'] in ('ours', 'mixed') and c['share'] is not None and c['share'] < 100:
        extra.append(f"{c['share']}%")
    if c['path']:
        extra.append(c['path'])
    return t + (' ' + ' '.join(extra) if extra else '')


VIEWER = None   # set by main(): read_viewer_boards() of the main checkout; None = not looked for (selftests)


def read_viewer_boards(root):
    """Lane 252: the Arena Viewer is a separate exe (wwe2k14_assets*.exe), not the game: it writes no case zip and no census line. What measures it is the file
    census (tools/filecensus/census.py -> out/s101/filecensus/<tag>/board.json): every arena and every wrestler x attire loaded from the player's files into TWO viewer
    processes (vk = the viewer's own picture, rdr = our renderer + the scene front-end) and compared. Per row: pass / tile fault / no effect yet / load failure.
    Counts only here. A board counts when its `plant` is empty (planted-fault boards are the census tool's own selftests) and, per kind, when it covers the kind
    (arenas >= 50 rows, wrestlers >= 200 rows: a partial or resumed run is not the whole). The newest such board speaks for each kind. Returns a dict or None."""
    import collections
    best = {}
    for f in glob.glob(str(Path(root) / 'out' / 's101' / 'filecensus' / '*' / 'board.json')):
        try:
            d = json.loads(Path(f).read_text(encoding='utf-8'))
            rows = d.get('rows') or []
            if d.get('plant'):
                continue
            mt = os.path.getmtime(f)
        except Exception:
            continue
        for kind, need in (('arena', 50), ('wrestler', 200)):
            r = [x for x in rows if x.get('kind') == kind]
            if len(r) >= need and (kind not in best or mt > best[kind]['mtime']):
                best[kind] = dict(tag=d.get('tag') or Path(f).parent.name, mtime=mt, n=len(r), counts=dict(collections.Counter(x.get('status') for x in r)),
                                  validation=sum(int(x.get('validation') or 0) for x in r))
    return best or None


def viewer_text(v):
    if not v:
        return None
    import time
    parts = []
    for kind, label in (('arena', 'arenas'), ('wrestler', 'wrestlers x attires')):
        b = v.get(kind)
        if b:
            parts.append(f"{label} {b['n']}: " + ', '.join(f'{n} {k}' for k, n in sorted(b['counts'].items(), key=lambda kv: -kv[1])) +
                         f" (board {b['tag']}, {time.strftime('%Y-%m-%d %H:%M', time.localtime(b['mtime']))}, validation {b['validation']})")
    return '; '.join(parts)


def render(zips, errors, runs_dir):
    cells, meta = build(zips)
    counts, counted, proven = headline(cells)
    thin_n, low_n = extras(cells)
    L = []
    w = L.append
    w('# Coverage board (Phase D, the gate)')
    w('')
    w('Generated by `py -I tools\\board\\coverage.py --write` from the case zips under `out\\s101\\runs` and the live J3 channels `out\\live\\j3*` (names, counts and dates only; nothing from')
    w('the game is copied here). Do not edit by hand: regenerate. Spec: RENDERER_ARC.md section 5 "Phase D: the gate", decision 10. Fault audit: `GATE_FAULTS.md`.')
    w('')
    w(f'**Headline: {len(zips)} case zips read ({sum(1 for z in zips if z["device"]=="pc")} PC, {sum(1 for z in zips if z["device"]=="phone")} phone), '
      f'newest {max((z["created"] for z in zips), default="none")}; validation layers ON in {sum(1 for z in zips if (z["problems"]["validation_level"] or 0) > 0)} zips, OFF in {sum(1 for z in zips if z["problems"]["validation_level"] == 0)}, not in the log window of {sum(1 for z in zips if z["problems"]["validation_level"] is None)}.** Of {counted} cells that apply (item x device x draw class): '
      f'**{proven} proven** ({counts["ours"]} ours, {counts["stream"]} stream), **{counts["mixed"]} mixed**, **{counts["unproven"]} unproven**, **{counts["none"]} no evidence at all**; '
      f'{counts["n/a"]} not applicable. Of the {counts["ours"]} ours cells, {thin_n} are drawn through thin draws and {counts["mixed"]} more are `mixed` (ours on under half of the frames, not counted as ours).')
    w('')
    w('How to read a cell: `ours` = the newest zip with a say has the class\'s census line with replaced > 0 and mismatch 0. `stream` = the class\'s')
    w('switch is off in that run (the game\'s own draws), or its census line shows nothing replaced. `unproven` = the switch is on (or the census')
    w('line shows a mismatch) but the clean census line is not in the zip, or zips exist and none says anything about the class: never "ours".')
    w('`none` = no zip of that item on that device. `n/a` = the class does not occur there ([U] an assumption in the tool, not evidence).')
    w('')
    w('**Phone column (lane 240).** Filled from what the phone already produces, nothing new on the device: `pwsh -NoProfile -File tools\\android\\pull_cases.ps1 -Board` '
      '(or the pull, then `py -I tools\\board\\coverage.py --write`). Sources: (1) case zips in `out\\cases` whose manifest platform is android (the bug button\'s zips; a PC zip there is skipped, never read as phone); '
      '(2) the newest game logs of the phone (`<files>\\logs\\wwe2k14_NNN.log`, `-Logs N`, default 3) in `out\\android\\phone_logs`, read as pseudo-zips: device phone, evidence = the census lines, '
      'item from the `[scene] sequence` lines (one family only: menus / matches / wm30 / create; a run that reached two families is unplaced; entrances and victory are the same sequence as the bout and Universe\'s id is unconfirmed, so those items stay `none` from logs and need a bug-button zip). '
      'A log with no census line adds nothing. A normal quit writes the end-of-run census block on Android too (OnClosing runs RunQuitFlushes, lane 216) but no zip, so quit the game before pulling.')
    w('')
    w('The newest zip with a say about a class wins; a newer zip that says nothing does not hide an older one, and (lane 213) neither does a newer zip whose census line is merely absent (switch on, no line) or saw no draw of the class at all: an older zip that proved the class (or showed a mismatch) still speaks; with none, the absence reads `unproven` (switch on) or `stream` (nothing drawn).')
    w('`mixed` = ours, but on under half of the census window\'s frames (share < 50): shown, never counted as ours. The share is meaningless where the census line has no fallback count (particles, shadow maps, floor crowd, small passes, pieces, UI sprites + rects: forced 0), and is then not shown.')
    w('After `ours`: the **share** (ours frames / ours + fallback frames of the census window, shown when under 100%) and the **path**: `thin` = the class')
    w('is drawn through thin draws (its thin census line is in the zip, holes 0, proof mismatches 0); `thin (no thin line)` = the thin switch is on but the')
    w('zip\'s window holds no thin line (the class stays ours on its own census line); `thin (proof not clean)` = holes or proof mismatches: the cell is')
    w('`unproven`; `full` = the thin switch was off. A class whose census says it is ours stays ours in a thin capture: the thin line only names the path.')
    w('')
    if errors:
        w(f'**Zips that could not be read ({len(errors)}):** ' + '; '.join(errors))
        w('')
    # per item
    w('## Core-loop items')
    w('')
    w('| Item | Gates the release | Device | Zips | Newest | State |')
    w('|---|---|---|---|---|---|')
    for item, name, blocks in ITEMS:
        for dev, dl in DEVICES:
            g = meta[(item, dev)]['zips']
            if item == 'viewer':
                vt = viewer_text(VIEWER) if dev == 'pc' else None
                if vt:
                    nb = sum(1 for k in ('arena', 'wrestler') if VIEWER.get(k))
                    w(f'| {name} | no | {dl} | {nb} census board(s) | {VIEWER[next(k for k in ("arena", "wrestler") if VIEWER.get(k))]["tag"]} | {vt} (not case zips: see the section "Items beyond the core loop" below) |')
                else:
                    w(f'| {name} | no | {dl} | 0 | - | **no evidence: next check** ' + ('(no complete census board under out/s101/filecensus)' if dev == 'pc' else '(the phone viewer, libviewer.so, writes no census board: not measurable yet)') + ' |')
                continue
            if not g:
                al = meta[(item, dev)]['also']
                if al:
                    mk = {k: sum(x['markers'][k] for x in al) for k in al[0]['markers']}
                    w(f'| {name} | no | {dl} | 0 | - | **no dedicated-route zip: class cells have no evidence.** Reached by log marker in {len(al)} other zip(s), newest {when(al[0])} ({al[0]["route"]}); markers {mk} |')
                else:
                    w(f'| {name} | {"yes" if blocks else "no"} | {dl} | 0 | - | **no evidence: next check** |')
                continue
            z = g[0]
            bad = zip_state(z)
            vl = z['problems']['validation_level']
            vs = 'validation level not in the log window' if vl is None else ('validation OFF (level 0)' if vl == 0 else f'validation ON (level {vl})')
            st = ('ran, no problem in the log read (the game log of the run when found, else the zip window)' if not bad else 'ran, problems: ' + ', '.join(bad)) + f'; {vs}'
            types = sorted({str(x['match_type']) for x in g if x['match_type'] is not None})
            extra = f'; match types seen: {", ".join(types)}' if item == 'matches' and types else ''
            al = meta[(item, dev)]['also']
            if al:
                extra += f'; also reached by log marker in {len(al)} other zip(s)'
            w(f'| {name} | {"yes" if blocks else "no"} | {dl} | {len(g)} | {when(z)} ({z["route"]}) | {st}{extra} |')
    w('')
    # per device: classes as rows, items as columns
    for dev, dl in DEVICES:
        w(f'## Which path draws what: {dl}')
        w('')
        w('| Class | ' + ' | '.join(n for _, n, _ in ITEMS) + ' |')
        w('|---|' + '---|' * len(ITEMS))
        for cls, label, _, _ in CLASSES:
            w(f'| {label} | ' + ' | '.join(cell_text(cells[(item, dev, cls)]) for item, _, _ in ITEMS) + ' |')
        w('')
    na = [(item, dev, cls, c) for (item, dev, cls), c in cells.items() if c['state'] == 'n/a' and c.get('note')]
    w('## Cells that read n/a by proof (the class was looked for and is not there)')
    w('')
    w('A class whose OWN detail line proves it joined nothing (arena / ring "objects joined 0", screens "joined to keys []", crowd props "joined entries {}", props "objects joined []", wrestlers "objects seen 0") '
      'is ABSENT in that zip: it does not override an older zip that proved the class ours, and the cell reads n/a only when every zip of the item that says anything is such a proof (at least 2). '
      'A joined object with nothing replaced is not a proof (it stays the game\'s draw). Classes with no joined count in their lines (crowd, far cards, floor crowd, flashes, circles, plane, rim, shadow casters, small passes, CAW, particles, post, shadow maps, ui*, movie) '
      'have no such proof; their zero is only "the game drew none" (the lane 213 rule).')
    w('')
    for item, dev, cls, c in sorted(na, key=lambda t: (t[0], t[1], t[2])):
        w(f'- {ITEM_NAME[item]} / {dict(DEVICES)[dev]} / {CLASS_LABEL[cls]}: {c["note"]}')
    if not na:
        w('- none')
    w('')
    # path per item
    w('## Per item: ours / the game\'s / not proven (PC)')
    w('')
    w('| Item | Ours | Mixed (ours on <50% of frames) | Stream (the game\'s draws) | Unproven | No evidence |')
    w('|---|---|---|---|---|---|')
    for item, name, _ in ITEMS:
        by = {'ours': [], 'mixed': [], 'stream': [], 'unproven': [], 'none': []}
        for cls, label, _, _ in CLASSES:
            c = cells[(item, 'pc', cls)]
            if c['state'] in by:
                by[c['state']].append(label + (' [thin]' if c['path'] == 'thin' else ''))
        w(f'| {name} | ' + ' | '.join(', '.join(by[k]) or '-' for k in ('ours', 'mixed', 'stream', 'unproven', 'none')) + ' |')
    w('')
    # paths legend
    w('## What each class\'s path is, and what made it')
    w('')
    w('| Class | Census tag | Switch (`--wwe2k14_rdr_<name>`) | Rides on | Path when ours | Lane / fate |')
    w('|---|---|---|---|---|---|')
    for cls, label, tag, sw in CLASSES:
        sws = sw + ('_ours' if sw not in ('crowd_cards', 'crowd_floor') else '') + (f' (+ {THIN[cls]})' if cls in THIN else '')
        par = CLASS_LABEL[PARENT[cls]] if PARENT.get(cls) else '-'
        w(f'| {label} | `{tag}` | `{sws}` | {par} | {INFO[cls][2]} | {INFO[cls][3]} |')
    w('')
    # evidence per zip
    w('## Evidence: the newest zip behind each item (PC)')
    w('')
    w('| Item | Newest zip (run folder) | Trigger | Route | Where (manifest words) | Switches on | Census lines (replaced / mismatch) |')
    w('|---|---|---|---|---|---|---|')
    for item, name, _ in ITEMS:
        g = meta[(item, 'pc')]['zips']
        if item == 'viewer':
            w(f'| {name} | file census board (not a case zip) | - | - | - | - | {viewer_text(VIEWER) or "none"} |')
            continue
        if not g:
            w(f'| {name} | none | | | | | |')
            continue
        z = next((x for x in g if x['census']), g[0])      # the newest zip that has census lines (a live window can hold none)
        on = ', '.join(sorted(k for k, v in z['promoted'].items() if v and k in SWITCHES)) or ('none' if z['promoted'] else 'unknown')
        cen = ', '.join(f"{k} {v['replaced']}/{v['mismatch']}" for k, v in sorted(z['census'].items())) or 'none'
        w(f'| {name} | {z["run"]} | {z["trigger"]} | {z["route"]} | {z["words"] or "-"} | {on} | {cen} |')
    w('')
    # bug-button cases
    bug = [z for z in zips if z['trigger'] in BUG_BUTTON]
    hitch = [z for z in zips if z['trigger'] == 'auto_hitch']
    w('## Bug-button cases')
    w('')
    w(f'{len(bug)} zip(s) with trigger key / chord / tap (the bug button), {len(hitch)} auto_hitch; PC {sum(1 for z in bug if z["device"]=="pc")}, '
      f'phone {sum(1 for z in bug if z["device"]=="phone")}.')
    if bug:
        w('')
        w('| Created | Device | Trigger | Run folder | Note |')
        w('|---|---|---|---|---|')
        for z in sorted(bug, key=lambda z: z['created'], reverse=True):
            w(f'| {z["created"]} | {z["device"]} | {z["trigger"]} | {z["run"]} | {z["note"] or "-"} |')
    w('')
    # faults
    w('## Gate faults (RENDERER_ARC Phase D; the full audit is GATE_FAULTS.md)')
    w('')
    w('| Fault | State (GATE_FAULTS) | Case zip (note matches) | Related run (not a bug case) |')
    w('|---|---|---|---|')
    for name, case_rx, rel_rx, fstate in FAULTS:
        cases = [z for z in zips if re.search(case_rx, z['note'], re.I)]   # the bug button's note only, with word boundaries
        rel = [z for z in zips if rel_rx and re.search(rel_rx, z['run'], re.I)]
        cs = ', '.join(sorted({z['name'] for z in cases})[:3]) if cases else 'none yet'
        rs = (f'{len(rel)} run(s), newest {when(sorted(rel, key=lambda z: z["created"])[-1])} {sorted(rel, key=lambda z: z["created"])[-1]["run"]}'
              if rel else 'none')
        w(f'| {name} | {fstate} | {cs} | {rs} |')
    w('')
    # next checks
    w('## Next checks (items with no evidence at all, then unproven cells)')
    w('')
    none_items = [(name, dl) for item, name, _ in ITEMS for dev, dl in DEVICES if not meta[(item, dev)]['zips'] and not (item == 'viewer' and dev == 'pc' and VIEWER)]
    if none_items:
        for name, dl in none_items:
            w(f'- **{name} on {dl}: no case zip at all.**')
    else:
        w('- Every item has a zip on both devices.')
    uns = [(item, dev, cls) for (item, dev, cls), c in cells.items() if c['state'] == 'unproven']
    if uns:
        w('')
        w('Unproven cells (a zip exists, the clean census line for the class is not in it), PC then phone:')
        by_item = {}
        for item, dev, cls in uns:
            by_item.setdefault((item, dev), []).append(CLASS_LABEL[cls])
        for (item, dev), labs in by_item.items():
            z = cells[(item, dev, next(c for c, l in CLASS_LABEL.items() if l == labs[0]))]['zip']
            w(f'- {ITEM_NAME[item]} / {dict(DEVICES)[dev]} (newest zip {z["name"]}, {when(z)}): ' + ', '.join(labs))
    nones = {}
    for (item, dev, cls), c in cells.items():
        if c['state'] == 'none':
            nones.setdefault((item, dev), []).append(CLASS_LABEL[cls])
    if nones:
        w('')
        w('No-evidence cells (the item has no zip on the device): ' + '; '.join(f'{ITEM_NAME[i]} / {dict(DEVICES)[d]} ({len(v)})' for (i, d), v in nones.items()) + '.')
    ungrouped = [z for z in zips if z['item'] is None]
    w('')
    w(f'Zips the tool could not assign to a core-loop item ({len(ungrouped)}; routes: {", ".join(sorted({z["route"] for z in ungrouped})) or "none"}): '
      'they count in the totals above but in no cell.')
    w('')
    w('## Items beyond the core loop (lane 252): match replays, cutscenes, the Arena Viewer')
    w('')
    w('Added so "own everything" can be seen. None of the three gates the release. How a run is assigned to each:')
    w('')
    w('- **Match replays.** The post-match highlight replay (the Spectacular clip). A zip is the item\'s own when its route name has `match_replay` / `instant_replay` / `sweep_replay` '
      '(the route `scenes\\s101_sweep_match_replay.txt`); never from a bare `replay`: `replay_play`, `dlc_replay`, `s21_replay_time` are the harness\'s input recordings. '
      'Any zip also REACHES it when its game log opens `cache:\\Spectacular.rps` after guest frame 0, with write access (the clip is recorded) or read-only (it is played back): '
      'the victory runs show the two opens ~580 frames apart [L: the file name says Spectacular, the playing is not traced]. The in-match INSTANT REPLAY (a pause-menu row or button) '
      'has no marker and no route: [U] whether it exists on this build\'s pause list (the 8 rows are logged by `s101_sweep_match_pause`, the text was not kept).')
    w('- **Cutscenes.** The scene banks that are not entrances or victory scenes: `SEASONnn` (30 Years story scenes), `UNIVERSEnn`, `WSDnn` (Create a Story; 4025 is the standalone default) '
      '(docs/formats/evt_scene_ids.md). Own zip: route name has `sweep_cutscene` / `cutscene_play` / `story_scene` (`scenes\\s101_sweep_cutscene_wm30.txt`). Reached by marker: a game-log open of '
      'a `SEASON0n` / `UNIVERSE0n` / `WSD0n` pack after frame 0 (a bank OPEN: the script\'s preload opens it at the bell, so this is reach, not "a cutscene played") or the scene director\'s '
      '`[scene][director] play: pushed scene` line (played for certain). No Universe cutscene route: the Universe first page is unconfirmed (`s101_sweep_universe` only looks).')
    w('- **Reach is not coverage.** A census is one block for the whole run and cannot be split by scene, so a zip that only REACHED an item by marker fills no class cell of it '
      '(the cells would read the match before the replay): only a dedicated route\'s zip does, and even then the pre-scene part of the run is in its counters (the same rule as the Victory row).')
    w('- **The Arena Viewer** is a separate exe, not the game: no case zip, no census line. Its row reads the file census instead (`tools/filecensus/census.py` -> `out/s101/filecensus/<tag>/board.json`): '
      'every arena and wrestler x attire loaded into the viewer\'s own backend (vk) and into our renderer (rdr) and compared; the cell counts are pass / tile fault / no effect yet / load failure '
      'for the newest complete clean board per kind (arenas >= 50 rows, wrestlers >= 200, `plant` empty). Its class cells are `n/a` (the viewer has no game census classes). '
      'The phone viewer (`libviewer.so`, ViewerActivity) writes nothing the board can read: not measurable until it does. The viewer\'s own `Mods are additive` toggles are not covered.')
    w('')
    return '\n'.join(L)


def selftest_213(st):
    """Lane 213 (S101): the past-bug re-find for the two tool faults the sweep exposed. Each plant shows the OLD rule's cell (the zip's own log window alone;
    a newer zip's absent census line counting as "unproven") and the fixed one. Planted logs only; nothing from the game."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'213 {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    def ln(sec, msg):
        return f'[2026-11-13 06:{sec // 60:02d}:{sec % 60:02d}.000] [info] [core] [t1] {msg}'

    UI = '[rdr][ui_text] ours frames 600, fallback 0, replaced 5000, kept 10 (not ready 0, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 10), slots ignored 0, ms 9.00'
    CVAR = "[wwe2k14] modified: wwe2k14_rdr_ui_text_ours = 'true' (source: command line)"
    TAIL = ['[wwe2k14][input] line 9: quit', '[rdr][live] picture: x.png', '[wwe2k14][case] wrote ZIP', 'Title terminated; hard-exiting process.']
    with tempfile.TemporaryDirectory(prefix='l213_board_') as t:
        t = Path(t)
        logs = t / 'logs'
        logs.mkdir()
        runs = t / 'runs'
        full = [ln(0, CVAR), ln(10, UI)] + [ln(20 + i, x) for i, x in enumerate(TAIL)]
        (logs / 'wwe2k14_9001.log').write_text('\n'.join(full), encoding='utf-8')
        # a concurrent run's log with the same time span but other lines: never lent to the zip
        (logs / 'wwe2k14_9002.log').write_text('\n'.join([ln(0, 'other run')] + [ln(20 + i, 'other ' + x) for i, x in enumerate(TAIL)]), encoding='utf-8')
        zwin = [ln(20 + i, x) for i, x in enumerate(TAIL)]          # the zip's window: the tail only, no census line, no cvar
        zp = st.make(str(runs), '20261113_062229_s101_sweep_match_pause', 'c1', '2026-11-13 06:00:24', log=zwin, flags=None)
        (zp.parent / 'run.log').write_text('s101_sweep_match_pause.txt -> wwe2k14_9001  EXITED after 24.0s PASS\n', encoding='utf-8')
        z_old = cov_read(zp, logs_dir=t / 'nologs')
        z_new = cov_read(zp, logs_dir=logs)
        check('old rule (zip window only): the census line is not seen', 'ui_text' in z_old['census'], False)
        check('fixed: the run.log-named game log gives the census line', (z_new['log_src'], 'ui_text' in z_new['census']), ('wwe2k14_9001', True))
        check('fixed: the logged cvar (switch on) is read from the full log', z_new['promoted'].get('ui_text'), True)
        check('fixed cell is ours (the old window alone, switch known, line absent, would be unproven)', class_info(z_new, 'ui_text')['state'], 'ours')
        # no run.log: found by time and by the zip's last lines, and the concurrent log is refused
        (zp.parent / 'run.log').unlink()
        import datetime as _dt
        made = _dt.datetime(2026, 11, 13, 6, 0, 26).timestamp()     # the log files were last written just after the zip was created
        for lp in logs.glob('*.log'):
            os.utime(lp, (made, made))
        z_t = cov_read(zp, logs_dir=logs)
        check('no run.log: the log is found by time + tail lines (the concurrent run is refused)', z_t['log_src'], 'wwe2k14_9001')
        (logs / 'wwe2k14_9001.log').write_text('\n'.join([ln(0, 'rewritten')]), encoding='utf-8')
        os.utime(logs / 'wwe2k14_9001.log', (made, made))
        check('a log that does not hold the zip tail is never used (falls back to the zip window)', cov_read(zp, logs_dir=logs)['log_src'], 'zip window')
    # (c) a newer zip whose census line is absent must not hide an older clean one
    OURS = '[rdr][arena] ours frames 1782, fallback 18, replaced 345673, kept 581 (not ready 558, unjoined 0, mismatch 0, variant 0), slots ignored 0, ms 479.98'
    with tempfile.TemporaryDirectory(prefix='l213_board_') as t:
        sw = ['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_arena_ours=true']
        st.make(t, '20261113_050000_s101_roster_a_match', 'old', '2026-11-13 05:00:00', log=[OURS], flags=sw)
        st.make(t, '20261113_060000_s101_roster_fatal4_match', 'new', '2026-11-13 06:00:00', log=['[info] a long run, census window absent'], flags=sw)
        zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
        newest = max(zips, key=lambda z: z['created'])
        check('plant: the newest zip alone reads unproven (the old rule showed this cell)', class_info(newest, 'arena')['state'], 'unproven')
        check('fixed: the older clean zip still speaks for the cell', build(zips)[0][('matches', 'pc', 'arena')]['state'], 'ours')
        st.make(t, '20261113_070000_s101_roster_x_match', 'bad', '2026-11-13 07:00:00', log=[OURS.replace('mismatch 0', 'mismatch 9')], flags=sw)
        zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
        check('a newer MISMATCH still wins over an older clean zip (only absence is weak)', build(zips)[0][('matches', 'pc', 'arena')]['state'], 'unproven')
    with tempfile.TemporaryDirectory(prefix='l213_board_') as t:
        st.make(t, '20261113_060000_s101_roster_fatal4_match', 'only', '2026-11-13 06:00:00', log=['[info] nothing'], flags=['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_arena_ours=true'])
        zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
        check('only a zip with the line absent: still unproven, never ours', build(zips)[0][('matches', 'pc', 'arena')]['state'], 'unproven')
    print(f'{"FAIL" if fails else "PASS"}  lane 213 selftest: {len(fails)} failed')
    return 1 if fails else 0


def selftest_216(st):
    """Lane 216 (S101): the end-of-run census flush. A scene shorter than one 600-frame window never printed its window line, so the class read unproven.
    The game now prints each class's final census line (same shape as a window line, after a '[rdr][end] end of run' header) before the hard exit and
    before the exit zip reads the log. Planted: a zip whose log holds ONLY the header + the end line proves the class; the same zip without it stays unproven
    (the past-bug re-find: the 30 Years plane arm / the victory scene). Planted logs only; nothing from the game."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'216 {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    HDR = '[rdr][end] end of run (route quit / title termination): the lines below are each class\'s final census, same shape as a window line'
    PLANE = '[rdr][plane_shadow] plane_shadow: frames 240 (not ready 0) claimed 480 (cls 1) replaced 480 (draws) | kept: unjoined 0, mismatch 0'
    check('the header line is not a census line', parse_line(HDR, {}, {}, {}), False)
    c = {}
    check('the end line is read as a census line', (parse_line(PLANE, c, {}, {}), 'plane_shadow' in c), (True, True))
    sw = ['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_plane_shadow_ours=true']
    with tempfile.TemporaryDirectory(prefix='l216_board_') as t:
        st.make(t, '20261113_060000_s101_roster_a_match', 'z', '2026-11-13 06:00:00', log=['[info] 20 s of match', HDR, PLANE, 'Title terminated; hard-exiting process.'], flags=sw)
        zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
        check('plant: a log with ONLY the end line proves the class', class_info(zips[0], 'plane')['state'], 'ours')
    with tempfile.TemporaryDirectory(prefix='l216_board_') as t:
        st.make(t, '20261113_060000_s101_roster_a_match', 'z', '2026-11-13 06:00:00', log=['[info] 20 s of match', HDR, 'Title terminated; hard-exiting process.'], flags=sw)
        zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
        check('re-find: the same log without the class line stays unproven (the old cell)', class_info(zips[0], 'plane')['state'], 'unproven')
    print(f'{"FAIL" if fails else "PASS"}  lane 216 selftest: {len(fails)} failed')
    return 1 if fails else 0


def selftest_251(st):
    """Lane 251 (S101): the plane shadow chain in 30 Years. The game's plane pass never runs there (the shadow reader's census: maps made 8, plane images made 0 in every wm30
    zip), so the plane cell read `unproven` from a zip whose switch was on and whose census line was absent. Positive absence (>= 2 zips of the item, each with the shadow
    census over >= 600 frames, maps made > 0, plane made 0, and no plane draws) reads n/a. Plants (each must NOT move the cell): one zip only; a zip with plane images made > 0;
    a window under 600 frames; a zip where the plane chain did replace draws (ours wins). The cell never turns n/a on a mismatch. Planted logs only."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'251 {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    def sh(frames=1371, maps=8, plane=0):
        return f'[rdr][shadows_ours] frames {frames} (unnamed 362), maps 512 px, plane 100%, images made {maps + plane} (map {maps}, plane {plane}), draws retargeted 1, clears 1, copies whole 1 (partial made whole 1, duplicates dropped 1), readers 1, refused 0, ms 1.00'
    PLANE = '[rdr][plane_shadow] plane_shadow: frames 240 (not ready 0) claimed 480 (cls 1) replaced 480 (draws) | kept: unjoined 0, mismatch 0'
    sw = ['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_plane_shadow_ours=true']
    c = {}
    parse_line(sh(), c, {}, {})
    check('the shadow line carries maps / plane images made', (c['shadows_ours']['maps_made'], c['shadows_ours']['plane_made']), (8, 0))

    def cell(logs):
        with tempfile.TemporaryDirectory(prefix='l251_board_') as t:
            for i, lg in enumerate(logs):
                st.make(t, f'2026111306{i:02d}00_s101_sweep_wm30_long', f'z{i}', f'2026-11-13 06:{i:02d}:00', log=lg, flags=sw)
            zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
            return build(zips)[0][('wm30', 'pc', 'plane')]['state']
    check('two zips, plane images made 0, line absent: n/a (the evidence)', cell([[sh()], [sh(4859)]]), 'n/a')
    check('plant: one zip only stays unproven', cell([[sh()]]), 'unproven')
    check('plant: plane images made > 0 in one zip stays unproven', cell([[sh()], [sh(plane=3)]]), 'unproven')
    check('plant: a short window (300 frames) proves nothing', cell([[sh()], [sh(300)]]), 'unproven')
    check('plant: the plane chain replaced draws in one zip: ours wins', cell([[sh()], [sh(plane=3), PLANE]]), 'ours')
    check('plant: a plane mismatch line is never absent', cell([[sh()], [sh(), PLANE.replace('mismatch 0', 'mismatch 100')]]), 'unproven')
    print(f'{"FAIL" if fails else "PASS"}  lane 251 selftest: {len(fails)} failed')
    return 1 if fails else 0


def selftest_225(st):
    """Lane 225 (S101): a class that found nothing of its kind in an item where it is assumed not to occur reads n/a, not "stream". The past-bug re-find: K5w's Create cells
    (Wrestlers, Wrestler pieces read 'stream' from lines whose every claim was "not ours", objects seen 0) and SW01's Universe shadow maps ('stream' from a line with frames 0).
    Both classes keep the game's reading where they DO occur (a match), and the editor's shadow casters / rim map (APPLIES since lane 225) keep 'stream'. Planted logs only."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'225 {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    WR = '[rdr][wrestlers] ours frames 2035, fallback 365, replaced 0, kept 17943 (not ready 730, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 17213), slots ignored 0, ms 20.86'
    SC = '[rdr][shadow_casters] ours frames 600, fallback 0, replaced 0, kept 13800 (not ready 0, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 13800), slots ignored 0, ms 4.69'
    SM = '[rdr][shadows_ours] frames 0 (unnamed 32), maps 512 px, plane 100%, images made 0 (map 0, plane 0), draws retargeted 0, clears 0, copies whole 0 (partial made whole 0, duplicates dropped 0), readers 0, refused 0, ms 2.78'
    WS = '[rdr][wrestlers] wrestlers: objects seen 0 joined 0 (ambiguous 0, unjoined 0), scale measured 0, claimed 17213 replaced 0 | not wrestler 17213'
    WS7 = '[rdr][wrestlers] wrestlers: objects seen 2 joined 0 (ambiguous 0, unjoined 2), scale measured 0, claimed 17213 replaced 0 | not wrestler 17213'
    sw = ['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_wrestlers_ours=true', '--wwe2k14_rdr_shadow_casters_ours=true', '--wwe2k14_rdr_shadows_ours=true']

    def cell(route, item, cls, lines):
        with tempfile.TemporaryDirectory(prefix='l225_board_') as t:
            st.make(t, '20261113_060000_' + route, 'z', '2026-11-13 06:00:00', log=['[info] 20 s'] + lines, flags=sw)
            zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
            return build(zips)[0][(item, 'pc', cls)]['state']

    check('create: wrestlers line + objects seen 0: n/a (the editor body is the caw class)', cell('s101_checkB_caw_walk', 'create', 'wrestlers', [WR, WS]), 'n/a')
    check('create: every claim not ours but objects seen 2 (e.g. Superstar Threads): stays the game\'s, never n/a', cell('s101_checkB_caw_walk', 'create', 'wrestlers', [WR, WS7]), 'stream')
    check('create: every claim not ours with no objects-seen proof (the shadow casters\' bug shape): stays the game\'s', cell('s101_checkB_caw_walk', 'create', 'wrestlers', [WR]), 'stream')
    check('create: shadow casters stay the game\'s until a run proves them ours (APPLIES since lane 225)', cell('s101_checkB_caw_walk', 'create', 'shadow_casters', [SC]), 'stream')
    check('universe: shadow maps line with frames 0: n/a (no shadow pass drawn)', cell('s101_sweep_universe', 'universe', 'shadow_maps', [SM]), 'n/a')
    check('re-find: the same wrestlers line in a match stays the game\'s (the class occurs there)', cell('s101_sweep_match_pause', 'matches', 'wrestlers', [WR]), 'stream')
    print(f'{"FAIL" if fails else "PASS"}  lane 225 selftest: {len(fails)} failed')
    return 1 if fails else 0


def selftest_247(st):
    """Lane 247 (S101): the Create editor's props cell. The props hook loads its set in the editor (the CAS arena looks like a match) but joins no object; the draws it counts as kept
    are the editor wrestler's loading window. Past-bug re-find: the 11-13 13:29 checkB_caw_walk lines (props kept 349, objects joined []) read 'stream'. Planted logs only."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'247 {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    PR = '[rdr][props] ours frames 14, fallback 163, replaced 0, kept 349 (not ready 326, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 23), slots ignored 0, ms 0.24, persist-only 0'
    PD = '[rdr][props] [rdr][props] ours frames 14, not ready 163, upload 0.0 ms, prepared 0; props: objects joined [] (by content 0, texture tie 0, tie left open 0), not a prop of ours 23, unjoined 0'
    PD1 = PD.replace('objects joined []', 'objects joined [305]')
    sw = ['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_props_ours=true']

    def cell(route, item, lines):
        with tempfile.TemporaryDirectory(prefix='l247_board_') as t:
            st.make(t, '20261113_130000_' + route, 'z', '2026-11-13 13:00:00', log=['[info] 20 s'] + lines, flags=sw)
            zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
            return build(zips)[0][(item, 'pc', 'props')]['state']

    check('create: props kept 349, objects joined []: n/a (the class does not occur in the editor)', cell('s101_checkB_caw_walk', 'create', [PR, PD]), 'n/a')
    check('plant: the same line without the detail line (no positive proof) stays the game\'s', cell('s101_checkB_caw_walk', 'create', [PR]), 'stream')
    check('plant: a prop object joined but nothing replaced stays the game\'s', cell('s101_checkB_caw_walk', 'create', [PR, PD1]), 'stream')
    check('re-find: in a match the same lines stay the game\'s (the class occurs there)', cell('s101_sweep_match_pause', 'matches', [PR, PD]), 'stream')
    print(f'{"FAIL" if fails else "PASS"}  lane 247 selftest: {len(fails)} failed')
    return 1 if fails else 0


def selftest_252(st):
    """Lane 252 (S101): a class whose own detail line proves it joined nothing is ABSENT in that zip (arena 0 / ring 0 / screens joined to keys [] / crowd props joined entries {}), the
    generalisation of lane 225's wrestlers_seen and lane 247's props_seen. Real lines (11-13 13:29 checkB_caw_walk, 12:59 sweep_match_type); planted: a joined object with nothing replaced
    stays the game's draw; one zip is not enough for n/a; an older zip that proved the class ours still speaks. Past-bug re-find: Create Arena + Ring and Matches Stage screens read 'stream'."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'252b {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    AR = '[rdr][arena] ours frames 2, fallback 26, replaced 0, kept 55 (not ready 52, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 3), slots ignored 0, ms 0.02, persist-only 0'
    ARD = '[rdr][arena] [rdr][arena] ours frames 2, not ready 26, arena 0; objects joined 0 (by content 0, declared 0), unjoined 0, ambiguous 0, not stage 3 (counted again on every soft retry)'
    ARD1 = ARD.replace('objects joined 0', 'objects joined 3')
    RG = '[rdr][ring] ours frames 14, fallback 27, replaced 0, kept 77 (not ready 54, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 23), slots ignored 0, ms 0.08, persist-only 0'
    RGD = '[rdr][ring] [rdr][ring] ours frames 14, not ready 27, upload 0.0 ms, prepared 0; ring 0: objects joined 0 (by content 0, texture tie 0, tie left open 0), not a ring piece 23, unjoined 0'
    SC = '[rdr][screens] ours frames 0, fallback 0, replaced 0, kept 0 (not ready 0, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 0), slots ignored 0, ms 0.00, persist-only 0'
    SCD = '[rdr][screens] [rdr][screens] ours frames 0, not ready 0, upload 0.0 ms, prepared 0; screens: objects joined to keys [] (unique 0, pair told apart by content 0, pair interchangeable 0), not a screen of ours 0'
    SCD1 = SCD.replace('keys []', 'keys [7]')
    SC_OURS = SC.replace('ours frames 0, fallback 0, replaced 0, kept 0', 'ours frames 600, fallback 0, replaced 900, kept 0')
    CPD = '[rdr][crowd_props] ours frames 0, not ready 0, upload 0.0 ms; decisions 0, replaced 0; mismatch: counts 0 vertices 0 indices 0 layout 0 no bytes 0; joined entries {}, sites {}'
    CP = '[rdr][crowd_props] ours frames 0, fallback 0, replaced 0, kept 0 (not ready 0, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 0), slots ignored 0, ms 0.00, persist-only 0'
    sw = ['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_arena_ours=true', '--wwe2k14_rdr_ring_ours=true', '--wwe2k14_rdr_screens_ours=true', '--wwe2k14_rdr_crowd_props_ours=true']

    def cell(zs, item, cls):
        """zs = [(route, lines), ...] oldest first."""
        with tempfile.TemporaryDirectory(prefix='l252b_board_') as t:
            for i, (route, lines) in enumerate(zs):
                st.make(t, '20261113_1%d0000_%s' % (i, route), 'z%d' % i, '2026-11-13 1%d:00:00' % i, log=['[info] 20 s'] + lines, flags=sw)
            zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
            return build(zips)[0][(item, 'pc', cls)]['state']

    M = 's101_sweep_match_type'
    check('real lines: matches screens joined [] in two zips: n/a', cell([(M, [SC, SCD]), (M, [SC, SCD])], 'matches', 'screens'), 'n/a')
    check('plant: one zip alone is not enough for n/a (stays the game\'s)', cell([(M, [SC, SCD])], 'matches', 'screens'), 'stream')
    check('plant: a joined screen with nothing replaced is not a proof (stays the game\'s)', cell([(M, [SC, SCD1]), (M, [SC, SCD1])], 'matches', 'screens'), 'stream')
    check('plant: one zip joined a screen, one did not: not n/a', cell([(M, [SC, SCD1]), (M, [SC, SCD])], 'matches', 'screens'), 'stream')
    check('an older zip that proved screens ours still speaks past two newer proofs of absence', cell([(M, [SC_OURS]), (M, [SC, SCD]), (M, [SC, SCD])], 'matches', 'screens'), 'ours')
    check('real lines: matches arena joined 0 in two zips: n/a', cell([(M, [AR, ARD]), (M, [AR, ARD])], 'matches', 'arena'), 'n/a')
    check('plant: arena joined 3, nothing replaced: the game\'s draw', cell([(M, [AR, ARD1]), (M, [AR, ARD1])], 'matches', 'arena'), 'stream')
    check('real lines: matches ring ring 0 objects joined 0 in two zips: n/a', cell([(M, [RG, RGD]), (M, [RG, RGD])], 'matches', 'ring'), 'n/a')
    check('real lines: create arena (caw_walk) alone: not ours (n/a, the class is not assumed in the editor)', cell([('s101_checkB_caw_walk', [AR, ARD])], 'create', 'arena'), 'n/a')
    check('re-find: create arena: an older zip that proved arena ours wins over a newer joined-0 zip',
          cell([('s101_checkB_caw_walk', [AR.replace('fallback 26, replaced 0, kept 55', 'fallback 2, replaced 900, kept 5')]), ('s101_checkB_caw_walk', [AR, ARD])], 'create', 'arena'), 'ours')
    check('crowd props: joined entries {} in two zips: n/a', cell([(M, [CP, CPD]), (M, [CP, CPD])], 'matches', 'crowd_props'), 'n/a')
    check('no detail line in the zip (only the census): stays the game\'s, never n/a', cell([(M, [SC]), (M, [SC])], 'matches', 'screens'), 'stream')
    print(f'{"FAIL" if fails else "PASS"}  lane 252 class-proof selftest: {len(fails)} failed')
    return 1 if fails else 0


def selftest_259(st):
    """Lane 259 (S101): a menu-filed zip of a run that played a match carries the match's crowd census; Menus must not read ours / mixed for the crowd family from it
    (the real case: s101_checkK_finish_logo 04:34, cards frames ours 154 / fallback 1428 = 'mixed 10 %' under Menus). The match item keeps reading it. Planted logs only."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'259 {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    CC = '[2026-11-13 04:34:23.866] [info] [core] [t1] [rdr][crowd] cards: frames ours 154, fallback 1428, card draws dropped 205173, cards drawn 205173, kept 0, ms ours 1304.54'
    CM = '[2026-11-13 04:34:23.866] [info] [core] [t1] [rdr][crowd] ours frames 4648, fallback 32, stream draws dropped 1480866, members drawn 1480866, ms ours 3069.75'
    sw = ['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_crowd_ours=true', '--wwe2k14_rdr_crowd_cards=true']

    def cell(route, item, cls, lines):
        with tempfile.TemporaryDirectory(prefix='l259_board_') as t:
            st.make(t, '20261113_060000_' + route, 'z', '2026-11-13 06:00:00', log=['[info] 20 s'] + lines, flags=sw, words='main menu list 1' if item == 'menus' else '')
            zips, _ = load_all(t, logs_dir=Path(t) / 'nologs')
            return build(zips)[0][(item, 'pc', cls)]['state']

    check('menus: far cards line from a run that played a match: n/a, not mixed', cell('s101_checkK_finish_logo', 'menus', 'far_cards', [CC]), 'n/a')
    check('menus: crowd members line: n/a', cell('s101_checkK_finish_logo', 'menus', 'crowd', [CM]), 'n/a')
    check('re-find: the same cards line in a match item still reads mixed (10 %)', cell('s101_sweep_match_pause', 'matches', 'far_cards', [CC]), 'mixed')
    print(f'{"FAIL" if fails else "PASS"}  lane 259 selftest: {len(fails)} failed')
    return 1 if fails else 0


def selftest_240(st):
    r"""Lane 240 (S101): the phone column fills itself from what the phone produces: android case zips in out\cases and plain game logs in
    out\android\phone_logs (tools\android\pull_cases.ps1). Planted: a PC zip in out\cases is NOT read as phone (and does not join the board); a phone log with
    no census line adds nothing (cells stay none); a phone log with a census line and one scene family fills that item's phone cell; a log that reached
    two families is unplaced. Past-bug re-find: the whole phone column read 'none' (105 cells) because no phone source was ever read. Planted logs only."""
    import tempfile
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'240 {name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    PLANE = '[rdr][plane_shadow] plane_shadow: frames 240 (not ready 0) claimed 480 (cls 1) replaced 480 (draws) | kept: unjoined 0, mismatch 0'
    CV = "[2026-11-13 09:00:00.000] [info] [core] [t1] [wwe2k14] modified: wwe2k14_rdr_plane_shadow_ours = 'true' (source: command line)"

    def SEQ(n):
        return f'[2026-11-13 09:00:01.000] [info] [core] [t1] [wwe2k14][scene] sequence 3 -> {n} (x) at frame 1, 1.0 s after start'

    def board(root):
        sk = []
        zips, _ = load_all(Path(root) / 'runs', phone_roots=[root], logs_dir=Path(root) / 'nologs', skipped=sk)
        return zips, sk

    def put_log(root, name, lines):
        d = Path(root) / 'out' / 'android' / 'phone_logs'
        d.mkdir(parents=True, exist_ok=True)
        (d / name).write_text('\n'.join(lines), encoding='utf-8')

    def cell(zips, item, cls):
        return build(zips)[0][(item, 'phone', cls)]['state']

    with tempfile.TemporaryDirectory(prefix='l240_board_') as t:
        (Path(t) / 'runs').mkdir()
        st.make(Path(t) / 'out', 'cases', 'pc_zip', '2026-11-13 09:00:00', platform='windows', log=[CV, PLANE], words='in a match')
        zips, sk = board(t)
        check('plant: a PC zip in out/cases is not read as phone', [z['device'] for z in zips], [])
        check('plant: ... and is reported as skipped', len(sk), 1)
        st.make(Path(t) / 'out', 'cases', 'phone_zip', '2026-11-13 09:00:00', platform='android', log=[CV, PLANE], words='in a match (type 0, arena 1)')
        zips, sk = board(t)
        check('an android zip in out/cases is the phone device', [(z['device'], z['item']) for z in zips], [('phone', 'matches')])
        check('... and its plane cell is ours on the phone', cell(zips, 'matches', 'plane'), 'ours')
    with tempfile.TemporaryDirectory(prefix='l240_board_') as t:
        (Path(t) / 'runs').mkdir()
        put_log(t, 'phone_001_1.log', [CV, SEQ(75), SEQ(9), '[info] no census line in this one'])
        zips, sk = board(t)
        check('plant: a phone log without census lines adds nothing', (zips, len(sk)), ([], 1))
        put_log(t, 'phone_002_2.log', [CV, SEQ(75), SEQ(9), PLANE])
        zips, sk = board(t)
        check('a phone log with a census line and one family (matches) fills that cell', (len(zips), zips[0]['device'], zips[0]['item'], cell(zips, 'matches', 'plane')), (1, 'phone', 'matches', 'ours'))
        check('... and the menus phone cell stays none', cell(zips, 'menus', 'ui'), 'none')
        put_log(t, 'phone_003_3.log', [CV, SEQ(75), SEQ(9), SEQ(31), PLANE])
        zips, sk = board(t)
        check('a log that reached two families is unplaced (no item), never guessed', sorted(str(z['item']) for z in zips), ['None', 'matches'])
        put_log(t, 'phone_004_4.log', [CV, SEQ(3), SEQ(75), PLANE])
        zips, sk = board(t)
        check('a menus-only log is the menus item', sorted(str(z['item']) for z in zips), ['None', 'matches', 'menus'])
    print(f'{"FAIL" if fails else "PASS"}  lane 240 selftest: {len(fails)} failed')
    return 1 if fails else 0


def cov_read(zp, logs_dir):
    return read_zip(zp, logs_dir=logs_dir)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', default='')
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        sys.path.insert(0, str(HERE))
        import selftest
        return selftest.main() | selftest_213(selftest) | selftest_216(selftest) | selftest_225(selftest) | selftest_240(selftest) | selftest_247(selftest) | selftest_251(selftest) | selftest_252(selftest) | selftest_259(selftest)
    mc = main_checkout()
    global VIEWER
    VIEWER = None if a.runs else read_viewer_boards(mc)
    runs = Path(a.runs) if a.runs else mc / 'out' / 's101' / 'runs'
    live = None if a.runs else mc / 'out' / 'live'
    roots = [] if a.runs else list(dict.fromkeys([str(mc), str(REPO)]))   # the main checkout and this checkout (pull_cases.ps1 writes under the checkout it runs in)
    skipped = []
    if not runs.exists():
        print(f'FAIL  runs folder not found: {runs}')
        return 1
    zips, errors = load_all(runs, live, phone_roots=roots, skipped=skipped)
    for x in skipped:
        print('note  phone source skipped: ' + x, file=sys.stderr)
    if not zips:
        print(f'FAIL  no case zips under {runs}')
        return 1
    if a.check:
        for e in errors:
            print('FAIL  ' + e)
        cells, _ = build(zips)
        counts, counted, proven = headline(cells)
        print(f'{"FAIL" if errors else "PASS"}  {len(zips)} zips read, {len(errors)} unreadable; cells: {counts}')
        return 1 if errors else 0
    text = render(zips, errors, runs)
    if a.write:
        out = REPO / 'docs' / 'renderer' / 'COVERAGE_BOARD.md'
        out.write_text(text, encoding='utf-8', newline='\n')
        cells, _ = build(zips)
        counts, counted, proven = headline(cells)
        print(f'wrote {out}: {len(zips)} zips; proven {proven} (ours {counts["ours"]}, stream {counts["stream"]}) / unproven {counts["unproven"]} / none {counts["none"]} of {counted}')
    else:
        sys.stdout.reconfigure(encoding='utf-8')
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
