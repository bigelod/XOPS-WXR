# XOPS-WXR - standing instructions

A VR mod for **X Operations 0.96** (2003, Team MITEI) targeting **WinlatorXR**
on a Meta Quest 3. 32-bit, Direct3D 8.1, injected as a DLL proxy.

This is a WinlatorXR VR port. **Read `Docs/general_wxr/CLAUDE.md` before
planning or writing any code** - it carries the standing choices for every port
in this family, and they bind here.

Then read, in order:

1. `Docs/ClaudeCode/PROJECT_OVERVIEW.md` - what the project is and why
2. `Docs/ClaudeCode/PROJECT_PLAN.md` - the plan, the intake interview, the risks
3. `Docs/ClaudeCode/CURRENT_STATUS.md` - what actually runs. State, not intent
4. `Docs/ClaudeCode/BUG_FIXES.md` - known defects, each with a fix and a test
5. `Docs/OpenCode/DeepSeek41Flash/README.md` - the 5 Nov pass: window, sync
   pixel and FOV implemented and desktop-verified, and the fullscreen finding

`Docs/MetaAI/` is the **first attempt**. Its reverse-engineering findings are
worth keeping; its code does not compile and its protocol is not XrApi. Read it
as a sketch, never as a baseline. See `PROJECT_OVERVIEW.md` section 6.

## Settled, do not relitigate without asking the owner

- **We mod the real `XOPS.exe`.** OpenXOPS is out of scope (owner, 31 Oct 2026).
- **Injection vector: a `d3d8.dll` proxy** in the game directory, wrapping the
  D3D8 interfaces rather than detouring them. The Wine DLL override this needs
  is confirmed working on the owner's device.
- **Paths.** The EXE is `xops096.exe`. Desktop test install and deploy
  default: `E:/tmp/Q3Mods/xops096`. WinlatorXR copy: `D:/VR/xops096-wxr`.
  Byte-identical binary in both. No Japanese locale needed.
- **Input design (owner, 31 Oct 2026):** the head drives the **view** and the
  **movement direction** (forward = where you are looking). **Aim comes from the
  dominant hand** - point the controller, shoot there - and is completely
  separate from the head. That last part is the project's hardest problem: read
  `PROJECT_PLAN.md` feature F-7, and run its test 1 (IDA Free, xrefs on the
  camera yaw global), before writing a line of aim code.
- **XrApi 0.5**, mono first, in-process, 32-bit, keep the game's D3D8, no
  wrappers. Being on an "old" XrApi version is not a problem to fix.
- **`build/` is tracked** - it holds the project file and deploy script.
  `obj/`, `out/`, `bin/` are ignored. Do not add a `Build/` ignore line: this
  repo has `core.ignorecase=true`, so it would re-ignore `build/`.

## The one fact that shapes everything

**XOPS transforms the in-game world on the CPU.** [device, probe run 4]
In-game, every draw across all three draw paths carries FVF `0x0144` (XYZRHW)
and `SetTransform` is called zero times per frame. The device's matrices are
used for the title and menu 3D only; the view matrix stops updating around
frame 59 and never moves again.

So **the in-game camera is not reachable through Direct3D.** It lives in the
game's own memory and that is where it has to be driven from - see
`PROJECT_PLAN.md` F-5. Resolution is baked in the same way: the game projects
to 640x480 itself, and `config.dat` has no resolution field.

What still works through D3D: getting in-process, the sync pixel via `Clear`
in `EndScene` (confirmed once per presented frame; the back buffer is not
lockable so this is the only route), and the window. Input works through
USER32 - `GetKeyboardState`, `GetCursorPos`, `SetCursorPos`, all IAT-patchable,
so we supply input directly and never synthesise events.

**When instrumenting, exclude the probe's own memory.** A scan that does not
will find its own globals and stack, and they track the camera perfectly.

## Two project rules, learned from the first attempt

- **Mark every claim with its evidence**: `[spec]`, `[runtime]`, `[general]`,
  `[derived]`, `[code]`, `[device]`, `[unverified]`. An unmarked claim reads as
  device-verified, and almost nothing here is.
- **Nothing is complete until the owner has confirmed it on hardware.**
  `CURRENT_STATUS.md` records state, not intent.
