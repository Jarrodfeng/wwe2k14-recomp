"""Session 16 item 9: find Lua 5.1 global values in a physical-memory snapshot (logs/snap_NNN_NAME_H.bin, the 512 MB
view of 0xA0000000..0xBFFFFFFF written by --wwe2k14_snapshot_trigger). Read-only; output is game-derived (stays local).

    py tools/analysis/lua_global.py SNAPSHOT NAME [NAME ...]

For each NAME: finds the interned TString (header 16 bytes: next, tt=4, marked, reserved, hash, len), then every
table node whose key points at it (TKey: value = the TString address, tt = 4 at +8), and prints the node's value
(TValue: 8 bytes + tt at +8; tt 3 = number: a big-endian double).
"""
import struct
import sys

BASE = 0xA0000000


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    data = open(sys.argv[1], 'rb').read()
    for name in sys.argv[2:]:
        needle = name.encode() + b'\0'
        pos = 0
        while True:
            i = data.find(needle, pos)
            if i < 0:
                break
            pos = i + 1
            hdr = i - 16
            tt = data[hdr + 4]
            ln = struct.unpack('>I', data[hdr + 12:hdr + 16])[0]
            if tt != 4 or ln != len(name):
                continue
            va = BASE + hdr
            print('%s: TString at 0x%08X (hash 0x%08X)' % (name, va, struct.unpack('>I', data[hdr + 8:hdr + 12])[0]))
            ptr = struct.pack('>I', va)
            q = 0
            while True:
                j = data.find(ptr, q)
                if j < 0:
                    break
                q = j + 1
                # Node layout (Lua 5.1, 32-bit, 8-byte lua_Number): i_val TValue (+0 value 8, +8 tt 4, pad) = 16 bytes,
                # then i_key TKeyNK (+16 value 8, +24 tt 4, +28 next 4) = 32 bytes per node. The pointer is the key value.
                if j % 4 or data[j + 8:j + 12] != b'\0\0\0\x04':
                    continue
                node = j - 16
                vtt = struct.unpack('>I', data[node + 8:node + 12])[0]
                if vtt == 3:
                    val = '%g' % struct.unpack('>d', data[node:node + 8])[0]
                else:
                    val = 'tt %d raw %s' % (vtt, data[node:node + 8].hex())
                print('  key ref at 0x%08X: node 0x%08X value %s' % (BASE + j, BASE + node, val))
    return 0


if __name__ == '__main__':
    sys.exit(main())
