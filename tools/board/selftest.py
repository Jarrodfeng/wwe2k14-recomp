#!/usr/bin/env python3
"""Selftest of the coverage board (S101 lane 93). Builds small synthetic case zips in a temp folder and runs them through the shipped
code path (coverage.read_zip -> build -> render), with planted faults whose expected verdicts are fixed:
  * a zip whose census line is MISSING while the class switch is on must show "unproven", never "ours" (the lane's plant);
  * a census line with replaced > 0 / mismatch 0 shows "ours"; with a mismatch it is not "ours";
  * a run with the switch off shows "stream"; no zip at all shows "none"; the phone is a separate device;
  * the newest zip with a say wins, and a newer zip that says nothing does not hide an older one;
  * bug-button triggers and validation errors are counted; an unreadable zip is reported, not skipped.
Exit 0 only when every check holds. Nothing from the game is read or written."""
import json, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import coverage as cov

OURS_ARENA = '[2026-11-12 12:46:52.003] [info] [core] [t1] [rdr][arena] ours frames 1782, fallback 18, replaced 345673, kept 581 (not ready 558, unjoined 0, mismatch 0, variant 0), slots ignored 0, ms 479.98'
MISMATCH_ARENA = OURS_ARENA.replace('mismatch 0', 'mismatch 7')
CVAR = "[2026-11-12 12:46:00.000] [info] [core] [t1] [wwe2k14] modified: wwe2k14_rdr_{}_ours = '{}' (source: command line)"


def make(root, run, name, created, platform='windows', trigger='run_exit', log=(), flags=None, note='', words=''):
    d = Path(root) / run
    d.mkdir(parents=True, exist_ok=True)
    if flags is not None:
        (d / 'plan.json').write_text(json.dumps({'route': f'C:/x/{run}.txt', 'flags': flags}), encoding='utf-8')
    man = {'format': 'wwe2k14-case', 'version': 1, 'id': name, 'created': created, 'trigger': trigger, 'note': note,
           'build': {'kind': 'dev'}, 'device': {'platform': platform}, 'where': {'words': words, 'match': {'type': None}}}
    with zipfile.ZipFile(d / f'{name}.zip', 'w') as z:
        z.writestr('manifest.json', json.dumps(man))
        z.writestr('log.txt', '\n'.join(log))
        z.writestr('capture/images.bin', b'planted game bytes are never read')
    return d / f'{name}.zip'


def state(zips, item, dev, cls):
    cells, _ = cov.build(zips)
    return cells[(item, dev, cls)]['state']


def L(msg):
    return '[2026-11-12 17:23:37.000] [info] [core] [t1] ' + msg


def lane156(check, state, make):
    """Lane 156: the columns added for every merged promotion. Each plant is a zip whose switch is on with a missing or mismatching census
    line (it must read unproven, never ours), and each has a twin with the line clean (it must read ours)."""
    OWN = '--wwe2k14_rdr_owner=true'

    def sw(*names):
        return [OWN] + [f'--wwe2k14_rdr_{n}=true' for n in names]

    def one(run, name, log, flags, **kw):
        with tempfile.TemporaryDirectory(prefix='l156_board_') as t:
            make(t, run, name, '2026-11-12 17:00:00', log=log, flags=flags, **kw)
            return cov.load_all(t)[0]

    ent = '20261112_170000_s100_owner_s89_brock'
    POST = L('[rdr][post] ours frames 1680, fallback 120, shapes {match 9, entrance 1507, two_layer 0, film 0, editor 164, cards 0, other 0}, dropped draws 26763')
    z = one(ent, 'p1', [], sw('post_ours'))
    check('156 post: switch on, no line is unproven', state(z, 'entrances', 'pc', 'post'), 'unproven')
    z = one(ent, 'p2', [POST], sw('post_ours'))
    check('156 post: twin with the line is ours', state(z, 'entrances', 'pc', 'post'), 'ours')
    check('156 post: share is ours frames / (ours + fallback)', cov.build(z)[0][('entrances', 'pc', 'post')]['share'], 93)
    z = one(ent, 'p3', [POST.replace('ours frames 1680', 'ours frames 0')], sw('post_ours'))
    check('156 post: a line that replaced nothing is stream', state(z, 'entrances', 'pc', 'post'), 'stream')
    # circles: mismatch plant
    CIR = L('[rdr][circles] circles: frames 900 (not ready 10) claimed 500 replaced 480 | kept: unjoined 3, mismatch {m} (shape 0, constants 0, late 0, mesh wait 0), variant 0 | join offset 1 (stamps held 4, relearned 0, replaced by offset -2..3: 0 480 0 0 0 0) | mesh checked 480 bad {b} (index 0) unreadable 0, upload failed 0, 0.50 ms')
    z = one(ent, 'c1', [CIR.format(m=0, b=0)], sw('circles_ours'))
    check('156 circles: clean line is ours', state(z, 'entrances', 'pc', 'circles'), 'ours')
    z = one(ent, 'c2', [CIR.format(m=4, b=0)], sw('circles_ours'))
    check('156 circles: switch on + a mismatch is unproven', state(z, 'entrances', 'pc', 'circles'), 'unproven')
    z = one(ent, 'c3', [CIR.format(m=0, b=2)], sw('circles_ours'))
    check('156 circles: a bad mesh is unproven', state(z, 'entrances', 'pc', 'circles'), 'unproven')
    # plane, small passes, movie
    PLN = L('[rdr][plane_shadow] plane_shadow: frames 900 (not ready 0) claimed 40 (mask 20, blur 20) replaced 40 (mask 20, blur 20) | kept: unjoined 0, mismatch {m} (constants 0, late 0), variant 0 | join offset 1 (stamps held 2, relearned 0) | rows verified 40 (mask draws min 1 max 2)')
    check('156 plane: clean is ours', state(one(ent, 'pl1', [PLN.format(m=0)], sw('plane_shadow_ours')), 'entrances', 'pc', 'plane'), 'ours')
    check('156 plane: mismatch is unproven', state(one(ent, 'pl2', [PLN.format(m=1)], sw('plane_shadow_ours')), 'entrances', 'pc', 'plane'), 'unproven')
    SMP = L('[rdr][small_passes] decisions 200, replaced 200 (sweat 100, prelight 100) | verify 20; read failed 0; bad: vertices {v} indices 0 shape 0 layout 0 clip 0 variant 0, fetch slot without image 0 | frames ready 900 idle 0')
    check('156 small passes: clean is ours', state(one(ent, 'sp1', [SMP.format(v=0)], sw('small_passes_ours')), 'entrances', 'pc', 'small_passes'), 'ours')
    check('156 small passes: bad vertices is unproven', state(one(ent, 'sp2', [SMP.format(v=3)], sw('small_passes_ours')), 'entrances', 'pc', 'small_passes'), 'unproven')
    wm = '20261112_170000_s100_owner_s90_wm30'
    MOV = L('[rdr][movie] ours frames 4200, fallback 0, replaced {r}, kept 0 (not ready 0, unjoined 0, mismatch {m}, variant 0, texture 0, palette 0, not ours 0), slots ignored 0, ms 105.56')
    check('156 movie: clean is ours', state(one(wm, 'm1', [MOV.format(r=9, m=0)], sw('movie_ours')), 'wm30', 'pc', 'movie'), 'ours')
    check('156 movie: switch on, line missing is unproven', state(one(wm, 'm2', [], sw('movie_ours')), 'wm30', 'pc', 'movie'), 'unproven')
    check('156 movie: mismatch is unproven', state(one(wm, 'm3', [MOV.format(r=9, m=2)], sw('movie_ours')), 'wm30', 'pc', 'movie'), 'unproven')
    # ui quads ride on the ui line
    UI = L('[rdr][ui] ours frames 3600, fallback 0, replaced 38985, kept 7952 (not ready 3332, unjoined 4620, mismatch 0, variant 0, texture 0, palette 0, not ours 0), slots ignored 0, ms 29.76')
    UIQ = L('[rdr][ui] quads: decisions 120, sprites 70, rects 50; kept: variant 0, geometry 0, quad 0, constants 0, alpha-to-coverage 0, texture 0; last quad 0,0 - 1,1')   # UiOurs::WindowDetail's line
    menu = '20261112_170000_s100_owner_boot_menu_quit'
    check('156 ui quads: line with quads is ours', state(one(menu, 'q1', [UI, UIQ], sw('ui_ours', 'ui_quads_ours')), 'menus', 'pc', 'ui_quads'), 'ours')
    check('156 ui quads: switch on, no quads text is unproven', state(one(menu, 'q2', [UI], sw('ui_ours', 'ui_quads_ours')), 'menus', 'pc', 'ui_quads'), 'unproven')
    check('156 ui quads: switch off is stream', state(one(menu, 'q3', [UI], sw('ui_ours')), 'menus', 'pc', 'ui_quads'), 'stream')
    check('156 ui quads: the ui class off makes quads stream', state(one(menu, 'q4', [UI, UIQ], [OWN, '--wwe2k14_rdr_ui_ours=false', '--wwe2k14_rdr_ui_quads_ours=true']), 'menus', 'pc', 'ui_quads'), 'stream')
    # far cards, signs, floor crowd
    CARD = L('[rdr][crowd] cards: frames ours 1419, fallback 1, card draws dropped 727996, cards drawn 727996, kept 0, ms ours 394.03')
    WARN = L('[rdr][crowd] WARN cards: bad state on this frame (no light-map page copy; record cards 9, stream card draws 9, unpaired record 0, unaccounted stream 0, order inversions 0, picture mismatches 2): every card of this frame stays the stream\'s')
    check('156 far cards: line is ours', state(one(ent, 'k1', [CARD], sw('crowd_ours', 'crowd_cards')), 'entrances', 'pc', 'far_cards'), 'ours')
    check('156 far cards: switch on, no line is unproven', state(one(ent, 'k2', [], sw('crowd_ours', 'crowd_cards')), 'entrances', 'pc', 'far_cards'), 'unproven')
    check('156 far cards: picture mismatches in a WARN make it unproven', state(one(ent, 'k3', [CARD, WARN], sw('crowd_ours', 'crowd_cards')), 'entrances', 'pc', 'far_cards'), 'unproven')
    CP = L('[rdr][crowd_props] ours frames 1199, not ready 1, upload 0.0 ms; decisions 22048, replaced 22048; mismatch: counts {c} vertices 0 indices 0 layout 0 no bytes 0; stream 32-bit index kept 0')
    check('156 signs: clean is ours', state(one(ent, 'cp1', [CP.format(c=0)], sw('crowd_props_ours')), 'entrances', 'pc', 'crowd_props'), 'ours')
    check('156 signs: counts mismatch is unproven', state(one(ent, 'cp2', [CP.format(c=5)], sw('crowd_props_ours')), 'entrances', 'pc', 'crowd_props'), 'unproven')
    FLOOR_REC = L('[scene][crowd] floor op 21: seen 138989, taken 138989, rejected: regs 0, type 0, group 0, lod 0, index 0, element 0')
    check('156 floor crowd: the record-side line alone is unproven, never ours', state(one(ent, 'f1', [FLOOR_REC], sw('crowd_ours', 'crowd_floor')), 'entrances', 'pc', 'floor_crowd'), 'unproven')
    FLOORW = L('[rdr][crowd] floor: ours frames 1598, floor members drawn 5800, floor draws kept 12 of 5812 floor stream draws, floor (class, LOD 0) checks: 120 of 120 equal the stream\'s triangles + vertices and 120 match its posed blob, 5800 of 5800 members use the slot-0 picture of their own stream draw')
    CROWDW = L('[rdr][crowd] ours frames 1598, fallback 33, stream draws dropped 248293, members drawn 248293, ms ours 721.78')
    z = one(ent, 'f3', [CROWDW, FLOORW], sw('crowd_ours', 'crowd_floor'))
    check('156 floor crowd: the 600-frame window line (lane 156) is ours and leaves the crowd line alone', (state(z, 'entrances', 'pc', 'floor_crowd'), state(z, 'entrances', 'pc', 'crowd')), ('ours', 'ours'))
    check('156 floor crowd has no share (no fallback count)', cov.build(z)[0][('entrances', 'pc', 'floor_crowd')]['share'], None)
    PAIR = L('[rdr][crowd] WARN the record does not pair with the stream frame: 10 of 615 member worlds match, 615 members vs 700 stream crowd draws (bad frames so far 100): this frame keeps the stream\'s crowd')
    check('156 crowd: a record-does-not-pair WARN is a mismatch', state(one(ent, 'cw', [CROWDW, PAIR], sw('crowd_ours')), 'entrances', 'pc', 'crowd'), 'unproven')
    # lane 239: persist-only binds are counted apart; a census line carrying them must not lower the share (the census no longer folds them into fallback)
    CROWDP = L('[rdr][crowd] ours frames 1598, fallback 33, stream draws dropped 248293, members drawn 248293, ms ours 721.78, persist-only 900')
    zp = one(ent, 'po1', [CROWDP], sw('crowd_ours'))
    check('239 crowd: a persist-only count leaves the share at ours / (ours + fallback)', (state(zp, 'entrances', 'pc', 'crowd'), cov.build(zp)[0][('entrances', 'pc', 'crowd')]['share']), ('ours', 98))
    ARENAP = L('[rdr][arena] ours frames 1782, fallback 18, replaced 345673, kept 581 (not ready 558, unjoined 0, mismatch 0, variant 0), slots ignored 0, ms 479.98, persist-only 400')
    zp = one(ent, 'po2', [ARENAP], sw('arena_ours'))
    check('239 arena: the persist-only tail leaves the line read as before (ours, share 99)', (state(zp, 'entrances', 'pc', 'arena'), cov.build(zp)[0][('entrances', 'pc', 'arena')]['share']), ('ours', 99))
    FLOOR = L('[rdr][crowd] ours frames 1598, fallback 33, stream draws dropped 248293, members drawn 248293, ms ours 721.78, floor members drawn 5800, floor draws kept 12 of 5812 floor stream draws, floor (class, LOD 0) checks: 120 of 120 equal the stream\'s triangles')
    z = one(ent, 'f2', [FLOOR], sw('crowd_ours', 'crowd_floor'))
    check('156 floor crowd: the draw-side text is ours', state(z, 'entrances', 'pc', 'floor_crowd'), 'ours')
    check('156 floor crowd: the crowd line on the same text is still read', state(z, 'entrances', 'pc', 'crowd'), 'ours')
    # pieces ride on the wrestlers detail line
    WR = L('[rdr][wrestlers] wrestlers: objects seen 7 joined 7 (ambiguous 1, unjoined 0), claimed 32362 replaced 2570 | palette rows 93612 exact 93612 | verify 121 bad {b}, vertex elements compared 280737: exact 280737 near 0 bad 0 | parts built 51 | models read 8 | pieces (draws unless noted): replaced {r} palette-refused 0 no owner 0, object-frames linked 681')
    check('156 pieces: replaced pieces are ours', state(one(ent, 'pc1', [WR.format(b=0, r=284)], sw('wrestlers_ours')), 'entrances', 'pc', 'pieces'), 'ours')
    check('156 pieces: a bad verify is unproven', state(one(ent, 'pc2', [WR.format(b=2, r=284)], sw('wrestlers_ours')), 'entrances', 'pc', 'pieces'), 'unproven')
    check('156 pieces: no pieces replaced is stream', state(one(ent, 'pc3', [WR.format(b=0, r=0)], sw('wrestlers_ours')), 'entrances', 'pc', 'pieces'), 'stream')
    # shadow maps
    SH = L('[rdr][shadows_ours] frames 2001 (unnamed 58), maps 512 px, plane 100%, images made 20 (map 11, plane 9), draws retargeted 251840, clears 12162, copies whole 18138 (partial made whole 5976, duplicates dropped 516), readers 124325, refused 0, ms 83.00')
    check('156 shadow maps: line is ours', state(one(ent, 'sh1', [SH], sw('shadows_ours')), 'entrances', 'pc', 'shadow_maps'), 'ours')
    check('156 shadow maps: switch on, no line is unproven', state(one(ent, 'sh2', [], sw('shadows_ours')), 'entrances', 'pc', 'shadow_maps'), 'unproven')
    # thin draws: the class counts as ours when its own census says so; the thin line only names the path
    ARENA = L('[rdr][arena] ours frames 1782, fallback 18, replaced 11915, kept 581 (not ready 558, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 23), slots ignored 0, ms 22.82')
    THIN = L('[rdr][thin] arena thin {t}, proof 12657 (ok {ok}), learned 492, pinned 492, holes {h}, proof mismatches {pm} (program 0, layout 0), unpinned 0, released 0, slot missing {sm}, tween key moved 0, frames 4200, ms thin 100.13')

    def path(z):
        return cov.build(z)[0][('entrances', 'pc', 'arena')]['path']
    z = one(ent, 't1', [ARENA, THIN.format(ok=12657, h=0, pm=0, sm=0, t=367383)], sw('arena_ours', 'arena_thin'))
    check('156 thin: a clean thin line gives path thin and the class stays ours', (state(z, 'entrances', 'pc', 'arena'), path(z)), ('ours', 'thin'))
    z = one(ent, 't2', [ARENA, THIN.format(ok=12657, h=3, pm=0, sm=0, t=367383)], sw('arena_ours', 'arena_thin'))
    check('156 thin: holes make the cell unproven', state(z, 'entrances', 'pc', 'arena'), 'unproven')
    z = one(ent, 't3', [ARENA, THIN.format(ok=12650, h=0, pm=7, sm=0, t=367383)], sw('arena_ours', 'arena_thin'))
    check('156 thin: proof mismatches make the cell unproven', state(z, 'entrances', 'pc', 'arena'), 'unproven')
    z = one(ent, 't4', [ARENA], sw('arena_ours', 'arena_thin'))
    check('156 thin: thin on, no thin line is unproven (thin draws never reach the seam; only the thin line speaks for them)', (state(z, 'entrances', 'pc', 'arena'), path(z)), ('unproven', 'thin (no thin line)'))
    z = one(ent, 't7', [ARENA, THIN.format(ok=12657, h=5000, pm=0, sm=0, t=0)], sw('arena_ours', 'arena_thin'))
    check('156 thin: thin 0 with holes 5000 is unproven (the old thin > 0 test read it ours)', state(z, 'entrances', 'pc', 'arena'), 'unproven')
    z = one(ent, 't8', [ARENA, THIN.format(ok=12657, h=0, pm=0, sm=77, t=367383)], sw('arena_ours', 'arena_thin'))
    check('156 thin: slot missing 77 is unproven', state(z, 'entrances', 'pc', 'arena'), 'unproven')
    z = one(ent, 't9', [ARENA.replace('mismatch 0', 'mismatch 5')], [OWN, '--wwe2k14_rdr_arena_ours=false'])
    check('156 a census line with mismatches is unproven even when the plan says the switch is off', state(z, 'entrances', 'pc', 'arena'), 'unproven')
    z = one(ent, 't5', [ARENA], sw('arena_ours') + ['--wwe2k14_rdr_arena_thin=false'])
    check('156 thin: thin switch off is path full', path(z), 'full')
    z = one(ent, 't6', [THIN.format(ok=12657, h=0, pm=0, sm=0, t=367383)], sw('arena_ours', 'arena_thin'))
    check('156 thin: a thin line alone (arena switch on, no census line) is unproven', state(z, 'entrances', 'pc', 'arena'), 'unproven')
    z = one(ent, 'mx', [POST.replace('ours frames 1680, fallback 120', 'ours frames 20, fallback 100')], sw('post_ours'))
    check('156 post at 20% of the frames is mixed, not ours', state(z, 'entrances', 'pc', 'post'), 'mixed')
    check('156 mixed is left out of the ours count', cov.headline(cov.build(z)[0])[0]['mixed'] >= 1 and cov.headline(cov.build(z)[0])[0]['ours'] == 0, True)
    # the live-driver channel: the item comes from the manifest's words, never the folder name
    with tempfile.TemporaryDirectory(prefix='l156_board_') as t:
        (Path(t) / 'runs').mkdir()
        make(str(Path(t) / 'live' / 'j3'), 'cases', 'case_live', '2026-11-12 18:00:00', log=[ARENA], flags=None, words='in a match (type 1, arena 30), scene word 4 (match load / entrances); FORCED scene (step 2)')
        zips, errs = cov.load_all(Path(t) / 'runs', Path(t) / 'live')
        check('156 live: a FORCED-scene zip is a 30 Years zip by its words', [z['item'] for z in zips], ['wm30'])
        zips2, _ = cov.load_all(Path(t) / 'runs', None)
        check('156 live: no live root, no live zips', len(zips2), 0)


def lane252(check, state, make):
    r"""Lane 252: match replays, cutscenes and the Arena Viewer as board items. Planted logs / boards only. Past-case re-find: the real victory runs open
    cache:\Spectacular.rps for write then read ~580 frames apart (frame 9497 / 10075), which the board could not show as "the replay was reached"."""
    import os, tempfile
    OWN = '--wwe2k14_rdr_owner=true'
    W = L("[wwe2k14][file] NtCreateFile 'cache:\\Spectacular.rps' open_if access=0xC0120089 -> 0x00000000 handle=0xF8000790 frame=9497")
    R = L("[wwe2k14][file] NtCreateFile 'cache:\\Spectacular.rps' open_if access=0x80120089 -> 0x00000000 handle=0xF8000790 frame=10075")
    BOOT = L("[wwe2k14][file] NtCreateFile 'cache:\\Spectacular.rps' overwrite_if access=0xC0120089 -> 0x00000000 handle=0xF8000720 frame=0")
    SEASON = L("[wwe2k14][file] NtCreateFile 'D:\\PAC\\EVT\\SEASON00.PAC' open access=0x80120089 -> 0x00000000 handle=0xF80007A0 frame=9011")
    ENT = L("[wwe2k14][file] NtCreateFile 'D:\\PAC\\EVT\\ENTRANCE02.PAC' open access=0x80120089 -> 0x00000000 handle=0xF80007A0 frame=900")
    UI = '[rdr][ui_text] ours frames 600, fallback 0, replaced 5000, kept 10 (not ready 0, unjoined 0, mismatch 0, variant 0, texture 0, palette 0, not ours 10), slots ignored 0, ms 9.00'
    sw = [OWN, '--wwe2k14_rdr_ui_text_ours=true']

    def zips_of(*runs):
        with tempfile.TemporaryDirectory(prefix='l252_board_') as t:
            for i, (run, log) in enumerate(runs):
                make(t, '20261113_06%02d00_%s' % (i, run), 'z%d' % i, '2026-11-13 06:%02d:00' % i, log=log, flags=sw)
            zips, _ = cov.load_all(t, logs_dir=Path(t) / 'nologs')
            cells, meta = cov.build(zips)
            text = cov.render(zips, [], t)
            return zips, meta, cells, text

    zips, meta, cells, text = zips_of(('s101_sweep_victory', [W, R, UI]))
    check('252: a victory zip that opened the Spectacular clip after frame 0 is still the Victory zip', zips[0]['item'], 'victory')
    check('252: ... and REACHES Match replays by marker (write 1, read 1)', (len(meta[('replays', 'pc')]['also']), zips[0]['markers']['replay_write'], zips[0]['markers']['replay_read']), (1, 1, 1))
    check('252: ... but fills no Match replays class cell (a reach is not a census)', state(zips, 'replays', 'pc', 'ui_text'), 'none')
    check('252: ... and its Victory cell is unchanged (ours)', state(zips, 'victory', 'pc', 'ui_text'), 'ours')
    check('252: the item table says reached, with no dedicated zip', 'Reached by log marker in 1 other zip(s)' in text, True)
    zips, meta, cells, text = zips_of(('s101_sweep_victory', [BOOT, UI]))
    check('252: the boot-time open at frame 0 (overwrite_if) is not a replay', (zips[0]['markers']['replay_write'], len(meta[('replays', 'pc')]['also'])), (0, 0))
    zips, meta, cells, text = zips_of(('s101_sweep_match_replay', [W, R, UI]))
    check('252: a route named match_replay is the Match replays item', zips[0]['item'], 'replays')
    check('252: ... and its census line fills that cell (ours)', state(zips, 'replays', 'pc', 'ui_text'), 'ours')
    check('252: ... the phone cell stays none', state(zips, 'replays', 'phone', 'ui_text'), 'none')
    for nm in ('dlc_replay', 'replay_play', 's21_replay_time', 's53_cutscene_hitch'):
        zips = zips_of((nm, [UI]))[0]
        check('252: the harness route %s is not a replay / cutscene item' % nm, zips[0]['item'] in ('replays', 'cutscenes'), False)
    zips, meta, cells, text = zips_of(('s101_sweep_wm30_long', [SEASON, ENT, UI]))
    check('252: a SEASON pack open after frame 0 reaches Cutscenes (and the zip stays 30 Years)', (zips[0]['item'], len(meta[('cutscenes', 'pc')]['also']), zips[0]['markers']['bank_open']), ('wm30', 1, 1))
    check('252: an ENTRANCE pack open is not a cutscene marker', zips_of(('s101_sweep_wm30_long', [ENT, UI]))[0][0]['markers']['bank_open'], 0)
    check('252: a dedicated cutscene route is the Cutscenes item', zips_of(('s101_sweep_cutscene_wm30', [SEASON, UI]))[0][0]['item'], 'cutscenes')
    check('252: director play counts as a cutscene marker', cov.reach_markers([L('[wwe2k14][scene][director] play: pushed scene 4025 (ctx type 4)')])['director_play'], 1)

    # the Arena Viewer: file census boards
    def board(root, tag, plant, na, nw, mt):
        d = Path(root) / 'out' / 's101' / 'filecensus' / tag
        d.mkdir(parents=True, exist_ok=True)
        rows = [{'kind': 'arena', 'status': 'pass' if i else 'tile fault', 'validation': 0} for i in range(na)] + [{'kind': 'wrestler', 'status': 'pass', 'validation': 0} for _ in range(nw)]
        f = d / 'board.json'
        f.write_text(json.dumps({'tag': tag, 'plant': plant, 'rows': rows}), encoding='utf-8')
        os.utime(f, (mt, mt))
    with tempfile.TemporaryDirectory(prefix='l252_viewer_') as t:
        check('252: no census board: no viewer reading', cov.read_viewer_boards(t), None)
        board(t, 'plant_mirror', 'mirror', 57, 240, 3000)
        check('252: a planted-fault board (plant set) is never read', cov.read_viewer_boards(t), None)
        board(t, 'partial', '', 4, 9, 3100)
        check('252: a partial board (4 arenas, 9 wrestlers) is not the whole', cov.read_viewer_boards(t), None)
        board(t, 'full_old', '', 57, 240, 3200)
        board(t, 'arenas_new', '', 57, 0, 3300)
        v = cov.read_viewer_boards(t)
        check('252: the newest complete board speaks per kind (arenas: arenas_new, wrestlers: full_old)', (v['arena']['tag'], v['wrestler']['tag']), ('arenas_new', 'full_old'))
        check('252: counts: 56 pass + 1 tile fault', v['arena']['counts'], {'tile fault': 1, 'pass': 56})
        cov.VIEWER = v
        try:
            zips, meta, cells, text = zips_of(('s101_sweep_victory', [UI]))
        finally:
            cov.VIEWER = None
        check('252: the viewer row reads the census (arenas 57 ... 1 tile fault)', 'arenas 57: 56 pass, 1 tile fault' in text, True)
        check("252: ... and the viewer's class cells are n/a, never a game census cell", state(zips, 'viewer', 'pc', 'arena'), 'n/a')


def main():
    fails = []

    def check(name, got, want):
        ok = got == want
        print(('PASS  ' if ok else 'FAIL  ') + f'{name}: {got!r}' + ('' if ok else f' (wanted {want!r})'))
        if not ok:
            fails.append(name)

    with tempfile.TemporaryDirectory(prefix='l93_board_') as t:
        # A: switch on + clean census line -> ours
        make(t, '20261112_100000_victory_a', 'case_a', '2026-11-12 10:00:00', log=[CVAR.format('arena', 'true'), OURS_ARENA],
             flags=['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_arena_ours=true'])
        # B (the plant): switch on for crowd, census line missing -> unproven
        make(t, '20261112_100100_entrance_b', 'case_b', '2026-11-12 10:01:00', log=[CVAR.format('crowd', 'true')],
             flags=['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_crowd_ours=true'])
        # C: menus run, owner on, ui switch off, no ui line -> stream; wrestlers on with a mismatching line -> unproven
        make(t, '20261112_100200_boot_menu_quit', 'case_c', '2026-11-12 10:02:00',
             log=[OURS_ARENA.replace('[arena]', '[wrestlers]').replace('mismatch 0', 'mismatch 3')],
             flags=['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_wrestlers_ours=true'])
        # D: caw_walk, old zip says ours for ui, newer zip (no plan, no ui line) says nothing -> older wins
        make(t, '20261112_090000_caw_walk', 'case_d1', '2026-11-12 09:00:00', words='x',
             log=[OURS_ARENA.replace('[arena]', '[ui]')], flags=['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_ui_ours=true'])
        make(t, '20261112_095900_caw_walk', 'case_d2', '2026-11-12 09:59:00', log=['[info] nothing here'])
        # E: wm30 on the phone only; two bug-button triggers and a validation error line
        make(t, '20261112_110000_wm30', 'case_e', '2026-11-12 11:00:00', platform='android', trigger='tap',
             log=['VUID-vkCmdDraw-foo-00001 something', '[rdr][PROBLEM] x'], flags=[])
        make(t, '20261112_110100_wm30', 'case_f', '2026-11-12 11:01:00', trigger='key', note='carry darkening', flags=[])
        # a corrupt zip beside them
        bad = Path(t) / 'bad_run'
        bad.mkdir()
        (bad / 'case_bad.zip').write_bytes(b'not a zip at all')

        zips, errors = cov.load_all(t)
        check('zips read', len(zips), 7)
        check('unreadable zip reported', len(errors), 1)
        check('plant: promoted + census line missing is unproven', state(zips, 'entrances', 'pc', 'crowd'), 'unproven')
        check('plant: never ours when the line is missing', state(zips, 'entrances', 'pc', 'crowd') == 'ours', False)
        check('clean census line is ours', state(zips, 'victory', 'pc', 'arena'), 'ours')
        check('switch off is stream', state(zips, 'menus', 'pc', 'ui'), 'stream')
        check('mismatching census line is not ours', state(zips, 'menus', 'pc', 'wrestlers'), 'unproven')
        check('newer zip with no say does not hide the older', state(zips, 'create', 'pc', 'ui'), 'ours')
        check('no zip at all is none', state(zips, 'universe', 'pc', 'ui'), 'none')
        check('class that cannot occur is n/a', state(zips, 'menus', 'pc', 'arena'), 'n/a')
        check('phone is its own device (30 Years phone has a zip)', state(zips, 'wm30', 'phone', 'arena') != 'none', True)
        check('a zip with no say is unproven, not none', state(zips, 'wm30', 'phone', 'ui'), 'unproven')
        # an arena census line planted into the PHONE group must not leak to the PC cell
        check('devices do not leak', state(zips, 'entrances', 'phone', 'arena'), 'none')
        text = cov.render(zips, errors, t)
        check('bug-button count in board', '2 zip(s) with trigger key / chord / tap' in text, True)
        check('validation / problem parsed', any(z['problems']['validation_errors'] == 1 and z['problems']['rdr_problem'] == 1 for z in zips), True)
        check('fault case matched by note', 'case_f.zip' in text, True)
        check('board names unreadable zips', 'could not be read' in text, True)
        e = [z for z in zips if z['name'] == 'case_e.zip'][0]
        check('bad run is flagged', bool(cov.zip_state(e)), True)
        counts, counted, proven = cov.headline(cov.build(zips)[0])
        check('headline sums', counts['ours'] + counts['mixed'] + counts['stream'] + counts['unproven'] + counts['none'], counted)
    # twin of the plant: with the line present the same cell must flip to ours (proves the check can fail)
    with tempfile.TemporaryDirectory(prefix='l93_board_') as t:
        make(t, '20261112_100100_entrance_b', 'case_b', '2026-11-12 10:01:00',
             log=[CVAR.format('crowd', 'true'), '[2026-11-12 12:46:52.378] [info] [core] [t1] [rdr][crowd] ours frames 1591, fallback 28, stream draws dropped 120151, members drawn 120151, ms ours 431.33'],
             flags=['--wwe2k14_rdr_owner=true', '--wwe2k14_rdr_crowd_ours=true'])
        zips, _ = cov.load_all(t)
        check('twin: the same cell with its census line is ours', state(zips, 'entrances', 'pc', 'crowd'), 'ours')
    lane156(check, state, make)
    lane252(check, state, make)
    print(f'{"FAIL" if fails else "PASS"}  coverage board selftest: {len(fails)} failed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
