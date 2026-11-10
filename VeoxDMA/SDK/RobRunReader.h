#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "RobRunSDK.h"
#include "Matrix4x4.h"
#include "RobRunCamera.h"
#include "Inventory.h"
#include "WorldEntities.h"
#include "SkeletonExtractor.h"

namespace RobRunReader {

struct EntityData {
    std::int32_t entityId = -1;
    Vector3 position{};
    Vector3 lookDirection{};
    float pitch = 0.f;
    float yaw = 0.f;
    float health = -1.f;
    float maxHealth = -1.f;
    bool healthKnown = false;
    bool isDead = false;
    bool isSprinting = false;
    bool isCrouching = false;
    bool inVehicle = false;
    int vehicleId = 0;
    int role = 0;
    float criticalHealth = -1.f;
    std::wstring nickname;
    std::wstring clanTag;
    std::uintptr_t viewPtr = 0;
    std::uintptr_t characterModel = 0;
    std::uintptr_t privateModel = 0;
    // Bone segments are extracted by the reader worker, never by the render
    // thread, so drawing only projects already-resolved 3D points.
    std::vector<Skeleton::BoneSegment> bones;
    // Named aim points sampled from the same pose as the skeleton.
    Skeleton::BonePoints bonePoints;
};

// Immutable view of the last completed read pass. The worker publishes a new
// snapshot every cycle; the overlay thread only ever reads one. This is what
// keeps DMA latency off the render thread.
struct Snapshot {
    std::vector<EntityData> entities;
    RobRunWorld::State world;
    RobRunInventory::State inventory;
    RobRunCamera::CameraState camera;
    std::int32_t localEntityId = -1;
    Vector3 localPosition{};
    std::uintptr_t game = 0;
    std::uintptr_t battleModel = 0;
    std::uintptr_t localCharacter = 0;
    std::uintptr_t cameraModel = 0;
    std::uint32_t failureCount = 0;
    Matrix4x4 fallbackViewProjection{};
    bool fallbackViewProjectionValid = false;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

// Resets state, resolves the base modules and starts the read worker.
void Init(std::uintptr_t gameAssemblyBase, std::uintptr_t unityPlayerBase);

// Stops and joins the read worker. Safe to call more than once.
void Shutdown();

// Runs one read pass on the calling thread. Main uses the worker; this remains
// available for diagnostics.
void Update();

// Returns the last published snapshot. The shared_ptr keeps it alive while the
// caller uses it even if the worker publishes a newer one.
SnapshotPtr GetSnapshot();

std::int32_t GetLocalEntityId();
Vector3 GetLocalPosition();
std::uintptr_t GetGamePtr();
std::uintptr_t GetBattleModelPtr();
std::uintptr_t GetLocalCharacterPtr();
std::uintptr_t GetCameraModelPtr();
std::uint32_t GetFailureCount();

// Enables the one-off item-catalog class scan. Disabled by default because
// the scan is intentionally bounded but not free.
void SetReadCatalogEnabled(bool enabled);

// Re-reads only the BattleCameraModel with uncached DMA reads and updates
// a thread-safe camera state used by GetViewProjection. Call this from the
// render thread (e.g. the overlay tick callback) so camera projection stays
// in sync with the game frame instead of trailing the worker by ~33 ms.
void RefreshCamera();

// Builds the view-projection from the verified BattleCameraModel. Returns
// false when the camera path is unavailable; callers can then use
// GetFallbackViewProjection, which is computed once per worker cycle.
bool GetViewProjection(Matrix4x4& out, int screenWidth, int screenHeight);

// Native Unity camera matrices, resolved by the worker. Never performs a GOM
// walk on the render thread.
bool GetFallbackViewProjection(Matrix4x4& out);

Vector2 WorldToScreen(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight);

// Bool-returning projection. Unlike WorldToScreen, a valid result at screen
// (0,0) is not confused with a behind-camera failure.
bool ProjectWorld(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight, Vector2& out);

} // namespace RobRunReader
