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

	// Web Radar (Localhost browser popout)
	bool WebRadarEnabled = true;
	int WebRadarPort = 8888;
	bool AutoMapSelect = true;
	bool WebRadarRotateWithPlayer = false;
	bool WebRadarFollowPlayer = true;

	float WorldMinX = -1232.33f;
	float WorldMaxX = 1131.41f;
	float WorldMinZ = -460.78f;
	float WorldMaxZ = 914.29f;

	std::string SelectedMap = "shooter_city.png";

	// In-game minimap & fullscreen ('M') map overlay
	bool InGameMinimap = true;
	bool InGameFullMap = true;
	bool Hide3DEspOnFullMap = true;
	bool InGameMapPlayers = true;
	bool InGameMapVehicles = true;
	bool InGameMapPlayerHeading = false;
	bool InGameMapPlayerNames = false;
	bool InGameMapDroppedBags = false;
	bool InGameMapValuables = false;
	bool InGameMapCargo = false;
	int InGameMapMinRarity = 3;      // 0=All, 1=Uncommon+, 2=Rare+, 3=Epic+, 4=Legendary+, 5=Mythical
	int InGameMapCargoMinRarity = 1; // 0=All, 1=Uncommon+, 2=Rare+, 3=Epic+, 4=Legendary+, 5=Mythical
	float InGameMapScale = 1.0f;

	// Live role counters (Police / Civilian / Criminal)
	bool RoleCounters = true;
	bool RoleCountersOnFullMap = true;
	bool RoleCountersOnMinimap = false;
	bool RoleCountersOnWebRadar = true;
	int RoleCounterScope = 0; // 0 = Full Map, 1 = In 300m Radius
	D2D1::ColorF RoleCounterPoliceColour = Colour(74, 144, 226);     // #4A90E2
	D2D1::ColorF RoleCounterCivilianColour = Colour(216, 216, 216); // #D8D8D8
	D2D1::ColorF RoleCounterCriminalColour = Colour(224, 74, 74);   // #E04A4A

	// Map marker colors (In-Game Map & Web Radar)
	D2D1::ColorF MapCriminalColour = Colour(255, 68, 68);
	D2D1::ColorF MapPoliceColour = Colour(55, 145, 255);
	D2D1::ColorF MapCivilianColour = Colour(175, 182, 192);
	D2D1::ColorF MapPoliceCarColour = Colour(55, 145, 255);

	void ToJsonColour(json* j, const std::string& name, const D2D1::ColorF* colour)
	{
		(*j)[ConfigName][name][LIT("r")] = colour->r;
		(*j)[ConfigName][name][LIT("g")] = colour->g;
		(*j)[ConfigName][name][LIT("b")] = colour->b;
		(*j)[ConfigName][name][LIT("a")] = colour->a;
	}

	void FromJsonColour(const json& j, const std::string& name, D2D1::ColorF* colour)
	{
		if (j[ConfigName].contains(name))
		{
			const auto& c = j[ConfigName][name];
			colour->r = c.value(LIT("r"), colour->r);
			colour->g = c.value(LIT("g"), colour->g);
			colour->b = c.value(LIT("b"), colour->b);
			colour->a = c.value(LIT("a"), colour->a);
		}
	}

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
		j[ConfigName][LIT("WebRadarEnabled")] = WebRadarEnabled;
		j[ConfigName][LIT("WebRadarPort")] = WebRadarPort;
		j[ConfigName][LIT("AutoMapSelect")] = AutoMapSelect;
		j[ConfigName][LIT("WebRadarRotateWithPlayer")] = WebRadarRotateWithPlayer;
		j[ConfigName][LIT("WebRadarFollowPlayer")] = WebRadarFollowPlayer;
		j[ConfigName][LIT("WorldMinX")] = WorldMinX;
		j[ConfigName][LIT("WorldMaxX")] = WorldMaxX;
		j[ConfigName][LIT("WorldMinZ")] = WorldMinZ;
		j[ConfigName][LIT("WorldMaxZ")] = WorldMaxZ;
		j[ConfigName][LIT("SelectedMap")] = SelectedMap;
		j[ConfigName][LIT("InGameMinimap")] = InGameMinimap;
		j[ConfigName][LIT("InGameFullMap")] = InGameFullMap;
		j[ConfigName][LIT("Hide3DEspOnFullMap")] = Hide3DEspOnFullMap;
		j[ConfigName][LIT("InGameMapPlayers")] = InGameMapPlayers;
		j[ConfigName][LIT("InGameMapVehicles")] = InGameMapVehicles;
		j[ConfigName][LIT("InGameMapPlayerHeading")] = InGameMapPlayerHeading;
		j[ConfigName][LIT("InGameMapPlayerNames")] = InGameMapPlayerNames;
		j[ConfigName][LIT("InGameMapDroppedBags")] = InGameMapDroppedBags;
		j[ConfigName][LIT("InGameMapValuables")] = InGameMapValuables;
		j[ConfigName][LIT("InGameMapCargo")] = InGameMapCargo;
		j[ConfigName][LIT("InGameMapMinRarity")] = InGameMapMinRarity;
		j[ConfigName][LIT("InGameMapCargoMinRarity")] = InGameMapCargoMinRarity;
		j[ConfigName][LIT("InGameMapScale")] = InGameMapScale;
		j[ConfigName][LIT("RoleCounters")] = RoleCounters;
		j[ConfigName][LIT("RoleCountersOnFullMap")] = RoleCountersOnFullMap;
		j[ConfigName][LIT("RoleCountersOnMinimap")] = RoleCountersOnMinimap;
		j[ConfigName][LIT("RoleCountersOnWebRadar")] = RoleCountersOnWebRadar;
		j[ConfigName][LIT("RoleCounterScope")] = RoleCounterScope;
		ToJsonColour(&j, LIT("RoleCounterPoliceColour"), &RoleCounterPoliceColour);
		ToJsonColour(&j, LIT("RoleCounterCivilianColour"), &RoleCounterCivilianColour);
		ToJsonColour(&j, LIT("RoleCounterCriminalColour"), &RoleCounterCriminalColour);
		ToJsonColour(&j, LIT("MapCriminalColour"), &MapCriminalColour);
		ToJsonColour(&j, LIT("MapPoliceColour"), &MapPoliceColour);
		ToJsonColour(&j, LIT("MapCivilianColour"), &MapCivilianColour);
		ToJsonColour(&j, LIT("MapPoliceCarColour"), &MapPoliceCarColour);
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
		WebRadarEnabled = s.value(LIT("WebRadarEnabled"), WebRadarEnabled);
		WebRadarPort = s.value(LIT("WebRadarPort"), WebRadarPort);
		AutoMapSelect = s.value(LIT("AutoMapSelect"), AutoMapSelect);
		WebRadarRotateWithPlayer = s.value(LIT("WebRadarRotateWithPlayer"), WebRadarRotateWithPlayer);
		WebRadarFollowPlayer = s.value(LIT("WebRadarFollowPlayer"), WebRadarFollowPlayer);
		WorldMinX = s.value(LIT("WorldMinX"), WorldMinX);
		WorldMaxX = s.value(LIT("WorldMaxX"), WorldMaxX);
		WorldMinZ = s.value(LIT("WorldMinZ"), WorldMinZ);
		WorldMaxZ = s.value(LIT("WorldMaxZ"), WorldMaxZ);
		SelectedMap = s.value(LIT("SelectedMap"), SelectedMap);
		InGameMinimap = s.value(LIT("InGameMinimap"), InGameMinimap);
		InGameFullMap = s.value(LIT("InGameFullMap"), InGameFullMap);
		Hide3DEspOnFullMap = s.value(LIT("Hide3DEspOnFullMap"), Hide3DEspOnFullMap);
		InGameMapPlayers = s.value(LIT("InGameMapPlayers"), InGameMapPlayers);
		InGameMapVehicles = s.value(LIT("InGameMapVehicles"), InGameMapVehicles);
		InGameMapPlayerHeading = s.value(LIT("InGameMapPlayerHeading"), InGameMapPlayerHeading);
		InGameMapPlayerNames = s.value(LIT("InGameMapPlayerNames"), InGameMapPlayerNames);
		InGameMapDroppedBags = s.value(LIT("InGameMapDroppedBags"), InGameMapDroppedBags);
		InGameMapValuables = s.value(LIT("InGameMapValuables"), InGameMapValuables);
		InGameMapCargo = s.value(LIT("InGameMapCargo"), InGameMapCargo);
		InGameMapMinRarity = s.value(LIT("InGameMapMinRarity"), InGameMapMinRarity);
		InGameMapCargoMinRarity = s.value(LIT("InGameMapCargoMinRarity"), InGameMapCargoMinRarity);
		InGameMapScale = s.value(LIT("InGameMapScale"), InGameMapScale);
		RoleCounters = s.value(LIT("RoleCounters"), RoleCounters);
		RoleCountersOnFullMap = s.value(LIT("RoleCountersOnFullMap"), RoleCountersOnFullMap);
		RoleCountersOnMinimap = s.value(LIT("RoleCountersOnMinimap"), RoleCountersOnMinimap);
		RoleCountersOnWebRadar = s.value(LIT("RoleCountersOnWebRadar"), RoleCountersOnWebRadar);
		RoleCounterScope = s.value(LIT("RoleCounterScope"), RoleCounterScope);
		FromJsonColour(j, LIT("RoleCounterPoliceColour"), &RoleCounterPoliceColour);
		FromJsonColour(j, LIT("RoleCounterCivilianColour"), &RoleCounterCivilianColour);
		FromJsonColour(j, LIT("RoleCounterCriminalColour"), &RoleCounterCriminalColour);
		FromJsonColour(j, LIT("MapCriminalColour"), &MapCriminalColour);
		FromJsonColour(j, LIT("MapPoliceColour"), &MapPoliceColour);
		FromJsonColour(j, LIT("MapCivilianColour"), &MapCivilianColour);
		FromJsonColour(j, LIT("MapPoliceCarColour"), &MapPoliceCarColour);
	}
};
