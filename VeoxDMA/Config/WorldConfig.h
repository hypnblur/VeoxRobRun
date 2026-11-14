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
	bool Valuables = true;
	bool PointsOfInterest = true;
	bool ShowEmpty = true;
	bool ShowDistance = true;
	int MaxDistance = 600;
	int FontSize = 10;

	// Per-collection toggles for the new metadata dump. The group flags above
	// stay as master switches: turning "Containers" off hides every container
	// subtype regardless of the individual checkbox.
	bool LootNodes = true;
	bool SafeDoors = true;
	bool StorageBoxes = true;
	bool Shelvings = true;
	bool CollectionTables = true;
	bool PlotContainers = true;
	bool DroppedBags = true;
	bool VehicleLoots = true;
	bool Cargos = true;
	bool PlotLoots = true;
	bool FurnitureLoots = true;
	bool BuildingLoots = true;
	bool CashLootNodes = true;
	bool SettlementCash = true;
	bool MoneyWorkbenches = true;
	bool SecurityAlarms = true;
	bool FirstAidCabinets = true;
	bool CryptoWorkbenches = true;
	bool VehicleTrunks = true;

	D2D1::ColorF ContainerColour = Colour(255, 190, 80);
	D2D1::ColorF LootColour = Colour(120, 255, 140);
	D2D1::ColorF VehicleColour = Colour(120, 190, 255);
	D2D1::ColorF GrenadeColour = Colour(255, 90, 90);
	D2D1::ColorF ValuableColour = Colour(255, 215, 0);
	D2D1::ColorF PointOfInterestColour = Colour(200, 150, 255);

	D2D1::ColorF LootNodeColour = Colour(255, 190, 80);
	D2D1::ColorF SafeDoorColour = Colour(255, 215, 0);
	D2D1::ColorF StorageBoxColour = Colour(255, 170, 60);
	D2D1::ColorF ShelvingColour = Colour(255, 150, 90);
	D2D1::ColorF CollectionTableColour = Colour(255, 200, 120);
	D2D1::ColorF PlotContainerColour = Colour(255, 160, 60);

	D2D1::ColorF DroppedBagColour = Colour(120, 255, 140);
	D2D1::ColorF VehicleLootColour = Colour(120, 255, 140);
	D2D1::ColorF CargoColour = Colour(255, 230, 120);
	D2D1::ColorF PlotLootColour = Colour(120, 255, 140);
	D2D1::ColorF FurnitureLootColour = Colour(160, 255, 180);
	D2D1::ColorF BuildingLootColour = Colour(160, 255, 180);

	D2D1::ColorF CashLootNodeColour = Colour(255, 215, 0);
	D2D1::ColorF SettlementCashColour = Colour(255, 225, 80);
	D2D1::ColorF MoneyWorkbenchColour = Colour(255, 200, 0);
	D2D1::ColorF SecurityAlarmColour = Colour(255, 75, 75);
	D2D1::ColorF FirstAidColour = Colour(100, 220, 255);
	D2D1::ColorF CryptoWorkbenchColour = Colour(255, 170, 0);
	D2D1::ColorF VehicleTrunkColour = Colour(140, 180, 255);

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

	void ToJsonBool(json* j, const std::string& name, bool value)
	{
		(*j)[ConfigName][name] = value;
	}
	void FromJsonBool(json j, const std::string& name, bool* value)
	{
		if (j[ConfigName].contains(name))
			*value = j[ConfigName][name];
	}

	json ToJson()
	{
		json j;
		j[ConfigName][LIT("Enabled")] = Enabled;
		j[ConfigName][LIT("Containers")] = Containers;
		j[ConfigName][LIT("Loot")] = Loot;
		j[ConfigName][LIT("Vehicles")] = Vehicles;
		j[ConfigName][LIT("Grenades")] = Grenades;
		j[ConfigName][LIT("Valuables")] = Valuables;
		j[ConfigName][LIT("PointsOfInterest")] = PointsOfInterest;
		j[ConfigName][LIT("ShowEmpty")] = ShowEmpty;
		j[ConfigName][LIT("ShowDistance")] = ShowDistance;
		j[ConfigName][LIT("MaxDistance")] = MaxDistance;
		j[ConfigName][LIT("FontSize")] = FontSize;

		ToJsonBool(&j, LIT("LootNodes"), LootNodes);
		ToJsonBool(&j, LIT("SafeDoors"), SafeDoors);
		ToJsonBool(&j, LIT("StorageBoxes"), StorageBoxes);
		ToJsonBool(&j, LIT("Shelvings"), Shelvings);
		ToJsonBool(&j, LIT("CollectionTables"), CollectionTables);
		ToJsonBool(&j, LIT("PlotContainers"), PlotContainers);
		ToJsonBool(&j, LIT("DroppedBags"), DroppedBags);
		ToJsonBool(&j, LIT("VehicleLoots"), VehicleLoots);
		ToJsonBool(&j, LIT("Cargos"), Cargos);
		ToJsonBool(&j, LIT("PlotLoots"), PlotLoots);
		ToJsonBool(&j, LIT("FurnitureLoots"), FurnitureLoots);
		ToJsonBool(&j, LIT("BuildingLoots"), BuildingLoots);
		ToJsonBool(&j, LIT("CashLootNodes"), CashLootNodes);
		ToJsonBool(&j, LIT("SettlementCash"), SettlementCash);
		ToJsonBool(&j, LIT("MoneyWorkbenches"), MoneyWorkbenches);
		ToJsonBool(&j, LIT("SecurityAlarms"), SecurityAlarms);
		ToJsonBool(&j, LIT("FirstAidCabinets"), FirstAidCabinets);
		ToJsonBool(&j, LIT("CryptoWorkbenches"), CryptoWorkbenches);
		ToJsonBool(&j, LIT("VehicleTrunks"), VehicleTrunks);

		ToJsonColour(&j, LIT("ContainerColour"), &ContainerColour);
		ToJsonColour(&j, LIT("LootColour"), &LootColour);
		ToJsonColour(&j, LIT("VehicleColour"), &VehicleColour);
		ToJsonColour(&j, LIT("GrenadeColour"), &GrenadeColour);
		ToJsonColour(&j, LIT("ValuableColour"), &ValuableColour);
		ToJsonColour(&j, LIT("PointOfInterestColour"), &PointOfInterestColour);

		ToJsonColour(&j, LIT("LootNodeColour"), &LootNodeColour);
		ToJsonColour(&j, LIT("SafeDoorColour"), &SafeDoorColour);
		ToJsonColour(&j, LIT("StorageBoxColour"), &StorageBoxColour);
		ToJsonColour(&j, LIT("ShelvingColour"), &ShelvingColour);
		ToJsonColour(&j, LIT("CollectionTableColour"), &CollectionTableColour);
		ToJsonColour(&j, LIT("PlotContainerColour"), &PlotContainerColour);
		ToJsonColour(&j, LIT("DroppedBagColour"), &DroppedBagColour);
		ToJsonColour(&j, LIT("VehicleLootColour"), &VehicleLootColour);
		ToJsonColour(&j, LIT("CargoColour"), &CargoColour);
		ToJsonColour(&j, LIT("PlotLootColour"), &PlotLootColour);
		ToJsonColour(&j, LIT("FurnitureLootColour"), &FurnitureLootColour);
		ToJsonColour(&j, LIT("BuildingLootColour"), &BuildingLootColour);
		ToJsonColour(&j, LIT("CashLootNodeColour"), &CashLootNodeColour);
		ToJsonColour(&j, LIT("SettlementCashColour"), &SettlementCashColour);
		ToJsonColour(&j, LIT("MoneyWorkbenchColour"), &MoneyWorkbenchColour);
		ToJsonColour(&j, LIT("SecurityAlarmColour"), &SecurityAlarmColour);
		ToJsonColour(&j, LIT("FirstAidColour"), &FirstAidColour);
		ToJsonColour(&j, LIT("CryptoWorkbenchColour"), &CryptoWorkbenchColour);
		ToJsonColour(&j, LIT("VehicleTrunkColour"), &VehicleTrunkColour);
		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		Enabled = j[ConfigName].value(LIT("Enabled"), Enabled);
		Containers = j[ConfigName].value(LIT("Containers"), Containers);
		Loot = j[ConfigName].value(LIT("Loot"), Loot);
		Vehicles = j[ConfigName].value(LIT("Vehicles"), Vehicles);
		Grenades = j[ConfigName].value(LIT("Grenades"), Grenades);
		Valuables = j[ConfigName].value(LIT("Valuables"), Valuables);
		PointsOfInterest = j[ConfigName].value(LIT("PointsOfInterest"), PointsOfInterest);
		ShowEmpty = j[ConfigName].value(LIT("ShowEmpty"), ShowEmpty);
		ShowDistance = j[ConfigName].value(LIT("ShowDistance"), ShowDistance);
		MaxDistance = j[ConfigName].value(LIT("MaxDistance"), MaxDistance);
		FontSize = j[ConfigName].value(LIT("FontSize"), FontSize);

		FromJsonBool(j, LIT("LootNodes"), &LootNodes);
		FromJsonBool(j, LIT("SafeDoors"), &SafeDoors);
		FromJsonBool(j, LIT("StorageBoxes"), &StorageBoxes);
		FromJsonBool(j, LIT("Shelvings"), &Shelvings);
		FromJsonBool(j, LIT("CollectionTables"), &CollectionTables);
		FromJsonBool(j, LIT("PlotContainers"), &PlotContainers);
		FromJsonBool(j, LIT("DroppedBags"), &DroppedBags);
		FromJsonBool(j, LIT("VehicleLoots"), &VehicleLoots);
		FromJsonBool(j, LIT("Cargos"), &Cargos);
		FromJsonBool(j, LIT("PlotLoots"), &PlotLoots);
		FromJsonBool(j, LIT("FurnitureLoots"), &FurnitureLoots);
		FromJsonBool(j, LIT("BuildingLoots"), &BuildingLoots);
		FromJsonBool(j, LIT("CashLootNodes"), &CashLootNodes);
		FromJsonBool(j, LIT("SettlementCash"), &SettlementCash);
		FromJsonBool(j, LIT("MoneyWorkbenches"), &MoneyWorkbenches);
		FromJsonBool(j, LIT("SecurityAlarms"), &SecurityAlarms);
		FromJsonBool(j, LIT("FirstAidCabinets"), &FirstAidCabinets);
		FromJsonBool(j, LIT("CryptoWorkbenches"), &CryptoWorkbenches);
		FromJsonBool(j, LIT("VehicleTrunks"), &VehicleTrunks);

		FromJsonColour(j, LIT("ContainerColour"), &ContainerColour);
		FromJsonColour(j, LIT("LootColour"), &LootColour);
		FromJsonColour(j, LIT("VehicleColour"), &VehicleColour);
		FromJsonColour(j, LIT("GrenadeColour"), &GrenadeColour);
		FromJsonColour(j, LIT("ValuableColour"), &ValuableColour);
		FromJsonColour(j, LIT("PointOfInterestColour"), &PointOfInterestColour);

		FromJsonColour(j, LIT("LootNodeColour"), &LootNodeColour);
		FromJsonColour(j, LIT("SafeDoorColour"), &SafeDoorColour);
		FromJsonColour(j, LIT("StorageBoxColour"), &StorageBoxColour);
		FromJsonColour(j, LIT("ShelvingColour"), &ShelvingColour);
		FromJsonColour(j, LIT("CollectionTableColour"), &CollectionTableColour);
		FromJsonColour(j, LIT("PlotContainerColour"), &PlotContainerColour);
		FromJsonColour(j, LIT("DroppedBagColour"), &DroppedBagColour);
		FromJsonColour(j, LIT("VehicleLootColour"), &VehicleLootColour);
		FromJsonColour(j, LIT("CargoColour"), &CargoColour);
		FromJsonColour(j, LIT("PlotLootColour"), &PlotLootColour);
		FromJsonColour(j, LIT("FurnitureLootColour"), &FurnitureLootColour);
		FromJsonColour(j, LIT("BuildingLootColour"), &BuildingLootColour);
		FromJsonColour(j, LIT("CashLootNodeColour"), &CashLootNodeColour);
		FromJsonColour(j, LIT("SettlementCashColour"), &SettlementCashColour);
		FromJsonColour(j, LIT("MoneyWorkbenchColour"), &MoneyWorkbenchColour);
		FromJsonColour(j, LIT("SecurityAlarmColour"), &SecurityAlarmColour);
		FromJsonColour(j, LIT("FirstAidColour"), &FirstAidColour);
		FromJsonColour(j, LIT("CryptoWorkbenchColour"), &CryptoWorkbenchColour);
		FromJsonColour(j, LIT("VehicleTrunkColour"), &VehicleTrunkColour);
	}
};
