#pragma once
#include "pch.h"
#include <array>
#include <mutex>
#include <set>

// Per-weapon class profile overriding global targeting and auto-fire settings
// whenever the local player equips a matching weapon class.
struct WeaponProfile
{
	bool Enabled = false;
	int TargetBone = -1;              // -1 = Inherit, 0 = Head, 1 = Neck, 2 = Chest, 3 = Pelvis, 4 = Closest
	int FovOverride = 0;              // 0 = Inherit global FOV, else 5..180 deg
	int SmoothnessOverride = 0;       // 0 = Inherit, else 1..100
	int SmoothAlgorithmOverride = -1; // -1 = Inherit, 0 = EMA, 1 = Linear, 2 = Bezier
	int TriggerKeyOverride = -1;      // -1 = Inherit global trigger key, else index into Aim::KeyNames()
	bool TriggerEnabled = true;       // allow auto-fire when this profile is active
	int TriggerMinDelayMs = -1;       // -1 = Inherit, else 0..500 ms
	int TriggerMaxDelayMs = -1;       // -1 = Inherit, else 0..500 ms
	float TriggerHitboxScale = 0.f;   // 0 = Inherit, else 0.25..2.0
	int TriggerMaxDistance = 0;       // 0 = Inherit, else 5..500 m

	json ToJson() const
	{
		json j;
		j[LIT("Enabled")] = Enabled;
		j[LIT("TargetBone")] = TargetBone;
		j[LIT("FovOverride")] = FovOverride;
		j[LIT("SmoothnessOverride")] = SmoothnessOverride;
		j[LIT("SmoothAlgorithmOverride")] = SmoothAlgorithmOverride;
		j[LIT("TriggerKeyOverride")] = TriggerKeyOverride;
		j[LIT("TriggerEnabled")] = TriggerEnabled;
		j[LIT("TriggerMinDelayMs")] = TriggerMinDelayMs;
		j[LIT("TriggerMaxDelayMs")] = TriggerMaxDelayMs;
		j[LIT("TriggerHitboxScale")] = TriggerHitboxScale;
		j[LIT("TriggerMaxDistance")] = TriggerMaxDistance;
		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.is_object()) return;
		Enabled = j.value(LIT("Enabled"), Enabled);
		TargetBone = j.value(LIT("TargetBone"), TargetBone);
		FovOverride = j.value(LIT("FovOverride"), FovOverride);
		SmoothnessOverride = j.value(LIT("SmoothnessOverride"), SmoothnessOverride);
		SmoothAlgorithmOverride = j.value(LIT("SmoothAlgorithmOverride"), SmoothAlgorithmOverride);
		TriggerKeyOverride = j.value(LIT("TriggerKeyOverride"), TriggerKeyOverride);
		TriggerEnabled = j.value(LIT("TriggerEnabled"), TriggerEnabled);
		TriggerMinDelayMs = j.value(LIT("TriggerMinDelayMs"), TriggerMinDelayMs);
		TriggerMaxDelayMs = j.value(LIT("TriggerMaxDelayMs"), TriggerMaxDelayMs);
		TriggerHitboxScale = j.value(LIT("TriggerHitboxScale"), TriggerHitboxScale);
		TriggerMaxDistance = j.value(LIT("TriggerMaxDistance"), TriggerMaxDistance);
	}
};

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
	static constexpr int kWeaponProfileCount = 5; // 0=Rifle, 1=SMG, 2=Shotgun, 3=Pistol, 4=DMR/Sniper

	AimConfig(const std::string& name)
	{
		ConfigName = name;
	}

	bool Enabled = false;
	int AimType = 0;        // 0 = Memory Aim, 1 = Hardware Aim
	int TargetBone = 0;     // 0 = Head, 1 = Neck, 2 = Chest, 3 = Pelvis, 4 = Closest
	int TargetPriority = 0; // 0 = Crosshair FOV, 1 = 3D Distance, 2 = Lowest Health
	bool DynamicBoneFallback = false; // drop to chest if head is occluded or low health
	int Smoothness = 6;     // 1 (instant) .. 100 (very slow)
	// Multi-curve smoothing algorithm: 0 = EMA, 1 = Linear, 2 = Bezier
	int SmoothAlgorithm = 0;
	float BezierBend = 0.35f;  // 0.0 .. 1.0 lateral curve factor for Bezier
	float BezierSpeed = 1.0f;  // 0.25 .. 3.0 travel speed multiplier for Linear / Bezier
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
	bool ShowDebug = false;
	// Only consider characters whose animator is still evaluating (the same
	// per-character state the ESP colours as "visible"). This is an
	// animator/cull check, not wall occlusion: read-only DMA cannot tell
	// whether a bone is behind geometry.
	bool VisibleBonesOnly = false;

	// --- Controller & Input Flexibility --------------------------------
	bool ControllerEnabled = true;           // poll hardware controller + host XInput gamepad
	int ControllerSlot = 0;                  // 0 = Auto (0..3), 1..4 = specific XInput slot
	bool ControllerSecondaryEnable = false;  // allow secondary gamepad activation alongside AimKey
	int ControllerAimKey = 19;               // default Gamepad LT
	float ControllerTriggerThreshold = 0.25f;// 0.05 .. 0.95 normalized LT/RT threshold
	float ControllerStickDeadzone = 0.15f;   // 0.00 .. 0.80 stick deflection threshold
	bool ControllerRequireStick = false;     // gate gamepad activation on right-stick deflection
	float ControllerStickScale = 120.0f;     // 10.0 .. 500.0 stick output deflection scale

	// --- Precision Auto-Fire (Trigger) ---------------------------------
	bool TriggerEnabled = false;
	int TriggerKey = 2;                      // index into Aim::KeyNames() (default Left Alt)
	int TriggerBone = 4;                     // 0 = Head, 1 = Neck, 2 = Chest, 3 = Pelvis, 4 = Any/Closest
	int TriggerMinDelayMs = 20;              // 0 .. 500 ms minimum reaction delay
	int TriggerMaxDelayMs = 60;              // 0 .. 500 ms maximum reaction delay
	int TriggerBurstMs = 35;                 // 10 .. 250 ms fire hold duration
	int TriggerCooldownMs = 60;              // 0 .. 500 ms pause between pulses
	float TriggerHitboxScale = 1.0f;         // 0.25 .. 2.0 screen hitbox radius scale
	int TriggerMaxDistance = 200;            // 5 .. 500 metres
	bool TriggerVisibleOnly = true;          // respect visibility & BVH occlusion

	// --- Per-Weapon Profiles -------------------------------------------
	bool WeaponProfilesEnabled = false;
	std::array<WeaponProfile, kWeaponProfileCount> WeaponProfiles{};

	// --- Memory Aim write path -----------------------------------------
	// Write the camera angles through DMA instead of moving the local cursor.
	bool MemoryWrite = false;
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
	int TargetSwitchDelay = 200;  // ms before allowing a new target (0 = instant)

	// --- Recoil / spread suppression ----------------------------------
	bool NoRecoil = false;
	bool NoSpread = false;
	bool DynamicRecoilCompensation = false;
	float RecoilStrength = 1.0f;

	// Role filters - all set to false to always allow all targets
	bool ExpIgnorePolice = false;
	bool ExpIgnoreCriminal = false;
	bool ExpIgnoreCivilian = false;
	bool ExpIgnoreTeammates = true;
	bool SkipKnocked = true;
	bool IgnoreKnock = false;
	bool HideCrosshair = false;

	// Manually trusted players (by publicId / account id). Aimbot skips these
	// the same way it skips party members when ExpIgnoreTeammates is on.
	// Accessed concurrently by the UI thread and the 500 Hz targeting worker;
	// always go through the synchronized helpers below (or lock TrustedMutex()).
	std::set<std::uint64_t> TrustedPlayers;

	static std::mutex& TrustedMutex()
	{
		static std::mutex m;
		return m;
	}

	bool IsTrusted(std::uint64_t publicId) const
	{
		if (!publicId) return false;
		std::lock_guard<std::mutex> lock(TrustedMutex());
		return TrustedPlayers.count(publicId) > 0;
	}

	void SetTrusted(std::uint64_t publicId, bool trusted)
	{
		if (!publicId) return;
		std::lock_guard<std::mutex> lock(TrustedMutex());
		if (trusted)
			TrustedPlayers.insert(publicId);
		else
			TrustedPlayers.erase(publicId);
	}

	int TrustedCount() const
	{
		std::lock_guard<std::mutex> lock(TrustedMutex());
		return static_cast<int>(TrustedPlayers.size());
	}

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
		j[ConfigName][LIT("TargetPriority")] = TargetPriority;
		j[ConfigName][LIT("DynamicBoneFallback")] = DynamicBoneFallback;
		j[ConfigName][LIT("Smoothness")] = Smoothness;
		j[ConfigName][LIT("SmoothAlgorithm")] = SmoothAlgorithm;
		j[ConfigName][LIT("BezierBend")] = BezierBend;
		j[ConfigName][LIT("BezierSpeed")] = BezierSpeed;
		j[ConfigName][LIT("HardwareGain")] = HardwareGain;
		j[ConfigName][LIT("Fov")] = Fov;
		j[ConfigName][LIT("AimKey")] = AimKey;
		j[ConfigName][LIT("MaxDistance")] = MaxDistance;
		j[ConfigName][LIT("ShowFov")] = ShowFov;
		j[ConfigName][LIT("ShowDebug")] = ShowDebug;
		j[ConfigName][LIT("VisibleBonesOnly")] = VisibleBonesOnly;
		j[ConfigName][LIT("ControllerEnabled")] = ControllerEnabled;
		j[ConfigName][LIT("ControllerSlot")] = ControllerSlot;
		j[ConfigName][LIT("ControllerSecondaryEnable")] = ControllerSecondaryEnable;
		j[ConfigName][LIT("ControllerAimKey")] = ControllerAimKey;
		j[ConfigName][LIT("ControllerTriggerThreshold")] = ControllerTriggerThreshold;
		j[ConfigName][LIT("ControllerStickDeadzone")] = ControllerStickDeadzone;
		j[ConfigName][LIT("ControllerRequireStick")] = ControllerRequireStick;
		j[ConfigName][LIT("ControllerStickScale")] = ControllerStickScale;
		j[ConfigName][LIT("TriggerEnabled")] = TriggerEnabled;
		j[ConfigName][LIT("TriggerKey")] = TriggerKey;
		j[ConfigName][LIT("TriggerBone")] = TriggerBone;
		j[ConfigName][LIT("TriggerMinDelayMs")] = TriggerMinDelayMs;
		j[ConfigName][LIT("TriggerMaxDelayMs")] = TriggerMaxDelayMs;
		j[ConfigName][LIT("TriggerBurstMs")] = TriggerBurstMs;
		j[ConfigName][LIT("TriggerCooldownMs")] = TriggerCooldownMs;
		j[ConfigName][LIT("TriggerHitboxScale")] = TriggerHitboxScale;
		j[ConfigName][LIT("TriggerMaxDistance")] = TriggerMaxDistance;
		j[ConfigName][LIT("TriggerVisibleOnly")] = TriggerVisibleOnly;
		j[ConfigName][LIT("WeaponProfilesEnabled")] = WeaponProfilesEnabled;
		{
			json profArr = json::array();
			for (int i = 0; i < kWeaponProfileCount; ++i)
				profArr.push_back(WeaponProfiles[i].ToJson());
			j[ConfigName][LIT("WeaponProfiles")] = profArr;
		}
		j[ConfigName][LIT("MemoryWrite")] = MemoryWrite;
		j[ConfigName][LIT("CursorFallback")] = CursorFallback;
		j[ConfigName][LIT("ResponseMs")] = ResponseMs;
		j[ConfigName][LIT("Prediction")] = Prediction;
		j[ConfigName][LIT("PredictionStrength")] = PredictionStrength;
		j[ConfigName][LIT("PredictionYScale")] = PredictionYScale;
		j[ConfigName][LIT("HardLock")] = HardLock;
		j[ConfigName][LIT("TargetSwitchDelay")] = TargetSwitchDelay;
		j[ConfigName][LIT("NoRecoil")] = NoRecoil;
		j[ConfigName][LIT("NoSpread")] = NoSpread;
		j[ConfigName][LIT("DynamicRecoilCompensation")] = DynamicRecoilCompensation;
		j[ConfigName][LIT("RecoilStrength")] = RecoilStrength;
		j[ConfigName][LIT("ExpIgnorePolice")] = ExpIgnorePolice;
		j[ConfigName][LIT("ExpIgnoreCriminal")] = ExpIgnoreCriminal;
		j[ConfigName][LIT("ExpIgnoreCivilian")] = ExpIgnoreCivilian;
		j[ConfigName][LIT("ExpIgnoreTeammates")] = ExpIgnoreTeammates;
		j[ConfigName][LIT("SkipKnocked")] = SkipKnocked;
		j[ConfigName][LIT("IgnoreKnock")] = IgnoreKnock;
		j[ConfigName][LIT("HideCrosshair")] = HideCrosshair;
		{
			std::lock_guard<std::mutex> lock(TrustedMutex());
			json arr = json::array();
			for (auto id : TrustedPlayers) arr.push_back(std::to_string(id));
			j[ConfigName][LIT("TrustedPlayers")] = arr;
		}
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
		TargetPriority = j[ConfigName].value(LIT("TargetPriority"), TargetPriority);
		DynamicBoneFallback = j[ConfigName].value(LIT("DynamicBoneFallback"), DynamicBoneFallback);
		Smoothness = j[ConfigName].value(LIT("Smoothness"), Smoothness);
		SmoothAlgorithm = j[ConfigName].value(LIT("SmoothAlgorithm"), SmoothAlgorithm);
		BezierBend = j[ConfigName].value(LIT("BezierBend"), BezierBend);
		BezierSpeed = j[ConfigName].value(LIT("BezierSpeed"), BezierSpeed);
		HardwareGain = j[ConfigName].value(LIT("HardwareGain"), HardwareGain);
		Fov = j[ConfigName].value(LIT("Fov"), Fov);
		AimKey = j[ConfigName].value(LIT("AimKey"), AimKey);
		MaxDistance = j[ConfigName].value(LIT("MaxDistance"), MaxDistance);
		ShowFov = j[ConfigName].value(LIT("ShowFov"), ShowFov);
		ShowDebug = j[ConfigName].value(LIT("ShowDebug"), ShowDebug);
		VisibleBonesOnly = j[ConfigName].value(LIT("VisibleBonesOnly"), VisibleBonesOnly);
		ControllerEnabled = j[ConfigName].value(LIT("ControllerEnabled"), ControllerEnabled);
		ControllerSlot = j[ConfigName].value(LIT("ControllerSlot"), ControllerSlot);
		ControllerSecondaryEnable = j[ConfigName].value(LIT("ControllerSecondaryEnable"), ControllerSecondaryEnable);
		ControllerAimKey = j[ConfigName].value(LIT("ControllerAimKey"), ControllerAimKey);
		ControllerTriggerThreshold = j[ConfigName].value(LIT("ControllerTriggerThreshold"), ControllerTriggerThreshold);
		ControllerStickDeadzone = j[ConfigName].value(LIT("ControllerStickDeadzone"), ControllerStickDeadzone);
		ControllerRequireStick = j[ConfigName].value(LIT("ControllerRequireStick"), ControllerRequireStick);
		ControllerStickScale = j[ConfigName].value(LIT("ControllerStickScale"), ControllerStickScale);
		TriggerEnabled = j[ConfigName].value(LIT("TriggerEnabled"), TriggerEnabled);
		TriggerKey = j[ConfigName].value(LIT("TriggerKey"), TriggerKey);
		TriggerBone = j[ConfigName].value(LIT("TriggerBone"), TriggerBone);
		TriggerMinDelayMs = j[ConfigName].value(LIT("TriggerMinDelayMs"), TriggerMinDelayMs);
		TriggerMaxDelayMs = j[ConfigName].value(LIT("TriggerMaxDelayMs"), TriggerMaxDelayMs);
		TriggerBurstMs = j[ConfigName].value(LIT("TriggerBurstMs"), TriggerBurstMs);
		TriggerCooldownMs = j[ConfigName].value(LIT("TriggerCooldownMs"), TriggerCooldownMs);
		TriggerHitboxScale = j[ConfigName].value(LIT("TriggerHitboxScale"), TriggerHitboxScale);
		TriggerMaxDistance = j[ConfigName].value(LIT("TriggerMaxDistance"), TriggerMaxDistance);
		TriggerVisibleOnly = j[ConfigName].value(LIT("TriggerVisibleOnly"), TriggerVisibleOnly);
		WeaponProfilesEnabled = j[ConfigName].value(LIT("WeaponProfilesEnabled"), WeaponProfilesEnabled);
		if (j[ConfigName].contains(LIT("WeaponProfiles")) &&
			j[ConfigName][LIT("WeaponProfiles")].is_array()) {
			const auto& arr = j[ConfigName][LIT("WeaponProfiles")];
			const std::size_t n = (std::min)(arr.size(), static_cast<std::size_t>(kWeaponProfileCount));
			for (std::size_t i = 0; i < n; ++i)
				WeaponProfiles[i].FromJson(arr[i]);
		}
		MemoryWrite = j[ConfigName].value(LIT("MemoryWrite"), MemoryWrite);
		CursorFallback = j[ConfigName].value(LIT("CursorFallback"), CursorFallback);
		ResponseMs = j[ConfigName].value(LIT("ResponseMs"), ResponseMs);
		Prediction = j[ConfigName].value(LIT("Prediction"), Prediction);
		PredictionStrength = j[ConfigName].value(LIT("PredictionStrength"), PredictionStrength);
		PredictionYScale = j[ConfigName].value(LIT("PredictionYScale"), PredictionYScale);
		HardLock = j[ConfigName].value(LIT("HardLock"), HardLock);
		TargetSwitchDelay = j[ConfigName].value(LIT("TargetSwitchDelay"), TargetSwitchDelay);
		NoRecoil = j[ConfigName].value(LIT("NoRecoil"), NoRecoil);
		NoSpread = j[ConfigName].value(LIT("NoSpread"), NoSpread);
		DynamicRecoilCompensation = j[ConfigName].value(LIT("DynamicRecoilCompensation"), DynamicRecoilCompensation);
		RecoilStrength = j[ConfigName].value(LIT("RecoilStrength"), RecoilStrength);
		ExpIgnorePolice = j[ConfigName].value(LIT("ExpIgnorePolice"), ExpIgnorePolice);
		ExpIgnoreCriminal = j[ConfigName].value(LIT("ExpIgnoreCriminal"), ExpIgnoreCriminal);
		ExpIgnoreCivilian = j[ConfigName].value(LIT("ExpIgnoreCivilian"), ExpIgnoreCivilian);
		ExpIgnoreTeammates = j[ConfigName].value(LIT("ExpIgnoreTeammates"), ExpIgnoreTeammates);
		SkipKnocked = j[ConfigName].value(LIT("SkipKnocked"), SkipKnocked);
		IgnoreKnock = j[ConfigName].value(LIT("IgnoreKnock"), IgnoreKnock);
		HideCrosshair = j[ConfigName].value(LIT("HideCrosshair"), HideCrosshair);
		if (j[ConfigName].contains(LIT("TrustedPlayers")) &&
			j[ConfigName][LIT("TrustedPlayers")].is_array()) {
			std::set<std::uint64_t> parsed;
			for (const auto& v : j[ConfigName][LIT("TrustedPlayers")]) {
				try {
					if (v.is_string())
						parsed.insert(std::stoull(v.get<std::string>()));
					else if (v.is_number_unsigned())
						parsed.insert(v.get<std::uint64_t>());
				} catch (...) {}
			}
			std::lock_guard<std::mutex> lock(TrustedMutex());
			TrustedPlayers = std::move(parsed);
		}
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
