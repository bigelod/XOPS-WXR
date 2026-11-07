# XOPS-WXR — NEXT STEPS

**Only work that is still open lives here.** Implemented features are in
`COMPLETED_STEPS.md`; history is in `WORKLOG.md`; architecture is in
`README.md` and `CURRENT_STEPS.md`.

**Last build:** `built Nov  7 2026 13:04:54`, SHA-256
`3a28945137d9a86ac48ba1ed4661fb1d5204a4f062691332abc8ecbc8a8303ca`.
Release: 289,280 bytes, SHA-256
`0c1dd6e03b1eca1c3e5ad694b9c99df70cc920f73e2a155ca8c258609af2ed74`.

---

## 1. Persist per-weapon hand tuning

`g_handAdjTable[32][3]` (and the per-weapon scale/vertical flags) are currently
**memory only**; `T` logs a tuned offset but it is lost on restart.

- Write them to `config.txt` or a new `weapon_tuning.txt`, keyed by weapon id
  and name; load at start.
- Keep the scale rules alongside: `GREN*` 1/3 + vertical, `MAC*` 0.75,
  possible sniper overrides.
- Tune every weapon once and keep the file with the mod, not the game.

## 2. Off-hand pose in two-handed aim

The **aim** is done (left grip, gun at the primary hand pointing at the
secondary). What is missing is the support hand:

- When the grip is held, pose the off hand so it supports the weapon instead
  of hanging at its raw controller pose.
- Per-weapon off-hand offset, saved with the same tooling as the primary hand.
- Keep one-handed behaviour untouched when the grip is not held.

## 3. Per-weapon muzzle point

- Add `muzzle_offset[3]` per weapon id, calibrated like the hand offset.
- When the game spawns its own muzzle flash / smoke / shells, translate those
  effect draws to the dominant hand's muzzle point.
- **Reuse the game's effects**; do not author new ones.

## 4. Quest 3 device pass (next)

Earlier device runs confirmed startup, injection, the DXVK export fix, AER in
gameplay, the AER/menu transition, `MODE_VR=1` menus in mono and AER,
world-anchored hands and the always-on laser. The open pass is the **7 Nov
direct-input build** and the spawn/aim base:

- Container at **1280x960** (native 4:3, no bars) unless testing stereo.
- Shipped config has `verbose_debug = false`; full log on demand.
- **Direct input:** confirm the startup log contains
  `INPUT direct player-update hook installed at IDA 0x413AC4`. In gameplay test
  movement, A/B/X/Y, left/right grips and both triggers. The game window is
  usually unfocused under WinlatorXR; that must not matter.
- **ESC / left menu:** confirm the left controller's menu button performs the
  game's own ESC action. The mod hooks the game's native ESC-consumer call at
  `0x413BA2` and calls `0x413CF0` with a fresh ESC edge; it must work on
  gameplay, menus, death and victory, even while the game window is unfocused.
  The physical ESC also stays valid. Desktop-verified 7 Nov: both paths leave
  gameplay for the menu after the direct-input image was fixed to preserve
  `keys[VK_ESCAPE]`.
- **Controls:** the launcher rewrites the canonical 20-byte key block into
  `config.dat`, and VR mode also enforces it in memory at `0x48C380`. Confirm
  after using the launcher's Play button, and try a deliberately custom
  `config.dat` to ensure it no longer breaks the mod.
- **Movement:** direct input uses the XrApi left-stick analog fields plus the
  runtime's digital `L_THUMBSTICK_*` bools, so the arrow-key mapping is no
  longer needed. Confirm the stick moves the player and does not swap weapons.
- **Spawn/aim:** capture the `STEER recentred. yaw A raw X -> Y (deg) B Z` line
  for several levels, especially the exact failing flow: BUILDING_EXT -> move
  -> ESC -> DUEL_EXT. The mod now waits 120 frames for the game's spawn yaw,
  then uses the camera yaw A as the origin. Confirm the spawn shows the open
  arena (not the gray wall) and that stick-forward moves toward what the
  camera sees. Right-stick recentre should leave the view still and re-zero
  the HMD XYZ/yaw.
- **Aim on every level:** fire on a level that failed before and send the
  `YAW ... map <id>` and `AIM base ... -> ... map <id>` lines. BUILDING_EXT is
  the known-good map; compare its map id with the failing ones. The logs show
  whether the game clamps/overwrites A or B. The shot trace reads A
  (`0x404CD5`); movement reads B, and both are now written from the same
  origin.
- **Hands:** tilt the head and confirm the hands/gun roll with the presented
  world. Crouch/stand and confirm the render follows the head while the hands
  stay at the level-start/recentre position. Right-stick click re-zeroes XYZ
  and yaw. `pos_scale` defaults to **1.0**; `eye_height` remains a manual trim.
- **Input policy:** in gameplay only `ESC` and `0` pass; menus, level
  objectives, death, victory and intro screens keep physical keys. `ESC` is
  always allowed. `0` enables full desktop keyboard/mouse in release too.
- **AER menus:** re-confirm the centre-third panel and hand pointer after the
  direct-input change; AER itself stays off on menu/death/objective/victory.
- **Runtime mouse on menus:** confirm the WinlatorXR mouse still points and
  clicks on the panel (gameplay stays XrApi-only). Desktop-verified 7 Nov:
  `MENU pointer: runtime mouse -> logical 350,245` and the three-click title
  sequence reaches `building.ext`. On device, check that moving the runtime
  mouse takes the pointer from the hand laser, and that the hand takes it back
  when moved.
- After this run, create `BUG_FIXES.md` from the device findings.

## 5. Stereo

- **AER is implemented and desktop-verified**; `stereo = aer` opts in.
- On device, confirm the **eye sign** (flip if depth looks inverted). Both
  reproject helpers are now 1:1 metres (the non-indexed `*100` centimetre
  assumption was removed with the height work), so there is no scale left to
  tune; one-line sign if needed.
- **Menus (mono and AER):** the runtime stays `MODE_VR=1` for the whole run
  and only `MODE_3D` drops to 0. Device-confirmed 7 Nov: the old
  `MODE_VR 1 -> 0` transition is gone. Re-confirm the centre-third panel and
  hand pointer after the direct-input change.
- **SBS is deferred.** It needs a double-width game target and a double-width
  container/window so each eye keeps 4:3. Plan: duplicate every world/model
  draw into two viewports with the existing eye shift, draw the HUD per half,
  and put a sync square in the top-left of the full frame and of the right
  half. Only attempt once AER is confirmed on device; it is allowed to be
  postponed until after V1.0.

## 6. Launcher - first version done, release plumbing left

`launcher-xr/` now has `XOPSVR.exe` (see `COMPLETED_STEPS.md`): Play, Options
and the update check work on the PC. What is left for a real release:

- Replace the temporary **Shogo test URLs** with the XOPS-WXR channel
  (`AutoUpdate/version.txt`, `notes.txt`) once those are published.
- Implement the **d3d8.dll download/install** behind the YES answer. It must
  stage both files, verify a minimum size, install the DLL next to
  `xops096.exe`, write `version.txt` last, and roll back on any failure -
  exactly the Shogo order. Never download Shogo's CShell.dll.
- Release step: put the new `d3d8.dll` in `AutoUpdate/`, rewrite
  `AutoUpdate/notes.txt`, bump `AutoUpdate/version.txt` and
  `launcher-xr/version.txt`, then commit/push.
- Optional: an icon and an updater for `XOPSVR.exe` itself.

## 7. Separate the mod from the probe

`src/d3d8probe.cpp` is still one translation unit containing the probe,
steering, HUD, hands and capture. Once the above settles, split the shipped
mod from the diagnostic hooks so a release build carries no probe machinery.

## 8. Lower-priority / not requested recently

- **Floating zoom scope** (HWXR Halo style) instead of the simple zoom toggle.
- **Removing the probe's debug key set** from release builds (covered by 7).

---

## Key files

```
src/d3d8probe.cpp                    all hooks, HUD, hands, laser, weapon clone, stereo
src/xrapi.cpp/.h                     XrApi parsing, poses, input
Docs/OpenCode/DeepSeek41Flash/COMPLETED_STEPS.md  implemented work
Docs/OpenCode/DeepSeek41Flash/CURRENT_STEPS.md    current architecture/state
Docs/OpenCode/DeepSeek41Flash/WORKLOG.md          experiment history
build/build-dev.bat                  build/deploy dev DLL
build/build-release.bat              build release DLL + launcher into Release\
tools/start-level.ps1                title -> level (do not auto-run during hand tests)
E:\tmp\Q3Mods\xops096\xops_wxr_probe.log         runtime log
E:\tmp\Q3Mods\xops096\config.txt               handedness / haptics / debug / stereo
launcher-xr\                        XOPSVR.exe source + build script
D:\Daven Bigelow\Documents\GitHub\HaloCEWXR      stereo reference
D:\Daven Bigelow\Documents\GitHub\Shogo-VR-WXR   two-hand aim + launcher
```
