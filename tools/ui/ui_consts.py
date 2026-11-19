r"""PN5 review B1: the UI index's tier constants, read from the shipped header (src\renderer_scene\ui_file_index.h) so the checks never hard-code them.
A tool that needs "how many pacs the first tier indexes" or "how many textures that is on the player's files" asks here:
  tier1_count()            kUiTier1Count (the pacs [0, kUiTier1Count) indexed at start-up)
  index_summary(text)      (textures, pacs) from a `index N textures in M pacs` line, or None
  pinned_textures()        the texture count of the first tier on the player's files, pinned in tools\ui\ui_tier1_textures.txt (re-pin: `out\tools\ui_ours_test.exe index`)."""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
HEADER = os.path.join(ROOT, 'src', 'renderer_scene', 'ui_file_index.h')
PIN = os.path.join(HERE, 'ui_tier1_textures.txt')


def tier1_count(header=HEADER):
    m = re.search(r'kUiTier1Count\s*=\s*(\d+)\s*;', open(header, encoding='utf-8').read())
    if not m:
        raise SystemExit('kUiTier1Count not found in %s' % header)
    return int(m.group(1))


def index_summary(text):
    m = re.search(r'index (\d+) textures in (\d+) pacs', text)
    return (int(m.group(1)), int(m.group(2))) if m else None


def pinned_textures(pin=PIN):
    """The pinned first-tier texture count (the first integer on a non-comment line); a missing pin is a loud failure, never a guess."""
    for line in open(pin, encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#'):
            return int(line.split()[0])
    raise SystemExit('no pinned texture count in %s' % pin)
