r"""Lane 62 (S101, UJ): the UI join census's selftest. Offline, 0 game runs.
  py -I tools\ui\selftest.py
1. Unit controls on synthetic bytes (the pac readers: DDS sizes, the texture directory, the .yfb parser) - a parser that reads nothing must fail.
2. The shipped census (subprocess) on the Create a Superstar zip: the unplanted run exits 0; each planted twin exits 1 with the right check FAIL:
   key_shift -> join_bytes (the join table off by one key), names_stripped -> names (loud "claimed 0"), corrupt_blob -> join ("not found") with join_bytes still PASS."""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import ui_pacs as up  # noqa: E402

ZIP = 'out/s101/runs/20261111_223313_s101_checkB_caw_walk/case_20261111_223420_216_0_pc_run_exit.zip:editor'
fails = []


def check(name, ok, detail=''):
    print('%s  %s %s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        fails.append(name)


def dds(w, h, fourcc, mips, bits=0):
    hdr = bytearray(128)
    hdr[0:4] = b'DDS '
    struct.pack_into('<I', hdr, 4, 124)
    struct.pack_into('<5I', hdr, 12, h, w, 0, 0, mips)
    struct.pack_into('<I', hdr, 76, 32)
    struct.pack_into('<I', hdr, 80, 4 if fourcc else 0x41)
    hdr[84:88] = fourcc.ljust(4, b'\0')
    struct.pack_into('<I', hdr, 88, bits)
    return bytes(hdr)


def units():
    lay = up.dds_layout(dds(512, 512, b'DXT5', 1) + bytes(512 * 512))
    check('unit.dxt5', lay is not None and lay['size'] == 512 * 512 and lay['mip0'] == 512 * 512, str(lay and lay['size']))
    lay = up.dds_layout(dds(8, 8, b'DXT1', 4) + bytes(64))
    check('unit.dxt1_mips', lay is not None and lay['size'] == 32 + 8 + 8 + 8, str(lay and lay['size']))  # 8x8 (4 blocks), 4x4, 2x2, 1x1 = 1 block each
    lay = up.dds_layout(dds(16, 16, b'', 1, 32) + bytes(1024))
    check('unit.rgba8', lay is not None and lay['size'] == 1024, str(lay and lay['size']))
    check('unit.not_dds', up.dds_layout(b'XXXX' + bytes(200)) is None)
    # a texture directory: header (count, 0x100, 0, 0x10), then 32 bytes per entry (name[16], ext[4], size, offset, 0)
    body = dds(4, 4, b'DXT1', 1) + bytes(8)
    d = struct.pack('<4I', 1, 0x100, 0, 0x10) + b'tex_a'.ljust(16, b'\0') + b'dds\0' + struct.pack('<3I', len(body), 48, 0) + body
    check('unit.directory', up.texture_directory(d) == {48: 'tex_a'}, str(up.texture_directory(d)))
    check('unit.directory_none', up.texture_directory(bytes(64)) == {})
    check('unit.scan', [o for o, _ in up.scan_leaf(d)] == [48], str([o for o, _ in up.scan_leaf(d)]))
    # a .yfb: name, 2 glyphs (codes 0x41, 0x56), their records, one kerning record
    t = bytearray(0x300)
    t[0x10:0x10 + 16] = b'name=test.yfb\0\0\0'
    struct.pack_into('<I', t, 0xA4, 2)
    struct.pack_into('<2H', t, 0xE8, 0x41, 0x56)
    recs = (0xE8 + 4 + 7) & ~7
    struct.pack_into('<4H', t, recs, 0, 0, 20, 40)
    struct.pack_into('<4H', t, recs + 16, 20, 0, 22, 40)
    struct.pack_into('<HHI', t, recs + 32, 0x41, 0x56, 4)
    y = up.parse_yfb(bytes(t))
    check('unit.yfb', y['name'] == 'test.yfb' and y['rects'][0x56] == (20, 0, 22, 40) and y['pairs'] == {(0x41, 0x56): 4}, str(y['pairs']))


def census(args):
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'ui_join_census.py'), ZIP, '--no-search'] + args, capture_output=True, text=True, timeout=1800, cwd=ROOT)
    return p.returncode, p.stdout + p.stderr


def twin(plant, check_name, extra_ok=()):
    rc, out = census(['--plant', plant])
    lines = {l.split()[1]: l.split()[0] for l in out.splitlines() if l.startswith(('PASS', 'FAIL'))}
    ok = rc == 1 and lines.get(check_name) == 'FAIL' and all(lines.get(c) == 'PASS' for c in extra_ok)
    check('plant.%s' % plant, ok, 'exit %d, %s %s%s' % (rc, check_name, lines.get(check_name), ''.join(', %s %s' % (c, lines.get(c)) for c in extra_ok)))
    return out


def main():
    units()
    rc, out = census([])
    lines = [l for l in out.splitlines() if l.startswith(('PASS', 'FAIL'))]
    check('census.unplanted', rc == 0 and len(lines) >= 10 and not any(l.startswith('FAIL') for l in lines), 'exit %d, %d checks' % (rc, len(lines)))
    twin('key_shift', 'join_bytes')
    o = twin('names_stripped', 'names')
    check('plant.names_stripped.loud', ' 0 2D draws claimed' in o, 'says "0 2D draws claimed"')
    o = twin('corrupt_blob', 'join', ('join_bytes', 'key_image'))
    check('plant.corrupt_blob.not_found', 'not found' in o and 'not found 1' in o, 'exactly one image reported "not found"')
    print('%s: %d FAIL' % ('OK' if not fails else 'FAILED', len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
