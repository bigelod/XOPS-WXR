# XOPS-WXR - OpenCode / DeepSeek V4.1 Flash handover

**Session date:** 5-6 Nov 2026.
**Read this first, then `COMPLETED_STEPS.md` for what is done, `NEXT_STEPS.md`
for what is left, and `WORKLOG.md` for the experiment history.**

This folder is the third agent's working notes. The second agent's folder,
`Docs/ClaudeCode/`, remains the **most accurate long-form record** of the
project and should still be read as current truth where it does not conflict
with this file. `Docs/MetaAI/` is the first attempt: keep its IDA findings,
discard its code and its protocol claims.

Evidence tags are the project's own: `[spec]`, `[runtime]`, `[general]`,
`[derived]`, `[code]`, `[device]`, `[unverified]`. Nothing in this folder is
`[device]` unless it says so; the build is PC-verified, not device-verified.

---

## 1. Where the project actually is

The desktop probe works. The camera is solved. **No VR code has run under
WinlatorXR and no build has been verified on the Quest 3.**

| Thing | State |
|---|---|
| Injection vector - `d3d8.dll` proxy | works on the desktop, forwards to `C:\Windows\system32\d3d8.dll` |
| Camera position and angles | **solved and writable** `[device, runs 8-13]` - see the address table below |
| XrApi 0.5 client | parses the **UDP Tester**, not yet a real device packet |
| HUD / menu flag | **solved**: game state flags drive `MODE_VR`; `InGameplay()` uses mission/result/player fields |
| Window / sync pixel / FOV | **implemented and desktop-verified** |
| Replacement HUD | **implemented**: target-resolution D3D texture in the centre square, desktop-verified |
| Decoupled aim (F-7) | not started - the hardest remaining problem |
| Height / crouch | blocked: the game re-clamps Y every frame `[device, run 13]` |
| Roll | plan settled (rotate the rendered frame), not implemented |
| Stereo | out of scope until mono is device-verified |

### The camera addresses, as inherited

| Address | Field | Evidence |
|---|---|---|
| `0x0048C3B0` | Y, vertical position | `[device, run 8]` |
| `0x0048C3B4` | horizontal axis 1 | `[device, run 9]` |
| `0x0048C3B8` | horizontal axis 2 | `[device, run 9]` |
| `0x0048C3BC` | yaw | `[device, run 10]` |
| `0x0048C3C4` | pitch | `[device, run 10]` |
| `0x0048E0A8`...`0x0048E0BC` | cluster B, the player's own orientation | `[device, run 11]` |

Cluster A is the render copy; cluster B is what movement reads. The probe
writes **both** by default. Yaw is positive; pitch is positive-down in XOPS,
so the pose's positive-up pitch is negated.

### RESOLVED: pitch sign is `+1` (owner, 5 Nov 2026)

`E:\tmp\Q3Mods\xops096\xops_wxr_steer.ini` contains:

```
yawSign 1
pitchSign 1
cluster 2
```

The owner drove the head through UP / RIGHT / DOWN / LEFT on the fullscreen
build and confirmed the pitch direction is **correct**. So **`pitchSign +1` is
the settled value** for this build, and the older note that `-1` was settled in
run 13 is superseded. `yawSign +1` and `cluster both` also held. Do not
"correct" the INI back to `-1`.

Also from the older deployed log (`built Nov  4 2026 11:44:17`, 374 frames):

- steering was **off** at shutdown, so that run did not exercise the camera;
- the tester's head position was exactly `0,0,0`, so positional DOF could not
  be judged in that run;
- the session saw **seven** distinct `SetTransform` call sites, not the three
  WORLD sites recorded earlier. Two additional WORLD sites appeared at
  `0x0040127A` and `0x00406529`. Not load-bearing for the current route, but
  the disassembly map in `CURRENT_STATUS.md` is now slightly stale.

---

## 2. What this pass changed

Three items from `Docs/ClaudeCode/NEXT_STEPS.md`, all implementable without the
headset. The build compiles cleanly and was deployed to
`E:\tmp\Q3Mods\xops096` with SHA-256 `match=True` (stamp
`built Nov  5 2026 09:18:08`, SHA-256
`24a2bacbfc881694c84fae05a876997751557559a67aae4d130cb20e4abb31b0`).

### 2a. Window: borderless, fills the display (NEXT_STEPS item 2)

- Strip `WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME`, add
  `WS_POPUP`, size to `SM_CXSCREEN x SM_CYSCREEN` at `(0,0)`.
- Applied after `CreateDevice`, after `Reset`, and **every frame** from
  `Present` (idempotent - it does nothing once correct).
- **`F` toggles it.** Off only stops re-applying; it does not restore the old
  chrome.
- **Correction, 5 Nov:** D3D8 windowed `Present` does **not** stretch a 640x480
  back buffer into a larger client rect. A 1920x1080 borderless window showed
  the image 1:1 in the top-left with black elsewhere. Passing the client rect
  as `Present`'s destination did not fix it. The working route is the game's
  own **fullscreen** config - see 2d.

### 2b. Red sync pixel (NEXT_STEPS item 3)

- At the top of the `EndScene` hook, before forwarding to the real `EndScene`
  (so it is the last draw of the scene):
  `Clear(1, &{0,0,10,10}, D3DCLEAR_TARGET, XRGB(sync,0,0), 0, 0)`.
- `sync` is `HMD_SYNC` from float 28, **rounded and echoed exactly**, clamped
  0..255. Green is always 0. Blue is 0 (no AER).
- Uses vtable slot 36 (`IDirect3DDevice8::Clear`), captured at hook time; no
  `LockRect`, so the non-lockable back buffer is irrelevant.
- Only drawn once a valid packet has arrived; with no packet there is nothing
  to echo.
- Assumption to check if it does not appear: XOPS has not left a non-back-buffer
  render target bound at `EndScene`. Nothing in the probe suggests it does, but
  this is the first place to look.

### 2c. Real FOV (NEXT_STEPS item 4)

The outbound packet now sends **`80.7 x 65.0`** degrees instead of
`104.5 x 104.5`, taken from the projection matrix measured on device
(`fovY 65.0`, `fovX 80.7`, 4:3). The field is degrees, applied only if > 1.

### 2d. Desktop verification - PASSED, 5 Nov 2026

Run on the desktop install with synthetic XrApi 0.5 packets matching the
tester's `Form1.BuildDataPacket` layout, in the game's own fullscreen mode.
**This is desktop-verified, not Quest-3 device-verified.**

| Check | Result | Evidence |
|---|---|---|
| Fullscreen, no chrome | **passes** - `CreateDevice` reports `windowed=0` at 640x480; display mode 640x480; no caption/border | `[desktop]` log + `GetWindowLong`/`GetWindowRect` |
| Red sync pixel, exact value | **passes** - screen `(0,0)` read `R=120 G=0 B=0`, then `R=36 G=0 B=0` after the feed changed | `[desktop]` screen capture, `PrintWindow`, and `GetPixel` all agree |
| Sync pixel drawn in gameplay | **passes** - log shows `SYNC first red square drawn: RGB(120,0,0)`; owner saw it in the `building.ext` level | `[desktop + owner]` |
| Outbound FOV | **passes** - captured `0.0 0.0 2 0 80.7 65.0` on 7278 | `[desktop]` UDP capture |
| Gameplay signature unchanged | `0` SetTransform, `84` indexed draws, camera fields live | `[desktop]` log |

**The important discovery: use the game's own fullscreen mode, not a stretched
window.** With the game windowed, D3D8 presented the 640x480 back buffer 1:1
in the top-left of a 1920x1080 window - black everywhere else. Passing the
client rect as `Present`'s destination rect did *not* fix it. Changing the
game's own config to fullscreen (`config.dat` byte `0x13` = `01`; the old
windowed file is `config - windowed.dat`, byte `0x13` = `00`) makes
`CreateDevice` report `windowed=0` at 640x480 and the display mode becomes
640x480, so the whole frame is presented and the GPU/display scales it. That
is the working path, and it also keeps the HUD and mouse mapping in their
native 640x480 coordinates.

**Superseded by the late-pass composite below:** the windowed limitation is
gone. The device is now created at the desktop size and the 640x480 game frame
is copied, stretched and presented from a texture, so the game no longer has
to be in its own fullscreen mode.

With the composite, the sync square is 10x10 in the **final presented frame**:
it is cleared after the scaled game frame and HUD, just before `Present`. In
fullscreen the probe also changes the window style from `0x14000000` to
`0x94000000` (adds `WS_POPUP`); it does not resize it.

**Max-resolution experiment revisited, 5 Nov (late pass).** Overriding
`CreateDevice` / `Reset` to ask for the desktop size does create a wider
device, window and display mode, and the game stays alive. As first observed,
it still draws the world and 2D HUD in a **640x480 area at the top-left** of
the larger back buffer. That is no longer a dead end: the probe now exploits
that corner as the game-frame texture. The back buffer is created at the
desktop/container size by default (`g_maxRes = TRUE`); `M` toggles the request
for the next `Reset`.

**Target-resolution composite + replacement HUD, 5 Nov (late pass).** The
device back buffer is the desktop/container size, but the game no longer draws
into its corner. It renders into its **own 640x480 render target**, which is
what keeps the native FOV and the 3D HUD weapon models intact:

1. `EnsureCompositeResources` creates a 640x480 `D3DUSAGE_RENDERTARGET`
   texture plus a D16 depth surface and binds them. The target stays bound
   **outside** `Present`, so title/demo draws that happen between scenes also
   land in it.
2. `hkSetViewport` maps any larger viewport the game requests back into
   640x480; `hkSetRenderTarget` keeps the game on that target.
3. `hkBeginScene` does not rebind or clear, so a pre-`BeginScene` title
   background survives; `hkEndScene` leaves the game target bound.
4. `Present` binds the real back buffer, draws the 640x480 texture at its
   native 4:3 aspect, then the GDI-rendered HUD/panel textures and the red sync
   square, presents, and rebinds the 640x480 target for the next frame.

The persistent binding is what fixed the title/demo regression: the intro
scene is drawn outside the game's `BeginScene`, so a per-scene bind missed it
and left an empty background. The render target is also the FOV/model fix:
with the game drawing against the 1920x1080 back buffer, its FOV changed and
the 3D HUD models went off-screen even when the projection patch was off.

**The game's default projection and FOV are not changed.** The outbound FOV
stays `80.7 x 65.0`. The supported device configuration is a **4:3 container
(1280x960)**: the native 4:3 frame then fills the window exactly - no bars, no
crop, no stretch, and no FOV change. On a non-4:3 container the native image
letterboxes; that is deliberate fallback behaviour, not the device setup.
The `COMPOSITE native 4:3` log line reports the rectangle and bar sizes.

There is an **opt-in experiment** (`patchAspect 1` in `xops_wxr_steer.ini`,
default off) that patches the game's hardcoded projection aspect at IDA
`0x403A92` and fills the whole window anamorphically. It is not the supported
path; the owner rejected the resulting FOV feel.

The GDI-over-the-window prototype did not composite in a captured image; a
texture drawn inside the D3D frame does. The HUD texture is only re-uploaded
when the player values or the size change. The game's 2D HUD is suppressed in
gameplay only when the custom HUD texture is valid, so a texture failure falls
back to the game's own HUD instead of a blank screen. Menus/title/objective/
death/victory keep their own 2D UI (`MODE_VR 0`).

**HUD values and layout.** From the player struct (`0x48DF00 + index*0x50`):
health word `+0x04` (FINE `>=0x50`, CAUTION `>=0x28`, DANGER `>0`, DEAD
otherwise), selected slot `+0x06`, weapon IDs `+0x0A/+0x0B`, current ammo
`+0x0C/+0x0D`, reserve `+0x0E/+0x0F`. Weapon names come from the pointer table
at `0x465DA0` (index 0 reads `None`). Left cluster: current ammo, then
remaining ammo, then `STATE <word>` in the state colour (FINE green, CAUTION
green-to-yellow, DANGER orange, DEAD red). Right cluster: selected weapon name
(`MP5SD` etc.) with the other slot's name below it; either reads white `None`
when its slot is empty.

**Death and reload status.** `HudShouldDraw()` is **gameplay only**. On death
and victory (mission result != 0) the custom HUD stops and the game's own 2D
UI is no longer suppressed, so the death summary/stats screen is visible. If
health reaches 0 during gameplay the STATE readout still shows `DEAD`. The byte
just before the player struct (`base - 1`) is the game's action state:
`3 = RELOADING`, `5 = CHANGING`; the HUD draws that word centred above the
panels in a larger status font. Font sizes were reduced slightly for VR:
big = region height `/ 10`, status = big `* 7/5`.

**Panels.** A second `A8R8G8B8` texture holds two rounded rectangles behind the
text, modelled on the original HUD boxes: a soft dark drop shadow, a
translucent white-ish fill (RGB 220, alpha 90), and a light border (RGB 255,
alpha 160). The text gets a black outline via a blue silhouette pass that is
converted to opaque black during upload, so it stays readable both on the
panels and on bright walls. The panel texture is separate from the text texture
so its alpha does not wash out the glyphs.

**Virtual hands + laser (5-6 Nov).** Both controller hands and the targeting
laser are real 3D geometry in the game's own pipeline. The important discovery:
XOPS's world vertices are **XYZ + TEX1** (stride 20, object space), and each
object is placed with its own **world matrix**. D3D then applies the view and
the 4:3 projection (`1.17726 / 1.56969 / 1.00125 / -0.10013`, near 0.1,
far 80). The hand/laser therefore have to reset **all three** transforms:

- `DrawHandAndLaserWorld` runs from the first 3D HUD weapon-model draw, while
  the 640x480 target and D16 depth buffer are still bound, so it lands in the
  gameplay layer underneath the HUD and not on top of it. It saves the device
  world/view/projection and sets **world = view = identity** plus the captured
  game projection, then draws with `XYZ | DIFFUSE | TEX1` (FVF 0x142, 24-byte
  vertices). Forgetting the world matrix was the bug that made the hands
  invisible: the leftover object matrix moved them out of the depth range.
- **Both hands** are solid 0.08 x 0.14 x 0.14 m 3D boxes at their own XrApi
  poses, each with six textured faces and a generated grey/black glove
  texture. The dominant hand gets `hand_flip`; the off-hand does not.
- **Laser:** a single continuous camera-facing 3D triangle strip sharing one
  right vector (so joints do not gap), 30 m long; width is scaled with
  distance so it reads as a roughly 2px beam. Depth test is `LESSEQUAL`,
  Z-write is off for the beam and on for the hand boxes, so walls really cut
  the laser off and hide a hand behind geometry.
- **Zoom:** Y toggles the game's zoom and hides the laser; the hands stay.
- **Fallback:** if the XrApi tester sends no hand position, a default
  view-space offset is used so the desktop test is still visible.

**Hand config.** `config.txt` is looked up **one directory above the game
folder** (for example `E:\tmp\Q3Mods\config.txt` when the game is
`E:\tmp\Q3Mods\xops096`), so a game update does not overwrite it. It is created
with defaults if missing:

```
handedness = right
hand_flip = false
```

`handedness` selects the dominant hand for the laser and the initial
shoot/fire-mode trigger swap (`N` still toggles both in-session).
`hand_flip` controls the 180-degree roll of the dominant virtual hand/gun
around its **roll/forward axis only** - not pitch or yaw. The default is now
the **Quest 3 layout**: `hand_flip = false` applies the roll correction
(Quest 3 hand data arrives 180 degrees rolled relative to other headsets;
HWXR, Shogo WXR and IMVR all hit this). Set `hand_flip = true` to recreate
the old, uncorrected gun roll on devices other than a Quest 3. The off-hand
stays unflipped.

The hand mesh is a compact dark palm plate with four **short, chunky finger
slabs** and a thick **thumb slab** angled outward from its own side, all using
the same glove texture and black outlines, SixDOFinator-calibration style:
their direction and the thumb side make roll/pitch/yaw obvious at a glance.

The game's own 3D HUD weapon-slot draws are **indexed** draws and are left
untouched; only the stride-28 2D `DrawPrimitiveUP` HUD set is replaced. The
crosshair's four UP line draws at `(317..323, 237..243)` are removed. **Still
open:** the 3D slot models are not moved into the centre square; they remain at
their own draw positions, and the replacement text is centred independently.
That is the next HUD placement pass.

`J` saves the composited back buffer to `xops_frame_<n>.bmp` next to the DLL
(the only reliable desktop inspection - window captures come back black).
`K` toggles the custom text HUD off/on for comparison; in probe mode `K` keeps
its classification meaning. `C` is **not** used, so the controller's drop key
`G` is never swallowed.

**MODE_VR** now uses the game's own state, not draw counts:
`0x490A44` mission index (`64` = title), `0x490A48` result (`1` = victory,
`2` = death/failure), and `0x48DDD8` player index (`-1` = menus/briefing).
Verified with two deaths and one victory: death and victory switch to 0,
respawn switches back to 1, and DUEL_EXT's low 39-indexed draw count no longer
drops head tracking. The internal renderer is still 640x480 - this is a
stretched output, not native yet.

**VR controller mapping (5 Nov).** Left stick moves, right stick turns the body
by a yaw offset (120 deg/s, `turnSpeed` in the INI), left grip walks, right
grip jumps, B reloads, A switches, X drops, Y toggles zoom, and the triggers
are shoot/fire-mode. Right-handed: right trigger shoots, left trigger cycles
fire mode. Left-handed swaps only those two (`N` toggles, `leftHanded` saved).
Fire mode/jump/reload/switch/drop and **zoom** are edge-pulsed; walk/shoot are
held. Zoom is a toggle in XOPS, so Y sends a press edge; the head still drives
the view, so the zoom is head-tracked.

**Steering is on by default.** `3` is only a debug toggle now. Input defaults
to **VR-only in gameplay**; `0` enables desktop keyboard/mouse for manual
tests without a headset or fake XrApi data, and pauses synthetic VR input.
**MODE_VR 0 screens always keep desktop input** (title, objective, menus,
death, victory) regardless of that toggle.

**The game no longer steals the desktop cursor.** XOPS recentres the pointer
with `SetCursorPos` every frame to read its mouse delta. When another window
has focus, `hkSetCursorPos` now records the requested position and returns
success **without** calling the real API, so the player's cursor stays where
they left it and the desktop is usable while the game keeps running in the
background. `hkGetCursorPos` still hands the game back exactly those
coordinates, so its computed look delta is zero and the view does not spin
when focus returns.

**Crash fixed, same day.** The probe then APPCRASHed three times in the CRT
`memcpy` at `d3d8.dll` RVA `0x985e`. The caller was `BuildSnapshot()` in the
inherited camera-hunt scanner: `VirtualQuery` and a raw `memcpy` of a game
region are not atomic, and the game can free the region in between. The scanner
is now **off by default** (the camera is already confirmed) and its copy uses
`ReadProcessMemory`, so an explicit re-enable cannot fault. The sync, window
and FOV code was not implicated. See `WORKLOG.md` for the evidence.

**Second failure fixed: fullscreen Reset.** On the title-to-level transition
XOPS called `Reset(640x480, windowed=0)` and our `FitWindow` had resized a
160x28 transition window to 1920x1080, so the Reset returned
`D3DERR_DEVICELOST` (`0x88760868`) and XOPS raised its â€œResetâ€ error. Both
`FitWindow` and the `Present` destination-rect override are now skipped when
the device is **fullscreen** (`g_deviceWindowed`, tracked from `CreateDevice`
and `Reset`).

**Minimize follow-up, 5 Nov.** A fullscreen game that loses focus (for example
when the owner alt-tabs to the chat) is minimized, and its next Reset also
returns `D3DERR_DEVICELOST`. `hkReset` now restores a minimized/hidden game
window before the real Reset and retries once if it still comes back lost. A
`ShowWindow` IAT hook was tried first and **reverted**: blocking the game's own
minimize changed startup state and made the frame-1 Reset fail. Build
`built Nov  5 2026 09:50:24`, SHA-256
`08f2be40f3188ef480de3ca5d85e62881d9bfb94f58b2f3ee252a874996bc878`.

### Test tooling: start a level

`tools/start-level.ps1` replays the title sequence at `(330, 242)` in the
native **640x480** frame. The first two clicks were recorded from the owner;
the briefing click was inferred from their note and the complete sequence was
tested end-to-end:

| Click | Time | Purpose |
|---|---|---|
| 1 | 0.00 s | title / begin |
| 2 | 0.56 s | mission selection |
| 3 | 3.00 s | mission briefing |

It is verified to reach `building.ext`. The sequence is in the game's native
640x480 frame. A larger back buffer still renders the game in a 640x480 area
at the top-left, so keep `--scale 1.0` there too; use `--scale` only if the
image is genuinely stretched. `tools/record_clicks.py` records a new sequence
and filters by **process id**, because XOPS changes its window handle during
the transition.

**Haptics + two-handed aim + input boundary + verbose logs (6 Nov, late
pass).** The outbound fields 1-2 are a per-hand vibration **duration in
frames**; the mod sends a 2-frame pulse per shot, decayed locally in the SHOGO
order, on the dominant hand - or both while the **left grip** is held.
Two-handed aim mounts the gun on the dominant hand and points it at the off
hand (`HandTwoHandedAim`, SHOGO's `UpdateTwoHanded`), and the shot follows it.
The left grip no longer walks. While the game window is unfocused the game
receives **no input at all**; in VR mode the physical A/B/X/Y keys are always
swallowed and the arrows are swallowed in gameplay, with manual PC mode (`0`)
exempt. Informative logging is behind `verbose_debug` (default false) in
`config.txt`; errors and state changes always log.

### What is still not built

**Desktop-verified now:** target-resolution composite, replacement HUD,
borderless window, exact sync pixel, real FOV, `MODE_VR` state.
**Still not built:** eye height, roll, decoupled aim, moving the 3D slot models
into the centre square. **Not device-verified:** nothing has run on the Quest 3
yet. See section 4.

---

## 3. Test this build on the desktop (procedure - already passed 5 Nov)

This is the first build that changes what you *see*. The procedure is kept so
the test can be repeated after any change to the composite, HUD, window, sync
or FOV path.

1. Set the WinlatorXR container/display to a **4:3 resolution, 1280x960**.
   That is the supported device configuration: the game's native 4:3 frame
   fills the container exactly with no bars, crop or FOV change.
2. Launch `E:\tmp\Q3Mods\xops096\xops096.exe` in the current windowed config.
   The probe resizes the window to the display and asks for a desktop-sized
   back buffer.
3. Start the **XrAPI UDP Tester** and point it at `127.0.0.1:7872`, or use the
   synthetic feeder, if the sync pixel matters. Vary the sync/frame-id field.
4. **Expect** the log to show:
   - `PROJECTION aspect patch disabled ...` (default FOV, no patch);
   - `RESOLUTION target-res override: 640x480 -> <desktop size> (windowed)`;
   - `COMPOSITE active: back buffer <w>x<h>, game render target 640x480`;
   - `COMPOSITE native 4:3: <w>x<h> -> <gw>x<gh> at <x>,<y> (bars 0,0)` on
     1280x960;
   - `HUD textures ... (text + panels)` once the level starts.
5. **Expect** the game world to fill the 4:3 container with the default FOV and
   the 3D weapon-slot models visible in the game's own slots. In gameplay the
   left cluster shows current ammo, remaining ammo and `STATE`, and the right
   cluster shows the selected weapon with the other slot below it.
   Title/menu/objective/death/victory keep their own 2D UI.
6. Press `J` in gameplay: `xops_frame_<n>.bmp` appears next to the DLL. Open
   it to inspect the exact frame; window captures of the D3D surface come back
   black on this box. `K` toggles the custom HUD off/on for comparison.
7. **Expect** the top-left 10x10 pixels of the presented frame to be pure red
   when a packet is arriving, matching the tester's sync field.

**What this proves:** the game renders into a native 640x480 target, so the
default FOV and the 3D weapon-slot models are preserved; the replacement HUD is
resolution-independent in the centre square. **What it does not prove:** headset
comfort (needs the Quest 3 and a real pose stream) or the final placement of the
3D slot models relative to the centre HUD.

---

## 4. Order of work from here

The live list is `NEXT_STEPS.md`. In short, what remains is per-weapon tuning
persistence, the off-hand support pose in two-handed aim, muzzle mapping, the
3D HUD slot placement decision, the first Quest 3 device pass (then
`BUG_FIXES.md`), AER device sign/scale and the deferred SBS experiment, and
the optional launcher.

Desktop verification of the composite, window, sync pixel, FOV, HUD, hands,
weapon clone, haptics, input boundary and AER is already done and recorded in
`COMPLETED_STEPS.md`.

---

## 5. Build and deploy

```
call "<VS2022>\VC\Auxiliary\Build\vcvarsall.bat" x86
build\build-probe.bat                 REM builds + deploys to E:\tmp\Q3Mods\xops096
build\build-probe.bat D:\VR\xops096-wxr
```

The script refuses a non-x86 compiler, wipes `out\probe`, and verifies the
deployed copy with SHA-256 (`match=True`). The DLL is
`out\probe\d3d8.dll`.

Line 1 of `xops_wxr_probe.log` is the build stamp. **Check it before
interpreting any report** - a stale build is invisible otherwise.

Current handover build: `built Nov  6 2026 00:04:41`, SHA-256
`ef596da3b33c4928e68f5a7b0da9d6903be4cb08d0df54607372301565932c2d`,
deployed to `E:\tmp\Q3Mods\xops096` (`match=True`).

---

## 6. Standing rules that still bind

- One change per deploy; the owner verifies on hardware.
- The sync pixel is not optional and must echo `HMD_SYNC`.
- The window must be borderless and fill the display.
- Normalise every quaternion on ingest; freeze on the last good pose.
- Never commit XOPS's own data files or binaries.
- Diagnostics behind one verbose flag, off by default.
- Mark every claim with its evidence. An unmarked claim reads as device-verified.
