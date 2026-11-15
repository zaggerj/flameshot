"""Sweep the mouse pointer across the notification area.

Windows only validates a tray icon when the pointer enters it: an icon whose
owner process is gone stays on screen as a "ghost" until it is hovered once.
Force-killed daemons (and the old installer, which called TerminateProcess on
every upgrade) leave one ghost each, so the tray ends up with half a dozen
identical icons while only one process is alive.

Run this after stopping the daemon properly; it does not touch any process.
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

u = ctypes.windll.user32
u.SetProcessDPIAware()


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def move(x, y):
    sw, sh = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    i = INPUT()
    i.type = 0
    i.mi.dx = int(x * 65535 / (sw - 1))
    i.mi.dy = int(y * 65535 / (sh - 1))
    i.mi.dwFlags = 0x8000 | 0x0001
    assert ctypes.sizeof(INPUT) == 40
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def main():
    span = int(sys.argv[1]) if len(sys.argv) > 1 else 700
    sw, sh = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    print("screen %dx%d" % (sw, sh))
    y = sh - 22
    x = sw - span
    # Two passes: the first hover makes Explorer drop the dead entries, the
    # second one covers icons that shifted left after the cleanup.
    for _ in range(2):
        x = sw - span
        while x < sw - 2:
            move(x, y)
            time.sleep(0.012)
            x += 2
        time.sleep(0.4)
    move(sw // 2, sh // 2)
    print("swept %d..%d at y=%d" % (sw - span, sw, y))
    return 0


if __name__ == "__main__":
    sys.exit(main())
