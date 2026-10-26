"""Session 55 part 3: type a string into the game window, to check --wwe2k14_keyboard_typing's key path without
anyone at the keyboard.

    py tools\\s55\\post_keys.py PID TEXT [--gap-ms 60] [--focus]   (PID = the game's process id; TEXT: {BS} {ENTER}
                                                                  {ESC} {LEFT} {RIGHT} for those keys)
    py tools\\s55\\post_keys.py PID "" --focus --hold BS:1500      (Session 67: hold a key that long, sent again
                                                                  every --repeat-ms like Windows' key repeat)

Without --focus the keys are posted as window messages; SDL drops them while the window has no keyboard focus (it
routes key events to the focused window only), so that only shows nothing leaks. --focus brings the game forward for
the typing, sends real key events (SendInput, scan codes: the keyboard layout applies as for a person typing) and
puts the previous foreground window back (a second or two).
"""
import argparse
import ctypes
import re
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_SHIFT = 0x10
SPECIAL = {"BS": 0x08, "ENTER": 0x0D, "ESC": 0x1B, "LEFT": 0x25, "RIGHT": 0x27}


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


def post(hwnd, vk, down):
    sc = user32.MapVirtualKeyW(vk, 0)
    lparam = 1 | (sc << 16) | ((0xC0000000) if not down else 0)
    user32.PostMessageW(hwnd, WM_KEYDOWN if down else WM_KEYUP, vk, lparam)


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def send(vk, down):
    sc = user32.MapVirtualKeyW(vk, 0)
    extended = vk in (0x25, 0x26, 0x27, 0x28)  # arrows
    flags = 0x0008 | (0 if down else 0x0002) | (0x0001 if extended else 0)  # SCANCODE, KEYUP, EXTENDEDKEY
    inp = INPUT(type=1)
    inp.ki = KEYBDINPUT(0, sc, flags, 0, 0)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("text")
    ap.add_argument("--gap-ms", type=int, default=60)
    ap.add_argument("--focus", action="store_true")
    ap.add_argument("--hold", default="", help="KEY:MS (BS, ENTER, ESC, LEFT, RIGHT): hold that key, after TEXT")
    ap.add_argument("--repeat-ms", type=int, default=33, help="the held key's repeat interval (Windows' fastest ~33)")
    a = ap.parse_args()
    hwnd = find_window(a.pid)
    if not hwnd:
        raise SystemExit(f"no visible window for pid {a.pid}")
    key = post
    previous = None
    if a.focus:
        previous = user32.GetForegroundWindow()
        user32.keybd_event(0x12, 0, 0, 0)  # (Alt down / up: lets this process change the foreground window)
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.3)
        if user32.GetForegroundWindow() != hwnd:
            raise SystemExit("could not bring the game window forward; nothing typed")
        key = lambda _h, vk, down: send(vk, down)
    for token in re.findall(r"\{[A-Z]+\}|.", a.text):
        if token.startswith("{"):
            vk, shift = SPECIAL[token[1:-1]], False
        else:
            scan = user32.VkKeyScanW(ord(token))
            if scan == -1:
                print(f"skipped {token!r}: no key on this layout")
                continue
            vk, shift = scan & 0xFF, bool(scan & 0x100)
        if shift:
            key(hwnd, VK_SHIFT, True)
        key(hwnd, vk, True)
        time.sleep(a.gap_ms / 2000)
        key(hwnd, vk, False)
        if shift:
            key(hwnd, VK_SHIFT, False)
        time.sleep(a.gap_ms / 2000)
    if a.hold:
        name, ms = a.hold.split(":")
        vk = SPECIAL[name]
        end = time.perf_counter() + int(ms) / 1000
        downs = 0
        while True:  # a key down again without its up = a repeat (SDL's key.repeat)
            key(hwnd, vk, True)
            downs += 1
            if time.perf_counter() >= end:
                break
            time.sleep(a.repeat_ms / 1000)
        key(hwnd, vk, False)
        print(f"held {name} {ms} ms: {downs} key downs (1 press + {downs - 1} repeats), released "
              f"{time.strftime('%H:%M:%S')}")
    if previous:
        time.sleep(0.2)
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(previous)
    print(f"{'typed' if a.focus else 'posted'} {a.text!r} to window {hwnd:#x}")


if __name__ == "__main__":
    main()
