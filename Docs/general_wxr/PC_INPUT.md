# PC input in XrAPI mods: do not synthesize Windows input

**Audience:** anyone writing or reviewing an XrAPI mod for WinlatorXR,
especially AI agents working from an existing port.

**Short version:** in a VR/XRAPI mod, do **not** use `SendInput`,
`mouse_event`, `keybd_event`, `PostMessage`/`SendMessage` with `WM_KEY*` or
`WM_MOUSE*`, or synthetic cursor warps to make the game do something.

Read the controller state from XrAPI and either:

1. **call the game's own input/action function directly**, or
2. **write the state its own input code already reads** (in-process), and let
   the game consume it normally.

This is not a style preference. On WinlatorXR these Windows input paths are
unreliable, focus-dependent, and frequently do nothing at all. Several ports
have already lost days to this.

---

## 1. Why the Windows input route fails on WinlatorXR

WinlatorXR does not run the game in a normal interactive desktop session.

- **The game window is usually not the real foreground window.** The runtime,
  the container UI, or another Wine window owns focus. `SendInput` and
  `keybd_event` go to the foreground window, not to the game.
- **`GetKeyboardState` can return `FALSE` or an empty array** when the game
  window is not focused, even though the game is still running and rendering.
- **`PostMessage` is easy to lose.** A game that pumps messages with
  `PeekMessageA`/`DispatchMessageA` may ignore key messages, only handle them
  inside a particular state, or never handle them in its window procedure at
  all. XOPS is exactly this case: its window procedure only handles
  `WM_DESTROY`; ESC and the game keys are consumed in the update loop.
- **The mod's own input isolation may block the synthesized event.** A VR mod
  normally blocks physical keyboard/mouse input. If it also blocks
  `WM_KEY*`/`WM_MOUSE*` in its window filter, any event you post is filtered
  out by your own code.
- **Keys are a layout/focus/timing abstraction.** XrApi buttons and triggers
  are the source of truth. Turning them into keys adds a fragile translation
  layer: edge loss, repeat timing, focus, keyboard layout, and game-specific
  key bindings.
- **WinlatorXR's InputAPI is not a shortcut either.** XrAPI 0.5's InputAPI
  sends clicks and scroll, not mouse motion, and it is a runtime-level input
  path, not a substitute for reading the controller state that is already
  arriving over 7872/7873.

The same code can appear to work on the desktop and fail on the headset. Treat
"it worked on PC" as no evidence for a synthetic-input path.

---

## 2. Do not use these in an XrAPI mod

| API / mechanism | Why it is a trap |
|---|---|
| `SendInput` | Goes to the foreground window; often not the game under WinlatorXR. |
| `mouse_event` | Same focus problem; cannot be trusted for look or clicks. |
| `keybd_event` | Same focus problem; key layout and repeat timing add more failure modes. |
| `PostMessage(WM_KEYDOWN/UP)` | Often ignored; the game may not handle keys in its wndproc, or may be in a state that never dispatches them. |
| `PostMessage(WM_LBUTTONDOWN/UP)` | Same; XOPS reads the mouse button from its own input array, not from messages. |
| `SetCursorPos` warps to simulate mouse look | Fights the game's own recentre poll; focus and clip behaviour differ under Wine. |
| Virtual key injection into another process | Still a key-shaped abstraction; you do not control when the game reads it. |

These are acceptable **only** in a clearly marked PC debug mode that is not
shipped, and only when no VR input is active. They must never be reachable in
the release build or while XrApi is driving input.

---

## 3. Do this instead

### 3.1 Preferred: call the game's own function

Find the code the game runs when the action happens, and call it.

Examples from the XOPS-WXR port:

- **ESC / menu:** the game consumes ESC at `0x413D5B`. A fresh edge either
  clears the message-loop flag (`0x491A9C = 0`, quit) or calls `0x413C00`
  (return to title). The mod hooks the game's native consumer call at
  `0x413BA2`, injects the ESC edge into the game's own input bytes
  (`0x48C93B`/`0x48C82F`), then calls the original consumer. No key event and
  no `PostMessage`.
- **Gameplay commands:** the game builds player input flags from
  `GetKeyboardState` in its own mapper at `0x407520`, then runs the player
  update at `0x407CF0`. The mod patches the `call 0x407CF0` site at
  `0x413AC4`; its wrapper builds the game's internal key image from XrApi,
  calls the game's mapper, then calls the original update. This is still a
  key image internally, but it is fully in-process and uses the game's own
  weapon-specific and edge logic. There is no OS key event.

The general pattern:

```c
/* Find the call the game makes when it wants the action.
   Replace it with a wrapper that supplies XrApi state and then calls
   the original. */
static void __cdecl ActionHook(void)
{
    if (xrActionPressed()) {
        /* Option A: write the exact state the game's consumer reads. */
        /* Option B: call the action function directly. */
    }
    original_action();
}
```

Use the real call site, not an arbitrary point in the frame. Input edges and
state transitions are often consumed at one specific place in the update.

### 3.2 Fallback: supply the state the game already reads

If the game genuinely polls `GetKeyboardState`, `GetCursorPos`, or a similar
API and there is no practical action function to call:

- hook the API **in-process** (IAT or inline, whichever fits),
- fill the buffer/state the game expects,
- return a result that says the call succeeded.

This is still preferable to OS events because:

- it is not affected by window focus,
- it cannot be intercepted by the foreground window,
- it is not affected by keyboard layout,
- it lands in the same array the game already uses.

But do not stop here if you can find the consumer. Supplying a key image is
still a translation layer; the direct consumer call is stronger.

**Important:** if the game window is unfocused, do not skip injection just
because the real API returned `FALSE`. If XrApi is delivering packets, that is
the source of truth. Clear the buffer, fill your state, and return success.

### 3.3 External process / no in-process access

If you cannot inject into the game process, use WinlatorXR's **InputAPI**
(XrAPI 0.5, port 7728) for the click/keyboard side. It is the runtime's
supported input path, not an OS event from your process.

Limits to remember:

- InputAPI sends clicks and scroll; it does **not** move the mouse.
- It is not a substitute for reading XrApi poses/buttons.
- Prefer an in-process call or state hook when the mod already runs in the
  game process.

---

## 4. XOPS-WXR case study

What was tried and what happened:

- **`SendInput` / `keybd_event` / `mouse_event`:** abandoned. Wrong focus
  target, unreliable under Wine.
- **`PostMessage(WM_KEYDOWN)` for ESC:** did not work on device. XOPS's window
  procedure does not handle keys; the ESC consumer is in the update loop.
- **Filling `GetKeyboardState` with synthetic keys:** worked intermittently on
  the desktop but was not trustworthy on device, and still depended on key
  bindings and focus. It is now a **dev-only fallback**, compiled and called
  only under `XOPS_DEV_BUILD` if the direct hook fails to install.
- **Direct player-update hook:** builds the game's internal key image from
  XrApi, calls the game's own mapper `0x407520`, then the real player update
  `0x407CF0`. This is the working path.
- **Direct ESC consumer hook:** calls the game's own `0x413CF0` with a fresh
  ESC edge. This is the working path for menu/quit.

The remaining non-debug mouse path in XOPS is the menu pointer, which still
feeds the game's own `GetCursorPos`/`ScreenToClient` and `VK_LBUTTON` state.
The next step is to call the title/mission-select action `0x411F80` with a
chosen hotspot instead of relying on the cursor.

---

## 5. Debug-only exception

A PC debug mode may use ordinary Windows input so a developer can drive the
game without a headset. If you do this:

- gate it behind a compile-time dev flag **and** a runtime manual-mode switch
  (XOPS uses the `0` key),
- make it impossible to enter from a headset-only session,
- never leave it enabled in a release build,
- document it as debug-only in the README and in the code.

Debug-only does not mean "use it for the release path when the direct call is
hard". It means it is not part of the product.

---

## 6. Review checklist for AI agents

Before writing or approving input code in an XrAPI mod:

- [ ] Does the code call `SendInput`, `mouse_event`, `keybd_event`,
      `PostMessage`, or `SendMessage` with `WM_KEY*`/`WM_MOUSE*`?
      If yes, stop and find the game's consumer.
- [ ] Is the action gated to a debug-only mode?
      If not, remove it from the release path.
- [ ] Did you find the exact function the game runs for this action?
      If not, search for the input consumer: key-state reads, mouse-button
      checks, menu hit-test functions, and the call sites that use them.
- [ ] If you are filling an input API buffer, are you doing it in-process and
      returning success even when the real call fails?
- [ ] Does the direct call run at the same point in the frame as the game's
      own call? Input edges are often consumed once, at a specific phase.
- [ ] Are you respecting the game's own edge detection? If you write a held
      key, do not make the game see a fresh edge every frame unless that is
      what a real held key does.
- [ ] Does the code work when the game window is unfocused?
- [ ] Is any debug fallback compiled and called only under a dev build?

Red flags in a patch:

- `#include <winuser.h>` plus `SendInput` in the release path.
- `PostMessage(g_gameWnd, WM_KEYDOWN, ...)`.
- `keybd_event(VK_...)`.
- `mouse_event(MOUSEEVENTF_...)`.
- "It worked on the desktop" as the only evidence.
- A comment like "Wine needs this" without a device log showing it works.

---

## 7. Bottom line

**XrApi is the source of truth. If a packet arrives, the mod is responsible for
delivering the action.**

Deliver it by running the game's own code, not by asking Windows to invent a
keyboard or mouse event. Windows input is a different process, a different
focus, and a different abstraction; on WinlatorXR it is usually the wrong one.
