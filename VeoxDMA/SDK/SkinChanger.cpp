#include "pch.h"
#include "SkinChanger.h"
#include "RobRunSDK.h"
#include "RobRunMemory.h"
#include "RobRunReader.h"
#include "../Config/ConfigInstance.h"
#include "../Memory/Memory.h"
#include <cstring>

extern Memory TargetProcess;

namespace SkinChanger {

using namespace RobRunMemory;

static std::uintptr_t g_gaBase = 0;

#define PL(p) ((p) >= 0x10000ULL && (p) < 0x7FFFFFFFFFFFULL)

// ---- Watchdog state (re-applies skin when game reverts) -----------------

struct SkinWatchdog {
    bool active = false;
    bool isFemale = false;
    char skinId[64] = {};
    std::uintptr_t description = 0;
    std::uintptr_t snapshot = 0;
    std::uintptr_t bodyOff = 0;
    std::uintptr_t loadingModel = 0;
    std::uintptr_t custModel = 0;
    std::uintptr_t verModel = 0;
    DWORD lastApplyTime = 0;
    int restoreCount = 0;
};

static SkinWatchdog g_watchdog;

// ---- DMA read/write helpers ---------------------------------------------

static std::uintptr_t RPtr(std::uintptr_t addr) { return ReadPtr(addr); }
static std::int32_t RI32(std::uintptr_t addr) { return ReadInt32(addr); }

static bool WPtr(std::uintptr_t addr, std::uintptr_t val) {
    return TargetProcess.Write(addr, &val, sizeof(val));
}
static bool WI32(std::uintptr_t addr, std::int32_t val) {
    return TargetProcess.Write(addr, &val, sizeof(val));
}
static bool WU8(std::uintptr_t addr, std::uint8_t val) {
    return TargetProcess.Write(addr, &val, sizeof(val));
}

// ---- IL2CPP string read -------------------------------------------------

static bool ReadIl2CppString(std::uintptr_t strObj, char* out, int maxLen) {
    if (!PL(strObj) || maxLen <= 0) return false;
    std::wstring ws;
    if (!ReadString(strObj, ws, maxLen)) return false;
    for (int i = 0; i < (int)ws.size() && i < maxLen - 1; ++i)
        out[i] = (char)ws[i];
    out[std::min((int)ws.size(), maxLen - 1)] = '\0';
    return true;
}

// ---- Core: look up skin by string ID via References chain ---------------

static std::uintptr_t GetByIdReferences(bool isFemale, const char* wantedId) {
    if (!PL(g_gaBase) || !wantedId || !*wantedId) return 0;

    const auto klass = RPtr(g_gaBase + rr::TypeInfo::GameModels);
    if (!PL(klass)) return 0;

    const auto staticFields = RPtr(klass + Off::IL2CPP_STATIC_FIELDS);
    if (!PL(staticFields)) return 0;

    const auto references = RPtr(staticFields + Off::GAME_MODELS_REFERENCES);
    if (!PL(references)) return 0;

    const auto catOffset = isFemale ? Off::REFERENCES_CHARACTER_FBODIES
                                    : Off::REFERENCES_CHARACTER_BODIES;
    const auto readOnlyEnum = RPtr(references + catOffset);
    if (!PL(readOnlyEnum)) return 0;

    const auto itemsList = RPtr(readOnlyEnum + Off::ENUM_ITEMS_LIST);
    if (!PL(itemsList)) return 0;

    const auto items = RPtr(itemsList + Off::LIST_ITEMS_ARRAY);
    const auto count = RI32(itemsList + Off::LIST_ITEMS_COUNT);
    if (!PL(items) || count <= 0 || count > 2048) return 0;

    for (int i = 0; i < count; ++i) {
        const auto desc = RPtr(items + Off::ARRAY_DATA_START + (std::uintptr_t)i * 8);
        if (!PL(desc)) continue;

        const auto idStr = RPtr(desc + Off::IDENTIFIED_ID_STRING);
        if (!PL(idStr)) continue;

        char itemId[128] = {};
        if (ReadIl2CppString(idStr, itemId, sizeof(itemId)) && itemId[0]) {
            if (_stricmp(itemId, wantedId) == 0)
                return desc;
        }
    }
    return 0;
}

// ---- Apply description to a CustomizationModel --------------------------

static bool ApplyDescriptionToModel(std::uintptr_t model, bool isFemale, std::uintptr_t desc) {
    if (!PL(model)) return false;

    const auto custData = RPtr(model + 0x10);
    if (PL(custData)) {
        const auto primOff = isFemale ? (std::uintptr_t)0xB0 : (std::uintptr_t)0x30;
        const auto prim = RPtr(custData + primOff);
        if (PL(prim)) {
            WPtr(prim + Off::TEMP_ENUM_VALUE_OBJ, desc);
            return true;
        }
    }

    const auto tempOff = isFemale ? Off::CHARTEMP_FBODY : Off::CHARTEMP_BODY;
    const auto tempEnum = RPtr(model + tempOff);
    if (PL(tempEnum)) {
        WPtr(tempEnum + Off::TEMP_ENUM_VALUE_OBJ, desc);
        auto ev = RI32(tempEnum + Off::TEMP_VERSION_INT);
        WI32(tempEnum + Off::TEMP_VERSION_INT, ev == INT_MAX ? 0 : ev + 1);
        return true;
    }
    return false;
}

// ---- Bump version on a VersionModel -------------------------------------

static void BumpVersionModel(std::uintptr_t verModel) {
    if (!PL(verModel)) return;
    const auto data = RPtr(verModel + 0x10);
    if (!PL(data)) return;
    auto cur = RI32(data + 0x18);
    WI32(data + 0x18, cur == INT_MAX ? 0 : cur + 1);
}

// ---- Update all battle models for local player --------------------------

static void UpdateAllBattleModels(std::uintptr_t battle, std::int32_t localId,
                                   bool isFemale, std::uintptr_t desc) {
    if (!PL(battle) || localId == 0) return;

    // PlayerEntityModel path
    const auto playerEnt = RPtr(battle + rr::BattleEntitiesModel::LocalPlayerEntityModel);
    if (PL(playerEnt)) {
        const auto serverModel = RPtr(playerEnt + rr::PlayerEntityModel::Server);
        if (PL(serverModel)) {
            ApplyDescriptionToModel(RPtr(serverModel + 0x230), isFemale, desc);
            BumpVersionModel(RPtr(serverModel + 0x238));
        }
    }

    // WorldEntitiesModel._battleCharacters dict
    const auto wem = RPtr(battle + 0x60);
    if (PL(wem)) {
        std::vector<RobRunMemory::DictEntry> entries;
        const auto charsDict = RPtr(wem + 0x18);
        if (PL(charsDict) && ReadDictInt(charsDict, entries, false)) {
            for (const auto& e : entries) {
                if (e.key == localId && PL(e.value)) {
                    ApplyDescriptionToModel(RPtr(e.value + 0x230), isFemale, desc);
                    BumpVersionModel(RPtr(e.value + 0x238));
                    break;
                }
            }
        }
    }

    // BattleModel.EntityModelDict (models at +0x50)
    std::vector<RobRunMemory::DictEntry> modelEntries;
    const auto modelsDict = RPtr(battle + Off::BATTLE_MODELS_DICT);
    if (PL(modelsDict) && ReadDictInt(modelsDict, modelEntries, false)) {
        for (const auto& e : modelEntries) {
            if (e.key == localId && PL(e.value)) {
                const auto sModel = RPtr(e.value + Off::BATTLE_CHAR_SERVER_MODEL);
                const auto simModel = RPtr(e.value + Off::BATTLE_CHAR_SIMULATED_MODEL);
                if (PL(sModel)) {
                    ApplyDescriptionToModel(RPtr(sModel + 0x230), isFemale, desc);
                    BumpVersionModel(RPtr(sModel + 0x238));
                }
                if (PL(simModel)) {
                    ApplyDescriptionToModel(RPtr(simModel + 0x230), isFemale, desc);
                    BumpVersionModel(RPtr(simModel + 0x238));
                }
                const auto pServer = RPtr(e.value + Off::BATTLE_CHAR_CLIENT_MODEL);
                if (PL(pServer)) {
                    ApplyDescriptionToModel(RPtr(pServer + 0x230), isFemale, desc);
                    BumpVersionModel(RPtr(pServer + 0x238));
                }
                break;
            }
        }
    }
}

// ---- Update CharacterCustomizationViewUpdater ---------------------------

static bool UpdateCustomizationUpdater(std::uintptr_t viewsUpdater, std::int32_t localId,
                                        bool isFemale, std::uintptr_t desc,
                                        std::uintptr_t* outCust, std::uintptr_t* outVer) {
    if (!PL(viewsUpdater) || localId == 0) return false;

    std::uintptr_t dicts[2] = { RPtr(viewsUpdater + 0x38), RPtr(viewsUpdater + 0x30) };
    for (int d = 0; d < 2; ++d) {
        if (!PL(dicts[d])) continue;
        std::vector<DictEntry> entries;
        if (!ReadDictInt(dicts[d], entries, false)) continue;

        for (const auto& e : entries) {
            if (e.key != localId || !PL(e.value)) continue;

            const auto list = RPtr(e.value + 0x10);
            if (!PL(list)) continue;
            const auto items = RPtr(list + 0x10);
            const auto count = RI32(list + 0x18);
            if (!PL(items) || count <= 0 || count > 100) continue;

            for (int j = 0; j < count; ++j) {
                const auto updater = RPtr(items + 0x20 + (std::uintptr_t)j * 8);
                if (!PL(updater)) continue;

                const auto modelUpdater = RPtr(updater + 0x10);
                if (!PL(modelUpdater)) continue;

                const auto cModel = RPtr(modelUpdater + 0x18);
                const auto vModel = RPtr(modelUpdater + 0x20);
                if (PL(cModel)) {
                    ApplyDescriptionToModel(cModel, isFemale, desc);
                    if (outCust) *outCust = cModel;
                }
                if (PL(vModel)) {
                    BumpVersionModel(vModel);
                    if (outVer) *outVer = vModel;
                }
                return true;
            }
        }
    }
    return false;
}

// ---- Battle apply (in-match skin change) --------------------------------

static int ApplyBattle(bool isFemale, const char* wantedId) {
    if (!PL(g_gaBase) || !wantedId || !*wantedId)
        return 0;

    const auto snap = RobRunReader::GetSnapshot();
    if (!snap || !PL(snap->battleModel))
        return 0;

    const auto battle = RPtr(snap->battleModel + rr::BattleModel::BattleEntitiesModel);
    if (!PL(battle))
        return 0;

    // Resolve local player
    std::uintptr_t clientModel = 0;
    std::uintptr_t localServer = 0;
    std::uintptr_t localSim = 0;
    std::int32_t localId = snap->localEntityId;

    const auto playerEnt = RPtr(battle + rr::BattleEntitiesModel::LocalPlayerEntityModel);
    if (PL(playerEnt)) {
        clientModel = RPtr(playerEnt + rr::PlayerEntityModel::Client);
        localServer = RPtr(playerEnt + rr::PlayerEntityModel::Server);
        if (!localId && PL(localServer))
            localId = RI32(localServer + Off::CHAR_ID);
    }

    // Fallback: walk EntityModelDict
    if (localId != 0) {
        std::vector<DictEntry> entries;
        const auto modelsDict = RPtr(battle + Off::BATTLE_MODELS_DICT);
        if (PL(modelsDict) && ReadDictInt(modelsDict, entries, false)) {
            for (const auto& e : entries) {
                if (e.key == localId && PL(e.value)) {
                    if (!PL(clientModel)) clientModel = RPtr(e.value + Off::BATTLE_CHAR_CLIENT_MODEL);
                    if (!PL(localServer)) localServer = RPtr(e.value + Off::BATTLE_CHAR_SERVER_MODEL);
                    localSim = RPtr(e.value + Off::BATTLE_CHAR_SIMULATED_MODEL);
                    break;
                }
            }
        }
    }

    if (!PL(clientModel))
        return 0;

    // Detect gender from snapshot class
    const auto maleLM = RPtr(clientModel + Off::CHAR_MALE_SKIN_LOADING_MODEL);
    const auto femaleLM = RPtr(clientModel + Off::CHAR_FEMALE_SKIN_LOADING_MODEL);
    const auto maleSnap = PL(maleLM) ? RPtr(maleLM + Off::SKIN_LOADING_CURRENT_VALUE) : 0;
    const auto femaleSnap = PL(femaleLM) ? RPtr(femaleLM + Off::SKIN_LOADING_CURRENT_VALUE) : 0;

    // Pick the correct loading model
    const auto loadingModelOff = isFemale ? Off::CHAR_FEMALE_SKIN_LOADING_MODEL
                                          : Off::CHAR_MALE_SKIN_LOADING_MODEL;
    const auto loadingModel = RPtr(clientModel + loadingModelOff);
    if (!PL(loadingModel))
        return 0;

    const auto snapshot = RPtr(loadingModel + Off::SKIN_LOADING_CURRENT_VALUE);
    if (!PL(snapshot))
        return 0;

    // Find description via References chain
    auto selectedDesc = GetByIdReferences(isFemale, wantedId);
    if (!PL(selectedDesc))
        return 0;

    // Update all battle model copies
    UpdateAllBattleModels(battle, localId, isFemale, selectedDesc);

    // Resolve BattleEntitiesUpdater via TypeInfo singleton
    std::uintptr_t updCust = 0, updVer = 0;
    std::uintptr_t viewsUpdater = 0;
    {
        const auto typeInfo = RPtr(g_gaBase + rr::TypeInfo::BattleEntitiesUpdater);
        if (PL(typeInfo)) {
            const auto statics = RPtr(typeInfo + Off::IL2CPP_STATIC_FIELDS);
            if (PL(statics)) {
                const auto inst = RPtr(statics);
                if (PL(inst))
                    viewsUpdater = inst;
            }
        }
    }
    if (PL(viewsUpdater) && localId != 0)
        UpdateCustomizationUpdater(viewsUpdater, localId, isFemale, selectedDesc, &updCust, &updVer);

    // Write to server/sim customization models
    if (PL(localServer)) {
        auto custModel = RPtr(localServer + 0x230);
        if (PL(custModel))
            ApplyDescriptionToModel(custModel, isFemale, selectedDesc);
        BumpVersionModel(RPtr(localServer + 0x238));
    }
    if (PL(localSim)) {
        auto simCust = RPtr(localSim + 0x230);
        if (PL(simCust))
            ApplyDescriptionToModel(simCust, isFemale, selectedDesc);
        BumpVersionModel(RPtr(localSim + 0x238));
    }

    // Write description into snapshot + bump versions
    const auto bodyDescOff = isFemale ? Off::SNAPSHOT_FEMALE_FBODY : Off::SNAPSHOT_MALE_BODY;
    if (!WPtr(snapshot + bodyDescOff, selectedDesc))
        return 0;

    // Verify write
    if (RPtr(snapshot + bodyDescOff) != selectedDesc)
        return 0;

    auto snapVer = RI32(snapshot + Off::SNAPSHOT_VERSION);
    WI32(snapshot + Off::SNAPSHOT_VERSION, snapVer == INT_MAX ? 0 : snapVer + 1);

    auto loadVer = RI32(loadingModel + Off::SKIN_LOADING_VERSION);
    WI32(loadingModel + Off::SKIN_LOADING_VERSION, loadVer == INT_MAX ? 0 : loadVer + 1);

    // Reset CharacterCustomizationView.isCustomized to force visual refresh
    if (PL(viewsUpdater) && localId != 0) {
        std::vector<DictEntry> viewEntries;
        const auto viewDict = RPtr(viewsUpdater + Off::VIEWS_CHARACTERS);
        if (PL(viewDict) && ReadDictInt(viewDict, viewEntries, false)) {
            for (const auto& e : viewEntries) {
                if (e.key == localId && PL(e.value)) {
                    auto custView = RPtr(e.value + Off::ENTITY_VIEW_CUSTOMIZATION_VIEW);
                    if (!PL(custView)) {
                        const auto battleCustView = RPtr(e.value + 0xC0);
                        if (PL(battleCustView))
                            custView = RPtr(battleCustView + 0x20);
                    }
                    if (PL(custView))
                        WU8(custView + Off::CUSTVIEW_IS_CUSTOMIZED, 0);
                    break;
                }
            }
        }
    }

    // Arm watchdog for passive re-apply
    g_watchdog.active = true;
    g_watchdog.isFemale = isFemale;
    strncpy_s(g_watchdog.skinId, wantedId, _TRUNCATE);
    g_watchdog.description = selectedDesc;
    g_watchdog.snapshot = snapshot;
    g_watchdog.bodyOff = bodyDescOff;
    g_watchdog.loadingModel = loadingModel;
    g_watchdog.custModel = updCust ? updCust
        : (PL(localSim) ? RPtr(localSim + 0x230) : (PL(localServer) ? RPtr(localServer + 0x230) : 0));
    g_watchdog.verModel = updVer ? updVer
        : (PL(localSim) ? RPtr(localSim + 0x238) : (PL(localServer) ? RPtr(localServer + 0x238) : 0));
    g_watchdog.lastApplyTime = GetTickCount();
    g_watchdog.restoreCount = 0;

    return 1;
}

// ---- Watchdog passive restore -------------------------------------------

static void WatchdogTick(bool isFemale) {
    if (!g_watchdog.active || !g_watchdog.description || !g_watchdog.snapshot)
        return;

    if (g_watchdog.isFemale != isFemale) {
        g_watchdog.active = false;
        return;
    }

    DWORD now = GetTickCount();
    if (now - g_watchdog.lastApplyTime < 800)
        return;

    auto curDesc = RPtr(g_watchdog.snapshot + g_watchdog.bodyOff);
    if (curDesc != g_watchdog.description) {
        g_watchdog.restoreCount++;

        WPtr(g_watchdog.snapshot + g_watchdog.bodyOff, g_watchdog.description);

        auto sv = RI32(g_watchdog.snapshot + Off::SNAPSHOT_VERSION);
        WI32(g_watchdog.snapshot + Off::SNAPSHOT_VERSION, sv == INT_MAX ? 0 : sv + 1);

        if (PL(g_watchdog.custModel))
            ApplyDescriptionToModel(g_watchdog.custModel, g_watchdog.isFemale, g_watchdog.description);
        if (PL(g_watchdog.verModel))
            BumpVersionModel(g_watchdog.verModel);

        g_watchdog.lastApplyTime = now;
    }
}

// ---- Public API ---------------------------------------------------------

void Init(std::uintptr_t gameAssemblyBase) {
    g_gaBase = gameAssemblyBase;
    g_watchdog = {};
}

void Shutdown() {
    g_gaBase = 0;
    g_watchdog = {};
}

void Tick() {
    if (!g_gaBase || !ConfigInstance.Skin.Enabled)
        return;

    const bool isFemale = (ConfigInstance.Skin.Gender == 2);

    // Manual apply triggered from menu
    if (ConfigInstance.Skin.ApplyNow) {
        ConfigInstance.Skin.ApplyNow = false;

        const SkinEntry* entry = nullptr;

        // Check outfit selection based on gender
        int sel = isFemale ? ConfigInstance.Skin.OutfitFemale : ConfigInstance.Skin.OutfitMale;
        if (sel > 0) {
            const auto& cat = isFemale ? kCosmeticCatalogs[OutfitFemale]
                                       : kCosmeticCatalogs[OutfitMale];
            int idx = sel - 1;
            if (idx < cat.count)
                entry = &cat.skins[idx];
        }

        if (entry) {
            int result = ApplyBattle(isFemale, entry->field);
            if (result == 1) {
                ConfigInstance.Skin.Status = 2;
                return;
            }
        }

        // If no outfit selected or apply failed, try retry later
        static int retries = 0;
        retries++;
        if (retries >= 20) {
            retries = 0;
            ConfigInstance.Skin.Status = -1;
            g_watchdog.active = false;
        }
        return;
    }

    // Watchdog passive restore
    WatchdogTick(isFemale);
}

} // namespace SkinChanger
