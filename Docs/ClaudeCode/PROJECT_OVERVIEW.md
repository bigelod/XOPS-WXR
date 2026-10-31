# PROJECT_OVERVIEW - XOPS-WXR

**What this is.** A VR mod for **X Operations ("XOPS") 0.96**, the 2003 Japanese
freeware FPS by Team MITEI (programmer *nine-two*, art *TENNKUU*), so it can be
played with 6DOF head tracking on a **Meta Quest 3** running **WinlatorXR**.

**Who should read this.** The owner, and any later agent session picking the
project up. Read in this order:

1. `Docs/general_wxr/` - the general WinlatorXR porting framework. Start at its
   `README.md`; `CLAUDE.md` there carries standing instructions that bind this
   project.
2. This file - what the project is and the decisions behind it.
3. `PROJECT_PLAN.md` - the step-by-step plan, with the intake interview that
   still needs the owner.
4. `CURRENT_STATUS.md` - what actually runs today. Short, because the answer is
   "nothing yet".
5. `BUG_FIXES.md` - defects already identified in the inherited work, with the
   fix and a cheap test for each.
6. `Docs/MetaAI/` - the first attempt. Read it as a **sketch and a history of
   guesses**, not as a working baseline. See section 6 below.

Evidence tags used throughout, following the general docs: **[spec]** from the
XrApi specification, **[runtime]** from WinlatorXR's own source, **[general]**
from `Docs/general_wxr/`, **[derived]** reasoned from a documented API,
**[device]** observed on hardware, **[unverified]** believed but untested.
Nothing in this project is **[device]** yet.

---

## 1. The goal, stated plainly

A player with the original XOPS 0.96 installation drops a DLL into the game
folder, launches it inside a WinlatorXR container on a Quest 3, and plays the
original single-player campaign with:

- the in-game camera driven by the headset - looking around and leaning work;
- the image filling the headset with correct frame-to-pose correlation (the red
  sync pixel), so it feels locked rather than swimmy;
- **movement that follows the head** - forward is wherever the player is
  looking;
- **aiming from the dominant controller**, completely independent of the head,
  so a player can look one way and shoot another.

Stereo depth and hand models are **later** goals, and possibly never - see
section 5.

The last two bullets are the owner's decision (31 Oct 2026) and they change the
shape of the project: in a 2003 mouse-look FPS the camera orientation *is* the
aim vector, so separating them is memory work on the game's player state, not a
D3D interception. It is the hardest thing on the list. `PROJECT_PLAN.md`
feature **F-7** sets out what has to be proven before any of it is built, and
the cheap fallbacks if the engine will not give it up.

## 2. Why XOPS is a good target

| Property | Value | Why it matters |
|---|---|---|
| Year | 2002-2003 | a 2003 renderer has enormous headroom on a Quest 3 **[general]** |
| Bitness | 32-bit native C/C++ | stays in-process, no IPC, Box86 handles it **[general]** |
| Renderer | Direct3D 8.1 | `SetTransform(D3DTS_VIEW)` is a writable camera **[unverified]** |
| Size | small single EXE, no anti-tamper | DLL proxy injection is viable |
| Multiplayer | not in scope | no anti-cheat and no netcode to desync |

The things that make it *hard* are also clear, and both come from its age:

- **It is a 640x480 game.** WinlatorXR captures the window, so a 640x480 window
  inside a 2800x1400 container shows the player a tiny picture on a desktop, and
  puts the sync pixel on window chrome where the runtime never reads it. The
  resolution has to be forced, and the game's 2D HUD was laid out in 640x480
  coordinates. This is the first real engineering problem, not the camera.
- **There is no source for the shipping binary and no mod API.** Everything goes
  through hooks into a binary we only partly understand.

## 3. The stack

```
Quest 3 (Android, arm64)
└── WinlatorXR APK
    ├── Box86             x86 -> arm64 translation
    ├── Wine + container  a virtual Windows box at the container resolution
    │   └── XOPS.exe      32-bit, Direct3D 8.1
    │       └── our DLL   proxy-loaded; hooks D3D8, speaks XrApi over UDP
    └── XR layer          captures the game window, warps it, presents it
```

Nothing we write runs on Android. Our code is a 32-bit Windows DLL talking
Win32, two UDP sockets and a handful of files. **[general]**

## 4. Approach, and the decisions already taken

Defaults inherited from `Docs/general_wxr/CLAUDE.md`. **Do not relitigate these
without asking the owner.**

| Decision | Choice | Why |
|---|---|---|
| Path | **mod the game directly** (Path A, XrApi) | no PCVR build exists to run; OXRWXR + OpenComposite is for general PCVR play and costs heavily **[general]** |
| XrApi version | **0.5** | human-readable, the UDP Tester simulates it, poses arrive without declaring spaces; 0.5 is 0.4 plus the InputAPI listener at no cost **[general]** |
| Bitness | **32-bit** | the game is; there is no OpenXR runtime to load |
| Renderer | **keep D3D8** | known-good under Box86/Wine; stereo needs no modern API |
| Wrappers | **none** (no dgVoodoo, no ReShade) | a layer inside two translation layers |
| Process model | **in-process** | no IPC, no second binary to ship and reap |
| Injection vector | **`d3d8.dll` proxy** (not `dinput.dll`) | the game provably loads `d3d8.dll`; the proxy *is* the hook, so `Direct3DCreate8` needs no detour. See `PROJECT_PLAN.md` section 4 |
| D3D8 headers | **mingw-w64's `d3d8.h`, no DirectX SDK, no D3DX** | the June 2010 SDK does not ship D3D8 - August 2007 was the last that did. We dynamic-load `d3d8.dll` anyway, so no `.lib` is needed |
| First presentation mode | **mono** (`MODE_VR=1`, `MODE_3D=0`) | one render, ships on its own, and is the right first milestone |
| Menus | **`MODE_VR=0`** | stops the pose stream, restores the runtime's mouse, leaves immersive presentation **[general; device-proven in the Interstellar Marines port]** |
| Supersampling | **container resolution, no code** | the runtime filters on downsample |
| Settings surface | in-game overlay or a hot-reloaded text file | editing an INI on a headset is painful, and XOPS has no menu we can extend |

### Settled: we mod the real binary, not OpenXOPS

**Owner's decision, 31 Oct 2026:** mod the shipping `XOPS.exe`.
[OpenXOPS](https://openxops.net/en/) - the New-BSD C/C++ reimplementation,
DirectX 9 or OpenGL, source on Subversion at `openxops.net/en/developer.php` -
is **out of scope for this project**. Fidelity to the original binary is the
point of the port, and someone else can port the reimplementation.

Recorded so no later session mistakes it for an oversight: this is the harder of
the two routes, and the general framework's own preference order
(`Docs/general_wxr/TOOLS.md`: source rebuild above hooking) points the other
way. The decision was made with that known. The consequences we accept:

- the camera, window, resolution, projection and sync square are all reached
  through hooks into a binary we only partly understand;
- **stereo stays out of scope** until mono is device-verified, and may never be
  affordable - see section 5;
- the cheapest-first discipline matters more here than it would with source:
  feature **F-5**'s "log every `SetTransform` for one frame" experiment comes
  before any camera code is written.

## 5. Scope

**In scope**

- Single-player campaign, mono presentation, 6DOF head tracking.
- **Head-relative movement** and **aim decoupled from the head** (owner's
  decision; feature F-7). Note this pulls what would normally be a stretch goal
  into the core, and it carries the project's largest single unknown - whether
  the engine keeps an aim angle that can be written without moving the camera.
- Forced VR-appropriate resolution and a borderless window filling the display.
- Sync pixel, correct and verified.
- Controller input for movement, turning, fire and the common actions.
- A non-VR / flat-window mode as a comfort and performance fallback.

**Out of scope for now, and each needs a named reason to come in**

- **Stereo (SBS or AER).** Depends on finding and offsetting the view and
  projection matrices twice per frame in a binary we do not own. Revisit after
  mono is device-verified; AER before SBS, since it buys depth at mono cost.
  **[general]**
- **Hand and weapon models rendered from controller poses.** Drawing art
  attached to a tracked hand is the expensive part of any port in this family,
  and it is not needed to deliver decoupled aim.
- **World-space HUD.** The HUD is 640x480 2D drawing. Scaling it is in scope;
  projecting it into the world is not.
- **Multiplayer.**
- **A renderer rewrite, a 64-bit build, or a newer XrApi version.** Explicitly
  not goals. **[general]**

## 6. The inherited work, assessed honestly

`Docs/MetaAI/` contains a plan, a progress list, a bug list and a Visual Studio
project with ~200 lines of C++. **Its documents claim far more than its code
supports, and the code as committed cannot have run.**

The valuable part is real and should be kept:

- the engine identification - native C/C++, not .NET, not Genesis3D;
- the specific IDA findings: `Direct3DCreate8` string at `.rdata:0045D1DC`, IAT
  `00463BA0`, SDK version `0xDC` = 220 = D3D8.1, `sub_4035D0` as the D3D init,
  `CreateDevice` called through `[edx+3Ch]` at `loc_403645`, device created at
  640x480, `dword_48C5BC` as the device pointer with 102 xrefs, `dword_48C5B8`
  as the `IDirect3D8`, `48C3C8` as the present parameters, `sub_401120` as the
  main frame loop;
- the overall architecture - DLL proxy, hook `CreateDevice`, hook the camera,
  hook `Present`;
- the DLL proxy `.def` file shape.

What is wrong is listed finding-by-finding in `BUG_FIXES.md`. The headline
items:

- **`CURRENT_PROGRESS.md` marks the XrApi transport, the sync pixel and the
  build system as complete.** None of them can be. The project defines
  `DllMain` twice - in `dllmain.cpp` and again in `XOPS_WXR.cpp` - so it does
  not link; it links `d3d8.lib` and `d3dx8.lib` from the June 2010 DirectX SDK,
  which has not shipped Direct3D 8 since August 2007; and what it implements is
  not XrApi.
- **The protocol is wrong in almost every field.** No `version` file is
  written, and **[runtime]** without one the runtime constructs no
  implementation, starts no listener and sends nothing at all - so the mod would
  have been silent on device regardless of the rest. The inbound parser expects
  `px,py,pz,qx,qy,qz,qw` CSV; the wire carries 28 space-separated floats, a
  frame id, a 19-character button string and the HMD name. The outbound packet
  is `"%d,0,0,0"`; it should be
  `<L haptic> <R haptic> <MODE_VR> <MODE_3D> <FOVX> <FOVY>`. The sync pixel is
  written as `frameId % 255` from a local counter rather than echoing the
  `HMD_SYNC` value received, which defeats the runtime's entire frame-matching
  mechanism - the one thing the general docs call non-optional.
- **`BUG_FIXES.md` describes fixes for bugs that could not have been
  observed** - a blue pixel on R5G6B5 hardware, a container NAT dropping
  packets, a camera launched through walls at 500x scale - and the committed
  code contains none of the described fixes. Treat that file as *plausible
  failure modes worth watching for*, which is genuinely useful, and not as a
  record of events.

**So: `Docs/MetaAI/` is a design sketch and a reverse-engineering notebook.**
Keep the IDA addresses, re-derive everything else. The code is a skeleton to
rewrite, not a baseline to debug.

## 7. What success looks like

The milestone list lives in `PROJECT_PLAN.md` section 5 and the live state in
`CURRENT_STATUS.md`. The one that matters most is milestone 6: the owner puts
the headset on, holds their head still, and the world holds still. Everything
before it is plumbing and everything after it is layered on top - a camera
defect found later gets misattributed to whatever was built next. **[general]**

## 8. Sources

- `Docs/general_wxr/` - the porting framework, written 29 Oct 2026 and verified
  against Quest 3 captures and WinlatorXR's own Java source.
- [Interstellar-Marines-WXR](https://github.com/bigelod/Interstellar-Marines-WXR) -
  the owner's sibling port. `XRAPI.cs` is a short, complete, device-tested 0.4
  client and the closest thing to a reference for the transport layer here. Its
  `Docs/ClaudeCode/` layout is the convention this folder follows.
- [OpenXOPS](https://openxops.net/en/) - the BSD reimplementation; source on SVN
  via `openxops.net/en/developer.php`.
- [HaloCEWXR](https://github.com/WinlatorXR/HaloCEWXR) - the canonical XrApi
  client, and the reference for all four presentation modes.
- [DirectX SDKs of a certain age](https://walbourn.github.io/directx-sdks-of-a-certain-age) -
  August 2007 was the last SDK to include Direct3D 8.
