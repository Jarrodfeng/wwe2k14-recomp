"""Session 80 (HD 520 arc, first launch): update the shipped list with runs' NATIVE pipeline records, keeping all of it.

merge_storage.py rebuilds a list from runs' storage only; this keeps every line of the old list (its shaders, the
emulated path's records, the other render path's records, records from runs no one repeats such as Jarrod's CAW
session) and appends each run's native records the list lacks (by their XXH3, the record's first 8 bytes). The SDK skips
an older record (no ext bits, Session 69) once a newer one of the same pipeline is known, so the old ones need not go.
Shaders the runs stored that the list has no `S` line for are reported (locate.py makes their lines).

    py tools\\shaders\\update_list.py db\\shaders\\shader_list.txt DIR [DIR ...] --out out\\s80\\shader_list.txt [--drop-covered]
    py tools\\shaders\\update_list.py LIST --variants V_LINES.txt --out NEWLIST     (Session 92 M6: only V lines; no DIR needed)

--variants FILE (m6_variants.py --list): merge its `V` lines (translation variant keys: hash, modification bits, ext,
scale; no shader code) into the list - every old line kept, duplicates dropped - and make sure the `H xvar` header line
is there (after the other H lines). The V lines go at the end, sorted.

Each DIR is one run's `cache\\shaders\\shareable` copy, or a folder of them (run_sweep -KeepShaderStorage).
--drop-covered: an older record (no ext bits) is left out when the runs stored a newer record of the same draw (the
same shaders, state, blend, target variant, layout counts, flags, quantize and primitive; the modifications may differ:
the older ones carry Session 66's earlier encodings, so the SDK's own exact sibling test never matches them and the
warm-up makes both guessed variants of the older record besides the newer one). A variant the runs never drew is then
made on first use, as any pipeline the list lacks.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import merge_storage as ms  # noqa: E402


def draw_identity(body):
    """A 64-byte record's draw without its modifications and ext bits (NativeRenderer::PipelineRecord: vertex hash,
    vertex modification, pixel hash, pixel modification, tint hash, state, blend, variant, layout counts x4, flags,
    quantize, host primitive, ext bits)."""
    v = struct.unpack('<5Q3I4H4B', body)
    return (v[0], v[2], v[4], v[5], v[6], v[7], v[8:12], v[12], v[13], v[14])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('list')
    ap.add_argument('dirs', nargs='*')
    ap.add_argument('--variants')
    ap.add_argument('--out', required=True)
    ap.add_argument('--drop-covered', action='store_true')
    ap.add_argument('--no-s-lines', action='store_true',
                    help='do not make S lines for shaders the list lacks (default: locate.py makes them)')
    a = ap.parse_args()
    lines = open(a.list, encoding='ascii').read().splitlines()
    heads = {}
    known = {'fsi': set(), 'fbo': set()}
    list_shaders = set()
    for ln in lines:
        p = ln.split()
        if len(p) >= 3 and p[0] == 'H' and p[1].startswith('native-'):
            heads[p[1][7:]] = bytes.fromhex(p[2])
        elif len(p) == 3 and p[0] == 'N':
            known[p[1]].add(bytes.fromhex(p[2])[:8])
        elif len(p) >= 2 and p[0] == 'S':
            list_shaders.add(int(p[1], 16))
    if not a.dirs and not a.variants:
        ap.error('give run DIRs and/or --variants')
    dirs = ms.storage_dirs(a.dirs) if a.dirs else []
    added = {'fsi': [], 'fbo': []}
    new_shaders = set()
    print('%-40s %6s %6s %8s' % ('run', '+fsi', '+fbo', '+shaders'))
    for d in dirs:
        n = {'fsi': 0, 'fbo': 0}
        for api in ('fsi', 'fbo'):
            head, recs = ms.read_records(os.path.join(d, '%s.native.%s.vk.xnps' % (ms.TITLE, api)), 16,
                                         ms.XNPS_RECORD)
            if head is None or not recs:
                continue
            if api not in heads or head != heads[api]:
                print('  %s: native %s header differs from the list (another SDK storage version): skipped' % (d, api))
                continue
            for r in recs:
                if r[:8] not in known[api]:
                    known[api].add(r[:8])
                    added[api].append(r)
                    n[api] += 1
        _, recs = ms.read_xsh_raw(os.path.join(d, ms.TITLE + '.xsh'))
        fresh = {h for h, _ in recs if h not in list_shaders} - new_shaders
        new_shaders |= fresh
        print('%-40s %6d %6d %8d' % (os.path.basename(d)[:40], n['fsi'], n['fbo'], len(fresh)))
    # --drop-covered: the draws the runs' newer records stand for (all the runs' records, also those the list had).
    covered = {'fsi': set(), 'fbo': set()}
    dropped = {'fsi': 0, 'fbo': 0}
    if a.drop_covered:
        for d in dirs:
            for api in ('fsi', 'fbo'):
                head, recs = ms.read_records(os.path.join(d, '%s.native.%s.vk.xnps' % (ms.TITLE, api)), 16,
                                             ms.XNPS_RECORD)
                if head is not None and api in heads and head == heads[api]:
                    covered[api] |= {draw_identity(r[8:]) for r in recs if r[8 + 63] & 1}
    # The new records go right after the last record of their render path.
    out = []
    for i, ln in enumerate(lines):
        p = ln.split()
        if a.drop_covered and len(p) == 3 and p[0] == 'N':
            body = bytes.fromhex(p[2])[8:]
            if not body[63] & 1 and draw_identity(body) in covered[p[1]]:
                dropped[p[1]] += 1
                ln = None
        if ln is not None:
            out.append(ln)
        if len(p) == 3 and p[0] == 'N':
            nxt = lines[i + 1].split() if i + 1 < len(lines) else []
            if not (len(nxt) == 3 and nxt[0] == 'N' and nxt[1] == p[1]):
                out.extend('N %s %s' % (p[1], r.hex().upper()) for r in added[p[1]])
                added[p[1]] = []
    for api, recs in added.items():  # (a render path the list had no record of: at the end)
        out.extend('N %s %s' % (api, r.hex().upper()) for r in recs)
    if a.variants:
        # The xvar header: 'XVAR', format 1, Modification::kVersion 12, record size 40 (little endian u32 each).
        xvar_head = 'H xvar ' + (b'XVAR' + struct.pack('<III', 1, 12, 40)).hex().upper()
        out = [x for x in out if not x.startswith('H xvar ')]
        have_v = {x for x in out if x.startswith('V ')}
        n_old = len(have_v)
        for ln in open(a.variants, encoding='ascii').read().splitlines():
            ln = ln.strip()
            if ln.startswith('V ') and len(ln.split()) == 8:
                have_v.add(ln)
        out = [x for x in out if not x.startswith('V ')]
        last = max([i for i, x in enumerate(out) if x.startswith('H ')] or [-1])
        out.insert(last + 1, xvar_head)
        out.extend(sorted(have_v))
        print('variants: +%d new V lines, %d in all' % (len(have_v) - n_old, len(have_v)))
    if new_shaders and not a.no_s_lines:
        # Session 93 (M6 piece B): the runs' shaders the list had no S line for used to be only reported, so a
        # fresh install never had their microcode and the precompile's V lines for them waited for ever.
        # locate.py finds each (exact in a pac / patched vertex shader / library image) and writes its S line.
        import argparse as _ap
        import tempfile
        import locate
        xshs = [os.path.join(d, ms.TITLE + '.xsh') for d in dirs]
        tmpdir = tempfile.mkdtemp()
        tmp = os.path.join(tmpdir, 's.txt')
        locate.cmd_match(_ap.Namespace(xsh=xshs, list=tmp, verbose=False))
        made = {}
        for ln in open(tmp, encoding='ascii').read().splitlines():
            q = ln.split()
            if len(q) >= 3 and q[0] == 'S' and int(q[1], 16) in new_shaders:
                made[int(q[1], 16)] = ln
        last_s = max([i for i, x in enumerate(out) if x.startswith('S ')] or [-1])
        out[last_s + 1:last_s + 1] = [made[h] for h in sorted(made)]
        new_shaders -= set(made)
        import shutil
        shutil.rmtree(tmpdir, ignore_errors=True)
        print('S lines made for %d shaders the list lacked' % len(made))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', newline='\n', encoding='ascii') as o:
        o.write('\n'.join(out) + '\n')
    print('list: %d lines -> %d (%s)' % (len(lines), len(out), a.out))
    if a.drop_covered:
        print('older records left out for a newer one of the same draw: %d fsi, %d fbo' % (dropped['fsi'], dropped['fbo']))
    if new_shaders:
        print('%d shaders the list has no S line for and locate.py cannot place (their pipelines are made once the game has them): %s' % (
            len(new_shaders), ', '.join('%016X' % h for h in sorted(new_shaders))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
