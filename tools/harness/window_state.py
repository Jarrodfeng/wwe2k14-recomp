"""Session 30 (#10): where the game windows are: focus, z-order, size, minimised.

  py tools/harness/window_state.py

For every wwe2k14.exe window: whether it has the focus (is the foreground window), its place in the z-order among the
visible top-level windows (1 = on top), its rectangle, and whether it is minimised. Checks the test windows of
tools/bench/run_list.ps1 (fullscreen, shown without activation and behind other windows) without asking Jarrod.
"""
import ctypes
import ctypes.wintypes as wt
import os

user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]


def exe_of(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ''
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = wt.DWORD(1024)
        return buf.value if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) else ''
    finally:
        kernel32.CloseHandle(h)


def main():
    order = []  # visible top-level windows, top of the z-order first (EnumWindows order)

    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            order.append(hwnd)
        return True

    user32.EnumWindows(EnumProc(cb), 0)
    fg = user32.GetForegroundWindow()
    found = 0
    for i, hwnd in enumerate(order, 1):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        exe = exe_of(pid.value)
        if os.path.basename(exe).lower() != 'wwe2k14.exe':
            continue
        found += 1
        r = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        title = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, title, 256)
        print('pid %d "%s": %s, z-order %d of %d visible windows, %dx%d at %d,%d%s' % (
            pid.value, title.value, 'FOCUSED' if hwnd == fg else 'not focused', i, len(order),
            r.right - r.left, r.bottom - r.top, r.left, r.top, ', MINIMISED' if user32.IsIconic(hwnd) else ''))
    if not found:
        print('no wwe2k14.exe window')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
