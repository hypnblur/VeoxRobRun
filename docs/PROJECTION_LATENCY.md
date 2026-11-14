# Projection Latency and Delay Audit

## Summary of Findings

The visual projection overlay lags behind rendered 3D player models due to three compounding temporal desynchronization layers in the architecture:

---

### 1. Exponential Moving Average Filter Delay (`PoseSmoothing`)

- **Location**: `VeoxDMA/SDK/RobRunReader.cpp` (Lines 2455–2525) & `VeoxDMA/Config/PlayerConfig.h` (Lines 69–70)
- **Mechanism**:
  ```cpp
  const float alpha = std::clamp(ConfigInstance.Player.PoseSmoothAlpha, 0.1f, 1.0f);
  const float inv = 1.0f - alpha;
  s.a.x = s.a.x * inv + r.a.x * alpha;
  ```
  `PoseSmoothing` defaults to `true` with `PoseSmoothAlpha = 0.5f`.
  For a first-order exponential smoothing filter sampled at $\Delta t \approx 33\text{ ms}$ (30 Hz worker loop), the group delay is:
  $$\tau = \Delta t \cdot \frac{1 - \alpha}{\alpha} = 33\text{ ms} \cdot \frac{1 - 0.5}{0.5} = 33\text{ ms}$$
  Lower alpha values (e.g. 0.3) increase filter delay to $77\text{ ms}$.
- **Flawed Velocity Bypass**:
  ```cpp
  const bool isFastMoving = entity.inVehicle || (Distance(rig.lastSampledPos, entity.position) > 0.4f);
  ```
  A character running at normal sprint speed ($5.0\text{ m/s}$) travels only $0.165\text{ m}$ per $33\text{ ms}$ cycle. Because $0.165\text{ m} < 0.4\text{ m}$, the bypass condition is never met during standard on-foot sprint motion. The exponential filter continuously lags $33\text{ ms}$ to $50\text{ ms}$ behind the character's true bone positions.
- **Physical Displacement**:
  At $5.5\text{ m/s}$ sprint speed, a $33\text{ ms}$ filter lag accounts for $0.18\text{ m}$ of trailing error directly along the movement vector.

---

### 2. Temporal Desynchronization: Live View-Projection vs. Stale Entity Poses

- **Location**: `VeoxDMA/Aim/AimController.cpp` (Line 533) vs `VeoxDMA/Esp/PlayerEsp.cpp` (Lines 41–47)
- **Mechanism**:
  - The targeting loop runs `RefreshCamera()` inside a dedicated thread at $\approx 500\text{ Hz}$ ($2\text{ ms}$ interval), continuously refreshing `g_freshCamera` to $T_{\text{current}}$.
  - The pose reader worker runs `Update()` at $\approx 30\text{ Hz}$, publishing snapshots containing entity bone coordinates recorded at $T_{\text{snapshot}} \approx T_{\text{current}} - (25\text{ to }50\text{ ms})$.
  - During rendering in `DrawPlayers()`:
    ```cpp
    const auto snapshot = RobRunReader::GetSnapshot(); // Stale world positions (30-50ms old)
    Matrix4x4 viewProjection{};
    RobRunReader::ResolveViewProjection(viewProjection, screenW, screenH); // Live camera (2ms old)
    ```
- **Consequence**:
  Projecting world coordinates from $35\text{ ms}$ in the past through a view-projection matrix from the present moment projects the target's past position onto the modern screen coordinates. When the target runs perpendicularly across the viewport, this creates an apparent screen offset:
  $$\Delta X_{\text{screen}} \approx \text{Projection}\left(\vec{P}_{\text{past}}, \mathbf{M}_{\text{current}}\right) - \text{Projection}\left(\vec{P}_{\text{current}}, \mathbf{M}_{\text{current}}\right)$$
  When the local observer also moves, the camera translation compounds the error.

---

### 3. Worker Loop Polling Interval (`Sleep(10)`)

- **Location**: `VeoxDMA/SDK/RobRunReader.cpp` (Lines 648–656)
- **Mechanism**:
  ```cpp
  while (!g_workerStop.load(std::memory_order_relaxed)) {
      Update();
      for (int i = 0; i < 10 && !g_workerStop.load(std::memory_order_relaxed); ++i)
          Sleep(1);
  }
  ```
  Each `UpdateInternal()` pass takes $\approx 15\text{ to }22\text{ ms}$ to execute scatter reads for transforms, bone hierarchies, and BVH occlusion tests. Adding an unconditional $10\text{ ms}$ sleep limits the worker loop rate to $30\text{ to }35\text{ Hz}$, capping the snapshot freshness and guaranteeing that published data is on average $15\text{ to }35\text{ ms}$ stale before the render thread receives it.

---

### 4. Cumulative Latency Budget

| Component | Latency Added | Displacement at 5.5 m/s |
|---|---|---|
| Hardware Memory Transport & Scatter Execution | $12 - 20\text{ ms}$ | $0.07 - 0.11\text{ m}$ |
| Worker Thread Cycle Delay ($10\text{ ms}$ sleep + frame phase) | $15 - 30\text{ ms}$ | $0.08 - 0.16\text{ m}$ |
| EMA Pose Smoothing Filter ($\alpha = 0.5$) | $33 - 40\text{ ms}$ | $0.18 - 0.22\text{ m}$ |
| **Total Pipeline Delay** | **$60 - 90\text{ ms}$** | **$0.33 - 0.50\text{ m}$** |

At $0.33\text{ to }0.50\text{ m}$ of lag, the projected skeleton trails behind a running character model by approximately the full width of a human torso.

---

## Remediation Architecture

### Fix A: Disable or Reconfigure Pose Smoothing
1. Set `ConfigInstance.Player.PoseSmoothing = false` or adjust `ConfigInstance.Player.PoseSmoothAlpha = 1.0f` to bypass the exponential filter and draw raw sampled points.
2. If smoothing is retained for stationary characters, drop the velocity bypass threshold from $0.4\text{ m}$ down to $0.05\text{ m}$ per pass so that any running character instantly bypasses smoothing.

### Fix B: Snapshot-Synchronized Camera Projection
In `PlayerEsp.cpp`:
Allow projection using the camera state captured at the same instant as the entity snapshot (`snapshot->camera`), or interpolate entity positions forward to match `g_freshCamera` timestamp:
$$\vec{P}_{\text{extrapolated}} = \vec{P}_{\text{snapshot}} + \vec{V}_{\text{entity}} \cdot (T_{\text{render}} - T_{\text{snapshot}})$$

### Fix C: Worker Rate Optimization
Reduce the fixed $10\text{ ms}$ sleep in `g_workerThread` to `Sleep(0)` or `Sleep(1)` when target entities are active in the scene, increasing snapshot publication rate from $30\text{ Hz}$ to $55 - 65\text{ Hz}$ and cutting snapshot latency in half.
