"""Summarise an input recording (--wwe2k14_input_record) as absolute-frame button presses, trigger pulls,
left-stick flicks and file loads, to turn it into a replay script with wait_file sync points.

Usage: py tools/input_scripts/summarize_recording.py <recording.txt>
"""
import sys

BUTTONS = {0x0001: 'UP', 0x0002: 'DOWN', 0x0004: 'LEFT', 0x0008: 'RIGHT', 0x0010: 'START', 0x0020: 'BACK',
           0x0040: 'LS', 0x0080: 'RS', 0x0100: 'LB', 0x0200: 'RB', 0x1000: 'A', 0x2000: 'B', 0x4000: 'X',
           0x8000: 'Y'}
NOISY_FILES = ('Cache1', 'partition0', 'plist360', 'Sound\\', '_startup')
STICK = 20000

events = []  # (frame, text); presses get their hold length appended once released
held = {}  # button/trigger/stick name -> index into events of its press
frame = 0
prev_buttons = prev_lt = prev_rt = 0
prev_stick = None


def press(name):
    held[name] = len(events)
    events.append([frame, f'press {name}'])


def release(name):
    if name in held:
        i = held.pop(name)
        events[i][1] += f'  (held {frame - events[i][0]} frames)'


for line in open(sys.argv[1], encoding='utf-8'):
    words = line.split()
    if not words:
        continue
    if words[0] == 'wait_frames':
        frame += int(words[1])
    elif words[0] == 'set':
        buttons = int(words[1], 16)
        lt, rt, lx, ly = (int(w) for w in words[2:6])
        for mask, name in BUTTONS.items():
            if buttons & mask and not prev_buttons & mask:
                press(name)
            elif prev_buttons & mask and not buttons & mask:
                release(name)
        for name, value, previous in (('LT', lt, prev_lt), ('RT', rt, prev_rt)):
            if value > 30 >= previous:
                press(name)
            elif previous > 30 >= value:
                release(name)
        stick = None
        if abs(lx) > STICK or abs(ly) > STICK:
            stick = 'L_' + ('RIGHT' if lx > STICK else 'LEFT' if lx < -STICK else '') + \
                    ('UP' if ly > STICK else 'DOWN' if ly < -STICK else '')
        if stick != prev_stick:
            if prev_stick:
                release(prev_stick)
            if stick:
                press(stick)
        prev_stick = stick
        prev_buttons, prev_lt, prev_rt = buttons, lt, rt
    elif line.startswith('# frame') and ' opened ' in line and not any(n in line for n in NOISY_FILES):
        events.append([frame, f'-- {line.split(" opened ", 1)[1].strip()}'])

for name in list(held):
    events[held[name]][1] += '  (still held at the end)'
for when, text in events:
    print(f'{when:6d}  {text}')
