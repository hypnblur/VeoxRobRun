#pragma once
#include "pch.h"

class RadarConfig
{
	std::string ConfigName;

public:
	RadarConfig(const std::string& name)
	{
		ConfigName = name;
	}

	bool Enabled = true;
	float Size = 300.f;
	float Zoom = 1.f;
	float Opacity = 0.85f;
	bool ShowPlayers = true;
	bool ShowVehicles = true;
	bool ShowLoot = false;
	bool RotateWithCamera = true;

	float WorldMinX = -600.f;
	float WorldMaxX = 600.f;
	float WorldMinZ = -400.f;
	float WorldMaxZ = 400.f;

	std::string SelectedMap = "looty_bay.png";

	json ToJson()
	{
		json j;
		j[ConfigName][LIT("Enabled")] = Enabled;
		j[ConfigName][LIT("Size")] = Size;
		j[ConfigName][LIT("Zoom")] = Zoom;
		j[ConfigName][LIT("Opacity")] = Opacity;
		j[ConfigName][LIT("ShowPlayers")] = ShowPlayers;
		j[ConfigName][LIT("ShowVehicles")] = ShowVehicles;
		j[ConfigName][LIT("ShowLoot")] = ShowLoot;
		j[ConfigName][LIT("RotateWithCamera")] = RotateWithCamera;
		j[ConfigName][LIT("WorldMinX")] = WorldMinX;
		j[ConfigName][LIT("WorldMaxX")] = WorldMaxX;
		j[ConfigName][LIT("WorldMinZ")] = WorldMinZ;
		j[ConfigName][LIT("WorldMaxZ")] = WorldMaxZ;
		j[ConfigName][LIT("SelectedMap")] = SelectedMap;
		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		auto& s = j[ConfigName];
		if (s.contains(LIT("Enabled")))          Enabled = s[LIT("Enabled")];
		if (s.contains(LIT("Size")))             Size = s[LIT("Size")];
		if (s.contains(LIT("Zoom")))             Zoom = s[LIT("Zoom")];
		if (s.contains(LIT("Opacity")))          Opacity = s[LIT("Opacity")];
		if (s.contains(LIT("ShowPlayers")))      ShowPlayers = s[LIT("ShowPlayers")];
		if (s.contains(LIT("ShowVehicles")))     ShowVehicles = s[LIT("ShowVehicles")];
		if (s.contains(LIT("ShowLoot")))         ShowLoot = s[LIT("ShowLoot")];
		if (s.contains(LIT("RotateWithCamera"))) RotateWithCamera = s[LIT("RotateWithCamera")];
		if (s.contains(LIT("WorldMinX")))        WorldMinX = s[LIT("WorldMinX")];
		if (s.contains(LIT("WorldMaxX")))        WorldMaxX = s[LIT("WorldMaxX")];
		if (s.contains(LIT("WorldMinZ")))        WorldMinZ = s[LIT("WorldMinZ")];
		if (s.contains(LIT("WorldMaxZ")))        WorldMaxZ = s[LIT("WorldMaxZ")];
		if (s.contains(LIT("SelectedMap")))      SelectedMap = s[LIT("SelectedMap")].get<std::string>();
	}
};
