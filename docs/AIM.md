# Aim / Device Connections

This document describes the two aim modes exposed on the **Device Connections**
tab and records why "silent aim" is not part of the feature set.

## Modes

### Memory Aim

- Runs on the PC that hosts the overlay (the same PC as the game in a
  single-PC setup).
- Each frame the reader publishes entity positions, the target bone point and
  the camera. `Aim::Tick` projects the target, measures the pixel error from
  the screen centre and converts it to relative mouse counts.
- The counts are emitted with `SendInput(..., MOUSEEVENTF_MOVE)`.
- The loop is closed: the next frame re-reads the target, so the gain does not
  have to match the game's sensitivity exactly.
- This moves the real host cursor. It is **not** silent: the view moves, and
  anyone watching the screen or a demo sees the aim move.

### Hardware Aim

- For two-PC DMA setups where the overlay PC is not the game PC.
- `Aim::Tick` builds the same pixel-error step and submits a
  `MotionIntent` to `Base::Hardware::Session` (`Source::Aim`).
- The selected KMBox / Makcu / Ferrum device performs the smoothing and emits
  real HID reports to the game PC.
- Requires `Base::Hardware::Ready()`; with no device connected nothing moves.
- Like Memory Aim, it moves the real view through a physical input device. It
  is not silent.

## Target selection

- Targets are filtered by distance (`MaxDistance`), dead state and the local
  entity.
- FOV is 5-180 degrees and is measured in **screen space**, exactly like the FOV
  circle and the ESP. The vertical FOV used for the conversion is recovered from
  the same view-projection matrix the ESP drew with, so the aim cone cannot
  disagree with the on-screen projection (including the native Unity camera
  fallback).
- The closest candidate wins, with a small hysteresis bonus for the previously
  selected entity to stop target flicker.
- The aim point is sampled from the animation pose: head, neck, chest or
  pelvis. Anchors resolve independently, so a partial rig still contributes
  whichever central points it exposed. A bone sample is only trusted when it is
  finite and within a few metres of the entity's transform root; otherwise a
  body-relative height above the root is used. This keeps a stale rig layout
  from pointing the aim at the world origin.
- Aim is suspended while the menu is open and whenever the activation key is
  not held.
- The activation key is polled from **all** sources and OR-ed: the game-PC
  keyboard bitmap over DMA, the aim device's reported buttons, and the host
  keyboard. Short-circuiting on the DMA bitmap used to hide a working device or
  host input whenever the bitmap resolved but pointed at the wrong session.
- "Visible bones only" reuses the ESP's animator/cull state. It is not wall
  occlusion.

## Diagnosing "aim does nothing"

The aim tab shows a live line and the overlay draws a small HUD (top-left,
toggle **Show aim debug**). It reports the activation key source, the number of
entities and in-FOV candidates, the chosen target, the pixel delta, the step
actually emitted, whether the hardware device is ready, and the exact reason the
loop stopped (`aim disabled`, `menu open`, `aim key not held`, `no entities`,
`no target in FOV`, `hardware device not ready`, ...). Read that reason first.

Common cases:

- **`menu open`**: aim is paused while the menu is visible. Close it before
  testing.
- **`no target in FOV`**: no entity projects inside the circle. Check the FOV
  slider and whether the player ESP is actually drawing targets. The diagnostics
  panel also shows `skeletons/players` and `aim pose/players`; if skeletons is
  lower than players, those rigs are partial or still failing to resolve, and aim
  falls back to body-relative heights for them.
- **`hardware device not ready`**: Hardware Aim needs a connected device; the
  device panel shows the link state and error.
- **`moving (memory)` with no view change**: Memory Aim uses `SendInput` on the
  overlay PC. On a two-PC DMA setup this cannot reach the game PC; use Hardware
  Aim.

## Why true silent aim is not implemented

"Silent aim" usually means one of two things:

1. **Visual snap-and-return.** The client briefly moves to the target for the
   shot frame and then returns. This is still a real view change. It is
   visible to spectators, demos and any client-side recording, and it is what
   most "silent" features actually do.
2. **Server-side / packet silent aim.** The shot packet claims a direction at
   the target while the client view never moves. This requires changing what
   the client sends, not what it displays.

This overlay is a **read-only DMA overlay**. It reads memory and never writes
to the game process, and it never touches the network path. That rules out
variant 2 by construction:

- The server validates shots against its own authoritative state: origin,
  direction, timestamp, weapon state, line of sight and (depending on the
  game) re-simulation. A shot whose reported direction does not match the
  client's reported look input is either rejected or reconciled.
- Producing a mismatched direction means modifying the outgoing packet or the
  client's aim state at send time. A read-only external reader cannot do that.
- Even a memory write to the camera/aim vector only changes what the client
  displays and sends on the next frame; the view still moves, so it is not
  silent, and the write itself is detectable.
- Packet spoofing from a second PC requires being inline on the network,
  dealing with transport encryption and session state, and is far outside the
  scope of a DMA overlay.

**Conclusion:** true silent aim is not achievable with a read-only DMA overlay.
The two modes above are closed-loop aim assistance: they move the real cursor
or a real HID device and therefore move the visible view. They are honest about
that in the UI ("Memory Aim" / "Hardware Aim", not "silent").

## Server-side checks to expect

These are the checks that make packet-level silent aim impractical and that
also shape how aggressive the smoothing can be before shots look implausible:

- Reported look direction vs. shot direction consistency.
- Fire-rate, ammo and weapon-state authority.
- Line-of-sight / obstruction re-checks.
- Latency and timestamp sanity for the shot.
- Statistical aim-angle and snap-speed analysis over a session.

The smoothing slider exists partly for this reason: an instant snap is both
visually obvious and statistically anomalous. It does not make the feature
undetectable, and nothing here should be presented as such.
