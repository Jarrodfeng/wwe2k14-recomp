# Gate inputs (Session 101, lane 300): the unpacked captures the offline gates read, rebuilt from the KEPT case zips.
#   py -I tools\gate\inputs.py [list]                 # one line per input: present / MISSING, the zip it comes from, the gates that read it
#   py -I tools\gate\inputs.py restore [NAME ...]     # unpack what is missing (into <name>.part, renamed when whole: never half a capture)
#   py -I tools\gate\inputs.py verify [NAME ...]      # restore, then check the unpacked files are the whole capture (counts, sizes from manifest.json)
# Why: the 2026-11-13 16:1x disk clean-up deleted the unpacked captures under out\s101\lane92 (the case zips under out\s101\runs\<stamp> were kept),
# and the gates that read them SKIPped or FAILed with "input missing". The unpacked folders are a cache of the zips: this file is the table that says
# which zip makes which folder, so any gate (tools\gate\gate.py calls restore first) or person can rebuild them in seconds. Nothing is copied into a
# worktree: the folders are made in the MAIN checkout's out\ (game-derived, local only, hard rule 1).
# Each restored folder gets a KEEP file naming the gate files that read it (tools\harness\tidy.py: KEEP = never deleted; its reason says why).
# Every path here is also named in the gate lane jsons / check scripts, which tidy protects whatever their age (lane 294).
import argparse, json, os, shutil, subprocess, sys, zipfile
from pathlib import Path


def main_checkout():
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return Path(env)
    here = Path(__file__).resolve().parents[2]
    try:
        common = subprocess.run(['git', '-C', str(here), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True,
                                text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return here


MAIN = main_checkout()
RUNS = 'out/s101/runs'
LANE92 = 'out/s101/lane92'

# name -> (folder under the main checkout, the case zip it is unpacked from, what it must hold, the gate files that read it)
# "holds" is the requirement read from the gates (see docs/renderer/SKIP_GAME_WORK_MAP.md, "Gate inputs").
INPUTS = {
    'checkB': dict(
        dest=LANE92 + '/case_20261111_215233_217_0_pc_run_exit',
        zip=RUNS + '/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip',
        frames=341,
        holds='Check B, run 7051: Brock entrance, plain stream (no promotion arms but the owner), scene records + frames.',
        gates='tools/gate/lanes/caster_ledger.json, ring_ledger.json, arena_world.json (needs), tools/caster/caster_rows_selftest.py, '
              'tools/ui/ui_quads_check.py (Check B capture, frames 200-260), tools/census/leftovers.py'),
    'r7079': dict(
        dest=LANE92 + '/case_20261112_122658_040_0_pc_run_exit',
        zip=RUNS + '/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip',
        frames=381,
        holds='Run 7079: the lite-free 12:26 Brock capture (381 frames), arena + wrestlers + crowd arms on, no thin arms.',
        gates='tools/gate/lanes/arena_world.json, circles_ours.json, plane_shadow_ours.json, skip_arena_ready.json, thin_j1.json (optional), '
              'tools/caster/caster_rows_selftest.py'),
    'j1': dict(
        dest=LANE92 + '/case_20261112_172341_872_0_pc_run_exit',
        zip=RUNS + '/20261112_172304_s100_owner_s89_brock/case_20261112_172341_872_0_pc_run_exit.zip',
        frames=343,
        holds='Check J1, run 7086: Brock entrance with every promotion switch on (arena / wrestlers / ring thin), scene records, light states, cost counters.',
        gates='tools/gate/lanes/d3_caster.json, d3_calls_runs.json, d3_arena.json, d3_ring.json, thin_j1.json, tools/d3/d3_arena_calls_check.py, '
              'd3_check.py, d3_ring_check.py, d3_wrestler_check.py, tools/caster/caster_cam_vectors.py, tools/thin/thin_j1_check.py'),
    'r7149': dict(
        dest=LANE92 + '/case_20261113_093550_855_0_pc_run_exit',
        zip=RUNS + '/20261113_093518_s100_owner_s89_brock/case_20261113_093550_855_0_pc_run_exit.zip',
        frames=375,
        holds='Run 7149: Brock caster verify capture (caster records + wrestler bones), scene records.',
        gates='tools/gate/lanes/d3_wrestlers.json, d3_ring.json, tools/d3/d3_ring_check.py, d3_wrestler_check.py'),
}


def _present(dest):
    return (dest / 'manifest.json').is_file() and (dest / 'capture' / 'images.bin').is_file()


def _frames(dest):
    d = dest / 'capture' / 'scene'
    return sum(1 for _ in d.glob('frame_*.rsc')) if d.is_dir() else 0


def restore_one(name, quiet=False):
    spec = INPUTS[name]
    dest, zp = MAIN / spec['dest'], MAIN / spec['zip']
    if _present(dest):
        return 'present'
    if not zp.is_file():
        return 'FAIL zip missing: ' + str(zp)
    part = dest.with_name(dest.name + '.part')
    if part.exists():
        shutil.rmtree(part, ignore_errors=True)
    part.mkdir(parents=True)
    with zipfile.ZipFile(zp) as z:
        z.extractall(part)
    if dest.exists():   # a half folder (some other tool, an interrupted tidy)
        shutil.rmtree(dest, ignore_errors=True)
    part.rename(dest)
    keep = ('KEEP: gate input "%s" (lane 300): %s Unpacked from %s. Read by: %s. Rebuild any time with `py -I tools\\gate\\inputs.py restore`.'
            % (name, spec['holds'], spec['zip'], spec['gates']))
    (dest / 'KEEP').write_text(keep + '\n', encoding='utf-8')
    return 'restored'


def verify_one(name):
    spec = INPUTS[name]
    dest = MAIN / spec['dest']
    if not _present(dest):
        return False, 'not unpacked'
    try:
        m = json.loads((dest / 'manifest.json').read_text(encoding='utf-8'))
        listed = m.get('files') or []
    except Exception as e:
        return False, 'manifest.json unreadable (%s)' % e
    bad = []
    for f in listed if isinstance(listed, list) else []:
        rel = f.get('path') if isinstance(f, dict) else f
        sz = f.get('bytes') if isinstance(f, dict) and f.get('path') != 'manifest.json' else None   # the manifest lists itself with 0 bytes
        if not rel:
            continue
        p = dest / rel
        if not p.is_file():
            bad.append('missing ' + rel)
        elif sz is not None and p.stat().st_size != sz:
            bad.append('size ' + rel)
        if len(bad) > 3:
            break
    n = _frames(dest)
    if n < spec['frames']:
        bad.append('only %d of %d scene records' % (n, spec['frames']))
    return (not bad), ('%d scene records, manifest ok' % n if not bad else '; '.join(bad))


def restore_all(names=None, quiet=False):
    """Used by tools\\gate\\gate.py before the checks. Returns the list of (name, result)."""
    out = []
    for n in names or INPUTS:
        try:
            r = restore_one(n)
        except Exception as e:
            r = 'FAIL %s' % e
        out.append((n, r))
        if not quiet and r != 'present':
            print('[gate] input %s: %s' % (n, r), flush=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', nargs='?', default='list', choices=['list', 'restore', 'verify'])
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    names = a.names or list(INPUTS)
    for n in names:
        if n not in INPUTS:
            print('unknown input', n, '(known: %s)' % ', '.join(INPUTS))
            return 2
    rc = 0
    for n in names:
        spec = INPUTS[n]
        if a.cmd == 'list':
            print('%-6s %-8s %s  <- %s' % (n, 'present' if _present(MAIN / spec['dest']) else 'MISSING', spec['dest'], spec['zip']))
            print('       holds: %s\n       read by: %s' % (spec['holds'], spec['gates']))
        elif a.cmd == 'restore':
            r = restore_one(n)
            print('%-6s %s' % (n, r))
            rc |= r.startswith('FAIL')
        else:
            r = restore_one(n)
            ok, why = verify_one(n)
            print('%s  %-6s %s (%s)' % ('PASS' if ok else 'FAIL', n, why, r))
            rc |= (not ok)
    return rc


if __name__ == '__main__':
    sys.exit(main())
