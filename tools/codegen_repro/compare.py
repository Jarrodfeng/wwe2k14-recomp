"""Compare two ReXGlue codegen output folders (e.g. generated/default vs a scratch re-run, or before/after an SDK
rebase). Session 8, task 2 (reused by task 17).

    py tools\\codegen_repro\\compare.py DIR_A DIR_B [--out DIR] [--quiet]

Reports, in a few lines (details in --out, default out\\harness\\codegen_repro\\compare_<time>\\):
  - function set: guest address of every function (PPCFuncMappings in <project>_init.cpp) and of every recompiled
    body (DEFINE_REX_FUNC in <project>_recomp.N.cpp): added / removed
  - function sizes: guest bytes of every recompiled body, measured from its code (see below): resized
  - code: bodies whose generated C++ differs although address and size are the same; identical bodies
  - switch tables: `switch (` statements and their `case` lines per function, totals and differing functions
  - files: byte-identical / differing / only in A / only in B (codegen.d and the stamps hold absolute paths and
    fingerprints, so they are listed separately as "path-bearing")
  - partition: functions that moved to another recomp.N.cpp (codegen.partition.json keeps them stable)

Where the sizes come from: ReXGlue's output has no size field (PPCFuncMappings is address -> name only; the funcs
headers are declarations). Every guest instruction is emitted as one `// mnemonic operands` comment line and every
branch target as a `loc_XXXXXXXX:` label, so a body's size = (address after its last instruction) - (its address),
walking the comment lines 4 bytes each and re-syncing at each label (labels are authoritative: an inline jump table
is data with no comment lines). Same method as tools\\analysis\\list_function.py; label re-syncs are counted.

Output is metadata about the game's code (addresses, sizes, hashes): it stays under out\\harness\\ (gitignored).
Exit code: 0 = fully identical (set, sizes, code), 1 = function set or sizes differ, 2 = error,
3 = same set and sizes but code differs (bodies, switch/case counts, names, or non-path files in a seeded run).
"""
import argparse
import glob
import hashlib
import os
import re
import sys
import time

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
MAP_RE = re.compile(r'^\s*\{ 0x([0-9A-Fa-f]+), (\w+) \},?\s*$', re.M)
LINE_RE = re.compile(r'^[ \t]*(?:(// )|loc_([0-9A-F]{8}):|(switch \()|(case \d+:))', re.M)
PATH_BEARING = {'codegen.d', 'codegen.stamp', 'codegen.build.stamp'}
DEF = 'DEFINE_REX_FUNC('


def project_prefix(d):
    inits = glob.glob(os.path.join(d, '*_init.cpp'))
    if len(inits) != 1:
        raise SystemExit(f'compare.py: expected one *_init.cpp in {d}, found {len(inits)}')
    return os.path.basename(inits[0])[:-len('_init.cpp')]


def read_text(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8', errors='replace')


def scan(d):
    """Function table of one generated folder: {address: record} plus the name->address mapping."""
    prefix = project_prefix(d)
    mapping = {}
    for m in MAP_RE.finditer(read_text(os.path.join(d, f'{prefix}_init.cpp'))):
        mapping[m.group(2)] = int(m.group(1), 16)
    funcs = {}
    unmapped = []
    files = sorted(glob.glob(os.path.join(d, f'{prefix}_recomp.*.cpp')),
                   key=lambda p: int(re.search(r'\.(\d+)\.cpp$', p).group(1)))
    for path in files:
        text = read_text(path)
        fname = os.path.basename(path)
        pos = 0
        while True:
            i = text.find(DEF, pos)
            if i < 0:
                break
            j = text.find(')', i)
            name = text[i + len(DEF):j]
            end = text.find('\n}\n', j)
            end = len(text) if end < 0 else end + 3
            body = text[i:end]
            pos = end
            if name in mapping:
                base = mapping[name]
            elif re.fullmatch(r'sub_[0-9A-Fa-f]{8}', name):
                base = int(name[4:], 16)
            else:
                unmapped.append(name)
                continue
            addr, insns, resync, switches, cases = base, 0, 0, 0, 0
            for m in LINE_RE.finditer(body):
                if m.group(1):
                    insns += 1
                    addr += 4
                elif m.group(2):
                    lab = int(m.group(2), 16)
                    if lab != addr:
                        resync += 1
                        addr = lab
                elif m.group(3):
                    switches += 1
                else:
                    cases += 1
            funcs[base] = {'name': name, 'file': fname, 'insns': insns, 'size': addr - base, 'resync': resync,
                           'switches': switches, 'cases': cases,
                           'sha1': hashlib.sha1(body.encode('utf-8', errors='replace')).hexdigest()}
    return prefix, mapping, funcs, unmapped


def file_hashes(d):
    out = {}
    for root, _, names in os.walk(d):
        for n in names:
            p = os.path.join(root, n)
            h = hashlib.sha256()
            with open(p, 'rb') as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    h.update(chunk)
            out[os.path.relpath(p, d).replace('\\', '/')] = h.hexdigest()
    return out


def compare(dir_a, dir_b, out_dir=None, quiet=False):
    t0 = time.time()
    pa, map_a, fa, un_a = scan(dir_a)
    pb, map_b, fb, un_b = scan(dir_b)
    t_scan = time.time() - t0
    lines = []
    say = lines.append
    say(f'A: {dir_a}')
    say(f'B: {dir_b}')
    addr_ma, addr_mb = set(map_a.values()), set(map_b.values())
    say(f'function set (PPCFuncMappings): A {len(addr_ma)}, B {len(addr_mb)}; added in B {len(addr_mb - addr_ma)}, '
        f'removed {len(addr_ma - addr_mb)}')
    names_changed = sum(1 for n, a in map_a.items() if a in addr_mb and map_b.get(n) != a)
    if names_changed:
        say(f'  {names_changed} mapped addresses have a different name in B')
    ka, kb = set(fa), set(fb)
    common = ka & kb
    resized = sorted(a for a in common if fa[a]['size'] != fb[a]['size'])
    changed = sorted(a for a in common if fa[a]['size'] == fb[a]['size'] and fa[a]['sha1'] != fb[a]['sha1'])
    ident = len(common) - len(resized) - len(changed)
    say(f'recompiled bodies: A {len(ka)}, B {len(kb)}; added {len(kb - ka)}, removed {len(ka - kb)}, resized {len(resized)}, '
        f'same size but different code {len(changed)}, identical code {ident}')
    say(f'  guest bytes covered: A {sum(f["size"] for f in fa.values()):,}, B {sum(f["size"] for f in fb.values()):,}; '
        f'instructions A {sum(f["insns"] for f in fa.values()):,}, B {sum(f["insns"] for f in fb.values()):,}; '
        f'label re-syncs A {sum(f["resync"] for f in fa.values())}, B {sum(f["resync"] for f in fb.values())}')
    if un_a or un_b:
        say(f'  bodies with no address (name not in PPCFuncMappings): A {len(un_a)}, B {len(un_b)}')
    sw = lambda fs: (sum(f['switches'] for f in fs.values()), sum(1 for f in fs.values() if f['switches']),
                     sum(f['cases'] for f in fs.values()))
    swa, swb = sw(fa), sw(fb)
    sw_diff = sorted(a for a in common if (fa[a]['switches'], fa[a]['cases']) != (fb[a]['switches'], fb[a]['cases']))
    say(f'switch tables: A {swa[0]} in {swa[1]} functions ({swa[2]} cases), B {swb[0]} in {swb[1]} functions '
        f'({swb[2]} cases); functions whose switch/case counts differ: {len(sw_diff)}')
    moved = sum(1 for a in common if fa[a]['file'] != fb[a]['file'])
    say(f'partition: {moved} common functions are in a different recomp file in B')
    ha, hb = file_hashes(dir_a), file_hashes(dir_b)
    both = set(ha) & set(hb)
    same = sorted(n for n in both if ha[n] == hb[n])
    diff = sorted(n for n in both if ha[n] != hb[n])
    diff_code = [n for n in diff if n not in PATH_BEARING]
    diff_path = [n for n in diff if n in PATH_BEARING]
    say(f'files: A {len(ha)}, B {len(hb)}; byte-identical {len(same)}, differing {len(diff_code)} '
        f'(+{len(diff_path)} path-bearing: {", ".join(diff_path) or "none"}), only in A {len(set(ha) - set(hb))}, '
        f'only in B {len(set(hb) - set(ha))}')
    if diff_code:
        say('  differing: ' + ', '.join(diff_code[:12]) + (' ...' if len(diff_code) > 12 else ''))
    same_set = not (addr_ma ^ addr_mb) and not (ka ^ kb) and not resized
    # Exit 3 (fixer, review finding 1): same set and sizes, but generated code differs somewhere. Non-function file
    # differences only count when no function moved file (a seeded partition); a fresh partition reshuffles every file.
    code_diff = bool(changed or sw_diff or names_changed or (moved == 0 and diff_code))
    say(('RESULT: identical function set and sizes' if same_set else 'RESULT: function set or sizes DIFFER') +
        (f'; {len(changed)} bodies differ in code only' if changed else '') +
        (f'; {len(sw_diff)} switch/case counts differ' if sw_diff else '') +
        (f'; {names_changed} names changed' if names_changed else '') +
        (f'; {len(diff_code)} non-path files differ' if moved == 0 and diff_code else '') +
        (' -> exit 3 (code differs)' if same_set and code_diff else '') + f'  (scan {t_scan:.0f} s)')

    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, 'compare_summary.txt'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        with open(os.path.join(out_dir, 'function_diffs.tsv'), 'w', encoding='utf-8') as f:
            f.write('kind\taddress\tname_a\tname_b\tsize_a\tsize_b\tinsns_a\tinsns_b\tswitches_a\tswitches_b\tcases_a\tcases_b\tfile_a\tfile_b\n')
            rows = [('added', a) for a in sorted(kb - ka)] + [('removed', a) for a in sorted(ka - kb)] + \
                   [('resized', a) for a in resized] + [('code', a) for a in changed] + \
                   [('switches', a) for a in sw_diff if a not in resized and a not in changed]
            for kind, a in rows:
                x, y = fa.get(a, {}), fb.get(a, {})
                f.write('\t'.join([kind, f'0x{a:08X}'] + [str(r.get(k, '')) for k in ('name',) for r in (x, y)] +
                                  [str(r.get(k, '')) for k in ('size', 'insns', 'switches', 'cases', 'file') for r in (x, y)]) + '\n')
            for a in sorted(addr_mb - addr_ma):
                f.write(f'mapping_added\t0x{a:08X}\n')
            for a in sorted(addr_ma - addr_mb):
                f.write(f'mapping_removed\t0x{a:08X}\n')
        for tag, fs in (('a', fa), ('b', fb)):
            with open(os.path.join(out_dir, f'functions_{tag}.tsv'), 'w', encoding='utf-8') as f:
                f.write('address\tname\tsize\tinsns\tresync\tswitches\tcases\tfile\tsha1\n')
                for a in sorted(fs):
                    r = fs[a]
                    f.write(f'0x{a:08X}\t{r["name"]}\t{r["size"]}\t{r["insns"]}\t{r["resync"]}\t{r["switches"]}\t{r["cases"]}\t{r["file"]}\t{r["sha1"]}\n')
        with open(os.path.join(out_dir, 'file_diffs.txt'), 'w', encoding='utf-8') as f:
            for n in diff:
                f.write(f'differ\t{n}\n')
            for n in sorted(set(ha) - set(hb)):
                f.write(f'only_a\t{n}\n')
            for n in sorted(set(hb) - set(ha)):
                f.write(f'only_b\t{n}\n')
        lines.append(f'details: {out_dir}')
    if not quiet:
        print('\n'.join(lines))
    return (1 if not same_set else 3 if code_diff else 0), lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dir_a')
    ap.add_argument('dir_b')
    ap.add_argument('--out', default=None, help='details folder (default out\\harness\\codegen_repro\\compare_<time>)')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    for d in (a.dir_a, a.dir_b):
        if not os.path.isdir(d):
            print(f'compare.py: not a folder: {d}')
            return 2
    out = a.out or os.path.join(REPO, 'out', 'harness', 'codegen_repro', 'compare_' + time.strftime('%Y%m%d_%H%M%S'))
    code, _ = compare(os.path.abspath(a.dir_a), os.path.abspath(a.dir_b), out, a.quiet)
    return code


if __name__ == '__main__':
    sys.exit(main())
