# Session 73 (Fast shaders tinted the wrestlers' skin): per replay run, how far the match shot is from the exact
# reference (run 4664) inside the skin test region (Lesnar's back and Bryan, 440,420-760,600) and in the whole shot.
#   py tools\perf\skin_diff.py RUN[=LABEL] ...
import sys

from PIL import Image, ImageChops

CAP = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/captures'
CROP = (440, 420, 760, 600)


def main():
    ref = Image.open('%s/4664_s72_match.png' % CAP).convert('RGB')
    for arg in sys.argv[1:]:
        run, _, label = arg.partition('=')
        img = Image.open('%s/%s_s72_match.png' % (CAP, run)).convert('RGB')
        whole = ImageChops.difference(ref, img).convert('L').histogram()
        skin = ImageChops.difference(ref.crop(CROP), img.crop(CROP)).convert('L').histogram()
        print('%s %-10s skin region: %6d px > 16 (max %3d) | whole shot: %6d px > 16, %6d differ' % (
            run, label, sum(skin[17:]), max(i for i, v in enumerate(skin) if v), sum(whole[17:]), sum(whole[1:])))


if __name__ == '__main__':
    main()
