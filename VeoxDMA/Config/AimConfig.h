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
	// Hardware Aim only: multiplies the pixel error before it is sent to the
	// device, so a low in-game sensitivity (where one mouse count travels less
	// than one screen pixel) can be compensated without changing Smoothness.
	float HardwareGain = 1.0f; // 0.25 .. 5.0
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

	// --- Recoil / spread suppression ----------------------------------
	bool NoRecoil = false;
	bool NoSpread = false;
	bool DynamicRecoilCompensation = true;
	float RecoilStrength = 1.0f;

	// --- Role filters ---------------------------------------------------
	// Entity roles are resolved by name through reflection, so these match the
	// ESP's labels exactly. Tick a box to ignore that role when picking a target.
	bool ExpIgnorePolice = false;
	bool ExpIgnoreCriminal = false;
	bool ExpIgnoreCivilian = false;
	// Skip entities that share the local player's party. Party membership is
	// read from the battle party data and matched by public id/entity id, so
	// same-role players outside the party are still valid targets. It does
	// nothing while the party data cannot be resolved.
	bool ExpIgnoreTeammates = true;

	// Hardware Device connection settings
	int DeviceIndex = 0;
	std::string ComPort = "";
	std::string IpAddr = "";
	std::string NetPort = "8899";
	std::string MacId = "";
	int BaudIndex = 0;
	bool ManualPort = false;
	bool FerrumController = false;

	json ToJson()
	{
		json j;
		j[ConfigName][LIT("Enabled")] = Enabled;
		j[ConfigName][LIT("AimType")] = AimType;
		j[ConfigName][LIT("TargetBone")] = TargetBone;
		j[ConfigName][LIT("Smoothness")] = Smoothness;
		j[ConfigName][LIT("HardwareGain")] = HardwareGain;
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
		j[ConfigName][LIT("NoRecoil")] = NoRecoil;
		j[ConfigName][LIT("NoSpread")] = NoSpread;
		j[ConfigName][LIT("DynamicRecoilCompensation")] = DynamicRecoilCompensation;
		j[ConfigName][LIT("RecoilStrength")] = RecoilStrength;
		j[ConfigName][LIT("ExpIgnorePolice")] = ExpIgnorePolice;
		j[ConfigName][LIT("ExpIgnoreCriminal")] = ExpIgnoreCriminal;
		j[ConfigName][LIT("ExpIgnoreCivilian")] = ExpIgnoreCivilian;
		j[ConfigName][LIT("ExpIgnoreTeammates")] = ExpIgnoreTeammates;
		j[ConfigName][LIT("DeviceIndex")] = DeviceIndex;
		j[ConfigName][LIT("ComPort")] = ComPort;
		j[ConfigName][LIT("IpAddr")] = IpAddr;
		j[ConfigName][LIT("NetPort")] = NetPort;
		j[ConfigName][LIT("MacId")] = MacId;
		j[ConfigName][LIT("BaudIndex")] = BaudIndex;
		j[ConfigName][LIT("ManualPort")] = ManualPort;
		j[ConfigName][LIT("FerrumController")] = FerrumController;
		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		Enabled = j[ConfigName].value(LIT("Enabled"), Enabled);
		AimType = j[ConfigName].value(LIT("AimType"), AimType);
		TargetBone = j[ConfigName].value(LIT("TargetBone"), TargetBone);
		Smoothness = j[ConfigName].value(LIT("Smoothness"), Smoothness);
		HardwareGain = j[ConfigName].value(LIT("HardwareGain"), HardwareGain);
		Fov = j[ConfigName].value(LIT("Fov"), Fov);
		AimKey = j[ConfigName].value(LIT("AimKey"), AimKey);
		MaxDistance = j[ConfigName].value(LIT("MaxDistance"), MaxDistance);
		ShowFov = j[ConfigName].value(LIT("ShowFov"), ShowFov);
		ShowDebug = j[ConfigName].value(LIT("ShowDebug"), ShowDebug);
		VisibleBonesOnly = j[ConfigName].value(LIT("VisibleBonesOnly"), VisibleBonesOnly);
		MemoryWrite = j[ConfigName].value(LIT("MemoryWrite"), MemoryWrite);
		CursorFallback = j[ConfigName].value(LIT("CursorFallback"), CursorFallback);
		ResponseMs = j[ConfigName].value(LIT("ResponseMs"), ResponseMs);
		Prediction = j[ConfigName].value(LIT("Prediction"), Prediction);
		PredictionStrength = j[ConfigName].value(LIT("PredictionStrength"), PredictionStrength);
		PredictionYScale = j[ConfigName].value(LIT("PredictionYScale"), PredictionYScale);
		HardLock = j[ConfigName].value(LIT("HardLock"), HardLock);
		NoRecoil = j[ConfigName].value(LIT("NoRecoil"), NoRecoil);
		NoSpread = j[ConfigName].value(LIT("NoSpread"), NoSpread);
		DynamicRecoilCompensation = j[ConfigName].value(LIT("DynamicRecoilCompensation"), DynamicRecoilCompensation);
		RecoilStrength = j[ConfigName].value(LIT("RecoilStrength"), RecoilStrength);
		ExpIgnorePolice = j[ConfigName].value(LIT("ExpIgnorePolice"), ExpIgnorePolice);
		ExpIgnoreCriminal = j[ConfigName].value(LIT("ExpIgnoreCriminal"), ExpIgnoreCriminal);
		ExpIgnoreCivilian = j[ConfigName].value(LIT("ExpIgnoreCivilian"), ExpIgnoreCivilian);
		ExpIgnoreTeammates = j[ConfigName].value(LIT("ExpIgnoreTeammates"), ExpIgnoreTeammates);
		DeviceIndex = j[ConfigName].value(LIT("DeviceIndex"), DeviceIndex);
		ComPort = j[ConfigName].value(LIT("ComPort"), ComPort);
		IpAddr = j[ConfigName].value(LIT("IpAddr"), IpAddr);
		NetPort = j[ConfigName].value(LIT("NetPort"), NetPort);
		MacId = j[ConfigName].value(LIT("MacId"), MacId);
		BaudIndex = j[ConfigName].value(LIT("BaudIndex"), BaudIndex);
		ManualPort = j[ConfigName].value(LIT("ManualPort"), ManualPort);
		FerrumController = j[ConfigName].value(LIT("FerrumController"), FerrumController);
	}
};
