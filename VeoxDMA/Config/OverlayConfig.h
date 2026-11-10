#pragma once
#include "pch.h"

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
	bool ClickThroughWhenClosed = true;
	bool AlwaysOnTop = true;
	D2D1::ColorF BackgroundColour = Colour(0, 0, 0);

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
		j[ConfigName][LIT("ClickThroughWhenClosed")] = ClickThroughWhenClosed;
		j[ConfigName][LIT("AlwaysOnTop")] = AlwaysOnTop;
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
		if (j[ConfigName].contains(LIT("ClickThroughWhenClosed")))
			ClickThroughWhenClosed = j[ConfigName][LIT("ClickThroughWhenClosed")];
		if (j[ConfigName].contains(LIT("AlwaysOnTop")))
			AlwaysOnTop = j[ConfigName][LIT("AlwaysOnTop")];
		FromJsonColour(j, LIT("BackgroundColour"), &BackgroundColour);
	}
};
