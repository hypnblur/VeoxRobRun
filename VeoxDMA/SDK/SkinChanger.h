#pragma once
#include "pch.h"
#include <string>
#include <vector>

namespace SkinChanger {

inline constexpr int kSlotCount = 4;

struct SlotState {
    bool supported = false;
    bool error = false;
    std::string real;
    std::string realName;
    std::string status;
    std::vector<std::string> options;
    std::vector<std::string> names;
};

struct WeaponState {
    std::string key;
    std::string name;
    std::string status;
    bool error = false;
    std::vector<std::string> skins;
    std::vector<std::string> names;
};

struct State {
    bool inMatch = false;
    bool female = false;
    bool error = false;
    std::string status;
    SlotState slots[kSlotCount];
    bool weaponsSupported = false;
    std::string heldWeapon;
    std::string heldSkin;
    std::vector<WeaponState> weapons;
};

const char* SlotName(int slot);
std::string Label(int slot, const std::string& id);
State GetState();
std::string Wanted(int slot, bool female);
void SetWanted(int slot, bool female, const std::string& id);
std::string WantedWeaponSkin(const std::string& weaponKey);
void SetWantedWeaponSkin(const std::string& weaponKey, const std::string& skinId);
void Start();
void Stop();

} // namespace SkinChanger
