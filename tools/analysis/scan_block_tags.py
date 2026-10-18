"""Count compressed-block tags in the dumped pac archives (Session 8 task 3, decoder study).

Usage: py tools/analysis/scan_block_tags.py [--root game_data/extracted/pac] [--out out/harness/decoder/tags.txt]
       [--names mot0,evt,ch103]   (only pacs whose path relative to --root contains one of these, case-insensitive)

CData::Copy (sub_82C35798) picks its decoder from the first word of a block: 'BPE ' (sub_82C41918), 'ZLIB'
(sub_82C41738), 'LZ4 ' (sub_82C50408); CData containers start with 'PACH' (sub_82C354D8 / sub_82C355A0 check it).
This scans every .pac for those four tags (any byte offset, so some hits are coincidences inside other data) and,
for 'BPE ' hits, reads the 16-byte header: the decoder reads the expanded size as a little-endian word at +12;
+4 and +8 are summarised as found (meaning unconfirmed). It also counts each pac's first word (XCTD magic
0x0FF512ED, EPAC, EPK8, PACH, other). Reads the dump only; writes counts and summary numbers
(no game bytes) to --out, which must stay under out/harness/ (gitignored).
"""
import argparse
import collections
import mmap
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TAGS = (b'BPE ', b'ZLIB', b'LZ4 ', b'PACH')
# First word of a pac file. 0x0FF512ED = the XCTD-compressed file magic (the vendored SDK says so:
# thirdparty/rexglue-sdk-src/src/kernel/xboxkrnl/xboxkrnl_io_info.cpp, XFileXctdCompressionInformation); tag hits
# inside such a file are hits in compressed bytes, so coincidences.
FIRST_WORDS = {bytes.fromhex('0ff512ed'): 'XCTD(0x0FF512ED)', b'EPAC': 'EPAC', b'EPK8': 'EPK8', b'PACH': 'PACH'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.join(ROOT, 'game_data', 'extracted', 'pac'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'harness', 'decoder', 'tags.txt'))
    ap.add_argument('--names', default='')
    a = ap.parse_args()
    out_abs = os.path.abspath(a.out)
    harness = os.path.normcase(os.path.join(ROOT, 'out', 'harness'))
    try:
        inside = os.path.commonpath([os.path.normcase(out_abs), harness]) == harness
    except ValueError:  # different drives
        inside = False
    if not inside or os.path.normcase(out_abs) == harness:
        raise SystemExit('--out must be a file under out/harness/')
    os.makedirs(os.path.dirname(out_abs), exist_ok=True)
    counts = collections.Counter()
    per_pac = []
    bpe_exp = 0
    bpe_plausible = 0
    f4_eq_exp = 0
    f8_hist = collections.Counter()
    f4_hist = collections.Counter()
    exp_hist = collections.Counter()
    align = collections.Counter()
    first_words = collections.Counter()
    total_bytes = 0
    for dirpath, _, files in os.walk(a.root):
        for fn in sorted(files):
            if not fn.lower().endswith('.pac'):
                continue
            p = os.path.join(dirpath, fn)
            names = [n.strip().lower() for n in a.names.split(',') if n.strip()]
            if names and not any(n in os.path.relpath(p, a.root).lower() for n in names):
                continue
            size = os.path.getsize(p)
            if size < 16:
                continue
            total_bytes += size
            c = collections.Counter()
            with open(p, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                fw = bytes(m[:4])
                first_words[FIRST_WORDS.get(fw, 'other')] += 1
                for tag in TAGS:
                    i = m.find(tag)
                    while i != -1:
                        c[tag] += 1
                        if tag == b'BPE ' and i + 16 <= size:
                            f4, f8, exp = struct.unpack_from('<III', m, i + 4)
                            # plausible: expanded size under 64 MiB
                            if 0 < exp < (64 << 20):
                                bpe_plausible += 1
                                bpe_exp += exp
                                exp_hist[min(exp.bit_length(), 27)] += 1
                                f4_hist['f4==exp' if f4 == exp else ('f4<exp' if f4 < exp else 'f4>exp')] += 1
                                if f4 == exp:
                                    f4_eq_exp += 1
                                f8_hist[f8 if f8 < 16 else 'big'] += 1
                                align[i % 2048 == 0 and 'sector' or (i % 16 == 0 and '16' or 'other')] += 1
                        i = m.find(tag, i + 1)
            counts.update(c)
            per_pac.append((os.path.relpath(p, a.root), size, c[b'BPE '], c[b'ZLIB'], c[b'LZ4 '], c[b'PACH']))
    lines = []
    lines.append(f'pac files {len(per_pac)}, {total_bytes / 1e9:.2f} GB scanned (filter: {a.names or "none"})')
    lines.append('first 4-byte word of each pac: ' + ', '.join(f'{k}={v}' for k, v in first_words.most_common()))
    lines.append('tag hits (any offset): ' + ', '.join(f'{t.decode()!r}={counts[t]}' for t in TAGS))
    lines.append(f"'BPE ' hits with a plausible header (expanded 1 B..64 MiB): {bpe_plausible}, sum of expanded "
                 f'sizes {bpe_exp / 1e6:.1f} MB')
    lines.append('  field +4 vs expanded: ' + ', '.join(f'{k}={v}' for k, v in f4_hist.most_common()))
    lines.append('  field +8 values: ' + ', '.join(f'{k}={v}' for k, v in f8_hist.most_common(8)))
    lines.append('  expanded size histogram (bit length: count): ' +
                 ', '.join(f'{k}:{v}' for k, v in sorted(exp_hist.items())))
    lines.append('  hit alignment: ' + ', '.join(f'{k}={v}' for k, v in align.most_common()))
    lines.append('pacs with the most BPE hits:')
    for row in sorted(per_pac, key=lambda r: -r[2])[:25]:
        lines.append(f'  {row[0]:40} {row[1] / 1e6:8.1f} MB  BPE {row[2]:6}  ZLIB {row[3]:5}  LZ4 {row[4]:4}  PACH {row[5]:6}')
    pacs_with_bpe = sum(1 for r in per_pac if r[2])
    lines.append(f'pacs with at least one BPE hit: {pacs_with_bpe} of {len(per_pac)}')
    text = '\n'.join(lines)
    with open(out_abs, 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
