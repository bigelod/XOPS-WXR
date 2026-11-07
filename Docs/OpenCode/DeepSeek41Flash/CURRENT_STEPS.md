# XOPS-WXR — CURRENT STEPS / handover

**Date:** 7 Nov 2026, DeepSeek V4.1 Flash session.
**Read with:** `COMPLETED_STEPS.md` (what is done), `NEXT_STEPS.md` (what is
left), `WORKLOG.md` (history), `README.md` (long-form architecture).
This file is the current state and how the pieces fit.

**Current build:** `built Nov  7 2026 02:17:23`, SHA-256
`b8c8e5252414c3d6163f10743a3f8f0ece489f3325e74941dd0cb37152184143`,
deployed to `E:\tmp\Q3Mods\xops096` (`match=True`).

Release build: 287,744 bytes, SHA-256
`00b543723f99c037c0f4882e86f476916277d7f80bb9f8f299cae33fc8588dbd`.
Latest code commit: `fe8d288` (not pushed).

## Where we stopped (7 Nov, night)

**Working on device:** AER in gameplay and the AER/menu transition (the old
`MODE_VR 1 -> 0` issue is gone); VR presentation, world-anchored hands and the
always-on laser behaved correctly in the 23:02 build. The 00:39 direct-input
build was desktop-smoke-tested but not device-tested. The 02:17 build adds the
latest fixes below.

**New in the 02:17 build (not yet device-tested):**

- **ESC is never blocked, and the XrApi left menu runs the game's own ESC
  consumer.** When the game window is unfocused, the input mute now clears
  everything except `ESC` and `0`. Emitting ESC through Wine did not work on
  device, so the mod hooks the game's native ESC-consumer call at IDA
  `0x413BA2` (the game's `call 0x413CF0`) and injects a fresh ESC edge into
  `0x48C93B` / `0x48C82F` before calling the original consumer. The consumer
  then either sets the message-loop flag `0x491A9C` to 0 (title/menu: quit) or
  calls `0x413C00` (return to title), exactly as a physical ESC would.
- **Canonical controls.** VR mode rewrites the game's 20-byte key-binding block
  at `0x48C380` every frame from the default XOPS layout. The launcher's Play
  button also writes those 20 bytes into `config.dat` (preserving the video
  flags and player name) before starting the game. A custom PC config with
  several actions on one key can no longer corrupt the direct-input image.
- **Aim timing restored.** `SteerAimWrite` is called again from the
  `GetKeyboardState` hook, i.e. before the game's shot step. The direct
  player-update hook runs after the shot, so the late call was the reason the
  00:39 build's shot could lag the laser.
- **Yaw diagnostics.** `YAW A ... B ... delta ... map <id>` every ~2 s and
  `AIM base A ... B ... head ... -> A ... B ... map <id>` on the first shots.
  The map id is the game's mission index (`0x490A44`), so BUILDING_EXT and the
  failing maps can be compared directly.

**Open aim question:** the shot trace reads A (`0x404CD5`), while the character
movement reads B. The owner reports aim works on BUILDING_EXT but not on the
other maps tested; the map id in the new yaw log lets those maps be compared
directly. If A and B diverge, the likely fix is to make the character yaw B the
single authority and derive A from it (or patch the clamp), not to keep a
separate camera offset.

**Tomorrow's first pass:** copy the 02:17 `d3d8.dll` into the container, confirm
`INPUT direct player-update hook installed at IDA 0x413AC4`, test the left menu
button as ESC, movement, A/B/X/Y/grips/triggers, spawn facing and shot
alignment, and send the `STEER recentred`, `YAW ... map`, and `AIM base ... map`
lines from a failing level.

---

## 1. State in one paragraph

The `d3d8.dll` proxy runs XOPS at the container resolution with a native
640x480, 4:3 composite and no FOV change. Head tracking writes the camera;
both hands are visible, depth-tested and **world-anchored** (they subtract the
same HMD translation the world uses, so moving the head moves the view, not the
hands). The dominant hand carries the cloned game weapon and an always-on
laser. Every level start makes the current HMD XYZ and yaw the new origin, and
a right-stick click does the same. Haptics, two-handed aim, the input focus
boundary, verbose logging and the Quest 3 hand-roll default are implemented.
Gameplay takes **no physical keyboard or mouse** except ESC and the PC-only
`0`. Menus, level objectives, death, victory and intro screens keep physical
keys; the floating panel also has the hand pointer. AER stereo is implemented
as an experiment. In
both mono and AER the runtime stays `MODE_VR=1` for the whole run; `MODE_3D`
drops to mono on the title/menu/death/objective/victory screens, and those
screens are drawn as a floating panel in the centre third with a hand laser
pointer. Mono is the default. Device verification of the direct input path and
the spawn/aim base is the next milestone.

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
   In every stereo mode, the non-gameplay screens are drawn at one third size
   in the centre cell of a 3x3 grid instead (`VrMenuPanel`/`CompositeFrame`),
   with the dominant-hand laser pointer dot on top. `MODE_VR` stays 1 there;
   only `MODE_3D` drops to mono.
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
  angled thumb.
- **Hands are world-anchored.** `HandViewPose` subtracts the same clamped,
  `pos_scale`-scaled head translation the world uses, plus `eye_height`, before
  projecting into the head basis. Moving the HMD moves the view, not the
  hands; the hands stay at their level-start/recentre position. They are *not*
  counter-rotated against `SpriteRoll` - the whole game image rolls with the
  head, so the world-fixed hands and gun roll with it. Counter-rotating glued
  them to the view and made them swing on every head roll.
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
  it in `Present` (`CompDrawQuadRot`). The hand/gun/laser pass is not
  counter-rotated, so the hands roll with the presented world instead of
  staying glued to the view. The 2D HUD and sync pixel are fixed. No scaling;
  black sprite corners are accepted.
- **AER stereo:** the engine CPU-bakes the world and submits it, so eye
  separation is applied by copying each draw's vertices and shifting x
  (`AerDrawPrimitive` and `AerDrawIndexed`, both camera-space metres), one eye
  per frame. `MODE_VR` stays `1` for the whole run in mono and AER; menus,
  death, objective and victory keep `MODE_3D=0` (mono) and are drawn as the
  centre-third panel. `stereo = mono | aer`; `mono` ships.
- **Level-start recentre:** `LevelActive()` (the game's mission/result/player
  state, without the draw signature) rises when a player exists in a mission.
  `hkBeginScene` sees that edge **before** `SteerTick` writes the camera and
  calls `SteerRecentre()`. B is the player's authoritative spawn yaw; A is the
  render copy the game makes during its update and can still be one frame
  behind (device log: A=0.0 while B=90.7 deg), so `SteerRecentre` now uses B
  for both. The rendered view and the shot then share one yaw base instead of
  facing 90 deg apart. A pause-menu round trip leaves `LevelActive` true and
  does not recentre. It asks the XrApi layer to zero the pose yaw and XYZ,
  making the player's current head position and facing the new `0,0,0` and
  zero-yaw origin against the player model.
- **Render-side 6DOF / eye height:** the game clamps its camera Y every frame,
  so `SteerTick` writes only yaw/pitch and never position. `CameraOffset()`
  rotates `headP` into the camera frame and the draw copies shift the world
  opposite the eye in all three axes. HMD Y is clamped ±1 m (before the gain),
  `eye_height` adds a manual float offset (default 0, ±5) and `pos_scale`
  scales the tracked translation (default 1.0 = 1:1, 0..2). Right-stick click
  recentres. Both paths are 1:1 metres; the old non-indexed `*100` was
  removed. `AerDrawPrimitive` converts `DrawPrimitive`'s **primitive count**
  to a vertex count with `PrimVertexCount()` before copying; without that, any
  nonzero offset drew two thirds of each triangle list from stale memory and
  tore the frame apart. `AerDrawIndexed` now passes the real stride, not a
  hardcoded 28.

## 4. Input, haptics, logging

- **Focus boundary:** unfocused means physical keyboard/mouse and debug keys
  are muted; XrApi sticks/triggers/buttons are still injected.
- **Direct gameplay input:** the mod no longer sends keys in gameplay. It
  patches the call at IDA `0x413AC4` (the game's `call 0x407CF0` player
  update) to a wrapper. The wrapper rebuilds the game's own 256-byte VK image
  at `0x48C920` from XrApi (movement, jump, reload, drop, switch, zoom, fire
  mode, shoot), calls the game's own input mapper `0x407520`, and then the real
  player update `0x407CF0`. This is in-process and uses no Wine keyboard state.
  If the call site does not match, it logs and falls back to the old key path.
- **VR input isolation:** in **gameplay** only, all physical keyboard and mouse
  input is removed from `GetKeyboardState`, and the window procedure also
  swallows the corresponding `WM_KEY*`/`WM_MOUSE*` messages. Only `ESC` (the
  way out) and `0` (PC-only manual mode) survive. Menus, level objectives,
  death, victory and intro screens keep physical keys by design. Pressing `0`
  toggles manual PC mode in every build, restoring the full physical
  keyboard/mouse.
- **Left stick as arrows:** WinlatorXR currently maps the left stick to the
  arrow keys rather than the XrApi analog fields. Those raw arrow presses are
  read before the filter and translated to W/A/S/D, so the stick moves the
  player instead of swapping weapons.
- **Floating-panel pointer:** on any non-gameplay screen the dominant hand's
  ray hits a 1 m plane, is mapped onto the centre-third panel, and is published
  through `GetCursorPos` in the game's 640x480 UI space. Right trigger injects
  the left mouse button and posts `WM_LBUTTONDOWN`/`WM_LBUTTONUP`; a
  screen-space dot is drawn at the same point.
- **Laser:** always on. `Y` toggles the game's zoom and no longer hides the
  laser.
- **Haptics:** outbound fields 1-2 are a per-hand duration in frames; a shot
  sends a 2-frame pulse, decayed before sending. Both hands while two-handed
  aim is held. The 7278 packet is sent every frame (0 haptics when idle) so
  `MODE_VR`/`MODE_3D` transitions are always communicated, plus a first frame
  at startup.
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
| Y | Zoom (toggle; the laser stays on) |

Left-handed mode swaps only shoot and fire mode. Walk is gone.

On the title/menu/death/objective/victory screens in AER, the dominant-hand
laser is the mouse pointer and the right trigger is left click. The current
WinlatorXR left-stick-to-arrow mapping is also translated to W/A/S/D; mapping
the stick to the analog XrApi left stick remains the cleaner long-term fix.

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
pos_scale = 1.0             # gain on tracked HMD translation, 0..2 (1.0 = 1:1)
hud_scale = 1.1             # HUD visual scale, 0.5..2.0 (1.0 = native)
```

`xops_wxr_steer.ini` still carries the settled steering facts (`yawSign`,
`pitchSign`, `cluster`, `leftHanded`, `turnSpeed`, `hudVisualScale`,
`patchAspect`).

## 8. Build / test

```
call "<VS2022>\VC\Auxiliary\Build\vcvarsall.bat" x86
build\build-dev.bat       REM builds + deploys the dev DLL to E:\tmp\Q3Mods\xops096
build\build-release.bat   REM release DLL + launcher into Release\
```

The scripts refuse a non-x86 compiler and verify the deployed DLL with SHA-256.
Line 1 of `xops_wxr_probe.log` is the build stamp - check it before
interpreting any report.

## 9. Standing rules

- Do not change the game's FOV/projection; native 4:3.
- `MODE_VR=1` for the whole run in mono and AER; `MODE_3D=0` (mono) on
  title/menu/death/objective/victory, `MODE_3D=2` only in AER gameplay.
- Reuse the game's models and effects; do not invent replacements.
- Only `0` remains from the old number keys; numpads, `J/K/N/T/Q/E` are ours.
- `SetCursorPos` must never move the real cursor when the game is unfocused.
- No physical input crosses between windows; XrApi input still does.
- In gameplay, no physical key or mouse button reaches the game except `ESC`
  and `0`. Menus, level objectives, death, victory and intro screens keep
  physical keys; the floating panel's hand pointer is also available. Manual
  PC mode (`0`) restores everything.
- `verbose_debug = false` ships; diagnostics never replace errors/state.
- Normalise every quaternion on ingest; freeze on the last good pose.
- Never commit XOPS's own data files or binaries.
