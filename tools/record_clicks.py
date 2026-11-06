"""Record left-click down/up events while the XOPS process is foreground.

Filter by **process**, not window handle: XOPS changes its window handle during
the title-to-level transition, and a handle filter then silently records zero
events (learned the hard way on 5 Nov 2026).

  python tools/record_clicks.py --pid 1234 --seconds 180
  python tools/record_clicks.py --hwnd 0x1234 --seconds 180   # pid derived

Stop early by creating the file named by --stop (default: mouse-stop.txt in
the current directory) or wait for --seconds.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short
user32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
user32.GetForegroundWindow.restype = wt.HWND
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]

VK_LBUTTON = 0x01


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--hwnd", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--seconds", type=float, default=180.0)
    ap.add_argument("--out", default="mouse-clicks.jsonl")
    ap.add_argument("--stop", default="mouse-stop.txt")
    args = ap.parse_args()

    if not args.pid and not args.hwnd:
        raise SystemExit("give --pid or --hwnd")

    pid = args.pid
    if not pid:
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(wt.HWND(args.hwnd), ctypes.byref(pid))
        pid = pid.value
    if not pid:
        raise SystemExit("could not resolve the process id")

    start = time.monotonic()
    prev = False
    events = []
    print(f"recording clicks for pid={pid} (foreground only), {args.seconds:.0f}s",
          flush=True)
    print(f"out: {args.out}", flush=True)

    with open(args.out, "w", encoding="ascii") as f:
        while time.monotonic() - start < args.seconds:
            if os.path.exists(args.stop):
                break
            fg = user32.GetForegroundWindow()
            fpid = wt.DWORD()
            user32.GetWindowThreadProcessId(fg, ctypes.byref(fpid))
            if fpid.value != pid:
                prev = bool(user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000)
                time.sleep(0.005)
                continue
            down = bool(user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000)
            if down != prev:
                pt = wt.POINT()
                user32.GetCursorPos(ctypes.byref(pt))
                ev = {"t": round(time.monotonic() - start, 4),
                      "x": pt.x, "y": pt.y,
                      "event": "down" if down else "up"}
                f.write(json.dumps(ev) + "\n")
                f.flush()
                events.append(ev)
                print(json.dumps(ev), flush=True)
            prev = down
            time.sleep(0.005)

    print(f"recorded {len(events)} events", flush=True)


if __name__ == "__main__":
    main()
