#pragma once
#include <d3d11.h>

namespace IconSprites {

// Loads OrbitResources.dll once. That DLL carries every icon the loot filter
// can draw; the exported sprite tree on disk is only a fallback for a build
// running without it.
void Init();
void Shutdown();

struct SpriteInfo {
    ID3D11ShaderResourceView* srv = nullptr;
    int width  = 0;
    int height = 0;
};

// Resolve one sprite stem (e.g. "fukurou_spark_9") to a decoded texture. The
// first call decodes and caches; misses are cached too so a missing sprite
// costs one lookup for the whole session.
SpriteInfo Get(const char* spriteName);

// Icon root used by the disk fallback (empty when the DLL is serving).
const wchar_t* Root();

// Icons embedded in OrbitResources.dll, and whether it is loaded.
int EmbeddedCount();
bool UsingEmbedded();

} // namespace IconSprites
