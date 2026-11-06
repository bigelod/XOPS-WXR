# XOPS-WXR - NEXT STEPS

**Session:** 6 Nov 2026 (DeepSeek V4.1 Flash, second half).
**State:** see `CURRENT_STEPS.md` for what is built and how it works.
**Read with:** `README.md` (architecture) and `WORKLOG.md` (history).

This file is the live plan. It now carries the owner's 6 Nov control,
haptics, logging, stereo and launcher requests, in the order they were asked
for. Experimental items are marked so they can be postponed without touching
the V1.0 path.

---

## 0. Standing decisions from 6 Nov (do not re-litigate)

### Controls (owner, 6 Nov)

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick L/R | Turn left / right |
| **Left grip** | **Two-handed aim mode (held)** |
| Left trigger | Fire mode |
| Right trigger | Shoot |
| Right grip | Jump |
| B | Reload |
| A | Switch weapon |
| X | Drop weapon |
| Y | Zoom |

- **Walk is gone.** The left grip used to send `TAB`; it no longer does.
- **Left-handed mode swaps only shoot and fire mode.** Every other button is
  identical (owner repeated this twice).
- **Y zoom** stays a toggle for now. A head-tracked floating scope (HWXR Halo
  style) is a later, advanced idea; do not build it yet.

### Two-handed aim (owner, 6 Nov)

- No `two_handed` config key. It is **on while the left grip is held**, off
  otherwise. Players choose when to use it, so one-handed weapons looking odd
  in it is acceptable.
- The gun is **mounted at the primary hand and points at the secondary hand**.
  Keep the gun hand's roll. SHOGO MAD WXR's `UpdateTwoHanded`
  (`Shogo-VR-WXR/game/VRStereo.cpp`, around line 3653) is the reference:
  - `aimF = normalize(offP - domP)`
  - `aimU = normalize(domU - aimF * dot(aimF, domU))`, fallback `domU`
  - `aimR = cross(aimU, aimF)`
  - Position stays at the dominant hand.
- Use the same basis for the laser, so barrel, laser and the hand-aim shot
  agree.
- **Haptics:** while two-handed aim is active, a shot buzzes **both**
  controllers (Shogo does the same for its two-hand grip).

### Input gating (owner, 6 Nov)

1. **When the game window does not have focus, the game receives no input at
   all** - keyboard, mouse, sticks, VR buttons, and every debug key. The game
   may keep running (`g_noPause`), but nothing crosses between windows.
   - Zero the `GetKeyboardState` array, return 0 from
     `GetAsyncKeyState`/`GetKeyState`, keep the cursor delta at zero, and skip
     all synthetic VR/stick/debug injection.
   - Sync the edge trackers so a key/button held across the focus change is
     not seen as a new press on return.
2. **A B X Y are never accepted from the physical keyboard in VR mode**
   (any screen), because WinlatorXR sends those for the controller buttons.
3. **Arrow keys are not accepted while `MODE_VR = 1`**, for the same reason.
4. **Manual PC mode (`0`) is exempt**: it is a real desktop keyboard, so
   A/B/X/Y and the arrows must work there.

### Logging (owner, 6 Nov)

- New config key `verbose_debug = FALSE` (**default**). When FALSE the log
  keeps only errors and game-state changes.
- Informative diagnostics (snapshots, probes, draw dumps, raw XR packets,
  per-weapon capture details, matrix dumps, key scans, one-time help text)
  must be behind `verbose_debug = TRUE`.
- **Set `verbose_debug = TRUE` in the test install's config**
  (`E:\tmp\Q3Mods\config.txt`, one directory above the game) so this PC keeps
  producing full logs while testing. Device/default stays FALSE.
- Keep always-on: build stamp, fatal errors, config load summary, `RESET`,
  `MODE_VR` transitions, `COMPOSITE active`/size, HUD texture creation,
  state tags, input/handedness changes, `HAPTIC` summary only if it remains
  useful at the owner's request (currently grouped with diagnostics).

---

## 1. Haptics on ammo decrease - IMPLEMENTED, desktop-verified 6 Nov

Protocol: outbound 7278, first two fields are **duration in frames**, per hand
(`XRAPI_V0_4_DESC.md` section 4). Values `<= 0` are ignored by the runtime, so
the value is decayed locally and re-sent each frame.

Current behaviour (mirrors SHOGO MAD WXR):

- Watch both magazine counters (`+0C/+0D`); a drop of 1-8 is a shot. A larger
  drop is a reload/pickup/respawn and is ignored.
- One pulse per shot: `2` frames by default, decayed **before** sending, so the
  runtime sees `1` then `0` (Shogo's "short kick").
- Dominant hand by default; both hands when two-handed aim is held.
- Gameplay only; counters reset on menus/death so a respawn cannot buzz.

Config (`config.txt`, one directory above the game):

```
haptics = true
haptics_hand = dominant   # dominant | both | left | right
haptic_frames = 2         # 1..5
```

Desktop evidence (6 Nov, `built Nov 6 2026 03:09:30`): 10 single shots logged
`HAPTIC fires=1 dur=4 frames hand=right ... ammo 9/0 ... 1/0`; the UDP capture
showed `0.0 4.0 1 0 80.7 65.0` decaying `4,3,2,1,0` on the right hand only.
A second run with the old `two_handed = true` config showed `hand=both` and
`1.0 1.0 1 0 80.7 65.0`. The 4-frame run predates the Shogo-matched default
of 2.

**Still to verify on device:** feel, and that 1 sent frame is perceptible
through WinlatorXR. `haptic_frames = 3` gives two sent frames if it is too
light.

---

## 2. Finish calibrating weapon holds (primary / dominant hand)

**Goal:** a good, saved grip offset for every weapon.

**Tooling already in the build:**

- `Q` + numpad slides the **hand box only**; the cloned gun stays put.
  `8/5` up/down, `4/6` left/right, `7/9` back/fwd.
- `T` saves the offset for the current weapon id and logs:
  `HAND saved adj 'AUG' (id 14):  0.010 -0.020  0.030`

**Work:**

- Visit every weapon and save the offset with `T`.
- Persist `g_handAdjTable[32][3]` to `config.txt` (or a new
  `weapon_tuning.txt`) keyed by weapon id and name, and load it at start.
- Keep the existing per-weapon scale/vertical flags alongside:
  `GREN*` scale 1/3 + vertical; `MAC*` 0.75; sniper rifles may need their own.
- Weapons seen: MP5SD/MP5, MK23 SD, AUG, M1, GLOCK18 SEMI, PSG1, MAC 10,
  shotgun, grenade, plus the rest of ids 0..31 at `0x465DA0`.

---

## 3. Off-hand pose in two-handed aim

The left grip now gives the aim basis (section 0). Next:

- Draw/pose the off hand so it supports the weapon instead of hanging at its
  raw controller pose when the hands are close.
- Per-weapon off-hand offset, saved with the same key scheme as the primary.
- Keep the default one-handed behaviour untouched when the grip is not held.

---

## 4. Map each weapon's barrel end (muzzle)

**Goal:** a per-weapon muzzle point so the game's own muzzle flash / smoke can
be shown at the right place (do not draw our own effects).

- Add `muzzle_offset[3]` per weapon id, calibrated the same way as the hand
  offset.
- When the game spawns its muzzle flash / smoke, translate those effect draws
  to the dominant hand's muzzle point. Reuse the game's effects.

---

## 5. Test on device

Checklist for the Quest 3 / WinlatorXR run:

- Container at **1280x960** (native 4:3, no bars) unless the stereo experiment
  says otherwise (SBS is double width, same height; AER stays 4:3).
- `VERBOSE_DEBUG = FALSE` in the shipped config; full log available on demand.
- Input mute: alt-tab away and type - nothing reaches the game; debug keys
  included.
- A/B/X/Y and arrows never leak from the physical keyboard in VR mode; manual
  `0` mode restores them.
- `MODE_VR` 1 only in gameplay; 0 on title/objective/menu/death/victory;
  desktop keyboard/mouse works there.
- Both hands visible at their 6DOF poses, depth-tested.
- Dominant laser and hand-aim hit where the laser points.
- Left grip: gun mounts at the dominant hand and points at the off hand; both
  controllers buzz on firing.
- Weapon clone appears, follows swaps, drops to hands-only on drop/empty
  (including grenade fired empty), returns on pickup/switch.
- Per-weapon textures (AUG/M1/Glock18/MK23 after the WRAP fix).
- Grenade vertical and correctly sized; MAC 10 smaller.
- Manual mode (`0`): default pose, `E`+numpad, `Q`+numpad, `T`.
- Haptics feel proportional to fire rate.
- Latency/comfort: tracking latency, judder, reprojection.

---

## 6. EXPERIMENTAL - true stereo (SBS first, AER fallback)

**Status:** not started. Owner explicitly allows cancelling/postponing until
after V1.0 if it cannot be done tidily. Take time and test on the desktop.

**Reference:** `D:\Daven Bigelow\Documents\GitHub\HaloCEWXR`. Use its stereo
path only - **do not port its non-VR mode**.

- **SBS:** double the presented width, same height (two eye images side by
  side). Runtime must be told the stereo layout.
- **AER fallback:** top-and-bottom, same 4:3 resolution per eye.
- The XrApi outbound packet already carries `MODE_3D`; confirm the value the
  runtime expects for SBS vs AER before drawing anything.
- The mod owns camera and projection, so both eyes can be rendered from the
  same world with an IPD offset and a per-eye projection.
- The 640x480 render target, HUD, hands, laser and composite must be evaluated
  per eye; decide whether the HUD is duplicated or drawn once in screen space.
- Sync pixel stays in the top-left of the **final presented** frame.

Open questions to answer from the HaloCEWXR code first:

- Exact `MODE_3D` values and whether the runtime wants a specific eye order.
- Whether the runtime crops/letterboxes the SBS image itself.
- How HaloCEWXR builds the per-eye projection (asymmetry, convergence).
- Whether the game's own 2D UI is duplicated or suppressed per eye.

---

## 7. BONUS - custom EXE launcher

**Reference:** `D:\Daven Bigelow\Documents\GitHub\Shogo-VR-WXR\launcher-xr`
(working launcher with auto-update).

- Two actions: **Play** (launch the game with the mod) and **Options** (edit
  the VR config: handedness, haptics, haptic frames, verbose debug, turn
  speed, hand flip, and later stereo mode).
- **Skip auto-update for now.** A later update can add it the same way Shogo
  does.
- Only start this after the experimental stereo work is either accepted or
  postponed.

---

## Known state / loose ends

- Weapon capture: multi-part by shared vertex buffer; primary = last VB
  group, secondary ignored. Parts are frozen after capture.
- Textures fixed by `D3DTADDRESS_WRAP` for negative atlas UVs.
- Empty/drop handling: explicit switch to an empty slot, drop, or grenade
  empty latches hands-only until pickup/switch.
- `g_handAdjTable` and the per-weapon scale table are **in memory only**;
  persistence is item 2.
- If a texture is still black, `verbose_debug` must be TRUE to see
  `model fmt: stride=.. fvf=.. uv0 .. .. tex=..`; check for a second texture
  stage/lightmap or a multi-texture model.
- Draw-order hunt keys (`J/K/N`) and the `TAIL` dump remain available (verbose
  only) for diagnosing any new weapon.

---

## Key files

```
src/d3d8probe.cpp                    all hooks, HUD, hands, laser, weapon clone
src/xrapi.cpp/.h                     XrApi parsing, poses, input
Docs/OpenCode/DeepSeek41Flash/CURRENT_STEPS.md   current architecture/state
Docs/OpenCode/DeepSeek41Flash/WORKLOG.md         experiment history
build/build-probe.bat                build/deploy
tools/start-level.ps1                title -> briefing -> level
E:\tmp\Q3Mods\xops096\xops_wxr_probe.log         runtime log
E:\tmp\Q3Mods\config.txt                         handedness / haptics / debug
D:\Daven Bigelow\Documents\GitHub\HaloCEWXR     stereo reference
D:\Daven Bigelow\Documents\GitHub\Shogo-VR-WXR   two-hand aim + launcher
```
