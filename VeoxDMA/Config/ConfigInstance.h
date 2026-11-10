#pragma once
#include "Pch.h"
#include "PlayerConfig.h"
#include "OverlayConfig.h"
#include "WorldConfig.h"

class ConfigInstances
{
public:
	PlayerConfig Player = PlayerConfig(LIT("Player"));
	OverlayConfig Overlay = OverlayConfig(LIT("Overlay"));
	WorldConfig World = WorldConfig(LIT("World"));

	json ToJson()
	{
		json jsoned;
		jsoned.merge_patch(Player.ToJson());
		jsoned.merge_patch(Overlay.ToJson());
		jsoned.merge_patch(World.ToJson());
		return jsoned;
	}

	void FromJson(json jsoned)
	{
		Player.FromJson(jsoned);
		Overlay.FromJson(jsoned);
		World.FromJson(jsoned);
	}
};
inline ConfigInstances ConfigInstance;
