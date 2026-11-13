#!/usr/bin/env python3
r"""Lane 5 / lane 11 offline acceptance (Session 101): the case ring, zip writer, case writer and intake, with no game.
  py -I tools\case\selftest.py [--keep]
 1. build src\case (tools\case\build.ps1) into out\case\case_selftest.exe (rebuilt when any of its sources, the capture format or the reader changed)
 2. ring cap: 3x+ the cap fed (s100_heenan_build, 100 frames), a zip written WHILE the ring is fed, settled peak never above the cap
 3. a zip from the s100cards capture (fed exactly as the live sink feeds the ring), picture.png = rplay's picture of the last frame
 4. intake: exit 0, report.md + sheet.png written; rplay on the unpacked capture\ has 0 validation errors and its last picture equals the
    original capture's (pixel diff 0)
 5. planted faults, each must FAIL: a capture frame removed; a manifest that is not JSON; one flipped byte; a zip with no picture;
    a ring that lost a geometry content (unresolved: the draw is marked no-geometry, intake FAILS with UNRESOLVED, replay still has 0
    validation errors); intake with no rplay (FAILS; --no-replay passes)
 6. the baseline: frame 0 aged out of the window and the cap exceeded by the static set (s100cards): replays pixel-identical
 7. a 100-frame window (s100_heenan_build) fed with geometry dropped after its last use, images released and read-back parts flagged:
    0 unresolved, the flagged frames carry kFramePart, the last frame replays pixel-identical to the original capture's
 8. the same 100 frames over a cap (frames shrink): a valid zip, 0 unresolved, 0 validation errors (the pixel difference to the original is
    printed as INFO: state outside the window, see docs\systems\case_zip.md)
 9. a same-second double trigger writes two distinct valid zips
11. S101 lane 19 (the case zip, complete): every manifest field has a value or a reason (build, device, settings, way back, names, timing);
    a phone zip carries APK / refresh / battery / thermal; a caller that knows nothing (--bare) gets a reason for each; the report's first
    line names the build and the non-default switches and flags a build that is not ours (the #72 / S83 re-find); planted faults (a 0 in
    timing.csv, an empty GPU name, a missing build.git, a 0x0 display) each FAIL intake with their own token
Exit 0 = all checks passed. Needs out\captures\{s100cards,s100_heenan_build} and out\renderer\dev\rplay.exe in the main checkout.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
def _main_checkout():
    # the checkout that holds out\ and game_data\ (a worktree has none of its own)
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    if (REPO / 'out' / 'captures').exists():
        return REPO
    common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return Path(common).resolve().parent if common else REPO


MAIN = _main_checkout()
CAPTURE = MAIN / 'out' / 'captures' / 's100cards'
CAPTURE_LONG = MAIN / 'out' / 'captures' / 's100_heenan_build'  # 100 frames
RPLAY = next((p for p in (REPO / 'out/renderer/dev/rplay.exe', MAIN / 'out/renderer/dev/rplay.exe') if p.exists()), None)
EXE = REPO / 'out' / 'case' / 'case_selftest.exe'
WORK = REPO / 'out' / 'case' / 'selftest'
INTAKE = REPO / 'tools' / 'case' / 'intake.py'
results = []


def _git_hash():
    p = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True)
    return p.stdout.strip() or 'nogit'


GIT = _git_hash()  # S101 lane 19: the build stamp the selftest's "game side" hands over (the real stamp is tools\build_stamp.cmake)


def check(name, ok, detail=''):
    results.append((name, ok))
    print(('PASS  ' if ok else 'FAIL  ') + name + (f': {detail}' if detail else ''))
    return ok


def run(cmd, timeout=900):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def pixels_equal(a, b):
    from PIL import Image, ImageChops
    ia, ib = Image.open(a).convert('RGBA'), Image.open(b).convert('RGBA')
    if ia.size != ib.size:
        return False, f'sizes {ia.size} vs {ib.size}'
    diff = ImageChops.difference(ia, ib).getbbox()
    return diff is None, 'identical' if diff is None else f'differ in {diff}'


def last_line(out):
    return out.strip().splitlines()[-1] if out.strip() else 'no output'


RESULT_RE = re.compile(r'RESULT zip=(.+?) zip_bytes=(\d+) frames=(\d+) capture_bytes=(\d+) unresolved=(\d+) trigger_to_zip_ms=([\d.]+) wall_ms=[\d.]+ picture=(\d)')


def make_zips(capture, out_dir, *args):
    """Runs case_selftest; returns (exit code, [RESULT matches], output)."""
    code, out = run([EXE, '--capture', capture, '--out', out_dir, '--build-git', GIT, '--sdk-pin', 'pin-selftest', *args])
    return code, [RESULT_RE.search(l) for l in out.splitlines() if l.startswith('RESULT')], out


def intake(zip_path, cases, *extra):
    return run([sys.executable, '-I', INTAKE, zip_path, '--out-root', cases, '--rplay', RPLAY, *extra])


def validation_errors(dest):
    text = (dest / 'report.md').read_text(encoding='utf-8') if (dest / 'report.md').exists() else ''
    m = re.search(r'validation errors: (\d+)', text)
    return int(m.group(1)) if m else None


def manifest_of(zip_path):
    import json
    with zipfile.ZipFile(zip_path) as z:
        return json.loads(z.read('manifest.json').decode('utf-8'))


REQUIRED_REASONS_BARE = ['where.address', 'build.git', 'build.sdk_pin', 'build.kind', 'build.apk', 'device.gpu', 'device.driver', 'device.display', 'device.refresh_hz',
                         'device.render', 'device.battery_pct', 'device.thermal', 'settings.launcher_file', 'way_back.builder', 'way_back.seed',
                         'way_back.inputs_rows', 'device.resolution_scale', 'where.route', 'names', 'timing.render_ms', 'timing.present_ms', 'timing.shown_ms']


def cut_zip(zip_path, out_name, file_edits=None, manifest_edit=None):
    """A copy of a case zip with some files replaced (their manifest entries kept consistent: size + xxh3) and the manifest edited, so a planted
    fault trips only the check it is aimed at."""
    import json
    import xxhash
    out = WORK / out_name
    with zipfile.ZipFile(zip_path) as src:
        order = [i.filename for i in src.infolist()]
        data = {n: src.read(n) for n in order}
    for n, b in (file_edits or {}).items():
        data[n] = b
    mf = json.loads(data['manifest.json'].decode('utf-8'))
    for f in mf['files']:
        if f['path'] in (file_edits or {}):
            f['bytes'] = len(data[f['path']])
            f['xxh3'] = '%016x' % xxhash.xxh3_64_intdigest(data[f['path']])
    if manifest_edit:
        manifest_edit(mf)
    data['manifest.json'] = json.dumps(mf, indent=1).encode('utf-8')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as dst:
        for n in order:
            dst.writestr(n, data[n])
    return out


SPEC = 'type=0;p=343:0,324:-1;arena=4;pace=1'
LIVE = 'live=type:0,arena:4,pace:1,ids:343/324'
WHERE_CASES = [  # (name, state line, expected address or None, expected basis start)
    ('match spec of the run, bell rung', f'seq=9 scene_word=4 clock=1200 go=1 spec={SPEC} {LIVE}', f'match:{SPEC}@bell', 'exact'),
    ('match spec of the run, before the bell', f'seq=9 scene_word=4 clock=0 go=1 spec={SPEC} {LIVE}', f'match:{SPEC}', 'exact'),
    ('after-verb scene forced', f'seq=9 clock=500 go=1 spec={SPEC} {LIVE} forced=after:6139', f'match:{SPEC}@after:6139', 'exact'),
    ('victory scene forced', f'seq=9 go=1 spec={SPEC} {LIVE} forced=victory:6139', f'match:{SPEC}@victory:6139', 'exact'),
    ('mid scene forced', f'seq=9 go=1 spec={SPEC} {LIVE} forced=mid:324', f'match:{SPEC}@scene:324', 'exact'),
    ('menu scene forced', 'seq=75 menu=1 forced=menu:4025', 'scene:4025', 'exact'),
    ('no spec: rebuilt from the live match description, bell kept', f'seq=9 scene_word=1 clock=900 {LIVE}', f'match:{SPEC}@bell', 'best-effort'),
    ('a stale wwe2k14_match (other wrestlers) becomes the live spec, best-effort', f'seq=9 clock=900 go=1 spec=type=0;p=1:0,2:-1;arena=4;pace=1 {LIVE}', f'match:{SPEC}@bell', 'best-effort: wwe2k14_match'),
    ('a stale wwe2k14_match (other type)', f'seq=9 go=1 spec=type=3;p=343:0,324:-1 {LIVE}', f'match:{SPEC}', 'best-effort: wwe2k14_match'),
    ('a stale wwe2k14_match (other arena) with a forced after', f'seq=9 go=1 spec=type=0;p=343:0,324:-1;arena=9 {LIVE} forced=after:6139', f'match:{SPEC}@after:6139', 'best-effort: wwe2k14_match'),
    ('a partial-seat spec is a prefix of the live match: exact', f'seq=9 go=1 spec=type=0;p=343:0 {LIVE}', 'match:type=0;p=343:0', 'exact'),
    ('arena 300 (random) agrees with any arena: exact', f'seq=9 go=1 spec=type=0;p=343:0,324:-1;arena=300 {LIVE}', 'match:type=0;p=343:0,324:-1;arena=300', 'exact'),
    ('a spec the builder did not build (match_go 0): best-effort', f'seq=9 go=0 spec={SPEC} {LIVE}', f'match:{SPEC}', 'best-effort'),
    ('a dry builder run: best-effort', f'seq=9 go=1 dry=1 spec={SPEC} {LIVE}', f'match:{SPEC}', 'best-effort'),
    ('the live match unreadable: the cvar, best-effort', f'seq=9 go=1 spec={SPEC}', f'match:{SPEC}', 'best-effort: the live match description was not readable'),
    ('more than six participants: the spec leaves them out, said', 'seq=9 live=type:6,arena:4,pace:1,ids:1/2/3/4/5/6/7/8', 'match:type=6;arena=4;pace=1', 'best-effort'),
    ('a rumble spec: only the type is compared', 'seq=9 go=1 spec=type=23;rumble=1,2,3 live=type:23,arena:4,pace:1,ids:9/8/7/6', 'match:type=23;rumble=1,2,3', 'exact'),
    ('a menu list the atlas does not know (2000): best-effort', 'seq=75 menu=2000', 'menu:list:2000', 'best-effort'),

    ('main menu list', 'seq=75 menu=1', 'menu:list:1', 'exact'),
    ('30 Years jump set', 'seq=9 wm30=5', 'wm30:5', 'best-effort'),
    ('a recording being replayed', 'seq=9 replay=C:\\\\x\\\\out\\\\moments\\\\bry\\\\bry.rec frame=3000', 'moment:bry@frame:3000', 'best-effort'),
    ('nothing names the state (boot)', 'seq=3', None, ''),
    ('a forced scene with no match spec known', 'seq=9 forced=after:6139', None, ''),
]


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def where_exe(header_text, tag):
    """Compiles src/case/where_test.cpp against `header_text` as case_where.h (a copy: a planted twin never touches the tree)."""
    inc = WORK / ('where_' + tag)
    inc.mkdir(parents=True, exist_ok=True)
    (inc / 'case_where.h').write_text(header_text, encoding='utf-8')
    for n in ('case_manifest.h', 'where_test.cpp'):  # the test source is copied too: "case_where.h" resolves next to the .cpp first, so a twin must sit beside it
        shutil.copy(REPO / 'src' / 'case' / n, inc / n)
    exe = inc / 'where_test.exe'
    r = subprocess.run([find_clang(), '-std=c++17', '-I', str(inc), str(inc / 'where_test.cpp'), '-o', str(exe)], capture_output=True, text=True, timeout=240)
    if r.returncode != 0 or not exe.exists():
        raise RuntimeError('where_test did not compile: ' + (r.stdout + r.stderr)[-500:])
    return exe


def where_run(exe):
    p = subprocess.run([str(exe)], input='\n'.join(c[1] for c in WHERE_CASES) + '\n', capture_output=True, text=True, timeout=60)
    return p.stdout.strip().splitlines()


def where_problems(lines):
    bad = []
    if len(lines) != len(WHERE_CASES):
        return ['where_test answered %d lines for %d state sets' % (len(lines), len(WHERE_CASES))]
    for (name, _, want, basis), got in zip(WHERE_CASES, lines):
        addr = got.split(' | ')[0][len('ADDRESS '):]
        gb = got.split(' | ')[1][len('BASIS '):]
        have = None if addr == '-' else addr
        if have != want or (want and not gb.startswith(basis)) or (want is None and 'REASON set' not in got):
            bad.append('%s: want %s (%s), got %s' % (name, want, basis, got[:150]))
    return bad


def lane32(zip_path, cases):
    """Section 12 (S101 lane 32): the case zip's where block names the place. The shipped case_where.h on synthetic state sets, the address resolved
    offline by tools/goto, intake printing it, an empty where failing, and planted faults."""
    import json
    header = (REPO / 'src' / 'case' / 'case_where.h').read_text(encoding='utf-8')
    bad = where_problems(where_run(where_exe(header, 'real')))
    check('lane32 where: %d synthetic state sets give the expected address, basis and reason (spec of the run, @bell, forced after / victory / mid / menu, live rebuild, menu list, wm30, moment, boot = null with a reason)' % len(WHERE_CASES), not bad, '; '.join(bad[:3]))
    twins = {
        'the match spec dropped': header.replace('spec = in.match_spec;', 'spec = std::string();'),
        'a stale spec taken as exact': header.replace('inline bool SpecAgrees(const std::string& spec, const Inputs::Live& l) {', 'inline bool SpecAgrees(const std::string& spec, const Inputs::Live& l) {\n  return true;', 1),
        'a spec nobody built taken as exact': header.replace('const bool built = in.match_go != 0 && !in.match_dry;', 'const bool built = true;', 1),
        'an unknown menu list taken as exact': header.replace('constexpr int kAtlasListFirst = 1, kAtlasListLast = 60;', 'constexpr int kAtlasListFirst = 0, kAtlasListLast = 9999;', 1),
        'a prefix spec refused': header.replace('if (ids.size() > l.ids.size()) return false;', 'if (ids.size() != l.ids.size()) return false;', 1),
        'the forced scene ignored': header.replace('if (in.forced_verb == "menu") {', 'if (false) {', 1).replace('} else if (!in.forced_verb.empty()) {', '} else if (false) {', 1),
        'the bell never added': header.replace('(bell ? "@bell" : "")', '""', 1),
        'a spec with the live ids dropped': header.replace('s += ";p=";', 's += ";q=";', 1),
    }
    for what, text in twins.items():
        if text == header:
            check('lane32 planted twin "%s" changes the header (anchor)' % what, False)
            continue
        tb = where_problems(where_run(where_exe(text, 'twin_' + re.sub(r'\W', '', what))))
        check('lane32 planted fault (%s) gives a wrong address and is caught' % what, bool(tb), (tb[0][:110] if tb else 'not caught'))
    # the zip the case writer made (the same Fill, the full extras): address, basis, match, scene, and what intake says
    mf = manifest_of(zip_path)
    w = mf['where']
    check('lane32 manifest: address = the run\'s spec + @bell, basis says exact, match type / arena / ids, scene bank + word + entrance steps, reasons for camera / attires',
          w['address'] == f'match:{SPEC}@bell' and w['address_basis'].startswith('exact') and w['match']['type'] == '0' and w['match']['arena'] == '4'
          and w['match']['wrestlers'] == ['343', '324'] and w['match']['clock_frames'] == 1200 and w['scene']['bank'] == 324 and w['scene']['scene_word'] == 4
          and w['scene']['entrance_steps'][0] == 5 and all(k in mf['unavailable'] for k in ('where.camera.kind', 'where.match.attires')) and w['screen']['node'] is None, json.dumps(w)[:200])
    sample = json.dumps(w, indent=1)
    code, out = intake(zip_path, cases, '--no-replay')
    rep = (cases / zip_path.stem / 'report.md').read_text(encoding='utf-8').splitlines()
    line2 = rep[3] if len(rep) > 3 else ''
    check('lane32 intake: line 2 names the match (type, arena, wrestlers by name), the scene bank and says "goto <address> returns here"; exit 0',
          code == 0 and 'Brock Lesnar' in line2 and 'Daniel Bryan' in line2 and 'scene bank 324' in line2 and f'goto match:{SPEC}@bell returns here' in line2, line2[:260])
    print('      sample where block (the selftest zip):')
    for l in sample.splitlines():
        print('        ' + l)

    def empty_where(m):  # a zip as before this lane: the address is null and nothing says why
        m['where']['address'] = None
        m['unavailable'].pop('where.address', None)
    code, out = intake(cut_zip(zip_path, 'l32_fault_empty_where.zip', manifest_edit=empty_where), cases, '--no-replay')
    check('lane32 planted fault: an empty where (address null, no reason) FAILS (FIELD where.address)', code != 0 and 'where.address has no value' in out, last_line(out))

    def reason_where(m):  # null with a reason is fine
        m['where']['address'] = None
        m['unavailable']['where.address'] = 'selftest: the game was in sequence 3'
    code, out = intake(cut_zip(zip_path, 'l32_control_reason.zip', manifest_edit=reason_where), cases, '--no-replay')
    check('lane32 control: a null address with a reason passes intake', code == 0, last_line(out))

    def unresolvable(m):
        m['where']['address'] = 'match:type=0;p=343:0;bogus=1'
    code, out = intake(cut_zip(zip_path, 'l32_fault_unresolvable.zip', manifest_edit=unresolvable), cases, '--no-replay')
    check('lane32 planted fault: an address tools/goto cannot resolve FAILS (WHERE)', code != 0 and 'WHERE' in out and 'does not resolve' in out, last_line(out))

    def no_basis(m):
        m['where']['address_basis'] = None
    code, out = intake(cut_zip(zip_path, 'l32_fault_no_basis.zip', manifest_edit=no_basis), cases, '--no-replay')
    check('lane32 planted fault: an address with no exact / best-effort basis FAILS (WHERE)', code != 0 and 'address_basis' in out, last_line(out))

    def wrong_spec(m):  # the match spec dropped from the address: another wrestler in slot 0, caught by comparing with the match block
        m['where']['address'] = 'match:type=0;p=999:0,324:-1;arena=4;pace=1'
    code, out = intake(cut_zip(zip_path, 'l32_fault_wrong_address.zip', manifest_edit=wrong_spec), cases, '--no-replay')
    check('lane32 planted fault: an address whose spec names another wrestler FAILS (WHERE, differs from where.match)', code != 0 and 'WHERE' in out and 'where.match' in out, last_line(out))

    def report_of(zp):
        return (cases / zp.stem / 'report.md').read_text(encoding='utf-8')

    # review: an old (version 1) manifest is read as old; correct addresses that tools/goto cannot plan are a WARN, not a FAIL; "returns here" only for a plan
    def v1(m):
        m['version'] = 1
        m['where']['address'] = None
        m['unavailable'].pop('where.address', None)
    z = cut_zip(zip_path, 'l32_old_v1.zip', manifest_edit=v1)
    code, out = intake(z, cases, '--no-replay')
    check('lane32 old zip: a version 1 manifest (no where.address, no reason) PASSES with the note "pre-lane-32 manifest: where not checked"',
          code == 0 and 'pre-lane-32 manifest: where not checked' in report_of(z), last_line(out))
    real_old = MAIN / 'out' / 's101' / 'runs' / '20261111_215201_s100_owner_s89_brock' / 'case_20261111_215233_217_0_pc_run_exit.zip'
    if real_old.exists():
        code, out = intake(real_old, cases, '--no-replay')
        check('lane32 the real Check-A zip from before this lane (%s) PASSES with the pre-lane-32 note' % real_old.name[:30], code == 0 and 'pre-lane-32 manifest: where not checked' in report_of(real_old), last_line(out))

    def menu_2000(m):
        m['where']['address'] = 'menu:list:2000'
        m['where']['address_basis'] = 'exact for the list: (selftest: a hand-labelled exact menu list)'
        m['where']['match'] = {'type': None, 'arena': None, 'wrestlers': [], 'attires': [], 'clock_frames': None}
    z = cut_zip(zip_path, 'l32_menu_2000.zip', manifest_edit=menu_2000)
    code, out = intake(z, cases, '--no-replay')
    rep = report_of(z)
    check('lane32 menu:list:2000 has no atlas node: intake WARNS (not a FAIL), and does not say "goto ... returns here"',
          code == 0 and 'WARN where.address "menu:list:2000"' in rep and 'returns here' not in rep.split('\n')[3], last_line(out))

    def moment(m):
        m['where']['address'] = 'moment:nosuchmoment_selftest@frame:3000'
        m['where']['address_basis'] = 'best-effort: a recording is being replayed (selftest)'
    z = cut_zip(zip_path, 'l32_moment.zip', manifest_edit=moment)
    code, out = intake(z, cases, '--no-replay')
    check('lane32 an unregistered moment: address is parsed, intake WARNS (the result does not depend on out\\moments), no "returns here"',
          code == 0 and 'WARN where.address "moment:nosuchmoment_selftest@frame:3000"' in report_of(z) and 'returns here' not in report_of(z).split('\n')[3], last_line(out))

    def unparseable(m):
        m['where']['address'] = 'nowhere:at-all'
    code, out = intake(cut_zip(zip_path, 'l32_fault_unparseable.zip', manifest_edit=unparseable), cases, '--no-replay')
    check('lane32 planted fault: an address that does not parse FAILS (WHERE)', code != 0 and 'does not parse' in out, last_line(out))

    def partial_seat(m):  # a partial-seat spec is a prefix of the match block: passes
        m['where']['address'] = 'match:type=0;p=343:0;arena=300'
    code, out = intake(cut_zip(zip_path, 'l32_partial_seat.zip', manifest_edit=partial_seat), cases, '--no-replay')
    check('lane32 a partial-seat spec (a prefix of the wrestlers) with arena 300 passes the agreement check', code == 0, last_line(out))

    def stale_exact(m):  # a spec of another match labelled exact: caught by the agreement check
        m['where']['address'] = 'match:type=0;p=1:0,2:-1;arena=4;pace=1'
    code, out = intake(cut_zip(zip_path, 'l32_fault_stale.zip', manifest_edit=stale_exact), cases, '--no-replay')
    check('lane32 planted fault: a stale spec (other wrestlers) labelled exact FAILS (WHERE, differs from where.match)', code != 0 and 'differs from where.match' in out, last_line(out))
    # a 30-entrant spec is refused by tools/goto (the builder takes six participants)
    sys.path.insert(0, str(REPO / 'tools' / 'goto'))
    import resolve as _resolve
    big = 'match:type=0;p=' + ','.join('%d:-1' % (100 + i) for i in range(30)) + ';arena=4'
    plan = _resolve.resolve(big)
    check('lane32 a 30-participant spec is refused (ERROR, at most 6)', plan.status == 'error' and 'at most 6' in plan.reason, plan.status + ' ' + plan.reason[:80])
    check('lane32 a participant id over 499 and a pace over 2 are refused',
          _resolve.resolve('match:type=0;p=600:0').status == 'error' and _resolve.resolve('match:type=0;p=1:0;pace=3').status == 'error')
    # the atlas knows exactly the lists case_where.h calls exact (1-60): the constants may not drift from docs/systems/game_atlas.json
    atlas = json.loads((REPO / 'docs' / 'systems' / 'game_atlas.json').read_text(encoding='utf-8'))
    nodes = atlas.get('nodes', atlas)
    lists = sorted(int(k.split(':')[1]) for k in nodes if k.startswith('list:'))
    hdr = (REPO / 'src' / 'case' / 'case_where.h').read_text(encoding='utf-8')
    mm = re.search(r'kAtlasListFirst = (\d+), kAtlasListLast = (\d+)', hdr)
    check('lane32 case_where.h\'s atlas list range (%s) equals the atlas\'s list nodes (%d..%d)' % (mm.groups() if mm else '?', lists[0], lists[-1]),
          mm is not None and lists == list(range(int(mm.group(1)), int(mm.group(2)) + 1)), str(lists[:3]))


def lane19(zip_path, cases, dest):
    """Section 11 (S101 lane 19): the case zip complete. zip_path = the s100cards zip made with the full 'game side' extras."""
    import csv
    import io
    mf = manifest_of(zip_path)
    b, d, s, wb, nm, un = mf['build'], mf['device'], mf['settings'], mf['way_back'], mf['names'], mf['unavailable']
    check('lane19 build: git hash, SDK pin, kind, version from the stamp; APK null with its reason',
          b['git'] == GIT and b['sdk_pin'] == 'pin-selftest' and b['kind'] == 'dev' and b['version'].startswith('v0.0.0') and b['apk'] is None
          and 'build.apk' in un, str(b))
    check('lane19 device: GPU / driver / display / render from the renderer\'s log lines; PC refresh, battery, thermal are null with a reason',
          d['gpu'] == 'Selftest GPU 9000' and d['driver'] == 'Vulkan 1.3.280, driver 0x0000ABCD' and d['display']['width'] == 1920
          and d['display']['height'] == 1080 and d['display']['refresh_hz'] is None and d['render']['width'] == 2560 and d['render']['height'] == 1440 and d['render']['scale'] == 2.0 and d['render']['resolution_scale'] == 2 and mf['where']['route'] == 'selftest_route.txt'
          and all(k in un for k in ('device.refresh_hz', 'device.battery_pct', 'device.thermal')), f"{d['gpu']} / {d['driver']} / {d['display']}")
    check('lane19 settings: the switches off their defaults and the launcher file',
          s['non_default'] == ['wwe2k14_case', 'vsync', 'wwe2k14_launch_arena'] and s['effective']['vsync'] == 'false'
          and s['launcher_file'] == 'settings_selftest.json', str(s))
    check('lane19 way back: builder, seed, replay mode, controller rows in the window',
          wb['builder'] == 'wwe2k14_launch=@quick; wwe2k14_launch_arena=3' and wb['seed'] == 12345 and wb['replay_mode'] is True
          and isinstance(wb['inputs_rows'], int) and wb['inputs_rows'] >= 1 and 'way_back.seed' not in un, str(wb))
    with zipfile.ZipFile(zip_path) as z:
        inputs = z.read('inputs.rec').decode('utf-8')
        timing = z.read('timing.csv').decode('utf-8')
    first = wb['first_frame']
    check('lane19 inputs.rec: the recorder\'s header, the pad state before the window, the window\'s rows; nothing after it',
          'flag wwe2k14_launch @quick' in inputs and f'S {first - 5} ' in inputs and f'S {first} ' in inputs and 'FFFF' not in inputs, inputs.replace('\n', ' | ')[:200])
    draws, untagged, uncl = nm['draws'], nm['untagged_draws'], nm['unclassified_draws']
    check('lane19 names: window sums of the [rdr][names] counters (the probe adds 10 / 1 / 2 / 3 / 1 a frame)',
          bool(draws) and draws == 10 * untagged and uncl == 2 * untagged and nm['textures_unnamed'] == 3 * untagged and nm['surfaces_guessed'] == untagged
          and nm['unnamed_draws'] == untagged + uncl, str(nm))
    rows = list(csv.DictReader(io.StringIO(timing)))
    cells = {c: [r[c] for r in rows] for c in ('step_ms', 'render_ms', 'present_ms', 'shown_ms')}
    check('lane19 timing.csv: step + render measured, a present gap missing on some rows is an empty cell, shown_ms empty with a reason, no 0 cell',
          all(c for c in cells['step_ms']) and all(c == '4.250' for c in cells['render_ms']) and all(c in ('', '16.700') for c in cells['present_ms'])
          and any(c == '' for c in cells['present_ms'] + cells['shown_ms']) and all(c == '' for c in cells['shown_ms'])
          and 'timing.shown_ms' in un and 'timing.render_ms' not in un
          and not any(c not in ('', None) and float(c) == 0.0 for col in cells.values() for c in col), f'{len(rows)} rows')
    head = (dest / 'report.md').read_text(encoding='utf-8').splitlines() if (dest / 'report.md').exists() else []
    line1 = head[2] if len(head) > 2 else ''
    check('lane19 intake: line 1 names the build and the non-default switches, and is not flagged NOT OUR BUILD',
          GIT in line1 and 'v0.0.0' in line1 and 'vsync=false' in line1 and 'wwe2k14_launch_arena=3' in line1 and 'NOT OUR BUILD' not in line1, line1[:160])
    print('      intake report, first ten lines:')
    for l in head[:10]:
        print('        ' + l)

    # a phone zip: APK number, display refresh (from the display-timing line), battery, thermal
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_phone', '--picture', WORK / 'orig.png', '--phone')
    if code == 0 and rs and rs[0]:
        pz = Path(rs[0].group(1))
        pm = manifest_of(pz)
        code, out = intake(pz, cases, '--no-replay')
        check('lane19 phone zip: APK number, refresh from display timing, battery, thermal are values (no reasons); intake exit 0',
              code == 0 and pm['build']['apk'] == 30101 and abs(pm['device']['display']['refresh_hz'] - 120.0) < 0.2 and pm['device']['battery_pct'] == 81.5
              and pm['device']['thermal'] == 'nominal' and pm['device']['platform'] == 'android'
              and not any(k in pm['unavailable'] for k in ('build.apk', 'device.refresh_hz', 'device.battery_pct', 'device.thermal')), last_line(out))
    else:
        check('lane19 phone zip', False, 'no zip: ' + last_line(out))

    # a caller that knows nothing: every required field comes out as a reason, never a made-up value
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_bare', '--picture', WORK / 'orig.png', '--bare')
    if code == 0 and rs and rs[0]:
        bz = Path(rs[0].group(1))
        bm = manifest_of(bz)
        code, out = intake(bz, cases, '--no-replay')
        rep = (cases / bz.stem / 'report.md').read_text(encoding='utf-8').splitlines()
        missing = [k for k in REQUIRED_REASONS_BARE if not bm['unavailable'].get(k)]
        check('lane19 bare zip: a reason for every required field (none missing), no value invented, intake exit 0',
              not missing and bm['build']['git'] == '' and bm['device']['gpu'] == '' and bm['way_back']['seed'] is None and bm['names']['draws'] is None
              and code == 0, f'missing reasons: {missing}; ' + last_line(out))
        check('lane19 bare zip: line 1 flags NOT OUR BUILD (unknown git hash, no SDK pin)', 'NOT OUR BUILD (unknown git hash, no SDK pin)' in rep[2], rep[2][:160])
    else:
        check('lane19 bare zip', False, 'no zip: ' + last_line(out))

    # the re-find (tester report #72, S83): a build with no git hash / no SDK pin is flagged on the first line; the same zip with a stamp is not
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_stranger', '--picture', WORK / 'orig.png', '--build-git', 'unknown', '--sdk-pin', 'unknown')
    if code == 0 and rs and rs[0]:
        sz = Path(rs[0].group(1))
        code, out = intake(sz, cases, '--no-replay')
        rep = (cases / sz.stem / 'report.md').read_text(encoding='utf-8').splitlines()
        check('lane19 re-find #72: a zip from a build with no git hash and no SDK pin is flagged on line 1 and still names the switches to copy',
              code == 0 and 'NOT OUR BUILD (unknown git hash, no SDK pin)' in rep[2] and 'vsync=false' in rep[2], rep[2][:200])
    else:
        check('lane19 re-find #72', False, 'no zip: ' + last_line(out))

    # planted faults (each cut keeps the manifest's hashes right, so only the aimed-at check can trip); the untouched cut must pass
    code, out = intake(cut_zip(zip_path, 'l19_clean.zip'), cases, '--no-replay')
    check('lane19 control: an unedited cut of the good zip passes intake', code == 0, last_line(out))
    bad_timing = timing.replace('4.250', '0.000', 1)
    code, out = intake(cut_zip(zip_path, 'l19_fault_timing_zero.zip', file_edits={'timing.csv': bad_timing.encode('utf-8')}), cases, '--no-replay')
    check('lane19 planted fault: a 0 in timing.csv where "unmeasured" is meant FAILS (TIMING)', code != 0 and 'TIMING' in out and 'HASH' not in out, last_line(out))

    def gpu_empty(m):
        m['device']['gpu'] = ''
    code, out = intake(cut_zip(zip_path, 'l19_fault_gpu_empty.zip', manifest_edit=gpu_empty), cases, '--no-replay')
    check('lane19 planted fault: an empty GPU name with no reason FAILS (FIELD device.gpu)', code != 0 and 'device.gpu has no value' in out, last_line(out))

    def no_git(m):
        del m['build']['git']
    code, out = intake(cut_zip(zip_path, 'l19_fault_no_git.zip', manifest_edit=no_git), cases, '--no-replay')
    check('lane19 planted fault: a manifest missing build.git FAILS (FIELD)', code != 0 and 'build.git is missing' in out, last_line(out))

    def zero_display(m):
        m['device']['display']['width'] = 0
    code, out = intake(cut_zip(zip_path, 'l19_fault_display_zero.zip', manifest_edit=zero_display), cases, '--no-replay')
    check('lane19 planted fault: a display forced to width 0 with no reason FAILS (FIELD device.display)', code != 0 and 'device.display has no value' in out, last_line(out))

    def zero_render(m):  # the lane 16 finding: every Check A zip recorded the render size as 0x0 at scale 1.0
        m['device']['render']['width'] = 0
        m['device']['render']['height'] = 0
    code, out = intake(cut_zip(zip_path, 'l19_fault_render_zero.zip', manifest_edit=zero_render), cases, '--no-replay')
    check('lane19 planted fault: a render size of 0x0 with no reason FAILS (FIELD device.render)', code != 0 and 'device.render has no value' in out, last_line(out))
    # review fixes: log-format drift must fail loudly; an old zip (no "unavailable") is read as old; a half-written recorder row is dropped
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_drift', '--picture', WORK / 'orig.png', '--drift')
    if code == 0 and rs and rs[0]:
        dz = Path(rs[0].group(1))
        code, out = intake(dz, cases, '--no-replay')
        check('lane19 planted fault: the renderer ran but its device line was not understood -> reason "PARSE FAILED", intake FAILS (PARSE)',
              code != 0 and 'PARSE' in out and 'PARSE FAILED' in manifest_of(dz)['unavailable'].get('device.gpu', ''), last_line(out))
    else:
        check('lane19 drift zip', False, 'no zip: ' + last_line(out))

    def old_manifest(m):
        del m['unavailable']
    code, out = intake(cut_zip(zip_path, 'l19_pre.zip', manifest_edit=old_manifest), cases, '--no-replay')
    rep = (cases / 'l19_pre' / 'report.md').read_text(encoding='utf-8') if (cases / 'l19_pre' / 'report.md').exists() else ''
    check('lane19 a zip written before lane 19 (no "unavailable") passes intake, noted as pre-lane-19', code == 0 and 'pre-lane-19 manifest' in rep, last_line(out))
    check('lane19 inputs.rec: a half-written last row (no newline) is dropped', 'PARTIALROW' not in inputs)
    # the Bryan zip of Check A: a truncated picture.png (hashes kept right, so only the picture check can trip). Twin: the whole picture passes (the control above).
    with zipfile.ZipFile(zip_path) as z:
        whole = z.read('picture.png')
    code, out = intake(cut_zip(zip_path, 'l19_fault_picture_truncated.zip', file_edits={'picture.png': whole[:len(whole) // 2]}), cases, '--no-replay')
    check('lane19 planted fault: a truncated picture.png FAILS (PICTURE, no IEND)', code != 0 and 'truncated' in out and 'HASH' not in out, last_line(out))
    code, out = intake(cut_zip(zip_path, 'l19_fault_picture_iend_only.zip', file_edits={'picture.png': whole[:-12] + b'\x00' * 12}), cases, '--no-replay')
    check('lane19 planted fault: a picture whose end was overwritten FAILS (PICTURE)', code != 0 and 'PICTURE' in out, last_line(out))
    # Check B (the Create a Superstar exit zip had NO PICTURE): at a run's exit no further frame is presented. A run-exit trigger must still get a whole
    # picture (from the last presented frame); a live key trigger in the same situation has no next frame to wait for (the control: it times out).
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_exit', '--picture', WORK / 'orig.png', '--no-next-frame', '--trigger', 'run_exit')
    if code == 0 and rs and rs[0]:
        ez = Path(rs[0].group(1))
        code, out = intake(ez, cases, '--no-replay')
        with zipfile.ZipFile(ez) as z:
            got = z.read('picture.png') if 'picture.png' in z.namelist() else b''
        check('lane19 run exit with no further frame: the zip still holds a whole picture (last presented frame), intake exit 0',
              rs[0].group(7) == '1' and got == (WORK / 'orig.png').read_bytes() and code == 0 and manifest_of(ez)['trigger'] == 'run_exit', last_line(out))
    else:
        check('lane19 run exit picture', False, 'no zip: ' + last_line(out))
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_nonext', '--picture', WORK / 'orig.png', '--no-next-frame')
    if code == 0 and rs and rs[0]:
        code, out = intake(rs[0].group(1), cases, '--no-replay')
        check('lane19 control: a key trigger waiting for a next frame that never comes ends with NO PICTURE and intake FAILS (PICTURE)',
              rs[0].group(7) == '0' and code != 0 and 'PICTURE' in out, last_line(out))
    else:
        check('lane19 control: no next frame', False, 'no zip: ' + last_line(out))
    l2 = head[3] if len(head) > 3 else ''
    l5 = next((l for l in head if l.startswith('5. ')), '')
    check('lane19 intake: the summary names the route, the render size and the RESOLUTION setting',
          'route selftest_route.txt' in l2 and '2560x1440' in l5 and 'RESOLUTION setting 2' in l5, (l2 + ' | ' + l5)[:200])

    def blank_reason(m):
        m['unavailable']['device.refresh_hz'] = ''
    code, out = intake(cut_zip(zip_path, 'l19_fault_blank_reason.zip', manifest_edit=blank_reason), cases, '--no-replay')
    check('lane19 planted fault: an empty reason FAILS (FIELD)', code != 0 and 'empty reason' in out, last_line(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keep', action='store_true')
    ap.parse_args()
    for need in (CAPTURE, CAPTURE_LONG):
        if not need.exists():
            print(f'FAIL  needs the capture {need}')
            return 1
    if RPLAY is None:
        print('FAIL  needs rplay.exe (tools\\renderer\\build.ps1)')
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    WORK.mkdir(parents=True)

    sources = list((REPO / 'src' / 'case').glob('*')) + [REPO / 'tools' / 'case' / 'build.ps1', REPO / 'src' / 'renderer_adapter' / 'adapter_frame.h',
                                                         REPO / 'src' / 'renderer_adapter' / 'capture_reader.h']
    sources = [s for s in sources if s.exists()]
    if not EXE.exists() or EXE.stat().st_mtime < max(s.stat().st_mtime for s in sources):
        code, out = run(['pwsh', '-NoProfile', '-File', REPO / 'tools/case/build.ps1'], 900)
        if not check('build src/case', code == 0 and EXE.exists(), last_line(out)):
            print(out[-2000:])
            return 1
    else:
        check('build src/case', True, 'exe is current')

    # S101 lane 59: a relative --wwe2k14_case_dir is warned about in the run log (Check E: zips landed in the exe's working folder); absolute ones are silent
    code, out = run([EXE, '--dir-test'])
    check('case_dir: relative warns, absolute is silent', code == 0 and 'OK case_dir warning' in out and 'DIR rel_cases is_absolute=0 warned=1' in out and 'is_absolute=1 warned=0' in out, last_line(out))

    # 2. the cap, with a zip written while the ring is fed
    code, out = run([EXE, '--capture', CAPTURE_LONG, '--out', WORK / 'zcap', '--cap-test'])
    print('      ' + out.strip().replace('\n', '\n      '))
    check('ring cap holds (3x+ the cap fed, a zip written meanwhile)', code == 0 and 'OK ring cap' in out and 'mid-run zip written' in out)

    # 3. a zip from s100cards
    code, out = run([RPLAY, '--capture', CAPTURE, '--validation', '1', '--out', WORK / 'orig.png'])
    check('rplay on the original capture', code == 0 and (WORK / 'orig.png').exists(), f'exit {code}')
    code, rs, out = make_zips(CAPTURE, WORK / 'zips', '--picture', WORK / 'orig.png')
    if not check('case zip written', code == 0 and len(rs) == 1 and rs[0] is not None, last_line(out)):
        return 1
    m = rs[0]
    zip_path = Path(m.group(1))
    print(f'      ring "RING" line: {next((l for l in out.splitlines() if l.startswith("RING")), "")}')
    print(f'      trigger to zip written: {m.group(6)} ms for {m.group(3)} frames, {int(m.group(4)) / 1e6:.1f} MB of capture, zip {int(m.group(2)) / 1e6:.1f} MB')
    check('no unresolved refs', m.group(5) == '0', f'{m.group(5)} unresolved')
    check('the zip has its picture', m.group(7) == '1')
    with zipfile.ZipFile(zip_path) as zf:
        first = zf.namelist()[0]
        check('manifest.json is the first entry', first == 'manifest.json', first)
        check('zip CRCs', zf.testzip() is None)
    mf = manifest_of(zip_path)
    check('manifest has window.unresolved / baseline_dropped (both 0)', mf['window'].get('unresolved') == 0 and mf['window'].get('baseline_dropped') == 0)

    # 4. intake
    cases = WORK / 'cases'
    code, out = intake(zip_path, cases)
    print('      ' + out.strip().replace('\n', '\n      '))
    dest = cases / zip_path.stem
    check('intake exit 0', code == 0)
    check('report.md + sheet.png', (dest / 'report.md').exists() and (dest / 'sheet.png').exists())
    if (dest / 'report.md').exists():
        head = (dest / 'report.md').read_text(encoding='utf-8').splitlines()
        check('report starts with the five-line summary', len([l for l in head[2:8] if re.match(r'\d\. ', l)]) == 5)
        check('report says 0 validation errors', 'validation errors: 0' in '\n'.join(head))
    if (dest / 'replay_last.png').exists():
        ok, detail = pixels_equal(WORK / 'orig.png', dest / 'replay_last.png')
        check("unpacked capture's last frame == original capture's", ok, detail)
    else:
        check("unpacked capture's last frame == original capture's", False, 'intake wrote no replay_last.png')
    if (dest / 'picture.png').exists():
        check('picture.png is what was handed in', (dest / 'picture.png').read_bytes() == (WORK / 'orig.png').read_bytes())

    # 5. planted faults
    def rewrite(name, drop=None, replace=None):
        out_zip = WORK / name
        with zipfile.ZipFile(zip_path) as src, zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_STORED) as dst:
            for item in src.infolist():
                if drop and item.filename == drop:
                    continue
                data = src.read(item.filename)
                if replace and item.filename == replace[0]:
                    data = replace[1]
                dst.writestr(item.filename, data)
        return out_zip

    frame_name = sorted(n for n in zipfile.ZipFile(zip_path).namelist() if n.startswith('capture/frames/'))[-1]
    code, out = intake(rewrite('fault_missing_frame.zip', drop=frame_name), cases)
    check('planted fault: a capture frame removed FAILS loudly', code != 0 and 'MISSING' in out, last_line(out))
    code, out = intake(rewrite('fault_manifest_not_json.zip', replace=('manifest.json', b'this is not json {')), cases)
    check('planted fault: manifest not JSON FAILS loudly', code != 0 and 'not JSON' in out, last_line(out))
    frame0 = 'capture/frames/frame_000000.bin'
    with zipfile.ZipFile(zip_path) as z:
        corrupted = bytearray(z.read(frame0))
    corrupted[-1] ^= 0xFF
    code, out = intake(rewrite('fault_flipped_byte.zip', replace=(frame0, bytes(corrupted))), cases)
    check('planted fault: one flipped byte FAILS (HASH)', code != 0 and 'HASH' in out, last_line(out))
    # no picture: the ring is told no picture; the zip then lacks picture.png and intake FAILS
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_nopic')
    if code == 0 and rs and rs[0]:
        code, out = intake(rs[0].group(1), cases, '--no-replay')
        check('planted fault: a zip with no picture FAILS (PICTURE)', code != 0 and 'PICTURE' in out and rs[0].group(7) == '0', last_line(out))
    else:
        check('planted fault: a zip with no picture FAILS (PICTURE)', False, 'no zip: ' + last_line(out))
    # lost geometry: unresolved is reported, the draw is marked no-geometry, intake FAILS, the replay still has 0 validation errors
    code, rs, out = make_zips(CAPTURE_LONG, WORK / 'zips_lost', '--picture', WORK / 'orig.png', '--drop-geometry', '--lose-geometry', '3')
    if code == 0 and rs and rs[0]:
        lost_zip = Path(rs[0].group(1))
        n_unres = int(rs[0].group(5))
        code, out = intake(lost_zip, cases)
        check('planted fault: a geometry content the ring never got is UNRESOLVED (manifest + intake FAIL), replay valid',
              n_unres > 0 and manifest_of(lost_zip)['window']['unresolved'] == n_unres and code != 0 and 'UNRESOLVED' in out
              and validation_errors(cases / lost_zip.stem) == 0, f'unresolved {n_unres}; ' + last_line(out))
    else:
        check('planted fault: a geometry content the ring never got is UNRESOLVED', False, 'no zip: ' + last_line(out))
    # no rplay: FAIL unless --no-replay
    code, out = run([sys.executable, '-I', INTAKE, zip_path, '--out-root', cases, '--rplay', WORK / 'no_such_rplay.exe'])
    check('intake without rplay FAILS (REPLAY)', code != 0 and 'REPLAY' in out, last_line(out))
    code, out = run([sys.executable, '-I', INTAKE, zip_path, '--out-root', cases, '--rplay', WORK / 'no_such_rplay.exe', '--no-replay'])
    check('intake --no-replay passes without rplay', code == 0, last_line(out))

    # 6. the baseline: frame 0 aged out of the window (s100cards, then a cap the static set exceeds)
    for name, args in (('window', ['--window-s', '0.005']), ('over the cap', ['--cap-mb', '10'])):
        code, rs, out = make_zips(CAPTURE, WORK / ('zips_b_' + name.split()[0]), '--picture', WORK / 'orig.png', *args)
        if code == 0 and rs and rs[0]:
            bz = Path(rs[0].group(1))
            code, out = intake(bz, cases)
            db = cases / bz.stem
            ident = False
            detail = f'intake exit {code}, {rs[0].group(3)} frame(s) in the zip'
            if (db / 'replay_last.png').exists():
                ident, d = pixels_equal(WORK / 'orig.png', db / 'replay_last.png')
                detail += f', last frame vs original: {d}'
            check(f'baseline: frame 0 aged out ({name}): the last frame replays identically', code == 0 and ident and rs[0].group(5) == '0', detail)
        else:
            check(f'baseline: frame 0 aged out ({name})', False, 'no zip: ' + last_line(out))

    # 7. a 100-frame window with drops, releases and read-back parts
    code, out = run([RPLAY, '--capture', CAPTURE_LONG, '--validation', '1', '--out', WORK / 'long_orig.png'])
    check('rplay on the 100-frame capture', code == 0 and (WORK / 'long_orig.png').exists(), f'exit {code}')
    code, rs, out = make_zips(CAPTURE_LONG, WORK / 'zips_long', '--picture', WORK / 'long_orig.png', '--drop-geometry', '--release-images', '--part-every', '7')
    if code == 0 and rs and rs[0]:
        lz = Path(rs[0].group(1))
        with zipfile.ZipFile(lz) as z:
            frames = sorted(n for n in z.namelist() if n.startswith('capture/frames/'))
            flagged = sum(1 for n in frames if struct.unpack_from('<I', z.read(n), 28)[0] & 8)
        want = sum(1 for i in range(100) if i % 7 == 6)
        check('100 frames in the zip, 0 unresolved, read-back parts flagged (kFramePart)', len(frames) == 100 and rs[0].group(5) == '0' and flagged == want,
              f'{len(frames)} frames, {rs[0].group(5)} unresolved, {flagged} flagged (want {want})')
        code, out = intake(lz, cases)
        dl = cases / lz.stem
        ident, d = pixels_equal(WORK / 'long_orig.png', dl / 'replay_last.png') if (dl / 'replay_last.png').exists() else (False, 'no replay_last.png')
        check('100-frame zip: intake exit 0, 0 validation errors, last frame identical to the original capture', code == 0 and validation_errors(dl) == 0 and ident,
              f'intake exit {code}, validation errors {validation_errors(dl)}, {d}')
    else:
        check('100-frame zip', False, 'no zip: ' + last_line(out))

    # 8. the same over a cap: frames shrink, the zip stays valid and complete about what it holds
    code, rs, out = make_zips(CAPTURE_LONG, WORK / 'zips_cap', '--picture', WORK / 'long_orig.png', '--drop-geometry', '--cap-mb', '20')
    if code == 0 and rs and rs[0]:
        cz = Path(rs[0].group(1))
        code, out = intake(cz, cases)
        dc = cases / cz.stem
        info = ''
        if (dc / 'replay_last.png').exists():
            info = pixels_equal(WORK / 'long_orig.png', dc / 'replay_last.png')[1]
        check('over-cap zip (frames shrunk): valid, 0 unresolved, 0 validation errors', code == 0 and rs[0].group(5) == '0' and validation_errors(dc) == 0,
              f'{rs[0].group(3)} frames; intake exit {code}')
        print(f'INFO  over-cap zip, last frame vs the full capture: {info} (state outside the window is not in a zip: docs\\systems\\case_zip.md)')
    else:
        check('over-cap zip', False, 'no zip: ' + last_line(out))

    # 10. S101 lane 11 (case 7041, the first real zip): an image whose record changed format after its content was read. The ring keeps the
    # newest table, so the content no longer fits; replayed, the upload copied 2 MB out of a 1 MB buffer. The writer must leave it out.
    for name, args in (('in the window', []), ('in the baseline', ['--window-s', '0.005'])):
        code, rs, out = make_zips(CAPTURE, WORK / ('zips_retype_' + name.split()[-1]), '--picture', WORK / 'orig.png', '--retype-image', *args)
        if code == 0 and rs and rs[0] and 'RETYPED' in out:
            rz = Path(rs[0].group(1))
            stale = int(re.search(r'stale=(\d+)', out).group(1))
            code, out = intake(rz, cases)
            check(f'retyped image ({name}): the stale content is dropped (stale > 0), intake exit 0, 0 validation errors',
                  stale > 0 and code == 0 and validation_errors(cases / rz.stem) == 0, f'stale {stale}; intake exit {code}, validation errors {validation_errors(cases / rz.stem)}')
        else:
            check(f'retyped image ({name})', False, 'no zip: ' + last_line(out))
    # its twin: the same table over the UNFILTERED contents (an old-writer zip, rebuilt here by patching images.bin of a good zip) must fail
    # under validation, or the check above proves nothing
    with zipfile.ZipFile(zip_path) as z:
        twin_dir = WORK / 'retype_twin'
        z.extractall(twin_dir)
    cap = twin_dir / 'capture'
    frame0 = (cap / 'frames' / 'frame_000000.bin').read_bytes()
    _, _, _, n_sec = struct.unpack_from('<IIII', frame0, 0)
    victim = None
    for s in range(n_sec):
        sid, esz, cnt, off = struct.unpack_from('<IIQQ', frame0, 32 + s * 24)
        if sid == 15 and cnt:  # kSecContents
            victim = struct.unpack_from('<I', frame0, off)[0]
    images = bytearray((cap / 'images.bin').read_bytes())
    patched = False
    if victim is not None:
        at = 16 + (victim - 1) * 24  # ids are 1-based, in order (CAPTURE_FORMAT.md)
        if struct.unpack_from('<I', images, at)[0] == victim:
            images[at + 10] = 16 if images[at + 10] != 16 else 1  # ImageRecord::format
            patched = True
            (cap / 'images.bin').write_bytes(bytes(images))
    code, out = run([RPLAY, '--capture', cap, '--validation', '1', '--out', WORK / 'twin.png'])
    check('planted fault: a content that does not fit its image record FAILS validation (twin of the retype checks)',
          patched and ('ERROR' in out or code != 0), f'patched {patched}; exit {code}; ' + last_line(out))

    # 9. a same-second double trigger
    code, rs, out = make_zips(CAPTURE, WORK / 'zips_double', '--picture', WORK / 'orig.png', '--double-trigger')
    paths = [Path(r.group(1)) for r in rs if r]
    ok = code == 0 and len(paths) == 2 and paths[0] != paths[1] and all(p.exists() for p in paths)
    if ok:
        for p in paths:
            code, out = intake(p, cases, '--no-replay')
            ok = ok and code == 0
    check('a same-second double trigger writes two distinct valid zips', ok, ', '.join(p.name for p in paths))

    lane19(zip_path, cases, dest)
    lane32(zip_path, cases)

    failed = [n for n, ok in results if not ok]
    print(f'{len(results) - len(failed)}/{len(results)} checks passed' + (f'; FAILED: {failed}' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
