# Session 73 (Jarrod's idea 9 of the third ten): are an APK's native libraries ready for phones with 16 KB memory
# pages (Android 15+; Pixel 8 and later can switch, newer phones ship with it)? Each .so's LOAD segments must be aligned
# to at least 16 KB, and (stored uncompressed) start at a 16 KB boundary in the APK. Reads the ELF program headers
# itself (no NDK needed).
#   py tools\android\check_page_align.py out\android\apk\b65.apk
import struct
import sys
import zipfile


def load_aligns(data):
    if data[:4] != b'\x7fELF' or data[4] != 2:
        return None
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    out = []
    for i in range(phnum):
        p_type, = struct.unpack_from('<I', data, phoff + i * phentsize)
        if p_type == 1:  # PT_LOAD
            p_align, = struct.unpack_from('<Q', data, phoff + i * phentsize + 0x30)
            out.append(p_align)
    return out


def main():
    bad = 0
    with zipfile.ZipFile(sys.argv[1]) as z:
        for info in z.infolist():
            if not info.filename.endswith('.so'):
                continue
            aligns = load_aligns(z.read(info))
            # The local header's own name and extra lengths (the extra differs from the central directory's).
            with open(sys.argv[1], 'rb') as raw:
                raw.seek(info.header_offset + 26)
                n, e = struct.unpack('<HH', raw.read(4))
            data_offset = info.header_offset + 30 + n + e
            stored = info.compress_type == zipfile.ZIP_STORED
            ok_align = aligns is not None and min(aligns) >= 16384
            ok_zip = (not stored) or data_offset % 16384 == 0
            bad += not (ok_align and ok_zip)
            print('%-40s LOAD align %-24s %s  %s' % (
                info.filename, ','.join(hex(a) for a in sorted(set(aligns or []))),
                'stored at %s16 KB' % ('' if data_offset % 16384 == 0 else 'NOT ') if stored else 'compressed',
                'OK' if ok_align and ok_zip else 'NOT 16 KB READY'))
    print('all 16 KB ready' if not bad else '%d libraries not 16 KB ready' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
