# XOPS-WXR — CURRENT STEPS / handover

**Date:** 6 Nov 2026, DeepSeek V4.1 Flash session.
**Read with:** `README.md` (architecture), `WORKLOG.md` (history),
`NEXT_STEPS.md` (the live plan, including stereo and the launcher).
This file is the live "where we are right now and what is next".

---

## 1. What works

- `d3d8.dll` proxy at the container resolution; 640x480 game render target
  composited to the back buffer (native 4:3, no FOV change).
- Centre-square HUD (text + panels), ammo, STATE colours, RELOADING/DEAD.
- Both VR hands as depth-tested 3D boxes at their XrApi poses.
- Dominant-hand laser, depth-tested, from the hand.
- **Hand aim:** while firing, the game's shot is aimed at the laser endpoint,
  and the view is restored before the world is drawn.
- **Haptics:** one short pulse per shot from the magazine counter, 2 frames
  (SHOGO-matched), dominant hand by default.
- **Two-handed aim:** hold the **left grip**; the gun stays on the dominant
  hand and points at the off hand. Both controllers buzz on firing while it
  is held. No config key.
- **Input boundary:** while the game window lacks focus it receives no input
  at all (keyboard, mouse, sticks, VR buttons, debug keys). In VR mode the
  physical A/B/X/Y keys are always swallowed, and the arrows are swallowed
  while `MODE_VR = 1`. Manual PC mode (`0`) is exempt.
- **Verbose logging** behind `verbose_debug` (default false); errors and
  state changes always log.
- **Manual/user mode (`0`):** ignores incoming 6DOF, installs a default pose
  for both hands, centres the cursor once, and drives the right hand with
  `E` + numpad (rotate) / `E` + numpad (move). `N` = attach, `J/K` = hunt.
- **Game weapon model cloned onto the dominant hand:**
  - The inventory model is captured from the game's own indexed draws.
  - It is frozen after capture (no constant spin).
  - It follows weapon swaps, drops, pickups and empty slots.
  - Grenade is scaled to 1/3 and mapped vertical.
- **Per-weapon hand offset (`Q` + numpad, `T` saves it).**

## 1a. VR controller mapping (owner, 6 Nov)

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick L/R | Turn L / R |
| Left grip (held) | Two-handed aim |
| Left trigger | Fire mode |
| Right trigger | Shoot |
| Right grip | Jump |
| B | Reload |
| A | Switch weapon |
| X | Drop weapon |
| Y | Zoom (toggle) |

- **Walk is gone** (the left grip used to send `TAB`).
- Left-handed mode swaps only shoot and fire mode; every other button is the
  same.

## 2. The weapon-model discovery (important)

The inventory weapon is drawn at the end of the frame as a group of indexed
draws. The group **shares one vertex buffer**; each draw is one submesh with
its own texture/UV range.

Examples measured with the `TAIL` dump:

```
MAC 10:   vb=0AA74660  numV=115  prim=2,45,4,4,2  tex=0407B4F0   (5 parts)
another:  vb=0AA745A0  numV=93   prim=18,46        tex=0407A990   (2 parts)
AUG:      vb=0C743020  numV=133  prim=...          tex=0A69E8F0   (1 part)
```

Rules:

- The **primary weapon** is the last contiguous run of draws sharing a VB.
- The **secondary weapon** is the previous VB group.
- The number of parts varies (1, 2, 5, ...), so fixed `N-2`/`N-3` indices are
  wrong. `N` also changes with level/inventory.
- The game mesh routine's call site is `IDA 0x0042A335`, but many world props
  (cans) use it too, so it is **not** a unique weapon signature.

### Model object layout (from disassembly at 0x0042A335)

```
[esi+0x04]  FVF
[esi+0x24]  device
[esi+0x28]  vertex buffer   <- model vertices
[esi+0x2C]  stride
[esi+0x30]  ? (base vertex)
[esi+0x3C]  index buffer    <- model indices
[esi+0x48]  submesh array
[esi+0x50]  draw-entry array (0x14 bytes each)
[esi+0x54]  entry count
[esi+0x58]  submesh count
[esi+0x0C]  flags (bit 0x10 = indexed path)
```

The indexed draw entry has `+04` base vertex, `+08` index count, `+0C/+10`
min/max index.

### Vertex format

- Stride **20** = `XYZ + TEX1`: u,v at floats **3,4**.
- Stride **24** = `XYZ + DIFFUSE + TEX1`: u,v at floats **4,5**.
- Stride **28** = `XYZRHW + DIFFUSE + TEX1`: u,v at floats **5,6**.

`CaptureGunModel` chooses the UV offset by stride.

### The black-texture bug (AUG / M1 / GLOCK18 / MK23)

The AUG log showed:

```
HIDE model fmt: stride=20 fvf=0x0144 uv0 -0.667, 0.181 uv1 -0.667, 0.231 tex=0A69E8F0
```

The UVs are **negative** (texture atlas offsets). The composite sets
`D3DTSS_ADDRESSU/V = CLAMP`, so the sample lands on the black edge.

**Fix applied in source but NOT yet built/deployed:**
`HandDrawCapturedGun` now sets `D3DTADDRESS_WRAP` for stage 0 before drawing
the part. Rebuild and retest AUG/M1/Glock18/MK23.

## 3. Capture pipeline (current code)

- `RecaptureWeapon()` (weapon change/pickup/sanity) clears all parts,
  releases their textures, sets `g_needParts`, `g_weaponVB = NULL`.
- At `EndScene`, if a capture is pending and `g_weaponVB` is null, take the
  **last tail draw's VB** and remember it; the primary weapon is the last VB
  group.
- On the following frame, every indexed draw with that VB is captured as a
  part (verts + indices + texture + axis mapping + scale).
- At `EndScene`, if parts were captured, `g_needParts = FALSE`; they are
  drawn frozen from then on.
- `HandDrawCapturedGun` loops the parts and draws each with its own texture.
- The original HUD copy is left untouched.

## 4. Empty / drop / pickup logic

`WatchWeaponSwap()` reads the player struct (`0x48DF00 + idx*0x50`).
The custom HUD primary rule is `active = sel ? w1 : w0` (0 = "None").

- The slot fields can lie briefly during a swap, so `g_weaponEmpty` requires
  `active == 0` **and** total ammo `cur0+cur1+res0+res1 == 0`, sustained for
  10 frames (`g_emptyCount`).
- **Drop latch:** any transition of a slot from non-zero to 0 (a deliberate
  drop, or the grenade emptying to NONE) sets `g_dropLatch`. While latched,
  the model is cleared and **no capture is attempted**; the latch clears only
  when a slot gains a weapon or the player switches to a non-empty slot.
  This prevents a transient NONE from capturing a world can.
- **Ammo sanity:** skipped while latched; otherwise if the hand is marked
  empty but any slot has ammo, force a recapture (`HIDE sanity: ammo ...`).

## 5. Manual controls

- `0` — toggle user-control mode. ON: default hand pose, cursor centred once,
  XrApi ignored. OFF: XrApi resumes.
- `E` + numpad 8/5 — pitch; 4/6 — yaw; 7/9 — roll.
- `E` + numpad 8/5/4/6/7/9 — move the hand (up/down/left/right/back/fwd).
- `Q` + numpad — slide the **hand box only** relative to the gun.
- `T` — save the hand offset for the current weapon (logged with its name).
- `J` — previous draw (hunt), `K` — next draw (hunt), `N` — attach.
- `V` — draw-order dump; `H` — HUD scale; `F` — fit window; `M` — target res.

## 6. Per-weapon tuning

- `g_gunScaleMul`: 1/3 for names starting `GREN`, 0.75 for `MAC*`, else 1.
- `g_gunVertical`: true for `GREN`, mapping the long axis to hand-up.
- `g_handAdj[3]`: per-weapon hand offset, table `g_handAdjTable[32][3]`.
  Currently in memory only — **not persisted to config yet**.

## 7. Build / test

Build + deploy:

```
powershell -File build\build-probe.bat
```

or the agent helper `deploy-probe.bat`. Deploy target:
`E:\tmp\Q3Mods\xops096\d3d8.dll`.
Game log: `E:\tmp\Q3Mods\xops096\xops_wxr_probe.log`.

**Config is `E:\tmp\Q3Mods\config.txt`** (one directory above the game):

```
handedness = right
hand_flip = false
haptics = true
haptics_hand = dominant
haptic_frames = 2
verbose_debug = true     # this test PC; ship with false
```

Useful log lines (most need `verbose_debug = true`):

```
HIDE  weapon VB 0x...; capturing parts next frame
HIDE  model fmt: stride=.. fvf=0x.... uv0 .. .. uv1 .. .. tex=0x....
HIDE  part N: <verts> v <indices> i tex=0x...
HIDE  assembled N part(s) for '<weapon>'
HIDE  current weapon NONE: hands only
HIDE  sanity: ammo ... with empty hand - showing weapon
HAND  saved adj '<weapon>' (id N): x y z
HAND  two-handed aim ON: dom x y z  off x y z  aim x y z
HAPTIC fires=N dur=F frames hand=both|left|right ...
TAIL  idx N  vb=0x... numV=.. prim=.. tex=0x...
```

Last built/deployed (haptics + two-handed aim + input mute + verbose):
`built Nov  6 2026 04:27:08`, SHA-256
`cdad87a2f59813e1a9ebcb00a3ccedea79e48359acb1439e896a68df0ddc19b5`.

## 8. Next steps

The live list is `NEXT_STEPS.md`. Immediate order:

1. Persist `g_handAdjTable` and per-weapon scale in `config.txt`.
2. Off-hand pose/offset in two-handed aim.
3. Per-weapon muzzle mapping.
4. Device test checklist (NEXT_STEPS section 5).
5. **Experimental:** true stereo, SBS first with AER fallback, using
   HaloCEWXR. May be postponed until after V1.0.
6. **Bonus:** custom EXE launcher (Shogo `launcher-xr`), auto-update later.

## 9. Standing rules

- Do not change the game's FOV/projection; native 4:3.
- `MODE_VR=0` on title/menu/death/victory; desktop input must work there.
- `MODE_VR=1` only in gameplay.
- Reuse the game's models and effects; do not invent replacements.
- Only `0` remains from the old number keys; numpads, `J/K/N/T/Q/E` are ours.
- `SetCursorPos` must never move the real cursor when the game is unfocused.
- **No input may cross between windows:** unfocused means mute, debug keys
  included.
- **A/B/X/Y and (in `MODE_VR=1`) the arrows are never taken from the physical
  keyboard in VR mode;** manual PC mode (`0`) is the exception.
- `verbose_debug = false` ships; diagnostics never replace errors/state.
- Normalise every quaternion on ingest; freeze on the last good pose.
- Never commit XOPS's own data files or binaries.
