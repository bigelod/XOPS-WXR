# What WinlatorXR is, and how a Windows game ends up in a headset

**Provenance.** Written from the outside - from building against it, not from
reading its source. The parts describing how a mod *interacts* with it are
verified on a Quest 3. The parts describing its internals are the working
model that has so far predicted behaviour correctly, and are labelled as such.

---

## 1. The stack, top to bottom

WinlatorXR is an **Android application** that runs unmodified 32-bit and
64-bit Windows executables and presents their output to a VR headset.

```
  Meta Quest / Pico (Android, arm64)
  └── WinlatorXR APK
      ├── Box86 / Box64        x86 and x86-64 instruction translation
      ├── Wine                 the Windows API, on Linux, on Android
      │   └── a "container"    a virtual Windows machine: drive letters,
      │       └── game.exe     a virtual display of a chosen resolution,
      │           └── your mod   registry, DLL overrides
      └── XR layer             captures the container's display, warps it,
                               hands poses in over UDP, presents to the
                               headset compositor
```

It descends from Winlator (the same stack minus VR), so Winlator knowledge
mostly transfers: containers, DXVK or WineD3D selection, Box64 tuning, drive
mappings.

The thing worth internalising: **nothing in your mod runs on Android.** Your
code is a Windows DLL or EXE inside Wine, speaking Win32. The only Android
thing you ever touch is a UDP socket and a couple of files.

---

## 2. The container, and why its resolution is your render resolution

A container is a self-contained Wine prefix with a fixed virtual display size,
set in the WinlatorXR UI.

That resolution propagates straight through to your game:

```
container resolution
  -> GetSystemMetrics(SM_CXSCREEN / SM_CYSCREEN)
     -> whatever the game uses to pick a mode
        -> the engine's back buffer
```

Two consequences that matter more than they sound:

**It is your supersampling control, for free.** The XR layer samples the
captured window down to the headset panel, and it *filters* when it does.
Render above panel resolution and that downsample is anti-aliasing. Confirmed
on a Quest 3: a 4200x2100 container against a nominal 2800x1400 - 2.25x the
pixels - looks markedly better for little cost on an old renderer. No code,
no wrapper, no render-to-texture.

Tell players the container size. It is a real graphics setting that lives
outside your mod, and it is often the best quality knob you have.

**2D art does not scale with it.** A HUD blitted at fixed pixel sizes covers
*less* of the view as the container grows. Budget for a HUD scale setting, and
prefer deriving it from the measured width against a reference width over
making the player discover the problem. Shogo ships
`scale *= 1 + strength * (reference_width / actual_width - 1)`, with
`strength` as the player-facing control, because the right amount turns out to
be a matter of taste.

---

## 3. How the image gets out: window capture

**This is the mechanism everything else follows from.** WinlatorXR does not
hook your renderer, intercept a swap chain or read a texture. It captures the
**game window** from the container's display and presents that.

Therefore:

- **The window must fill the display, with no title bar and no border.**
  Borderless fullscreen stretched to the container size, or true fullscreen.
  Anything else and the headset shows you a desktop with a small game on it -
  and worse, the sync pixel in the top-left corner lands on window chrome and
  is never seen. Strip `WS_CAPTION | WS_THICKFRAME | WS_BORDER`, size to
  screen, and re-apply every frame because engines rebuild windows.
- **Stereo is your job, not the runtime's.** Side-by-side means *you* render
  twice into two halves of one buffer. The runtime is told which mode you are
  in (`MODE_3D`) and splits accordingly. There are **four** presentation modes
  - mono, SBS, AER (one eye per frame, alternating, flagged in the sync
  square's blue channel) and non-VR (a flat floating window with tracking
  still live). They are a design choice, not a quality slider; see
  `XRAPI_V0_4_DESC.md` section 5a before assuming SBS is the target.
- **Anything drawn into the window reaches the headset**, including debug
  overlays. Useful: a few lines of on-screen text is often the fastest
  diagnostic channel you have on a standalone device.
- **Mono costs nothing extra.** `MODE_3D = 0` shows one image to both eyes.
  No depth, but a perfectly playable "big screen with head tracking", and the
  right first milestone.
- **AER buys depth at mono cost.** One eye per frame, alternating, so the
  scene is rendered once per frame but each eye refreshes at half rate. On a
  mobile GPU that is often a better trade than SBS.

---

## 4. How input gets in

Two separate paths, and knowing which one you need shapes the whole port.

### Path A: XrApi (preferred) - the game reads poses directly

The game opens a UDP socket and receives head and hand poses, buttons, sticks,
IPD and FOV; it sends back haptics, mode flags and FOV. Full detail in
`XRAPI_V0_4_DESC.md`.

This is the path for a real VR port: it gives 6DOF, it gives per-hand data,
and it lets the game treat the headset as a first-class input device. It needs
somewhere in the game to *put* that data - a camera you can write, or a mod
API, or source code.

### Path B: InputAPI - the runtime injects mouse and keyboard

WinlatorXR also accepts synthetic mouse and keyboard events over UDP, so a
helper can translate controller motion into the input a non-VR game already
understands. The runtime injects these **at the Android level**, above Wine,
which is the whole point: Wine-side `SendKeys` fails against many games that
read input through DirectInput or raw devices, and this path does not.

**Verified against working implementations** - `WXRKeyboard` (an on-screen
keyboard for containers) and `XeRxKEYs` (motion gestures to flat-game input),
both C#, both at <https://github.com/bigelod>.

```
UDP OUT (to the runtime)  port 7728
UDP IN                    port 7287        reserved; unused today

Mouse:     M,<L><R><M><scrollUp><scrollDown>     T or F per button
           e.g.  M,TFFFF         left click
                 M,FFFTF         scroll up

Keyboard:  K,<keycode>[,<keycode>...]
           e.g.  K,45            k
                 K,50,45         K        (50 = shift, prefixed)
                 K,37,54         Ctrl+C
```

Keys arriving in one message are pressed **together**, which is how modifiers
work - there is no separate down/up event. Messages are processed in order,
with a 50 ms hold between press and release for keyboard events.

**The keycodes are X11 keycodes, not Windows virtual-key codes.** This catches
people out. `KEY_ESC` is 9, `KEY_1` is 10, `KEY_CTRL_L` is 37, `KEY_SHIFT_L`
is 50, `KEY_ALT_L` is 64. `XeRxKEYs`' `XKeycode.cs` is a complete 700-line
mapping from .NET `SendKeys` notation (`{ESC}`, `^` for Ctrl, `%` for Alt,
`{;}`, `{!}`) to these codes, with shifted characters emitted as `50,<code>`.
Copy it rather than rebuilding it. `{BREAK}` and `{HELP}` have no equivalent
and are dropped.

> **A note on the T/F form.** Earlier notes in this project recorded the mouse
> message as `M,1,0,0,0,0` (comma-separated 1/0). The current shipping code
> sends `M,TFFFF` - one `T`/`F` per button, no separators. Use `T`/`F`: it is
> what ships and clicks do work on device. (Reading the `cmod_bionic` branch,
> the field is parsed with Java's `Boolean.parseBoolean`, which is true only
> for the literal string `"true"` - so the branch alone does not explain why
> either documented form works. Not worth chasing; follow the shipping
> clients.)

**InputAPI sends clicks and scroll, and cannot move the mouse.** Clicks work.
`XeRxKEYs` handles `MouseMove` with the comment *"Not supported at this
time"*, and that is the state of it.

This is not a detail - **it shapes the whole plan for a closed-source game.**
"Head-look drives the mouse" cannot go through InputAPI. The options are:

1. **Move the mouse from inside the container instead.** Ordinary Windows
   mouse-motion calls - `SendInput`, `mouse_event`, `SetCursorPos` - *may*
   work under Wine where InputAPI will not. `XeRxKEYs` ships `SendInput` and
   raw mouse-event output modules alongside its InputAPI one, so the hybrid
   shape is already built: InputAPI for keys and clicks, in-container
   injection for motion. **Unverified, and worth testing early** - it is the
   cheapest thing that would unlock head-look for an unmoddable game, and a
   half-hour experiment settles it.
2. Write the camera in the game's own code, which is Path A.
3. Accept no head-look and design around it - viable for an RTS, a tabletop
   strategy game or a point-and-click, where the head is a window onto a
   scene rather than the aim vector.

Decide this at intake. It is the difference between a weekend and a month.

### Handshake for Path B

The same `Z:\tmp\xr` directory as Path A, with one extra file:

| File | Contents | Notes |
|---|---|---|
| `version` | `0.5` | the newer clients write 0.5 here, not 0.4 |
| `input_version` | `0.1` | InputAPI's own version; **delete on shutdown** |

The desktop fallback in these clients is `<current drive>:/xrtemp`, derived
from where the helper is running rather than hardcoded.

`WXRKeyboard` also carries a **remote debug** mode: point `targetIP` at a
headset's LAN address (it reads the clipboard for convenience) and drive its
InputAPI from a desktop. It requires an app inside WinlatorXR to have already
triggered XrApi 0.5 or newer, and it disables that instance's window-switching
behaviour. For developing an injection-based port this is the equivalent of
the UDP tester for a pose-based one, and worth setting up early.

### Path C: a separate gesture app, with no game changes at all

`XeRxKEYs` is worth studying as an architecture, not only as a tool. It is a
desktop app that reads XrApi poses through an `XRModule`, matches them against
user-recorded **motion gestures** (sensitivity, cooldown, trigger conditions),
and emits input through a pluggable `OutModule` - WinlatorXR InputAPI,
`SendInput`, `SendKeys` or raw mouse events.

Nothing in the game is modified. For a title with no source, no mod API and
hostile anti-tamper, this may be the only route - and because the gesture
library is data rather than code, a profile for a new game is authored rather
than programmed. Before committing to a hard port, check whether a gesture
profile would deliver most of the experience.

### Path D: feed the game's *existing* head-tracking support

The one to check first for cockpit games, because it can need **no game code
at all.**

Thousands of racing and flight titles already support head tracking through
TrackIR, FreeTrack, SimConnect, FSUIPC or a UDP protocol - because that market
has had head tracking since long before VR. If the target is one of them, the
job is not to write a VR mod but to **supply poses in a format it already
accepts**.

`opentrackWXR` (<https://github.com/WinlatorXR/opentrackWXR>) is exactly that:
a fork of [opentrack](https://github.com/opentrack/opentrack) with an XrApi
tracker module added (`tracker-wxr/`). XrApi poses go in; opentrack's existing
output protocols go out - FreeTrack, SimConnect, FSUIPC, FlightGear, vjoystick,
raw UDP, X-Plane. A `wxr-module-only` branch carries just the tracker module
if you want to port it into your own opentrack build.

It runs as a **second executable alongside the game**, with a delayed start of
3-5 seconds, rather than as anything injected. Its README notes you may have
to "Switch To" the target game for input to map correctly.

And it uses the `MODE_3D = -1` mode (`XRAPI_V0_4_DESC.md` section 5a), which
tells the runtime to **keep its own right-controller-gyro mouse emulation**
while still delivering HMD 6DOF. So the player gets free head-look in the
cockpit and keeps the mouse they already had. For a sim, that is often the
entire feature.

Before planning a port of any cockpit game, check whether it supports TrackIR
or FreeTrack. If it does, this path is days rather than months.

### Path E: the game already has a PCVR mode - run that

**Check this first, before any of the above.** If the target has a PCVR build,
there may be nothing to write.

Two components turn WinlatorXR into a PCVR target:

- **[OXRWXR](https://github.com/WinlatorXR/OpenXR-WXR)** - a Windows **OpenXR
  runtime** for WinlatorXR, using **XrApi 0.6** as its interface layer. A
  native OpenXR game sees a normal runtime.
- **[OpenComposite for WinlatorXR](https://github.com/WinlatorXR/OpenComposite)** -
  implements **OpenVR** on top of that runtime, so SteamVR-era games work
  without SteamVR. A fork of upstream OpenComposite, kept close to it, with
  WinlatorXR compatibility fixes.

```
native OpenXR game  ------------------------>  OXRWXR  ->  XrApi  ->  WinlatorXR
OpenVR / SteamVR game -> OpenComposite ---->  OXRWXR  ->  XrApi  ->  WinlatorXR
```

### It is not a free trade - it costs heavily in performance

**This path is not a shortcut to a better port, and it is usually not the one
to choose for a specific game.**

Each layer is real work at runtime. An OpenVR call goes through OpenComposite
into OpenXR, through OXRWXR, out over XrApi, into WinlatorXR - all inside
Box64 and Wine, on a mobile GPU. And a PCVR title was built for a desktop GPU
to begin with, so you are paying translation overhead on content that was
already too expensive.

What this path is *for* is **general PCVR play**: letting someone run their
existing VR library on a standalone headset without a per-game mod existing at
all. That is a different goal from making one game run *well*. Judged as
"ship a good port of this title", a direct mod usually wins, because a mod
targets the one game's own renderer and spends nothing on generality.

So: **modding the game directly remains the normal recommendation**, and Paths
A to D are the common case - which is also the case for the retro titles this
scene cares about, none of which have a PCVR build to run.

Check Path E so you know it exists and can tell a player about it. Choose it
when the goal is breadth, when no mod exists and none is planned, or when the
title is light enough to absorb the overhead. Do not choose it to avoid
writing a mod for a game that deserves one.

Upstream OpenComposite's game compatibility spreadsheet is the place to check
a specific title.

Worth knowing even for a mod author: it is why XrApi 0.6 exists in the shape
it does, and why `system` carries a per-eye size for "direct PC VR frames".

### Combining paths

Nothing stops combining these. XrApi for the camera, InputAPI for actions you
cannot reach in code, Wine-side injection for mouse motion, and the runtime's
own mouse left in place when the game already knows what to do with a head.

---

## 5. Performance, honestly

Three multiplying costs sit between your game and the panel: instruction
translation (Box64), the Windows API layer (Wine), and the mobile GPU.

What this means in practice:

- **Old games are the sweet spot.** A 1998 renderer has so much headroom on a
  Quest 3 that 2.25x supersampling costs little. This is the single biggest
  reason to pick old titles.
- **Stereo roughly doubles scene cost.** Two renders per frame. Mono is a
  legitimate performance mode, not a failure.
- **Fill rate is the usual wall**, not CPU, once the game is from the fixed-
  function or early-shader era. Resolution and post-processing are your levers.
- **Post-processing is pure loss.** Anti-aliasing, ambient occlusion, depth of
  field and motion blur all cost fill and all look worse after the compositor
  resamples. Turn them off, and verify they *stay* off - games from this era
  are notorious for silently restoring graphics settings, which is a known
  live problem in the Interstellar Marines port.
- **Square-ish resolutions suit headsets.** Panels are nearer square than
  16:9. Forcing an unusual mode like 1400x1400 can be a win, and getting a
  game to accept one without letterboxing or distortion is a recurring fight
  worth budgeting for.

---

## 6. Where to inject your code - the pattern across every port so far

You need your code inside the game process. In order of preference:

| Vector | How | Seen in |
|---|---|---|
| **Native mod API** | the game already loads a DLL of yours | Shogo (`CShell.dll`) |
| **Source rebuild** | the game is open source; just add files | Tremulous (ioquake3) |
| **DLL proxy** | drop a DLL the game loads by name; forward the real exports | AoE2 and Black and White (`ddraw.dll`), Contagion (`version.dll`) |
| **Runtime shim** | stand in for a library the game already uses | HaloCE WXR (OpenXR runtime entry points) |
| **External + injection** | separate process, InputAPI | closed games |

The DLL proxy is the workhorse. `ddraw.dll`, `d3d9.dll`, `dinput8.dll`,
`version.dll` are all loaded early, from the game's own directory, by name -
so a replacement that forwards every export and adds a thread of its own gets
you in with no patching of the executable at all.

A game with an existing VR mod for PCVR is the best starting position of all:
retargeting one is far less work than writing one, and it is exactly what the
Shogo, Far Cry and Crysis WinlatorXR ports did.

---

## 7. The reference ports

Read these before starting. They are the accumulated practice of this scene:

- <https://github.com/WinlatorXR/HaloCEWXR> - the canonical one; the XrApi
  client in `WinXrApiUDP` is what most later clients are adapted from
- <https://github.com/WinlatorXR/SixDOFinator_SampleProject> - minimal 6DOF
  starting point, the thing to read first if you want the smallest possible
  working example
- <https://github.com/SgtBilko76/Farcry_vrmod-WinlatorXR> - retargeting an
  existing PCVR mod
- <https://github.com/SgtBilko76/Crysis-VR-WinlatorXR> - a heavier engine
- <https://github.com/SgtBilko76/alpinefactionVR> - Red Faction
- <https://github.com/terx1232/BioShock-Quest-VR> - Unreal 2.5 era
