# Session 73 (Fast shaders tinted the skin; the bisection's fixed runs did not reproduce): three fast-forwarded replay
# configurations, one per line in out\s73\ff_extras.txt - (A) the build's default full-precision list, (B) run 4725's
# command-line list, (C) every pixel shader of the replay at full precision (sanity: about the exact picture).
import re

R = r'C:/Users/Jarrod/wwe2k14-recomp'
LOGS = R + '/out/build/win-amd64-release/logs'
t = open(LOGS + '/wwe2k14_4725.log', encoding='utf-8', errors='replace').read()
lst4725 = re.search(r"modified: gpu_native_ps_full_precision_shaders = '([0-9A-F,]+)'", t).group(1)
seen = open(LOGS + '/wwe2k14_4720.log', encoding='utf-8', errors='replace').read()
allps = sorted(set(re.findall(r'PS ([0-9A-F]{16})', seen)) | set(lst4725.split(',')))
base = ('--wwe2k14_replay_play=s21_lb --wwe2k14_use_launcher_settings=true '
        '--wwe2k14_settings_file=' + R.replace('/', '\\') + '\\out\\s72\\settings_native720.json '
        '--render_target_path_vulkan=fbo --gpu_native_ps_relaxed_precision=true')
with open(R + '/out/s73/ff_extras.txt', 'w') as f:
    f.write('\n'.join([base, base + ' --gpu_native_ps_full_precision_shaders=' + lst4725,
                       base + ' --gpu_native_ps_full_precision_shaders=' + ','.join(allps)]))
print(len(lst4725.split(',')), 'in the 4725 list;', len(allps), 'in the all list')
