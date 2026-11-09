r"""Session 98 (cap / particles): reader for the entrance particle effects of pac\prtcls.pac (our code; read-only, prints only).

    py -I tools\assets\prtc.py BANK            list the effects of an entrance bank (PCE6 section, e.g. 107 = Kane)
    py -I tools\assets\prtc.py BANK ENTRY      decode one effect (entry id, e.g. 5): emitters and their command records

FILE LAYOUT [C = read from the data; U = meaning guessed]
  prtcls.pac: EPAC, 12-byte directory records from +0x80C (char[4] name, LE32 start in 2 KB sectors from +0x4000, LE32 size);
    sections PCE6 (entrance effects, named by the ENTRANCE BANK: 0107 = Kane, 0139 / 0602 = Cena, 0600 = Kane_Champ), PCCH, PCMN
    (game paths "/PCE6/%04d", "/PCCH/%04d", "/PCMN/%04d" .. at 0x82000AE4-0x82000B78) [C].
  one PACH per bank; entry ids: N (a nested PACH: N/0 = 'PRTC' effect, N/1 = its texture directory, same layout as a ch pac's
    entry 10) for N = 0.. (Kane 107: 0 1 2 5 10-13 20-23) [C]; 110-133 and 210-223 = one shared effect in every bank (md5 37899a42) [C];
    16000 = a small LE table [U: effect -> cue / trigger timing].
  PRTC (all little-endian): +0 'PRTC', +4 u32 0x600 [U version], +8 u32, +12 u16 4 u16 1, then offsets / counts [U]; the body is a
    run of 'rootchk' blocks (one per emitter: +8 u32, +16 char[16] emitter name e.g. "Smoke_Ground" / "Smoke") each followed by
    'cmd_chk' records of 60 bytes: char[8] 'cmd_chk', u32 COMMAND, u32 A, s32 B (-1 or a key), u32 BYTES (4 / 8 / 12), then 8 x f32
    (or a 32-char file name for command 22 = the sprite texture, "Smoke0001.tga") [C: the layout; the game parser is sub_82A10760
    ("cmd_chk" at 0x82057468, 48 bytes per command in memory) and sub_82A0F190 ("rootchk")]. Session 99: what each command
    does is decoded from its handler (src\assets\particles.h header; A / B = the effect frames the command runs on).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MAIN = REPO if os.path.isdir(os.path.join(REPO, 'game_data')) else r'C:\Users\Jarrod\wwe2k14-recomp'
sys.path.insert(0, os.path.join(MAIN, 'tools', 'wm30'))
import pac  # noqa: E402

PAC = os.path.join(MAIN, 'game_data', 'extracted', 'pac', 'prtcls.pac')


def directory(d):
    """[(section, name, byte offset)] of the EPAC directory."""
    out, sec = [], d[0x800:0x804].decode('latin1')
    for o in range(0x80C, 0x4000 - 11, 12):
        nm = d[o:o + 4]
        st = struct.unpack_from('<I', d, o + 4)[0]
        if nm == b'\0\0\0\0' and st == 0:
            break
        if not nm.isdigit():
            sec = nm.decode('latin1')
            continue
        out.append((sec, nm.decode(), 0x4000 + st * 0x800))
    return out


def bank_entries(d, bank):
    for sec, nm, at in directory(d):
        if sec == 'PCE6' and int(nm) == bank:
            return {eid: pac.decode(raw) for eid, raw in pac.pach(d, at)}
    return {}


def records(b):
    """[('root', name) | ('cmd', command, A, B, nbytes, payload)] of a PRTC body, in file order."""
    out = []
    o = 0
    while True:
        r = b.find(b'rootchk\0', o)
        c = b.find(b'cmd_chk\0', o)
        if r < 0 and c < 0:
            break
        if r >= 0 and (c < 0 or r < c):
            name = b[r + 16:r + 32].split(b'\0')[0].decode('latin1')
            # Session 99 (particles 2): +8 u32 mode (0 camera-facing, 2 oriented quads, 1 no rotation), +32 u16 max particles,
            # +34 u16 command count (the 48-byte header the game reads into the emitter at +192, sub_82A10760) [C]
            mode = struct.unpack_from('<I', b, r + 8)[0] if r + 12 <= len(b) else -1
            mx = struct.unpack_from('<H', b, r + 32)[0] if r + 34 <= len(b) else -1
            out.append(('root', '%s (mode %d, max %d)' % (name, mode, mx), r))
            o = r + 8
            continue
        cmd, a, bb, n = struct.unpack_from('<IIiI', b, c + 8)
        pay = b[c + 24:c + 56]
        if cmd == 22:
            val = pay.split(b'\0')[0].decode('latin1')
        else:
            val = [round(x, 4) for x in struct.unpack('<8f', pay)]
        out.append(('cmd', cmd, a, bb, n, val, c))
        o = c + 8
    return out


def main(argv):
    d = pac.load(PAC)
    bank = int(argv[0])
    ents = bank_entries(d, bank)
    if len(argv) == 1:
        for eid, blob in sorted(ents.items()):
            kind = blob[:4]
            if kind == b'PACH':
                sub = dict(pac.pach(blob))
                prtc = pac.decode(sub.get(0, b''))
                roots = [r[1] for r in records(prtc) if r[0] == 'root']
                print(eid, 'effect', len(prtc), 'emitters', roots, 'texture bytes', len(sub.get(1, b'')))
            else:
                print(eid, 'data', len(blob), blob[:16].hex())
        return 0
    eid = int(argv[1])
    sub = dict(pac.pach(ents[eid]))
    prtc = pac.decode(sub[0])
    print('PRTC', len(prtc), prtc[:64].hex())
    for r in records(prtc):
        if r[0] == 'root':
            print('emitter', r[1], '@%#x' % r[2])
        else:
            print('  cmd %2d A %4d B %5d n %2d  %s' % r[1:6])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
