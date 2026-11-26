"""q asm FUNC [FROM TO | --at ..] --writes rN|sp+D   /   --reads rN|sp+D   (S111 QARGS)
Every instruction of the function (or of the window) that writes the register, or stores to the stack slot (--writes), or reads it / loads the slot (--reads).
Writes: the destination of mr / li / lis / addi / loads / ALU ops; the BASE of an update form (lwzu / stwu ...); a `bl` / `bctrl` clobbers r0 and r3..r12 (a
__savegpr / __restgpr helper only r0, r11, r12). `--writes rN` also opens with an `entry` row: what rN holds when the function starts (an argument, or the caller's
value of a callee-saved register, with the prologue instruction that saves it).
Stack slots: a store / load based on r1 counts, as a store of its width overlapping sp+D (offsets are from the sp AFTER the prologue's stwu). A base that is a
frame copy (`mr rX,r1` / `addi rX,r1,N`, the only write of rX in the function) counts too; any other base (r31 as an ordinary pointer) does not.
`--reads sp+D` also lists `addi rX,r1,D`, the address of the slot being taken (the callee may then read or write it).
Not modelled: a read through an indexed store / load on r1, the registers a `bl` callee reads (a call is listed as a possible read of r3..r10)."""
import re

import qdecomp

GPR_RE = re.compile(r'\br(\d+)\b')
SLOT_RE = re.compile(r'(?:sp|r1)\s*([+-])\s*((?:0[xX])?[0-9A-Fa-f]+)')
CLOBBER = ['r0'] + [f'r{i}' for i in range(3, 13)]
HELPER_KILL = ['r0', 'r11', 'r12']
NO_DEST = qdecomp.NO_DEST
RMW = ('rlwimi', 'rldimi', 'rldimi.', 'rlwimi.')


class RegsError(Exception):
    pass


def parse_target(s):
    """'r22' -> ('r', 'r22'); 'sp+208' / 'r1+0xD0' -> ('s', 208)"""
    t = s.strip()
    m = SLOT_RE.fullmatch(t)
    if m:
        v = int(m.group(2), 16) if m.group(2).lower().startswith('0x') or re.search(r'[A-Fa-f]', m.group(2)) else int(m.group(2))
        return 's', v if m.group(1) == '+' else -v
    m = re.fullmatch(r'r(\d+)', t)
    if m and int(m.group(1)) <= 31:
        return 'r', f'r{int(m.group(1))}'
    raise RegsError(f'{s}: not a register (r0..r31) or a stack slot (sp+D)')


def _regs(s):
    return {f'r{n}' for n in GPR_RE.findall(s)}


def frame_aliases(insns):
    """{rX: D}: registers whose ONLY write in the function is `mr rX,r1` / `addi rX,r1,D` (a frame copy: rX + off is sp + D + off)"""
    writes, alias = {}, {}
    for ins in insns:
        ef = effects(ins, {}, {})
        for r in ef['w']:
            writes[r] = writes.get(r, 0) + 1
        ops = qdecomp.split_ops(ins.ops)
        m = ins.mn.rstrip('+-')
        if m == 'mr' and len(ops) == 2 and ops[1] == 'r1' and ops[0] != 'r1':
            alias[ops[0]] = 0
        elif m == 'addi' and len(ops) == 3 and ops[1] == 'r1' and ops[0] != 'r1':
            try:
                alias[ops[0]] = int(ops[2], 0)
            except ValueError:
                pass
    return {r: d for r, d in alias.items() if writes.get(r) == 1}


def effects(ins, alias, helpers):
    """-> {'w': regs written, 'r': regs read, 'sw': [(sp offset, width)], 'sr': [...], 'taken': [sp offset], 'wtag': {reg: why it is written}}"""
    m = ins.mn.rstrip('+-')
    ops = qdecomp.split_ops(ins.ops)
    e = {'w': set(), 'r': set(), 'sw': [], 'sr': [], 'taken': [], 'wtag': {}}
    key = m if m in qdecomp.STORES or m in qdecomp.LOADS else m.rstrip('.')

    def slot(base, off):
        if base == 'r1':
            return off
        if base in alias:
            return off + alias[base]
        return None
    if key in qdecomp.STORES or key in qdecomp.LOADS:
        store = key in qdecomp.STORES
        width = (qdecomp.STORES if store else qdecomp.LOADS)[key][0]
        if ops:
            (e['r'] if store else e['w']).update(_regs(ops[0]))
        mem = qdecomp.MEM_RE.match(ops[1]) if len(ops) > 1 else None
        if mem:
            try:
                off = int(mem.group(1), 0)
            except ValueError:
                off = None
            base = mem.group(2)
            e['r'].add(base)
            if off is not None:
                if key.endswith('u') and base == 'r1':      # stwu r1,-96(r1): the stored back chain is sp+0 of the NEW frame
                    off = 0
                s = slot(base, off)
                if s is not None:
                    (e['sw'] if store else e['sr']).append((s, width))
            if key in qdecomp.UPDATE_FORMS:
                e['w'].add(base)
                e['wtag'][base] = f'update form: {base} receives the address'
        else:
            for o in ops[1:3]:
                e['r'].update(_regs(o))
            if key in qdecomp.UPDATE_FORMS and len(ops) > 1:
                e['w'].update(_regs(ops[1]))
                e['wtag'].update({r: 'update form: base receives the address' for r in _regs(ops[1])})
        return e
    if m in ('lmw', 'stmw') and ops:
        first = int(ops[0][1:]) if re.fullmatch(r'r\d+', ops[0]) else 31
        regs = {f'r{i}' for i in range(first, 32)}
        mem = qdecomp.MEM_RE.match(ops[1]) if len(ops) > 1 else None
        if m == 'lmw':
            e['w'] |= regs
        else:
            e['r'] |= regs
        if mem:
            e['r'].add(mem.group(2))
            try:
                s = slot(mem.group(2), int(mem.group(1), 0))
            except ValueError:
                s = None
            if s is not None:
                (e['sr'] if m == 'lmw' else e['sw']).append((s, 4 * len(regs)))
        return e
    if m in ('bl', 'bctrl', 'blrl'):
        t = qdecomp.branch_target(ins)
        if t is not None and t in helpers:
            e['w'].update(HELPER_KILL)
            e['wtag'].update({r: f'call to {helpers[t]}: clobbers r0, r11, r12 only' for r in HELPER_KILL})
        else:
            e['w'].update(CLOBBER)
            e['wtag'].update({r: 'call: clobbers r0, r3-r12' for r in CLOBBER})
            e['r'].update(f'r{i}' for i in range(3, 11))
        if m == 'bctrl' and ops:
            e['r'].update(_regs(ins.ops))
        return e
    if m.startswith(NO_DEST):
        e['r'] = _regs(ins.ops)
        return e
    if ops:
        e['w'].update(_regs(ops[0]) if re.fullmatch(r'r\d+', ops[0]) else set())
        for o in ops[1:]:
            e['r'].update(_regs(o))
        if m in RMW and ops[0].startswith('r'):
            e['r'].add(ops[0])
        if m in ('addi', 'addi.') and len(ops) == 3 and ops[1] in ['r1'] + list(alias):
            try:
                d = int(ops[2], 0) + (0 if ops[1] == 'r1' else alias[ops[1]])
                e['taken'].append(d)
            except ValueError:
                pass
    return e


def entry_row(reg, insns, helpers):
    n = int(reg[1:])
    if reg == 'r1':
        return 'the stack pointer: the caller\'s frame; stwu r1 moves it'
    if 3 <= n <= 10:
        return f'argument: the caller\'s value (in_{reg}); decomp calls it in_{reg}'
    if reg == 'r13':
        return 'the thread-local block pointer (set by the system, never written here)'
    if n >= 14:
        note = ''
        for ins in insns[:24]:
            m = ins.mn.rstrip('+-')
            if m in ('std', 'stw') and qdecomp.split_ops(ins.ops)[:1] == [reg] and '(r1)' in ins.ops:
                note = f'; saved by {m} {ins.ops} at {ins.addr:08X}'
                break
            t = qdecomp.branch_target(ins) if m == 'bl' else None
            if t is not None and t in helpers:
                mm = re.search(r'_(\d+)$', helpers[t])
                if mm and int(mm.group(1)) <= n:
                    note = f'; saved by the helper call bl {helpers[t]} at {ins.addr:08X}'
                    break
        return f'callee-saved: the caller\'s value (in_{reg}){note}'
    return 'no defined value at entry (volatile)'


def slot_hit(items, d):
    return [(o, w) for o, w in items if o <= d < o + w]


def run(a, f, insns, con, lo, hi):
    """the --writes / --reads answer for function row f over the insns with lo <= addr <= hi -> rows (list of dicts)"""
    helpers = {r[0]: r[1] for r in con.execute("SELECT addr, name FROM funcs WHERE name GLOB '__save*' OR name GLOB '__rest*'")}
    alias = frame_aliases(insns)
    rows = []
    for mode, spec in (('writes', a.writes), ('reads', a.reads)):
        if not spec:
            continue
        kind, val = parse_target(spec)
        if mode == 'writes' and kind == 'r' and lo <= f['addr'] <= hi:
            rows.append({'mode': mode, 'addr': f['addr'], 'text': f'<entry> {val}', 'line': 0, 'why': entry_row(val, insns, helpers)})
        for ins in insns:
            if not lo <= ins.addr <= hi:
                continue
            e = effects(ins, alias, helpers)
            why = []
            if kind == 'r':
                hit = val in (e['w'] if mode == 'writes' else e['r'])
                if hit:
                    why = [e['wtag'].get(val, mode[:-1] + 's ' + val)] if mode == 'writes' else (['call: a possible argument read'] if ins.mn.rstrip('+-') in ('bl', 'bctrl', 'blrl') else ['reads ' + val])
            else:
                hits = slot_hit(e['sw'] if mode == 'writes' else e['sr'], val)
                if hits:
                    why = [f'{"stores" if mode == "writes" else "loads"} {hits[0][1]} byte(s) at sp+{hits[0][0]}']
                elif mode == 'reads' and val in e['taken']:
                    why = [f'address of sp+{val} taken']
            if why:
                rows.append({'mode': mode, 'addr': ins.addr, 'text': f'{ins.mn} {ins.ops}'.strip(), 'line': ins.line, 'why': '; '.join(why)})
    return rows, sorted(alias.items())


def main(a, f, insns, con):
    """called from qx6.asm_main when --writes / --reads is given (a = its parsed args, f = the funcs row)"""
    import json
    import qx6
    if a.into:
        raise qx6.XError('--writes / --reads does not combine with --into')
    if len(a.range) == 2:
        lo, hi = qx6.parse_addr(a.range[0]), qx6.parse_addr(a.range[1])
    elif not a.range:
        lo, hi = f['addr'], f['addr'] + f['size'] - 4
    else:
        raise qx6.XError('give FROM and TO (both), or none')
    try:
        rows, alias = run(a, f, insns, con, lo, hi)
    except RegsError as e:
        raise qx6.XError(str(e))
    spec = ' and '.join(x for x in (a.writes and f'--writes {a.writes}', a.reads and f'--reads {a.reads}') if x)
    if a.json:
        print(json.dumps({'func': f['name'], 'window': [lo, hi], 'frame_copies': dict(alias), 'rows': rows[:a.limit], 'truncated': max(0, len(rows) - a.limit)}, indent=1))
        return 0
    print_rows(f['name'], f, spec, rows[:a.limit], alias, lo, hi)
    if len(rows) > a.limit:
        print(f'... {len(rows) - a.limit} more (--limit N)')
    return 0


def print_rows(name, f, spec, rows, alias, lo, hi):
    print(f'{name}  0x{f["addr"]:08X}..0x{f["addr"] + f["size"]:08X}  ({f["file"]})')
    print(f'window 0x{lo:08X}..0x{hi:08X}: {len(rows)} row(s) for ' + spec)
    for r in rows:
        s = f'{r["addr"]:08X}  {r["text"]}'
        print(f'{s:<56} [:{r["line"]}]  {r["why"]}')
    if alias:
        print('frame copies (counted as sp): ' + ', '.join(f'{r}=sp+{d}' for r, d in alias))
    print('(a `bl` is a write of r3-r12 and a possible read of r3-r10; a store / load on r31 counts only when r31 is a frame copy, see above)')
