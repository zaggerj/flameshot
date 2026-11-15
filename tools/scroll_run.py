"""Drive a real scrolling capture end to end and report the stitching verdict.

Opens the sticky test page in an Edge app window, runs `flameshot.exe scroll`
with its stderr captured directly (the flameshot-cli wrapper buffers the
child's output, which hid the log), drags a region, and waits for the capture
to finish. The verdict comes from the application's own stitcher log
(duplicates / gaps / VERDICT / row gaps), not from a mock.
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32
u.SetProcessDPIAware()

ROOT = r"C:/Users/Administrator/WorkBuddy/2026-11-14-11-08-20"
EXE = os.environ.get("FLAMESHOT_EXE") or os.path.join(ROOT, "build", "src", "flameshot.exe")
EDGE = next((p for p in [
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    r"C:/Program Files/Microsoft/Edge/Application/msedge.exe",
] if os.path.exists(p)), None)
EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def all_windows():
    out = []

    def cb(hwnd, lp):
        if not u.IsWindowVisible(hwnd):
            return True
        n = u.GetWindowTextLengthW(hwnd)
        if n:
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(hwnd, b, n + 1)
            r = wintypes.RECT()
            u.GetWindowRect(hwnd, ctypes.byref(r))
            out.append((hwnd, b.value, r.left, r.top, r.right, r.bottom))
        return True

    u.EnumWindows(EnumProc(cb), 0)
    return out


def find_window(part, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        for w in all_windows():
            if part in w[1]:
                return w
        time.sleep(0.4)
    return None


def client_rect(hwnd):
    r = wintypes.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    pt = wintypes.POINT(0, 0)
    u.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y, r.right, r.bottom


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.POINTER(wintypes.ULONG))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


INPUT_MOUSE = 0
MOVE_ABS = 0x8000 | 0x0001  # MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE
LEFTDOWN = 0x0002
LEFTUP = 0x0004


def send_mouse(dx, dy, flags):
    i = INPUT()
    i.type = INPUT_MOUSE
    i.mi.dx = dx
    i.mi.dy = dy
    i.mi.dwFlags = flags
    # INPUT must be 40 bytes on x64; sizeof check guards against struct drift.
    assert ctypes.sizeof(INPUT) == 40, ctypes.sizeof(INPUT)
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def drag(x1, y1, x2, y2):
    """Drag with SendInput absolute moves: SetCursorPos does not produce
    WM_MOUSEMOVE, so the overlay would see a click and cancel the selection."""
    smx, smy = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    ax = lambda x, y: send_mouse(int(x * 65535 / (smx - 1)),
                                 int(y * 65535 / (smy - 1)), MOVE_ABS)
    ax(x1, y1)
    time.sleep(0.35)
    send_mouse(0, 0, LEFTDOWN)
    time.sleep(0.15)
    steps = 24
    for i in range(1, steps + 1):
        ax(x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
        time.sleep(0.03)
    time.sleep(0.35)
    send_mouse(0, 0, LEFTUP)


def wait_and_report(p, log, log_path, t0):
    """Confirm the region picker with Enter, wait for the capture, report."""
    # The region-confirmation overlay (ScrollRegionPicker) needs an explicit
    # Enter to start the capture; it owns the foreground after the drag.
    time.sleep(1.2)
    KEYEVENTF_KEYUP = 0x0002

    class KBINPUT(ctypes.Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                    ("dwExtraInfo", ctypes.c_void_p),
                    # pad to MOUSEINPUT size (32): SendInput rejects a cbSize
                    # that does not match the 40-byte x64 INPUT and fails
                    # silently.
                    ("_pad", ctypes.c_ubyte * 8)]

    class INPUT_K(ctypes.Structure):
        class _U(ctypes.Union):
            _fields_ = [("ki", KBINPUT)]
        _anonymous_ = ("u",)
        _fields_ = [("type", wintypes.DWORD), ("u", _U)]
    assert ctypes.sizeof(INPUT_K) == 40, ctypes.sizeof(INPUT_K)
    down_k = INPUT_K(); down_k.type = 1; down_k.ki.wVk = 0x0D  # VK_RETURN
    up_k = INPUT_K(); up_k.type = 1
    up_k.ki.wVk = 0x0D; up_k.ki.dwFlags = KEYEVENTF_KEYUP
    u.SendInput(1, ctypes.byref(down_k), ctypes.sizeof(INPUT_K))
    time.sleep(0.05)
    u.SendInput(1, ctypes.byref(up_k), ctypes.sizeof(INPUT_K))
    print("confirmed at %.1fs" % (time.time() - t0))

    try:
        rc = p.wait(timeout=420)
    except subprocess.TimeoutExpired:
        print("TIMEOUT - killing")
        p.kill()
        rc = None
    log.close()
    print("exit", rc, "in %.1fs" % (time.time() - t0))
    text = open(log_path, encoding="utf-8", errors="replace").read()
    for line in text.splitlines():
        if any(k in line for k in ("VERDICT", "duplicates", "gaps", "rows",
                                   "row gap", "Scrolling capture", "stitched",
                                   "error", "saved", "degraded", "blank")):
            print("  ", line)
    return 0 if rc == 0 else 1


def main():
    page = sys.argv[1] if len(sys.argv) > 1 else "sticky.html"
    tag = sys.argv[2] if len(sys.argv) > 2 else "run"

    if page.startswith("@"):
        # Target an existing window by title substring instead of opening a
        # controlled Edge page — used to reproduce failures on real apps.
        target_title = page[1:]
        win = find_window(target_title, timeout=15)
        if not win:
            print("target window %r not found" % target_title)
            return 2
        hwnd = win[0]
        u.SetForegroundWindow(hwnd)
        time.sleep(1.0)
        cl, ct, cw, ch = client_rect(hwnd)
        print("client", cl, ct, cw, ch)
        x1, y1 = cl + 40, ct + 60
        x2, y2 = cl + cw - 40, ct + ch - 60
        print("selection", (x1, y1), (x2, y2))

        log_path = os.path.join(ROOT, "tools", "scroll-%s.log" % tag)
        log = open(log_path, "w")
        env = os.environ.copy()
        env["PATH"] = (r"D:/install/Scoop/apps/msys2/current/mingw64/bin;"
                       + env.get("PATH", ""))
        p = subprocess.Popen([EXE, "scroll"], cwd=os.path.dirname(EXE),
                             env=env, stdout=log, stderr=log)
        print("scroll pid", p.pid)
        t0 = time.time()
        sel = None
        while time.time() - t0 < 25:
            cand = [w for w in all_windows()
                    if w[1] == "flameshot" and w[4] - w[2] > 1200]
            if cand:
                sel = cand[0]
                break
            time.sleep(0.2)
        if not sel:
            print("no selector")
            p.kill()
            return 3
        print("selector", sel[1:])
        time.sleep(0.8)
        drag(x1, y1, x2, y2)
        print("dragged at %.1fs" % (time.time() - t0))
        return wait_and_report(p, log, log_path, t0)

    url = "file:///" + os.path.abspath(os.path.join(ROOT, "tools", page)).replace("\\", "/")

    for w in all_windows():
        if "SCROLL-TEST" in w[1]:
            u.PostMessageW(w[0], 0x0010, 0, 0)
    time.sleep(0.8)

    proc = subprocess.Popen(
        [EDGE, "--app=" + url, "--window-size=1100,760",
         "--user-data-dir=" + os.path.join(ROOT, "tools", "edgeprofile"),
         "--no-first-run", "--disable-features=msEdgeSidebarV2"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    win = find_window("SCROLL-TEST")
    if not win:
        print("edge window not found")
        return 2
    hwnd = win[0]
    u.SetWindowPos(hwnd, -1, 40, 40, 0, 0, 0x0001 | 0x0040)  # topmost, move
    u.SetForegroundWindow(hwnd)
    time.sleep(2.5)

    cl, ct, cw, ch = client_rect(hwnd)
    print("client", cl, ct, cw, ch)
    x1, y1 = cl + 4, ct + 48
    x2, y2 = cl + cw - 8, ct + ch - 6
    # FLAMESHOT_SCROLL_TEST_MARGIN=120 deliberately drags the selection well
    # outside the window (over the desktop wallpaper). The capture must clip it
    # back to the window content; this is the regression for the 2026-11-15
    # "long image with the whole desktop stitched behind the window" report.
    margin = int(os.environ.get("FLAMESHOT_SCROLL_TEST_MARGIN", "0"))
    if margin:
        x1 -= margin
        y1 -= margin
        x2 += margin
        y2 += margin
        print("selection expanded by", margin)
    print("selection", (x1, y1), (x2, y2))

    log_path = os.path.join(ROOT, "tools", "scroll-%s.log" % tag)
    log = open(log_path, "w")
    env = os.environ.copy()
    env["PATH"] = r"D:/install/Scoop/apps/msys2/current/mingw64/bin;" + env.get("PATH", "")
    p = subprocess.Popen([EXE, "scroll"], cwd=os.path.dirname(EXE), env=env,
                         stdout=log, stderr=log)
    print("scroll pid", p.pid)

    t0 = time.time()
    sel = None
    while time.time() - t0 < 25:
        # The overlay window title is "flameshot" — match it explicitly, a
        # size heuristic also matches the (topmost) Edge window.
        cand = [w for w in all_windows()
                if w[1] == "flameshot" and w[4] - w[2] > 1200]
        if cand:
            sel = cand[0]
            break
        time.sleep(0.2)
    if not sel:
        print("no selector; windows", all_windows()[:6])
        p.kill()
        return 3
    print("selector", sel[1:])
    time.sleep(0.8)
    drag(x1, y1, x2, y2)
    print("dragged at %.1fs" % (time.time() - t0))
    wait_and_report(p, log, log_path, t0)
    if "--keep" not in sys.argv:
        proc.kill()


if __name__ == "__main__":
    sys.exit(main())
