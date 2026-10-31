# BUG_FIXES.md - Bugs Fixed (Short Bullet Form)

- **dnSpy .NET false lead** - Thought XOPS was .NET, tried dnSpy. Fixed: confirmed native C/C++ 32-bit (dnSpy says "not a .NET assembly"), switched to IDA Free.

- **Engine mis-ID Genesis3D** - Searched for `geCamera` symbols, found none. Fixed: engine is Team MITEI custom D3D8, use `SetTransform(push 2)` not geCamera.

- **Wrong vtable offset for CreateDevice** - Initially hooked device `0x3C` before D3D object created. Fixed: hook `Direct3DCreate8` first, then hook `IDirect3D8::CreateDevice` at vtable[15] (`[edx+3Ch]` @ `loc_403645`).

- **g_pDevice not captured** - Used hardcoded `48C5BC` address, broke on ASLR. Fixed: get device from `CreateDevice` out param instead of static address.

- **Present hook crash on LockRect** - Called `LockRect` on backbuffer without `GetBackBuffer`. Fixed: proper `GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO)` + `Release()` after.

- **Backbuffer format assume X8R8G8B8** - Wrote DWORD red on R5G6B5, got blue pixel. Fixed: check `D3DFORMAT` from present params `48C3C8`, handle both.

- **UDP bind to 0.0.0.0 failed in WinlatorXR** - Socket bound to all interfaces, WinlatorXR container NAT dropped packets. Fixed: bind specifically to `127.0.0.1:7872` and send to `127.0.0.1:7278`.

- **String-of-floats parse vs binary** - XrAPI sends CSV in some WinlatorXR versions, binary float[7] in others. Fixed: try binary heuristic (`fabs(f[0]) < 10`) then fallback to `sscanf`.

- **FrameId never wrapped** - Red pixel went >255, compositor lost sync. Fixed: `frameId % 255` per spec.

- **MinHook hook order** - Enabled `Present` hook before `CreateDevice` returned device, nullptr vtable. Fixed: enable device hooks only after `SUCCEEDED(hr)` and `outDev != null`.

- **Scale too large (500x)** - Head movement launched camera through walls. Fixed: reduced to 50.0f, made configurable.

- **Proxy DLL missing exports** - XOPS failed to load with our `dinput.dll` (missing `DirectInputCreateA`). Fixed: added `dinput.def` forwarding to `real_dinput.dll`.

- **VS2019 vs VS2022 toolset mismatch** - Project created with v142 but OpenXOPS docs say VS2019, user on VS2022. Fixed: dual toolset v143 target, document both.

- **DirectX SDK June 2010 not found** - `d3d8.lib` missing, link error. Fixed: use `$(DXSDK_DIR)` env var, add README with install path.

- **Thread not detached** - XrThread blocked DllMain, game hung on load. Fixed: `std::thread(...).detach()` after first CreateDevice.
