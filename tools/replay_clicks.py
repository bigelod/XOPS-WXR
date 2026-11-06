"""Replay recorded left-click events into the XOPS window.

The recorded coordinates are in the game's native **640x480** frame. The
current working display path is fullscreen 640x480 at (0,0), so scale=1.0 is
correct. A larger back buffer still renders the game in a 640x480 area at the
top-left, so keep scale=1.0 there as well; use --scale only if the image is
actually stretched, and --offset-x/--offset-y for a windowed mode.

Usage:
  python tools/replay_clicks.py --hwnd 0x1234 \
      --file tools/title-clicks-640x480.jsonl

The caller (tools/start-level.ps1) finds the window and brings it forward.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.GetForegroundWindow.restype = wt.HWND
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
user32.mouse_event.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
                               ctypes.c_uint, ctypes.c_void_p]

HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hwnd", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--file", default=r"tools\title-clicks-640x480.jsonl")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--start-delay", type=float, default=0.5)
    ap.add_argument("--scale", type=float, default=1.0,
                    help="recorded coords are 640x480; multiply only if the image "
                         "is actually stretched to a larger frame")
    ap.add_argument("--scale-x", type=float, default=0.0,
                    help="anisotropic X scale (overrides --scale)")
    ap.add_argument("--scale-y", type=float, default=0.0,
                    help="anisotropic Y scale (overrides --scale)")
    ap.add_argument("--offset-x", type=float, default=0.0)
    ap.add_argument("--offset-y", type=float, default=0.0)
    args = ap.parse_args()

    events = []
    with open(args.file, "r", encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    if not events:
        print("no events")
        return

    hwnd = args.hwnd
    user32.ShowWindow(hwnd, 5)                 # SW_SHOW
    user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE)
    time.sleep(0.2)
    user32.SetForegroundWindow(hwnd)
    time.sleep(args.start_delay)

    # Normalise the first event to t=0: the recording's initial wait before the
    # first click is not part of the sequence.
    t0 = events[0]["t"]
    start = time.monotonic()
    for ev in events:
        target = (ev["t"] - t0) / args.speed
        delay = target - (time.monotonic() - start)
        if delay > 0:
            time.sleep(delay)
        sx = args.scale_x if args.scale_x else args.scale
        sy = args.scale_y if args.scale_y else args.scale
        px = int(round(ev["x"] * sx + args.offset_x))
        py = int(round(ev["y"] * sy + args.offset_y))
        user32.SetCursorPos(px, py)
        if ev["event"] == "down":
            user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, None)
        else:
            user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, None)
        print(f"{target:6.3f}s  {ev['event']:4s}  {px},{py}", flush=True)

    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)
    print("replay finished")


if __name__ == "__main__":
    main()
