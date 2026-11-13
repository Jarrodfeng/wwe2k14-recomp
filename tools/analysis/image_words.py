"""Print big-endian u32 words of the guest image (out/harness/index/image.bin, base 0x82000000).
Answers: what is in the vtable / table at ADDR. Run from the main checkout directory (where out/ lives):
  py <worktree>/tools/analysis/image_words.py 0x82232380 [count]
"""
import struct
import sys

a = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 32
d = open(r'out/harness/index/image.bin', 'rb').read()
for i in range(n):
    off = a - 0x82000000 + 4 * i
    w = struct.unpack('>I', d[off:off + 4])[0]
    print(f'0x{a + 4 * i:08X}: 0x{w:08X}')
