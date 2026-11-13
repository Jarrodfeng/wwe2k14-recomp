#!/usr/bin/env python3
r"""Case intake (Session 101, lane 5; docs\renderer\CASE_ZIP.md, docs\systems\case_zip.md): a case zip in, a first report out, nobody asked.

  py -I tools\case\intake.py ZIP [--out-root DIR] [--rplay EXE] [--no-replay]
  py -I tools\case\intake.py --watch DIR [--interval 5] [--once]

For a zip: unpack to <out-root>\<zip name>\ (default out\cases); check manifest.json against the files (every listed file present, size and
xxh3 equal; a mismatch is a loud FAIL); replay capture\ with rplay (--validation 1) and count the validation errors; the gallery sheet of
the last frame; the timing summary; where the game was in words; the unnamed counts when present. Writes report.md (a five-line summary
first) and sheet.png beside the unpacked files. Exit 0 = every check passed, 1 = a FAIL, 2 = bad usage.
--watch polls DIR for new zips (never a .part), processes each once, and leaves <zip>.done beside it (its text: the exit code).
The unpacked folder holds game data when the zip does: it stays under out\ (CLAUDE.md hard rule 1).
"""
import argparse
import csv
import io
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

try:
    import xxhash
except ImportError:  # a checker that cannot read its input fails loudly
    print('FAIL  python module xxhash is missing (pip install xxhash): the manifest hashes cannot be checked')
    sys.exit(1)

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools' / 'goto'))  # S101 lane 32: check_where uses the offline goto resolver
def _main_checkout():
    # the checkout that holds out\ and game_data\ (a worktree has none of its own)
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    if (REPO / 'out' / 'captures').exists():
        return REPO
    common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return Path(common).resolve().parent if common else REPO


MAIN = _main_checkout()


class Fail(Exception):
    pass


def find_tool(name, rel):
    for root in (REPO, MAIN):
        p = root / rel
        if p.exists():
            return p
    return None


def check_manifest(zf, dest, fails):
    """Reads manifest.json and verifies every file it lists. Returns the manifest (or raises Fail)."""
    names = zf.namelist()
    if 'manifest.json' not in names:
        raise Fail('manifest.json is not in the zip')
    raw = zf.read('manifest.json')
    try:
        m = json.loads(raw.decode('utf-8'))
    except Exception as e:
        raise Fail(f'manifest.json is not JSON ({e})')
    if m.get('format') != 'wwe2k14-case':
        raise Fail(f"manifest.json format is {m.get('format')!r}, not 'wwe2k14-case'")
    if m.get('version') not in (1, 2):
        fails.append(f"manifest version {m.get('version')} (this intake reads versions 1 and 2; reading on)")
    listed = {}
    for f in m.get('files', []):
        listed[f['path']] = f
    for path, f in listed.items():
        if path == 'manifest.json':
            continue
        if path not in names:
            fails.append(f'MISSING  {path} is listed in the manifest but not in the zip')
            continue
        data = zf.read(path)
        if len(data) != f['bytes']:
            fails.append(f"SIZE     {path}: manifest says {f['bytes']} bytes, zip holds {len(data)}")
            continue
        h = '%016x' % xxhash.xxh3_64_intdigest(data)
        if h != f['xxh3']:
            fails.append(f"HASH     {path}: manifest xxh3 {f['xxh3']}, file {h}")
    for n in names:
        if not n.endswith('/') and n not in listed:
            fails.append(f'UNLISTED {n} is in the zip but not in the manifest')
    # The window's own claim: how many capture frames are there.
    frames = [n for n in names if n.startswith('capture/frames/frame_')]
    want = m.get('window', {}).get('capture_frames', 0)
    if m.get('part_filter') != 'ours' and len(frames) != want:
        fails.append(f'FRAMES   the manifest says {want} capture frames, the zip holds {len(frames)}')
    # S101 lane 11: a zip that cannot replay whole says so loudly (the ring marks a draw it had no bytes for and counts it here).
    wn = m.get('window', {})
    if wn.get('unresolved', 0):
        fails.append(f"UNRESOLVED {wn['unresolved']} draw / content refs had no bytes in the ring (the replay shows them missing)")
    if wn.get('baseline_dropped', 0):
        fails.append(f"BASELINE {wn['baseline_dropped']} baseline textures were dropped for the cap (frame 0 replays without them)")
    if m.get('part_filter') != 'ours' and 'picture.png' not in names:
        fails.append('PICTURE  picture.png is missing: a GAME-part zip must hold what was on screen')
    return m


# S101 lane 19: every field the manifest must carry as a value or a reason (manifest.unavailable[path]). kind: 'str' (non-empty, not "unknown"),
# 'pos' (a number above 0), 'num' (any number, 0 allowed), 'wh' (width and height above 0), 'names' (the window's draw count).
REQUIRED = [
    ('build.git', 'str'), ('build.sdk_pin', 'str'), ('build.kind', 'str'), ('build.apk', 'pos'),
    ('device.gpu', 'str'), ('device.driver', 'str'), ('device.display', 'wh'), ('device.display.refresh_hz', 'pos'), ('device.render', 'wh'), ('device.render.scale', 'pos'), ('device.render.resolution_scale', 'pos'), ('where.address', 'str'), ('where.route', 'str'),
    ('device.battery_pct', 'num'), ('device.thermal', 'str'),
    ('settings.launcher_file', 'str'), ('way_back.builder', 'str'), ('way_back.seed', 'num'), ('way_back.inputs_rows', 'num'),
    ('names', 'names'),
]
TIMING_COLUMNS = ('step_ms', 'render_ms', 'present_ms', 'shown_ms')
_ABSENT = object()
REASON_KEY = {'device.display.refresh_hz': 'device.refresh_hz', 'device.render.resolution_scale': 'device.resolution_scale', 'device.render.scale': 'device.render'}  # where a field sits in the manifest vs the name its reason is filed under


def _dig(m, path):
    """The value at a dotted path, or _ABSENT when a key on the way is missing (a key holding null is None, not absent)."""
    cur = m
    for part in path.split('.'):
        if not isinstance(cur, dict) or part not in cur:
            return _ABSENT
        cur = cur[part]
    return cur


def _field_value(m, path, kind):
    """Returns (present, problem): present = the value is real; problem = a text when the manifest has the field malformed or missing."""
    if kind == 'wh':
        w, h = _dig(m, path + '.width'), _dig(m, path + '.height')
        if w is _ABSENT or h is _ABSENT:
            return False, 'missing'
        return bool(isinstance(w, (int, float)) and isinstance(h, (int, float)) and w > 0 and h > 0), None
    if kind == 'names':
        v = _dig(m, 'names.draws')
        return (v is not _ABSENT and v is not None), ('missing' if v is _ABSENT else None)
    v = _dig(m, path)
    if v is _ABSENT:
        return False, 'missing'
    if v is None:
        return False, None
    if kind == 'str':
        return isinstance(v, str) and v.strip() != '' and v != 'unknown', None
    if kind == 'pos':
        return isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0, None
    return isinstance(v, (int, float)) and not isinstance(v, bool), None


def check_fields(m, fails):
    """Every required manifest field has a real value, or a non-empty reason in manifest.unavailable. A missing key, an empty GPU name or a
    number forced to 0 where 'unmeasured' is meant all FAIL."""
    if 'unavailable' not in m:  # S101 lane 19 review: a zip written before lane 19 is read as such, not failed
        return True
    un = m.get('unavailable')
    if not isinstance(un, dict):
        fails.append('FIELD    manifest "unavailable" is not an object')
        un = {}
    for path, kind in REQUIRED:
        if path == 'where.address' and (m.get('version') or 1) < 2:  # S101 lane 32: a version 1 manifest has no where block to check
            continue
        present, problem = _field_value(m, path, kind)
        if problem == 'missing':
            fails.append(f'FIELD    {path} is missing from the manifest')
        elif not present:
            reason = un.get(REASON_KEY.get(path, path) if kind != 'names' else 'names')
            if not isinstance(reason, str) or not reason.strip():
                fails.append(f'FIELD    {REASON_KEY.get(path, path)} has no value and no reason in "unavailable"')
    for path, reason in un.items():
        if isinstance(reason, str) and reason.startswith('PARSE FAILED'):  # review fix 5: the renderer ran but its log lines were not understood
            fails.append(f'PARSE    unavailable[{path}]: {reason.strip()}')
        if not isinstance(reason, str) or not reason.strip():
            fails.append(f'FIELD    unavailable[{path}] has an empty reason')


def check_picture(dest, fails):
    """picture.png must be a whole PNG: it ends with its IEND chunk and decodes completely (the Bryan zip of Check A held a truncated one)."""
    p = dest / 'picture.png'
    if not p.exists():
        return
    data = p.read_bytes()
    if data[:8] != bytes([0x89]) + b'PNG' + bytes([13, 10, 26, 10]) or data[-12:] != bytes([0, 0, 0, 0]) + b'IEND' + bytes([0xAE, 0x42, 0x60, 0x82]):
        fails.append(f'PICTURE  picture.png is truncated or not a PNG ({len(data)} bytes, no IEND chunk at its end)')
        return
    try:
        from PIL import Image
        with Image.open(p) as im:
            im.load()  # a full decode; raises on a truncated stream
    except ImportError:
        pass
    except Exception as e:
        fails.append(f'PICTURE  picture.png does not decode fully ({e})')


def check_timing(dest, m, fails):
    """timing.csv: a measured cell is above 0 (a 0 stands for 'unmeasured', which is an empty cell); a column with no cell at all needs a
    reason in manifest.unavailable['timing.<column>']."""
    p = dest / 'timing.csv'
    if not p.exists():
        return
    rows = list(csv.DictReader(io.StringIO(p.read_text(encoding='utf-8'))))
    un = m.get('unavailable') if isinstance(m.get('unavailable'), dict) else {}
    for col in TIMING_COLUMNS:
        cells = [r.get(col, '') for r in rows]
        measured = [c for c in cells if c not in ('', None)]
        zeros = [c for c in measured if _is_zero(c)]
        if zeros:
            fails.append(f'TIMING   {col}: {len(zeros)} cell(s) are 0 (an unmeasured cell is empty, never 0)')
        if rows and not measured and 'unavailable' in m and not (isinstance(un.get('timing.' + col), str) and un['timing.' + col].strip()):
            fails.append(f'TIMING   {col}: no cell is measured and the manifest gives no reason (unavailable["timing.{col}"])')


def _is_zero(cell):
    try:
        return float(cell) == 0.0
    except ValueError:
        return False


def not_ours(m):
    """Why this zip does not come from a stamped build of ours (empty list = it does): an unknown git hash, no SDK pin."""
    why = []
    b = m.get('build') or {}
    git = b.get('git')
    if not isinstance(git, str) or git.strip() in ('', 'unknown'):
        why.append('unknown git hash')
    pin = b.get('sdk_pin')
    if not isinstance(pin, str) or pin.strip() in ('', 'unknown'):
        why.append('no SDK pin')
    elif pin.startswith('unpinned'):
        why.append(f'SDK DLLs not on the pin list ({pin})')
    return why


def run(cmd, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr)


def replay(rplay, capture_dir, dest, fails, notes):
    """Returns (validation_errors or None, last picture path or None)."""
    if rplay is None:
        # S101 lane 11: no rplay is a FAIL (a zip nobody replayed is not checked); --no-replay is the explicit way to skip
        fails.append('REPLAY   rplay.exe not found (tools\\renderer\\build.ps1 builds it): the capture was NOT replayed (pass --no-replay to skip on purpose)')
        return None, None
    last = dest / 'replay_last.png'
    code, out = run([str(rplay), '--capture', str(capture_dir), '--validation', '1', '--out', str(last)], 600)
    errors = 0
    for line in out.splitlines():
        if 'validation errors' in line:
            try:
                errors = int(line.rsplit('validation errors', 1)[1].split()[0])
            except Exception:
                pass
    if code != 0:
        fails.append(f'REPLAY   rplay exit {code} (4 = validation errors): ' + ' | '.join(out.strip().splitlines()[-3:]))
        errors = errors or None
    stats = [l for l in out.splitlines() if l.startswith('[rplay] capture:')]
    if stats:
        notes.append(stats[0].replace('[rplay] ', ''))
    return errors, (last if last.exists() else None)


def gallery(rplay, capture_dir, dest, notes):
    if rplay is None:
        return None
    gdir = dest / 'gallery'
    shutil.rmtree(gdir, ignore_errors=True)
    gdir.mkdir(parents=True)
    code, out = run([str(rplay), '--capture', str(capture_dir), '--gallery', str(gdir), '--out', str(dest / 'gallery_last.png')], 600)
    if code != 0 or not list(gdir.glob('*.png')):
        notes.append(f'gallery: rplay --gallery gave {len(list(gdir.glob("*.png")))} pictures (exit {code})')
        return None
    sheeter = find_tool('gallery_sheet', 'tools/renderer/gallery_sheet.py')
    if sheeter is None:
        notes.append('gallery: tools\\renderer\\gallery_sheet.py not found')
        return None
    code, out = run([sys.executable, '-I', str(sheeter), str(gdir)], 300)
    sheet = gdir / 'sheet.png'
    if code != 0 or not sheet.exists():
        notes.append('gallery: gallery_sheet.py failed: ' + out.strip()[-200:])
        return None
    shutil.copyfile(sheet, dest / 'sheet.png')
    return dest / 'sheet.png'


def timing_summary(dest):
    p = dest / 'timing.csv'
    if not p.exists():
        return None
    rows = list(csv.DictReader(io.StringIO(p.read_text(encoding='utf-8'))))
    steps = []
    for r in rows:
        try:
            steps.append((float(r['step_ms']), int(r['guest_frame'])))
        except (ValueError, KeyError):
            pass
    out = {'rows': len(rows), 'measured': len(steps)}
    for col in ('render_ms', 'present_ms'):  # S101 lane 19
        vals = sorted(float(r[col]) for r in rows if r.get(col) not in ('', None) and not _is_zero(r[col]))
        out[col] = (vals[len(vals) // 2], len(vals)) if vals else None
    if steps:
        ms = sorted(s[0] for s in steps)
        out['p50'] = ms[len(ms) // 2]
        out['p95'] = ms[min(len(ms) - 1, int(len(ms) * 0.95))]
        out['worst'] = sorted(steps, reverse=True)[:5]
        out['over'] = {t: sum(1 for x in ms if x >= t) for t in (25, 33, 50, 100)}
    return out


def _ids(name):
    """{id: name} from db/ids/<name>.json (our own id tables, in the repo; {} when absent)."""
    try:
        import json as _json
        with open(Path(__file__).resolve().parents[2] / 'db' / 'ids' / (name + '.json'), encoding='utf-8') as f:
            return {e['id']: e['name'] for e in _json.load(f).get('entries', []) if 'id' in e and 'name' in e}
    except Exception:
        return {}


def _goto_status(addr):
    """('plan' | 'not_reachable' | 'error' | 'exception', reason) from tools/goto's offline resolver."""
    try:
        import resolve as _resolve
        plan = _resolve.resolve(addr)
        return plan.status, plan.reason
    except Exception as e:  # a resolver that cannot run is the resolver's problem: a note, never a FAIL of the zip
        return 'exception', f'{type(e).__name__}: {e}'


def check_where(m, fails, notes):
    """S101 lane 32 (manifest version >= 2): where.address, when there is one, must have a basis, agree with where.match, parse, and (a basis that says
    "exact" on a match: / scene: address) resolve offline through tools/goto. A resolve error on a best-effort / menu / moment address, or a resolver
    exception, is a WARN note: "goto <address> returns here" is printed only when the status is `plan`. Returns the goto status (or None)."""
    w = m.get('where')
    if not isinstance(w, dict) or not w.get('address'):
        return None
    addr, basis = w['address'], (w.get('address_basis') or '').strip()
    if not basis:
        fails.append('WHERE    where.address has no address_basis (exact vs best-effort must be said)')
    mt = w.get('match') or {}
    if addr.startswith('match:type=') and (mt.get('wrestlers') or mt.get('type') is not None):  # a numeric spec must agree with the match block it was made with
        body = addr[len('match:'):].split('@')[0]
        kv = dict(p.partition('=')[::2] for p in body.split(';') if p)
        ids = [x.split(':')[0] for x in kv.get('p', '').split(',') if x]
        want = [str(i) for i in (mt.get('wrestlers') or [])]
        bad_ids = 'rumble' not in kv and ids != want[:len(ids)]  # a partial-seat spec names a prefix; a rumble spec's entrants are drawn
        bad_type = mt.get('type') is not None and kv.get('type') != str(mt['type'])
        bad_arena = mt.get('arena') is not None and 'arena' in kv and kv['arena'] not in (str(mt['arena']), '300')  # 300 = random
        if bad_ids or bad_type or bad_arena:
            fails.append(f'WHERE    where.address "{addr}" differs from where.match (type {mt.get("type")}, arena {mt.get("arena")}, wrestlers {mt.get("wrestlers")})')
    try:
        import address as _address
        _address.parse(addr)
    except Exception as e:
        fails.append(f'WHERE    where.address "{addr}" does not parse as a goto address: {e}')
        return None
    status, reason = _goto_status(addr)
    exact = basis.startswith('exact')
    if status == 'error' and exact and addr.split(':')[0] in ('match', 'scene'):
        fails.append(f'WHERE    where.address "{addr}" is labelled exact but does not resolve in tools/goto: {reason[:160]}')
    elif status in ('error', 'exception'):
        notes.append(f'WARN where.address "{addr}" ({"exact" if exact else "best-effort"}) does not resolve in tools/goto ({status}: {reason[:140]}): not a zip fault')
    return status


def where_words(m):
    w = m.get('where', {})
    parts = [w.get('words') or '(no words recorded)']
    mt = w.get('match', {})
    if mt.get('type') or mt.get('arena') or mt.get('wrestlers'):
        chars, arenas, types = _ids('characters'), _ids('arenas'), _ids('match_types')
        def nm(table, v):
            try:
                return f"{table[int(v)]} ({v})" if int(v) in table else str(v)
            except (TypeError, ValueError):
                return str(v)
        parts.append('match: ' + ', '.join(x for x in [nm(types, mt['type']) if mt.get('type') is not None else '', ('arena ' + nm(arenas, mt['arena'])) if mt.get('arena') is not None else ''] +
                                           [nm(chars, i) for i in (mt.get('wrestlers') or [])] if x))
    sc = w.get('scene') or {}
    if sc.get('bank') is not None:
        parts.append(f"scene bank {sc['bank']} playing (ring type {sc.get('ctx_type')})")
    steps = [s for s in (sc.get('entrance_steps') or []) if isinstance(s, int) and s > 0]
    if steps:
        parts.append('entrance steps ' + ', '.join(str(s) for s in sc['entrance_steps']))
    fz = w.get('forced')
    if fz:
        parts.append('FORCED SCENE %s %s at frame %s (reached by the scene director, not played to)' % (fz.get('verb'), fz.get('id'), fz.get('frame')))  # S101 lane32
    if w.get('address'):
        st = m.get('_where_status')
        parts.append(f"address {w['address']} ({w.get('address_basis') or 'no basis'}); " +
                     (f"goto {w['address']} returns here" if st == 'plan' else f"goto status {st or 'not checked'} (the address is recorded, a plan is not claimed)"))
    if w.get('route'):
        parts.append('route ' + w['route'])
    parts.append(f"guest frame {w.get('guest_frame')}")
    return '; '.join(parts)


def build_words(b):
    git = b.get('git') or 'unknown'
    pin = b.get('sdk_pin') or 'unknown'
    apk = f", APK {b['apk']}" if b.get('apk') else ''
    return f"Build {b.get('version') or 'unversioned'} git {git}{' (dirty)' if b.get('dirty') else ''} {b.get('kind') or 'unknown kind'}{apk}, SDK {pin}"


def switch_words(m, limit=8):
    s = m.get('settings') or {}
    eff = s.get('effective') or {}
    items = [f'{k}={v}' for k, v in eff.items()]
    if not items:
        what = 'non-default settings: none'
    else:
        what = 'non-default settings: ' + ', '.join(items[:limit]) + (f' (+{len(items) - limit} more, all in report.md)' if len(items) > limit else '')
    lf = s.get('launcher_file')
    return what + (f'; launcher file {lf}' if lf else '')


def device_words(d):
    def val(x, unit=''):
        return f'{x}{unit}' if x is not None and x != '' else 'n/a'
    disp, ren = d.get('display') or {}, d.get('render') or {}
    out = f"{d.get('platform') or 'unknown platform'} / {val(d.get('gpu'))} / {val(d.get('driver'))}"
    out += f"; display {disp.get('width') or 'n/a'}x{disp.get('height') or 'n/a'} @ {val(disp.get('refresh_hz'), ' Hz')}"
    out += f"; render {ren.get('width') or 'n/a'}x{ren.get('height') or 'n/a'} (draw scale x{ren.get('scale')}, RESOLUTION setting {val(ren.get('resolution_scale'))})"
    out += f"; battery {val(d.get('battery_pct'), '%')}, thermal {val(d.get('thermal'))}"
    return out


def intake(zip_path, out_root, rplay, do_replay):
    zip_path = Path(zip_path)
    if not zip_path.exists():
        print(f'FAIL  {zip_path} does not exist')
        return 1
    dest = Path(out_root) / zip_path.stem
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    fails, notes = [], []
    try:
        with zipfile.ZipFile(zip_path) as zf:
            bad = zf.testzip()
            if bad:
                raise Fail(f'zip CRC error in {bad}')
            m = check_manifest(zf, dest, fails)
            if check_fields(m, fails):  # S101 lane 19 (True: a pre-lane-19 manifest)
                notes.append('pre-lane-19 manifest: no "unavailable" object, so the build / device / settings fields are not checked')
            if (m.get('version') or 1) >= 2:  # S101 lane 32
                m['_where_status'] = check_where(m, fails, notes)
            else:
                notes.append('pre-lane-32 manifest: where not checked')
            zf.extractall(dest)
            check_timing(dest, m, fails)
            check_picture(dest, fails)
    except zipfile.BadZipFile as e:
        print(f'FAIL  {zip_path.name}: not a zip ({e})')
        return 1
    except Fail as e:
        print(f'FAIL  {zip_path.name}: {e}')
        (dest / 'report.md').write_text(f'# {zip_path.name}\n\nFAIL: {e}\n', encoding='utf-8')
        return 1

    errors, last_png = None, None
    capture = dest / 'capture'
    if do_replay and capture.exists() and not [f for f in fails if f.startswith(('MISSING', 'SIZE', 'HASH', 'FRAMES'))]:
        errors, last_png = replay(rplay, capture, dest, fails, notes)
        gallery(rplay, capture, dest, notes)
    elif capture.exists():
        notes.append('replay: skipped' + (' (--no-replay)' if not do_replay else ' (the manifest check failed)'))
    else:
        notes.append('replay: no capture\\ in this zip (a tester-safe zip)')
    if not (dest / 'sheet.png').exists():
        pic = dest / 'picture.png'
        if pic.exists():
            shutil.copyfile(pic, dest / 'sheet.png')
            notes.append('sheet.png is the zip\'s own picture (no gallery)')

    fview = find_tool('fview', 'out/renderer/dev/fview.exe')
    notes.append('fview: ' + ('present, op count not wired yet' if fview else 'not built yet: no op count'))
    t = timing_summary(dest)
    w = m.get('window', {})
    dev = m.get('device', {})
    build = m.get('build', {})
    names = m.get('names', {})

    status = 'PASS' if not fails else 'FAIL'
    stranger = not_ours(m)
    lines = [
        # S101 lane 19: the first line a reader sees names the build and the switches (#72: a report from another recomp; S83: the modified lines name
        # the switches to copy)
        (f"1. {status}: " + (f"NOT OUR BUILD ({', '.join(stranger)}): " if stranger else '') +
         f"{m.get('id')} ({m.get('trigger')}{', note: ' + m['note'] if m.get('note') else ''}), {m.get('created')}. {build_words(build)}; {switch_words(m)}."),
        f'2. Where: {where_words(m)}.',
        f"3. Window: {w.get('frames')} frames over {w.get('seconds', 0):.1f} s, {w.get('capture_frames')} in capture\\; "
        + (f'replay validation errors: {errors}.' if errors is not None else 'not replayed.'),
        '4. Timing: ' + (f"p50 {t['p50']:.1f} ms, p95 {t['p95']:.1f} ms, worst {t['worst'][0][0]:.1f} ms at frame {t['worst'][0][1]}; >=33 ms: {t['over'][33]}, >=50 ms: {t['over'][50]}; "
                        + (f"render p50 {t['render_ms'][0]:.1f} ms ({t['render_ms'][1]} frames)" if t.get('render_ms') else 'render not measured')
                        + (f", present gap p50 {t['present_ms'][0]:.1f} ms" if t.get('present_ms') else ', present gap not measured') + '; shown not measured.'
                        if t and t.get('measured') else 'no step times measured in this window.'),
        f'5. Device: {device_words(dev)}.',
    ]
    md = [f'# Case report: {zip_path.name}', ''] + lines + ['']
    if fails:
        md += ['## FAILS', ''] + [f'- {f}' for f in fails] + ['']
    md += ['## Details', '']
    md.append(f"- trigger `{m.get('trigger')}`, part filter `{m.get('part_filter')}`, created {m.get('created')} (UTC offset {m.get('utc_offset_min')} min)")
    wb = m.get('way_back', {})
    md.append(f"- way back: first frame {wb.get('first_frame')}, moment {wb.get('moment_frame')}, builder {wb.get('builder')}, seed {wb.get('seed')}, "
              f"replay mode {wb.get('replay_mode')}, inputs `{wb.get('inputs')}` ({wb.get('inputs_rows')} controller rows in the window)")
    eff = (m.get('settings') or {}).get('effective') or {}
    md.append('- settings off their defaults: ' + (', '.join(f'{k}={v}' for k, v in eff.items()) if eff else 'none') +
              f"; launcher settings file: {(m.get('settings') or {}).get('launcher_file')}")
    md.append(f"- device: {device_words(dev)}")
    unnamed = {k: v for k, v in names.items()}
    md.append('- names of the window: ' + (', '.join(f'{k} {v}' for k, v in unnamed.items()) if any(v is not None for v in unnamed.values()) else 'not recorded'))
    un = m.get('unavailable') or {}
    if un:
        md.append('- not available, with the reason: ' + '; '.join(f'{k}: {v}' for k, v in un.items()))
    if t and t.get('worst'):
        md.append('- worst frames: ' + ', '.join(f'{ms:.1f} ms @ {fr}' for ms, fr in t['worst']))
    for n in notes:
        md.append(f'- {n}')
    md.append('')
    md.append('Files: `states.txt` (every state value), `log.txt`, `inputs.rec`, `timing.csv`, `picture.png`, `capture\\` (open with rplay / fview), `sheet.png`.')
    (dest / 'report.md').write_text('\n'.join(md) + '\n', encoding='utf-8')
    print(f'{status}  {zip_path.name}: report {dest / "report.md"}')
    for f in fails:
        print('  ' + f)
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('zip', nargs='?')
    ap.add_argument('--watch')
    ap.add_argument('--interval', type=float, default=5.0)
    ap.add_argument('--once', action='store_true', help='with --watch: process what is there and return')
    ap.add_argument('--out-root', default=str(MAIN / 'out' / 'cases'))
    ap.add_argument('--rplay')
    ap.add_argument('--no-replay', action='store_true')
    a = ap.parse_args()
    if a.rplay:  # an rplay given that does not exist is "no rplay" (replay() then FAILs unless --no-replay)
        rplay = Path(a.rplay) if Path(a.rplay).exists() else None
    else:
        rplay = find_tool('rplay', 'out/renderer/dev/rplay.exe')
    if a.watch:
        watch = Path(a.watch)
        worst = 0
        while True:
            for z in sorted(watch.glob('*.zip')):
                done = z.with_name(z.name + '.done')
                if done.exists():
                    continue
                size = z.stat().st_size
                time.sleep(1.0)
                if z.stat().st_size != size:
                    continue  # still arriving
                code = intake(z, a.out_root, rplay, not a.no_replay)
                done.write_text(str(code), encoding='utf-8')
                worst = max(worst, code)
            if a.once:
                return worst
            time.sleep(a.interval)
    if not a.zip:
        ap.print_usage()
        return 2
    return intake(a.zip, a.out_root, rplay, not a.no_replay)


if __name__ == '__main__':
    sys.exit(main())
