# PROJECT_PLAN template - `<GAME>` to standalone VR via WinlatorXR

**How to use this file.** Copy it into the new project's `Docs/` folder,
rename nothing, and fill it in *with the owner* before writing code. Replace
every `<ANGLE BRACKET>` placeholder. Delete sections that genuinely do not
apply and say why in one line rather than leaving them blank - a silently
missing section reads as an oversight later.

Sections 1 and 2 are an intake interview. **Do not skip them and do not
invent answers.** Every question in section 1 is one an agent cannot resolve
from the code, and a wrong assumption there propagates through the whole port.

---

## 1. Intake - questions only the owner can answer

> Fill this in by asking. Mark anything still unknown as `TBD` rather than
> guessing; a visible `TBD` is a prompt, a guess is a bug.

### The game

| Question | Answer |
|---|---|
| Game, version, patch level | `<...>` |
| Store or distribution (GOG, Steam, disc, mod) | `<...>` |
| Install path on the owner's machine | `<...>` |
| Bitness | `<32 / 64>` |
| Renderer | `<DirectDraw / D3D7 / D3D8 / D3D9 / OpenGL>` |
| Engine | `<...>` |
| Source available? Under what licence? | `<...>` |
| Mod API or SDK? | `<...>` |
| **Does a VR build already exist?** (official VR, SteamVR/OpenVR, mature PCVR mod) - if so, OXRWXR + OpenComposite may need no new code | `<...>` |
| Existing PCVR mod to retarget? | `<...>` |
| Existing modding community or documentation? | `<...>` |
| **Native head-tracking support?** (TrackIR / FreeTrack / SimConnect / FSUIPC) - if yes, opentrackWXR may remove the need for a mod entirely | `<...>` |

### The port

| Question | Answer |
|---|---|
| **Real VR port (XrApi, 6DOF) or injection port (InputAPI, head-look as mouse)?** | `<...>` |
| Target headsets | `<...>` |
| Presentation mode: mono / SBS / AER / non-VR (see XRAPI section 5a) | `<...>` |
| Ship a non-VR mode as a comfort and performance fallback? | `<...>` |
| Recommended container resolution | `<...>` |
| First-person, third-person, or tabletop/strategy framing | `<...>` |
| Hands in view at all? | `<...>` |
| If injection: is mouse **motion** needed? (InputAPI cannot; needs Wine-side injection) | `<...>` |
| Multiplayer in scope? | `<...>` |

### Constraints

| Question | Answer |
|---|---|
| May the result be published? Where? | `<...>` |
| May any of the game's source appear in the repository? | `<...>` |
| Which files are **release-owned** and must never be touched by automation? | `<...>` |
| Who verifies on hardware, and how fast is that loop? | `<...>` |
| Any prior work to inherit or discard? | `<...>` |

---

## 2. Hard rules

> Carry forward whatever the owner states, verbatim. These are binding on
> every automated pass including agents, scripts and build steps. The Shogo
> project's rules are reproduced as a model; replace with this project's.

**Release-owned files are read-only.** `<e.g. AutoUpdate/, every version.txt
under Release/, Release/ReadMe.txt>` are human-approved release artifacts.
Automation must never create, modify, overwrite, move or delete them. Reading
for verification is fine; writing is not. The only files an automated pass may
write under `Release/` are `<list them>`.

**Licensed source handling.** `<If the game's source licence forbids
redistribution:>` the repository contains **no** original source - and that
includes diffs and context lines, since a unified diff of a licensed file
contains that file. Edits to the licensed tree are recorded as **prose** notes
(file, function, line, what to add) in `<SourceModifications/>`, with a
matching entry in this plan. A `leakcheck`-style script verifies the
repository against a **pristine** source tree before any publish.

**Superseded documents.** `<e.g. Docs/Pre-LaunchDocs/>` may be read but is not
current truth.

---

## 3. What exists today

### Repository layout

```
<fill in once the shape is known>
  game/ or src/        the mod's own source, under our own licence
  build/               project file + build-and-deploy script
  tools/               leakcheck, packaging
  Docs/                this plan, feature docs, bug list
  Release/             shippable artifacts (release-owned - see hard rules)
```

### Current state

`<One honest paragraph. What runs, what does not, what is uncommitted, what
was inherited from a previous attempt and whether it is worth keeping.>`

### The blocker

`<The one thing standing between here and the next milestone. If there is more
than one, they are not all blockers - pick the one that gates the others.>`

---

## 4. Approach

### Injection vector

`<Run the existing PCVR build via OXRWXR + OpenComposite (no mod) / native mod
API / source rebuild / DLL proxy (which name) / runtime shim / external
process / opentrackWXR into the game's own head-tracking support>`

Why: `<one or two sentences>`

### Decisions taken deliberately

Record these so they are not relitigated every session. Defaults from
`TIPS.md`, with the reasoning; change them only for a stated reason:

| Decision | Choice | Why |
|---|---|---|
| Bitness | `<32-bit>` | no OpenXR runtime to load; stays in-process |
| Renderer | `<keep the game's own>` | known-good under Box64/Wine; stereo needs no modern API |
| Wrappers (dgVoodoo, ReShade) | `<none>` | another layer inside two translation layers |
| Process model | `<in-process>` | no IPC, no second binary to ship and reap |
| Presentation mode | `<mono first; AER or SBS if affordable>` | mono ships; AER gives depth at mono cost; non-VR needs no camera work |
| Supersampling | `<container resolution, no code>` | the runtime already filters on downsample |
| Settings surface | `<in-game menu>` | editing an INI on a headset is painful |

---

## 5. Milestones

Lifted from `FIRST_STEPS.md`. Each leaves something testable; each has a human
checkpoint. Tick them off here as the project's live status.

- [ ] **0** Feasibility assessed, intake complete
- [ ] **1** Unmodified game runs in a container
- [ ] **2** Our code loads and writes a log with a build stamp
- [ ] **3** Window is borderless and fills the display
- [ ] **4** XrApi client parses real device packets; button table built from a device log
- [ ] **5** Sync pixel drawn, last, 10x10, both halves
- [ ] **6** **6DOF head tracking feels locked** (the big one)
- [ ] **7** Presentation mode chosen and measured (mono / SBS / AER / non-VR)
- [ ] **8** Hands, if the genre wants them
- [ ] **9** Full input mapping, owner-confirmed
- [ ] **10** HUD scale, comfort settings, in-game menu

---

## 6. Features

> One subsection per feature, written *before* implementing. Keep completed
> ones in a separate `COMPLETE_FEATURES.md` with what was learned - the
> learning is the part that cannot be reconstructed from the diff.

### Feature `<n>` - `<name>`

**Goal.** `<What the player gets. One paragraph, no implementation.>`

**Can it be done at all?** `<The honest answer, including what the engine does
not provide. If a route was rejected, say which and why - this is the section
that stops a later session rediscovering a dead end.>`

**What must be proven first.** `<The assumption the whole feature rests on,
and the cheapest experiment that settles it. Build nothing until it is
answered, in writing, here.>`

**Approach.** `<The plan, in steps.>`

**Fallbacks.** `<If the primary route fails. Ranked.>`

**Affected files.** `<...>`

**Settings.** `<CVar or INI key, default, and whether it needs a restart.
Prefer a good default over a knob; prefer auto-detection with a strength
control over a raw value.>`

**Edge cases and risks.** `<...>`

**Acceptance criteria.** `<Checkboxes. At least one must be an on-device
observation by the owner, not an assertion by the agent.>`

---

## 7. Verification

**Build and deploy.** `<command>`. Confirm the printed build stamp, freshness
line and SHA-256 `match=True` at every target. A stamp older than the newest
source file means the deploy is refused - do not work around this.

**Confirm the right binary is running.** First line of `<log file>` carries
the build stamp. Check it before interpreting any device report.

**Desktop pass.** With the XrApi UDP tester: `<what this can and cannot
prove>`. Remember it proves the code path runs, not that the values are right.

**On-device pass** - the only real gate:

- [ ] `<per-feature acceptance criteria>`
- [ ] head tracking still feels locked
- [ ] frame rate acceptable at the recommended container size
- [ ] correct in both right- and left-handed mode
- [ ] previously working features unaffected `<name the ones the change could
      plausibly have touched, not all of them>`

---

## 8. Risks

| Risk | Likelihood | What we do about it |
|---|---|---|
| `<...>` | `<...>` | `<...>` |

Standing risks worth listing in most projects of this kind:

- **The engine fights a hand-rolled window or mode change.** Consistency
  checks that reset the display mode will win against a per-frame fix.
- **A hook behaves differently under Box64/Wine** than on the desktop. Prove
  hooks on device early.
- **Tuned values do not transfer** from tester to device. Usually a frame-of-
  reference problem, not a precision problem.
- **Saved config overrides new defaults** on the test device, so a shipped
  default change appears to do nothing.
- **Art-dependent code breaks with mods** that replace the game's models.
  Degrade to stock behaviour rather than rendering something broken.
- **Licence exposure** if any original source reaches the repository.

---

## 9. What we learned

> Append as you go. Not code structure - git shows that. The things that are
> invisible afterwards: which frame an offset is measured in and why, which
> assets are named inconsistently, what a wrong value looked like on screen,
> which approach was tried and abandoned. This is the section that makes the
> *next* port cheaper, and the one most likely to be skipped under time
> pressure.

### `<Topic>`

`<What was expected, what was actually true, how it was established, and what
to do about it next time.>`
