"""Bulk labelling selection (after the Session 10 pilot): every function worth a Sonnet label, in labelling order.

    py tools\\labelling\\pick_bulk.py [--dry-run]

Writes (derived from the game: out\\ only, never committed) to out\\harness\\labelling\\bulk\\:

  selection.json             every eligible function: the ordered ones with wave / batch (w01b01 ...), then the
                             key-tagged ones (wave / batch null, key: true, key_reasons)
  key_for_main_session.json  the key-tagged functions only (CLAUDE.md: key systems go to the main session)
  selection_summary.md       counts per bucket, waves

Deterministic: the same inputs (coverage.json, the code index, db\\*.json, docs\\systems|modules, pilot.json) give
byte-identical files. No randomness, no seed.

  eligible   ran in coverage (any window: coverage.json "functions"), a function with a body in the index (no import
             thunks), not a compiler save/restore helper (__savegprlr_* etc.), no db\\*.json entry, not one of the 200
             pilot functions (pilot.json: handled separately), at least MIN_INSNS instructions. Unlike the pilot, label
             re-syncs (inline jump-table data) and functions overlapping the next one are kept (flagged in notes).
  key        tagged, not dropped, and kept out of the waves (written to key_for_main_session.json):
               docs     the address appears in docs\\systems\\*.md or docs\\modules\\*.md (sub_X / 0xX token)
               callers  it has at least one direct caller (itself not counted) and every direct caller has a db entry
                        in a key-system file (KEY_DB_FILES)
  order      the rest, leaf first: bucket "0" (no direct callees and no bctrl), "1-2" (direct callees + bctrl sites
             1..2), "3+"; inside a bucket the most-called first: distinct direct callers, then call sites, then address.
             Labelling the most-called leaves first makes later batches' callee digests (export 3b) show db names.
  waves      WAVE_BATCHES batches of BATCH_SIZE (200 functions) per wave, in order: w01b01 = functions 1..20, ...
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
from common import h, q  # noqa: E402

BULK = os.path.join(common.OUT, 'bulk')
SELECTION = os.path.join(BULK, 'selection.json')
MIN_INSNS = 3
BATCH_SIZE = 20
WAVE_BATCHES = 10
KEY_DB_FILES = ('camera.json', 'match_state.json', 'match_setup.json', 'ai_decision.json', 'video_tron.json',
                'cutscene_motion.json', 'file_loading.json')
BUCKETS = ('0', '1-2', '3+')
DOC_RX = re.compile(r'(?:sub_|0x)([0-9A-Fa-f]{8})\b')   # = common.docs_addresses()


def docs_mentions():
    """{address: [doc paths relative to the repo]} for docs\\systems\\*.md and docs\\modules\\*.md (the same tokens as
    common.docs_addresses)."""
    out = collections.defaultdict(list)
    for p in sorted(glob.glob(os.path.join(common.REPO, 'docs', 'systems', '*.md')) +
                    glob.glob(os.path.join(common.REPO, 'docs', 'modules', '*.md'))):
        rel = os.path.relpath(p, common.REPO)
        with open(p, encoding='utf-8', errors='replace') as f:
            for m in DOC_RX.finditer(f.read()):
                a = int(m.group(1), 16)
                if rel not in out[a]:
                    out[a].append(rel)
    return out


def call_bucket(n_out):
    return '0' if n_out == 0 else ('1-2' if n_out <= 2 else '3+')


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dry-run', action='store_true', help='print the summary, write nothing')
    args = ap.parse_args()

    con = common.connect()
    for w in common.index_warnings(con):
        print(f'pick_bulk: WARNING {w}', file=sys.stderr)
    cov = common.load_coverage()
    cov_funcs = cov['functions']
    db = common.db_entries()
    db_addrs = {a for a, _, _ in db}
    key_db = collections.defaultdict(list)    # caller address -> [(name, system, file)] in key-system files
    for a, fn, e in db:
        if fn in KEY_DB_FILES:
            key_db[a].append((e.get('name'), e.get('system'), fn))
    docs = docs_mentions()
    with open(common.PILOT, encoding='utf-8') as f:
        pilot = json.load(f)
    pilot_addrs = {int(x['address'], 16) for x in pilot['functions']}

    # the whole index in memory: one pass over funcs / edges / ptrs / rtti_slot instead of queries per function
    funcs = {r['addr']: r for r in con.execute('SELECT addr, name, insns, size, resync, has_body, n_bctrl FROM funcs')}
    helpers = {a for a, r in funcs.items() if q.HELPER_RE.match(r['name'] or '')}
    callees = collections.defaultdict(set)     # src -> distinct dst (helpers hidden, as q.py callees shows them)
    callers = collections.defaultdict(set)     # dst -> distinct src
    sites_in = collections.Counter()           # dst -> call sites
    for src, dst in con.execute('SELECT src, dst FROM edges'):
        if dst not in helpers:
            callees[src].add(dst)
        if dst is not None:
            callers[dst].add(src)
            sites_in[dst] += 1
    img = common.GuestImage()   # section lookup only: data words in .pdata are unwind entries, not references
    data_ptrs, pdata_ptrs = collections.Counter(), collections.Counter()
    for value, at in con.execute("SELECT value, at FROM ptrs WHERE kind = 'func'"):
        (pdata_ptrs if img.section(at)[0] == '.pdata' else data_ptrs)[value] += 1
    vt_slots = collections.Counter(r[0] for r in con.execute('SELECT func FROM rtti_slot'))
    starts = sorted(a for a, r in funcs.items() if r['has_body'])
    nxt = dict(zip(starts, starts[1:]))

    excluded = collections.Counter()
    elig = []
    for key in sorted(cov_funcs):
        a = int(key, 16)
        f = funcs.get(a)
        if f is None or not f['has_body']:
            excluded['no body in the index (import thunk)'] += 1
        elif a in helpers:
            excluded['compiler save/restore helper'] += 1
        elif a in db_addrs:
            excluded['already in db\\*.json'] += 1
        elif a in pilot_addrs:
            excluded['pilot function (pilot.json)'] += 1
        elif f['insns'] < MIN_INSNS:
            excluded[f'under {MIN_INSNS} instructions'] += 1
        else:
            n_callees = len(callees.get(a, ()))
            n_out = n_callees + f['n_bctrl']
            notes = []
            if f['resync']:
                notes.append('label re-syncs in the listing (inline jump-table data)')
            if a in nxt and a + f['size'] > nxt[a]:
                notes.append('overlaps the next function (exception funclet?)')
            if not callers.get(a):
                notes.append(f'no direct callers (data words pointing at it {data_ptrs[a]} + .pdata '
                             f'{pdata_ptrs[a]}, vtable slots {vt_slots[a]})')
            if f['n_bctrl']:
                notes.append(f'{f["n_bctrl"]} indirect call(s) (bctrl)')
            reasons = []
            if a in docs:
                reasons.append('docs: ' + ', '.join(docs[a]))
            up = callers.get(a, set()) - {a}
            if up and all(c in key_db for c in up):
                reasons.append('callers: all ' + str(len(up)) + ' direct caller(s) are db-labelled in key-system files: '
                               + ', '.join(f'{h(c)} ' + '/'.join(f'{n} [{s}] ({fn})' for n, s, fn in key_db[c])
                                           for c in sorted(up)[:6]) + (' ...' if len(up) > 6 else ''))
            elig.append({
                'address': h(a), 'n_instructions': f['insns'], 'n_callees': n_callees,
                'n_indirect_calls': f['n_bctrl'], 'n_out': n_out, 'bucket': call_bucket(n_out),
                'n_callers': len(callers.get(a, ())), 'n_call_sites': sites_in[a],
                'scenarios': cov_funcs[key], 'key': bool(reasons), 'key_reasons': reasons,
                'notes': '; '.join(notes), '_a': a,
            })

    rank = {b: i for i, b in enumerate(BUCKETS)}
    ordered = sorted((c for c in elig if not c['key']),
                     key=lambda c: (rank[c['bucket']], -c['n_callers'], -c['n_call_sites'], c['_a']))
    keyed = sorted((c for c in elig if c['key']),
                   key=lambda c: (rank[c['bucket']], -c['n_callers'], -c['n_call_sites'], c['_a']))
    per_wave = BATCH_SIZE * WAVE_BATCHES
    batches = collections.OrderedDict()
    for i, c in enumerate(ordered):
        w, b = i // per_wave + 1, (i % per_wave) // BATCH_SIZE + 1
        c['order'] = i + 1
        c['wave'] = f'w{w:02d}'
        c['batch'] = f'w{w:02d}b{b:02d}'
        batches.setdefault(c['batch'], []).append(c['address'])
    for c in keyed:
        c['order'], c['wave'], c['batch'] = None, None, None
    fields = ('address', 'order', 'wave', 'batch', 'bucket', 'n_instructions', 'n_callees', 'n_indirect_calls',
              'n_out', 'n_callers', 'n_call_sites', 'scenarios', 'key', 'key_reasons', 'notes')
    functions = [{k: c[k] for k in fields} for c in ordered + keyed]
    n_waves = len({c['wave'] for c in ordered})

    m = q.meta(con)
    by_bucket = {b: sum(1 for c in ordered if c['bucket'] == b) for b in BUCKETS}
    key_docs = sum(1 for c in keyed if any(r.startswith('docs:') for r in c['key_reasons']))
    key_callers = sum(1 for c in keyed if any(r.startswith('callers:') for r in c['key_reasons']))
    doc = {
        'format': 'wwe2k14-labelling-bulk/1',
        'note': 'Bulk labelling selection (tools/labelling/pick_bulk.py). Derived from the game: out/ only.',
        'inputs': {'coverage_built': cov.get('built'), 'index_codegen_fingerprint': m.get('codegen_fingerprint'),
                   'index_built_at': m.get('built_at'), 'db_entries': len(db),
                   'db_files': sorted({fn for _, fn, _ in db}), 'pilot_sha256': sha256(common.PILOT),
                   'docs_files': sorted({p for ps in docs.values() for p in ps})},
        'criteria': {
            'eligible': f'ran in coverage (any window), a function with a body (no import thunks), no save/restore '
                        f'helpers, no db\\*.json entry, not a pilot function, >= {MIN_INSNS} instructions',
            'key': 'mentioned in docs/systems|modules, or every direct caller (itself excluded, >= 1) db-labelled '
                   'in ' + ', '.join(KEY_DB_FILES) + '; kept out of the waves',
            'buckets': '0 = no direct callees and no bctrl; 1-2 / 3+ = direct callees (helpers hidden) + bctrl sites',
            'order': 'bucket (0, 1-2, 3+), then distinct direct callers desc, call sites desc, address',
            'waves': f'{WAVE_BATCHES} batches of {BATCH_SIZE} per wave',
        },
        'counts': {'ran_in_coverage': len(cov_funcs), 'eligible': len(elig), 'excluded': dict(excluded),
                   'key': len(keyed), 'key_docs': key_docs, 'key_callers': key_callers, 'ordered': len(ordered),
                   'ordered_by_bucket': by_bucket, 'waves': n_waves, 'batches': len(batches),
                   'last_wave_functions': len(ordered) - (n_waves - 1) * per_wave if ordered else 0},
        'batches': batches,
        'functions': functions,
    }
    key_doc = {
        'format': 'wwe2k14-labelling-bulk-key/1',
        'note': 'Key-system candidates for the main session (CLAUDE.md: key systems get two labels and main-session '
                'review). Not in any bulk wave. Derived from the game: out/ only.',
        'criteria': doc['criteria']['key'],
        'counts': {'key': len(keyed), 'docs': key_docs, 'callers': key_callers,
                   'by_bucket': {b: sum(1 for c in keyed if c['bucket'] == b) for b in BUCKETS}},
        'functions': [{k: c[k] for k in fields if k not in ('order', 'wave', 'batch', 'key')} for c in keyed],
    }
    summary = render_summary(doc, ordered, keyed)
    if args.dry_run:
        print(summary)
        return 0
    os.makedirs(BULK, exist_ok=True)
    for path, d in ((SELECTION, doc), (os.path.join(BULK, 'key_for_main_session.json'), key_doc)):
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(d, f, indent=1)
            f.write('\n')
    with open(os.path.join(BULK, 'selection_summary.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(summary)
    print(f'pick_bulk: eligible {len(elig)}: {len(ordered)} ordered in {n_waves} waves ({len(batches)} batches), '
          f'{len(keyed)} key-tagged; wrote {BULK}')
    return 0


def render_summary(doc, ordered, keyed):
    c = doc['counts']
    per_wave = BATCH_SIZE * WAVE_BATCHES
    L = ['# Bulk labelling selection', '',
         'Written by `tools\\labelling\\pick_bulk.py` (deterministic). Derived from the game: stays in `out\\`.', '',
         f'- Ran in coverage: {c["ran_in_coverage"]}. **Eligible: {c["eligible"]}** (a function with a body, no '
         f'save/restore helper, no db entry, not a pilot function, >= {MIN_INSNS} instructions).',
         '  Excluded: ' + ', '.join(f'{k} {v}' for k, v in c['excluded'].items()) + '.',
         f'- **Key-tagged: {c["key"]}** (docs\\systems|modules mention {c["key_docs"]}, all direct callers in '
         f'key-system db files {c["key_callers"]}; overlap counted in both): kept out of the waves, listed in '
         '`key_for_main_session.json`.',
         f'- **Ordered: {c["ordered"]}** in **{c["waves"]} waves** of {per_wave} ({WAVE_BATCHES} batches of '
         f'{BATCH_SIZE}; {c["batches"]} batches, last wave {c["last_wave_functions"]} functions).',
         '- Order: leaf first by bucket, then most-called first (distinct direct callers, call sites, address).', '',
         '| bucket | functions | waves | callers: max / median | instructions: median |',
         '|---|---|---|---|---|']
    for b in BUCKETS:
        fs = [x for x in ordered if x['bucket'] == b]
        if not fs:
            L.append(f'| {b} | 0 | - | - | - |')
            continue
        ws = sorted({x['wave'] for x in fs}, key=lambda w: int(w[1:]))
        nc = sorted(x['n_callers'] for x in fs)
        ni = sorted(x['n_instructions'] for x in fs)
        L.append(f'| {b} | {len(fs)} | {ws[0]}-{ws[-1]} | {nc[-1]} / {nc[len(nc) // 2]} | {ni[len(ni) // 2]} |')
    L.append('')
    kb = collections.Counter(x['bucket'] for x in keyed)
    L.append('Key-tagged by bucket: ' + ', '.join(f'{b} {kb[b]}' for b in BUCKETS) + '.')
    L.append('')
    n0 = sum(1 for x in ordered if x['n_callers'] == 0)
    L.append(f'Ordered functions with no direct callers: {n0} (export section 3c). With notes for label re-syncs: '
             f'{sum(1 for x in ordered if "re-syncs" in x["notes"])}.')
    L.append('')
    L.append('Wave w01: ' + ', '.join(f'{b} {sum(1 for x in ordered if x["wave"] == "w01" and x["bucket"] == b)}'
                                      for b in BUCKETS)
             + f'; callers {ordered[0]["n_callers"]} (first) to '
               f'{ordered[min(per_wave, len(ordered)) - 1]["n_callers"]} (last).' if ordered else 'No waves.')
    L.append('')
    return '\n'.join(L)


if __name__ == '__main__':
    sys.exit(main())
