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
	bool GrenadePrediction = false;
	bool LegacyContainers = true;
	bool LegacyLoot = true;
	bool LegacyCargo = true;
	bool LegacyValuables = true;
	bool ShowEmpty = true;
	bool ShowDistance = true;
	int MaxDistance = 600;
	int FontSize = 10;
	int MinValueFilter = 0;

	bool InstantLoot = false;
	bool InstantUnlock = false;
	bool UnrevealedLootLabels = false;
	bool GoldKeyDoors = false;
	bool PurpleKeyDoors = false;

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
	bool CargoBushes = true;
	bool CargoCrystals = true;
	bool CargoDrugs = true;
	bool CargoCash = true;
	bool CargoExchange = true;
	bool CargoBoxes = true;
	bool CargoGoods = true;
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
	bool DrugWorkbenches = true;
	bool Weed = true;
	bool DefenseBlocks = true;
	bool HouseEsp = true;
	bool ShowWorkbenchLabels = true;
	bool ShowWorkbenchContents = true;
	int HouseMaxDistance = 300;

	D2D1::ColorF ContainerColour = Colour(255, 190, 80);
	D2D1::ColorF LootColour = Colour(120, 255, 140);
	D2D1::ColorF VehicleColour = Colour(120, 190, 255);
	D2D1::ColorF GrenadeColour = Colour(255, 90, 90);
	D2D1::ColorF ValuableColour = Colour(255, 215, 0);
	D2D1::ColorF LootNodeColour = Colour(255, 190, 80);
	D2D1::ColorF SafeDoorColour = Colour(255, 215, 0);
	D2D1::ColorF StorageBoxColour = Colour(255, 170, 60);
	D2D1::ColorF ShelvingColour = Colour(255, 150, 90);
	D2D1::ColorF CollectionTableColour = Colour(255, 200, 120);
	D2D1::ColorF PlotContainerColour = Colour(255, 160, 60);

	D2D1::ColorF DroppedBagColour = Colour(120, 255, 140);
	D2D1::ColorF VehicleLootColour = Colour(120, 255, 140);
	D2D1::ColorF CargoColour = Colour(255, 230, 120);
	D2D1::ColorF CargoBushColour = Colour(100, 200, 80);
	D2D1::ColorF CargoCrystalColour = Colour(160, 120, 255);
	D2D1::ColorF CargoDrugColour = Colour(255, 100, 160);
	D2D1::ColorF CargoCashColour = Colour(255, 215, 0);
	D2D1::ColorF CargoExchangeColour = Colour(80, 200, 220);
	D2D1::ColorF CargoBoxColour = Colour(200, 160, 100);
	D2D1::ColorF CargoGoodsColour = Colour(180, 220, 140);
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
	D2D1::ColorF DrugWorkbenchColour = Colour(0, 220, 100);
	D2D1::ColorF DefenseBlockColour = Colour(255, 80, 80);

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
		j[ConfigName][LIT("LegacyContainers")] = LegacyContainers;
		j[ConfigName][LIT("LegacyLoot")] = LegacyLoot;
		j[ConfigName][LIT("LegacyCargo")] = LegacyCargo;
		j[ConfigName][LIT("LegacyValuables")] = LegacyValuables;
		j[ConfigName][LIT("ShowWorkbenchLabels")] = ShowWorkbenchLabels;
		j[ConfigName][LIT("ShowWorkbenchContents")] = ShowWorkbenchContents;
		j[ConfigName][LIT("Containers")] = Containers;
		j[ConfigName][LIT("Loot")] = Loot;
		j[ConfigName][LIT("Vehicles")] = Vehicles;
		j[ConfigName][LIT("Grenades")] = Grenades;
		j[ConfigName][LIT("GrenadePrediction")] = GrenadePrediction;
		j[ConfigName][LIT("ShowEmpty")] = ShowEmpty;
		j[ConfigName][LIT("ShowDistance")] = ShowDistance;
		j[ConfigName][LIT("MaxDistance")] = MaxDistance;
		j[ConfigName][LIT("FontSize")] = FontSize;
		j[ConfigName][LIT("MinValueFilter")] = MinValueFilter;
		j[ConfigName][LIT("InstantLoot")] = InstantLoot;
		j[ConfigName][LIT("UnrevealedLootLabels")] = UnrevealedLootLabels;
		j[ConfigName][LIT("GoldKeyDoors")] = GoldKeyDoors;
		j[ConfigName][LIT("PurpleKeyDoors")] = PurpleKeyDoors;
		j[ConfigName][LIT("InstantUnlock")] = InstantUnlock;

		ToJsonBool(&j, LIT("LootNodes"), LootNodes);
		ToJsonBool(&j, LIT("SafeDoors"), SafeDoors);
		ToJsonBool(&j, LIT("StorageBoxes"), StorageBoxes);
		ToJsonBool(&j, LIT("Shelvings"), Shelvings);
		ToJsonBool(&j, LIT("CollectionTables"), CollectionTables);
		ToJsonBool(&j, LIT("PlotContainers"), PlotContainers);
		ToJsonBool(&j, LIT("DroppedBags"), DroppedBags);
		ToJsonBool(&j, LIT("VehicleLoots"), VehicleLoots);
		ToJsonBool(&j, LIT("Cargos"), Cargos);
		ToJsonBool(&j, LIT("CargoBushes"), CargoBushes);
		ToJsonBool(&j, LIT("CargoCrystals"), CargoCrystals);
		ToJsonBool(&j, LIT("CargoDrugs"), CargoDrugs);
		ToJsonBool(&j, LIT("CargoCash"), CargoCash);
		ToJsonBool(&j, LIT("CargoExchange"), CargoExchange);
		ToJsonBool(&j, LIT("CargoBoxes"), CargoBoxes);
		ToJsonBool(&j, LIT("CargoGoods"), CargoGoods);
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
		ToJsonBool(&j, LIT("DrugWorkbenches"), DrugWorkbenches);
		ToJsonBool(&j, LIT("Weed"), Weed);
		ToJsonBool(&j, LIT("DefenseBlocks"), DefenseBlocks);
		ToJsonBool(&j, LIT("HouseEsp"), HouseEsp);
		j[ConfigName][LIT("HouseMaxDistance")] = HouseMaxDistance;

		ToJsonColour(&j, LIT("ContainerColour"), &ContainerColour);
		ToJsonColour(&j, LIT("LootColour"), &LootColour);
		ToJsonColour(&j, LIT("VehicleColour"), &VehicleColour);
		ToJsonColour(&j, LIT("GrenadeColour"), &GrenadeColour);
		ToJsonColour(&j, LIT("ValuableColour"), &ValuableColour);
		ToJsonColour(&j, LIT("LootNodeColour"), &LootNodeColour);
		ToJsonColour(&j, LIT("SafeDoorColour"), &SafeDoorColour);
		ToJsonColour(&j, LIT("StorageBoxColour"), &StorageBoxColour);
		ToJsonColour(&j, LIT("ShelvingColour"), &ShelvingColour);
		ToJsonColour(&j, LIT("CollectionTableColour"), &CollectionTableColour);
		ToJsonColour(&j, LIT("PlotContainerColour"), &PlotContainerColour);
		ToJsonColour(&j, LIT("DroppedBagColour"), &DroppedBagColour);
		ToJsonColour(&j, LIT("VehicleLootColour"), &VehicleLootColour);
		ToJsonColour(&j, LIT("CargoColour"), &CargoColour);
		ToJsonColour(&j, LIT("CargoBushColour"), &CargoBushColour);
		ToJsonColour(&j, LIT("CargoCrystalColour"), &CargoCrystalColour);
		ToJsonColour(&j, LIT("CargoDrugColour"), &CargoDrugColour);
		ToJsonColour(&j, LIT("CargoCashColour"), &CargoCashColour);
		ToJsonColour(&j, LIT("CargoExchangeColour"), &CargoExchangeColour);
		ToJsonColour(&j, LIT("CargoBoxColour"), &CargoBoxColour);
		ToJsonColour(&j, LIT("CargoGoodsColour"), &CargoGoodsColour);
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
		ToJsonColour(&j, LIT("DrugWorkbenchColour"), &DrugWorkbenchColour);
		ToJsonColour(&j, LIT("DefenseBlockColour"), &DefenseBlockColour);
		return j;
	}

	void FromJson(const json& j)
	{
		const json empty = json::object();
		const auto& settings = j.contains(ConfigName) ? j[ConfigName] : empty;
		LegacyContainers = settings.value(LIT("LegacyContainers"), true);
		LegacyLoot = settings.value(LIT("LegacyLoot"), true);
		LegacyCargo = settings.value(LIT("LegacyCargo"), true);
		LegacyValuables = settings.value(LIT("LegacyValuables"), true);
		ShowWorkbenchLabels = settings.value(LIT("ShowWorkbenchLabels"), true);
		ShowWorkbenchContents = settings.value(LIT("ShowWorkbenchContents"), true);
		GoldKeyDoors = settings.value(LIT("GoldKeyDoors"), false);
		PurpleKeyDoors = settings.value(LIT("PurpleKeyDoors"), false);
		Containers = settings.value(LIT("Containers"), true);
		Loot = settings.value(LIT("Loot"), true);
		Grenades = settings.value(LIT("Grenades"), true);
		Cargos = settings.value(LIT("Cargos"), true);
		LootNodes = settings.value(LIT("LootNodes"), true);
		SafeDoors = settings.value(LIT("SafeDoors"), true);
		StorageBoxes = settings.value(LIT("StorageBoxes"), true);
		Shelvings = settings.value(LIT("Shelvings"), true);
		CollectionTables = settings.value(LIT("CollectionTables"), true);
		PlotContainers = settings.value(LIT("PlotContainers"), true);
		VehicleTrunks = settings.value(LIT("VehicleTrunks"), true);
		DroppedBags = settings.value(LIT("DroppedBags"), true);
		VehicleLoots = settings.value(LIT("VehicleLoots"), true);
		PlotLoots = settings.value(LIT("PlotLoots"), true);
		FurnitureLoots = settings.value(LIT("FurnitureLoots"), true);
		BuildingLoots = settings.value(LIT("BuildingLoots"), true);
		CargoBushes = settings.value(LIT("CargoBushes"), true);
		CargoCrystals = settings.value(LIT("CargoCrystals"), true);
		CargoDrugs = settings.value(LIT("CargoDrugs"), true);
		CargoCash = settings.value(LIT("CargoCash"), true);
		CargoExchange = settings.value(LIT("CargoExchange"), true);
		CargoBoxes = settings.value(LIT("CargoBoxes"), true);
		CargoGoods = settings.value(LIT("CargoGoods"), true);
		CashLootNodes = settings.value(LIT("CashLootNodes"), true);
		SettlementCash = settings.value(LIT("SettlementCash"), true);
		MoneyWorkbenches = settings.value(LIT("MoneyWorkbenches"), true);
		CryptoWorkbenches = settings.value(LIT("CryptoWorkbenches"), true);

		// Convert the old group-or-subtype rules into effective subtype choices.
		if (!settings.contains(LIT("LegacyContainers")) && Containers) {
			LootNodes = true;
			SafeDoors = true;
			StorageBoxes = true;
			Shelvings = true;
			CollectionTables = true;
			PlotContainers = true;
			VehicleTrunks = true;
		}
		const bool oldLoot = !settings.contains(LIT("LegacyLoot")) && Loot;
		const bool hasLegacyCargo = settings.contains(LIT("LegacyCargo"));
		if (oldLoot) {
			DroppedBags = true;
			VehicleLoots = true;
			PlotLoots = true;
			FurnitureLoots = true;
			BuildingLoots = true;
		}
		if (!hasLegacyCargo && (oldLoot || Cargos)) {
			Cargos = true;
			CargoBushes = true;
			CargoCrystals = true;
			CargoDrugs = true;
			CargoCash = true;
			CargoExchange = true;
			CargoBoxes = true;
			CargoGoods = true;
		}
		if (!settings.contains(LIT("LegacyValuables")) && settings.value(LIT("Valuables"), true)) {
			CashLootNodes = true;
			SettlementCash = true;
			MoneyWorkbenches = true;
			CryptoWorkbenches = true;
		}

		if (!j.contains(ConfigName))
			return;
		Enabled = j[ConfigName].value(LIT("Enabled"), Enabled);
		Vehicles = j[ConfigName].value(LIT("Vehicles"), Vehicles);
		GrenadePrediction = j[ConfigName].value(LIT("GrenadePrediction"), false);
		ShowEmpty = j[ConfigName].value(LIT("ShowEmpty"), ShowEmpty);
		ShowDistance = j[ConfigName].value(LIT("ShowDistance"), ShowDistance);
		MaxDistance = j[ConfigName].value(LIT("MaxDistance"), MaxDistance);
		FontSize = j[ConfigName].value(LIT("FontSize"), FontSize);
		MinValueFilter = j[ConfigName].value(LIT("MinValueFilter"), MinValueFilter);
		InstantLoot = j[ConfigName].value(LIT("InstantLoot"), InstantLoot);
		UnrevealedLootLabels = j[ConfigName].value(LIT("UnrevealedLootLabels"), false);
		InstantUnlock = j[ConfigName].value(LIT("InstantUnlock"), InstantUnlock);

		FromJsonBool(j, LIT("SecurityAlarms"), &SecurityAlarms);
		FromJsonBool(j, LIT("FirstAidCabinets"), &FirstAidCabinets);
		FromJsonBool(j, LIT("DrugWorkbenches"), &DrugWorkbenches);
		FromJsonBool(j, LIT("Weed"), &Weed);
		FromJsonBool(j, LIT("DefenseBlocks"), &DefenseBlocks);
		FromJsonBool(j, LIT("HouseEsp"), &HouseEsp);
		HouseMaxDistance = j[ConfigName].value(LIT("HouseMaxDistance"), HouseMaxDistance);

		FromJsonColour(j, LIT("ContainerColour"), &ContainerColour);
		FromJsonColour(j, LIT("LootColour"), &LootColour);
		FromJsonColour(j, LIT("VehicleColour"), &VehicleColour);
		FromJsonColour(j, LIT("GrenadeColour"), &GrenadeColour);
		FromJsonColour(j, LIT("ValuableColour"), &ValuableColour);
		FromJsonColour(j, LIT("LootNodeColour"), &LootNodeColour);
		FromJsonColour(j, LIT("SafeDoorColour"), &SafeDoorColour);
		FromJsonColour(j, LIT("StorageBoxColour"), &StorageBoxColour);
		FromJsonColour(j, LIT("ShelvingColour"), &ShelvingColour);
		FromJsonColour(j, LIT("CollectionTableColour"), &CollectionTableColour);
		FromJsonColour(j, LIT("PlotContainerColour"), &PlotContainerColour);
		FromJsonColour(j, LIT("DroppedBagColour"), &DroppedBagColour);
		FromJsonColour(j, LIT("VehicleLootColour"), &VehicleLootColour);
		FromJsonColour(j, LIT("CargoColour"), &CargoColour);
		FromJsonColour(j, LIT("CargoBushColour"), &CargoBushColour);
		FromJsonColour(j, LIT("CargoCrystalColour"), &CargoCrystalColour);
		FromJsonColour(j, LIT("CargoDrugColour"), &CargoDrugColour);
		FromJsonColour(j, LIT("CargoCashColour"), &CargoCashColour);
		FromJsonColour(j, LIT("CargoExchangeColour"), &CargoExchangeColour);
		FromJsonColour(j, LIT("CargoBoxColour"), &CargoBoxColour);
		FromJsonColour(j, LIT("CargoGoodsColour"), &CargoGoodsColour);
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
		FromJsonColour(j, LIT("DrugWorkbenchColour"), &DrugWorkbenchColour);
		FromJsonColour(j, LIT("DefenseBlockColour"), &DefenseBlockColour);
	}
};
