# FUTURE_GOALS.md - Remaining Features (Not Yet Started)

These are planned but not implemented in the current MinHook DLL.

### 1. Weapon / Viewmodel Decoupling
- Currently gun moves with head (viewmodel is part of view matrix). Need to find viewmodel render path (likely second SetTransform or separate world matrix) and freeze or add counter-rotation.
- Look for `dword_48C5BC` xrefs that render hands - nop or offset.

### 2. True World-Space UI / Crosshair
- 2D HUD is screen-locked, uncomfortable in HMD. Project HUD to world quad at 2m distance.
- Hook `DrawPrimitive` / `DrawPrimitiveUP` after UI detection (FVF `D3DFVF_XYZRHW`).
- Alternative: hide crosshair and use WinlatorXR controller ray.

### 3. Stereo Mode Toggle
- Add config file `XOPS_WXR.ini` with `mode=monocular/stereo`.
- For stereo: hook SetTransform twice per frame with eye offset, render to two viewports side-by-side, adjust Projection matrix for IPD.
- Requires Present hook to do double-wide render (1280x480 -> two 640x480 eyes).

### 4. FOV and Aspect Correction
- XOPS 640x480 is 4:3, stretched in WinlatorXR. Patch projection matrix `D3DTS_PROJECTION` (push 3) to set FOV 90-110 for VR.
- Need to find original FOV calc (likely `D3DXMatrixPerspectiveFovLH`).

### 5. Physical Crouch / Roomscale
- Map HMD Y (py) to in-game crouch. Find player height address (scan for crouch toggle in Cheat Engine) and write.
- Clamp movement to avoid falling through floor - XOPS collision is simple BSP.

### 6. Controller Input - WinlatorXR Gamepad
- XrAPI sends controller state on 7872 (buttons + thumbsticks). Map to `IDirectInputDevice8::GetDeviceState` hook or directly to XOPS key buffer.
- Need to intercept `DIK_` codes used by XOPS (WASD + mouse).

### 7. Near Clip Fix
- When you lean close to walls, geometry clips (near plane ~0.1). Hook `SetTransform(D3DTS_PROJECTION)` and push near plane to 0.01.
- Check `D3DPRESENT_PARAMETERS` depth format.

### 8. Calibration UI
- In-game console or external overlay to tune:
  - `pos_scale` (currently 50.0f)
  - `rot_scale`
  - `ipd` for future stereo
- Save to ini, hot reload.

### 9. Performance - Disable VSync / Triple Buffer
- XOPS locks to 60 via D3D Present interval. For VR need 72/90 Hz. Patch `PresentationInterval` in `48C3C8`.

### 10. OpenXOPS Reference Path (Optional)
- If injection proves too fragile, port hook logic to OpenXOPS BSD codebase as a compile-time option `WXR_MONO`. Keep binary compat but use clean renderer.
