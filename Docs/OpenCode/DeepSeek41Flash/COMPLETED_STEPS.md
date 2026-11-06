# XOPS-WXR â€” COMPLETED STEPS

**Everything in this file is implemented.** Unless a line says otherwise it is
also **desktop/PC-verified**. Device verification on the Quest 3 is a separate
pass; `BUG_FIXES.md` will be created from that run.

**Last build covered:** `built Nov  6 2026 09:49:19`, SHA-256
`53b831374c82e436a037d2e8f591e605bac9dc357d29e2df5b5c89be482e0bad`.

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

## 3. HUD

- **Replacement target-resolution HUD** inside the centre square: text and
  rounded panels, ammo/reserve, weapon names, `STATE` colours, and
  `RELOADING` / `CHANGING` / `DEAD` status.
- Game's 2D HUD suppressed in gameplay **only when the custom HUD is valid**
  (texture failure falls back to the game HUD); crosshair draws removed.
- Title, briefing, menus, objective, death and victory keep the game's own 2D
  UI (`MODE_VR 0`). The custom HUD is **gameplay-only**, so the death
  summary/stats screen is not covered by it.
- 3D weapon-slot models are left untouched (where to place them is still an
  open decision, see `NEXT_STEPS.md`).

## 4. Hands and weapons

- **Both virtual hands** at their XrApi poses, depth-tested against the world.
- Hand model: compact dark **palm plate**, four **short chunky finger slabs**
  and a thick **thumb angled outward** from its own side; all glove-textured
  with black outlines. Shape follows the SixDOFinator calibration reference.
- **Quest 3 hand roll is the default:** `hand_flip = false` applies the 180Â°
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
  shoot and fire mode.
- **Manual PC mode (`0`):** default two-hand pose, cursor centred once, right
  hand driven by `E`/`Q` + numpad; incoming 6DOF ignored.
- **Focus boundary:** while the game window is unfocused, **physical desktop
  input** is muted (keyboard, mouse, debug keys) but **XrApi/runtime input
  still arrives**, because the UDP Tester normally holds focus.
- **Reserved keys:** physical A/B/X/Y are never accepted in VR mode; the four
  arrows are not accepted while `MODE_VR = 1`. Manual PC mode is exempt.
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
  target res `M`, verbose diagnostics on demand.

## 8. Experimental stereo â€” AER

- **AER is implemented and desktop-verified.** `stereo = aer` opts in;
  `mono` is the shipping default.
- The camera is CPU-baked, so AER works by **reprojecting each draw**: the
  non-indexed world path (centimetres) shifts x by `-d*100`, the indexed model
  path (metres) by `-d`; the hand/laser pass shifts its view. Shared vertex
  buffers are copied, never modified.
- One eye per presented frame; `MODE_3D = 2`; sync pixel blue 255 = right.
  The eye flips after `Present`, so image, flag and log agree.
- Desktop evidence: pair grabs `eye=left`/`eye=right`; full-frame MAD 8.07
  between eyes vs 0.033 for mono; depth-dependent 5-12 px shift at 1920.
- **Still open:** device eye sign / centimetre scale, and SBS. See
  `NEXT_STEPS.md`.

---

## 9. Launcher (Play / Options / update check)

`launcher-xr/` contains a Win32 `XOPSVR.exe` built with `build.cmd`:

- **Play** finds `xops096.exe` next to the launcher (or up to two folders up)
  and starts it.
- **Options** edits `config.txt` **in the game folder**: handedness, Flip
  hands, haptics on/off, haptics hand, verbose debug, stereo (mono/AER) and
  roll direction. Haptic frame length and head roll are intentionally not
  exposed; head roll is always on and the frame count is preserved.
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
