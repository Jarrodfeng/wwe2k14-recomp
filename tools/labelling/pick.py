"""Session 10 Track B, step B1: pick the functions for the Haiku labelling pilot (SESSION10_PLAN.md).

    py tools\\labelling\\pick.py [--seed S] [--dry-run]

Writes out\\harness\\labelling\\pilot.json and pilot_summary.md (derived from the game: out\\ only). Deterministic:
the same inputs (coverage.json, the code index, db\\*.json, docs\\systems) and the same seed give byte-identical
files. There is no randomness; the seed only breaks ties (SHA-1 of "seed:address") and is recorded in pilot.json.

What it picks: 180 NEW functions in b01..b09 (20 each) + the 20 entries of db\\match_setup.json as b10 (they already
have labels A and B: a comparison set; export.py hides those labels from the labellers).

  eligible   ran in coverage (cov.py "all"), a sub_ function with a body (no import thunks, no save/restore helpers),
             no db\\*.json entry, not mentioned in docs\\systems or docs\\modules (already partly described there),
             8..250 instructions, no label re-syncs (inline jump-table data confuses the listing) and not
             overlapping the next function (exception funclets).
  cells      every batch gets the same mix of size bucket x outgoing-call bucket (CELLS below). Outgoing calls =
             distinct direct callees (as q.py shows them) + indirect call sites (bctrl): a function with 0 direct
             callees but a bctrl is not a leaf.
  spread     inside a cell the next pick is a candidate whose scenario key is still under its fair share in this
             batch (quota / 9, rounded up), then furthest below its quota overall (quotas ~ sqrt of the key's
             candidate count, at least 2), then least used in this batch, then whose 1 MB address region is least
             used (this batch, then overall), then the seeded hash. Batches are filled round-robin (slot k of every
             batch before slot k+1; the batch that picks first rotates), so they come out with similar mixes.
  scenario   key = "widespread" if it ran in >= 10 scenarios, else its rarest scenario (fewest functions), or
             "transition" if it ran only in to_* / quit windows (they belong to no scenario).
  CRT        up to one per batch from the block 0x82C78000..0x82C80000 (it holds the compiler's save/restore
             helpers and the db entries crt_rand, crt_srand, crt_time, crt_getptd_create_block: likely the C
             runtime, UNCONFIRMED for the picks themselves), at most 2 outgoing calls, >= 10 callers, most callers
             first. Useful because a memcpy/strlen-style purpose is checkable.
"""
import argparse
import collections
import hashlib
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
from common import h  # noqa: E402

SEED = 's10-pilot-1'
N_BATCHES = 9
PER_BATCH = 20
MIN_INSNS, MAX_INSNS = 8, 250
SIZE_BUCKETS = (('S', 8, 14), ('M', 15, 59), ('L', 60, 250))
# (size bucket, outgoing-call bucket, functions per batch): 20 per batch; S 4 (20%), M 11, L 5 (25%);
# 0 calls 9, 1-2 calls 7, 3+ calls 4 (20%).
CELLS = (('S', '0', 3), ('S', '1-2', 1),
         ('M', '0', 5), ('M', '1-2', 4), ('M', '3+', 2),
         ('L', '0', 1), ('L', '1-2', 2), ('L', '3+', 2))
WIDESPREAD = 10
CRT_LO, CRT_HI = 0x82C78000, 0x82C80000
CRT_MIN_CALLERS = 10
REGION_SHIFT = 20   # 1 MB address regions
IMAGE_BASE = 0x82000000


def size_bucket(n):
    for name, lo, hi in SIZE_BUCKETS:
        if lo <= n <= hi:
            return name
    return '-'


def call_bucket(n_out):
    return '0' if n_out == 0 else ('1-2' if n_out <= 2 else '3+')


def scenario_key(tags, scen_sizes):
    if not tags:
        return 'never_seen'
    if len(tags) >= WIDESPREAD:
        return 'widespread'
    known = [t for t in tags if t in scen_sizes]
    if not known:
        return 'transition'
    return min(known, key=lambda t: (scen_sizes[t], t))


def seeded(seed, a):
    return int(hashlib.sha1(f'{seed}:{a:08X}'.encode()).hexdigest()[:12], 16)


def describe(con, a, cov_funcs, scen_sizes, seed):
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (a,)).fetchone()
    ce = common.callees(con, a)
    n_out = len(ce) + f['n_bctrl']
    tags = cov_funcs.get(h(a), [])
    return {
        'addr': a, 'name': f['name'], 'insns': f['insns'], 'size': f['size'], 'resync': f['resync'],
        'has_body': f['has_body'], 'n_callees': len(ce), 'n_indirect': f['n_bctrl'], 'n_out': n_out,
        'n_callers': common.n_callers(con, a),
        'n_data_ptrs': con.execute("SELECT COUNT(*) FROM ptrs WHERE value = ? AND kind = 'func'", (a,)).fetchone()[0],
        'n_vtable_slots': con.execute('SELECT COUNT(*) FROM rtti_slot WHERE func = ?', (a,)).fetchone()[0],
        'tags': tags, 'key': scenario_key(tags, scen_sizes), 'region': (a - IMAGE_BASE) >> REGION_SHIFT,
        'sb': size_bucket(f['insns']), 'cb': call_bucket(n_out), 'hash': seeded(seed, a),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seed', default=SEED, help=f'tie-break seed (default {SEED}); recorded in pilot.json')
    ap.add_argument('--dry-run', action='store_true', help='print the tables, write nothing')
    args = ap.parse_args()

    con = common.connect()
    for w in common.index_warnings(con):
        print(f'pick: WARNING {w}', file=sys.stderr)
    cov = common.load_coverage()
    cov_funcs = cov['functions']
    scen_sizes = {s: v['count'] for s, v in cov['scenarios'].items()}
    db = common.db_entries()
    db_addrs = {a for a, _, _ in db}
    docs = common.docs_addresses()

    # next function start, for the overlap test
    starts = [r['addr'] for r in con.execute('SELECT addr FROM funcs WHERE has_body = 1 ORDER BY addr')]
    nxt = {a: b for a, b in zip(starts, starts[1:])}

    excluded = collections.Counter()
    elig = []
    for key in sorted(cov_funcs):
        a = int(key, 16)
        f = con.execute('SELECT name, insns, size, resync, has_body FROM funcs WHERE addr = ?', (a,)).fetchone()
        if f is None or not f['has_body'] or not f['name'].startswith('sub_'):
            excluded['not a sub_ function with a body'] += 1
        elif a in db_addrs:
            excluded['already in db\\*.json'] += 1
        elif a in docs:
            excluded['mentioned in docs\\systems or docs\\modules'] += 1
        elif not MIN_INSNS <= f['insns'] <= MAX_INSNS:
            excluded[f'outside {MIN_INSNS}..{MAX_INSNS} instructions'] += 1
        elif f['resync']:
            excluded['label re-syncs (inline jump-table data)'] += 1
        elif a in nxt and a + f['size'] > nxt[a]:
            excluded['overlaps the next function'] += 1
        else:
            elig.append(describe(con, a, cov_funcs, scen_sizes, args.seed))

    by_key = collections.Counter(c['key'] for c in elig)
    total_new = N_BATCHES * PER_BATCH
    norm = sum(math.sqrt(n) for n in by_key.values())
    quota = {k: max(2.0, total_new * math.sqrt(n) / norm) for k, n in by_key.items()}
    pools = collections.defaultdict(list)
    for c in elig:
        pools[(c['sb'], c['cb'])].append(c)

    room = {b: {(s, cb): n for s, cb, n in CELLS} for b in range(1, N_BATCHES + 1)}
    batches = {b: [] for b in range(1, N_BATCHES + 1)}
    batch_key = {b: collections.Counter() for b in batches}
    batch_region = {b: collections.Counter() for b in batches}
    key_used, region_used = collections.Counter(), collections.Counter()
    chosen = {}

    def take(c, b, note):
        c = dict(c)
        c['batch'] = b
        c['notes'] = note
        chosen[c['addr']] = c
        batches[b].append(c)
        room[b][(c['sb'], c['cb'])] -= 1
        batch_key[b][c['key']] += 1
        batch_region[b][c['region']] += 1
        key_used[c['key']] += 1
        region_used[c['region']] += 1

    # 1. CRT / library-style candidates, at most one per batch
    crt = sorted((c for c in elig if CRT_LO <= c['addr'] < CRT_HI and c['n_out'] <= 2
                  and c['n_callers'] >= CRT_MIN_CALLERS), key=lambda c: (-c['n_callers'], c['addr']))
    crt_batches = set()
    for c in crt:
        if len(crt_batches) == N_BATCHES:
            break
        for b in batches:
            if b not in crt_batches and room[b][(c['sb'], c['cb'])] > 0:
                take(c, b, f'crt_block_candidate: in 0x{CRT_LO:08X}..0x{CRT_HI:08X} next to the db crt_* entries, '
                           f'{c["n_callers"]} callers; likely C runtime / library (unconfirmed)')
                crt_batches.add(b)
                break

    # 2. fill every cell of every batch, round-robin over the batches (the batch that picks first rotates)
    share = {k: math.ceil(v / N_BATCHES) for k, v in quota.items()}   # a key's fair share per batch

    def score(c, b):
        k = c['key']
        return (batch_key[b][k] >= share[k], key_used[k] / quota[k], batch_key[b][k], batch_region[b][c['region']],
                region_used[c['region']], c['hash'])

    order = list(batches)
    rnd = 0
    for sb, cb, n in CELLS:
        for _ in range(n):
            rot = order[rnd % N_BATCHES:] + order[:rnd % N_BATCHES]
            rnd += 1
            for b in rot:
                if room[b][(sb, cb)] <= 0:
                    continue
                pool = [c for c in pools[(sb, cb)] if c['addr'] not in chosen]
                if not pool:
                    raise SystemExit(f'pick: no candidates left for cell {sb}/{cb}')
                take(min(pool, key=lambda c: score(c, b)), b, '')

    # 3. b10: the comparison set (db\match_setup.json), labels hidden from the labellers by export.py
    compare = [describe(con, a, cov_funcs, scen_sizes, args.seed) for a, fn, _ in db if fn == common.COMPARE_DB]
    for c in compare:
        c['batch'] = N_BATCHES + 1
        c['notes'] = f'comparison set: labelled in db\\{common.COMPARE_DB} (labels A and B in hidden_labels.json)'
    batches[N_BATCHES + 1] = compare

    def leaf_first(c):
        return (c['n_out'], c['n_callees'], c['insns'], c['addr'])

    functions = []
    for b in sorted(batches):
        for c in sorted(batches[b], key=leaf_first):
            notes = [c['notes']] if c['notes'] else []
            if c['n_callers'] == 0:
                notes.append(f'no direct callers (data words pointing at it {c["n_data_ptrs"]}, vtable slots '
                             f'{c["n_vtable_slots"]})')
            if c['n_indirect']:
                notes.append(f'{c["n_indirect"]} indirect call(s) (bctrl)')
            if b > N_BATCHES and (c['resync'] or not MIN_INSNS <= c['insns'] <= MAX_INSNS):
                notes.append('outside the new-function size / re-sync rules (comparison set is taken as is)')
            functions.append({
                'address': h(c['addr']), 'batch': f'b{b:02d}', 'n_instructions': c['insns'],
                'n_callees': c['n_callees'], 'n_indirect_calls': c['n_indirect'], 'n_callers': c['n_callers'],
                'scenarios': c['tags'], 'in_db': c['addr'] in db_addrs,
                'size_bucket': c['sb'], 'call_bucket': c['cb'], 'scenario_key': c['key'],
                'region_1mb': f'0x{IMAGE_BASE + (c["region"] << REGION_SHIFT):08X}',
                'notes': '; '.join(notes),
            })

    m = common.q.meta(con)
    doc = {
        'format': 'wwe2k14-labelling-pilot/1',
        'note': 'Session 10 Track B pilot selection (tools/labelling/pick.py). Derived from the game: out/ only.',
        'seed': args.seed,
        'inputs': {'coverage_built': cov.get('built'), 'index_codegen_fingerprint': m.get('codegen_fingerprint'),
                   'index_built_at': m.get('built_at'), 'db_entries': len(db),
                   'db_files': sorted({fn for _, fn, _ in db})},
        'criteria': {
            'eligible': f'ran in coverage, sub_ with a body, not in db, not in docs/systems|modules, '
                        f'{MIN_INSNS}..{MAX_INSNS} instructions, no label re-syncs, no overlap',
            'cells_per_batch': [{'size': s, 'calls': cb, 'n': n} for s, cb, n in CELLS],
            'size_buckets': {s: [lo, hi] for s, lo, hi in SIZE_BUCKETS},
            'calls': 'distinct direct callees (save/restore helpers hidden) + indirect call sites',
            'scenario_key': f'widespread (>= {WIDESPREAD} scenarios) | rarest scenario | transition',
            'scenario_quota': {k: round(v, 2) for k, v in sorted(quota.items())},
            'crt_block': [h(CRT_LO), h(CRT_HI)],
            'order_in_batch': 'leaf first: outgoing calls, direct callees, instructions, address',
        },
        'counts': {'ran_in_coverage': len(cov_funcs), 'eligible': len(elig), 'excluded': dict(excluded),
                   'eligible_by_scenario_key': dict(sorted(by_key.items())),
                   'eligible_by_cell': {f'{s}/{cb}': len(pools[(s, cb)]) for s, cb, _ in CELLS}},
        'batches': {f'b{b:02d}': [f['address'] for f in functions if f['batch'] == f'b{b:02d}']
                    for b in sorted(batches)},
        'functions': functions,
    }
    summary = render_summary(doc, functions)
    if args.dry_run:
        print(summary)
        return 0
    os.makedirs(common.OUT, exist_ok=True)
    with open(common.PILOT, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, indent=1)
        f.write('\n')
    with open(os.path.join(common.OUT, 'pilot_summary.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(summary)
    n_new = sum(1 for x in functions if not x['in_db'])
    print(f'pick: {n_new} new + {len(functions) - n_new} comparison functions in {len(doc["batches"])} batches '
          f'(eligible {len(elig)}); wrote {common.PILOT} and pilot_summary.md')
    return 0


def render_summary(doc, functions):
    bnames = list(doc['batches'])
    L = ['# Labelling pilot: selection (Session 10, Track B, B1)', '',
         'Written by `tools\\labelling\\pick.py` (deterministic; seed `' + doc['seed'] + '` breaks ties only). '
         'Derived from the game: stays in `out\\`.', '',
         '## How the functions were chosen', '',
         f'- Eligible: ran in coverage ({doc["counts"]["ran_in_coverage"]} functions in any window), a `sub_` function '
         f'with a body, no `db\\*.json` entry, not mentioned in `docs\\systems` / `docs\\modules`, '
         f'{MIN_INSNS}-{MAX_INSNS} instructions, no label re-syncs, not overlapping the next function: '
         f'**{doc["counts"]["eligible"]} eligible**.',
         '  Excluded: ' + ', '.join(f'{k} {v}' for k, v in doc['counts']['excluded'].items()) + '.',
         '- Every new batch (b01-b09) has the same cell mix, size bucket x outgoing calls (direct callees + indirect '
         'call sites): ' + ', '.join(f'{s}/{cb} x{n}' for s, cb, n in CELLS) +
         f'. Sizes: S {SIZE_BUCKETS[0][1]}-{SIZE_BUCKETS[0][2]}, M {SIZE_BUCKETS[1][1]}-{SIZE_BUCKETS[1][2]}, '
         f'L {SIZE_BUCKETS[2][1]}-{SIZE_BUCKETS[2][2]} instructions.',
         '- Inside a cell: a scenario key still under its fair share in the batch (quota / 9, rounded up), then the '
         'key furthest below its quota overall (sqrt-weighted), then the key least used in the batch, then the least '
         'used 1 MB address region, then the seeded hash. Batches filled round-robin, rotating which batch picks '
         'first.',
         f'- Scenario key: `widespread` (ran in >= {WIDESPREAD} scenarios), else the rarest scenario it ran in, or '
         '`transition` (only to_* / quit windows).',
         f'- CRT / library-style: one per batch from 0x{CRT_LO:08X}..0x{CRT_HI:08X} (next to the db `crt_*` entries; '
         f'>= {CRT_MIN_CALLERS} callers, <= 2 outgoing calls, most callers first). Likely C runtime, unconfirmed.',
         '- b10: the 20 entries of `db\\match_setup.json` (labels A and B exist; hidden from labellers, '
         'in `batches\\b10\\hidden_labels.json`). Taken as they are (sizes up to 1015 instructions).',
         '- Order inside a batch: leaf first (outgoing calls, direct callees, instructions, address).', '']

    def table(title, key, values):
        L.append(f'## {title}')
        L.append('')
        L.append('| batch | ' + ' | '.join(values) + ' |')
        L.append('|---|' + '---|' * len(values))
        for b in bnames:
            fs = [f for f in functions if f['batch'] == b]
            L.append(f'| {b} | ' + ' | '.join(str(sum(1 for f in fs if key(f) == v)) for v in values) + ' |')
        L.append('')

    table('Size bucket per batch', lambda f: f['size_bucket'], ['S', 'M', 'L', '-'])
    table('Outgoing-call bucket per batch', lambda f: f['call_bucket'], ['0', '1-2', '3+'])
    keys = sorted({f['scenario_key'] for f in functions})
    L.append('## Scenario key per batch')
    L.append('')
    L.append('| key | ' + ' | '.join(bnames) + ' | total |')
    L.append('|---|' + '---|' * (len(bnames) + 1))
    for k in keys:
        row = [sum(1 for f in functions if f['batch'] == b and f['scenario_key'] == k) for b in bnames]
        L.append(f'| {k} | ' + ' | '.join(str(x) for x in row) + f' | {sum(row)} |')
    L.append('')
    L.append('## Other per-batch figures')
    L.append('')
    L.append('| batch | functions | instructions min / median / max | 1 MB regions | CRT block | no direct callers |')
    L.append('|---|---|---|---|---|---|')
    for b in bnames:
        fs = [f for f in functions if f['batch'] == b]
        ins = sorted(f['n_instructions'] for f in fs)
        L.append(f'| {b} | {len(fs)} | {ins[0]} / {ins[len(ins) // 2]} / {ins[-1]} | '
                 f'{len({f["region_1mb"] for f in fs})} | {sum(1 for f in fs if "crt_block" in f["notes"])} | '
                 f'{sum(1 for f in fs if f["n_callers"] == 0)} |')
    L.append('')
    L.append('Eligible pool by cell: ' + ', '.join(f'{k} {v}' for k, v in doc['counts']['eligible_by_cell'].items())
             + '.')
    L.append('')
    L.append('Eligible pool by scenario key (quota): ' + ', '.join(
        f'{k} {v} ({doc["criteria"]["scenario_quota"][k]})' for k, v in doc['counts']['eligible_by_scenario_key'].items())
        + '.')
    L.append('')
    return '\n'.join(L)


if __name__ == '__main__':
    sys.exit(main())
