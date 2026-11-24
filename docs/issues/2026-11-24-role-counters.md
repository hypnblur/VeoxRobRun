# Feature: Live role counters (Police / Civilian / Criminal) on the full map and web radar

**Labels:** `enhancement`, `radar`, `webradar`, `ui`
**Area:** `VeoxDMA/Esp/Radar.cpp`, `VeoxDMA/Esp/WebRadar.cpp`, `VeoxDMA/Esp/WebRadarHtml.h`, `VeoxDMA/Config/RadarConfig.h`, `VeoxDMA/UI/VeoxMenu.cpp`

---

## Summary

Add a compact counter strip that shows how many **police**, **civilian**, and **criminal** players are currently present. It must render:

1. On the in-game **fullscreen map opened with `M`** (overlay drawn by `Radar::DrawInGameMapOverlay()` when `snapshot->inGameMap.fullscreenOpen` is true).
2. On the **web radar** page served by `WebRadar`, in the top bar area.

Exact 1:1 accuracy is not required. Being spot-on matters, but **±1 (worst case ±2) is acceptable** — a player who joined this pass may be missing, a role label may resolve one pass late. Do not add extra DMA reads to fix that; one consistent source of truth is better than two disagreeing ones.

---

## Current state / anchors

| What | Where |
|---|---|
| Player snapshot + `role` / `roleName` / `IsTeammate()` | `SDK/RobRunReader.h` (`EntityData`, `Snapshot`) |
| Role label normalization | `SDK/RobRunReader.cpp` → `RoleLabel(int role, const std::string& name)` (~line 6153). Returns the reflected game name when present (`"Police Officer"`, `"Criminal"`, `"Civilian"`, `"Hubber"`), else a fallback for hashed ids. |
| Full map / minimap overlay draw | `Esp/Radar.cpp` → `DrawInGameMapOverlay()` (~line 1737). `isFull` = `M` map open. Canvas→screen rect conversion already handled there (`minX/maxX/minY/maxY`). |
| Web radar JSON state | `Esp/WebRadar.cpp` → `BuildStateJson()` (~line 283). Adds `players`, `vehicles`, `bounds`, `defaults`, `inGameMap`, `local`. |
| Web radar page markup/CSS/JS | `Esp/WebRadarHtml.h` |
| Radar settings + JSON persistence | `Config/RadarConfig.h` |
| Radar settings UI | `UI/VeoxMenu.cpp` (~line 2992–3020, "Fullscreen Map (M)" block) |

---

## Requirements

- [ ] **R1 — Classification helper.** Add one shared, dependency-free classifier used by both surfaces, e.g. `Esp/RoleCounts.h`:
  - `enum class RoleBucket { Police, Civilian, Criminal, Unknown };`
  - `RoleBucket ClassifyRole(int role, const std::string& roleName);`
  - `struct RoleCounts { int police, civilian, criminal, unknown, total; };`
  - `RoleCounts CountRoles(const Snapshot& snap);`
  - Matching rules: case-insensitive, substring based — **not** exact equality (existing code compares `== "Police Officer"`, which is brittle when the reflected label is localized or suffixed):
    - `police`, `cop`, `officer`, `law` → Police
    - `criminal`, `gang`, `mafia`, `cartel`, `convict` → Criminal
    - `civilian`, `citizen`, `hubber`, `worker` → Civilian
    - empty / `Role:<id>` fallback / unresolved → Unknown
- [ ] **R2 — Counting rules.**
  - Skip the local player (`entityId == snap.localEntityId`).
  - Skip dead players (`isDead`). Keep knocked-out / handcuffed players counted — they are still on the server.
  - Count everyone else in `snap.entities`, regardless of `IsTeammate()`, `visible`, or occlusion. This is a population count, not an ESP draw list.
  - `total` = police + civilian + criminal + unknown.
- [ ] **R3 — Full map (`M`) strip.** Draw a single horizontal strip near the **top center** of the map rect already computed in `DrawInGameMapOverlay()` (inset from `minX/maxX/minY` by ~8 px so it never clips). Must:
  - Draw **after** map markers so it sits on top; clip/ignore if the strip would exceed `rectW`.
  - Respect `InGameMapScale` for font/padding size.
  - Layout per entry: colored dot + label + count, e.g. `● POL 4   ● CIV 12   ● CRM 7`.
  - Only render on the fullscreen map by default; minimap stays clean (`RoleCountersOnMinimap`, default `false`).
  - Render `Unknown` only when `> 0`, dimmed (`alpha ~110`).
  - No per-frame `std::string` churn: format into a fixed `char[128]` with `snprintf`, cache the built text against `snapshot->stampUs` so it rebuilds once per reader pass, not once per frame.
- [ ] **R4 — Web radar strip.** In `BuildStateJson()` add:
  ```json
  "roleCounts": { "police": 4, "civilian": 12, "criminal": 7, "unknown": 0, "total": 23 }
  ```
  In `WebRadarHtml.h`, render a matching top strip (dot + label + number per role), styled from the existing page tokens and driven by `accent`/`accentBright` where sensible. It must update on the normal polling tick with no layout shift between updates (fixed-width numbers, tabular-nums).
- [ ] **R5 — Config keys** in `RadarConfig.h`, wired through `ToJson()` **and** `FromJson()`:
  - `RoleCounters` (bool, default `true`)
  - `RoleCountersOnFullMap` (bool, default `true`)
  - `RoleCountersOnMinimap` (bool, default `false`)
  - `RoleCountersOnWebRadar` (bool, default `true`)
  - `RoleCounterPoliceColour`, `RoleCounterCivilianColour`, `RoleCounterCriminalColour` (`Colour`, defaults blue `#4A90E2`, light gray `#D8D8D8`, red `#E04A4A` — deliberately not the teammate green)
- [ ] **R6 — Menu rows.** In the radar tab next to "Fullscreen Map (M)": master toggle `Role Counters`, then the three surface toggles and three colour pickers. Follow the existing `ToggleRow` / colour-picker helper style in `VeoxMenu.cpp`.
- [ ] **R7 — Derived counts exposed once.** If cheap, also publish `snap.localRoleName`-independent counts through a `RobRunReader::GetRoleCounts()`-style accessor used by both `Radar` and `WebRadar`, so the two surfaces can never disagree.
- [ ] **R8 — No new DMA traffic.** Counts must be derived from the already-published `Snapshot`. Do not add scatter reads, do not touch the aim worker, do not delay the pose pass.

---

## Accuracy contract

Counts may be off by one, max two, during transitions. Accepted sources of drift:

- Player entered the entity list mid-pass → appears next pass.
- `roleName` reflection resolved a pass late → counted as `Unknown` briefly.
- A fresh pose read failed → entity keeps stale bones but is still counted (we count entities, not bones).
- Spectators/observers are not players and must not be counted.

Anything worse than ±2 steady-state is a bug, not a tolerance.

---

## Files to touch

```
VeoxDMA/Esp/RoleCounts.h          (new — classification + counting, header-only)
VeoxDMA/Esp/Radar.cpp             (DrawInGameMapOverlay: top strip)
VeoxDMA/Esp/WebRadar.cpp          (BuildStateJson: roleCounts payload)
VeoxDMA/Esp/WebRadarHtml.h        (top strip markup + CSS + JS update)
VeoxDMA/Config/RadarConfig.h      (settings + JSON round-trip)
VeoxDMA/UI/VeoxMenu.cpp           (toggles + colour pickers)
```

## Acceptance criteria

- [ ] Pressing `M` in-game shows `POL / CIV / CRM` counts at the top of the map, tinted with the configured colours.
- [ ] `http://127.0.0.1:<WebRadarPort>/` shows the same numbers in its top strip, within one poll tick of the overlay.
- [ ] Numbers change live as players join, die, or change role; steady-state disagreement between overlay and web radar is impossible (same source).
- [ ] Toggles and colour pickers persist across restart via the radar config JSON.
- [ ] All counters off → zero visual change, zero added allocations.
- [ ] `Release|x64` builds clean with MSVC v143/v145, `/std:c++20`, no new warnings.
- [ ] Render thread performs no heap allocation in the counter path.

## Out of scope

- Per-role ESP/aim filtering, team assignment UI, and teammate green styling (tracked separately).
- Historical graphs, per-zone counts, or role counts for the standalone popout radar window (nice-to-have follow-up).

---

## Agent brief (copy-paste)

> Implement the role counter feature described in this issue in the VeoxDMA C++20 codebase.
> Start by reading `Esp/RoleCounts.h` (create it), then `Esp/Radar.cpp::DrawInGameMapOverlay`, `Esp/WebRadar.cpp::BuildStateJson`, `Esp/WebRadarHtml.h`, `Config/RadarConfig.h`, and the radar section of `UI/VeoxMenu.cpp`.
> Reuse the existing canvas→screen rect math in `DrawInGameMapOverlay` — do not recompute map geometry.
> Keep classification substring/case-insensitive and centralized; both surfaces must call the same helper.
> Cache the formatted overlay string by `snapshot->stampUs`; no per-frame strings, no new DMA reads.
> Verify with a Release x64 build and a live `M`-map + browser check; counts must match within ±2 at all times.
