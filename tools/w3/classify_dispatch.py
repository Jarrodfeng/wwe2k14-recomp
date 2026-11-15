"""S102 W3: read the recompiled render-command dispatcher sub_82BB53D0 (local generated code, never committed) and classify every opcode's case:
  exit    the case goes straight to the epilogue (the record does nothing)
  simple  the case is EXACTLY (non-comment lines): `ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0);`, `ctx.lr = 0x...;`, `sub_X(ctx, base);`, `goto loc_82BB5BD0;`
          (r3 = the record, the return address the case sets, one call, back to the epilogue); anything else in the block (a probe call, a store) makes it `other`
  other   anything else (inline code, several calls, loops): the native dispatcher hands these to the recompiled function
`simple` cases carry their handler and the lr they set (the native code sets the same lr before a direct handler call).
Also pins the generated text the native loop mirrors: sha256 of sub_82BB5BE8's body, of sub_82BB53D0's entry (up to the switch) and of its epilogue.

  py -I tools\\w3\\classify_dispatch.py --gen DIR [--emit] [--table] [--hashes]
"""
import argparse
import glob
import hashlib
import re
import sys

EXIT_LABEL = 'loc_82BB5BD0'


def function_text(gen, func):
    for p in sorted(glob.glob(gen + '/wwe2k14_recomp.*.cpp')):
        t = open(p, encoding='utf-8', errors='replace').read()
        i = t.find('DEFINE_REX_FUNC(%s)' % func)
        if i >= 0:
            return t[i:t.find('\n}\n', i)]
    sys.exit('function %s not found under %s' % (func, gen))


def code_lines(block):
    """The non-comment, non-label lines of a text block (stripped)."""
    out = []
    for line in block:
        s = line.strip()
        if not s or s.startswith('//') or re.match(r'loc_[0-9A-F]{8}:$', s):
            continue
        out.append(s)
    return out


def classify_text(text):
    sw = re.search(r'switch \(ctx\.r11\.u32\) \{(.*?)\n\t\}', text, re.S)
    if not sw:
        sys.exit('no switch in the dispatcher text')
    cases = {int(m.group(1)): m.group(2) for m in re.finditer(r'case (\d+):\s*goto (loc_[0-9A-F]+);', sw.group(1))}
    blocks, cur = {}, None
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r'(loc_[0-9A-F]{8}):', s)
        if m:
            cur = m.group(1)
            blocks[cur] = []
        elif cur:
            blocks[cur].append(line)
    out = {}
    for op in range(0, 105):
        lab = cases.get(op)
        if lab is None:
            out[op] = ('missing', None, 0)
        elif lab == EXIT_LABEL:
            out[op] = ('exit', None, 0)
        else:
            b = code_lines(blocks.get(lab, []))
            ok = (len(b) == 4 and b[0] == 'ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 0);' and re.match(r'ctx\.lr = 0x[0-9A-F]{8};$', b[1])
                  and re.match(r'sub_[0-9A-F]{8}\(ctx, base\);$', b[2]) and b[3] == 'goto %s;' % EXIT_LABEL)
            if ok:
                out[op] = ('simple', b[2].split('(')[0], int(b[1].split('=')[1].strip(' ;'), 16))
            else:
                out[op] = ('other', ';'.join(b[:6]), 0)
    return out


def classify(gen, func='sub_82BB53D0'):
    return classify_text(function_text(gen, func))


def hashes(gen):
    loop = function_text(gen, 'sub_82BB5BE8')
    disp = function_text(gen, 'sub_82BB53D0')
    head = disp[:disp.index('switch (ctx.r11.u32)')]
    epi = disp[disp.index('%s:' % EXIT_LABEL):]
    h = lambda t: hashlib.sha256(t.encode('utf-8')).hexdigest()
    return {'loop_5BE8': h(loop), 'dispatcher_entry': h(head), 'dispatcher_epilogue': h(epi)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--emit', action='store_true')
    ap.add_argument('--table', action='store_true', help='print the full op table as the native header X-table lines')
    ap.add_argument('--hashes', action='store_true')
    a = ap.parse_args()
    if a.hashes:
        for k, v in hashes(a.gen).items():
            print(k, v)
        return
    out = classify(a.gen)
    n = {k: sum(1 for v in out.values() if v[0] == k) for k in ('exit', 'simple', 'other', 'missing')}
    if a.table:
        for op, (k, h, lr) in out.items():
            print('  T(%d, %s, %s, %s)' % (op, {'exit': 'Exit', 'simple': 'Call', 'other': 'Fall'}[k], h if k == 'simple' else 'nullptr', '0x%08X' % lr if k == 'simple' else '0'))
        return
    print('ops 0..104:', n, '(> 104 exits at the first compare)')
    for op, (k, h, lr) in out.items():
        if a.emit:
            if k == 'simple':
                print('  X(%d, %s)' % (op, h))
        else:
            print('%3d %-7s %s %s' % (op, k, h or '', '0x%08X' % lr if lr else ''))


if __name__ == '__main__':
    main()
