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
#include "CollisionBVH.h"

namespace RobRunReader {

struct EntityData {
    std::int32_t entityId = -1;
    // Battle slot public id (Steam/account id). Used by the party filter.
    std::uint64_t publicId = 0;
    // True when the reader matched this entity against the local party's
    // member ids. Unresolved party data leaves every entity false.
    bool isPartyMember = false;
    bool isClanMate = false;
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
    // Role name resolved through reflection ("Criminal", "Police Officer",
    // "Civilian", "Hubber"). Stored as text because the game's role ids are
    // hashed enums, not small indices.
    std::string roleName;
    float criticalHealth = -1.f;
    std::wstring nickname;
    std::wstring clanTag;
    std::wstring weaponName;
    // UTF-8 copies for the ESP/radar, built once per reader pass. Converting
    // per entity per rendered frame allocated several strings at 144 Hz.
    std::string nicknameUtf8;
    std::string clanTagUtf8;
    std::string weaponNameUtf8;
    int weaponIndex = -1;
    bool isFiring = false;
    bool isReloading = false;
    bool hasArmor = false;
    float armor = 0.f;
    float stamina = -1.f;
    std::int32_t bagValue = 0;
    std::int32_t bagCash = 0;
    std::int32_t bagItems = 0;
    std::int32_t equipValue = 0;
    bool isAimingAtLocal = false;
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
    // Per-bone world-space 4x4 matrices for mesh-based chams skinning.
    std::vector<Matrix4x4> boneWorldMatrices;
    std::vector<int> boneMap;
    int boneCount = 0;
};

// Live state of one in-game map viewport (top-left minimap or fullscreen 'M'
// map), read directly from BattleModel.MapConfigModel.
struct InGameMapVariance {
    bool valid = false;
    Vector2 position{};    // World (X, Z) center of the orthographic map camera
    float rotation = 0.f;  // Live map camera rotation
    float cameraSize = 0.f; // Orthographic half-height in world metres
    bool rectValid = false;
    // Screen-space bounding box in Unity UI pixels (Y=0 at bottom) and
    // transform center/scale so the overlay converts with current display height.
    float uiMinX = 0.f, uiMinY = 0.f, uiMaxX = 0.f, uiMaxY = 0.f;
    float uiCenterX = 0.f, uiCenterY = 0.f;
    float uiScaleX = 1.f, uiScaleY = 1.f;
};

struct InGameMapState {
    bool valid = false;
    bool fullscreenOpen = false;
    float normalizedZoom = 0.f;
    InGameMapVariance minimap;
    InGameMapVariance fullscreen;
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
    std::shared_ptr<const RobRunWorld::PlotState> plots;
    std::shared_ptr<const RobRunInventory::State> inventory;
    RobRunCamera::CameraState camera;
    InGameMapState inGameMap;
    std::int32_t localEntityId = -1;
    Vector3 localPosition{};
    float localYaw = 0.f;
    float localPitch = 0.f;
    int observerCount = 0;
    bool spectatorAlert = false;
    std::uintptr_t game = 0;
    std::uintptr_t battleModel = 0;
    std::uintptr_t localCharacter = 0;
    std::uintptr_t cameraModel = 0;
    std::uintptr_t lookDirectionModel = 0;
    std::uintptr_t lookDirDirect = 0;
    std::uintptr_t playerRotationInput = 0;
    std::uintptr_t battleEntities = 0;
    std::uintptr_t battleEntitiesUpdater = 0;
    std::uint32_t failureCount = 0;
    // Coverage diagnostics: how many players actually produced drawable bones
    // or a usable named aim pose this pass. These make "some skeletons do not
    // render" measurable from the menu instead of guessed.
    std::uint32_t skeletonCount = 0;
    std::uint32_t poseCount = 0;
    // Players whose fresh pose read failed on the last pass. They keep their
    // previous bones instead of accepting a cached (visibly trailing) pose.
    std::uint32_t poseFailures = 0;
    // Bone jitter diagnostics: per-pass aggregate across all entities.
    std::uint32_t boneTotal = 0;        // total bones evaluated
    std::uint32_t boneInvalid = 0;      // failed TRS validation
    std::uint32_t boneNormZero = 0;     // quaternion norm ~0 (unread/zeroed)
    std::uint32_t boneNormBad = 0;      // quaternion norm outside [0.5,1.5] but > 0
    std::uint32_t bonePosInf = 0;       // non-finite position
    std::uint32_t boneChainDrop = 0;    // valid TRS but parent invalid (chain break)
    std::uint32_t poseScatterOk = 0;    // ProcessPoseData succeeded
    std::uint32_t poseScatterFail = 0;  // ProcessPoseData failed, tried fresh
    std::uint32_t poseFreshOk = 0;      // SampleLayout fallback succeeded
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
float GetLocalYaw();
std::uintptr_t GetGamePtr();
std::uintptr_t GetBattleModelPtr();
std::uintptr_t GetLocalCharacterPtr();
std::uintptr_t GetCameraModelPtr();
std::uint32_t GetFailureCount();
std::string GetLocationId();

// Enables the one-off item-catalog class scan. Disabled by default because
// the scan is intentionally bounded but not free.
void SetReadCatalogEnabled(bool enabled);

// Re-reads only the BattleCameraModel with uncached DMA reads and updates
// a thread-safe camera state used by GetViewProjection. Call this from the
// render thread (e.g. the overlay tick callback) so camera projection stays
// in sync with the game frame instead of trailing the worker by ~33 ms.
void RefreshCamera();

// Returns the camera written by the most recent RefreshCamera() call (or the
// worker's copy when the render path has not run yet). Aim uses this instead of
// snapshot->camera so its angles are solved from the same camera the on-screen
// projection was built with; the worker snapshot trails by up to one cycle.
bool GetFreshCamera(RobRunCamera::CameraState& out);

// Builds the view-projection from the verified BattleCameraModel. Returns
// false when the camera path is unavailable; callers can then use
// GetFallbackViewProjection, which is computed once per worker cycle.
bool GetViewProjection(Matrix4x4& out, int screenWidth, int screenHeight);

// Native Unity camera matrices, resolved by the worker. Never performs a GOM
// walk on the render thread.
bool GetFallbackViewProjection(Matrix4x4& out);

// Single resolution path shared by the ESP, the aim cone and the debug HUD: the
// verified managed camera first, the native Unity camera fallback second, so
// every consumer projects with the same answer.
bool ResolveViewProjection(Matrix4x4& out, int screenWidth, int screenHeight);

// Latches the newest view-projection matrix once at the start of a render
// pass so every draw callback in that frame (players, world, BVH, aim) shares
// an identical camera timestamp without re-multiplying matrices.
void BeginRenderFrame(int screenWidth, int screenHeight);
void EndRenderFrame();

// Lightweight render-thread feedback telling the worker whether any enemy is
// currently projected on screen and how close the nearest on-screen enemy is.
// Used to schedule manual VMM Mem/TLB cache refreshes when nobody is in sight
// or everyone is far away, avoiding mid-fight stutters under -norefresh.
void ReportRenderState(bool anyOnScreen, float nearestOnScreenDist);

// Single role-label resolver. Role ids are hashed enums, so names come from the
// reflection pass and the known-id fallbacks live here; the aim filter and the
// ESP must both go through it so they cannot disagree about a target.
std::string RoleLabel(int role, const std::string& name);

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
    std::string path;
};
BvhStatus GetBvhStatus();
void ReloadBvh();
const CollisionBVH::Scene& GetBvhScene();

bool IsOccluded(const Vector3& origin, const Vector3& target, float tMin = 0.20f, bool checkDynamic = true);
bool IsCameraOccluded(const Vector3& target, float tMin = 0.20f, bool checkDynamic = true);
CollisionBVH::DynamicOccluders GetDynamicOccluders();

} // namespace RobRunReader
