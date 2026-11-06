# XOPS-WXR
An attempt to mod classic singleplayer game X Operations into a 6DOF VR experience on standalones via WinlatorXR

Created with the help of MetaAI, Claude Code Opus 5 Medium effort, and OpenCode GO DeepSeek V4.1 Flash

Thanks to Luboš, GmoLargey, and others for their work on [WinlatorXR](https://github.com/WinlatorXR/WinlatorXR) and everyone else who had contributed to portable PC gaming via Winlator on other platforms

The rest of the README below has been created with the help of AI:

## Features

- **6DOF head tracking.** HMD yaw and pitch drive the game camera; your real-world forward direction is the direction the movement stick walks. Head yaw is written to both camera clusters so view and movement agree.
- **Visual-only head translation.** Leaning, crouching and standing move the rendered view, not the game's player position. The world is shifted at draw time by the HMD offset, so the game stays flat for gameplay while the view follows your head.
  - HMD height is clamped to **±1 m** before the gain.
  - **Right-stick click** recentres: the current HMD height becomes the flat game player height (`0`).
  - `pos_scale` sets the gain (`1.0` = 1:1, `0` disables translation).
  - `eye_height` is a manual float vertical calibration offset (default `0`, range ±5 m).
- **Hands, gun and laser.** Both tracked hands are drawn in the game's 3D pass with a no-roll head basis, so they stay fixed relative to the VR camera when the world rolls. They live in the default/recentre head-height frame, so moving the HMD moves the view, not the hands. The dominant hand carries a clone of the game's own weapon model and a depth-tested laser. The clone follows weapon swaps, drops, pickups and empty slots.
- **Quest 3 hand roll corrected by default** (`hand_flip = false`). Set `hand_flip = true` to recreate the old/uncorrected roll on other headsets.
- **Two-handed aim.** Hold the **left grip** and the gun points at the off hand; the laser, shot and haptics all follow it.
- **Text-only HUD.** A target-resolution text/panel HUD replaces the game's spinning 3D weapon-slot models; the weapon still appears in the VR hand. Title, menus, objective, death and victory keep the game's own 2D UI.
- **Haptics.** A short pulse per shot, driven by the magazine counter. Both controllers buzz while two-handed aim is held.
- **Head roll.** The presented 640×480 game sprite rotates with head tilt; the HUD, sync pixel and hands stay fixed. Black sprite corners are accepted and hidden by the lenses. `roll = true`, `roll_sign = -1` by default.
- **Automatic 2D/VR switching.** `MODE_VR = 0` on title/menu/death/victory, `MODE_VR = 1` in gameplay, so flat menus remain usable.
- **Launcher.** Play, Options (including HMD position gain) and an update check. Config lives in the game folder, next to `xops096.exe`.
- **Experimental AER stereo.** `stereo = aer` opts in; `mono` is the shipping default until the device pass confirms the eye sign.
- **Debug keys are locked behind `0`.** WinlatorXR does not map `0`, so a device session cannot trigger probe/HUD/dump keys by accident.

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
| Manual PC / debug mode | `0` (desktop keyboard; unlocks the debug keys) |

Left-handed mode (`handedness = left`) swaps only **Shoot** and **Fire mode**.

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
5. Start `XOPSVR.exe` (or run `xops096.exe` directly).
6. In the headset, use **mono** for the first run. AER stereo is experimental.

**Config**

`config.txt` is created in the game folder on first run if missing:

```ini
handedness = right
hand_flip = false           # false = Quest 3 roll correction; true = old/other-device roll
haptics = true
haptics_hand = dominant     # dominant | both | left | right
haptic_frames = 2           # 1..5
verbose_debug = false       # true while testing
stereo = mono               # experimental: aer
roll = true
roll_sign = -1              # +1 if the horizon tilts the wrong way
eye_height = 0.0            # manual render offset, metres, +/-5
pos_scale = 1.0             # HMD translation gain, 0..2 (1.0 = 1:1)
```

The launcher's **Options** dialog edits the same file, including **HMD position gain** (`pos_scale`).

**Quick checks**

- The first line of `xops_wxr_probe.log` must show the build you installed. A stale `d3d8.dll` is otherwise invisible.
- If the game exits before the first frame on device, confirm XOPS is in **Windowed** mode and that the log shows `windowed 1`. Fullscreen startup is not supported on WinlatorXR yet.
- If the view moves too far when you move your head, lower `pos_scale` (or set it to `0` to disable translation) in Options.
- If the horizon rolls the wrong way, set `roll_sign = +1`.
- If the hands are rolled on a non-Quest-3 headset, set `hand_flip = true`.

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
- `build-release.bat` compiles with `XOPS_DEV_BUILD=0` to `out\release\d3d8.dll`, builds `launcher-xr\XOPSVR.exe`, and assembles the `Release\` folder. The release DLL has no probe log, no debug keys and no draw/frame capture.
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

**Camera and 6DOF.** The game camera lives in known globals (the render copy and the player's own orientation are separate clusters). `SteerTick` writes yaw and pitch from the HMD; head yaw is applied to both clusters so movement follows the head. The game re-clamps its camera Y every frame, so the HMD's **translation** is applied at draw time instead: the world vertex copies are shifted opposite the eye by `CameraOffset()` on the non-indexed and indexed world paths. The hands are placed in the default/recentre head-height frame and take only the head's **orientation**, so moving the HMD moves the view, not the hands. Right-stick click recentres; `pos_scale` and `eye_height` tune the result.

**XrApi.** The mod listens on UDP **7872** for head/hand poses, sticks, buttons, IPD, FOV and the sync id, and sends to **7278** the per-hand haptic durations, `MODE_VR`, `MODE_3D` and the FOV. Poses are converted from OpenXR (right-handed, +Y up, −Z forward, metres) into the game's left-handed, +Y-up, +Z-forward space. The 10×10 red sync pixel in the top-left echoes `HMD_SYNC`; blue carries the AER eye flag.

**HUD, hands and weapon clone.** The custom HUD is a target-resolution D3D texture drawn inside the centre square. The game's own 2D HUD and spinning 3D slot models are suppressed in gameplay while the custom HUD is active; the slot capture and tail recording still run so the VR hand's cloned weapon model can be assembled from the game's own draws. The hands and laser are drawn with the game's white texture and glove texture using a no-roll head basis.

**Launcher.** `XOPSVR.exe` finds `xops096.exe` next to it (or up to two folders up), edits `config.txt` in the game folder, and checks the release channel over HTTPS. The current test channel is the Shogo WXR AutoUpdate URLs; the DLL download step is deliberately disabled until XOPS-WXR has its own channel.