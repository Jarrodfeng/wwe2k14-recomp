"""S102 W3 gate check: the opcode table in src\\nf\\replay_native.h (kind, handler and the lr a case sets) equals what the local generated code says about the
recompiled dispatcher sub_82BB53D0 (tools\\w3\\classify_dispatch.py; a case counts as `Call` only when its non-comment lines are exactly load / lr / call / goto),
and the generated text the native loop mirrors is the text it was written from (sha256 pins of sub_82BB5BE8, the dispatcher's entry and its epilogue).
A drift (a regenerated game, a hand edit) fails loudly; a missing input is exit 2, never a pass.

    py -I tools\\w3\\check_table.py --gen C:\\Users\\Jarrod\\wwe2k14-recomp\\generated\\default [--plant NAME] [--expect-fail]

--plant: a planted twin, the check must FAIL on it (with --expect-fail the exit is 0 when it did):
  kind    one Call entry of the header's table is changed to Exit
  lr      one Call entry's lr is changed
  probe   `probe(ctx, base);` is inserted into a copy of the generated dispatcher's op 35 block (the native code would skip the probe)
  loop    one line of a copy of the generated sub_82BB5BE8 is changed (the pin must notice)
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import classify_dispatch as cd  # noqa: E402

HEADER = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'src', 'nf', 'replay_native.h')
PINS = {
    'loop_5BE8': 'c4efbd85c17cb565a3892ad7234c1a52d8c8fa43ef83bd09e4644f01e029381f',
    'dispatcher_entry': 'b616c5702b6feef50b3b932960b698e2808f2408f7da0c0993a91bed7291f548',
    'dispatcher_epilogue': 'a258ba7dad0824d74641ca882a3b96f7b6f4918d4a5480298e004429f6464890',
}


def header_table(text):
    t = {}
    for m in re.finditer(r'T\((\d+), (Exit|Call|Fall), (nullptr|sub_[0-9A-F]{8}), (0x[0-9A-F]{8}|0)\)', text):
        t[int(m.group(1))] = (m.group(2), m.group(3), int(m.group(4), 16))
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--plant', choices=['kind', 'lr', 'probe', 'loop'])
    ap.add_argument('--expect-fail', action='store_true', help='exit 0 when the check FAILS (the gate runs a planted twin this way: a twin that is caught passes)')
    a = ap.parse_args()
    if not os.path.exists(HEADER):
        print('check_table: %s is missing' % HEADER)
        return 2
    text = open(HEADER, encoding='utf-8').read()
    disp = cd.function_text(a.gen, 'sub_82BB53D0')
    loop = cd.function_text(a.gen, 'sub_82BB5BE8')
    if a.plant:
        if a.plant == 'kind':
            new = text.replace('T(35, Call, sub_82C1A2A8, 0x82BB5', 'T(35, Exit, nullptr, 0x82BB5', 1)
            planted = new != text
            text = new
        elif a.plant == 'lr':
            m = re.search(r'T\(35, Call, sub_82C1A2A8, (0x[0-9A-F]{8})\)', text)
            new = text.replace(m.group(0), m.group(0).replace(m.group(1), '0x82BB5000'), 1) if m else text
            planted, text = new != text, new
        elif a.plant == 'probe':
            i = disp.index('sub_82C1A2A8(ctx, base);')
            new = disp[:i] + 'probe(ctx, base);\n\t' + disp[i:]
            planted, disp = new != disp, new
        else:
            new = loop.replace('cr6.compare<int32_t>(ctx.r3.s32, 105, xer);', 'cr6.compare<int32_t>(ctx.r3.s32, 104, xer);', 1)
            planted, loop = new != loop, new
        if not planted:
            print('check_table: THE PLANT DID NOT APPLY (%s)' % a.plant)
            return 2
    ours = header_table(text)
    gen = cd.classify_text(disp)
    problems = []
    if sorted(ours) != list(range(105)):
        problems.append('the header table does not hold ops 0..104 exactly (%d entries)' % len(ours))
    for op, (kind, h, lr) in gen.items():
        want = ({'exit': 'Exit', 'simple': 'Call', 'other': 'Fall'}[kind], h if kind == 'simple' else 'nullptr', lr if kind == 'simple' else 0)
        if ours.get(op) != want:
            problems.append('op %d: header %s, generated code %s' % (op, ours.get(op), want))
    import hashlib
    head = disp[:disp.index('switch (ctx.r11.u32)')]
    epi = disp[disp.index('%s:' % cd.EXIT_LABEL):]
    got = {'loop_5BE8': loop, 'dispatcher_entry': head, 'dispatcher_epilogue': epi}
    for k, t in got.items():
        if hashlib.sha256(t.encode('utf-8')).hexdigest() != PINS[k]:
            problems.append('the generated %s changed since the native code was written from it (pinned sha256 differs): read it again' % k)
    for p in problems[:12]:
        print('check_table: ' + p)
    print('check_table: %s' % ('FAIL (%d problems)' % len(problems) if problems else 'PASS (105 ops equal, 3 pins equal)'))
    failed = bool(problems)
    if a.expect_fail:
        return 0 if failed else 1
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
