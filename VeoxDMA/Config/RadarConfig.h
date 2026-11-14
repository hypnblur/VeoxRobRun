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
		Enabled = s.value(LIT("Enabled"), Enabled);
		Size = s.value(LIT("Size"), Size);
		Zoom = s.value(LIT("Zoom"), Zoom);
		Opacity = s.value(LIT("Opacity"), Opacity);
		ShowPlayers = s.value(LIT("ShowPlayers"), ShowPlayers);
		ShowVehicles = s.value(LIT("ShowVehicles"), ShowVehicles);
		ShowLoot = s.value(LIT("ShowLoot"), ShowLoot);
		RotateWithCamera = s.value(LIT("RotateWithCamera"), RotateWithCamera);
		WorldMinX = s.value(LIT("WorldMinX"), WorldMinX);
		WorldMaxX = s.value(LIT("WorldMaxX"), WorldMaxX);
		WorldMinZ = s.value(LIT("WorldMinZ"), WorldMinZ);
		WorldMaxZ = s.value(LIT("WorldMaxZ"), WorldMaxZ);
		SelectedMap = s.value(LIT("SelectedMap"), SelectedMap);
	}
};
