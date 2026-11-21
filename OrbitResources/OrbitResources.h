#pragma once
#include <windows.h>

#ifdef ORBITRESOURCES_EXPORTS
#define ORBIT_API __declspec(dllexport)
#else
#define ORBIT_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

ORBIT_API BOOL OrbitGetIcon(const char* spriteName, const void** ppData, DWORD* pSize);
ORBIT_API DWORD OrbitGetIconCount(void);

#ifdef __cplusplus
}
#endif
