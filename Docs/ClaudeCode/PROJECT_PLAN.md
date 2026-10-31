# PROJECT_PLAN - X Operations 0.96 to standalone VR via WinlatorXR

Filled in from the template at `Docs/general_wxr/PROJECT_PLAN.md`, structured on
the staged plan in `Docs/general_wxr/FIRST_STEPS.md`.

**Section 1 is an intake interview and it is not finished.** Every `TBD` there
is a question an agent cannot answer from the code, and a wrong assumption
propagates through the whole port. Answer them with the owner before writing
code.

---

## 1. Intake - questions only the owner can answer

### Q-A - SETTLED: mod the real binary

**Owner's decision, 31 Oct 2026:** *"We're modding the real binary not building
openxops, someone else can make that."*

So: hook `XOPS.exe`. OpenXOPS is **out of scope for this project** and is not to
be reopened as an alternative route. Fidelity to the original binary is the
point of the port, and that is a sufficient reason on its own.

What this costs us, recorded here so no later session mistakes it for an
oversight: the camera, the window, the resolution, the projection matrix and the
sync square all have to be reached through hooks into a binary we only partly
understand, which is why feature **F-5**'s "log every `SetTransform` for one
frame" experiment comes before any camera code, and why stereo stays out of
scope until mono is device-verified.

### The game

| Question | Answer |
|---|---|
| Game, version, patch level | X Operations **0.96** (Team MITEI; programmer nine-two, art TENNKUU; 2002-2003) |
| Store or distribution | freeware; original distribution. Exact archive the owner has: `TBD` |
| Install path | **`D:/VR/xops096-wxr`** (owner, 31 Oct 2026). Treat this as the single install the mod is deployed to; the container path, if it differs, is `TBD` |
| Bitness | 32-bit native C/C++ **[MetaAI, plausible - confirm with `dumpbin /headers`]** |
| Renderer | Direct3D **8.1** (`Direct3DCreate8`, SDK version `0xDC` = 220) **[MetaAI, plausible - confirm by `d3d8.dll` in the import table]** |
| Engine | Team MITEI in-house. Not Genesis3D, not .NET **[MetaAI]** |
| Source available? Licence? | **No.** (A New-BSD reimplementation, OpenXOPS, exists but is explicitly out of scope - see Q-A) |
| Mod API or SDK? | None known. Community tools exist for maps/missions; relevance `TBD` |
| Does a VR build already exist? | No. Path E (OXRWXR + OpenComposite) does not apply |
| Existing PCVR mod to retarget? | None known |
| Modding community or documentation? | Yes - an active Japanese XOPS scene (map and mission editors, add-ons). Worth searching before disassembling anything. Specific resources: `TBD` |
| Native head-tracking support? | No. Path D (opentrackWXR) does not apply |
| Default resolution | **640x480**, set at `CreateDevice` **[MetaAI]**. Whether the game exposes any resolution option at all: `TBD` |
| Does it load `dinput.dll`? | Moot - we proxy `d3d8.dll`, and **the owner has confirmed that override works** (31 Oct 2026) |
| Japanese locale or codepage needed? | **No** (owner, 31 Oct 2026). Risk R-6 is closed |

### The port

| Question | Answer |
|---|---|
| Real VR port (XrApi, 6DOF) or injection port (InputAPI)? | **Real VR port**, XrApi 0.5, in-process |
| Target headsets | Meta Quest 3 (`EUREKA`). Others untested |
| Presentation mode | **Mono first** (`MODE_VR=1`, `MODE_3D=0`). AER considered after mono is device-verified; SBS last |
| Ship a non-VR mode as fallback? | **Yes** - `MODE_VR=2`, and `MODE_VR=0` for menus |
| Framing | First-person |
| Hands in view at all? | **No**, not in the first version. XOPS renders a viewmodel weapon; decoupling it from the view is a later feature |
| Multiplayer in scope? | **No** |
| Recommended container resolution | Start **1400x1400** (square suits the panel) and compare against 2800x1400. Needs device measurement; see risk R-3 |
| Head-driven aiming, or head-look only? | **Settled: aim is completely separate from the head.** The head drives the view *and* the movement direction (forward = where the head is looking); the weapon aims from the **dominant hand** (motion aim). See feature F-7 - this is the most consequential decision in the project after the injection vector |

### Constraints

| Question | Answer |
|---|---|
| May the result be published? Where? | `TBD` - presumably GitHub, as with the sibling ports |
| May any of the game's source appear in the repository? | There is no source to leak - the shipping binary has none released. XOPS's own *data* files and binaries must never be committed regardless |
| Release-owned files that automation must never touch | `TBD` - nothing exists yet. Declare them before the first release |
| Who verifies on hardware, and how fast is the loop? | The owner, on a Quest 3. Loop length: `TBD` |
| Prior work to inherit or discard? | `Docs/MetaAI/` - **keep the IDA findings, discard the code and the protocol**. See `PROJECT_OVERVIEW.md` section 6 and `BUG_FIXES.md` |
| Is `adb` / `scrcpy` available to the owner? | `TBD` - it changes how diagnostics are collected |

> ### CHECKPOINT 0 - with the owner
> Q-A is settled, the install path is known, the `d3d8.dll` override is
> confirmed working, no locale workaround is needed, and the input design is
> settled. What remains here: the exact archive/build of XOPS 0.96, the
> container's own configuration, whether `adb`/`scrcpy` are available, and the
> publication question. None of those gate stage 1.

---

## 2. Hard rules

Binding on every pass, human or automated.

- **The standing choices in `Docs/general_wxr/CLAUDE.md` apply in full.** XrApi
  0.4/0.5, mod directly, keep the game's renderer, stay 32-bit, measure
  performance on-device, no wrappers. A higher version number and a modern
  renderer are not goals.
- **The sync pixel is not optional.** Echo `HMD_SYNC` exactly as received, red
  channel, green 0, 10x10 at the top-left, drawn last. Never rescale or
  gamma-correct it.
- **Normalise every quaternion on ingest, unconditionally.**
- **Freeze on the last good pose** when packets drop. Never zero.
- **Convert each pose independently of the head.** Share only the recentre
  origin.
- **One change per deploy.** Nothing is "done" until the owner has verified it
  on hardware. Do not mark a milestone complete on the strength of reasoning -
  that is exactly how `Docs/MetaAI/` came to claim a working port that cannot
  link.
- **Embed `__DATE__ __TIME__`** and log it as line 1. The deploy script refuses
  a binary older than the newest source file.
- **Never commit XOPS's own data files or binaries.** Not maps, not textures,
  not `ezds.dll`, not `XOPS.exe`.
- **Diagnostics behind one verbose flag, off by default**, including file dumps.
- **Mark claims with their evidence.** `[spec]`, `[runtime]`, `[general]`,
  `[derived]`, `[device]`, `[unverified]`. An unmarked claim reads as
  device-verified, and in this project almost nothing is.

---

## 3. What exists today

### Repository layout - current

```
README.md                  one paragraph
LICENSE
.gitignore                 build/ is tracked; obj/, out/, bin/ ignored
Docs/general_wxr/          the general framework (read-only, shared)
Docs/MetaAI/               first attempt: 4 docs + a ~200-line VS project
Docs/ClaudeCode/           this folder
```

### Repository layout - proposed

```
src/                       the mod's own source, our licence
  dllmain.cpp              the single DllMain, proxy export forwarding
  d3d8proxy.cpp/.h         Direct3DCreate8 + IDirect3D8/Device8 wrappers
  xrapi.cpp/.h             the XrApi 0.5 client: handshake, sockets, parse
  camera.cpp/.h            pose -> view matrix
  syncpixel.cpp/.h         the red square
  window.cpp/.h            borderless, fill the display, re-applied per frame
  log.cpp/.h               the log file, build stamp on line 1
  config.cpp/.h            hot-reloaded settings
external/d3d8/d3d8.h       mingw-w64 header, no SDK dependency
build/                     .vcxproj + build-and-deploy.ps1   (un-ignore this)
tools/                     packaging
Docs/
```

### Current state

**Nothing has been built, run or verified.** There is no compilable project in
the repository root; `Docs/MetaAI/XOPS_WXR/` contains a Visual Studio project
that cannot link (two `DllMain` definitions) and could not work if it did (see
`BUG_FIXES.md`). The IDA reverse-engineering findings in `Docs/MetaAI/` are the
only inherited asset of value, and they are plausible but unconfirmed by this
session.

### The blocker

**Stage 1 has not happened.** Nothing else can be attributed to anything until
the unmodified game is known to run flat in a container, and until the remaining
intake `TBD`s are answered. The three that were blocking are now closed: the
install path is `D:/VR/xops096-wxr`, the `d3d8.dll` override works, and no
Japanese locale is needed. What is left is device and owner observation, not
code.

---

## 4. Approach

### Injection vector - `d3d8.dll` proxy

Ship a `d3d8.dll` next to `XOPS.exe` that exports `Direct3DCreate8` (plus the
handful of other d3d8 exports, forwarded), loads the real
`C:\windows\system32\d3d8.dll`, and returns **our wrapper object** in place of
the real `IDirect3D8`.

Why this rather than the first attempt's `dinput.dll` + MinHook:

- **The game provably loads `d3d8.dll`.** Whether it loads `dinput.dll` is an
  unverified claim.
- **No detouring at all.** We own `Direct3DCreate8`, so we can hand back a
  wrapper and intercept `CreateDevice`, `SetTransform`, `Present`, `EndScene`
  and `Reset` by *being* the vtable - no MinHook, no vtable index arithmetic,
  and nothing that can behave differently under Box86/Wine than on the desktop.
  This removes risk R-1 and defect D-6 outright.
- **It makes the resolution override trivial**: rewrite
  `D3DPRESENT_PARAMETERS` on the way through `CreateDevice`.

**The DLL override is confirmed working.** Wine needs to be told to prefer the
native DLL in the application directory over its own builtin `d3d8`, and the
owner has confirmed (31 Oct 2026) that this works in their WinlatorXR
container - so the vector is settled, not a bet. Write down the exact setting
and where it lives at stage 1, because the installer will need to reproduce it
on a player's device.

Remaining cost: a wrapper for two interfaces is more code than two detours - a
few hundred lines of mechanical forwarding. Worth it, for the reasons above.
**MinHook stays as the fallback** in case the override behaves differently after
a WinlatorXR update: hooking `Direct3DCreate8` from a `dinput.dll` proxy is then
the route, and the correct vtable indices are in `BUG_FIXES.md` D-6.

### Interfaces to wrap, and what each is for

| Interface / method | What we do |
|---|---|
| `Direct3DCreate8` | return the wrapper |
| `IDirect3D8::CreateDevice` | force resolution and windowed mode; add `D3DPRESENTFLAG_LOCKABLE_BACKBUFFER` only if the `Clear`-rect sync route fails; return a wrapped device |
| `IDirect3DDevice8::SetTransform` | `D3DTS_VIEW`: compose the head pose. `D3DTS_PROJECTION`: later, for FOV and the near plane |
| `IDirect3DDevice8::EndScene` | draw the sync square last; drive the per-frame XrApi send; re-apply the window style |
| `IDirect3DDevice8::Present` | fallback site for the sync square if `EndScene` fires more than once per frame |
| `IDirect3DDevice8::Reset` | re-apply the resolution and window override; the engine will fight us here |

### The sync square - draw it with `Clear`, not `LockRect`

**[derived]** `IDirect3DDevice8::Clear` takes an array of `D3DRECT`. A single
`Clear(1, &rect, D3DCLEAR_TARGET, D3DCOLOR_XRGB(sync, 0, eye), 0, 0)` over
`{0,0,10,10}` writes exact byte values with no filtering, no blending, no
texture, no vertex buffer and no shader - which is precisely what the sync
reader needs. It also avoids needing a lockable back buffer, which is what the
first attempt's `LockRect` route silently required.

Do it as the last call before the original `EndScene`. Verify on device that
`EndScene` is called once per presented frame; if not, move it to `Present`.

### Camera composition - the formula, because the first attempt's is wrong

D3D uses row vectors: `p_view = p_world * V`, and `V` is the inverse of the
camera's world matrix `C`. A head pose is an offset in the **camera's own
frame**, so it composes *before* `C`:

```
C' = H * C        =>        V' = (H * C)^-1 = C^-1 * H^-1 = V * H^-1
```

So: **`V_new = V_game * inverse(H_head)`**, where `H_head` is the head's pose
built as rotation-then-translation in the engine's axis convention and engine
units.

This matters because it is the correct composition and it also does the right
thing conceptually: the head moves relative to wherever the game's own mouse
look is pointing, rather than replacing it.

The first attempt instead added metres-scaled position straight into `_41/_42/
_43` of the view matrix and multiplied by the raw head rotation without
inverting it. Both are wrong in ways that produce a plausible-looking but
drifting, mirrored camera - see `BUG_FIXES.md` D-8.

Unknowns to measure, not guess: the engine's **unit scale** (metres to XOPS
units - the first attempt's `50.0f` is a guess with no measurement behind it),
and its **handedness and axis order**. `Docs/general_wxr/XRAPI_V0_4_DESC.md`
section 6 gives the conversion pattern; `TIPS.md` gives the rule that decides
when you have it right - *a correct value transfers*.

### Decisions taken deliberately

| Decision | Choice | Why |
|---|---|---|
| Bitness | 32-bit | the game is; no OpenXR runtime to load |
| Renderer | keep D3D8 | known-good under Box86/Wine; stereo needs no modern API |
| Wrappers | none | a layer inside two translation layers |
| Process model | in-process | no IPC, no second binary |
| XrApi version | 0.5 | readable, testable, and includes InputAPI at no cost |
| Presentation mode | mono first | ships on its own; AER later for depth at mono cost |
| Hooking mechanism | DLL wrapper, MinHook as fallback | nothing to behave differently under Box86 |
| D3D8 headers | mingw-w64, no SDK | June 2010 SDK has no D3D8 |
| D3DX | **not used at all** | `d3dx8.lib` does not exist in any current SDK, and a 4x4 multiply is 20 lines |
| Sync square | `Clear` with a `D3DRECT` | exact bytes, no lockable back buffer needed |
| Supersampling | container resolution | the runtime filters on downsample |

---

## 5. Milestones

From `Docs/general_wxr/FIRST_STEPS.md`. Each leaves something testable. Live
state is tracked in `CURRENT_STATUS.md` - **this list is the definition, that
file is the truth.**

- [ ] **0** Intake complete; feasibility confirmed (Q-A settled: mod the real binary)
- [ ] **1** Unmodified XOPS 0.96 runs flat in a WinlatorXR container, and every
      workaround needed is written down
- [ ] **2** Our DLL loads and writes a log whose line 1 is the build stamp
- [ ] **3** The game renders at a VR-appropriate resolution, borderless, filling
      the display, and survives a device reset
- [ ] **4** XrApi 0.5 client parses a **real device packet**; the button table is
      built from a device log, not from a document
- [ ] **5** Sync pixel drawn last, 10x10, echoing `HMD_SYNC`
- [ ] **6** **6DOF head tracking feels locked** - the big one
- [ ] **7** Presentation mode chosen and measured on device
- [ ] **8** Aim decoupled from the head, and weapon/viewmodel decoupling - now
      required by the input design, not optional. See F-7
- [ ] **9** Full controller input mapping, owner-confirmed
- [ ] **10** HUD scale, comfort settings, settings surface

### Stage-by-stage detail

**Stage 1 - flat in a container.** Before any VR. Launch unmodified XOPS, play a
mission on the 2D screen. Record: the container resolution, the DXVK/WineD3D choice, whether intro/mouse
capture/save paths misbehave, and **exactly where the `d3d8.dll` override is
configured** (it is known to work; what is missing is the written-down steps).
No locale workaround is needed. Every workaround here is something an installer
will later need to automate.

> **CHECKPOINT 1 - owner, on device.** "Does unmodified XOPS run, and what did
> you have to do to make it run?"

**Stage 2 - get inside.** Smallest possible change: the proxy loads, forwards
every export, and writes one log line with `__DATE__ __TIME__`. Build the
deploy script with its freshness guard **now**, before there is anything hard to
debug. Confirm `Direct3DCreate8` is reached by logging it.

> **CHECKPOINT 2 - owner, on device.** "Does the log appear, and is line 1 the
> stamp of the build you just made?" This is also where we learn whether the
> Wine DLL override works at all - far cheaper to find out now.

**Stage 3 - own the window and the resolution.** Two jobs that are one problem
here, and the reason this stage is bigger for XOPS than for a modern game:

1. Rewrite `BackBufferWidth`/`Height` in `CreateDevice` to the container size
   (`GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)`), windowed.
2. Strip `WS_CAPTION | WS_THICKFRAME | WS_BORDER`, size to the display, make it
   idempotent, re-apply every frame from `EndScene`.

Expect the game to fight this: a 2003 engine may hardcode 640x480 in its 2D
drawing, its mouse-to-screen mapping and its menu hit-testing. **Find out early
whether the HUD survives a resolution change** - if it does not, the fallback is
to render at 640x480 into a window stretched to the display, which keeps the
sync corner valid and costs sharpness. Record which it was.

> **CHECKPOINT 3 - on device.** "Does the game fill the whole view, with no
> title bar, and is the HUD still usable?"

**Stage 4 - the XrApi client.** Build and test it on the desktop with the UDP
Tester; do not go to the device for this. Handshake files to `Z:\tmp\xr`
(`version` = `0.5`, `vr` = `VR`, deleted on clean shutdown) with a
`<drive>:\xrtemp` fallback; read `system` for the headset, defaulting to
META/QUEST 3; bind **one** of 7872/7873 on 127.0.0.1; drain every queued
datagram each frame and keep the newest; parse 28 floats + frame id + the 19-
or 21-character bool string + trailing tokens, tolerating extra tokens; send
`0 0 1 0 104.5 104.5` on 7278 every frame; normalise every quaternion; log the
raw packet verbatim every few seconds behind the verbose flag.

> **CHECKPOINT 4 - owner.** One raw packet captured on the actual Quest 3,
> compared field by field against the tester's. Then the owner presses each
> control in turn while the bool string is logged, and **the button table is
> built from what moved**.

**Stage 5 - the sync pixel.** `Clear` a 10x10 `D3DRECT` at the top-left with
`RGB(sync, 0, 0)`, last thing before `EndScene` returns. Nothing visible to
check here; the payoff is judged at checkpoint 6.

**Stage 6 - the camera, mono.** Orientation first, position second, in that
order, with a recentre binding added immediately. Use the formula in section 4.
Measure the unit scale rather than guessing it: log the game's own camera
position across a known walk and work out units-per-metre from the geometry.

> **CHECKPOINT 6 - on device, and this is the big one.** Does the world hold
> still when the head holds still? Does it swim when turning quickly (suspect
> stage 5)? Does leaning move the view the right way and the right amount? Is
> the horizon level? **Do not proceed until this is right.**

**Stages 7-10.** As in `FIRST_STEPS.md`. Stage 7 is a decision, not a step.
Stage 8 is the first stage worth *skipping* and probably should be. Stage 9 is
`F-7` below. Stage 10 is HUD scale first, since 2D art does not scale with the
container.

---

## 6. Features

Written before implementing, in the order they should be attempted. `F-1` to
`F-5` are the milestone path above; those below it are the candidates after
mono works.

### F-1 - the proxy and the log

**Goal.** Our code runs inside XOPS and can tell us things.
**Can it be done?** Yes - the owner has confirmed the `d3d8.dll` override works
in their container (31 Oct 2026), which was the only real doubt.
**Must be proven first.** Nothing structural. The first build is itself the
test: a proxy that only forwards and logs. Record the override setting's exact
location while doing it, for the installer.
**Deploy target.** `D:/VR/xops096-wxr` - the owner's install, and the path the
deploy script writes to.
**Fallbacks.** 1. `dinput.dll` proxy + MinHook on `Direct3DCreate8`.
2. `ddraw.dll` or another early-loaded name. 3. An external process (last
resort - it reintroduces IPC).
**Acceptance.** Owner confirms the log file on device with the right build stamp.

### F-2 - resolution and window override

**Goal.** The game fills the headset view with no chrome.
**Can it be done?** The resolution rewrite is straightforward at
`CreateDevice`. **Whether XOPS's 2D layer survives it is the open question.**
**Must be proven first.** Run the game at a non-640x480 size on the desktop and
look at the HUD, the menus and the mouse mapping. This is a desktop experiment,
so it is cheap.
**Fallbacks.** 1. 640x480 back buffer in a borderless window stretched to the
display - keeps the sync corner, loses sharpness. 2. A non-4:3 mode with
letterboxing, if the engine distorts.
**Risks.** The engine's own consistency checks resetting the mode; `Reset`
losing our override.
**Acceptance.** Owner confirms borderless full-view on device, HUD readable,
and that it survives alt-tab and a level change.

### F-3 - XrApi 0.5 client

**Goal.** Poses, buttons and sticks arrive, and the mod reports its mode.
**Can it be done?** Yes. This is well-trodden; the sibling port's `XRAPI.cs` is
a device-tested reference for every field.
**Must be proven first.** Nothing - but do not skip writing `version`.
**Acceptance.** A device packet logged verbatim and parsed field-for-field; the
button table built from the owner's presses.

### F-4 - sync pixel

**Goal.** The runtime can match each captured frame to the pose it was rendered
for.
**Can it be done?** Yes, via `Clear` with a rect.
**Must be proven first.** That `EndScene` is called exactly once per presented
frame. Log a counter against `Present` for one run.
**Acceptance.** Judged at checkpoint 6: tracking feels locked, not swimmy.

### F-5 - 6DOF head tracking, mono

**Goal.** Looking around and leaning work, and feel locked.
**Can it be done?** Depends on a single unknown: **does XOPS set its view
matrix through `SetTransform(D3DTS_VIEW)` once per frame?** The first attempt
asserts it does but recorded the wrong vtable index, which means it was reading
the disassembly rather than observing calls.
**Must be proven first.** With the proxy in place, log every `SetTransform`
call for one frame - type, and the matrix. That answers it in one run, and it
also gives us the game's handedness and unit scale for free. **Do this before
writing any camera code.**
**Fallbacks.** 1. Patch the game's own camera variables directly (the first
attempt's `dword_48C5BC` xrefs are the starting point). 2. Hook the engine's
camera setup function rather than the D3D call. 3. Non-VR mode (`MODE_VR=2`),
which needs no camera work at all and stays available if the camera proves
unreachable.
**Settings.** `pos_scale`, `rot_enabled`, `recentre` binding, `verbose`. Hot
reloaded from a text file next to the DLL; defaults chosen to be right rather
than tunable.
**Acceptance.** Checkpoint 6, on device, in the owner's words.

### F-6 - non-VR / flat-window mode

**Goal.** A comfort and performance fallback, and the whole shipping plan if
F-5 fails. `MODE_VR=2`, mono; `MODE_VR=0` in menus and on pause.
**Note.** Read bool 19 (`IS_IMMERSIVE`) and damp tracking to about 0.8 when it
is set, or tracking reads 25% too fast. **[general]**

### F-7 - decoupled aim, head-relative movement, and input mapping

**Settled by the owner, 31 Oct 2026.** Three separate vectors, and that is the
design:

| Vector | Driven by |
|---|---|
| **View** - where the camera points | the head (feature F-5) |
| **Movement** - which way "forward" goes | **the head.** Forward = the head's facing direction |
| **Aim** - where shots go | **the dominant hand.** Point the controller, shoot there - motion aim, completely independent of the head |

**This is the hardest feature in the project, and it is now on the critical
path.** It is worth being plain about why, because the rest of the plan was
written assuming the cheap option.

XOPS is a 2003 mouse-look FPS: the camera orientation *is* the aim vector. There
is one angle pair in the game's state and both read from it. Separating them
means the mod has to supply the engine with an aim direction that is not the
camera's - which means **finding and writing the player's aim state in the
game's memory**, not merely intercepting a D3D call. The view can be composed
on top of a matrix we already see go past; the aim cannot.

**What must be proven first**, before any aim code is written:

1. **Does the game keep a player aim angle in memory that is distinct from the
   render camera?** Cheap test, and the right tool is Cheat Engine, not a
   disassembler: search for a float that changes as the mouse turns, then check
   whether writing it turns the shots without turning the view.
   `Docs/general_wxr/TOOLS.md` is explicit that this beats reading
   disassembly for exactly this job.
2. **If there is only one angle pair**, then aim and view cannot be separated at
   this layer, and the options narrow to: (a) drive the single angle from the
   dominant controller and let the *head* add a view-only offset on top of it -
   which inverts the usual arrangement but delivers the owner's intent, since
   the head then genuinely is not aiming; or (b) fall back to head-aim and ask
   the owner to re-decide with the cost known.

**Aim source: dominant-hand motion aim** (owner, 31 Oct 2026). The player points
the controller and shoots where it points. This is the full-fidelity answer and
the one players expect from a VR port - and it is also the version that depends
entirely on test 1 above, because it needs the weapon's direction to be settable
in world space while the camera stays on the head.

**So test 1 is not optional and it is not a formality.** Run it before writing
aim code. If it comes back negative, the owner re-decides with the cost known;
`right stick aim` is the fallback that needs no memory writing at all (inject
mouse motion) and still satisfies "aim is not the head", and a laser pointer
with a crosshair we draw ourselves is the middle option. Both are recorded here
so a later session does not rediscover them.

**Expect viewmodel work to come with it.** Once the weapon points somewhere the
camera does not, XOPS's first-person weapon model is visibly wrong - it is drawn
as part of the view. Decoupling or hiding it becomes part of this feature rather
than a later polish item, and anything we draw in its place must carry whatever
flag the engine uses for view-model rendering, or it will sort against the world
and draw through the gun. **[general]**

**Head-relative movement** is the easy half of this feature and should land
first. The game reads movement as four key states; what changes is only *which*
keys we press for a given stick deflection, after rotating the stick vector by
the head's yaw relative to the body. No memory writing, no engine knowledge -
and it is independently testable, so build it before touching aim.

**Note on head-relative movement as a comfort choice.** Most VR ports make
movement *controller*-relative precisely so the player can look one way and walk
another. Head-relative means every head turn re-aims the walk. The owner has
asked for head-relative; implement it, and expose the alternative as a setting
if it turns out to fight the player during combat.

**Mechanism for input generally.** Hook the game's input reads, or inject with
`SendInput` in-container, or use WinlatorXR's InputAPI for keys and clicks -
noting that **InputAPI cannot move the mouse**, only click and scroll, so any
mouse *motion* must come from inside the container. **[general]**

**Note.** Decide per control whether the game wants a *held state* or a
*key-down edge*; a trigger held forever where an edge was expected is why the
Shogo multiplayer respawn hung. **[general]**

**Write the full mapping table out** - one line per control, VR action to game
action - and have the owner confirm it before implementing and again on device.
Button layout is pure preference and the owner is the authority. **[general]**

**Acceptance.** Owner on device, with controller overlays visible in the
screenshot: walking goes where they are looking; shots land where the
**controller** points and *not* where the head is looking; the crosshair agrees
with where shots land; and the weapon model is not visibly pointing somewhere
different from the shots. **[general]** - a screenshot showing where the
controller actually is, is the datum that makes hand and weapon placement a
measurement rather than a guess.

### F-8 - HUD scale

**Goal.** The HUD is readable in the headset.
2D art at fixed pixel sizes covers less of the view as the container grows.
Prefer deriving the scale from the measured width against a reference width,
with a single strength control, over exposing a raw number. **[general]**

### F-9 - stereo (AER before SBS)

**Goal.** Depth. **Deferred** - see `PROJECT_OVERVIEW.md` section 5. AER first:
one eye per frame, `MODE_3D=2`, camera offset `+/- IPD/2` from packet float 25,
eye flag in the sync square's **blue** channel (0 left, 255 right). If stereo
is attempted at all, centralise mode selection in one function and make SBS win
over AER. **[general]**

---

## 7. Verification

**Build.** `build/build-and-deploy.ps1`, which must: wipe `obj/` and `out/`;
build Release|Win32; read the `__DATE__ __TIME__` stamp out of the binary and
**refuse to deploy** if it is older than the newest source file; copy to each
target and verify with a SHA-256 comparison. Step 3 is the one that matters -
testing a stale build is invisible without it. **[general]**

**Confirm the right binary is running.** Line 1 of the log carries the build
stamp. Check it before interpreting any device report.

**Desktop pass.** XrAPI UDP Tester plus the desktop build of XOPS. This proves
*the code path runs*. It does **not** prove the values are right: the tester's
quaternions are often not unit length and its hand separation does not match a
headset. **[general]**

**On-device pass** - the only real gate:

- [ ] log line 1 shows the expected build stamp
- [ ] the game fills the view, no chrome, HUD readable
- [ ] head tracking feels locked, not swimmy
- [ ] leaning moves the view the right way and by a plausible amount
- [ ] frame rate acceptable at the recommended container size, per the
      container's own counters - not a timer in the mod
- [ ] previously working features unaffected (name the ones the change could
      plausibly have touched)

---

## 8. Risks

| Id | Risk | Likelihood | What we do about it |
|---|---|---|---|
| R-1 | A hook behaves differently under Box86/Wine than on the desktop | medium | prefer the DLL-wrapper vector, which detours nothing; prove any detour on device early |
| R-2 | XOPS's 2D HUD, menus or mouse mapping assume 640x480 and break at VR resolutions | **high** | test on the desktop at stage 3, before any VR work; fall back to a stretched 640x480 window |
| R-3 | The container resolution that looks good costs too many frames | medium | measure on device with the container's own counters; it is a setting, not code |
| R-4 | XOPS does not set its camera through `SetTransform(D3DTS_VIEW)`, or does so in a way we cannot compose with | medium | log every `SetTransform` for one frame before writing camera code; fallbacks in F-5 |
| R-5 | ~~The Wine DLL override for `d3d8.dll` is unavailable or unreliable~~ | **CLOSED** | owner confirms `d3d8.dll` override works, 31 Oct 2026 |
| R-6 | ~~The game needs a Japanese locale or codepage~~ | **CLOSED** | owner confirms none needed, 31 Oct 2026 |
| R-7 | ~~`.gitignore` ignores `build/`~~ | **CLOSED** | `build/` un-ignored 31 Oct 2026; `obj/`, `out/`, `bin/` ignored instead. Note `core.ignorecase=true`, so a `Build/` line would re-ignore `build/` - do not add one |
| R-12 | **Aim cannot be decoupled from the view in a binary we do not own** - the game's mouse look *is* its aim, and the chosen design (dominant-hand motion aim) needs them separate | **high** | feature F-7 test 1: settle it with Cheat Engine before building anything on it. Fallback is right-stick aim via injected mouse motion, which needs no memory writing and still satisfies "aim is not the head" |
| R-8 | Tuned values do not transfer from tester to device | high | measure offsets in the frame they are used in; a correct value transfers **[general]** |
| R-9 | A saved config on the test device overrides a changed default | medium | say so explicitly when handing over a build |
| R-10 | The `Reset` path loses the resolution and window override | high | re-apply from `Reset` and from `EndScene`, idempotently |
| R-11 | Work is marked done on reasoning rather than on hardware | **demonstrated** - it is what happened to `Docs/MetaAI/` | nothing is complete until the owner confirms on device; `CURRENT_STATUS.md` records state, not intent |

---

## 9. What we learned

> Append as the project goes. Not code structure - git shows that. The
> invisible things: which frame an offset is measured in and why, what a wrong
> value looked like on screen, which approach was abandoned and why.

### Inherited documentation can be confidently wrong

The first attempt's docs described a working XrApi client, a working sync pixel
and a working build. The code defines `DllMain` twice, so it never linked; the
protocol it implements is not XrApi; and without a `version` file the runtime
would never have sent it a single packet. The documents were not lying about
intent - they were written as if the plan had already happened.

Two habits follow, and they are now hard rules in section 2: **mark every claim
with its evidence**, and **never mark a milestone complete without a human
observation on hardware**.

### The June 2010 DirectX SDK cannot build a D3D8 project

August 2007 was the last SDK to ship `d3d8.h` and `d3d8.lib`. A D3D8 project
that lists `$(DXSDK_DIR)` as its include path and links `d3d8.lib`/`d3dx8.lib`
will not compile against a current installation. We use mingw-w64's `d3d8.h`,
dynamic-load `d3d8.dll`, and do not use D3DX at all.
Source: <https://walbourn.github.io/directx-sdks-of-a-certain-age>
