#pragma once
#include "pch.h"

class WorldConfig
{
	std::string ConfigName;

public:
	WorldConfig(const std::string& name)
	{
		ConfigName = name;
	}

	bool Enabled = true;
	bool Containers = true;
	bool Loot = true;
	bool Vehicles = true;
	bool Grenades = true;
	bool ShowEmpty = true;
	bool ShowDistance = true;
	int MaxDistance = 600;
	int FontSize = 10;
	D2D1::ColorF ContainerColour = Colour(255, 190, 80);
	D2D1::ColorF LootColour = Colour(120, 255, 140);
	D2D1::ColorF VehicleColour = Colour(120, 190, 255);
	D2D1::ColorF GrenadeColour = Colour(255, 90, 90);

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
		j[ConfigName][LIT("Enabled")] = Enabled;
		j[ConfigName][LIT("Containers")] = Containers;
		j[ConfigName][LIT("Loot")] = Loot;
		j[ConfigName][LIT("Vehicles")] = Vehicles;
		j[ConfigName][LIT("Grenades")] = Grenades;
		j[ConfigName][LIT("ShowEmpty")] = ShowEmpty;
		j[ConfigName][LIT("ShowDistance")] = ShowDistance;
		j[ConfigName][LIT("MaxDistance")] = MaxDistance;
		j[ConfigName][LIT("FontSize")] = FontSize;
		ToJsonColour(&j, LIT("ContainerColour"), &ContainerColour);
		ToJsonColour(&j, LIT("LootColour"), &LootColour);
		ToJsonColour(&j, LIT("VehicleColour"), &VehicleColour);
		ToJsonColour(&j, LIT("GrenadeColour"), &GrenadeColour);
		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		if (j[ConfigName].contains(LIT("Enabled")))
			Enabled = j[ConfigName][LIT("Enabled")];
		if (j[ConfigName].contains(LIT("Containers")))
			Containers = j[ConfigName][LIT("Containers")];
		if (j[ConfigName].contains(LIT("Loot")))
			Loot = j[ConfigName][LIT("Loot")];
		if (j[ConfigName].contains(LIT("Vehicles")))
			Vehicles = j[ConfigName][LIT("Vehicles")];
		if (j[ConfigName].contains(LIT("Grenades")))
			Grenades = j[ConfigName][LIT("Grenades")];
		if (j[ConfigName].contains(LIT("ShowEmpty")))
			ShowEmpty = j[ConfigName][LIT("ShowEmpty")];
		if (j[ConfigName].contains(LIT("ShowDistance")))
			ShowDistance = j[ConfigName][LIT("ShowDistance")];
		if (j[ConfigName].contains(LIT("MaxDistance")))
			MaxDistance = j[ConfigName][LIT("MaxDistance")];
		if (j[ConfigName].contains(LIT("FontSize")))
			FontSize = j[ConfigName][LIT("FontSize")];
		FromJsonColour(j, LIT("ContainerColour"), &ContainerColour);
		FromJsonColour(j, LIT("LootColour"), &LootColour);
		FromJsonColour(j, LIT("VehicleColour"), &VehicleColour);
		FromJsonColour(j, LIT("GrenadeColour"), &GrenadeColour);
	}
};
