"""Report on `watch` hits (src/guest_watch.h): who wrote each watched guest address, when, and from where.

    py tools/watch/watch_report.py RUN [--address 0x...] [--no-resolve] [--stack N]

RUN is the run number (212), a folder name under out/harness/watch, or a path to the folder. Reads
<folder>/hits.tsv (one row per logged hit) and the run's log for the summary lines (counts per writing site,
including the hits the rate limit did not log) and the script marks. Guest return addresses from the guest stack
walk are resolved to their functions with the code index (py tools/q/q.py addr ...; --no-resolve skips that).

Writers are named as the app logged them: sub_XXXXXXXX+0xOFF = the guest function whose generated body holds the
host instruction after the store, OFF a host byte offset. The raw exe offsets (host_stack column) resolve offline
with py tools/q/q.py addr --host wwe2k14.exe+0x... against the same build's map.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
WATCH = ROOT / 'out' / 'harness' / 'watch'
Q = ROOT / 'tools' / 'q' / 'q.py'


def find_dir(run):
    p = pathlib.Path(run)
    if p.is_dir():
        return p
    for name in (run, run.zfill(3) if run.isdigit() else run):
        if (WATCH / name).is_dir():
            return WATCH / name
    sys.exit('watch_report: no folder %s under %s' % (run, WATCH))


def find_log(folder):
    m = re.match(r'(?:(.+)_)?(\d+)$', folder.name)
    if not m:
        return None
    log = ROOT / 'out' / 'build' / (m.group(1) or 'win-amd64-release') / 'logs' / ('wwe2k14_%s.log' % m.group(2))
    return log if log.is_file() else None


_cache = {}


def resolve(addr):
    if addr in _cache:
        return _cache[addr]
    name = addr
    try:
        r = subprocess.run([sys.executable, str(Q), 'addr', addr, '--json'], capture_output=True, text=True,
                           timeout=30)
        j = json.loads(r.stdout or '{}')
        if j.get('function'):
            name = '%s+%s' % (j['function'], j.get('offset', '?'))
    except (OSError, ValueError, subprocess.TimeoutExpired):
        pass
    _cache[addr] = name
    return name


_stores = {}


def store_sites(addr):
    """{function: [store sites]} that build ADDR with a lis pair ending in a store (tools/q imm)."""
    if addr in _stores:
        return _stores[addr]
    out = {}
    try:
        r = subprocess.run([sys.executable, str(Q), 'imm', addr, '--json'], capture_output=True, text=True,
                           timeout=60)
        for f in json.loads(r.stdout or '{}').get('funcs', []):
            sites = [s['site'] for s in f.get('sites', []) if '+st' in s.get('op', '')]
            if sites:
                out[f['func']] = sites
    except (OSError, ValueError, subprocess.TimeoutExpired):
        pass
    _stores[addr] = out
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--address', help='only this watched address')
    ap.add_argument('--no-resolve', action='store_true', help="don't resolve guest return addresses with tools/q")
    ap.add_argument('--stack', type=int, default=4, help='host/guest frames shown per hit (default 4)')
    a = ap.parse_args()
    folder = find_dir(a.run)
    tsv = folder / 'hits.tsv'
    rows = []
    if tsv.is_file():
        lines = tsv.read_text(encoding='utf-8', errors='replace').splitlines()
        cols = lines[0].split('\t') if lines else []
        for line in lines[1:]:
            if line.startswith('hit\t'):
                continue  # a second header (file appended by a rerun with the same number)
            parts = line.split('\t')
            if len(parts) == len(cols):
                rows.append(dict(zip(cols, parts)))
    want = int(a.address, 16) if a.address else None
    if want is not None:
        rows = [r for r in rows if int(r['address'], 16) == want]
    log = find_log(folder)
    marks, summaries, arms = [], [], []
    if log:
        for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
            m = re.search(r'\[wwe2k14\]\[input\] mark (\d+)(?: (.*))? \(frame (\d+)\)', line)
            if m:
                marks.append('%s "%s" frame %s' % (m.group(1), m.group(2) or '', m.group(3)))
            if '[wwe2k14][watch]' in line and ' summary 0x' in line:
                summaries.append(line[line.index('[wwe2k14][watch]') + 17:])
            if '[wwe2k14][watch] slot ' in line or "[wwe2k14][watch] can't watch" in line:
                arms.append(line[line.index('[wwe2k14][watch]') + 17:])
    print('watch folder %s; log %s' % (folder, log or '(not found)'))
    print('marks: ' + (', '.join(marks) or '-'))
    for line in arms:
        print('armed: ' + line)
    by_addr = {}
    for r in rows:
        by_addr.setdefault(r['address'], []).append(r)
    for addr in sorted(by_addr):
        hits = by_addr[addr]
        print('\n%s (%s bytes): %d logged hit(s)' % (addr, hits[0]['len'], len(hits)))
        if not a.no_resolve:
            stores = dict(store_sites(addr))
            word = '0x%08X' % (int(addr, 16) & ~3)  # a 1/2-byte watch inside a word written whole
            if word.lower() != addr.lower():
                for func, sites in store_sites(word).items():
                    stores.setdefault(func, []).extend('%s (word %s)' % (s, word) for s in sites)
            writers = sorted({r['writer'].split('+')[0] for r in hits})
            print('  writers: %s; static store sites of this address (lis pair + store, tools/q imm) in them: %s'
                  % (', '.join(writers), '; '.join('%s %s' % (w, ','.join(stores[w])) for w in writers
                                                    if w in stores) or 'none (stored through a base register)'))
        for r in hits:
            names = r.get('host_names') or ''
            host = (names.split(' < ') if names else r['host_stack'].split(','))[1:1 + a.stack]
            guest = [g for g in r['guest_ret'].split(',') if g][:a.stack]
            if not a.no_resolve:
                guest = [resolve('0x' + g) for g in guest]
            print('  #%s frame %s  %s -> %s  by %s  (thread %s, guest thread %s)' % (
                r['hit'], r['frame'], r['old'], r['new'], r['writer'], r['thread'], r['guest_thread']))
            print('      host callers: %s' % (' < '.join(host) or '-'))
            print('      guest lr %s; guest return addresses: %s' % (
                r['guest_lr'] if a.no_resolve else resolve('0x' + r['guest_lr']), ' < '.join(guest) or '-'))
    if not rows:
        print('\nno hits.tsv rows%s' % (' for ' + a.address if a.address else ''))
    if summaries:
        print('\nsummaries from the log (all writes, including the ones the rate limit did not log):')
        for s in summaries:
            if want is None or ('0x%08X' % want) in s:
                print('  ' + s)


if __name__ == '__main__':
    main()
