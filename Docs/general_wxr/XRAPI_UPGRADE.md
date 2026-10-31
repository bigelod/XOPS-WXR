# Upgrading a port to a newer XrApi version

**Who this is for.** An agent or developer with a working port on XrApi 0.4 or
0.5 that wants the newer versions' data or performance. Read
`XRAPI_V0_4_DESC.md` section 8 first for what each version is.

> **Read this as reference, not as a roadmap.** In practice these ports stay on
> **0.4 or 0.5**, and that is the recommendation. The 0.6 and 0.7 material
> below exists so an agent *knows those versions are there* and can dig
> further if a specific need turns up - analog triggers being the likeliest.
> Nothing in a normal port should be waiting on an upgrade, and "we are on an
> old version" is not a problem to be fixed.

**Provenance.** Field orders here are read from the runtime's own encoders
(`XrVersion05` to `XrVersion07` in WinlatorXR's
`app/src/main/java/com/winlator/xr/api/`) and the spec at
<https://winlatorxr.github.io/xrapi.html>. The upstream (app to runtime)
format for 0.6 is from the spec. Space *type* values are OpenXR's and are
marked where this document has not verified them - check
`OpenXR-WXR/src/WinXrApiUDP.cpp`, which is a working 0.6/0.7 client, before
relying on them.

---

## 1. Start at 0.4 or 0.5. Genuinely.

Not as a beginner's compromise - as the right engineering choice for a first
port, and for most ports.

**0.4 and 0.5 are human-readable.** One line of ASCII per frame that you can
read in a log, diff against another capture by eye, and paste back into a
tester:

```
client0 0.210 0.290 -0.930 0.320 0.0 0.0 -0.091 -0.215 -0.284 ... 64 TFFFFFFFFFFFFFFFFFF META
```

When a hand ends up in the wrong place, you can *see* whether the data or your
maths is at fault. That is worth more on a standalone device - no debugger,
slow deploy cycle - than any amount of saved CPU.

**The XrAPI UDP Tester speaks them.** You can run the whole VR path on a
desktop, with a real debugger, and hand-edit a packet to reproduce a specific
pose. See `TOOLS.md`.

**They give poses directly.** Head and both hands arrive as quaternions and
positions in the packet, every frame, with nothing to declare or request.

**Nothing before 0.6 changes the packet shape**, so 0.1 to 0.5 share one
parser, and 0.5 is 0.4 plus the InputAPI listener at no cost. **Ask for 0.5
rather than 0.4 unless you have a reason not to** - you may want keyboard or
mouse injection later, and it costs nothing to have it available.

Get the port *working and verified on device* at 0.4 or 0.5. Only then
consider upgrading, and only for a reason you can name.

---

## 2. Reasons to upgrade, and reasons not to

### Upgrade to 0.6 if you need

- **analog triggers and squeezes** - 0.4 has them only as booleans, and there
  is no way to recover an axis from a bit. This is the strongest reason on the
  list.
- **arbitrary spaces** - anything beyond head and two hands
- **the display refresh rate** (`HMD_FPS`) - not knowable from inside the
  container, so genuinely unavailable otherwise, though rarely load-bearing
- to **implement an OpenXR runtime**, which is what 0.6 was shaped for
- **pose velocities** - but see below, because you can usually compute these
  yourself

### Velocity is a weak reason - derive it instead

Any mod that runs its own code per frame can finite-difference what it already
receives:

```c
VEC_SUB(vVel, vHandNow, vHandPrev);
DFLOAT fSpeed = VEC_MAG(vVel) / fDt;
```

That is exactly how Shogo's gesture melee works, and it works well. So do not
upgrade for velocity alone - you would be paying the whole 0.6 migration for
three lines of arithmetic.

Two things to get right when differencing, both of which the runtime's
velocity would have handled for you:

- **Gate on a fresh frame.** `HMD_SYNC` changes when the packet is new. If you
  difference on every game frame regardless, a duplicated or dropped packet
  reads as a hand that stopped dead and then jumped, which produces phantom
  gestures. Advance your previous-position only when the sync value moved, and
  divide by the elapsed time since *that* sample.
- **Mind the quantisation.** 0.4 sends positions at three decimals, so one
  millimetre is the smallest step. At 72 Hz that single quantum is already
  about 0.07 m/s of noise in a differenced value. Fine for "did they swing?";
  not fine for a threshold near walking pace. Smooth over two or three samples
  if you need the low end.

**When the runtime's velocity is genuinely better:**

- **Angular velocity.** Differencing *positions* is trivial; differencing
  *orientations* means taking a relative quaternion and converting it to an
  axis-angle rate, which is easy to get subtly wrong and unpleasant to debug.
  If you need rotational rates, 0.6 hands them over.
- **Noise.** The runtime's figures come from the headset's own fusion rather
  than from differenced, decimal-rounded samples.
- **Validity.** 0.6 carries velocity flags, so you know when a value is
  meaningless instead of inferring it from a suspicious number.

### Upgrade to 0.7 if

- packet formatting or parsing shows up **in a profile**. That is the whole
  benefit: identical data, no text handling on either side.

### Do not upgrade

- because a higher number sounds better. 0.6 is a **different packet**, not a
  refinement, and it costs you the readable log and the tester.
- while anything about the port is still unverified on device. Upgrading
  changes the data path; do it from a known-good baseline or you will not know
  which change broke what.

### What you give up

| | 0.4 / 0.5 | 0.6 | 0.7 |
|---|---|---|---|
| Readable in a log | yes | mostly - but poses are in a space list | **no** |
| Hand-editable in the tester | yes | awkward | **no** |
| Poses without declaring spaces | yes | **no** | **no** |
| Per-frame text cost | yes | yes | **no** |

Keeping the 0.4 parser behind a switch is cheap and keeps the diagnostic
route open. Several of the harder bugs in these ports were solved by reading a
packet.

---

## 3. 0.4/0.5 to 0.6 - what actually changes

**This is not a parser tweak. It is an architecture change**, because poses
stop arriving unasked.

### Inbound: the new packet

```
client<N>
  L_THUMBSTICK_X L_THUMBSTICK_Y R_THUMBSTICK_X R_THUMBSTICK_Y
  L_TRIGGER L_SQUEEZE R_TRIGGER R_SQUEEZE
  HMD_IPD HMD_FOVX HMD_FOVY HMD_FPS
  HMD_SYNC HMD_RECENTER
  <19 button chars>
  <locate count> [ <space> <base space> <x y z qx qy qz qw> <velocity x7> ] ...
```

Compared with 0.4:

| | 0.4 | 0.6 |
|---|---|---|
| L/R/HMD quaternions and positions | **in the packet** | only via the space list |
| sticks | 4 floats | 4 floats, unchanged |
| triggers / squeezes | booleans only | **analog floats** |
| IPD, FOVX, FOVY | present | present |
| refresh rate | - | **`HMD_FPS`** |
| recenter | - | **`HMD_RECENTER`** |
| sync | present | present |
| 19 button chars | present | present |
| immersive / SBS flags | appended | appended |

Poses carry **five** decimals, not three: at three, a quaternion rounded to
about a twentieth of a degree. Velocities carry three. A pose that could not
be located arrives as plain zeros.

### Upstream: you must declare what you want

The app-to-runtime message on 7278 grows. After the fields you already send:

```
L_HAPTICS R_HAPTICS MODE_VR MODE_3D HMD_FOVX HMD_FOVY
  <reference space count>  [ <space> <type> <x y z qx qy qz qw> ] ...
  <controller count>       [ <space> <type> <grip> <x y z qx qy qz qw> ] ...
  <locate count>           [ <space> <base space> ] ...
```

- **controller `type`**: `1` = left, `2` = right
- **controller `grip`**: `0` = aim, `1` = grip
- **`space` indices are yours to assign** - they are handles you choose and
  then refer to in the locate list
- **`type` for reference spaces** follows OpenXR's `XrReferenceSpaceType`
  *(unverified here - confirm against `OpenXR-WXR/src/WinXrApiUDP.cpp`)*

The runtime returns a pose and velocity for **each locate pair you asked
for**, and nothing else. Ask for nothing and you get no poses at all - which
is the most likely way this migration first fails.

Messages can be truncated: sending only the leading fields is valid, which is
how a 0.4-style state message keeps working. **But a truncated message
declares no spaces**, so a port that upgrades its version file without adding
the declarations gets buttons, sticks and sync, and no tracking. If head and
hands vanish after switching to 0.6, this is why.

### A migration order that keeps you sane

1. **Change nothing but the parser first.** Keep sending your existing 0.4
   state message, bump `version` to `0.6`, and confirm you still get buttons,
   sticks, triggers and sync. Expect no poses.
2. **Then add the declarations** - one reference space, two controller spaces,
   three locate pairs - and confirm poses arrive.
3. **Then wire the poses in**, replacing your old per-controller fields.
4. **Then adopt the extras** you came for: analog triggers, velocities,
   refresh rate.
5. Keep the 0.4 path behind a flag until the 0.6 path is device-verified.

### Watch for

- **`version` is read once, at startup.** You cannot switch versions mid-run,
  and an unrecognised string means *no data at all* rather than an error.
- **Coordinate conventions are unchanged** - still OpenXR's right-handed, +Y
  up, -Z forward, metres. Your existing conversion carries over.
- **Still normalise quaternions on ingest.**
- **Five-decimal poses** may expose precision assumptions in your parser.
- A pose list entry for a pair that could not be located is zeros, not
  absent - treat zeros as "no data", not as the origin.

---

## 4. 0.6 to 0.7 - the same data, in binary

The cheap step, once you are on 0.6. **Identical fields in identical order**,
little-endian. Only the inbound direction changes; **what the app sends back
is unchanged.**

```
magic        4 bytes   'W' 'X' 'R' 0x07
12 floats              L stick x,y   R stick x,y
                       L trigger, L squeeze, R trigger, R squeeze
                       IPD, FOVX, FOVY, refresh rate
4 ints                 HMD_SYNC
                       HMD_RECENTER
                       button bitfield   bit i = ControllerButton i, 19 bits
                       space count
per space    64 bytes   2 ints  space, base space
                       14 floats  pose x y z qx qy qz qw, then velocity x7
```

Header is 68 bytes (4 + 12x4 + 4x4); each space adds 64.

### Differences that matter

- **Check the magic.** `WXR\x07`. It is how you distinguish a binary packet
  from a text one, which makes supporting both trivial - and worth doing, so
  you can fall back without a rebuild.
- **Buttons become a bitfield.** Bit `i` is `ControllerButton` ordinal `i`,
  19 bits. The order is the same as the `T`/`F` string, so an existing mapping
  transfers directly.
- **NaN and infinity are sanitised to 0** by the runtime. In the text format
  it stopped reading at the first one, so this is a bug fix as much as a
  format change.
- **The immersive / SBS flags are not in the layout above.** The text format
  appended them after the button string; this document has not established
  where they live in the binary packet. If you depend on them - and you do in
  the flat-window modes - **verify before switching.**
- **The runtime must advertise it.** Line 7 of `system` reads `BINARY_UDP`. A
  runtime that does not write that line does not support 0.7; check for it
  rather than assuming.

### What you gain

Neither side formats or parses numbers per frame. The runtime had already
removed `String.format` from its 0.6 text encoder - it was building a
`Formatter` and several strings for each of forty-odd numbers, every VR frame -
so the remaining win is the parsing on your side plus the smaller packet.

**Measure first.** If your port's frame time is dominated by the game's
renderer, as it usually is on a mobile GPU behind two translation layers, this
buys nothing you can feel. The reason to do it is a profile, not a changelog.

### What you lose

The readable log and the tester. Mitigate by keeping the text path behind a
flag, and by logging decoded values rather than the raw packet once binary is
on.

---

## 5. A checklist

- [ ] The port is device-verified at 0.4 or 0.5 before upgrading
- [ ] The reason for upgrading is written down, and it is a need or a profile
- [ ] `version` file updated - and nothing else assumes the old number
- [ ] Parser handles the new layout; old parser retained behind a flag
- [ ] **0.6:** spaces declared upstream, and poses confirmed arriving
- [ ] **0.6:** zero-poses treated as "not located"
- [ ] **0.7:** magic checked; text fallback retained
- [ ] **0.7:** `BINARY_UDP` present in `system` line 7
- [ ] **0.7:** immersive / SBS flags still reaching you, if you use them
- [ ] Quaternions still normalised on ingest
- [ ] Sync pixel still echoing `HMD_SYNC` exactly, green still 0
- [ ] Re-verified on device, in both handedness modes
