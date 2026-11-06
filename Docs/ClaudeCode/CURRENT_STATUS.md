# CURRENT_STATUS - XOPS-WXR

**As of 3 Nov 2026.** This file records **state, not intent**. A row moves to
"working" only after the owner has observed it on a Quest 3. Nothing is
"working" today.

> **5 Nov 2026 update - see `Docs/OpenCode/DeepSeek41Flash/README.md` section 2d.**
> The borderless/fullscreen window, the red sync pixel and the real-FOV change
> are now implemented and **desktop-verified** (the sync value echoed exactly:
> captured `RGB(120,0,0)` and `RGB(36,0,0)` at screen `(0,0)`, including in
> gameplay). The working display path is the game's **own fullscreen config**
> (`config.dat` byte `0x13 = 01`), not a stretched 1920x1080 window. **No Quest 3
> verification has happened yet.** The owner also confirmed on 5 Nov that
> **`pitchSign +1` gives the correct up/down direction** on the fullscreen
> build, so the saved INI is right and the older "`-1` settled" note is
> superseded for this setup. **MODE_VR is now driven by the game's own state**
> (`0x490A44` mission index, `0x490A48` result: 1 = victory, 2 = death,
> `0x48DDD8` player index), and two deaths plus one victory were verified on
> the desktop; DUEL_EXT's low draw count no longer drops head tracking. The
> rows below dated 3 Nov still describe the code before this pass.

---

## One-line summary

**The camera is solved.** A diagnostic probe is built and running on the
desktop, and all five camera fields - X, Y, Z, yaw and pitch - are confirmed
on hardware by driving each one and watching the view respond [device, runs
8-10, 3 Nov 2026]. Roll is handled by rotating the rendered frame instead of
by a field (owner). This was the project's largest risk.

No VR code exists yet and nothing has run under WinlatorXR. The route is
settled - **mod the real `XOPS.exe`**, not OpenXOPS (owner, 31 Oct 2026).

## Milestones

| # | Milestone | State |
|---|---|---|
| 0 | Intake complete | **in progress** - route, install path, injection vector, locale and input design all settled; a few `TBD`s left, none blocking |
| 1 | Unmodified XOPS runs flat in a container | not started |
| 2 | Our DLL loads and logs a build stamp | not started |
| 3 | VR resolution, borderless, fills the display | not started |
| 4 | XrApi 0.5 client parses a real device packet | not started |
| 5 | Sync pixel drawn last, echoing `HMD_SYNC` | not started |
| 6 | **6DOF head tracking feels locked** | not started |
| 7 | Presentation mode chosen and measured | not started |
| 8 | Aim decoupled from the head; weapon decoupling | not started - **required**, not optional, by the owner's input design |
| 9 | Controller input mapping, owner-confirmed | not started |
| 10 | HUD scale, comfort settings, settings surface | not started |

## Subsystems

| Subsystem | State | Notes |
|---|---|---|
| Repository skeleton | partial | `src/d3d8probe.cpp` + `.def`, `build/build-probe.bat`. No mod yet |
| **d3d8 diagnostic probe** | **run once on the desktop, 2 Nov 2026** | `src/d3d8probe.cpp`. Loaded, hooked, logged 1302 frames. v2 built and deployed, not yet run |
| Build and deploy script | partial | `build/build-probe.bat` builds and deploys the probe with a verified SHA-256. The mod's own script still needs the build-stamp freshness guard. Deploy default: `E:/tmp/Q3Mods/xops096` |
| DLL proxy / injection | **proven on the desktop** | the `d3d8.dll` proxy loads from the game directory, forwards to `C:\Windows\system32\d3d8.dll`, and its vtable patches take. Not yet proven under Wine |
| Logging + build stamp | **done, in the probe** | line 1 of the log is `built Nov  2 2026 21:30:55`. Carry the same pattern into the mod |
| XrApi 0.5 client | absent | the sibling port's `XRAPI.cs` is the reference |
| Handshake files | absent | **`version` = `0.5` is the single most important line of the project**; without it the runtime sends nothing at all **[runtime]** |
| Sync pixel | absent, **plan confirmed viable** | `Clear` with a 10x10 `D3DRECT` last before `EndScene`; `EndScene` is once per presented frame **[device]**, and the back buffer is not lockable, so this is the only route |
| Window / resolution override | absent | **harder than planned**: the game projects to 640x480 itself, so a bigger back buffer does not enlarge the view. Window de-chroming still works normally |
| Camera (mono 6DOF) | **not reachable through D3D; hunting it in memory** | in-game the world is software-transformed, so the device matrices do nothing **[device]**. The camera must be driven from the game's own variables; probe v5 is hunting them differentially. Handedness settled (**left-handed**); scale ~1 unit = 1 m, unconfirmed |
| Controller input | absent | design settled: head drives view **and** movement direction; **aim is dominant-hand motion aim**, fully independent of the head. See F-7 |
| HUD scale | absent | |
| Stereo (AER / SBS) | out of scope | revisit only after mono is device-verified |

## What we know about XOPS, and how well

| Claim | Evidence | Confidence |
|---|---|---|
| Native 32-bit C/C++, not .NET | dnSpy reported "not a .NET assembly" **[MetaAI]** | high |
| Direct3D 8.1; `Direct3DCreate8` string at `.rdata:0045D1DC`, IAT `00463BA0`, SDK version `0xDC` | IDA **[MetaAI]** | high - and cheap to re-confirm from the import table |
| Not Genesis3D - in-house Team MITEI engine | no `geCamera` symbols **[MetaAI]** | medium-high |
| `sub_4035D0` is the D3D init; `CreateDevice` called via `[edx+3Ch]` at `loc_403645` | IDA **[MetaAI]** | high; index 15 = `0x3C` is correct for `IDirect3D8::CreateDevice` **[derived]** |
| Device created at 640x480 | IDA **[MetaAI]** | high |
| `dword_48C5BC` = device pointer (102 xrefs), `dword_48C5B8` = `IDirect3D8`, `48C3C8` = present parameters | IDA **[MetaAI]** | medium - plausible, unverified here |
| `sub_401120` is the main frame loop | IDA **[MetaAI]** | medium |
| `SetTransform` is vtable index 37, offset `0x94` | **[device]** - hooked, and calls arrived | settled. The inherited `vtbl[44]` is `SetLight` |
| `SetTransform(PROJECTION)` returns to `0x00403AB9`; `SetTransform(VIEW)` returns to `0x00403CA8` | **[device]** | settled. `4013AB9` was a mangled `403AB9` - the digits were right; `0x403AA4` is where the argument setup starts, `0x403AB9` is the return address |
| Three `WORLD` call sites: `0x00414273`, `0x004021B8`, `0x004143A3` | **[device]** | settled |
| `CreateDevice` returns to `0x0040369A` | **[device]** | settled - adjacent to the first attempt's `loc_403645` |
| Device: 640x480 windowed, X8R8G8B8, D16, HAL, hardware vertex processing, swap effect COPY, **vsync ON** | **[device]** | settled. Vsync matters later - a VR target wants 72/90 Hz |
| The back buffer is **not lockable** (`Flags = 0`) | **[device]** | settled - **`BUG_FIXES.md` D-7 confirmed on hardware**: the first attempt's `LockRect` sync pixel could never have worked |
| Projection: fovY **65 deg**, fovX 80.7 deg, aspect 4:3, near **0.1**, far **80.1** | **[device]** | settled |
| The engine is **left-handed** | **[device]** - `(right x up) . forward = +1.0000` | settled. No handedness guessing needed |
| Window carries `WS_CAPTION`/`WS_BORDER`; client 640x480 in a 646x509 window on a 1920x1080 screen | **[device]**, and visible in the screenshots | settled - F-2 is required exactly as planned |
| HUD is FVF `0x0144` = XYZRHW \| DIFFUSE \| TEX1, stride 28, first set from `0x00404A94` | **[device]** | settled - the handle for F-8 |
| Input is live: `GetKeyboardState` 1302 calls, `GetCursorPos` 1347, `SetCursorPos` 1188, over 1302 frames | **[device]** | settled. **D-17 confirmed in execution**, not merely in the import table |
| `EndScene` is called **exactly once per presented frame** | **[device]** - 1 Begin, 1 End, 1 Present on every sampled frame | settled - the sync square goes in `EndScene` |
| The game sets its **projection** matrix through `SetTransform` | same **[code]** | high. This is the anchor for FOV and the near plane later. **Which address** is unresolved - see the open question below |
| `sub_401120` is the **main render function** | `sub esp, 220h` = 544 bytes of locals, holding the `D3DMATRIX` / `D3DVIEWPORT` / present-params structs it builds before calling out **[code, 2 Nov 2026]** | high - and it matches the first attempt's "main frame loop" |
| `SetRenderState` is index 50, offset `0xC8`; a block of them at `0x4039C9` | **[code]** for the site; **[derived]** for the name | high - `0xC8` is index 50 in the same vtable enumeration that puts `SetTransform` at `0x94`, so the two confirm each other |
| The draw call is `DrawPrimitiveUP`, offset `0x120` (index 72), in the `loc_401396` loop | **[code]** for the site; **[derived]** for the name | high. **This is a real finding**: XOPS submits geometry from system memory per call, with no vertex buffers. It is how 2D HUD drawing will be recognised later (`D3DFVF_XYZRHW`), and it is relevant to performance |
| The game sets its **view** matrix through `SetTransform(D3DTS_VIEW)` | strongly implied - the projection site is confirmed and the two are normally set together - but the `push 2` site has not been located yet | medium-high. Find the `push 2` / `call [...+94h]` site; it is probably adjacent |
| The game loads `dinput.dll` | asserted **[MetaAI]** | low - and moot: we proxy `d3d8.dll`, and the owner has confirmed that override works **[device]** |
| The matrices live in `sub_401120`'s stack locals (`var_1EC`, `var_1E8`, `var_1E4` at `-1ECh` onward) | **[code]** | medium - a `D3DMATRIX` is 64 bytes, so consecutive 4-byte locals from `-1ECh` is the right shape. Not load-bearing: the wrapper sees the matrix as an argument |
| XOPS keeps a player aim angle separable from the render camera | unknown | **none - and F-7 depends on it.** Settle in IDA Free: xrefs on the camera's yaw global say whether the weapon code reads the same variable |
| Metres-to-XOPS-units scale is ~50 | guess **[MetaAI]**, no measurement | **none - measure it** |
| XOPS exposes any resolution option | unknown | none |
| No Japanese locale or codepage needed | owner, 31 Oct 2026 **[device]** | high |

## THE CAMERA IS SOLVED [device, runs 8-10]

**All five fields are confirmed by driving them and watching the view move.**

| Address | Field |
|---|---|
| `0x0048C3B0` | Y, vertical position |
| `0x0048C3B4` | horizontal axis 1 |
| `0x0048C3B8` | horizontal axis 2 |
| `0x0048C3BC` | yaw |
| `0x0048C3C4` | pitch |

Writes take effect from `BeginScene`, in cluster A. The history below is kept
because the route to getting here was not straight, and two of the wrong turns
are worth not repeating.

### How the first one was found [device, run 8]

**`0x0048C3B0` holds the player's vertical position (Y).**

Confirmed on hardware on 3 Nov 2026: the owner sine-drove that address by
+/-0.5 units through the probe's active-probe mode and **the view rose and fell
on screen**. His words: "I pressed 9 and it started controlling up/down", and
on being asked, "vertical not pitch" - the whole view was lifted, the horizon
did not rotate. So this is a translation, not a tilt.

Two things follow, and the second is as important as the first.

1. **The in-game camera can be driven from memory.** Everything up to run 7
   was inference from correlation. This is a write that changed what was
   rendered, observed by the owner. F-5 has a route.
2. **Cluster A is authoritative, not a stale copy.** Runs 7 and 8 found two
   clusters of statics holding the same position to within 0.007, and which
   one to write to was open. A write to A changed the image, so A is not a
   per-frame copy that gets overwritten. [device, for Y only - the other
   fields are still unconfirmed]

### The two clusters, as they stand

Addresses are IDA VAs; ASLR is off in this binary [device], so they are fixed.

| Address | Observed range | Reading | Status |
|---|---|---|---|
| `0x0048C3B0` | 1.929 .. 1.934 | **Y, vertical position** | **CONFIRMED [device, run 8]** |
| `0x0048C3B4` | 5.82 .. 14.44 | **one horizontal axis** | **CONFIRMED [device, run 9]** |
| `0x0048C3B8` | -26.88 .. -20.86 | **the other horizontal axis** | **CONFIRMED [device, run 9]** |
| `0x0048C3BC` | 0.597 .. 3.345 rad | **yaw** | **CONFIRMED [device, run 10]** |
| `0x0048C3C4` | -0.524 .. +0.332 rad (-30.0 deg .. +19.0 deg) | **pitch** | **CONFIRMED [device, run 10]** |
| `0x0048E0A8` | -26.89 .. -20.87 | Z, mirror of A | [unverified] |
| `0x0048E0B0` | 5.82 .. 14.44 | X, mirror of A | [unverified] |
| `0x0048E0B4` | 0.597 .. 3.345 rad | yaw, mirror of A | [unverified] |
| `0x0048E0BC` | -0.524 .. 0.048 rad | pitch, mirror of A | [unverified] |

The owner also reports [device, run 7] that sine-driving members of these
clusters moved **X, Z, pitch and yaw** on screen - but run 7 had no way to
record which address produced which effect, so those are not attributed yet.
That is what the labelled-tag keys added in probe v13/v14 exist to settle.

Note the layout is **not** a contiguous `(x, y, z)`: Y sits *before* X, and in
cluster B the X and Z floats are 8 bytes apart rather than 4. These are
separate fields of a struct, not a vector. The XYZ-shape filter built in v11
was therefore looking for something that is not there - it cost nothing, but
it was not what found the answer. [derived]

### The position triple is complete [device, run 9]

The owner sine-drove both horizontal fields at a yaw of 169.8 degrees and
reported what he saw: `0x0048C3B4` slid him **front/back**, `0x0048C3B8` slid
him **left/right**. At that yaw the facing is almost exactly axis-aligned
(`|cos| = 0.98`, `|sin| = 0.18`), so the two are cleanly distinguished rather
than ambiguous: `0x0048C3B4` is the axis he was facing along and `0x0048C3B8`
is the perpendicular one.

So all three position components are now confirmed by observation on screen.
Which of the two horizontal fields deserves the name "X" is a labelling
question that only matters for matching the view matrix, and is not settled.

### Yaw and pitch are confirmed, and the camera is fully solved [device, run 10]

The owner sine-drove both and reported: numpad 4 (`0x0048C3BC`) is yaw, numpad
5 (`0x0048C3C4`) is pitch. Writes took effect at the default `BEGINSCENE`
phase.

**The derived-vector hypothesis is disproved.** The concern was that the game
might turn yaw into a forward vector during its update, making a write in
`BeginScene` too late to matter. It is not: writing the angle after the update
and before the draw changes what is rendered. So the game reads these angles
during its own transform step, and the write phase machinery added in v15 was
not needed. It stays in the probe because it cost little and it is the right
instrument if some other field later refuses a write.

**Cluster B does need resolving, and run 11 resolved it the other way round.**
See the next section - the earlier reading here was wrong.

### A is the render copy, B is the player [device, run 11]

Writing A turned the view, so runs 8-10 concluded A was authoritative and B
could be ignored. Run 11 showed that is backwards in the way that matters.

With steering on, **B's yaw froze at the recentre value and never moved
again** while A's tracked the head:

```
WATCH  0x0048C3BC   2.4016 = 137.60 deg   A+0C  YAW    <- written, moves
WATCH  0x0048E0B4   2.3108 = 132.40 deg   B+0C  YAW    <- frozen at recentre
```

Before steering the two were identical every frame. So:

- **B is the player's own orientation**, updated from the mouse. We suppress
  the mouse while steering, so B stopped changing.
- **A is a copy the game makes during its update.** Our write lands after that
  copy and before the draw, which is why it changes the image and nothing
  else.
- **Movement reads B.** The owner's symptom - "the UDP controls seemed good,
  just couldn't move forward relative to head yaw" - is exactly this: walking
  kept going in the direction he faced when steering started.

Position is unaffected: B's position tracked A's to within 0.007 all session,
so only orientation diverges.

**So the mod writes both.** B is what makes walking follow the head, which is
the owner's input design ("forward = where you are looking"); A as well so
the current frame renders correctly rather than one frame late. Probe v18
defaults to both and can write either alone for comparison.

### Settled conventions [device, run 11]

| Convention | Value | How |
|---|---|---|
| pitch sign | **negative** - XOPS pitch is positive-down | owner's direct report [device, run 13] |
| yaw sign | positive | unchanged through runs 11-12 |
| cluster | both | from the freeze above |

These are persisted to `xops_wxr_steer.ini` beside the DLL, so they are
recorded once rather than rediscovered each session. The DOF mode is
deliberately **not** persisted: it is a choice about how much to drive today,
not a fact about XOPS, and saving it meant run 12's "yaw+pitch only" would
reload over a better default.

**I got the pitch sign wrong twice, the same way both times.** Runs 11 and 12
each ended on `+1` after the owner flipped the sign, and I read that history
as evidence for `+1`. It was not - he was comparing, not settling, and on run
13 he reported plainly that `+1` was "still up/down reverse". A toggle history
is not evidence; the person looking at the screen is. Recorded because the
mistake was in the reasoning, not the data.

### Settled: VR-exclusive input during gameplay [owner, 3 Nov 2026]

Once the XrApi path is fully running, **every desktop input is dead during
gameplay** - no mouse, no WASD, nothing. **ESC is the single exception**, so
the player can still reach the menu and quit. All other input arrives from the
headset and controllers over UDP.

Menus are deliberately not suppressed: there the player wants ordinary input,
and `MODE_VR = 0` hands mouse control back to the runtime anyway.

Two things make this cheap rather than fiddly:

- We already own `GetKeyboardState` and supply the array the game reads
  [device], so suppression is zeroing bytes, not fighting the OS. The cursor
  is already pinned to its last `SetCursorPos` value by the same mechanism.
- **A gameplay detector, but heuristic rather than free - I overclaimed
  this.** I said `SetTransform == 0` separates gameplay from menus, on the
  strength of every run since 4. It separates gameplay from the **title**,
  which transforms 25-35 times a frame. It does **not** separate the
  in-mission menu that ESC opens: run 13 caught that screen reporting zero
  SetTransform calls and 231 UP draws, so the single-signal detector called a
  menu "gameplay" - which with VR-exclusive input on would have left the
  player unable to use that menu.

  The draw mix still separates all three with real margin [device]:

  | Screen | `DrawIndexedPrimitive` per frame |
  |---|---|
  | gameplay | 84-88 |
  | in-mission menu | 28 |
  | title | 26-48 |

  so v22 requires `SetTransform == 0` **and** `DrawIndexedPrimitive >= 60`.
  That is a heuristic about this build's draw counts, marked `[unverified]`,
  not a fact about the engine. **The right answer is the game's own
  menu/playing flag**, which is one walk-and-press-ESC scan away with the
  probe machinery that already exists. Until that is found, VR-exclusive input
  should not be trusted to release the keyboard in menus.

Implemented in probe v21, **off by default** and toggled with `.`. It has to
stay off until VR input is complete: with it on and no pose arriving, the game
is unplayable by design. The order inside the keyboard hook matters and is
deliberate - the probe's own control keys are read out first, then the game
loses the desktop keyboard, then VR input is added back.

### Consequence: F-5 has a complete route

Full 6DOF head tracking is now reachable with no unknowns in the camera path:

| Head DOF | How |
|---|---|
| position X, Y, Z | write `0x0048C3B4`, `0x0048C3B0`, `0x0048C3B8` [device] |
| yaw | write `0x0048C3BC` [device] |
| pitch | write `0x0048C3C4` [device] |
| roll | rotate the rendered frame (owner, 3 Nov 2026) |

This was the project's largest risk - `PROJECT_PLAN.md` called the camera the
hardest problem in the port, and the first attempt never located it at all.

### The next real design question

The game keeps applying its own mouse deltas to yaw and pitch every frame. If
we write head orientation there too, the two fight. The resolution is already
in hand and is what the input design calls for anyway: **we own
`GetKeyboardState`, `GetCursorPos` and `SetCursorPos`** [device, all patched
and called every frame], so we can suppress the game's own look input entirely
and supply movement from the head while driving aim from the dominant hand.

Per the owner's input design (31 Oct 2026), the head drives view and movement
direction while aim comes from the controller - so these two paths must stay
separate, and yaw written for the view must not drag the weapon with it. That
is F-7 and it is now the hardest remaining problem, rather than F-5.

### Positional DOF is not yet testable, and roll is not built

Two gaps that are **not** failures of the camera work, and neither is
blocked:

- **Positional DOF was written wrongly at first, and is now a delta.** The
  first implementation wrote `recentre reference + head offset`, an absolute
  value - which cancels the game's own movement, because the game steps the
  player during its update and we then overwrite the result from a reference
  captured seconds earlier. **Walking would have stopped** the moment
  positional DOF was switched on; `DOF_LOOK` being the default is the only
  reason no run showed it. v20 applies the head as a delta against the offset
  applied last frame, on top of wherever the game currently has the player,
  so walking, gravity and stairs all survive and the head only contributes its
  own motion. Full 6DOF is now the default mode.
- **Horizontal 6DOF works; height does not, and cannot this way** [device,
  run 13]. With the head moving and no keys or sticks held, the two horizontal
  fields tracked it. Height did not: the head moved through a 1.4 m vertical
  range and `0x0048C3B0` moved **0.028**. XOPS re-clamps the player to the
  ground every frame, so a delta written in `BeginScene` is undone before the
  next one arrives. This is the game asserting its own physics, not a bad
  write.

  So **crouching and standing cannot be driven by writing Y.** Options, none
  yet tried: find a separate eye-height or crouch field; write Y later in the
  frame than the clamp; or accept a fixed eye height and let the game own it.
  v22 detects the clamp and says so in the log rather than leaving two of
  three axes silently dead.
- **Positional DOF beyond that has not been exercised yet.** Floats 22-24 were constant
  across every packet in run 11, so the recentred offset was 0,0,0 and
  translation could not be judged. That was the owner driving the tester by
  hand and starting with look and thumbsticks, **not** a limit of the tester -
  it can send position (owner, 3 Nov 2026). The probe now says when the
  incoming position is not varying, so a flat zero reads as "nothing is being
  sent" rather than as broken code.
- **Roll does nothing yet** [device, run 11] - correctly, because it is not
  implemented. It is not a field to write; see below.

### Roll: settled by rotating the frame, not by finding a field [owner, 3 Nov 2026]

XOPS almost certainly has no roll field - the view never rolls, so there was
no reason to store one. The owner's decision is to **rotate the entire
rendered display instead**, which is not a workaround but the mathematically
correct thing: a camera roll about the view axis *is* a 2D rotation of the
rendered frame.

Two costs were considered and one dismissed:

- Rotating a 4:3 image leaves empty corners, and the baked 640x480 with a
  CPU-side projection means we cannot widen the FOV to compensate. **Not a
  problem**: with `MODE_VR` true the eye never sees the panel corners (owner).
- The back buffer is not lockable [device], so getting the frame back as a
  texture to draw rotated needs `CopyRects` to an off-screen surface rather
  than a lock. This is the real work item. [derived]

**So no roll hunt is needed.** This removes a whole search from the plan.

### Pitch range is a useful cross-check

`-0.524` rad is `-30.02` degrees, which is a clean clamp value and strong
evidence that field is an angle in radians rather than a direction-vector
component. [derived]

### Y does not equal the eye height

`0x0048C3B0` rests at ~1.932, while the view matrix put the eye at Y 2.210 to
3.486 across runs. The ~0.28-1.5 gap means this is a body or feet origin with
the eye offset added later, **or** the view matrix figures are not comparable
because they are stale after frame ~59. Unresolved, and it matters for F-5:
driving HMD height means knowing which origin we are writing. [unverified]

---

## Where the camera question stood before run 8, after four probe runs

Three of my own conclusions have been wrong in a row. This section states only
what the data supports, and names the mistakes so they are not repeated.

### Established [device]

| Finding | Evidence |
|---|---|
| In-game, **all three draw paths are used**: ~93 `DrawPrimitiveUP`, ~84 `DrawIndexedPrimitive`, ~8 `DrawPrimitive` per frame | run 4 `DRAWPATH` |
| In-game, **every draw carries FVF `0x0144` (XYZRHW)** and `SetTransform` is called **zero** times per frame | run 4 |
| At the **title screen** the picture is the opposite: 40 of 44 draws are XYZ (`0x0102`, `0x0142`), with 25 `SetTransform` per frame | run 4 frames 0-2 |
| The view matrix updates smoothly up to about **frame 59**, then stops for the rest of the session | runs 3 and 4 |
| While it updates, it describes a real camera - a fall under gravity settling at a floor, plus a constant 0.070 units/frame walk | run 3 |

**The consistent reading:** XOPS uses the device's matrices for its menu and
title 3D, and **transforms the in-game world itself on the CPU**, submitting
pre-transformed vertices through all three draw calls. The view-matrix updates
that end at frame 59 belong to the pre-gameplay camera, not to play.

So the practical conclusion from run 2 stands after all: **the in-game camera is
not reachable through Direct3D.** What was wrong in run 2 was the *reasoning* -
"zero in four sampled frames" is not "never" - and the fix for that was
hooking all three draw paths, which is what made the conclusion solid.

### My mistakes, recorded so they are not repeated

1. **Run 2:** concluded "SetTransform is never called in-game" from four
   sampled frames. It is called - just not during play.
2. **Run 3:** over-corrected to "the camera is live in the view matrix", which
   was true of frames 0-59 and false of gameplay.
3. **Run 4:** the memory hunt "found" three addresses that tracked the camera
   perfectly. **All three were the probe's own memory** - two stack copies of
   an `eye[3]` local and the `g_lastEye` global in our own DLL. A scanner that
   searches its own process and does not exclude itself will always find
   itself first, and it looks exactly like success.

### The hunt: v5 worked mechanically and answered wrongly

Run 5 narrowed 4552 candidates to exactly 1 over fourteen intervals, and the
survivor was **`0x005DEFA0`, holding a constant `0.012` with zero neighbours**
- about 1/83, which is a frame-time delta, not a world coordinate. A clean
funnel to a confident wrong answer.

Two design faults, both mine:

1. **The STILL test demanded exact equality.** A standing player still has
   gravity resolved every frame, so the position jitters in the low bits. The
   first still interval therefore deleted the real camera. (1367 -> 244 at
   interval 11 is that happening.)
2. **Elimination has no way back.** One bad interval is fatal - and bad
   intervals are easy to produce: stopping partway through a "moving" interval,
   or walking a loop back to the same spot, both make a position look
   unchanged.

### What probe v6 does instead

**Scores instead of eliminating.** Nothing is ever removed; every candidate
carries a running tally, +1 when its behaviour matches the interval and -1
when it does not. An odd interval costs a point rather than the answer.

- tolerant thresholds: under 0.0008 counts as "did not move", over 0.005 as
  "moved", so low-bit jitter no longer disqualifies;
- the initial pool ignores floats that merely jitter;
- **triple detection**: for each high scorer, it checks whether the next two
  floats score as well. A stored position is almost always `x,y,z` together, so
  `triple=2` is far stronger evidence than any single float;
- reports the top dozen every fourth interval, so the picture builds up instead
  of arriving once at the end.

### The parallel route, which may well be faster

The scanner is one way in. The other is IDA, and the project now has a precise
starting point it did not have before: **`0x00406189` is the single call site
every in-game draw passes through**, and the EXE has **no ASLR** (base
`0x400000`), so every address is literal.

Working back from that draw call reaches the software transform, and whatever
globals it reads for the camera are the target. That is a bounded,
well-defined IDA task, and it does not depend on getting a scanner's heuristics
right.

## Open questions in the disassembly

1. ~~Two different addresses for the `push 3` / `SetTransform` call.~~
   **RESOLVED 2 Nov 2026 from the PE header**: `.text` runs `0x401000` -
   `0x45D000`, so `0x403AA4` is valid code and `4013AB9` is far outside the
   image - a transcription slip, as suspected. **Still open:** whether there is
   *also* a second, per-frame projection site inside `sub_401120`. The probe
   answers that directly - it logs every distinct `SetTransform` call site as an
   IDA address, and prints the full list at shutdown.

   Original finding, for the record:

   **Two different addresses have been reported for the `push 3` / `SetTransform`
   call.** The 31 Oct reading gave `0x403AA4`; the 2 Nov reading gave
   `4013AB9`, which cannot be right as written - it is seven hex digits, far
   outside a 32-bit image based at `0x400000`. Most likely an OCR or
   transcription slip for something like `0x4013AB` or `0x401AB9`.

   Worth resolving rather than picking one, because **both could be real**: an
   engine commonly sets the projection once at device creation and again per
   frame, and `0x403AA4` sits near the `sub_4035D0` init code while a
   `0x401xxx` address sits inside `sub_401120`, the render function. If there
   are two, the per-frame one is the one that matters for FOV.

2. **The `D3DTS_VIEW` (`push 2`) site has still not been located.** It is the
   same `call [...+94h]` shape and is probably adjacent to the projection one.
   Whatever builds the matrix it passes reads the camera globals - the input to
   feature F-7 test 1.

3. **One claim to discard.** The 2 Nov analysis suggests the
   `lea edx, [esp+230h+var_220]` immediately before `call [ecx+120h]` "will have
   the matrix data". It will not: `0x120` is `DrawPrimitiveUP`, whose third
   argument is `pVertexStreamZeroData` - that `lea` is pointing at **vertex
   data**, not a matrix. Matrices reach the device through `SetTransform`
   (`0x94`), never through a draw call. Following that advice would mean
   logging vertices while believing they were matrices, which is an expensive
   kind of wrong.

## Inherited code

`Docs/MetaAI/XOPS_WXR/` - **does not compile, and would not work if it did.**
Two `DllMain` definitions; links `d3d8.lib`/`d3dx8.lib` from an SDK that has
not shipped Direct3D 8 since August 2007; and implements a protocol that is not
XrApi. 15 defects catalogued in `BUG_FIXES.md`. Keep the IDA findings, rewrite
the code.

## Environment - unknown and needed

Known:

- [x] Install paths: **`E:/tmp/Q3Mods/xops096`** (desktop test, deploy
      default) and `D:/VR/xops096-wxr` (WinlatorXR copy). The same
      `xops096.exe` byte-for-byte, so findings transfer between them
- [x] `d3d8.dll` DLL override works in the owner's container - **the injection
      vector is settled**. The exact setting and its location still need writing
      down for the installer
- [x] No Japanese locale or codepage needed

Still to ask at checkpoint 0/1:

- [ ] WinlatorXR version, container resolution, DXVK or WineD3D, Box86 settings
- [ ] Where exactly the `d3d8.dll` override is configured, in reproducible steps
- [ ] Whether `adb` and `scrcpy` are available
- [ ] Visual Studio 2022 present? Any DirectX SDK installed? (we need none)
- [ ] How long one deploy-and-test cycle takes

## Next action

**Run probe v7** (built and deployed), which adds an **active probe**: drive a
sine wave into one candidate variable at a time and let a human judge the
result. This is the owner's idea and it is a better instrument than the passive
scanner, which has now twice produced a confident wrong answer. Nothing in a
game oscillates smoothly on a 1.5-second cycle unless we are making it, so the
signal is unmistakable.

**It works in two phases.**

**The strongest filter is a negative one (owner's observation, 2 Nov 2026).**
The player does not move unless a key is held and does not turn unless the
mouse moves. So **any variable that changes while there is no input at all
cannot be the camera** - it is a timer, an animation phase, an enemy, a
particle, or physics on something that is not us.

That matters because it is a negative test. "It changed while I walked" has
thousands of coincidental matches; "it stayed perfectly still while I did
nothing, every time" has very few. It also cleanly removes the class of thing
that beat the scanner twice - the frame-time delta that survived run 5 would be
vetoed instantly.

Idle is detected exactly, not approximately: no key or mouse button down (both
live in the same `GetKeyboardState` array), and no cursor movement. We own both
`GetCursorPos` and `SetCursorPos`, so the delta we compute is the same one the
game computes for its own look. A run must last 12 frames before it is trusted,
so a player who has just released a key or just landed is not mistaken for an
idle one.

**A second structural filter, from the same constraint.** The owner plays
single-player on flat ground and never jumps, so **the player's height is a
constant**. A stored position is three consecutive floats, and on flat ground
two of them move together while the middle one never changes at all. Two movers
astride one constant is a far rarer pattern than any single moving float.

The probe tracks each candidate's immediate neighbours and whether they have
stayed perfectly constant, then ranks the list by shape before score:

```
PROBE  [ 1] 0x... score=8 moves=301 range  -47.854..-40.213  <<< X of XYZ, constant Y next to it
PROBE  [ 1] its constant neighbour holds  2.2340 - that is the player height if this is the position
```

**Y does not need its own hunt** - it falls out of the struct. Find X and Z and
the float between them is the height, which the probe reports directly. That
value is also checkable against what we already know: the view matrix put the
in-game eye at Y 2.21-2.48 across runs, so a constant neighbour near there is
strong corroboration rather than a coincidence.

*Phase 1 - build the candidate list (same as before).* In a level: stand still
~5s, walk ~5s, stand still ~5s, repeated four or five times. This scores the
candidates **and runs the idle veto**.

**The still periods now matter more than the walking.** Hands completely off
the keyboard and mouse - any held key or mouse movement ends the idle run and
the veto stops accumulating. Watch the `CAMHUNT interval` lines: the `vetoed by
idle` count should climb steadily. If it stays at 0 and the probe list warns
that no idle run was observed, something is still generating input.

*Phase 2 - probe.* Press **9** (not a function key - XOPS uses those). The
probe freezes the scanner, drops every candidate that never actually moved or
is no longer addressable, takes the best 256 by score, and starts wiggling the
first one.

| Key | Effect |
|---|---|
| **9** | toggle probe mode |
| **8** | next candidate (restores the previous one first) |
| **7** | previous candidate |
| **0** | **tag** the current candidate - logged as `TAG #n` with its address and range |
| **-** / **=** | halve / double the swing |

**Movement is deliberately untouched.** An earlier version put these controls
on W/Q/S/A/D and swallowed them, which left the player unable to walk while
probing - and walking is the most useful test there is, because a camera
variable tracks the player and a coincidence does not. Only the six keys above
are swallowed; W/A/S/D and the mouse work normally throughout.

**Each variable is swung across its own observed range**, not by an arbitrary
amount. The probe records the highest and lowest value every candidate has
been seen to hold, then oscillates between them. A coordinate that lives
between -47 and -40 gets swung across that span; an angle gets swung across
its span. This is both more informative - the motion is in proportion to what
the variable actually does - and much safer, because most of what looks like
"writing to memory crashed the game" is really "writing an absurd value
crashed the game".

**These keys are swallowed - the game never sees them.** That is only possible
because we own `GetKeyboardState`, and it matters: pressing W must step the
probe, not walk the player somewhere else while the owner is trying to judge
what changed.

Each candidate's original value is saved on selection and restored when
leaving it, so a wrong guess does not accumulate. Writes happen in
`BeginScene` - after the game's own update for the frame, before it transforms
and draws, which is the window where a camera variable still affects this
frame's image.

**What to look for**, in rough order of value:

- the whole view sliding smoothly back and forth along one axis -> **camera
  position**;
- the view swinging left/right or up/down -> **a view angle**;
- the weapon or HUD moving while the world stays put -> viewmodel or HUD,
  worth tagging separately;
- geometry distorting or the world scaling -> a transform parameter, possibly
  FOV;
- nothing at all -> the variable is recomputed every frame before use, or is
  not read on this path.

Tag anything interesting with **S** and describe each tag afterwards; the log
records the address against each tag number so the descriptions can be matched
up later.

**Crash-proofing, since we are writing into live game memory:**

- every read and write is fault-guarded, and a faulting candidate is dropped
  rather than retried;
- each address is re-checked as committed and writable before it is selected,
  because heap blocks get freed and a candidate list outlives the memory it
  points at;
- written values are clamped to the observed range plus one swing, so nothing
  wilder than the variable has already held can reach the game;
- the original value is restored when leaving a candidate, when probe mode is
  switched off, **and on DLL unload**, so a crash mid-probe does not leave a
  corrupted value behind;
- after eight faults the probe disarms itself.

It can still destabilise the game - a value can be perfectly in-range and
still wrong for the game's logic. If it dies, note which index it died on.

## Changelog

| Date | Change |
|---|---|
| 2026-11-02 | **Probe v7: active probing**, on the owner's suggestion. Drives a sine wave into one candidate at a time; F9 arms it, W steps, S tags, A/D set amplitude - all swallowed before the game sees them, so stepping does not move the player. Replaces judging by correlation with judging by eye. |
| 2026-11-02 | **Probe run 5: the differential scanner ran correctly and gave a wrong answer.** 4552 -> 1, and the survivor was a frame-time delta (constant 0.012). Cause: the STILL test used exact equality, so low-bit jitter from gravity deleted the real camera at the first still interval, and elimination cannot recover. v6 replaces elimination with scoring plus triple detection. Movement-key detection confirmed working (`KEYS vk 0x57 'W'`). |
| 2026-11-02 | **Probe run 4 (695 frames) settles the draw-path question.** All three draw paths are used in game - 93 UP, 84 indexed, 8 direct - and **every one carries FVF 0x0144 (XYZRHW)** with zero `SetTransform` calls. The title screen is the opposite: 40 of 44 draws are XYZ with 25 transforms. So XOPS uses the device matrices for menu 3D and **software-transforms the in-game world**. Run 2's conclusion was right; its reasoning was not. Also: the run-4 memory hunt's three "perfect" candidates were **the probe's own stack and globals** - self-contamination. Probe v5 replaces it with a differential scanner over `MEM_PRIVATE` only, keyed off the movement keys. |
| 2026-11-02 | **Probe run 3 (325 frames) OVERTURNS run 2's conclusion.** `SetTransform(VIEW)` *does* track the live camera - the hunt log shows it moving smoothly frame by frame, a player falling under gravity and walking at 0.070 units/frame. Run 2's "never called in-game" was a sampling artefact. Also caught: the probe only hooked `DrawPrimitiveUP`, so "every draw is XYZRHW" was never a claim about all draws. First unit-scale estimate: **~1 unit = 1 metre**. The `.data` camera hunt found nothing - expected, the camera is likely on the heap. Probe v4 hooks all three draw paths and scans all memory with frame-to-frame narrowing. |
| 2026-11-02 | **Probe run 2 (367 frames) settles the big question - badly.** Every draw in every frame is `D3DFVF_XYZRHW`; `SetTransform` is never called in-game; the camera froze at frame 52 while the player moved. **XOPS is a software-T&L engine.** The `SetTransform(VIEW)` camera route is dead and the resolution override no longer works as planned; the proxy, sync pixel, window and input plans all survive. Probe v3 built: hunts the camera in `.data` using the level-load eye position `-18.402 3.481 4.180`. |
| 2026-11-02 | **Probe run on the desktop, 1302 frames.** Proxy loads and hooks take. Device/window/projection/handedness all captured **[device]**; `EndScene` is once per frame; back buffer not lockable (D-7 confirmed); input confirmed live. Call sites: PROJECTION `0x403AB9`, VIEW `0x403CA8`, three WORLD sites - the `4013AB9` conflict resolved as a mangled `403AB9`. **New headline risk: zero `SetTransform` calls per frame once in a level.** Probe v2 built to settle it. |
| 2026-11-02 | **Probe built and deployed.** `src/d3d8probe.cpp` - a dependency-free `d3d8.dll` proxy that logs call sites, matrices, window/device params, draw calls and input calls. Builds Win32/Release, deployed to `E:/tmp/Q3Mods/xops096` with SHA-256 verified. **Not yet run.** |
| 2026-11-02 | **Import table read from `xops096.exe`.** No DirectInput: input is pure USER32 (`GetKeyboardState`, `GetCursorPos`/`SetCursorPos`), so we can supply input directly instead of spoofing it (D-17). The first attempt's `dinput.dll` proxy could never have loaded (D-16). PE32 i386, base `0x400000`, ASLR off, `.text` `0x401000`-`0x45D000` - which resolves the `4013AB9` address conflict. |
| 2026-11-02 | Second IDA pass: `sub_401120` is the main render function (544 bytes of locals holding the matrix/viewport structs); `SetRenderState` block at `0x4039C9` via `0xC8`; the draw call is **`DrawPrimitiveUP`** via `0x120`, in the `loc_401396` loop. Two open questions logged: the conflicting `push 3` address, and the "matrix data before the draw call" claim, which is vertex data. |
| 2026-11-02 | Owner, IDA Free: `call [...+94h]` at `0x403AA4` after `push 3` - **`SetTransform` is vtable offset `0x94` / index 37, confirmed**, and `0x403AA4` is the projection-matrix site. `BUG_FIXES.md` D-6 confirmed from the binary. Next: locate the `push 2` (`D3DTS_VIEW`) site nearby. |
| 2026-10-31 | Owner: aim source is **dominant-hand motion aim**. |
| 2026-10-31 | Owner: install path `D:/VR/xops096-wxr`; `d3d8.dll` override confirmed working (risk R-5 closed); no Japanese locale needed (R-6 closed); input design settled - head drives view and movement, aim fully separate. `build/` un-ignored (R-7 closed). |
| 2026-10-31 | Owner settled Q-A: mod the real binary, OpenXOPS out of scope. |
| 2026-10-31 | Folder created. General framework and the first attempt reviewed; `PROJECT_OVERVIEW.md`, `PROJECT_PLAN.md`, this file and `BUG_FIXES.md` written. No code written, no hardware touched. |
