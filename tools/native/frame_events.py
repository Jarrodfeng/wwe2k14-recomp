"""One frame of a segment log as its events: pass starts (with the first draw's shaders), batch flushes, copies
(resolves), serves, renames and dead passes - no per-draw lines. Session 80 (serving the scene copies).

    py -I tools\\native\\frame_events.py LOG FRAME [PAGE ...]

With PAGEs (hex, e.g. 108BE 1F0CA), also the draws whose textures include one of them.
"""
import re
import sys

log, frame = sys.argv[1], sys.argv[2]
pages = [p.upper().lstrip('0X') for p in sys.argv[3:]]
tag = re.compile(r'\[native\]\[(seg|truth)\] frame (\d+) (.*)')
events = re.compile(r'truth pass:|batch flush|serve|rename|resolve|dead pass|memory resolve|deferred')
n = -1
last_draw = ''
with open(log, encoding='utf-8', errors='replace') as f:
    for line in f:
        m = tag.search(line)
        if not m or m.group(2) != frame:
            continue
        text = m.group(3)
        if text.startswith('truth pass:'):
            n += 1
        if text.startswith('truth draw:'):
            last_draw = text
            continue
        if text.startswith('draw textures:'):
            if pages and any('0x' + p in text.upper().replace('0X', '0x') for p in pages):
                print(f'[{n}]   {last_draw[:150]}')
                print(f'[{n}]     {text[:230]}')
            continue
        if events.search(text):
            print(f'[{n}] {text[:260]}')
