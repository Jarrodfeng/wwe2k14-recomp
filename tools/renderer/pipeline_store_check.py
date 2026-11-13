"""Lane 276 (Session 101): the pipeline store, offline (rplay), 0 game runs. docs\\renderer\\SKIP_GAME_WORK_MAP.md section 19.10.
What it answers: a pipeline list + the driver's cache kept on disk make the second session's first frame cheap (no pipeline built on demand), the picture
stays byte-equal, and a bad / stale / foreign file is refused by name and costs only the on-demand builds it replaces.

Runs rplay on one capture (default s100_match300, 3 frames, validation 0), each in a fresh store folder under --out:
  record      session 1, empty store: N on-demand builds, a list of N and a cache written
  prebuild    session 2 (--prebuild before frame 0): on-demand builds 0, N prebuilt and N drawn, frame 0 render_us far below session 1's, picture equal to session 1's
  slices      session 2 without --prebuild: the time slices build some, every pipeline is either prebuilt or on demand (sum = N), picture equal
  cache-only  list deleted, cache kept: the on-demand builds are cheap (the cache hits), picture equal
  list-bad    flipped payload byte / truncated / wrong magic / wrong version: "list refused: <reason>" logged, N on-demand builds, picture equal, rc 0; the next session is clean again
  cache-bad   another vendor in the header / flipped blob byte / truncated / garbage: "cache refused: <reason>", picture equal, rc 0; the next session's cache loads
  stale       every record's shader hash changed (checksum rewritten): list loads, nothing prebuilt, N on-demand, picture equal; a record unused for > 12 sessions is dropped at the
              save, a recent stale one is kept
  plants      the same bad files with the guard switched off (--store-plant 1 / 2): the check must FAIL there (exit 0 of the plant lines = the check caught the missing guard)
usage: py -I tools\\renderer\\pipeline_store_check.py [--rplay EXE] [--capture DIR] [--out DIR] [--frames N]     exit 0 = every line passes"""
import argparse, os, re, shutil, struct, subprocess, sys

FNV_OFF, FNV_PRIME, M64 = 0xcbf29ce484222325, 0x100000001b3, (1 << 64) - 1
LIST_MAGIC, CACHE_MAGIC = 0x4C535057, 0x43535057
RECORD_BYTES = 56 + 16 + 4 + 16 * 8 + 8 + 2 + 10 + 4 + 4 + 96
HEADER = 40   # ListHeader: magic, version, session, count, payload, vendor, device, caps mask, checksum
NAME_AT = RECORD_BYTES - 96
SEEN_AT = RECORD_BYTES - 96 - 4
VHASH_AT = 56


def fnv(data, seed=FNV_OFF):
    h = seed
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M64
    return h


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    d = here
    while d and not os.path.isdir(os.path.join(d, 'out', 'captures')):
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return d


def run(cmd, timeout=600):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    return p.returncode, p.stdout + p.stderr


class Check:
    def __init__(self):
        self.fails = 0

    def line(self, ok, text):
        print(('PASS ' if ok else 'FAIL ') + text)
        if not ok:
            self.fails += 1


def parse_summary(log):
    m = re.search(r'at shutdown: on-demand builds (\d+) \(([\d.]+) ms\), prebuilt from the list (\d+) \(([\d.]+) ms, (\d+) drawn this session, (\d+) refused\), waiting (\d+) \((\d+) unresolved\), list loaded (\d+), cache loaded (\d+) bytes', log)
    if not m:
        return None
    k = ['on_demand', 'on_demand_ms', 'prebuilt', 'prebuilt_ms', 'drawn', 'refused', 'waiting', 'unresolved', 'list_loaded', 'cache_loaded']
    return dict(zip(k, [float(x) if '.' in x else int(x) for x in m.groups()]))


def frame0_render_us(csv):
    rows = open(csv).read().strip().splitlines()
    return float(rows[1].split(',')[4])


class Runner:
    def __init__(self, rplay, capture, out, frames):
        self.rplay, self.capture, self.out, self.frames, self.n = rplay, capture, out, frames, 0

    def session(self, store, extra=(), tag='run'):
        self.n += 1
        base = os.path.join(self.out, '%02d_%s' % (self.n, tag))
        cmd = [self.rplay, '--capture', self.capture, '--frames', str(self.frames), '--pipeline-store', store, '--timing', base + '.csv', '--out', base + '.png'] + list(extra)
        rc, log = run(cmd)
        open(base + '.log', 'w').write(log)
        return {'rc': rc, 'log': log, 'sum': parse_summary(log), 'png': base + '.png', 'f0': frame0_render_us(base + '.csv') if os.path.exists(base + '.csv') else -1}


def same_picture(a, b):
    return os.path.exists(a) and os.path.exists(b) and open(a, 'rb').read() == open(b, 'rb').read()


def rewrite_list(path, fn):
    """Reads the list, lets fn(header_fields, [record bytearrays]) change it, writes it back with a valid checksum."""
    d = open(path, 'rb').read()
    magic, ver, session, count, payload, vendor, device, caps, checksum = struct.unpack('<IIIIIIIIQ', d[:HEADER])
    recs = [bytearray(d[HEADER + i * RECORD_BYTES:HEADER + (i + 1) * RECORD_BYTES]) for i in range(count)]
    session, recs = fn(session, recs)
    body = b''.join(bytes(r) for r in recs)
    open(path, 'wb').write(struct.pack('<IIIIIIIIQ', magic, ver, session, len(recs), len(body), vendor, device, caps, fnv(body)) + body)


def main():
    ap = argparse.ArgumentParser()
    root = repo_root()
    ap.add_argument('--rplay', default=os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'out', 'renderer', 'dev', 'rplay.exe'))
    ap.add_argument('--capture', default=os.path.join(root, 'out', 'captures', 's100_match300'))
    ap.add_argument('--out', default=os.path.join(os.getcwd(), 'out', 'l276_check'))
    ap.add_argument('--frames', type=int, default=3)
    a = ap.parse_args()
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    R, C = Runner(a.rplay, a.capture, a.out, a.frames), Check()

    def fresh(name):
        d = os.path.join(a.out, 'store_' + name)
        shutil.rmtree(d, ignore_errors=True)
        return d

    def clone(src, name):
        d = fresh(name)
        shutil.copytree(src, d)
        return d

    gold = fresh('gold')
    s1 = R.session(gold, tag='record')
    n = s1['sum']['on_demand'] if s1['sum'] else 0
    C.line(s1['rc'] == 0 and n >= 20 and os.path.exists(os.path.join(gold, 'pipelines.bin')) and os.path.exists(os.path.join(gold, 'pipeline_cache.bin')),
           'record: rc 0, %d on-demand builds (%.1f ms), list and cache written' % (n, s1['sum']['on_demand_ms'] if s1['sum'] else -1))
    if n < 20:
        print('stopping: nothing to compare against')
        return 1
    golden_list = open(os.path.join(gold, 'pipelines.bin'), 'rb').read()
    golden_cache = open(os.path.join(gold, 'pipeline_cache.bin'), 'rb').read()

    # prebuild
    st = clone(gold, 'prebuild')
    s2 = R.session(st, ['--prebuild'], 'prebuild')
    S = s2['sum'] or {}
    C.line(s2['rc'] == 0 and S.get('on_demand') == 0 and S.get('prebuilt') == n and S.get('drawn') == n and S.get('refused') == 0,
           'prebuild: on-demand %s, prebuilt %s, drawn %s, refused %s (wanted 0 / %d / %d / 0)' % (S.get('on_demand'), S.get('prebuilt'), S.get('drawn'), S.get('refused'), n, n))
    C.line(0 < s2['f0'] < 0.25 * s1['f0'], 'prebuild: frame 0 render %.1f ms against session 1\'s %.1f ms (wanted < 25 %%)' % (s2['f0'] / 1000, s1['f0'] / 1000))
    C.line(same_picture(s1['png'], s2['png']), 'prebuild: picture byte-equal to session 1\'s')
    print('     frame-0 render_us: session 1 %.0f, prebuilt session %.0f; builds: %.1f ms on demand -> %.1f ms prebuilt' % (s1['f0'], s2['f0'], s1['sum']['on_demand_ms'], S.get('prebuilt_ms', -1)))

    # slices
    st = clone(gold, 'slices')
    s3 = R.session(st, [], 'slices')
    S = s3['sum'] or {}
    C.line(s3['rc'] == 0 and S and S['on_demand'] + S['prebuilt'] == n and S['prebuilt'] > 0 and same_picture(s1['png'], s3['png']),
           'slices: on-demand %s + prebuilt %s = %d, picture equal' % (S.get('on_demand'), S.get('prebuilt'), n))

    # cache only
    st = clone(gold, 'cacheonly')
    os.remove(os.path.join(st, 'pipelines.bin'))
    s4 = R.session(st, [], 'cacheonly')
    S = s4['sum'] or {}
    C.line(s4['rc'] == 0 and S and S['on_demand'] == n and S['cache_loaded'] > 0 and S['on_demand_ms'] < 0.5 * s1['sum']['on_demand_ms'] and same_picture(s1['png'], s4['png']),
           'cache only: %d on-demand builds cost %.1f ms against %.1f ms with no cache, picture equal' % (n, S.get('on_demand_ms', -1), s1['sum']['on_demand_ms']))

    # bad files
    def expect_refusal(label, mutate, what, plant=0):
        st = clone(gold, label)
        mutate(st)
        r = R.session(st, ['--prebuild'] + (['--store-plant', str(plant)] if plant else []), label)
        S = r['sum'] or {}
        refused = ('%s refused:' % what) in r['log']
        reason = re.search(r'%s refused: ([^\n(]*)' % what, r['log'])
        ok = r['rc'] == 0 and refused and same_picture(s1['png'], r['png'])
        if what == 'list':
            ok = ok and S and S['on_demand'] == n and S['prebuilt'] == 0
        return ok, (reason.group(1).strip() if reason else 'NOT refused'), r, st

    def flip_list_payload(st):
        p = os.path.join(st, 'pipelines.bin')
        d = bytearray(open(p, 'rb').read())
        d[HEADER + NAME_AT + 3] ^= 0x5A   # a byte of record 0's name: only the checksum notices it
        open(p, 'wb').write(d)

    def trunc_list(st):
        p = os.path.join(st, 'pipelines.bin')
        d = open(p, 'rb').read()
        open(p, 'wb').write(d[:len(d) // 2])

    def magic_list(st):
        p = os.path.join(st, 'pipelines.bin')
        d = bytearray(open(p, 'rb').read())
        d[0] ^= 0xFF
        open(p, 'wb').write(d)

    def version_list(st):
        p = os.path.join(st, 'pipelines.bin')
        d = bytearray(open(p, 'rb').read())
        d[4] = 9
        open(p, 'wb').write(d)

    def vendor_list(st):
        p = os.path.join(st, 'pipelines.bin')
        d = bytearray(open(p, 'rb').read())
        d[20] ^= 0x01   # the vendor id in the header (a list from another GPU)
        open(p, 'wb').write(d)

    def caps_list(st):
        p = os.path.join(st, 'pipelines.bin')
        d = bytearray(open(p, 'rb').read())
        d[28] ^= 0x04   # the format-caps mask in the header
        open(p, 'wb').write(d)

    for label, fn in (('list_flip', flip_list_payload), ('list_trunc', trunc_list), ('list_magic', magic_list), ('list_version', version_list), ('list_vendor', vendor_list), ('list_caps', caps_list)):
        ok, why, r, st = expect_refusal(label, fn, 'list')
        # the next session on that store must be clean again (the refused list was rewritten)
        nxt = R.session(st, ['--prebuild'], label + '_next')
        S = nxt['sum'] or {}
        ok2 = nxt['rc'] == 0 and S and S['on_demand'] == 0 and S['prebuilt'] == n
        C.line(ok and ok2, 'list-bad %s: refused (%s), %d on-demand builds, picture equal; next session clean (on-demand %s, prebuilt %s)' % (label, why, n, S.get('on_demand'), S.get('prebuilt')))

    def vendor_cache(st):
        p = os.path.join(st, 'pipeline_cache.bin')
        d = bytearray(open(p, 'rb').read())
        d[8] ^= 0x01   # the stored vendor id
        open(p, 'wb').write(d)

    def flip_cache_blob(st):
        p = os.path.join(st, 'pipeline_cache.bin')
        d = bytearray(open(p, 'rb').read())
        d[56 + 200] ^= 0xA5
        open(p, 'wb').write(d)

    def trunc_cache(st):
        p = os.path.join(st, 'pipeline_cache.bin')
        d = open(p, 'rb').read()
        open(p, 'wb').write(d[:1000])

    def junk_cache(st):
        open(os.path.join(st, 'pipeline_cache.bin'), 'wb').write(b'not a cache at all' * 50)

    for label, fn in (('cache_vendor', vendor_cache), ('cache_flip', flip_cache_blob), ('cache_trunc', trunc_cache), ('cache_junk', junk_cache)):
        ok, why, r, st = expect_refusal(label, fn, 'cache')
        nxt = R.session(st, ['--prebuild'], label + '_next')
        okn = nxt['rc'] == 0 and 'cache refused' not in nxt['log'] and (nxt['sum'] or {}).get('cache_loaded', 0) > 0
        C.line(ok and okn, 'cache-bad %s: refused (%s), picture equal; next session loads the rewritten cache' % (label, why))

    # one record out of range (checksum valid): that record alone is dropped, the rest load and are prebuilt
    st = clone(gold, 'badrecord')

    def bad_record(session, recs):
        recs[0][4] = 99   # DrawState.topology
        return session, recs

    rewrite_list(os.path.join(st, 'pipelines.bin'), bad_record)
    r = R.session(st, ['--prebuild'], 'badrecord')
    S = r['sum'] or {}
    C.line(r['rc'] == 0 and S and 'list refused' not in r['log'] and 'failed the range checks and were dropped' in r['log'] and S['prebuilt'] == n - 1 and S['on_demand'] == 1 and same_picture(s1['png'], r['png']),
           'one bad record: dropped alone (list not refused), %s prebuilt, %s on demand, picture equal' % (S.get('prebuilt'), S.get('on_demand')))

    # a scene is live: no slices (and the explicit prebuild is not asked), everything on demand
    st = clone(gold, 'scenelive')
    r = R.session(st, ['--scene-live'], 'scenelive')
    S = r['sum'] or {}
    C.line(r['rc'] == 0 and S and S['prebuilt'] == 0 and S['on_demand'] == n and same_picture(s1['png'], r['png']),
           'scene live: no slice ran (prebuilt %s, on demand %s of %d), picture equal' % (S.get('prebuilt'), S.get('on_demand'), n))

    # stale: every shader hash changed, the checksum valid
    st = clone(gold, 'stale')
    def stale(session, recs):
        for rec in recs:
            vh = struct.unpack('<Q', bytes(rec[VHASH_AT:VHASH_AT + 8]))[0] ^ 0x1234567
            rec[VHASH_AT:VHASH_AT + 8] = struct.pack('<Q', vh)
        return session, recs

    rewrite_list(os.path.join(st, 'pipelines.bin'), stale)
    r = R.session(st, ['--prebuild'], 'stale')
    S = r['sum'] or {}
    C.line(r['rc'] == 0 and S and S['list_loaded'] == n and S['prebuilt'] == 0 and S['on_demand'] == n and S['unresolved'] == n and same_picture(s1['png'], r['png']) and 'list refused' not in r['log'],
           'stale hashes: list loads (%s), %s unresolved, nothing prebuilt, %s on-demand builds, picture equal' % (S.get('list_loaded'), S.get('unresolved'), S.get('on_demand')))

    # ageing: a list of session 30 with record 0 stale and last seen in session 0 (dropped), record 1 stale and seen in session 25 (kept), every other record seen in 29
    st = clone(gold, 'aging')

    def age(session, recs):
        for i, rec in enumerate(recs):
            seen = 0 if i == 0 else 25 if i == 1 else 29
            rec[SEEN_AT:SEEN_AT + 4] = struct.pack('<I', seen)
            if i < 2:
                rec[VHASH_AT:VHASH_AT + 8] = struct.pack('<Q', 0xDEAD0000 + i)
        return 30, recs

    rewrite_list(os.path.join(st, 'pipelines.bin'), age)
    r = R.session(st, ['--prebuild'], 'aging')
    d = open(os.path.join(st, 'pipelines.bin'), 'rb').read()
    magic, ver, session, count = struct.unpack('<IIII', d[:16])
    # n real records (records 2..n-1 prebuilt; the two stale ones' real twins built on demand) + the kept stale record 1; record 0 (seen 0, 31 - 0 > 12) dropped
    C.line(r['rc'] == 0 and session == 31 and count == n + 1,
           'ageing: session %d (wanted 31), %d records saved (wanted %d: the old stale record dropped, the recent stale one kept)' % (session, count, n + 1))

    # plants: the guard off, the check must see it
    print('-- plants (each line passes when the check CATCHES the missing guard) --')
    ok, why, r, st = expect_refusal('plant_list', flip_list_payload, 'list', plant=1)
    C.line(not ok and why == 'NOT refused', 'plant 1 (list checksum unchecked): a flipped list byte is NOT refused -> the list-bad check fails there (%s)' % why)
    ok, why, r, st = expect_refusal('plant_cache', vendor_cache, 'cache', plant=2)
    C.line(not ok and why == 'NOT refused', 'plant 2 (cache device/checksum unchecked): another vendor is NOT refused -> the cache-bad check fails there (%s)' % why)

    print('RESULT: %s (%d failing lines)' % ('PASS' if not C.fails else 'FAIL', C.fails))
    return 1 if C.fails else 0


if __name__ == '__main__':
    sys.exit(main())
