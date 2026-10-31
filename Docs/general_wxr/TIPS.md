# Tips for an agent porting a PC game to standalone VR via WinlatorXR

Written for whoever (or whatever) picks up the next port. Every item here was
paid for once already. The ordering is roughly "how much time it saves".

---

## Part 1: Architectural decisions - get these right and the rest is work

### Do not modernise the renderer

**Modify the game's existing DirectX. Do not port it to DX11.**

The temptation is strong: DX9 or DX7 feels archaic, stereo rendering feels
like it wants modern APIs, render-to-texture would solve several problems at
once. Resist it.

- The game's renderer **already works** under Box64 and Wine, in the
  translation path the whole WinlatorXR stack is tuned for. A rewrite trades a
  known-good path for an unknown one on a mobile GPU behind two translation
  layers.
- Stereo needs **no** modern API. It is two renders into two halves of one
  buffer. Any engine that can render to a sub-rectangle can do it.
- The things you actually need - a camera transform you can write, a 2D blit
  for the sync pixel, a window you can resize - exist in every API back to
  DirectDraw.
- A renderer rewrite is a project of its own. It will consume the time the
  *port* needed, and the port is what players want.

**dgVoodoo and similar wrappers deserve a specific warning.** They are a
natural idea for an old game and they were how the PCVR ancestor of the Shogo
port did its upscaling. On standalone they are a liability: another
translation layer inside two existing ones, and a second thing to debug when
the image is wrong. The Shogo port removed dgVoodoo entirely and got better
results from a larger container. If a wrapper is in the parent project,
removing it is likely to be progress.

### Stay 32-bit if the game is 32-bit

Do not port to 64-bit to "modernise". You do not need to:

- **There is no OpenXR or OpenVR to satisfy.** That is the whole point of
  XrApi. No 64-bit runtime to load, no bitness boundary to cross. The reason
  other VR mods go 64-bit is to load a 64-bit OpenXR runtime; that reason does
  not exist here.
- Box86 handles 32-bit fine. Shogo ships a 32-bit `CShell.dll` from MSBuild
  v143, Win32/Release.
- Staying in-process is worth more than any benefit of 64-bit. The Shogo port
  began as an *out-of-process* 64-bit bridge with shared memory and became
  dramatically simpler when the XrApi client moved in-process: no IPC, no
  second executable to ship, start and reap, no lifetime problems, no
  serialisation.

Mixed bitness is the thing to avoid. A 32-bit game plus a 64-bit helper means
an IPC design, and that design will be the biggest source of bugs in the
project.

### Render fullscreen or borderless-stretched, and keep enforcing it

WinlatorXR captures the **window**, so the window is part of your render
pipeline:

- Strip `WS_CAPTION | WS_THICKFRAME | WS_BORDER`, size to the full display.
- **Re-apply every frame.** Engines rebuild their window on mode changes,
  alt-tab and device loss. Make the fix idempotent and call it from your
  per-frame update; this is cheap and saves a class of intermittent bug.
- A title bar does not merely look wrong - it moves the top-left corner, so
  the sync pixel lands on chrome and head tracking silently degrades.
- Watch for the engine's own consistency checks. Shogo's `CheckRenderer()`
  resets the screen mode whenever surface, render and client sizes disagree,
  and will fight a hand-rolled resize once a second and win. Move all three
  together, or let the container size do it for you.

### Start on an XrApi version you can read, and upgrade only for a reason

0.4 and 0.5 put one line of ASCII per frame on the wire. You can read it in a
log, diff two captures by eye, and paste one into the UDP Tester to reproduce
a pose on the desktop. On a standalone device with no debugger, that is worth
more than any amount of saved CPU.

0.6 changes the packet shape - poses no longer arrive unless you declare the
spaces you want located - and 0.7 makes it binary. Both are real upgrades for
real needs (analog triggers, velocities, formatting cost in a profile), and
both cost you the readable log and the tester. Upgrade from a device-verified
baseline, one step at a time, keeping the old parser behind a flag.
`XRAPI_UPGRADE.md` has the field-by-field detail.

The general form of this: **a higher version number is not a goal.** Neither
is a newer API, a modern renderer or a 64-bit build. Each is a cost to be
justified by something you need.

### Choose the presentation mode deliberately, and consider shipping several

There are four (`XRAPI_V0_4_DESC.md` section 5a), and the instinct to treat
side-by-side stereo as "the real one" and the rest as fallbacks is wrong:

- **SBS** renders the scene twice per frame. Full per-eye frame rate, double
  the scene cost.
- **AER** renders one eye per frame, alternating. Real depth at mono cost,
  with each eye refreshing at half rate. On a mobile GPU behind two
  translation layers this is frequently the better bargain.
- **Mono** has no depth and costs least.
- **Non-VR** puts a flat window in the headset with tracking still flowing -
  the Wii treatment. It needs **no camera work at all**, which means it stays
  available when a game's camera turns out to be unreachable, and it cannot
  induce vection, which makes it the comfort option.

Two practical consequences. First, **non-VR mode is a viable early target**,
not a defeat - for a game with a hostile camera it may be the only shippable
route, and for a strategy or platform game it may be the *better* experience.
Second, if you support more than one mode, **centralise the selection in one
function**. HWXR had to order its SBS tests before its AER tests at three
separate sites to stop a runtime toggle setting both flags at once.

### Prefer the engine's own code paths over new ones

A setting that engages a second code path only when enabled is a second thing
to maintain and a second thing to debug on a device you cannot attach a
debugger to. Where a container setting, an existing CVar or an existing engine
mechanism gets you the feature, use it and document it. Shogo's supersampling
shipped as *a sentence in the README* rather than a `VRRenderScale` setting,
and that was the better engineering decision.

---

## Part 2: Working method - how to not lose days

### Measure; do not infer from screenshots

**The single most valuable lesson from the Shogo port.**

Three rounds of weapon-hand placement were tuned by looking at screenshots and
adjusting numbers. All three produced values that worked only on the machine
they were found on. The round that *fixed* it started by logging the actual
transforms - at which point the answer was obvious and arrived first try.

So: when something is in the wrong place, do not reason about where it should
go. Add a log line that prints where it *is*, in the frame you care about, and
look at the number. On a standalone headset, where there is no debugger and
each iteration costs a deploy, this is the difference between an afternoon and
a week.

Things worth instrumenting on day one:

- the raw inbound packet, verbatim, every few seconds
- the button bool string on every press
- the camera and hand transforms after conversion
- window, surface and render dimensions when they change
- a model or node dump, once, if the game has art you must attach to

### Put diagnostics behind one verbose flag

All of the above, gated by a single setting, **off by default**. Including the
file dumps: a player should not be writing a 177 KB model dump every launch.
Make the flag reachable from an in-game menu, because editing an INI on a
standalone headset is genuinely painful and you will be asking a human to do
it repeatedly otherwise.

### Measure offsets in the frame you will use them in

If you attach something to an object - a hand to a weapon, a camera to a head -
express the offset in **that object's** frame, not in the frame of the thing
being attached.

Shogo's lent hand was offset along the *hand's* axes, which rotate with the
wrist. Values found at one wrist angle meant nothing at another, so every
round of tuning corrected for the previous round rather than for reality, and
the offsets grew instead of converging. Re-expressed in the *gun's* frame -
where a grip is a fixed property of the model - a single estimate held in both
handedness modes, on desktop and on device, with no trim at all.

The test for whether your frame is right: **a correct value transfers.** If a
number only works where it was found, the frame is wrong. Stop tuning and fix
the frame.

### Make the simulator honest, then still verify on hardware

A desktop UDP tester is essential - it turns a 10-minute deploy-to-headset
cycle into a 10-second one. But it lies in ways that look like deep bugs:
non-unit quaternions, implausible hand separation, perfectly still poses that
hide smoothing problems. Normalise on ingest, and treat desktop as "the code
path runs", never "the values are right".

### One change per deploy, and verify the right binary is running

Embed `__DATE__ __TIME__` in the DLL and log it as the first line of your log.
Then your build script can refuse to deploy a binary older than the newest
source file. Shogo's build guard caught a stale build being tested as new,
which is a failure mode that wastes a whole session and is invisible without
the check. Hash the deployed file and compare.

### Ask the human for a screenshot *with the controller overlay on*

A long and expensive bug ended the moment a screenshot showed where the
*controller* was, not just where the hand was. If something tracked is in the
wrong place, the tracked thing's real position is the missing datum - and on a
headset only the human can capture it.

---

## Part 3: VR-specific correctness

### The sync pixel is not optional, and it is not a detail

See `XRAPI_V0_4_DESC.md` section 5. Wrong or missing sync means reprojection
against the wrong pose, which means motion sickness that frame-rate work
cannot fix. Do it before anything else and verify it on hardware.

### Separate head from hands, completely

A recurring bug across these ports (named explicitly in the Interstellar
Marines notes) is hands that move with head angle. If a hand pose passes
through the head's transform, every head turn swings the hands.

Convert each pose independently from the packet. Share only the recentre
origin. If hands "track funny", suspect this before suspecting the maths.

### Decide early what the aim vector is, and keep it single

Camera yaw/pitch driven per-hand makes the view jerk on every off-hand action.
Drive the *view* from one source - head, or the dominant hand - and let only
the *shot* vary per hand.

### Handle handedness once, at ingest

Swap the two hands' poses as the packet is parsed, so everything downstream
can say "right hand" and mean "dominant hand".

Then **never test the handedness flag again** for anything spatial. A second
test applies the swap twice and silently selects the wrong hand - which is
exactly how Shogo's melee gesture ended up requiring left-handed players to
swing their right hand. Audit for `if (leftHanded)` after you introduce the
ingest swap; the surviving cases should only be physical channels that are
genuinely not swapped, such as haptic motors.

### Mod-owned objects must render in the same pass as the game's own

If you create objects alongside the game's first-person view, they must carry
whatever flag the engine uses for view-model rendering (`FLAG_REALLYCLOSE` in
LithTech; every engine of the era has an equivalent). Without it, your object
is an ordinary world object whose depth is compared against the *world* rather
than against the view model, and it draws through the weapon it is attached to.

### Freeze, do not zero, on dropped input

Hold the last good pose. One zeroed frame is a violent jolt in a headset.

### Haptics are a level, not an event

Send amplitude every frame for as long as the condition holds.

---

## Part 4: Shipping

### Fewer settings, better defaults

Standalone players want open-and-play. Every setting is a support question and
a thing a player can get wrong. Prefer auto-detection with a strength control
over a raw value, and prefer a sensible default over a knob.

Watch for defaults you ship being **overridden by a saved config** on a test
device. A fixed default does nothing for a tester whose INI already has the
old value; say so explicitly when you hand over a build.

### Expose on-device settings in-game, not in an INI

Editing a text file on a headset means a human doing file management in a
mobile VR shell. Anything a tester might change belongs in a menu.

### Respect the source licence, and automate the check

Several of these games have source releases whose licence forbids
redistribution. That is compatible with publishing a *mod*, but only if the
repository contains none of the original source - which includes diffs and
context lines, since a unified diff of someone else's file contains their file.

The workable pattern, from Shogo:

- the mod's own files live in the repository under your own licence
- edits to the licensed source are described in **prose** notes: file,
  function, line, and what to add - no quoted original code, no diffs
- a script checks the repository against a **pristine** copy of the source and
  reports any copied block

Run that check against a pristine tree, not your working one. Against a tree
with the mod integrated it reports your own added lines and the result is
noise.

### Document what you learned, in the repository

Not the code structure - git shows that. Document the things that are
*invisible* afterwards: which frame an offset is measured in and why, which
model nodes are named inconsistently, the measured container size and its
cost, what a wrong value looked like on screen. That is what the next port -
or the next agent on this one - cannot reconstruct.
