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
IAT write, not `LoadLibrary`). Scaling it to 1.25× (`h 18 -> 22`) made the text
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

`built Nov  5 2026 22:40:22`, SHA-256
`8cfdeb585feed90573fe17c0d7e574e1f8178f2704ddff3470b186c17c9b3629`,
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
