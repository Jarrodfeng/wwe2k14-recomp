"""Session 92 (M6 groundwork): the shader translation variants a set of runs needed, from their logs.

Every TRUE NATIVE run logs each translation variant once ([m6][variant], SDK pipeline_cache.cpp, gpu_m6_variant_log):
  [m6][variant] nat PS 0123456789ABCDEF mod 0000000000000042 ext 00000010 scale 0.0000x0.0000 by translator (#17)
This tool reads logs (files, or folders searched for wwe2k14_*.log), makes the union, and prints:
  - how many variants of each kind (emu / nat, VS / PS), how many made by the translator vs a replacement;
  - the coverage curve: per log (in the order given / file time), how many variants it added that no earlier log had -
    a sweep is complete when the last items add nothing;
  - the pixel position scales seen (a NATIVE twin draw translates at the draw's own scale: a resolution input to M6).
With --out FILE it writes the union, keys only (no shader code, rule 1), one variant a line:
  V <emu|nat> <VS|PS> <hash> <modification> <native_ext> <scale_x> <scale_y> <translator|replacement>

With --list FILE it writes the shader list's form (Session 92 M6, docs\\systems\\shaders.md section 6): the translator's variants
only (a replacement never touches the translator), without the last field, in sorted order:
  V <emu|nat> <VS|PS> <hash> <modification> <native_ext> <scale_x> <scale_y>
Merge them into a list with: py tools\\shaders\\update_list.py LIST --variants FILE --out NEWLIST.

  py -I tools\\shaders\\m6_variants.py out\\census\\<pass> [more logs or folders] [--out out\\m6\\variants.txt] [--list out\\m6\\v_lines.txt]
"""
import os
import re
import sys

LINE = re.compile(r'\[m6\]\[variant\] (emu|nat) (VS|PS) ([0-9A-F]{16}) mod ([0-9A-F]{16}) ext ([0-9A-F]{8}) '
                  r'scale ([0-9.]+)x([0-9.]+) by (translator|replacement)')


def logs_of(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            found = []
            for root, _dirs, files in os.walk(p):
                for f in files:
                    if f.startswith('wwe2k14_') and f.endswith('.log'):
                        found.append(os.path.join(root, f))
            found.sort(key=os.path.getmtime)
            out += found
        elif os.path.isfile(p):
            out.append(p)
        else:
            print(f'm6_variants: {p} not found', file=sys.stderr)
    return out


def main(argv):
    out_file = None
    list_file = None
    paths = []
    i = 0
    while i < len(argv):
        if argv[i] == '--list' and i + 1 < len(argv):
            list_file = argv[i + 1]
            i += 2
            continue
        if argv[i] == '--out' and i + 1 < len(argv):
            out_file = argv[i + 1]
            i += 2
            continue
        paths.append(argv[i])
        i += 1
    if not paths:
        print(__doc__)
        return 2
    logs = logs_of(paths)
    union = {}
    curve = []
    for log in logs:
        added = 0
        seen_here = 0
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = LINE.search(line)
                if not m:
                    continue
                seen_here += 1
                key = m.groups()[:7]
                if key not in union:
                    union[key] = m.group(8)
                    added += 1
        curve.append((log, seen_here, added))
    if not union:
        print('m6_variants: no [m6][variant] lines (was gpu_m6_variant_log on? TRUE NATIVE turns it on)')
        return 1
    kinds = {}
    by = {'translator': 0, 'replacement': 0}
    scales = {}
    for key, how in union.items():
        kinds[(key[0], key[1])] = kinds.get((key[0], key[1]), 0) + 1
        by[how] += 1
        if key[5] != '0.0000' or key[6] != '0.0000':
            scales[(key[5], key[6])] = scales.get((key[5], key[6]), 0) + 1
    print(f'm6_variants: {len(union)} variants from {len(logs)} logs ({by["translator"]} by the translator, '
          f'{by["replacement"]} replacements)')
    print('  by kind: ' + ', '.join(f'{a} {b} {n}' for (a, b), n in sorted(kinds.items())))
    print('  twin scales: ' + (', '.join(f'{x}x{y} ({n})' for (x, y), n in sorted(scales.items())) or 'none'))
    print('  coverage (log: variants seen / new):')
    for log, seen_here, added in curve:
        print(f'    {os.path.basename(log)}: {seen_here} / +{added}')
    if out_file:
        os.makedirs(os.path.dirname(os.path.abspath(out_file)), exist_ok=True)
        with open(out_file, 'w', encoding='utf-8', newline='\n') as f:
            for key in sorted(union):
                f.write('V ' + ' '.join(key) + ' ' + union[key] + '\n')
        print(f'  written: {out_file}')
    if list_file:
        os.makedirs(os.path.dirname(os.path.abspath(list_file)), exist_ok=True)
        keys = sorted(k for k, how in union.items() if how == 'translator')
        with open(list_file, 'w', encoding='ascii', newline='\n') as f:
            for key in keys:
                f.write('V ' + ' '.join(key) + '\n')
        print(f'  written: {list_file} ({len(keys)} V lines)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
