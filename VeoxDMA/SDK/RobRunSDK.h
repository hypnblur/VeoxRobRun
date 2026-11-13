#pragma once
#include <cstdint>

// ============================================================
// ROB RUN - Steam 4262460 - Build 26549963
//
// Canonical memory reference for the supported build. Every offset in this
// header is tied to the PE fingerprint below. If the game updates, re-verify
// the fingerprint first and then re-check the affected path.
//
// Sections marked "live" were read successfully from the running build.
// Sections marked "metadata" are declaration-derived and are only used behind
// defensive validation (pointer/class/field checks and finite-value checks).
// ============================================================

namespace rr {

inline constexpr const char* kBuild = "26549963";
inline constexpr std::uint32_t kPeTimestamp = 0x6AF2C76Fu;
inline constexpr std::uint32_t kPeSizeOfImage = 0x08FC9000u;
inline constexpr const char* kGameAssemblySha256 =
    "ecd0a4275325fce38c9794576c541891cb8dfaa0b0ac3a38fdd2dc9711ecb81e";
inline constexpr std::uint32_t kUnityPlayerPeTimestamp = 0x6A28669Fu;
inline constexpr std::uint32_t kUnityPlayerPeSizeOfImage = 0x02426000u;
inline constexpr const wchar_t* kProcess = L"ROB RUN.exe";
inline constexpr const wchar_t* kModule = L"GameAssembly.dll";
inline constexpr const wchar_t* kUnityModule = L"UnityPlayer.dll";

// ------------------------------------------------------------------
// Module-relative metadata slots (live verified)
// ------------------------------------------------------------------
namespace ClassTable {
inline constexpr std::uintptr_t Slot = 0x8625440;
inline constexpr std::uintptr_t MetadataHeaderSlot = 0x8625550;
}

// Loaded class indices are capture-specific. They are useful as a fast path,
// but every resolved pointer must still be validated by class identity.
namespace ClassIndex {
inline constexpr int BattleCharacterModel       = 4309;
inline constexpr int CharacterTransformModel    = 3426;
inline constexpr int BattleEntitiesModel        = 5510;
inline constexpr int BattleEntitiesUpdater      = 5517;
inline constexpr int BattleModel                = 5520;
inline constexpr int BattleCameraModel          = 7868;
inline constexpr int PlayerEntityModel          = 8692;
inline constexpr int BattleCharacterEntityModel = 9036;
inline constexpr int GameGame                   = 29335;
inline constexpr int GameGameModel              = 29336;
inline constexpr int CinemachineBrain           = 29521;
inline constexpr int CinemachineCore            = 29618;
}

// ------------------------------------------------------------------
// Il2Cpp object / class / field metadata (live verified)
// ------------------------------------------------------------------
namespace Object {
inline constexpr std::uintptr_t klass         = 0x00; // Il2CppClass*
inline constexpr std::uintptr_t monitor       = 0x08;
inline constexpr std::uintptr_t m_CachedPtr   = 0x10; // UnityEngine.Object -> native
}

namespace Klass {
inline constexpr std::uintptr_t name_ptr      = 0x10; // char*
inline constexpr std::uintptr_t namespace_ptr = 0x18; // char*
inline constexpr std::uintptr_t parent        = 0x58; // Il2CppClass*
inline constexpr std::uintptr_t fields        = 0x80; // FieldInfo*
inline constexpr std::uintptr_t static_fields = 0xB8;
inline constexpr std::uintptr_t instance_size = 0xF8; // uint32
inline constexpr std::uintptr_t flags         = 0x118; // uint32
inline constexpr std::uintptr_t field_count   = 0x124; // uint16
}

namespace FieldInfo {
inline constexpr std::uintptr_t stride         = 0x20;
inline constexpr std::uintptr_t name           = 0x00; // char*
inline constexpr std::uintptr_t type           = 0x08; // Il2CppType*
inline constexpr std::uintptr_t parent         = 0x10; // Il2CppClass*
inline constexpr std::uintptr_t offset         = 0x18; // int32
}

namespace Il2CppType {
inline constexpr std::uintptr_t code = 0x0A; // type-code byte
inline constexpr std::uint8_t CodeBoolean        = 0x02;
inline constexpr std::uint8_t CodeClass          = 0x12;
inline constexpr std::uint8_t CodeGenericInstance = 0x15;
inline constexpr std::uint8_t CodeObject         = 0x1C;
}

// ------------------------------------------------------------------
// Managed collections (live verified)
// ------------------------------------------------------------------
namespace Il2CppString {
inline constexpr std::uintptr_t length = 0x10; // int32 UTF-16 code units
inline constexpr std::uintptr_t chars  = 0x14; // UTF-16 data, not a char*
}

namespace Array {
inline constexpr std::uintptr_t max_length = 0x18; // uint64 length
inline constexpr std::uintptr_t vector     = 0x20; // element data
inline constexpr std::uintptr_t stride     = 0x08; // reference array stride
}

namespace List {
inline constexpr std::uintptr_t items = 0x10; // backing array pointer
inline constexpr std::uintptr_t count = 0x18; // int32 count
}

namespace Dict {
inline constexpr std::uintptr_t entries    = 0x18; // entries array pointer
inline constexpr std::uintptr_t count      = 0x20; // int32 count incl. removed
inline constexpr std::uintptr_t freeList   = 0x24;
inline constexpr std::uintptr_t freeCount  = 0x28;
inline constexpr std::uintptr_t version    = 0x2C;

// Dictionary<int, reference>: hash, next, key, pad, value.
inline constexpr std::uintptr_t entry_stride = 0x18;
inline constexpr std::uintptr_t entry_hash   = 0x00;
inline constexpr std::uintptr_t entry_next   = 0x04;
inline constexpr std::uintptr_t entry_key    = 0x08;
inline constexpr std::uintptr_t entry_value  = 0x10;

// Dictionary<reference, reference>: hash, next, key(8), value(8).
inline constexpr std::uintptr_t ref_entry_key   = 0x08;
inline constexpr std::uintptr_t ref_entry_value = 0x10;

// Allocated entry predicate used by this runtime: next >= -1.
inline constexpr std::int32_t entry_allocated_min = -1;
}

// ------------------------------------------------------------------
// Game and entity roots (live verified)
// ------------------------------------------------------------------
namespace Game {
inline constexpr std::uintptr_t ActiveUpdaters = 0x20; // List<reference>
inline constexpr std::uintptr_t GameModel      = 0xD0;
}

namespace GameModel {
inline constexpr std::uintptr_t BattleModel = 0x10;
}

namespace BattleModel {
inline constexpr std::uintptr_t BattleCharacterModel     = 0x20;
inline constexpr std::uintptr_t PlayerRotationInput      = 0x40;
inline constexpr std::uintptr_t EntityModelDict          = 0x50;
inline constexpr std::uintptr_t BattleEntitiesModel      = 0x90;
inline constexpr std::uintptr_t PlayerEntityModel        = 0xD0;
inline constexpr std::uintptr_t BattleCameraModel        = 0x358;

inline constexpr std::uintptr_t WorldEntitiesModel       = 0x518;
inline constexpr std::uintptr_t WorldEntitiesModelAlt    = 0x60;

inline constexpr std::uintptr_t CharacterSlotsModel = 0x248;
inline constexpr std::uintptr_t CharacterSlotsModelLegacy = 0x4C8;
inline constexpr std::uintptr_t ProlongedInteractionDialog = 0x188;
inline constexpr std::uintptr_t CoinsModel                 = 0x4A8;
inline constexpr std::uintptr_t CryptoCoinsModel           = 0x4B0;
inline constexpr std::uintptr_t DelayedUseAction           = 0x610;
}

namespace PlayerRotationInputModel {
inline constexpr std::uintptr_t Yaw          = 0x10;
inline constexpr std::uintptr_t Pitch        = 0x14;
inline constexpr std::uintptr_t RotationQuat = 0x1C;
}

namespace BattleEntitiesModel {
inline constexpr std::uintptr_t EntityDict             = 0x50; // Dictionary<int, entity>
inline constexpr std::uintptr_t WorldEntitiesModel     = 0x60; // new offset set
inline constexpr std::uintptr_t CharacterSlotsRoot     = 0x80; // legacy fallback
inline constexpr std::uintptr_t LocalPlayerEntityModel = 0xD0;
inline constexpr std::uintptr_t Player                 = 0xD0; // new offset set alias
}

namespace CharacterSlotsRoot {
inline constexpr std::uintptr_t SlotDict = 0x18; // Dictionary<int, slot>
}

namespace CharacterSlotsModel {
inline constexpr std::uintptr_t SlotDict = 0x18; // new offset set
inline constexpr std::uintptr_t SlotDictLegacy = 0x10;
}

namespace BattleCharacterSlotModel {
inline constexpr std::uintptr_t _data       = 0x10;
inline constexpr std::uintptr_t Description = 0x18;
}

namespace BattleCharacterSlotData {
inline constexpr std::uintptr_t EntityId = 0x20;
inline constexpr std::uintptr_t PublicId = 0x28;
inline constexpr std::uintptr_t Nickname = 0x30; // wrapper; string at +0x18
inline constexpr std::uintptr_t ClanTag  = 0x38; // wrapper; string at +0x18
inline constexpr std::uintptr_t wrapper_value = 0x18;
}

namespace PlayerEntityModel {
inline constexpr std::uintptr_t EntityId             = 0x10; // int32 local id
inline constexpr std::uintptr_t Client               = 0x18; // new offset set
inline constexpr std::uintptr_t Server               = 0x20; // new offset set
inline constexpr std::uintptr_t BattleCharacterModel = 0x20; // server-side character
inline constexpr std::uintptr_t PublicId             = 0x28; // int64
}

namespace BattleCharacterEntityModel {
inline constexpr std::uintptr_t Slots          = 0x18; // array
inline constexpr std::uintptr_t ClientModel    = 0x20; // new offset set
inline constexpr std::uintptr_t ServerModel    = 0x28; // new offset set
inline constexpr std::uintptr_t SimulatedModel = 0x30; // legacy path
inline constexpr std::uintptr_t BattleCharacterModel = 0x20; // alias for ClientModel
}

namespace BattleCharacterModel {
inline constexpr std::uintptr_t PrivateModel            = 0x10;
inline constexpr std::uintptr_t Id                      = 0x38;
inline constexpr std::uintptr_t Data                    = 0x40;
inline constexpr std::uintptr_t Config                  = 0x48;
inline constexpr std::uintptr_t BodyIdModel             = 0x50;
inline constexpr std::uintptr_t CharacterTransformModel = 0x58;
inline constexpr std::uintptr_t ReplicatedTransform     = 0x60;
inline constexpr std::uintptr_t LastPosition            = 0x68;
inline constexpr std::uintptr_t LookDirection           = 0x70;
inline constexpr std::uintptr_t ReplicatedLookDirection = 0x78;
inline constexpr std::uintptr_t LookDirectionInput      = 0xB0;
inline constexpr std::uintptr_t Velocity                = 0xB8;
inline constexpr std::uintptr_t IsDeadWrapper           = 0xF0;
inline constexpr std::uintptr_t HitBoxAccess            = 0x120;
inline constexpr std::uintptr_t HitBoxId                = 0x128;
inline constexpr std::uintptr_t SkeletonAnimation       = 0x130;
inline constexpr std::uintptr_t Skeleton                = 0x138;
inline constexpr std::uintptr_t Animator                = 0x140;
inline constexpr std::uintptr_t IsSprinting             = 0x148;
inline constexpr std::uintptr_t Weapon                  = 0x150;
inline constexpr std::uintptr_t WeaponReload            = 0x158;
inline constexpr std::uintptr_t IsBackgroundReload      = 0x200;
inline constexpr std::uintptr_t RobberInventoryCost     = 0x268;
inline constexpr std::uintptr_t Knockout                = 0x298;
inline constexpr std::uintptr_t ProlongedInteraction    = 0x340;
inline constexpr std::uintptr_t Crouch                  = 0x160;
inline constexpr std::uintptr_t ItemInHands             = 0x170;
inline constexpr std::uintptr_t CharacterSpeed          = 0x180;
inline constexpr std::uintptr_t HasArmor                = 0x1B8;
inline constexpr std::uintptr_t InVehicle               = 0x1C8;
inline constexpr std::uintptr_t State                   = 0x1E0;
inline constexpr std::uintptr_t PlayerRole              = 0x208;
inline constexpr std::uintptr_t Pbm                     = 0x258;
inline constexpr std::uintptr_t CenterPosition          = 0x290;
inline constexpr std::uintptr_t Revival                 = 0x2A0;
inline constexpr std::uintptr_t Knockdown               = 0x2D0;
inline constexpr std::uintptr_t CapsuleColliderParams   = 0x318;
}

namespace CharacterTransformModel {
inline constexpr std::uintptr_t Position = 0x10; // V3 + pitch/yaw/roll (radians)
inline constexpr std::uintptr_t X        = 0x10; // new offset set
inline constexpr std::uintptr_t Y        = 0x14; // new offset set
inline constexpr std::uintptr_t Z        = 0x18; // new offset set
inline constexpr std::uintptr_t Yaw      = 0x20; // new offset set
}

namespace BattleCharacterPrivateModel {
inline constexpr std::uintptr_t HealthModelAlt = 0x38;
inline constexpr std::uintptr_t Inventory      = 0x150;
inline constexpr std::uintptr_t HealthModel    = 0x1B0;
inline constexpr std::uintptr_t Aiming         = 0x1F0;
inline constexpr std::uintptr_t EquipSlots     = 0x158;
inline constexpr std::uintptr_t InitArmorModel = 0x168;
inline constexpr std::uintptr_t Pockets        = 0x170;
inline constexpr std::uintptr_t SprintModel    = 0x1D8;
inline constexpr std::uintptr_t CheatSprintEnabled          = 0x238;
inline constexpr std::uintptr_t CheatReduceIncomeDamage     = 0x240;
inline constexpr std::uintptr_t CheatReduceIncomeDetention  = 0x250;
inline constexpr std::uintptr_t InventoryUseItem            = 0x260;
inline constexpr std::uintptr_t InteractModel               = 0x288;
inline constexpr std::uintptr_t MoveSpeedModel              = 0x2C8;
inline constexpr std::uintptr_t Stamina                     = 0x2E8;

}

namespace HealthModel {
inline constexpr std::uintptr_t Health    = 0x10; // primitive wrapper
inline constexpr std::uintptr_t IsDead    = 0x18; // primitive wrapper
inline constexpr std::uintptr_t MaxAmount = 0x20; // int32
inline constexpr std::uintptr_t CriticalHealth = 0x24; // int32
}

// ------------------------------------------------------------------
// New offset set: character data, knockout and player-client fields
// ------------------------------------------------------------------
namespace BattleData {
inline constexpr std::uintptr_t Common = 0x20; // BattleCharacterModel+0x40 -> +0x20
}

namespace CommonModel {
inline constexpr std::uintptr_t Role = 0x20;
inline constexpr std::uintptr_t Position = 0x28;
inline constexpr std::uintptr_t LookDirection = 0x30;
inline constexpr std::uintptr_t IsFiring = 0x68;
inline constexpr std::uintptr_t IsSprinting = 0x88;
inline constexpr std::uintptr_t ReloadState = 0x90;
inline constexpr std::uintptr_t IsBackgroundReload = 0xA0;
inline constexpr std::uintptr_t IsCrouching = 0xA8;
inline constexpr std::uintptr_t IsDead = 0xE0;
inline constexpr std::uintptr_t VehicleEntityId = 0xF0;
}

namespace ReferenceData {
inline constexpr std::uintptr_t Value = 0x18;
}

namespace Description {
inline constexpr std::uintptr_t Name = 0x10;
}

namespace WeaponModel {
inline constexpr std::uintptr_t ItemInHands = 0x10;
}

namespace KnockoutModel {
inline constexpr std::uintptr_t Phase            = 0x18;
inline constexpr std::uintptr_t PhaseDescription = 0x18;
inline constexpr std::uintptr_t DescriptionId    = 0x10;
inline constexpr std::uintptr_t EndTs            = 0x10; // TimestampData (+0x18 i64)
inline constexpr std::uintptr_t IsSurrendered    = 0x70;
inline constexpr std::uintptr_t IsHandcuffing    = 0x71;
inline constexpr std::uintptr_t IsHandcuffed     = 0x72;
}

namespace PlayerClient {
inline constexpr std::uintptr_t BattleCameraModel = 0x258;
inline constexpr std::uintptr_t AimAssist  = 0x2A0;
inline constexpr std::uintptr_t AutoFire   = 0x2A8;
inline constexpr std::uintptr_t Recoil     = 0x2B8;
inline constexpr std::uintptr_t FakeSpread = 0x2C0;
inline constexpr std::uintptr_t InventoryProgressItem   = 0x1A8;
inline constexpr std::uintptr_t InventoryIntention      = 0x1B0;
inline constexpr std::uintptr_t AccelerationWeightSpeed = 0x258;
inline constexpr std::uintptr_t AutoLootAfterBreak      = 0x2B0;
}

namespace AimAssist {
inline constexpr std::uintptr_t IsActive   = 0x10;
inline constexpr std::uintptr_t HasTarget  = 0x12;
inline constexpr std::uintptr_t Correction = 0x28;
inline constexpr std::uintptr_t BlockTimer = 0x34;
}

namespace RecoilModel {
inline constexpr std::uintptr_t OffsetVec = 0x10;
}

namespace FakeSpreadModel {
inline constexpr std::uintptr_t Spread = 0x10;
inline constexpr std::uintptr_t Air    = 0x14;
inline constexpr std::uintptr_t Move   = 0x18;
}

namespace LookDirectionModel {
inline constexpr std::uintptr_t Value = 0x10;
inline constexpr std::uintptr_t Yaw   = 0x1C;
inline constexpr std::uintptr_t Pitch = 0x20;
}

namespace LookDirectionInput {
inline constexpr std::uintptr_t X        = 0x10;
inline constexpr std::uintptr_t Y        = 0x14;
inline constexpr std::uintptr_t Contains = 0x18;
}

namespace ReplicatedTransformModel {
inline constexpr std::uintptr_t PositionPrimitive    = 0x10;
inline constexpr std::uintptr_t OrientationPrimitive = 0x18;
}

namespace SkeletonConfig {
inline constexpr std::uintptr_t BonesCount     = 0x10;
inline constexpr std::uintptr_t BoneDataSize   = 0x14;
inline constexpr std::uintptr_t LocalPosOriOff = 0x18;
inline constexpr std::uintptr_t ModelOriOff    = 0x1C;
inline constexpr std::uintptr_t LengthOff      = 0x20;
inline constexpr std::uintptr_t HierarchyData  = 0x28;
inline constexpr std::uintptr_t RootIndex      = 0x34;
inline constexpr std::uintptr_t NeckIndex      = 0x44;
inline constexpr std::uintptr_t Head1Index     = 0x48;
}

namespace SkeletonModel {
inline constexpr std::uintptr_t Description = 0x10;
inline constexpr std::uintptr_t BonesData   = 0x18;
}

inline constexpr std::uint32_t BattlePlayerCapacity = 320;

// ------------------------------------------------------------------
// Active view updater (live verified)
// ------------------------------------------------------------------
namespace BattleEntitiesUpdater {
inline constexpr std::uintptr_t _battleModel          = 0x70;
inline constexpr std::uintptr_t _characterEntityViews = 0x110; // Dictionary<int, view>
}

// Updater list chain:
//   list  = Q(game + 0x20)
//   array = Q(list + 0x10); count = I32(list + 0x18)
//   a = Q(array + 0x20 + i*8)
//   b = Q(a + 0x50); c = Q(b + 0x18); updater = Q(c + 0x90)
namespace UpdaterChain {
inline constexpr std::uintptr_t a_step = 0x50;
inline constexpr std::uintptr_t b_step = 0x18;
inline constexpr std::uintptr_t c_step = 0x90;
}

// ------------------------------------------------------------------
// Character visual rig (live verified)
// ------------------------------------------------------------------
namespace BattleCharacterEntityView {
inline constexpr std::uintptr_t NativeObject = 0x10; // native Unity object
inline constexpr std::uintptr_t MovementView = 0x20;
inline constexpr std::uintptr_t AnimationView = 0x28;
inline constexpr std::uintptr_t WithWeaponView = 0x30;
inline constexpr std::uintptr_t HeadPosition = 0xE8;
}

namespace AnimationView {
inline constexpr std::uintptr_t HeadView  = 0x28;
inline constexpr std::uintptr_t TorsoView = 0x30;
}

namespace HeadView {
inline constexpr std::uintptr_t Neck  = 0x20;
inline constexpr std::uintptr_t Head  = 0x28;
inline constexpr std::uintptr_t Top   = 0x30; // optional, present in some views
}

namespace TorsoView {
inline constexpr std::uintptr_t Root       = 0x20; // at the feet, not the pelvis
inline constexpr std::uintptr_t LowerSpine = 0x28;
inline constexpr std::uintptr_t UpperSpine = 0x30;
}

namespace WithWeaponView {
inline constexpr std::uintptr_t LeftArm  = 0x58;
inline constexpr std::uintptr_t RightArm = 0x60;
inline constexpr std::uintptr_t LeftLeg  = 0x68;
inline constexpr std::uintptr_t RightLeg = 0x70;
}

namespace LimbIK {
inline constexpr std::uintptr_t Bones = 0x40; // Transform[]; arms/legs are 3 elements
}

namespace PoseIKView {
inline constexpr std::uintptr_t Bones   = 0x30;
inline constexpr std::uintptr_t BoneSet = 0x50; // Dictionary<string, Transform>
}

// ------------------------------------------------------------------
// Native Unity Transform (live verified)
// ------------------------------------------------------------------
namespace UnityTransform {
inline constexpr std::uintptr_t native     = 0x10; // managed -> native
inline constexpr std::uintptr_t hierarchy  = 0x28; // native -> hierarchy
inline constexpr std::uintptr_t index      = 0x30; // uint32
inline constexpr std::uintptr_t matrices   = 0x18; // hierarchy -> LocalTRS[]
inline constexpr std::uintptr_t parents    = 0x20; // hierarchy -> int32[]
inline constexpr std::uintptr_t pointers   = 0x30; // hierarchy -> Transform*[]
inline constexpr std::uintptr_t count      = 0x38; // int32
inline constexpr std::uintptr_t trs_stride = 0x30;
}

namespace LocalTRS {
inline constexpr std::uintptr_t translation = 0x00; // float4, xyz used
inline constexpr std::uintptr_t rotation    = 0x10; // quaternion xyzw
inline constexpr std::uintptr_t scale       = 0x20; // float4, xyz used
}

// UnityPlayer get_position icall and traced native implementation.
namespace UnityPlayerRva {
inline constexpr std::uintptr_t get_position_icall = 0x000CCA20;
inline constexpr std::uintptr_t native_impl_a      = 0x006684F0;
inline constexpr std::uintptr_t native_impl_b      = 0x00059AF0;
}

// ------------------------------------------------------------------
// Camera (live verified)
// ------------------------------------------------------------------
namespace BattleCameraModelOffsets {
inline constexpr std::uintptr_t Position    = 0x10; // V3
inline constexpr std::uintptr_t Orientation = 0x1C; // quaternion xyzw
inline constexpr std::uintptr_t LookQuat    = 0x2C; // quaternion xyzw
inline constexpr std::uintptr_t Forward     = 0x3C; // V3
inline constexpr std::uintptr_t Right       = 0x48; // V3
inline constexpr std::uintptr_t Up          = 0x54; // V3
inline constexpr std::uintptr_t Distance    = 0x60; // float
inline constexpr std::uintptr_t BlockEnd    = 0x60; // new offset set: end of camera block
inline constexpr float DefaultFovDeg        = 60.0f;
}

namespace Cinemachine {
// CinemachineCore class -> static_fields(+0xB8) -> core instance at +0x08
inline constexpr std::uintptr_t CoreInstance = 0x08;
inline constexpr std::uintptr_t BrainList    = 0x10; // List<CinemachineBrain>
inline constexpr std::uintptr_t BrainFov      = 0xB0; // vertical FOV degrees
inline constexpr std::uintptr_t BrainStateLensFov = 0xB0; // new offset set alias
}

// ------------------------------------------------------------------
// Local inventory (live verified)
// ------------------------------------------------------------------
namespace CharacterInventoryModel {
inline constexpr std::uintptr_t CellsSize  = 0x10; // InventoryCellsSizeDescription*
inline constexpr std::uintptr_t Backpack   = 0x20; // EquipSlotModel*
inline constexpr std::uintptr_t BaseLines  = 0x28; // int32
}

namespace InventoryCellsSizeDescription {
inline constexpr std::uintptr_t Columns       = 0x20; // int32
inline constexpr std::uintptr_t MaximumLines  = 0x24; // int32
}

namespace InventorySlotsModel {
inline constexpr std::uintptr_t Models = 0x18; // Dictionary<desc, slot>
}

namespace InventorySlotDescription {
inline constexpr std::uintptr_t Id    = 0x10; // managed string
inline constexpr std::uintptr_t Index = 0x18; // int32
}

namespace InventorySlotModel {
inline constexpr std::uintptr_t Item  = 0x18; // ItemModel*
inline constexpr std::uintptr_t Stack = 0x20; // StackModel*
}

namespace InventoryItemModel {
inline constexpr std::uintptr_t Data = 0x10; // InventorySlotData*
}

namespace InventoryStackModel {
inline constexpr std::uintptr_t Data = 0x10; // InventorySlotData*
}

namespace InventorySlotData {
inline constexpr std::uintptr_t Value = 0x18; // inherited packed uint32
}

namespace PrimitiveData {
inline constexpr std::uintptr_t ModelData = 0x10; // new offset set: primitive model -> data
inline constexpr std::uintptr_t value = 0x18; // verified inflated wrapper
}

// Packed InventorySlot: durability bits 0..3, payload 4..18, item 19..31.
namespace PackedSlot {
inline constexpr std::uint32_t DurabilityBits = 4;
inline constexpr std::uint32_t PayloadBits    = 15;
inline constexpr std::uint32_t ItemBits       = 13;
inline constexpr std::uint32_t DurabilityMask = 0xF;
inline constexpr std::uint32_t PayloadMask    = 0x7FFF;
inline constexpr std::uint32_t ItemMask       = 0x1FFF;
inline constexpr std::uint32_t DurabilityShift = 0;
inline constexpr std::uint32_t PayloadShift    = 4;
inline constexpr std::uint32_t ItemShift       = 19;
}

namespace InventoryRva {
inline constexpr std::uintptr_t SlotItemGetter      = 0x048D1CB0;
inline constexpr std::uintptr_t StackValueGetter    = 0x048D1130;
}

// ------------------------------------------------------------------
// Item catalog and cash (live verified, except rarity mapping)
// ------------------------------------------------------------------
namespace InventoryItemDescription {
inline constexpr std::uintptr_t Id          = 0x10; // inherited string Id
inline constexpr std::uintptr_t Index       = 0x18; // int32
inline constexpr std::uintptr_t EnumItemIndex = 0x18; // new offset set alias
inline constexpr std::uintptr_t CellWidth   = 0x24; // int32
inline constexpr std::uintptr_t CellHeight  = 0x28; // int32
inline constexpr std::uintptr_t StackSize   = 0x2C; // int32
inline constexpr std::uintptr_t Rarity      = 0x30; // inline int32 (metadata)
inline constexpr std::uintptr_t IsStackable = 0x34; // bool
inline constexpr std::uintptr_t InventoryLines = 0x58; // int32
}

namespace InventoryItemDescriptions {
inline constexpr std::uintptr_t StaticItemEnum = 0x00;
inline constexpr std::uintptr_t StaticCash1    = 0xA0;
}

// Rarity names are declared, but numeric constants are not established.
namespace Rarity {
inline constexpr int NoRare = 0;
inline constexpr int Common = 1;
inline constexpr int Uncommon = 2;
inline constexpr int Rare = 3;
inline constexpr int Epic = 4;
inline constexpr int Legendary = 5;
inline constexpr int Exotic = 6;
}

// ------------------------------------------------------------------
// Held item / vehicle (metadata)
// ------------------------------------------------------------------
namespace ItemInHandsModel {
inline constexpr std::uintptr_t Current = 0x10; // IInventoryItemModel*
}

namespace EquipItemModel {
inline constexpr std::uintptr_t Data = 0x10; // wrapper holding item-definition ref
}

namespace InVehicleModel {
inline constexpr std::uintptr_t EntityId    = 0x10; // wrapper
inline constexpr std::uintptr_t SeatSlot    = 0x18; // wrapper
inline constexpr std::uintptr_t VehicleType = 0x20; // wrapper
}

namespace VehicleSeatData {
inline constexpr std::uintptr_t Occupied = 0x20; // wrapper
inline constexpr std::uintptr_t EntityId = 0x28; // wrapper
}

// ------------------------------------------------------------------
// World entities (metadata)
// ------------------------------------------------------------------
namespace WorldEntitiesModel {
inline constexpr std::uintptr_t NotSpawnedBattleCharacters = 0x480;
inline constexpr std::uintptr_t BattleCharacters           = 0x488;
inline constexpr std::uintptr_t Containers                 = 0x490;
inline constexpr std::uintptr_t AccessEntities             = 0x498;
inline constexpr std::uintptr_t SecurityAlarms             = 0x5E0;
inline constexpr std::uintptr_t DroppedLootBags            = 0x4A0; // old "Loots"
inline constexpr std::uintptr_t Loots                      = 0x4A0;
inline constexpr std::uintptr_t VehicleLoots               = 0x4A8;
inline constexpr std::uintptr_t Doors                      = 0x4B0;
inline constexpr std::uintptr_t Vehicles                   = 0x4C0;
inline constexpr std::uintptr_t Cars                       = 0x4C8;
inline constexpr std::uintptr_t Helicopters                = 0x4D0;
inline constexpr std::uintptr_t StaticLootObjects          = 0x4F0;
inline constexpr std::uintptr_t RandomLootObjects          = 0x4F8;
inline constexpr std::uintptr_t LootNodes                  = 0x500;
inline constexpr std::uintptr_t VehicleTrunks              = 0x510;
inline constexpr std::uintptr_t Cargos                     = 0x518;
inline constexpr std::uintptr_t CollectionTables           = 0x528;
inline constexpr std::uintptr_t SafeDoors                  = 0x5C8;
inline constexpr std::uintptr_t StorageBoxes               = 0x560;
inline constexpr std::uintptr_t PlotContainers             = 0x578;
inline constexpr std::uintptr_t Shelvings                  = 0x580;
inline constexpr std::uintptr_t MoneyWorkbenches           = 0x530;
inline constexpr std::uintptr_t Grenades                   = 0x620;
inline constexpr std::uintptr_t SettlementCash             = 0x650;
inline constexpr std::uintptr_t FurnitureLoots             = 0x6A0;
inline constexpr std::uintptr_t BuildingLoots              = 0x6B8;
inline constexpr std::uintptr_t CashLootNodes              = 0x6E8;
inline constexpr std::uintptr_t PointOfInterest            = 0x6F0;
inline constexpr std::uintptr_t CargoSellers               = 0x858;
inline constexpr std::uintptr_t BarterTraders              = 0x8A8;
inline constexpr std::uintptr_t FirstAidCabinetModels      = 0x710;
inline constexpr std::uintptr_t CryptoWorkbenchesPublic    = 0x758;
inline constexpr std::uintptr_t PlotLoots                  = 0x770;
inline constexpr std::uintptr_t LocationSecurityAlarms     = 0x7B0; // direct 0x320
inline constexpr std::uintptr_t SecurityPanels             = 0x508;
inline constexpr std::uintptr_t CryptoWorkbenchesRO        = 0x758;
inline constexpr std::uintptr_t CargoOpenWorkbenches       = 0x538; // direct 0xE0
inline constexpr std::uintptr_t CargoExchangeWorkbenches   = 0x548; // direct 0xF0
inline constexpr std::uintptr_t CargoProductionWorkbenches = 0x740; // direct 0x2C0
inline constexpr std::uintptr_t CargoChargeWorkbenches     = 0x750; // direct 0x2D0
inline constexpr std::uintptr_t ArchitectTables            = 0x768; // direct 0x2E8
inline constexpr std::uintptr_t Buyers                     = 0x720;
inline constexpr std::uintptr_t CharacterLaunderers        = 0x8B0; // direct 0x458
inline constexpr std::uintptr_t CashWithdrawal             = 0x8B8; // direct 0x460
inline constexpr std::uintptr_t BankManagers               = 0x8C0; // direct 0x468
inline constexpr std::uintptr_t InteractEntities           = 0x5B8;
inline constexpr std::uintptr_t UnlockEntities             = 0x5A0;
inline constexpr std::uintptr_t BreachEntities             = 0x5A8;
inline constexpr std::uintptr_t LocationDoors              = 0x690; // direct 0x210
inline constexpr std::uintptr_t SafeDoorsRO                = 0x5C8;
}

namespace ContainerModel {
inline constexpr std::uintptr_t Id          = 0x10;
inline constexpr std::uintptr_t Description = 0x18; // LocationContainerDescription*
inline constexpr std::uintptr_t Position    = 0x28; // PositionModel*; xyz at +0x10
inline constexpr std::uintptr_t IsEmpty     = 0x38; // bool primitive wrapper
}

namespace LootModel {
inline constexpr std::uintptr_t Id        = 0x10;
inline constexpr std::uintptr_t Transform = 0x20; // TransformModel*
}

namespace TransformModel {
inline constexpr std::uintptr_t X = 0x10; // scalar position wrapper/float
inline constexpr std::uintptr_t Y = 0x18;
inline constexpr std::uintptr_t Z = 0x20;
}

namespace StaticLootObjectModel {
inline constexpr std::uintptr_t Description = 0x18; // V3 position at +0x30
}

namespace RandomLootObjectModel {
inline constexpr std::uintptr_t Description = 0x20; // V3 position at +0x24
}

namespace VehicleModel {
inline constexpr std::uintptr_t Transform   = 0x28;
inline constexpr std::uintptr_t Description = 0x118;
}

namespace VehicleTransformModel {
inline constexpr std::uintptr_t Position = 0x10; // primitive<Vector3> wrapper
}

namespace CarModel {
inline constexpr std::uintptr_t Transform   = 0xC0;
inline constexpr std::uintptr_t Description = 0x78;
}

namespace HelicopterModel {
inline constexpr std::uintptr_t Transform   = 0x70;
inline constexpr std::uintptr_t Description = 0x48;
}

namespace GrenadeModel {
inline constexpr std::uintptr_t Description = 0x18;
inline constexpr std::uintptr_t Transform   = 0x30; // GrenadeTransformModel*
}

namespace GrenadeTransformModel {
inline constexpr std::uintptr_t Position = 0x18; // inline V3
}

// ------------------------------------------------------------------
// Extra world / valuable models (new metadata dump)
// Positions are inline Vector3 fields inside the description model.
// ------------------------------------------------------------------
namespace LootNodeModel {
inline constexpr std::uintptr_t Description         = 0x10;
inline constexpr std::uintptr_t DescriptionPosition = 0x38;
}

namespace SafeDoorModel {
inline constexpr std::uintptr_t Description         = 0x28;
inline constexpr std::uintptr_t DescriptionPosition = 0x14;
}

namespace StorageBoxModel {
inline constexpr std::uintptr_t DescriptionModel    = 0x20;
inline constexpr std::uintptr_t DescriptionPosition = 0x34;
}

namespace ShelvingModel {
inline constexpr std::uintptr_t DescriptionModel    = 0x18;
inline constexpr std::uintptr_t DescriptionPosition = 0x20;
}

namespace CollectionTableModel {
inline constexpr std::uintptr_t DescriptionModel    = 0x10;
inline constexpr std::uintptr_t DescValue           = 0x10; // LocationCollectionTableDescription
inline constexpr std::uintptr_t Position            = 0x20; // Vector3
inline constexpr std::uintptr_t Orientation         = 0x2C; // Vector3
inline constexpr std::uintptr_t DescriptionPosition = 0x20;
inline constexpr std::uintptr_t RewardModel         = 0x30;
}

namespace PlotContainerModel {
inline constexpr std::uintptr_t Description         = 0x10;
inline constexpr std::uintptr_t DescriptionPosition = 0x50;
}

namespace CashLootNodeModel {
inline constexpr std::uintptr_t CashAmountData      = 0x10; // CashAmountData -> +0x18 _value
inline constexpr std::uintptr_t Description         = 0x18; // LocationCashLootNodeDescription
inline constexpr std::uintptr_t DescriptionPosition = 0x24;
inline constexpr std::uintptr_t Position            = 0x24;
}

namespace SettlementCashModel {
inline constexpr std::uintptr_t Data                = 0x10; // SettlementCashPublicData
inline constexpr std::uintptr_t Description         = 0x18; // LocationSettlementCashDescription
inline constexpr std::uintptr_t DescriptionPosition = 0x1C;
inline constexpr std::uintptr_t EntityId            = 0x10; // int32
inline constexpr std::uintptr_t TaxEntityId         = 0x14; // int32
inline constexpr std::uintptr_t ConquestEntityId    = 0x18; // int32
inline constexpr std::uintptr_t CashPoint           = 0x1C; // Vector3
inline constexpr std::uintptr_t TaxPoint            = 0x28; // Vector3
inline constexpr std::uintptr_t ConquestPoint       = 0x34; // Vector3
}

namespace SettlementCashPublicData {
inline constexpr std::uintptr_t TaxCashAmount       = 0x28; // IntData -> +0x18 _value
inline constexpr std::uintptr_t CashAmount          = 0x50; // IntData -> +0x18 _value
}

namespace MoneyWorkbenchModel {
inline constexpr std::uintptr_t Data                = 0x10; // MoneyWorkbenchData
inline constexpr std::uintptr_t DescriptionModel    = 0x20; // DescriptionModel<LocationMoneyWorkbenchDescription>
inline constexpr std::uintptr_t DescValue           = 0x10; // LocationMoneyWorkbenchDescription
inline constexpr std::uintptr_t Position            = 0x20; // Vector3
inline constexpr std::uintptr_t Orientation         = 0x2C; // Vector3
inline constexpr std::uintptr_t DescriptionPosition = 0x20;
}

namespace MoneyWorkbenchData {
inline constexpr std::uintptr_t DirtyAmount         = 0x20; // CashAmountData -> +0x18 _value
inline constexpr std::uintptr_t CleanAmount         = 0x28; // CashAmountData -> +0x18 _value
}

namespace CashAmountData {
inline constexpr std::uintptr_t Value               = 0x18; // int32
}

namespace DroppedBagModel {
inline constexpr std::uintptr_t Transform        = 0x20;
inline constexpr std::uintptr_t InteractPosition = 0x28;
}

namespace VehicleLootModel {
inline constexpr std::uintptr_t Transform        = 0x20;
inline constexpr std::uintptr_t InteractPosition = 0x28;
}

namespace CargoModel {
inline constexpr std::uintptr_t Transform        = 0x30;
inline constexpr std::uintptr_t InteractPosition = 0x38;
}

namespace PlotLootModel {
inline constexpr std::uintptr_t Position = 0x18;
}

namespace FurnitureLootModel {
inline constexpr std::uintptr_t Position = 0x18;
}

namespace BuildingLootModel {
inline constexpr std::uintptr_t Position = 0x18;
}

namespace PointOfInterestModel {
inline constexpr std::uintptr_t Description         = 0x38;
inline constexpr std::uintptr_t DescriptionPosition = 0x30;
}

// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Verified player state, inventory, currency & experimental writes
// ------------------------------------------------------------------
namespace WeaponReloadModel {
inline constexpr std::uintptr_t State = 0x10; // ReferenceData -> +0x18 non-null when reloading
inline constexpr std::uintptr_t EndTs = 0x18; // TimestampData (+0x18 i64)
}

namespace KnockdownModel {
inline constexpr std::uintptr_t EndTs           = 0x10; // TimestampData (+0x18 i64)
inline constexpr std::uintptr_t IsKnockedForward = 0x18; // BoolData (+0x18 u8)
}

namespace CoinsByDataModel {
inline constexpr std::uintptr_t Data  = 0x10; // IntData (+0x18 i32)
inline constexpr std::uintptr_t Value = 0x18;
}

namespace RobberInventoryCostModel {
inline constexpr std::uintptr_t CharacterInventory      = 0x10;
inline constexpr std::uintptr_t VehicleInventory        = 0x14;
inline constexpr std::uintptr_t CharacterEquip          = 0x18;
inline constexpr std::uintptr_t CharacterInventoryCash  = 0x1C;
inline constexpr std::uintptr_t CharacterInventoryItems = 0x20;
inline constexpr std::uintptr_t VehicleInventoryCash    = 0x24;
inline constexpr std::uintptr_t VehicleInventoryItems   = 0x28;
}

namespace EquipSlotsModel {
inline constexpr std::uintptr_t Models            = 0x10; // Dictionary<EquipSlotDescription, EquipSlotModel>
inline constexpr std::uintptr_t SlotDescriptionId = 0x10;
inline constexpr std::uintptr_t ItemModel         = 0x18;
inline constexpr std::uintptr_t DurabilityModel   = 0x20;
}

namespace PocketsModel {
inline constexpr std::uintptr_t Slots      = 0x20; // -> +0x10 Dictionary<PocketSlotDescription, PocketSlotModel>
inline constexpr std::uintptr_t Models     = 0x10;
inline constexpr std::uintptr_t ItemModel  = 0x18;
inline constexpr std::uintptr_t StackModel = 0x20;
}

namespace InitArmorModel {
inline constexpr std::uintptr_t HasArmor = 0x10; // BoolData (+0x18)
inline constexpr std::uintptr_t Amount   = 0x18; // IntData (+0x18)
}

namespace StaminaModel {
inline constexpr std::uintptr_t Amount            = 0x18; // IntData (+0x18)
inline constexpr std::uintptr_t DecreaseDirection = 0x20; // BoolData (+0x18)
inline constexpr std::uintptr_t Config            = 0x28; // BattleCharacterStaminaConfig
inline constexpr std::uintptr_t ConfigWaste       = 0x10; // float
inline constexpr std::uintptr_t ConfigRecovery    = 0x14; // float
}

namespace BattleCharacterConfig {
inline constexpr std::uintptr_t SprintCooldownTime        = 0x70; // float
inline constexpr std::uintptr_t MaxSprintAngleDot         = 0x78; // float
inline constexpr std::uintptr_t EmptyHandsCharacterSpeed  = 0x90;
inline constexpr std::uintptr_t CargoCharacterSpeed       = 0x98;
}

namespace SprintModel {
inline constexpr std::uintptr_t TransitionTime = 0x28; // float
inline constexpr std::uintptr_t CooldownTime   = 0x30; // float
}

namespace CrouchModel {
inline constexpr std::uintptr_t TransitionTime = 0x20; // float
}

namespace InventoryProgressItemModel {
inline constexpr std::uintptr_t Progress    = 0x10; // float
inline constexpr std::uintptr_t ReloadEndTs = 0x18; // i64
}

namespace InventoryIntentionModel {
inline constexpr std::uintptr_t Now    = 0xA0; // SetNow -> +0x10 i64 _time (ms)
inline constexpr std::uintptr_t TimeMs = 0x10;
}

namespace ProlongedInteractionDialogModel {
inline constexpr std::uintptr_t Progress = 0x18; // float
}

namespace ProlongedInteractionModel {
inline constexpr std::uintptr_t EndTs = 0x18; // TimestampData (+0x18 i64)
}

namespace DelayedUseActionModel {
inline constexpr std::uintptr_t StartTs = 0x18; // float
inline constexpr std::uintptr_t EndTs   = 0x28; // float
}

namespace AutoLootAfterBreakModel {
inline constexpr std::uintptr_t AutoLootEndTs = 0x28; // float
}

namespace RadiusInteractEntityModel {
inline constexpr std::uintptr_t PositionModel = 0x10; // XYZ at +0x10..+0x18
}

namespace SecurityAlarmModel {
inline constexpr std::uintptr_t AlarmTs      = 0x10; // TimestampData (+0x18 i64)
inline constexpr std::uintptr_t IsAlarm      = 0x18; // BoolData (+0x18 u8)
inline constexpr std::uintptr_t IsHacked     = 0x20; // BoolData (+0x18 u8)
inline constexpr std::uintptr_t Description  = 0x28;
inline constexpr std::uintptr_t DescEntityId = 0x20; // LocationSecurityAlarmDescription
}

namespace SecurityPanelModel {
inline constexpr std::uintptr_t IsHacked    = 0x10; // BoolData (+0x18 u8)
inline constexpr std::uintptr_t Description = 0x18; // LocationPanelDescription
inline constexpr std::uintptr_t DescPos     = 0x30; // Vector3
}

namespace WorkbenchWorldModels {
inline constexpr std::uintptr_t CryptoIsActive          = 0x20; // Primitive<bool>
inline constexpr std::uintptr_t CryptoDescription       = 0x30;
inline constexpr std::uintptr_t CargoOpenDescription    = 0x18; // InteractPoint at +0x38
inline constexpr std::uintptr_t CargoOpenInteractPoint  = 0x38;
inline constexpr std::uintptr_t ArchitectDescription    = 0x10; // Position at +0x14
inline constexpr std::uintptr_t ArchitectDescPos        = 0x14;
}

namespace TraderDescriptions {
inline constexpr std::uintptr_t CargoSellerPos       = 0x28; // LocationCargoSellerDescription
inline constexpr std::uintptr_t BarterTraderPos      = 0x30; // LocationBarterTraderDescription
inline constexpr std::uintptr_t BuyerDescription     = 0x18; // BuyerModel -> Desc -> +0x2C
inline constexpr std::uintptr_t BuyerDescPos         = 0x2C;
inline constexpr std::uintptr_t LaundererInteract    = 0x24; // LocationCharacterLaunderingDescription
inline constexpr std::uintptr_t CashWithdrawalPoint  = 0x1C; // LocationCashWithdrawalDescription
inline constexpr std::uintptr_t BankManagerPoint     = 0x18; // LocationBankManagerDescription
}

namespace AccessEntityModel {
inline constexpr std::uintptr_t InteractPositionModel = 0x20; // XYZ at +0x10
inline constexpr std::uintptr_t PositionModel         = 0x28; // XYZ at +0x10
inline constexpr std::uintptr_t AccessStateModel      = 0x30;
inline constexpr std::uintptr_t AccessGranted         = 0x18; // BoolData (+0x18 u8)
}

namespace DoorModel {
inline constexpr std::uintptr_t State               = 0x30; // DoorStateModel -> 0x10 RefData -> 0x18 Desc -> 0x10 Id
inline constexpr std::uintptr_t Position            = 0x38; // PositionModel (XYZ +0x10)
inline constexpr std::uintptr_t OpenCloseRequest    = 0x48;
inline constexpr std::uintptr_t StateChangeCooldown = 0x58; // +0x10 TimestampData (+0x18 i64)
}

namespace LocationDoorModel {
inline constexpr std::uintptr_t UnlockState     = 0x28; // ReferenceData -> +0x18 -> +0x10 Id ("locked"/"unlocked")
inline constexpr std::uintptr_t Description     = 0x30; // LocationDoorDescription
inline constexpr std::uintptr_t DescWorldPos    = 0x30; // Vector3
inline constexpr std::uintptr_t InnerDoorModel  = 0x38;
}

namespace TimedEntityModels {
inline constexpr std::uintptr_t UnlockTime = 0x18; // UnlockEntityModel (i64 ms)
inline constexpr std::uintptr_t CastTime   = 0x18; // BreachEntityModel (i64 ms)
}

// Spawn message declarations (metadata only; no live invocation path)
// ------------------------------------------------------------------
namespace SetInventoryItemCheatMessage {
inline constexpr std::uintptr_t Rva = 0x049C5E60;
inline constexpr std::uintptr_t SlotSource      = 0x20;
inline constexpr std::uintptr_t InventorySlot   = 0x28;
inline constexpr std::uintptr_t EquipSlot       = 0x30;
inline constexpr std::uintptr_t PocketSlot      = 0x38;
inline constexpr std::uintptr_t Item            = 0x40;
inline constexpr std::uintptr_t Count           = 0x48;
inline constexpr std::uintptr_t Ammo            = 0x50;
inline constexpr std::uintptr_t Durability      = 0x58;
inline constexpr std::uintptr_t CumulativePoints = 0x60;
inline constexpr std::uintptr_t EntityId        = 0x68;
}

namespace CheatFillExternalInventoryMessage {
inline constexpr std::uintptr_t Rva  = 0x049B0A60;
inline constexpr std::uintptr_t Item = 0x28;
}

namespace CheatAddCoinsMessage {
inline constexpr std::uintptr_t Rva    = 0x049823E0;
inline constexpr std::uintptr_t Amount = 0x20;
}

// ------------------------------------------------------------------
// Read consistency / session recovery
// ------------------------------------------------------------------
namespace Read {
inline constexpr std::uint64_t ScatterFlags = 0x121;

inline constexpr std::uint64_t RefreshTlbPartial    = 0x2001000400000000ull;
inline constexpr std::uint64_t RefreshTlbFull       = 0x2001080000000000ull;
inline constexpr std::uint64_t RefreshMemory        = 0x2001100000000000ull;
inline constexpr std::uint64_t RefreshSpecificProcess = 0x2002000300000000ull;

inline constexpr std::uint32_t PartialRefreshIntervalMs = 250;
inline constexpr std::uint32_t RecoveryRefreshMinMs     = 500;
inline constexpr std::uint32_t SceneRetryStartMs        = 100;
inline constexpr std::uint32_t SceneRetryMaxMs          = 1000;
inline constexpr std::uint32_t PersistentFailureMs      = 5000;
inline constexpr std::uint32_t ReconnectRetryMs         = 2000;
}

// ------------------------------------------------------------------
// Verified accessor RVAs. These are useful for fingerprint checks; the
// overlay reads memory directly and does not call into the game.
// ------------------------------------------------------------------
namespace AccessorRva {
inline constexpr std::uintptr_t LoadedClassTable = 0x6E7360;
inline constexpr std::uintptr_t FieldsIterator  = 0x72BEE0;
inline constexpr std::uintptr_t MethodsIterator = 0x72CAE9;
inline constexpr std::uintptr_t ClassName       = 0x6460;
inline constexpr std::uintptr_t ClassNamespace  = 0x6470;
inline constexpr std::uintptr_t StaticFields    = 0x2110;
inline constexpr std::uintptr_t InstanceSize    = 0x72C5A0;
inline constexpr std::uintptr_t ClassFlags      = 0x6F9590;
inline constexpr std::uintptr_t FieldOffset     = 0x6DD930;
inline constexpr std::uintptr_t FieldType       = 0x2B8210;
inline constexpr std::uintptr_t MethodToken     = 0x3ABE0;
inline constexpr std::uintptr_t MethodParamCount = 0x6E6BB0;
inline constexpr std::uintptr_t MethodReturnType = 0xCA60;
inline constexpr std::uintptr_t TypeCode        = 0x73E360;
}

// Legacy TypeInfo singletons. The verified root path is Game+0xD0 ->
// GameModel+0x10 -> BattleModel; these remain as a fallback for sessions where
// the Game object has not been located yet.
namespace TypeInfo {
inline constexpr std::uintptr_t UnityCamera                = 0x80F3C10;
inline constexpr std::uintptr_t BattleModel                = 0x81C59A0;
inline constexpr std::uintptr_t BattleEntitiesModel        = 0x81C2048;
inline constexpr std::uintptr_t BattleEntitiesUpdater      = 0x81C2510;
inline constexpr std::uintptr_t BattleCharacterEntityModel = 0x81BB720;
inline constexpr std::uintptr_t PlayerEntityModel          = 0x81F8398;
inline constexpr std::uintptr_t BattleCameraModel          = 0x81B8DB8;
inline constexpr std::uintptr_t ClientCharacterSlotsModel  = 0x816E270;
inline constexpr std::uintptr_t GlobalModel                = 0x813B310;
inline constexpr std::uintptr_t BlindedEffect              = 0x81DC358; // new offset set
}

} // namespace rr
