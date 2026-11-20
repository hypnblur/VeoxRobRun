#pragma once
#include "pch.h"
#include <string>
#include <algorithm>

inline std::string FormatCash(int amount)
{
	std::string s = std::to_string(amount);
	int n = (int)s.length() - 3;
	while (n > 0) {
		s.insert(n, ",");
		n -= 3;
	}
	return s;
}

inline std::uint32_t ColorToImU32(const D2D1::ColorF& c)
{
	return ((std::uint32_t)(c.a * 255.f) << 24) |
	       ((std::uint32_t)(c.b * 255.f) << 16) |
	       ((std::uint32_t)(c.g * 255.f) << 8) |
	       ((std::uint32_t)(c.r * 255.f));
}

class LootFilterConfig
{
	std::string ConfigName;

public:
	LootFilterConfig(const std::string& name)
	{
		ConfigName = name;
	}

	// Master toggles
	bool Enabled = true;
	bool ShowAllOnMap = true;
	bool ShowOnMinimap = true;
	bool ShowNames = true;
	bool ShowPrices = true;
	bool ShowDistance = true;
	bool HideEmpty = true;
	float BaseDotSize = 3.5f;
	int MaxDistance = 1000;

	// Price filtering & tiers
	bool FilterByMinPrice = false;
	int MinPrice = 0;
	int HighValueThreshold = 10000;
	int MidValueThreshold = 2500;
	bool HighValuePulse = true;

	// Price tier colours
	D2D1::ColorF HighValueColour = Colour(255, 215, 0);     // Gold
	D2D1::ColorF MidValueColour = Colour(180, 130, 255);    // Purple / Amethyst
	D2D1::ColorF StandardColour = Colour(120, 255, 140);    // Light Green

	// Category toggles & colours
	// Valuables & Cash
	bool CashNodes = true;
	D2D1::ColorF CashNodeColour = Colour(255, 215, 0);
	bool SettlementCash = true;
	D2D1::ColorF SettlementCashColour = Colour(255, 225, 80);
	bool MoneyWorkbenches = true;
	D2D1::ColorF MoneyWorkbenchColour = Colour(255, 200, 0);
	bool CollectionTables = true;
	D2D1::ColorF CollectionTableColour = Colour(255, 200, 120);
	bool DroppedBags = true;
	D2D1::ColorF DroppedBagColour = Colour(120, 255, 140);
	bool CryptoWorkbenches = true;
	D2D1::ColorF CryptoColour = Colour(255, 170, 0);
	bool DrugLabs = true;
	D2D1::ColorF DrugLabColour = Colour(0, 220, 100);

	// Containers & Safes
	bool SafeDoors = true;
	D2D1::ColorF SafeDoorColour = Colour(255, 215, 0);
	bool StorageBoxes = true;
	D2D1::ColorF StorageBoxColour = Colour(255, 170, 60);
	bool Shelving = true;
	D2D1::ColorF ShelvingColour = Colour(255, 150, 90);
	bool PlotContainers = true;
	D2D1::ColorF PlotContainerColour = Colour(255, 160, 60);
	bool LootNodes = true;
	D2D1::ColorF LootNodeColour = Colour(255, 190, 80);
	bool Containers = true;
	D2D1::ColorF ContainerColour = Colour(255, 190, 80);
	bool VehicleTrunks = true;
	D2D1::ColorF VehicleTrunkColour = Colour(140, 180, 255);
	bool FirstAid = true;
	D2D1::ColorF FirstAidColour = Colour(100, 220, 255);

	// World & Loose Loot
	bool StaticLoot = true;
	D2D1::ColorF StaticLootColour = Colour(120, 255, 140);
	bool RandomLoot = true;
	D2D1::ColorF RandomLootColour = Colour(160, 255, 180);
	bool VehicleLoot = true;
	D2D1::ColorF VehicleLootColour = Colour(120, 255, 140);
	bool Cargo = true;
	D2D1::ColorF CargoColour = Colour(255, 230, 120);
	bool PlotLoot = true;
	D2D1::ColorF PlotLootColour = Colour(120, 255, 140);
	bool FurnitureLoot = true;
	D2D1::ColorF FurnitureLootColour = Colour(160, 255, 180);
	bool BuildingLoot = true;
	D2D1::ColorF BuildingLootColour = Colour(160, 255, 180);

	// Custom item search
	std::string ItemSearchQuery = "";

	void ToJsonColour(json* j, const std::string& name, const D2D1::ColorF& colour)
	{
		(*j)[ConfigName][name][LIT("r")] = colour.r;
		(*j)[ConfigName][name][LIT("g")] = colour.g;
		(*j)[ConfigName][name][LIT("b")] = colour.b;
		(*j)[ConfigName][name][LIT("a")] = colour.a;
	}

	void FromJsonColour(const json& j, const std::string& name, D2D1::ColorF& colour)
	{
		if (j.contains(ConfigName) && j[ConfigName].contains(name))
		{
			colour.r = j[ConfigName][name].value(LIT("r"), colour.r);
			colour.g = j[ConfigName][name].value(LIT("g"), colour.g);
			colour.b = j[ConfigName][name].value(LIT("b"), colour.b);
			colour.a = j[ConfigName][name].value(LIT("a"), colour.a);
		}
	}

	json ToJson()
	{
		json j;
		auto& s = j[ConfigName];
		s[LIT("Enabled")] = Enabled;
		s[LIT("ShowAllOnMap")] = ShowAllOnMap;
		s[LIT("ShowOnMinimap")] = ShowOnMinimap;
		s[LIT("ShowNames")] = ShowNames;
		s[LIT("ShowPrices")] = ShowPrices;
		s[LIT("ShowDistance")] = ShowDistance;
		s[LIT("HideEmpty")] = HideEmpty;
		s[LIT("BaseDotSize")] = BaseDotSize;
		s[LIT("MaxDistance")] = MaxDistance;

		s[LIT("FilterByMinPrice")] = FilterByMinPrice;
		s[LIT("MinPrice")] = MinPrice;
		s[LIT("HighValueThreshold")] = HighValueThreshold;
		s[LIT("MidValueThreshold")] = MidValueThreshold;
		s[LIT("HighValuePulse")] = HighValuePulse;

		ToJsonColour(&j, LIT("HighValueColour"), HighValueColour);
		ToJsonColour(&j, LIT("MidValueColour"), MidValueColour);
		ToJsonColour(&j, LIT("StandardColour"), StandardColour);

		s[LIT("CashNodes")] = CashNodes;
		s[LIT("SettlementCash")] = SettlementCash;
		s[LIT("MoneyWorkbenches")] = MoneyWorkbenches;
		s[LIT("CollectionTables")] = CollectionTables;
		s[LIT("DroppedBags")] = DroppedBags;
		s[LIT("CryptoWorkbenches")] = CryptoWorkbenches;
		s[LIT("DrugLabs")] = DrugLabs;

		ToJsonColour(&j, LIT("CashNodeColour"), CashNodeColour);
		ToJsonColour(&j, LIT("SettlementCashColour"), SettlementCashColour);
		ToJsonColour(&j, LIT("MoneyWorkbenchColour"), MoneyWorkbenchColour);
		ToJsonColour(&j, LIT("CollectionTableColour"), CollectionTableColour);
		ToJsonColour(&j, LIT("DroppedBagColour"), DroppedBagColour);
		ToJsonColour(&j, LIT("CryptoColour"), CryptoColour);
		ToJsonColour(&j, LIT("DrugLabColour"), DrugLabColour);

		s[LIT("SafeDoors")] = SafeDoors;
		s[LIT("StorageBoxes")] = StorageBoxes;
		s[LIT("Shelving")] = Shelving;
		s[LIT("PlotContainers")] = PlotContainers;
		s[LIT("LootNodes")] = LootNodes;
		s[LIT("Containers")] = Containers;
		s[LIT("VehicleTrunks")] = VehicleTrunks;
		s[LIT("FirstAid")] = FirstAid;

		ToJsonColour(&j, LIT("SafeDoorColour"), SafeDoorColour);
		ToJsonColour(&j, LIT("StorageBoxColour"), StorageBoxColour);
		ToJsonColour(&j, LIT("ShelvingColour"), ShelvingColour);
		ToJsonColour(&j, LIT("PlotContainerColour"), PlotContainerColour);
		ToJsonColour(&j, LIT("LootNodeColour"), LootNodeColour);
		ToJsonColour(&j, LIT("ContainerColour"), ContainerColour);
		ToJsonColour(&j, LIT("VehicleTrunkColour"), VehicleTrunkColour);
		ToJsonColour(&j, LIT("FirstAidColour"), FirstAidColour);

		s[LIT("StaticLoot")] = StaticLoot;
		s[LIT("RandomLoot")] = RandomLoot;
		s[LIT("VehicleLoot")] = VehicleLoot;
		s[LIT("Cargo")] = Cargo;
		s[LIT("PlotLoot")] = PlotLoot;
		s[LIT("FurnitureLoot")] = FurnitureLoot;
		s[LIT("BuildingLoot")] = BuildingLoot;

		ToJsonColour(&j, LIT("StaticLootColour"), StaticLootColour);
		ToJsonColour(&j, LIT("RandomLootColour"), RandomLootColour);
		ToJsonColour(&j, LIT("VehicleLootColour"), VehicleLootColour);
		ToJsonColour(&j, LIT("CargoColour"), CargoColour);
		ToJsonColour(&j, LIT("PlotLootColour"), PlotLootColour);
		ToJsonColour(&j, LIT("FurnitureLootColour"), FurnitureLootColour);
		ToJsonColour(&j, LIT("BuildingLootColour"), BuildingLootColour);

		s[LIT("ItemSearchQuery")] = ItemSearchQuery;

		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		const auto& s = j[ConfigName];
		Enabled = s.value(LIT("Enabled"), Enabled);
		ShowAllOnMap = s.value(LIT("ShowAllOnMap"), ShowAllOnMap);
		ShowOnMinimap = s.value(LIT("ShowOnMinimap"), ShowOnMinimap);
		ShowNames = s.value(LIT("ShowNames"), ShowNames);
		ShowPrices = s.value(LIT("ShowPrices"), ShowPrices);
		ShowDistance = s.value(LIT("ShowDistance"), ShowDistance);
		HideEmpty = s.value(LIT("HideEmpty"), HideEmpty);
		BaseDotSize = s.value(LIT("BaseDotSize"), BaseDotSize);
		MaxDistance = s.value(LIT("MaxDistance"), MaxDistance);

		FilterByMinPrice = s.value(LIT("FilterByMinPrice"), FilterByMinPrice);
		MinPrice = s.value(LIT("MinPrice"), MinPrice);
		HighValueThreshold = s.value(LIT("HighValueThreshold"), HighValueThreshold);
		MidValueThreshold = s.value(LIT("MidValueThreshold"), MidValueThreshold);
		HighValuePulse = s.value(LIT("HighValuePulse"), HighValuePulse);

		FromJsonColour(j, LIT("HighValueColour"), HighValueColour);
		FromJsonColour(j, LIT("MidValueColour"), MidValueColour);
		FromJsonColour(j, LIT("StandardColour"), StandardColour);

		CashNodes = s.value(LIT("CashNodes"), CashNodes);
		SettlementCash = s.value(LIT("SettlementCash"), SettlementCash);
		MoneyWorkbenches = s.value(LIT("MoneyWorkbenches"), MoneyWorkbenches);
		CollectionTables = s.value(LIT("CollectionTables"), CollectionTables);
		DroppedBags = s.value(LIT("DroppedBags"), DroppedBags);
		CryptoWorkbenches = s.value(LIT("CryptoWorkbenches"), CryptoWorkbenches);
		DrugLabs = s.value(LIT("DrugLabs"), DrugLabs);

		FromJsonColour(j, LIT("CashNodeColour"), CashNodeColour);
		FromJsonColour(j, LIT("SettlementCashColour"), SettlementCashColour);
		FromJsonColour(j, LIT("MoneyWorkbenchColour"), MoneyWorkbenchColour);
		FromJsonColour(j, LIT("CollectionTableColour"), CollectionTableColour);
		FromJsonColour(j, LIT("DroppedBagColour"), DroppedBagColour);
		FromJsonColour(j, LIT("CryptoColour"), CryptoColour);
		FromJsonColour(j, LIT("DrugLabColour"), DrugLabColour);

		SafeDoors = s.value(LIT("SafeDoors"), SafeDoors);
		StorageBoxes = s.value(LIT("StorageBoxes"), StorageBoxes);
		Shelving = s.value(LIT("Shelving"), Shelving);
		PlotContainers = s.value(LIT("PlotContainers"), PlotContainers);
		LootNodes = s.value(LIT("LootNodes"), LootNodes);
		Containers = s.value(LIT("Containers"), Containers);
		VehicleTrunks = s.value(LIT("VehicleTrunks"), VehicleTrunks);
		FirstAid = s.value(LIT("FirstAid"), FirstAid);

		FromJsonColour(j, LIT("SafeDoorColour"), SafeDoorColour);
		FromJsonColour(j, LIT("StorageBoxColour"), StorageBoxColour);
		FromJsonColour(j, LIT("ShelvingColour"), ShelvingColour);
		FromJsonColour(j, LIT("PlotContainerColour"), PlotContainerColour);
		FromJsonColour(j, LIT("LootNodeColour"), LootNodeColour);
		FromJsonColour(j, LIT("ContainerColour"), ContainerColour);
		FromJsonColour(j, LIT("VehicleTrunkColour"), VehicleTrunkColour);
		FromJsonColour(j, LIT("FirstAidColour"), FirstAidColour);

		StaticLoot = s.value(LIT("StaticLoot"), StaticLoot);
		RandomLoot = s.value(LIT("RandomLoot"), RandomLoot);
		VehicleLoot = s.value(LIT("VehicleLoot"), VehicleLoot);
		Cargo = s.value(LIT("Cargo"), Cargo);
		PlotLoot = s.value(LIT("PlotLoot"), PlotLoot);
		FurnitureLoot = s.value(LIT("FurnitureLoot"), FurnitureLoot);
		BuildingLoot = s.value(LIT("BuildingLoot"), BuildingLoot);

		FromJsonColour(j, LIT("StaticLootColour"), StaticLootColour);
		FromJsonColour(j, LIT("RandomLootColour"), RandomLootColour);
		FromJsonColour(j, LIT("VehicleLootColour"), VehicleLootColour);
		FromJsonColour(j, LIT("CargoColour"), CargoColour);
		FromJsonColour(j, LIT("PlotLootColour"), PlotLootColour);
		FromJsonColour(j, LIT("FurnitureLootColour"), FurnitureLootColour);
		FromJsonColour(j, LIT("BuildingLootColour"), BuildingLootColour);

		ItemSearchQuery = s.value(LIT("ItemSearchQuery"), ItemSearchQuery);
	}
};
