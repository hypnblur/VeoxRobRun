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
    std::wstring weaponName;
    int weaponIndex = -1;
    bool isFiring = false;
    bool isReloading = false;
    bool hasArmor = false;
    bool isKnockedDown = false;
    bool isKnockedOut = false;
    bool isSurrendered = false;
    bool isHandcuffed = false;
    std::string reloadPhase;
    // Aggregated animator/cull state for this character. All skeleton segments
    // share it, so it doubles as the "visible bones only" aim gate. This is an
    // animator check, not wall occlusion.
    bool visible = true;
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
    // World/inventory reads walk dozens of dictionaries and can take hundreds
    // of milliseconds. They run on their own thread and publish immutable
    // states, so the player/pose pass never waits for them.
    std::shared_ptr<const RobRunWorld::State> world;
    std::shared_ptr<const RobRunInventory::State> inventory;
    RobRunCamera::CameraState camera;
    std::int32_t localEntityId = -1;
    Vector3 localPosition{};
    std::uintptr_t game = 0;
    std::uintptr_t battleModel = 0;
    std::uintptr_t localCharacter = 0;
    std::uintptr_t cameraModel = 0;
    std::uintptr_t lookDirectionModel = 0;
    std::uintptr_t lookDirDirect = 0;
    std::uintptr_t playerRotationInput = 0;
    std::uint32_t failureCount = 0;
    // Coverage diagnostics: how many players actually produced drawable bones
    // or a usable named aim pose this pass. These make "some skeletons do not
    // render" measurable from the menu instead of guessed.
    std::uint32_t skeletonCount = 0;
    std::uint32_t poseCount = 0;
    // Players whose fresh pose read failed on the last pass. They keep their
    // previous bones instead of accepting a cached (visibly trailing) pose.
    std::uint32_t poseFailures = 0;
    Matrix4x4 fallbackViewProjection{};
    bool fallbackViewProjectionValid = false;
    // Publish time (GetTickCount64). Lets the overlay report how old the pose
    // data it is drawing actually is.
    std::uint64_t stampMs = 0;
    // Publish time in steady_clock microseconds. GetTickCount64 only ticks at
    // ~15.6 ms, which is the same order as the whole pose interval, so the
    // render thread cannot interpolate from it. This stamp is high-resolution.
    std::uint64_t stampUs = 0;
    // Monotonic id that changes whenever a scene/session root changes (game,
    // BattleModel, BattleEntitiesModel, updater, local character or local
    // entity id). Memory aim uses this to stop writing while the level is
    // being torn down and rebuilt instead of scribbling over freed objects.
    std::uint64_t sessionGeneration = 0;
    // Time the generation last changed (GetTickCount64).
    std::uint64_t sessionChangedMs = 0;
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

// Which camera GetViewProjection is currently projecting with: "native" (the
// Unity render camera), "managed" (BattleCameraModel), "snapshot" (worker
// copy) or "none". Useful for confirming the ESP is glued to the rendered frame
// while memory aim rewrites the managed camera block.
const char* GetViewProjectionSource();

// Native-camera identification diagnostics: whether the Unity render camera was
// found and how closely it agrees with the managed camera (position error in
// metres, forward dot product). Negative values mean "not measured".
struct CameraDebug {
    bool nativeEnabled = true;
    bool nativeValid = false;
    float positionError = -1.f;
    float forwardDot = -1.f;
    // Age of the worker snapshot the ESP is drawing from, in milliseconds.
    // Negative when no snapshot has been published yet.
    std::int64_t snapshotAgeMs = -1;
};
CameraDebug GetCameraDebug();

Vector2 WorldToScreen(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight);

// Bool-returning projection. Unlike WorldToScreen, a valid result at screen
// (0,0) is not confused with a behind-camera failure.
bool ProjectWorld(const Matrix4x4& viewProjection, const Vector3& world,
    int screenWidth, int screenHeight, Vector2& out);

struct BvhStatus {
    bool loaded = false;
    uint32_t nodeCount = 0;
    uint32_t triCount = 0;
};
BvhStatus GetBvhStatus();

} // namespace RobRunReader
