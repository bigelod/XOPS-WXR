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

`Docs/MetaAI/` is the **first attempt**. Its reverse-engineering findings are
worth keeping; its code does not compile and its protocol is not XrApi. Read it
as a sketch, never as a baseline. See `PROJECT_OVERVIEW.md` section 6.

## Settled, do not relitigate without asking the owner

- **We mod the real `XOPS.exe`.** OpenXOPS is out of scope (owner, 31 Oct 2026).
- **Injection vector: a `d3d8.dll` proxy** in the game directory, wrapping the
  D3D8 interfaces rather than detouring them. The Wine DLL override this needs
  is confirmed working on the owner's device.
- **Install / deploy path: `D:/VR/xops096-wxr`.** No Japanese locale needed.
- **Input design (owner, 31 Oct 2026):** the head drives the **view** and the
  **movement direction** (forward = where you are looking). **Aim comes from the
  dominant hand** - point the controller, shoot there - and is completely
  separate from the head. That last part is the project's hardest problem: read
  `PROJECT_PLAN.md` feature F-7, and run its Cheat Engine test, before writing a
  line of aim code.
- **XrApi 0.5**, mono first, in-process, 32-bit, keep the game's D3D8, no
  wrappers. Being on an "old" XrApi version is not a problem to fix.
- **`build/` is tracked** - it holds the project file and deploy script.
  `obj/`, `out/`, `bin/` are ignored. Do not add a `Build/` ignore line: this
  repo has `core.ignorecase=true`, so it would re-ignore `build/`.

## Two project rules, learned from the first attempt

- **Mark every claim with its evidence**: `[spec]`, `[runtime]`, `[general]`,
  `[derived]`, `[code]`, `[device]`, `[unverified]`. An unmarked claim reads as
  device-verified, and almost nothing here is.
- **Nothing is complete until the owner has confirmed it on hardware.**
  `CURRENT_STATUS.md` records state, not intent.
