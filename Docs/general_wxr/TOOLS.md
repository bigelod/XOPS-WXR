# Tools for a WinlatorXR port

Grouped by the job you need done. Items marked **[proven]** have been used in
a shipped port in this family; **[likely]** means it is the standard tool for
the job but has not been exercised here, so verify before depending on it.

---

## 1. Build

### Visual Studio 2022 / MSBuild, toolset v143 **[proven]**

The whole family builds on this. Two settings matter more than everything else
in the project file:

- **Platform: Win32**, not x64, when the game is 32-bit. AoE2, Shogo, Black
  and White, Contagion - all Win32. See `TIPS.md` on staying 32-bit.
- **Configuration: Release.** Debug CRT mismatches against a 1998 host DLL
  are a bad way to spend a day.

Expect deprecation warnings when compiling 1990s C++ (`/Zc:forScope-` and
friends). They are noise; do not "fix" them by modernising the source.

Command line, so a script can drive it:

```
msbuild build\YourMod.vcxproj /p:Configuration=Release /p:Platform=Win32
```

### DirectX SDK, June 2010 **[proven]**

Needed for `d3dx9.lib` - `ID3DXFont`, `ID3DXLine` and the rest. The Contagion
port requires it. If you would rather not depend on it, the alternative is
drawing text and lines with pure D3D9, which is more code but removes a
large installer from every contributor's setup.

### vcpkg **[proven, for source rebuilds]**

Only relevant when the game is open source and you are rebuilding it. The
Tremulous port uses a `vcpkg.json` manifest (SDL2, openal-soft, curl, zlib,
libjpeg-turbo, freetype) that VS 2022 17.6+ restores on solution open.

### A build-and-deploy script **[proven - copy this pattern]**

Shogo's `build/build-and-deploy.ps1` does five things, and each one has caught
a real mistake:

1. wipes `obj/` and `out/` - no stale objects
2. builds Release|Win32
3. reads the `__DATE__ __TIME__` stamp compiled into the binary and
   **refuses to deploy** if it is older than the newest source file
4. copies to every target (release folder, game install) and verifies with a
   SHA-256 comparison, printing `match=True`
5. refuses to touch release-owned files (version manifests, readme)

Step 3 is the one to copy first. Testing a stale build is invisible without
it, and it wastes an entire session when it happens. Embed the stamp:

```c
static const char* kBuildStamp = "built " __DATE__ " " __TIME__;
```

and log it as the first line of your log file, so the human can confirm from
the device which binary they are actually running.

---

## 2. Getting inside the game

### MinHook **[likely - the standard choice]**

Small, permissive, x86 and x64, ideal for function detours when you need to
intercept the game's own calls. The usual pattern: a DLL proxy gets you
loaded, MinHook gets you *into* the specific functions.

Typical targets for a port of this kind: the D3D device's `Present` / `EndScene`
(a reliable per-frame hook and the natural place to draw the sync pixel), the
window procedure, the camera or view-matrix setup, input polling.

Note that hooking under Box64 and Wine is one more thing that can differ from
the desktop. Test the hook on device early rather than building a whole
feature on an unverified hook.

### DLL proxying **[proven]**

The workhorse vector. Name your DLL something the game already loads from its
own directory, forward every export to the real one, and start your own thread.

| Name | Loaded by | Used in |
|---|---|---|
| `ddraw.dll` | DirectDraw-era games | AoE2, Black and White |
| `d3d9.dll` | D3D9 games | common |
| `dinput8.dll` | anything using DirectInput | common |
| `version.dll` | almost everything, very early | Contagion (Source) |

`version.dll` is the popular choice precisely because it is loaded early and
almost never actually used for anything.

### dnSpy **[likely]**

For managed code - .NET and Unity/Mono games. Decompiles to readable C#, and
can edit and save assemblies in place. If the target is Unity, this plus the
Mono runtime is often the entire toolchain, and that is the shape the
Borderlands 2 work (`BL2_MonoVR`) takes.

Not useful for native C++ games, which is most of this family.

### IDA Free, Ghidra, x64dbg **[likely]**

For native games with no source and no mod API, when you need to find the
camera. IDA Free is capable enough for 32-bit work; Ghidra is free, fully
featured and scriptable; x64dbg is the live debugger.

Pragmatic ordering before reaching for a disassembler:

1. Does the game have **source**? Rebuild. (Tremulous.)
2. Does it have a **mod API** or SDK? Use it. (Shogo.)
3. Does it have an **existing PCVR mod**? Retarget that - far less work than
   starting over, and what the Far Cry, Crysis and Shogo ports did.
4. Does it have a **modding community**? Someone has already found the camera
   and written it down. Search before you disassemble.
5. Only then disassemble.

### Cheat Engine **[likely]**

Underrated for this work. Finding the camera position in memory by searching
for a float that changes as you walk is often faster than reading
disassembly, and it gives you a concrete address to work back from.

---

## 3. Testing without a headset

### XrAPI UDP Tester **[proven - the single highest-value tool]**

Sends synthetic XrApi packets to `127.0.0.1:7872` so the game can run its
entire VR path on a desktop. Turns a 10-minute build-deploy-headset-launch
cycle into seconds, and lets you use a real debugger.

Use it for everything *except* final verification. See `TIPS.md`: its
quaternions are often not unit length, and its hand separation does not match
a real headset, so values tuned against it may not transfer. "The code path
runs" - yes. "The numbers are right" - no.

A capture of a real device packet pasted into the tester closes most of that
gap, which is why logging the raw packet verbatim is worth doing.

### WXRKeyboard and XeRxKEYs **[proven - read the source]**

Two C# projects at <https://github.com/bigelod> that are the reference
implementations of the **InputAPI** path, and the fastest way to get it right:

- **`WXRKeyboard`** - an on-screen keyboard for use inside a container. The
  file to read is `OnScreenKeyboardDemo/WXR/WXRInputAPI.cs`: it is a complete,
  short InputAPI client, including the `Z:/tmp/xr` handshake, the
  `<drive>:/xrtemp` desktop fallback, the `input_version` file, and a
  **remote debug** mode that drives a headset's InputAPI from a desktop over
  the LAN.
- **`XeRxKEYs`** - motion gestures to flat-game input, with a pluggable
  architecture worth copying: an `XRModule` supplies head and hand poses, an
  `OutModule` emits input (InputAPI, `SendInput`, `SendKeys`, raw mouse
  events), and gestures are user-recorded data with sensitivity and cooldown.
  `OutputModules/WXR/XKeycode.cs` is a 700-line mapping from .NET `SendKeys`
  notation to X11 keycodes - take it wholesale rather than rebuilding it.
  `DataPlan.txt` documents the data model.

If the port is injection-based rather than pose-based, start by reading these
two rather than by writing anything.

### OXRWXR and OpenComposite **[reference - check before writing a mod]**

- **[OXRWXR](https://github.com/WinlatorXR/OpenXR-WXR)** - a Windows **OpenXR
  runtime** for WinlatorXR, built on **XrApi 0.6**. For a game with native
  OpenXR support this is the whole integration. `src/WinXrApiUDP.cpp` is also
  a good 0.6/0.7 client to read.
- **[OpenComposite for WinlatorXR](https://github.com/WinlatorXR/OpenComposite)** -
  **OpenVR** on top of that runtime, so SteamVR-era games run without
  SteamVR. A fork of upstream OpenComposite kept close to it; upstream's game
  compatibility spreadsheet is the place to check a specific title.

Together these mean a game with an existing PCVR build may need no mod at all.
Worth knowing about - but **not a free trade**: the layers cost real
performance, and the stack is aimed at general PCVR play rather than at making
one title run well. For a specific game a direct mod usually wins. See
`WINLATORXR_DESC.md`, Path E.

### opentrackWXR **[proven]**

<https://github.com/WinlatorXR/opentrackWXR> - opentrack with an XrApi tracker
module. For any game that already supports TrackIR, FreeTrack, SimConnect,
FSUIPC or similar, this converts headset poses into something the game accepts
natively, with no mod at all. Run it as a second executable with a 3-5 second
delayed start.

Read `tracker-wxr/wxr.cpp` for a compact, independent XrApi client - including
the `MODE_3D = -1` mode that keeps the runtime's own mouse emulation. The
`wxr-module-only` branch is just the module, for porting into another opentrack
build.

### A desktop fallback path in your own code **[proven]**

Have the mod fall back from `Z:\tmp\xr` to a local directory when `Z:` does
not exist, so the same binary runs on both. One binary, two environments, no
conditional compilation.

---

## 4. On the device

### adb **[proven]**

Logs, file push and pull, and getting your build onto the headset without
touching the UI. `adb logcat` for the Android side; your own log file for the
Windows side.

### scrcpy **[likely]**

Mirrors and controls the headset screen from the desktop. Makes "show me what
you see" a screen share instead of a photograph, and makes capturing a
screenshot with the controller overlay visible straightforward - which, per
`TIPS.md`, is sometimes the datum that ends a bug.

### Measuring performance **[proven practice]**

**Do not build frame timing into the mod.** Performance is measured **on the
device**, with Android-level and WinlatorXR-level tooling - the container's own
overlays and counters, and the standard Android profiling and `adb` tools -
not by instrumenting the game.

That is the right division: the mod cannot see the compositor, the translation
layers or the GPU, which is where the time actually goes. The runtime does
derive your frame rate from the sync pixel, so WinlatorXR already knows what
the game is delivering without any help from you.

So when a change might cost frames, the question for the owner is "what does
the container report?", not "shall I add a timer?".

### Your own log file **[proven]**

On a standalone device with no debugger, the log *is* the debugger. Make it
cheap, make it timestamped, put the build stamp on line 1, and gate the
verbose parts behind a flag that is off by default.

### An on-screen debug overlay **[proven, indirectly]**

Everything drawn into the window reaches the headset. A few lines of text in
the corner showing pose values, mode and frame id is a diagnostic channel that
needs no file copying at all. Cheap to add, and the fastest way to answer "is
the data arriving?"

---

## 5. Project hygiene

### `leakcheck.py` **[proven - and worth copying verbatim in spirit]**

Compares the repository against a **pristine** copy of the game's source
release and reports any copied block, so you can prove the repository is
publishable under a licence that forbids redistributing the original.

```
python tools/leakcheck.py <pristine source tree>
```

Run it against a *pristine* tree. Against your working tree - with the mod
integrated - it matches your own added lines and the output is noise. Shogo's
copy says so in its docstring because this mistake was made.

### `makepayload.py` / `package_overlay.py` **[proven]**

Packing the files an installer carries. Project-specific, but the idea
generalises: one script that builds the shippable bundle, so releases are
reproducible rather than assembled by hand.

---

## 6. Things to be sceptical of

- **RenderDoc / apitrace / PIX.** Graphics capture inside Box64 and Wine on
  Android is unlikely to work. Capture on the desktop build if you must, and
  expect the device to differ.
- **Any tool that needs a 64-bit host process.** You are 32-bit on purpose. It's what most of these games are. WinlatorXR can run both, but we don't need to.
- **Shader-based upscalers and post-process injectors (ReShade and similar).**
  Another layer inside two translation layers, on a mobile GPU, in front of a
  compositor that resamples anyway. The container-resolution route gets you
  supersampling for free; start there. WinlatorXR has its own way to auto-inject ReShade as well if stereo rendering is just not working well in-mod, so users can try that as a fallback themselves with no effort on our part.
- **dgVoodoo and wrapper layers generally.** See `TIPS.md`. Removing one was
  progress in the Shogo port.
