# WinlatorXR porting framework

Generic, game-agnostic guidance for taking a PC game to standalone VR
(Quest, Pico) through **WinlatorXR** and its **XrApi 0.4** protocol.

Distilled from the Shogo-VR-WXR port and from the other ports in this family
(HaloCEWXR, Far Cry, Crysis, BioShock, Alpine Faction, SixDOFinator,
Tremulous, AoE2, Black and White, Contagion). Nothing here is specific to
Shogo; the Shogo work is cited only as evidence.

## Using this set in another project

Copy these files into the new project as **`Docs/general_wxr/`** - for example
`Docs/general_wxr/TIPS.md`. That subfolder name is the convention; keep it, so
the general WinlatorXR guidance is always distinguishable from a project's own
docs at a glance.

What travels and what does not:

- **These files are general.** They describe WinlatorXR, XrApi and the
  practice of porting. Copy them as they are, `CLAUDE.md` included.
- **`CLAUDE.md` carries the standing instructions** - the choices not to
  relitigate, the non-negotiables in the code, and how to work. Claude Code
  picks up a `CLAUDE.md` in a subfolder when it touches files there, so to have
  it apply from the start of a session, add a line to the project's **root**
  `CLAUDE.md`:

  ```markdown
  This is a WinlatorXR VR port. Read `Docs/general_wxr/CLAUDE.md` before
  planning or writing any code.
  ```
- **`PROJECT_PLAN.md` is a template.** Copy it *out* of `general_wxr` to the
  project's own `Docs/PROJECT_PLAN.md` and fill it in, so the blank template
  stays pristine for next time.
- **Per-game findings belong in the project's own docs**, not in here. If
  something turns out to be general - a protocol fact, a trap any port would
  hit - fold it back into this set so the next port inherits it.

The canonical copy lives in `Release/Docs/` of **Shogo-VR-WXR**, which is a
gitignored path, deliberately: the set is reused across the owner's ports
rather than published with any one of them.

## Read in this order

| File | What it is | When |
|---|---|---|
| **[WINLATORXR_DESC.md](WINLATORXR_DESC.md)** | What WinlatorXR is and how a Windows EXE reaches a headset | first, once |
| **[XRAPI_V0_4_DESC.md](XRAPI_V0_4_DESC.md)** | The protocol: UDP packets, handshake files, haptics, the red sync pixel, presentation modes, and every version 0.1-0.7 | before writing the client |
| **[TIPS.md](TIPS.md)** | Hard-won advice - architectural decisions and working method | before deciding anything |
| **[FIRST_STEPS.md](FIRST_STEPS.md)** | Staged plan from zero to 6DOF, with human checkpoints | as the working plan |
| **[TOOLS.md](TOOLS.md)** | Toolchain, injection vectors, testing without a headset | as needed |
| **[XRAPI_UPGRADE.md](XRAPI_UPGRADE.md)** | Moving an existing port from 0.4/0.5 to 0.6 or 0.7, and why not to rush | only once a port works |
| **[PROJECT_PLAN.md](PROJECT_PLAN.md)** | Template to copy per game; starts with an owner intake interview | at the start of each port |

## The seven things that matter most

If you read nothing else:

1. **The red sync pixel is not optional.** WinlatorXR reads a shade of red
   from the top-left corner of the captured frame to match the image to the
   pose it was rendered for. Without it the world reprojects against the wrong
   head pose and players get motion sick - and no amount of frame-rate work
   fixes it. `XRAPI_V0_4_DESC.md` section 5.
2. **Start on XrApi 0.4 or 0.5.** They are human-readable, the UDP Tester can
   simulate them, and poses arrive without declaring spaces. 0.6 changes the
   packet shape and 0.7 makes it binary - both are upgrades to make later, for
   a named reason. See `XRAPI_UPGRADE.md`.
3. **There are four presentation modes, and SBS is not automatically the
   goal.** Mono, side-by-side, AER (one eye per frame, alternating - depth at
   mono cost) and non-VR (a flat floating window with live tracking, needing
   no camera work at all). Pick deliberately; consider shipping more than one.
4. **Keep the game's renderer and keep it 32-bit.** There is no OpenXR runtime
   to satisfy, so there is no reason to modernise. Stereo is two renders into
   two halves of one buffer. Wrappers (dgVoodoo, ReShade) are a layer inside
   two translation layers.
5. **The window *is* the output.** Borderless, filling the display, re-applied
   every frame. Chrome in the corner means no sync pixel.
6. **Measure; do not infer.** On a standalone device with no debugger, a log
   line beats a screenshot and an afternoon of reasoning. Instrument first.
7. **Measure offsets in the frame you will use them in.** A value that only
   works where it was found means the frame is wrong. Stop tuning and fix the
   frame.

## Reference implementations worth reading before writing

- **[HaloCEWXR](https://github.com/WinlatorXR/HaloCEWXR)** - the canonical
  XrApi client; most later pose clients descend from its `WinXrApiUDP`
- **[SixDOFinator_SampleProject](https://github.com/WinlatorXR/SixDOFinator_SampleProject)** -
  the smallest working 6DOF example
- **[WXRKeyboard](https://github.com/bigelod/WXRKeyboard)** - a complete, short
  InputAPI client (`WXR/WXRInputAPI.cs`), plus LAN remote-debug
- **[XeRxKEYs](https://github.com/bigelod/XeRxKEYs)** - poses to flat-game
  input with no game changes; `XKeycode.cs` is the X11 keycode mapping, take
  it wholesale
- **[opentrackWXR](https://github.com/WinlatorXR/opentrackWXR)** - XrApi poses
  into TrackIR / FreeTrack / SimConnect, so a game with native head-tracking
  support needs no mod at all; `tracker-wxr/wxr.cpp` is a compact standalone
  client

## Before writing a mod: does the game already do VR?

[OXRWXR](https://github.com/WinlatorXR/OpenXR-WXR) is a Windows **OpenXR
runtime** for WinlatorXR built on XrApi 0.6, and
[OpenComposite for WinlatorXR](https://github.com/WinlatorXR/OpenComposite)
puts **OpenVR** on top of it. A game with official VR support, a SteamVR mode
or a mature PCVR mod may therefore need **no new code at all**.

But it is **not a free trade**: those layers cost real performance, and the
stack is aimed at general PCVR play - running an existing VR library with no
per-game work - rather than at making one title run well.

**Modding the game directly is still the normal recommendation**, and it is
the only option for the retro titles this scene targets, none of which have a
PCVR build. Know Path E exists; choose it for breadth, not to avoid writing a
mod.

## The official specification

<https://winlatorxr.github.io/xrapi.html> - 0.6 current, 0.5 archived at
`xrapi05.html`. The runtime's own source is at
`app/src/main/java/com/winlator/xr/api/` in the
[WinlatorXR repository](https://github.com/WinlatorXR/WinlatorXR)
(`XrVersion01` to `XrVersion07`, plus `XrFramesync` for the sync pixel).
Between the two: the spec is the authority on what fields *mean*, the source
on what the code *does*, and a device capture beats both.

## When something does not work

Symptom to likely cause, for the failures that are specific to this stack.
Each is explained where it is cited.

| Symptom | Likely cause |
|---|---|
| No packets arrive at all, no error | `version` file missing, misspelled or an unsupported number - the runtime then builds no implementation and starts no listener. It is read **once**, at startup. |
| Packets stop arriving after they had been working | You sent `MODE_VR = 0`, which disables the whole inbound stream. Deliberate for menus and a good default there; an accident everywhere else. Keep sending the outbound packet so you can turn it back on. |
| Every packet arrives twice | You bound 7872 *and* 7873. From 0.4 the runtime sends the same packet to both; they are duplicates, not a fallback. |
| Head tracking swims, lags or sickens, but the maths is right | The sync pixel. Wrong value, wrong corner, drawn before something that overwrites it, filtered or blended, green not 0 - or a title bar moving the corner. |
| Sync implemented and nothing changed | The window has chrome, so the runtime is sampling the title bar, not your pixel. |
| Hands swing when you turn your head | A hand pose routed through the head transform. Convert each pose independently; share only the recentre origin. |
| Left-handed mode picks the wrong hand | The handedness swap applied twice. Swap once at ingest, then never test the flag again for anything spatial. |
| Your own objects draw through the weapon | They lack the engine's view-model flag (`FLAG_REALLYCLOSE` in LithTech), so they sort against the world instead of the gun. |
| FOV changes have no effect | The field is **degrees**, applied only if greater than 1. Tangents of half-angles are all below 1 and are ignored. |
| Haptics never stop | Sending 0 does not clear them - the runtime stores only values greater than zero. Haptics are a **duration in frames**, so decay the number you send. |
| A hand's basis is stretched or asymmetric | A non-unit quaternion. Normalise on ingest, unconditionally; tester packets are often several percent long. |
| Values tuned on the desktop are wrong on device | Usually the frame of reference, not precision. A correct value transfers; if it only works where it was found, fix the frame. |
| Tracking is 25% too fast in a flat-window mode | The window is HMD-attached. Read bool 19 (`IS_IMMERSIVE`) and damp - about 0.8 - when it is set. |
| On 0.6: buttons and sticks work, no poses | You did not declare any spaces. 0.6 returns poses only for locate pairs you asked for. |
| A pose is all zeros | On 0.6 that means "could not be located", not the origin. |
| Testing a change that appears to do nothing | The build stamp. Embed `__DATE__ __TIME__`, log it on line 1, and refuse to deploy a binary older than the newest source file. |
| A shipped default change does nothing on the test device | A saved config is overriding it. Check the device's own INI. |

## Status

Written 29 Oct 2026, from a V1.2-era Shogo port.

Verified against: Quest 3 packet captures; the runtime's own Java source
(`com/winlator/xr/api/`, `cmod_bionic` branch) for versions 0.1-0.7 and the
framesync reader; the official spec at
<https://winlatorxr.github.io/xrapi.html>; and the client code of HWXR,
opentrackWXR, SixDOFinator, WXRKeyboard and XeRxKEYs.

Claims are tagged **[runtime]** or **[spec]** where they come from those
sources rather than from experience. Where this set is uncertain it says so -
treat those marks as work for whoever ports next, not as settled fact.

Still re-verify the protocol against a live capture at the start of each new
port. It costs ten minutes, and the APK moves.

**Scope, so nothing here reads as a to-do list.** These ports target **XrApi
0.4 or 0.5** and should stay there; 0.6 and 0.7 are documented so an agent
knows they exist. Performance is measured on-device with Android and
WinlatorXR tooling, not by instrumenting a mod. And writing a mod directly
remains the normal path - the OpenXR and OpenVR stack is for general PCVR
play, at a real cost in performance.
