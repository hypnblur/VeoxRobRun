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
	float Opacity = 0.5f;
	bool ShowPlayers = true;
	bool ShowVehicles = false;
	bool ShowLoot = false;
	bool ShowContainers = true;
	bool ShowDroppedBags = true;
	bool ShowValuables = true;
	int MinLootValue = 0;
	bool ShowLootNames = true;
	bool ShowPlayerNames = true;
	bool ShowPlayerDistances = true;
	bool RotateWithCamera = false;
	bool ShowMap = true;

	// Pop-out standalone OS window
	bool PopoutWindow = true;
	int PopoutX = 80;
	int PopoutY = 80;
	int PopoutW = 650;
	int PopoutH = 650;

	float WorldMinX = -600.f;
	float WorldMaxX = 600.f;
	float WorldMinZ = -400.f;
	float WorldMaxZ = 400.f;

	std::string SelectedMap = "city_colliders_map.png";

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
		j[ConfigName][LIT("ShowContainers")] = ShowContainers;
		j[ConfigName][LIT("ShowDroppedBags")] = ShowDroppedBags;
		j[ConfigName][LIT("ShowValuables")] = ShowValuables;
		j[ConfigName][LIT("MinLootValue")] = MinLootValue;
		j[ConfigName][LIT("ShowLootNames")] = ShowLootNames;
		j[ConfigName][LIT("ShowPlayerNames")] = ShowPlayerNames;
		j[ConfigName][LIT("ShowPlayerDistances")] = ShowPlayerDistances;
		j[ConfigName][LIT("RotateWithCamera")] = RotateWithCamera;
		j[ConfigName][LIT("ShowMap")] = ShowMap;
		j[ConfigName][LIT("PopoutWindow")] = PopoutWindow;
		j[ConfigName][LIT("PopoutX")] = PopoutX;
		j[ConfigName][LIT("PopoutY")] = PopoutY;
		j[ConfigName][LIT("PopoutW")] = PopoutW;
		j[ConfigName][LIT("PopoutH")] = PopoutH;
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
		ShowContainers = s.value(LIT("ShowContainers"), ShowContainers);
		ShowDroppedBags = s.value(LIT("ShowDroppedBags"), ShowDroppedBags);
		ShowValuables = s.value(LIT("ShowValuables"), ShowValuables);
		MinLootValue = s.value(LIT("MinLootValue"), MinLootValue);
		ShowLootNames = s.value(LIT("ShowLootNames"), ShowLootNames);
		ShowPlayerNames = s.value(LIT("ShowPlayerNames"), ShowPlayerNames);
		ShowPlayerDistances = s.value(LIT("ShowPlayerDistances"), ShowPlayerDistances);
		RotateWithCamera = s.value(LIT("RotateWithCamera"), RotateWithCamera);
		ShowMap = s.value(LIT("ShowMap"), ShowMap);
		PopoutWindow = s.value(LIT("PopoutWindow"), PopoutWindow);
		PopoutX = s.value(LIT("PopoutX"), PopoutX);
		PopoutY = s.value(LIT("PopoutY"), PopoutY);
		PopoutW = s.value(LIT("PopoutW"), PopoutW);
		PopoutH = s.value(LIT("PopoutH"), PopoutH);
		WorldMinX = s.value(LIT("WorldMinX"), WorldMinX);
		WorldMaxX = s.value(LIT("WorldMaxX"), WorldMaxX);
		WorldMinZ = s.value(LIT("WorldMinZ"), WorldMinZ);
		WorldMaxZ = s.value(LIT("WorldMaxZ"), WorldMaxZ);
		SelectedMap = s.value(LIT("SelectedMap"), SelectedMap);
	}
};
