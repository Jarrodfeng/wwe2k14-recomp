# Session 73 (Fast shaders tinted the skin): the next bisection round - splits one group of pixel shaders (from
# out\s73\bisect_groups.txt, a line per group) into three and writes profiles_s73_bisect.tsv, each line keeping the
# characters' list + one part at full precision.
#   py tools\android\emulate\make_bisect.py GROUP_LINE_INDEX
import sys

CHARS = ("046D67063E250B56,1B66FF87A6433D53,1D20CB5D36B02E5B,2B8A869CF01395DB,3C06B3329BE81996,4C9D02186B6E5E9D,"
         "58782AADE08EE063,741439F55F5B15D4,91BF021CA7C0C1A5,925085B612A44F10,9864A5F6683152D7,9C2FD0E90E71F4A3,"
         "9F8F910305279A11,AFB4E9E073971D7A,B08B5E734D48F5D0,B1F58C0AA0E3C83A,BBB2E028DCF36E8D,C61723668154F870,"
         "CA3E51C5BA993041,D5604D4251D60877,E86BD728B5D916BA,F75AE782D8A42A61,FEE45804EDD0EC05").split(',')
R = r'C:/Users/Jarrod/wwe2k14-recomp'
groups = open(R + '/out/s73/bisect_groups.txt').read().split('\n')
group = groups[int(sys.argv[1])].split(',')
n = len(group)
parts = [group[i * n // 3:(i + 1) * n // 3] for i in range(3)]
with open(R + '/tools/android/emulate/profiles_s73_bisect.tsv', 'w') as f:
    f.write('# Session 73 part 7: which pixel shaders tint the skin at half precision (each line keeps one part full).\n')
    for i, part in enumerate(parts):
        f.write('p%d\t--gpu_native_ps_relaxed_precision=true --gpu_native_ps_full_precision_shaders=%s\n' %
                (i, ','.join(CHARS + part)))
with open(R + '/out/s73/bisect_groups.txt', 'w') as f:
    f.write('\n'.join(','.join(p) for p in parts))
print([len(p) for p in parts], parts)
