# CURRENT_PROGRESS.md - In-Progress Features

### Completed / Working in DLL template

- [x] **IDA mapping**
  - Confirmed engine is custom D3D8, not Genesis3D / .NET
  - Located `Direct3DCreate8` string at `.rdata:0045D1DC`, IAT `00463BA0`, SDK `0xDC = 220 = DX8.1`
  - Traced `sub_4035D0` InitD3D flow: `GetAdapterDisplayMode [ecx+20h]` @ `loc_403601`, `CreateDevice [edx+3Ch]` @ `loc_403645` with 640x480
  - Identified `g_pDevice` = `dword_48C5BC` (102 xrefs) and `g_pD3D8` = `dword_48C5B8`

- [x] **MinHook bootstrap**
  - DLL proxy `dinput.dll` loads on XOPS startup
  - Hooks `Direct3DCreate8` export from `d3d8.dll`
  - Hooks `IDirect3D8::CreateDevice` at vtable 15 (0x3C) to capture device pointer

- [x] **Device hooks**
  - `Present` at vtable[15] (0x3C) - matches device vtable, hooked after CreateDevice succeeds
  - `SetTransform` at vtable[44] (0xB0) - matches `push 2` / `call [ecx+B0h]` in `sub_401120` loop for `D3DTS_VIEW`
  - Both hooks enabled via `MH_EnableHook(MH_ALL_HOOKS)`

- [x] **WinlatorXR XrAPI transport**
  - UDP listener thread on `127.0.0.1:7872`
  - Parses both CSV string and binary float array for `px,py,pz,qx,qy,qz,qw`
  - UDP sender on `127.0.0.1:7278` replies with frameId string
  - Winsock init in detached thread

- [x] **Monocular 6DOF**
  - Global `HMDState` with atomic floats
  - In `SetTransform(Type==2)`: adds `hmd * 50.0f` to matrix `_41/_42/_43`
  - Optional quat rotation via `D3DXMatrixRotationQuaternion` * view
  - Both eyes same image, head position moves camera (like HWXR monocular mode)

- [x] **Compositor sync pixel**
  - In `Present` hook: `GetBackBuffer(0) -> LockRect -> p[0] = frameId << 16` (red shade)
  - `frameId` wraps at 255 per WinlatorXR spec

- [x] **Build system**
  - VS2022 solution `XOPS_WXR.sln`, toolset v143, Win32
  - References `$(DXSDK_DIR)Include/Lib/x86` (June 2010) + `external/MinHook/include/lib`
  - Outputs `dinput.dll` to `bin/Release/` ready for WinlatorXR container

### In Progress / Partial

- [~] **Scale tuning** - 50.0f is estimate, needs in-headset calibration. Currently too fast in small rooms, too slow for peek.
- [~] **Rotation handling** - quat * view matrix multiplies correctly but causes roll discomfort. Testing position-only vs pos+rot modes.
- [~] **Backbuffer format handling** - code assumes `X8R8G8B8`. XOPS may be `R5G6B5` on some GPUs. Need format check in `GetDisplayMode` result.
- [~] **Controller passthrough** - UDP data includes controllers but not yet mapped to XOPS input. Currently only head pose used.
