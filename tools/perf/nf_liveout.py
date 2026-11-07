"""Which registers does guest code read after a native-replaced function returns? (Session 95, lean natives.)

Answers: for each hooked function, at every direct call site (and tail call, followed to the tail caller's own callers),
is any volatile register read before it is written? Those are the only registers a lean native must leave as the
original does. Works on the generated C++ (generated\\default\\wwe2k14_recomp.*.cpp, local only, never committed): each
statement names the context registers it reads and writes, so no PPC decoder is needed.

The walk: forward over the caller's control flow from the instruction after the call. A read of a tracked register is a
hit. A write removes it. A direct call to another guest function counts that function's own live-in registers as reads
(computed the same way, memoised, depth limited), then the path stops: anything still held across a second call would
have to survive that callee too (unconfirmed for a callee that leaves it alone; the poison run covers that). An indirect
call stops the path: the callee cannot have a private convention, its arguments are set by the ABI. A return follows to
the function's callers up to --up levels (a caller of a caller reading a leftover would be a private convention).

    py tools\\perf\\nf_liveout.py sub_82C32A88 sub_825D0618 ... [--up 2] [--show 5]

Prints per function: call sites walked, and each register read with up to --show sites. "none" = no caller reads any.
The sites are guest function names and generated-code text is never printed.
"""
import argparse
import glob
import mmap
import os
import re
import sys
from collections import defaultdict

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if not os.path.isdir(os.path.join(R, 'generated', 'default')):
    R = r'C:\Users\Jarrod\wwe2k14-recomp'
GEN = os.path.join(R, 'generated', 'default')

FUNC = re.compile(rb'^DEFINE_REX_FUNC\((\w+)\) \{', re.M)
REG = re.compile(r'\bctx\.([rfv]\d+)\b')
LABEL = re.compile(r'^\s*(loc_[0-9A-Fa-f]+):')
GOTO = re.compile(r'\bgoto (loc_[0-9A-Fa-f]+);')
CALL = re.compile(r'^\s*(?:if \(.*\) )?\{?\s*((?:__imp__)?(?:sub_[0-9A-Fa-f]{8}|__\w+))\(ctx, base\);')
WRITE = re.compile(r'^\s*ctx\.([rf]\d+)\.(?:u64|s64|f64)\s*=(?!=)')
VSTORE = re.compile(r'simde_mm_store\w*\((?:\(\w+\*\))?ctx\.(v\d+)\.')
HELPER = re.compile(r'^(?:__imp__)?__(save|rest)')
VOLATILE = {f'r{i}' for i in [0] + list(range(3, 13))} | {f'f{i}' for i in range(14)} | \
    {f'v{i}' for i in list(range(14)) + list(range(32, 64))}


class Index:
    def __init__(self):
        self.where = {}
        self.callers = defaultdict(set)   # target -> functions with a direct call
        self.tail = defaultdict(set)      # target -> functions that end in a call to it
        self.cache = {}
        for path in sorted(glob.glob(os.path.join(GEN, 'wwe2k14_recomp.*.cpp'))):
            with open(path, 'rb') as fh:
                data = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
                ms = list(FUNC.finditer(data))
                for i, m in enumerate(ms):
                    end = ms[i + 1].start() if i + 1 < len(ms) else len(data)
                    self.where[m.group(1).decode()] = (path, m.end(), end)
                data.close()
        self.files = {}

    def lines(self, name):
        name = name.replace('__imp__', '')
        if name in self.cache:
            return self.cache[name]
        if name not in self.where:
            self.cache[name] = None
            return None
        path, a, b = self.where[name]
        if path not in self.files:
            if len(self.files) > 24:
                self.files.clear()
            self.files[path] = open(path, 'rb').read()
        text = self.files[path][a:b].decode('utf-8', 'replace')
        out = []
        op = ''
        for l in text.split('\n'):
            s = l.strip()
            if s.startswith('//'):
                op = s[2:].strip()
            elif s:
                out.append(l + ' //@' + op)
        labels = {}
        for i, l in enumerate(out):
            m = LABEL.match(l.partition(' //@')[0])
            if m:
                labels[m.group(1)] = i
        self.cache[name] = (out, labels)
        return self.cache[name]


def effects(line):
    """(reads, writes) of one statement."""
    w = set()
    m = WRITE.match(line)
    body = line
    if m:
        w.add(m.group(1))
        body = line[m.end():]
    m = VSTORE.search(line)
    if m:
        w.add(m.group(1))
        body = line.replace('ctx.' + m.group(1) + '.', '', 1)
    return set(REG.findall(body)), w


def walk(ix, name, start, tracked, livein, depth, hits, site, ups, seen_up):
    """Forward from statement `start` of `name` with `tracked` registers. hits: register -> set of sites."""
    fn = ix.lines(name)
    if fn is None:
        return
    lines, labels = fn
    seen = {}
    stack = [(start, frozenset(tracked))]
    while stack:
        i, t = stack.pop()
        while True:
            if not t or i >= len(lines):
                break
            prev = seen.get(i)
            if prev is not None and t <= prev:
                break
            seen[i] = (prev | t) if prev is not None else t
            line, _, mn = lines[i].partition(' //@')
            s = line.strip()
            if s == 'return;' or s.startswith('return;') or s == '}':
                if ups > 0 and (name, t) not in seen_up:
                    seen_up.add((name, t))
                    follow(ix, name, t, livein, depth, hits, f'{site}>ret', ups - 1, seen_up)
                break
            cm = CALL.match(line)
            if cm:
                callee = cm.group(1).replace('__imp__', '')
                if HELPER.match(callee):
                    i += 1
                    continue
                for r in t & livein(callee, depth + 1):
                    hits[r].add(f'{site} -> arg of {callee}')
                break
            if 'PPC_CALL_INDIRECT' in line or 'CALL_INDIRECT' in line or 'PPC_CALL' in line:
                break
            reads, writes = effects(line)
            ops = re.split(r'[ ,]+', mn.strip())
            if ops[0].startswith('vupkd3d') and len(ops) > 2:
                # writes its first operand through a helper the statement patterns do not see
                reads, writes = {ops[2]}, writes | {ops[1]}
            if ops[0].startswith('subfe') and len(ops) > 3 and ops[2] == ops[3]:
                reads = reads - {ops[2]}  # rD = rA - rA + carry - 1: the carry only, not rA's value
            for r in t & reads:
                hits[r].add(f'{site} at {mn}')
            t = t - writes
            g = GOTO.search(line)
            if g:
                tgt = labels.get(g.group(1))
                if tgt is not None:
                    if s.startswith('if'):
                        stack.append((tgt, t))
                    else:
                        i = tgt
                        continue
                elif not s.startswith('if'):
                    break
            if s.startswith('switch'):
                for lm in GOTO.finditer(' '.join(x.partition(' //@')[0] for x in lines[i:i + 200])):
                    tgt = labels.get(lm.group(1))
                    if tgt is not None:
                        stack.append((tgt, t))
                break
            i += 1


def follow(ix, target, tracked, livein, depth, hits, site, ups, seen_up):
    """Every return point of a direct call to `target`, and tail callers one level up."""
    for caller in sorted(ix.callers.get(target, ())):
        fn = ix.lines(caller)
        if fn is None:
            continue
        lines, _ = fn
        for i, l in enumerate(lines):
            m = CALL.match(l.partition(' //@')[0])
            if m and m.group(1).replace('__imp__', '') == target:
                walk(ix, caller, i + 1, tracked, livein, depth, hits, f'{caller}', ups, seen_up)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('funcs', nargs='+')
    ap.add_argument('--up', type=int, default=2)
    ap.add_argument('--show', type=int, default=5)
    ap.add_argument('--track', default='', help='extra registers to track, e.g. v27-v31 or v27,v29')
    a = ap.parse_args()
    tracked_set = set(VOLATILE)
    for part in filter(None, a.track.split(',')):
        mm = re.fullmatch(r'([rfv])(\d+)(?:-(?:[rfv])?(\d+))?', part)
        if not mm:
            ap.error('bad --track ' + part)
        for n in range(int(mm.group(2)), int(mm.group(3) or mm.group(2)) + 1):
            tracked_set.add(f'{mm.group(1)}{n}')
    ix = Index()
    print(f'{len(ix.where)} functions indexed', flush=True)
    # direct callers: one scan of every file for the wanted names plus, lazily, their callers
    pat = re.compile(rb'\b(?:__imp__)?(sub_[0-9A-Fa-f]{8}|__\w+)\(ctx, base\);')
    for path in sorted(glob.glob(os.path.join(GEN, 'wwe2k14_recomp.*.cpp'))):
        data = open(path, 'rb').read()
        ms = list(FUNC.finditer(data))
        for i, m in enumerate(ms):
            end = ms[i + 1].start() if i + 1 < len(ms) else len(data)
            me = m.group(1).decode()
            for c in set(pat.findall(data, m.end(), end) if False else pat.findall(data[m.end():end])):
                ix.callers[c.decode()].add(me)
    print('call graph done', flush=True)

    memo = {}

    def livein(fn, depth):
        if fn in memo:
            return memo[fn]
        if depth > 6 or ix.lines(fn) is None:
            return set()
        memo[fn] = set()
        hits = defaultdict(set)
        walk(ix, fn, 0, tracked_set, livein, depth, hits, fn, 0, set())
        memo[fn] = set(hits)
        return memo[fn]

    for f in a.funcs:
        hits = defaultdict(set)
        follow(ix, f, tracked_set, livein, 0, hits, f, a.up, set())
        n = len(ix.callers.get(f, ()))
        print(f'{f}: {n} direct callers')
        if not hits:
            print('  none')
        for r in sorted(hits, key=lambda x: (x[0], int(x[1:]))):
            sites = sorted(hits[r])
            print(f'  {r}: {len(sites)} sites: ' + '; '.join(sites[:a.show]))


if __name__ == '__main__':
    sys.exit(main())
