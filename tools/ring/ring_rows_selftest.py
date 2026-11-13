r"""S101 lane 194 gate check: Ring D3 pieces R0-R2 (the ring's per-call node rows in the scene record; docs\renderer\SKIP_GAME_WORK_MAP.md 16.7). Offline, 0 game runs, headless.

  py -I tools\ring\ring_rows_selftest.py [--no-build]

  1. builds out\ring\ring_rows_test.exe (tools\ring\build_ring_rows.ps1) and runs the unit test: the reader, the contributor and the record code against a fake guest BUILT FROM THE LISTINGS TABLE
     tools\ring\ring_call_sites.tsv (not the stream's values), the game's frames replayed in live order; the null-edit self-test (unedited = 0 differences, one flipped bit = exactly 1);
  2. the planted twins, each of which must fail its NAMED check: dedupe_model_r5, late_copy, endless_retry, dropped_frame_failure, offset0, late_record, ordinal_by_model_r5, alpha_zero, no_forward;
  3. the listings: every call site of the table is the vtable-slot load (or the bctrl) the table says, in the generated code of the main checkout (SKIP when the main checkout has no generated\ code);
  4. the Python reader (ledger_io Record.ring_rows) on the record the unit test wrote;
  5. ring_rows_check.py's pure parts on synthetic data: rows equal over the palette count, the slot -> node base vote, the frame offset vote (+1), and its plants (no_npal_cut = the stale-slot bug,
     re-found: the never-moving posts "mismatch" when the slots past the palette are compared; wrong_offset0; no_call_index).
Plants of the binder-side pieces (lod_flip_ordinal, canvas_marked, alpha_zero at c185) belong to R3 / R4 and are NOT built here (SKIP_GAME_WORK_MAP.md 16.7)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
sys.path.insert(0, HERE)
import ledger_io  # noqa: E402
import ring_rows_check as RC  # noqa: E402

TEST = os.path.join(ROOT, 'out', 'ring', 'ring_rows_test.exe')
SITES = os.path.join(ROOT, 'tools', 'ring', 'ring_call_sites.tsv')
SAMPLE = os.path.join(ROOT, 'out', 'ring', 'ring_rows_sample.rsc')
bad = 0


def check(name, ok, why=''):
    global bad
    print('%s  %-40s %s' % ('PASS' if ok else 'FAIL', name, why))
    bad += 0 if ok else 1


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


class FakeRec:
    def __init__(self, ents):
        self.ents = ents

    def ring_rows(self):
        return self.ents


def synthetic(base=1, shift=1, stale=True):
    """A fake capture: one ring part with a palette of [1, 2, 3] nodes (npal 3), 6 frames, 2 calls each. Entries are stamped (guest - shift); the stream bank holds c76..c84 = the palette slots' rows,
    and c85..c138 = STALE junk (the previous draw's rows) when `stale`. Returns (entries, draws [(guest, object, call, key)], banks {key: (bank dict, palette)})."""
    palette = [1, 2, 3]
    ents, draws, banks = [], [], {}
    for k in range(6):
        g = 2000 + k
        for call in range(2):
            rows = [[float(100 * k + 10 * call + n) + 0.25 * j for j in range(12)] for n in range(1, 4)]   # node n (1-based) at rows[n - 1]
            ents.append(dict(kind=1, layout=1, klass=6, object=0x50001000, frame=g - shift, call=call, keyed=call, r5=0, first_node=0, n_nodes=3, flags=0, alpha=0.5, rows=rows))
            bank = {}
            for slot in range(3):
                for row in range(3):
                    bank[76 + 3 * slot + row] = tuple(rows[palette[slot] - base][4 * row:4 * row + 4])
            for r in range(85, 139):
                bank[r] = (7.0 + r, -1.0, 3.0 * k, 0.5) if stale else (0.0, 0.0, 0.0, 0.0)
            key = (k, call)
            draws.append((g, 0x50001000, call, key))
            banks[key] = (bank, palette)
    return ents, draws, banks


def main():
    if '--no-build' not in sys.argv:
        rc, out = run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'ring', 'build_ring_rows.ps1')], 900)
        check('build', rc == 0, out.strip().splitlines()[-1] if out.strip() else '')
        if rc != 0:
            print(out[-1500:])
            return 1
    # 1. the unit test
    rc, out = run([TEST, '--sites', SITES, '--write-record', SAMPLE], 300)
    check('unit', rc == 0 and ' 0 failed' in out, out.strip().splitlines()[-1])
    # 2. the planted twins
    plants = (('dedupe_model_r5', ['hook.counts_per_group', 'hook.per_height_rows']), ('late_copy', ['hook.per_height_rows']), ('endless_retry', ['hook.unreadable_once']),
              ('dropped_frame_failure', ['hook.old_calls_not_failures']), ('offset0', ['contrib.live_order_join']), ('late_record', ['contrib.live_order_join']),
              ('ordinal_by_model_r5', ['contrib.cross_check']), ('alpha_zero', ['hook.alpha']), ('no_forward', ['hook.per_height_rows', 'hook.nothing_refused']))
    for plant, named in plants:
        rc, out = run([TEST, '--sites', SITES, '--plant', plant], 300)
        check('unit plant ' + plant, rc == 1 and all(('FAIL  ' + n) in out for n in named), 'exit %d, names %s' % (rc, ', '.join(named)))
    # 3. the listings
    main_co = main_checkout()
    lister = os.path.join(main_co, 'tools', 'analysis', 'list_function.py')
    if not os.path.isdir(os.path.join(main_co, 'generated', 'default')):
        print('SKIP  listings                                 (no generated\\default in %s)' % main_co)
    else:
        rows = [l.rstrip('\r\n').split('\t') for l in open(SITES, encoding='utf-8') if l.strip() and not l.startswith('#') and not l.startswith('class\t')]
        seen = set()
        for r in rows:
            caller, addr, slot = r[2], r[3], int(r[4])
            if int(addr, 16) == 0 or (caller, addr) in seen:
                continue
            seen.add((caller, addr))
            p = subprocess.run([sys.executable, '-I', lister, caller[4:]], capture_output=True, text=True, cwd=main_co, timeout=120)
            lines = {}
            for l in p.stdout.splitlines():
                m = re.match(r'(0x[0-9A-Fa-f]{8})\s+(.*)', l)
                if m:
                    lines[int(m.group(1), 16)] = m.group(2)
            a = int(addr, 16)
            win = [lines.get(a - d, '') for d in (0, 4, 8, 12)]
            hit = any(re.match(r'lwz r\d+,%d\(r\d+\)' % slot, w) for w in win)
            check('listing %s @%s' % (caller, addr), hit, 'vtable slot offset %d loaded at/before the cited address' % slot)
    # 4. the Python reader
    try:
        rec = ledger_io.Record(SAMPLE)
        rr = rec.ring_rows()
        ropes = [e for e in rr if e['klass'] in (1, 2)]
        check('reader.sample', len(rr) > 0 and len(ropes) > 0 and all(len(e['rows']) == e['n_nodes'] and len(e['rows'][0]) == 12 for e in rr if e['n_nodes']) and any(e['flags'] & 1 for e in ropes),
              '%d entries (%d ropes), rows per entry read, foreign flag on the ropes' % (len(rr), len(ropes)))
    except (OSError, ValueError, KeyError, IndexError) as e:
        check('reader.sample', False, str(e))
    # 5. ring_rows_check's pure parts
    ents, draws, banks = synthetic()
    table = RC.build_table([FakeRec(ents)])
    sample = [(next((e for e in table.get(g - 1, {}).get(obj, ()) if e['call'] == call), None), (lambda b: (lambda r: b[r]))(banks[key][0]), banks[key][1]) for g, obj, call, key in draws]
    base, votes = RC.vote_base(sample)
    check('check.base_vote', base == 1 and votes[1] == len(draws) and votes[0] == 0, 'base 1 (rows[node - 1]) wins: %s' % votes)
    by_frame = {}
    for g, obj, call, key in draws:
        by_frame.setdefault(g, []).append((obj, call, key))
    bank_of = lambda key: (lambda r: banks[key][0][r])  # noqa: E731
    pal_of = lambda key: banks[key][1]  # noqa: E731
    off, ov = RC.vote_offset(by_frame, table, 1, pal_of, bank_of)
    check('check.offset_vote', off == 1 and ov[1] == len(draws), 'measured +1: %s' % {k: v for k, v in ov.items() if v})
    v = [RC.compare_draw(next((e for e in table.get(g - 1, {}).get(obj, ()) if e['call'] == call), None), bank_of(key), pal_of(key), 1)[0] for g, obj, call, key in draws]
    check('check.rows_equal_over_palette', all(x == 'equal' for x in v), 'every draw equal over npal 3 although c85..c138 hold stale junk')
    vp = [RC.compare_draw(next((e for e in table.get(g - 1, {}).get(obj, ()) if e['call'] == call), None), bank_of(key), pal_of(key), 1, ('no_npal_cut',))[0] for g, obj, call, key in draws]
    check('check.plant no_npal_cut', any(x != 'equal' for x in vp), 'the stale slots counted: %d of %d draws now mismatch (the wrestlers\' stale-palette bug, re-found)' % (sum(1 for x in vp if x != 'equal'), len(vp)))
    off0, _ = RC.vote_offset(by_frame, table, 1, pal_of, bank_of)
    blind = [RC.compare_draw(next((e for e in table.get(g - 0, {}).get(obj, ()) if e['call'] == call), None), bank_of(key), pal_of(key), 1)[0] for g, obj, call, key in draws]
    check('check.plant wrong_offset0', off0 == 1 and not any(x == 'equal' for x in blind), 'joined at offset 0 no draw is equal (%d absent, %d mismatching)' % (blind.count('absent'), sum(1 for x in blind if x not in ('absent', 'equal'))))
    nc = [RC.compare_draw(next((e for e in table.get(g - 1, {}).get(obj, ()) if e['call'] == 0), None), bank_of(key), pal_of(key), 1)[0] for g, obj, call, key in draws]
    check('check.plant no_call_index', any(x != 'equal' for x in nc), 'every call takes the first entry: %d of %d draws mismatch' % (sum(1 for x in nc if x != 'equal'), len(nc)))
    # the unreadable / unknown entries are named, never "equal"
    e0 = dict(ents[0], flags=16, n_nodes=0, rows=[])
    e1 = dict(ents[0], flags=4, n_nodes=0, rows=[])
    k0 = draws[0][3]
    check('check.named_refusals', RC.compare_draw(e0, bank_of(k0), pal_of(k0), 1)[0] == 'unreadable' and RC.compare_draw(e1, bank_of(k0), pal_of(k0), 1)[0] == 'count_unknown' and RC.compare_draw(None, bank_of(k0), pal_of(k0), 1)[0] == 'absent',
          'unreadable / count_unknown / absent are verdicts of their own')
    rc, out = run([sys.executable, '-I', os.path.join(HERE, 'ring_rows_check.py'), os.path.join(ROOT, 'out', 'ring', 'does_not_exist'), '--arena', '4'])
    check('check.missing input fails loudly', rc == 1 and 'FAIL' in out, 'exit %d' % rc)
    print('ring_rows selftest: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
