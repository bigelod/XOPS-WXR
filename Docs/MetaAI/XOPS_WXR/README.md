# XOPS-WXR - X Operations 0.96 Monocular 6DOF VR (WinlatorXR)

This VS2022 project hooks the custom engine you found in IDA:

- .rdata:0045D1DC Direct3DCreate8 / IAT 00463BA0
- sub_4035D0 InitD3D -> call [edx+3Ch] = CreateDevice (102 xrefs to g_pDevice dword_48C5BC)
- SetTransform at [device+0xB0] push 2 = D3DTS_VIEW

## How it works (WinlatorXR XrAPI)
- UDP 7872: headset -> game (pose)
- UDP 7278: game -> headset (frame sync string)
- Top-left pixel R = frameId % 255 = sync counter for compositor

Monocular = both eyes same image + 6DOF translation, same as HWXR's monocular mode.

## Setup (VS2022)

1. Install DirectX SDK June 2010 -> set DXSDK_DIR env var (e.g. C:\DXSDK)
   - Lib: $(DXSDK_DIR)Lib\x86 contains d3d8.lib, d3dx8.lib
2. Get MinHook:
   git clone https://github.com/TsudaKageyu/minhook external/MinHookSrc
   Build external/MinHookSrc/build/VC17/MinHookVC17.sln Win32 Release
   Copy:
     external/MinHookSrc/include/MinHook.h -> external/MinHook/include/
     external/MinHookSrc/bin/Release/libMinHook.x86.lib -> external/MinHook/lib/
     external/MinHookSrc/bin/Release/libMinHook.x86.dll -> bin/Release/ (optional)

3. Open XOPS_WXR.sln in VS2022, select Win32, Release, Build.

Output: bin/Release/dinput.dll

Drop dinput.dll + libMinHook.x86.dll (if dynamic) + real dinput renamed to real_dinput.dll next to XOPS.exe in WinlatorXR container.

In WinlatorXR: Immersive mode, Render Mode = Monocular 6DOF.

## Tuning

- In XOPS_WXR.cpp, if your IDA shows SetTransform at different vtable offset, change:
  vtbl[44] -> your offset (0xB0/4 = 44). Check your sub_401120+12A xref.
- Scale: 50.0f converts meters to XOPS units (640x480 engine). Adjust if head moves too fast/slow.
- Rotation: currently multiplies view by HMD quat. For pure positional 6DOF, comment out rotation part.

## IDA tips for XOPS custom engine (Team MITEI / Nine-two)

- No geCamera symbols (custom engine, not Genesis3D) - use D3DTS_VIEW push 2 search
- g_pDevice = dword_48C5BC, 102 xrefs = all render paths
- View matrix built near push 2 in sub_401120

Enjoy!
