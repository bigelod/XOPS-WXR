# Test tools

These are desktop test helpers, not part of the shipped mod.

## Start a level from the title

```
tools\start-level.ps1
```

It finds the running `xops096` window, brings it forward, and replays
`title-clicks-640x480.jsonl`:

| Click | Time | Position |
|---|---|---|
| 1 | 0.0 s | (330, 242) |
| 2 | 0.56 s | (330, 242) |
| 3 | 3.0 s | (330, 242) |

The first two clicks were captured from the owner on 5 Nov 2026. The owner
reported that the mission briefing needed one more click; the third was added
at the same coordinate and the full three-click sequence was **verified** to
reach `building.ext`. If a future screen moves the briefing prompt, re-record
with `record_clicks.py` rather than guessing.

**Coordinate space:** the recorded clicks are in the game's native **640x480**
frame. `start-level.ps1` reads the game's client rect and computes
`scaleX = clientW/640`, `scaleY = clientH/480` automatically. That matters
when the 640x480 software frame is stretched to a 1920x1080 client: X is 3.0
and Y is 2.25, not a single uniform factor.

If the game is fullscreen 640x480, both factors are 1.0. If the image is not
stretched to the client (the `Present` dest override is off), use
`replay_clicks.py --scale-x 1 --scale-y 1` manually. `--offset-x/--offset-y`
handles a windowed image that does not start at `(0,0)`.

## Record a new sequence

```
python tools\record_clicks.py --pid <xops096 pid> --seconds 180
```

It records left-click down/up with timings, but only while the **XOPS process**
is foreground. Filtering by process rather than window handle matters: XOPS
changes its window handle during the title-to-level transition, and an
`--hwnd` filter silently records zero events there. `--hwnd` is still accepted;
the pid is derived from it.

Stop early by creating `mouse-stop.txt`, or let the timer expire.

## Replay an arbitrary recording

```
python tools\replay_clicks.py --hwnd 0x1234 --file my-clicks.jsonl --scale 1.0
```

`start-level.ps1` is just a convenience wrapper around this.
