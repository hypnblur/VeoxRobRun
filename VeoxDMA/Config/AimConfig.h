#pragma once
#include "pch.h"

// Aim settings for the Device Connections tab. Memory Aim drives the host
// cursor through SendInput (relative counts); Hardware Aim submits the same
// motion to the selected KMBox/Makcu/Ferrum device through DeviceSession.
class AimConfig
{
	std::string ConfigName;

public:
	AimConfig(const std::string& name)
	{
		ConfigName = name;
	}

	bool Enabled = false;
	int AimType = 0;        // 0 = Memory Aim, 1 = Hardware Aim
	int TargetBone = 0;     // 0 = Head, 1 = Neck, 2 = Chest, 3 = Pelvis
	int Smoothness = 6;     // 1 (instant) .. 100 (very slow)
	int Fov = 40;           // 5 .. 180 degrees
	int AimKey = 0;         // index into Aim::KeyNames()
	int MaxDistance = 400;  // metres
	bool ShowFov = true;
	// Draws the live aim state (key source, candidate count, chosen target,
	// pixel delta and the reason the loop stopped) in the top-left corner.
	bool ShowDebug = true;
	// Only consider characters whose animator is still evaluating (the same
	// per-character state the ESP colours as "visible"). This is an
	// animator/cull check, not wall occlusion: read-only DMA cannot tell
	// whether a bone is behind geometry.
	bool VisibleBonesOnly = false;

	json ToJson()
	{
		json j;
		j[ConfigName][LIT("Enabled")] = Enabled;
		j[ConfigName][LIT("AimType")] = AimType;
		j[ConfigName][LIT("TargetBone")] = TargetBone;
		j[ConfigName][LIT("Smoothness")] = Smoothness;
		j[ConfigName][LIT("Fov")] = Fov;
		j[ConfigName][LIT("AimKey")] = AimKey;
		j[ConfigName][LIT("MaxDistance")] = MaxDistance;
		j[ConfigName][LIT("ShowFov")] = ShowFov;
		j[ConfigName][LIT("ShowDebug")] = ShowDebug;
		j[ConfigName][LIT("VisibleBonesOnly")] = VisibleBonesOnly;
		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		if (j[ConfigName].contains(LIT("Enabled")))
			Enabled = j[ConfigName][LIT("Enabled")];
		if (j[ConfigName].contains(LIT("AimType")))
			AimType = j[ConfigName][LIT("AimType")];
		if (j[ConfigName].contains(LIT("TargetBone")))
			TargetBone = j[ConfigName][LIT("TargetBone")];
		if (j[ConfigName].contains(LIT("Smoothness")))
			Smoothness = j[ConfigName][LIT("Smoothness")];
		if (j[ConfigName].contains(LIT("Fov")))
			Fov = j[ConfigName][LIT("Fov")];
		if (j[ConfigName].contains(LIT("AimKey")))
			AimKey = j[ConfigName][LIT("AimKey")];
		if (j[ConfigName].contains(LIT("MaxDistance")))
			MaxDistance = j[ConfigName][LIT("MaxDistance")];
		if (j[ConfigName].contains(LIT("ShowFov")))
			ShowFov = j[ConfigName][LIT("ShowFov")];
		if (j[ConfigName].contains(LIT("ShowDebug")))
			ShowDebug = j[ConfigName][LIT("ShowDebug")];
		if (j[ConfigName].contains(LIT("VisibleBonesOnly")))
			VisibleBonesOnly = j[ConfigName][LIT("VisibleBonesOnly")];
	}
};
