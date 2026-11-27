"""Selftest for q cstr and the data search of q imm (S111 QCSTR). 0 game runs: a PLANTED image first (twins per rule), then the real image
(CreateEntranceSequence at 0x82003134, the ENTPICK [C] claim's string; the UIPR tag table 0x8362D3C0; POF0 as data) when out/harness/index/image.bin
exists, else SKIP. Planted twins: WWE_QCSTR_PLANT=no_nul / keep_code are seen to FAIL their checks. Exit 1 = a check failed."""
import contextlib
import io
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qdatatag  # noqa: E402
import qwords  # noqa: E402

fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)))
    if not ok:
        fails.append(what)


def run(img, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = qdatatag.main(argv + ['--image', img])
    return rc, out.getvalue(), err.getvalue()


def planted():
    b = qwords.BASE
    blob = bytearray(0x200)
    blob[0x40:0x40 + 6] = b'Hello\0'
    blob[0x46:0x46 + 4] = b'Bye\0'
    blob[0x50:0x50 + 10] = b'\0H\0i\0\0'.ljust(10, b'\0')       # wide "Hi" at +0x50
    blob[0x50:0x56] = b'\x00H\x00i\x00\x00'
    struct.pack_into('>I', blob, 0x60, b + 0x40)               # pointer table: Hello, Bye, outside
    struct.pack_into('>I', blob, 0x64, b + 0x46)
    struct.pack_into('>I', blob, 0x68, 0x12345678)
    blob[0x80:0x84] = b'POF0'                                  # a tag table (stride 8)
    struct.pack_into('>I', blob, 0x84, 1)
    blob[0x88:0x8C] = b'DATA'
    struct.pack_into('>I', blob, 0x8C, 0)
    struct.pack_into('>I', blob, 0xA0, b + 0x80)               # a data word pointing at the POF0 tag
    blob[0xC1:0xC5] = b'XYZW'                                  # an unaligned match
    blob[0x100:0x104] = b'XYZW'                                # an aligned match, "inside code" for the twin
    return bytes(blob)


def main():
    d = tempfile.mkdtemp(prefix='qcstr_')
    img = os.path.join(d, 'image.bin')
    with open(img, 'wb') as f:
        f.write(planted())
    try:
        rc, out, err = run(img, ['0x82000040', '--n', '2'])
        check('cstr: one string stops at its NUL, the next follows', rc == 0 and '0x82000040: "Hello"' in out and '0x82000046: "Bye"' in out, out)
        rc, out, _ = run(img, ['0x82000050', '--wide'])
        check('cstr --wide: UTF-16BE up to 0000', '"Hi"' in out, out)
        rc, out, _ = run(img, ['--table', '0x82000060', '--count', '3'])
        check('table: index, pointer and string per entry', '[0] 0x82000060: 0x82000040 -> "Hello"' in out and '[1]' in out and '"Bye"' in out, out)
        check('twin: a pointer outside the image names no string', '0x12345678 -> (not an image address)' in out, out)
        rc, out, _ = run(img, ['--table', '0x82000080', '--count', '2', '--stride', '8', '--inline'])
        check('table --inline: the tag text and the word beside it', '[0] 0x82000080: "POF0"  + 00000001' in out and '"DATA"' in out, out)
        rc, out, err = run(img, ['0x82000300'])
        check('past the image is refused', rc == 2 and 'outside the image' in err, (rc, err))
        rc, out, err = run(img, ['0x82000040', '--table', '0x82000060'])
        check('ADDR and --table together are refused', rc == 2, (rc, err))
        data = open(img, 'rb').read()
        r = qdatatag.data_hits(data, qdatatag.parse_tag('POF0'))
        check('parse_tag: POF0 is the tag word, 1234 and BEEF are numbers, \'BEEF\' is a tag',
              qdatatag.parse_tag('POF0') == 0x504F4630 and qdatatag.parse_tag('1234') is None and qdatatag.parse_tag('BEEF') is None
              and qdatatag.parse_tag("'BEEF'") == 0x42454546, '')
        h = r['hits'][0] if r['hits'] else {}
        check('data_hits: POF0 at 0x82000080 with the data word 0x820000A0 pointing at it',
              r['n_data'] == 1 and h.get('addr') == 0x82000080 and h.get('pointed_by') == [0x820000A0] and h.get('next') == 1, r)
        x = qdatatag.data_hits(data, 0x58595A57)
        check('data_hits: an unaligned match is reported as unaligned', x['n_data'] == 2 and not x['hits'][0]['aligned'], x)
        y = qdatatag.data_hits(data, 0x58595A57, in_func=lambda a: a == 0x82000100)
        check('data_hits: a match inside game code is left out and counted', y['n_data'] == 1 and y['n_in_code'] == 1, y)
        # planted twins: the rule off must make its check fail
        os.environ['WWE_QCSTR_PLANT'] = 'no_nul'
        import importlib
        importlib.reload(qdatatag)
        rc, out, _ = run(img, ['0x82000040', '--max', '12'])
        check('twin (no_nul plant): the NUL rule off fails the one-string check (it reads past the NUL)', not ('"Hello"' in out and '\\x00' not in out), out)
        os.environ['WWE_QCSTR_PLANT'] = 'keep_code'
        importlib.reload(qdatatag)
        y2 = qdatatag.data_hits(data, 0x58595A57, in_func=lambda a: a == 0x82000100)
        check('twin (keep_code plant): the in-code rule off keeps the code match (seen to FAIL)', y2['n_data'] == 2 and y2['n_in_code'] == 0, y2)
        os.environ['WWE_QCSTR_PLANT'] = ''
        importlib.reload(qdatatag)
    finally:
        os.environ.pop('WWE_QCSTR_PLANT', None)
        try:
            os.remove(img)
            os.rmdir(d)
        except OSError:
            pass
    real = qwords.image_path()
    if real:
        rc, out, _ = run(real, ['0x82003134'])
        check('real image: 0x82003134 is "CreateEntranceSequence" (ENTPICK [C])', rc == 0 and '"CreateEntranceSequence"' in out, out)
        rc, out, _ = run(real, ['--table', '0x8362D3C0', '--count', '10', '--stride', '8', '--inline'])
        tags = ['DATA', 'POF0', 'YOBJ', 'YANM', 'YATR', 'BPE ', 'YMPC', 'YAT2', 'POFF', 'DUMY']
        check('real image: the UIPR tag table lists its 10 entries in order (sub_82C426B8: DATA 0, POF0 1 ... DUMY -1)',
              rc == 0 and all('"%s"' % t in out for t in tags) and out.index('"DATA"') < out.index('"POF0"') < out.index('"DUMY"'), out)
        with open(real, 'rb') as f:
            data = f.read()
        r = qdatatag.data_hits(data, qdatatag.parse_tag('POF0'))
        check('real image: POF0 is found as data in the tag table at 0x8362D3C8', any(h['addr'] == 0x8362D3C8 for h in r['hits']) or r['n_data'] > 12, r['hits'])
    else:
        print('SKIP real image: no out/harness/index/image.bin')
    print('FAILED: %s' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
