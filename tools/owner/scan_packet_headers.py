# Lane 231: lists every PM4 packet header built with lis+ori in a guest address range (generated listings, read only): py tools/owner/scan_packet_headers.py 82CA0000 82CD1000. Answers: which D3D-library functions can emit a memory-writing packet.
import glob, re, sys
LO, HI = int(sys.argv[1], 16), int(sys.argv[2], 16)
OPS = {0x3C: 'WAIT_REG_MEM', 0x3D: 'MEM_WRITE', 0x3E: 'REG_TO_MEM', 0x45: 'COND_WRITE', 0x58: 'EVENT_WRITE_SHD',
       0x5A: 'EVENT_WRITE_EXT', 0x5B: 'EVENT_WRITE_ZPD', 0x40: 'INTERRUPT', 0x46: 'EVENT_WRITE', 0x3F: 'INDIRECT_BUFFER',
       0x48: 'ME_INIT', 0x39: 'VIZ_QUERY'}
res = []
for path in glob.glob('C:/Users/Jarrod/wwe2k14-recomp/generated/default/wwe2k14_recomp.*.cpp'):
    text = open(path, encoding='utf-8').read()
    for m in re.finditer(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', text):
        start = int(m.group(1), 16)
        if not (LO <= start < HI):
            continue
        end = text.find('\n}\n', m.start())
        body = text[m.start():end]
        addr = start
        regs = {}
        for line in body.splitlines()[1:]:
            s = line.strip()
            lab = re.match(r'loc_([0-9A-F]{8}):', s)
            if lab:
                addr = int(lab.group(1), 16)
                continue
            if not s.startswith('// '):
                continue
            ins = s[3:]
            mm = re.match(r'lis r(\d+),(-?\d+)', ins)
            if mm:
                regs[mm.group(1)] = (int(mm.group(2)) & 0xFFFF) << 16
            mm = re.match(r'ori r(\d+),r(\d+),(\d+)', ins)
            if mm and mm.group(2) in regs:
                v = regs[mm.group(2)] | int(mm.group(3))
                if (v >> 30) == 3:
                    op = (v >> 8) & 0x7F
                    res.append((addr, start, v, OPS.get(op, 'op%02X' % op)))
                regs[mm.group(1)] = v
            addr += 4
for a, f, v, o in sorted(res):
    print(f'{a:08X} in {f:08X} hdr {v:08X} {o}')
