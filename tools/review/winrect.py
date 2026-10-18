"""Find the game window and its client rectangle in physical desktop pixels (DPI-aware).

    py tools\\review\\winrect.py            prints the desktop size and the wwe2k14 window's client rect

Used by record.py to crop the desktop capture to the game's client area.
"""
import ctypes
import ctypes.wintypes as wt
import json
import sys

user32 = ctypes.windll.user32
try:  # per-monitor DPI aware, so every rectangle below is in physical pixels
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except OSError:
    user32.SetProcessDPIAware()


def desktop_size():
    return user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)


def find_window(title_prefix='wwe2k14'):
    found = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if buf.value.startswith(title_prefix + ' [') or buf.value == title_prefix:
            found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(proto(cb), 0)
    return found[0] if found else (None, None)


def client_rect(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return {'x': pt.x, 'y': pt.y, 'w': r.right - r.left, 'h': r.bottom - r.top}


HWND_TOPMOST = -1
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE, SWP_SHOWWINDOW = 0x1, 0x2, 0x10, 0x40


def make_topmost(hwnd):
    """Put the game window above every other window without activating it (no focus change), so a desktop
    capture sees the game and not whatever app is in front (the first capture, run 186, recorded the Claude app).
    The state goes away when the game exits."""
    user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_uint]
    return bool(user32.SetWindowPos(hwnd, wt.HWND(HWND_TOPMOST), 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))


def window_info():
    hwnd, title = find_window()
    info = {'desktop': desktop_size(), 'hwnd': hwnd, 'title': title}
    if hwnd:
        info['client'] = client_rect(hwnd)
        wr = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(wr))
        info['window'] = {'x': wr.left, 'y': wr.top, 'w': wr.right - wr.left, 'h': wr.bottom - wr.top}
    return info


def keep_topmost_while_lock(prefix, lock_json):
    """Loop: while the run lock is held by a holder whose name starts with `prefix`, keep the game window topmost.
    Exits when the lock is held by someone else or free for 10 minutes."""
    import time
    idle = 0
    while idle < 600:
        try:
            with open(lock_json, encoding='utf-8-sig') as f:
                name = json.load(f).get('name', '')
        except (OSError, ValueError):
            name = ''
        if name.startswith(prefix):
            idle = 0
            hwnd, _ = find_window()
            if hwnd:
                make_topmost(hwnd)
        else:
            idle += 1
        time.sleep(1)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--keep-topmost-while-lock':
        keep_topmost_while_lock(sys.argv[2], sys.argv[3])
    else:
        json.dump(window_info(), sys.stdout, indent=1)
        print()
