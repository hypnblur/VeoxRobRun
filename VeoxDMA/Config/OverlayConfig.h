#pragma once
#include "pch.h"

// Last on-screen position (and size, when the window is resizable) of one of
// the floating windows. Coordinates are in overlay/monitor space, which is the
// space ImGui uses for windows on the pinned display.
struct WindowPlacement
{
	bool Saved = false;
	float X = 0.f;
	float Y = 0.f;
	float Width = 0.f;
	float Height = 0.f;

	json ToJson() const
	{
		json j;
		j[LIT("Saved")] = Saved;
		j[LIT("X")] = X;
		j[LIT("Y")] = Y;
		j[LIT("Width")] = Width;
		j[LIT("Height")] = Height;
		return j;
	}

	void FromJson(const json& j)
	{
		if (!j.is_object())
			return;
		if (j.contains(LIT("Saved"))) Saved = j[LIT("Saved")];
		if (j.contains(LIT("X"))) X = j[LIT("X")];
		if (j.contains(LIT("Y"))) Y = j[LIT("Y")];
		if (j.contains(LIT("Width"))) Width = j[LIT("Width")];
		if (j.contains(LIT("Height"))) Height = j[LIT("Height")];
	}
};

// Window/render settings for the external overlay. Mirrors PureDMADBD's
// OverlayConfig so the same monitor/transparency/frame-cap behaviour ports
// across without dragging the DBD-specific fields along.
class OverlayConfig
{
	std::string ConfigName;

public:
	OverlayConfig(const std::string& name)
	{
		ConfigName = name;
	}

	int MonitorIndex = 0;            // display the overlay is pinned to
	bool FPSEnabled = false;         // small FPS readout in the top-left
	bool StatusPanel = true;         // feature status dots in the top-left
	int FrameCap = 240;              // default 240 fps, 0 = uncapped
	std::string FontName = "Maven Pro";
	bool Transparent = true;         // per-pixel alpha over the game
	bool HighContrastMenu = false;   // solid menu fills for bright scenes
	bool ClickThroughWhenClosed = true;
	bool AlwaysOnTop = true;
	bool BackdropVisible = false;
	bool BackdropFullscreen = true;
	bool BackdropCustomRes = false;
	int BackdropWidth = 1920;
	int BackdropHeight = 1080;
	// When enabled, only player ESP is drawn (world ESP, radar and the money
	// HUD are suppressed). Driven by BattleModeKey (Hold or Toggle).
	bool BattleMode = false;
	int BattleModeKey = 0;           // index into Aim::KeyNames()
	int BattleModeBehavior = 0;      // 0 = Hold, 1 = Toggle
	int MenuKey = 0;                 // 0=Insert, 1=Home, 2=Delete, 3=End, 4=F11, 5=F12, 6=Right Shift
	bool PanicMode = false;          // true = immediately blank overlay & stop targeting
	int PanicKey = 0;                // 0=None, 1=End, 2=F8, 3=F9, 4=F10, 5=F11, 6=F12
	int ReattachKey = 0;             // 0=Off, 1=F1, 2=F2...
	bool CustomCameraDistance = false; // extended third-person camera
	float CameraDistance = 3.5f;     // follow distance in meters (1.0 - 15.0)
	bool FovChanger = false;         // override Cinemachine virtual camera FOV
	float CameraFov = 90.0f;         // target FOV in degrees (20.0 - 160.0)
	D2D1::ColorF BackgroundColour = Colour(0, 0, 0);
	D2D1::ColorF AccentColour = Colour(139, 92, 246);

	// Saved screen placements. The menu and radar remember size as well as
	// position; the looking-alert window auto-sizes to its text.
	WindowPlacement MenuWindow;
	WindowPlacement RadarWindow;
	WindowPlacement AlertWindow;

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
		j[ConfigName][LIT("MonitorIndex")] = MonitorIndex;
		j[ConfigName][LIT("FPSEnabled")] = FPSEnabled;
		j[ConfigName][LIT("StatusPanel")] = StatusPanel;
		j[ConfigName][LIT("FrameCap")] = FrameCap;
		j[ConfigName][LIT("Transparent")] = Transparent;
		j[ConfigName][LIT("HighContrastMenu")] = HighContrastMenu;
		j[ConfigName][LIT("ClickThroughWhenClosed")] = ClickThroughWhenClosed;
		j[ConfigName][LIT("AlwaysOnTop")] = AlwaysOnTop;
		j[ConfigName][LIT("BackdropVisible")] = BackdropVisible;
		j[ConfigName][LIT("BackdropFullscreen")] = BackdropFullscreen;
		j[ConfigName][LIT("BackdropCustomRes")] = BackdropCustomRes;
		j[ConfigName][LIT("BackdropWidth")] = BackdropWidth;
		j[ConfigName][LIT("BackdropHeight")] = BackdropHeight;
		j[ConfigName][LIT("BattleMode")] = BattleMode;
		j[ConfigName][LIT("BattleModeKey")] = BattleModeKey;
		j[ConfigName][LIT("BattleModeBehavior")] = BattleModeBehavior;
		j[ConfigName][LIT("MenuKey")] = MenuKey;
		j[ConfigName][LIT("PanicMode")] = PanicMode;
		j[ConfigName][LIT("PanicKey")] = PanicKey;
		j[ConfigName][LIT("ReattachKey")] = ReattachKey;
		j[ConfigName][LIT("CustomCameraDistance")] = CustomCameraDistance;
		j[ConfigName][LIT("CameraDistance")] = CameraDistance;
		j[ConfigName][LIT("FovChanger")] = FovChanger;
		j[ConfigName][LIT("CameraFov")] = CameraFov;
		j[ConfigName][LIT("FontName")] = FontName;
		j[ConfigName][LIT("MenuWindow")] = MenuWindow.ToJson();
		j[ConfigName][LIT("RadarWindow")] = RadarWindow.ToJson();
		j[ConfigName][LIT("AlertWindow")] = AlertWindow.ToJson();
		ToJsonColour(&j, LIT("BackgroundColour"), &BackgroundColour);
		ToJsonColour(&j, LIT("AccentColour"), &AccentColour);
		return j;
	}
	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		auto& s = j[ConfigName];
		MonitorIndex = s.value(LIT("MonitorIndex"), MonitorIndex);
		FPSEnabled = s.value(LIT("FPSEnabled"), FPSEnabled);
		StatusPanel = s.value(LIT("StatusPanel"), StatusPanel);
		FrameCap = s.value(LIT("FrameCap"), FrameCap);
		Transparent = s.value(LIT("Transparent"), Transparent);
		HighContrastMenu = s.value(LIT("HighContrastMenu"), HighContrastMenu);
		ClickThroughWhenClosed = s.value(LIT("ClickThroughWhenClosed"), ClickThroughWhenClosed);
		AlwaysOnTop = s.value(LIT("AlwaysOnTop"), AlwaysOnTop);
		BackdropVisible = s.value(LIT("BackdropVisible"), BackdropVisible);
		BackdropFullscreen = s.value(LIT("BackdropFullscreen"), BackdropFullscreen);
		BackdropCustomRes = s.value(LIT("BackdropCustomRes"), BackdropCustomRes);
		BackdropWidth = s.value(LIT("BackdropWidth"), BackdropWidth);
		BackdropHeight = s.value(LIT("BackdropHeight"), BackdropHeight);
		BattleMode = s.value(LIT("BattleMode"), BattleMode);
		BattleModeKey = s.value(LIT("BattleModeKey"), BattleModeKey);
		BattleModeBehavior = s.value(LIT("BattleModeBehavior"), BattleModeBehavior);
		MenuKey = s.value(LIT("MenuKey"), MenuKey);
		PanicMode = s.value(LIT("PanicMode"), PanicMode);
		PanicKey = s.value(LIT("PanicKey"), PanicKey);
		ReattachKey = s.value(LIT("ReattachKey"), ReattachKey);
		CustomCameraDistance = s.value(LIT("CustomCameraDistance"), CustomCameraDistance);
		CameraDistance = s.value(LIT("CameraDistance"), CameraDistance);
		FovChanger = s.value(LIT("FovChanger"), FovChanger);
		if (s.contains(LIT("fov_on"))) FovChanger = s.value(LIT("fov_on"), FovChanger);
		CameraFov = s.value(LIT("CameraFov"), CameraFov);
		if (s.contains(LIT("fov_value"))) CameraFov = s.value(LIT("fov_value"), CameraFov);
		FontName = s.value(LIT("FontName"), FontName);
		if (s.contains(LIT("MenuWindow")))
			MenuWindow.FromJson(s[LIT("MenuWindow")]);
		if (s.contains(LIT("RadarWindow")))
			RadarWindow.FromJson(s[LIT("RadarWindow")]);
		if (s.contains(LIT("AlertWindow")))
			AlertWindow.FromJson(s[LIT("AlertWindow")]);
		FromJsonColour(j, LIT("BackgroundColour"), &BackgroundColour);
		FromJsonColour(j, LIT("AccentColour"), &AccentColour);
	}
};
