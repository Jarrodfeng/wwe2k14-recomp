# Session 73 (Fast shaders tinted the skin): bisection helpers that never trust run order.
#   py tools\perf\fp16_bisect.py make HASH,HASH,...   -> out\s73\ff_extras.txt: three lines, each the characters'
#                                                       list + one third of the given hashes at full precision
#   py tools\perf\fp16_bisect.py read RUN RUN ...     -> per run: the extra hashes it kept full (read from its own log)
#                                                       and the skin region's error against run 4664
import re
import sys

from PIL import Image, ImageChops

R = r'C:/Users/Jarrod/wwe2k14-recomp'
LOGS = R + '/out/build/win-amd64-release/logs'
CAP = R + '/out/build/win-amd64-release/captures'
CROP = (440, 420, 760, 600)
CHARS = ("046D67063E250B56,1B66FF87A6433D53,1D20CB5D36B02E5B,2B8A869CF01395DB,3C06B3329BE81996,4C9D02186B6E5E9D,"
         "58782AADE08EE063,741439F55F5B15D4,91BF021CA7C0C1A5,925085B612A44F10,9864A5F6683152D7,9C2FD0E90E71F4A3,"
         "9F8F910305279A11,AFB4E9E073971D7A,B08B5E734D48F5D0,B1F58C0AA0E3C83A,BBB2E028DCF36E8D,C61723668154F870,"
         "CA3E51C5BA993041,D5604D4251D60877,E86BD728B5D916BA,F75AE782D8A42A61,FEE45804EDD0EC05").split(',')
BASE = ('--wwe2k14_replay_play=s21_lb --wwe2k14_use_launcher_settings=true '
        '--wwe2k14_settings_file=C:\\Users\\Jarrod\\wwe2k14-recomp\\out\\s72\\settings_native720.json '
        '--render_target_path_vulkan=fbo --gpu_native_ps_relaxed_precision=true')


def make(hashes):
    n = len(hashes)
    parts = [hashes[i * n // 3:(i + 1) * n // 3] for i in range(3)]
    with open(R + '/out/s73/ff_extras.txt', 'w') as f:
        f.write('\n'.join(BASE + ' --gpu_native_ps_full_precision_shaders=' + ','.join(CHARS + p) for p in parts))
    print(parts)


def read(runs):
    ref = Image.open('%s/4664_s72_match.png' % CAP).convert('RGB').crop(CROP)
    for run in runs:
        text = open('%s/wwe2k14_%s.log' % (LOGS, run), encoding='utf-8', errors='replace').read()
        m = re.search(r"modified: gpu_native_ps_full_precision_shaders = '([0-9A-F,]+)'", text)
        extra = sorted(set(m.group(1).split(',')) - set(CHARS)) if m else ['(default list)']
        img = Image.open('%s/%s_s72_match.png' % (CAP, run)).convert('RGB').crop(CROP)
        bad = sum(ImageChops.difference(ref, img).convert('L').histogram()[17:])
        print(run, 'skin px > 16: %5d' % bad, 'full:', ','.join(extra))


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(sys.argv[2].split(','))
    else:
        read(sys.argv[2:])
