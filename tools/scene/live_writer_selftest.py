# The live scene writer's acceptance test (Session 101, lane 26; gate lane live_writer). Offline: no game, no GPU.
#   py -I tools\scene\live_writer_selftest.py [--root DIR]
# 1. builds out\scene\live_scene_selftest.exe (tools\scene\build_live_writer.ps1) when a source is newer
# 2. runs it (fake contributors, the folder sink, the case ring, planted faults, a full queue); every OK/FAIL line it prints must be OK
# 3. opens the case zip it wrote and checks the scene part with its own eyes: capture/scene/frame_NNNNNN.rsc for every capture/frames/frame_NNNNNN.bin,
#    each byte-identical to the record the test emitted for that frame's guest frame, the manifest lists each with its size
# 4. positive control: the same check on a copy of the zip with ONE byte of ONE scene record flipped must report a mismatch (a checker that
#    cannot fail proves nothing)
# Exit 0 = all passed.
import argparse, json, os, struct, subprocess, sys, zipfile
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
args = ap.parse_args()
REPO = Path(args.root).resolve()
EXE = REPO / 'out' / 'scene' / 'live_scene_selftest.exe'
OUT = REPO / 'out' / 'scene' / 'selftest_run'
FAILS = []


def check(ok, name, detail):
    print(('OK    ' if ok else 'FAIL  ') + name + ': ' + detail)
    if not ok:
        FAILS.append(name)


def run(cmd, timeout):
    try:
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout, cwd=str(REPO))
        return p.returncode, (p.stdout or '') + (p.stderr or '')
    except subprocess.TimeoutExpired:
        return 124, f'timed out after {timeout} s'


sources = [REPO / 'src' / 'renderer_scene' / n for n in ('live_scene.cpp', 'live_scene.h', 'live_sinks.cpp', 'live_sinks.h', 'live_scene_selftest.cpp', 'record_io.cpp', 'record_io.h', 'scene_record.h')]
sources += list((REPO / 'src' / 'case').glob('*')) + [REPO / 'tools' / 'scene' / 'build_live_writer.ps1']
newest = max(p.stat().st_mtime for p in sources if p.exists())
if not EXE.exists() or EXE.stat().st_mtime < newest:
    code, out = run(['pwsh', '-NoProfile', '-File', REPO / 'tools' / 'scene' / 'build_live_writer.ps1'], 900)
    check(code == 0 and EXE.exists(), 'build', (out.strip().splitlines() or ['no output'])[-1])
    if code != 0:
        print(out)
        sys.exit(1)
else:
    check(True, 'build', 'exe is current')

code, out = run([EXE, '--out', OUT], 300)
lines = out.splitlines()
oks = [l for l in lines if l.startswith('OK ')]
bad = [l for l in lines if l.startswith('FAIL ')]
for l in bad:
    print('      ' + l)
check(code == 0 and not bad and len(oks) >= 18, 'selftest.exe', f'exit {code}, {len(oks)} checks OK, {len(bad)} FAIL')
for l in lines:
    if l.startswith('OK off_is_free'):
        print('      ' + l)

# ---- the zip's scene part ----
exp = (OUT / 'expected.txt')
if not exp.exists():
    check(False, 'zip', 'expected.txt missing: the exe wrote no case zip')
    sys.exit(1)
zip_path, first_guest, frames, scene_records = exp.read_text().split('\n')[:4]
first_guest, frames, scene_records = int(first_guest), int(frames), int(scene_records)


def verify(zpath):
    """Returns (problems list, scene file count)."""
    problems = []
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        frame_names = sorted(n for n in names if n.startswith('capture/frames/frame_'))
        scene_names = sorted(n for n in names if n.startswith('capture/scene/frame_'))
        manifest = json.loads(z.read('manifest.json'))
        hashes = {f['path']: f for f in manifest.get('files', [])}
        if len(frame_names) != frames:
            problems.append(f'{len(frame_names)} frames in the zip, the exe reported {frames}')
        if len(scene_names) != len(frame_names):
            problems.append(f'{len(scene_names)} scene records for {len(frame_names)} frames')
        for fn in frame_names:
            idx = fn.rsplit('_', 1)[1].split('.')[0]
            sn = f'capture/scene/frame_{idx}.rsc'
            if sn not in names:
                problems.append(f'{sn} missing')
                continue
            fb = z.read(fn)
            guest = struct.unpack_from('<Q', fb, 16)[0]  # FrameFileHeader.guest_frame
            if guest != first_guest + int(idx):
                problems.append(f'{fn}: guest frame {guest}, expected {first_guest + int(idx)}')
            want = (OUT / 'expected' / f'frame_{guest}.rsc').read_bytes()
            got = z.read(sn)
            if got != want:
                problems.append(f'{sn}: bytes differ from the emitted record of guest frame {guest} ({len(got)} vs {len(want)})')
            if sn in hashes and hashes[sn].get('bytes') != len(got):
                problems.append(f'{sn}: manifest bytes {hashes[sn].get("bytes")} != {len(got)}')
        if not any(h.startswith('capture/scene/') for h in hashes):
            problems.append('the manifest lists no scene file')
    return problems, len(scene_names)


problems, n = verify(zip_path)
check(not problems and n == scene_records and n > 0, 'zip_scene_bytes', f'{n} scene records in the zip, all byte-identical to the emitted ones' if not problems else '; '.join(problems[:5]))

# ---- positive control: one flipped byte must be reported ----
tampered = OUT / 'tampered.zip'
with zipfile.ZipFile(zip_path) as zin, zipfile.ZipFile(tampered, 'w', zipfile.ZIP_STORED) as zout:
    flipped = False
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename.startswith('capture/scene/frame_') and not flipped:
            b = bytearray(data)
            b[-1] ^= 0xFF
            data = bytes(b)
            flipped = True
        zout.writestr(item, data)
tp, _ = verify(tampered)
check(bool(tp), 'positive_control', 'a zip with one scene byte flipped is reported: ' + (tp[0] if tp else 'NOT REPORTED (the checker cannot fail)'))

# ---- review fix 4: of two records for one guest frame the zip carries the newest ----
e2 = OUT / 'expected2.txt'
if e2.exists():
    with zipfile.ZipFile(e2.read_text().strip()) as z:
        got = z.read('capture/scene/frame_000000.rsc') if 'capture/scene/frame_000000.rsc' in z.namelist() else b''
    check(got == b'BBB', 'newest_record_in_zip', f'the zip holds {got!r} for guest frame 7000 (b"BBB" = the newer record)')
else:
    check(False, 'newest_record_in_zip', 'expected2.txt missing')

# ---- the reader: rsc_dump FOLDER --frame N (built here from record_io + rsc_dump, so the check never reads a stale exe) ----
dump = REPO / 'out' / 'scene' / 'rsc_dump_l26.exe'
code, o = run(['C:/Program Files/LLVM/bin/clang++.exe', '-O1', '-std=c++20', '-D_CRT_SECURE_NO_WARNINGS', REPO / 'tools' / 'scene' / 'rsc_dump.cpp', REPO / 'src' / 'renderer_scene' / 'record_io.cpp', '-o', dump], 600)
check(code == 0 and dump.exists(), 'rsc_dump_build', (o.strip().splitlines() or ['built'])[-1])
if dump.exists():
    code, o = run([dump, OUT / 'capture', '--frame', '5'], 60)
    check(code == 0 and 'guest_frame 1005' in o and 'objects 5' in o, 'reader_folder_frame', 'rsc_dump FOLDER --frame 5 -> guest_frame 1005, 5 objects' if code == 0 else o.strip())
    code, o = run([dump, OUT / 'capture', '--frame', '9999'], 60)
    check(code == 2 and 'no scene record' in o, 'reader_missing_frame', 'a frame with no record fails loudly (exit 2)' if code == 2 else f'exit {code}: {o.strip()}')

print('PASSED' if not FAILS else 'FAILED: ' + ', '.join(FAILS))
sys.exit(1 if FAILS else 0)
