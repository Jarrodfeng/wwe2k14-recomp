# Session 73 (the phone's shader storage loses shaders between runs): walk a .xsh shader storage file as the SDK's
# loader does (VulkanPipelineCache::InitializeShaderStorage: header, then {hash, 31-bit dword count + type bit, guest
# ucode}; the loader stops at the first record whose ucode does not hash to its header's hash and cuts the file there)
# and report where it would stop and why.
#   py tools\perf\xsh_census.py FILE.xsh
import struct
import sys

import xxhash  # pip install xxhash


def main():
    d = open(sys.argv[1], 'rb').read()
    p, n, why = 8, 0, 'end of file'
    while p + 12 <= len(d):
        h, bits = struct.unpack_from('<QI', d, p)
        count = bits & 0x7FFFFFFF
        if p + 12 + count * 4 > len(d):
            why = 'torn record at %d' % p
            break
        code = d[p + 12:p + 12 + count * 4]
        if xxhash.xxh3_64_intdigest(code) != h:
            why = 'record %d at byte %d: hash mismatch (dwords %d, type %d)' % (n, p, count, bits >> 31)
            break
        p += 12 + count * 4
        n += 1
    print('%s: %d bytes, %d records valid (%d bytes); the loader stops: %s' % (sys.argv[1], len(d), n, p, why))


if __name__ == '__main__':
    main()
