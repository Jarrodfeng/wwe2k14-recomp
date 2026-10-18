"""Hotkey test for the inspector: waits for a NEW release log with the inspector's "panel settings drawn" line, then
posts F6 key-down/up to the game window (our own dev build) twice, DELAY_HIDE and DELAY_SHOW seconds after it.
Expect "[wwe2k14][inspector] hidden" then "shown" lines in that log. Launches nothing, kills nothing.

Start it (background) before a release run of tools/input_scripts/inspector_selftest.txt; with the defaults the
presses land after the script's own hide/show (run 285: hidden at frame 1660, shown at 1846).

    py tools\\inspector\\hotkey_test.py [delay_hide] [delay_show]
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

REPO = r'C:/Users/Jarrod/wwe2k14-recomp'
sys.path.insert(0, os.path.join(REPO, 'tools', 'review'))
import winrect  # noqa: E402

delay_hide = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
delay_show = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
LOGS = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'logs')
before = set(os.listdir(LOGS))
t0 = time.time()
found = None
while time.time() - t0 < 2400 and not found:
    for n in sorted(os.listdir(LOGS)):
        if n in before or not n.endswith('.log'):
            continue
        try:
            with open(os.path.join(LOGS, n), encoding='utf-8', errors='replace') as f:
                if '[wwe2k14][inspector] panel settings drawn' in f.read():
                    found = n
        except OSError:
            pass
    time.sleep(0.5)
if not found:
    print('no inspector log')
    sys.exit(1)
user32 = ctypes.windll.user32
user32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
user32.MapVirtualKeyW.restype = ctypes.c_uint
WM_KEYDOWN, WM_KEYUP, VK_F6 = 0x100, 0x101, 0x75
scan = user32.MapVirtualKeyW(VK_F6, 0)


def press(hwnd):
    user32.PostMessageW(hwnd, WM_KEYDOWN, VK_F6, 1 | (scan << 16))
    time.sleep(0.1)
    user32.PostMessageW(hwnd, WM_KEYUP, VK_F6, 1 | (scan << 16) | (1 << 30) | (1 << 31))


time.sleep(delay_hide)
hwnd, title = winrect.find_window()
print('log', found, 'window', title, flush=True)
press(hwnd)
print(time.strftime('%H:%M:%S'), 'F6 posted (expect hidden)', flush=True)
time.sleep(delay_show)
press(hwnd)
print(time.strftime('%H:%M:%S'), 'F6 posted (expect shown)', flush=True)
