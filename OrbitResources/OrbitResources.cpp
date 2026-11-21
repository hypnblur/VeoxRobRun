#include "OrbitResources.h"
#include "resource.h"

// Every icon the loot filter can draw is embedded in this DLL as a string-named
// RCDATA resource: the resource name is the sprite stem the catalog references
// (e.g. "fukurou_spark_9"), and the payload is a 64x64 RGBA PNG. tools/loot/
// gen_orbit_icons.py generates icons.rc and resource.h from the exported game
// sprites, so this file stays free of per-icon bookkeeping.

static HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

ORBIT_API BOOL OrbitGetIcon(const char* spriteName, const void** ppData, DWORD* pSize)
{
    if (!spriteName || !spriteName[0] || !ppData || !pSize) return FALSE;
    *ppData = nullptr;
    *pSize  = 0;

    // Resource names are UTF-16 identifiers.
    wchar_t wide[256]{};
    if (MultiByteToWideChar(CP_UTF8, 0, spriteName, -1, wide,
            (int)(sizeof(wide) / sizeof(wide[0]))) <= 0)
        return FALSE;

    HRSRC hRes = FindResourceW(g_hModule, wide, RT_RCDATA);
    if (!hRes) return FALSE;

    HGLOBAL hData = LoadResource(g_hModule, hRes);
    if (!hData) return FALSE;

    *ppData = LockResource(hData);
    *pSize  = SizeofResource(g_hModule, hRes);
    return (*ppData != nullptr && *pSize != 0);
}

ORBIT_API DWORD OrbitGetIconCount(void)
{
    return ORBIT_ICON_COUNT;
}
