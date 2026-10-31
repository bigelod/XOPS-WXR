# WinlatorXR XrApi - the wire protocol

**Sections 1-7 describe version 0.4**, which is what most ports target and
what this file is named for. **Section 8 covers every version, 0.1 to 0.7**,
including the two that change the packet - and `XRAPI_UPGRADE.md` covers
moving between them.

**Provenance.** Written first from working 0.4 clients (`game/XrApiUDP.cpp`
in Shogo-VR-WXR, HWXR's `WinXrApiUDP`, `opentrackWXR`'s `tracker-wxr`) and
Quest 3 captures, then **checked against the runtime's own source**:
`app/src/main/java/com/winlator/xr/api/` on WinlatorXR's `cmod_bionic` branch -
`XrInterface`, `XrVersion01` through `XrVersion07`, `XrAPI` and `XrFramesync`.
Where the two disagreed, the runtime won and the correction is marked
**[runtime]**.

The **official specification** is at <https://winlatorxr.github.io/xrapi.html>
(0.6 current, 0.5 archived at `xrapi05.html`). Facts taken from it are marked
**[spec]**. It is the authority on field meanings; the Java source is the
authority on what the code actually does; a device capture beats both.

Sections 1-7 describe **0.4**, which is what most ports target and the version
this document is named for. **Section 8 is the version history, 0.1 to 0.7** -
read it before choosing a version, because 0.6 changes the packet layout
completely and 0.7 makes it binary.

---

## 1. The shape of it

XrApi is **not** OpenXR. There is no `xr*` call, no loader, no runtime to
link against. It is two UDP sockets on localhost carrying whitespace-separated
ASCII, plus a handshake done through files, plus one red square drawn in the
corner of the game's own output.

That is the entire integration surface. It is why a 1998 32-bit DirectX game
can drive a 2024 standalone headset without a line of modern graphics code.

```
          game process (32-bit, inside the Wine container)
                 |                        ^
   out: 127.0.0.1:7278                    |  in: 127.0.0.1:7872 and :7873
   haptics + modes + FOV                  |  poses + buttons + frame id
                 v                        |
          WinlatorXR (Android, native 64-bit)
                 |
         captures the game window, warps it, presents to the compositor
```

**[runtime] 7873 is not a fallback.** From 0.4 the runtime's `getPortsOut()`
returns `{7872, 7873}` and `send()` loops over *every* port in that list, so
the same packet goes to both. Bind either one; binding both means receiving
each packet twice.

Three channels, and all three matter:

| Channel | Direction | Carries |
|---|---|---|
| UDP 7872 **and** 7873 | runtime to game | head and hand poses, buttons, sticks, IPD, FOV, frame id |
| UDP 7278 | game to runtime | haptics, mode flags, requested FOV |
| The red sync pixel | game to runtime, *inside the video* | which frame this image is |

The third is easy to miss and impossible to do without. See section 5.

---

## 2. Handshake: the files under `Z:\tmp\xr`

Before any packet flows, the game announces itself by writing files into the
container's shared temp directory. `Z:` is the container's mapping of the
Android side; if `Z:\` does not exist you are on a desktop test box and should
fall back to a local path.

```c
strcpy(dir, "Z:\\tmp\\xr");
if (GetFileAttributesA("Z:\\") == INVALID_FILE_ATTRIBUTES)
    strcpy(dir, "D:\\xrtemp");          /* desktop fallback */
```

| File | Written by | Contents | Meaning |
|---|---|---|---|
| `version` | the game | `0.4` | which protocol the game speaks |
| `vr` | the game | `VR` | this process wants VR; delete on shutdown |
| `system` | the runtime | seven lines, see below | headset and display information |
| `mask` | the runtime | visibility mask data | the lens-hidden area per eye, for `xrGetVisibilityMaskKHR`; absent if the headset has none |

**[runtime] `version` decides everything, is read once, and an unrecognised
value means total silence.** `XrAPI.updateImplementation()` reads the *first
line*, matches it with `startsWith("0.1")` through `startsWith("0.7")`, and
constructs that version's implementation. No match - a typo, a stray BOM, an
unsupported number - leaves the implementation null, and then **no UDP
listener is ever started and no packet is ever sent**. If nothing arrives at
all, suspect this file before suspecting your socket.

It is also read *once*: the method returns early once an implementation
exists, so the version cannot change during a session.

**[runtime] The runtime wipes the directory at startup**, then writes `system`
(and `mask`). So write your `version` and `vr` files *after* the runtime is
up, or be prepared to rewrite them - and do not assume a file you wrote
earlier survived.

**[runtime] `system` has seven lines, not two:**

```
1  manufacturer        e.g. META          (Build.MANUFACTURER, upper case)
2  product             e.g. EUREKA        (Build.PRODUCT - a codename, not a model name)
3  Android release     e.g. 14
4  security patch date
5  screen size
6  per-eye size        for direct PC VR frames
7  BINARY_UDP          a capability advert: this runtime understands 0.7
```

Older clients read only the first two lines and stop, which is why this
document previously said "make and model". Line 2 is a **product codename** -
`EUREKA` is a Quest 3, `HOLLYWOOD` a Quest 2, `PANTHER`, `SEACLIFF` and
`MONTEREY` others - so a client that wants a human-readable model has to map
them. `opentrackWXR` carries such a mapping, commented out.

Here's how Halo CE VR WinlatorXR (HWXR) handles it, for example:
>> if (hmdMake.empty())      hmdMake = "META";
>> else if (hmdMake == "OCULUS") hmdMake = "META";
>>
>> if (hmdModel.empty())     hmdModel = "QUEST 3";
>> 
>> else if (hmdModel == "EUREKA" || hmdModel == "PANTHER") hmdModel = "QUEST 3";
>> else if (hmdMake == "META") {
>>	 //SEACLIFF - Quest Pro - Untested, assuming hands upside down
>> 	 //HOLLYWOOD - Quest 2 - Works! Hands upside down...
>> 	 //MONTEREY - Quest 1 - Untested, unsupported
>>   hmdModel = "QUEST 2";
>> }


Line 7 is how a runtime advertises that binary packets are available; a client
that does not know 0.7 never reads that far.

`system` is the one you *read*.

Newer clients write **`0.5`** into `version` rather than `0.4`, and an
InputAPI helper adds its own `input_version` (`0.1`), deleted on shutdown -
see `WINLATORXR_DESC.md`, "Handshake for Path B". The protocol described in
this document is what a 0.4 client sends and receives, and a runtime
advertising 0.5 has so far accepted it unchanged.

`system` is the one you *read*. It is also the cleanest "am I on a headset or
a desktop?" test available, and other ports use it as exactly that - the
Black and White port selects its WinlatorXR backend when `Z:\tmp\xr\system`
exists. Default to `META` / `QUEST 3` if it is missing rather than failing.

Delete `vr` on clean shutdown. Leaving it behind can convince the runtime that
a dead process still wants VR.

---

## 3. Inbound packet (runtime to game, port 7872)

One line of ASCII, space-separated:

```
client <28 floats> <frame id> <bool string> <HMD name>
```

A real capture from a Quest 3:

```
client0 0.210 0.290 -0.930 0.320 0.0 0.0 -0.091 -0.215 -0.284 ... 64 TFFFFFFFFFFFFFFFFFF META
```

Note `client0`, not `client` - the token is a prefix, so match it loosely.

### Float layout

Indexed from 0:

| Index | Field |
|---|---|
| 0-3 | left controller quaternion (x, y, z, w) |
| 4-5 | left thumbstick (x, y) |
| 6-8 | left controller position (x, y, z), metres |
| 9-12 | right controller quaternion |
| 13-14 | right thumbstick |
| 15-17 | right controller position |
| 18-21 | HMD quaternion |
| 22-24 | HMD position |
| 25 | IPD, metres |
| 26 | FOV x (tangent of the half-angle the runtime wants covered) |
| 27 | FOV y |
| 28 | **frame id / HMD_SYNC** - see section 5 |

Then the bool string, then the HMD name. **Ignore trailing tokens** you do not
recognise; the format gains fields at the end between versions, and a strict
parser will reject a packet it could have used.

### Bool string

A run of `T`/`F` characters, one per control:

| Index | Control |
|---|---|
| 0 | left grip |
| 1 | left menu |
| 2 | left stick click |
| 7 | left trigger |
| 8 | X |
| 9 | Y |
| 10 | A |
| 11 | B |
| 12 | right grip |
| 13 | right stick click |
| 18 | right trigger |

Indices not listed are unused or unverified - the Shogo client binds only the
above, and that list was reconciled against a device log rather than trusted
from a document. **Do the same.** Log the raw string on every press for one
session and read off which index moved. One minute, settled forever.

**[runtime] The button string is always exactly 19 characters, and the two
flags are a separate token.** `GUEST_BUTTON_COUNT` is
`ControllerButton.R_TRIGGER.ordinal() + 1` = 19, and the encoder writes exactly
that many `T`/`F` characters. From 0.3, `getFlags()` appends `" "` followed by
two more characters - immersive, then SBS - so the wire carries
`...TFFFFFFFFFFFFFFFFFF TF`.

A client that collects `T`/`F` characters while ignoring whitespace therefore
sees a 21-character run in which indices 19 and 20 are the flags, which is how
every existing client reads them and why this document previously described
them as "chars 19 and 20". Both views are workable; know which one your parser
implements.

(The `ControllerButton` enum does continue past 19 with `L_THUMBREST` and
`R_THUMBREST`, but `GUEST_BUTTON_COUNT` stops before them, so they never reach
the wire and never collide with the flags.)

Chars 19 and 20 are the runtime's `IS_IMMERSIVE` / `IS_SBS` hints. A mod that renders its own stereo and owns its
own camera does not need them - but **they are essential in the modes where
the runtime owns presentation** (`MODE_VR = 2`, and `MODE_3D = -1`), because
there they are the only way to learn what the runtime decided. See section 5a.
Accept both lengths and bounds-check every index:

```c
#define XB(i)  (((i) < n && szBools[(i)] == 'T') ? 1 : 0)
```

---

## 4. Outbound packet (game to runtime, port 7278)

Six values, space-separated:

```
<L haptic> <R haptic> <MODE_VR> <MODE_3D> <FOV x> <FOV y>
```

- **haptics** - **[spec] vibration duration in frames**, per hand. Not an
  amplitude, which is what this document said before the spec was checked:
  you are telling the runtime *how long to buzz*, not how hard. A small value
  is a click; a larger one is a rumble. It also explains the parser quirk
  below - zero means "nothing to add" rather than "stop".

  For "buzz while the gun is firing", keep topping the duration up each frame
  while that is true. HWXR and XeRxKEYs both decay a local value and keep
  sending the decreasing number, which fits this model.
**[spec] MODE_VR** - four values, not two:

| Value | Meaning |
|---|---|
| `0` | disabled - **the runtime sends no UDP data at all** |
| `1` | VR |
| `2` | screen mode, **no automatic head tracking** |
| `3` | screen mode **with head tracking mapped to the mouse**, in 3DoF immersive mode |

`0` is worth knowing about: it is an off switch for the whole inbound stream.
An *accidental* 0 therefore goes silent and looks broken - but a deliberate one
is a legitimate and useful mode, which this document previously understated.

**[device - Interstellar Marines port, Quest 3]** Sending `MODE_VR = 0` while
the player is in a menu is a good default, and does three things at once:

- **The pose stream stops**, which is exactly right when nothing is consuming
  it. There is no reason to pay for 6DOF data while a menu is up.
- **The runtime's own mouse control of the screen comes back**, so flat menus
  become usable with no camera work and no input mapping on the mod's side.
- **The display leaves immersive presentation** and the frame is shown as a
  flat panel, so the player can take in the whole screen at once rather than
  reading a menu through a VR projection designed for a world.

That makes `0` the natural choice for menus, pause and any pre-game state, and
`2` / `3` the choice when you want a flat window but still want the pose data
(`2`) or runtime-driven mouse-look (`3`) - `opentrackWXR` wants `2` because it
forwards raw HMD data to the game's own head-tracking support.

Two practical cautions if you switch to `0` and back:

- **Keep sending the outbound packet every frame**, or you cannot turn the
  stream back on. The mode is only ever re-read from a packet you send.
- **Expect a gap** when you return to `1`: the stream has to restart, so hold
  the last good pose across it rather than treating the silence as a dropout
  to zero.

`2` and `3` are both "flat window in the headset". The difference is who moves
the mouse: in `2` the runtime leaves its normal screen-mode input alone (which
is why `opentrackWXR` uses it - it wants raw HMD data to forward to the game's
own head-tracking support, not a runtime-driven mouse), and in `3` the runtime
maps head motion onto the mouse for you.

**[spec] MODE_3D** - four values:

| Value | Meaning |
|---|---|
| `0` | no 3D (mono) |
| `1` | SBS, side-by-side |
| `2` | AER, alternate eye rendering |
| `-1` | **user controlled** - the player chooses in WinlatorXR's XR settings |

Section 5a covers AER and the flat-window modes in detail.

Mono is a legitimate shipping mode, not a fallback: the Tremulous port ships
`XR_SendOut(0)` and lets the runtime show one image to both eyes.
- **FOV x / y** - the FOV the game is actually rendering with, so the runtime
  can warp correctly.

**[spec] These are degrees, and they are applied only if greater than 1.**
That resolves a discrepancy this document previously flagged as unresolved:
`opentrackWXR` and HWXR send `104.5` because the field is an angle in degrees.
A client that sends tangents of half-angles instead - all of which are less
than 1 for any sane FOV - is sending values the runtime **ignores**, and it
will appear to work only because the runtime keeps its default.

**Start both at `104.5`.** It is the default in both `opentrackWXR` and HWXR
(`fovVarE` / `fovVarF`), and what most of their development was done against:

```
0 0 1 0 104.5 104.5
```

`104.5` / `104.5` is the default in both `opentrackWXR` and HWXR
(`fovVarE` / `fovVarF`), and it is what most of their development was done
against.

**Treat these two fields as tuning values, not as a unit conversion to get
right.** In practice they are trial and error: send a pair, look through the
headset, judge whether the world's scale and the edges look right, adjust.
The inbound FOV fields (floats 26-27) are tangents of half-angles, but the
outbound pair is not reliably the same convention - Shogo computes and sends
tangents, the other two send 104.5 - and chasing that discrepancy is less
useful than tuning by eye.

Two things follow:

- **Make them loadable, not compiled in.** HWXR reads its FOV values from a
  flag file, with `104.5` as the fallback when the file is short - so they can
  be tuned on the device without a rebuild. Copy that: it turns a build cycle
  into a text edit, and the values are per-game.
- **Ask for help rather than deriving it.** The right pair for a given game is
  known to the people who have tuned other games on the same hardware. This is
  a good question for the WinlatorXR community, and a poor use of a day of
  solo experimentation.

Open a socket, `sendto`, close. Do not hold a send socket open across frames
unless you have measured that you need to.

**[runtime] A value of 0 does not always mean 0.** The runtime's parser is:

```java
if ((value > 0) || (i >= 2)) input[i] = value;
```

Fields from index 2 onward - `MODE_VR`, `MODE_3D`, `HMD_FOVX`, `HMD_FOVY` -
accept any value, including zero and negatives. **That is what makes
`MODE_3D = -1` possible**, and it has been true since 0.2.

The first two fields are the haptics, and for those **only values greater than
zero are stored**. Sending `0 0 1 0 104.5 104.5` therefore leaves the previous
haptic levels in place rather than clearing them. Both HWXR and XeRxKEYs decay
their haptic values locally and keep sending the decreasing number, which
works with this behaviour; a mod that expects an explicit zero to stop a
rumble will be surprised. In 0.1 the rule applies to *every* field, so no
field can be set to zero at all.

---

## 5. The red sync pixel - the most important section here

**WinlatorXR reads a shade of red from the top-left corner of the captured
frame to learn which frame it is looking at.** `HMD_SYNC` arrives as float 28
of the inbound packet; the game draws it back as red, 0-255, in the top-left
corner of what it renders.

```c
int r = (int)(sync + 0.5f);              /* clamp 0..255 */
FillRect(screen, {0, 0, 10, 10}, RGB(r, 0, 0));
```

The square carries **two** fields, not one:

| Channel | Carries |
|---|---|
| **Red** | the frame id, in every mode |
| Green | unused - keep it 0 |
| **Blue** | the AER eye flag: 0 = left eye, 255 = right eye. 0 in every other mode |

So the general form is `RGB(frameId, 0, eyeFlag)`. A mod that never uses AER
sets blue to 0 and forgets about it; see section 5a before using it.

### Why this is not optional

The runtime captures the game window asynchronously from the game's own frame
loop. Without the sync value it cannot tell a fresh frame from one it has
already shown, so it cannot match the image to the pose that image was
rendered for. The result is reprojection against the wrong head pose: the
world swims when you turn your head, by a varying amount.

That is **exactly** the stimulus that causes motion sickness, and no amount of
frame-rate work fixes it, because the defect is correlation and not latency.
Players describe it as "laggy" or "floaty" and stop playing within minutes.

Get the sync pixel right **before** tuning anything else. A port at 30 fps
with correct sync feels better than one at 72 fps without it.

### Practical details, each learned the hard way

- **Draw a 10x10 square, not one pixel.** **[runtime]** The reader samples
  exactly one pixel, at (0,0), as BGRA - so geometrically one pixel would do.
  Draw ten anyway, as HWXR does: it costs nothing and it survives a border, a
  scaling pass or an odd window size shifting the corner by a pixel.
- **In side-by-side, draw it twice** - top-left of the full frame *and*
  top-left of the right-eye half - so whichever half the runtime samples
  carries the id.
- **It must be the last thing drawn**, after the world and after the HUD, or
  something else overwrites it.
- **The window must have no title bar and must fill the display.** With chrome
  in the corner, the runtime samples the chrome. This is the single most
  common cause of "I implemented sync and nothing changed". See `TIPS.md`.
- **Draw it with filtering and blending off.** The runtime samples exact byte
  values, so anything that alters them corrupts the id. HWXR sets point
  sampling on all three filters and disables alpha blending, fog and Z before
  drawing the square.
- **[runtime] Do not gamma-correct it yourself - the runtime learns the
  mapping.** This is the detail most worth knowing. `XrFramesync` does not
  assume the red value survives unchanged. It collects the *distinct* red
  values it observes until it has 22 of them (`step = 12`, `limit = 256`, so
  `256/12 + 1`), sorts them, persists them to shared preferences, and from
  then on maps an observed value to `indexOf(value) * step`. In other words
  it calibrates itself to whatever colour space your renderer actually
  produces. If an unexpected value shows up after calibration it clears the
  table and relearns.

  Two consequences. **Echo `HMD_SYNC` exactly as received** - do not rescale,
  pre-correct or invent your own ids; the runtime's values step by 12 for this
  reason. And **keep green at 0**, because the remapping is gated on `g == 0`.
  HWXR's commented-out `pow(frameId/255, 1/2.2)` and disabled
  `D3DRS_SRGBWRITEENABLE` are that lesson already learned: the correction
  belongs on the runtime side, and it is already there.

- **[runtime] The runtime measures your frame rate from this pixel.** It
  counts changes in the red value to derive FPS. A sync value that never
  changes reads as a frozen game.
- **Optional: block for a fresh sync value** before rendering (Shogo calls
  this `VRSyncHold`). This pins the game's frame clock to the runtime's and
  removes tearing between the two. It is a deliberate stall, so leave it off
  by default and expose it as a setting. HWXR and SixDOFinator both do the
  same thing with a spin on `lastFrameID == currFrameID`.

### A free debug channel

The corner is a place the runtime reads, but the rest of the frame is yours.
HWXR draws a **green** 10x10 square just below the sync square while SBS is
active, purely so a glance at a screenshot says which mode is running. On a
standalone device, with no debugger and a slow deploy loop, an on-screen mode
indicator costs four vertices and answers a question that otherwise needs a
log file pulled off the headset.

---

## 5a. The four presentation modes

`MODE_VR` and `MODE_3D` together select one of four, and the choice is a
design decision rather than a quality setting. Taken from HWXR, which
implements all four.

| Mode | `MODE_VR` | `MODE_3D` | Video | Depth | Per-eye frame rate | Scene cost per frame |
|---|---|---|---|---|---|---|
| **Mono VR** | 1 | 0 | mono | none | full | 1x |
| **SBS stereo** | 1 | 1 | side-by-side | yes | full | 2x |
| **AER** | 1 | 2 | mono, alternating | yes | **half** | 1x |
| **Non-VR** | 2 | 0 | mono | none | full | 1x |
| **Non-VR, runtime mouse kept** | 2 | **-1** | mono | none | full | 1x |

Five, then, counting the last as its own mode - which it is, because it
changes who owns the mouse.

### AER - alternate eye rendering

**The idea.** Render *one* eye per game frame, alternating, and tell the
runtime which eye each frame was for. The runtime holds the other eye's last
image. The result is true stereo depth while still rendering one view per
frame.

**The trade.** Each eye receives only half the fresh frames - at 72 fps in the
game, each eye updates at 36. Motion in depth can show a slight shear between
the eyes and fast head motion is less forgiving. The head *pose* is still
fresh every frame, though, which is the part that matters most for comfort.

Against SBS, which renders twice per frame: AER keeps full frame rate at mono
cost and spends per-eye freshness instead. On a mobile GPU behind two
translation layers that is often the better bargain, which is the whole reason
the mode exists.

**How to implement it**, from the two reference implementations:

1. Offset a single camera by `+/- IPD/2` along its local right vector,
   flipping the sign every frame. SixDOFinator does exactly this, moving one
   eye transform by `-IPD * 0.5` for left and `+IPD * 0.5` for right. HWXR
   instead keeps per-eye render surfaces and blits the chosen one into the
   presented frame.
2. Flip the eye flag once per presented frame, in your post-frame hook.
3. Write the flag into the **blue** channel of the sync square:

   | Eye | Camera offset | Blue |
   |---|---|---|
   | left | `-IPD/2` | 0 |
   | right | `+IPD/2` | 255 |

   **[runtime]** The reader is `aerTargetFBO = b > 0 ? 1 : 0`, so strictly
   *any* non-zero blue selects the right eye. Send 255; it is what the spec
   states and what both implementations do. **[runtime]** It also only acts on
   a frame whose red value changed (`aerShouldUpdate`), which is another
   reason to echo `HMD_SYNC` faithfully.

   HWXR: `D3DCOLOR_ARGB(255, frameId, 0, renderEyeVal)` with `renderEyeVal`
   255 or 0. SixDOFinator: `Color32(frameId, 0, 0, 1)` for left and
   `Color32(frameId, 0, 255, 1)` for right. The two agree, so treat
   **blue 255 = right eye** as settled.
4. Send `MODE_3D = 2`.
5. **Use the IPD from the packet** (float 25), not a constant.

**Make the modes mutually exclusive, and make SBS win.** HWXR tests SBS
*before* AER everywhere, and the reason is instructive: a runtime toggle could
set both flags, and AER would then render on top of SBS. Ordering the tests
made a stray toggle a visual no-op instead of a mode conflict. Three separate
sites depend on that ordering there - so if you support more than one mode,
centralise the selection in one function rather than repeating the precedence
at each site.

In SBS and non-VR modes the eye flag must be 0.

### Non-VR mode - 6DOF without VR

`MODE_VR = 2`, mono video. The game renders flat and the runtime shows it on a
**floating window** in the headset - but tracking keeps flowing, so the mod
can still drive aiming, gestures and motion controls from the controllers.

Think of it as the Wii treatment: a flat screen you sit in front of, played
with motion controls. For a retro game this is a genuinely attractive target
rather than a consolation prize:

- **No camera work at all.** The hardest and riskiest part of a VR port, a
  head-driven in-game camera, simply does not exist - and that is where
  comfort problems come from. If a game's camera turns out to be hostile or
  unreachable, this route stays open when a real VR port does not.
- **Cheapest mode to run.** Mono, one render, no stereo.
- **Comfortable by construction.** A flat window cannot induce vection.
- **It suits genres a VR camera actively ruins** - strategy, platformers,
  racing, anything with a fixed or scripted camera.

Note the deliberate choice of **mono video rather than no video**: the mode
stays on the normal presentation path specifically so 6DOF data keeps flowing.
HWXR's own notes call this out.

HWXR also reads bool index **19** (`IS_IMMERSIVE`) only in this mode, forcing
it true otherwise - one of the two extra bools the longer string carries.

Worth considering non-VR mode as a **shipping fallback for every port**,
player-selectable: a comfort option for people who cannot tolerate VR
movement, and a performance option for weaker hardware.

### `MODE_3D = -1` - keep WinlatorXR's own mouse emulation

The one that is easy to miss, because it is a *negative* value in a field
whose other values are a mode enum.

By default, WinlatorXR maps the **right controller's gyro to the mouse** - the
behaviour that makes a flat Windows game playable in a headset at all. A mod
that takes over tracking normally wants that suppressed, so it can own aiming.

Sometimes you want **both**: the runtime's mouse emulation left exactly as it
is, *and* HMD 6DOF delivered to the application. That is what `-1` asks for.

`opentrackWXR` sends precisely:

```
0 0 2 -1 104.5 104.5
```

`MODE_VR = 2` (non-VR), `MODE_3D = -1`, then its target FOV.

**Why this combination is valuable.** It is the right answer for **racing and
flight simulators**, and for anything else where:

- the head should look around freely - that is the whole point of head
  tracking in a cockpit - while
- aiming, steering or pointing stays on the mouse the player already has, with
  the runtime's own gyro mapping, which is well tuned and needs no code.

The alternative - taking over the mouse yourself and re-deriving its
behaviour from controller poses - is work with no benefit in a game whose
camera and controls were never built around a head.

**It also hands window placement to the runtime.** Along with keeping the
mouse, `-1` lets WinlatorXR decide how to present the flat window: either
**floating in space** at a fixed point the player can look around within, or
**attached to the HMD** - its *immersive* mode - so the window follows the
head like a visor.

That is a sensible thing to delegate. Which one feels right depends on the
game and the player, the runtime already has UI for it, and a mod has no
business overriding a preference it cannot evaluate.

But it means **the mod no longer knows its own presentation**, and that
changes the maths. This is what bool index **19** (`IS_IMMERSIVE`) is for, and
`opentrackWXR` shows exactly why it matters: it keeps **two separate sets of
tracking scales** and picks between them on that bool.

```
window floating in space   yaw/pitch/roll scale  1.0   (1:1)
window attached to the HMD  yaw/pitch/roll scale  0.8   (damped)
```

The reason is worth understanding, because it generalises. When the window
floats, turning your head 30 degrees moves your eyes 30 degrees relative to a
fixed screen, and a 1:1 mapping is correct. When the window is attached to
your head, it turns *with* you - so the same head turn gives the game a full
30 degrees while your eyes have not moved relative to the image at all, and
1:1 overshoots. Damping to about 0.8 compensates.

So in these modes:

- read bool 19 every frame and branch on it;
- keep two scale sets, or at least one scale plus a multiplier;
- expose them as settings, because the comfortable value is per-player.

`opentrackWXR` also reads bool **20** (`IS_SBS`) and currently does nothing
with it, so the slot is there if presentation ever needs more than a boolean.

> The *semantics* in this subsection come from the owner of these projects,
> and the two scale sets corroborate them. The wire values are read from
> `opentrackWXR`'s source; the behaviour they select lives in the WinlatorXR
> APK, which this document has not read. Verify on device.

### Mode selection by flag file

HWXR selects these with files next to the game rather than through a menu:

| File | Effect |
|---|---|
| `VR/nonvr.txt` | enables non-VR mode; its *contents* are a float view-angle scale |
| `VR/altroomscale.txt` | alternative roomscale mode; presence alone is enough |
| a thumbstick-flip flag file | swaps the sticks |

Cheap, needs no UI, and a file is easier to create on a headset than a setting
is to find in a menu. Worth copying for *developer* options specifically -
keep player options in a menu, per `TIPS.md`.

Note also that HWXR writes `version` as `0.3` in non-VR mode and `0.2`
otherwise, with `0.4` present but commented out pending fallback-port support.
So a runtime that works with this document's 0.4 client also accepts 0.2 and
0.3 clients: these mode fields have been sent **since XrApi 0.2**, and using
them does not require advertising a newer version.

---

## 6. Coordinate conversion

XrApi delivers poses in the OpenXR convention: **right-handed, +Y up, -Z
forward, metres**. Almost no 1990s or 2000s engine uses that.

### Recentre

Keep the inverse of the pose captured at recentre time and apply it to
everything:

```c
QMul(centerInv, qIn, q);                        /* orientation */
d = pos - centerPos;  QRotate(centerInv, d, p)  /* position */
```

### Handedness

Derive basis vectors by rotating the canonical axes, *then* flip for a
left-handed engine:

```c
QRotate(q, {0, 0, -1}, fw);     /* OpenXR forward */
QRotate(q, {0, 1,  0}, up);
f = { fw[0], fw[1], -fw[2] };   /* left-handed: flip z */
u = { up[0], up[1], -up[2] };
p = { pp[0], pp[1], -pp[2] };
```

Do not try to flip the quaternion itself. Rotate the axes, flip the results.
Mirroring a quaternion by hand reliably produces a basis that is subtly not
orthonormal, and then you spend a day on the symptoms.

### Units

Metres in, engine units out. Find the engine's scale early and put it in one
constant. A magic `* 64.0f` scattered through the code cannot be changed later.

---

## 7. Pitfalls that cost real days

**Non-unit quaternions.** The standard rotation shortcut
`v + 2w(u x v) + 2u x (u x v)` is a *rotation* only when `|q| = 1`. For a
non-unit quaternion it also scales by `|q|^2`. Real headset packets are unit
to about 1e-4. **Simulated packets from a tester often are not** - captures
used during Shogo's development were 1.4% to 10.8% long, which stretched each
hand's basis by a *different* amount and produced an asymmetry that looked
like a deep bug in the pose maths. Normalise on ingest, unconditionally:

```c
static void QNormalise(DFLOAT q[4])
{
    DFLOAT n2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (n2 < 1e-12f) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; return; }
    DFLOAT inv = 1.0f / (DFLOAT)sqrt(n2);
    q[0] *= inv; q[1] *= inv; q[2] *= inv; q[3] *= inv;
}
```

**Tester data is not device data.** Beyond the quaternion issue, hand
separation in a desktop tester capture was 0.75 m against 0.26 m on a real
headset. Anything tuned against synthetic poses must be re-checked on
hardware - and prefer tuning values whose frame of reference does not depend
on the pose at all (`TIPS.md`, "measure in the frame you will use").

**Drain the socket; do not read one packet per frame.** Read every queued
datagram and keep the newest. One-per-frame builds a queue and you act on
steadily staler poses.

**Keep the last good pose.** Packets drop. Freeze on the previous value rather
than snapping to zero - one zeroed frame is a violent camera jolt.

**Log the raw packet periodically** behind a verbose flag (Shogo: every 5 s),
one line exactly as received, so a device capture can be diffed against a
tester capture by eye and pasted straight back into the tester. This one
feature converted several unanswerable questions into five-minute answers.

---

## 8. The version history, 0.1 to 0.7

**[runtime]** Read from `XrVersion01` to `XrVersion07`. Each class extends the
one before, so the list is cumulative: 0.4 *is* 0.1's packet plus three
changes. The version your `version` file names is the behaviour you get, and
old versions are kept deliberately - the project's stated aim is long-term
compatibility, so a 0.4 client keeps working.

| Version | What it adds |
|---|---|
| **0.1** | the baseline: 28 floats + sync + 19 button chars on 7872, app state in on 7278. `MODE_VR` / `MODE_3D` are read from **flag files** (`vr`, `sbs`) in the handshake directory, not from UDP. No field can be set to 0. |
| **0.2** | app state moves to UDP, and **fields from index 2 on accept 0 and negatives** - this is where `MODE_3D = -1` becomes possible |
| **0.3** | appends the **immersive and SBS flags** (`" TF"`) after the button string |
| **0.4** | adds **7873** alongside 7872; the runtime sends to both |
| **0.5** | adds the **InputAPI** listener on **7728** - the `M,` and `K,` messages |
| **0.6** | **a different packet.** See below. |
| **0.7** | 0.6's packet as **binary** |

Nothing before 0.6 changes the tracking packet's shape, which is why one
parser handles 0.1 through 0.5 and why this document's sections 1-7 serve all
of them.

### 0.6 - the packet changes completely

Not an extension. The old packet's per-controller quaternions and positions
are **gone**, replaced by an OpenXR-shaped space model.

Inbound (runtime to app) becomes:

```
client<N> L_THUMBSTICK_X L_THUMBSTICK_Y R_THUMBSTICK_X R_THUMBSTICK_Y
          L_TRIGGER L_SQUEEZE R_TRIGGER R_SQUEEZE
          HMD_IPD HMD_FOVX HMD_FOVY HMD_FPS
          HMD_SYNC HMD_RECENTER
          <19 button chars>
          <locate space count> [ <space> <base space> <7-float pose> <7-float velocity> ] ...
```

Three things to notice:

- **Analog triggers and squeezes arrive for the first time** - 0.4 only had
  them as booleans in the button string.
- **`HMD_FPS`** (the display refresh rate) and **`HMD_RECENTER`** are new
  fields.
- **Poses now come from a space list the app asked for.** Upstream, the app
  declares reference spaces, controllers (`1` = left, `2` = right; grip `0` =
  aim, `1` = grip) and locate-space requests; the runtime returns each
  requested pair's pose *and velocity*. Poses are sent with **five** decimals
  because three rounded a quaternion to roughly a twentieth of a degree.

### 0.7 - the same thing in binary

Identical fields in identical order, little-endian, magic `WXR\x07`: 12
floats, then sync, recenter, a 19-bit **button bitfield** and the space count
as ints, then 2 ints + 14 floats per space. NaN and infinity are sanitised to
0 - in the text format the runtime stopped reading at the first one. What the
app sends *back* is unchanged.

The runtime advertises support by writing `BINARY_UDP` as line 7 of `system`.

### Is this "true OpenXR"? Yes - through OXRWXR, which is built on 0.6

The spec's caveat is narrower than it first reads. It says *XrApi itself* is
**"not yet designed to function as a complete replacement for the OpenXR
runtime"**, and that is true: XrApi is a transport, and a mod that speaks it
directly is implementing its own VR support.

But a complete runtime exists on top of it.
**[OXRWXR](https://github.com/WinlatorXR/OpenXR-WXR)** - "OpenXR Runtime for
WinlatorXR" - is a Windows OpenXR runtime that **"uses XrAPI v0.6 as its
interface layer"**, derived from OpenXR-Simulator and rebuilt around
standalone hardware. Its `src/WinXrApiUDP.cpp` is the XrApi client inside the
runtime.

That is why 0.6 looks the way it does. The space model, the velocities, the
analog triggers, the refresh rate and the per-eye size on line 6 of `system`
are not generic improvements - they are **what an OpenXR runtime needs**.
0.6 was shaped by its first serious consumer.

And one layer further out,
**[OpenComposite for WinlatorXR](https://github.com/WinlatorXR/OpenComposite)**
implements **OpenVR** on top of that OpenXR runtime - a fork of upstream
OpenComposite kept deliberately close to it - so SteamVR-era titles reach the
same stack.

```
native OpenXR game ------------------------------>  OXRWXR  -> XrApi 0.6/0.7 -> WinlatorXR
OpenVR / SteamVR game -> OpenComposite ---------->  OXRWXR  -> XrApi 0.6/0.7 -> WinlatorXR
your own mod -----------------------------------------------> XrApi 0.4/0.5 -> WinlatorXR
```

**But it is not a free trade.** Every layer in those first two rows costs
performance, inside Box64 and Wine on a mobile GPU, running content built for
a desktop one. That stack exists to allow **general PCVR play** - an existing
VR library on a standalone headset with no per-game work - which is a
different goal from making one game run well.

**Modding the game directly, against 0.4, remains the normal
recommendation.** A mod targets one game's own renderer and pays nothing for
generality. See `WINLATORXR_DESC.md`, Path E, for when the other stack is the
right answer.

### Which version should a new port target?

Migration instructions for moving between versions are in
**`XRAPI_UPGRADE.md`**.

- **0.4** for a typical mod. It is what the Shogo, Halo CE and opentrack ports
  use, every existing client parses it, this document's sections 1-7 describe
  it, and poses arrive ready to use without declaring spaces.
- **0.5** if you also want to inject keyboard or mouse events - it is 0.4 plus
  the InputAPI listener, so there is no cost to asking for it.
- **0.6** if you need analog triggers, the refresh rate, pose velocities, or
  arbitrary spaces - and particularly if you are implementing a runtime shim
  rather than a mod.
- **0.7** only when packet formatting cost shows up in a profile. It is 0.6's
  data with the text handling removed.

**Pick deliberately and write the choice down.** The version is a single line
in a file, it is read once at startup, and an unrecognised value produces
silence rather than an error.
