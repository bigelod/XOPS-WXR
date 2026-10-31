# CURRENT_STATUS - XOPS-WXR

**As of 31 Oct 2026.** This file records **state, not intent**. A row moves to
"working" only after the owner has observed it on a Quest 3. Nothing is
"working" today.

---

## One-line summary

**Nothing is built.** The repository contains documentation and a prior
attempt's non-compiling sketch. The project is at milestone **0**: the route is
settled - **mod the real `XOPS.exe`**, not OpenXOPS (owner, 31 Oct 2026) - and
what remains at this milestone is the rest of the intake, which needs the owner
and the device.

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
| Repository skeleton | **absent** | no `src/`, no project file. `build/` now exists and is tracked |
| Build and deploy script | absent | write it at stage 2, with the freshness guard, before anything is hard to debug. Deploy target: `D:/VR/xops096-wxr` |
| DLL proxy / injection | absent | vector chosen (`d3d8.dll` wrapper), unproven |
| Logging + build stamp | absent | first deliverable |
| XrApi 0.5 client | absent | the sibling port's `XRAPI.cs` is the reference |
| Handshake files | absent | **`version` = `0.5` is the single most important line of the project**; without it the runtime sends nothing at all **[runtime]** |
| Sync pixel | absent | plan: `Clear` with a 10x10 `D3DRECT`, last before `EndScene` |
| Window / resolution override | absent | the biggest unknown after the camera - see risk R-2 |
| Camera (mono 6DOF) | absent | composition formula settled; unit scale and handedness unmeasured |
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
| The camera is set via `SetTransform(D3DTS_VIEW)` | asserted **[MetaAI]**, but with the **wrong vtable index** (see `BUG_FIXES.md` D-6), which suggests it was inferred rather than observed | **low - prove it before writing camera code** |
| The game loads `dinput.dll` | asserted **[MetaAI]** | low - and moot: we proxy `d3d8.dll`, and the owner has confirmed that override works **[device]** |
| XOPS keeps a player aim angle separable from the render camera | unknown | **none - and F-7 depends on it.** Settle with Cheat Engine before building aim |
| Metres-to-XOPS-units scale is ~50 | guess **[MetaAI]**, no measurement | **none - measure it** |
| XOPS exposes any resolution option | unknown | none |
| No Japanese locale or codepage needed | owner, 31 Oct 2026 **[device]** | high |

## Inherited code

`Docs/MetaAI/XOPS_WXR/` - **does not compile, and would not work if it did.**
Two `DllMain` definitions; links `d3d8.lib`/`d3dx8.lib` from an SDK that has
not shipped Direct3D 8 since August 2007; and implements a protocol that is not
XrApi. 15 defects catalogued in `BUG_FIXES.md`. Keep the IDA findings, rewrite
the code.

## Environment - unknown and needed

Known:

- [x] Install path: **`D:/VR/xops096-wxr`**
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

**Stage 1**, and it is no longer blocked on anything. Run the unmodified
XOPS 0.96 flat in a WinlatorXR container and write down everything it took -
container resolution, DXVK or WineD3D, and the **reproducible steps for the
`d3d8.dll` override** (it is known to work; what is missing is the write-up an
installer would need). Fill in the remaining intake `TBD`s while doing it.

Two things can run in parallel, both on the desktop and both cheap:

- **the desktop resolution test** - run XOPS at a non-640x480 size and see
  whether the HUD, menus and mouse mapping survive it (risk R-2);
- **the Cheat Engine aim test** - find out whether the game keeps a player aim
  angle that can be written without moving the camera. Feature F-7's whole
  design rests on the answer, so it is worth knowing early.

## Changelog

| Date | Change |
|---|---|
| 2026-10-31 | Owner: aim source is **dominant-hand motion aim**. |
| 2026-10-31 | Owner: install path `D:/VR/xops096-wxr`; `d3d8.dll` override confirmed working (risk R-5 closed); no Japanese locale needed (R-6 closed); input design settled - head drives view and movement, aim fully separate. `build/` un-ignored (R-7 closed). |
| 2026-10-31 | Owner settled Q-A: mod the real binary, OpenXOPS out of scope. |
| 2026-10-31 | Folder created. General framework and the first attempt reviewed; `PROJECT_OVERVIEW.md`, `PROJECT_PLAN.md`, this file and `BUG_FIXES.md` written. No code written, no hardware touched. |
