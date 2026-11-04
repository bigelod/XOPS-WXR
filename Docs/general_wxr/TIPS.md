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

### The player's world yaw is not the client's view-angle variable

Every room-to-world transform - where the hand is, which way a positional
lean goes - needs the player's yaw **in the world**. In id Tech 3 and its
derivatives the client-side variable with the obvious name is only half of
it: `cl.viewangles` is what the client has accumulated, and the server keeps
the rest in `ps.delta_angles` and adds it back. That is how a spawn point, a
teleporter or a `setviewpos` turns the player without the mouse moving.

Use `cl.viewangles[YAW] + SHORT2ANGLE( cl.snap.ps.delta_angles[YAW] )`.
Expect the same split under another name in any engine where the server can
reorient the player. The symptom is subtle and constant: a hand drawn beside
the wrong hip, a lean that goes sideways - everything off by the map's spawn
angle, which is why it never shows up standing at yaw 0 in a test harness.

### If you compensate for something, test it against a MOVING input

A compensation - "the aim follows the hand, so rotate the camera back" -
cannot be tested with the input held still. Two different traps, both paid
for in Tremulous WXR:

- Many such errors are **exactly zero** at the origin. A frame correction
  that forgets the body's yaw is perfect at yaw 0 and wrong everywhere else.
  Testing at the origin is testing the one case that cannot fail.
- A **fixed** controller angle gets absorbed by the engine. Here the server
  folded it into `delta_angles` the moment anything set the view angles,
  after which the aim was constant and a camera that tracked the gun was
  indistinguishable from one that did not.

Sweep the input, on a different period from anything else that moves, and
assert the compensated quantity holds still. A spoofer needs a sweep mode for
this reason, not just a hold mode.

### A rotation does not cancel against itself across a change of basis

Tempting and wrong: "that term is in both frames, so it cancels - leave it
out of both." True for an additive offset. False for a rotation. Re-express a
basis through `V . G^T . H` with a shared rotation `Rz(b)` left out of `G`
and `H`, and what survives is `b` conjugated by `G` - equal to `Rz(b)` only
when `b` is zero or every rotation shares an axis. Build each frame with the
full angle, and let the arithmetic do the cancelling.

### Get a capture before you write a sanity check

A bound on incoming tracking data is an assumption about a coordinate
system, written as an assertion. Tremulous WXR saw a first packet reporting
a head height of 0.006 m, decided no real head is six millimetres off the
floor, and gated recentring behind "at least 0.5 m up".

XrApi positions are HEAD-CENTRED. A real Quest 3 capture has the head at
y = 0.006 and both hands at y = -0.23, a palm's drop below it. There is no
floor in that space. The gate never passed on hardware, the recentre never
happened, and the hands collapsed onto the head - which presented as "the
gun is stuck to my forehead" and, separately, as the dominant-hand setting
having no effect, because with no offset both hands resolve to one place.

The check did not validate the data. It validated a belief, and rejected the
hardware when the two disagreed. Worse, the synthetic poses in the test
spoofer had been "corrected" to the same wrong space, so the rig agreed with
the bug.

Get one real capture, commit it, and replay it. One packet of ground truth
is worth more than any amount of synthesised input, because synthetic poses
encode what you BELIEVE the protocol says and a capture encodes what it
does. Tremulous WXR keeps `scripts/SamplePoseData.txt` and replays it with
`xr_spoof.py --sample`.

### Send the FOV you actually rendered with, not a configured one

The FOV field in the outbound packet tells the runtime how wide the image it
is compositing is. Tremulous WXR sent a cvar holding 104.5 x 104.5 while the
engine rendered 99.3 x 67.0 - the vertical out by 37 degrees, because the
configured value was square and the real frame is 16:9.

This does not present as "the FOV is wrong". The runtime maps the frame onto
the display at the wrong scale, so the world moves at a different rate from
the head, and differently in pitch than in yaw. From inside the headset that
reads as the view sliding, as rotations that do not return to where they
started - as drift. Owner's words: *"up/down/left/right rotations start to
feel like they drift off from their axes."*

Take the value from the live frame every frame. Games change FOV - zoom,
aspect, cutscenes - so this cannot be a setting even if the first value
happens to be right.

### Controller poses arrive upside down on some headsets - ship the override

Meta controllers report the hands rolled 180 degrees. HWXR carries this as
comments against the Meta codenames in its detection chain (`XRAPI_V0_4_DESC.md`,
"HMD DETECTION FOR UPSIDE DOWN HANDS"), and Tremulous WXR confirmed a Quest 3
behaves the same way even though it takes a different branch there - so key
the correction off the MAKE, not the model. Some headsets are also reported
backwards, so provide a yaw trim as well as a roll one.

Detection is a guess about hardware you do not have. Ship the guess, and ship
an override with it that a player can use **without the console** - typing
from inside a headset means the on-screen keyboard. A `handsfix.txt` read
with plain `fopen` next to the executable costs twenty lines and saves a
device round trip per wrong guess. Read it outside the game's filesystem: the
player creates it by hand, so it is not in an archive, and on a pure server
the filesystem would refuse to see it at all.

Note what this kind of defect does to your test discipline: a rolled model is
visible and **not measurable**. No log line is wrong, so no amount of
instrumentation finds it. Those are the cases worth a screenshot.

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

### Version your config before you need to

Settings that are archived outlive the defaults that created them, and the
people holding stale values are exactly the people testing for you. A
Tremulous WXR trim changed its "detect" sentinel from `-1` to `auto`; every
machine that had run the previous build still had `-1` saved, read it as a
literal minus one degree, and silently lost detection. The bug looked like
the original defect had never been fixed.

Keep a `cfgVersion` setting. Bump it when a default changes in a way an old
saved value would defeat, and migrate on startup - touching **only** values
equal to the old default, because discarding a player's deliberate tuning is
worse than the bug you are fixing. Log what you changed.

Knowing the trap is not a mechanism for it. The deploy script here had been
warning about stale configs for weeks when this happened.

### If the log is the only channel back, it must work from the first line

Initialisation is where configuration is decided, and it usually happens
before the log file is open. Tremulous WXR registered cvars, migrated stale
ones and read a player override file before `fopen`, and the logging for all
three was silently dropped - so a settings repair happened invisibly and an
override file announced itself into nothing.

Buffer early lines in memory and flush them as soon as the file exists,
after the build stamp so the order still reads. Count anything you had to
drop.
