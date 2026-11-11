#pragma once
#include "pch.h"

// Aim settings for the Device Connections tab.
//
// Memory Aim writes the camera orientation/look quaternion and basis vectors
// into BattleCameraModel over DMA (one scatter transaction per step), which
// works on a two-PC setup. If the device rejects writes, or the game rebuilds
// the camera from its own input state, the optional cursor fallback drives the
// host cursor through SendInput instead.
//
// Hardware Aim submits the same pixel error to the selected KMBox/Makcu/Ferrum
// device through DeviceSession.
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
	// pixel delta, write verdict and the reason the loop stopped) in the
	// top-left corner.
	bool ShowDebug = true;
	// Only consider characters whose animator is still evaluating (the same
	// per-character state the ESP colours as "visible"). This is an
	// animator/cull check, not wall occlusion: read-only DMA cannot tell
	// whether a bone is behind geometry.
	bool VisibleBonesOnly = false;

	// --- Memory Aim write path -----------------------------------------
	// Write the camera angles through DMA instead of moving the local cursor.
	bool MemoryWrite = true;
	// If the write is unavailable, fall back to SendInput cursor movement so
	// aim still does something on a single-PC setup.
	bool CursorFallback = true;
	// 0 = derive from Smoothness (20..92 ms). Any other value is an explicit
	// exponential response time in milliseconds.
	int ResponseMs = 0;

	// --- Target leading / sticky lock ----------------------------------
	bool Prediction = false;
	int PredictionStrength = 100; // percent, 0..200 => 0..2x lead
	int PredictionYScale = 50;    // percent, 0..100 of the Y lead
	bool HardLock = false;        // keep the current target outside the FOV

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
		j[ConfigName][LIT("MemoryWrite")] = MemoryWrite;
		j[ConfigName][LIT("CursorFallback")] = CursorFallback;
		j[ConfigName][LIT("ResponseMs")] = ResponseMs;
		j[ConfigName][LIT("Prediction")] = Prediction;
		j[ConfigName][LIT("PredictionStrength")] = PredictionStrength;
		j[ConfigName][LIT("PredictionYScale")] = PredictionYScale;
		j[ConfigName][LIT("HardLock")] = HardLock;
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
		if (j[ConfigName].contains(LIT("MemoryWrite")))
			MemoryWrite = j[ConfigName][LIT("MemoryWrite")];
		if (j[ConfigName].contains(LIT("CursorFallback")))
			CursorFallback = j[ConfigName][LIT("CursorFallback")];
		if (j[ConfigName].contains(LIT("ResponseMs")))
			ResponseMs = j[ConfigName][LIT("ResponseMs")];
		if (j[ConfigName].contains(LIT("Prediction")))
			Prediction = j[ConfigName][LIT("Prediction")];
		if (j[ConfigName].contains(LIT("PredictionStrength")))
			PredictionStrength = j[ConfigName][LIT("PredictionStrength")];
		if (j[ConfigName].contains(LIT("PredictionYScale")))
			PredictionYScale = j[ConfigName][LIT("PredictionYScale")];
		if (j[ConfigName].contains(LIT("HardLock")))
			HardLock = j[ConfigName][LIT("HardLock")];
	}
};
