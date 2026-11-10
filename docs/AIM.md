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
- FOV is 5-180 degrees. When the camera is valid the check is angular around
  the camera forward vector; otherwise it falls back to a screen-space radius
  derived from the camera's vertical FOV.
- The closest candidate wins, with a small hysteresis bonus for the previously
  selected entity to stop target flicker.
- The aim point is sampled from the animation pose: head, neck, chest or
  pelvis. If the rig cannot be sampled, a body-relative height above the
  transform root is used.
- Aim is suspended while the menu is open and whenever the activation key is
  not held.

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
