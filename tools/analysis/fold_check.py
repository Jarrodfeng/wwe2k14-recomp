r"""Gate check for tools/analysis/fold.py (S103 lane T2). Needs the local generated code; writes under out\analysis\fold_check\.

  py -I tools\analysis\fold_check.py

  1. sub_82C04DF0 (7,942 instructions) folds to under a quarter of its size, 15 "set a parameter" copies found, and the check proves no
     instruction address is lost (every listing address is in exactly one output line).
  2. sub_82BF5C58 (the twin, Fable Q3.2): the copy is exactly 0x82BF5E94..0x82BF6510 (416 instructions) with the join 0x82BF6514.
  3. Planted twins: a deleted instruction line, a deleted fold line, a duplicated line, a wrong count and a range cut short must each
     make the address check FAIL."""
import io
import os
import re
import sys
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fold  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(ROOT, 'out', 'analysis', 'fold_check')
os.makedirs(OUT, exist_ok=True)
fails = []


def expect(name, ok):
    print(('PASS ' if ok else 'FAIL ') + name)
    if not ok:
        fails.append(name)


def run_check(fold_lines, lst_lines, tag):
    fp, lp = os.path.join(OUT, tag + '.fold'), os.path.join(OUT, tag + '.lst')
    open(fp, 'w', encoding='utf-8').write('\n'.join(fold_lines) + '\n')
    open(lp, 'w', encoding='utf-8').write('\n'.join(lst_lines) + '\n')
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = fold.check(fp, lp)
    return rc, buf.getvalue().strip()


lst = fold.make_listing('82C04DF0')
out, insns, _ = fold.fold(lst)
rc, msg = run_check(out, lst, 'c04df0')
print(msg)
expect('sub_82C04DF0: no instruction address lost', rc == 0)
items = sum(1 for ln in out if ln.startswith('0x'))
expect(f'sub_82C04DF0: {len(insns)} instructions fold to {items} items (< 25 %)', len(insns) == 7942 and items < len(insns) // 4)
heads = [ln for ln in out if re.match(r'^0x\S+ \[\d+\] SET_PARAM ', ln)]
expect(f'sub_82C04DF0: {len(heads)} set-parameter copies (the 15 headers `cmplwi rX,65535` at 0x82C05084 .. 0x82C0C174)', len(heads) == 15)

lst2 = fold.make_listing('82BF5C58')
out2, _, _ = fold.fold(lst2)
expect('sub_82BF5C58: the copy is 0x82BF5E94..0x82BF6510 [416], join 0x82BF6514',
       any(ln.startswith('0x82BF5E94..0x82BF6510 [416] SET_PARAM ') and 'skip_if[+16]==0xFFFF->0x82BF6514' in ln for ln in out2))
rc2, msg2 = run_check(out2, lst2, 'bf5c58')
print(msg2)
expect('sub_82BF5C58: no instruction address lost', rc2 == 0)

# planted twins
first_insn = next(i for i, ln in enumerate(out) if re.match(r'^0x[0-9A-F]{8}  ', ln))
t = list(out); del t[first_insn]
expect('twin: a deleted instruction line is caught', run_check(t, lst, 't1')[0] != 0)
fi = next(i for i, ln in enumerate(out) if '] SET_PARAM ' in ln)
t = list(out); del t[fi]
expect('twin: a deleted fold line is caught', run_check(t, lst, 't2')[0] != 0)
t = list(out); t.insert(first_insn, out[first_insn])
expect('twin: a duplicated line is caught', run_check(t, lst, 't3')[0] != 0)
t = list(out); t[fi] = re.sub(r'\[(\d+)\]', lambda m: '[%d]' % (int(m.group(1)) + 1), t[fi], 1)
expect('twin: a wrong count is caught', run_check(t, lst, 't4')[0] != 0)
t = list(out); t[fi] = re.sub(r'\.\.(0x[0-9A-F]{8}) ', lambda m: '..0x%08X ' % (int(m.group(1), 16) - 8) , t[fi], 1)
expect('twin: a range cut short is caught', run_check(t, lst, 't5')[0] != 0)
print('fold_check: %s' % ('OK' if not fails else 'FAILED: ' + '; '.join(fails)))
sys.exit(1 if fails else 0)
