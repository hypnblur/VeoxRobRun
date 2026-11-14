#pragma once
#include "pch.h"
class PlayerConfig
{
	std::string ConfigName;

public:
	PlayerConfig(const std::string& name)
	{
		ConfigName = name;
	}

	bool BoxEsp = true;
	bool Health = true;
	bool Distance = true;
	int MaxDistance = 1000;
	bool PrimaryWeapon = true;
	bool SecondaryWeapon = false;
	D2D1::ColorF TextColour = Colour(255, 255, 255);
	int FontSize = 11;
	// Camera occlusion pass over the city colliders: each bone is ray-cast
	// from the camera, so only geometry blocks it. Without the BVH the reader
	// falls back to the animator/cull state (see the menu note).
	bool VisCheck = false;
	D2D1::ColorF VisibleColour = Colour(255, 50, 50);  // unoccluded from camera
	D2D1::ColorF HiddenColour = Colour(255, 255, 255); // occluded / culled
	bool WallCheck = false;
	std::string BvhPath = "city_colliders.bvh";
	// Project the ESP with the Unity render camera instead of the managed
	// BattleCameraModel block. The managed block is read *after* memory aim
	// writes its angles, so it already matches the frame being rendered; the
	// native matrices are the previous rendered frame and visibly trail when
	// the camera moves, so this stays off by default.
	bool NativeEspCamera = false;
	bool WeaponEsp = true;
	bool LookingAtYou = true;
	bool LookingAlert = true;
	bool ShowRole = true;
	// Colour the role tag per role instead of using RoleColour.
	bool ExpRoleColors = false;
	// Compact overlay with coin/crypto balances and bag value, fed by the
	// inventory worker.
	bool ExpMoneyHud = true;
	bool ShowFiring = true;
	bool ShowReloading = true;
	bool ShowArmor = true;
	bool ShowArmorValue = true;
	bool ShowStamina = true;
	bool SpectatorAlert = true;
	bool WeaponSprite = true;

	D2D1::ColorF NameColour = Colour(255, 255, 255);
	D2D1::ColorF DistanceColour = Colour(200, 200, 200);
	D2D1::ColorF HealthTextColour = Colour(100, 255, 100);
	D2D1::ColorF WeaponColour = Colour(255, 200, 50);
	D2D1::ColorF RoleColour = Colour(50, 200, 255);
	D2D1::ColorF LookingColour = Colour(255, 50, 50);
	D2D1::ColorF StateColour = Colour(255, 180, 0);
	D2D1::ColorF BoxColour = Colour(255, 255, 255);
	D2D1::ColorF FiringColour = Colour(255, 80, 80);
	D2D1::ColorF ReloadColour = Colour(255, 200, 50);
	D2D1::ColorF ArmorColour = Colour(100, 180, 255);
	D2D1::ColorF StaminaColour = Colour(60, 220, 100);

	// Worker-side pose smoothing. The reader blends every new pose sample into
	// the previous one before publishing, so the overlay draws the smoothed
	// points directly. PoseSmoothAlpha is the weight of the new sample: 1 keeps
	// the pose raw, lower values damp jitter from partial or stale reads.
	bool PoseSmoothing = true;
	float PoseSmoothAlpha = 0.5f;
    void ToJsonColour(json* j, const std::string& name, D2D1::ColorF* colour)
    {
        (*j)[ConfigName][name][LIT("r")] = colour->r;
        (*j)[ConfigName][name][LIT("g")] = colour->g;
        (*j)[ConfigName][name][LIT("b")] = colour->b;
        (*j)[ConfigName][name][LIT("a")] = colour->a;

    }
    void FromJsonColour(json j, const std::string& name, D2D1::ColorF* colour)
    {
        if (j[ConfigName].contains(name))
        {
            colour->r = j[ConfigName][name][LIT("r")];
            colour->g = j[ConfigName][name][LIT("g")];
            colour->b = j[ConfigName][name][LIT("b")];
            colour->a = j[ConfigName][name][LIT("a")];
        }
    }

    json ToJson()
    {
        json j;
        j[ConfigName][LIT("BoxEsp")] = BoxEsp;
        j[ConfigName][LIT("Health")] = Health;
        j[ConfigName][LIT("Distance")] = Distance;
        j[ConfigName][LIT("MaxDistance")] = MaxDistance;
        j[ConfigName][LIT("FontSize")] = FontSize;
        j[ConfigName][LIT("PrimaryWeapon")] = PrimaryWeapon;
        j[ConfigName][LIT("SecondaryWeapon")] = SecondaryWeapon;
        ToJsonColour(&j, LIT("TextColour"), &TextColour);
        j[ConfigName][LIT("VisCheck")] = VisCheck;
        ToJsonColour(&j, LIT("VisibleColour"), &VisibleColour);
        ToJsonColour(&j, LIT("HiddenColour"), &HiddenColour);
        j[ConfigName][LIT("WallCheck")] = WallCheck;
        j[ConfigName][LIT("BvhPath")] = BvhPath;
        j[ConfigName][LIT("NativeEspCameraV2")] = NativeEspCamera;
        j[ConfigName][LIT("WeaponEsp")] = WeaponEsp;
        j[ConfigName][LIT("LookingAtYou")] = LookingAtYou;
        j[ConfigName][LIT("LookingAlert")] = LookingAlert;
        j[ConfigName][LIT("ShowRole")] = ShowRole;
        j[ConfigName][LIT("ExpRoleColors")] = ExpRoleColors;
        j[ConfigName][LIT("ExpMoneyHud")] = ExpMoneyHud;
        j[ConfigName][LIT("ShowFiring")] = ShowFiring;
        j[ConfigName][LIT("ShowReloading")] = ShowReloading;
        j[ConfigName][LIT("ShowArmor")] = ShowArmor;
        j[ConfigName][LIT("ShowArmorValue")] = ShowArmorValue;
        j[ConfigName][LIT("ShowStamina")] = ShowStamina;
        j[ConfigName][LIT("SpectatorAlert")] = SpectatorAlert;
        j[ConfigName][LIT("WeaponSprite")] = WeaponSprite;
        ToJsonColour(&j, LIT("NameColour"), &NameColour);
        ToJsonColour(&j, LIT("DistanceColour"), &DistanceColour);
        ToJsonColour(&j, LIT("HealthTextColour"), &HealthTextColour);
        ToJsonColour(&j, LIT("WeaponColour"), &WeaponColour);
        ToJsonColour(&j, LIT("RoleColour"), &RoleColour);
        ToJsonColour(&j, LIT("LookingColour"), &LookingColour);
        ToJsonColour(&j, LIT("StateColour"), &StateColour);
        ToJsonColour(&j, LIT("BoxColour"), &BoxColour);
        ToJsonColour(&j, LIT("FiringColour"), &FiringColour);
        ToJsonColour(&j, LIT("ReloadColour"), &ReloadColour);
        ToJsonColour(&j, LIT("ArmorColour"), &ArmorColour);
        ToJsonColour(&j, LIT("StaminaColour"), &StaminaColour);
        j[ConfigName][LIT("PoseSmoothing")] = PoseSmoothing;
        j[ConfigName][LIT("PoseSmoothAlpha")] = PoseSmoothAlpha;

        return j;
    }
    void FromJson(const json& j)
    {
        if (!j.contains(ConfigName))
            return;
        BoxEsp = j[ConfigName].value(LIT("BoxEsp"), BoxEsp);
        Health = j[ConfigName].value(LIT("Health"), Health);
        Distance = j[ConfigName].value(LIT("Distance"), Distance);
        PrimaryWeapon = j[ConfigName].value(LIT("PrimaryWeapon"), PrimaryWeapon);
        SecondaryWeapon = j[ConfigName].value(LIT("SecondaryWeapon"), SecondaryWeapon);
        FontSize = j[ConfigName].value(LIT("FontSize"), FontSize);
        MaxDistance = j[ConfigName].value(LIT("MaxDistance"), MaxDistance);
        FromJsonColour(j, LIT("TextColour"), &TextColour);
        VisCheck = j[ConfigName].value(LIT("VisCheck"), VisCheck);
        FromJsonColour(j, LIT("VisibleColour"), &VisibleColour);
        FromJsonColour(j, LIT("HiddenColour"), &HiddenColour);
        WallCheck = j[ConfigName].value(LIT("WallCheck"), WallCheck);
        BvhPath = j[ConfigName].value(LIT("BvhPath"), BvhPath);
        NativeEspCamera = j[ConfigName].value(LIT("NativeEspCameraV2"), NativeEspCamera);
        WeaponEsp = j[ConfigName].value(LIT("WeaponEsp"), WeaponEsp);
        LookingAtYou = j[ConfigName].value(LIT("LookingAtYou"), LookingAtYou);
        LookingAlert = j[ConfigName].value(LIT("LookingAlert"), LookingAlert);
        ShowRole = j[ConfigName].value(LIT("ShowRole"), ShowRole);
        ExpRoleColors = j[ConfigName].value(LIT("ExpRoleColors"), ExpRoleColors);
        ExpMoneyHud = j[ConfigName].value(LIT("ExpMoneyHud"), ExpMoneyHud);
        ShowFiring = j[ConfigName].value(LIT("ShowFiring"), ShowFiring);
        ShowReloading = j[ConfigName].value(LIT("ShowReloading"), ShowReloading);
        ShowArmor = j[ConfigName].value(LIT("ShowArmor"), ShowArmor);
        ShowArmorValue = j[ConfigName].value(LIT("ShowArmorValue"), ShowArmorValue);
        ShowStamina = j[ConfigName].value(LIT("ShowStamina"), ShowStamina);
        SpectatorAlert = j[ConfigName].value(LIT("SpectatorAlert"), SpectatorAlert);
        WeaponSprite = j[ConfigName].value(LIT("WeaponSprite"), WeaponSprite);
        FromJsonColour(j, LIT("NameColour"), &NameColour);
        FromJsonColour(j, LIT("DistanceColour"), &DistanceColour);
        FromJsonColour(j, LIT("HealthTextColour"), &HealthTextColour);
        FromJsonColour(j, LIT("WeaponColour"), &WeaponColour);
        FromJsonColour(j, LIT("RoleColour"), &RoleColour);
        FromJsonColour(j, LIT("LookingColour"), &LookingColour);
        FromJsonColour(j, LIT("StateColour"), &StateColour);
        FromJsonColour(j, LIT("BoxColour"), &BoxColour);
        FromJsonColour(j, LIT("FiringColour"), &FiringColour);
        FromJsonColour(j, LIT("ReloadColour"), &ReloadColour);
        FromJsonColour(j, LIT("ArmorColour"), &ArmorColour);
        FromJsonColour(j, LIT("StaminaColour"), &StaminaColour);
        PoseSmoothing = j[ConfigName].value(LIT("PoseSmoothing"), PoseSmoothing);
        PoseSmoothAlpha = j[ConfigName].value(LIT("PoseSmoothAlpha"), PoseSmoothAlpha);
    }
};

