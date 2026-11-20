r"""S104 lane CDT: the unit of tools\arena\draw_states.py. Offline, 0 game runs, no capture and no game file: every buffer here is invented in code.

  py -I tools\arena\draw_states_unit.py

Checks: the fixed-state decode (byte offsets pinned independently of pcap.FIXED_DT: +9 = depth write), the op-list draw order, the token groups (s, a, s z, a z, z, none,
the ambiguous case), the summary's counting of unjoined / ambiguous / not-a-stage draws, the frame selection and filters, the segments, and the header-layout check
(the real headers pass; planted twins - a renamed field, a changed type, a reordered field, a dtype that lost a field - must each FAIL)."""
import os
import struct
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import draw_states as ds  # noqa: E402
import pcap  # noqa: E402

FAILS = []


def check(name, cond, detail=''):
    print('%s %s%s' % ('PASS' if cond else 'FAIL', name, (' : ' + detail) if detail and not cond else ''))
    if not cond:
        FAILS.append(name)


def fixed_record(cull=0, ff=0, zt=1, zw=1, zc=6, blend=(1, 6, 7, 0), at=1, af=6, ref=1 / 255.0, program=0):
    """One 72-byte record laid out by hand from the header's offsets (DrawState: cull +5, front face +6, depth test +8, write +9, compare +10, blend[0] +24;
    DrawFixed: alpha test +56, func +57, ref +64 as a float)."""
    b = bytearray(72)
    struct.pack_into('<H', b, 0, program)
    b[5], b[6], b[8], b[9], b[10] = cull, ff, zt, zw, zc
    b[24:28] = bytes(blend)
    b[56], b[57] = at, af
    struct.pack_into('<f', b, 64, ref)
    return bytes(b)


def frame_buffer(records, draw_ops):
    """An invented frame file: header, a section table (ops = section 1, fixed = section 7), the two tables."""
    ops = b''.join(struct.pack('<II', 2, i) for i in draw_ops) + struct.pack('<II', 0, 0)  # a non-draw op at the end
    fixed = b''.join(records)
    table = 32 + 24 * 2
    ops_off, fix_off = table, table + len(ops)
    head = struct.pack('<IIIIQII', 0x52464352, 2, 5, 2, 777, 0, 0).ljust(32, b'\0')
    sec = struct.pack('<IIQQ', 1, 8, len(draw_ops) + 1, ops_off) + struct.pack('<IIQQ', 7, 72, len(records), fix_off)
    return head + sec + ops + fixed


def test_decode_and_order():
    recs = [fixed_record(zw=1), fixed_record(cull=2, ff=1, zt=1, zw=0, zc=3, blend=(1, 6, 1, 0), at=1, af=6, ref=0.5, program=9), fixed_record(blend=(0, 0, 0, 0), at=0)]
    buf = frame_buffer(recs, [2, 0, 1])
    fr = pcap.FrameData(buf, types.SimpleNamespace(frames=['frame_000005.bin']), 0)
    fx = fr.fixeds()
    check('decode: three rows', len(fx) == 3, str(len(fx)))
    st = ds.state_of(fx[1])
    want = dict(program=9, cull=2, ff=1, zt=1, zw=0, zc=3, bl=1, src=6, dst=1, op=0, at=1, af=6, ref=0.5)
    check('decode: row 1 fields (depth write = byte +9)', st == want, str(st))
    check('decode: row 0 depth write 1, row 2 blend off', ds.state_of(fx[0])['zw'] == 1 and ds.state_of(fx[2])['bl'] == 0 and ds.state_of(fx[2])['at'] == 0)
    check('decode: alpha ref is the float at +64', abs(ds.state_of(fx[0])['ref'] - 1 / 255.0) < 1e-9)
    check('state text names the factors', 'SRC_ALPHA/ONE' in ds.state_text(st) and 'zw0' in ds.state_text(st) and 'LEQUAL' in ds.state_text(st), ds.state_text(st))
    check('op order: draws in the op list order, non-draw ops skipped', ds.draw_order(fr) == [2, 0, 1], str(ds.draw_order(fr)))
    # a section of the wrong element size is a loud error, never a misread
    bad = bytearray(buf)
    struct.pack_into('<I', bad, 32 + 24 + 4, 64)  # section 7's element size
    try:
        pcap.FrameData(bytes(bad), types.SimpleNamespace(frames=['frame_000005.bin']), 0)
        check('decode: a section 7 of 64 bytes is refused', False)
    except pcap.PError:
        check('decode: a section 7 of 64 bytes is refused', True)


def test_groups():
    cases = [({'s'}, 's'), ({'k', 's'}, 's'), ({'a', 'r'}, 'a'), ({'s', 'z'}, 's z'), ({'a', 'z', 'w'}, 'a z'), ({'z'}, 'z'), (set(), 'none'), ({'r', 'l(106)'}, 'none'),
             ({'s', 'a'}, 's a'), ({'s', 'a', 'z'}, 's a z')]
    for toks, want in cases:
        check('group %s -> %s' % (sorted(toks), want), ds.group_of(toks) == want, ds.group_of(toks))
    flags = {1: {'s'}, 2: {'s', 'r'}, 3: {'a', 'z'}, 4: set()}
    c = ds.classify([2, 1, 1], flags)
    check('classify: two slots, one group -> joined', c['status'] == 'joined' and c['group'] == 's' and c['slots'] == [1, 2], str(c))
    c = ds.classify([1, 3], flags)
    check('classify: hits in two groups -> ambiguous', c['status'] == 'ambiguous' and c['group'] == 'ambig' and c['slots'] == [1, 3], str(c))
    c = ds.classify([], flags)
    check('classify: no slot -> unjoined', c['status'] == 'unjoined' and c['group'] == '-')
    c = ds.classify([4], flags)
    check('classify: a slot with no tokens is group none', c['group'] == 'none' and c['status'] == 'joined')
    c = ds.classify([99], flags)
    check('classify: a slot the file has no flags for is group none', c['group'] == 'none')


def row(i, group, status, zw, site='A', slots=(), klass='model_part', bl=(1, 6, 7)):
    st = dict(program=0, cull=0, ff=0, zt=1, zw=zw, zc=6, bl=bl[0], src=bl[1], dst=bl[2], op=0, at=1, af=6, ref=1 / 255.0)
    toks = {'s': {'s'}, 'a': {'a'}, 's z': {'s', 'z'}, 'a z': {'a', 'z'}, 'z': {'z'}}.get(group, set())
    return dict(i=i, klass=klass, site=site, status=status, group=group, slots=list(slots), tokens='-', union=set(toks), call=0, state=st, ps=0)


def test_summary():
    rows = [row(0, 'none', 'joined', 1, slots=[1]), row(1, 's', 'joined', 1, slots=[44]), row(2, 's', 'joined', 1, 'B', slots=[44]), row(3, 's z', 'joined', 0, slots=[223]),
            row(4, 'a z', 'joined', 0, slots=[229], bl=(1, 6, 1)), row(5, 'ambig', 'ambiguous', 1, slots=[1, 223]), row(6, '-', 'unjoined', 1), row(7, '-', 'unjoined', 1),
            row(8, '-', 'nojoin', 1, klass='crowd')]
    s = ds.Summary()
    s.add(rows, filtered=2)
    check('summary: counts joined / ambiguous / unjoined / nojoin', (s.joined, s.ambiguous, s.unjoined, s.nojoin, s.stage) == (5, 1, 2, 1, 8), str((s.joined, s.ambiguous, s.unjoined, s.nojoin, s.stage)))
    check('summary: group s has two state keys (site A, site B)', len(s.groups['s']) == 2 and sum(s.groups['s'].values()) == 2)
    check('summary: z write per group', dict(s.zw['s z']) == {0: 1} and dict(s.zw['a z']) == {0: 1} and dict(s.zw['s']) == {1: 2})
    check('summary: the unjoined and ambiguous draws are in the groups, not dropped', sum(s.groups['-'].values()) == 2 and sum(s.groups['ambig'].values()) == 1)
    text = '\n'.join(s.lines())
    check('summary text prints ambiguous, unjoined and filtered counts', 'ambiguous 1' in text and 'unjoined 2' in text and 'filtered out 2' in text, text)
    check('summary text prints the additive blend', 'SRC_ALPHA/ONE' in text)
    t = ds.Summary()
    t.merge(s)
    t.merge(s)
    check('summary: merge adds', t.joined == 10 and t.ambiguous == 2 and t.unjoined == 4 and t.filtered == 4)


def test_filters_and_segments():
    rows = [row(0, 'none', 'joined', 1, slots=[1]), row(1, 's', 'joined', 1, 'A', slots=[44]), row(2, 's', 'joined', 1, 'B', slots=[45]), row(3, 's z', 'joined', 0, slots=[223]),
            row(4, '-', 'unjoined', 1, klass='model_part')]
    check('filter: --slots', [r['i'] for r in rows if ds.keep_row(r, {44, 45}, set())] == [1, 2])
    check('filter: --tokens z', [r['i'] for r in rows if ds.keep_row(r, set(), {'z'})] == [3])
    check('filter: --tokens none matches the no-token group', [r['i'] for r in rows if ds.keep_row(r, set(), {'none'})] == [0])
    check('filter: --tokens s,z', [r['i'] for r in rows if ds.keep_row(r, set(), {'s', 'z'})] == [1, 2, 3])
    segs = ds.segments(rows)
    check('segments: the A / B copies of one group are one run', [(s[0], s[1], s[2]) for s in segs] == [(0, 0, 1), (1, 2, 2), (3, 3, 1), (4, 4, 1)], str([(s[0], s[1], s[2]) for s in segs]))
    check('segments: the run keeps its slots', segs[1][4] == {44, 45})
    check('frames: ranges and lists', ds.parse_frames('30,60,90-92') == [30, 60, 90, 91, 92])


# ---------------------------------------------------------------- the header layout check -------------------------------------------------------------
def read_headers():
    with open(os.path.join(ds.ROOT, ds.HEADER_FIXED), encoding='utf-8', errors='replace') as f:
        a = f.read()
    with open(os.path.join(ds.ROOT, ds.HEADER_API), encoding='utf-8', errors='replace') as f:
        b = f.read()
    return a, b


def must_fail(name, fixed, api, dtype=None):
    try:
        ds.check_layout(dtype if dtype is not None else pcap.FIXED_DT, fixed, api)
        check(name, False, 'the layout check passed a planted twin')
    except ds.LayoutError as e:
        check(name, True)
        return str(e)
    return ''


def test_layout():
    import numpy as np
    fixed, api = read_headers()
    try:
        ds.check_layout(pcap.FIXED_DT, fixed, api)
        check('layout: pcap.FIXED_DT matches the real headers (72 bytes)', True)
    except ds.LayoutError as e:
        check('layout: pcap.FIXED_DT matches the real headers (72 bytes)', False, str(e))
    flat, total = ds.header_layout(fixed, api)
    names = [f[0] for f in flat]
    off = {f[0]: f[1] for f in flat}
    check('layout: the headers give 72 bytes and depth_write at +9, alpha_ref at +64', total == 72 and off.get('depth_write') == 9 and off.get('alpha_ref') == 64 and off.get('blend') == 24, str((total, off)))
    check('layout: the duplicate name `pad` becomes pad_2', 'pad' in names and 'pad_2' in names)
    # planted twins of the header text
    check('planted: header text really contains the field', 'uint8_t depth_test, depth_write' in api and 'uint8_t alpha_test' in fixed)
    msg = must_fail('planted: a field renamed in api.h (depth_write -> depth_writ) fails', fixed, api.replace('depth_test, depth_write', 'depth_test, depth_writ'))
    check('planted: the message names the field', 'depth_writ' in msg or 'depth_write' in msg, msg)
    must_fail('planted: a field renamed in adapter_frame.h (alpha_test -> alpha_cmp) fails', fixed.replace('uint8_t alpha_test', 'uint8_t alpha_cmp'), api)
    must_fail('planted: a field type widened (alpha_ref float -> double) fails', fixed.replace('float alpha_ref', 'double alpha_ref'), api)
    must_fail('planted: a field type changed (alpha_func uint8_t -> uint16_t) fails', fixed.replace('uint8_t alpha_func', 'uint16_t alpha_func'), api)
    must_fail('planted: a new member added to DrawState fails', fixed, api.replace('uint8_t pad;\n  StencilFace front', 'uint8_t pad;\n  uint8_t extra;\n  StencilFace front').replace('uint8_t pad;\r\n  StencilFace front', 'uint8_t pad;\r\n  uint8_t extra;\r\n  StencilFace front'))
    must_fail('planted: the blend array length changed (kMaxColorTargets 4 -> 8) fails', fixed, api.replace('kMaxColorTargets = 4', 'kMaxColorTargets = 8'))
    must_fail('planted: a blend member renamed (write_mask -> wmask) fails', fixed, api.replace('alpha_op, write_mask', 'alpha_op, wmask'))
    # planted twin of the dtype (pcap.FIXED_DT drifting from the headers)
    drift = np.dtype([f for f in ((n, pcap.FIXED_DT.fields[n][0]) for n in pcap.FIXED_DT.names) if f[0] != 'clip_off'] + [('padx', 'u1')])
    must_fail('planted: a dtype that lost a field fails', fixed, api, drift)
    swapped = list(pcap.FIXED_DT.names)
    swapped[4], swapped[5] = swapped[5], swapped[4]
    must_fail('planted: a dtype with two fields swapped fails', fixed, api, np.dtype([(n, pcap.FIXED_DT.fields[n][0]) for n in swapped]))
    must_fail('planted: no struct in the header text fails', 'int x;', 'int y;')


def main():
    for t in (test_decode_and_order, test_groups, test_summary, test_filters_and_segments, test_layout):
        t()
    print('%s: %d failed' % ('FAIL' if FAILS else 'PASS', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
