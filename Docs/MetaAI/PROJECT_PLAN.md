# PROJECT_PLAN.md - XOPS-WXR: Monocular 6DOF VR for X Operations 0.96

## What are we trying to do?

Create a **VR injection mod for X Operations 0.96** - the freeware FPS from 2003 by Team MITEI / nine-two - that runs inside **WinlatorXR containers** using its **XrAPI**.

This is **NOT a full OpenXR / OpenVR port**. We are deliberately following the pattern of **Halo CE VR (HWXR)** and **Shogo Mobile Armor Division VR (Shogo-VR-WXR)**:

- **Monocular 6DOF "VR"**: Both eyes see the same image. No stereo rendering, no eye separation. But you get full 6DOF head tracking - position moves the camera, so you can lean, peek, dodge.
- **Game renders itself**: We do not take over the renderer. We patch the game's existing D3D8 camera and add a sync marker for the WinlatorXR compositor.

Target platform: **WinlatorXR on Android (Quest / phone XR)** where Winlator runs Windows x86 binaries in a container and provides head tracking via UDP.

### Why XOPS 0.96 specifically?

- Custom lightweight proprietary engine, **not Genesis3D** (no `geCamera` symbols despite similarity to Shogo). IDA shows direct D3D8 calls.
- Native **C/C++ 32-bit, DirectX 8.1** (`Direct3DCreate8` at `.rdata:0045D1DC`, IAT `00463BA0`, SDK version `0xDC = 220 = DX8.1`).
- Small binary, no anti-cheat, loads `dinput.dll` -> perfect for DLL proxy injection.
- We rejected the **OpenXOPS** BSD reimplementation fork. Goal is to mod the *real* binary in place.

### Engine research (IDA Free + dnSpy)

- dnSpy: **"not a .NET assembly"** -> native only.
- `sub_4035D0` = `InitD3D`:
  - `Direct3DCreate8` -> `GetAdapterDisplayMode` `[ecx+20h]` at `loc_403601`
  - `CreateDevice` `[edx+3Ch]` (vtable index 15) at `loc_403645` with `640x480` (`0x280` / `0x1E0`)
  - Stores device in `dword_48C5BC` = `g_pDevice` (**102 xrefs** - every render path)
  - `dword_48C5B8` = `g_pD3D8`, `48C3C8` = `D3DPRESENT_PARAMETERS`
- Camera:
  - No `geCamera`. It's `IDirect3DDevice8::SetTransform` with `D3DTS_VIEW = 2` (`push 2`) and `D3DTS_PROJECTION = 3`
  - Vtable offset `0xB0` (index 44) - found near main frame loop `sub_401120`
- This gives us the injection point: hook `SetTransform(Type==2)` and offset `_41/_42/_43` (translation).

### WinlatorXR XrAPI protocol

- **Port 7872**: WinlatorXR -> game. Sends head pose + controller state (string of floats or binary float array: `px,py,pz,qx,qy,qz,qw`)
- **Port 7278**: Game -> WinlatorXR. Reply with frame counter / floats. Used for compositor timing.
- **Sync pixel**: Game must paint a **shade of red into pixel (0,0)** of its backbuffer every Present. Value = `frameId % 255`. Compositor locks to this to know a new frame is ready.
- Supports modes: full stereo, **monocular VR (6DOF without stereo)**, and flat screen.

### Architecture

**DLL proxy injection:**

```
XOPS.exe -> loads dinput.dll (our proxy) -> loads real_dinput.dll
          |
          +-> MinHook -> hooks Direct3DCreate8 (d3d8.dll export)
                       -> hooks IDirect3D8::CreateDevice vtable[15]
                       -> hooks IDirect3DDevice8::Present vtable[15] and SetTransform vtable[44]

UDP thread: recvfrom 127.0.0.1:7872 -> parse pose -> global HMDState
SetTransform hook: if Type==2, add HMD translation (meters * 50.0f scale) to view matrix, optionally apply quat rotation
Present hook: lock backbuffer, write (0,0) = RGB(frameId,0,0), sendto 127.0.0.1:7278
```

Build: **VS2019/2022 + DirectX SDK June 2010 + MinHook x86**. Output `dinput.dll` (or `xinput1_3.dll`) next to `XOPS.exe` in WinlatorXR container. Set WinlatorXR to Monocular 6DOF.

### Non-goals

- No OpenXR runtime, no eye buffers, no projection matrix hacking for stereo (future goal)
- No rewriting renderer, no OpenXOPS integration
