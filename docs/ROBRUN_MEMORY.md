# ROB RUN - Memory Reference

Windows x64 - Unity IL2CPP

This is the build-pinned reference used by the overlay in this repository. The
canonical constants live in [`VeoxDMA/SDK/RobRunSDK.h`](../VeoxDMA/SDK/RobRunSDK.h);
this document explains the paths and how each one is consumed.

Sections are split into **Live verified** and **Metadata**, matching the source
reference. Metadata offsets are declaration-derived: the code validates every
pointer, class identity, field offset and finite value before use.

## Build

| Item | Value |
| --- | --- |
| Process | `ROB RUN.exe` |
| Code module | `GameAssembly.dll` |
| PE TimeDateStamp | `0x6AF2C76F` |
| PE SizeOfImage | `0x08FC9000` |
| GameAssembly SHA-256 | `ecd0a4275325fce38c9794576c541891cb8dfaa0b0ac3a38fdd2dc9711ecb81e` |
| UnityPlayer PE TimeDateStamp | `0x6A28669F` |
| UnityPlayer PE SizeOfImage | `0x02426000` |

Offsets below are specific to this build. Native pointers, module bases and
object addresses must be resolved per session.

## Notation

- `Q(address)`: little-endian `uint64` read.
- `I32(address)`: little-endian `int32` read.
- `U32(address)`: little-endian `uint32` read.
- `F32(address)`: little-endian `float32` read.
- `V3(address)`: three consecutive `float32` values, x/y/z.

`RVA`: relative virtual address; `VA = owning module base + RVA`.
`Object + offset`: field address. `Q(Object + offset)`: referenced object.
All offsets are hexadecimal unless explicitly marked decimal.

---

## Il2Cpp objects and metadata

**Live verified**

| Address / field | Value |
| --- | --- |
| `Managed object +0x00` | `Il2CppClass*` |
| `Managed object +0x08` | monitor pointer |
| `Il2CppClass +0x10` | `char* name` |
| `Il2CppClass +0x18` | `char* namespace` |
| `Il2CppClass +0x58` | `Il2CppClass* parent` |
| `Il2CppClass +0x80` | `FieldInfo* fields` |
| `Il2CppClass +0xB8` | `static_fields` pointer |
| `Il2CppClass +0xF8` | `uint32 instance size` |
| `Il2CppClass +0x118` | `uint32 class flags` |
| `Il2CppClass +0x124` | `uint16 field_count` |

`FieldInfo` stride: `0x20`.

| Address / field | Value |
| --- | --- |
| `FieldInfo +0x00` | `char* name` |
| `FieldInfo +0x08` | `Il2CppType* type` |
| `FieldInfo +0x10` | `Il2CppClass* declaring parent` |
| `FieldInfo +0x18` | `int32 field offset` |
| `Il2CppType +0x0A` | type-code byte used by the field resolver |

Relevant type codes: `0x02` bool, `0x12` class, `0x15` generic instance,
`0x1C` object. Walk class parents to resolve inherited fields. Open-generic dump
offsets of `0x0` are not usable instance offsets; resolve fields on the live
inflated generic class.

Implementation: `Il2Cpp::Fields`, `Il2Cpp::FindField`,
`Il2Cpp::ReadPrimitive*`.

---

## Managed collections

**Live verified**

| Address / field | Value |
| --- | --- |
| `String +0x10` | `int32` UTF-16 code-unit length |
| `String +0x14` | UTF-16 character data (not a `char*`, not null-terminated UTF-8) |
| `Array +0x18` | `uint64` length |
| `Array +0x20` | element data |
| `List<T> +0x10` | backing array pointer |
| `List<T> +0x18` | `int32` count |
| `Dictionary +0x18` | entries array pointer |
| `Dictionary +0x20` | `int32` count, including removed slots |
| `Dictionary +0x24` | `int32` freeList |
| `Dictionary +0x28` | `int32` freeCount |
| `Dictionary +0x2C` | `int32` version |

`Dictionary<int, reference>` entry stride `0x18`:

| Offset | Field |
| --- | --- |
| `+0x00` | `uint32 hash` |
| `+0x04` | `int32 next` |
| `+0x08` | `int32 key` |
| `+0x0C` | padding |
| `+0x10` | `uint64 value` |

`Dictionary<reference, reference>` entry stride `0x18`:

| Offset | Field |
| --- | --- |
| `+0x00` | `uint32 hash` |
| `+0x04` | `int32 next` |
| `+0x08` | `uint64 key` |
| `+0x10` | `uint64 value` |

Allocated entry predicate used by this runtime: `next >= -1`. Enumerate `count`
entries, skip removed entries and null values. Re-read entries/count/version
after capture and discard a changed snapshot.

Implementation: `RobRunMemory::ReadDictInt`, `ReadDictRef`, `ReadList`,
`ReadString`.

---

## Game and entity roots

**Live verified**

| Address / field | Value |
| --- | --- |
| `Game.Game +0x20` | active updater `List<reference>` |
| `Game.Game +0xD0` | `Game.GameModel*` |
| `Game.GameModel +0x10` | `Game.Battle.BattleModel*` |
| `BattleEntitiesModel +0x60` | `WorldEntitiesModel*` (dump path) |
| `BattleModel +0x60` | `WorldEntitiesModel*` fallback |
| `BattleModel +0x510` | `WorldEntitiesModel*` legacy fallback |
| `BattleModel +0x90` | `Game.Battle.BattleEntitiesModel*` |
| `BattleModel +0x248` | character slots model (dump path) |
| `BattleModel +0x4C8` | character slots model legacy fallback |
| `Character slots model +0x18` | slot dictionary, keyed by entity ID |
| `Character slots model +0x10` | slot dictionary legacy fallback |
| `BattleEntitiesModel +0x50` | entity dictionary, keyed by entity ID |
| `BattleEntitiesModel +0x80` | character slots root (legacy fallback) |
| `Character slots root +0x18` | slot dictionary, keyed by entity ID |
| `BattleEntitiesModel +0xD0` | local `PlayerEntityModel*` |
| `PlayerEntityModel +0x10` | local `int32` entity ID |
| `PlayerEntityModel +0x18` | local client `BattleCharacterModel*` |
| `PlayerEntityModel +0x20` | local server `BattleCharacterModel*` |
| `BattleCharacterEntityModel +0x20` | client `BattleCharacterModel*` (dump path) |
| `BattleCharacterEntityModel +0x30` | simulated `BattleCharacterModel*` fallback |
| `BattleCharacterModel +0x38` | character ID (new offset set) |
| `BattleCharacterModel +0x40` | character data (new offset set) |
| `BattleCharacterModel +0x58` | `CharacterTransformModel*` |
| `BattleCharacterModel +0x150` | weapon model (new offset set) |
| `BattleCharacterModel +0x258` | PBM (new offset set) |
| `BattleCharacterModel +0x298` | knockout model (new offset set) |
| `CharacterTransformModel +0x10` | position `V3` (`x`/`y`/`z`) |
| `CharacterTransformModel +0x20` | yaw in radians (new offset set) |

Character rotation values are radians. The reader prefers the new offset set
and falls back to the legacy slots root / world-entities offset only when the
new path fails dictionary validation.

Implementation: `RobRunReader::ResolveBattleModel`, `RobRunReader::Update`.

---

## New offset set: gameplay fields

**Metadata** — the read-only overlay does not act on these, but they are part of
the canonical offset map.

| Address / field | Value |
| --- | --- |
| `BattleCharacterModel +0x40` -> `+0x20` | battle data |
| `BattleData +0x20` | common model |
| `CommonModel +0x20` | role |
| `CommonModel +0x28` | position `Vector3` primitive |
| `CommonModel +0x30` | look direction `Vector3` primitive |
| `CommonModel +0x88` | `IsSprinting` primitive |
| `CommonModel +0xA8` | `IsCrouching` primitive |
| `CommonModel +0xE0` | `IsDead` primitive |
| `CommonModel +0xF0` | `VehicleEntityId` primitive |
| `HealthModel +0x24` | critical-health `int32` |
| `BattleCharacterModel +0x298` | knockout model |
| `KnockoutModel +0x18` | phase / phase description |
| `KnockoutDescription +0x10` | description ID |
| `PlayerEntityModel +0x18` | client |
| `PlayerEntityModel +0x20` | server |
| `PlayerClient +0x2A0` | aim-assist model |
| `PlayerClient +0x2A8` | auto-fire model |
| `PlayerClient +0x2B8` | recoil model |
| `PlayerClient +0x2C0` | fake-spread model |
| `AimAssist +0x10` | is active |
| `AimAssist +0x12` | has target |
| `AimAssist +0x28` | correction |
| `RecoilModel +0x10` | offset vector |
| `FakeSpreadModel +0x10` | spread |
| `FakeSpreadModel +0x14` | air |
| `FakeSpreadModel +0x18` | move |
| `BattlePlayerCapacity` | `320` |
| `TypeInfo.BlindedEffect` | RVA `0x81DC358` |

Other aliases from the new set: `PrimitiveData.ModelData = 0x10`,
`InventoryItemDescription.EnumItemIndex = 0x18`,
`ReferenceData.Value = 0x18`, `Description.Name = 0x10`,
`WeaponModel.ItemInHands = 0x10`.

---

## Active view updater

**Live verified**

```text
list = Q(game + 0x20)
array = Q(list + 0x10)
count = I32(list + 0x18)
For each element:
  a = Q(array + 0x20 + i*8)
  b = Q(a + 0x50)
  c = Q(b + 0x18)
  updater = Q(c + 0x90)
Require updater class == Game.Battle.BattleEntitiesUpdater.
Require Q(updater + 0x70) == current BattleModel.
```

| Address / field | Value |
| --- | --- |
| `BattleEntitiesUpdater +0x110` | view dictionary keyed by entity ID |

Keep and revalidate each intermediate pointer; this is an active-list path.

Implementation: `RobRunReader::FindUpdater`,
`SkeletonExtractor::BuildSkeleton`.

---

## Names and clan tags

**Live verified**

```text
slotData = Q(slot + 0x10)
nicknameWrapper = Q(slotData + 0x30)
nicknameString = Q(nicknameWrapper + 0x18)
clanWrapper = Q(slotData + 0x38)
clanString = Q(clanWrapper + 0x18)
```

The `+0x18` wrapper value offset applies to these verified concrete wrappers.

Implementation: `RobRunReader::ReadSlotIdentity`,
`RobRunMemory::ReadWrapperString`.

---

## Life state

**Live verified**

| Address / field | Value |
| --- | --- |
| `BattleCharacterModel +0xF0` | `IsDead` primitive-model wrapper |

Resolve `<Value>k__BackingField` on its actual runtime class. If the wrapper
delegates to `_data`, resolve `_data` and then its inherited `_value`. Expected
value type: bool, exactly one byte; valid values are `0` or `1`.

```text
0 = alive; 1 = dead. Failed reads or changed identity = unknown.
```

Implementation: `Il2Cpp::ReadPrimitiveBool`.

---

## Camera

**Live verified**

| Address / field | Value |
| --- | --- |
| `BattleModel +0x358` | `BattleCameraModel*` |
| `BattleCameraModel +0x10` | position `V3` |
| `BattleCameraModel +0x1C` | orientation quaternion, x/y/z/w |
| `BattleCameraModel +0x2C` | look quaternion, x/y/z/w |
| `BattleCameraModel +0x3C` | forward `V3` |
| `BattleCameraModel +0x48` | right `V3` |
| `BattleCameraModel +0x54` | up `V3` |
| `BattleCameraModel +0x60` | `float distance` / end of camera block |

Cinemachine:

```text
CinemachineCore class +0xB8 -> static_fields
Q(static_fields + 0x08) -> core instance
Q(core instance + 0x10) -> brain list
Enumerate and validate Cinemachine.CinemachineBrain instances.
```

| Address / field | Value |
| --- | --- |
| `CinemachineBrain +0xB0` | active vertical FOV in degrees |

```text
HorizontalFOV = 2*atan(tan(VerticalFOV*pi/360)*aspect)*180/pi
aspect = viewport width / viewport height
```

Implementation: `RobRunCamera::Read`, `ReadCinemachineFov`,
`BuildViewProjection`. `BuildViewProjection` emits row-vector matrices
(`clip = [x y z 1] * view * projection`), which is the same memory order as
Unity's column-major native matrices and matches `WorldToScreen` and the
`CameraFinder` fallback. `ProjectWorld` is the bool-returning projection used
by the ESP so a valid result at screen `(0,0)` is not mistaken for a failure.

---

## Coordinates and distance

**Live verified**

- Unity position: metres, Y up.
- Converted position: `(Unity.z*100, Unity.x*100, Unity.y*100)`, centimetres,
  Z up.
- Converted camera forward uses the same axis permutation.

```text
PitchDegrees = atan2(forward.z, hypot(forward.x,forward.y))*180/pi
YawDegrees = atan2(forward.y,forward.x)*180/pi
DistanceMetres = length(playerUnityPosition-cameraUnityPosition)
```

The ESP keeps Unity coordinates and projects them directly; the axis
permutation is only needed by consumers that want Z-up centimetres.

---

## Character visual rig

**Live verified**

| Address / field | Value |
| --- | --- |
| `view +0x10` | native Unity object pointer |
| `view +0x28` | `AnimationView*` |
| `view +0x30` | `WithWeaponView*` |
| `AnimationView +0x28` | `HeadView*` |
| `AnimationView +0x30` | `TorsoView*` |
| `HeadView +0x20` | neck `Transform*` |
| `HeadView +0x28` | head `Transform*` |
| `TorsoView +0x20` | root `Transform*` at the feet; not anatomical pelvis |
| `TorsoView +0x28` | lower spine `Transform*` |
| `TorsoView +0x30` | upper spine `Transform*` |
| `WithWeaponView +0x58` | left arm `LimbIK*` |
| `WithWeaponView +0x60` | right arm `LimbIK*` |
| `WithWeaponView +0x68` | left leg `LimbIK*` |
| `WithWeaponView +0x70` | right leg `LimbIK*` |
| `LimbIK +0x40` | `_bones Transform[]*` |

Verified three-element limbs: upper/lower/end. Arms: upper arm / forearm /
hand. Legs: thigh / calf / foot. Other array lengths require separate mapping;
do not truncate or guess order.

Clavicle: parent of the corresponding upper-arm Transform. Pelvis: deepest
shared ancestor of left and right thigh Transforms.

`WithWeaponView +0x30` -> ArmedPose/PoseIK view candidate (metadata path).

| Address / field | Value |
| --- | --- |
| `PoseIKView +0x30` | `_bones` |
| `PoseIKView +0x50` | `_boneSet Dictionary<string,Transform>` |

`_boneSet` is a serialized subset, not a complete hierarchy enumeration.

Implementation: `SkeletonExtractor::BuildFromView`.

---

## Native Unity transform

**Live verified**

| Address / field | Value |
| --- | --- |
| `Managed Transform +0x10` | native `Transform*` |
| `Native Transform +0x28` | hierarchy pointer |
| `Native Transform +0x30` | `uint32` transform index |
| `Hierarchy +0x18` | `LocalTRS*` array |
| `Hierarchy +0x20` | `int32*` parent-index array |

`LocalTRS` stride: `0x30`.

| Address / field | Value |
| --- | --- |
| `LocalTRS +0x00` | translation `float4`, xyz used |
| `LocalTRS +0x10` | rotation quaternion `float4`, xyzw |
| `LocalTRS +0x20` | scale `float4`, xyz used |

```text
Parent = I32(parentArray + index*4); -1 ends the chain.
Start p = local.translation.xyz.
For each ancestor: p = Rotate(q, p * scale.xyz) + translation.xyz.
```

Multiplication `p*scale` is componentwise. Parent indices need not be lower
than child indices. Reject cycles, non-finite values, invalid quaternions and
out-of-bounds indices. Revalidate native pointer, hierarchy, index and both
array pointers after capture.

UnityPlayer `get_position` icall RVA: `0x000CCA20`.
Traced native implementation RVAs: `0x006684F0` -> `0x00059AF0`.

Implementation: `SkeletonExtractor` (`Snapshot`, hierarchy walk).

---

## Local inventory

**Live verified**

```text
character = Q(localPlayerEntity + 0x20)
privateModel = Q(character + 0x10)
inventory = Q(privateModel + 0x150)
slotsModel = Q(inventory + 0x18)
```

Resolve `slotsModel._models` on the actual runtime class.
`_models` is `Dictionary<InventorySlotDescription, InventorySlotModel>`.

| Address / field | Value |
| --- | --- |
| `InventorySlotDescription +0x10` | managed string ID |
| `InventorySlotDescription +0x18` | `int32 index` |
| `InventorySlotModel +0x18` | `ItemModel*` |
| `InventorySlotModel +0x20` | `StackModel*` |
| `ItemModel +0x10` | `InventorySlotData*` |
| `StackModel +0x10` | the same `InventorySlotData*` |

`InventorySlotData` inherited `_value`: `+0x18` in the verified inflated class.
`_value` is a packed `uint32 InventorySlot`, not an item-definition pointer.

Implementation: `RobRunInventory::Read`.

---

## Packed inventory slot

**Live verified**

```text
DurabilityBits = 4; PayloadBits = 15; ItemBits = 13
DurabilityShift = 0; PayloadShift = 4; ItemShift = 19
DurabilityMask = 0xF; PayloadMask = 0x7FFF; ItemMask = 0x1FFF
durability = raw & 0xF
payload = (raw >> 4) & 0x7FFF
encodedItem = (raw >> 19) & 0x1FFF
encodedItem == 0: empty slot
Otherwise itemDefinitionIndex = encodedItem - 1
Stackable item quantity = payload, directly; no extra +1
```

Non-stackable `StackModel.Value` returns `1`. Quantity bit field = bits 4..18;
item bit field = bits 19..31.

`InventorySlot.Item` getter RVA: `0x048D1CB0`.
`InventorySlotStackModel.Value` getter RVA: `0x048D1130`.

This is bit-field decoding; no cryptographic decryption was established.

Implementation: `RobRunInventory::DecodeSlot`.

---

## Item catalog and cash

**Live verified**

Resolve class
`Models.ReferencesProject.Descriptions.Inventory.InventoryItemDescriptions`.

| Address / field | Value |
| --- | --- |
| `static_fields +0x00` | item enum object |
| `static_fields +0xA0` | Cash1 definition |

Resolve `enum._items` on its actual runtime class, then enumerate the List
backing array.

| Address / field | Value |
| --- | --- |
| `InventoryItemDescription +0x10` | inherited string `Id` |
| `InventoryItemDescription +0x18` | `int32 Index` |
| `InventoryItemDescription +0x24/+0x28` | `int32` cell dimensions |
| `InventoryItemDescription +0x2C` | `int32 StackSize` |
| `InventoryItemDescription +0x34` | `bool IsStackable` |
| `InventoryItemDescription +0x58` | `int32 InventoryLines` |

`Cash1.Id = cash_1`; `Cash1.Index = 19` decimal; packed encoded item = `20`
decimal. `Cash1.StackSize = 25000` decimal; cells `1x1`; `IsStackable = true`.
A changed client value alone does not establish server persistence.

Implementation: `RobRunInventory::ReadCatalog` (opt-in, bounded class scan).

---

## Item rarity

**Metadata**

| Address / field | Value |
| --- | --- |
| `InventoryItemDescription +0x30` | inline `RarityItem`, `int32` underlying value |

Declared names: `NoRare`, `Common`, `Uncommon`, `Rare`, `Epic`, `Legendary`,
`Exotic`. Numeric constants are absent from the supplied declarations; their
mapping is unresolved.

`RarityItem value__ +0x10` describes the boxed layout. Do not add `+0x10` when
reading the inline field at `itemDefinition +0x30`.

---

## Inventory grid

**Live verified**

| Address / field | Value |
| --- | --- |
| `CharacterInventoryModel +0x10` | `InventoryCellsSizeDescription*` |
| `CharacterInventoryModel +0x20` | backpack `EquipSlotModel*` |
| `CharacterInventoryModel +0x28` | `int32 base lines` |
| `InventoryCellsSizeDescription +0x20` | `int32 columns` |
| `InventoryCellsSizeDescription +0x24` | `int32 maximum lines` |

Backpack `EquipSlotModel`: resolve `ItemModel -> _data -> _value` definition
pointer.

| Address / field | Value |
| --- | --- |
| `Backpack item definition +0x58` | `int32 InventoryLines` |

```text
ActiveLines = min(MaximumLines, BaseLines + BackpackInventoryLines)
```

Implementation: `RobRunInventory::Read`.

---

## Held item and health

**Metadata**

| Address / field | Value |
| --- | --- |
| `BattleCharacterModel +0x170` | `ItemInHandsModel*` |
| `ItemInHandsModel +0x10` | `Current IInventoryItemModel*` |
| `EquipItemModel +0x10` | `_data` wrapper holding an item-definition reference |
| `InventorySlotItemModel +0x10` | `_data` wrapper holding a packed InventorySlot |
| `BattleCharacterModel +0x208` | `PlayerRoleDescription` primitive wrapper |
| `BattleCharacterPrivateModel +0x1B0` | `HealthModel*` |
| `HealthModel +0x10` | `_health` wrapper |
| `HealthModel +0x18` | `_isDead` wrapper |
| `HealthModel +0x20` | `int32 maxAmount` |

Role is a gameplay role; it does not establish administrative permission.
Remote character `PrivateModel` can be null; numeric remote health is not
established.

Implementation: `RobRunReader::ReadHealth`, `ReadRoleAndVehicle`.

---

## World entities

**Metadata**

| Address / field | Value |
| --- | --- |
| `BattleModel +0x510` | `WorldEntitiesModel*` |
| `WorldEntitiesModel +0x20` | containers dictionary |
| `WorldEntitiesModel +0x38` | loots dictionary |
| `WorldEntitiesModel +0x40` | vehicleLoots dictionary |
| `WorldEntitiesModel +0x58` | vehicles dictionary |
| `WorldEntitiesModel +0x60` | cars dictionary |
| `WorldEntitiesModel +0x68` | helicopters dictionary |
| `WorldEntitiesModel +0x98` | staticLootObjects dictionary |
| `WorldEntitiesModel +0xA0` | randomLootObjects dictionary |
| `WorldEntitiesModel +0x1A0` | grenades dictionary |

Additional dictionaries exist at `+0x10` through `+0x8D0`; the commonly used
ones are exposed in `rr::WorldEntitiesModel`.

| Address / field | Value |
| --- | --- |
| `ContainerModel +0x18` | `LocationContainerDescription*` |
| `ContainerModel +0x28` | `PositionModel*`; concrete xyz starts `+0x10` |
| `ContainerModel +0x38` | `IsEmpty` bool primitive wrapper |
| `LootModel +0x20` | `TransformModel*` |
| `StaticLootObjectModel +0x18` | description; description position `V3 +0x30` |
| `RandomLootObjectModel +0x20` | description; description position `V3 +0x24` |
| `VehicleModel +0x28` | transform; description `+0x118` |
| `CarModel +0xC0` | transform; description `+0x78` |
| `HelicopterModel +0x70` | transform; description `+0x48` |
| `VehicleTransformModel +0x10` | position `primitive<Vector3>` wrapper |
| `GrenadeModel +0x18` | description; `+0x30` `GrenadeTransformModel*` |
| `GrenadeTransformModel +0x18` | inline position `V3` |

`TransformModel` scalar position wrappers: `+0x10/+0x18/+0x20`, x/y/z.

`BattleCharacterModel +0x1C8` -> `InVehicleModel*`;
`InVehicleModel +0x10` entityId wrapper, `+0x18` seatSlot wrapper, `+0x20`
vehicleType wrapper. `VehicleSeatData +0x20` Occupied wrapper, `+0x28` EntityId
wrapper. Seat zero has not been established as the driver seat.

Implementation: `RobRunWorld::Read`, `Esp/WorldEsp.cpp`.

---

## Spawn message declarations

**Metadata**

| Message | RVA | Fields |
| --- | --- | --- |
| `SetInventoryItemCheatMessage` | `0x049C5E60` | slotSource `+0x20`, inventorySlot `+0x28`, equipSlot `+0x30`, pocketSlot `+0x38`, item `+0x40`, count `+0x48`, ammo `+0x50`, durability `+0x58`, cumulativePoints `+0x60`, entityId `+0x68` |
| `CheatFillExternalInventoryMessage` | `0x049B0A60` | item `+0x28` |
| `CheatAddCoinsMessage` | `0x049823E0` | amount `+0x20` |

These declarations do not establish an accepted server command or live
invocation path. The overlay is read-only and does not call them.

---

## Threading model

- `RobRunReader::Init` starts a dedicated read worker. All DMA access for the
  overlay happens on that thread.
- Each pass builds a complete `Snapshot` (entities, world, inventory, camera,
  bones, fallback view-projection) and publishes it under a mutex as a
  `shared_ptr<const Snapshot>`.
- The render thread calls `GetSnapshot()` once per draw and only projects
  already-resolved data. It never performs a DMA read, a class-name scan, or a
  GameObjectManager walk.
- Per pass the worker reads each entity's transform block (position, pitch,
  yaw) with one uncached read, then samples the cached rig layout with one
  uncached read of the animation matrix array. Health, role, vehicle and
  identity are refreshed every `500 ms`; world markers every `250 ms`;
  inventory every `1000 ms`.
- A rig layout (matrix array, parent table, selected bones) is resolved once
  per entity view and revalidated every `2 s`. This replaced the old
  per-refresh walk of class names and pointer chains, which issued hundreds of
  one-byte reads per character.
- `Shutdown` stops and joins the worker; the attach thread is joined before the
  reader so `Init`/`Shutdown` cannot race.

---

## Read consistency and session recovery

- Require exact-byte reads; discard partial reads.
- The PE fingerprint (`TimeDateStamp` + `SizeOfImage`) is the hard gate.
  Accessor-byte signatures are advisory: a mismatch is surfaced but does not
  force repeated re-resolution on its own.
- Validate class pointers, scene roots, dictionary versions and native
  Transform identities.
- Static pointer chains use `Memory::Read` (VMM data cache, `dwFlags = 0`).
- Fast-changing values use `Memory::ReadFresh`
  (`VMMDLL_FLAG_NOCACHE`): the per-entity transform block, the animation matrix
  array and the camera block. Uncached reads are only cheap when there are few
  of them, so they are limited to one or two reads per entity per pass.
- Earlier builds issued one uncached USB transaction per 8-byte field, which is
  what made a pass take seconds. The fix is the rig-layout cache plus cadenced
  detail reads, not routing everything through the cache.
- VMM background refresh remains enabled; `-norefresh` is omitted.
  `VMMDLL_OPT_CONFIG_TICK_PERIOD = 100`, `READCACHE_TICKS = 2` (200 ms) and
  `TLBCACHE_TICKS = 20` (2 s). `VMMDLL_OPT_REFRESH_FREQ_*` are one-shot refresh
  commands, not interval settings.
- The procinfo/CR3 plugin wait is bounded to `30 s`; a missing `info.db` or
  symbols can no longer hang the attach thread.

```text
Tick period:        VMMDLL_OPT_CONFIG_TICK_PERIOD      = 0x2000000400000000
Read cache ticks:   VMMDLL_OPT_CONFIG_READCACHE_TICKS  = 0x2000000500000000
TLB cache ticks:    VMMDLL_OPT_CONFIG_TLBCACHE_TICKS   = 0x2000000600000000
Manual mem refresh: VMMDLL_OPT_REFRESH_FREQ_MEM        = 0x2001100000000000
Manual TLB refresh: VMMDLL_OPT_REFRESH_FREQ_TLB        = 0x2001080000000000
Specific process:   VMMDLL_OPT_REFRESH_SPECIFIC_PROCESS = 0x2002000300000000 | PID
```

- Scene retry delay: `100 ms` increasing to `1000 ms`.
- Persistent sample failures for `5000 ms` trigger a full VMM session reopen.
- Failed reconnects retry after `2000 ms`.
- Game object search: 8-byte class-pointer pattern aligned to 8 bytes.
- Filter: private, non-stack, non-TEB RW/write-copy candidate regions under
  `256 MiB`.
- Require exactly one candidate with `Game.Game` and `Game.GameModel` class
  identities.
- Revalidate the class table after refreshing mappings.

Implementation: `Il2Cpp::VerifyFingerprint`, `VerifyAccessors`,
`RobRunReader::RevalidateSession`.

---

## Module-relative metadata slots

```text
class_table_slot     = GameAssembly.dll + 0x8625440
metadata_header_slot = GameAssembly.dll + 0x8625550

class = Q(Q(GameAssemblyBase + class_table_slot) + classIndex*8)
```

**AOB fallback (`Il2Cpp::ClassTableSlotRva`).** If the hardcoded slot no longer
dereferences to a table of printable `Il2CppClass` pointers, the resolver scans
`.text` for the `GetClassByIndex` accessor and decodes its RIP-relative slot:

```text
48 63 C8 48 8B 05 ?? ?? ?? ?? 48 8B 04 C8 C3
```

This signature is unique in the supported build and resolves to
`0x8625440`. The scan is chunked (1 MiB), cached for the session, and only runs
when the header slot fails validation. `RobRunReader` then falls back to
`FindClassByName("BattleModel")` / `FindClassByName("BattleEntitiesUpdater")`
plus the class static field, so a shifted TypeInfo RVA no longer blocks
attach. That fallback is rate-limited to one attempt every three seconds.

---

## Loaded class indices

**Capture-specific**

| Class | Index (decimal) |
| --- | --- |
| `Models.Models.BattleCharacter.BattleCharacterModel` | 4309 |
| `Models.Models.TransformModel.CharacterTransformModel` | 3426 |
| `Game.Battle.BattleEntitiesModel` | 5510 |
| `Game.Battle.BattleEntitiesUpdater` | 5517 |
| `Game.Battle.BattleModel` | 5520 |
| `Game.Battle.Entities.Player.Camera.BattleCameraModel` | 7868 |
| `Game.Battle.EntityModel.Player.PlayerEntityModel` | 8692 |
| `Game.Battle.EntityModel.BattleCharacter.BattleCharacterEntityModel` | 9036 |
| `Game.Game` | 29335 |
| `Game.GameModel` | 29336 |
| `Cinemachine.CinemachineBrain` | 29521 |
| `Cinemachine.CinemachineCore` | 29618 |

---

## Verified accessor signatures

| Accessor | RVA | Bytes |
| --- | --- | --- |
| Loaded-class-table lookup | `0x6E7360` | `4c 8b 05 e9 e1 f3 07 49 63 80 ec 00 00 00 48 2b c8 ...` |
| Fields iterator | `0x72BEE0` | `48 89 5c 24 08 57 48 83 ec 20 48 8b fa 48 8b d9 ...` |
| Methods iterator | `0x72CAE9` | `66 83 bb 20 01 00 00 00 74 3c 48 8b 83 98 00 00 00 ...` |
| Class name | `0x6460` | `48 8b 41 10 c3` |
| Class namespace / method name | `0x6470` | `48 8b 41 18 c3` |
| Static fields | `0x2110` | `48 8b 81 b8 00 00 00 c3` |
| Instance size | `0x72C5A0` | `8b 81 f8 00 00 00 c3` |
| Class flags | `0x6F9590` | `8b 81 18 01 00 00 c3` |
| Field offset | `0x6DD930` | `48 63 41 18 c3` |
| Field type | `0x2B8210` | `48 8b 41 08 c3` |
| Method token | `0x3ABE0` | `8b 41 48 c3` |
| Method param count | `0x6E6BB0` | `0f b6 41 52 c3` |
| Method return type | `0xCA60` | `48 8b 41 28 c3` |
| Type code | `0x73E360` | `0f be 41 0a c3` |

`Il2Cpp::VerifyAccessors` checks the short accessor prefixes above against the
live module. Full signature strings for the longer functions are preserved in
the source reference.

---

## Renderer / customization declarations

**Metadata**

Source assemblies: `Models.dll`, `Battle.dll`, `Global.dll`.

Renderer references do not establish native Mesh buffer, skin-weight,
bind-pose or material layouts. Native renderer/mesh extraction and an
end-to-end character customization root remain unverified. The relevant
declaration dumps (`LodCharacterView`, `CharacterCustomizationView`,
`BattleCharacterCustomizationView`) are not used by this overlay.

---

## Implementation map

| File | Responsibility |
| --- | --- |
| `VeoxDMA/SDK/RobRunSDK.h` | Canonical build-pinned offsets and constants |
| `VeoxDMA/SDK/Il2Cpp.h/.cpp` | Class table, field walk, primitive wrappers, fingerprint checks |
| `VeoxDMA/SDK/RobRunMemory.h/.cpp` | UTF-16 strings, lists, stable dictionary snapshots |
| `VeoxDMA/SDK/RobRunReader.h/.cpp` | Game roots, updater discovery, entities, slots, health, role |
| `VeoxDMA/SDK/RobRunCamera.h/.cpp` | BattleCameraModel, Cinemachine FOV, view-projection |
| `VeoxDMA/SDK/Inventory.h/.cpp` | Packed slots, local inventory, grid, optional catalog |
| `VeoxDMA/SDK/WorldEntities.h/.cpp` | Containers, loot, vehicles, cars, helicopters, grenades |
| `VeoxDMA/SDK/SkeletonExtractor.cpp` | Native Transform hierarchy and rig anchors |
| `VeoxDMA/Esp/PlayerEsp.cpp` | Skeleton, box, health bar, name/clan/distance |
| `VeoxDMA/Esp/WorldEsp.cpp` | World-entity labels and markers |
| `VeoxDMA/Esp/EspDraw.cpp` | Outlined text, boxes, stacked labels/bars |
| `VeoxDMA/Overlay/*` | D3D11 overlay, notifications, monitor selection |
| `VeoxDMA/Config/*` | Player, overlay and world ESP settings |

## Known limits

- Remote `BattleCharacterPrivateModel` may be null, so remote numeric health
  can legitimately be unknown.
- Rarity numeric constants are unresolved; the inline `+0x30` value is read
  but not named.
- World-entity offsets are metadata-derived and therefore gated by pointer,
  class and finite-value validation; verify them live before relying on them.
- The item catalog resolver performs a bounded, cached class-name scan and is
  opt-in from the Misc tab.
- Spawn messages are documented only. No write path or server command is
  implemented.
