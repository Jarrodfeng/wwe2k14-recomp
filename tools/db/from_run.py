"""Session 30 (#8): proposed function-database entries from a run's live findings.

  py tools/db/from_run.py RUN [RUN ...] [--coverage] [--diffcov B:A] [--out FILE] [--print]

Reads the run logs (out/build/win-amd64-release/logs/wwe2k14_RUN.log, or the coverage build's with --coverage) and
collects, per guest function:
  - runtime trace lines (--wwe2k14_trace): number of calls seen, callers (return addresses, "via" callers' callers,
    @callers summaries), sample argument registers r3-r10 and return values (numbers only), which registers / returns
    pointed at readable memory (the dumped bytes themselves are NOT copied: they are game data);
  - hardware-watch hits (script `watch ADDR`): the guest data the function writes, with the run and the hit count;
  - with --diffcov B:A (coverage runs): the functions that ran in coverage window B but not in A (tools/coverage/
    diffcov.py), as "ran while B, not while A".
It writes db/proposed/<runs>.json (a subfolder: the db tools read only db/*.json) in the db schema (CLAUDE.md "Function database") with name and summary left EMPTY and
confidence "unknown": the evidence is for the two-label / settle process (labellers A and B, then the settle pass), which
names the functions. It never edits db/*.json. An address that already has a db entry is marked with that entry's name.
"""
import argparse
import collections
import datetime
import glob
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGS = {'release': os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs'),
        'coverage': os.path.join(REPO, 'out', 'build', 'win-amd64-coverage', 'logs')}

TRACE_CALL = re.compile(r'\[wwe2k14\]\[trace\] (0x[0-9A-F]{8}) #(\d+) from (0x[0-9A-F]{8}) (r3=.*?) \(frame (\d+), '
                        r'thread \d+\)(?: via (0x[0-9A-F]{8}|\?))?(.*)$')
TRACE_RET = re.compile(r'\[wwe2k14\]\[trace\] (0x[0-9A-F]{8}) #\d+ -> r3=(0x[0-9A-F]+) f1=(\S+)(.*)$')
TRACE_CALLERS = re.compile(r'\[wwe2k14\]\[trace\] (0x[0-9A-F]{8}) callers over (\d+) calls \(\d+ distinct[^)]*\):(.*)$')
REG = re.compile(r'(r\d+)=(0x[0-9A-F]+)')
DEREF = re.compile(r'\*(r\d+)=0x[0-9A-F]+: (not readable|not a guest address we read|[0-9A-F]{2})')
WATCH_HIT = re.compile(r'\[wwe2k14\]\[watch\] hit #(\d+) (0x[0-9A-F]{8})/(\d+) (\S+) -> (\S+) frame (\d+) by '
                       r'(?:sub_|0x)?([0-9A-Fa-f]{8})')


def db_index():
    names = {}
    for p in glob.glob(os.path.join(REPO, 'db', '*.json')):
        if os.path.basename(p).startswith('proposed_'):
            continue
        try:
            data = json.load(open(p, encoding='utf-8'))
        except (OSError, ValueError):
            continue
        entries = data if isinstance(data, list) else data.get('functions', data.get('entries', []))
        if isinstance(entries, dict):
            entries = list(entries.values())
        for e in entries if isinstance(entries, list) else []:
            if isinstance(e, dict) and e.get('address'):
                names.setdefault(e['address'].upper().replace('0X', '0x'),
                                 {'name': e.get('name', ''), 'file': os.path.basename(p),
                                  'confidence': e.get('confidence', '')})
    return names


def new_fn():
    return {'calls': 0, 'callers': collections.Counter(), 'via': collections.Counter(),
            'args': collections.defaultdict(list), 'rets': [], 'deref': collections.Counter(),
            'ret_readable': 0, 'writes': collections.defaultdict(lambda: [0, set()]), 'runs': set(),
            'chains': collections.Counter(),
            'diff': []}


def sample(lst, v, cap=6):
    if v not in lst and len(lst) < cap:
        lst.append(v)


def scan_log(path, run, fns):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '[wwe2k14][trace]' in line:
                m = TRACE_CALL.search(line)
                if m:
                    fn = fns[m.group(1)]
                    fn['runs'].add(run)
                    fn['calls'] += 1
                    fn['callers'][m.group(3)] += 1
                    if m.group(6) and m.group(6) != '?':
                        fn['via'][m.group(6)] += 1
                    for reg, val in REG.findall(m.group(4)):
                        sample(fn['args'][reg], val)
                    for reg, kind in DEREF.findall(m.group(7)):
                        if not kind.startswith('not'):
                            fn['deref'][reg] += 1
                    continue
                m = TRACE_RET.search(line)
                if m:
                    fn = fns[m.group(1)]
                    sample(fn['rets'], m.group(2))
                    if '*ret: ' in m.group(4) and '*ret: not' not in m.group(4):
                        fn['ret_readable'] += 1
                    continue
                m = TRACE_CALLERS.search(line)
                if m:
                    fn = fns[m.group(1)]
                    fn['runs'].add(run)
                    for lr, n in re.findall(r'(0x[0-9A-F]{8}) x(\d+)', m.group(3)):
                        fn['callers'][lr] += int(n)
                    fn['calls'] = max(fn['calls'], int(m.group(2)))
                continue
            if '[wwe2k14][watch] hit #' in line:
                m = WATCH_HIT.search(line)
                if m:
                    fn = fns['0x' + m.group(7).upper()]
                    fn['runs'].add(run)
                    w = fn['writes'][m.group(2)]
                    w[0] += 1
                    if len(w[1]) < 4:
                        w[1].add('%s->%s' % (m.group(4), m.group(5)))
                    chain = re.findall(r'sub_([0-9A-F]{8})\+', line.split('host:', 1)[1]) if 'host:' in line else []
                    if len(chain) > 1:
                        fn['chains'][' < '.join('0x' + x for x in chain[1:5])] += 1


def diffcov(run, spec, fns):
    b, a = spec.split(':', 1)
    out = subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'coverage', 'diffcov.py'), b, a, '--run', run,
                          '--top', '25'], capture_output=True, text=True)
    for ln in out.stdout.splitlines():
        m = re.match(r'\s+(0x[0-9A-F]{8})\s+elsewhere\s+(\d+)', ln)
        if m:
            fn = fns[m.group(1)]
            fn['runs'].add(run)
            fn['diff'].append('ran in coverage window %s but not in %s (coverage run %s; in %s other recorded windows)'
                              % (b, a, run, m.group(2)))


def evidence(addr, fn, kind_of_run):
    ev = []
    runs = ', '.join(sorted(fn['runs']))
    if fn['calls']:
        callers = ', '.join('%s x%d' % c for c in fn['callers'].most_common(5))
        ev.append('trace (%s %s): %d calls seen; callers (return addresses): %s' % (kind_of_run, runs, fn['calls'],
                                                                                    callers))
    if fn['via']:
        ev.append("trace: callers' callers: %s" % ', '.join('%s x%d' % c for c in fn['via'].most_common(3)))
    for reg in ('r3', 'r4', 'r5', 'r6'):
        if fn['args'].get(reg):
            ev.append('trace: %s on entry: %s' % (reg, ', '.join(fn['args'][reg])))
    if fn['deref']:
        ev.append('trace: argument registers pointing at readable guest memory: %s' %
                  ', '.join('%s (%d calls)' % d for d in sorted(fn['deref'].items())))
    if fn['rets']:
        ev.append('trace: r3 on return: %s%s' % (', '.join(fn['rets']),
                                                 '; the returned r3 pointed at readable memory (%d calls)' %
                                                 fn['ret_readable'] if fn['ret_readable'] else ''))
    for data, (n, vals) in sorted(fn['writes'].items()):
        ev.append('watch (%s %s): writes guest 0x%s %d times (%s)' % (kind_of_run, runs, data[2:], n,
                                                                       ', '.join(sorted(vals))))
    for chain, n in fn['chains'].most_common(2):
        ev.append('watch: called from (host call stack, nearest first): %s (%d hits)' % (chain, n))
    ev += fn['diff']
    return ev


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--coverage', action='store_true', help='runs of the coverage build (its own log numbers)')
    ap.add_argument('--diffcov', action='append', default=[], help='B:A coverage windows (coverage runs)')
    ap.add_argument('--out')
    ap.add_argument('--print', action='store_true', help='print the entries instead of writing a file')
    a = ap.parse_args()
    kind = 'coverage' if a.coverage else 'release'
    fns = collections.defaultdict(new_fn)
    runs = []
    for r in a.runs:
        r = '%03d' % int(r)
        path = os.path.join(LOGS[kind], 'wwe2k14_%s.log' % r)
        if not os.path.exists(path):
            print('no log %s' % path)
            continue
        runs.append(r)
        scan_log(path, r, fns)
        for spec in a.diffcov:
            diffcov(r, spec, fns)
    known = db_index()
    today = datetime.datetime.now().strftime('%Y-%m-%d %H:%M')
    entries = []
    for addr in sorted(fns):
        fn = fns[addr]
        ev = evidence(addr, fn, kind + ' run')
        if not ev:
            continue
        e = {'address': addr, 'name': '', 'summary': '', 'evidence': ev, 'confidence': 'unknown', 'system': '',
             'source': 'tools/db/from_run.py, %s runs %s (%s): observations only; name it through the two-label '
                       'process' % (kind, ', '.join(runs), today)}
        if addr in known:
            e['existing'] = known[addr]
        entries.append(e)
    doc = {'note': 'Proposed entries from live findings (Session 30, #8). Not part of the function database: labellers '
                   'A and B and the settle pass name them (CLAUDE.md). Numbers only; no game data.',
           'kind': kind, 'runs': runs, 'made': today, 'entries': entries}
    if a.print:
        print(json.dumps(doc, indent=2))
        return 0
    out = a.out or os.path.join(REPO, 'db', 'proposed', '%s%s.json' % ('cov_' if a.coverage else '', '_'.join(runs)))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')
    have = sum(1 for e in entries if 'existing' in e)
    print('%s: %d functions (%d already in the db, marked "existing"); runs %s' % (out, len(entries), have,
                                                                                ', '.join(runs)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
