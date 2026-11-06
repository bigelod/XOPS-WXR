# XOPS-WXR — NEXT STEPS

**Session:** 5–6 Nov 2026 (DeepSeek V4.1 Flash).
**State:** see `CURRENT_STEPS.md` for what is built and how it works.
**Last build:** `built Nov  6 2026 02:44`, SHA-256
`9fa03f69c3ac0eca979186918e802768e1c86747cf4f979deb8b7eeffe782c66`,
deployed to `E:\tmp\Q3Mods\xops096`.

Work items for the next session, in the order the owner asked for them.

---

## 1. Haptic feedback over UDP on ammo decrease

**Goal:** vibrate the controllers in proportion to how fast the weapon is
firing, driven by the ammo counter going down.

**Where the data is** (`ReadHudVals`, player base
`0x48DF00 + idx*0x50`, idx from `0x48DDD8`):

```
+0x0C / +0x0D   current magazine ammo for the two slots
+0x0E / +0x0F   reserve ammo
+0x0A / +0x0B   weapon ids in the two slots
+0x06           selected slot
```

**Plan:**

- Track the previous frame's current-ammo pair (`cur0`, `cur1`).
- On a decrease, compute the number of rounds fired this frame.
- Send a UDP packet to the runtime with:
  - hand side (dominant / both),
  - intensity scaled by rounds-per-second (e.g. `min(1.0, rounds * 60 / 30)`
    for automatic fire, a fixed pulse for single-shot),
  - duration (e.g. 40–80 ms).
- Rate-limit to the frame rate so a 60 Hz stream is not flooded.
- Add a config key, e.g. `haptics = true`, and a debug log
  `HAPTIC fires=N intensity=...` so it can be verified without a headset.

**Open:** confirm the runtime's haptic UDP port and packet format with the
WinlatorXR/XrApi spec. The audio/pose stream is on `127.0.0.1:7872`; haptics
may be a separate port or an extra field. Do not guess the wire format:
read the runtime source/spec first.

---

## 2. Finish calibrating weapon holds (primary / dominant hand)

**Goal:** a good, saved grip offset for every weapon.

**Tooling already in the build:**

- `Q` + numpad slides the **hand box only**; the cloned gun stays put.
  `8/5` up/down, `4/6` left/right, `7/9` back/fwd.
- `T` saves the offset for the current weapon id and logs:

  ```
  HAND saved adj 'AUG' (id 14):  0.010 -0.020  0.030
  ```

**Work:**

- Visit every weapon and save the offset with `T`.
- Persist `g_handAdjTable[32][3]` to `config.txt` (or a new
  `weapon_tuning.txt`) keyed by weapon id and name, and load it at start.
- Keep the existing per-weapon scale/vertical flags alongside:
  - `GREN*`: scale 1/3, vertical mapping.
  - `MAC*`: scale 0.75.
  - Sniper rifles (PSG1 etc.) may need their own scale/offset.
- Weapons seen so far: MP5SD/MP5, MK23 SD, AUG, M1, GLOCK18 SEMI, PSG1,
  MAC 10, shotgun, grenade, plus whatever else the inventory lists
  (the weapon-name table is at `0x465DA0`, ids 0..31).

---

## 3. Two-handed aiming (config-gated)

**Config:** `two_handed = FALSE` (default) / `TRUE` in `config.txt`.

When TRUE:

- The off-hand is placed by the same method used for the dominant hand:
  a modifier key (owner suggested `X`) + numpad.
- `T` saves the off-hand offset per weapon.
- The primary hand is settled first (existing `Q` + numpad / `T`).
- Both hands are then drawn at their calibrated poses; the weapon stays
  attached to the dominant hand.
- Persist off-hand offsets per weapon id, same as the primary.
- Keep the default FALSE so single-hand aiming is unchanged.

---

## 4. Map each weapon's barrel end (muzzle)

**Goal:** a per-weapon muzzle point so the game's own muzzle flash / smoke
can be shown at the right place (do not draw our own effects).

**Method:** reuse the calibration mapping. For each weapon:

- Place a virtual point at the barrel tip using
  `Q`/`X`-style hand offsets (or a dedicated muzzle offset in the same
  per-weapon table).
- `T` (or a new key) saves `muzzle_offset[3]` per weapon id.
- Grenade is not applicable.

**Use:** when the game spawns its muzzle flash / smoke, translate the effect
draws to the dominant hand's muzzle point. That is the "reuse the game's
effects" path; no new effects are authored.

---

## 5. Test on device

Checklist for the Quest 3 / WinlatorXR run:

- Container at **1280x960** (native 4:3, no bars).
- `MODE_VR` 1 only in gameplay; 0 on title/objective/menu/death/victory;
  desktop keyboard/mouse works on those screens.
- Both hands visible at their 6DOF poses, depth-tested.
- Dominant laser and hand-aim hit where the laser points.
- Weapon clone appears, follows swaps, drops to hands-only on drop/empty
  (including grenade fired empty), returns on pickup/switch.
- Per-weapon textures (AUG/M1/Glock18/MK23 after the WRAP fix).
- Grenade vertical and correctly sized; MAC 10 smaller.
- Manual mode (`0`): default pose, `E`+numpad, `Q`+numpad, `T`.
- Haptics feel proportional to fire rate.
- Latency/comfort: check tracking latency, judder, reprojection.

---

## Known state / loose ends

- Weapon capture: multi-part by shared vertex buffer; primary = last VB
  group, secondary ignored. Parts are frozen after capture.
- Textures were fixed by `D3DTADDRESS_WRAP` for negative atlas UVs.
- Empty/drop handling: explicit switch to an empty slot, drop, or grenade
  empty latches hands-only until pickup/switch.
- `g_handAdjTable` and the per-weapon scale table are **in memory only**;
  persistence is still to do (item 2).
- If a texture is still black, capture logs
  `model fmt: stride=.. fvf=.. uv0 .. .. tex=..`; check for a second
  texture stage/lightmap or a multi-texture model.
- Draw-order hunt keys (`J/K/N`) and the `TAIL` dump remain available for
  diagnosing any new weapon.

---

## Key files

```
src/d3d8probe.cpp                    all hooks, HUD, hands, laser, weapon clone
src/xrapi.cpp/.h                     XrApi parsing, poses, input
Docs/OpenCode/DeepSeek41Flash/CURRENT_STEPS.md   current architecture/state
Docs/OpenCode/DeepSeek41Flash/WORKLOG.md         experiment history
build/build-probe.bat                build/deploy
tools/start-level.ps1                title -> briefing -> level
E:\tmp\Q3Mods\xops096\xops_wxr_probe.log         runtime log
E:\tmp\Q3Mods\config.txt                         handedness / hand_flip
```
