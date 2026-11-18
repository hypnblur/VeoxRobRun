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
	bool SecondaryWeapon = true;
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
	bool ShowBvh = false;
	int BvhDrawDist = 100;
	// Project the ESP with the Unity render camera instead of the managed
	// BattleCameraModel block. When enabled, this uses the hardware-synchronized
	// native matrices to eliminate mouse-turn projection jitter.
	bool NativeEspCamera = true;
	bool WeaponEsp = true;
	bool LookingAtYou = true;
	bool LookingAlert = true;
	bool ShowRole = true;
	// Colour the role tag per role instead of using RoleColour.
	bool ExpRoleColors = true;
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
	bool HideTeammates = false;
	bool HiddenPlayerEsp = true;
	float HiddenAlpha = 0.35f;
	float VisMaxDistance = 150.0f;
	int VisRayCount = 6;
	bool CarCabinSolid = false;
	bool VisRing = true;
	bool StatusRing = true;

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
	D2D1::ColorF VisRingColour = Colour(0, 255, 128);
	D2D1::ColorF StatusRingColour = Colour(0, 255, 128);

	// Worker-side pose smoothing. The reader blends every new pose sample into
	// the previous one before publishing, so the overlay draws the smoothed
	// points directly. PoseSmoothAlpha is the weight of the new sample: 1 keeps
	// the pose raw, lower values damp jitter from partial or stale reads.
	bool PoseSmoothing = true;
	float PoseSmoothAlpha = 0.4f;

	// Character 3D Capsule Chams (100% read-only DMA overlay silhouette)
	bool Chams = false;
	D2D1::ColorF ChamsColour = Colour(140, 90, 255);
	float ChamsAlpha = 0.45f;
	bool ChamsVisCheck = true;
	D2D1::ColorF ChamsVisibleColour = Colour(255, 65, 65);
	D2D1::ColorF ChamsHiddenColour = Colour(120, 80, 255);
	bool ChamsOutline = false;
	D2D1::ColorF ChamsOutlineColour = Colour(200, 180, 255);
	float ChamsScale = 1.0f;
	bool Skeleton = true;
	float SkeletonThickness = 2.0f;
	bool HeadCircle = true;

	// Target Tracers / Snaplines
	bool Snaplines = false;
	int SnaplineOrigin = 0; // 0 = Bottom Center, 1 = Screen Center, 2 = Top Center
	D2D1::ColorF SnaplineColour = Colour(255, 255, 255);

	// Off-Screen Directional Indicator Arrows
	bool OffscreenArrows = false;
	float OffscreenRadius = 350.f;
	D2D1::ColorF OffscreenColour = Colour(255, 70, 70);

	// Head Hitbox Dot
	bool HeadDot = false;
	float HeadDotRadius = 3.5f;
	D2D1::ColorF HeadDotColour = Colour(255, 50, 50);
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
        j[ConfigName][LIT("ShowBvh")] = ShowBvh;
        j[ConfigName][LIT("BvhDrawDist")] = BvhDrawDist;
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
        j[ConfigName][LIT("Chams")] = Chams;
        ToJsonColour(&j, LIT("ChamsColour"), &ChamsColour);
        j[ConfigName][LIT("ChamsAlpha")] = ChamsAlpha;
        j[ConfigName][LIT("ChamsVisCheck")] = ChamsVisCheck;
        ToJsonColour(&j, LIT("ChamsVisibleColour"), &ChamsVisibleColour);
        ToJsonColour(&j, LIT("ChamsHiddenColour"), &ChamsHiddenColour);
        j[ConfigName][LIT("ChamsOutline")] = ChamsOutline;
        ToJsonColour(&j, LIT("ChamsOutlineColour"), &ChamsOutlineColour);
        j[ConfigName][LIT("ChamsScale")] = ChamsScale;
        j[ConfigName][LIT("Skeleton")] = Skeleton;
        j[ConfigName][LIT("SkeletonThickness")] = SkeletonThickness;
        j[ConfigName][LIT("HeadCircle")] = HeadCircle;
        j[ConfigName][LIT("HiddenAlpha")] = HiddenAlpha;
        j[ConfigName][LIT("VisMaxDistance")] = VisMaxDistance;
        j[ConfigName][LIT("VisRayCount")] = VisRayCount;
        j[ConfigName][LIT("CarCabinSolid")] = CarCabinSolid;
        j[ConfigName][LIT("VisRing")] = VisRing;
        j[ConfigName][LIT("StatusRing")] = StatusRing;
        ToJsonColour(&j, LIT("VisRingColour"), &VisRingColour);
        ToJsonColour(&j, LIT("StatusRingColour"), &StatusRingColour);
        j[ConfigName][LIT("Snaplines")] = Snaplines;
        j[ConfigName][LIT("SnaplineOrigin")] = SnaplineOrigin;
        ToJsonColour(&j, LIT("SnaplineColour"), &SnaplineColour);
        j[ConfigName][LIT("OffscreenArrows")] = OffscreenArrows;
        j[ConfigName][LIT("OffscreenRadius")] = OffscreenRadius;
        ToJsonColour(&j, LIT("OffscreenColour"), &OffscreenColour);
        j[ConfigName][LIT("HideTeammates")] = HideTeammates;
        j[ConfigName][LIT("HiddenPlayerEsp")] = HiddenPlayerEsp;
        j[ConfigName][LIT("HeadDot")] = HeadDot;
        j[ConfigName][LIT("HeadDotRadius")] = HeadDotRadius;
        ToJsonColour(&j, LIT("HeadDotColour"), &HeadDotColour);

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
        ShowBvh = j[ConfigName].value(LIT("ShowBvh"), ShowBvh);
        BvhDrawDist = j[ConfigName].value(LIT("BvhDrawDist"), BvhDrawDist);
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
        Chams = j[ConfigName].value(LIT("Chams"), Chams);
        FromJsonColour(j, LIT("ChamsColour"), &ChamsColour);
        ChamsAlpha = j[ConfigName].value(LIT("ChamsAlpha"), ChamsAlpha);
        ChamsVisCheck = j[ConfigName].value(LIT("ChamsVisCheck"), ChamsVisCheck);
        FromJsonColour(j, LIT("ChamsVisibleColour"), &ChamsVisibleColour);
        FromJsonColour(j, LIT("ChamsHiddenColour"), &ChamsHiddenColour);
        ChamsOutline = j[ConfigName].value(LIT("ChamsOutline"), ChamsOutline);
        FromJsonColour(j, LIT("ChamsOutlineColour"), &ChamsOutlineColour);
        ChamsScale = j[ConfigName].value(LIT("ChamsScale"), ChamsScale);
        Skeleton = j[ConfigName].value(LIT("Skeleton"), Skeleton);
        SkeletonThickness = j[ConfigName].value(LIT("SkeletonThickness"), SkeletonThickness);
        HeadCircle = j[ConfigName].value(LIT("HeadCircle"), HeadCircle);
        HiddenAlpha = j[ConfigName].value(LIT("HiddenAlpha"), HiddenAlpha);
        VisMaxDistance = j[ConfigName].value(LIT("VisMaxDistance"), VisMaxDistance);
        VisRayCount = j[ConfigName].value(LIT("VisRayCount"), VisRayCount);
        CarCabinSolid = j[ConfigName].value(LIT("CarCabinSolid"), CarCabinSolid);
        VisRing = j[ConfigName].value(LIT("VisRing"), VisRing);
        StatusRing = j[ConfigName].value(LIT("StatusRing"), StatusRing);
        FromJsonColour(j, LIT("VisRingColour"), &VisRingColour);
        FromJsonColour(j, LIT("StatusRingColour"), &StatusRingColour);
        Snaplines = j[ConfigName].value(LIT("Snaplines"), Snaplines);
        SnaplineOrigin = j[ConfigName].value(LIT("SnaplineOrigin"), SnaplineOrigin);
        FromJsonColour(j, LIT("SnaplineColour"), &SnaplineColour);
        OffscreenArrows = j[ConfigName].value(LIT("OffscreenArrows"), OffscreenArrows);
        OffscreenRadius = j[ConfigName].value(LIT("OffscreenRadius"), OffscreenRadius);
        FromJsonColour(j, LIT("OffscreenColour"), &OffscreenColour);
        HideTeammates = j[ConfigName].value(LIT("HideTeammates"), HideTeammates);
        HiddenPlayerEsp = j[ConfigName].value(LIT("HiddenPlayerEsp"), HiddenPlayerEsp);
        HeadDot = j[ConfigName].value(LIT("HeadDot"), HeadDot);
        HeadDotRadius = j[ConfigName].value(LIT("HeadDotRadius"), HeadDotRadius);
        FromJsonColour(j, LIT("HeadDotColour"), &HeadDotColour);
    }
};

