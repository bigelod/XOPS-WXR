# First steps: from "this game might work" to 6DOF in a headset

A sequenced plan with explicit **human checkpoints**. Each checkpoint is a
place where an agent cannot proceed on reasoning alone - it needs a fact only
the owner, the device or the hardware can supply. Stop there and ask; do not
guess past one.

The order is deliberate. Each stage leaves something *testable*, so a failure
is attributable to the stage that caused it. Resist the urge to build the
whole port and then debug it.

---

## Stage 0 - Decide whether to do it at all

Answer these before writing code. Most of it is reading, not building.

1. **Bitness and renderer.** 32-bit? DirectDraw, D3D7, D3D8, D3D9, OpenGL?
   (`dumpbin /headers`, or just look at which `d3d*.dll` ships with it.)
2. **Is there source?** A released SDK, a GPL rebuild, a leaked tree? What
   does its licence permit? This shapes everything downstream.
3. **Is there a mod API?** A game that loads a DLL of yours is the easiest
   possible target.
4. **Does a VR version already exist - and can it simply run?** Official VR
   support, a SteamVR mode, or a mature PCVR mod may need **no new code at
   all**: OXRWXR is a Windows OpenXR runtime on WinlatorXR, and OpenComposite
   puts OpenVR on top of it. See `WINLATORXR_DESC.md`, Path E. Writing a mod
   is what you do when a game has no VR support - establish that it has none.
5. **Is there an existing PCVR mod to retarget?** If it will not simply run,
   retargeting one is still far less work than writing one, and three ports in
   this family did exactly that.
6. **Is there a modding community?** Someone has probably already documented
   the camera. Search before disassembling.
7. **How heavy is it?** Old is good. A 1998 renderer leaves enough headroom
   on a Quest 3 for 2.25x supersampling; a 2007 engine may not leave enough
   for stereo at all.
8. **Does it already support head tracking?** TrackIR, FreeTrack,
   SimConnect, FSUIPC, a UDP protocol? For racing and flight games the answer
   is often yes, and then `opentrackWXR` supplies poses in a format the game
   already accepts and the port may need **no game code at all**. Check this
   before anything else - it is the difference between days and months.
9. **What genre is it, really?** This decides the whole input design: a
   first-person shooter wants 6DOF hands; an RTS wants a tabletop view and a
   laser pointer; a cockpit game wants head-look plus the mouse it already
   has; a point-and-click may want nothing but injected events.

> ### CHECKPOINT 0 - with the owner
> Confirm: the game, where it is installed, which build or patch level, and
> whether the plan is a **real VR port** (XrApi, 6DOF) or an **injection
> port** (InputAPI, head-look as mouse). Everything after this diverges on
> that answer.
>
> If it is an **injection port**, settle one more thing here, because it
> decides the architecture: **does the design need mouse *motion*?** InputAPI
> cannot move the mouse - clicks and scroll only. Head-look therefore needs
> either Wine-side injection (`SendInput` / `mouse_event`) alongside InputAPI,
> or no head-look at all, which is perfectly viable for an RTS or a
> point-and-click. See `WINLATORXR_DESC.md`, Path B.
>
> Also confirm licence expectations - whether the result can be
> published at all, and whether any of the game's source may appear in the
> repository.

---

## Stage 1 - Get it running flat, in a container

Before any VR. Launch the unmodified game in WinlatorXR and play it on the 2D
screen.

- Pick a container resolution and write it down. 2800x1400 is the usual
  starting point; see `WINLATORXR_DESC.md` on why this is also your
  supersampling control.
- Note what is already broken: launcher issues, intro movies, mouse capture,
  save paths, missing codecs.
- Turn off post-processing and verify it *stays* off after a relaunch. Games
  of this era silently restore graphics settings, and this is a live problem
  in at least one port in this family.

> ### CHECKPOINT 1 - with the owner, on the device
> "Does the unmodified game run in a container, and what did you have to do to
> make it run?" Every workaround here is a thing your installer or launcher
> will need to automate later.

---

## Stage 2 - Get your code into the process

Pick the vector (see `WINLATORXR_DESC.md` section 6 and `TOOLS.md` section 2),
then prove it with the smallest possible change:

**Write a log file.** That is the whole deliverable for this stage. One file,
one line, with `__DATE__ __TIME__` compiled in.

Then add the build-and-deploy script with its freshness guard *now*, before
there is anything complicated to debug. You want the "am I running the binary
I just built?" question answered permanently, from the start.

> ### CHECKPOINT 2 - with the owner, on the device
> "Does the log file appear, and does its first line show the build stamp you
> just built?" If a hook was needed to get loaded, this is also where you
> learn whether that hook works under Box64 and Wine - which is not a given,
> and is far cheaper to find out now than after a feature depends on it.

---

## Stage 3 - Own the window

Still no VR. Make the game's window fill the display with no chrome.

- Strip `WS_CAPTION | WS_THICKFRAME | WS_BORDER`, size to screen metrics.
- Make it idempotent and call it every frame - engines rebuild windows.
- Watch for the engine fighting you (Shogo's `CheckRenderer()` resets the
  screen mode when surface, render and client sizes disagree).

This is a prerequisite for the sync pixel, not a cosmetic step: with chrome in
the corner, the runtime samples the chrome and head tracking silently degrades.

> ### CHECKPOINT 3 - on the device
> "Is the game filling the whole view with no title bar?"

---

## Stage 4 - The XrApi client, in-process

Now the protocol. Build it standalone and verify it before wiring it to
anything.

1. Write the handshake files to `Z:\tmp\xr` (with a desktop fallback path);
   read `system` for the headset information. **Choose your API version here
   and write it down** - `version` is read once, an unrecognised value yields
   no data at all, and 0.6 upward use a different packet shape.

   **Use 0.5** (or 0.4) for a first port, and do not treat that as a
   compromise: those versions are human-readable in a log, the XrAPI UDP
   Tester can simulate them, and poses arrive without declaring spaces. 0.5 is
   0.4 plus the InputAPI listener at no cost, so ask for it unless you have a
   reason not to. Upgrade later, from a working baseline, for a named reason -
   see `XRAPI_UPGRADE.md`.
2. Open the receive socket on 7872. (From 0.4 the runtime sends to 7872
   *and* 7873 - they are duplicates, not a fallback, so bind one.)
3. **Drain** every queued datagram per frame, keep the newest.
4. Parse: 28 floats, frame id, bool string, HMD name. Tolerate extra trailing
   tokens and both bool-string lengths.
5. **Normalise every quaternion on ingest.** Unconditionally. See
   `XRAPI_V0_4_DESC.md` section 7.
6. Log the raw packet verbatim every few seconds behind a verbose flag.
7. Send the outbound packet on 7278 each frame - zero haptics is fine for now.

Test it entirely on the desktop with the UDP tester. Do not go to the device
for this stage.

> ### CHECKPOINT 4 - with the owner
> Ask for **one raw packet captured on the actual headset** and compare it
> field-by-field against what the tester sends. This is where you find out
> that the spec and the device disagree, if they do - and it is much cheaper
> to find out now than through a downstream symptom. Also ask the owner to
> press each button in turn while the bool string is logged, and build the
> button table from what *moved*, not from this document.

---

## Stage 5 - The sync pixel

Small, and the highest-value thing in the whole project. Draw `HMD_SYNC` as
red in a 10x10 square in the top-left corner, last, after everything else.
Twice in side-by-side mode. Full detail and rationale in
`XRAPI_V0_4_DESC.md` section 5.

Do this **before** the camera. If it is wrong, head tracking will feel broken
later and you will debug the camera for it.

> ### CHECKPOINT 5 - on the device
> Nothing to see directly - the square is near-black at low sync values. What
> you are checking is further on, at checkpoint 6: whether head tracking feels
> *locked* or *swimmy*. Swimmy with correct maths means sync.

---

## Stage 6 - 6DOF camera (the first real milestone)

**Mono first.** `MODE_3D = 0`, one image to both eyes. No stereo, no hands.
Just the head driving the game's camera.

1. Find where the game sets its view transform.
2. Convert the HMD pose: recentre inverse, rotate the canonical axes, flip for
   handedness, scale metres to engine units
   (`XRAPI_V0_4_DESC.md` section 6).
3. Apply **orientation first.** Get looking around correct before positional
   tracking - fewer variables.
4. Then position: add the head's offset to the game's camera origin.
5. Add a recentre binding immediately. You will use it constantly.
6. Freeze on the last good pose when packets drop. Never zero.

Pitfalls, in the order they usually bite: axis flipped (world mirrored);
pitch and roll swapped (tilting looks like turning); position in metres added
to engine units (camera at the far edge of the level, or glued still); yaw
double-applied because the game is also applying mouse look.

> ### CHECKPOINT 6 - on the device, and this is the big one
> "Does looking around feel locked to your head?" Ask specifically:
> - does the world stay still when you hold your head still?
> - does it swim or lag when you turn quickly? (suspect sync, stage 5)
> - does leaning move the view the right way and the right amount?
> - is the horizon level?
>
> **Do not proceed until this is right.** Everything after it is layered on
> top, and a camera defect will be misattributed to whatever you build next.

---

## Stage 7 - Depth: pick a presentation mode

Only now, and **this is a decision, not a step.** Four modes are available
(`XRAPI_V0_4_DESC.md` section 5a): mono, SBS, AER and non-VR. Measure before
committing - the cost difference is large and the comfort difference larger.

### SBS - `MODE_3D = 1`, render twice into the two halves of one buffer

- Offset each eye by IPD/2 along the camera's right vector. IPD arrives in the
  packet (float 25) - use it, do not hardcode 0.064.
- Set the per-eye viewport or rect, render, repeat.
- Use the FOV the packet asks for (floats 26-27, tangents of half-angles) and
  report back what you actually used.
- Draw the sync square in both halves.
- Measure the cost. If stereo is not affordable, **mono is a legitimate
  shipping mode** - it is what the Tremulous port does.

### AER - `MODE_3D = 2`, one eye per frame

Depth at mono cost, each eye refreshing at half rate. Often the better trade
on mobile hardware:

- offset one camera by `+/- IPD/2` along its local right vector, flipping sign
  each frame
- write the eye into the sync square's **blue** channel: 0 = left, 255 = right
- keep red as the frame id, as always

### Non-VR - `MODE_VR = 2`, a flat window with live tracking

No camera work at all, no stereo, and it cannot induce vection. Consider it
both as a shipping comfort option and as the **whole plan** for a game whose
camera is unreachable.

**For menus specifically, prefer `MODE_VR = 0` over `2`.** It stops the pose
stream you are not using, restores the runtime's own mouse control of the
screen, and drops the frame out of immersive presentation so the player can see
a whole menu at once. The Interstellar Marines port switches to `0` on every
menu and pause and back to `1` on entering a level; see `XRAPI_V0_4_DESC.md`
section 4 for the two cautions that come with it.

> ### CHECKPOINT 7 - on the device
> "Is there depth, and is it comfortable?" Watch for swapped eyes (depth
> inverted, instantly uncomfortable), mismatched halves, and a frame rate that
> has fallen off a cliff.

---

## Stage 8 - Hands

Genre-dependent, and the first stage you should consider *skipping*. A
tabletop RTS wants a laser pointer, not hands; a vehicle game may want none.

For a first-person game, in order:

1. **Convert both hand poses independently** of the head. Share only the
   recentre origin. Hands that move with head angle is the most common bug in
   this family of ports, and the cause is almost always a pose routed through
   the head transform.
2. **Swap the two hands at ingest** if left-handed mode is on, so everything
   downstream says "right" and means "dominant". Then never test the
   handedness flag again for anything spatial - a second test applies the swap
   twice. See `TIPS.md`.
3. **Aim the weapon from the dominant hand**, and keep the *view* on one
   source so it does not jerk per-hand.
4. **Only then** attach visible art - hands, or a weapon model. This is the
   expensive part and the part where frames of reference matter most; read
   `TIPS.md` "measure offsets in the frame you will use them in" before
   starting, not after three rounds of tuning.

> ### CHECKPOINT 8 - on the device, with controller overlays visible
> Ask for screenshots **with the controller models rendered**. Where the
> controller actually is relative to the game's geometry is the datum that
> makes hand placement a measurement instead of a guess, and only the device
> can supply it. One expensive bug in the Shogo port ended the instant this
> was visible.

---

## Stage 9 - Input mapping

Enumerate every control the game has and map it explicitly. Write the table
down and have the owner confirm it before implementing - the Contagion plan is
a good model: *VR action -> game action (PC equivalent)*, one line each, with
separate sections for distinct player states.

Decisions to make deliberately:

- which actions are **held** versus **edge-triggered** (a VR trigger is a held
  state; a game expecting a key-down edge may wait forever - this is why the
  Shogo multiplayer respawn hung)
- which stick does movement, which does turning, and whether turning is smooth
  or snap
- what happens in menus - most ports block game input while a menu is open and
  pass through a single escape key
- whether grips are duplicated or distinct, and whether any binding should
  deliberately *not* swap with handedness

> ### CHECKPOINT 9 - with the owner
> Confirm the mapping table before writing it, and again on the device after.
> Button layout is pure preference and the owner is the authority.

---

## Stage 10 - Comfort, polish, settings

In roughly this order of player impact:

1. **HUD scale and placement.** 2D art at fixed pixel sizes does not follow
   container resolution, and a HUD in the peripheral is unreadable in a
   headset. Derive from measured width against a reference width, with a
   strength control.
2. **Crouch, height and recentre** behaviour.
3. **Turn off post-processing** - and verify it stays off.
4. **Haptics**, as levels rather than events.
5. **Supersampling** via container resolution. Document the recommended size
   and its cost; it needs no code.
6. **An in-game settings menu.** Not an INI. A tester on a standalone headset
   cannot easily edit files, and you will otherwise be asking them to do it
   repeatedly.

---

## The shape of a good iteration

Once the stages are done, the loop that worked:

1. one change
2. build and deploy with the freshness guard
3. desktop test with the UDP tester, if the change is testable there
4. device test by the owner, with logs and screenshots
5. read the **logs** before theorising; add instrumentation if the log cannot
   answer the question
6. commit nothing until the owner has verified on hardware

The two failure modes to watch for in yourself: tuning numbers instead of
fixing the frame they are measured in, and reasoning about a screenshot when
a log line would have given the answer outright.
