"""Session 77 (XDET_PLAN.md gap 17): count fused multiply-add instructions (AArch64 fmadd / fmsub / fnmadd / fnmsub,
fmla / fmls) per function in an Android libmain.so with symbols (out/android/symbols/libmain.so). The PC build has no
FMA target feature, so it never fuses; a fused instruction on ARM that the source did not ask for (std::fma) is a
cross-device difference by construction. Functions are grouped: generated guest code, our hooks (wwe2k14 namespace,
REX_HOOK bodies), the SDK (rex::), other (libraries). Per op the first operand's width: d (double: std::fma, which
the guest's fmadd asks for; the vector fmla v.2d are two such calls packed) or s / v.4s (float: contraction unless
the source called a float fma).
Session 77 before -ffp-contract=off (APK x77b): generated 5,895 fused, all double (std::fma); ours 201 (cinecam,
motion_fix, dlaa, ...), 136 of them float.
    py tools/xdet/fused_scan.py [libmain.so] [--list N]"""
import collections
import glob
import os
import re
import subprocess
import sys

R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FUSED = re.compile(r'\t(fmadd|fmsub|fnmadd|fnmsub|fmla|fmls)\s+([sdhv])')
FUNC = re.compile(r'^[0-9a-f]+ <(.+)>:$')


def objdump():
    ndk = sorted(glob.glob(os.path.join(os.environ['LOCALAPPDATA'], 'Android', 'Sdk', 'ndk', '*')))[-1]
    return os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin', 'llvm-objdump.exe')


def hooked_names():
    """Our REX_HOOK bodies take the guest function's name (sub_X); the generated original becomes __imp__sub_X."""
    names = set()
    for path in glob.glob(os.path.join(R, 'src', '**', '*.h'), recursive=True):
        names.update(re.findall(r'REX_HOOK\w*\((sub_[0-9A-Fa-f]{8})\)', open(path, encoding='utf-8',
                                                                               errors='replace').read()))
    return names


HOOKED = hooked_names()


def group(name):
    if name in HOOKED:
        return 'ours'
    if re.match(r'^(__imp__)?(sub|loc|fn)_8[0-9A-Fa-f]{7}', name):
        return 'generated'
    if 'wwe2k14' in name or name.startswith('_ZN7wwe2k14') or 'Hook' in name or name.startswith('__rex_hook'):
        return 'ours'
    if name.startswith('_ZN3rex') or 'rex::' in name:
        return 'sdk'
    return 'other'


def main():
    argv, top = sys.argv[1:], 20
    if '--list' in argv:
        i = argv.index('--list')
        top = int(argv[i + 1])
        del argv[i:i + 2]
    so = argv[0] if argv else os.path.join(R, 'out', 'android', 'symbols', 'libmain.so')
    p = subprocess.Popen([objdump(), '-d', '--no-show-raw-insn', so], stdout=subprocess.PIPE, text=True,
                         errors='replace', bufsize=1 << 20)
    per_func, per_group, per_op = collections.Counter(), collections.Counter(), collections.Counter()
    func = '?'
    for line in p.stdout:
        m = FUNC.match(line)
        if m:
            func = m.group(1)
            continue
        f = FUSED.search(line)
        if f:
            per_func[func] += 1
            per_group[group(func)] += 1
            # (d = double: what std::fma emits, as the guest's fmadd asks; s / v = float: contraction unless the
            # source called a float fma)
            per_op['%s %s / %s' % (group(func), f.group(1), f.group(2))] += 1
    p.wait()
    print('%s: %d fused instructions in %d functions' % (so, sum(per_func.values()), len(per_func)))
    print('by group: %s' % ', '.join('%s %d' % kv for kv in sorted(per_group.items())))
    print('by op: %s' % ', '.join('%s %d' % kv for kv in sorted(per_op.items())))
    for g in ('generated', 'ours', 'sdk', 'other'):
        names = [(n, c) for n, c in per_func.most_common() if group(n) == g]
        if names:
            print('== %s (%d functions; top %d)' % (g, len(names), top))
            for n, c in names[:top]:
                print('  %5d  %s' % (c, n[:150]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
