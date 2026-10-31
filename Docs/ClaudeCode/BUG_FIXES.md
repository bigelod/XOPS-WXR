# BUG_FIXES - XOPS-WXR

Defects found in the inherited work (`Docs/MetaAI/`), each with the fix and a
**cheap test that settles it**. Ranked by consequence: the first five would each
on their own have produced a mod that did nothing at all on device.

**Nothing here has been fixed yet**, because no code has been written in this
repository. These are specifications for the rewrite, not a record of repairs.
When one is genuinely fixed and the owner has confirmed it on hardware, mark it
`FIXED (device-verified <date>)` and keep the original text - a finding's
history is worth more than a tidy list.

Evidence tags: **[runtime]** WinlatorXR's own source, **[spec]** the XrApi
specification, **[general]** `Docs/general_wxr/`, **[derived]** reasoned from a
documented API, **[code]** read directly in the inherited source.

Line references are to `Docs/MetaAI/XOPS_WXR/XOPS_WXR/XOPS_WXR.cpp` unless
stated otherwise.

---

## D-1 - No handshake files are written, so the runtime never sends anything

**Severity: fatal.** This alone makes the whole mod silent.

**[code]** Nothing writes `Z:\tmp\xr\version` or `Z:\tmp\xr\vr`. The UDP thread
binds 7872 and waits.

**[runtime]** `XrAPI.updateImplementation()` reads the *first line* of `version`
and matches `startsWith("0.1")` through `startsWith("0.7")`. No match - a
missing file, a typo, a stray BOM, an unsupported number - leaves the
implementation null, and then **no UDP listener is started and no packet is ever
sent**. It is read **once**, at startup, so the version cannot change during a
session. The runtime also **wipes the directory at startup** before writing
`system`, so files written too early do not survive.

**Fix.** On init: locate the handshake directory - `Z:\tmp\xr`, falling back to
`<current drive>:\xrtemp` when `Z:\` does not exist, so one binary runs on both
desktop and device. Write `version` containing `0.5` and `vr` containing `VR`.
Read `system` for the headset identity (7 lines; line 2 is a product codename -
`EUREKA` is a Quest 3), defaulting to `META` / `QUEST 3` when absent. Delete
`vr` on clean shutdown. Be prepared to rewrite the files if the runtime wiped
them.

**Confirm.** On device, `ls Z:\tmp\xr` shows `version`, `vr` and `system`, and
the raw-packet log starts producing lines. The presence of `system` is also the
cleanest "am I on a headset?" test available. **[general]**

---

## D-2 - The inbound packet format is wrong

**Severity: fatal.**

**[code]** The parser tries a binary `float[7]` with a sanity heuristic
(`fabs(f[0]) < 10`), then falls back to `sscanf_s("%f,%f,%f,%f,%f,%f,%f")` -
comma-separated position and quaternion.

**[spec]** XrApi 0.4/0.5 sends one line of **space-separated ASCII**:

```
client0 <28 floats> <frame id> <19-or-21-char T/F string> <HMD name>
```

Floats 0-3 left controller quaternion, 4-5 left stick, 6-8 left controller
position, 9-12 right quaternion, 13-14 right stick, 15-17 right position,
**18-21 HMD quaternion, 22-24 HMD position**, 25 IPD, 26-27 FOV x/y, then the
frame id (`HMD_SYNC`). Nothing in the packet is CSV, nothing is binary before
0.7, and the head pose is at indices 18-24, not 0-6.

**Fix.** Rewrite the parser to the real format. Match `client` as a **prefix**
(the token is `client0`). **Ignore trailing tokens you do not recognise** - the
format gains fields at the end between versions and a strict parser rejects
packets it could have used. Accept **both** bool-string lengths: 19 characters,
or 21 when the immersive and SBS flags are appended as a separate token from
0.3 onward. Bounds-check every index.

**Confirm.** Log one raw packet verbatim and compare field by field against
the capture the owner takes on the Quest 3 - checkpoint 4 in
`PROJECT_PLAN.md`.

---

## D-3 - The outbound packet format is wrong

**Severity: fatal for mode control; blocks menus, FOV and haptics.**

**[code]** `sprintf_s(reply, "%d,0,0,0", frame)` sent to 7278 from the `Present`
hook - a frame counter.

**[spec]** The outbound packet is six **space-separated** values:

```
<L haptic> <R haptic> <MODE_VR> <MODE_3D> <FOV x> <FOV y>
```

A sane mono starting packet is `0 0 1 0 104.5 104.5`. Haptics are a **duration
in frames**, not an amplitude; FOV is in **degrees** and is applied only if
greater than 1, so tangents of half-angles are silently ignored.

**Fix.** Send the six-field packet every frame. Use **`MODE_VR = 0` in menus and
on pause** - it stops the pose stream, restores the runtime's own mouse control
of the screen, and drops the frame out of immersive presentation so a flat menu
is readable. **[general; device-proven in the Interstellar Marines port]** Two
cautions that come with it: **keep sending the outbound packet even while the
mode is 0**, or the stream cannot be turned back on; and expect a gap when
returning to 1, so hold the last good pose across it.

**Also:** format the floats with an **invariant locale**. The sibling port's
`XRAPI.cs` comments that this is "not optional" - a comma-decimal locale emits
`104,5` and corrupts the field. In C that means avoiding a `setlocale`-sensitive
path, or formatting the integer and fractional parts by hand.

**Confirm.** Set `MODE_VR = 0` deliberately and watch the pose stream stop, then
set it to 1 and watch it resume.

---

## D-4 - The sync pixel carries a local counter instead of `HMD_SYNC`

**Severity: fatal for comfort.** This is the defect the general docs single out
as the one no amount of frame-rate work can compensate for.

**[code]** `int frame = g_frameId.fetch_add(1) % 255;` then `p[0] = frame << 16`.
The value drawn is the mod's own counter, not the frame id the runtime sent.

**Why it matters.** The runtime reads red from the top-left pixel to learn
**which pose the image in front of it was rendered for**. A local counter
carries no such information, so the runtime reprojects against the wrong head
pose and the world swims when the player turns - which is exactly the stimulus
that causes motion sickness. Players call it "laggy" and stop within minutes.
**[general]**

**[runtime]** Also: do **not** gamma-correct or rescale the value. `XrFramesync`
collects the distinct red values it observes until it has 22 of them, sorts
them, persists them, and thereafter maps an observed value to
`indexOf(value) * step` - it calibrates itself to whatever colour space the
renderer produces. The remapping is **gated on green == 0**. And the runtime
derives the game's frame rate from changes in this value, so a value that never
changes reads as a frozen game.

**Fix.** Draw `RGB(sync, 0, eyeFlag)` where `sync` is float 28 of the packet
**echoed exactly as received**, green is 0, and `eyeFlag` is 0 in every mode
except AER's right eye. Draw it **last**, after the world and the HUD. 10x10,
not one pixel - the reader samples (0,0) only, but ten survives a border or an
odd window size shifting the corner. Under SBS, draw it in both halves.

**Confirm.** Checkpoint 6: does tracking feel *locked* or *swimmy*? Swimmy with
correct camera maths means sync. Optionally draw a second, differently-coloured
10x10 square just below as a visible mode indicator - a glance at a screenshot
then answers a question that would otherwise need a log pulled off the headset.
**[general]**

---

## D-5 - The project cannot link: two `DllMain` definitions

**Severity: fatal, and the clearest evidence the project was never built.**

**[code]** `dllmain.cpp` defines `BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)`
and `XOPS_WXR.cpp` defines `DllMain` again. Both are in the project. The linker
reports a duplicate symbol.

**Fix.** One `DllMain`, in `dllmain.cpp`. Keep it minimal: `DisableThread-
LibraryCalls`, open the log, write the build stamp, start nothing heavy. **Do
not** do socket or hook work inside `DllMain` - which is also defect D-11.

**Confirm.** The build produces a DLL. That is the whole test, and it is the
reason `CURRENT_PROGRESS.md`'s "build system - complete" cannot be true.

---

## D-6 - `SetTransform` is at vtable index 37, not 44

**Severity: high.** Index 44 is `SetLight`, so the inherited code would hook
lighting and pass a `D3DMATRIX*` where a `D3DLIGHT8*` was expected.

**[code]** `MH_CreateHook(vtbl[44], &hkSetTransform, ...)`, with the comment
"SetTransform at ~0xB0 (index 44)".

**[derived]** `IDirect3DDevice8`'s vtable, counting the three `IUnknown` slots
first: `TestCooperativeLevel` 3, `GetAvailableTextureMem` 4,
`ResourceManagerDiscardBytes` 5, `GetDirect3D` 6, `GetDeviceCaps` 7,
`GetDisplayMode` 8, `GetCreationParameters` 9, `SetCursorProperties` 10,
`SetCursorPosition` 11, `ShowCursor` 12, `CreateAdditionalSwapChain` 13,
`Reset` 14, **`Present` 15**, `GetBackBuffer` 16, `GetRasterStatus` 17,
`SetGammaRamp` 18, `GetGammaRamp` 19, `CreateTexture` 20,
`CreateVolumeTexture` 21, `CreateCubeTexture` 22, `CreateVertexBuffer` 23,
`CreateIndexBuffer` 24, `CreateRenderTarget` 25,
`CreateDepthStencilSurface` 26, `CreateImageSurface` 27, `CopyRects` 28,
`UpdateTexture` 29, `GetFrontBuffer` 30, `SetRenderTarget` 31,
`GetRenderTarget` 32, `GetDepthStencilSurface` 33, `BeginScene` 34,
**`EndScene` 35**, `Clear` 36, **`SetTransform` 37** (offset `0x94`),
`GetTransform` 38, `MultiplyTransform` 39, `SetViewport` 40, `GetViewport` 41,
`SetMaterial` 42, `GetMaterial` 43, **`SetLight` 44** (offset `0xB0`).

`Present` at 15 and `IDirect3D8::CreateDevice` at 15 (`0x3C`) are both correct
in the inherited code - which is what makes the `0xB0` claim look like a
coincidence rather than a reading: `SetLight(2, ...)` and
`SetTransform(D3DTS_VIEW = 2, ...)` both compile to a `push 2`, so a
disassembly search for `push 2` near a `call [ecx+...]` finds either.

**Fix.** Avoid the question entirely: **wrap the interfaces instead of detouring
the vtable** (`PROJECT_PLAN.md` section 4). A wrapper's method is selected by
the compiler from the header, so there is no index to get wrong, and there is no
detour that might behave differently under Box86/Wine. If MinHook is used as a
fallback, index 37.

**Confirm.** With the wrapper in place, log every `SetTransform` call for one
frame with its `Type` and matrix. That single log both proves the camera is
reachable through `D3DTS_VIEW` and hands over the game's handedness and unit
scale - and it is the experiment that should precede any camera code at all.

---

## D-7 - `LockRect` on the back buffer needs a flag the game does not set

**Severity: high.** The sync square would never appear.

**[code]** `GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)` then
`bb->LockRect(&lr, nullptr, 0)`.

**[derived]** A Direct3D 8 back buffer is only lockable if the device was created
with `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` in `D3DPRESENT_PARAMETERS.Flags`. A
2003 game has no reason to set it. Without it, `LockRect` fails and the code -
which checks `SUCCEEDED` and then silently does nothing - draws no square at
all. On a device where it *did* succeed, locking the back buffer every frame is
also a full pipeline stall.

**Fix.** Draw the square with **`Clear` and a `D3DRECT`** instead:

```c
D3DRECT r = { 0, 0, 10, 10 };
dev->Clear(1, &r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(sync, 0, eye), 0.0f, 0);
```

Exact byte values, no filtering, no blending, no texture, no vertex buffer, no
stall, and no lockable back buffer required. Call it as the last thing before
the original `EndScene` returns.

**Fallbacks, in order.** 1. Add `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` in our
`CreateDevice` wrapper and keep `LockRect` - we control the present parameters,
so this is available, just slower. 2. A two-triangle pre-transformed quad with
filtering, blending, fog and Z explicitly disabled, as HaloCEWXR does.

**Confirm.** On the desktop build, screenshot the window and read the top-left
pixel's value; it should equal the `HMD_SYNC` the tester sent.

---

## D-8 - The camera composition is mathematically wrong

**Severity: high.** It would produce a camera that looks almost right and drifts.

**[code]**

```c
m._41 += g_hmd.px * scale;  /* ... _42, _43 */
D3DXMatrixRotationQuaternion(&rot, &q);
D3DXMatrixMultiply(&result, (D3DXMATRIX*)&m, &rot);
```

Two errors. First, adding an offset into the view matrix's translation row is
not the same as moving the camera: `_41/_42/_43` of a view matrix are already
`-eye . basis`, so adding a world-space offset there translates in **view**
space against an un-rotated basis. Second, the head rotation is applied
**un-inverted**, and after the translation, so the translation is then rotated
too.

**[derived] The correct composition.** D3D uses row vectors:
`p_view = p_world * V`, and `V = C^-1` where `C` is the camera's world matrix.
The head pose `H` is an offset expressed in the camera's **own** frame, so it
composes before `C`:

```
C' = H * C   =>   V' = (H * C)^-1 = C^-1 * H^-1 = V * H^-1
```

So **`V_new = V_game * inverse(H_head)`**, with `H_head` built as
rotation-then-translation in the engine's axis convention and engine units. For
a rigid transform the inverse is free: transpose the 3x3 and negate the rotated
translation.

Sanity check it the way the derivation is checked: a head moving `+x` by `d` in
camera-local space must shift every world point by `-d` in view x. `V * T(-d)`
does exactly that.

This also behaves correctly by construction: the head moves *relative to where
the game's own mouse look is pointing*, rather than fighting it.

**Fix.** Implement the formula above. Apply **orientation first** and get
looking-around right before adding position - fewer variables. Add a recentre
binding immediately; keep the inverse of the pose captured at recentre and apply
it to everything. Normalise the quaternion on ingest, unconditionally. Derive
basis vectors by rotating the canonical axes and *then* flipping for a
left-handed engine - do not try to mirror the quaternion itself. **[general]**

**Confirm.** Checkpoint 6. Pitfalls in the order they usually bite: axis flipped
(world mirrored); pitch and roll swapped (tilting reads as turning); metres
added to engine units (camera at the far edge of the level, or glued still); yaw
double-applied because the game is also applying mouse look.

---

## D-9 - `50.0f` metres-to-units is a guess, and it is presented as a finding

**Severity: medium, and it is the project's most instructive defect.**

**[code]** `const float scale = 50.0f;` with the comment "XOPS units ~ 50x
smaller than meters, tune scale". `CURRENT_PROGRESS.md` then reports it as
"currently too fast in small rooms, too slow for peek" - a report of device
behaviour from code that cannot have run.

**Fix.** **Measure it.** Log the game's own camera position - available in the
view matrix we are already intercepting - across a walk between two points whose
real-world separation is knowable from the level geometry, and compute
units-per-metre. Put the result in **one** named constant. A magic `* 64.0f`
scattered through the code cannot be changed later. **[general]**

**The rule this illustrates**, from `Docs/general_wxr/TIPS.md`: *a correct value
transfers.* If a number only works where it was found, the frame of reference is
wrong - stop tuning and fix the frame. Three rounds of screenshot-driven tuning
in the Shogo port produced three values that each worked only on the machine
they were found on; the round that logged the actual transform got it first try.

**Confirm.** The measured scale makes a 1 m real lean move the camera by 1 m of
level geometry, checked against something of known size in the level.

---

## D-10 - No draining, no freeze-on-drop, no normalisation, no timeout

**Severity: medium.** Each is a known, named trap.

**[code]** A blocking `recv` in a tight loop writing seven atomics, with no
socket timeout, no concept of "newest packet", no last-good-pose handling and no
quaternion normalisation. The thread has no exit path.

**Fix.**

- **Drain** every queued datagram each frame and keep the newest. Reading one
  packet per frame builds a queue and the mod acts on steadily staler poses.
  **[general]**
- **Freeze on the last good pose** when packets drop. Never zero - one zeroed
  frame is a violent jolt in a headset.
- **Normalise every quaternion on ingest, unconditionally.** The rotation
  shortcut `v + 2w(u x v) + 2u x (u x v)` is a rotation only when `|q| = 1`;
  otherwise it also scales by `|q|^2`. Device packets are unit to about 1e-4,
  but **tester packets often are not** - captures during the Shogo port were
  1.4% to 10.8% long, which stretched each hand's basis by a different amount
  and looked exactly like a deep bug in the pose maths. **[general]**
- Set a **receive timeout** (the sibling port uses 500 ms) so the thread can
  notice shutdown and so a dead stream is distinguishable from a quiet one.
- Give the thread a **clean exit** so the DLL can unload.
- Bind **one** of 7872/7873, not both. **[runtime]** From 0.4 the runtime sends
  the same packet to every port in its list; binding both means receiving every
  packet twice. That is worth stating because it is the opposite of what the
  port numbers suggest - 7873 is not a fallback.

**Confirm.** Pull the tester's plug mid-session: the view should freeze, not
snap. Log the quaternion magnitude on ingest for one tester run and watch it
differ from 1.

---

## D-11 - Hook and library work inside `DllMain`

**Severity: medium.** A real deadlock risk, and the kind that appears only
sometimes.

**[code]** `DllMain` calls `MH_Initialize()`, then `GetModuleHandleA("d3d8.dll")`
and `LoadLibraryA("d3d8.dll")` if absent, then creates and enables a hook.

**Why.** `LoadLibrary` from `DllMain` runs under the loader lock and can
deadlock or run another DLL's initialiser at a point the loader does not expect.
It is a long-standing Win32 rule, and Wine's loader is a different
implementation of the same lock - exactly the sort of thing that works on the
desktop and hangs on device. `CURRENT_PROGRESS.md`'s own note that "XrThread
blocked DllMain, game hung on load" is the same family of bug, half-diagnosed.

**Fix.** `DllMain` does the minimum: `DisableThreadLibraryCalls`, open the log,
write the build stamp. Everything else happens on first call into an export -
which, with the `d3d8.dll` proxy, is `Direct3DCreate8`, a natural and safe
initialisation point outside the loader lock.

**Confirm.** The game reaches the main menu with the proxy installed, repeatedly,
on device. A load-order deadlock is intermittent, so test it more than once.

---

## D-12 - The build depends on a DirectX SDK that cannot build D3D8

**Severity: medium - it blocks the first build.**

**[code]** `#pragma comment(lib, "d3d8.lib")`, `#pragma comment(lib,
"d3dx8.lib")`, `#include <d3dx8.h>`, and a README instructing the user to
install the **DirectX SDK (June 2010)** and set `DXSDK_DIR`.

**The June 2010 SDK contains no Direct3D 8 at all.** August 2007 was the last
release to ship `d3d8.h`, `d3d8caps.h` and `d3d8types.h`; June 2010 is the last
SDK overall, and it carries D3DX9/10/11 only.
Source: <https://walbourn.github.io/directx-sdks-of-a-certain-age>

**Fix.** Remove the DirectX SDK dependency entirely:

- take `d3d8.h` from **mingw-w64**, which is permissively licensed and ships a
  complete set of D3D8 headers, and vendor it under `external/d3d8/`;
- **do not link `d3d8.lib`** - the proxy `LoadLibrary`s the real `d3d8.dll` and
  `GetProcAddress`es `Direct3DCreate8`, so no import library is needed;
- **do not use D3DX for anything.** A 4x4 matrix multiply, a quaternion-to-
  matrix and a rigid inverse are about sixty lines in total, with no installer
  for every contributor and nothing extra to ship.

**Confirm.** A clean machine with Visual Studio 2022 and no DirectX SDK builds
the project.

---

## D-13 - `Reset` is not hooked, so overrides are lost

**Severity: medium**, and certain to bite once the window work lands.

**[code]** Only `CreateDevice`, `Present` and (intended) `SetTransform` are
hooked.

**Why.** `IDirect3DDevice8::Reset` takes a fresh `D3DPRESENT_PARAMETERS` and is
called on device loss, mode change and alt-tab. Any resolution or windowing
override applied at `CreateDevice` is discarded there. **[general]** also warns
that engines rebuild their windows, that the fix must be idempotent and
re-applied every frame, and that an engine's own consistency checks will fight a
hand-rolled resize once a second and win.

**Fix.** Wrap `Reset` and apply the same present-parameter rewrite. Re-apply the
window style every frame from `EndScene`, idempotently. Watch for the engine
resetting the mode when surface, render and client sizes disagree - move all
three together, or let the container size do it.

**Confirm.** Alt-tab, change level, and let the device be lost; the game still
fills the view with no chrome afterwards.

---

## D-14 - The documentation records fixes for events that did not happen

**Severity: process, and the most expensive defect in the set**, because it
caused a plan to be built on a baseline that did not exist.

`Docs/MetaAI/BUG_FIXES.md` lists fifteen fixes. Several describe observations
that require a running mod on hardware - "wrote DWORD red on R5G6B5, got blue
pixel", "WinlatorXR container NAT dropped packets", "head movement launched
camera through walls", "frameId went >255, compositor lost sync" - and **none of
the described fixes is present in the committed code**: there is no back-buffer
format check, no `recvfrom` change, and the scale is still an unmeasured
constant. `CURRENT_PROGRESS.md` likewise marks the transport, the sync pixel and
the build as complete, against a project that does not link.

The documents were not lying about intent. They were written as though the plan
had already happened.

**Fix - two standing rules, now in `PROJECT_PLAN.md` section 2.**

1. **Mark every claim with its evidence** - `[spec]`, `[runtime]`, `[general]`,
   `[derived]`, `[code]`, `[device]`, `[unverified]`. An unmarked claim reads as
   device-verified.
2. **Nothing is complete without a human observation on hardware.**
   `CURRENT_STATUS.md` records state, not intent.

**Keep the file, though.** Read as *plausible failure modes worth watching for*,
it is useful: the R5G6B5 back-buffer format, the loader-lock hang, the ASLR
hazard of hardcoded addresses, and the hook-ordering trap are all real traps
that a port of this kind can hit.

---

## D-15 - Smaller things, worth fixing while rewriting

- **Hardcoded addresses and ASLR.** `Docs/MetaAI/` correctly notes that reading
  the device from `dword_48C5BC` breaks under ASLR and that taking it from
  `CreateDevice`'s out-parameter is right. Keep that. Any address that must be
  used should be rebased against `GetModuleHandle(NULL)`, never used absolute.
- **`#pragma comment(lib, "libMinHook.x86.lib")` with the comment "or .x86.lib
  for 32-bit XOPS"** - the comment contradicts the line it annotates. Moot if
  the wrapper vector removes MinHook.
- **No build stamp.** `__DATE__ __TIME__` is not embedded and no log is opened,
  so there is no way to tell from the device which binary is running. The deploy
  script must refuse a binary older than the newest source file. **[general]**
- **No verbose flag.** Every diagnostic in the project must sit behind one
  setting, off by default, including file dumps.
- **The `.def` file forwards to `real_dinput.dll`** - correct for a `dinput`
  proxy, and to be replaced by d3d8's export set. Note the general advice that
  `version.dll` is the popular proxy name because it is loaded early and almost
  never used; for us `d3d8.dll` is better still, because being the renderer's
  entry point is the whole point.
- **`.gitignore` excludes `build/`, `*.dll` and `*.lib`** - which is where the
  project file and deploy script are meant to live. Un-ignore `build/` before
  committing it, or the deploy script will be silently untracked. (Risk R-7.)
