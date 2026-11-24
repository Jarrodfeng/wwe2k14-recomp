#!/usr/bin/env python3
r"""S106 PSEED proof P1 (0 game runs): the census must contain every FrontEnd-family pipeline the game really built.
The game's side is a stored list from a real run (default: the S105 run-136 list, a main-checkout path held by tools\\harness\\keep_list.txt; its records are the full RecordKey, same
device: the renderer's own store). The census side is the per-job lists tools\\renderer\\pipeline_census.py wrote (DIR\\jobs\\*\\pipelines.bin). A FrontEnd-family record = one whose name
starts with yBG_ / yProp_ / yCh_ / yAudience / yParticle (names are kept in the store; a stored name is cut at 95 bytes, so names compare by prefix).
Verdicts (the exit code): 0 = every game record is in the census by FULL KEY (or is listed in --known-misses), 1 = a miss, or a planted twin was NOT caught.
  py -I tools\\renderer\\pseed_proof.py --census out\\pipeseed\\census [--game-list PATH] [--known-misses FILE] [--twins]
--twins runs the planted twins: the census with its map-arm jobs removed (jobs named *_m0 only) must FAIL on the _shadow names (arm b off), and the census with no roster jobs (c*_*) must
FAIL on the yCh names (arm d off). A proof that cannot fail is not a proof: the twins are the unsafe direction of this verdict (it must not say PASS on a census that lacks an arm).
Baseline measured by the probe before this build (docs\\research\\s106\\PSEED.md): 35 / 52 stage-family keys (mirrored copies counted) from the choice rig alone.
P2 (the loader, offline through the rig, 0 game runs): py -I tools\renderer\pseed_proof.py --p2 RECORD.rsc --seed LIST --game GAME_DIR (a fresh store with the seed builds fewer pipelines on demand and draws seed records;
the planted twin, every vertex hash flipped, must not help). rplay takes --pipeline-seed too (a capture-driven run for the main session)."""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'harvest'))
import pipeline_seed as P   # noqa: E402
import choice_ours   # noqa: E402  (main_checkout: where the S105 run-136 list lives, whichever checkout this runs from)

FE_PREFIX = (b'yBG_', b'yProp_', b'yCh_', b'yAudience', b'yParticle')


def default_game_list():
    return os.path.join(choice_ours.main_checkout(), 'out', 's105', 'chk9', 'b3cache', 'renderer_pipelines', 'pipelines.bin')


def name_of(rec):
    return rec[P.NAME:].split(b'\0')[0]


def load_census(census_dir, only=None):
    recs = []
    for path in sorted(glob.glob(os.path.join(census_dir, 'jobs', '*', 'pipelines.bin'))):
        tag = os.path.basename(os.path.dirname(path))
        if only and not only(tag):
            continue
        recs += P.decode_list(path)
    return recs


def verdict(game, census, known):
    """`known` = a set of FULL KEYS (P.key_of bytes; F3: a program NAME would hide every missing state of that program). Returns (fe records, misses, allowed misses)."""
    have = {P.key_of(r) for r in census}
    fe = [r for r in game if name_of(r).startswith(FE_PREFIX)]
    absent = [r for r in fe if P.key_of(r) not in have]
    miss = [r for r in absent if P.key_of(r) not in known]
    return fe, miss, [r for r in absent if P.key_of(r) in known]


def load_known(path):
    """--known-misses file: one line per allowed miss = the record's full key as hex (656 digits: `print_key` shows it), then anything (a comment). Names are NOT accepted."""
    known = set()
    with open(path, encoding='utf-8') as f:
        for no, ln in enumerate(f, 1):
            w = ln.split()
            if not w or w[0].startswith('#'):
                continue
            if len(w[0]) != P.RECORD * 2 or not re.fullmatch(r'[0-9a-fA-F]+', w[0]):
                raise SystemExit('%s:%d: a known miss is the full key in hex (%d digits), not a name' % (path, no, P.RECORD * 2))
            known.add(bytes.fromhex(w[0]))
    return known


COLD_RE = re.compile(r'pipeline built on demand: program (.+?), class \d+, ([0-9.]+) ms')


def cold_names(zip_path, member, min_ms=15.0):
    """Surface run 780's cold builds: the programs ('<vertex> + <fragment>') built on demand at >= min_ms AFTER `[turbo] match on` in the run's log (docs\\research\\s106\\PSEED.md)."""
    import zipfile
    with zipfile.ZipFile(zip_path) as z:
        lines = z.read(member).decode('utf-8', 'replace').splitlines()
    start = next((i for i, ln in enumerate(lines) if '[turbo] match on' in ln), None)
    if start is None:
        raise SystemExit('%s has no "[turbo] match on" line' % member)
    out = []
    for ln in lines[start:]:
        m = COLD_RE.search(ln)
        if m and float(m.group(2)) >= min_ms and m.group(1) not in out:
            out.append(m.group(1))
    return out


def cold_verdict(cold, census, allowed):
    """The census must hold a record for every cold program of 780 (names compare exactly; a stored name cut at 95 bytes compares by prefix). P1b checks program NAMES only, not their states (P1 checks the full keys).
    Returns (misses, allowed misses)."""
    names = {name_of(r).decode('latin1') for r in census}
    # a stored name is cut at 95 bytes (kNameBytes 96 with its NUL): only a 95-byte stored name can be a PREFIX of the cold name; any shorter one must match exactly (N4: a short census name
    # that merely starts the cold program's name used to cover it, and the reverse as well)
    absent = [c for c in cold if not any(c == n or (len(n) == 95 and c.startswith(n)) for n in names if n)]
    return [c for c in absent if c not in allowed], [c for c in absent if c in allowed]


def p2_verdict(none_keys, seed_keys, seeded_keys, none_demand, seeded_demand, seeded_drawn):
    """P2 (F5): the contract is 'on-demand 0 for the seed's keys'. required = the seed's keys the no-seed run had to build on demand; the seeded run must hold every one of them (prebuilt AND
    drawn) and build nothing else on demand than the no-seed run's other keys. Returns (ok, why)."""
    required = none_keys & seed_keys
    if not required:
        return False, 'the seed holds none of the keys the no-seed run built: the proof has nothing to prove'
    lost = required - seeded_keys
    if lost:
        return False, '%d of %d seed keys the no-seed run built on demand are not drawn from the seed in the seeded run' % (len(lost), len(required))
    if seeded_drawn < len(required):
        return False, 'only %d seed records drawn for %d required keys' % (seeded_drawn, len(required))
    if seeded_demand + len(required) > none_demand:
        return False, 'the seeded run built %d on demand; the no-seed run %d with %d of them in the seed: %d seed keys were still built on demand' % (seeded_demand, none_demand, len(required), seeded_demand + len(required) - none_demand)
    return True, '%d seed keys all prebuilt and drawn, 0 of them built on demand' % len(required)


def store_keys(store):
    keys = set()
    for r in P.decode_list(os.path.join(store, 'pipelines.bin')):
        n = P.neutral(r)
        if n is not None:
            keys.add(P.key_of(n))
    return keys


def seed_keys_of(text):
    keys = set()
    for ln in text.splitlines():
        if ln.startswith('R ') and len(ln.strip()) == 2 + P.RECORD * 2:
            keys.add(P.key_of(bytes.fromhex(ln[2:].strip())))
    return keys


def rig_p2(root, game, record, store, seed):
    """One frame_rig run on a fresh store (headless 160x90; the rig, not the game): returns (on-demand builds, seed records drawn, unresolved)."""
    os.makedirs(store, exist_ok=True)
    cmd = [os.path.join(root, 'out', 'tools', 'frame_rig.exe'), '--record', record, '--game', game, '--size', '160x90', '--prefix', os.path.join(store, 'o'),
           '--camera', '0.001,0,0,0,0,0.001,0,0,0,0,0.0001,0,0,0,0.5,1', '--pipeline-store', store]
    if seed:
        cmd += ['--pipeline-seed', seed]
    r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=600)
    m = re.search(r'on-demand builds (\d+) .*?prebuilt from the list \d+ \(.*?, (\d+) drawn this session.*?\((\d+) unresolved\)', r.stdout + r.stderr)
    if r.returncode != 0 or not m:
        raise RuntimeError('frame_rig exit %d, no store line: %s' % (r.returncode, (r.stdout + r.stderr)[-300:]))
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def make_twin(text, version):
    """The P2 planted twin: every R record's vertex hash flipped (nothing resolves), the H rdr line rebuilt over the flipped records (S106 PSEEDF2 N3: the old H line failed the loader's
    checksum, so the twin was refused before the 'resolves nothing' path it exists to test)."""
    recs = []
    for ln in text.split('\n'):
        ln = ln.rstrip(' \r')
        if ln.startswith('R ') and len(ln) == 2 + P.RECORD * 2:
            b = bytearray(bytes.fromhex(ln[2:]))
            b[56] ^= 0xFF
            recs.append(bytes(b))
    return P.rewrite_list(text, recs, version)


def twin_caught(none_keys, seed_keys, twin_keys, none_demand, twin_demand, twin_drawn):
    """The twin (a seed that resolves nothing) is caught when the P2 verdict says it does NOT help: the same p2_verdict the real seed must pass."""
    return not p2_verdict(none_keys, seed_keys, twin_keys, none_demand, twin_demand, twin_drawn)[0]


def p2(root, game, record, seed_list):
    """P2, the loader: with the seed, a fresh store builds fewer pipelines on demand (the seed's keys are prebuilt and drawn); the planted twin (every vertex hash flipped: nothing
    resolves) must NOT help. Exit 1 when the seed does not help or the twin does."""
    work = tempfile.mkdtemp(prefix='pseed_p2_', dir=os.path.join(root, 'out'))
    with open(seed_list, encoding='utf-8', errors='replace') as f:
        text = f.read()
    twin_path = os.path.join(work, 'twin.txt')
    with open(twin_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(make_twin(text, P.list_version(root)))
    none = rig_p2(root, game, record, os.path.join(work, 'none'), '')
    seeded = rig_p2(root, game, record, os.path.join(work, 'seeded'), seed_list)
    planted = rig_p2(root, game, record, os.path.join(work, 'twin'), twin_path)
    print('P2 no seed: %d on-demand builds; seeded: %d on-demand, %d seed records drawn; twin (hashes flipped): %d on-demand, %d drawn, %d unresolved' % (none[0], seeded[0], seeded[1], planted[0], planted[1], planted[2]))
    # F5: every key of the seed that the no-seed run built on demand must be prebuilt and drawn in the seeded run (not 'at least one build fewer')
    ok, why = p2_verdict(store_keys(os.path.join(work, 'none')), seed_keys_of(text), store_keys(os.path.join(work, 'seeded')), none[0], seeded[0], seeded[1])
    print('  ' + why)
    caught = planted[0] >= none[0] and planted[1] == 0 and twin_caught(store_keys(os.path.join(work, 'none')), seed_keys_of(text), store_keys(os.path.join(work, 'twin')), none[0], planted[0], planted[1])
    print('  seed helps: %s; twin (a seed that resolves nothing) %s' % ('yes' if ok else 'NO', 'does not help (caught)' if caught else 'HELPS: the proof cannot tell a dead seed from a live one'))
    return 0 if ok and caught else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--census')
    ap.add_argument('--p2', metavar='RECORD', help='the loader proof on this scene record (arena_rec output) with --seed LIST and --game DIR; needs out/tools/frame_rig.exe built from this tree')
    ap.add_argument('--seed', help='the shader_list.txt with R lines (P2)')
    ap.add_argument('--game', help='game_data/extracted of the main checkout (P2)')
    ap.add_argument('--game-list', default='', help='default: the S105 run-136 list under the main checkout (out\\s105\\chk9\\b3cache\\renderer_pipelines\\pipelines.bin)')
    ap.add_argument('--known-misses', default='', help='a file of FULL KEYS in hex (one per line, then a comment) the census is known not to draw yet (GPU-skinned ring pairs: arm c); each is printed when used')
    ap.add_argument('--surface-zip', default='', help="Surface run 780's bench zip: every cold program (>= 15 ms after match on) must have a census record")
    ap.add_argument('--surface-member', default='warm_1/wwe2k14_780.log')
    ap.add_argument('--known-cold', default='', help='a file of cold program names (one per line, exact) allowed to be missing (printed when used)')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--twins', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.p2:
        return p2(os.path.normpath(os.path.join(HERE, '..', '..')), a.game, a.p2, a.seed)
    if not a.census:
        ap.error('--census DIR (P1) or --p2 RECORD (P2)')
    known = load_known(a.known_misses) if a.known_misses else set()
    game = P.decode_list(a.game_list or default_game_list())
    census = load_census(a.census)
    fe, miss, allowed = verdict(game, census, known)
    print('P1: %d of %d FrontEnd-family keys of the game list are in the census (%d known misses allowed)' % (len(fe) - len(miss) - len(allowed), len(fe), len(allowed)))
    for r in allowed:
        print('  ALLOWED MISS %s  key %s...' % (name_of(r).decode('latin1'), P.key_of(r).hex()[:24]))
    for r in miss[:40]:
        print('  MISS %s  key %s' % (name_of(r).decode('latin1'), P.key_of(r).hex()))
    bad = bool(miss)
    if a.surface_zip:
        cold = cold_names(a.surface_zip, a.surface_member)
        allowed_cold = set()
        if a.known_cold:
            with open(a.known_cold, encoding='utf-8') as f:
                allowed_cold = {ln.strip() for ln in f if ln.strip() and not ln.startswith('#')}
        cmiss, callowed = cold_verdict(cold, census, allowed_cold)
        print("P1b: %d of Surface 780's %d cold programs have a census record (%d known misses allowed)" % (len(cold) - len(cmiss) - len(callowed), len(cold), len(callowed)))
        for c in callowed:
            print('  ALLOWED COLD MISS ' + c)
        for c in cmiss:
            print('  COLD MISS ' + c)
        bad = bad or bool(cmiss)
    if a.twins:
        _, m_b, _ = verdict(game, load_census(a.census, only=lambda t: t.endswith('_m0')), known)
        caught_b = any(b'_shadow' in name_of(r) for r in m_b)
        print('twin arm b off (no map-arm jobs): %s' % ('caught (%d misses, the _shadow names among them)' % len(m_b) if caught_b else 'NOT CAUGHT: the proof would pass a census with no map arm'))
        _, m_d, _ = verdict(game, load_census(a.census, only=lambda t: not t.startswith('c')), known)
        caught_d = any(name_of(r).startswith(b'yCh_') for r in m_d)
        print('twin arm d off (no roster jobs): %s' % ('caught (%d misses, yCh names among them)' % len(m_d) if caught_d else 'NOT CAUGHT: the proof would pass a census with no character arm'))
        bad = bad or not (caught_b and caught_d)
    print('pseed_proof: ' + ('FAILED' if bad else 'ok'))
    return 1 if bad else 0


def selftest():
    """The unsafe direction of each verdict of the proof: a known miss that hides other states, a partial loader, a census with the 780 programs missing."""
    fails = []

    def need(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            fails.append(what)

    def rec(name, cull=1, depth=P.DEPTH_TOKEN):
        r = bytearray(P.synth(0xAAAA, 0xBBBB, name=name, depth=depth))
        r[5] = cull   # DrawState byte 5 = cull (api.h DrawState; bytes 0-1 are `program`, zero in a stored record): a state field the key covers
        return bytes(r)

    census = [rec(b'yBG_x', 1)]
    game = [rec(b'yBG_x', 1), rec(b'yBG_x', 2)]   # the same program in two states: the census drew one
    fe, miss, allowed = verdict(game, census, set())
    need(len(fe) == 2 and len(miss) == 1, 'P1: the census missing one STATE of a program is a miss')
    fe, miss, allowed = verdict(game, census, {P.key_of(game[0])})   # the unsafe direction: a listed key of the OTHER state must not hide the missing one
    need(len(miss) == 1 and not allowed, 'F3: a known miss by full key hides only that key (another state of the same program still misses)')
    fe, miss, allowed = verdict(game, census, {P.key_of(game[1])})
    need(not miss and len(allowed) == 1, 'F3: the listed full key is allowed and reported')
    need(cold_verdict(['yBG_a_vertex + yBG_a_fragment'], [rec(b'yBG_b')], set())[0] == ['yBG_a_vertex + yBG_a_fragment'], 'F4: a cold program of 780 with no census record is a miss')
    need(cold_verdict(['yBG_a_vertex + yBG_a_fragment'], [rec(b'yBG_a_vertex + yBG_a_fragment')], set()) == ([], []), 'F4: a cold program with a census record is found (names compare by prefix)')
    seed = {b'k%d' % i for i in range(6)}
    none_keys = set(seed) | {b'other'}
    ok, _ = p2_verdict(none_keys, seed, seed | {b'other'}, 7, 1, 6)
    need(ok, 'F5: a loader that prebuilds and draws every seed key passes')
    ok, why = p2_verdict(none_keys, seed, set(list(seed)[:3]) | {b'other'}, 7, 4, 3)
    need(not ok, 'F5: a partial loader (half the keys, still fewer builds than no seed) FAILS')
    ok, why = p2_verdict(none_keys, seed, seed | {b'other'}, 7, 5, 6)
    need(not ok, 'F5: a seed key still built on demand FAILS')
    ok, why = p2_verdict({b'other'}, seed, {b'other'}, 1, 1, 0)
    need(not ok, 'F5: a seed that matches nothing FAILS')
    # S106 PSEEDF2 N4: names compare exactly, or by prefix only when the stored name is the 95-byte cut
    need(cold_verdict(['yBG_a_vertex + yBG_a_fragment'], [rec(b'yBG_a_vertex')], set())[0] == ['yBG_a_vertex + yBG_a_fragment'], 'N4 twin: a short census name that is only a prefix of the cold program does NOT cover it')
    cut = b'y' * 95
    need(cold_verdict([cut.decode() + ' + more'], [rec(cut)], set()) == ([], []), 'N4: a 95-byte (cut) census name covers the longer cold program it starts')
    # S106 PSEEDF2 N3: the P2 twin is rebuilt with a valid header; its verdict goes through p2_verdict
    good = [P.synth(0xAAAA, 0xBBBB, name=b'yBG_a'), P.synth(0xCCCC, 0, name=b'rdr_ui')]
    seed_text = 'S 1 2\n' + '\n'.join(P.seed_lines(good, 2)) + '\n'
    tw = make_twin(seed_text, 2)
    probs = P.check_text(tw, {0xAAAA, 0xBBBB, 0xCCCC}, 2)
    need(tw.startswith('S 1 2\n') and tw.count('R ') == 2 and not any('checksum' in p or 'count' in p or 'H rdr' in p for p in probs), 'N3: the planted twin carries a valid H rdr line (the loader reads it, not refuses it by checksum)')
    need(len([p for p in probs if 'not in the shader catalog' in p]) == 2, 'N3: ... and every vertex hash is flipped (nothing resolves)')
    k = {b'k%d' % i for i in range(3)}
    need(twin_caught(k | {b'o'}, k, k | {b'o'}, 4, 4, 0), 'N3: a twin that draws no seed record is caught')
    need(not twin_caught(k | {b'o'}, k, k | {b'o'}, 4, 1, 3), 'N3 twin: a twin that behaves like a live seed is NOT caught (the proof would say so)')
    print('pseed_proof selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'every check passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
