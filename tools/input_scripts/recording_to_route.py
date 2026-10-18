"""Session 30 (#1 / carried-over idea 3): turn an input recording into a state-driven route.

  py tools/input_scripts/recording_to_route.py RECORDING.txt [-o tools/input_scripts/short/NAME.txt] [--window 90]

A recording (--wwe2k14_input_record=FILE; games built in Session 30 or later) has the presses ("set" lines with
"wait_frames" between them), "# frame F: opened PATH" lines and "# frame F state NAME = V" lines whenever a route
state changed (boot_step, seq, menu, cursor, target, logo_*: src/input_script.h RecordStates). For each press this
first waits for the last game file that opened since the previous press (a load the press needs, e.g. SaveData.Dat
before the title takes START), then pairs it with what the press changed within --window frames (before the next
press):
  - a state changed -> "press_until_state BUTTON NAME VALUE" (works fast-forwarded: a dropped press is pressed again),
    with a "wait_state NAME BEFORE" first when the route is not known to be there already;
  - else a game file opened -> "press_until_file BUTTON FILE";
  - else the recorded timing: "wait_frames GAP" + "press BUTTON HOLD" (marked; such a route wants normal speed).
States are preferred in this order: menu, logo_screen, logo_phase, target, cursor, the logo cursors, boot_step
(it counts every accepted menu press, so only the title presses use it), seq.
The output starts with `on_fail quit` and ends with `quit`; read it through before using it as a test.
"""
import argparse
import os
import re
import sys

BUTTONS = {0x0001: 'UP', 0x0002: 'DOWN', 0x0004: 'LEFT', 0x0008: 'RIGHT', 0x0010: 'START', 0x0020: 'BACK',
           0x0040: 'LS', 0x0080: 'RS', 0x0100: 'LB', 0x0200: 'RB', 0x1000: 'A', 0x2000: 'B', 0x4000: 'X',
           0x8000: 'Y'}
STICK = 20000
# boot_step last but one: it also counts every accepted menu press (states.toml), so its absolute values after the boot
# depend on the whole history; it is the only signal of the title presses.
PRIORITY = ['menu', 'logo_screen', 'logo_phase', 'target', 'cursor', 'logo_grid_cursor', 'logo_size_cursor',
            'logo_tool_cursor', 'logo_category_cursor', 'boot_step', 'seq']
NOISY_FILES = ('cache1', 'partition0', 'plist360', 'sound\\', '_startup', '.xex')
STATE = re.compile(r'^# frame (\d+) state (\w+) = (\d+)')
OPENED = re.compile(r'^# frame (\d+): opened (.+)$')


def parse(path):
    """-> presses [(frame, name, hold)], states [(frame, name, value)], files [(frame, path)]."""
    presses, states, files = [], [], []
    held = {}
    frame = 0
    prev_buttons = prev_lt = prev_rt = 0
    prev_stick = None

    def down(name):
        held[name] = len(presses)
        presses.append([frame, name, 5])

    def up(name):
        if name in held:
            i = held.pop(name)
            presses[i][2] = max(1, frame - presses[i][0])

    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        m = STATE.match(line)
        if m:
            states.append((int(m.group(1)), m.group(2), int(m.group(3))))
            continue
        m = OPENED.match(line)
        if m:
            files.append((int(m.group(1)), m.group(2).strip()))
            continue
        words = line.split()
        if not words or words[0].startswith('#'):
            continue
        if words[0] == 'wait_frames':
            frame += int(words[1])
        elif words[0] == 'set':
            buttons = int(words[1], 16)
            lt, rt, lx, ly = (int(w) for w in words[2:6])
            for mask, name in BUTTONS.items():
                if buttons & mask and not prev_buttons & mask:
                    down(name)
                elif prev_buttons & mask and not buttons & mask:
                    up(name)
            for name, value, previous in (('LT', lt, prev_lt), ('RT', rt, prev_rt)):
                if value > 30 >= previous:
                    down(name)
                elif previous > 30 >= value:
                    up(name)
            stick = None
            if abs(lx) > STICK or abs(ly) > STICK:
                stick = 'L_' + ('RIGHT' if lx > STICK else 'LEFT' if lx < -STICK else '') + \
                        ('UP' if ly > STICK else 'DOWN' if ly < -STICK else '')
            if stick != prev_stick:
                if prev_stick:
                    up(prev_stick)
                if stick:
                    down(stick)
            prev_stick = stick
            prev_buttons, prev_lt, prev_rt = buttons, lt, rt
    return [tuple(p) for p in presses], states, files


def value_at(states, name, frame):
    """The last recorded value of NAME at or before FRAME (None if never)."""
    v = None
    for f, n, val in states:
        if f > frame:
            break
        if n == name:
            v = val
    return v


def convert(path, window):
    presses, states, files = parse(path)
    out = ['# Made from the recording %s by tools/input_scripts/recording_to_route.py.' % os.path.basename(path),
           '# Each press waits for the state it changed in the recording (press_until_state) or the file it opened;',
           '# presses that changed neither keep the recorded timing (marked "timing").',
           'on_fail quit', '']
    known = {}  # state -> value the route is known to be at
    timed = 0
    prev_frame = 0
    prev_press = -1
    for i, (frame, button, hold) in enumerate(presses):
        loads = [p for f, p in files if prev_press < f <= frame and not any(x in p.lower() for x in NOISY_FILES)]
        if loads:
            out.append('wait_file %s 3600' % re.split(r'[\\/]', loads[-1])[-1])
        prev_press = frame
        nxt = presses[i + 1][0] if i + 1 < len(presses) else frame + window
        end = min(nxt, frame + window)
        changed = {}
        for f, n, val in states:
            if frame < f <= end and n not in changed and val != value_at(states, n, frame):
                changed[n] = val
        pick = next((n for n in PRIORITY if n in changed), None)
        if pick:
            before = value_at(states, pick, frame)
            if before is not None and known.get(pick) != before:
                out.append('wait_state %s %d 1800 2' % (pick, before))
            out.append('press_until_state %s %s %d 24 30 %d' % (button, pick, changed[pick], min(hold, 20)))
            known[pick] = changed[pick]
            for n, val in changed.items():
                known[n] = val
        else:
            opened = next((p for f, p in files if frame < f <= end and not any(x in p.lower() for x in NOISY_FILES)),
                          None)
            retry = (i + 1 < len(presses) and presses[i + 1][1] == button and presses[i + 1][0] - frame <= 120)
            if opened:
                name = re.split(r'[\\/]', opened)[-1]
                out.append('press_until_file %s %s 3 %d %d' % (button, name, max(60, end - frame), min(hold, 20)))
            elif retry:
                # Nothing happened and the same button follows soon: a dropped press the player (or script) repeated.
                # The next press's press_until_state presses again by itself if needed.
                out.append('# (frame %d: %s changed nothing and was pressed again: left out)' % (frame, button))
                continue
            else:
                timed += 1
                gap = frame - prev_frame
                if gap > 0:
                    out.append('wait_frames %d   # timing (frame %d)' % (gap, frame))
                out.append('press %s %d   # timing: no state or file changed after this press' % (button, hold))
            known.clear()
        prev_frame = frame + hold
    out += ['', 'quit', '']
    return out, len(presses), timed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('recording')
    ap.add_argument('-o', '--out')
    ap.add_argument('--window', type=int, default=90, help='frames after a press to look for its effect (90)')
    a = ap.parse_args()
    lines, n, timed = convert(a.recording, a.window)
    if timed:
        lines.insert(3, '# harness: realtime (%d press(es) keep the recorded timing)' % timed)
    text = '\n'.join(lines)
    if a.out:
        with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print('%s: %d presses, %d by state or file, %d by timing' % (a.out, n, n - timed, timed))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
