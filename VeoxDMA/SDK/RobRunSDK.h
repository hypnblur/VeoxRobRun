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
inline constexpr std::uintptr_t BattleEntitiesModel = 0x90;
inline constexpr std::uintptr_t BattleCameraModel   = 0x358;

// New offset set: WorldEntitiesModel is reached directly from BattleModel.
// The older +0x510 path is kept as a validated fallback for sessions where the
// direct field is null.
inline constexpr std::uintptr_t WorldEntitiesModel       = 0x60;
inline constexpr std::uintptr_t WorldEntitiesModelLegacy = 0x510;

// New offset set: character slots live on BattleModel, not under
// BattleEntitiesModel. The old root is retained as a fallback.
inline constexpr std::uintptr_t CharacterSlotsModel = 0x248;
inline constexpr std::uintptr_t CharacterSlotsModelLegacy = 0x4C8;
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
}

namespace BattleCharacterEntityModel {
inline constexpr std::uintptr_t ClientModel    = 0x20; // new offset set
inline constexpr std::uintptr_t ServerModel    = 0x28; // new offset set
inline constexpr std::uintptr_t SimulatedModel = 0x30; // legacy path
inline constexpr std::uintptr_t BattleCharacterModel = 0x20; // alias for ClientModel
}

namespace BattleCharacterModel {
inline constexpr std::uintptr_t PrivateModel            = 0x10;
inline constexpr std::uintptr_t Id                      = 0x38; // new offset set
inline constexpr std::uintptr_t Data                    = 0x40; // new offset set
inline constexpr std::uintptr_t CharacterTransformModel = 0x58;
inline constexpr std::uintptr_t IsDeadWrapper           = 0xF0;
inline constexpr std::uintptr_t Weapon                  = 0x150; // new offset set
inline constexpr std::uintptr_t ItemInHands             = 0x170; // legacy path
inline constexpr std::uintptr_t InVehicle               = 0x1C8;
inline constexpr std::uintptr_t PlayerRole              = 0x208;
inline constexpr std::uintptr_t Pbm                     = 0x258; // new offset set
inline constexpr std::uintptr_t Knockout                = 0x298; // new offset set
}

namespace CharacterTransformModel {
inline constexpr std::uintptr_t Position = 0x10; // V3 + pitch/yaw/roll (radians)
inline constexpr std::uintptr_t X        = 0x10; // new offset set
inline constexpr std::uintptr_t Y        = 0x14; // new offset set
inline constexpr std::uintptr_t Z        = 0x18; // new offset set
inline constexpr std::uintptr_t Yaw      = 0x20; // new offset set
}

namespace BattleCharacterPrivateModel {
inline constexpr std::uintptr_t Inventory   = 0x150;
inline constexpr std::uintptr_t HealthModel = 0x1B0;
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
inline constexpr std::uintptr_t Position = 0x28; // primitive<Vector3>
inline constexpr std::uintptr_t LookDirection = 0x30; // primitive<Vector3>
inline constexpr std::uintptr_t IsSprinting = 0x88;
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
}

namespace PlayerClient {
inline constexpr std::uintptr_t AimAssist  = 0x2A0;
inline constexpr std::uintptr_t AutoFire   = 0x2A8;
inline constexpr std::uintptr_t Recoil     = 0x2B8;
inline constexpr std::uintptr_t FakeSpread = 0x2C0;
}

namespace AimAssist {
inline constexpr std::uintptr_t IsActive   = 0x10;
inline constexpr std::uintptr_t HasTarget  = 0x12;
inline constexpr std::uintptr_t Correction = 0x28;
}

namespace RecoilModel {
inline constexpr std::uintptr_t OffsetVec = 0x10;
}

namespace FakeSpreadModel {
inline constexpr std::uintptr_t Spread = 0x10;
inline constexpr std::uintptr_t Air    = 0x14;
inline constexpr std::uintptr_t Move   = 0x18;
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
inline constexpr std::uintptr_t NotSpawnedBattleCharacters = 0x10;
inline constexpr std::uintptr_t BattleCharacters           = 0x18;
inline constexpr std::uintptr_t Containers                 = 0x20;
inline constexpr std::uintptr_t AccessEntities             = 0x28;
inline constexpr std::uintptr_t SecurityAlarms             = 0x30;
inline constexpr std::uintptr_t Loots                      = 0x38;
inline constexpr std::uintptr_t VehicleLoots               = 0x40;
inline constexpr std::uintptr_t Doors                      = 0x48;
inline constexpr std::uintptr_t Vehicles                   = 0x58;
inline constexpr std::uintptr_t Cars                       = 0x60;
inline constexpr std::uintptr_t Helicopters                = 0x68;
inline constexpr std::uintptr_t StaticLootObjects          = 0x98;
inline constexpr std::uintptr_t RandomLootObjects          = 0xA0;
inline constexpr std::uintptr_t LootNodes                  = 0xB0;
inline constexpr std::uintptr_t Grenades                   = 0x1A0;
inline constexpr std::uintptr_t FirstAidCabinetModels      = 0x290;
inline constexpr std::uintptr_t CashLootNodes              = 0x268;
inline constexpr std::uintptr_t PointOfInterest            = 0x270;
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
