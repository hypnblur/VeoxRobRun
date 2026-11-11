#pragma once
#include "pch.h"
class PlayerConfig
{
	std::string ConfigName;

public:
	PlayerConfig(const std::string& name)
	{
		ConfigName = name;
	}

	bool Health = true;
	bool Distance = true;
	int MaxDistance = 1000;
	bool PrimaryWeapon = true;
	bool SecondaryWeapon = false;
	D2D1::ColorF TextColour = Colour(255, 255, 255);
	int FontSize = 11;
	// Animator/cull check, not wall occlusion. Unity animates local rotations,
	// so the reader compares full local TRS across the whole skeleton and
	// colours every segment from one per-character result.
	bool VisCheck = false;
	D2D1::ColorF VisibleColour = Colour(255, 50, 50);  // animator active
	D2D1::ColorF HiddenColour = Colour(255, 255, 255); // pose frozen / culled
	bool WallCheck = false;
	std::string BvhPath = "city_colliders.bvh";
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
        j[ConfigName][LIT("Health")] = Health;
        j[ConfigName][LIT("Distance")] = Distance;
        j[ConfigName][LIT("MaxDistance")] = MaxDistance;
        j[ConfigName][LIT("FontSize")] = FontSize;
        j[ConfigName][LIT("PrimaryWeapon")] = PrimaryWeapon;
        j[ConfigName][LIT("SecondaryWeapon")] = SecondaryWeapon;
        ToJsonColour(&j, LIT("TextColour"), &TextColour);
        j[ConfigName][LIT("VisCheck")] = VisCheck;
        ToJsonColour(&j, LIT("VisibleColour"), &VisibleColour);
        ToJsonColour(&j, LIT("HiddenColour"), &HiddenColour);
        j[ConfigName][LIT("WallCheck")] = WallCheck;
        j[ConfigName][LIT("BvhPath")] = BvhPath;

        return j;
    }
    void FromJson(const json& j)
    {
        if (!j.contains(ConfigName))
            return;
        if (j[ConfigName].contains(LIT("Health")))
            Health = j[ConfigName][LIT("Health")];
        if (j[ConfigName].contains(LIT("Distance")))
            Distance = j[ConfigName][LIT("Distance")];
        if (j[ConfigName].contains(LIT("PrimaryWeapon")))
            PrimaryWeapon = j[ConfigName][LIT("PrimaryWeapon")];
        if (j[ConfigName].contains(LIT("SecondaryWeapon")))
            SecondaryWeapon = j[ConfigName][LIT("SecondaryWeapon")];
        if (j[ConfigName].contains(LIT("FontSize")))
            FontSize = j[ConfigName][LIT("FontSize")];
        if (j[ConfigName].contains(LIT("MaxDistance")))
            MaxDistance = j[ConfigName][LIT("MaxDistance")];
        FromJsonColour(j, LIT("TextColour"), &TextColour);
        if (j[ConfigName].contains(LIT("VisCheck")))
            VisCheck = j[ConfigName][LIT("VisCheck")];
        FromJsonColour(j, LIT("VisibleColour"), &VisibleColour);
        FromJsonColour(j, LIT("HiddenColour"), &HiddenColour);
        if (j[ConfigName].contains(LIT("WallCheck")))
            WallCheck = j[ConfigName][LIT("WallCheck")];
        if (j[ConfigName].contains(LIT("BvhPath")))
            BvhPath = j[ConfigName][LIT("BvhPath")];
    }
};

