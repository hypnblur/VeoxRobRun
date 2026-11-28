#pragma once
#include "pch.h"
#include "../Input/Hotkey.h"
#include "SkeletonExtractor.h"
#include <algorithm>
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
	int SmoothAlgorithmOverride = -1; // -1 = Inherit, 0 = EMA, 1 = Linear, 2 = Bezier, 3 = ABCurves
	int TriggerBindingOverride = Hotkey::Inherit;
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
		j[LIT("TriggerBindingOverride")] = TriggerBindingOverride;
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
		TriggerBindingOverride = Hotkey::Read(j, LIT("TriggerBindingOverride"), LIT("TriggerKeyOverride"),
			Hotkey::LegacyAim, Hotkey::Inherit, Hotkey::AimInputs | Hotkey::AllowAlways | Hotkey::AllowInherit);
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
	int TargetBone = 0;     // 0 = Head, 1 = Neck, 2 = Chest, 3 = Pelvis, 4 = Closest, 5 = Head & Body (Random)
	bool NearestBone = false;
	std::uint32_t NearestBoneMask = Skeleton::kDefaultAimBones;
	int BoneSwitchDelay = 100; // ms the next bone must stay closest
	int TargetPriority = 0; // 0 = Crosshair FOV, 1 = 3D Distance, 2 = Lowest Health
	bool DynamicBoneFallback = false; // drop to chest if head is occluded or low health
	int Smoothness = 6;     // 1 (instant) .. 100 (very slow)
	// Multi-curve smoothing algorithm: 0 = EMA, 1 = Linear, 2 = Bezier, 3 = ABCurves
	int SmoothAlgorithm = 0;
	float BezierBend = 0.35f;  // 0.0 .. 1.0 lateral curve factor for Bezier
	float BezierSpeed = 1.0f;  // 0.25 .. 3.0 travel speed multiplier for Linear / Bezier / ABCurves
	// ABCurves options:
	// - ABCurvesRenderer: 1 kHz fixed-point GRU packet renderer (works with any curve on Hardware/Cursor)
	// - ABCurvesScale: sensitivity multiplier into ABCurves common angular count space (0.25 .. 4.0)
	// - ABCurvesSeed: 0 = random seed per target, >0 = fixed seed
	bool ABCurvesRenderer = false;
	float ABCurvesScale = 1.0f;
	int ABCurvesSeed = 0;
	// Hardware Aim only: multiplies the pixel error before it is sent to the
	// device, so a low in-game sensitivity (where one mouse count travels less
	// than one screen pixel) can be compensated without changing Smoothness.
	float HardwareGain = 1.0f; // 0.25 .. 5.0
	int Fov = 40;           // 5 .. 180 degrees
	bool DynamicFov = false;
	int MinDynamicFov = 5;
	int AimBinding = VK_RBUTTON;
	int SecondaryAimBinding = Hotkey::Off;
	int HeadOverrideBinding = VK_LSHIFT;
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
	bool ControllerSecondaryEnable = false;  // allow controller activation alongside the primary binding
	int ControllerAimBinding = CTL_VK_LT;
	float ControllerTriggerThreshold = 0.25f;// 0.05 .. 0.95 normalized LT/RT threshold
	float ControllerStickDeadzone = 0.15f;   // 0.00 .. 0.80 stick deflection threshold
	bool ControllerRequireStick = false;     // gate gamepad activation on right-stick deflection
	float ControllerStickScale = 120.0f;     // 10.0 .. 500.0 stick output deflection scale

	// --- Precision Auto-Fire (Trigger) ---------------------------------
	bool TriggerEnabled = false;
	int TriggerBinding = VK_LMENU;
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
	bool Prediction = false;      // Adaptive target prediction
	bool HardLock = false;        // keep the current target outside the FOV
	bool KeepLockAfterVehicleExit = true;
	int TargetSwitchDelay = 200;  // ms before allowing a new target (0 = instant)

	// --- Natural Aim Assist / Humanization -----------------------------
	float DeadzonePx = 0.0f;       // 0..25 px: stop pulling once crosshair is already on target
	float HumanizeOffsetCm = 0.0f; // 0..10 cm: slow wandering offset around the bone
	bool SoftEntry = false;        // gentle ramp-in + FOV edge falloff on target entry

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
		j[ConfigName][LIT("NearestBone")] = NearestBone;
		j[ConfigName][LIT("NearestBoneMask")] = NearestBoneMask;
		j[ConfigName][LIT("BoneSwitchDelay")] = BoneSwitchDelay;
		j[ConfigName][LIT("TargetPriority")] = TargetPriority;
		j[ConfigName][LIT("DynamicBoneFallback")] = DynamicBoneFallback;
		j[ConfigName][LIT("Smoothness")] = Smoothness;
		j[ConfigName][LIT("SmoothAlgorithm")] = SmoothAlgorithm;
		j[ConfigName][LIT("BezierBend")] = BezierBend;
		j[ConfigName][LIT("BezierSpeed")] = BezierSpeed;
		j[ConfigName][LIT("ABCurvesRenderer")] = ABCurvesRenderer;
		j[ConfigName][LIT("ABCurvesScale")] = ABCurvesScale;
		j[ConfigName][LIT("ABCurvesSeed")] = ABCurvesSeed;
		j[ConfigName][LIT("HardwareGain")] = HardwareGain;
		j[ConfigName][LIT("Fov")] = Fov;
		j[ConfigName][LIT("DynamicFov")] = DynamicFov;
		j[ConfigName][LIT("MinDynamicFov")] = MinDynamicFov;
		j[ConfigName][LIT("AimBinding")] = AimBinding;
		j[ConfigName][LIT("SecondaryAimBinding")] = SecondaryAimBinding;
		j[ConfigName][LIT("HeadOverrideBinding")] = HeadOverrideBinding;
		j[ConfigName][LIT("MaxDistance")] = MaxDistance;
		j[ConfigName][LIT("ShowFov")] = ShowFov;
		j[ConfigName][LIT("ShowDebug")] = ShowDebug;
		j[ConfigName][LIT("VisibleBonesOnly")] = VisibleBonesOnly;
		j[ConfigName][LIT("ControllerEnabled")] = ControllerEnabled;
		j[ConfigName][LIT("ControllerSlot")] = ControllerSlot;
		j[ConfigName][LIT("ControllerSecondaryEnable")] = ControllerSecondaryEnable;
		j[ConfigName][LIT("ControllerAimBinding")] = ControllerAimBinding;
		j[ConfigName][LIT("ControllerTriggerThreshold")] = ControllerTriggerThreshold;
		j[ConfigName][LIT("ControllerStickDeadzone")] = ControllerStickDeadzone;
		j[ConfigName][LIT("ControllerRequireStick")] = ControllerRequireStick;
		j[ConfigName][LIT("ControllerStickScale")] = ControllerStickScale;
		j[ConfigName][LIT("TriggerEnabled")] = TriggerEnabled;
		j[ConfigName][LIT("TriggerBinding")] = TriggerBinding;
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
		j[ConfigName][LIT("HardLock")] = HardLock;
		j[ConfigName][LIT("KeepLockAfterVehicleExit")] = KeepLockAfterVehicleExit;
		j[ConfigName][LIT("TargetSwitchDelay")] = TargetSwitchDelay;
		j[ConfigName][LIT("DeadzonePx")] = DeadzonePx;
		j[ConfigName][LIT("HumanizeOffsetCm")] = HumanizeOffsetCm;
		j[ConfigName][LIT("SoftEntry")] = SoftEntry;
		j[ConfigName][LIT("NoRecoil")] = NoRecoil;
		j[ConfigName][LIT("NoSpread")] = NoSpread;
		j[ConfigName][LIT("DynamicRecoilCompensation")] = DynamicRecoilCompensation;
		j[ConfigName][LIT("RecoilStrength")] = RecoilStrength;
		j[ConfigName][LIT("ExpIgnorePolice")] = ExpIgnorePolice;
		j[ConfigName][LIT("ExpIgnoreCriminal")] = ExpIgnoreCriminal;
		j[ConfigName][LIT("ExpIgnoreCivilian")] = ExpIgnoreCivilian;
		j[ConfigName][LIT("ExpIgnoreTeammates")] = ExpIgnoreTeammates;
		j[ConfigName][LIT("SkipKnocked")] = SkipKnocked;
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
		NearestBone = false;
		NearestBoneMask = Skeleton::kDefaultAimBones;
		BoneSwitchDelay = 100;
		DynamicFov = false;
		MinDynamicFov = 5;
		HeadOverrideBinding = VK_LSHIFT;
		KeepLockAfterVehicleExit = true;
		if (!j.contains(ConfigName))
			return;
		Enabled = j[ConfigName].value(LIT("Enabled"), Enabled);
		AimType = j[ConfigName].value(LIT("AimType"), AimType);
		TargetBone = j[ConfigName].value(LIT("TargetBone"), TargetBone);
		NearestBone = j[ConfigName].value(LIT("NearestBone"), NearestBone);
		const auto bones = j[ConfigName].value(LIT("NearestBoneMask"), NearestBoneMask) & Skeleton::kAllAimBones;
		if (bones != 0) NearestBoneMask = bones;
		BoneSwitchDelay = std::clamp(j[ConfigName].value(LIT("BoneSwitchDelay"), BoneSwitchDelay), 0, 1000);
		TargetPriority = j[ConfigName].value(LIT("TargetPriority"), TargetPriority);
		DynamicBoneFallback = j[ConfigName].value(LIT("DynamicBoneFallback"), DynamicBoneFallback);
		Smoothness = j[ConfigName].value(LIT("Smoothness"), Smoothness);
		SmoothAlgorithm = j[ConfigName].value(LIT("SmoothAlgorithm"), SmoothAlgorithm);
		BezierBend = j[ConfigName].value(LIT("BezierBend"), BezierBend);
		BezierSpeed = j[ConfigName].value(LIT("BezierSpeed"), BezierSpeed);
		ABCurvesRenderer = j[ConfigName].value(LIT("ABCurvesRenderer"), ABCurvesRenderer);
		ABCurvesScale = j[ConfigName].value(LIT("ABCurvesScale"), ABCurvesScale);
		ABCurvesSeed = j[ConfigName].value(LIT("ABCurvesSeed"), ABCurvesSeed);
		HardwareGain = j[ConfigName].value(LIT("HardwareGain"), HardwareGain);
		Fov = j[ConfigName].value(LIT("Fov"), Fov);
		DynamicFov = j[ConfigName].value(LIT("DynamicFov"), DynamicFov);
		MinDynamicFov = std::clamp(j[ConfigName].value(LIT("MinDynamicFov"), 5), 1, 180);
		const auto& aim = j[ConfigName];
		AimBinding = Hotkey::Read(aim, LIT("AimBinding"), LIT("AimKey"),
			Hotkey::LegacyAim, VK_RBUTTON, Hotkey::AimInputs | Hotkey::AllowAlways);
		SecondaryAimBinding = Hotkey::Read(aim, LIT("SecondaryAimBinding"), LIT("SecondaryAimKey"),
			Hotkey::LegacyAim, Hotkey::Off, Hotkey::AimInputs | Hotkey::AllowAlways | Hotkey::AllowOff);
		HeadOverrideBinding = Hotkey::Read(aim, LIT("HeadOverrideBinding"), LIT("HeadOverrideKey"),
			Hotkey::LegacyAim, VK_LSHIFT, Hotkey::AimInputs | Hotkey::AllowOff);
		MaxDistance = j[ConfigName].value(LIT("MaxDistance"), MaxDistance);
		ShowFov = j[ConfigName].value(LIT("ShowFov"), ShowFov);
		ShowDebug = j[ConfigName].value(LIT("ShowDebug"), ShowDebug);
		VisibleBonesOnly = j[ConfigName].value(LIT("VisibleBonesOnly"), VisibleBonesOnly);
		ControllerEnabled = j[ConfigName].value(LIT("ControllerEnabled"), ControllerEnabled);
		ControllerSlot = j[ConfigName].value(LIT("ControllerSlot"), ControllerSlot);
		ControllerSecondaryEnable = j[ConfigName].value(LIT("ControllerSecondaryEnable"), ControllerSecondaryEnable);
		ControllerAimBinding = Hotkey::Read(aim, LIT("ControllerAimBinding"), LIT("ControllerAimKey"),
			Hotkey::LegacyAim, CTL_VK_LT, Hotkey::AimInputs | Hotkey::AllowAlways);
		ControllerTriggerThreshold = j[ConfigName].value(LIT("ControllerTriggerThreshold"), ControllerTriggerThreshold);
		ControllerStickDeadzone = j[ConfigName].value(LIT("ControllerStickDeadzone"), ControllerStickDeadzone);
		ControllerRequireStick = j[ConfigName].value(LIT("ControllerRequireStick"), ControllerRequireStick);
		ControllerStickScale = j[ConfigName].value(LIT("ControllerStickScale"), ControllerStickScale);
		TriggerEnabled = j[ConfigName].value(LIT("TriggerEnabled"), TriggerEnabled);
		TriggerBinding = Hotkey::Read(aim, LIT("TriggerBinding"), LIT("TriggerKey"),
			Hotkey::LegacyAim, VK_LMENU, Hotkey::AimInputs | Hotkey::AllowAlways);
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
		HardLock = j[ConfigName].value(LIT("HardLock"), HardLock);
		KeepLockAfterVehicleExit = j[ConfigName].value(LIT("KeepLockAfterVehicleExit"), KeepLockAfterVehicleExit);
		TargetSwitchDelay = j[ConfigName].value(LIT("TargetSwitchDelay"), TargetSwitchDelay);
		DeadzonePx = j[ConfigName].value(LIT("DeadzonePx"), DeadzonePx);
		HumanizeOffsetCm = j[ConfigName].value(LIT("HumanizeOffsetCm"), HumanizeOffsetCm);
		SoftEntry = j[ConfigName].value(LIT("SoftEntry"), SoftEntry);
		NoRecoil = j[ConfigName].value(LIT("NoRecoil"), NoRecoil);
		NoSpread = j[ConfigName].value(LIT("NoSpread"), NoSpread);
		DynamicRecoilCompensation = j[ConfigName].value(LIT("DynamicRecoilCompensation"), DynamicRecoilCompensation);
		RecoilStrength = j[ConfigName].value(LIT("RecoilStrength"), RecoilStrength);
		ExpIgnorePolice = j[ConfigName].value(LIT("ExpIgnorePolice"), ExpIgnorePolice);
		ExpIgnoreCriminal = j[ConfigName].value(LIT("ExpIgnoreCriminal"), ExpIgnoreCriminal);
		ExpIgnoreCivilian = j[ConfigName].value(LIT("ExpIgnoreCivilian"), ExpIgnoreCivilian);
		ExpIgnoreTeammates = j[ConfigName].value(LIT("ExpIgnoreTeammates"), ExpIgnoreTeammates);
		SkipKnocked = j[ConfigName].value(LIT("SkipKnocked"), SkipKnocked);
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
