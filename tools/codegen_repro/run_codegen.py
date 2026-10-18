"""Re-run ReXGlue codegen into a scratch folder and compare it with the current generated/default (Session 8, task 2).

    py tools\\codegen_repro\\run_codegen.py [--fresh] [--stamp NAME] [--rexglue EXE] [--against DIR] [--no-compare]

What it runs: the same rexglue.exe the build runs (read from out\\build\\win-amd64-release\\build.ninja, the codegen
custom command from generated\\rexglue.cmake; fallback: CMakeCache rexglue_DIR\\..\\..\\..\\bin) with the same
manifest (wwe2k14_manifest.toml + its config\\*.toml includes), into out\\harness\\codegen_repro\\<stamp>\\default\\.
generated\\ is never touched:
  - ReXGlue resolves file_path / game_root / out_directory_path / includes against the MANIFEST's folder
    (src/codegen/manifest.cpp, project_recompiler.cpp: configDir / path), and codegen also rewrites
    <manifest dir>\\generated\\rexglue.cmake and the manifest's sdk_version stamp (codegen_command.cpp). So the
    manifest is copied into the scratch folder with those four keys made absolute (the copy is then parsed and every
    other key checked equal to the original), and rexglue runs with the scratch folder as working directory.
  - By default the scratch output is seeded with generated\\default\\codegen.partition.json (which function goes in
    which recomp.N.cpp; ReXGlue keeps it stable across runs), so a deterministic codegen gives byte-identical files.
    --fresh starts without it (the function set and per-function code are still comparable; file contents not).
The codegen step (CPU-heavy) runs inside the run lock (tools\\runlock.ps1); the compare runs after it is released.
Outputs (game-derived, gitignored): out\\harness\\codegen_repro\\<stamp>\\ = manifest.toml, default\\, codegen.log,
codegen_stdout.txt, run.json, compare\\ (see compare.py). Exit: as compare.py (0 identical, 1 set/sizes differ, 3 same set+sizes but code differs), 2 error.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'harness'))
from runlock import ensure_lock  # noqa: E402
import compare as cmp  # noqa: E402

REWRITE = {('project', 'game_root'), ('entrypoint', 'file_path'), ('entrypoint', 'out_directory_path'),
           ('entrypoint', 'includes')}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def find_rexglue(preset='win-amd64-release'):
    ninja = os.path.join(REPO, 'out', 'build', preset, 'build.ninja')
    if os.path.exists(ninja):
        with open(ninja, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.search(r'&& (\S+rexglue\.exe) codegen (\S+)', line)
                if m:
                    return os.path.normpath(m.group(1)), m.group(2), 'build.ninja codegen command'
    cache = os.path.join(REPO, 'out', 'build', preset, 'CMakeCache.txt')
    with open(cache, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('rexglue_DIR:PATH='):
                d = line.split('=', 1)[1].strip()
                return os.path.normpath(os.path.join(d, '..', '..', '..', 'bin', 'rexglue.exe')), None, 'CMakeCache rexglue_DIR'
    raise SystemExit('run_codegen.py: cannot find rexglue.exe (no build.ninja codegen command, no rexglue_DIR)')


def pin_of(path):
    """The tools\\sdk_pins.json entries listing this file (name + sha256)."""
    sha = sha256(path)
    name = os.path.basename(path).lower()
    try:
        with open(os.path.join(REPO, 'tools', 'sdk_pins.json'), encoding='utf-8-sig') as f:
            pins = json.load(f)['pins']
    except (OSError, ValueError, KeyError) as e:
        return sha, f'pin list unreadable ({e})'
    ids = [p['id'] for p in pins if any(x['name'].lower() == name and x['sha256'].lower() == sha for x in p['files'])]
    return sha, ('pinned ' + '/'.join(ids)) if ids else 'UNPINNED'


def toml_str(s):
    return '"' + s.replace('\\', '/').replace('"', '') + '"'


def write_temp_manifest(src, dst, out_dir):
    """Copy the manifest with the path keys made absolute; verify everything else parses identically."""
    base = os.path.dirname(os.path.abspath(src))
    with open(src, 'rb') as f:
        orig = tomllib.load(f)
    with open(src, encoding='utf-8') as f:
        text = f.read()
    absp = lambda p: os.path.normpath(os.path.join(base, p))
    new = {
        ('project', 'game_root'): toml_str(absp(orig['project']['game_root'])),
        ('entrypoint', 'file_path'): toml_str(absp(orig['entrypoint']['file_path'])),
        ('entrypoint', 'out_directory_path'): toml_str(out_dir),
        ('entrypoint', 'includes'): '[' + ', '.join(toml_str(absp(p)) for p in orig['entrypoint'].get('includes', [])) + ']',
    }
    out_lines, section, done = [], None, set()
    for line in text.splitlines(keepends=True):
        m = re.match(r'^\s*\[\s*([A-Za-z0-9_.]+)\s*\]\s*(#.*)?$', line)
        if m:
            section = m.group(1)
        else:
            k = re.match(r'^(\s*)([A-Za-z_]+)\s*=', line)
            if k and (section, k.group(2)) in new:
                # The value must sit on this one line (true for this manifest; checked by the re-parse below).
                out_lines.append(f'{k.group(1)}{k.group(2)} = {new[(section, k.group(2))]}\n')
                done.add((section, k.group(2)))
                continue
        out_lines.append(line)
    missing = set(new) - done - ({('entrypoint', 'includes')} if 'includes' not in orig['entrypoint'] else set())
    if missing:
        raise SystemExit(f'run_codegen.py: could not rewrite {missing} in {src}')
    with open(dst, 'w', encoding='utf-8', newline='') as f:
        f.writelines(out_lines)
    with open(dst, 'rb') as f:
        copy = tomllib.load(f)
    for (sec, key), _ in new.items():
        if key in orig.get(sec, {}):
            ov, cv = orig[sec][key], copy[sec][key]
            if key == 'out_directory_path':
                assert os.path.normpath(cv) == os.path.normpath(out_dir), (cv, out_dir)
            elif key == 'includes':
                assert [os.path.normpath(x) for x in cv] == [absp(x) for x in ov], (cv, ov)
                assert all(os.path.exists(x) for x in cv), 'an include does not exist'
            else:
                assert os.path.normpath(cv) == absp(ov), (cv, ov)
            orig[sec].pop(key)
            copy[sec].pop(key)
    if orig != copy:
        raise SystemExit('run_codegen.py: the manifest copy differs from the original beyond the path keys')
    return {k[1]: v for k, v in new.items()}


def do_codegen(a):
    ensure_lock('codegen_repro ' + a.stamp)   # re-runs this step under tools\runlock.ps1 unless already held
    stamp_dir = os.path.join(REPO, 'out', 'harness', 'codegen_repro', a.stamp)
    out_dir = os.path.join(stamp_dir, 'default')
    if os.path.exists(out_dir):
        raise SystemExit(f'run_codegen.py: {out_dir} exists; pick another --stamp')
    os.makedirs(out_dir)
    exe, ninja_manifest, exe_src = (a.rexglue, None, '--rexglue') if a.rexglue else find_rexglue()
    manifest = os.path.join(REPO, 'wwe2k14_manifest.toml')
    if ninja_manifest and os.path.normcase(os.path.normpath(ninja_manifest)) != os.path.normcase(manifest):
        print(f'NOTE: build.ninja runs codegen on {ninja_manifest}, not {manifest}')
    temp_manifest = os.path.join(stamp_dir, 'manifest.toml')
    rewritten = write_temp_manifest(manifest, temp_manifest, out_dir)
    seeded = None
    if not a.fresh:
        seed = os.path.join(a.against, 'codegen.partition.json')
        if os.path.exists(seed):
            shutil.copy2(seed, os.path.join(out_dir, 'codegen.partition.json'))
            seeded = seed
    with open(manifest, 'rb') as f:
        man = tomllib.load(f)
    inputs = {os.path.relpath(p, REPO): sha256(p) for p in
              [manifest] + [os.path.join(REPO, x) for x in man['entrypoint'].get('includes', [])]}
    exe_sha, exe_pin = pin_of(exe)
    log = os.path.join(stamp_dir, 'codegen.log')
    cmd = [exe, '--log-file', log, 'codegen', temp_manifest, '--ignore-stamp']
    print(f'rexglue: {exe} ({exe_src}; sha256 {exe_sha[:12]}, {exe_pin})')
    print(f'manifest copy: {temp_manifest} (paths absolute; out -> {out_dir}); partition seed: {seeded or "none (fresh)"}')
    t0 = time.time()
    with open(os.path.join(stamp_dir, 'codegen_stdout.txt'), 'wb') as so:
        rc = subprocess.call(cmd, cwd=stamp_dir, stdout=so, stderr=subprocess.STDOUT)
    elapsed = time.time() - t0
    print(f'codegen exit {rc} after {elapsed:.0f} s')
    meta = {'stamp': a.stamp, 'when': time.strftime('%Y-%m-%dT%H:%M:%S'), 'rexglue': exe, 'rexglue_from': exe_src,
            'rexglue_sha256': exe_sha, 'rexglue_pin': exe_pin, 'command': cmd, 'cwd': stamp_dir,
            'manifest': manifest, 'manifest_rewrites': rewritten, 'inputs_sha256': inputs,
            'partition_seed': seeded, 'exit_code': rc, 'elapsed_s': round(elapsed, 1)}
    with open(os.path.join(stamp_dir, 'run.json'), 'w', encoding='utf-8') as f:
        json.dump(meta, f, indent=2)
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--stamp', default=time.strftime('%Y%m%d_%H%M%S'))
    ap.add_argument('--fresh', action='store_true', help='no partition seed (file contents will not match)')
    ap.add_argument('--rexglue', default=None, help='another rexglue.exe (default: the one the build runs)')
    ap.add_argument('--against', default=os.path.join(REPO, 'generated', 'default'), help='folder to compare with')
    ap.add_argument('--no-compare', action='store_true')
    ap.add_argument('--codegen-only', action='store_true', help=argparse.SUPPRESS)   # the locked child step
    a = ap.parse_args()
    # Fixer (review nit): the stamp becomes a folder under out/harness/codegen_repro; never let it escape (gitignore).
    if not re.fullmatch(r'[A-Za-z0-9_.-]+', a.stamp) or a.stamp in ('.', '..'):
        print(f'run_codegen.py: bad --stamp {a.stamp!r} (letters, digits, _ . - only)')
        return 2
    if a.codegen_only:
        return do_codegen(a)
    # Codegen in a child process that holds the run lock; compare afterwards, outside the lock.
    child = [sys.executable, os.path.abspath(__file__), '--codegen-only', '--stamp', a.stamp, '--against', a.against]
    if a.fresh:
        child.append('--fresh')
    if a.rexglue:
        child += ['--rexglue', a.rexglue]
    rc = subprocess.call(child)
    stamp_dir = os.path.join(REPO, 'out', 'harness', 'codegen_repro', a.stamp)
    if rc != 0:
        print(f'run_codegen.py: codegen failed (exit {rc}); see {stamp_dir}')
        return 2
    if a.no_compare:
        return 0
    code, _ = cmp.compare(a.against, os.path.join(stamp_dir, 'default'), os.path.join(stamp_dir, 'compare'))
    return code


if __name__ == '__main__':
    sys.exit(main())
