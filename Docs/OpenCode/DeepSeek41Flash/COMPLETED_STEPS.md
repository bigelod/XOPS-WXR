# XOPS-WXR — COMPLETED STEPS

**Everything in this file is implemented.** Unless a line says otherwise it is
also **desktop/PC-verified**. Device verification on the Quest 3 is a separate
pass; `BUG_FIXES.md` will be created from that run.

**Last build covered:** `built Nov  6 2026 18:19:57`, SHA-256
`bb8a2f709add941f23a4723f1a71586800255474d010db1ed94e785a0ebfcb43`.
Release: 283,136 bytes, SHA-256
`4fa2a83fef7b19b67ab09c908001ac8dab4e8bfd6e0ba27da190c0e7bd652ac0`.

---

## 1. Injection and presentation

- **`d3d8.dll` proxy** next to `xops096.exe`, forwarding every call to
  `C:\Windows\system32\d3d8.dll`. Works on the desktop.
- **Borderless window / fullscreen handling**; the device is created at the
  container size and the game's native **640x480** frame is composited at
  **4:3** with no FOV change.
- **Real FOV** sent outbound: `80.7 x 65.0` degrees (the game's measured
  projection), not a guessed 104.5.
- **Exact sync pixel:** 10x10 at `(0,0)`, red echoes `HMD_SYNC` rounded and
  clamped; green 0. In AER the blue channel carries the eye flag.
- **`MODE_VR` from game state**, not draw counts: mission index `0x490A44`,
  result `0x490A48`, player index `0x48DDD8`. Gameplay 1, title/menu/death/
  victory 0.
- **Reset/device recovery:** restores a minimized or hidden window before
  `Reset`, retries once on `D3DERR_DEVICELOST`; target-resolution retry falls
  back to 640x480.

## 2. Camera and 6DOF

- Camera position/angles **solved and writable**; cluster A (render copy) and
  cluster B (movement) both written.
- **Settled signs:** `yawSign +1`, `pitchSign +1`, `cluster both`; persisted in
  `xops_wxr_steer.ini`.
- Head tracking, recentre, and the right-stick yaw offset (turn composes with
  head look rather than overwriting it).
- **Focus-safe cursor:** when another window has focus, `SetCursorPos` does not
  move the real cursor and the game's look delta stays zero.
- **Hand aim:** while firing, the game's shot is aimed at the laser endpoint
  and the view is restored before the world is drawn.
- **Head roll:** the presented 640x480 game sprite is rotated as a 2D quad in
  `Present` by the head roll. The hands/gun/laser are built on a **no-roll head
  basis** (yaw/pitch only), so they behave like part of the VR camera and stay
  fixed while the world rolls; the HUD and sync pixel are drawn afterwards and
  stay fixed too. `roll` (default on) and `roll_sign` (default `-1`) in
  `config.txt`; `/` toggles roll live. Black corners at the sprite corners are
  accepted because the HMD lenses hide them.
- **Render-side 6DOF and eye height:** the game re-clamps its own camera Y
  every frame, so the HMD translation is **not written to the game's
  position**. Instead `CameraOffset()` rotates the head's `headP` into the
  camera frame and the world vertex copies are shifted opposite the eye in
  `AerDrawPrimitive`/`AerDrawIndexed`. Hands, gun and laser subtract the same
  clamped, `pos_scale`-scaled `ScaledHeadP()` plus `eye_height`, so they are
  **world-anchored at the recentre position**: moving the HMD moves the view,
  not the hands. They are not counter-rotated against the sprite roll; the
  whole world and the hands roll together.
  Both world paths are camera-space **metres** - the old `*100`
  centimetre assumption on the non-indexed path was the cause of the view
  flying through floors and ceilings and is gone. **`DrawPrimitive`'s third
  argument is a primitive count, not a vertex count:** the copy path now runs
  it through `PrimVertexCount()` (3 vertices per triangle). Previously it
  copied only one third of a triangle list and the remaining two thirds of
  the draw read stale buffer memory, so *any* nonzero offset tore the frame
  apart. The indexed path also passes the real `stride` now instead of a
  hardcoded 28. HMD Y is clamped to **±1 m**
  (far more than a sit/stand change needs) *before* the gain; `pos_scale` is
  the gain on the tracked translation (default **1.0** = 1:1, range 0..2;
  also editable in the launcher's Options dialog); `eye_height` is a separate
  manual float offset (default `0`, clamped ±5). A **right-stick click** calls
  `SteerRecentre()`, so the current HMD height becomes the flat game player
  height (`0`); a level start does the same from the `LevelActive()` rising
  edge **before** `SteerTick` writes the camera, so the untouched spawn yaw is
  the reference. Head yaw is still written to both camera clusters, so
  stick-forward follows the head. This is visual-only and does **not** add
  collision: standing or leaning into geometry can still clip, and the
  intended mitigation is lowering `pos_scale`, not solving world collisions.
  Verified with a ±1 m sine feed at `pos_scale = 1.0` (log `dy` tracked `hmdY`
  1:1, clamped at ±1.000) and a constant 0.8 m re-zero (0.000 -> 0.800 ->
  click -> 0.000). After the count fix, back-buffer grabs at zero, +Y, +X and
  +Z are all coherent; the log confirms `verts = 3 * count`.

## 3. HUD

- **Replacement target-resolution HUD** inside the centre square: text and
  rounded panels, ammo/reserve, weapon names, `STATE` colours, and
  `RELOADING` / `CHANGING` / `DEAD` status.
- Game's 2D HUD suppressed in gameplay **only when the custom HUD is valid**
  (texture failure falls back to the game HUD); crosshair draws removed.
- Title, briefing, menus, objective, death and victory keep the game's own 2D
  UI, now shown as the centre-third floating panel with `MODE_VR` still 1 and
  `MODE_3D=0`. The custom HUD is **gameplay-only**, so the death
  summary/stats screen is not covered by it.
- The game's spinning **3D weapon-slot HUD is hidden** while the custom text
  HUD is active; the text HUD is the only slot display. The capture and tail
  recording still run before the draw is skipped, so the VR hand's cloned
  weapon model still loads on spawn, swap, drop and pickup. `K` restores the
  game's slots for comparison.

## 4. Hands and weapons

- **Both virtual hands** at their XrApi poses, depth-tested against the world.
- Hand model: compact dark **palm plate**, four **short chunky finger slabs**
  and a thick **thumb angled outward** from its own side; all glove-textured
  with black outlines. Shape follows the SixDOFinator calibration reference.
- **Quest 3 hand roll is the default:** `hand_flip = false` applies the 180°
  roll correction; `hand_flip = true` recreates the old roll for non-Quest-3
  devices.
- **Dominant-hand laser**, depth-tested; a continuous strip that walls cut off.
- **Weapon clone:** captured from the game's own indexed draws, multi-part via
  the shared vertex buffer, frozen after capture, follows swaps, drops,
  pickups and empty slots; grenade scaled 1/3 and mapped vertical, MAC 10 at
  0.75.
- **Negative-UV textures fixed** with `D3DTADDRESS_WRAP` (AUG/M1/Glock18/
  MK23 class).
- **Empty/drop latch and ammo sanity**, so a transient NONE cannot capture a
  world prop and an ammo refill forces a recapture.
- **Per-weapon hand offset tooling:** `Q` + numpad slides the hand box, `T`
  saves the offset for the current weapon (currently in memory only; see
  `NEXT_STEPS.md`).
- **Two-handed aim:** holds while the **left grip** is down; the gun stays on
  the dominant hand and points at the off hand, keeping gun-hand roll. The
  laser and the hand-aim shot use the same basis. Shots buzz **both**
  controllers while it is held. Left grip no longer sends `TAB`.

## 5. Input

- **VR controller mapping:** left stick move; right stick turn L/R; left grip
  two-handed aim; left trigger fire mode; right trigger shoot; right grip jump;
  B reload; A switch; X drop; Y zoom (toggle). Left-handed mode swaps only
  shoot and fire mode. **The laser is always on**; Y no longer hides it.
- **VR input isolation:** in gameplay and on the floating non-VR panel, *all*
  physical keyboard and mouse input is removed from `GetKeyboardState`, and the
  window procedure swallows the corresponding `WM_KEY*`/`WM_MOUSE*` messages.
  Only `ESC` (the way out) and `0` (PC-only manual mode) survive. This fixes
  the arrow-key -> weapon-swap leak through either input path. Mono and AER
  menus behave the same: there is no desktop input while the panel is up.
- **Left-stick-as-arrows fallback:** WinlatorXR currently maps the left stick
  to the arrow keys. Those raw presses are captured before the filter and
  translated to W/A/S/D, so the stick moves the player instead of swapping
  weapons. Mapping the stick to the analog XrApi left stick remains cleaner.
- **Floating-panel laser pointer:** the dominant hand's ray is intersected with a 1 m
  plane, mapped onto the centre-third panel, published through `GetCursorPos`
  in 640x480 UI space, and clicked with the right trigger (`VK_LBUTTON` plus
  posted `WM_LBUTTONDOWN`/`WM_LBUTTONUP`). A screen-space dot marks the same
  point.
- **Manual PC mode (`0`):** default two-hand pose, cursor centred once, right
  hand driven by `E`/`Q` + numpad; incoming 6DOF ignored. **Every debug key is
  locked behind `0`:** while VR input is active, only `0` is accepted, and the
  probe/HUD/window/dump keys do nothing until manual mode is on. WinlatorXR
  does not map `0` by default, so a device session cannot trigger them by
  accident.
- **Focus boundary:** while the game window is unfocused, **physical desktop
  input** is muted (keyboard, mouse, debug keys) but **XrApi/runtime input
  still arrives**, because the UDP Tester normally holds focus.
- **Reserved keys:** physical A/B/X/Y and all four arrows are never accepted in
  VR mode, on any screen. Manual PC mode is exempt.
- Edge-pulsed buttons where the game polls once per frame; held walk/shoot
  where appropriate.

## 6. Haptics

- Outbound fields 1-2 are a **duration in frames**, per hand (protocol fact,
  not an amplitude). Values `<= 0` are ignored by the runtime, so the value is
  decayed locally and re-sent.
- One pulse per shot, **2 frames** by default, decayed **before** sending (the
  SHOGO order: runtime sees `1` then `0`).
- Driven by magazine-counter drops; a drop over 8 rounds is treated as a
  reload/pickup/respawn and ignored.
- Dominant hand by default; **both hands while two-handed aim is held**.
- **Outbound 7278 is sent every frame** (menus included), with **0 haptic
  durations when nothing is buzzing**, and a first frame at startup. `MODE_VR`
  and `MODE_3D` are only re-read from an outbound packet, so skipping idle
  frames stalls a mode switch. The packet is sent before `Present`.
- Config: `haptics`, `haptics_hand` (dominant/both/left/right),
  `haptic_frames` (1..5).
- Desktop-verified with UDP capture (`0.0 1.0 ...` per shot, both hands with
  the grip held).

## 7. Diagnostics and tooling

- **`verbose_debug`** (default `false`): informative diagnostics are gated;
  errors and game-state changes always log. The test PC runs with `true`.
- **`.` grabs two consecutive composited frames** (`xops_frame_<n>.bmp`), which
  is exactly an AER left/right pair; the back buffer is the reliable capture
  path on this box.
- **`tools/start-level.ps1`** replays the title -> mission -> briefing clicks;
  `record_clicks.py` records new sequences by process id.
- Weapon-hunt keys `J/K/N`, draw dump `V`, HUD comparison `K`, window `F`,
  target res `M`, verbose diagnostics on demand. All are **locked behind `0`**
  while VR input is active.

## 8. Experimental stereo — AER

- **AER is implemented and desktop-verified.** `stereo = aer` opts in;
  `mono` is the shipping default.
- **Runtime stays `MODE_VR=1` for the whole run in mono and AER**, following
  HWXR. Only `MODE_3D` toggles: `2` in AER gameplay, `0` on title/menu/death/
  objective/victory and throughout mono. This avoids the `MODE_VR 1 -> 0`
  transition that killed the runtime on device.
- **Non-VR screens become a floating panel** in every stereo mode: the game's
  640x480 frame is drawn at one third size in the centre cell of a 3x3 grid,
  with a dominant-hand laser pointer and right-trigger click.
- The camera is CPU-baked, so AER works by **reprojecting each draw**: both the
  non-indexed world path and the indexed model path shift x by `-d` in metres.
  (Both are camera-space metres: the projection is near 0.1 / far 80 and the
  player's own Y is ~1.9. The earlier non-indexed `-d*100` was a centimetre
  assumption and is removed.) Shared vertex buffers are copied, never modified.
- One eye per presented frame; `MODE_3D = 2`; sync pixel blue 255 = right.
  The eye flips after `Present`, so image, flag and log agree.
- Desktop evidence: pair grabs `eye=left`/`eye=right`; full-frame MAD 8.07
  between eyes vs 0.033 for mono; depth-dependent 5-12 px shift at 1920.
- **Still open:** device eye sign, and SBS. See `NEXT_STEPS.md`.

---

## 9. Launcher (Play / Options / update check)

`launcher-xr/` contains a Win32 `XOPSVR.exe` built with `build.cmd`:

- **Play** finds `xops096.exe` next to the launcher (or up to two folders up)
  and starts it.
- **Options** edits `config.txt` **in the game folder**: handedness, Flip
  hands, haptics on/off, haptics hand, verbose debug, stereo (mono/AER), roll
  direction, **HMD position gain (`pos_scale`)** and **HUD scale
  (`hud_scale`, 0.5-2.0, 1.0 = native)**. Haptic frame length and head roll
  are intentionally not exposed; head roll is always on and the frame count is
  preserved.
- **Update check** reads `version.txt` and `notes.txt` over HTTPS (WinHTTP),
  compares by value (same rules as the Shogo launcher), shows the notes in a
  YES/NO box, and logs to `XOPSVR_update.log`. For now the channel is the
  **Shogo WXR AutoUpdate URLs** for testing, and the DLL download step is
  deliberately disabled - YES only reports that downloads are off and writes
  nothing.
- A legacy `config.txt` one folder above the game is migrated into the game
  folder on first use.

Verified on the PC: the update prompt shows Shogo 1.2 against local 0.1, the
Options dialog saves `E:\tmp\Q3Mods\xops096\config.txt`, and Play launches the
game, which then reads that same file.

---

## How this was tested on PC

- Desktop runs through the XR UDP Tester and synthetic feeders, in the game's
  own levels and in a NONE/NONE level for the hands.
- Camera, window, composite, HUD, sync pixel, FOV, input gating and haptics
  were each checked against logs, UDP captures and back-buffer BMPs.
- Hand pose work used the exact `Docs/general_wxr/SixDOFinatorCalibration/
  SamplePoseData.txt` packet.
- Device-only questions (comfort, latency, haptic strength, AER eye sign) are
  deliberately left for the Quest 3 pass and `BUG_FIXES.md`.
