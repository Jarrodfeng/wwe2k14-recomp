"""Session 76: hold keyboard keys in the running game without a person at the keyboard (tests of the keyboard player).

Posts WM_KEYDOWN / WM_KEYUP to the game's window only (PostMessage, not SendInput: nothing reaches another program if
the game loses focus). The keyboard driver (ReXGlue MnkInputDriver, --mnk_mode=true) takes keys only while the game
window has focus, so start the game with the window activated (live game_start window "foreground").

  py tools\\live\\post_keys.py D 2.0          hold D for 2 s
  py tools\\live\\post_keys.py Return 0.15    tap Enter
  py tools\\live\\post_keys.py Shift+Right    tap the d-pad right (the default binds)
  py tools\\live\\post_keys.py --list         the game windows found
  py tools\\live\\post_keys.py mouse 375 409 [click]   move the pointer to (x, y) of the game's 1280 x 720 screen
                                              (the picture filling the window), then a left click
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x0200, 0x0201, 0x0202, 0x0001
MAPVK_VK_TO_VSC = 0

NAMED = {"Return": 0x0D, "Enter": 0x0D, "Space": 0x20, "Tab": 0x09, "Backspace": 0x08, "Escape": 0x1B,
         "Left": 0x25, "Up": 0x26, "Right": 0x27, "Down": 0x28, "Shift": 0x10}
EXTENDED = {0x25, 0x26, 0x27, 0x28}


def vk_of(name):
    if name in NAMED:
        return NAMED[name]
    if len(name) == 1 and name.isalnum():
        return ord(name.upper())
    if name.startswith("0x"):
        return int(name, 16)
    sys.exit(f"unknown key {name!r}")


def game_windows():
    found = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if h:
            buf = ctypes.create_unicode_buffer(1024)
            n = wt.DWORD(1024)
            if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
                if buf.value.lower().endswith("\\wwe2k14.exe") and user32.IsWindowVisible(hwnd):
                    title = ctypes.create_unicode_buffer(256)
                    user32.GetWindowTextW(hwnd, title, 256)
                    found.append((hwnd, pid.value, title.value))
            kernel32.CloseHandle(h)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


def lparam(vk, up):
    sc = user32.MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)
    lp = 1 | (sc << 16)
    if vk in EXTENDED:
        lp |= 1 << 24
    if up:
        lp |= (1 << 30) | (1 << 31)
    return lp


def main():
    wins = game_windows()
    if "--list" in sys.argv:
        for w in wins:
            print(hex(w[0]), w[1], w[2], "foreground" if user32.GetForegroundWindow() == w[0] else "")
        return
    if len(wins) != 1:
        sys.exit(f"expected one game window, found {len(wins)}")
    hwnd = wins[0][0]
    if sys.argv[1] == "mouse":
        rect = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        x = round(float(sys.argv[2]) * rect.right / 1280)
        y = round(float(sys.argv[3]) * rect.bottom / 720)
        pos = (y << 16) | (x & 0xFFFF)
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, pos)
        if "click" in sys.argv[4:]:
            time.sleep(0.15)
            user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos)
            time.sleep(0.1)
            user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, pos)
        print(f"mouse to ({x}, {y}) of {rect.right} x {rect.bottom}{' + click' if 'click' in sys.argv[4:] else ''}")
        return
    vks = [vk_of(k) for k in sys.argv[1].split("+")]  # Shift+Right: down in order, up in reverse
    hold = float(sys.argv[2]) if len(sys.argv) > 2 else 0.15
    focused = user32.GetForegroundWindow() == hwnd
    for vk in vks:
        user32.PostMessageW(hwnd, WM_KEYDOWN, vk, lparam(vk, False))
        time.sleep(0.02)
    end = time.time() + hold
    while time.time() < end:
        time.sleep(0.05)
    for vk in reversed(vks):
        user32.PostMessageW(hwnd, WM_KEYUP, vk, lparam(vk, True))
    print(f"held {sys.argv[1]} ({' '.join(f'0x{v:02X}' for v in vks)}) {hold:.2f} s on window {hex(hwnd)}; "
          f"game window focused: {focused}")


if __name__ == "__main__":
    main()
