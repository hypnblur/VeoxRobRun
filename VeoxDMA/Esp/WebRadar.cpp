#include "pch.h"
#include "WebRadar.h"
#include "WebRadarHtml.h"
#include "MapSpriteData.h"
#include "Radar.h"
#include "RobRunReader.h"
#include "RobRunSDK.h"
#include "ConfigInstance.h"
#include "EspDraw.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <utility>

namespace WebRadar {

namespace {

std::atomic<bool> g_serverRunning{ false };
std::atomic<int> g_activeStreams{ 0 };
std::thread g_serverThread;
int g_serverPort = 8888;
std::mutex g_socketMutex;
SOCKET g_listenSocket = INVALID_SOCKET;

// RAM cache for /maps/<filename>
std::mutex g_mapCacheMutex;
std::unordered_map<std::string, std::shared_ptr<const std::vector<char>>> g_mapFileCache;

// Shared state for vehicle driver/motion memory and cached loot serialization
std::mutex g_stateBuildMutex;

struct LastVehicleDriver {
	std::int32_t entityId = -1;
	int role = 0;
	std::string roleName;
	std::string nicknameUtf8;
	bool isTeammate = false;
	std::uint64_t stampMs = 0;
};
std::unordered_map<int, LastVehicleDriver> g_lastVehDriver;
std::uint64_t g_lastVehDriverSession = 0;

struct VehicleMotion {
	Vector3 anchorPos{};
	std::uint64_t lastMovedMs = 0;
	bool initialized = false;
};
std::unordered_map<int, VehicleMotion> g_vehMotion;
std::uint64_t g_vehMotionSession = 0;

std::uint64_t g_localPublicId = 0;
std::uint64_t g_localPubSession = 0;
std::string g_lastAutoLocationId;

const void* g_cachedLootWorldPtr = nullptr;
std::uint64_t g_cachedLootVer = 0;
json g_cachedLootArray = json::array();

void CloseListenSocket()
{
	SOCKET s = INVALID_SOCKET;
	{
		std::lock_guard<std::mutex> lock(g_socketMutex);
		s = std::exchange(g_listenSocket, INVALID_SOCKET);
	}
	if (s != INVALID_SOCKET)
		closesocket(s);
}

bool SendAll(SOCKET sock, const char* data, std::size_t len)
{
	std::size_t sent = 0;
	while (sent < len) {
		const int chunk = static_cast<int>((std::min)(len - sent, static_cast<std::size_t>(1 << 20)));
		const int n = send(sock, data + sent, chunk, 0);
		if (n <= 0) return false;
		sent += static_cast<std::size_t>(n);
	}
	return true;
}

std::string UrlDecode(const std::string& in)
{
	std::string out;
	out.reserve(in.size());
	auto hexVal = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (std::size_t i = 0; i < in.size(); ++i) {
		if (in[i] == '+') {
			out.push_back(' ');
		} else if (in[i] == '%' && i + 2 < in.size()) {
			const int hi = hexVal(in[i + 1]);
			const int lo = hexVal(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
			out.push_back(in[i]);
		} else {
			out.push_back(in[i]);
		}
	}
	return out;
}

std::string RgbHex(int r, int g, int b)
{
	char buf[10];
	std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
		std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
	return buf;
}

// ---------------------------------------------------------------------------
// Exact In-Game Map Role & Rarity Helpers (1:1 with Radar.cpp)
// ---------------------------------------------------------------------------
struct Rgb { int r, g, b; };

Rgb GetInGameMapRoleRgb(int role, const std::string& roleName, bool isTeammate)
{
	if (isTeammate) return { 75, 235, 235 };
	const std::string label = RobRunReader::RoleLabel(role, roleName);
	if (label == "Criminal") return { 255, 68, 68 };
	if (label == "Police Officer") return { 55, 145, 255 };
	if (label == "Civilian" || label == "Hubber") return { 70, 225, 90 };
	return { 255, 68, 68 };
}

Rgb MuteRoleRgb(Rgb c)
{
	return {
		static_cast<int>(c.r * 0.55f + 185.f * 0.45f),
		static_cast<int>(c.g * 0.55f + 190.f * 0.45f),
		static_cast<int>(c.b * 0.55f + 198.f * 0.45f)
	};
}

int RolePriority(int role, const std::string& roleName, bool isTeammate)
{
	if (isTeammate) return 1;
	const std::string label = RobRunReader::RoleLabel(role, roleName);
	if (label == "Criminal") return 4;
	if (label == "Police Officer") return 3;
	return 2;
}

int ResolveItemRarityTier(const RobRunWorld::Entity& we, const LootFilter::ItemDef* def)
{
	if (we.rarity >= 1 && we.rarity <= 6)
		return we.rarity;
	if (def) {
		switch (def->rarity) {
		case LootFilter::Rarity::Common:    return 1;
		case LootFilter::Rarity::Uncommon:  return 2;
		case LootFilter::Rarity::Rare:      return 3;
		case LootFilter::Rarity::Epic:      return 4;
		case LootFilter::Rarity::Legendary: return 5;
		case LootFilter::Rarity::Exotic:    return 6;
		default: break;
		}
	}
	return 0;
}

std::string MapRarityHex(int tier, const char* fallback)
{
	switch (tier) {
	case 1: return "#dce2ea"; // Common
	case 2: return "#2ee04e"; // Uncommon
	case 3: return "#3aa0ff"; // Rare
	case 4: return "#b64cff"; // Epic
	case 5: return "#ffd02a"; // Legendary
	case 6: return "#ff3a80"; // Mythical / Exotic
	default: return fallback;
	}
}

bool IsCargoKind(RobRunWorld::Kind k)
{
	switch (k) {
	case RobRunWorld::Kind::Cargo:
	case RobRunWorld::Kind::CargoBush:
	case RobRunWorld::Kind::CargoCrystal:
	case RobRunWorld::Kind::CargoDrug:
	case RobRunWorld::Kind::CargoCash:
	case RobRunWorld::Kind::CargoExchange:
	case RobRunWorld::Kind::CargoBox:
	case RobRunWorld::Kind::CargoGoods:
		return true;
	default:
		return false;
	}
}

bool IsMapLootKind(RobRunWorld::Kind k)
{
	switch (k) {
	case RobRunWorld::Kind::Loot:
	case RobRunWorld::Kind::StaticLoot:
	case RobRunWorld::Kind::RandomLoot:
	case RobRunWorld::Kind::VehicleLoot:
	case RobRunWorld::Kind::PlotLoot:
	case RobRunWorld::Kind::FurnitureLoot:
	case RobRunWorld::Kind::BuildingLoot:
	case RobRunWorld::Kind::CashLootNode:
	case RobRunWorld::Kind::SettlementCash:
	case RobRunWorld::Kind::SafeDoor:
		return true;
	default:
		return false;
	}
}

bool LookupEmbeddedSprite(const std::string& name, const std::uint8_t*& outPtr, std::size_t& outSize)
{
	struct Entry { const char* name; const std::uint8_t* ptr; std::size_t size; };
	static constexpr Entry kTable[] = {
		{ "player_arrow_base.png", MapSpriteData::kMapPngPlayerArrowBase, sizeof(MapSpriteData::kMapPngPlayerArrowBase) },
		{ "player_arrow_fill.png", MapSpriteData::kMapPngPlayerArrowFill, sizeof(MapSpriteData::kMapPngPlayerArrowFill) },
		{ "car_base.png",          MapSpriteData::kMapPngCarBase,         sizeof(MapSpriteData::kMapPngCarBase) },
		{ "car_fill.png",          MapSpriteData::kMapPngCarFill,         sizeof(MapSpriteData::kMapPngCarFill) },
		{ "police_car_base.png",   MapSpriteData::kMapPngPoliceCarBase,   sizeof(MapSpriteData::kMapPngPoliceCarBase) },
		{ "police_car_fill.png",   MapSpriteData::kMapPngPoliceCarFill,   sizeof(MapSpriteData::kMapPngPoliceCarFill) },
		{ "helicopter_base.png",   MapSpriteData::kMapPngHelicopterBase,  sizeof(MapSpriteData::kMapPngHelicopterBase) },
		{ "helicopter_fill.png",   MapSpriteData::kMapPngHelicopterFill,  sizeof(MapSpriteData::kMapPngHelicopterFill) },
		{ "dropped_bag_base.png",  MapSpriteData::kMapPngDroppedBagBase,  sizeof(MapSpriteData::kMapPngDroppedBagBase) },
		{ "dropped_bag_fill.png",  MapSpriteData::kMapPngDroppedBagFill,  sizeof(MapSpriteData::kMapPngDroppedBagFill) },
		{ "weapon_base.png",       MapSpriteData::kMapPngWeaponBase,      sizeof(MapSpriteData::kMapPngWeaponBase) },
		{ "weapon_fill.png",       MapSpriteData::kMapPngWeaponFill,      sizeof(MapSpriteData::kMapPngWeaponFill) },
		{ "armor_base.png",        MapSpriteData::kMapPngArmorBase,       sizeof(MapSpriteData::kMapPngArmorBase) },
		{ "armor_fill.png",        MapSpriteData::kMapPngArmorFill,       sizeof(MapSpriteData::kMapPngArmorFill) },
		{ "valuable_base.png",     MapSpriteData::kMapPngValuableBase,    sizeof(MapSpriteData::kMapPngValuableBase) },
		{ "valuable_fill.png",     MapSpriteData::kMapPngValuableFill,    sizeof(MapSpriteData::kMapPngValuableFill) },
		{ "cash_base.png",         MapSpriteData::kMapPngCashBase,        sizeof(MapSpriteData::kMapPngCashBase) },
		{ "cash_fill.png",         MapSpriteData::kMapPngCashFill,        sizeof(MapSpriteData::kMapPngCashFill) },
		{ "safe_base.png",         MapSpriteData::kMapPngSafeBase,        sizeof(MapSpriteData::kMapPngSafeBase) },
		{ "safe_fill.png",         MapSpriteData::kMapPngSafeFill,        sizeof(MapSpriteData::kMapPngSafeFill) },
		{ "cargo_base.png",        MapSpriteData::kMapPngCargoBase,       sizeof(MapSpriteData::kMapPngCargoBase) },
		{ "cargo_fill.png",        MapSpriteData::kMapPngCargoFill,       sizeof(MapSpriteData::kMapPngCargoFill) },
		{ "medicine_base.png",     MapSpriteData::kMapPngMedicineBase,    sizeof(MapSpriteData::kMapPngMedicineBase) },
		{ "medicine_fill.png",     MapSpriteData::kMapPngMedicineFill,    sizeof(MapSpriteData::kMapPngMedicineFill) },
		{ "key_base.png",          MapSpriteData::kMapPngKeyBase,         sizeof(MapSpriteData::kMapPngKeyBase) },
		{ "key_fill.png",          MapSpriteData::kMapPngKeyFill,         sizeof(MapSpriteData::kMapPngKeyFill) },
		{ "generic_base.png",      MapSpriteData::kMapPngGenericLootBase, sizeof(MapSpriteData::kMapPngGenericLootBase) },
		{ "generic_fill.png",      MapSpriteData::kMapPngGenericLootFill, sizeof(MapSpriteData::kMapPngGenericLootFill) },
	};
	for (const auto& e : kTable) {
		if (name == e.name) {
			outPtr = e.ptr;
			outSize = e.size;
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// High-Speed Frame JSON Builder (ports Radar.cpp::DrawInGameMapOverlay 1:1)
// ---------------------------------------------------------------------------
std::string BuildStateJson(bool forceIncludeLoot, std::uint64_t& clientLootVer)
{
	std::lock_guard<std::mutex> lock(g_stateBuildMutex);

	const auto snapshot = RobRunReader::GetSnapshot();
	const std::string loc = RobRunReader::GetLocationId();
	const auto& cfg = ConfigInstance.Radar;
	const auto& pcfg = ConfigInstance.Player;

	// Auto-select map if location changed (thread-safe RequestSetMap)
	if (cfg.AutoMapSelect && !loc.empty() && loc != g_lastAutoLocationId) {
		g_lastAutoLocationId = loc;
		const char* mapped = Radar::MapFileForLocation(loc);
		if (mapped) {
			const auto& files = Radar::GetMapFiles();
			for (int i = 0; i < (int)files.size(); ++i) {
				if (files[i] == mapped) {
					Radar::RequestSetMap(i);
					break;
				}
			}
		}
	}

	json j;
	j["status"] = "ok";
	j["locationId"] = loc;

	const std::string activeMap = Radar::GetCurrentMapName();
	j["mapName"] = activeMap;
	j["mapUrl"] = "/maps/" + activeMap;
	j["availableMaps"] = Radar::GetMapFiles();

	// Sync VeoxMenu accent color live
	{
		const auto& ac = ConfigInstance.Overlay.AccentColour;
		const float ar = std::clamp(ac.r, 0.08f, 1.f);
		const float ag = std::clamp(ac.g, 0.08f, 1.f);
		const float ab = std::clamp(ac.b, 0.08f, 1.f);
		j["accent"] = RgbHex((int)(ar * 255.f), (int)(ag * 255.f), (int)(ab * 255.f));
		j["accentBright"] = RgbHex(
			(int)(std::clamp(ar * 0.78f + 0.22f, 0.f, 1.f) * 255.f),
			(int)(std::clamp(ag * 0.78f + 0.22f, 0.f, 1.f) * 255.f),
			(int)(std::clamp(ab * 0.78f + 0.22f, 0.f, 1.f) * 255.f));
	}

	j["bounds"] = {
		{ "minX", cfg.WorldMinX },
		{ "maxX", cfg.WorldMaxX },
		{ "minZ", cfg.WorldMinZ },
		{ "maxZ", cfg.WorldMaxZ }
	};

	j["defaults"] = {
		{ "players", cfg.InGameMapPlayers },
		{ "vehicles", cfg.InGameMapVehicles },
		{ "names", cfg.InGameMapPlayerNames },
		{ "bags", cfg.InGameMapDroppedBags },
		{ "valuables", cfg.InGameMapValuables },
		{ "cargo", cfg.InGameMapCargo },
		{ "minRarity", cfg.InGameMapMinRarity },
		{ "cargoMinRarity", cfg.InGameMapCargoMinRarity },
		{ "scale", cfg.InGameMapScale }
	};

	if (snapshot && snapshot->inGameMap.valid) {
		j["inGameMap"] = {
			{ "valid", true },
			{ "fullscreenOpen", snapshot->inGameMap.fullscreenOpen },
			{ "minimapCamSize", snapshot->inGameMap.minimap.valid ? snapshot->inGameMap.minimap.cameraSize : 0.f }
		};
	} else {
		j["inGameMap"] = { { "valid", false }, { "fullscreenOpen", false }, { "minimapCamSize", 0.f } };
	}

	// Local player (prefer fresh camera forward for zero-lag heading)
	if (snapshot && !snapshot->localPosition.IsZero()) {
		float yaw = snapshot->localYaw;
		RobRunCamera::CameraState freshCam{};
		if (RobRunReader::GetFreshCamera(freshCam) && freshCam.valid) {
			const float hSq = freshCam.forward.x * freshCam.forward.x + freshCam.forward.z * freshCam.forward.z;
			if (hSq > 1e-4f)
				yaw = std::atan2(freshCam.forward.x, freshCam.forward.z);
		} else if (snapshot->camera.valid) {
			const float hSq = snapshot->camera.forward.x * snapshot->camera.forward.x + snapshot->camera.forward.z * snapshot->camera.forward.z;
			if (hSq > 1e-4f)
				yaw = std::atan2(snapshot->camera.forward.x, snapshot->camera.forward.z);
		}
		j["local"] = {
			{ "valid", true },
			{ "x", snapshot->localPosition.x },
			{ "y", snapshot->localPosition.y },
			{ "z", snapshot->localPosition.z },
			{ "yaw", yaw },
			{ "entityId", snapshot->localEntityId }
		};
	} else {
		j["local"] = { { "valid", false } };
	}

	// Vehicles + Players (exact Radar.cpp::DrawInGameMapOverlay occupant/driver/motion logic)
	json playersOut = json::array();
	json vehiclesOut = json::array();

	if (snapshot) {
		const std::int32_t localId = snapshot->localEntityId;
		const bool useFastVeh = !snapshot->fastVehicles.empty();
		const bool haveWorld = snapshot->world && snapshot->world->valid;

		if (snapshot->sessionGeneration != g_localPubSession) {
			g_localPubSession = snapshot->sessionGeneration;
			g_localPublicId = 0;
		}
		if (snapshot->sessionGeneration != g_lastVehDriverSession) {
			g_lastVehDriverSession = snapshot->sessionGeneration;
			g_lastVehDriver.clear();
		}
		if (snapshot->sessionGeneration != g_vehMotionSession) {
			g_vehMotionSession = snapshot->sessionGeneration;
			g_vehMotion.clear();
		}
		for (const auto& ent : snapshot->entities) {
			if (ent.entityId == localId && ent.publicId > 0) {
				g_localPublicId = ent.publicId;
				break;
			}
		}

		struct MapVehicle {
			int id = 0;
			RobRunWorld::Kind kind = RobRunWorld::Kind::Car;
			Vector3 position{};
			std::uint64_t ownerPublicId = 0;
			std::string ownerRoleName;
			std::string ownerNicknameUtf8;
			bool hasDriver = false;
		};
		std::vector<MapVehicle> mapVehicles;
		auto isWorldVehicle = [](RobRunWorld::Kind k) {
			return k == RobRunWorld::Kind::Car ||
			       k == RobRunWorld::Kind::Helicopter ||
			       k == RobRunWorld::Kind::Vehicle;
		};

		if (useFastVeh) {
			mapVehicles.reserve(snapshot->fastVehicles.size());
			for (const auto& fv : snapshot->fastVehicles) {
				mapVehicles.push_back(MapVehicle{
					fv.id, fv.kind, fv.position, fv.ownerPublicId, fv.ownerRoleName, fv.ownerNicknameUtf8, fv.hasDriver
				});
			}
		} else if (haveWorld) {
			for (const auto& we : snapshot->world->entities) {
				if (!isWorldVehicle(we.kind)) continue;
				mapVehicles.push_back(MapVehicle{ we.id, we.kind, we.position, 0, {}, {}, false });
			}
		}

		const std::size_t vehCount = mapVehicles.size();
		struct VehicleOccupantInfo {
			bool occupied = false;
			bool localOccupied = false;
			bool isPolice = false;
			int priority = 0;
			Rgb color{ 210, 216, 224 };
			Vector3 driverPos{};
			bool hasDriverPos = false;
			std::vector<std::string> occupantNames;
		};
		std::vector<VehicleOccupantInfo> vehOcc(vehCount);
		std::vector<int> playerMatchedVeh(snapshot->entities.size(), -1);
		const std::uint64_t vehNowMs = GetTickCount64();

		if (vehCount > 0) {
			for (std::size_t ei = 0; ei < snapshot->entities.size(); ++ei) {
				const auto& ent = snapshot->entities[ei];
				if (ent.isDead) continue;
				if (!ent.inVehicle && ent.vehicleId <= 0) continue;

				const bool isLocal = (ent.entityId == localId);
				const bool isTeammate = ent.isPartyMember || ent.isClanMate;
				if (!isLocal && pcfg.HideTeammates && isTeammate) continue;

				int matchedIdx = -1;
				if (ent.vehicleId > 0) {
					for (std::size_t vi = 0; vi < vehCount; ++vi) {
						if (mapVehicles[vi].id == ent.vehicleId) {
							matchedIdx = static_cast<int>(vi);
							break;
						}
					}
				}
				if (matchedIdx < 0 && ent.inVehicle &&
					std::isfinite(ent.position.x) && std::isfinite(ent.position.z))
				{
					float bestDistSq = 36.f;
					for (std::size_t vi = 0; vi < vehCount; ++vi) {
						const float dx = mapVehicles[vi].position.x - ent.position.x;
						const float dz = mapVehicles[vi].position.z - ent.position.z;
						const float dSq = dx * dx + dz * dz;
						if (dSq < bestDistSq) {
							bestDistSq = dSq;
							matchedIdx = static_cast<int>(vi);
						}
					}
				}

				if (matchedIdx >= 0) {
					playerMatchedVeh[ei] = matchedIdx;
					auto& occ = vehOcc[static_cast<std::size_t>(matchedIdx)];
					if (isLocal) {
						occ.localOccupied = true;
						g_lastVehDriver.erase(mapVehicles[static_cast<std::size_t>(matchedIdx)].id);
					} else {
						if (!ent.nicknameUtf8.empty())
							occ.occupantNames.push_back(ent.nicknameUtf8);
						const int prio = RolePriority(ent.role, ent.roleName, isTeammate);
						if (!occ.occupied || prio > occ.priority) {
							occ.occupied = true;
							occ.priority = prio;
							occ.isPolice = (RobRunReader::RoleLabel(ent.role, ent.roleName) == "Police Officer");
							occ.color = GetInGameMapRoleRgb(ent.role, ent.roleName, isTeammate);
							if (std::isfinite(ent.position.x) && std::isfinite(ent.position.z)) {
								occ.driverPos = ent.position;
								occ.hasDriverPos = true;
							}
							auto& ld = g_lastVehDriver[mapVehicles[static_cast<std::size_t>(matchedIdx)].id];
							ld.entityId = ent.entityId;
							ld.role = ent.role;
							ld.roleName = ent.roleName;
							if (!ent.nicknameUtf8.empty())
								ld.nicknameUtf8 = ent.nicknameUtf8;
							ld.isTeammate = isTeammate;
							ld.stampMs = vehNowMs;
						}
					}
				}
			}
		}

		constexpr std::uint64_t kVehRecentMoveMs = 10'000ULL;
		constexpr std::uint64_t kLastDriverMaxAgeMs = 120'000ULL;

		for (std::size_t vi = 0; vi < vehCount; ++vi) {
			const auto& mv = mapVehicles[vi];
			const auto& occ = vehOcc[vi];
			if (occ.localOccupied) continue; // Never draw local player's vehicle while seated inside it
			const Vector3& vehWorldPos = (occ.occupied && occ.hasDriverPos) ? occ.driverPos : mv.position;
			if (!std::isfinite(vehWorldPos.x) || !std::isfinite(vehWorldPos.z)) continue;

			auto& mot = g_vehMotion[mv.id];
			if (!mot.initialized) {
				mot.anchorPos = vehWorldPos;
				mot.lastMovedMs = 0;
				mot.initialized = true;
			} else {
				const float mdx = vehWorldPos.x - mot.anchorPos.x;
				const float mdz = vehWorldPos.z - mot.anchorPos.z;
				if (mdx * mdx + mdz * mdz > 0.1225f) {
					mot.anchorPos = vehWorldPos;
					mot.lastMovedMs = vehNowMs;
				}
			}
			const bool recentlyMoved = (mot.lastMovedMs != 0) && (vehNowMs - mot.lastMovedMs <= kVehRecentMoveMs);
			const bool activeRemoteVeh = !occ.occupied && (mv.hasDriver || recentlyMoved);

			const LastVehicleDriver* remembered = nullptr;
			if (!occ.occupied) {
				auto it = g_lastVehDriver.find(mv.id);
				if (it != g_lastVehDriver.end()) {
					if (vehNowMs - it->second.stampMs <= kLastDriverMaxAgeMs) {
						remembered = &it->second;
					} else {
						g_lastVehDriver.erase(it);
					}
				}
			}

			Rgb vehRgb{ 210, 216, 224 };
			std::string label;
			const bool isOwnVeh = !occ.occupied &&
				(occ.localOccupied || (g_localPublicId > 0 && mv.ownerPublicId == g_localPublicId));

			if (isOwnVeh) {
				vehRgb = { 70, 225, 90 };
			} else if (occ.occupied) {
				vehRgb = occ.color;
				for (std::size_t ni = 0; ni < occ.occupantNames.size(); ++ni) {
					if (ni > 0) label += ", ";
					label += occ.occupantNames[ni];
				}
			} else if (activeRemoteVeh) {
				if (remembered) {
					vehRgb = GetInGameMapRoleRgb(remembered->role, remembered->roleName, remembered->isTeammate);
					label = remembered->nicknameUtf8;
				}
				if (label.empty() && !mv.ownerNicknameUtf8.empty())
					label = mv.ownerNicknameUtf8;
				if (!remembered && !mv.ownerRoleName.empty())
					vehRgb = GetInGameMapRoleRgb(0, mv.ownerRoleName, false);
			} else if (remembered || !mv.ownerRoleName.empty()) {
				Rgb baseRole{ 210, 216, 224 };
				if (remembered) {
					baseRole = GetInGameMapRoleRgb(remembered->role, remembered->roleName, remembered->isTeammate);
					label = remembered->nicknameUtf8;
				} else {
					baseRole = GetInGameMapRoleRgb(0, mv.ownerRoleName, false);
				}
				if (label.empty() && !mv.ownerNicknameUtf8.empty())
					label = mv.ownerNicknameUtf8;
				vehRgb = MuteRoleRgb(baseRole);
			}

			const bool isPoliceVeh =
				(occ.occupied && occ.isPolice) ||
				(!occ.occupied && remembered && RobRunReader::RoleLabel(remembered->role, remembered->roleName) == "Police Officer") ||
				(!occ.occupied && !remembered && !mv.ownerRoleName.empty() && RobRunReader::RoleLabel(0, mv.ownerRoleName) == "Police Officer");

			const char* sprite = (mv.kind == RobRunWorld::Kind::Helicopter)
				? "helicopter"
				: (isPoliceVeh ? "police_car" : "car");

			vehiclesOut.push_back({
				{ "id", mv.id },
				{ "x", vehWorldPos.x },
				{ "z", vehWorldPos.z },
				{ "sprite", sprite },
				{ "color", RgbHex(vehRgb.r, vehRgb.g, vehRgb.b) },
				{ "label", label },
				{ "occupied", occ.occupied || occ.localOccupied }
			});
		}

		// Players
		for (std::size_t ei = 0; ei < snapshot->entities.size(); ++ei) {
			const auto& ent = snapshot->entities[ei];
			if (ent.isDead) continue;
			if (!std::isfinite(ent.position.x) || !std::isfinite(ent.position.z)) continue;

			const bool isLocal = (ent.entityId == localId);
			const bool isTeammate = ent.isPartyMember || ent.isClanMate;
			if (!isLocal && pcfg.HideTeammates && isTeammate) continue;

			float entYaw = ent.yaw;
			const float lookHorizSq = ent.lookDirection.x * ent.lookDirection.x + ent.lookDirection.z * ent.lookDirection.z;
			if (std::isfinite(lookHorizSq) && lookHorizSq > 0.01f)
				entYaw = std::atan2(ent.lookDirection.x, ent.lookDirection.z);

			float dist = 0.f;
			if (!snapshot->localPosition.IsZero())
				dist = Distance(ent.position, snapshot->localPosition);

			const Rgb rgb = isLocal ? Rgb{ 255, 255, 255 } : GetInGameMapRoleRgb(ent.role, ent.roleName, isTeammate);
			const bool inVeh = ent.inVehicle || (playerMatchedVeh[ei] >= 0);

			playersOut.push_back({
				{ "id", ent.entityId },
				{ "name", ent.nicknameUtf8 },
				{ "isLocal", isLocal },
				{ "isTeammate", isTeammate },
				{ "roleLabel", RobRunReader::RoleLabel(ent.role, ent.roleName) },
				{ "color", RgbHex(rgb.r, rgb.g, rgb.b) },
				{ "x", ent.position.x },
				{ "z", ent.position.z },
				{ "yaw", std::isfinite(entYaw) ? entYaw : 0.f },
				{ "distance", dist },
				{ "inVehicle", inVeh },
				{ "matchedVeh", playerMatchedVeh[ei] >= 0 }
			});
		}

		// Rebuild cached loot array ONLY when snapshot->world pointer changes
		const void* curWorldPtr = (haveWorld) ? static_cast<const void*>(snapshot->world.get()) : nullptr;
		if (curWorldPtr != g_cachedLootWorldPtr) {
			g_cachedLootWorldPtr = curWorldPtr;
			++g_cachedLootVer;
			g_cachedLootArray = json::array();

			if (haveWorld) {
				const auto& lf = ConfigInstance.LootFilter;
				for (const auto& we : snapshot->world->entities) {
					if (!std::isfinite(we.position.x) || !std::isfinite(we.position.z)) continue;

					if (we.kind == RobRunWorld::Kind::DroppedBag) {
						g_cachedLootArray.push_back({
							{ "cat", "bag" },
							{ "sprite", "dropped_bag" },
							{ "tier", 0 },
							{ "alwaysShow", true },
							{ "color", "#ffc832" },
							{ "x", we.position.x },
							{ "z", we.position.z }
						});
						continue;
					}

					if (we.hasEmptyFlag && we.empty) continue;
					if (we.inVehicle || we.inTable) continue;

					if (IsCargoKind(we.kind)) {
						const LootFilter::ItemDef* def = lf.FindItemForEntity(we.kind, we.label);
						const int tier = ResolveItemRarityTier(we, def);
						const int effTier = (tier > 0) ? tier : 1;
						const bool isCargoCash = (we.kind == RobRunWorld::Kind::CargoCash);
						g_cachedLootArray.push_back({
							{ "cat", "cargo" },
							{ "sprite", isCargoCash ? "cash" : "cargo" },
							{ "tier", effTier },
							{ "alwaysShow", false },
							{ "color", isCargoCash ? "#5ff582" : MapRarityHex(tier, "#ff6ee6") },
							{ "x", we.position.x },
							{ "z", we.position.z }
						});
						continue;
					}

					if (IsMapLootKind(we.kind)) {
						const LootFilter::ItemDef* def = lf.FindItemForEntity(we.kind, we.label);
						const bool isCash = (we.kind == RobRunWorld::Kind::CashLootNode ||
											 we.kind == RobRunWorld::Kind::SettlementCash);
						const bool isSafe = (we.kind == RobRunWorld::Kind::SafeDoor);
						const int tier = ResolveItemRarityTier(we, def);
						const bool isCatalogVal = def && def->category == LootFilter::Category::Valuables;
						const int effTier = (tier > 0) ? tier : (isCatalogVal ? 2 : 1);

						const char* sprite = "generic";
						const char* fallbackHex = "#64ff8c";
						if (isSafe) {
							sprite = "safe";
							fallbackHex = "#ffd02a";
						} else if (isCash) {
							sprite = "cash";
							fallbackHex = "#5ff582";
						} else if (def) {
							switch (def->category) {
							case LootFilter::Category::Weapons:
							case LootFilter::Category::WeaponMaintenance: sprite = "weapon"; break;
							case LootFilter::Category::Armor:             sprite = "armor"; break;
							case LootFilter::Category::Valuables:         sprite = "valuable"; break;
							case LootFilter::Category::Medicine:          sprite = "medicine"; break;
							case LootFilter::Category::Keys:
							case LootFilter::Category::Documents:
							case LootFilter::Category::Tools:
							case LootFilter::Category::BreachingTools:    sprite = "key"; break;
							case LootFilter::Category::Cargo:             sprite = "cargo"; break;
							default:                                      sprite = "generic"; break;
							}
						}

						g_cachedLootArray.push_back({
							{ "cat", "loot" },
							{ "sprite", sprite },
							{ "tier", effTier },
							{ "alwaysShow", isCash || isSafe },
							{ "color", (isCash || isSafe) ? std::string(fallbackHex) : MapRarityHex(tier, fallbackHex) },
							{ "x", we.position.x },
							{ "z", we.position.z }
						});
					}
				}
			}
		}
	}

	j["players"] = std::move(playersOut);
	j["vehicles"] = std::move(vehiclesOut);
	j["lootVer"] = g_cachedLootVer;
	if (forceIncludeLoot || clientLootVer != g_cachedLootVer) {
		j["loot"] = g_cachedLootArray;
		clientLootVer = g_cachedLootVer;
	}

	return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---------------------------------------------------------------------------
// Dedicated SSE Stream Handler (50 Hz zero-overhead push over 1 TCP socket)
// ---------------------------------------------------------------------------
void RunSseClient(SOCKET client)
{
	struct Guard {
		SOCKET s;
		~Guard() {
			closesocket(s);
			g_activeStreams.fetch_sub(1, std::memory_order_relaxed);
		}
	} guard{ client };

	const std::string header =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream; charset=utf-8\r\n"
		"Cache-Control: no-cache, no-transform\r\n"
		"Connection: keep-alive\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"X-Accel-Buffering: no\r\n\r\n";

	if (!SendAll(client, header.data(), header.size()))
		return;

	std::uint64_t clientLootVer = 0;
	bool firstFrame = true;

	while (g_serverRunning.load(std::memory_order_relaxed)) {
		try {
			std::string payload = BuildStateJson(firstFrame, clientLootVer);
			firstFrame = false;

			std::string frame;
			frame.reserve(payload.size() + 8);
			frame.append("data: ");
			frame.append(payload);
			frame.append("\n\n");

			if (!SendAll(client, frame.data(), frame.size()))
				break;
		} catch (...) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 50 Hz
	}
}

// ---------------------------------------------------------------------------
// Worker Thread: Lightweight HTTP 1.1 + SSE Web Server
// ---------------------------------------------------------------------------
void RunServerLoop(int port)
{
	WSADATA wsaData{};
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		g_serverRunning.store(false);
		return;
	}
	struct WsaGuard {
		~WsaGuard() { WSACleanup(); }
	} wsaGuard;

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET) {
		g_serverRunning.store(false);
		return;
	}

	int opt = 1;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(static_cast<u_short>(port));

	if (bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
		closesocket(listenSock);
		g_serverRunning.store(false);
		return;
	}

	if (listen(listenSock, 16) == SOCKET_ERROR) {
		closesocket(listenSock);
		g_serverRunning.store(false);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_socketMutex);
		if (!g_serverRunning.load()) {
			closesocket(listenSock);
			return;
		}
		g_listenSocket = listenSock;
	}

	printf("[WebRadar] Server listening on http://localhost:%d\n", port);

	while (g_serverRunning.load()) {
		SOCKET activeListen = INVALID_SOCKET;
		{
			std::lock_guard<std::mutex> lock(g_socketMutex);
			activeListen = g_listenSocket;
		}
		if (activeListen == INVALID_SOCKET) break;

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(activeListen, &readSet);

		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 100000;

		int sel = select(0, &readSet, nullptr, nullptr, &tv);
		if (sel <= 0) continue;

		sockaddr_in clientAddr{};
		int clientAddrLen = sizeof(clientAddr);
		SOCKET client = accept(activeListen, (sockaddr*)&clientAddr, &clientAddrLen);
		if (client == INVALID_SOCKET) continue;

		try {
			// Disable Nagle's algorithm so small state frames flush immediately
			int nodelay = 1;
			setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

			DWORD timeoutMs = 2000;
			setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
			setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

			char reqBuf[4096] = {};
			int bytesRead = recv(client, reqBuf, sizeof(reqBuf) - 1, 0);
			if (bytesRead <= 0) {
				closesocket(client);
				continue;
			}

			std::string request(reqBuf, bytesRead);
			std::istringstream reqStream(request);
			std::string method, path, proto;
			reqStream >> method >> path >> proto;

			if (method == "OPTIONS") {
				const std::string resp =
					"HTTP/1.1 204 No Content\r\n"
					"Access-Control-Allow-Origin: *\r\n"
					"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
					"Access-Control-Allow-Headers: Content-Type\r\n"
					"Content-Length: 0\r\n"
					"Connection: close\r\n\r\n";
				SendAll(client, resp.data(), resp.size());
				closesocket(client);
				continue;
			}

			// Root SPA
			if (path == "/" || path == "/index.html" || path.empty()) {
				const std::string& body = GetWebRadarHtml();
				const std::string header =
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: text/html; charset=utf-8\r\n"
					"Content-Length: " + std::to_string(body.size()) + "\r\n"
					"Access-Control-Allow-Origin: *\r\n"
					"Connection: close\r\n\r\n";
				if (SendAll(client, header.data(), header.size()))
					SendAll(client, body.data(), body.size());
			}
			// High-speed 50 Hz SSE Stream
			else if (path.rfind("/api/stream", 0) == 0) {
				if (g_activeStreams.load(std::memory_order_relaxed) < 8) {
					g_activeStreams.fetch_add(1, std::memory_order_relaxed);
					std::thread(RunSseClient, client).detach();
					continue; // ownership transferred to SSE thread
				} else {
					const std::string busy = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
					SendAll(client, busy.data(), busy.size());
				}
			}
			// One-shot / fallback JSON state
			else if (path.rfind("/api/state", 0) == 0) {
				std::uint64_t dummyVer = 0;
				const std::string body = BuildStateJson(true, dummyVer);
				const std::string header =
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/json; charset=utf-8\r\n"
					"Content-Length: " + std::to_string(body.size()) + "\r\n"
					"Access-Control-Allow-Origin: *\r\n"
					"Connection: close\r\n\r\n";
				if (SendAll(client, header.data(), header.size()))
					SendAll(client, body.data(), body.size());
			}
			// Embedded 2-pass map sprites (/sprites/<name>.png)
			else if (path.rfind("/sprites/", 0) == 0) {
				std::string spriteName = path.substr(9);
				const auto q = spriteName.find('?');
				if (q != std::string::npos) spriteName.resize(q);
				const std::uint8_t* ptr = nullptr;
				std::size_t sz = 0;
				if (LookupEmbeddedSprite(spriteName, ptr, sz) && ptr && sz > 0) {
					const std::string header =
						"HTTP/1.1 200 OK\r\n"
						"Content-Type: image/png\r\n"
						"Content-Length: " + std::to_string(sz) + "\r\n"
						"Cache-Control: public, max-age=86400\r\n"
						"Access-Control-Allow-Origin: *\r\n"
						"Connection: close\r\n\r\n";
					if (SendAll(client, header.data(), header.size()))
						SendAll(client, reinterpret_cast<const char*>(ptr), sz);
				} else {
					const std::string nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
					SendAll(client, nf.data(), nf.size());
				}
			}
			// Switch active map
			else if (path.rfind("/api/setmap", 0) == 0) {
				const auto pos = path.find("name=");
				if (pos != std::string::npos) {
					std::string rawName = path.substr(pos + 5);
					const auto amp = rawName.find('&');
					if (amp != std::string::npos) rawName.resize(amp);
					const std::string name = UrlDecode(rawName);
					const auto& files = Radar::GetMapFiles();
					for (int i = 0; i < (int)files.size(); ++i) {
						if (files[i] == name) {
							Radar::RequestSetMap(i);
							break;
						}
					}
				}
				const std::string body = "{\"status\":\"ok\"}";
				const std::string header =
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/json\r\n"
					"Content-Length: " + std::to_string(body.size()) + "\r\n"
					"Access-Control-Allow-Origin: *\r\n"
					"Connection: close\r\n\r\n";
				if (SendAll(client, header.data(), header.size()))
					SendAll(client, body.data(), body.size());
			}
			// BVH 2D collision wall contours
			else if (path == "/api/bvh") {
				json lines = json::array();
				const auto scene = RobRunReader::GetBvhScene();
				const auto snap = RobRunReader::GetSnapshot();

				if (scene && scene->valid && !scene->tris.empty()) {
					const float centerY = snap ? snap->localPosition.y : 0.f;
					constexpr float range = 180.f;
					constexpr float rangeSq = range * range;
					const float cx = snap ? snap->localPosition.x : 0.f;
					const float cz = snap ? snap->localPosition.z : 0.f;

					int count = 0;
					for (const auto& tri : scene->tris) {
						const float midY = (tri.v0.y + tri.v1.y + tri.v2.y) * 0.3333f;
						if (std::abs(midY - centerY) > 8.0f) continue;

						const float midX = (tri.v0.x + tri.v1.x + tri.v2.x) * 0.3333f;
						const float midZ = (tri.v0.z + tri.v1.z + tri.v2.z) * 0.3333f;
						const float dx = midX - cx, dz = midZ - cz;
						if (dx * dx + dz * dz > rangeSq) continue;

						const CollisionBVH::Float3 e1 = tri.v1 - tri.v0;
						const CollisionBVH::Float3 e2 = tri.v2 - tri.v0;
						const CollisionBVH::Float3 n = CollisionBVH::cross(e1, e2);
						const float len = std::sqrt(CollisionBVH::dot(n, n));
						if (len < 1e-6f) continue;
						if (std::abs(n.y / len) > 0.65f) continue;

						lines.push_back({ tri.v0.x, tri.v0.z, tri.v1.x, tri.v1.z });
						lines.push_back({ tri.v1.x, tri.v1.z, tri.v2.x, tri.v2.z });
						lines.push_back({ tri.v2.x, tri.v2.z, tri.v0.x, tri.v0.z });
						if (++count > 1200) break;
					}
				}

				const std::string body = lines.dump();
				const std::string header =
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/json\r\n"
					"Content-Length: " + std::to_string(body.size()) + "\r\n"
					"Access-Control-Allow-Origin: *\r\n"
					"Connection: close\r\n\r\n";
				if (SendAll(client, header.data(), header.size()))
					SendAll(client, body.data(), body.size());
			}
			// Cached map image binary (/maps/<filename>)
			else if (path.rfind("/maps/", 0) == 0) {
				std::string filename = path.substr(6);
				const auto q = filename.find('?');
				if (q != std::string::npos) filename.resize(q);
				filename = UrlDecode(filename);

				bool allowed = !filename.empty() &&
					filename.find("..") == std::string::npos &&
					filename.find('/') == std::string::npos &&
					filename.find('\\') == std::string::npos &&
					filename.find(':') == std::string::npos &&
					filename.find('\0') == std::string::npos;
				if (allowed) {
					const auto& knownMaps = Radar::GetMapFiles();
					allowed = std::find(knownMaps.begin(), knownMaps.end(), filename) != knownMaps.end();
				}

				std::shared_ptr<const std::vector<char>> data;
				if (allowed) {
					{
						std::lock_guard<std::mutex> lk(g_mapCacheMutex);
						auto it = g_mapFileCache.find(filename);
						if (it != g_mapFileCache.end())
							data = it->second;
					}
					if (!data) {
						const std::filesystem::path fullPath = Radar::GetMapsDirectory() / filename;
						std::ifstream f(fullPath, std::ios::binary | std::ios::ate);
						if (f.is_open()) {
							const std::streamoff rawSize = f.tellg();
							constexpr std::streamoff kMaxMapBytes = 32 * 1024 * 1024;
							if (rawSize > 0 && rawSize <= kMaxMapBytes) {
								f.seekg(0, std::ios::beg);
								auto buf = std::make_shared<std::vector<char>>(static_cast<std::size_t>(rawSize));
								if (f.read(buf->data(), rawSize)) {
									std::lock_guard<std::mutex> lk(g_mapCacheMutex);
									g_mapFileCache[filename] = buf;
									data = buf;
								}
							}
						}
					}
				}

				if (data && !data->empty()) {
					const std::string header =
						"HTTP/1.1 200 OK\r\n"
						"Content-Type: image/png\r\n"
						"Content-Length: " + std::to_string(data->size()) + "\r\n"
						"Cache-Control: public, max-age=3600\r\n"
						"Access-Control-Allow-Origin: *\r\n"
						"Connection: close\r\n\r\n";
					if (SendAll(client, header.data(), header.size()))
						SendAll(client, data->data(), data->size());
				} else {
					const std::string nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
					SendAll(client, nf.data(), nf.size());
				}
			} else {
				const std::string nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
				SendAll(client, nf.data(), nf.size());
			}
		} catch (...) {
			const std::string errResp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			SendAll(client, errResp.data(), errResp.size());
		}

		closesocket(client);
	}

	CloseListenSocket();
}

} // namespace

void Init()
{
	if (ConfigInstance.Radar.WebRadarEnabled) {
		Start(ConfigInstance.Radar.WebRadarPort);
	}
}

bool Start(int port)
{
	Stop();
	if (port <= 0 || port > 65535) port = 8888;
	g_serverPort = port;
	g_serverRunning.store(true);
	g_serverThread = std::thread(RunServerLoop, port);
	return true;
}

void Stop()
{
	g_serverRunning.store(false);
	CloseListenSocket();
	if (g_serverThread.joinable()) {
		g_serverThread.join();
	}
	// Give detached SSE stream threads a brief window to notice g_serverRunning == false
	for (int i = 0; i < 20 && g_activeStreams.load(std::memory_order_relaxed) > 0; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
	}
}

bool IsRunning()
{
	return g_serverRunning.load();
}

int GetPort()
{
	return g_serverPort;
}

std::string GetUrl()
{
	return "http://localhost:" + std::to_string(g_serverPort);
}

void OpenInBrowser()
{
	if (!IsRunning()) {
		Start(ConfigInstance.Radar.WebRadarPort);
	}
	const std::string url = GetUrl();
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace WebRadar
