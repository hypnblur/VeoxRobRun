#pragma once
#include "pch.h"
#include <map>

// Cosmetic skin changer settings (see SDK/SkinChanger.h). Items are stored by
// the game's id string, per slot and separately for male and female
// characters, and applied automatically in every match while Enabled is on.
// An empty id keeps the real item.
class SkinConfig
{
    std::string ConfigName;

public:
    // Must match SkinChanger::kSlotCount and its slot order.
    static constexpr int kSlots = 4;
    static constexpr const char* kSlotKeys[kSlots] = { "Outfit", "Mask", "Bag", "Keychain" };

    SkinConfig(const std::string& name) { ConfigName = name; }

    bool Enabled = false;
    std::string Male[kSlots];
    std::string Female[kSlots];
    // Weapon type (skin id prefix, e.g. "rc_556") -> chosen skin id.
    std::map<std::string, std::string> WeaponSkins;

    json ToJson()
    {
        json j;
        j[ConfigName][LIT("Enabled")] = Enabled;
        for (int i = 0; i < kSlots; ++i) {
            j[ConfigName][std::string("Male") + kSlotKeys[i]] = Male[i];
            j[ConfigName][std::string("Female") + kSlotKeys[i]] = Female[i];
        }
        json weapons = json::object();
        for (const auto& [weapon, skin] : WeaponSkins)
            if (!skin.empty()) weapons[weapon] = skin;
        j[ConfigName][LIT("WeaponSkins")] = weapons;
        return j;
    }

    void FromJson(const json& j)
    {
        if (!j.contains(ConfigName))
            return;
        Enabled = j[ConfigName].value(LIT("Enabled"), Enabled);
        for (int i = 0; i < kSlots; ++i) {
            Male[i] = j[ConfigName].value(std::string("Male") + kSlotKeys[i], Male[i]);
            Female[i] = j[ConfigName].value(std::string("Female") + kSlotKeys[i], Female[i]);
        }
        WeaponSkins.clear();
        if (j[ConfigName].contains(LIT("WeaponSkins")) && j[ConfigName][LIT("WeaponSkins")].is_object())
            for (const auto& [weapon, skin] : j[ConfigName][LIT("WeaponSkins")].items())
                if (skin.is_string()) WeaponSkins[weapon] = skin.get<std::string>();
    }
};
