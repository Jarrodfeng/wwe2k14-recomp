# Session 73 (Fast shaders tinted the skin; more than one shader is needed at full precision): for the parts in
# out\s73\bisect_groups.txt, one profile per part that keeps every OTHER part (+ the characters) at full precision.
# A run that is fixed says its left-out part holds no culprit.
#   py tools\android\emulate\make_bisect_complement.py
from make_bisect_chars import CHARS

R = r'C:/Users/Jarrod/wwe2k14-recomp'
parts = [p.split(',') for p in open(R + '/out/s73/bisect_groups.txt').read().split('\n')]
with open(R + '/tools/android/emulate/profiles_s73_bisect.tsv', 'w') as f:
    f.write('# Session 73 part 7: complement round (each line leaves one part relaxed).\n')
    for i in range(len(parts)):
        full = [h for j, p in enumerate(parts) if j != i for h in p]
        f.write('without%d\t--gpu_native_ps_relaxed_precision=true --gpu_native_ps_full_precision_shaders=%s\n' %
                (i, ','.join(CHARS + full)))
print('parts', parts)
