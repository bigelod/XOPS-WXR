# WinlatorXR port — standing instructions

This folder is the **general WinlatorXR porting guidance**, shared across the
owner's ports and not specific to this game. It is copied into each project as
`Docs/general_wxr/`.

Read [README.md](README.md) for the map. Do not re-derive what these files
already settle — and if you find something here that is wrong, say so and fix
the file rather than working around it.

## Before planning anything

- Fill in the intake interview at the top of `PROJECT_PLAN.md` **with the
  owner**. Those questions cannot be answered from the code, and a wrong
  assumption there propagates through the whole port.
- Copy `PROJECT_PLAN.md` out to the project's own `Docs/PROJECT_PLAN.md` to
  fill in. Leave the template in this folder blank.
- Check whether the port is even needed: a game with native head-tracking
  support (`opentrackWXR`, Path D) or an existing PCVR build (Path E) may need
  little or no new code. See `WINLATORXR_DESC.md`.

## Standing choices — do not relitigate these without asking

- **Target XrApi 0.4 or 0.5.** They are human-readable, the UDP Tester
  simulates them, and poses arrive without declaring spaces. 0.6 and 0.7 are
  documented in `XRAPI_UPGRADE.md` so you know they exist — **not as a
  roadmap**. Being on an "old" version is not a problem to fix.
- **Mod the game directly.** OXRWXR + OpenComposite is for general PCVR play
  and costs heavily in performance; it is not a shortcut to a better port.
- **Keep the game's own renderer, and keep it 32-bit.** No DX11 rewrite, no
  64-bit migration, no dgVoodoo or other wrapper layers. There is no OpenXR
  runtime to satisfy, which is the usual reason for both.
- **Measure performance on-device**, with Android and WinlatorXR tooling. Do
  not build frame timing into the mod — it cannot see the compositor, the
  translation layers or the GPU, which is where the time goes.
- **A higher version number, a newer API and a modern renderer are not goals.**
  Each is a cost that needs a named need to justify it.

## Non-negotiables in the code

- **The sync pixel is not optional.** Echo `HMD_SYNC` exactly as received, red
  channel, green 0, 10x10 at the top-left, drawn last, in both halves under
  SBS. Never rescale or gamma-correct it. Get this right before the camera.
- **The window must be borderless and fill the display**, re-applied every
  frame. Chrome in the corner means the sync pixel is never read.
- **Normalise every quaternion on ingest**, unconditionally.
- **Swap handedness once, at ingest.** Then never test the handedness flag
  again for anything spatial — a second test applies the swap twice.
- **Convert each pose independently of the head.** Share only the recentre
  origin.
- **Freeze on the last good pose** when packets drop. Never zero.
- **Measure offsets in the frame you will use them in.** If a tuned value only
  works where it was found, the frame is wrong — stop tuning and fix the frame.

## How to work

- **Instrument before theorising.** On a standalone device there is no
  debugger and each iteration costs a deploy. Add the log line; read the
  number. Do not reason about a screenshot when a log would answer it.
- Keep diagnostics behind **one verbose flag, off by default** — including
  file dumps.
- **Embed `__DATE__ __TIME__`**, log it on line 1, and make the deploy script
  refuse a binary older than the newest source file.
- **One change per deploy.** The owner verifies on hardware; nothing is "done"
  until they have.
- Expose anything a tester might change **in an in-game menu**, not an INI —
  editing files on a headset is painful. Remember that a saved config on the
  test device overrides a changed default, and say so when handing over a
  build.
- When the owner reports a problem, ask for **logs and a screenshot with the
  controller overlays visible** before guessing.

## Licensing

If the game's source licence forbids redistribution, the repository must
contain **none of it** — including diffs and context lines, since a unified
diff of a licensed file contains that file. Record edits as prose notes (file,
function, line, what to add) and verify with a `leakcheck`-style script run
against a **pristine** source tree, not the working one.

## Keeping this folder useful

Per-game findings go in the project's own docs. If something turns out to be
**general** — a protocol fact, a trap any port would hit — fold it back into
these files so the next port inherits it, and note where it came from.
