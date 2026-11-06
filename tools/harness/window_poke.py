"""Session 92: poke the game window to provoke an "outdated swapchain" (minimise / restore, hide / show, resize).

  py tools/harness/window_poke.py [minimize|hide|resize|all] --exe <image path substring> [--wait 2.0] [--shots DIR]

Finds the wwe2k14.exe window (as window_state.py does), grabs the window rectangle (ImageGrab) before and after the
poke, and prints whether the picture changed. Never changes the display mode. Restores without activation
(SW_SHOWNOACTIVATE / SWP_NOACTIVATE).
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
SW_HIDE, SW_SHOWNOACTIVATE, SW_MINIMIZE, SW_SHOWMINNOACTIVE, SW_RESTORE = 0, 4, 6, 7, 9
SWP_NOACTIVATE, SWP_NOZORDER = 0x10, 0x4


def exe_of(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)
    if not h:
        return ''
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = wt.DWORD(1024)
        return buf.value if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) else ''
    finally:
        kernel32.CloseHandle(h)


def find_hwnd(path_substr):
    """Top-level window of the ONE wwe2k14.exe whose image path contains path_substr (case-insensitive).
    Returns (hwnd, pid); refuses (None) when there is none or more than one process / window."""
    out = []

    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if user32.IsWindowVisible(hwnd):
            exe = exe_of(pid.value)
            if os.path.basename(exe).lower() == 'wwe2k14.exe' and path_substr.lower() in exe.lower():
                out.append((hwnd, pid.value))
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    if len(out) != 1:
        print('refusing: %d matching windows for path containing %r' % (len(out), path_substr))
        return None
    return out[0]


def rect(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def grab(hwnd, path):
    from PIL import ImageGrab
    img = None
    for _ in range(5):
        try:
            img = ImageGrab.grab(bbox=rect(hwnd), all_screens=True)
            if path:
                img.save(path)
            return img
        except Exception as e:  # rect mid-animation / off-screen right after a restore: retry
            print('grab retry:', e)
            time.sleep(0.5)
    return img


def differs(a, b):
    if a is None or b is None:
        return None
    from PIL import ImageChops
    return ImageChops.difference(a.convert('RGB'), b.convert('RGB')).getbbox() is not None


def main():
    args = sys.argv[1:]
    wait = 2.0
    shots = None
    mode = 'minimize'
    exe_sub = None
    i = 0
    while i < len(args):
        if args[i] == '--wait':
            wait = float(args[i + 1]); i += 1
        elif args[i] == '--exe':
            exe_sub = args[i + 1]; i += 1
        elif args[i] == '--shots':
            shots = args[i + 1]; i += 1
        else:
            mode = args[i]
        i += 1
    if not exe_sub:
        print('--exe <path substring, e.g. live_s92win> is required (other games may be running)'); return 2
    found = find_hwnd(exe_sub)
    if not found:
        return 1
    hwnd, pid = found
    print('target pid', pid)
    if shots:
        os.makedirs(shots, exist_ok=True)
    r0 = rect(hwnd)
    print('hwnd %#x rect %s mode %s' % (hwnd, r0, mode))
    time.sleep(1.0)
    before = grab(hwnd, shots and os.path.join(shots, 'before.png'))
    t = time.strftime('%H:%M:%S')
    if mode in ('minimize', 'all'):
        print(t, 'SW_SHOWMINNOACTIVE'); user32.ShowWindow(hwnd, SW_SHOWMINNOACTIVE)
        time.sleep(wait)
        print(time.strftime('%H:%M:%S'), 'SW_SHOWNOACTIVATE (restore)'); user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, SW_RESTORE)
    if mode in ('hide', 'all'):
        print(time.strftime('%H:%M:%S'), 'SW_HIDE'); user32.ShowWindow(hwnd, SW_HIDE)
        time.sleep(wait)
        print(time.strftime('%H:%M:%S'), 'SW_SHOWNOACTIVATE'); user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
    if mode in ('resize', 'all'):
        l, tp, rr, b = r0
        print(time.strftime('%H:%M:%S'), 'resize smaller'); user32.SetWindowPos(hwnd, 0, l, tp, (rr - l) - 200, (b - tp) - 100, SWP_NOACTIVATE | SWP_NOZORDER)
        time.sleep(wait)
        print(time.strftime('%H:%M:%S'), 'resize back'); user32.SetWindowPos(hwnd, 0, l, tp, rr - l, b - tp, SWP_NOACTIVATE | SWP_NOZORDER)
    time.sleep(1.5)
    a1 = grab(hwnd, shots and os.path.join(shots, 'after1.png'))
    time.sleep(1.5)
    a2 = grab(hwnd, shots and os.path.join(shots, 'after2.png'))
    print('window rect after', rect(hwnd), 'iconic', bool(user32.IsIconic(hwnd)))
    print('content changed between the two after-shots (frames presenting): %s' % differs(a1, a2))
    print('content changed before vs after: %s' % differs(before, a2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
