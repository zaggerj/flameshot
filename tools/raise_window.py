"""Raise a window by title substring, without Add-Type or COM.

`scroll_run.py @title` drags inside the target window's client rect, so a window
that is *behind* another app receives nothing and the capture silently targets
whatever happens to be on top. This helper restores and lifts the window first.
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

u = ctypes.windll.user32
u.SetProcessDPIAware()

SW_RESTORE = 9
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_SHOWWINDOW = 0x0040


def find(needle):
    hits = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not u.IsWindowVisible(hwnd):
            return True
        n = u.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, buf, n + 1)
        if needle.lower() in buf.value.lower():
            hits.append((hwnd, buf.value))
        return True

    u.EnumWindows(cb, 0)
    return hits


def main():
    needle = sys.argv[1]
    hits = find(needle)
    if not hits:
        print("no window matching %r" % needle)
        return 2
    hwnd, title = hits[0]
    u.ShowWindow(hwnd, SW_RESTORE)
    u.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)
    u.SetForegroundWindow(hwnd)
    u.BringWindowToTop(hwnd)
    time.sleep(0.4)
    u.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)
    time.sleep(0.2)
    rect = wt.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(rect))
    print("raised %r hwnd=%s rect=(%d,%d %dx%d)" %
          (title, hwnd, rect.left, rect.top,
           rect.right - rect.left, rect.bottom - rect.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
