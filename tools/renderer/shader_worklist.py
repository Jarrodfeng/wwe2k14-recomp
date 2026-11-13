#!/usr/bin/env python3
"""Which game effects still need writing, ranked by how many draws in real captures use them. Also proves the adapter's shader hashes: every hash a capture
names must be in db\\shaders\\inventory.json. Hashes and names only.

  py tools\\renderer\\shader_worklist.py [capture folders under out\\captures, default all]
"""
import collections
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
PLAYER = os.path.join(ROOT, 'out', 'renderer', 'release', 'rplay.exe')


def main():
    with open(os.path.join(ROOT, 'db', 'shaders', 'inventory.json'), encoding='utf-8') as f:
        inventory = json.load(f)
    owner = {}
    for name, effect in inventory['effects'].items():
        for row in effect.get('hashes', []):  # 'PS FD103DDE03710DCB g2'
            owner[row.split()[1].upper()] = name
    folders = sys.argv[1:] or sorted(d for d in glob.glob(os.path.join(ROOT, 'out', 'captures', '*')) if os.path.isdir(d))
    by_effect = collections.Counter()
    unknown = collections.Counter()
    bound = total = 0
    for folder in folders:
        out = subprocess.run([PLAYER, '--capture', folder, '--frames', '100000'], capture_output=True, text=True).stderr
        match = re.search(r'capture: (\d+) frames, (\d+) draws, (\d+) bound', out)
        if match:
            total += int(match.group(2))
            bound += int(match.group(3))
        for shader, draws in re.findall(r'missing shader ([0-9A-F]{16}): (\d+) draws', out):
            if shader in owner:
                by_effect[owner[shader]] += int(draws)
            else:
                unknown[shader] += int(draws)
    print(f'{total} draws, {bound} bound ({100.0 * bound / max(total, 1):.1f}%)')
    print(f'hashes not in the inventory: {len(unknown)}')
    for shader, draws in unknown.most_common(10):
        print(f'  {shader}: {draws}')
    print('effects to write, by shader-draws in these captures:')
    for name, draws in by_effect.most_common():
        print(f'  {name}: {draws}')


if __name__ == '__main__':
    sys.exit(main())
