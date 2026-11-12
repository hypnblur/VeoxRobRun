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
	int FrameCap = 0;                // 0 = uncapped, otherwise 1..1000
	bool Transparent = true;         // per-pixel alpha over the game
	bool HighContrastMenu = false;   // solid menu fills for bright scenes
	bool ClickThroughWhenClosed = true;
	bool AlwaysOnTop = true;
	D2D1::ColorF BackgroundColour = Colour(0, 0, 0);

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
		j[ConfigName][LIT("FrameCap")] = FrameCap;
		j[ConfigName][LIT("Transparent")] = Transparent;
		j[ConfigName][LIT("HighContrastMenu")] = HighContrastMenu;
		j[ConfigName][LIT("ClickThroughWhenClosed")] = ClickThroughWhenClosed;
		j[ConfigName][LIT("AlwaysOnTop")] = AlwaysOnTop;
		j[ConfigName][LIT("MenuWindow")] = MenuWindow.ToJson();
		j[ConfigName][LIT("RadarWindow")] = RadarWindow.ToJson();
		j[ConfigName][LIT("AlertWindow")] = AlertWindow.ToJson();
		ToJsonColour(&j, LIT("BackgroundColour"), &BackgroundColour);
		return j;
	}
	void FromJson(const json& j)
	{
		if (!j.contains(ConfigName))
			return;
		if (j[ConfigName].contains(LIT("MonitorIndex")))
			MonitorIndex = j[ConfigName][LIT("MonitorIndex")];
		if (j[ConfigName].contains(LIT("FPSEnabled")))
			FPSEnabled = j[ConfigName][LIT("FPSEnabled")];
		if (j[ConfigName].contains(LIT("FrameCap")))
			FrameCap = j[ConfigName][LIT("FrameCap")];
		if (j[ConfigName].contains(LIT("Transparent")))
			Transparent = j[ConfigName][LIT("Transparent")];
		if (j[ConfigName].contains(LIT("HighContrastMenu")))
			HighContrastMenu = j[ConfigName][LIT("HighContrastMenu")];
		if (j[ConfigName].contains(LIT("ClickThroughWhenClosed")))
			ClickThroughWhenClosed = j[ConfigName][LIT("ClickThroughWhenClosed")];
		if (j[ConfigName].contains(LIT("AlwaysOnTop")))
			AlwaysOnTop = j[ConfigName][LIT("AlwaysOnTop")];
		if (j[ConfigName].contains(LIT("MenuWindow")))
			MenuWindow.FromJson(j[ConfigName][LIT("MenuWindow")]);
		if (j[ConfigName].contains(LIT("RadarWindow")))
			RadarWindow.FromJson(j[ConfigName][LIT("RadarWindow")]);
		if (j[ConfigName].contains(LIT("AlertWindow")))
			AlertWindow.FromJson(j[ConfigName][LIT("AlertWindow")]);
		FromJsonColour(j, LIT("BackgroundColour"), &BackgroundColour);
	}
};
