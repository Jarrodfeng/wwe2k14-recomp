#!/usr/bin/env python3
"""S102 T12: GPU-free unit test of the constant-bank change detection (switch wwe2k14_rdr_bank_dirty). Builds src\\renderer_adapter\\bank_dirty_test.cpp and runs it:
the model of reader.h PushBanks around the real pieces of bank_dirty.h, ON vs OFF vs an oracle over 528,000 banks (every write path, partial and equal-value writes, block
boundaries, device changes, frame boundaries, marks on and off); then plants 1-7, each of which must be caught (exit 1); then a wiring lint of reader.h / rdr_switches.h
(the switch is a PerFrame entry of the table read through BoolSeen, the cvar exists with default false, the glue calls the header's pieces, the census line exists).
  py -I tools\\renderer\\bank_dirty_unit.py [--root DIR] [--bench]
Exit 0 = the clean run passes, all seven plants are caught, the lint passes."""
import argparse, os, re, subprocess, sys

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
PLANTS = {
    1: 'a write path (single dwords) that never marks its block',
    2: 'a marked range one register (vec4) short',
    3: 'literals taken as unchanged after the table changed',
    4: 'dirty taken as "differs" after a device change',
    5: 'dirty literal blocks taken as "differs" (no compare)',
    6: 'blocks only the last emit\'s literals covered are not revisited',
    7: 'the inline compare one dword short (60 of 64 bytes)',
    8: "the device-change reset follows the previous draw's switch value (flip scenarios)",
}


def lint(root, reader_text=None):
    bad = []
    def read(p):
        with open(os.path.join(root, p), encoding='utf-8', errors='replace') as f:
            return f.read()
    sw = read('src/renderer_adapter/rdr_switches.h')
    if not re.search(r'X\(BankDirty,\s*"wwe2k14_rdr_bank_dirty",\s*PerFrame,\s*0\)', sw):
        bad.append('rdr_switches.h: BankDirty is not a PerFrame default-0 entry of the table')
    rd = reader_text if reader_text is not None else read('src/renderer_adapter/reader.h')
    if not re.search(r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_bank_dirty,\s*false', rd):
        bad.append('reader.h: the cvar wwe2k14_rdr_bank_dirty (default false) is missing')
    if 'rdr_sw::BoolSeen(rdr_sw::kBankDirty' not in rd:
        bad.append('reader.h: the switch is not read through BoolSeen')
    for piece in ('bank_dirty::ChangedBlocks', 'bank_dirty::EmitPatches'):
        if piece not in rd:
            bad.append('reader.h: the glue does not call ' + piece)
    if '[rdr][bank_dirty] frames' not in rd or '% 600' not in rd:
        bad.append('reader.h: the 600-frame census line is missing')
    if 'c.bd.OnDeviceChange()' not in rd or 'c.bd.AfterEmit()' not in rd or 'c.bd.PlanFor(' not in rd:
        bad.append('reader.h: the bookkeeping (OnDeviceChange / PlanFor / AfterEmit) is not the header State calls')
    if re.search(r'\b(dirty_exact|lit_serial_last|lc\.serial)\b', re.sub(r'c\.bd\.dirty_exact|//[^\n]*', '', rd)):
        bad.append('reader.h: bookkeeping fields written outside bank_dirty.h State')
    # F1: the serial bump sits in the same block as lc.lits.clear(), and nothing else writes the literal table
    m = re.search(r'\n([ \t]*)lc\.lits\.clear\(\);[^\n]*\n([ \t]*)c\.bd\.OnLitRebuild\(\);', rd)
    if not m or m.group(1) != m.group(2):
        bad.append('reader.h: c.bd.OnLitRebuild() is not right after lc.lits.clear() in the same block')
    pb = rd[rd.find('inline uint32_t PushBanks('):]
    pb = pb[:pb.find('\n}\n')]
    writes = re.findall(r'\blc\.(lits|by_block|mask|start|key|generation)\b[^\n;]*?(?:=[^=]|\.clear\(|\.resize\(|\.push_back\(|\|=|\+\+)', pb)
    allowed = [i for i in re.finditer(r'\n([ \t]*)lc\.generation = g_frame\.generation;', pb)]
    blk = pb[pb.find('lc.generation = g_frame.generation;'):pb.find('const uint64_t lit_mask[2]')]
    outside = pb.replace(blk, '')
    write_re = (r'^[ \t]*(?:std::memcpy\(|for \([^\n]*\) )?lc\.(lits|by_block|mask|start|key)\b[^\n;]*?'
                r'((?<![=!<>])=(?!=)|\.clear\(|\.resize\(|\.push_back\(|\|=)|LiteralList\([^\n]*lc\.lits')
    if re.search(write_re, outside, re.M):
        bad.append('reader.h: lc.lits / by_block / mask / start / key written outside the rebuild block that bumps the serial')
    hd = read('src/renderer_adapter/bank_dirty.h')
    ep = hd[hd.find('inline uint32_t EmitPatches'):]
    ep = ep[:ep.find('\n}\n')]
    if 'memcmp' in ep:
        bad.append('bank_dirty.h: memcmp in EmitPatches (R13 5246 site must use Equal64)')
    if 'std::memcmp(block, last.dwords' in rd:
        bad.append('reader.h: the old per-block memcmp is still in PushBanks (it lives in bank_dirty.h EmitPatches now)')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--bench', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    out = os.path.join(root, 'out', 't12')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'bank_dirty_test.exe')
    ra = os.path.join(root, 'src', 'renderer_adapter')
    b = subprocess.run([CLANG, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-I', ra, os.path.join(ra, 'bank_dirty_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL  did not compile\n' + b.stdout + b.stderr)
        return 1
    fails = 0
    p = subprocess.run([exe] + (['bench'] if args.bench else []), capture_output=True, text=True, timeout=900)
    lines = [l for l in p.stdout.strip().split('\n') if l.startswith(('PASS', 'FAIL', 'BENCH'))]
    print(('PASS  ' if p.returncode == 0 else 'FAIL  ') + 'clean run: ' + ' | '.join(lines) if lines else 'FAIL  clean run: no output (exit %d)' % p.returncode)
    if p.returncode != 0 or not any(l.startswith('PASS') for l in lines):
        fails += 1
        print(p.stdout[-3000:])
    for plant, what in PLANTS.items():
        q = subprocess.run([exe, str(plant)], capture_output=True, text=True, timeout=900)
        caught = q.returncode == 1 and 'FAIL:' in q.stdout
        print(('PASS  ' if caught else 'FAIL  ') + 'planted twin %d (%s): %s' % (plant, what, 'caught' if caught else 'NOT caught (exit %d)' % q.returncode))
        fails += not caught
    bad = lint(root)
    # F1 plant: a reader.h copy without the serial bump must fail the lint
    with open(os.path.join(root, 'src/renderer_adapter/reader.h'), encoding='utf-8', errors='replace') as f:
        orig = f.read()
    planted = re.sub(r'[ \t]*c\.bd\.OnLitRebuild\(\);[^\n]*\n', '', orig)
    if planted == orig:
        print('FAIL  lint plant DID NOT APPLY (OnLitRebuild line not found)')
        fails += 1
    else:
        pl = lint(root, planted)
        print(('PASS  ' if pl else 'FAIL  ') + 'planted twin 9 (reader.h without the serial bump): ' + ('caught by the lint' if pl else 'NOT caught'))
        fails += not pl
    for m in bad:
        print('FAIL  lint: ' + m)
    if not bad:
        print('PASS  lint: switch table entry, cvar, BoolSeen read, glue calls the header, census line, no old memcmp in PushBanks')
    fails += len(bad)
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
