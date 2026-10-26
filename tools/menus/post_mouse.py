"""Session 67: post mouse input to the game window at a point of the game's 1280 x 720 screen, to check
--wwe2k14_mouse_menus without anyone at the mouse (src/mouse_menu.h). Posted window messages: no focus taken.

    py tools/menus/post_mouse.py PID X Y [--click] [--wheel N] [--stretch] [--real [--right]]

X Y = the game-screen point (the picture is 16:9, centred in the window: as the presenter letterboxes it; --stretch
for present_letterbox=false). --click = a left click there; --wheel N = N wheel detents (+ = towards you = DOWN).
"""
import argparse
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEWHEEL = 0x0200, 0x0201, 0x0202, 0x020A


def find_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        p = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("x", type=float)
    ap.add_argument("y", type=float)
    ap.add_argument("--click", action="store_true")
    ap.add_argument("--wheel", type=int, default=0)
    ap.add_argument("--right", action="store_true", help="a right click (with --real)")
    ap.add_argument("--stretch", action="store_true")
    ap.add_argument("--real", action="store_true",
                    help="move the real pointer there and click / scroll with real input (SDL takes a click's "
                         "position from the real pointer, not the message); the pointer is put back after")
    a = ap.parse_args()
    hwnd = find_window(a.pid)
    if not hwnd:
        sys.exit(f"no visible window for pid {a.pid}")
    r = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if a.stretch:
        cx, cy = a.x * w / 1280, a.y * h / 720
    else:
        k = min(w / 1280, h / 720)
        cx, cy = (w - 1280 * k) / 2 + a.x * k, (h - 720 * k) / 2 + a.y * k
    cx, cy = int(round(cx)), int(round(cy))
    if a.real:  # real input: SetCursorPos + mouse_event (the window must be in front)
        if user32.GetForegroundWindow() != hwnd:  # real clicks go to whatever window is at that point
            sys.exit("the game window is not in front: no real input sent (it would land on another window)")
        old = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(old))
        pt = wintypes.POINT(cx, cy)
        user32.ClientToScreen(hwnd, ctypes.byref(pt))
        for dx in (-6, -3, 0):
            user32.SetCursorPos(pt.x + dx, pt.y)
            time.sleep(0.05)
        if a.click:
            time.sleep(0.1)
            user32.mouse_event(0x0002, 0, 0, 0, 0)  # LEFTDOWN
            time.sleep(0.08)
            user32.mouse_event(0x0004, 0, 0, 0, 0)  # LEFTUP
        if a.right:
            time.sleep(0.1)
            user32.mouse_event(0x0008, 0, 0, 0, 0)  # RIGHTDOWN
            time.sleep(0.08)
            user32.mouse_event(0x0010, 0, 0, 0, 0)  # RIGHTUP
        for _ in range(abs(a.wheel)):
            user32.mouse_event(0x0800, 0, 0, ctypes.c_uint32(-120 if a.wheel > 0 else 120).value, 0)  # WHEEL
            time.sleep(0.12)
        time.sleep(0.3)
        user32.SetCursorPos(old.x, old.y)
        print(f"real input at screen ({pt.x}, {pt.y}) = game ({a.x:.0f}, {a.y:.0f})"
              f"{' click' if a.click else ''}{' right click' if a.right else ''}"
              f"{f' wheel {a.wheel}' if a.wheel else ''}; pointer put back")
        return
    lp = (cy << 16) | (cx & 0xFFFF)
    for dx in (-3, -1, 0):  # a short move ending on the point
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, ((cy << 16) | ((cx + dx) & 0xFFFF)))
        time.sleep(0.03)
    if a.click:
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
        time.sleep(0.08)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    if a.wheel:
        # WM_MOUSEWHEEL: wParam high word = delta (+120 = away from you), lParam = SCREEN coordinates
        pt = wintypes.POINT(cx, cy)
        user32.ClientToScreen(hwnd, ctypes.byref(pt))
        slp = ((pt.y & 0xFFFF) << 16) | (pt.x & 0xFFFF)
        for _ in range(abs(a.wheel)):
            delta = -120 if a.wheel > 0 else 120
            user32.PostMessageW(hwnd, WM_MOUSEWHEEL, (delta & 0xFFFF) << 16, slp)
            time.sleep(0.05)
    print(f"window {hwnd:#x} client {w}x{h}: game ({a.x:.0f}, {a.y:.0f}) -> client ({cx}, {cy})"
          f"{' click' if a.click else ''}{f' wheel {a.wheel}' if a.wheel else ''}")


if __name__ == "__main__":
    main()
