#pragma once
#include "Pch.h"
#include "PlayerConfig.h"
#include "OverlayConfig.h"
#include "WorldConfig.h"
#include "AimConfig.h"

class ConfigInstances
{
public:
	PlayerConfig Player = PlayerConfig(LIT("Player"));
	OverlayConfig Overlay = OverlayConfig(LIT("Overlay"));
	WorldConfig World = WorldConfig(LIT("World"));
	AimConfig Aim = AimConfig(LIT("Aim"));

	json ToJson()
	{
		json jsoned;
		jsoned.merge_patch(Player.ToJson());
		jsoned.merge_patch(Overlay.ToJson());
		jsoned.merge_patch(World.ToJson());
		jsoned.merge_patch(Aim.ToJson());
		return jsoned;
	}

	void FromJson(json jsoned)
	{
		Player.FromJson(jsoned);
		Overlay.FromJson(jsoned);
		World.FromJson(jsoned);
		Aim.FromJson(jsoned);
	}
};
inline ConfigInstances ConfigInstance;
