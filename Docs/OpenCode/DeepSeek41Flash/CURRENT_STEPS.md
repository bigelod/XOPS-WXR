# XOPS-WXR â€” CURRENT STEPS / handover

**Date:** 6 Nov 2026, DeepSeek V4.1 Flash session.
**Read with:** `COMPLETED_STEPS.md` (what is done), `NEXT_STEPS.md` (what is
left), `WORKLOG.md` (history), `README.md` (long-form architecture).
This file is the current state and how the pieces fit.

**Current build:** `built Nov  6 2026 11:09:06`, SHA-256
`d6852fc74c1aba51ba064a7bedc2cbfc2893a5b5246ebd1b1b604e5e135c561a`,
deployed to `E:\tmp\Q3Mods\xops096` (`match=True`).

---

## 1. State in one paragraph

The `d3d8.dll` proxy runs XOPS at the container resolution with a native
640x480, 4:3 composite and no FOV change. Head tracking writes the camera;
both hands are visible and depth-tested; the dominant hand carries the cloned
game weapon and a laser; the replacement HUD is in the centre square.
Haptics, two-handed aim, the input focus boundary, verbose logging, the
Quest 3 hand-roll default and the chunkier calibration hand are implemented
and PC-verified. The HMD's 6DOF translation (including height) is applied to
the world draws at render time, so it is visual only; a right-stick click
makes the current HMD height the new zero. AER stereo is implemented as an
experiment; mono is the default. Device testing is the next real milestone.

---

## 2. How the frame is assembled

1. `CreateDevice`/`Reset` ask for the desktop/container back buffer.
2. The game draws into a dedicated **640x480 render target** (not the back
   buffer), which keeps its native FOV. The game's own spinning 3D
   weapon-slot HUD is hidden at draw time, but its capture/tail records still
   run so the VR hand's cloned weapon model loads normally.
   The target stays bound outside `Present`, so title/demo draws between
   scenes also land in it.
3. `Present` binds the real back buffer, draws the 640x480 texture at native
   4:3 (rotated as a 2D sprite by the head roll), then the GDI-rendered
   HUD/panel textures and the sync square. The HUD and sync pixel are drawn
   after the sprite, so they stay fixed.
4. The sync square is 10x10 at `(0,0)`: red echoes `HMD_SYNC`; blue is the AER
   eye flag (0 outside AER).

`hkSetViewport` maps larger viewports back into the 640x480 target and
`hkSetRenderTarget` keeps the game there. On a 4:3 container (1280x960) the
native frame fills it with no bars or crop.

## 3. Camera, hands and the weapon clone

- The camera lives in the game's own globals; `SteerTick` composes head pose,
  recentre and right-stick yaw. Cluster A is the render copy, cluster B is
  what movement reads; both are written.
- Hand pose is converted from XrApi into view space by `HandViewPose`, using a
  **no-roll head basis** (yaw/pitch only), then drawn as boxes in the game's
  3D pass. The hand is a palm plate with four chunky finger slabs and an
  angled thumb. Because the basis has no head roll, the hands/gun behave like
  part of the VR camera.
- **Weapon capture:** the inventory weapon arrives as a group of indexed
  draws sharing one vertex buffer. The primary weapon is the last contiguous
  VB group; each draw is one submesh. Parts are copied (verts + indices +
  texture + axis mapping + scale) and **frozen**; the original HUD copy is
  left untouched. Swaps/drops/pickups trigger a recapture through the
  player-struct watcher.
- **Textures:** weapon UVs can be negative atlas offsets, so stage 0 uses
  `D3DTADDRESS_WRAP`.
- **Two-handed aim:** while the left grip is held, the dominant hand's basis
  is rebuilt to point at the off hand (`HandTwoHandedAim`); the gun, laser and
  hand-aim shot all use it. Haptics buzz both hands.
- **Roll:** `HeadRoll()` gives the head's roll; the game sprite is rotated by
  it in `Present` (`CompDrawQuadRot`). The hand/gun/laser pass is built on the
  no-roll head basis and also gets the opposite view rotation, so it stays
  fixed relative to the VR camera while the world rolls. The 2D HUD and sync
  pixel are fixed. No scaling; black sprite corners are accepted.
- **AER stereo:** the engine CPU-bakes the world and submits it, so eye
  separation is applied by copying each draw's vertices and shifting x
  (`AerDrawPrimitive` and `AerDrawIndexed`, both camera-space metres), one eye
  per frame. `stereo = mono | aer`; `mono` ships.
- **Render-side 6DOF / eye height:** the game clamps its camera Y every frame,
  so `SteerTick` writes only yaw/pitch and never position. `CameraOffset()`
  rotates `headP` into the camera frame and the draw copies shift the world
  opposite the eye in all three axes. Hands/gun/laser stay in the head frame.
  HMD Y is clamped ±1 m, `eye_height` adds a manual float offset (default 0,
  ±5) and `pos_scale` scales the tracked translation (default 0.1, 0..2).
  Right-stick click recentres, making the current elevation the new 0. Both
  paths are 1:1 metres; the old non-indexed `*100` was removed.

## 4. Input, haptics, logging

- **Focus boundary:** unfocused means physical keyboard/mouse and debug keys
  are muted; XrApi sticks/triggers/buttons are still injected. In VR mode
  physical A/B/X/Y are always swallowed and arrows are swallowed while
  `MODE_VR = 1`; manual PC mode (`0`) is exempt.
- **Haptics:** outbound fields 1-2 are a per-hand duration in frames; a shot
  sends a 2-frame pulse, decayed before sending. Both hands while two-handed
  aim is held.
- **Logging:** `Log()` is always on for errors/state; `LogD()` is gated by
  `verbose_debug` (default false, true on this test PC).

## 5. VR controller mapping (owner, 6 Nov)

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick L/R | Turn L / R |
| Right stick click | Recentre (current HMD height becomes 0) |
| Left grip (held) | Two-handed aim |
| Left trigger | Fire mode |
| Right trigger | Shoot |
| Right grip | Jump |
| B | Reload |
| A | Switch weapon |
| X | Drop weapon |
| Y | Zoom (toggle) |

Left-handed mode swaps only shoot and fire mode. Walk is gone.

## 6. Manual / debug keys

**All debug keys below are locked behind `0`.** While VR input is active only
`0` is accepted; WinlatorXR does not map `0`, so the device cannot trigger them
by accident. Pressing `0` turns manual PC mode on and unlocks the rest.

- `0` toggle manual PC mode (default pose, XrApi ignored; cursor centred once).
- `E` + numpad: pitch/yaw/roll; with `E`, numpad also moves the hand.
- `Q` + numpad: slide the hand box; `T` saves the per-weapon offset.
- `J/K` previous/next draw (hunt), `N` attach.
- `V` draw dump, `H` HUD scale, `F` window fit, `M` target res.
- `.` grab two consecutive composited frames (`xops_frame_<n>.bmp`).
- `/` toggle head-roll live (for comparing roll on/off).
- Verbose-only keys and probe keys are muted while the game is unfocused.

## 7. Config

`E:\tmp\Q3Mods\xops096\config.txt`, in the game folder next to the EXE (a
legacy file one level up is still read if the local one is missing):

```
handedness = right
hand_flip = false           # false = Quest 3 roll (default); true = old/other-device roll
haptics = true
haptics_hand = dominant     # dominant | both | left | right
haptic_frames = 2           # 1..5
verbose_debug = true        # this test PC; ship with false
stereo = mono               # experimental: aer
roll = true                 # rotate the world sprite with head tilt
roll_sign = -1              # default; +1 if the device horizon tilts the wrong way
eye_height = 0.0            # manual render-side vertical offset, metres, +/-5
pos_scale = 0.1             # gain on tracked HMD translation, 0..2 (1.0 = 1:1)
```

`xops_wxr_steer.ini` still carries the settled steering facts (`yawSign`,
`pitchSign`, `cluster`, `leftHanded`, `turnSpeed`, `hudVisualScale`,
`patchAspect`).

## 8. Build / test

```
call "<VS2022>\VC\Auxiliary\Build\vcvarsall.bat" x86
build\build-probe.bat                 REM builds + deploys to E:\tmp\Q3Mods\xops096
```

The script wipes `out\probe`, refuses a non-x86 compiler, and verifies the
deployed DLL with SHA-256. Line 1 of `xops_wxr_probe.log` is the build stamp -
check it before interpreting any report.

## 9. Standing rules

- Do not change the game's FOV/projection; native 4:3.
- `MODE_VR=0` on title/menu/death/victory; desktop input must work there.
- `MODE_VR=1` only in gameplay.
- Reuse the game's models and effects; do not invent replacements.
- Only `0` remains from the old number keys; numpads, `J/K/N/T/Q/E` are ours.
- `SetCursorPos` must never move the real cursor when the game is unfocused.
- No physical input crosses between windows; XrApi input still does.
- A/B/X/Y and (in `MODE_VR=1`) the arrows are never taken from the physical
  keyboard in VR mode; manual PC mode is the exception.
- `verbose_debug = false` ships; diagnostics never replace errors/state.
- Normalise every quaternion on ingest; freeze on the last good pose.
- Never commit XOPS's own data files or binaries.
