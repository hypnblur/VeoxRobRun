#pragma once
#include "pch.h"
#include "../SDK/SkinChanger.h"

class SkinConfig
{
    std::string ConfigName;

public:
    SkinConfig(const std::string& name) { ConfigName = name; }

    bool Enabled = false;

    // 1 = male, 2 = female. Determines which outfit catalog + snapshot offsets.
    int Gender = 1;

    // Set to true from the menu to trigger an immediate apply.
    bool ApplyNow = false;

    // Runtime status: 0 = idle, 1 = applying, 2 = applied, -1 = error
    int Status = 0;

    // 0 = no override (keep game default). 1+ = index into the weapon's skin array.
    int WeaponSkin[SkinChanger::kWeaponCount] = {};

    // Cosmetic selections (same 0 = off, 1+ = catalog index).
    int OutfitMale   = 0;
    int OutfitFemale = 0;
    int MaskSkin     = 0;
    int BagSkin      = 0;
    int KeychainSkin = 0;

    json ToJson()
    {
        json j;
        j[ConfigName][LIT("Enabled")] = Enabled;
        j[ConfigName][LIT("Gender")]  = Gender;

        json ws = json::array();
        for (int i = 0; i < SkinChanger::kWeaponCount; ++i)
            ws.push_back(WeaponSkin[i]);
        j[ConfigName][LIT("WeaponSkin")] = ws;

        j[ConfigName][LIT("OutfitMale")]   = OutfitMale;
        j[ConfigName][LIT("OutfitFemale")] = OutfitFemale;
        j[ConfigName][LIT("MaskSkin")]     = MaskSkin;
        j[ConfigName][LIT("BagSkin")]      = BagSkin;
        j[ConfigName][LIT("KeychainSkin")] = KeychainSkin;
        return j;
    }

    void FromJson(const json& j)
    {
        if (!j.contains(ConfigName))
            return;
        Enabled      = j[ConfigName].value(LIT("Enabled"), Enabled);
        Gender       = j[ConfigName].value(LIT("Gender"), Gender);
        OutfitMale   = j[ConfigName].value(LIT("OutfitMale"), OutfitMale);
        OutfitFemale = j[ConfigName].value(LIT("OutfitFemale"), OutfitFemale);
        MaskSkin     = j[ConfigName].value(LIT("MaskSkin"), MaskSkin);
        BagSkin      = j[ConfigName].value(LIT("BagSkin"), BagSkin);
        KeychainSkin = j[ConfigName].value(LIT("KeychainSkin"), KeychainSkin);

        if (j[ConfigName].contains(LIT("WeaponSkin"))) {
            const auto& arr = j[ConfigName][LIT("WeaponSkin")];
            for (int i = 0; i < SkinChanger::kWeaponCount && i < (int)arr.size(); ++i)
                WeaponSkin[i] = arr[i].get<int>();
        }
    }
};
