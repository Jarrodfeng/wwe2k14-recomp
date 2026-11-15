"""L1: which registry fragment variants sample a cube at slot 13 and have no analytic twin (so a draw of them keeps its group's bake)."""
import re
import sys

text = open(sys.argv[1] if len(sys.argv) > 1 else 'src/renderer/shaders/spv/fx_registry.h', encoding='utf-8').read()
samplers = {}
for m in re.finditer(r'inline constexpr Sampler kS_(\w+)\[\] = \{(.*?)\};', text):
    samplers[m.group(1)] = re.findall(r'\{(\d+), (\d+)\}', m.group(2))
names = set(re.findall(r'^  \{"(\w+)",', text, re.M))
bad, ok = [], 0
for ident, s in samplers.items():
    if ('13', '1') in s and ident.endswith('_fragment'):
        if ident.replace('_fragment', '_pl_fragment') in names or ident.endswith('_pl_fragment'):
            ok += 1
        else:
            bad.append(ident)
print('twinned', ok, 'untwinned', len(bad))
for b in bad:
    print('  ', b)
