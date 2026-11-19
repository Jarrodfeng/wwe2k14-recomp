#!/usr/bin/env python3
"""S103 I3 review (item 2): the 2D layer's register shadow must be fed ONLY where the original op-21 recorder runs. In src/scene_crowd.h's hook on sub_82BB27D8 every call of
__imp__sub_82BB27D8 must be immediately preceded (previous code line) by wwe2k14::q2d_tap::OnOp21Regs, every OnOp21Regs must be followed by that call, and OnOp21Regs must appear nowhere else
in the hook (not before the crowd skips, not on the mode-1 path that never calls the recorder).   py -I tools/scene/shadow_site_lint.py [--root DIR]"""
import os, re, sys
root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
src = open(os.path.join(root, 'src', 'scene_crowd.h'), encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
i = src.index('REX_HOOK_RAW(sub_82BB27D8)')
j = src.index('\nREX_HOOK_RAW(', i + 10)
body = src[i:j].split('\n')
code = [(n, re.sub(r'//.*$', '', l).strip()) for n, l in enumerate(body) if re.sub(r'//.*$', '', l).strip()]
calls = [k for k, (n, l) in enumerate(code) if '__imp__sub_82BB27D8(ctx, base);' in l]
regs = [k for k, (n, l) in enumerate(code) if 'OnOp21Regs(' in l]
bad = []
for k in calls:
    if k == 0 or 'OnOp21Regs(' not in code[k - 1][1]:
        bad.append('call at hook line %d has no OnOp21Regs just before it' % code[k][0])
for k in regs:
    if k + 1 >= len(code) or '__imp__sub_82BB27D8(ctx, base);' not in code[k + 1][1]:
        bad.append('OnOp21Regs at hook line %d is not followed by the original recorder call' % code[k][0])
ok = not bad and len(calls) >= 4 and len(calls) == len(regs)
print(('PASS' if ok else 'FAIL') + '  shadow_site: %d original-recorder calls, %d OnOp21Regs, %s' % (len(calls), len(regs), '; '.join(bad) or 'each shadow update sits right before an original call'))
sys.exit(0 if ok else 1)
