# NEXT_STEPS - XOPS-WXR

**As of 3 Nov 2026.** Written at the end of the session that got head tracking
driving the game from UDP on the desktop.

Read `CURRENT_STATUS.md` first for state and evidence. This file is only "what
to do next, and why, in order". Same rules apply: every claim carries its
evidence tag, and **nothing is complete until the owner has confirmed it on
hardware**.

> **5 Nov 2026 update - item 1 below is solved for the states that matter.**
> The game's own globals replace the draw-count heuristic: `0x490A44` mission
> index (`64` = title), `0x490A48` result (`0` playing, `1` victory, `2`
> death/failure), `0x48DDD8` player index (`-1` = menus/briefing). `MODE_VR`
> is 1 only in gameplay and 0 on title/objective/menu/death/victory; two
> deaths and one victory were verified on the desktop. This also fixes
> DUEL_EXT, whose 39 indexed draws fell below the old 60 threshold. See
> `Docs/OpenCode/DeepSeek41Flash/README.md` and `WORKLOG.md`.

---

## Where we actually are

Working on the desktop, confirmed by the owner on screen [device, runs 8-13]:

- **All five camera fields found and writable**, in cluster A at `0x0048C3B0`
  (Y), `0x0048C3B4` / `0x0048C3B8` (the two horizontal axes), `0x0048C3BC`
  (yaw), `0x0048C3C4` (pitch). Writes land in `BeginScene`.
- **Cluster B at `0x0048E0A8`** is the player's own orientation; A is a render
  copy the game makes during its update. **Write both** - B is what makes
  walking follow the head.
- **The XrApi 0.5 client works**: handshake, inbound parse, outbound packet,
  thousands of packets a session with no loss.
- **Head yaw and pitch steer the view.** Horizontal 6DOF translation works.
- **The left stick walks the player**, head-relative for free, because the game
  moves along the yaw field we are writing.
- **The game no longer pauses when unfocused**, so the tester can hold focus.

Not working, or not built:

- **Height / crouch** - the game pulls the player back to the ground every
  frame [device, run 13]. (gravity)
- **Roll** - decided on a plan (rotate the frame) but not implemented.
- **The red sync pixel** - not implemented. Blocks any headset use due to comfort.
- **Stereo** - nothing. Mono first, per the settled decisions.
- **Decoupled aim** (F-7) - nothing. The hardest problem left.
- **Right controller turning** - nothing.

---

## The order to do things in, and why that order

### 1. Find the game's menu/playing flag  [blocks VR-exclusive input]

**Why first:** it is small, it is blocking, and the current stand-in is a
heuristic I got wrong once already. The owner's design is that during gameplay
the game sees no desktop input except ESC - and if the detector mistakes a menu
for gameplay, the player cannot use the menu they just opened. Run 13 caught
exactly that.

The present test is `SetTransform == 0 && DrawIndexedPrimitive >= 60`
[unverified, heuristic]. It works on the three screens seen so far but it is a
statement about draw counts, not about the game.

**The test:** the probe already does differential scanning over the EXE's own
data. Start in gameplay, press ESC, come back, repeat. A flag that changes on
every transition and at no other time is an `int` or a `BYTE`, not a float, so
**the scan has to be widened to integers** - everything so far has only looked
at floats. That is the one piece of new code needed.

### 2. Remove the window chrome  [prerequisite for the sync pixel]

XOPS runs in a `WS_CAPTION | WS_BORDER` window, client 640x480 inside 646x509
[device, every run]. `Docs/general_wxr/TIPS.md` is explicit that the window
must have **no title bar and must fill the display**, because the runtime
samples pixel (0,0) and with chrome there it samples the chrome.

This is listed before the sync pixel deliberately: implementing sync first and
then wondering why nothing changed is the single most common failure mode the
general docs describe.

`CreateWindowExA` and `ShowWindow` are both imported [device], so the style can
be changed at creation or afterwards with `SetWindowLongA` +
`SetWindowPos`.

### 3. The red sync pixel  [blocks the headset entirely]

Non-optional, and the general docs are emphatic about why: without it the
runtime cannot match an image to the pose it was rendered for, and the result
is reprojection against the wrong head pose - the exact stimulus that causes
motion sickness, which no frame-rate work fixes.

**The route is already known and confirmed:** the back buffer is not lockable
[device], so `Clear` with a rect in `EndScene` is the only way in - and
`EndScene` is called exactly once per presented frame [device]. Draw a 10x10
square at the top left, **last**, with filtering and blending off.

- red = `HMD_SYNC` (float 28 of the inbound packet) **echoed exactly as
  received** - do not rescale or gamma-correct it; the runtime calibrates
  itself to whatever values it sees
- green = 0, always (the runtime's remapping is gated on `g == 0`)
- blue = AER eye flag, 0 until AER is used

`XrPose::sync` already carries the value and is logged every two seconds.

### 4. Send XOPS's real FOV, not the default  [small, and currently wrong]

We send `104.5 104.5`, which is the documented starting default. XOPS actually
projects at **fovY 65.0 deg, fovX 80.7 deg, aspect 4:3** [device, from the
projection matrix, every run]. The field means "the FOV the game is actually
rendering with, so the runtime can warp correctly", so sending 104.5 asks the
runtime to warp for a lens we are not rendering.

Cheap to fix in `XrSendFrame`'s caller. Worth doing before judging how the
image looks in the headset, or that judgement is against the wrong warp.

### 5. Decide what to do about eye height

Three options, none tried:

- find a separate eye-height or crouch field (another probe hunt)
- write Y **later in the frame** than the clamp - the write-phase switch
  already in the probe (`2`) can test `ENDSCENE` in one run
- accept a fixed eye height and let the game own it

Option two is nearly free to test and should be tried first.

### 6. Roll, by rotating the rendered frame

Settled as a decision (owner, 3 Nov 2026): XOPS has no roll field and does not
need one, because a camera roll about the view axis **is** a 2D rotation of the
rendered frame.

The corner-cropping worry is void - with `MODE_VR` true the eye never sees the
panel corners (owner). The real work is the readback: the back buffer is not
lockable, so getting the frame as a texture to draw rotated needs `CopyRects`
to an off-screen surface rather than a lock.

### 7. F-7: decoupled aim  [the hardest thing left]

Unchanged from `PROJECT_PLAN.md`, and now the project's main risk. The head
drives view and movement direction; **aim comes from the dominant hand** and
must be completely separate. Writing yaw for the view must not drag the weapon
with it.

Run `PROJECT_PLAN.md` F-7 test 1 (IDA Free, xrefs on the camera yaw global)
before writing aim code. We now know the exact address to xref -
`0x0048C3BC` for the render copy and `0x0048E0B4` for the player's own - which
is a much better starting point than F-7 was written with.

### 8. Separate the mod from the probe

`src/d3d8probe.cpp` is now ~2600 lines and holds three things: the diagnostic
instrumentation, the camera hunt, and the actual mod. The mod parts are the
keepers - `src/xrapi.cpp` is already clean and separate, and the steering,
input and watch-table code should follow it out.

Not urgent, but do it before the file gets harder to work in than the problem
it solves.

### 9. Stage 1 on the device

Unmodified XOPS in a WinlatorXR container, then the mod. Nothing in this file
has run on a Quest 3 yet.

---

## Probe controls, as built

Everything is swallowed before the game sees it. `W/A/S/D` and the mouse are
left alone except where noted.

| Key | Does |
|---|---|
| `3` | steering on/off (also recentres) |
| `;` | recentre - head pose and the game's own yaw/position |
| `[` / `]` | flip yaw / pitch sign |
| `'` | cycle DOF: yaw+pitch -> +height -> full 6DOF |
| `` ` `` | cycle cluster written: A / B / both |
| `1` | left stick -> W/A/S/D |
| `.` | VR-exclusive input (desktop keys dead in gameplay, ESC excepted) |
| `\` | run while unfocused |
| `2` | write phase: BeginScene / Input / EndScene |
| `9` | probe mode on/off |
| numpad `1`-`9` | pick that watch variable directly (NumLock on) |
| `8` / `7` | next / previous variable |
| `0` | tag the current variable |
| `U` `K` `L` `O` `P` | classify what a sine wave did: tilt / lift / turn / front-back / left-right |
| `-` / `=` | halve / double the swing |
| `5` | dump the watch list and both raw structs |
| `4` | rescan for new candidates |
| `6` | arm the ballistic window (height cheat + gravity) |

Right stick click also recentres.

Settled conventions persist to `xops_wxr_steer.ini` beside the DLL: `yawSign`,
`pitchSign`, `cluster`. The DOF mode deliberately does not - it is a choice
about today, not a fact about XOPS.

---

## Mistakes worth not repeating

Recorded because each cost a run, and the pattern in two of them is the same -
reasoning where asking or measuring was available.

- **I excluded the EXE from the memory scan for three runs.** `MEM_PRIVATE`
  leaves out `MEM_IMAGE`, and a 2003 game keeps the player position in a
  static global, which lives in the EXE's `.data`. Nothing was findable until
  that was fixed. Exclude *yourself*, not all images.
- **I got the pitch sign wrong twice by reading the owner's toggle history
  instead of his report.** He was comparing, not settling. The person looking
  at the screen is the evidence.
- **I called the gameplay detector "free" on one signal.** It separated
  gameplay from the title but not from the in-mission menu. Check a claimed
  invariant against every state, not the states that happen to be in the log.
- **I wrote positional DOF as an absolute value**, which would have cancelled
  the game's own movement the moment it was switched on. Deltas, not
  absolutes, when the game owns the same variable.
- **Three build failures from the same cause**: a patch anchored on a function
  name matched its *forward declaration* near the top of the file, and
  inserted the definition where nothing it used was declared yet. Anchor on
  the definition's opening brace.
- **`near` is still a macro in `windef.h`.** A variable called `near` silently
  vanishes and the line stops parsing.

---

## Files

| Path | What |
|---|---|
| `src/d3d8probe.cpp` | hooks, camera hunt, steering, input - the whole probe |
| `src/xrapi.cpp` / `.h` | the WinlatorXR XrApi 0.5 client, standalone |
| `src/d3d8probe.def` | exports `Direct3DCreate8` plus four forwards |
| `build/build-probe.bat` | x86-only build, both units, deploys with a hash check |
| `out/probe/` | build output, ignored |

Build from an **x86** Native Tools prompt, or via
`vcvarsall.bat x86 && build\build-probe.bat`. The x86 guard is there because a
64-bit DLL will not load into XOPS and the failure is silent.
