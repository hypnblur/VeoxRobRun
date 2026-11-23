#pragma once
#include "Pch.h"
#include "PlayerConfig.h"
#include "OverlayConfig.h"
#include "WorldConfig.h"
#include "AimConfig.h"
#include "RadarConfig.h"
#include "LootFilterConfig.h"
#include "SkinConfig.h"
#include <cassert>

class ConfigInstances
{
public:
	PlayerConfig Player = PlayerConfig(LIT("Player"));
	OverlayConfig Overlay = OverlayConfig(LIT("Overlay"));
	WorldConfig World = WorldConfig(LIT("World"));
	AimConfig Aim = AimConfig(LIT("Aim"));
	RadarConfig Radar = RadarConfig(LIT("Radar"));
	LootFilterConfig LootFilter = LootFilterConfig(LIT("LootFilter"));
	SkinConfig Skin = SkinConfig(LIT("Skin"));

	static_assert(
		std::string_view("Player") != std::string_view("Overlay") &&
		std::string_view("Player") != std::string_view("World") &&
		std::string_view("Player") != std::string_view("Aim") &&
		std::string_view("Player") != std::string_view("Radar") &&
		std::string_view("Player") != std::string_view("LootFilter") &&
		std::string_view("Overlay") != std::string_view("World") &&
		std::string_view("Overlay") != std::string_view("Aim") &&
		std::string_view("Overlay") != std::string_view("Radar") &&
		std::string_view("Overlay") != std::string_view("LootFilter") &&
		std::string_view("World") != std::string_view("Aim") &&
		std::string_view("World") != std::string_view("Radar") &&
		std::string_view("World") != std::string_view("LootFilter") &&
		std::string_view("Aim") != std::string_view("Radar") &&
		std::string_view("Aim") != std::string_view("LootFilter") &&
		std::string_view("Aim") != std::string_view("Skin") &&
		std::string_view("Radar") != std::string_view("LootFilter") &&
		std::string_view("Radar") != std::string_view("Skin") &&
		std::string_view("LootFilter") != std::string_view("Skin"),
		"Config section names must be unique — merge_patch silently overwrites on collision");

	void DisableAllMemWriteFeatures()
	{
		Aim.MemoryWrite = false;
		Aim.NoRecoil = false;
		Aim.NoSpread = false;
		Aim.DynamicRecoilCompensation = false;
		Aim.HideCrosshair = false;
		Overlay.CustomCameraDistance = false;
		Overlay.FovChanger = false;
		World.InstantLoot = false;
		World.InstantUnlock = false;
		Skin.Enabled = false;
	}

	json ToJson()
	{
		if (!Overlay.MemWriteMaster)
			DisableAllMemWriteFeatures();
		json jsoned;
		jsoned.merge_patch(Player.ToJson());
		jsoned.merge_patch(Overlay.ToJson());
		jsoned.merge_patch(World.ToJson());
		jsoned.merge_patch(Aim.ToJson());
		jsoned.merge_patch(Radar.ToJson());
		jsoned.merge_patch(LootFilter.ToJson());
		jsoned.merge_patch(Skin.ToJson());
		return jsoned;
	}

	void FromJson(json jsoned)
	{
		Player.FromJson(jsoned);
		Overlay.FromJson(jsoned);
		World.FromJson(jsoned);
		Aim.FromJson(jsoned);
		Radar.FromJson(jsoned);
		LootFilter.FromJson(jsoned);
		Skin.FromJson(jsoned);
		if (!Overlay.MemWriteMaster)
			DisableAllMemWriteFeatures();
	}
};
inline ConfigInstances ConfigInstance;
