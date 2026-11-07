"""Session 92: prove from a run's LOG that every switch a check asked for engaged at run time.

  py -I tools\\harness\\feature_proof.py LOG [--expect cvar=value ...] [--from-log] [--json] [-v]

Why: a switch set on the command line only proves the value was set. Session 92 M5c: four owned_check runs had the TRUE
NATIVE latch flags but no `--wwe2k14_d3d_owned=true`, so no host queue, so the latch never engaged and the runs proved
nothing. The table tools\\harness\\feature_proofs.toml says, per switch, which log line proves it engaged (and its
requirements); this tool reads the log once and prints one line per feature:

  PROVEN       every proof line was found (numbers > 0 where the entry says), the requirements are proven too
  NOT ENGAGED  the switch is wanted (set in the log, or --expect) but a proof is missing, or a requirement is not proven
  NOT SEEN     a switch the DEFAULTS turned on (not --expect) whose entry says `scene = true`: its proof line needs a scene or a
               flag this route may not have (Session 95: the stack defaults; never fails the run)
  NO ENTRY     a wanted switch with no table entry (only for --expect, and for switches with a [meta] watch prefix that the
               log set: the table must grow; never fails the run)

Wanted = the switches the log's `modified: X = 'Y'` lines set to an "on" value (the last line per switch wins), plus every
--expect cvar=value (what the CHECK asked for: owned_check passes the on arm's flags, so a flag the run lost is NOT
ENGAGED). --expect cvar=off (false / 0) removes a switch from the wanted set. --from-log is the default (kept for callers
that want to say it). Exit 1 if any wanted feature is NOT ENGAGED, 2 on a usage error, else 0.
-v also prints the proof lines found. --json prints one JSON object (features, ok) instead of the text.
"""
import json
import os
import re
import sys

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, 'feature_proofs.toml')
MODIFIED = re.compile(r"modified: (\w+) = '([^']*)'")
OFF = {'', '0', 'false', 'off', 'no'}
NEEDLES = ('[d3d', '[pace]', '[native]', '[m6]', 'modified:', 'wakes', 'coherency',  # ([m6]: the translation store's open line)
           '[quirks]', 'left off (vulkan_', '[tex]', '[storage-window]', '[crowd]', '[eds]', '[post_swap]', '[scene_swap]', '[s94]', '[jobs]', '[s95]', '[mirror]', '[diag]', '[vfetch]', '[vinput]', '[sparse]', '[skin1]', '[wwe2k14][power]', '[wwe2k14][turbo]', '[wwe2k14][nf]', '[wwe2k14][idle]', '[wwe2k14][tick]', '[display]',  # Session 93 (+ Session 94: [s94]): the quirks / transcode / window / crowd proofs
           '[s59]', '[life]', '[hang]', '[vkpc]', '[mem]', '[dynres]', '[perf_hint]', 'SYNC-HAZARD')  # Session 95: the Android wave (LIFE, HANG, PLAIN, DEATH, PACE, TILER)


def load_table(path=TABLE):
    with open(path, 'rb') as f:
        t = tomllib.load(f)
    feats = t.get('feature', [])
    names = [x['name'] for x in feats]
    if len(set(names)) != len(names):
        raise ValueError('feature_proofs.toml: duplicate feature names')
    for x in feats:
        for r in x.get('requires', []):
            if r not in names:
                raise ValueError('feature_proofs.toml: %s requires unknown %s' % (x['name'], r))
        for p in x['proofs']:
            for rx in p['any']:
                re.compile(rx)
    return feats, t.get('meta', {})


def is_on(feat, value):
    v = str(value).strip()
    if 'bit' in feat:
        try:
            return (int(v, 0) & int(feat['bit'])) != 0
        except ValueError:
            return False
    on = [s.lower() for s in feat.get('on', [])]
    if '*' in on:  # any value that is set (a path or a name), for switches whose value is not a flag
        return v.lower() not in ('', 'false', '0', 'off', 'none')
    return v.lower() in on


def scan(log_path, feats):
    """One pass: the last `modified:` value per switch, and per (feature, proof, regex) the largest number / a match."""
    mods = {}
    rxs = []  # (feature name, proof index, compiled)
    for f in feats:
        for pi, p in enumerate(f['proofs']):
            for rx in p['any']:
                rxs.append((f['name'], pi, re.compile(rx), p.get('gt', 0)))
    found = {}  # (name, pi) -> (best number or None, line)
    with open(log_path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if not any(n in line for n in NEEDLES):
                continue
            m = MODIFIED.search(line)
            if m:
                mods[m.group(1)] = m.group(2)
                continue
            for name, pi, rx, gt in rxs:
                mm = rx.search(line)
                if not mm:
                    continue
                key = (name, pi)
                if rx.groups:
                    try:
                        n = int(mm.group(1))
                    except ValueError:
                        continue
                    if n > gt and (key not in found or found[key][0] is None or n > found[key][0]):
                        found[key] = (n, line.strip())
                elif key not in found:
                    found[key] = (None, line.strip())
    return mods, found


def parse_expect(items):
    out = {}
    for it in items:
        if '=' not in it:
            sys.exit('--expect wants cvar=value, got %r' % it)
        k, v = it.split('=', 1)
        out[k.lstrip('-').strip()] = v.strip()
    return out


def check(log_path, expect=None, table=TABLE):
    """-> dict(features=[{name, cvar, state, detail, evidence}], no_entry=[...], ok=bool)."""
    feats, meta = load_table(table)
    mods, found = scan(log_path, feats)
    wanted = dict(mods)
    expect = expect or {}
    for k, v in expect.items():
        wanted[k] = v
    by_name = {f['name']: f for f in feats}
    state = {}  # name -> (state, detail, evidence)

    def proven_own(f):
        details, evid = [], []
        for pi, p in enumerate(f['proofs']):
            hit = found.get((f['name'], pi))
            if hit is None:
                return False, 'no log line for: %s' % p['what'], evid
            n, line = hit
            details.append('%s%s' % (p['what'], ' %d' % n if n is not None else ''))
            evid.append(line)
        return True, '; '.join(details), evid

    def resolve(name, stack=()):
        if name in state:
            return state[name]
        f = by_name[name]
        ok, detail, evid = proven_own(f)
        if ok:
            for r in f.get('requires', []):
                if r in stack:
                    continue
                s = resolve(r, stack + (name,))
                if s[0] != 'PROVEN':
                    ok, detail = False, 'requires %s, which is not proven (%s)' % (r, s[1])
                    break
        state[name] = ('PROVEN' if ok else 'NOT ENGAGED', detail, evid)
        return state[name]

    rows = []
    for f in feats:
        if f['cvar'] not in wanted:
            continue
        v = wanted[f['cvar']]
        if not is_on(f, v):
            continue
        s = resolve(f['name'])
        row_state = 'NOT SEEN' if s[0] != 'PROVEN' and f.get('scene') and f['cvar'] not in expect else s[0]
        rows.append({'name': f['name'], 'cvar': f['cvar'], 'value': v, 'state': row_state, 'detail': s[1], 'evidence': s[2]})
    entry_cvars = {f['cvar'] for f in feats}
    prefixes = tuple(meta.get('watch_prefixes', []))
    no_entry = []
    for k, v in sorted(wanted.items()):
        if k in entry_cvars or v.lower() in OFF:
            continue
        if k in expect or (prefixes and k.startswith(prefixes)):
            no_entry.append('%s=%s' % (k, v))
    ok = all(r['state'] in ('PROVEN', 'NOT SEEN') for r in rows)
    return {'log': log_path, 'features': rows, 'no_entry': no_entry,
            'not_engaged': [r['name'] for r in rows if r['state'] == 'NOT ENGAGED'], 'ok': ok}


def main(argv):
    args = list(argv)
    verbose = '-v' in args
    as_json = '--json' in args
    args = [a for a in args if a not in ('-v', '--json', '--from-log')]
    expect_items = []
    while '--expect' in args:
        i = args.index('--expect')
        if i + 1 >= len(args):
            sys.exit('--expect wants cvar=value')
        expect_items.append(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    log = args[0]
    if not os.path.isfile(log):
        print('feature_proof: no such log: %s' % log)
        return 2
    if tomllib is None:
        print('feature_proof: needs Python 3.11+ (tomllib)')
        return 2
    res = check(log, parse_expect(expect_items))
    if as_json:
        print(json.dumps(res))
        return 0 if res['ok'] else 1
    for r in res['features']:
        print('%-12s %s (%s=%s): %s' % (r['state'], r['name'], r['cvar'], r['value'], r['detail']))
        if verbose:
            for e in r['evidence']:
                print('    | ' + e[:260])
    if res['no_entry']:
        ne = res['no_entry']
        shown = ne if verbose or len(ne) <= 4 else ne[:4] + ['... +%d more (-v lists all)' % (len(ne) - 4)]
        print('NO ENTRY     %d switch(es) set with no proof entry (add them to feature_proofs.toml): %s' %
              (len(ne), ', '.join(shown)))
    if not res['features']:
        print('feature_proof: no wanted switch with a table entry in %s' % os.path.basename(log))
    print('FEATURES %s (%d proven, %d not engaged)' % ('OK' if res['ok'] else 'NOT ENGAGED',
          sum(1 for r in res['features'] if r['state'] == 'PROVEN'), len(res['not_engaged'])))
    return 0 if res['ok'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
