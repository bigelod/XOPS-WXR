# XOPS-WXR
An attempt to mod classic singleplayer game X Operations into a 6DOF VR experience on standalones via WinlatorXR

Created with the help of MetaAI, Claude Code Opus 5 Medium effort, and OpenCode GO DeepSeek V4.1 Flash

Thanks to Luboš, GmoLargey, and others for their work on [WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) and everyone else who had contributed to portable PC gaming via Winlator on other platforms

The rest of the README below has been created with the help of AI:

**Status: V0.1 alpha, device-verified on Quest 3.** Tested end to end on Quest 3
with both left- and right-handed setups, in both mono and AER. **Mono is the
recommended default** because it is more comfortable; AER works but can be
less comfortable for some players.

## Features

- **6DOF head tracking.** HMD yaw and pitch drive the game camera; your real-world forward direction is the direction the movement stick walks. Head yaw is written to both camera clusters so view and movement agree.
- **Per-level recentre.** Every level start waits 0.1 s for the game to place
  the spawn, then makes the current HMD XYZ/yaw the new origin against the
  player model. It recentres from the camera yaw the game actually presents,
  and resolves the player slot from the live player index, so aim stays correct
  on maps that use a different slot (BUILDING_EXT runs as player 6, DUEL_EXT
  as player 0). **Right-stick click** does the same at any time.
- **Hand aim.** The game's shot is aimed at the hand laser. The aim is written
  both in the input phase and after the frame is drawn, so a shot processed
  early in the game's update still uses the hand direction.
- **Visual-only head translation.** Leaning, crouching and standing move the rendered view, not the game's player position. The world is shifted at draw time by the HMD offset, so the game stays flat for gameplay while the view follows your head.
  - HMD height is clamped to **±1 m** before the gain.
  - `pos_scale` sets the gain (`1.0` = 1:1, `0` disables translation).
  - `eye_height` is a manual float vertical calibration offset (default `0`, range ±5 m).
- **Hands, gun and laser.** Both tracked hands are drawn in the game's 3D pass and stay world-anchored at the level-start/recentre position: they roll with the presented world, not with the current HMD height. The dominant hand carries a clone of the game's own weapon model and an always-on, depth-tested laser. The clone follows weapon swaps, drops, pickups and empty slots.
- **Quest 3 hand roll corrected by default** (`hand_flip = false`). Set `hand_flip = true` to recreate the old/uncorrected roll on other headsets.
- **Two-handed aim.** Hold the **left grip** and the gun points at the off hand; the laser, shot and haptics all follow it.
- **Text-only HUD.** A target-resolution text/panel HUD replaces the game's spinning 3D weapon-slot models; the weapon still appears in the VR hand.
- **Haptics.** A short pulse per shot, driven by the magazine counter. Both controllers buzz while two-handed aim is held.
- **Head roll.** The presented 640×480 game sprite rotates with head tilt; the HUD, sync pixel and hands stay fixed. Black sprite corners are accepted and hidden by the lenses. `roll = true`, `roll_sign = -1` by default.
- **Floating panels for non-gameplay screens.** `MODE_VR` stays `1` for the
  whole run; the title, menus, objective, death and victory screens are shown
  as a centre-third panel with a dominant-hand laser pointer and the
  WinlatorXR-supplied mouse. AER is used in gameplay only.
- **VR-only gameplay input.** In gameplay the game is driven from XrApi, not
  from the desktop keyboard/mouse; only `ESC` and `0` are accepted from the
  physical keyboard. Menus keep physical keys. The left controller's menu
  button runs the game's own ESC consumer. A **victory** result screen freezes
  the game's mouse look so the camera cannot spin before the objectives panel.
- **Launcher.** Play, Options (including HMD position gain and HUD scale) and
  an update check against XOPS-WXR's own AutoUpdate channel. Config lives in
  the game folder, next to `xops096.exe`. Play refuses to start a second game
  instance; the updater refuses to run while the game is open.
- **Experimental AER stereo.** `stereo = aer` opts in; it is device-verified on
  Quest 3, but **mono is the recommended default** for comfort.
- **Debug keys are locked behind `0`.** WinlatorXR does not map `0`, so a device session cannot trigger probe/HUD/dump keys by accident.

## To-do

V0.1 is feature-complete. Remaining work is post-launch:

- **Muzzle effects and hand/weapon placement.** Move the game's own muzzle
  flash, smoke and shell effects from the hidden weapon position onto the VR
  hand; polish per-weapon hand and muzzle offsets.
- **AER stereo (optional).** Device-verified on Quest 3, but mono is more
  comfortable; keep mono the default and decide later whether to expose AER
  more prominently.
- **Optional menu simplification.** Call the game's title/mission-select action
  (`0x411F80`) directly instead of relying on the cursor hit-test.
- **Not planned:** SBS stereo.

## Default Controls

| Action | Binding |
|---|---|
| Move | Left stick |
| Turn left / right | Right stick X |
| Recentre / reset HMD height | Right stick click |
| Two-handed aim (hold) | Left grip |
| Fire mode | Left trigger |
| Shoot | Right trigger |
| Jump | Right grip |
| Reload | B |
| Switch weapon | A |
| Drop weapon | X |
| Zoom (toggle) | Y |
| Menu / back | `ESC` or the left controller menu button |
| Point / click on a menu | Dominant-hand laser, or the WinlatorXR mouse; right trigger clicks the laser |
| Manual PC / debug mode | `0` (desktop keyboard; unlocks the debug keys) |

In gameplay only `ESC` and `0` are accepted from the physical keyboard; menus,
objectives, death, victory and intro screens keep physical keys. Left-handed
mode (`handedness = left`) swaps only **Shoot** and **Fire mode** (and the
position of the gun to left hand).

## Setup - Release

**Requirements**

- X Operations, the **`xops096.exe`** build (game files are not included).
- [WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) on a standalone headset.
- Container resolution **1280×960** (native 4:3). **DXVK 1.10.1** for D3D8+ (the last DXVK with `d3d8.dll`); Turnip recommended.
- XOPS set to **Windowed** mode. The mod turns the window borderless and full-display; the game's own fullscreen mode is not used.

**Install**

1. Install X Operations in WinlatorXR.
2. Set XOPS to **Windowed** mode (video options, or copy `config - windowed.dat` over `config.dat` in the game folder).
3. Back up any existing `d3d8.dll` in the game folder.
4. Copy these files into the game folder, next to `xops096.exe`:
   - `d3d8.dll` — the mod
   - `XOPSVR.exe` — the launcher
   - `config.txt` — settings
   - `version.txt` — the launcher's local version
5. Start `XOPSVR.exe` (or run `xops096.exe` directly). The launcher refuses to
   start a second copy while XOPS is already running; close the existing game
   first.
6. In the headset, **mono** is recommended for the first run; AER is also
   available but can be less comfortable.

**Config**

`config.txt` is created in the game folder on first run if missing:

```ini
handedness = right
hand_flip = false           # false = Quest 3 roll correction; true = old/other-device roll
haptics = true
haptics_hand = dominant     # dominant | both | left | right
haptic_frames = 2           # 1..5
verbose_debug = false       # true while testing
stereo = mono               # mono recommended; aer optional (less comfortable)
roll = true
roll_sign = -1              # +1 if the horizon tilts the wrong way
eye_height = 0.0            # manual render offset, metres, +/-5
pos_scale = 1.0             # HMD translation gain, 0..2 (1.0 = 1:1)
hud_scale = 1.1             # HUD visual scale, 0.5..2.0 (1.0 = native)
```

The launcher's **Options** dialog edits the same file, including **HMD position gain** (`pos_scale`) and **HUD scale** (`hud_scale`).

**Quick checks**

- The first line of `xops_wxr_probe.log` must show the build you installed. A stale `d3d8.dll` is otherwise invisible.
- If the game exits before the first frame on device, confirm XOPS is in **Windowed** mode and that the log shows `windowed 1`. Fullscreen startup is not supported on WinlatorXR yet.
- If the view moves too far when you move your head, lower `pos_scale` (or set it to `0` to disable translation) in Options.
- If the horizon rolls the wrong way, set `roll_sign = +1`.
- If the hands are rolled on a non-Quest-3 headset, set `hand_flip = true`.
- The launcher checks XOPS-WXR's GitHub AutoUpdate channel on startup. If it
  offers an update, close XOPS before accepting; it replaces `d3d8.dll` next to
  `xops096.exe` and advances `version.txt`. A failed or invalid download leaves
  the existing DLL untouched.

## Build Advice - For Contributors

**Toolchain**

- Visual Studio 2022, **x86** toolchain. No DirectX SDK and no MinHook are required; the mod declares the small D3D8 surface it uses and patches import-table/vtable entries itself.

**Build the mod**

Two targets share `src/d3d8probe.cpp`; `XOPS_DEV_BUILD` selects the diagnostics at compile time.

```bat
call "<VS2022>\VC\Auxiliary\Build\vcvarsall.bat" x86

build\build-dev.bat        REM Windows-PC dev: probe, debug keys, verbose log
build\build-release.bat    REM release: no probe log, keys, dumps or grabs
```

- `build-dev.bat` compiles with `XOPS_DEV_BUILD=1` to `out\dev\d3d8.dll` and deploys to `E:\tmp\Q3Mods\xops096` by default. This is the build to use while developing or diagnosing on a Windows PC: it keeps the probe keys, `HOOKS`/`HOOK` trace, verbose log, draw dumps and frame grabs.
- `build-release.bat` compiles with `XOPS_DEV_BUILD=0` to `out\release\d3d8.dll`, builds `launcher-xr\XOPSVR.exe`, and assembles the `Release\` folder. The release DLL has no probe keys, draw dumps or frame grabs. Set `verbose_debug = true` in Options for a safe Quest diagnostic log (XrApi raw/sticks, mode, EYE, ROLL); with `false` it writes no log.
- `build\build-probe.bat` is kept as the old dev-build alias.
- `XOPS_DEV_BUILD` defaults to `1` if a build script does not define it.

**Build the launcher**

```bat
launcher-xr\build.cmd
```

This produces `launcher-xr\XOPSVR.exe`. It is a build artifact and is not committed.

**Source layout**

- `src/d3d8probe.cpp` — the whole mod: D3D8 hooks, camera steering, render-side 6DOF, HUD, hands, weapon clone, laser, roll, haptics, launcher-facing config.
- `src/xrapi.cpp` / `src/xrapi.h` — XrApi UDP client: packet parsing, pose conversion, recentre, input, outbound haptics/modes.
- `launcher-xr/` — Win32 launcher (`XOPSVR.cpp`, `XOPSVR.rc`, `Resource.h`, `build.cmd`).
- `tools/` — desktop test helpers (`start-level.ps1`, click recording).

**Testing on the desktop**

- Use the XrApi UDP Tester or a synthetic Python feeder against `127.0.0.1:7872`.
- `tools/start-level.ps1` replays the title → mission → briefing clicks.
- `xops_wxr_probe.log` holds the build stamp, camera/pose diagnostics, draw-path counts and the end-of-session summary.
- Check the build stamp before interpreting any report; a stale DLL looks like a code bug.

**House rules**

- Do not commit XOPS's game data, game binaries or build outputs (`*.dll`, `*.exe`, `*.obj`, `Release/` are ignored).
- Keep `config.txt` in the game folder; the mod still reads the old parent-folder file if the local one is missing.
- `verbose_debug = false` is the shipping default; turn it on for a test run and include the log with any bug report.

## How it works

**Injection.** `d3d8.dll` sits next to `xops096.exe` and forwards every call to the system `d3d8.dll` (DXVK 1.10.1's `d3d8.dll` on the device). At device creation it hooks the `IDirect3DDevice8` vtable and the few import-table entry points it needs (`Present`, `EndScene`, transforms, draw calls, texture/state changes). No external injector is required. The proxy exports **only `Direct3DCreate8`**: DXVK 1.10.1 does not export the optional Microsoft debug helpers (`DebugSetMute`, `Validate*`), so the game must see the same NULL `GetProcAddress` result it sees without the mod. Re-exporting them caused a jump to address 0, and a wrapper changed the call/return stack shape and moved the crash to address 1.

**Presentation.** The game renders into a dedicated **640×480** render target, keeping its native FOV. `Present` binds the real back buffer, draws that target as a 4:3 sprite (rotated as a 2D quad by head roll), then draws the replacement HUD and the sync pixel on top.

**Camera and 6DOF.** The game camera lives in known globals: cluster **A** is the rendered view and cluster **B** is the player look/aim. B is a **per-player array** (`0x48DED4 + playerIndex * 0x50`); the local index is read from `0x48DDD8` on every access because different maps use different slots (BUILDING_EXT runs as player 6, DUEL_EXT as player 0). `SteerTick` writes yaw and pitch from the HMD to both clusters, so movement follows the head. Every level start waits 0.1 s for the game to place its spawn, then recentres from the camera yaw the game actually presents; right-stick click does the same at any time. The game's shot is aimed at the hand laser in the input phase and again at `EndScene` for a shot processed early in the next frame. The game re-clamps its camera Y every frame, so the HMD's **translation** is applied at draw time instead: the world vertex copies are shifted opposite the eye by `CameraOffset()` on the non-indexed and indexed world paths. `pos_scale` and `eye_height` tune the result. On a victory result screen the published cursor is frozen so the game's own mouse look cannot spin the camera before the objective panel.

**Input.** Gameplay is driven from XrApi, not from the desktop: the mod builds the game's key image, calls its own mapper and player update, and uses the game's own ESC consumer for the left menu button. Only `ESC` and `0` pass from the physical keyboard in gameplay; menus keep physical keys. The non-gameplay panels accept both the dominant-hand laser and the WinlatorXR mouse, with the most recently moved pointer winning; the cursor is remapped from the centre-third panel into the game's 640x480 UI space.

**XrApi.** The mod listens on UDP **7872** for head/hand poses, sticks, buttons, IPD, FOV and the sync id, and sends to **7278** the per-hand haptic durations, `MODE_VR`, `MODE_3D` and the FOV. Poses are converted from OpenXR (right-handed, +Y up, −Z forward, metres) into the game's left-handed, +Y-up, +Z-forward space. The 10×10 red sync pixel in the top-left echoes `HMD_SYNC`; blue carries the AER eye flag.

**HUD, hands and weapon clone.** The custom HUD is a target-resolution D3D texture drawn inside the centre square. The game's own 2D HUD and spinning 3D slot models are suppressed in gameplay while the custom HUD is active; the slot capture and tail recording still run so the VR hand's cloned weapon model can be assembled from the game's own draws. The hands and laser are world-anchored at the level-start/recentre position and roll with the presented world.

**Launcher.** `XOPSVR.exe` finds `xops096.exe` next to it (or up to two folders up), edits `config.txt` in the game folder, and checks XOPS-WXR's AutoUpdate channel over HTTPS (`version.txt`, `notes.txt`, `d3d8.dll` under `github.com/bigelod/XOPS-WXR`). On YES it downloads and verifies the DLL, replaces the one next to `xops096.exe`, then advances `version.txt`. It refuses to update while the game is running and refuses to start a second game instance.