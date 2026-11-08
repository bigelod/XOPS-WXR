# XOPS-WXR — CURRENT STEPS / handover

**Date:** 7 Nov 2026, DeepSeek V4.1 Flash session.
**Read with:** `COMPLETED_STEPS.md` (what is done), `NEXT_STEPS.md` (what is
left), `WORKLOG.md` (history), `README.md` (long-form architecture).
This file is the current state and how the pieces fit.

**Current build:** `built Nov  8 2026 11:17:17`, SHA-256
`0a1179df0ee70ec9d69eb8307ead612f21c3e66d2a23e34aa1136c22bcbf4b67`,
deployed to `E:\tmp\Q3Mods\xops096` (`match=True`).

Release build: 289,280 bytes, SHA-256
`ca58672b68392e0ab988999463326c9a8fb0620a85a418ede1fe563e7b4abe87`.
Latest code commit: `aa75a19` (not pushed).

## Where we stopped (7 Nov, night)

**Working on device:** AER in gameplay and the AER/menu transition (the old
`MODE_VR 1 -> 0` issue is gone); VR presentation, world-anchored hands and the
always-on laser behaved correctly in the 23:02 build. The 00:39 direct-input
build was desktop-smoke-tested but not device-tested. The 11:17 build adds the
latest fixes below and was desktop-tested end to end (menus, movement, turn,
fire).

**New in the 11:17 build (not yet device-tested):**

- **Spawn grace is now 0.3 s.** The 1 s grace was reliable on PC; the level
  edge now waits 18 frames before taking the camera yaw as the origin. Same
  recentre and right-stick behaviour as the 13:26 build.
- **Per-level mouse-look probe (dev).** In manual mode (`0`), `LookProbeTick`
  logs `LOOK map=<id> A yaw,pitch B yaw,pitch delta ...` whenever any of those
  fields changes. This is the probe suggested for the remaining aim bug: move
  the mouse left/right then up/down on a bad level and the log shows which
  field the game's own look actually drives, per map.
- **Launcher refuses a second game instance.** The 7 Nov StrangeCrash logs show
  the launcher started `xops096.exe` twice 17 s apart while the first was still
  running. Two d3d8 proxies/hook sets/log writers is a plausible cause of the
  "strange crash after a few levels". Play now checks for a running
  `xops096.exe` and shows a warning instead of starting another.

- **The runtime mouse works on menus again.** The window procedure now blocks
  the mouse in **gameplay only**; menus and the floating panel let the
  WinlatorXR-supplied mouse through. On the panel, the runtime mouse and the
  hand laser are arbitrated by "most recent movement wins", and the real cursor
  is remapped from the centre-third rectangle into the game's 640x480 UI space.
  Mouse buttons also survive the unfocused-window mute on non-gameplay screens,
  so a runtime click is not eaten because the container owns focus.
  Desktop-verified: the three-click title sequence still reaches
  `building.ext`, and the log shows `MENU pointer: runtime mouse -> logical
  350,245` both with and without live XrApi poses.

- **ESC is never blocked, and the XrApi left menu runs the game's own ESC
  consumer.** When the game window is unfocused, the input mute now clears
  everything except `ESC` and `0`. Emitting ESC through Wine did not work on
  device, so the mod hooks the game's native ESC-consumer call at IDA
  `0x413BA2` (the game's `call 0x413CF0`) and injects a fresh ESC edge into
  `0x48C93B` / `0x48C82F` before calling the original consumer. The consumer
  then either sets the message-loop flag `0x491A9C` to 0 (title/menu: quit) or
  calls `0x413C00` (return to title), exactly as a physical ESC would.
- **Physical ESC works in gameplay again (11:17).** The direct-input rebuild
  called `ZeroMemory` on the game's shared VK image at `0x48C920`, wiping
  `keys[VK_ESCAPE]` just before the ESC consumer read `0x48C93B` later in the
  same frame. `DirectInputBuild` now preserves the polled ESC byte, and
  `EscConsumerHook` restores the game's saved cur/prev values instead of
  zeroing them. Desktop-verified: physical ESC logs
  `ESC consumer: physical ESC edge` and returns to the menu (`A44=61`,
  `1A6C=1`); the XrApi left menu still logs the injected edge and transitions
  the same way.
- **Spawn yaw fixed for level changes (11:17).** A level applies its spawn
  orientation a few frames after the player exists. The old recentre ran on the
  rising edge and, on a level entered after quitting another one, read the
  pre-spawn value (`~0`) and pinned the view there. `SteerRecentre` also took
  the player-body yaw **B** as the origin, which on DUEL_EXT put the camera
  180 deg into the wall and made stick-forward move backwards.
  The mod now:
  1. leaves the game's yaw untouched for 18 frames after the level edge
     (`SPAWN grace` log), so the game can apply its real spawn view;
  2. recentres from the **camera yaw A** (the view the game actually presents),
     not the animating body yaw;
  3. writes both A and B from that origin plus the HMD yaw, so the body and
     movement follow what the player sees.
  Right-stick recentre uses the same path.
  Desktop-verified on the exact failing flow (BUILDING_EXT -> sticks -> ESC ->
  DUEL_EXT): spawn shows the open arena instead of the gray wall
  (`A B = 179.8 deg` after grace), and one second of stick-forward moves toward
  the crates (screenshot MAD 14.4, crates visibly closer).
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

**Input audit (release, non-debug):**

- **Gameplay:** no synthetic keyboard state. `DirectInputBuild` runs the game's
  own mapper `0x407520` with an XrApi-built image, then the real player update
  `0x407CF0`. ESC uses the game's own consumer at `0x413BA2`/`0x413CF0`.
  Movement/buttons come from XrApi; no `SendInput`, `keybd_event`, `mouse_event`
  or `PostMessage` is used.
- **Dev fallback:** `StickKeys`/`VrInputKeys` (synthetic W/A/S/D and command
  keys) are compiled and called only under `XOPS_DEV_BUILD`, for the case where
  the direct hook fails to install. Release logs that case and does nothing.
- **Debug-only:** `ProbeKeys`, `ManualHandKeys`, the numpad hand controls and
  the one-time `SetCursorPos` centring in manual mode (`0`).
- **Remaining non-debug Windows input:** the menu pointer is now deliberate.
  It feeds the game's own `GetCursorPos`/`ScreenToClient` and `VK_LBUTTON`
  state, and it is the runtime's mouse, not one the mod invents. No window
  messages are posted. A direct title/mission-select call (`0x411F80`) remains
  an optional simplification, not a requirement.
- **Desktop test (11:49):** with a static pose feeder and after `start-level`,
  right trigger produced `HAPTIC fires=1` shots and the ammo counter fell
  26 -> 14; head yaw moved A/B together 135.7 -> 95.6 deg (0.7 rad); left stick
  moved the player (screenshot MAD 40.4); right stick turned the view
  (MAD 17.9). No A/B divergence in any log.
- **Isolation, not action control:** `GetCursorPos`/`SetCursorPos` hooks in
  gameplay only freeze the physical mouse so it cannot turn the view; they do
  not drive game actions.

**Spawn yaw resolved on desktop:** the shot trace reads A (`0x404CD5`) and the
movement reads B. The spawn bug was a reference-capture problem, not a
convention split: a level applies its spawn orientation after `LevelActive()`
goes true, and the old recentre used the body yaw B before it had settled.
The mod now waits 18 frames (0.3 s), takes A (the presented view) as the origin,
and writes both A and B from it. Device pass: repeat the failing
quit-and-reenter flow on DUEL_EXT and check the spawn view and stick-forward.

**Open aim issue (owner, 7 Nov):** the spawn view is correct, but after
returning to the title and entering DUEL_EXT the shot does not follow the hand
laser: the MARKED screenshot shows the laser on the target and the impact on
the ground below/left. BUILDING_EXT aim was correct in the same session. The
owner's StrangeCrash log has AIM lines for maps 6, 2 and 4: the yaw written to
A/B is held exactly, and the read-back pitch also matches the written value, so
the game is *not* discarding the aim write. The remaining suspects are the hand
ray/origin used by the shot versus the drawn laser, or a per-level difference
in the look fields. A dev mouse-look probe (`LOOK ...`) now logs A/B yaw and
pitch plus the input deltas in manual mode; run it per level and send the
lines. That is the same approach used to solve the original 6DOF pitch/yaw.

**Tomorrow's first pass:** copy the 11:17 `d3d8.dll` into the container, confirm
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
