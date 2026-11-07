# WORKLOG - OpenCode / DeepSeek V4.1 Flash

## 2026-11-05 - first pass

### Read

`Docs/MetaAI/` (all four docs), `Docs/ClaudeCode/` (all five), then
`Docs/general_wxr/` in its recommended order (`README`, `CLAUDE`,
`FIRST_STEPS`, `PROJECT_PLAN`, `XRAPI_V0_4_DESC`, `TIPS`, `TOOLS`). Also read
`src/d3d8probe.cpp`, `src/xrapi.cpp/.h`, `build/build-probe.bat`, the deployed
log and `xops_wxr_steer.ini`.

### State found

- Probe builds with VS2022 x86, no DirectX SDK, no MinHook.
- Camera solved `[device, runs 8-13]`; addresses in `README.md` section 1.
- XrApi client has only ever seen the UDP Tester, never a real Quest 3 packet.
- Last deployed build was `built Nov  4 2026 11:44:17`; 374 frames; steering
  off; tester sent a zero head position.
- **Saved config conflict:** `xops_wxr_steer.ini` had `pitchSign 1`, while the
  code default and `CURRENT_STATUS.md` said `-1` was settled. Risk R-9.
  **Resolved 5 Nov:** the owner confirmed the pitch direction is correct with
  `+1` on the fullscreen build. The INI is right; the old `-1` note is
  superseded.
- The last log shows five WORLD `SetTransform` sites, not the three previously
  recorded: `0x00414273`, `0x004021B8`, `0x004143A3`, plus `0x0040127A` and
  `0x00406529`.

### Changed

`src/d3d8probe.cpp` only.

1. **Borderless full-display window** (stage 3).
   - New `FitWindow()`: strips caption/thick frame/border/dialog frame, adds
     `WS_POPUP`, sizes to the screen at `(0,0)`.
   - Called after `CreateDevice`, after `Reset`, and idempotently each
     `Present`.
   - `F` toggles; default ON.
2. **Red sync pixel** (stage 5).
   - New `DrawSyncPixel()` called last in `hkEndScene`, before the game's
     `EndScene`.
   - `Clear` on vtable slot 36 with a 10x10 `D3DRECT`, `XRGB(round(HMD_SYNC),
     0, 0)`.
   - `o_Clear` captured in `HookDevice` so it survives a `Reset`.
   - No packet -> no square, because there is nothing to echo.
3. **Real FOV** (NEXT_STEPS item 4).
   - Outbound `XrSendFrame` now sends `80.7 x 65.0` degrees, not `104.5`.
   - Constants `XOPS_FOV_X` / `XOPS_FOV_Y` with the device measurement in the
     comment.
4. File header and startup log updated so the probe no longer claims to be
   read-only.
5. **Attempted:** pass the full client rect as `Present`'s destination rect, to
   stretch the 640x480 back buffer into the 1920x1080 window. It did **not**
   fix the windowed case (still 640x480 in the top-left). The route that works
   is the game's own fullscreen config.

### Verified

- `build\build-probe.bat` compiles both translation units cleanly on VS2022
  x86, Release, `/MT`.
- Deployed `out\probe\d3d8.dll` (214,016 bytes, stamp
  `built Nov  5 2026 09:18:08`) to `E:\tmp\Q3Mods\xops096`; SHA-256
  `24a2bacbfc881694c84fae05a876997751557559a67aae4d130cb20e4abb31b0`,
  `match=True`.
- Desktop test run on this exact build; results below. Still **not**
  Quest-3 device-verified.

### Desktop verification, 5 Nov 2026

Feeder: a Python UDP sender matching `XrAPITestUDP/Form1.BuildDataPacket`
exactly - `client0`, 29 numbers (28 floats + frame id), a 21-char T/F run and
`META` - plus a listener on 7278.

- **Fullscreen path**: after setting `config.dat` byte `0x13` to `01` (the
  windowed copy is `config - windowed.dat`), `CreateDevice` reported
  `windowed 0`, 640x480, and the display mode became 640x480. Window rect
  `0,0 640x480`, no chrome. The whole 4:3 frame is presented; the GPU/display
  scales it. This is the working path.
- **Windowed path (contrast)**: `windowed 1`, a 1920x1080 borderless window
  showed the 640x480 image 1:1 in the top-left and black elsewhere. The
  `Present` dest-rect experiment did not change that.
- **Sync pixel exact, fixed 120**: screen `(0,0)` = `R=120 G=0 B=0` by screen
  capture, `PrintWindow` and `GetPixel`; log `SYNC first red square drawn:
  RGB(120,0,0)`.
- **Sync pixel exact, fixed 36**: screen `(0,0)` = `R=36 G=0 B=0`. Two
  different values, so it is echoing the stream, not a constant.
- **In gameplay** (`building.ext`): log shows `0 SetTransform`, `84` indexed
  draws, live camera fields, and the owner saw the red square. Sync survives
  gameplay.
- **Outbound FOV**: captured `0.0 0.0 2 0 80.7 65.0` on 7278. Mode 2 because
  steering was off; the FOV pair is the real projection, not the 104.5
  default.

Caveats: the sync pixel was verified on the desktop with synthetic packets, not
on a Quest 3 through WinlatorXR. The windowed `Present` dest-rect code remains
in place but is unproven; it may be dead weight if fullscreen is always used.

### Crash diagnosis, 5 Nov - the camera scanner, not the mod

Three APPCRASH events appeared in the Application log:

| Time | Faulting module | Exception / offset |
|---|---|---|
| 09:17:30 | `d3d8.dll` (ours) | `0xc0000005` at RVA `0x985e` |
| 09:20:35 | `d3d8.dll` (ours) | `0xc0000005` at RVA `0x985e` |
| 09:27:29 | `d3d8.dll` (ours) | `0xc0000005` at RVA `0x985e` |

A `/MAP` build put `0x985e` inside the statically linked CRT `memcpy`, on the
`rep movs` path. The only bulk copy in this DLL is
`BuildSnapshot()` in the inherited camera-hunt scanner:

```c
memcpy(g_snap + g_snapUsed, mbi.BaseAddress, mbi.RegionSize);
```

`VirtualQuery` and the copy are not atomic. The game can free or reprotect a
region between them, and the raw copy then faults. The log's last lines before
the 09:20 crash are `CAMHUNT snapshot` lines, and the 09:27 run died at frame
540 - a multiple of `SCAN_INTERVAL` (30), which is exactly when the scanner
runs.

**Fix:**
- `g_huntDone` now defaults to `TRUE`. The camera is already confirmed
  `[device, runs 8-13]`, so the differential scanner is dead weight; the
  confirmed watch table is independent of it.
- `BuildSnapshot()` uses `ReadProcessMemory` instead of `memcpy`, and skips a
  region it cannot read. Even an explicit re-enable cannot take the process
  down this way.
- The startup log now says the scanner is off and why.

Build `built Nov  5 2026 09:29:49`, SHA-256
`1b0e6ab73215df4f07c23e16ccb6a2bbbbc9af8ff62793e9dd6bc53d24c9bc77`,
deployed with `match=True`. The title screen then survived 35 s with no new
WER event, where the 09:27 build died after ~9 s.

### Reset failure, 5 Nov - the fullscreen window fight

The next title-to-level transition produced XOPS's own "Reset" error dialog
(garbled Japanese, as the bitmap font has no UI encoding for it), and OK
terminated the game. The log is unambiguous:

```
WINDOW  fit-to-display #2: style 0xB4CA0000 -> 0xB40A0000, 160x28 -> 1920x1080
RESET  requested 640x480  windowed=0  fmt=22  flags=0x00000000  (frame 1726)
RESET  hr=0x88760868
```

`0x88760868` is `D3DERR_DEVICELOST`. In **fullscreen** the game owns the
window and the display mode; our per-frame `FitWindow` resized a 160x28
transition window to 1920x1080, and the following Reset could not set the mode.
The `Present` destination-rect override was also being applied in fullscreen.

**Fix:**
- `g_deviceWindowed` is set from `CreateDevice` and every `Reset`.
- `FitWindow()` returns immediately when the device is fullscreen.
- The `Present` destination-rect override only applies when windowed.
- Build `built Nov  5 2026 09:32:22`, SHA-256
  `b08f00e035b7f1e52ef6c8275bb15bcc99a08d85d60143ec6d00a1fa22d9dc71`,
  deployed `match=True`.

**Verified:** a focused synthetic left-click on the title screen now takes the
game into a level with **no `RESET` line**, no error dialog and no crash; the
screenshot shows normal gameplay. (Focus matters: the first click test failed
because the game was not foreground. `SetWindowPos(HWND_TOPMOST)` +
`SetForegroundWindow` then `mouse_event` at `(320,440)` works.)

### Max-resolution experiment, 5 Nov - negative result

Question: can we make the game create a **1920x1080** fullscreen device instead
of 640x480 and get a bigger image?

Test: `ApplyMaxRes()` overrides `BackBufferWidth/Height` in `CreateDevice` and
`Reset` to the desktop mode captured at init, before the game switches to
640x480. Reached via the `M` key for a future re-test.

Result: the device, window and display mode all became **1920x1080**, and the
game stayed alive - but the screenshot shows the world and HUD still occupying
a **640x480 area at the top-left**, with the rest of the 1920x1080 frame black.
The software renderer's projection and 2D layer really are hardcoded at
640x480. This confirms risk **R-16** and matches the earlier windowed result.

**Reverted:** `g_maxRes` defaults to `FALSE`; the code remains behind `M` for a
future experiment. Build `built Nov  5 2026 09:41:37`, SHA-256
`fbe2354aa318c528783a1256f90cc1c472d2cab66822953e22e5c11806bc3f66`.

**Consequence:** the working path stays the game's own **fullscreen 640x480**
mode, scaled by the GPU/display and by WinlatorXR. A native higher resolution
would require patching the engine's own screen-size and projection constants
(and scaling the HUD and mouse mapping), not a D3D back-buffer override.

### Click recording and replay, 5 Nov

The owner demonstrated the title-to-level clicks. The usable sequence is
**three left clicks at (330, 242)** in the native 640x480 frame:

```
0.00s down  0.08s up      title / begin
0.56s down  0.64s up      mission selection
3.00s down  3.08s up      mission briefing
```

The first two clicks are a real recording. The owner then reported that the
briefing needed one more click; the third was added at the same coordinate and
the complete sequence was tested end-to-end (see the verification below). A
second recording attempt captured zero events because of the hwnd bug described
next, so the third click is inferred, not independently captured.

Saved as `tools/title-clicks-640x480.jsonl`; `tools/start-level.ps1` finds the
game window and replays it. `tools/replay_clicks.py` is the generic replayer
(`--scale` for a larger frame); `tools/record_clicks.py` records new sequences.

Two mistakes worth keeping:

- **The first recorder filtered on `--hwnd` and captured 0 events.** XOPS
  changes its window handle during the title-to-level transition, so the
  foreground window never matched the original handle. `record_clicks.py` now
  filters by **process id**; `--hwnd` derives the pid.
- **A `ShowWindow` IAT hook (build 09:46) was a regression.** It blocked the
  game's own minimize/hide calls, but it changed startup state so the game
  launched with no usable main window and its frame-1 Reset failed with
  `D3DERR_DEVICELOST`. Reverted in 09:50.

**Reset recovery now:** `hkReset` restores a minimized/hidden game window
before calling the real Reset, and if the result is still
`D3DERR_DEVICELOST` it restores the window and retries the Reset once. This is
a narrower fix than blocking ShowWindow and does not change startup.

**Verified:** a fresh launch plus `tools/start-level.ps1` reached `building.ext`
with no `RESET` line and no crash; frame 600 showed `93` UP / `8` /
`84` indexed draws, and the screenshot is normal gameplay. Build
`built Nov  5 2026 09:50:24`, SHA-256
`08f2be40f3188ef480de3ca5d85e62881d9bfb94f58b2f3ee252a874996bc878`.
The coordinates are in the game's native 640x480 frame. A larger back buffer
still draws the game in a 640x480 area at the top-left, so keep the same
coordinates there; `--scale` is only for an actually stretched image.

### Full-window output, input remap and HUD, 5 Nov

The game's final `Present` passes an **explicit 640x480 destination rect**
(`0x403AFE`/`0x403B0E` builds it), not NULL. Overriding that rect with the
client rect makes D3D8 stretch the 640x480 software output to the window. The
1920x1080 windowed test logs `PRESENT dest override: 640x480 -> 1920x1080` and
fills the frame.

Because the game still reasons in 640x480, input must be mapped back:

- `ScreenToClient` divides by `clientW/640`, `clientH/480`.
- `ClientToScreen` multiplies by the same factors.
- Without both, a stretched menu is unclickable: the physical click lands at
  `(990,544)` but the game sees 1920-space coordinates instead of `(330,242)`.
- `tools/start-level.ps1` now computes the click scale from the client rect
  automatically (3.0 x 2.25 at 1920x1080), and the click sequence enters a
  level.

**HUD transform.** A draw dump (`V`, also auto-armed at frame 600) identified
the HUD: `DrawPrimitiveUP`, FVF `0x0144`, stride 28, `prim=6` (triangle fan),
`count=2`, four vertices, with coordinates around `x 47..303, y 375..489`.
The first attempt tore the scene apart because `DrawPrimitiveUP`'s `count` is
the **primitive** count, not the vertex count: copying `count * stride` left
half the fan uninitialized. `PrimVertexCount()` now expands the primitive
count (fan/strip = +2, list = x3, etc.) before copying. With that fixed,
`hudVisualScale 1.1` puts the HUD in the middle third and leaves the world and
weapon model untouched. The game-space scale is derived from the visual scale
and the Present stretch (`0.367 x 0.489` at 1920x1080), so the HUD is not
squeezed by the non-uniform output. It is gated to gameplay, so the title,
briefing and menus are not transformed. `H` cycles
1.00/1.10/1.25/1.50/1.75/0.90; `V` dumps the next 120 draws.

**Font clarity.** Vertex scaling does not add resolution: the HUD glyphs are
still rasterized at the original 640x480 font size, so enlarging the quads only
magnifies them. Crisper text needs the 2D text path, not the world renderer:
hook the font creation to rasterize a larger HUD font (or draw the HUD text at
the target resolution). Native software resolution would sharpen the world but
only helps the font if the font is regenerated larger.

**MODE_VR.** `MODE_VR = 1` only when the previous frame had the gameplay draw
signature; otherwise `0`. Steering and stick movement are gated to gameplay.
The title/briefing now send 0; the death screen has not been observed yet.

**Config.** The test install is currently **windowed** (`config.dat` byte
`0x13 = 00`) so the 640x480 software frame is stretched to the 1920x1080
window. The fullscreen config is preserved as `config - fullscreen.dat`.

**Still not native.** The software renderer remains 640x480; the output is
stretched. True native resolution means patching the internal framebuffer and
projection constants, not the D3D back buffer. The Present path, input remap
and HUD transform are independent of that and survive a later renderer patch.

Build `built Nov  5 2026 10:09:52`, SHA-256
`bb00fc7704839eb5fe2876729cdbebdbe0de10b49caad73e21d1f03dcb362182`.

### Real game-state detector - VERIFIED, 5 Nov

The draw-count heuristic is replaced by the game's own state, found by
following the EXE's `mission successful` / `mission failure` strings:

| Global | Meaning | Values seen |
|---|---|---|
| `0x490A44` | selected mission index | `64` (0x40) title; `2` BUILDING_EXT; `6` DUEL_EXT; `-1` none |
| `0x490A48` | mission result | `0` playing/briefing; `1` victory; `2` death/failure |
| `0x48DDD8` | local player index | `-1` title/menu/briefing; `0..0x17` while a player exists |

`InGameplay()` now returns false for `A44 == 0x40`, `A48 != 0`, `A44 < 0`,
or `DDD8 < 0`; otherwise it requires `indexed >= 30`. That threshold clears
the in-mission menu (~28) while accepting the smallest real level seen
(DUEL_EXT, 39-53). The old 60 threshold would have dropped head tracking in
DUEL_EXT.

Verified with two deaths and one victory on the desktop:

```
title          A44=64 A48=0 DDD8=-1             -> mode 0
BUILDING_EXT   A44=2  A48=0 DDD8=6   84 indexed  -> mode 1
death 1        A44=2  A48=2 DDD8=6   98 indexed  -> mode 0
DUEL_EXT       A44=6  A48=0 DDD8=0   39 indexed  -> mode 1
death 2        A44=6  A48=2 DDD8=0   44 indexed  -> mode 0
respawn        A44=6  A48=0 DDD8=0   39 indexed  -> mode 1
victory        A44=6  A48=1 DDD8=0   48 indexed  -> mode 0
```

Latest build `built Nov  5 2026 10:34:33`, SHA-256
`d32318d2409bbfadb3817e69411e0a004656f7cfbede2b69c15ebc03826815cd`.
The `F` key was pressed accidentally during this run; it toggles the
window-fit dev switch and is not persisted, so it does not affect the state
evidence.

### Font-scaling experiment - NEGATIVE, 5 Nov

The game creates exactly one font through `CreateFontIndirectA`:

```
h=18  w=0  weight=500  italic=0  charset=128 (Shift-JIS)  face=<garbled Japanese>
```

It is created before `EnsureInit`, so the hook is installed in `DllMain` (an
IAT write, not `LoadLibrary`). Scaling it to 1.25Ã— (`h 18 -> 22`) made the text
**worse**: the HUD uses a bitmap glyph atlas drawn as many tiny quads, so the
larger font changes the atlas and layout rather than adding clean detail.
Reverted to 1.0.

The GDI `DrawTextA/W` capture confirmed the HUD text is **not** per-frame GDI
strings; the repeated calls are the font atlas/menu text. A crisp HUD therefore
means replacing the glyph quads, not scaling the game font: read the player
values, suppress the default HUD text, and draw a new compact HUD at the target
resolution inside the single XrApi image.

Also confirmed: the 3D gun at the bottom-right is the **viewmodel** (world
geometry), not part of the 2D HUD. Moving it is viewmodel decoupling, a
separate feature.

Current build (font 1.0, capture-only hooks): `built Nov  5 2026 10:51:28`,
SHA-256 `eaf00e8a7543fdd8122091b25b72d9b0c5da1b99b253b42fa7dbee0201f955a3`.

### VR controller mapping - implemented, 5 Nov

Owner's mapping, right-handed:

| VR control | Game action | Key |
|---|---|---|
| left stick | move | W/A/S/D |
| right stick L/R | turn | yaw offset (120 deg/s) |
| left grip | walk | TAB (held) |
| left trigger | fire mode | X (edge) |
| right trigger | shoot | mouse left (held) |
| right grip | jump | SPACE (edge) |
| B | reload | R (edge) |
| A | switch weapon | Z (edge) |
| X button | drop weapon | G (edge) |
| Y | zoom toggle | SHIFT (edge) |

**Left-handed:** only shoot and fire mode swap - left trigger = shoot, right
trigger = fire mode; every other control is unchanged. `N` toggles it and
`leftHanded` is saved in `xops_wxr_steer.ini`.

**Held vs edge:** held keys are walk, shoot and movement; fire mode, jump,
reload, switch, drop and **zoom** are edge-pulsed for three frames, because the
game polls `GetKeyboardState` once per frame and a one-frame edge can be missed.
Zoom is a **toggle** in XOPS, so Y sends a press edge rather than holding
SHIFT; holding it would be the wrong model.

**Turning:** right stick accumulates a yaw offset added to the head yaw, so
body turn and head look compose instead of overwriting each other. `turnSpeed`
in the INI (default 120 deg/s); a snap-turn option is a later setting.

**Zoom:** Y edge-toggles SHIFT; the head still drives the camera, so the zoomed
view is head-tracked for free. The Halo-style floating scope is not built.

Steering is **ON by default**; `3` remains a debug toggle. Input defaults to
**VR-only** in gameplay: desktop keyboard/mouse are suppressed there. `0`
enables desktop keyboard/mouse for manual tests without a headset or fake
XrApi data, and pauses synthetic VR steering/input. **MODE_VR 0 screens
(title/objective/menu/death/victory) always keep desktop input** regardless
of that toggle; only gameplay is VR-exclusive. ESC remains available.

Build `built Nov  5 2026 11:14:24`, SHA-256
`8ef97ce83557d21201d4a522486b1e2247754c25bb046ccf122587e4a3b019cf`.

### Deliberately not done

- Did not touch `xops_wxr_steer.ini`.
- Did not guess a menu flag or add an integer scan yet.
- Did not refactor the probe into the mod - the file is still one unit by
  design until the camera/aim work stabilises.
- Did not implement roll, height, or aim.

### Next session

1. Resolve the `pitchSign 1` INI (the fullscreen gameplay run logged
   `signs: yaw +1 pitch +1`), then re-run steering.
2. Gate `FitWindow` on `windowed=0` so it does not touch a fullscreen device's
   window. Re-run the fullscreen test after that change.
3. Decide whether to keep or remove the `Present` dest-rect experiment; it did
   not fix the windowed case.
4. Stage 1 on the Quest 3: the unmodified game and this DLL in a WinlatorXR
   container, with `config.dat` fullscreen. This is the first real device
   verification.
5. Menu/playing flag, eye height, roll, F-7 aim - unchanged from
   `Docs/ClaudeCode/NEXT_STEPS.md`.

## 2026-11-05 - target-resolution composite + replacement HUD

### Goal

Replace the game's low-resolution 2D HUD with a readable, target-resolution
HUD inside the centre square of a 3x3 screen grid; keep the game's 3D
weapon-slot model draws; remove the crosshair. The HUD must still be one image
for XrApi (no OpenVR/OpenXR overlay layers).

### Negative attempt first

The earlier GDI prototype drew text over the game window after `Present`. It
did not appear in the captured window (the capture came back black), so it was
left disabled. The conclusion stands: the HUD must be inside the D3D frame.

### What is deployed

`g_maxRes = TRUE` by default. `CreateDevice`/`Reset` ask for the desktop
(`GetSystemMetrics`) back-buffer size, but the game no longer draws into its
corner. It renders into a dedicated 640x480 render target:

1. `EnsureCompositeResources` creates a 640x480
   `D3DUSAGE_RENDERTARGET` texture plus a D16 depth surface and binds them.
   The target stays bound outside `Present`, so title/demo draws that happen
   between scenes also land in it.
2. `hkSetViewport` maps any viewport larger than 640x480 back into the RT;
   `hkSetRenderTarget` keeps the game on that target.
3. `hkBeginScene` does not rebind or clear, so a pre-`BeginScene` title
   background survives; `hkEndScene` leaves the game target bound.
4. `Present` binds the real back buffer, draws the 640x480 texture at its
   native **4:3** aspect, then the GDI HUD/panel textures and the red sync
   square, presents, and rebinds the 640x480 target for the next frame.

The persistent binding fixed the title/demo regression: the intro scene is
drawn outside the game's `BeginScene`, so per-scene binding missed it and left
an empty background with cursor trails. The RT is also the FOV/model fix: with
the game drawing against the 1920x1080 back buffer, its FOV changed and the
indexed 3D HUD models went off-screen.

**Default:** native 4:3 presentation, default projection/FOV (`80.7 x 65.0`).
On the supported 4:3 container (1280x960) the rectangle fills the window
exactly - no bars, no crop, no stretch. On a non-4:3 container it letterboxes
as a fallback. **Opt-in experiment:** `patchAspect 1` patches the hardcoded
projection aspect (`00403A91  68 AB AA AA 3F  push 1.333333`, immediate at IDA
`0x403A92`) and fills the whole window anamorphically. Default is off.

The old `DrawSyncPixel` in `hkEndScene` is skipped while the composite is
active; otherwise it keeps the old path. Default-pool textures and the render
target are released before `Reset` and recreated afterwards.
`CreateDevice`/`Reset` retry at the original 640x480 if the target-size request
fails, and the composite then stays off as the fallback.

### D3D8 vtable note

The extra `GetInfo` method at index 65 in Wine's `d3d8.h` is why the existing
device indices above 64 are one higher than a naive reading. The composite
uses: `GetBackBuffer` 16, `CreateTexture` 20, `CreateImageSurface` 27,
`CopyRects` 28, `SetViewport` 40, `GetViewport` 41, `SetRenderState` 50,
`GetRenderState` 51, `SetTexture` 61, `Get/SetTextureStageState` 62/63,
`DrawPrimitiveUP` 72. Texture `GetSurfaceLevel` 15 / `LockRect` 16 /
`UnlockRect` 17; surface `LockRect` 9 / `UnlockRect` 10.

### HUD values and drawing

`ReadHudVals()` maps the active player struct at `0x48DF00 + index*0x50`:
health word `+0x04`, selected slot `+0x06`, weapon IDs `+0x0A/+0x0B`, current
ammo `+0x0C/+0x0D`, reserve `+0x0E/+0x0F`. Names come from the pointer table
at `0x465DA0`; id 0 is rendered as white `None`. The field order was corrected
on 5 Nov after the owner noticed the first HUD showed reserve on the left:
the game's own box reads `30 | 60` for a loaded MP5SD, matching `+0D=30`,
`+0F=60`.

GDI renders into a top-down 32bpp DIB (Consolas/Arial bold, antialiased), then
the upload derives the alpha byte from the pixel's brightest channel, scaled
1.2x, so antialiased text composites cleanly. The DIB is only redrawn when the
player values or the target size change.

**Panels.** A second `A8R8G8B8` texture holds two rounded rectangles behind the
text. `RenderPanelsToDib` draws with blue-channel layer codes (30 = shadow,
70 = fill, 140 = border); `HudUploadPanels` maps those to independent RGBA
values: shadow black alpha 50, fill RGB 220 alpha 90, border RGB 255 alpha 160.
The text texture is separate, and each text run gets an 8-way blue silhouette
that `HudUpload` converts to opaque black, giving a dark outline on the light
panels and on bright walls. The panel rectangles are measured from the actual
text widths and clamped to the HUD region.

Layout inside the centre square:
- left: current ammo (large), remaining ammo (small, after a `/`), then
  `STATE <word>` in the state colour;
- right: selected weapon name (large), other slot name below (small);
- state colours: FINE green, CAUTION lerps green -> yellow as health falls
  from `0x50` to `0x28`, DANGER orange, DEAD red. The game has no `CRITICAL`
  string in this build.

The game's own 2D HUD (stride-28 `DrawPrimitiveUP`) is suppressed while
`g_compositeActive && g_hudValid && HudShouldDraw()`. `HudShouldDraw()` is
`InGameplay()` **or** `PlayerDead()`, where the latter reads mission result
`0x490A48 == 2`. On death `HudVals.dead` forces the STATE readout to `DEAD`
(red), so the final custom-HUD frames never say `FINE` while the game switches
to its death screen. If the HUD texture fails, the game's HUD stays visible.
Indexed draws are untouched, so the 3D slot/model draws remain in the frame.
The four crosshair line draws at `(317..323, 237..243)` are still removed.

**Reload / change status.** The byte just before the player struct
(`base - 1`, e.g. `0x48DEFF` for index 0) is the game's action state:
`3 = RELOADING`, `5 = CHANGING` (the HUD code pushes `4655A4h` / `465598h`
at `0x405346` / `0x405392`). `HudVals.reload` carries that byte; the HUD draws
the word centred above the panels in a larger status font with the same dark
outline. **Font size:** big = HUD region height `/ 10`, status = big `* 7/5`
(slightly smaller for VR).

### Virtual hands + laser

The first pass drew the hand/laser as a screen-space overlay. That was wrong:
it could not intersect or be occluded by the map. A second pass tried
per-vertex depth on XYZRHW, but the world depth still did not match.

The decisive finding: **the world vertices are XYZ + TEX1 (stride 20), and
every object is placed by its own world matrix.** The dump showed `x,y,z,u,v`
with small object-space z values, and the device still had a model matrix
bound at the HUD-model draw. The hand pass must therefore reset **world as
well as view**:

- `DrawHandAndLaserWorld` is called from the first 3D HUD weapon-model draw,
  while the 640x480 target and D16 depth are bound, so it is in the gameplay
  layer under the HUD rather than on top of it. It saves the
  world/view/projection, sets **world = view = identity** and the captured
  game projection, and draws `XYZ | DIFFUSE | TEX1` (FVF 0x142, 24-byte
  vertices).
- **Both hands** are solid 0.08 x 0.14 x 0.14 m 3D boxes at their own XrApi
  poses, with six textured faces and a generated grey/black glove texture.
  The dominant hand applies `hand_flip`; the off-hand does not.
- **Laser:** one continuous camera-facing triangle strip with a shared right
  vector, 30 m long; width is scaled with distance so it reads about 2 px. The
  depth test is real D3D depth (`LESSEQUAL`), Z-write off for the beam, on for
  the hands. A wall cuts the beam off.
- Y zoom hides the laser (hands stay).
- If the tester sends no hand position, a fallback view-space offset is used.

**Config:** `config.txt` one directory above the game folder (created if
missing):

```
handedness = right   # or left
hand_flip = false    # true = 180-degree roll of the dominant hand/gun only
```

`handedness` sets the dominant hand (laser + initial trigger swap); `N` toggles
both in-session. `hand_flip` negates the dominant hand's up and right vectors
about forward, i.e. roll only.

**The bug that hid the hands.** The first 3D-pass version reset only the view
and projection. The leftover **world** matrix from the last model draw then
transformed the hand vertices out of the depth range, so the pass drew and was
then invisible. A full-screen magenta test quad with depth off also vanished,
which proved the pass itself was fine and the transform was wrong. Setting
`D3DTS_WORLD` to identity made both hands and the laser appear. The world
matrix should have been the *first* suspect: the HUD-model `WORLDZ` dump shows
object-space coordinates (`z` around -0.1..0.1), which only make sense with a
model matrix.

The game also clears after the hand pass (`flags=0x2` depth only, twice per
frame; the `flags=0x3` colour clear is the **next** frame's start-of-scene
clear and happens before `BeginScene`). Those depth clears are harmless; the
hands survive to `Present`.

**Verification:** a mock feeder drove both hands with independent X/Y/Z
sinewaves (and small yaw/pitch). The log showed both raw positions changing on
all three axes, e.g.:

```
HAND raw L -0.392 -0.060  0.551   R  0.419 -0.086  0.512
HAND raw L -0.339 -0.112  0.337   R  0.171 -0.105  0.313
HAND raw L -0.120 -0.193  0.432   R  0.190 -0.183  0.537
```

The one-time matrix log confirmed `HAND view` is identity and `HAND proj` is
`1.17726 / 1.56969 / 1.00125 / -0.10013`. In the wall-facing test frame the
laser stopped at the wall instead of crossing it, and the hand boxes were
partially hidden by it, confirming real depth intersection.

### Background cursor (6 Nov)

XOPS recentres the pointer with `SetCursorPos` every frame and reads it back
to compute its look delta. With "run while unfocused" on, it did that even
when another window had focus, so it dragged the owner's desktop cursor into
the game window and made the machine unusable while the game kept running.

`hkSetCursorPos` now checks the **real** foreground window (the unhooked
`GetForegroundWindow`). If it is not the game window, it records the requested
position for the fake look delta and returns success **without** calling the
real API. The desktop cursor therefore stays where the player left it, the
game still sees a zero delta (because `hkGetCursorPos` returns exactly what
was "set"), and refocusing does not snap the view.

### Verification

Desktop run, `building.ext`, 1920x1080 windowed desktop (the device container
will be 1280x960, where the 4:3 rectangle fills exactly):

```
PROJECTION aspect patch disabled by xops_wxr_steer.ini
RESOLUTION target-res override: 640x480 -> 1920x1080 (windowed)
COMPOSITE active: back buffer 1920x1080, game render target 640x480 fmt=22
COMPOSITE native 4:3: 1920x1080 -> 1440x1080 at 240,0 (bars 480,0), default FOV
HUD      textures 1024x512 (text + panels, draw region 640x360)
HUDVALS  health=110 state=FINE selectedSlot=1 slots=22,14 current=12,30 reserve=24,60
MODE_VR -> 1 (in-game)
```

On the 1920x1080 desktop the native 4:3 frame is 1440x1080 with side bars, as
expected. On the device container at 1280x960 the same code produces
1280x960 at 0,0 with `bars 0,0` - no bars, no crop, no stretch, and the
game's default `80.7 x 65.0` FOV. The BMP showed `30 / 60`, green
`STATE FINE`, `MP5SD` and `MK23 SD` in the centre square, the two **rounded
semi-transparent panels** behind them, **and the two 3D weapon-slot models
visible at the bottom-right**, matching the native HUD.

Additional verification in this pass:

- **Title / intro demo:** BMPs at frames 156 and 826 show the 3D brick-wall
  intro and the character/menu background, not empty space. The persistent RT
  binding fixed this; no cursor trails in the captured frames.
- **Reloading:** a shot then `R` produced the game action byte 3 and the
  custom HUD showed `RELOADING` centred above the panels (frame 989).
- **Death:** forcing mission result `0x490A48 = 2` made the custom HUD show
  `STATE DEAD` in red and kept it on screen (frame 914); the game's own HUD did
  not take over with the old STATE text.
- **Font:** big = region height `/ 10`, status = `1.4x`; the panels shrink with
  the text.

`K` toggled the custom HUD off and back on, confirming the fallback path.
Window captures of the D3D surface are black, so the back-buffer BMP is the
reliable evidence.

**Keys:** `J` = save the next composited frame as `xops_frame_<n>.bmp`; `K` =
toggle the custom HUD (in probe mode `K` still classifies). `G` is deliberately
not used: it is the game's native drop-weapon key, which the controller's X
button pulses.

### Build

`built Nov  6 2026 00:04:41`, SHA-256
`ef596da3b33c4928e68f5a7b0da9d6903be4cb08d0df54607372301565932c2d`,
deployed to `E:\tmp\Q3Mods\xops096` (`match=True`).

### Open / next

- **3D slot model placement.** The models are visible again in their native
  slots. They are not moved into the centre square; the replacement text is
  placed independently. Decide whether to move them with the HUD or leave the
  game's own slots.
- **Death screen UI.** The custom HUD stays up and suppresses the game's 2D
  HUD while dead. Check on device that the game's death/respawn UI is not
  hidden; if it is, narrow the suppression on the death screen.
- The composite keeps the 640x480 RT bound outside Present and binds the back
  buffer for the final quad. It is smooth on the desktop; measure on the Quest.

## 2026-11-06 - haptics, two-handed aim, input boundary, verbose logs

### Haptics (implemented, desktop-verified)

Protocol fact from `XRAPI_V0_4_DESC.md` section 4: outbound 7278 fields 1-2 are
a vibration **duration in frames**, per hand, not an amplitude, and the runtime
only stores values greater than zero. So the value is decayed locally and
re-sent each frame; sending 0 does not stop a buzz.

- `HapticTick()` watches both magazine counters (`+0C/+0D`) each frame, not
  only when the HUD redraws. A drop of 1..8 rounds is a shot; a bigger drop is
  a reload/pickup/respawn and is ignored.
- One pulse per shot, `2` frames by default, **decayed before it is sent** -
  the SHOGO MAD WXR order (`VRStereo.cpp` line ~2403). The runtime sees `1`
  then `0`, a short kick. Automatic fire feels stronger because pulses arrive
  faster, not because each is longer.
- Dominant hand by default; **both hands while the left grip is held**.
- Gameplay only; counters reset on menus/death so a respawn is silent.

Config (`config.txt`, one directory above the game):
`haptics`, `haptics_hand` (dominant|both|left|right), `haptic_frames` (1..5).

Evidence, desktop `building.ext`, synthetic tester:

```
HAPTIC fires=1 dur=2 frames hand=right slot 1 weapon 14 ammo 12/29 (shots 1)
...
capture: 0.0 4.0 1 0 80.7 65.0 -> 3,2,1,0   (4-frame build)
capture: 0.0 1.0 1 0 80.7 65.0 -> 0          (2-frame build, Shogo order)
grip held: HAPTIC ... hand=both ... capture 1.0 1.0 1 0 80.7 65.0
```

### Two-handed aim (implemented, geometry verified)

- The `two_handed` config key was removed. Two-handed aim is **held on the
  left grip**, off otherwise.
- `HandTwoHandedAim()` mirrors SHOGO's `UpdateTwoHanded`: aimF =
  normalize(offP - domP); aimU = normalize(domU - aimF*dot(aimF,domU));
  aimR = up x forward. Position stays at the dominant hand, roll is kept.
- Used by the draw pass (gun, hand box, laser) **and** `SteerAimWrite`, so the
  barrel, beam and shot agree.
- Left grip no longer sends `TAB`; walk is gone.

Evidence with a synthetic pose feeder (right hand 0.250,-0.250,0.350; left
0.100,-0.100,0.750):

```
HAND two-handed aim ON: dom 0.250 -0.250 0.350 off 0.100 -0.100 0.750
     aim -0.331 +0.331 +0.883
HAND two-handed aim off
```

`normalize(off-dom) = normalize(-0.15, 0.15, 0.4) = (-0.331, 0.331, 0.883)`,
exact.

### Input boundary (implemented, negative-tested)

- `GameWindowFocused()` asks the **real** (unhooked) `GetForegroundWindow`,
  because the hook lies to keep the game running unfocused.
- Unfocused: `GetKeyboardState` is zeroed, `GetAsyncKeyState`/`GetKeyState`
  return 0, the cursor delta is forced to zero, and all synthetic stick/VR/
  debug input is skipped. Edge tables are synced so a held key/button is not a
  new press when focus returns.
- VR mode (`g_vrExclusive`): physical `A B X Y` are always swallowed; the four
  arrows are swallowed while `MODE_VR = 1`. Manual PC mode (`0`) allows them.

Evidence: with Notepad focused, a synthetic `0` produced **no** `HAND user
control ON` line. With the game focused the same key toggled it, and the
subsequent fire test worked.

### Verbose logging (implemented)

- New `LogD()` (verbose only) and `g_verboseDebug`, loaded from
  `verbose_debug` in `config.txt` (default FALSE). 219 informational call
  sites moved to `LogD`; errors, config and game-state changes stayed on
  `Log`. The frame dump also requires `verbose_debug`.
- `HandLoadConfig()` now runs before the diagnostic startup block so the flag
  is known before anything is written.
- The test install (`E:\tmp\Q3Mods\config.txt`) is set to
  `verbose_debug = true`; a shipped install defaults to false.

### Controls (owner, 6 Nov)

Left stick move; right stick turn; **left grip two-handed aim**; left trigger
fire mode; right trigger shoot; right grip jump; B reload; A switch; X drop;
Y zoom. Left-handed swaps only shoot/fire-mode.

### Build

`built Nov  6 2026 04:27:08`, SHA-256
`cdad87a2f59813e1a9ebcb00a3ccedea79e48359acb1439e896a68df0ddc19b5`,
deployed to `E:\tmp\Q3Mods\xops096` (`match=True`).

### Deliberately not done

- No true stereo yet (next experimental step).
- Launcher not started.
- Per-weapon offset persistence still in memory only.

## 2026-11-06 - experimental AER stereo (desktop-verified)

### The discovery that shaped it

During gameplay the log shows **0 SetTransform calls**: XOPS pre-transforms
the world on the CPU and submits it through the **non-indexed
`DrawPrimitive`** path with vertex buffers. Dumping a world draw shows
camera-space coordinates in **centimetres** (a room spans x -300..200,
y 0..206, z -21..178) and layout XYZ + DIFFUSE + TEX1, stride 24. The real
current FVF from `GetVertexShader` is `0x102`/`0x142`; the probe's
`g_curFvf` is stale at `0x144` because the game sets the drawing FVF through
a state block. The **indexed** path (about 84 draws) carries smaller
metre-scale geometry (actors / view models).

A view-matrix eye offset therefore does nothing to the world; the camera is
already baked into the vertices. Stereoscopic AER has to be done by
**reprojecting each draw** at submission time.

### Implemented - AER

- `stereo = mono | sbs | aer` in `config.txt`, default `mono`.
- `g_eyeRight` flips once per presented frame, **after** `Present`, so the
  image, the blue sync eye flag and the log label all agree. (Flipping it
  before the composite made the flag describe the opposite eye.)
- `AerDrawPrimitive`: the cm world path copies the draw into a scratch buffer
  and shifts x by `-d*100`; `AerDrawIndexed`: the m path shifts x by `-d`.
  Both re-submit with `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` so the
  game's shared buffers are never modified. `g_hudModelPass` leaves the 3D
  HUD slot models and everything after them screen-fixed.
- The virtual hands/laser get the same offset through their view matrix.
- Sync square: `RGB(HMD_SYNC, 0, eyeFlag)`, blue 255 = right. `MODE_3D=2`.
- IPD is packet float 25 (64 mm fallback).

### Verification - desktop

Test pose feeder (left hand forward of the right), `building.ext`:

```
DUMP  wrote ...\\xops_frame_592.bmp  eye=right
DUMP  wrote ...\\xops_frame_593.bmp  eye=left
full-frame MAD between the pair: 8.07   (mono consecutive frames: 0.033)
region shift at 1920: walls ~5-9 px, near floor ~12 px
```

An anaglyph of the pair shows the world doubled at the wall corner (depth
parallax) while the custom HUD panels are identical in both eyes. The gun
clone stays intact after `g_hudModelPass`; an earlier build without that flag
turned the game's slot model into black spikes.

`. ` (VK_OEM_PERIOD) now grabs **two consecutive** composited frames
(`xops_frame_<n>.bmp`), which is exactly an AER left/right pair.

### Not done - SBS

SBS needs two viewports per draw and a double-width render target/presented
image. The draw duplication is now well understood, but the container/window
has to be double width for each half to keep a 4:3 eye image, and that is not
tidy on the supported 1280x960 container. Deferred until the AER eye sign and
centimetre scale are confirmed on the device; the plan is in `NEXT_STEPS.md`.

### Build

`built Nov  6 2026 05:21`, SHA-256
`e2c96caef36c37a697a8cbd8d8f6b8121f0339acbd5fbdedf57f4a40f9752cf7`,
deployed to `E:\tmp\Q3Mods\xops096` (`match=True`).
Test config left at `stereo = mono`, `verbose_debug = true`.

### 2026-11-06 - correction: XrApi input while unfocused

The first focus-mute implementation muted **all** synthetic input when the game
window was not foreground. That broke the normal desktop test workflow: the
XR UDP Tester holds focus, so shoot / jump / reload / move stopped working
while turning still worked (turning is camera state written by `SteerTick`,
not an injected key).

Corrected semantics: unfocused mutes **physical desktop** input only
(`GetKeyboardState` is emptied, `GetAsyncKeyState`/`GetKeyState` return 0,
cursor delta forced to zero, `ProbeKeys` and `ManualHandKeys` skipped).
`StickKeys` and `VrInputKeys` still inject the runtime's sticks, triggers and
buttons into the emptied array.

Verified with the synthetic feeder and Notepad deliberately in the foreground:
the feeder's right trigger fired (`HAPTIC ... hand=both`), and a physical `0`
sent to Notepad did **not** log `HAND user control ON` (log count unchanged).
The earlier apparent leak was a failed `SetForegroundWindow`, not the probe.

## 2026-11-06 - hand_flip default inverted for Quest 3

Quest 3 hand data arrives 180 degrees rolled relative to the other headsets;
HWXR, Shogo WXR and IMVR all carry a fix for it. The old default
(`hand_flip = false`, no roll) therefore looked upside down on Quest 3.

The config meaning is now inverted:

- `hand_flip = false` (**default**) applies the 180-degree roll correction -
  the Quest 3 layout.
- `hand_flip = true` recreates the old, uncorrected gun roll for devices other
  than a Quest 3.

Internally `g_handFlip` now means "apply the correction" and defaults TRUE;
the config parse inverts the value. The startup line reports both:

```
HAND  config ...  hand_flip=false  (roll: Quest 3 corrected)
```

Only the dominant hand/gun is rolled (up and right negated about forward);
the off-hand is unchanged. The PC test's `config.txt` has `hand_flip = false`,
so the Quest 3 roll is what the desktop now previews (the synthetic tester
sends standard quaternions, so the PC image looks rolled; real Quest 3 data
is rolled the other way and the correction brings it upright).

Build `built Nov  6 2026 06:14`, SHA-256
`8271cf6f99da7273e788e3bc13592ff3099c8023b4f00a5a9f58eb4a6da5267d`.

### 2026-11-06 - finger and thumb slabs on the virtual hands

The bare palm box gave no readable roll/pitch. Following the
`Docs/general_wxr/SixDOFinatorCalibration` hand markings, the hand is now a
compact palm plate plus:

- four **short, chunky glove-textured finger slabs** with black outlines:
  half-extents 4.2 mm wide, 9.5 mm thick, 7.5-8.5 mm long, staggered and
  fanned slightly, all but touching the palm's leading edge;
- one **thick thumb slab** (5.5 x 9.5 x 13 mm) on the inner side (local
  -right on both hands, because both controller bases point +right outward),
  angled **outward from its own side** and forward. The first version angled
  it across the palm, which looked like a broken joint.

The palm is 60% of the earlier length (3.8 cm total) and 2.4 cm thick, so the
fingers and thumb are nearly as thick as the hand body. All parts share the
glove texture and black outlines. `HandDrawBoxBasis` is the shared six-face
box helper. Verified with the exact SixDOFinator `SamplePoseData.txt` packet
in a NONE/NONE level: both hands read clearly, the left thumb is on the inner
side, and the stubby fingers point forward. `.` grabs the frame pair.

Build `built Nov  6 2026 06:41`, SHA-256
`74931a59f64cc9a559c1e49d14261881eba296c010832f174402cd76f82a057a`.

Test hand placement is **data, not code**: to bring the hands forward for a
clear look, `SamplePoseData.txt` is copied and the two hand `z` values are
made 0.10 m more negative (OpenXR forward is -Z). The mod's fallback pose is
unchanged; editing it would change shipping behaviour for a test preference.
The scratch feeder for this is `%TEMP%\opencode\xr_feeder_forward.py`.

## 2026-11-06 - head roll via the presented 2D sprite

The world is CPU pre-transformed and submitted already projected, so rolling
the D3D projection did nothing to it: only the hand geometry (which is in head
space) moved. The fix is the owner's original idea - **rotate the presented
640x480 sprite** in `Present`, then counter-roll the hand/gun/laser pass.

- `HeadRoll()` derives the roll from the head basis (no-roll up = world up
  projected off the forward vector).
- `CompDrawQuadRot` draws the game texture as a rotated 2D quad around the
  centre. **No scaling**: black corners are accepted because the HMD lenses
  hide them.
- The hand/gun/laser pass gets a **view rotation** that cancels the sprite on
  screen. Camera space is y-up and the sprite is y-down, so the view angle is
  `+sprite`, not `-sprite`; the first attempt used the wrong sign and the
  hands rolled twice.
- The custom HUD is drawn after the sprite and stays fixed; the sync pixel is
  fixed at the top-left.
- `roll_sign` default is **-1** (the owner reported the original +1 direction
  as inverted). `/` toggles roll live for comparison.

Verified on the PC in `building.ext` with a static +25 degree head-roll pose
and the `/` toggle, same camera and hands:

- World sprite: clearly rolled by 25 degrees with black corners.
- HUD: the white HUD text is **pixel-identical** in both roll states (336
  bright pixels, centroid 696.8,597.0 exactly), confirming it is drawn after
  the sprite and stays fixed.
- A mirrored **-25 degree** head roll produces the symmetric sprite angle,
  so `roll_sign = -1` is consistent.

**The hand fix.** The first version built the hands in the full head frame, so
they naturally rotated +25 degrees with the head - i.e. they moved *with* the
world, which the owner rejected. The measurement below showed it: with the
head level and sprite off the laser axis was 45.0 degrees at (1159,740); with
+25 degrees head roll it was 70.0 degrees at (1058,806), exactly a +25 degree
rotation about the screen centre.

Final design: `HandViewPose` now uses a **no-roll head basis** (yaw and pitch
only; up = world up projected off the head forward). The hands/gun/laser are
therefore part of the VR camera and do not roll with the world. The existing
hand-pass view rotation cancels the sprite on top, and the HUD is drawn after
the sprite. The result with the SixDOFinator sample hands and a smooth +/-25
degree sine roll (8 s period) is: world rocks, hands and HUD stay fixed. The
owner confirmed it was fixed.

- `roll_sign` default is **-1** (the original +1 direction was inverted).
  `/` toggles roll live for comparison.

Build `built Nov  6 2026 09:12:29`, SHA-256
`2cc5662fcf8d04f93585b0e0e6e739dfa25d3ffaf3ba265b3edf21ec3e9cb179`.

### 2026-11-06 - custom HUD no longer covers the death summary

`HudShouldDraw()` used to be `InGameplay() || PlayerDead()`, so after dying the
custom HUD stayed up and the game's 2D HUD stayed suppressed, covering the
post-death stats panel. It is now **gameplay only**: on mission result != 0
(death or victory) the custom HUD stops and the game's own 2D UI draws
normally. A health-driven `DEAD` state is still shown if the player dies
during gameplay, but the summary screen belongs to the game.

Build `built Nov  6 2026 09:24:03`, SHA-256
`107ac4c9fe5aa187e0fa9e85e3810760d12bc2f389c7235f946dd01738e91413`.

## 2026-11-06 - launcher (Play / Options / update test) and config move

### Launcher

New `launcher-xr/` Win32 app, `XOPSVR.exe` (MSVC x86, no dependencies):

- **Play** finds `xops096.exe` beside the launcher (or up to two folders up)
  and starts it.
- **Options** edits `config.txt` in the game folder: handedness, "Flip hands",
  haptics on/off, haptics hand, verbose debug, stereo (mono/AER), roll
  direction. Haptic frames are preserved but hidden; head roll is always on
  and has no checkbox.
- **Update check** uses the Shogo WXR AutoUpdate `version.txt`/`notes.txt`
  URLs as a temporary test channel. The version compare is value-based (same
  rules as Shogo). The prompt is real; the DLL download is deliberately
  disabled and YES writes nothing.
- Test log `XOPSVR_update.log` next to the EXE.

Verified on the PC: online 1.2 vs local 0.1 produced the YES/NO prompt with
Shogo's notes; YES logged "download disabled"; Options saved
`E:\tmp\Q3Mods\xops096\config.txt`; Play started xops096.exe and the mod then
logged `HAND config E:\tmp\Q3Mods\xops096\config.txt`.

The real XOPS release plumbing (own URLs + d3d8.dll download/install) is still
in `NEXT_STEPS.md`; never point the download at Shogo's CShell.dll.

### Config location

`config.txt` moved from one folder above the game into the game folder, next
to xops096.exe, in both the mod and the launcher. The mod still reads the old
parent file if the local one is missing; the launcher copies a legacy parent
file into the game folder once.

Build: mod `built Nov  6 2026 09:49:19`, SHA-256
`53b831374c82e436a037d2e8f591e605bac9dc357d29e2df5b5c89be482e0bad`.
Launcher `version.txt` 0.1 (XOPSVR.exe is a build artifact, not committed).

## 2026-11-06 - render-side 6DOF / eye height, weapon-slot HUD, debug-key lock

### Weapon-slot HUD

The game's spinning 3D weapon-slot models are now skipped at draw time when
the custom text HUD is active. The capture and tail recording still run
**before** the skip, so the VR hand's cloned weapon model still loads; verified
`HIDE assembled 1 part(s) for 'MP5SD'`. `K` restores the game's slots.

### Debug-key lock

Every debug/probe key now requires manual PC mode. `ProbeKeys` ignores all
keys except `0` while `g_vrExclusive` is set, so a WinlatorXR session (which
does not map `0`) cannot trigger them. Verified: `F` before `0` did nothing;
after `0` it logged `WINDOW fit-to-display: off`.

### 6DOF and eye height

The game re-clamps its own camera Y every frame, so writing the player
position was never going to hold the head's rise/fall. It is now **visual
only**:

- `CameraOffset()` rotates `headP` into the camera's own axes and returns the
  offset in metres. HMD Y is clamped to ±1 m **before** the gain; `eye_height`
  is a separate manual float (default 0, ±5); `pos_scale` scales the tracked
  translation (default **0.1**, 0..2).
- `AerDrawPrimitive` and the stride-20 `AerDrawIndexed` path copy the world
  vertices and shift them opposite the eye on x/y/z. Hands, gun and laser stay
  in the head frame. Game position writes (Y and both horizontals) are gone;
  yaw/pitch writes remain, so stick-forward still follows the head.
- Right-stick click calls `SteerRecentre()` (moved before the stick-move/steer
  gates so it always fires). This makes the current HMD height the new zero.
- **Root cause of the "through the floor/ceiling" jump:** the non-indexed
  world path multiplied the offset by 100 as if its vertices were centimetres.
  The projection is near 0.1 / far 80 and the player's own Y is ~1.9, so it is
  camera-space **metres**. The `*100` is removed (and the same mistake in the
  AER eye shift). A 1 m head move now shifts the world 1 m, not 100 m.
- Added a status line, a few times a second:
  `EYE hmdY=... tracked=... manual=... total=...`.

### Tests

- ±1 m sine, 6 s period, right-stick pulse at 14 s. Log `dy` tracked `hmdY`
  1:1 (`hmdY=-0.931` -> `dy=+0.931`); the ±1 m clamp held while the recentre
  offset pushed `hmdY` past ±1.5.
- Constant-height re-zero: 0.000 -> 0.800 -> right-stick click ->
  0.000 and stayed at 0.000. `STEER recentre requested by the right stick
  click` appears in the log before it.
- Debug-key lock: `F` before `0` did nothing; `F` after `0` toggled the
  window.
- Old command, for contrast: the pre-fix build logged `dy=-50.000` for a
  0.5 m feed and threw the view through the level.

### Follow-up: shipping gain reduced to 0.1

The owner tested the 1:1 build on device and found the render broke as soon as
the HMD moved off `0,0,0`, so the shipping gain is now **0.1**:

- `pos_scale` default 0.1 (range 0..2; `0` disables translation entirely).
- The ±1 m HMD-height clamp is applied to the **raw** height before the gain,
  so lowering the gain does not loosen the physical limit.
- `eye_height` is still added after the gain so it keeps its ±5 m range.

Verified at 0.1: constant 0.8 m feed logs `tracked=+0.080` and the world draw
shifts `dy=-0.080`; the right-stick click still drops `hmdY` and `tracked` to
0.000. Raise the gain only after 0.1 is stable on hardware.

### Root cause: primitive count vs vertex count (any nonzero offset tore the frame)

The owner then reported that even a tiny X/Y/Z movement broke the render. The
gain was not the real problem: the copy path had a count bug.

`IDirect3DDevice8::DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount)`
passes a **primitive** count. `AerDrawPrimitive` treated it as a vertex count,
so for a triangle list it copied and shifted only `count` vertices, then
`DrawPrimitiveUP(..., count, ...)` rendered `3 * count` vertices - two thirds
of them stale data from earlier draws. Any nonzero `CameraOffset` therefore
produced garbage triangles.

Fix:

- `AerDrawPrimitive` now computes `verts = PrimVertexCount(prim, count)` (the
  helper already used by the HUD path), copies/shifts `verts`, and passes the
  original primitive count to `DrawPrimitiveUP`.
- The log now reports `prim`, `count` and `verts`; the world draws show
  `count=460 verts=1380`, confirming the 3x conversion.
- `AerDrawIndexed` was passing a hardcoded stride 28 to
  `DrawIndexedPrimitiveUP`; it now passes the real `stride` (20 or 28).

Verified with back-buffer grabs at `0,0,0`, +0.5 m Y, +0.5 m X and +0.5 m Z
(at `pos_scale = 0.1`): all four frames are coherent, with the expected small
vertical/horizontal parallax. `pos_scale` stays at the owner-requested 0.1 as
a cautious default; it can be raised now that the copy is correct.

### Hands used the unscaled head offset

Re-checked the packet against `XRAPI_V0_4_DESC.md`: floats 18-21 are the HMD
quaternion and 22-24 the HMD position, so the sine feeder was moving the head
correctly (confirmed by `EYE hmdY`). The hands, however, subtracted the **raw**
`g_pose.headP` while the world used `pos_scale * headP`. At 0.1 the hands'
parallax was ten times the world's, which reads as "the hands move, not the
head".

Fix: a single `ScaledHeadP()` helper returns the clamped, gain-scaled head
translation used by both `CameraOffset()` and `HandViewPose()`. The manual
`eye_height` is also subtracted along the head-up axis for the hands, so they
stay anchored when the calibration offset changes.

Before/after, same ±1 m head sine with the hands held fixed:

| | hand screen positions (640x480) |
|---|---|
| pre-fix | `270,728` -> `875,-480` -> `1130,2800` -> `245,-326` (flying) |
| post-fix | `522..625, 421..678` (bounded, correct close-range parallax) |

This matched-parallax version was an intermediate step. The owner then
specified the final rule: the hands live in the default/recentre head-height
frame and take only the head's orientation, not its live translation.

### Final hand rule and `pos_scale` in Options

- `HandViewPose` now uses the **raw** hand position (`rel = hp`) rotated by
  the no-roll head basis. No `headP` and no `eye_height` subtraction. So
  `0,0,0` is the default HMD/head height; a controller below it is at hand
  height; moving the HMD moves the view/world, not the hands.
- `pos_scale` default is back to **1.0** (1:1), range 0..2.
- The launcher's Options dialog has an **HMD position gain** edit box
  (0.0-2.0). `Config` reads/writes `pos_scale` and preserves the rest of
  `config.txt`.

Verified:

- Launcher: the Options dialog opened with `1.00`, accepted `0.55`, and wrote
  `pos_scale = 0.55` to the game-folder config. (Test channel was parked at
  9.9 locally so the startup update prompt did not block the dialog.)
- Mod: continuous sine on X, Y and Z, each ±0.5 m, `pos_scale = 1.0`.
  `EYE hmdY` stayed within ±0.51; `offset primitive` showed dx/dy/dz up to
  ±0.47 with `verts = 3 * count`; back-buffer frames stayed coherent.
  `HAND screen` stayed exactly `577,516` / `78,145` for the whole run,
  confirming the hands no longer follow the HMD translation.

Build: `built Nov  6 2026 12:58:01`, SHA-256
`8b9d428afd56e13bfcfd382fddcea470ce97592374ad8773edbad001d59d9c8c`.
Launcher `XOPSVR.exe`: 187,392 bytes, SHA-256
`5ba78f8743e5e8f01410d68e010185133ab227abe3f79fb645d5fbe5f5159861`
(build artifact, not committed).

## 2026-11-06 - device bring-up, DXVK export fix, dev/release split, HUD scale

### Crash on device

The first device runs exited before the first frame. The probe log showed the
device created and the D3D8 hooks installed, but zero `HOOK` calls. The Wine
log said `Ask to run at NULL, will segfault` (address 0), then address 1 after a
`DebugSetMute` wrapper. The game has the string `DebugSetMute` and looked it up
dynamically; DXVK 1.10.1's `d3d8.dll` does not export it. The proxy was
re-exporting it as a naked jump (address 0) and then a typed wrapper (address
1, wrong call/return stack shape).

Fix: **export only `Direct3DCreate8`**. The game sees the same NULL for the
optional debug helpers it sees with DXVK alone. Device run then reached
gameplay: `MODE_VR 1`, thousands of packets, hands/gun/HUD and roll working.

### Outbound UDP every frame

`XrSendFrame` is called from `Present` **every frame**, before `o_Present`,
including menus when `MODE_VR` is 0. `HapticTick` returns 0 for both hands
when nothing is buzzing; the runtime ignores 0 for the haptic fields and uses
the decayed duration. Added a **first frame at startup** (`0 0 MODE_VR=0 ...`)
to awaken the runtime, matching IMVR's `SendFirstFrame`. `MODE_VR` and
`MODE_3D` are only re-read from an outbound packet, so there is no idle-frame
gap.

### Device input finding

The log showed `move 0.00, 0.00` for the left stick almost every interval
while the right stick `turn` worked. The owner saw up/down on the left
controller triggering weapon swaps. That means WinlatorXR is mapping the left
stick to **keyboard arrow keys**, not the XrApi left-stick analog. The mod's
`move` path never sees it. The fix is in the WinlatorXR controller layout:
map the left stick to the left thumbstick (analog/XrApi) or to W/A/S/D, and
remove the arrow-key mapping.

### Dev/release split

`XOPS_DEV_BUILD` (default 1) now selects diagnostics at compile time:

- `build\build-dev.bat` - probe keys, `HOOKS`/`HOOK` trace, verbose log, draw
  dumps, frame grabs; output `out\dev\d3d8.dll`.
- `build\build-release.bat` - no probe log, no debug keys, no draw/grab;
  builds the launcher and assembles `Release\` with a clean config/INSTALL.
- `build\build-probe.bat` remains the dev alias.

### HUD scale

The replacement HUD was only adjustable through the dev `H` key and
`xops_wxr_steer.ini`. Added `hud_scale` to `config.txt` (0.5..2.0, 1.0 native,
1.1 default; config overrides the INI) and a **HUD scale** field to the
launcher Options. The release package ships `hud_scale = 1.0`.

Current dev build: `built Nov  7 2026 01:48:59`, SHA-256
`9888d85939cf5b6599e829eb42ab9c352a6ed5524f0f426e6e5a9bd8326dacde`.
Release build: 287,232 bytes, SHA-256
`8e713d60cb75ea5e3a14947e89d8909242f9591fbdaf8f6d0b231ca94cf758d0`.

### Device follow-up: version 0.4, pitch sign, release verbose

- The `version` file advertised **0.5**, but this client only implements the
  0.4 packet. 0.5 adds the InputAPI listener on 7728, which is not present.
  It now writes **0.4**; the runtime reads the file once, so a runtime restart
  is required.
- Look up/down and the hand-aim shot were both inverted on the Quest because
  the device had no `xops_wxr_steer.ini` and fell back to the code default
  `pitchSign = -1`. The saved desktop INI and the device report both settle
  **+1**; `SteerAimWrite` uses the same sign for the camera and the shot, so
  one default fixes both. `hand_flip` is visual only: it negates the hand's
  up/right vectors, not its forward/aim vector.
- Release now keeps **safe verbose logging** when `verbose_debug = true`:
  build/config, XrApi raw packet + sticks every ~2 s, MODE_VR/pose, EYE, ROLL,
  HUD. Probe keys, CAMHUNT/scan ticks, draw dumps and frame grabs remain
  dev-only. With `verbose_debug = false` release writes no log.
- Open: AER works in-game but crashes on the transition back to a menu. Need
  the probe log and Winelog from that transition to see whether it is the
  `AerDraw*` path, `RESET`, or the runtime's MODE_3D switch.
- Arrow-key weapon swaps: WinlatorXR maps the left stick to arrow keys, and
  XOPS binds up/down to weapon swaps. The mod now blocks **all** arrow keys
  whenever `g_vrExclusive` is on - in gameplay and in the flat menu screen -
  while leaving other desktop keys available for menus.

### 2026-11-06 - hands anchored, level-start recentre, full VR input isolation, floating menu panel

Device report: hands rose and fell with physical head height, rolled with the
head, arrows still swapped weapons, Y still hid the laser, and some levels shot
wrong. Changes:

- `HandViewPose` now subtracts the same clamped, `pos_scale`-scaled
  `ScaledHeadP()` the world uses, plus `eye_height`, before projecting into the
  no-roll head basis. Hands are world-anchored at the level-start/recentre
  position; moving the HMD moves the view, not the hands.
- Removed the hand counter-rotation against `SpriteRoll`. The entire game image
  rolls with the head; counter-rotating the hands made them view-glued and they
  swung on every head roll. They now roll with the presented world.
- The game's `LevelActive()` state (mission/result/player, no draw signature)
  rises at a level spawn. `hkBeginScene` catches that edge **before**
  `SteerTick` writes the camera and calls `SteerRecentre()`, so the untouched
  spawn yaw is captured; an old menu/level head offset cannot leak in. Pausing
  and resuming leaves `LevelActive` true, so it does not recentre on every
  pause. The current HMD XYZ and yaw become the new `0,0,0` / zero-yaw origin
  against the player model. Right-stick click still recentres at any time.
- **Full VR input isolation:** in gameplay and on the floating non-VR panel, all
  physical keys/mouse are stripped from `GetKeyboardState`, and the window
  procedure now also swallows `WM_KEY*`/`WM_MOUSE*`. Only `ESC` and `0`
  survive. A/B/X/Y and arrows are swallowed in VR on every screen. Mono and AER
  menus behave the same: the panel owns the pointer, not the desktop mouse.
- The raw arrow keys from WinlatorXR's left-stick mapping are captured before
  the filter and translated to W/A/S/D. This makes the current layout move the
  player and guarantees the arrows can never reach the weapon-swap binding.
- Laser gate removed: it always draws. Y only toggles zoom.
- `MODE_VR=1` is now sent for the whole run in **mono and AER** (HWXR model);
  only `MODE_3D` toggles 2 <-> 0. Every non-gameplay screen is drawn at one
  third size in the centre of the view, with a dominant-hand laser pointer
  published through `GetCursorPos` and right-trigger click (both `VK_LBUTTON`
  and posted `WM_LBUTTONDOWN`/`WM_LBUTTONUP`). AER itself is off there.
- AER -> menu crash was not reproduced in the supplied Wine log: the log ended
  with `MODE_VR -> 0`, two menu frames and a clean `SHUTDOWN`, no Wine page
  fault. Keeping `MODE_VR=1` in both modes may avoid it; needs a device run.

### 2026-11-06 - spawn yaw/aim base and WinlatorXR input injection

Device log `E:\tmp\XOPS\RunLogs\xops_wxr_probe.log` (release 18:20:08):

- **Spawn facing / shot mismatch:** at the level-start edge the log showed
  `yaw A raw 0.0000 -> 0.0000  B 1.5816` (90.7 deg). A is the render copy the
  game makes during its update; B is the player's authoritative orientation.
  A can still be one frame stale at a spawn, so the view used A=0 while the
  shot and movement used B=90.7 deg. `SteerRecentre` now sets
  `g_refYaw = g_refYawB` whenever B is readable, so view and shot share a base.
- **Input injection:** the game's real input path is confirmed by
  disassembly: `0x406950` copies the previous `0x48c920` array to `0x48c814`
  and calls `GetKeyboardState(0x48c920)`; `0x407520` turns the configured key
  mappings into the player input bits at `player+0x10/+0x11`. The configured
  keys are the ones the mod already pulses (W/S/A/D, R, G, Shift, X, Z, Space,
  LButton).
  The failure was in the hook: if Wine's `GetKeyboardState` returned FALSE
  (normal when WinlatorXR/the container owns focus), the `if (r && p)` block
  was skipped and **no XrApi key was injected at all**. The hook now zeroes the
  array on failure and injects the XrApi state anyway, and returns TRUE because
  it owns the buffer. The arrow capture also happens before the focus mute, so
  the left-stick-as-arrows mapping survives an unfocused game window.
- This is direct control of the game's own input handling, not OS-level key
  synthesis: the game reads the array we fill. No WinlatorXR InputAPI listener
  is needed for commands.
- **XrApi is the source of truth in gameplay.** Physical keys are removed
  every frame, focused or not; `VrExclusiveKeys` no longer returns early on
  focus. The synthetic XrApi keys are added back immediately after. Menus,
  level objectives, death, victory and intro screens keep physical keys, and
  `0` toggles manual PC input in every build (release included).

### 2026-11-07 - direct player-update input; no Wine keys in gameplay

The owner's report: the key-state route still does not work on WinlatorXR.
XrApi is the source of truth, so gameplay no longer depends on Wine's keyboard
state at all.

- **Code hook:** the game calls its player update `0x407CF0` from
  IDA `0x413AC4`, immediately after its own input mapper `0x407520` and before
  the player update. `HookPlayerUpdate` patches that 5-byte `call` to a
  wrapper (`E8 rel32` -> our function), after verifying the original target is
  `0x407CF0`. If the check fails it logs and leaves the old key fallback in
  place.
- **Wrapper:** `PlayerUpdateHook` rebuilds the game's 256-byte VK image at
  `0x48C920` from XrApi (left stick analog + digital direction bools, jump,
  reload, drop, switch, zoom, fire mode, shoot), calls the game's own mapper
  `0x407520`, then runs the original `0x407CF0`. The image is deliberately left
  in place so the next frame's input poll copies it to the previous-state
  buffer; the mapper's own edge detection then behaves exactly as for a held
  key. No OS key events, no `SendInput`, no Wine keyboard state.
- **Aim and two-handed state** are set in the wrapper before the shot step:
  `g_twoHanded` from the left grip, `SteerAimWrite` while the shoot trigger is
  held.
- **Movement** uses the XrApi left-stick analog fields plus the runtime's
  digital `L_THUMBSTICK_*` bools, so it works even when WinlatorXR sends the
  stick only as directions.
- **Spawn/aim:** `SteerRecentre` uses the player orientation B for both A and
  B so the view and shot share a base. Device confirmation still needed.
- **Local desktop smoke test (7 Nov):** the hook installed at `0x413AC4`, the
  game reached gameplay, feeder packets with `moveY=1.0` moved the player
  (`h1`/`h2` changed), and feeder A/B presses switched the active weapon
  (FAMAS/MP5 captured). No crash. The old key path is now only a fallback.

### 2026-11-07 - ESC/menu, canonical controls, early aim write

Owner findings after the direct-input build:

- **ESC must never be blocked, and the XrApi left menu is ESC.** The
  unfocused-window mute was clearing the whole 256-byte array, including ESC.
  It now clears everything except `ESC` and `0`, and
  `GetAsyncKeyState`/`GetKeyState` apply the same exception. The left
  controller's menu button (`L_MENU`, bool 1) now emits a real ESC by setting
  `VK_ESCAPE` in the game image and posting `WM_KEYDOWN`/`WM_KEYUP` to the game
  window. It works on gameplay and menus with no physical keyboard.
- **XOPS controls must be canonical.** A custom PC `config.dat` can put several
  actions on one physical key, which corrupts the direct-input image. VR mode
  now rewrites the 20-byte key-binding block at `0x48C380` every frame to the
  default layout. The launcher's Play button also writes those 20 bytes into
  `config.dat` (preserving the video flags and player name).
- **Aim timing.** The direct-input build called `SteerAimWrite` from the player
  update wrapper, after the game's shot step. It is called again from the
  `GetKeyboardState` hook, before the shot, as the old path did. The late call
  was removed.
- **Yaw diagnostics.** `YAW A ... B ... delta ... map <id>` logs every ~2 s and
  `AIM base A ... B ... head ... -> A ... B ... map <id>` logs on the first
  shots. The map id is the game's mission index (`0x490A44`), so BUILDING_EXT
  (known good) and the failing maps can be compared directly. The shot trace
  reads A (`0x404CD5`); movement reads B. These logs will show whether A/B
  diverge or are clamped.
- Open design question: the owner reports we are turning a virtual camera
  offset instead of the in-game character yaw. The likely next fix is to make B
  the single authority and derive A from it (or patch the clamp), with the
  camera offset zeroed at level load.

**Paused 7 Nov night.** Next session: device-test the 01:48 build (left menu
as ESC, canonical controls, early aim), capture `STEER recentred`, `YAW ... map`
and `AIM base ... map` lines from BUILDING_EXT and a failing map, and
re-confirm the input policy. See `CURRENT_STEPS.md` "Where we stopped" and
`NEXT_STEPS.md` section 4.
