#!/usr/bin/env python3
"""S103 PN3 lint: the creator table of src\\scene_live\\picture_source.h (kCreators: call site -> class) equals `q callers 0x822951F0`, and each site's class follows the function it sits in.

  py -I tools\\scene\\picture_creators_lint.py --root .

The birth hook (picture_hooks.h) names a texture's creator by the call site (lr - 4). The table is DATA from the recompiler's own call graph (tools\\q: never typed from memory); this lint re-derives it:
  1. the set of sites in kCreators == the set of call sites of sub_822951F0 in `q callers` (a site missing from the table, or a table site no longer a call site, FAILS);
  2. the class of every site follows the function that holds it: render-target creators -> Gpu, sub_827FBB10 -> Movie, the named-texture loaders -> Loader, every other function -> Unknown.
  3. (A4) every site is a plain `call` (a TAIL CALL / branch into the birth would reach the hook with a caller's lr, not a site of this table: FAIL), and the data words / vtable slots that point at the birth equal
     the acknowledged set (an indirect birth reaches the hook with an unknown lr -> creator Unknown: BC is read, a 4-byte picture is refused `unclassified_creator`; a NEW word is a reviewed change). Planted twins
     (self-test): a tail-call line and an unacknowledged data word each FAIL.
Fails loudly when q cannot run (no index): never a silent pass. 0 game runs."""
import argparse
import os
import re
import subprocess
import sys

ACK_DATA_WORDS = {0x821D6EB0}   # a function-pointer word at the birth (an indirect caller): creator Unknown by construction (see 3 above)
GPU = {'sub_825BB398', 'sub_827E8CC8', 'sub_827EA4F0', 'sub_828135A0', 'sub_827506A0', 'sub_82BC39E0', 'sub_82BCB9B8'}
MOVIE = {'sub_827FBB10'}
LOADER = {'sub_82BAA6F8', 'sub_82BAA568', 'sub_82BA9D68', 'sub_82BA9EF8', 'sub_82BAA048', 'sub_82BAA958', 'sub_82BA9CB8', 'sub_82BAA158', 'sub_82B124A0'}


def parse(text):
    sites, kinds_bad, data, slots = {}, [], set(), []
    for ln in text.splitlines():
        d = re.match(r'\s+data words pointing at it: \d+: (.*)', ln)
        if d:
            data |= {int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{8}', d.group(1))}
            continue
        if 'vtable slot:' in ln:
            slots.append(ln.strip())
            continue
        m = re.match(r'\s+(sub_[0-9A-F]{8})\s+(.*?)(\s+\[ran:.*)?$', ln)
        if not m:
            continue
        for s, kind in re.findall(r'0x([0-9A-F]{8}) (\w+)', m.group(2)):
            sites[int(s, 16)] = m.group(1)
            if kind != 'call':
                kinds_bad.append('0x%s %s in %s' % (s, kind, m.group(1)))
    return sites, kinds_bad, data, slots


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    src = open(os.path.join(root, 'src', 'scene_live', 'picture_source.h'), encoding='utf-8').read()
    table = {int(m.group(1), 16): m.group(2) for m in re.finditer(r'\{0x([0-9A-Fa-f]{8}),\s*Creator::(\w+)\}', src)}
    if not table:
        print('FAIL: kCreators not found in picture_source.h')
        return 1
    r = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'q', 'q.py'), 'callers', '0x822951F0', '--limit', '200'], capture_output=True, text=True, timeout=120, cwd=root)
    if r.returncode or 'sub_822951F0:' not in r.stdout:
        print('FAIL: q callers did not run: %s %s' % (r.returncode, (r.stderr or r.stdout)[-300:]))
        return 1
    bad = 0
    sites, kinds_bad, data, slots = parse(r.stdout)
    for k in kinds_bad:
        print('FAIL: not a plain call: %s' % k)
        bad += 1
    for d in sorted(data - ACK_DATA_WORDS):
        print('FAIL: a data word points at the birth that is not acknowledged: 0x%08X (an indirect birth: review it)' % d)
        bad += 1
    for s in slots:
        print('FAIL: q lists a vtable slot for the birth: %s' % s)
        bad += 1
    # planted twins: the parser must see a tail call and a new data word
    tw_sites, tw_kinds, tw_data, _ = parse(r.stdout + '  sub_8299AAAA                0x8299AAB0 tail\n  data words pointing at it: 2: 0x821D6EB0, 0x8200BEEF\n')
    if not tw_kinds or (0x8200BEEF not in tw_data):
        print('FAIL: the self-test twins (tail call, new data word) were not seen by the parser')
        bad += 1
    head = re.search(r'sub_822951F0: (\d+) call sites', r.stdout)
    if head and int(head.group(1)) != len(sites):
        print('FAIL: q says %s call sites, parsed %d' % (head.group(1), len(sites)))
        bad += 1
    for s in sorted(set(sites) - set(table)):
        print('FAIL: call site 0x%08X (%s) is not in kCreators' % (s, sites[s]))
        bad += 1
    for s in sorted(set(table) - set(sites)):
        print('FAIL: kCreators site 0x%08X is not a call site of sub_822951F0 in q callers' % s)
        bad += 1
    for s, fn in sorted(sites.items()):
        want = 'Gpu' if fn in GPU else 'Movie' if fn in MOVIE else 'Loader' if fn in LOADER else 'Unknown'
        if s in table and table[s] != want:
            print('FAIL: site 0x%08X in %s is %s, its function says %s' % (s, fn, table[s], want))
            bad += 1
    print('%s: picture_creators_lint: %d call sites (q callers), %d table rows; Gpu %d, Movie %d, Loader %d, Unknown %d' % ('FAIL' if bad else 'PASS', len(sites), len(table),
          sum(v == 'Gpu' for v in table.values()), sum(v == 'Movie' for v in table.values()), sum(v == 'Loader' for v in table.values()), sum(v == 'Unknown' for v in table.values())))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
