# Lane 231: lists loads/stores through a device block pointer (11024 fence block, 11028 scratch block) per block offset: py tools/owner/scan_block_readers.py 82C00000 83000000 11024. Answers: who reads which word of the fence / scratch block.
import glob, re, sys
LO, HI = int(sys.argv[1], 16), int(sys.argv[2], 16)
OFF = int(sys.argv[3])      # device offset holding the block pointer: 11024 fence block, 11028 scratch
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
        blk = {}   # reg -> True when it holds the block pointer
        for line in body.splitlines()[1:]:
            s = line.strip()
            lab = re.match(r'loc_([0-9A-F]{8}):', s)
            if lab:
                addr = int(lab.group(1), 16)
                continue
            if not s.startswith('// '):
                continue
            ins = s[3:]
            mm = re.match(r'(lwz|lwzx|lbz|lhz|stw|stb|sth) r(\d+),(-?\d+)\(r(\d+)\)', ins)
            if mm:
                op, rt, d, ra = mm.group(1), mm.group(2), int(mm.group(3)), mm.group(4)
                if op.startswith('l') and d == OFF:
                    blk[rt] = True
                elif op.startswith('l'):
                    if ra in blk:
                        res.append((addr, start, 'READ', d))
                    blk.pop(rt, None)
                elif ra in blk:
                    res.append((addr, start, 'WRITE', d))
            else:
                mm = re.match(r'(?:mr|addi|add) r(\d+),r(\d+)', ins)
                if mm and mm.group(2) in blk:
                    blk[mm.group(1)] = True
            addr += 4
by = {}
for a, f, k, d in res:
    by.setdefault((d, k), set()).add((f, a))
for (d, k), v in sorted(by.items()):
    fs = sorted({f for f, a in v})
    print(f'+{d} {k}: ' + ', '.join('%08X' % f for f in fs))
