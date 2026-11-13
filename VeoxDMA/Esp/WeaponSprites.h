#pragma once
#include <d3d11.h>
#include <string>

namespace WeaponSprites {

void Init();
void Shutdown();

struct SpriteInfo {
	ID3D11ShaderResourceView* srv = nullptr;
	int width = 0;
	int height = 0;
};

// Looks up by weapon name (UTF-8). Returns nullptr srv if not loaded.
SpriteInfo Get(const std::string& weaponName);

} // namespace WeaponSprites
