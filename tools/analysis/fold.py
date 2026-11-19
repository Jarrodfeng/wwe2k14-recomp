r"""Fold the repeated idioms of a recompiled function's PPC listing into one line each (S103 lane T2, Fable Q3.2).

  py -I tools\analysis\fold.py sub_82C04DF0 [-o out\s103\decomp\sub_82C04DF0.fold]     list + fold (reads the generated code)
  py -I tools\analysis\fold.py --lst out\s103\decomp\sub_82C04DF0.lst [-o FILE]        fold a listing made by list_function.py
  py -I tools\analysis\fold.py --check FOLDFILE --lst LSTFILE                          the gate: every listing address is in exactly one output line
  py -I tools\analysis\fold.py sub_82C04DF0 --expand 0x82C05B3C                        the raw listing lines of the fold holding that address

Output lines (one per item; every instruction address of the listing is in exactly one item, the check proves it):
  0x82C05B3C  bl 0x82bb1320                      an unfolded instruction (the listing line)
  loc_82C05830:                                  a branch target that is not folded away
  0xLO..0xHI [N] SET_PARAM desc=r29 ...          a contiguous range of N instructions (the "set a parameter by descriptor" copy)
  0xA,0xB,0xC [3] STAMP ...                      scattered instructions (the record stamp, interleaved with other code by the compiler)
  0xLO..0xHI [N] PROLOGUE / EPILOGUE / HANDLE_CHECK

The folds (each is a contiguous range or an exact address list; nothing is dropped, and what a fold hides is summarised on its line):
  SET_PARAM    from `lhz rX,16(rD); lhz rY,18(rD); cmplwi rX,65535; beq JOIN` to JOIN (register +16, count +18, type +20, side +24 of the
               descriptor rD). The line gives the skip target, the side-1 / other-side type dispatch (type -> callee), every callee with its
               count, every non-stack load / store with its count, the stack slots used (r1+N), and `entries=` when other code branches
               into the middle of the range (such a range is cut at the entry so a reader can still start there).
  STAMP        `lwz rA,N(r1); rlwinm rB,rA,0,0,15; stw rB,N(r1)` (clear the low half) and `lwz; clrlwi rD,rC,16; stw` (clear the high half)
               on one stack word in one basic block; both together = ZERO_WORD.
  HANDLE_CHECK `mr r3,rH; bl 0x82294f20; lwz rT,4(rH); cmpwi crN,rT,0; beq crN,X`.
  PROLOGUE     function start through `stwu / stwux r1`. EPILOGUE: `addi r1,r1,K` through the closing b / blr (and the `li r3` / `mr r3` before it)."""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
HANDLE_FN = 0x82294F20   # q: the handle check (30 calls in sub_82C04DF0), db label by the S103 review

LINE_RE = re.compile(r'^(0x[0-9A-Fa-f]{8})\s{2}(.*)$')
LABEL_RE = re.compile(r'^\s+(loc_[0-9A-F]{8}):$')
COND_BR = re.compile(r'^b(eq|ne|lt|ge|gt|le|ns|so|dnz|dz)(\s|$)')


def generated_dir():
    sys.path.insert(0, HERE)
    import fields
    return fields.generated_dir()


def make_listing(name):
    """Same address rules as list_function.py: 4 bytes per PPC comment line, loc_ labels re-sync. Returns lst lines."""
    name = 'sub_' + name.upper().removeprefix('0X').removeprefix('SUB_')
    start = int(name[4:], 16)
    marker = f'DEFINE_REX_FUNC({name})'
    for path in glob.glob(os.path.join(generated_dir(), 'wwe2k14_recomp.*.cpp')):
        text = open(path, encoding='utf-8').read()
        i = text.find(marker)
        if i < 0:
            continue
        body = text[i:text.find('\n}\n', i)]
        out, address = [], start
        for line in body.splitlines()[1:]:
            s = line.strip()
            lab = re.match(r'loc_([0-9A-F]{8}):', s)
            if lab:
                address = int(lab.group(1), 16)
                out.append(f'           loc_{lab.group(1)}:')
            elif s.startswith('// '):
                out.append(f'0x{address:08X}  {s[3:]}')
                address += 4
        return out
    sys.exit(f'{name} not found in generated code')


class Insn:
    __slots__ = ('addr', 'text', 'mn', 'ops', 'target', 'fold')

    def __init__(self, addr, text):
        self.addr, self.text, self.fold = addr, text, None
        parts = text.split(None, 1)
        self.mn = parts[0]
        self.ops = [o.strip() for o in parts[1].split(',')] if len(parts) > 1 else []
        self.target = None
        if (self.mn == 'b' or COND_BR.match(self.mn)) and self.ops and self.ops[-1].startswith('0x'):
            self.target = int(self.ops[-1], 16)


def parse(lines):
    insns, labels = [], {}
    for ln in lines:
        m = LINE_RE.match(ln)
        if m:
            insns.append(Insn(int(m.group(1), 16), m.group(2).strip()))
            continue
        m = LABEL_RE.match(ln)
        if m:
            labels[int(m.group(1)[4:], 16)] = m.group(1)
    return insns, labels


def mem(op):
    m = re.match(r'^(-?\d+)\((r\d+)\)$', op)
    return (int(m.group(1)), m.group(2)) if m else None


# ---------- folds ----------

def find_set_param(insns, idx_of, ext_targets_of):
    """Regions [start, join) from the header `cmplwi rX,65535; beq join`, where rX = `lhz rX,16(rD)` (the descriptor's register field).
    The lhz is normally just above; an alternative path of the same copy carries its own cmplwi + beq below a branch target
    (the lhz was done before the split), so the lhz is searched back up to 80 instructions."""
    regions = []
    n = len(insns)
    for c in range(n - 2):
        x = insns[c]
        if not (x.mn == 'cmplwi' and len(x.ops) == 3 and x.ops[2] == '65535'):
            continue
        rx, cr = x.ops[1], x.ops[0]
        b = None
        for j in range(c + 1, min(c + 5, n)):
            if insns[j].mn == 'beq' and insns[j].ops[0] == cr and insns[j].target and insns[j].target > x.addr and insns[j].target in idx_of:
                b = insns[j]
                break
        if b is None:
            continue
        def search_back(k0):
            for k in range(k0 - 1, max(-1, k0 - 80), -1):
                y = insns[k]
                m = mem(y.ops[1]) if y.mn == 'lhz' and len(y.ops) == 2 and y.ops[0] == rx else None
                if m and m[0] == 16:
                    return m[1], k
                if y.ops and y.ops[0] == rx and not y.mn.startswith(('st', 'cmp', 'b')):
                    break
            return None, None
        d, lhz_i = search_back(c)
        if d is None:   # an alternative path: the lhz is above the branch that jumps to this path's label
            for k in range(c, max(-1, c - 5), -1):
                for src in ext_targets_of.get(insns[k].addr, []):
                    if src in idx_of and idx_of[src] < k:
                        d, lhz_i = search_back(idx_of[src])
                        if d is not None:
                            break
                if d is not None:
                    break
        if d is None:
            continue
        start = c
        if 0 < c - lhz_i < 10:
            start = lhz_i
        else:
            for k in range(c, max(-1, c - 5), -1):
                if insns[k].addr in ext_targets_of:
                    start = k
        join = b.target
        end = idx_of[join]
        body = insns[start:end]
        if any(y.mn == 'lwz' and mem(y.ops[1]) == (24, d) for y in body) and any(y.mn == 'lwz' and mem(y.ops[1]) == (20, d) for y in body):
            regions.append((start, end, d, join))
    return regions


def describe_set_param(insns, lo, hi, d, join, cuts):
    body = insns[lo:hi]
    # type dispatch per side
    side_t = None
    for k, x in enumerate(body):
        if x.mn == 'lwz' and mem(x.ops[1]) == (24, d):
            rs = x.ops[0]
            for y in body[k + 1:k + 4]:
                if y.mn == 'cmpwi' and y.ops[1] == rs and y.ops[2] == '1':
                    nxt = body[body.index(y) + 1]
                    if nxt.mn == 'bne' and nxt.target:
                        side_t = nxt.target
            if side_t:
                break
    chains = {'side1': {}, 'other': {}}
    typereg = None
    cur_type = None
    side = 'side1'
    for x in body:
        if side_t and x.addr >= side_t:
            side = 'other'
        if x.mn == 'lwz' and mem(x.ops[1]) == (20, d):
            typereg = x.ops[0]
        elif x.mn == 'cmpwi' and typereg and len(x.ops) == 3 and x.ops[1] == typereg and x.ops[2].isdigit() and 1 <= int(x.ops[2]) <= 11:
            cur_type = int(x.ops[2])
        elif x.mn == 'bl' and cur_type is not None:
            chains[side].setdefault(cur_type, x.ops[0])
            cur_type = None
    calls, stores, loads, slots = {}, {}, {}, set()
    for x in body:
        if x.mn == 'bl':
            calls[x.ops[0]] = calls.get(x.ops[0], 0) + 1
        elif x.mn == 'addi' and len(x.ops) == 3 and x.ops[1] == 'r1':
            slots.add(int(x.ops[2], 0))
        else:
            mm = mem(x.ops[1]) if len(x.ops) == 2 else None
            if mm and x.mn[0] in 'ls' and mm[1] != 'r1' and not (mm[1] == d and mm[0] in (16, 18, 20, 24)):
                key = f'{mm[0]}({mm[1]})'
                (stores if x.mn.startswith('st') else loads)[key] = (stores if x.mn.startswith('st') else loads).get(key, 0) + 1

    def chain_txt(c):
        return ' '.join(f'{t}:{c[t]}' for t in sorted(c)) or '-'

    def cnt(dd, lim=14):
        items = sorted(dd.items(), key=lambda kv: -kv[1])
        s = ' '.join(f'{k}x{v}' if v > 1 else k for k, v in items[:lim])
        return s + (f' ...+{len(items) - lim}' if len(items) > lim else '')
    first = body[0].addr
    return (f'SET_PARAM desc={d} skip_if[+16]==0xFFFF->0x{join:08X} side1{{{chain_txt(chains["side1"])}}} other{{{chain_txt(chains["other"])}}} '
            f'calls[{cnt(calls)}] reads[{cnt(loads)}] writes[{cnt(stores)}] r1+[{",".join(str(s) for s in sorted(slots)[:8])}{" ...+%d" % (len(slots) - 8) if len(slots) > 8 else ""}]'
            + (f' entries[{",".join("0x%08X" % c for c in cuts)}]' if cuts else '') + f' @0x{first:08X}')


def claim(insns, idxs, text):
    group = [insns[i] for i in idxs]
    for g in group:
        if g.fold is not None:
            return False
    rec = {'text': text, 'addrs': [g.addr for g in group], 'idx': list(idxs)}
    for g in group:
        g.fold = rec
    return True


def fold_regions(insns, idx_of, labels):
    ext_by_target = {}
    for x in insns:
        if x.target:
            ext_by_target.setdefault(x.target, []).append(x.addr)
    regions = find_set_param(insns, idx_of, ext_by_target)
    starts = sorted(r[0] for r in regions)
    # innermost (latest start) first, so a copy nested in another's range keeps its own line and the outer one is cut at it
    for lo, hi, d, join in sorted(regions, key=lambda r: -r[0]):
        lo_a, hi_a = insns[lo].addr, join
        cuts = sorted(t for t, srcs in ext_by_target.items()
                      if lo_a < t < hi_a and any(s < lo_a or s >= hi_a for s in srcs) and t in idx_of)
        bounds = sorted({idx_of[c] for c in cuts} | {s for s in starts if lo < s < hi})
        pieces, start = [], lo
        for ci in bounds:
            pieces.append((start, ci))
            start = ci
        pieces.append((start, hi))
        for pi, (a, b) in enumerate(pieces):
            if b - a < 6 or any(insns[k].fold is not None for k in range(a, b)):
                continue
            text = describe_set_param(insns, a, b, d, join, cuts if pi == 0 else [])
            claim(insns, range(a, b), text.replace('SET_PARAM', 'SET_PARAM' if pi == 0 else 'SET_PARAM_PART', 1))
            insns[a].fold['range'] = True
    return regions


def fold_prologue_epilogue(insns, idx_of):
    n = len(insns)
    # prologue
    for i in range(min(40, n)):
        x = insns[i]
        if x.mn in ('stwu', 'stwux') and x.ops and x.ops[0] == 'r1':
            ok = all(insns[k].mn in ('mflr', 'mfcr', 'bl', 'stw', 'std', 'stfd', 'stvx', 'stvx128', 'li', 'stfs', 'lis', 'ori', 'mr')
                     for k in range(i))
            if ok and i >= 1:
                claim(insns, range(0, i + 1), f'PROLOGUE frame_setup via {x.text}')
                insns[0].fold['range'] = True
            break
    # epilogues
    i = 0
    while i < n:
        x = insns[i]
        if x.mn == 'addi' and x.ops[:2] == ['r1', 'r1'] and len(x.ops) == 3 and not x.ops[2].startswith('-') and x.fold is None:
            j = i
            ok = False
            while j < n and j - i < 40:
                y = insns[j]
                if y.fold is not None:
                    break
                if y.mn in ('blr', 'b') :
                    ok = y.mn == 'blr' or (y.target is None or y.target not in idx_of or True)
                    break
                if y.mn not in ('addi', 'lfd', 'lwz', 'ld', 'mtlr', 'lvx', 'lvx128', 'mtcrf', 'lfs', 'lbz', 'lhz', 'ori', 'mr'):
                    break
                if y.mn in ('lwz', 'ld', 'lfd') and not (mem(y.ops[1]) and mem(y.ops[1])[1] == 'r1'):
                    break
                j += 1
            if ok:
                s = i
                if i > 0 and insns[i - 1].fold is None and insns[i - 1].mn in ('li', 'mr') and insns[i - 1].ops[0] == 'r3':
                    s = i - 1
                end = j + 1
                if insns[j].mn == 'b' and insns[j].target in idx_of:
                    i = j + 1
                    continue   # a jump back into this function is not an epilogue
                claim(insns, range(s, end), f'EPILOGUE return via {insns[j].text}' + (f' ({insns[s].text})' if s != i else ''))
                insns[s].fold['range'] = True
                i = end
                continue
        i += 1


def fold_handle_checks(insns):
    n = len(insns)
    for i in range(n - 4):
        a, b, c, d, e = insns[i:i + 5]
        if (a.mn == 'mr' and a.ops[0] == 'r3' and b.mn == 'bl' and b.ops[0] == f'0x{HANDLE_FN:08x}' and c.mn == 'lwz'
                and mem(c.ops[1]) == (4, a.ops[1]) and d.mn == 'cmpwi' and d.ops[1] == c.ops[0] and d.ops[2] == '0'
                and e.mn == 'beq' and e.target and all(g.fold is None for g in (a, b, c, d, e))):
            claim(insns, range(i, i + 5), f'HANDLE_CHECK {a.ops[1]} (sub_{HANDLE_FN:08X}; skip to 0x{e.target:08X} when [{a.ops[1]}+4]==0)')
            insns[i].fold['range'] = True


def writes_reg(x):
    if x.mn.startswith(('st', 'cmp', 'b', 'mt', 'dcb', 'icbi', 'sync', 'tw', 'td', 'nop')):
        return None
    if x.ops:
        return x.ops[0]
    return None


def fold_stamps(insns, branch_targets):
    """Scattered stamps: (lwz rA,N(r1); mask; stw rB,N(r1)) in one basic block."""
    n = len(insns)
    # basic block id per insn: a branch / call ends a block; a branch target starts one
    blk, cur = [], 0
    for x in insns:
        if x.addr in branch_targets:
            cur += 1
        blk.append(cur)
        if x.mn.startswith('b') and x.mn not in ('bl',) or x.mn == 'bl':
            cur += 1
    found = []   # (slot, kind, idxs)
    for i in range(n):
        x = insns[i]
        if x.mn != 'lwz' or x.fold is not None:
            continue
        mm = mem(x.ops[1])
        if not mm or mm[1] != 'r1':
            continue
        ra, slot = x.ops[0], mm[0]
        for j in range(i + 1, min(i + 30, n)):
            if blk[j] != blk[i]:
                break
            y = insns[j]
            if y.fold is not None:
                continue
            kind = None
            if y.mn == 'rlwinm' and y.ops[1] == ra and y.ops[2:] == ['0', '0', '15']:
                kind = 'CLEAR_LO16'
            elif (y.mn == 'clrlwi' and y.ops[1] == ra and y.ops[2] == '16') or (y.mn == 'rlwinm' and y.ops[1] == ra and y.ops[2:] == ['0', '16', '31']):
                kind = 'CLEAR_HI16'
            if kind:
                rb = y.ops[0]
                for k in range(j + 1, min(j + 30, n)):
                    if blk[k] != blk[i]:
                        break
                    z = insns[k]
                    if z.mn == 'stw' and z.ops[0] == rb and mem(z.ops[1]) == (slot, 'r1') and z.fold is None:
                        # no write to rb between j and k that matters, none to the slot between i and k
                        clean = all(not (writes_reg(w) == rb) for w in insns[j + 1:k]) and \
                            all(not (w.mn.startswith('st') and len(w.ops) == 2 and mem(w.ops[1]) == (slot, 'r1')) for w in insns[i + 1:k])
                        if clean:
                            found.append((slot, kind, (i, j, k)))
                        break
                    if writes_reg(z) == rb:
                        break
                break
            if writes_reg(y) == ra:
                break
    used = set()
    by_slot = {}
    for slot, kind, idxs in found:
        if used & set(idxs):
            continue
        used |= set(idxs)
        by_slot.setdefault((slot, blk[idxs[0]]), []).append((kind, idxs))
    for (slot, _), items in by_slot.items():
        kinds = {k for k, _ in items}
        if kinds == {'CLEAR_LO16', 'CLEAR_HI16'} and len(items) == 2:
            idxs = sorted(items[0][1] + items[1][1])
            claim(insns, idxs, f'STAMP ZERO_WORD {slot}(r1) [low half cleared, high half cleared]')
        else:
            for kind, idxs in items:
                claim(insns, list(idxs), f'STAMP {kind} {slot}(r1)')


def fold(lines):
    insns, labels = parse(lines)
    idx_of = {x.addr: i for i, x in enumerate(insns)}
    fold_regions(insns, idx_of, labels)
    fold_prologue_epilogue(insns, idx_of)
    fold_handle_checks(insns)
    targets = {x.target for x in insns if x.target}
    fold_stamps(insns, targets)
    out, emitted = [], set()
    for x in insns:
        if x.addr in labels and (x.fold is None or x.fold['idx'][0] == idx_of[x.addr]) and x.addr in targets:
            out.append(f'           {labels[x.addr]}:')
        if x.fold is None:
            out.append(f'0x{x.addr:08X}  {x.text}')
        elif id(x.fold) not in emitted:
            emitted.add(id(x.fold))
            f = x.fold
            addrs = f['addrs']
            contiguous = f.get('range') or addrs == [insns[i].addr for i in range(f['idx'][0], f['idx'][0] + len(addrs))]
            span = f'0x{addrs[0]:08X}..0x{addrs[-1]:08X}' if contiguous else ','.join(f'0x{a:08X}' for a in addrs)
            out.append(f'{span} [{len(addrs)}] {f["text"]}')
    return out, insns, emitted


def read_lst(path):
    return open(path, encoding='utf-8').read().splitlines()


def check(fold_path, lst_path):
    """The gate: the listing's instruction addresses == the union of the folded output's addresses, each exactly once."""
    lst = [int(LINE_RE.match(l).group(1), 16) for l in read_lst(lst_path) if LINE_RE.match(l)]
    addr_set = sorted(lst)
    claimed = []
    problems = []
    for ln in read_lst(fold_path):
        m = re.match(r'^(0x[0-9A-Fa-f]{8})\.\.(0x[0-9A-Fa-f]{8}) \[(\d+)\] ', ln)
        if m:
            lo, hi, cnt = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))
            inside = [a for a in addr_set if lo <= a <= hi]
            if len(inside) != cnt:
                problems.append(f'range {m.group(1)}..{m.group(2)} says {cnt}, listing has {len(inside)}')
            claimed += inside
            continue
        m = re.match(r'^((?:0x[0-9A-Fa-f]{8},)+0x[0-9A-Fa-f]{8}) \[(\d+)\] ', ln)
        if m:
            a = [int(t, 16) for t in m.group(1).split(',')]
            if len(a) != int(m.group(2)):
                problems.append(f'list says {m.group(2)}, has {len(a)}')
            claimed += a
            continue
        m = LINE_RE.match(ln)
        if m:
            claimed.append(int(m.group(1), 16))
    seen, dup = set(), set()
    for a in claimed:
        (dup if a in seen else seen).add(a)
    missing = [a for a in addr_set if a not in seen]
    extra = [a for a in seen if a not in set(addr_set)]
    if dup:
        problems.append(f'{len(dup)} addresses in more than one line, e.g. 0x{min(dup):08X}')
    if missing:
        problems.append(f'{len(missing)} listing addresses lost, e.g. 0x{missing[0]:08X}')
    if extra:
        problems.append(f'{len(extra)} addresses not in the listing, e.g. 0x{min(extra):08X}')
    items = sum(1 for ln in read_lst(fold_path) if re.match(r'^0x', ln))
    print(f'fold check: {len(addr_set)} listing instructions, {items} folded items, ratio {items / max(1, len(addr_set)):.3f}; '
          + ('OK, no instruction address lost' if not problems else 'FAILED: ' + '; '.join(problems)))
    return 0 if not problems else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('func', nargs='?')
    ap.add_argument('--lst')
    ap.add_argument('-o', '--out')
    ap.add_argument('--check', metavar='FOLDFILE')
    ap.add_argument('--expand', metavar='ADDR')
    a = ap.parse_args()
    if a.check:
        sys.exit(check(a.check, a.lst))
    lines = read_lst(a.lst) if a.lst else make_listing(a.func)
    out, insns, _ = fold(lines)
    if a.expand:
        t = int(a.expand, 0)
        for x in insns:
            if x.addr == t and x.fold:
                for ad in x.fold['addrs']:
                    print(f'0x{ad:08X}  ' + next(i.text for i in insns if i.addr == ad))
                return
        sys.exit('address not in a fold')
    text = '\n'.join(out) + '\n'
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        open(a.out, 'w', encoding='utf-8').write(text)
    else:
        sys.stdout.write(text)
    n_in = len(insns)
    n_items = sum(1 for ln in out if ln.startswith('0x'))
    print(f'-- {n_in} instructions -> {n_items} items ({n_items / max(1, n_in):.1%}); folds: ' +
          ', '.join(f'{k} {v}' for k, v in sorted(count_kinds(out).items())), file=sys.stderr)


def count_kinds(out):
    c = {}
    for ln in out:
        m = re.match(r'^0x\S+ \[\d+\] (\w+)', ln)
        if m:
            c[m.group(1)] = c.get(m.group(1), 0) + 1
    return c


if __name__ == '__main__':
    main()
