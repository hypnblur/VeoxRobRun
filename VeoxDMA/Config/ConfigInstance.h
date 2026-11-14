#pragma once
#include "Pch.h"
#include "PlayerConfig.h"
#include "OverlayConfig.h"
#include "WorldConfig.h"
#include "AimConfig.h"
#include "RadarConfig.h"
#include <cassert>

class ConfigInstances
{
public:
	PlayerConfig Player = PlayerConfig(LIT("Player"));
	OverlayConfig Overlay = OverlayConfig(LIT("Overlay"));
	WorldConfig World = WorldConfig(LIT("World"));
	AimConfig Aim = AimConfig(LIT("Aim"));
	RadarConfig Radar = RadarConfig(LIT("Radar"));

	static_assert(
		std::string_view("Player") != std::string_view("Overlay") &&
		std::string_view("Player") != std::string_view("World") &&
		std::string_view("Player") != std::string_view("Aim") &&
		std::string_view("Player") != std::string_view("Radar") &&
		std::string_view("Overlay") != std::string_view("World") &&
		std::string_view("Overlay") != std::string_view("Aim") &&
		std::string_view("Overlay") != std::string_view("Radar") &&
		std::string_view("World") != std::string_view("Aim") &&
		std::string_view("World") != std::string_view("Radar") &&
		std::string_view("Aim") != std::string_view("Radar"),
		"Config section names must be unique — merge_patch silently overwrites on collision");

	json ToJson()
	{
		json jsoned;
		jsoned.merge_patch(Player.ToJson());
		jsoned.merge_patch(Overlay.ToJson());
		jsoned.merge_patch(World.ToJson());
		jsoned.merge_patch(Aim.ToJson());
		jsoned.merge_patch(Radar.ToJson());
		return jsoned;
	}

	void FromJson(json jsoned)
	{
		Player.FromJson(jsoned);
		Overlay.FromJson(jsoned);
		World.FromJson(jsoned);
		Aim.FromJson(jsoned);
		Radar.FromJson(jsoned);
	}
};
inline ConfigInstances ConfigInstance;
