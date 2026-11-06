# XOPS-WXR — NEXT STEPS

**Only work that is still open lives here.** Implemented features are in
`COMPLETED_STEPS.md`; history is in `WORKLOG.md`; architecture is in
`README.md` and `CURRENT_STEPS.md`.

**Last build:** `built Nov  6 2026 18:01:24`, SHA-256
`0d24ca6ea2b685d3b303faf5fbf016194829696b0582415a4f88b313129cd524`.
Release: 282,624 bytes, SHA-256
`a4b214618fe7ad8dfc7c5a050eb35728c17091db8321853bf0109624b332c908`.

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

## 4. Quest 3 device pass

PC testing has covered the feature set so far. The first device pass is still
outstanding:

- Container at **1280x960** (native 4:3, no bars) unless testing stereo.
- Shipped config has `verbose_debug = false`; full log on demand.
- Check: input mute while unfocused (physical only), XrApi input still works,
  A/B/X/Y and arrow blocking, full keyboard/mouse isolation in gameplay,
  `MODE_VR` screens, both hands and their angles, laser and hand-aim,
  two-handed aim, weapon clone across swaps/drops/pickups and textures, haptics
  feel, tracking latency/judder.
- **6DOF / eye height:** crouch and stand and confirm the render follows the
  head without moving the player, and that the hands **stay at their
  level-start/recentre position** instead of riding with the current HMD
  translation. Confirm the right-stick click re-zeroes XYZ and yaw. `pos_scale`
  defaults to **1.0** (1:1) and is editable in the launcher's Options dialog.
  Do not change the draw code.
- **Level start:** begin several levels and confirm the view's XYZ and yaw are
  relative to the player model at spawn (no inherited yaw/height from the
  menu), and that shots follow the right hand on every level.
- **Hand roll:** tilt the head and confirm the hands/gun roll with the
  presented world instead of staying glued to the view. The previous
  counter-rotation was removed.
- **Left stick:** with WinlatorXR's current arrow-key mapping the mod now
  translates arrows to W/A/S/D. Confirm movement and that no arrow reaches the
  weapon-swap binding. Mapping the stick to the analog XrApi left stick remains
  the cleaner long-term fix.
- **Roll:** tilt the head and confirm the world tilts the correct way and the
  HUD stays fixed. The default is `roll_sign = -1`; if it is inverted on
  device, set `+1`. If the black corners are visible in the lenses, note it
  for the stereo pass.
- After this run, create `BUG_FIXES.md` from the device findings.

## 5. Stereo

- **AER is implemented and desktop-verified**; `stereo = aer` opts in.
- On device, confirm the **eye sign** (flip if depth looks inverted). Both
  reproject helpers are now 1:1 metres (the non-indexed `*100` centimetre
  assumption was removed with the height work), so there is no scale left to
  tune; one-line sign if needed.
- **AER menus:** the runtime now stays `MODE_VR=1` for the whole run and only
  `MODE_3D` drops to 0. Confirm the title/menu/death/objective/victory screens
  appear as the centre-third floating panel, AER is visibly off there, and the
  hand laser pointer + right-trigger click select menu items. This also tests
  whether the old `MODE_VR 1 -> 0` crash is gone.
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
