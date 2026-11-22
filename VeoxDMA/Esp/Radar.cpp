#include "pch.h"
#include "Radar.h"
#include "MapSpriteData.h"
#include "EspDraw.h"
#include "IconSprites.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "../Overlay/Overlay.h"
#include "../ImGUI/imgui.h"
#include <d3d11.h>
#include <wincodec.h>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

ID3D11ShaderResourceView* g_mapSRV = nullptr;
int g_mapWidth = 0;
int g_mapHeight = 0;

std::vector<std::string> g_mapFiles;
int g_currentMapIndex = -1;
std::filesystem::path g_mapsDir;
std::atomic<int> g_pendingMapIndex{ -1 };
std::mutex g_radarStateMutex;
bool g_comInitialized = false;

enum class MapSpriteSlot : int {
	Weapon = 0,
	Armor,
	Valuable,
	Cash,
	Safe,
	Cargo,
	Medicine,
	Key,
	Generic,
	DroppedBag,
	Car,
	PoliceCar,
	Helicopter,
	PlayerArrow,
	COUNT
};
struct MapSpritePair {
	ID3D11ShaderResourceView* base = nullptr; // full silhouette (drawn with untinted fuser-safe charcoal)
	ID3D11ShaderResourceView* fill = nullptr; // interior fill (drawn with dynamic role/rarity tint)
};
MapSpritePair g_mapIconSRVs[static_cast<int>(MapSpriteSlot::COUNT)] = {};

ID3D11ShaderResourceView* DecodeEmbeddedPng(const std::uint8_t* pngData, std::size_t pngSize, ID3D11Device* device)
{
	if (!pngData || !pngSize || !device) return nullptr;
	IWICImagingFactory* factory = nullptr;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory)
		return nullptr;

	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, pngSize);
	if (!hMem) { factory->Release(); return nullptr; }
	void* pMem = GlobalLock(hMem);
	if (!pMem) { GlobalFree(hMem); factory->Release(); return nullptr; }
	std::memcpy(pMem, pngData, pngSize);
	GlobalUnlock(hMem);

	IStream* stream = nullptr;
	if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream)) || !stream) {
		GlobalFree(hMem);
		factory->Release();
		return nullptr;
	}

	IWICBitmapDecoder* decoder = nullptr;
	if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder) {
		stream->Release(); factory->Release(); return nullptr;
	}
	IWICBitmapFrameDecode* frame = nullptr;
	if (FAILED(decoder->GetFrame(0, &frame)) || !frame) {
		decoder->Release(); stream->Release(); factory->Release(); return nullptr;
	}
	IWICFormatConverter* conv = nullptr;
	if (FAILED(factory->CreateFormatConverter(&conv)) || !conv) {
		frame->Release(); decoder->Release(); stream->Release(); factory->Release(); return nullptr;
	}
	if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
		conv->Release(); frame->Release(); decoder->Release(); stream->Release(); factory->Release(); return nullptr;
	}
	UINT w = 0, h = 0;
	if (FAILED(conv->GetSize(&w, &h)) || w == 0 || h == 0 || w > 1024 || h > 1024) {
		conv->Release(); frame->Release(); decoder->Release(); stream->Release(); factory->Release(); return nullptr;
	}
	std::vector<BYTE> pixels(static_cast<std::size_t>(w) * h * 4);
	const HRESULT copyHr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
	conv->Release(); frame->Release(); decoder->Release(); stream->Release(); factory->Release();
	if (FAILED(copyHr)) return nullptr;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init{};
	init.pSysMem = pixels.data();
	init.SysMemPitch = w * 4;

	ID3D11Texture2D* tex = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, &init, &tex)) || !tex) return nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	device->CreateShaderResourceView(tex, nullptr, &srv);
	tex->Release();
	return srv;
}

void ReleaseMapSprites()
{
	for (auto& sp : g_mapIconSRVs) {
		if (sp.base) { sp.base->Release(); sp.base = nullptr; }
		if (sp.fill) { sp.fill->Release(); sp.fill = nullptr; }
	}
}

void InitMapSprites(ID3D11Device* device)
{
	ReleaseMapSprites();
	if (!device) return;
	auto load = [&](MapSpriteSlot slot,
		const std::uint8_t* baseData, std::size_t baseSz,
		const std::uint8_t* fillData, std::size_t fillSz) {
		auto& dst = g_mapIconSRVs[static_cast<int>(slot)];
		dst.base = DecodeEmbeddedPng(baseData, baseSz, device);
		dst.fill = DecodeEmbeddedPng(fillData, fillSz, device);
	};
	load(MapSpriteSlot::Weapon,      MapSpriteData::kMapPngWeaponBase,      sizeof(MapSpriteData::kMapPngWeaponBase),      MapSpriteData::kMapPngWeaponFill,      sizeof(MapSpriteData::kMapPngWeaponFill));
	load(MapSpriteSlot::Armor,       MapSpriteData::kMapPngArmorBase,       sizeof(MapSpriteData::kMapPngArmorBase),       MapSpriteData::kMapPngArmorFill,       sizeof(MapSpriteData::kMapPngArmorFill));
	load(MapSpriteSlot::Valuable,    MapSpriteData::kMapPngValuableBase,    sizeof(MapSpriteData::kMapPngValuableBase),    MapSpriteData::kMapPngValuableFill,    sizeof(MapSpriteData::kMapPngValuableFill));
	load(MapSpriteSlot::Cash,        MapSpriteData::kMapPngCashBase,        sizeof(MapSpriteData::kMapPngCashBase),        MapSpriteData::kMapPngCashFill,        sizeof(MapSpriteData::kMapPngCashFill));
	load(MapSpriteSlot::Safe,        MapSpriteData::kMapPngSafeBase,        sizeof(MapSpriteData::kMapPngSafeBase),        MapSpriteData::kMapPngSafeFill,        sizeof(MapSpriteData::kMapPngSafeFill));
	load(MapSpriteSlot::Cargo,       MapSpriteData::kMapPngCargoBase,       sizeof(MapSpriteData::kMapPngCargoBase),       MapSpriteData::kMapPngCargoFill,       sizeof(MapSpriteData::kMapPngCargoFill));
	load(MapSpriteSlot::Medicine,    MapSpriteData::kMapPngMedicineBase,    sizeof(MapSpriteData::kMapPngMedicineBase),    MapSpriteData::kMapPngMedicineFill,    sizeof(MapSpriteData::kMapPngMedicineFill));
	load(MapSpriteSlot::Key,         MapSpriteData::kMapPngKeyBase,         sizeof(MapSpriteData::kMapPngKeyBase),         MapSpriteData::kMapPngKeyFill,         sizeof(MapSpriteData::kMapPngKeyFill));
	load(MapSpriteSlot::Generic,     MapSpriteData::kMapPngGenericLootBase, sizeof(MapSpriteData::kMapPngGenericLootBase), MapSpriteData::kMapPngGenericLootFill, sizeof(MapSpriteData::kMapPngGenericLootFill));
	load(MapSpriteSlot::DroppedBag,  MapSpriteData::kMapPngDroppedBagBase,  sizeof(MapSpriteData::kMapPngDroppedBagBase),  MapSpriteData::kMapPngDroppedBagFill,  sizeof(MapSpriteData::kMapPngDroppedBagFill));
	load(MapSpriteSlot::Car,         MapSpriteData::kMapPngCarBase,         sizeof(MapSpriteData::kMapPngCarBase),         MapSpriteData::kMapPngCarFill,         sizeof(MapSpriteData::kMapPngCarFill));
	load(MapSpriteSlot::PoliceCar,   MapSpriteData::kMapPngPoliceCarBase,   sizeof(MapSpriteData::kMapPngPoliceCarBase),   MapSpriteData::kMapPngPoliceCarFill,   sizeof(MapSpriteData::kMapPngPoliceCarFill));
	load(MapSpriteSlot::Helicopter,  MapSpriteData::kMapPngHelicopterBase,  sizeof(MapSpriteData::kMapPngHelicopterBase),  MapSpriteData::kMapPngHelicopterFill,  sizeof(MapSpriteData::kMapPngHelicopterFill));
	load(MapSpriteSlot::PlayerArrow, MapSpriteData::kMapPngPlayerArrowBase, sizeof(MapSpriteData::kMapPngPlayerArrowBase), MapSpriteData::kMapPngPlayerArrowFill, sizeof(MapSpriteData::kMapPngPlayerArrowFill));
}

bool DrawMapSprite(ImDrawList* dl, MapSpriteSlot slot, const ImVec2& c, float half, ImU32 fillCol)
{
	const int idx = static_cast<int>(slot);
	if (idx < 0 || idx >= static_cast<int>(MapSpriteSlot::COUNT)) return false;
	const auto& sp = g_mapIconSRVs[idx];
	if (!sp.base || !sp.fill) return false;
	const int alpha = static_cast<int>((fillCol >> IM_COL32_A_SHIFT) & 0xFF);
	const ImU32 outlineCol = IM_COL32(28, 30, 36, std::min(255, alpha + 25));
	const ImVec2 p0(c.x - half, c.y - half);
	const ImVec2 p1(c.x + half, c.y + half);
	dl->AddImage(reinterpret_cast<ImTextureID>(sp.base), p0, p1, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), outlineCol);
	dl->AddImage(reinterpret_cast<ImTextureID>(sp.fill), p0, p1, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), fillCol);
	return true;
}

bool DrawMapSpriteRotated(ImDrawList* dl, MapSpriteSlot slot, const ImVec2& c, float half, float relYaw, ImU32 fillCol)
{
	const int idx = static_cast<int>(slot);
	if (idx < 0 || idx >= static_cast<int>(MapSpriteSlot::COUNT)) return false;
	const auto& sp = g_mapIconSRVs[idx];
	if (!sp.base || !sp.fill) return false;
	const int alpha = static_cast<int>((fillCol >> IM_COL32_A_SHIFT) & 0xFF);
	const ImU32 outlineCol = IM_COL32(28, 30, 36, std::min(255, alpha + 25));

	// Sprite points UP at relYaw = 0 (fwd = (sin, -cos), right = (cos, sin))
	const float cs = std::cos(relYaw);
	const float sn = std::sin(relYaw);
	auto rot = [&](float lx, float ly) -> ImVec2 {
		return ImVec2(c.x + lx * cs - ly * sn, c.y + lx * sn + ly * cs);
	};
	const ImVec2 p1 = rot(-half, -half); // top-left
	const ImVec2 p2 = rot( half, -half); // top-right
	const ImVec2 p3 = rot( half,  half); // bottom-right
	const ImVec2 p4 = rot(-half,  half); // bottom-left
	const ImVec2 uv1(0.f, 0.f), uv2(1.f, 0.f), uv3(1.f, 1.f), uv4(0.f, 1.f);
	dl->AddImageQuad(reinterpret_cast<ImTextureID>(sp.base), p1, p2, p3, p4, uv1, uv2, uv3, uv4, outlineCol);
	dl->AddImageQuad(reinterpret_cast<ImTextureID>(sp.fill), p1, p2, p3, p4, uv1, uv2, uv3, uv4, fillCol);
	return true;
}

bool LoadTextureFromFile(const wchar_t* path, ID3D11Device* device)
{
	if (g_mapSRV) { g_mapSRV->Release(); g_mapSRV = nullptr; }
	g_mapWidth = 0;
	g_mapHeight = 0;

	IWICImagingFactory* factory = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (FAILED(hr)) return false;

	IWICBitmapDecoder* decoder = nullptr;
	hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(hr)) { factory->Release(); return false; }

	IWICBitmapFrameDecode* frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr)) { decoder->Release(); factory->Release(); return false; }

	IWICFormatConverter* converter = nullptr;
	hr = factory->CreateFormatConverter(&converter);
	if (FAILED(hr)) { frame->Release(); decoder->Release(); factory->Release(); return false; }

	hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut);
	if (FAILED(hr)) { converter->Release(); frame->Release(); decoder->Release(); factory->Release(); return false; }

	UINT w = 0, h = 0;
	hr = converter->GetSize(&w, &h);
	if (FAILED(hr) || w == 0 || h == 0 || w > 16384 || h > 16384) {
		converter->Release();
		frame->Release();
		decoder->Release();
		factory->Release();
		return false;
	}

	const std::size_t pixelBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
	std::vector<BYTE> pixels(pixelBytes);
	hr = converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
	converter->Release();
	frame->Release();
	decoder->Release();
	factory->Release();
	if (FAILED(hr)) return false;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA sub{};
	sub.pSysMem = pixels.data();
	sub.SysMemPitch = w * 4;

	ID3D11Texture2D* tex = nullptr;
	hr = device->CreateTexture2D(&desc, &sub, &tex);
	if (FAILED(hr)) return false;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	hr = device->CreateShaderResourceView(tex, &srvDesc, &g_mapSRV);
	tex->Release();
	if (SUCCEEDED(hr)) {
		g_mapWidth = (int)w;
		g_mapHeight = (int)h;
	}
	return SUCCEEDED(hr);
}

void ScanMapFiles()
{
	g_mapFiles.clear();
	if (g_mapsDir.empty() || !std::filesystem::exists(g_mapsDir))
		return;

	for (const auto& entry : std::filesystem::directory_iterator(g_mapsDir)) {
		if (!entry.is_regular_file()) continue;
		auto ext = entry.path().extension().string();
		for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp")
			g_mapFiles.push_back(entry.path().filename().string());
	}
	std::sort(g_mapFiles.begin(), g_mapFiles.end());
}

bool LoadMapByName(const std::string& filename)
{
	auto* device = Overlay::GetDevice();
	if (!device) return false;

	std::filesystem::path fullPath = g_mapsDir / filename;
	if (!std::filesystem::exists(fullPath)) {
		printf("[Radar] Map file not found: %s\n", fullPath.string().c_str());
		return false;
	}

	std::wstring wpath = fullPath.wstring();
	if (LoadTextureFromFile(wpath.c_str(), device)) {
		printf("[Radar] Loaded map: %s (%dx%d)\n", filename.c_str(), g_mapWidth, g_mapHeight);
		return true;
	}
	printf("[Radar] Failed to load map: %s\n", filename.c_str());
	return false;
}

ImVec2 WorldToRadar(const Vector3& world, const Vector3& center,
	float yaw, bool rotate, float radarSize, const ImVec2& radarCenter, float zoom)
{
	const auto& cfg = ConfigInstance.Radar;
	float worldW = cfg.WorldMaxX - cfg.WorldMinX;
	float worldH = cfg.WorldMaxZ - cfg.WorldMinZ;
	if (worldW == 0.f) worldW = 1.f;
	if (worldH == 0.f) worldH = 1.f;

	float dx = world.x - center.x;
	float dz = world.z - center.z;

	float plotX = dx;
	float plotZ = dz;

	if (rotate) {
		float fwd = dx * std::sin(yaw) + dz * std::cos(yaw);
		float right = dx * std::cos(yaw) - dz * std::sin(yaw);
		plotX = right;
		plotZ = fwd;
	}

	float scale = radarSize / std::max(worldW, worldH) * zoom;
	return ImVec2(radarCenter.x + plotX * scale, radarCenter.y - plotZ * scale);
}

struct RoleMarker {
	const char* text;
	ImU32 color;
};

RoleMarker GetRoleMarker(int role, const std::string& roleName)
{
	const auto& pcfg = ConfigInstance.Player;
	const std::string label = RobRunReader::RoleLabel(role, roleName);

	if (label == "Criminal")
		return { "C", Esp::D2DColorToImU32(pcfg.CriminalColour) };
	if (label == "Police Officer")
		return { "P", Esp::D2DColorToImU32(pcfg.PoliceColour) };
	if (label == "Civilian")
		return { "CI", Esp::D2DColorToImU32(pcfg.CivilianColour) };
	if (label == "Hubber")
		return { "CI", Esp::D2DColorToImU32(pcfg.HubberColour) };

	return { "CI", IM_COL32(220, 220, 100, 255) };
}

} // namespace

namespace Radar {

void Init()
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	g_comInitialized = (hr == S_OK || hr == S_FALSE);

	InitMapSprites(Overlay::GetDevice());

	wchar_t exePath[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
	g_mapsDir = exeDir / L"maps";

	if (!std::filesystem::exists(g_mapsDir))
		std::filesystem::create_directories(g_mapsDir);

	ScanMapFiles();

	const auto& selected = ConfigInstance.Radar.SelectedMap;
	g_currentMapIndex = -1;
	for (int i = 0; i < (int)g_mapFiles.size(); i++) {
		if (g_mapFiles[i] == selected) {
			g_currentMapIndex = i;
			break;
		}
	}

	if (g_currentMapIndex >= 0) {
		LoadMapByName(g_mapFiles[g_currentMapIndex]);
	} else if (!g_mapFiles.empty()) {
		g_currentMapIndex = 0;
		ConfigInstance.Radar.SelectedMap = g_mapFiles[0];
		LoadMapByName(g_mapFiles[0]);
	} else {
		printf("[Radar] No map files found in maps/ folder\n");
	}
}

struct LootRenderInfo {
	bool show = false;
	ImU32 colour = IM_COL32(255, 255, 255, 255);
	float dotSize = 3.5f;
	bool hasHighValueRing = false;
	bool showDot = true;
	LootFilter::Category category = LootFilter::Category::Misc;
	ImU32 textColour = IM_COL32(235, 235, 235, 240);
	std::string icon;             // catalogue sprite stem, empty when the row has none
	std::string text;
};

static bool EvaluateLootEntity(const RobRunWorld::Entity& ent, const Vector3& localPos, bool haveLocalPos, LootRenderInfo& out)
{
	const auto& lf = ConfigInstance.LootFilter;

	// Vehicles have their own radar toggle when the filter is off; with the
	// filter on they resolve through the catalogue like any other entity.
	const bool isVehicle = ent.kind == RobRunWorld::Kind::Car ||
		ent.kind == RobRunWorld::Kind::Helicopter ||
		ent.kind == RobRunWorld::Kind::Vehicle;
	if (!lf.Enabled && isVehicle)
		return false;

	if (!lf.Enabled) {
		// Fallback to legacy Radar/World toggles when LootFilter is disabled
		bool show = false;
		ImU32 dotCol = IM_COL32(180, 255, 180, 160);
		float dotSize = 2.5f;
		switch (ent.kind) {
		case RobRunWorld::Kind::DroppedBag:
			show = ConfigInstance.Radar.ShowDroppedBags;
			dotCol = IM_COL32(120, 255, 140, 220);
			dotSize = 3.5f;
			break;
		case RobRunWorld::Kind::CashLootNode:
		case RobRunWorld::Kind::SettlementCash:
		case RobRunWorld::Kind::MoneyWorkbench:
			show = ConfigInstance.Radar.ShowValuables;
			dotCol = IM_COL32(255, 215, 0, 230);
			dotSize = 4.f;
			break;
		case RobRunWorld::Kind::SafeDoor:
		case RobRunWorld::Kind::Container:
		case RobRunWorld::Kind::LootNode:
		case RobRunWorld::Kind::StorageBox:
		case RobRunWorld::Kind::Shelving:
		case RobRunWorld::Kind::CollectionTable:
		case RobRunWorld::Kind::PlotContainer:
		case RobRunWorld::Kind::Cargo:
		case RobRunWorld::Kind::CargoBush:
		case RobRunWorld::Kind::CargoCrystal:
		case RobRunWorld::Kind::CargoDrug:
		case RobRunWorld::Kind::CargoCash:
		case RobRunWorld::Kind::CargoExchange:
		case RobRunWorld::Kind::CargoBox:
		case RobRunWorld::Kind::CargoGoods:
		case RobRunWorld::Kind::VehicleTrunk:
			show = ConfigInstance.Radar.ShowContainers;
			dotCol = IM_COL32(255, 190, 80, 180);
			dotSize = 3.f;
			break;
		case RobRunWorld::Kind::DrugWorkbench:
			show = true;
			dotCol = IM_COL32(0, 220, 100, 230);
			dotSize = 4.5f;
			break;
		case RobRunWorld::Kind::DefenseBlock:
			show = ConfigInstance.World.DefenseBlocks;
			dotCol = IM_COL32(255, 80, 80, 230);
			dotSize = 4.f;
			break;
		default:
			show = ConfigInstance.Radar.ShowLoot;
			dotCol = IM_COL32(180, 255, 180, 160);
			dotSize = 2.5f;
			break;
		}
		if (!show) return false;
		out.show = true;
		out.colour = dotCol;
		out.dotSize = dotSize;
		out.hasHighValueRing = false;
		out.text = ent.label;
		return true;
	}

	// 1. Resolve the catalog entry and its per-item settings
	const LootFilter::ItemDef* itemDef = lf.FindItemForEntity(ent.kind, ent.label);
	if (!itemDef)
		return false;

	const ItemSettings& item = lf.GetItem(itemDef->id);
	if (!item.visible)
		return false;

	// 2. Distance check — per-item limit clamped by the global limit (0 = unlimited)
	float distance = 0.f;
	if (haveLocalPos) {
		const float dx = ent.position.x - localPos.x;
		const float dy = ent.position.y - localPos.y;
		const float dz = ent.position.z - localPos.z;
		distance = std::sqrt(dx * dx + dy * dy + dz * dz);

		int maxDistance = item.useGlobalDistance ? lf.GlobalMaxDistance : item.maxDistance;
		if (maxDistance <= 0) maxDistance = (std::max)(lf.GlobalMaxDistance, 0);
		const int catMax = lf.ResolvedMaxDistance(itemDef->category);
		if (catMax > 0 && (item.useGlobalDistance || item.maxDistance <= 0))
			maxDistance = catMax;
		if (lf.GlobalMaxDistance > 0)
			maxDistance = (maxDistance > 0)
				? (std::min)(maxDistance, lf.GlobalMaxDistance)
				: lf.GlobalMaxDistance;
		if (maxDistance > 0 && distance > (float)maxDistance)
			return false;
	}

	// 3. Empty container check - only rows that opted into HideEmpty
	if (lf.HideEmpty && itemDef->hideWhenEmpty && ent.hasEmptyFlag && ent.empty)
		return false;

	// 4. Base colour and dot size from the per-item settings
	ImU32 finalCol = ColorToImU32(item.color);
	float baseSize = lf.ResolvedDotSize(itemDef->category);

	// 5. Price filter
	if (lf.FilterByMinPrice && lf.MinPrice > 0) {
		if (ent.hasCashAmount) {
			if (ent.cashAmount < lf.MinPrice)
				return false;
		} else {
			if (lf.MinPrice > 0)
				return false;
		}
	}

	// 6. Item search query
	if (!lf.SearchQuery.empty()) {
		std::string hay = ent.label;
		std::string needle = lf.SearchQuery;
		std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (hay.find(needle) == std::string::npos)
			return false;
	}

	// 7. Value tiering & highlighting
	bool highValueRing = false;

	if (ent.hasCashAmount) {
		if (ent.cashAmount >= lf.HighValueThreshold) {
			finalCol = ColorToImU32(lf.HighValueColour);
			baseSize += 1.5f;
			highValueRing = lf.HighValuePulse;
		} else if (ent.cashAmount >= lf.MidValueThreshold) {
			finalCol = ColorToImU32(lf.MidValueColour);
			baseSize += 0.8f;
		}
	}

	// 8. Label text, gated by the per-item render flags
	std::string text;
	const bool wantName  = item.Has(LootFilter::RenderName);
	const bool wantPrice = item.Has(LootFilter::RenderPrice);
	const bool wantDist  = item.Has(LootFilter::RenderDistance);
	if (wantName || wantDist) {
		if (wantName)
			text = ent.label;
		if (wantPrice && ent.hasCashAmount) {
			if (!text.empty())
				text += " | ";
			text += "$" + FormatCash(ent.cashAmount);
			if (ent.kind == RobRunWorld::Kind::MoneyWorkbench && ent.cleanCash > 0 && ent.dirtyCash > 0) {
				text += " (C:$" + FormatCash(ent.cleanCash) + " D:$" + FormatCash(ent.dirtyCash) + ")";
			}
		} else if (wantName && !lf.HideEmpty && ent.hasEmptyFlag) {
			text += ent.empty ? " [empty]" : " [loot]";
		}
		if (wantDist && haveLocalPos) {
			if (!text.empty()) text += " ";
			text += "[" + std::to_string((int)distance) + "m]";
		}
	}

	out.show = true;
	out.colour = finalCol;
	out.dotSize = baseSize;
	out.hasHighValueRing = highValueRing;
	out.showDot = item.Has(LootFilter::RenderDot);
	out.category = itemDef->category;
	out.textColour = RarityColorU32(itemDef->rarity);
	out.icon.clear();
	if (item.Has(LootFilter::RenderIcon)) {
		// Vehicles label themselves with their exact model+paint id.
		if (isVehicle && !ent.label.empty() && IconSprites::Get(ent.label.c_str()).srv)
			out.icon = ent.label;
		else if (itemDef->iconSprite[0])
			out.icon = itemDef->iconSprite;
	}
	out.text = std::move(text);
	return true;
}

void Draw()
{
	if (!ConfigInstance.Radar.Enabled) return;

	const auto snapshot = RobRunReader::GetSnapshot();
	const Vector3 localPos = snapshot ? snapshot->localPosition : Vector3{};
	const bool haveLocal = snapshot && !localPos.IsZero();

	float localYaw = 0.f;
	if (snapshot) {
		if (std::isfinite(snapshot->localYaw) && snapshot->localYaw != 0.f) {
			localYaw = snapshot->localYaw;
		}
		if (localYaw == 0.f) {
			for (const auto& ent : snapshot->entities) {
				if (ent.entityId == snapshot->localEntityId) {
					localYaw = ent.yaw;
					break;
				}
			}
		}
		if (localYaw == 0.f && snapshot->camera.valid) {
			localYaw = std::atan2(snapshot->camera.forward.x, snapshot->camera.forward.z);
		}
	}

	const auto& cfg = ConfigInstance.Radar;
	const float radarSize = cfg.Size;

	// Restore the last on-screen placement, defaulting to the top-left corner
	// where the in-game map is situated.
	const WindowPlacement& savedPlace = ConfigInstance.Overlay.RadarWindow;
	static float s_lastSizeSetting = -1.f;
	if (savedPlace.Saved)
	{
		ImGui::SetNextWindowPos(ImVec2(savedPlace.X, savedPlace.Y), ImGuiCond_FirstUseEver);
		if (savedPlace.Width > 50.f && savedPlace.Height > 50.f)
			ImGui::SetNextWindowSize(ImVec2(savedPlace.Width, savedPlace.Height),
				ImGuiCond_FirstUseEver);
		else
			ImGui::SetNextWindowSize(ImVec2(radarSize + 16.f, radarSize + 36.f),
				ImGuiCond_FirstUseEver);
	}
	else
	{
		ImGui::SetNextWindowPos(ImVec2(10.f, 10.f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(radarSize + 16.f, radarSize + 36.f),
			ImGuiCond_FirstUseEver);
	}
	if (s_lastSizeSetting >= 0.f && s_lastSizeSetting != cfg.Size)
		ImGui::SetNextWindowSize(ImVec2(radarSize + 16.f, radarSize + 36.f), ImGuiCond_Always);
	s_lastSizeSetting = cfg.Size;

	const float op = std::clamp(cfg.Opacity, 0.0f, 1.0f);
	const int bgAlpha = (int)(op * 220.f);
	const int borderAlpha = std::clamp((int)(op * 140.f + 40.f), 0, 255);
	const int titleAlpha = std::clamp((int)(op * 200.f + 55.f), 20, 255);
	const int titleActiveAlpha = std::clamp((int)(op * 220.f + 35.f), 30, 255);

	ImGui::SetNextWindowBgAlpha(op);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 12, 18, bgAlpha));
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(40, 80, 160, borderAlpha));
	ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(18, 20, 30, titleAlpha));
	ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(30, 120, 255, titleActiveAlpha));

	if (!ImGui::Begin("Radar", &ConfigInstance.Radar.Enabled,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar(2);
		return;
	}

	// Track the live placement so it is saved with the rest of the config.
	{
		WindowPlacement& place = ConfigInstance.Overlay.RadarWindow;
		const ImVec2 wp = ImGui::GetWindowPos();
		const ImVec2 ws = ImGui::GetWindowSize();
		place.Saved = true;
		place.X = wp.x;
		place.Y = wp.y;
		place.Width = ws.x;
		place.Height = ws.y;
	}

	ImVec2 contentPos = ImGui::GetCursorScreenPos();
	ImVec2 contentSize = ImGui::GetContentRegionAvail();
	float side = std::min(contentSize.x, contentSize.y);
	if (side < 50.f) side = 50.f;

	ImVec2 radarMin = contentPos;
	ImVec2 radarMax = ImVec2(contentPos.x + side, contentPos.y + side);
	ImVec2 radarCenter = ImVec2(contentPos.x + side * 0.5f, contentPos.y + side * 0.5f);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(radarMin, radarMax, true);

	const bool drawMapTexture = (g_mapSRV != nullptr) && cfg.ShowMap;
	if (drawMapTexture) {
		const float worldW = cfg.WorldMaxX - cfg.WorldMinX;
		const float worldH = cfg.WorldMaxZ - cfg.WorldMinZ;
		const bool validBounds = worldW > 0.f && worldH > 0.f && cfg.Zoom > 0.f;
		if (cfg.RotateWithCamera && haveLocal && validBounds) {
			float scale = side / std::max(worldW, worldH) * cfg.Zoom;

			float normX = (localPos.x - cfg.WorldMinX) / worldW;
			float normZ = (localPos.z - cfg.WorldMinZ) / worldH;

			float halfViewU = (side / (2.f * scale)) / worldW;
			float halfViewV = (side / (2.f * scale)) / worldH;

			ImVec2 uvA(normX - halfViewU, normZ - halfViewV);
			ImVec2 uvB(normX + halfViewU, normZ + halfViewV);

			float s = std::sin(-localYaw);
			float c = std::cos(-localYaw);

			auto rotatePoint = [&](float ox, float oy) -> ImVec2 {
				float rx = ox * c - oy * s;
				float ry = ox * s + oy * c;
				return ImVec2(radarCenter.x + rx, radarCenter.y + ry);
			};

			float hs = side * 0.5f * 1.42f;
			ImVec2 p1 = rotatePoint(-hs, -hs);
			ImVec2 p2 = rotatePoint(hs, -hs);
			ImVec2 p3 = rotatePoint(hs, hs);
			ImVec2 p4 = rotatePoint(-hs, hs);

			dl->AddImageQuad((ImTextureID)g_mapSRV,
				p1, p2, p3, p4,
				uvA, ImVec2(uvB.x, uvA.y), uvB, ImVec2(uvA.x, uvB.y),
				IM_COL32(255, 255, 255, (int)(op * 255.f)));
		}
		else {
			if (haveLocal && validBounds) {
				float scale = side / std::max(worldW, worldH) * cfg.Zoom;

				float normX = (localPos.x - cfg.WorldMinX) / worldW;
				float normZ = (localPos.z - cfg.WorldMinZ) / worldH;

				float halfViewU = (side / (2.f * scale)) / worldW;
				float halfViewV = (side / (2.f * scale)) / worldH;

				ImVec2 uvA(normX - halfViewU, normZ - halfViewV);
				ImVec2 uvB(normX + halfViewU, normZ + halfViewV);

				dl->AddImage((ImTextureID)g_mapSRV, radarMin, radarMax, uvA, uvB,
					IM_COL32(255, 255, 255, (int)(op * 255.f)));
			}
			else {
				dl->AddImage((ImTextureID)g_mapSRV, radarMin, radarMax,
					ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
					IM_COL32(255, 255, 255, (int)(op * 255.f)));
			}
		}
	}
	else {
		// Clean transparent/dark backing with range rings so the radar functions
		// seamlessly whether overlaid on the in-game minimap or running standalone.
		const int circleBgAlpha = (int)(op * 180.f);
		if (circleBgAlpha > 0) {
			dl->AddCircleFilled(radarCenter, side * 0.5f, IM_COL32(14, 16, 24, circleBgAlpha));
		}
		const int gridAlpha = (int)(op * 50.f + 25.f);
		dl->AddCircle(radarCenter, side * 0.5f * 0.66f, IM_COL32(50, 90, 160, gridAlpha), 64, 1.f);
		dl->AddCircle(radarCenter, side * 0.5f * 0.33f, IM_COL32(50, 90, 160, gridAlpha), 64, 1.f);
		dl->AddLine(ImVec2(radarCenter.x, radarCenter.y - side * 0.5f),
			ImVec2(radarCenter.x, radarCenter.y + side * 0.5f), IM_COL32(50, 90, 160, gridAlpha), 1.f);
		dl->AddLine(ImVec2(radarCenter.x - side * 0.5f, radarCenter.y),
			ImVec2(radarCenter.x + side * 0.5f, radarCenter.y), IM_COL32(50, 90, 160, gridAlpha), 1.f);
	}

	const int ringBorderAlpha = std::clamp((int)(op * 120.f + 60.f), 60, 255);
	dl->AddCircle(radarCenter, side * 0.5f, IM_COL32(40, 80, 160, ringBorderAlpha), 64, 2.f);

	if (haveLocal && snapshot) {
		if (snapshot->world && snapshot->world->valid) {
			for (const auto& ent : snapshot->world->entities) {
				bool show = false;
				ImU32 dotCol = IM_COL32(255, 255, 255, 200);
				float dotSize = 3.f;

				const bool isVehicle = ent.kind == RobRunWorld::Kind::Car ||
					ent.kind == RobRunWorld::Kind::Helicopter ||
					ent.kind == RobRunWorld::Kind::Vehicle;

				if (ConfigInstance.LootFilter.Enabled) {
					// One switchboard: vehicles resolve through the catalogue too.
					if (ConfigInstance.LootFilter.ShowOnMinimap) {
						LootRenderInfo lootInfo;
						if (EvaluateLootEntity(ent, localPos, haveLocal, lootInfo)) {
							show = true;
							dotCol = lootInfo.colour;
							dotSize = lootInfo.dotSize;
						}
					}
				} else if (isVehicle) {
					if (cfg.ShowVehicles) {
						show = true;
						dotCol = IM_COL32(120, 190, 255, 230);
						dotSize = 4.f;
					}
				} else if (cfg.ShowLoot) {
					show = true;
					dotCol = IM_COL32(255, 190, 80, 180);
					dotSize = 2.5f;
				}

				if (!show) continue;

				ImVec2 pos = WorldToRadar(ent.position, localPos, localYaw,
					cfg.RotateWithCamera, side, radarCenter, cfg.Zoom);

				float ddx = pos.x - radarCenter.x;
				float ddy = pos.y - radarCenter.y;
				if (ddx * ddx + ddy * ddy > (side * 0.5f) * (side * 0.5f)) continue;

				dl->AddCircleFilled(pos, dotSize, dotCol);
			}
		}

		if (cfg.ShowPlayers) {
			for (const auto& ent : snapshot->entities) {
				if (ent.entityId == snapshot->localEntityId) continue;
				if (ent.isDead) continue;
				// All players allowed on radar; party members receive a distinct friendly perimeter
				const RoleMarker marker = GetRoleMarker(ent.role, ent.roleName);
				const ImU32 ringColor = ent.isPartyMember ? IM_COL32(50, 220, 100, 255) : marker.color;

				ImVec2 pos = WorldToRadar(ent.position, localPos, localYaw,
					cfg.RotateWithCamera, side, radarCenter, cfg.Zoom);

				float ddx = pos.x - radarCenter.x;
				float ddy = pos.y - radarCenter.y;
				if (ddx * ddx + ddy * ddy > (side * 0.5f) * (side * 0.5f)) continue;

				const float badgeRadius = (marker.text[1] != '\0') ? 8.5f : 7.0f;

				// Dark badge circle with colored perimeter
				dl->AddCircleFilled(pos, badgeRadius, IM_COL32(10, 12, 16, 220));
				dl->AddCircle(pos, badgeRadius, ringColor, 0, 1.5f);

				// Centered letter label (C for Criminals, CI for Civilians, P for Police)
				ImVec2 ts = ImGui::CalcTextSize(marker.text);
				ImVec2 tp(pos.x - ts.x * 0.5f, pos.y - ts.y * 0.5f);
				dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 255), marker.text);
				dl->AddText(tp, marker.color, marker.text);
				// Direction pointer for other players
				if (ent.yaw != 0.f || ent.lookDirection.x != 0.f || ent.lookDirection.z != 0.f) {
					float otherYaw = ent.yaw;
					if (otherYaw == 0.f) {
						otherYaw = std::atan2(ent.lookDirection.x, ent.lookDirection.z);
					}
					float otherAngle = otherYaw;
					if (cfg.RotateWithCamera) {
						float camYaw = localYaw;
						if (snapshot && snapshot->camera.valid) {
							camYaw = std::atan2(snapshot->camera.forward.x, snapshot->camera.forward.z);
						}
						otherAngle = otherYaw - camYaw;
					}
					const float os = std::sin(otherAngle);
					const float oc = std::cos(otherAngle);
					const ImVec2 oStart(pos.x + os * badgeRadius, pos.y - oc * badgeRadius);
					const ImVec2 oEnd(pos.x + os * (badgeRadius + 5.f), pos.y - oc * (badgeRadius + 5.f));
					dl->AddLine(oStart, oEnd, ringColor, 2.0f);
				}
			}
		}

		// Local character center dot
		dl->AddCircleFilled(radarCenter, 5.f, IM_COL32(50, 220, 100, 255));
		dl->AddCircle(radarCenter, 5.f, IM_COL32(0, 0, 0, 220), 0, 1.5f);

		// Local character directional arrowhead pointing in facing direction
		// In game: yaw 0 = North (+Z), +PI/2 = East (+X), PI = South (-Z), -PI/2 = West (-X)
		// On screen: North is UP (-Y), East is RIGHT (+X), South is DOWN (+Y), West is LEFT (-X)
		float faceAngle = localYaw;
		if (cfg.RotateWithCamera) {
			float camYaw = localYaw;
			if (snapshot && snapshot->camera.valid) {
				camYaw = std::atan2(snapshot->camera.forward.x, snapshot->camera.forward.z);
			}
			faceAngle = localYaw - camYaw;
		}

		const float s = std::sin(faceAngle);
		const float c = std::cos(faceAngle);
		const ImVec2 fwd(s, -c);
		const ImVec2 rgt(c, s);

		const float tipLen = 16.f;
		const float baseLen = 4.f;
		const float halfWidth = 5.5f;

		const ImVec2 tip(radarCenter.x + fwd.x * tipLen, radarCenter.y + fwd.y * tipLen);
		const ImVec2 leftWing(radarCenter.x + fwd.x * baseLen - rgt.x * halfWidth,
		                      radarCenter.y + fwd.y * baseLen - rgt.y * halfWidth);
		const ImVec2 rightWing(radarCenter.x + fwd.x * baseLen + rgt.x * halfWidth,
		                       radarCenter.y + fwd.y * baseLen + rgt.y * halfWidth);

		// Solid arrowhead with outline
		dl->AddTriangleFilled(tip, leftWing, rightWing, IM_COL32(50, 255, 120, 255));
		dl->AddTriangle(tip, leftWing, rightWing, IM_COL32(0, 0, 0, 220), 1.5f);
		dl->AddLine(radarCenter, tip, IM_COL32(50, 255, 120, 255), 2.0f);
	}

	dl->PopClipRect();

	if (ImGui::IsWindowHovered()) {
		float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.f) {
			ConfigInstance.Radar.Zoom = std::clamp(
				ConfigInstance.Radar.Zoom + wheel * 0.1f, 0.2f, 10.f);
		}
	}

	ImGui::End();
	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar(2);
}

const char* MapFileForLocation(const std::string& id)
{
	std::string lower = id;
	for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
	if (lower == "shootercity")   return "city_colliders_map.png";
	if (lower == "settlement")    return "settlement.png";
	if (lower == "settlement2")   return "settlement_2.png";
	if (lower == "prison")        return "prison.png";
	if (lower == "hub")           return "hangar.png";
	if (lower == "sectorcenter")  return "city_colliders_map.png";
	if (lower == "clanbattle")    return "city_colliders_map.png";
	if (lower == "lootybay")      return "looty_bay.png";
	if (lower == "lennysmotel")   return "lennys_motel.png";
	return nullptr;
}

void CalibrateBoundsForMap(const std::string& mapName)
{
	if (mapName == "city_colliders_map.png" || mapName == "map.png") {
		ConfigInstance.Radar.WorldMinX = -600.f;
		ConfigInstance.Radar.WorldMaxX = 600.f;
		ConfigInstance.Radar.WorldMinZ = -400.f;
		ConfigInstance.Radar.WorldMaxZ = 400.f;
	} else if (mapName == "settlement.png" || mapName == "settlement_2.png") {
		ConfigInstance.Radar.WorldMinX = -450.f;
		ConfigInstance.Radar.WorldMaxX = 450.f;
		ConfigInstance.Radar.WorldMinZ = -450.f;
		ConfigInstance.Radar.WorldMaxZ = 450.f;
	} else if (mapName == "prison.png") {
		ConfigInstance.Radar.WorldMinX = -350.f;
		ConfigInstance.Radar.WorldMaxX = 350.f;
		ConfigInstance.Radar.WorldMinZ = -350.f;
		ConfigInstance.Radar.WorldMaxZ = 350.f;
	} else if (mapName == "hangar.png") {
		ConfigInstance.Radar.WorldMinX = -200.f;
		ConfigInstance.Radar.WorldMaxX = 200.f;
		ConfigInstance.Radar.WorldMinZ = -200.f;
		ConfigInstance.Radar.WorldMaxZ = 200.f;
	} else if (mapName == "looty_bay.png") {
		ConfigInstance.Radar.WorldMinX = -500.f;
		ConfigInstance.Radar.WorldMaxX = 500.f;
		ConfigInstance.Radar.WorldMinZ = -500.f;
		ConfigInstance.Radar.WorldMaxZ = 500.f;
	} else if (mapName == "lennys_motel.png") {
		ConfigInstance.Radar.WorldMinX = -300.f;
		ConfigInstance.Radar.WorldMaxX = 300.f;
		ConfigInstance.Radar.WorldMinZ = -300.f;
		ConfigInstance.Radar.WorldMaxZ = 300.f;
	}
}

void AutoSelectMap(const std::string& locationId)
{
	if (locationId.empty()) return;
	const char* file = MapFileForLocation(locationId);
	if (!file) return;
	for (int i = 0; i < (int)g_mapFiles.size(); i++) {
		if (g_mapFiles[i] == file) {
			SetMap(i);
			break;
		}
	}
}

void DrawFullMap()
{
	if (!ConfigInstance.Radar.PopoutWindow) return;

	if (ConfigInstance.Radar.AutoMapSelect)
	{
		static std::string lastLocationId;
		const std::string loc = RobRunReader::GetLocationId();
		if (!loc.empty() && loc != lastLocationId) {
			lastLocationId = loc;
			AutoSelectMap(loc);
		}
	}

	const auto snapshot = RobRunReader::GetSnapshot();
	const auto& cfg = ConfigInstance.Radar;

	ImGui::SetNextWindowPos(
		ImVec2((float)cfg.PopoutX, (float)cfg.PopoutY), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(
		ImVec2((float)cfg.PopoutW, (float)cfg.PopoutH), ImGuiCond_FirstUseEver);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 12, 18, 240));
	ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(16, 18, 28, 240));
	ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(25, 60, 120, 240));

	if (!ImGui::Begin("Full Map", &ConfigInstance.Radar.PopoutWindow,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
		return;
	}

	// Persist window position/size back to config
	{
		const ImVec2 wp = ImGui::GetWindowPos();
		const ImVec2 ws = ImGui::GetWindowSize();
		ConfigInstance.Radar.PopoutX = (int)wp.x;
		ConfigInstance.Radar.PopoutY = (int)wp.y;
		ConfigInstance.Radar.PopoutW = (int)ws.x;
		ConfigInstance.Radar.PopoutH = (int)ws.y;
	}

	const ImVec2 contentPos = ImGui::GetCursorScreenPos();
	const ImVec2 contentSize = ImGui::GetContentRegionAvail();
	if (contentSize.x < 10.f || contentSize.y < 10.f) {
		ImGui::End();
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
		return;
	}

	const float worldW = cfg.WorldMaxX - cfg.WorldMinX;
	const float worldH = cfg.WorldMaxZ - cfg.WorldMinZ;
	if (worldW <= 0.f || worldH <= 0.f) {
		ImGui::End();
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
		return;
	}

	// Fill the entire content area; zoom scales from there
	const float zoom = cfg.Zoom;
	float drawW = contentSize.x * zoom;
	float drawH = contentSize.y * zoom;

	// Center of content area — pan offset via scroll
	static ImVec2 s_pan{0.f, 0.f};
	const ImVec2 drawOrigin(
		contentPos.x + (contentSize.x - drawW) * 0.5f + s_pan.x,
		contentPos.y + (contentSize.y - drawH) * 0.5f + s_pan.y);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 clipMin = contentPos;
	const ImVec2 clipMax(contentPos.x + contentSize.x, contentPos.y + contentSize.y);
	dl->PushClipRect(clipMin, clipMax, true);

	// Map texture or dark background
	const ImVec2 mapMin = drawOrigin;
	const ImVec2 mapMax(drawOrigin.x + drawW, drawOrigin.y + drawH);
	if (g_mapSRV && cfg.ShowMap) {
		dl->AddImage((ImTextureID)g_mapSRV, mapMin, mapMax,
			ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), IM_COL32(255, 255, 255, 200));
	} else {
		dl->AddRectFilled(mapMin, mapMax, IM_COL32(18, 22, 32, 220));
	}
	dl->AddRect(mapMin, mapMax, IM_COL32(40, 80, 160, 180), 0.f, 0, 1.5f);

	// Helper: world XZ -> screen pixel
	auto worldToScreen = [&](float wx, float wz) -> ImVec2 {
		float nx = (wx - cfg.WorldMinX) / worldW;
		float nz = (wz - cfg.WorldMinZ) / worldH;
		return ImVec2(drawOrigin.x + nx * drawW, drawOrigin.y + nz * drawH);
	};

	// Draw world entities
	if (snapshot && snapshot->world && snapshot->world->valid) {
		const Vector3 localPos = snapshot->localPosition;
		const bool haveLocal = !localPos.IsZero();

		for (const auto& ent : snapshot->world->entities) {
			if (ent.kind == RobRunWorld::Kind::Car || ent.kind == RobRunWorld::Kind::Helicopter ||
				ent.kind == RobRunWorld::Kind::Vehicle) {
				if (!cfg.ShowVehicles) continue;
				ImVec2 sp = worldToScreen(ent.position.x, ent.position.z);
				if (sp.x < clipMin.x - 8.f || sp.x > clipMax.x + 8.f ||
					sp.y < clipMin.y - 8.f || sp.y > clipMax.y + 8.f)
					continue;
				dl->AddCircleFilled(sp, 4.f, IM_COL32(120, 190, 255, 220));
				if (cfg.ShowLootNames && !ent.label.empty()) {
					ImVec2 ts = ImGui::CalcTextSize(ent.label.c_str());
					dl->AddText(ImVec2(sp.x - ts.x * 0.5f, sp.y + 5.f),
						IM_COL32(220, 220, 220, 180), ent.label.c_str());
				}
				continue;
			}

			LootRenderInfo lootInfo;
			if ((!ConfigInstance.LootFilter.Enabled || ConfigInstance.LootFilter.ShowOnMap) &&
				EvaluateLootEntity(ent, localPos, haveLocal, lootInfo)) {
				ImVec2 sp = worldToScreen(ent.position.x, ent.position.z);
				if (sp.x < clipMin.x - 8.f || sp.x > clipMax.x + 8.f ||
					sp.y < clipMin.y - 8.f || sp.y > clipMax.y + 8.f)
					continue;

				float markerPx = lootInfo.dotSize;
				bool drewSprite = false;
				if (!lootInfo.icon.empty()) {
					const auto sprite = IconSprites::Get(lootInfo.icon.c_str());
					if (sprite.srv) {
						const float size = (float)ConfigInstance.LootFilter.ResolvedIconSize(lootInfo.category);
						float drawW = size, drawH = size;
						if (sprite.width > 0 && sprite.height > 0) {
							if (sprite.width >= sprite.height)
								drawH = size * (float)sprite.height / (float)sprite.width;
							else
								drawW = size * (float)sprite.width / (float)sprite.height;
						}
						markerPx = drawH * 0.5f;
						dl->AddImage(reinterpret_cast<ImTextureID>(sprite.srv),
							ImVec2(sp.x - drawW * 0.5f, sp.y - drawH * 0.5f),
							ImVec2(sp.x + drawW * 0.5f, sp.y + drawH * 0.5f));
						drewSprite = true;
					}
				}
				if (!drewSprite && lootInfo.showDot) {
					dl->AddCircleFilled(sp, lootInfo.dotSize, lootInfo.colour);
					if (lootInfo.hasHighValueRing) {
						dl->AddCircle(sp, lootInfo.dotSize + 2.5f, lootInfo.colour, 0, 1.5f);
					}
				}

				const bool showLabel = !lootInfo.text.empty() &&
					(ConfigInstance.LootFilter.Enabled || cfg.ShowLootNames);

				if (showLabel) {
					ImVec2 ts = ImGui::CalcTextSize(lootInfo.text.c_str());
					ImVec2 tp(sp.x - ts.x * 0.5f, sp.y + markerPx + 1.f);
					dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 240), lootInfo.text.c_str());
					dl->AddText(ImVec2(tp.x - 1.f, tp.y - 1.f), IM_COL32(0, 0, 0, 240), lootInfo.text.c_str());
					dl->AddText(tp, lootInfo.textColour, lootInfo.text.c_str());
				}
			}
		}
	}

	// Draw players
	if (snapshot && cfg.ShowPlayers) {
		for (const auto& ent : snapshot->entities) {
			if (ent.isDead) continue;
			const bool isLocal = ent.entityId == snapshot->localEntityId;

			const RoleMarker marker = GetRoleMarker(ent.role, ent.roleName);
			ImU32 badgeCol = marker.color;
			if (isLocal) badgeCol = IM_COL32(50, 255, 120, 255);
			else if (ent.isPartyMember) badgeCol = IM_COL32(50, 220, 100, 255);

			ImVec2 sp = worldToScreen(ent.position.x, ent.position.z);
			if (sp.x < clipMin.x - 16.f || sp.x > clipMax.x + 16.f ||
				sp.y < clipMin.y - 16.f || sp.y > clipMax.y + 16.f)
				continue;

			const float r = isLocal ? 6.f : 5.f;
			dl->AddCircleFilled(sp, r, IM_COL32(10, 12, 16, 220));
			dl->AddCircle(sp, r, badgeCol, 0, 1.5f);

			const char* txt = isLocal ? "ME" : marker.text;
			ImVec2 ts = ImGui::CalcTextSize(txt);
			ImVec2 tp(sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f);
			dl->AddText(ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 255), txt);
			dl->AddText(tp, badgeCol, txt);

			if (cfg.ShowPlayerNames && !ent.nickname.empty() && !isLocal) {
				const std::string& nm = ent.nicknameUtf8;
				ImVec2 ns = ImGui::CalcTextSize(nm.c_str());
				ImVec2 np(sp.x - ns.x * 0.5f, sp.y + r + 2.f);
				dl->AddText(ImVec2(np.x + 1.f, np.y + 1.f), IM_COL32(0, 0, 0, 200),
					nm.c_str());
				dl->AddText(np, badgeCol, nm.c_str());
			}
		}
	}

	dl->PopClipRect();

	// Zoom with mouse wheel, pan with middle-drag
	if (ImGui::IsWindowHovered()) {
		float wheel = ImGui::GetIO().MouseWheel;
		if (wheel != 0.f)
			ConfigInstance.Radar.Zoom = std::clamp(cfg.Zoom + wheel * 0.15f, 0.5f, 8.f);
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f)) {
			ImVec2 d = ImGui::GetIO().MouseDelta;
			s_pan.x += d.x;
			s_pan.y += d.y;
		}
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
			s_pan = ImVec2(0.f, 0.f);
			ConfigInstance.Radar.Zoom = 1.f;
		}
	}

	ImGui::End();
	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar(2);
}

namespace {

ImU32 GetInGameMapRoleColor(int role, const std::string& roleName, bool isTeammate)
{
	if (isTeammate)
		return IM_COL32(75, 235, 235, 255);
	const std::string label = RobRunReader::RoleLabel(role, roleName);
	if (label == "Criminal")
		return IM_COL32(255, 68, 68, 255);
	if (label == "Police Officer")
		return IM_COL32(55, 145, 255, 255);
	if (label == "Civilian" || label == "Hubber")
		return IM_COL32(70, 225, 90, 255);
	return IM_COL32(255, 68, 68, 255);
}

int RolePriority(int role, const std::string& roleName, bool isTeammate)
{
	if (isTeammate) return 1;
	const std::string label = RobRunReader::RoleLabel(role, roleName);
	if (label == "Criminal") return 4;
	if (label == "Police Officer") return 3;
	return 2;
}

ImU32 LightenColor(ImU32 col, int add)
{
	const int r = std::min(255, static_cast<int>((col >> IM_COL32_R_SHIFT) & 0xFF) + add);
	const int g = std::min(255, static_cast<int>((col >> IM_COL32_G_SHIFT) & 0xFF) + add);
	const int b = std::min(255, static_cast<int>((col >> IM_COL32_B_SHIFT) & 0xFF) + add);
	const int a = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFF);
	return IM_COL32(r, g, b, a);
}

// Clean Rob Run flat-vector comic car icon (2-pass seamless black outer contour).
void DrawCarIcon(ImDrawList* dl, const ImVec2& c, float scale, ImU32 fillCol)
{
	const int alpha = static_cast<int>((fillCol >> IM_COL32_A_SHIFT) & 0xFF);
	// Fuser-safe dark charcoal (pure/near-black gets luma-keyed transparent on DMA fusers).
	const ImU32 outline = IM_COL32(26, 28, 32, std::min(255, alpha + 20));
	const ImU32 glassCol = IM_COL32(46, 58, 74, alpha);
	const ImU32 glassHi = IM_COL32(150, 195, 235, std::min(170, alpha));
	const ImU32 tireCol = IM_COL32(32, 35, 40, alpha);
	const ImU32 rimCol = IM_COL32(225, 232, 240, alpha);

	const float out = std::max(1.2f, 1.05f * scale);
	const float bw = 7.2f * scale;   // half-width of lower chassis
	const float bh = 2.6f * scale;   // half-height of lower chassis
	const float bodyTop = c.y - bh * 0.45f;
	const float bodyBot = c.y + bh * 1.15f;
	const float cabWTop = 3.5f * scale;
	const float cabWBot = 5.4f * scale;
	const float cabTop = bodyTop - 3.3f * scale;
	const float wheelY = bodyBot - 0.1f * scale;
	const float wheelX = bw * 0.56f;
	const float wheelR = 2.25f * scale;
	const float round = 2.6f * scale;

	// Pass 1: seamless outer black contour
	const ImVec2 cabOut[4] = {
		ImVec2(c.x - cabWBot - out, bodyTop + out * 0.35f),
		ImVec2(c.x - cabWTop - out * 0.85f, cabTop - out),
		ImVec2(c.x + cabWTop + out * 0.85f, cabTop - out),
		ImVec2(c.x + cabWBot + out, bodyTop + out * 0.35f),
	};
	dl->AddQuadFilled(cabOut[0], cabOut[1], cabOut[2], cabOut[3], outline);
	dl->AddCircleFilled(ImVec2(c.x - cabWTop * 0.92f, cabTop + 0.3f * scale), out * 1.05f, outline, 10);
	dl->AddCircleFilled(ImVec2(c.x + cabWTop * 0.92f, cabTop + 0.3f * scale), out * 1.05f, outline, 10);
	dl->AddRectFilled(
		ImVec2(c.x - bw - out, bodyTop - out * 0.7f),
		ImVec2(c.x + bw + out, bodyBot + out),
		outline, round + out * 0.6f);
	dl->AddCircleFilled(ImVec2(c.x - wheelX, wheelY), wheelR + out * 0.85f, outline, 12);
	dl->AddCircleFilled(ImVec2(c.x + wheelX, wheelY), wheelR + out * 0.85f, outline, 12);

	// Pass 2: inner vibrant car body + roof fill
	const ImVec2 cabIn[4] = {
		ImVec2(c.x - cabWBot, bodyTop + 0.4f * scale),
		ImVec2(c.x - cabWTop, cabTop),
		ImVec2(c.x + cabWTop, cabTop),
		ImVec2(c.x + cabWBot, bodyTop + 0.4f * scale),
	};
	dl->AddQuadFilled(cabIn[0], cabIn[1], cabIn[2], cabIn[3], fillCol);
	dl->AddRectFilled(ImVec2(c.x - bw, bodyTop), ImVec2(c.x + bw, bodyBot), fillCol, round);

	// Subtle top-hood shine for native flat-vector depth
	dl->AddLine(
		ImVec2(c.x - bw * 0.78f, bodyTop + 0.55f * scale),
		ImVec2(c.x + bw * 0.78f, bodyTop + 0.55f * scale),
		LightenColor(fillCol, 45), std::max(1.0f, 0.75f * scale));

	// Clean front & rear side windows (split by center B-pillar)
	const float winTop = cabTop + 0.75f * scale;
	const float winBot = bodyTop - 0.15f * scale;
	const float pillar = 0.55f * scale;
	const ImVec2 winL[4] = {
		ImVec2(c.x - cabWBot * 0.78f, winBot),
		ImVec2(c.x - cabWTop * 0.82f, winTop),
		ImVec2(c.x - pillar, winTop),
		ImVec2(c.x - pillar, winBot),
	};
	const ImVec2 winR[4] = {
		ImVec2(c.x + pillar, winBot),
		ImVec2(c.x + pillar, winTop),
		ImVec2(c.x + cabWTop * 0.82f, winTop),
		ImVec2(c.x + cabWBot * 0.78f, winBot),
	};
	dl->AddQuadFilled(winL[0], winL[1], winL[2], winL[3], glassCol);
	dl->AddQuadFilled(winR[0], winR[1], winR[2], winR[3], glassCol);
	dl->AddLine(
		ImVec2(c.x - cabWTop * 0.65f, winTop + 0.35f * scale),
		ImVec2(c.x - pillar - 0.3f * scale, winTop + 0.35f * scale),
		glassHi, std::max(1.0f, 0.65f * scale));

	// Clean round wheels + bright hubcaps
	dl->AddCircleFilled(ImVec2(c.x - wheelX, wheelY), wheelR, tireCol, 12);
	dl->AddCircleFilled(ImVec2(c.x + wheelX, wheelY), wheelR, tireCol, 12);
	dl->AddCircleFilled(ImVec2(c.x - wheelX, wheelY), wheelR * 0.48f, rimCol, 10);
	dl->AddCircleFilled(ImVec2(c.x + wheelX, wheelY), wheelR * 0.48f, rimCol, 10);
}

// Clean Rob Run flat-vector helicopter icon (2-pass seamless black outer contour).
void DrawHeliIcon(ImDrawList* dl, const ImVec2& c, float scale, ImU32 fillCol)
{
	const int alpha = static_cast<int>((fillCol >> IM_COL32_A_SHIFT) & 0xFF);
	const ImU32 outline = IM_COL32(26, 28, 32, std::min(255, alpha + 20));
	const ImU32 glassCol = IM_COL32(46, 58, 74, alpha);
	const ImU32 rotorIn = IM_COL32(232, 238, 246, alpha);

	const float out = std::max(1.2f, 1.05f * scale);
	const float bodyR = 4.5f * scale;
	const float bodyCx = c.x + 1.2f * scale;
	const float bodyCy = c.y + 0.4f * scale;
	const float tailEndX = c.x - 8.2f * scale;
	const float tailY = bodyCy - 0.5f * scale;
	const float skidY = bodyCy + bodyR + 1.5f * scale;
	const float rotorY = bodyCy - bodyR - 1.6f * scale;
	const float rotorHalf = 8.4f * scale;

	// Pass 1: seamless outer black silhouette (skids, tail, fin, cabin, rotor mast + blades)
	dl->AddLine(ImVec2(bodyCx - 4.4f * scale, skidY), ImVec2(bodyCx + 4.8f * scale, skidY), outline, out * 2.1f);
	dl->AddLine(ImVec2(bodyCx - 2.2f * scale, bodyCy + bodyR * 0.6f), ImVec2(bodyCx - 2.2f * scale, skidY), outline, out * 1.6f);
	dl->AddLine(ImVec2(bodyCx + 2.2f * scale, bodyCy + bodyR * 0.6f), ImVec2(bodyCx + 2.2f * scale, skidY), outline, out * 1.6f);

	const ImVec2 tailOut[4] = {
		ImVec2(bodyCx - bodyR * 0.4f, tailY - 1.9f * scale - out),
		ImVec2(tailEndX - out, tailY - 0.8f * scale - out),
		ImVec2(tailEndX - out, tailY + 1.0f * scale + out),
		ImVec2(bodyCx - bodyR * 0.4f, tailY + 2.1f * scale + out),
	};
	dl->AddQuadFilled(tailOut[0], tailOut[1], tailOut[2], tailOut[3], outline);
	dl->AddCircleFilled(ImVec2(tailEndX, tailY - 1.2f * scale), 2.2f * scale + out * 0.8f, outline, 12);
	dl->AddCircleFilled(ImVec2(bodyCx, bodyCy), bodyR + out, outline, 18);
	dl->AddLine(ImVec2(bodyCx, bodyCy - bodyR), ImVec2(bodyCx, rotorY), outline, out * 1.8f);
	dl->AddLine(ImVec2(bodyCx - rotorHalf, rotorY), ImVec2(bodyCx + rotorHalf, rotorY), outline, out * 2.1f);

	// Pass 2: inner role-colored fuselage + tail boom + tail rotor
	const ImVec2 tailIn[4] = {
		ImVec2(bodyCx - bodyR * 0.35f, tailY - 1.6f * scale),
		ImVec2(tailEndX, tailY - 0.6f * scale),
		ImVec2(tailEndX, tailY + 0.8f * scale),
		ImVec2(bodyCx - bodyR * 0.35f, tailY + 1.8f * scale),
	};
	dl->AddQuadFilled(tailIn[0], tailIn[1], tailIn[2], tailIn[3], fillCol);
	dl->AddCircleFilled(ImVec2(tailEndX, tailY - 1.2f * scale), 1.9f * scale, fillCol, 12);
	dl->AddCircleFilled(ImVec2(tailEndX, tailY - 1.2f * scale), 0.75f * scale, outline, 8);
	dl->AddCircleFilled(ImVec2(bodyCx, bodyCy), bodyR, fillCol, 18);

	// Cockpit bubble canopy (front-upper quadrant)
	const ImVec2 canopy[4] = {
		ImVec2(bodyCx + 0.2f * scale, bodyCy - bodyR * 0.72f),
		ImVec2(bodyCx + bodyR * 0.76f, bodyCy - bodyR * 0.48f),
		ImVec2(bodyCx + bodyR * 0.82f, bodyCy + bodyR * 0.18f),
		ImVec2(bodyCx + 0.2f * scale, bodyCy + bodyR * 0.18f),
	};
	dl->AddQuadFilled(canopy[0], canopy[1], canopy[2], canopy[3], glassCol);

	// Bright inner top rotor blade & skids highlight
	dl->AddLine(ImVec2(bodyCx - rotorHalf * 0.92f, rotorY), ImVec2(bodyCx + rotorHalf * 0.92f, rotorY),
		rotorIn, std::max(1.2f, 1.0f * scale));
	dl->AddLine(ImVec2(bodyCx - 4.0f * scale, skidY), ImVec2(bodyCx + 4.4f * scale, skidY),
		rotorIn, std::max(1.0f, 0.8f * scale));
}

// Smooth rounded-corner, flat-notched player arrow matching Rob Run's native green player arrow.
void DrawPlayerArrow(ImDrawList* dl, const ImVec2& c, float relYaw, float radius, ImU32 fillCol)
{
	if (DrawMapSpriteRotated(dl, MapSpriteSlot::PlayerArrow, c, radius * 1.15f, relYaw, fillCol))
		return;

	const int alpha = static_cast<int>((fillCol >> IM_COL32_A_SHIFT) & 0xFF);
	const ImU32 outline = IM_COL32(26, 28, 32, std::min(255, alpha + 25));

	const float dirX = std::sin(relYaw);
	const float dirY = -std::cos(relYaw);
	const float perpX = -dirY;
	const float perpY = dirX;

	auto ptAt = [&](float fwd, float side) -> ImVec2 {
		return ImVec2(
			c.x + dirX * (fwd * radius) + perpX * (side * radius),
			c.y + dirY * (fwd * radius) + perpY * (side * radius));
	};

	// Exact silhouette of the native Rob Run arrow: wide arrowhead with shallow flat tail notch
	const ImVec2 tip     = ptAt( 1.00f,  0.00f);
	const ImVec2 wingL   = ptAt(-0.76f, -0.82f);
	const ImVec2 tailL   = ptAt(-0.76f, -0.44f);
	const ImVec2 notchL  = ptAt(-0.44f, -0.34f);
	const ImVec2 notchR  = ptAt(-0.44f,  0.34f);
	const ImVec2 tailR   = ptAt(-0.76f,  0.44f);
	const ImVec2 wingR   = ptAt(-0.76f,  0.82f);

	const float outT = std::max(1.4f, radius * 0.16f);
	const float capR = outT * 0.50f;

	// Pass 1: smooth dark outer contour + rounded corner caps (no sharp miter spikes)
	const ImVec2 poly[7] = { tip, wingL, tailL, notchL, notchR, tailR, wingR };
	dl->AddPolyline(poly, 7, outline, ImDrawFlags_Closed, outT);
	dl->AddCircleFilled(tip,   capR, outline, 10);
	dl->AddCircleFilled(wingL, capR, outline, 10);
	dl->AddCircleFilled(wingR, capR, outline, 10);
	dl->AddCircleFilled(tailL, capR * 0.85f, outline, 8);
	dl->AddCircleFilled(tailR, capR * 0.85f, outline, 8);

	// Pass 2: clean convex-decomposed vibrant fill
	const ImVec2 center = ptAt(-0.10f, 0.00f);
	dl->AddTriangleFilled(tip, wingL, center, fillCol);
	dl->AddTriangleFilled(tip, center, wingR, fillCol);
	dl->AddQuadFilled(center, wingL, tailL, notchL, fillCol);
	dl->AddQuadFilled(center, notchR, tailR, wingR, fillCol);
	dl->AddTriangleFilled(center, notchL, notchR, fillCol);

	// Crisp outer border stroke on top so edges stay razor-sharp like the native arrow
	dl->AddPolyline(poly, 7, outline, ImDrawFlags_Closed, std::max(1.1f, outT * 0.50f));

	// Subtle inner highlight along the leading edges for a crisp native-sprite feel
	const ImU32 hiCol = LightenColor(fillCol, 42);
	dl->AddLine(ptAt(0.76f, 0.00f), ptAt(-0.45f, -0.52f), hiCol, std::max(1.0f, radius * 0.08f));
}

// Resolve game rarity tier (1=Common, 2=Uncommon, 3=Rare, 4=Epic, 5=Legendary, 6=Mythical/Exotic).
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

ImU32 MapRarityColor(int tier, ImU32 fallback)
{
	switch (tier) {
	case 1: return IM_COL32(220, 226, 234, 240); // Common - silver-white
	case 2: return IM_COL32(46, 224, 78, 250);   // Uncommon - green
	case 3: return IM_COL32(58, 160, 255, 250);  // Rare - blue
	case 4: return IM_COL32(182, 76, 255, 250);  // Epic - purple
	case 5: return IM_COL32(255, 208, 42, 255);  // Legendary - gold
	case 6: return IM_COL32(255, 58, 128, 255);  // Mythical / Exotic - reddish-pink
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

enum class MapItemIcon {
	Weapon,
	Armor,
	Valuable,
	Cash,
	Safe,
	Cargo,
	Medicine,
	Key,
	Generic
};

void DrawMapCategoryIcon(ImDrawList* dl, const ImVec2& c, float scale, MapItemIcon icon, ImU32 col)
{
	MapSpriteSlot slot = MapSpriteSlot::Generic;
	switch (icon) {
	case MapItemIcon::Weapon:   slot = MapSpriteSlot::Weapon; break;
	case MapItemIcon::Armor:    slot = MapSpriteSlot::Armor; break;
	case MapItemIcon::Valuable: slot = MapSpriteSlot::Valuable; break;
	case MapItemIcon::Cash:     slot = MapSpriteSlot::Cash; break;
	case MapItemIcon::Safe:     slot = MapSpriteSlot::Safe; break;
	case MapItemIcon::Cargo:    slot = MapSpriteSlot::Cargo; break;
	case MapItemIcon::Medicine: slot = MapSpriteSlot::Medicine; break;
	case MapItemIcon::Key:      slot = MapSpriteSlot::Key; break;
	case MapItemIcon::Generic:  slot = MapSpriteSlot::Generic; break;
	}
	if (DrawMapSprite(dl, slot, c, 7.8f * scale, col))
		return;

	const ImU32 outline = IM_COL32(26, 28, 32, 245);
	const ImU32 inner   = IM_COL32(26, 28, 32, 210);
	const float s = 3.8f * scale;
	const float outT = std::max(1.3f, 0.95f * scale);

	switch (icon) {
	case MapItemIcon::Weapon: {
		// Compact rifle/gun silhouette: stock + receiver + barrel + magazine
		const float rx0 = c.x - s * 1.15f, rx1 = c.x + s * 0.55f;
		const float ry0 = c.y - s * 0.42f, ry1 = c.y + s * 0.12f;
		// Dark outline backing
		dl->AddRectFilled(ImVec2(rx0 - outT, ry0 - outT), ImVec2(rx1 + outT, ry1 + outT), outline, 1.0f);
		dl->AddRectFilled(ImVec2(rx1 - 0.5f, ry0 + s * 0.05f - outT), ImVec2(c.x + s * 1.35f + outT, ry0 + s * 0.34f + outT), outline, 0.8f);
		dl->AddRectFilled(ImVec2(c.x - s * 0.25f - outT, ry1 - 0.5f), ImVec2(c.x + s * 0.22f + outT, c.y + s * 0.88f + outT), outline, 0.8f);
		dl->AddRectFilled(ImVec2(rx0 - outT, ry1 - 0.5f), ImVec2(c.x - s * 0.65f + outT, c.y + s * 0.68f + outT), outline, 0.8f);
		// Rarity-colored fill
		dl->AddRectFilled(ImVec2(rx0, ry0), ImVec2(rx1, ry1), col, 0.8f);
		dl->AddRectFilled(ImVec2(rx1 - 0.5f, ry0 + s * 0.08f), ImVec2(c.x + s * 1.32f, ry0 + s * 0.32f), col, 0.6f);
		dl->AddRectFilled(ImVec2(c.x - s * 0.22f, ry1 - 0.5f), ImVec2(c.x + s * 0.18f, c.y + s * 0.84f), col, 0.6f);
		dl->AddRectFilled(ImVec2(rx0, ry1 - 0.5f), ImVec2(c.x - s * 0.68f, c.y + s * 0.64f), col, 0.6f);
		break;
	}
	case MapItemIcon::Armor: {
		// Shield silhouette
		const ImVec2 shield[5] = {
			ImVec2(c.x - s * 0.85f, c.y - s * 0.85f),
			ImVec2(c.x + s * 0.85f, c.y - s * 0.85f),
			ImVec2(c.x + s * 0.85f, c.y + s * 0.05f),
			ImVec2(c.x,             c.y + s * 1.00f),
			ImVec2(c.x - s * 0.85f, c.y + s * 0.05f),
		};
		dl->AddPolyline(shield, 5, outline, ImDrawFlags_Closed, outT * 1.8f);
		dl->AddConvexPolyFilled(shield, 5, col);
		dl->AddPolyline(shield, 5, outline, ImDrawFlags_Closed, std::max(1.0f, outT * 0.7f));
		break;
	}
	case MapItemIcon::Valuable: {
		// Faceted gem / diamond
		const ImVec2 gem[5] = {
			ImVec2(c.x - s * 0.55f, c.y - s * 0.82f),
			ImVec2(c.x + s * 0.55f, c.y - s * 0.82f),
			ImVec2(c.x + s * 0.98f, c.y - s * 0.15f),
			ImVec2(c.x,             c.y + s * 0.98f),
			ImVec2(c.x - s * 0.98f, c.y - s * 0.15f),
		};
		dl->AddPolyline(gem, 5, outline, ImDrawFlags_Closed, outT * 1.8f);
		dl->AddConvexPolyFilled(gem, 5, col);
		dl->AddPolyline(gem, 5, outline, ImDrawFlags_Closed, std::max(1.0f, outT * 0.7f));
		dl->AddLine(gem[4], gem[2], LightenColor(col, 55), std::max(1.0f, 0.7f * scale));
		break;
	}
	case MapItemIcon::Cash: {
		// Banknote stack
		const ImVec2 p0(c.x - s * 1.05f, c.y - s * 0.65f);
		const ImVec2 p1(c.x + s * 1.05f, c.y + s * 0.65f);
		dl->AddRectFilled(ImVec2(p0.x - outT, p0.y - outT), ImVec2(p1.x + outT, p1.y + outT), outline, 1.5f);
		dl->AddRectFilled(p0, p1, col, 1.2f);
		dl->AddCircleFilled(c, s * 0.32f, inner, 10);
		break;
	}
	case MapItemIcon::Safe: {
		// Safe box with dial
		const ImVec2 p0(c.x - s * 0.88f, c.y - s * 0.88f);
		const ImVec2 p1(c.x + s * 0.88f, c.y + s * 0.88f);
		dl->AddRectFilled(ImVec2(p0.x - outT, p0.y - outT), ImVec2(p1.x + outT, p1.y + outT), outline, 2.0f);
		dl->AddRectFilled(p0, p1, col, 1.5f);
		dl->AddCircle(c, s * 0.40f, inner, 12, std::max(1.1f, 0.8f * scale));
		dl->AddCircleFilled(c, s * 0.16f, inner, 8);
		break;
	}
	case MapItemIcon::Cargo: {
		// Cargo crate with diagonal brace
		const ImVec2 p0(c.x - s * 0.90f, c.y - s * 0.90f);
		const ImVec2 p1(c.x + s * 0.90f, c.y + s * 0.90f);
		dl->AddRectFilled(ImVec2(p0.x - outT, p0.y - outT), ImVec2(p1.x + outT, p1.y + outT), outline, 1.2f);
		dl->AddRectFilled(p0, p1, col, 1.0f);
		dl->AddLine(ImVec2(p0.x + 1.2f, p1.y - 1.2f), ImVec2(p1.x - 1.2f, p0.y + 1.2f), inner, std::max(1.1f, 0.85f * scale));
		dl->AddRect(p0, p1, outline, 1.0f, 0, std::max(1.0f, outT * 0.7f));
		break;
	}
	case MapItemIcon::Medicine: {
		// Medical cross (+)
		const float arm = s * 0.95f;
		const float bar = s * 0.34f;
		dl->AddRectFilled(ImVec2(c.x - bar - outT, c.y - arm - outT), ImVec2(c.x + bar + outT, c.y + arm + outT), outline, 1.0f);
		dl->AddRectFilled(ImVec2(c.x - arm - outT, c.y - bar - outT), ImVec2(c.x + arm + outT, c.y + bar + outT), outline, 1.0f);
		dl->AddRectFilled(ImVec2(c.x - bar, c.y - arm), ImVec2(c.x + bar, c.y + arm), col, 0.8f);
		dl->AddRectFilled(ImVec2(c.x - arm, c.y - bar), ImVec2(c.x + arm, c.y + bar), col, 0.8f);
		break;
	}
	case MapItemIcon::Key: {
		// Key ring + blade
		const ImVec2 ringC(c.x - s * 0.45f, c.y);
		const float ringR = s * 0.48f;
		dl->AddCircleFilled(ringC, ringR + outT, outline, 12);
		dl->AddRectFilled(ImVec2(ringC.x + ringR * 0.4f, c.y - s * 0.18f - outT),
			ImVec2(c.x + s * 1.05f + outT, c.y + s * 0.18f + outT), outline, 0.8f);
		dl->AddRectFilled(ImVec2(c.x + s * 0.55f - outT, c.y + s * 0.10f),
			ImVec2(c.x + s * 0.92f + outT, c.y + s * 0.62f + outT), outline, 0.6f);
		dl->AddCircleFilled(ringC, ringR, col, 12);
		dl->AddCircleFilled(ringC, ringR * 0.42f, inner, 10);
		dl->AddRectFilled(ImVec2(ringC.x + ringR * 0.4f, c.y - s * 0.16f),
			ImVec2(c.x + s * 1.02f, c.y + s * 0.16f), col, 0.6f);
		dl->AddRectFilled(ImVec2(c.x + s * 0.58f, c.y + s * 0.12f),
			ImVec2(c.x + s * 0.88f, c.y + s * 0.56f), col, 0.5f);
		break;
	}
	case MapItemIcon::Generic:
	default: {
		const float r = s * 0.72f;
		dl->AddCircleFilled(c, r + outT, outline, 12);
		dl->AddCircleFilled(c, r, col, 12);
		break;
	}
	}
}

} // namespace

void DrawInGameMapOverlay()
{
	const auto& cfg = ConfigInstance.Radar;
	if (!cfg.InGameMinimap && !cfg.InGameFullMap)
		return;

	const auto snapshot = RobRunReader::GetSnapshot();
	if (!snapshot || !snapshot->inGameMap.valid)
		return;

	const auto& map = snapshot->inGameMap;
	const bool isFull = map.fullscreenOpen;
	if (isFull && !cfg.InGameFullMap) return;
	if (!isFull && !cfg.InGameMinimap) return;

	const auto& var = isFull ? map.fullscreen : map.minimap;
	if (!var.valid || var.cameraSize <= 0.1f)
		return;

	Vector2 mapPos = var.position;
	float mapRot = var.rotation;
	float mapCamSize = var.cameraSize;
	if (isFull) {
		Vector2 livePos{};
		float liveRot = 0.f, liveSize = 0.f;
		if (RobRunReader::GetFreshFullMapVariance(livePos, liveRot, liveSize)) {
			mapPos = livePos;
			mapRot = liveRot;
			mapCamSize = liveSize;
		}
	}

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	if (display.x <= 0.f || display.y <= 0.f)
		return;

	// Convert Unity bottom-left UI Canvas reference pixels to ImGui top-left
	// screen pixels, scaling proportionally when CanvasScaler reference size
	// (e.g. 1920x1080) differs from the current display resolution.
	float minX = 0.f, maxX = 0.f, minY = 0.f, maxY = 0.f;
	if (var.rectValid) {
		const float sx = (var.uiScreenW > 100.f) ? (display.x / var.uiScreenW) : 1.f;
		const float sy = (var.uiScreenH > 100.f) ? (display.y / var.uiScreenH) : 1.f;
		minX = var.uiMinX * sx;
		maxX = var.uiMaxX * sx;
		minY = display.y - var.uiMaxY * sy;
		maxY = display.y - var.uiMinY * sy;
	} else if (isFull) {
		// Fallback on the first frame 'M' opens if RectTransform hasn't resolved yet:
		// MapMeshView.Camera renders into a 1:1 square container centered on screen.
		const float side = display.x;
		minX = (display.x - side) * 0.5f;
		maxX = minX + side;
		minY = (display.y - side) * 0.5f;
		maxY = minY + side;
	} else {
		const float uiScale = display.y / 1080.f;
		const float pad = 24.f * uiScale;
		const float side = 250.f * uiScale;
		minX = pad;
		minY = pad;
		maxX = pad + side;
		maxY = pad + side;
	}

	const float rectW = maxX - minX;
	const float rectH = maxY - minY;
	if (rectW < 40.f || rectH < 40.f)
		return;

	const float cx = (minX + maxX) * 0.5f;
	const float cy = (minY + maxY) * 0.5f;
	const float halfH = mapCamSize;
	const float halfW = halfH * (rectW / rectH);
	if (halfW <= 0.01f || halfH <= 0.01f)
		return;

	// Fullscreen ('M') map is always North-up (rotRad = 0).
	// Rotating minimap aligns its top edge (+Y in UI, -Y on screen) with the
	// 3D camera's horizontal forward direction in world XZ space.
	constexpr float kDegToRad = 3.1415926535f / 180.f;
	float rotRad = 0.f;
	if (!isFull && std::fabs(mapRot) > 0.01f) {
		RobRunCamera::CameraState freshCam{};
		bool haveCam = RobRunReader::GetFreshCamera(freshCam) && freshCam.valid;
		const Vector3& fwd = haveCam ? freshCam.forward : snapshot->camera.forward;
		const bool camOk = haveCam || snapshot->camera.valid;
		if (camOk && (fwd.x * fwd.x + fwd.z * fwd.z) > 1e-4f) {
			rotRad = std::atan2(fwd.x, fwd.z);
		} else {
			rotRad = -mapRot * kDegToRad;
		}
	}
	const float cosR = std::cos(rotRad);
	const float sinR = std::sin(rotRad);

	// For the minimap, the Unity RectTransform includes the outer dark HUD frame
	// (top FPS/PING/LOSS bar + side/bottom borders). Inset the clip rect to the
	// actual visible map square while keeping cx/cy/rectW/rectH intact for projection.
	const float insetL = isFull ? 0.f : (rectW * 0.083f);
	const float insetR = isFull ? 0.f : (rectW * 0.083f);
	const float insetT = isFull ? 0.f : (rectH * 0.105f);
	const float insetB = isFull ? 0.f : (rectH * 0.090f);
	const float clipMinX = std::clamp(minX + insetL, 0.f, display.x);
	const float clipMinY = std::clamp(minY + insetT, 0.f, display.y);
	const float clipMaxX = std::clamp(maxX - insetR, 0.f, display.x);
	const float clipMaxY = std::clamp(maxY - insetB, 0.f, display.y);
	if (clipMaxX - clipMinX < 20.f || clipMaxY - clipMinY < 20.f)
		return;

	auto worldToMap = [&](const Vector3& world, ImVec2& out) -> bool {
		if (!std::isfinite(world.x) || !std::isfinite(world.z)) return false;
		const float dx = world.x - mapPos.x;
		const float dz = world.z - mapPos.y;
		// Unity top-down orthographic camera rotated by rotRad clockwise from +Z:
		//   right = (cosR, -sinR), up/forward = (sinR, cosR)
		const float right = dx * cosR - dz * sinR;
		const float up    = dx * sinR + dz * cosR;
		const float nx = right / halfW;
		const float ny = up    / halfH;
		if (!isFull && (nx < -0.98f || nx > 0.98f || ny < -0.98f || ny > 0.98f))
			return false;
		out.x = cx + nx * (rectW * 0.5f);
		out.y = cy - ny * (rectH * 0.5f);
		const float margin = isFull ? 36.f : -1.f;
		if (out.x < clipMinX - margin || out.x > clipMaxX + margin ||
			out.y < clipMinY - margin || out.y > clipMaxY + margin)
			return false;
		return true;
	};

	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	if (!dl) return;

	// Bold 2x native-matched icon scaling for both minimap and fullscreen map.
	const float scale = std::clamp(cfg.InGameMapScale, 0.5f, 2.0f) * (isFull ? 2.15f : 1.85f);
	ImFont* font = ImGui::GetFont();
	const float fontSize = std::max(18.f, 9.5f * scale);

	dl->PushClipRect(ImVec2(clipMinX, clipMinY), ImVec2(clipMaxX, clipMaxY), true);

	// 1. Optional valuables/loot, cargo & dropped bags (drawn underneath vehicles/players)
	if (snapshot->world && snapshot->world->valid &&
		(cfg.InGameMapValuables || cfg.InGameMapCargo || cfg.InGameMapDroppedBags))
	{
		const auto& lf = ConfigInstance.LootFilter;
		// Rarity index: 0=All(1+), 1=Uncommon(2)+, 2=Rare(3)+, 3=Epic(4)+, 4=Legendary(5)+, 5=Mythical(6)
		const int minRarityTier = std::clamp(cfg.InGameMapMinRarity, 0, 5) + 1;
		const int minCargoRarityTier = std::clamp(cfg.InGameMapCargoMinRarity, 0, 5) + 1;

		for (const auto& we : snapshot->world->entities) {
			ImVec2 pt{};
			if (cfg.InGameMapDroppedBags && we.kind == RobRunWorld::Kind::DroppedBag) {
				if (!worldToMap(we.position, pt)) continue;
				if (!DrawMapSprite(dl, MapSpriteSlot::DroppedBag, pt, 8.2f * scale, IM_COL32(255, 200, 50, 250))) {
					const float r = 4.2f * scale;
					dl->AddQuadFilled(
						ImVec2(pt.x, pt.y - r), ImVec2(pt.x + r, pt.y),
						ImVec2(pt.x, pt.y + r), ImVec2(pt.x - r, pt.y),
						IM_COL32(255, 200, 50, 240));
					dl->AddQuad(
						ImVec2(pt.x, pt.y - r), ImVec2(pt.x + r, pt.y),
						ImVec2(pt.x, pt.y + r), ImVec2(pt.x - r, pt.y),
						IM_COL32(26, 28, 32, 240), 1.4f);
				}
				continue;
			}

			if (we.hasEmptyFlag && we.empty) continue;
			if (we.inVehicle || we.inTable) continue;

			const bool isCargo = IsCargoKind(we.kind);
			if (isCargo) {
				if (!cfg.InGameMapCargo) continue;
				const LootFilter::ItemDef* def = lf.FindItemForEntity(we.kind, we.label);
				const int tier = ResolveItemRarityTier(we, def);
				const int effectiveCargoTier = (tier > 0) ? tier : 1;
				if (effectiveCargoTier < minCargoRarityTier) continue;
				if (!worldToMap(we.position, pt)) continue;
				const ImU32 col = (we.kind == RobRunWorld::Kind::CargoCash)
					? IM_COL32(95, 245, 130, 245)
					: MapRarityColor(tier, IM_COL32(255, 110, 230, 245));
				const MapItemIcon icon = (we.kind == RobRunWorld::Kind::CargoCash)
					? MapItemIcon::Cash
					: MapItemIcon::Cargo;
				DrawMapCategoryIcon(dl, pt, scale, icon, col);
				continue;
			}

			if (cfg.InGameMapValuables && IsMapLootKind(we.kind)) {
				const LootFilter::ItemDef* def = lf.FindItemForEntity(we.kind, we.label);
				const bool isCash = (we.kind == RobRunWorld::Kind::CashLootNode ||
									 we.kind == RobRunWorld::Kind::SettlementCash);
				const bool isSafe = (we.kind == RobRunWorld::Kind::SafeDoor);
				const int tier = ResolveItemRarityTier(we, def);

				// Always show cash/safes; filter general ground/world loot by minimum rarity tier
				if (!isCash && !isSafe) {
					const bool isCatalogValuable = def && def->category == LootFilter::Category::Valuables;
					const int effectiveTier = (tier > 0) ? tier : (isCatalogValuable ? 2 : 1);
					if (effectiveTier < minRarityTier)
						continue;
				}

				if (!worldToMap(we.position, pt)) continue;

				MapItemIcon icon = MapItemIcon::Generic;
				ImU32 fallbackCol = IM_COL32(100, 255, 140, 245);
				if (isSafe) {
					icon = MapItemIcon::Safe;
					fallbackCol = IM_COL32(255, 208, 42, 250);
				} else if (isCash) {
					icon = MapItemIcon::Cash;
					fallbackCol = IM_COL32(95, 245, 130, 245);
				} else if (def) {
					switch (def->category) {
					case LootFilter::Category::Weapons:
					case LootFilter::Category::WeaponMaintenance:
						icon = MapItemIcon::Weapon;
						break;
					case LootFilter::Category::Armor:
						icon = MapItemIcon::Armor;
						break;
					case LootFilter::Category::Valuables:
						icon = MapItemIcon::Valuable;
						break;
					case LootFilter::Category::Medicine:
						icon = MapItemIcon::Medicine;
						break;
					case LootFilter::Category::Keys:
					case LootFilter::Category::Documents:
					case LootFilter::Category::Tools:
					case LootFilter::Category::BreachingTools:
						icon = MapItemIcon::Key;
						break;
					case LootFilter::Category::Cargo:
						icon = MapItemIcon::Cargo;
						break;
					default:
						icon = MapItemIcon::Generic;
						break;
					}
				}

				const ImU32 col = (isCash || isSafe) ? fallbackCol : MapRarityColor(tier, fallbackCol);
				DrawMapCategoryIcon(dl, pt, scale, icon, col);
			}
		}
	}

	// Match players currently driving/riding to their live vehicle (car vs heli)
	// using high-frequency snapshot->fastVehicles (80-500 Hz) with world fallback.
	const auto& pcfg = ConfigInstance.Player;
	const std::int32_t localId = snapshot->localEntityId;
	const bool useFastVeh = !snapshot->fastVehicles.empty();
	const bool haveWorld = snapshot->world && snapshot->world->valid;

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

	// Track the local player's publicId so we never mark our own parked/driven
	// vehicle (the game already draws our own car icon on the map).
	static std::uint64_t s_localPublicId = 0;
	static std::uint64_t s_localPubSession = 0;
	if (snapshot->sessionGeneration != s_localPubSession) {
		s_localPubSession = snapshot->sessionGeneration;
		s_localPublicId = 0;
	}
	for (const auto& ent : snapshot->entities) {
		if (ent.entityId == localId && ent.publicId > 0) {
			s_localPublicId = ent.publicId;
			break;
		}
	}

	const std::size_t vehCount = mapVehicles.size();
	struct VehicleOccupantInfo {
		bool occupied = false;
		bool localOccupied = false;
		bool isPolice = false;
		int priority = 0;
		ImU32 color = IM_COL32(215, 220, 228, 220);
		Vector3 driverPos{};
		bool hasDriverPos = false;
		std::vector<std::string> occupantNames;
	};
	std::vector<VehicleOccupantInfo> vehOcc(vehCount);
	std::vector<int> playerMatchedVeh(snapshot->entities.size(), -1);

	// Remember the last live driver seen inside each vehicle so if they drive
	// outside the 200-300 m player replication bubble (including helicopters or
	// stolen cars without CharacterOwner), we keep tracking their live role &
	// name on the moving vehicle and suppress their stale frozen ghost arrow.
	struct LastVehicleDriver {
		std::int32_t entityId = -1;
		int role = 0;
		std::string roleName;
		std::string nicknameUtf8;
		bool isTeammate = false;
		std::uint64_t stampMs = 0;
	};
	static std::unordered_map<int, LastVehicleDriver> s_lastVehDriver;
	static std::uint64_t s_lastVehDriverSession = 0;
	const std::uint64_t vehNowMs = GetTickCount64();
	if (snapshot->sessionGeneration != s_lastVehDriverSession) {
		s_lastVehDriverSession = snapshot->sessionGeneration;
		s_lastVehDriver.clear();
	}

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
				float bestDistSq = 36.f; // 6 m XZ fallback radius
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
					s_lastVehDriver.erase(mapVehicles[static_cast<std::size_t>(matchedIdx)].id);
				} else {
					if (!ent.nicknameUtf8.empty())
						occ.occupantNames.push_back(ent.nicknameUtf8);
					const int prio = RolePriority(ent.role, ent.roleName, isTeammate);
					if (!occ.occupied || prio > occ.priority) {
						occ.occupied = true;
						occ.priority = prio;
						occ.isPolice = (RobRunReader::RoleLabel(ent.role, ent.roleName) == "Police Officer");
						occ.color = GetInGameMapRoleColor(ent.role, ent.roleName, isTeammate);
						if (std::isfinite(ent.position.x) && std::isfinite(ent.position.z)) {
							occ.driverPos = ent.position;
							occ.hasDriverPos = true;
						}
						auto& ld = s_lastVehDriver[mapVehicles[static_cast<std::size_t>(matchedIdx)].id];
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

	// Track per-vehicle motion so far/unmatched vehicles that are actively moving
	// (or moved in the last 10 s) show their driver/owner's role color & name on the full map.
	struct VehicleMotion {
		Vector3 anchorPos{};
		std::uint64_t lastMovedMs = 0;
		bool initialized = false;
	};
	static std::unordered_map<int, VehicleMotion> s_vehMotion;
	static std::uint64_t s_vehMotionSession = 0;
	if (snapshot->sessionGeneration != s_vehMotionSession) {
		s_vehMotionSession = snapshot->sessionGeneration;
		s_vehMotion.clear();
	}
	constexpr std::uint64_t kVehRecentMoveMs = 10'000ULL;
	constexpr std::uint64_t kLastDriverMaxAgeMs = 120'000ULL;
	std::unordered_set<std::int32_t> trackedByVehicleEntityIds;

	// 2. Vehicles (cars & helicopters — 80-500 Hz live positions, colored by driver/owner role)
	if (cfg.InGameMapVehicles && vehCount > 0)
	{
		for (std::size_t vi = 0; vi < vehCount; ++vi) {
			const auto& mv = mapVehicles[vi];
			const auto& occ = vehOcc[vi];
			if (!occ.occupied &&
				(occ.localOccupied || (s_localPublicId > 0 && mv.ownerPublicId == s_localPublicId)))
				continue; // Game already marks our own vehicle on the map

			const Vector3& vehWorldPos = (occ.occupied && occ.hasDriverPos) ? occ.driverPos : mv.position;

			auto& mot = s_vehMotion[mv.id];
			if (!mot.initialized) {
				mot.anchorPos = vehWorldPos;
				mot.lastMovedMs = 0;
				mot.initialized = true;
			} else {
				const float mdx = vehWorldPos.x - mot.anchorPos.x;
				const float mdz = vehWorldPos.z - mot.anchorPos.z;
				if (mdx * mdx + mdz * mdz > 0.1225f) { // > 0.35 m movement
					mot.anchorPos = vehWorldPos;
					mot.lastMovedMs = vehNowMs;
				}
			}
			const bool recentlyMoved = (mot.lastMovedMs != 0) && (vehNowMs - mot.lastMovedMs <= kVehRecentMoveMs);
			const bool activeRemoteVeh = !occ.occupied && (mv.hasDriver || recentlyMoved);

			// Check remembered live driver (covers helicopters & stolen cars leaving 200-300 m bubble
			// AND recently exited parked vehicles so they keep a soft role tint + name).
			const LastVehicleDriver* remembered = nullptr;
			if (!occ.occupied) {
				auto it = s_lastVehDriver.find(mv.id);
				if (it != s_lastVehDriver.end()) {
					if (vehNowMs - it->second.stampMs <= kLastDriverMaxAgeMs) {
						remembered = &it->second;
						if (activeRemoteVeh && remembered->entityId >= 0)
							trackedByVehicleEntityIds.insert(remembered->entityId);
					} else {
						s_lastVehDriver.erase(it);
					}
				}
			}

			ImVec2 pt{};
			if (!worldToMap(vehWorldPos, pt)) continue;

			auto muteRoleColor = [](ImU32 col) -> ImU32 {
				// 100% opaque soft tint (55% role + 45% silver-grey) for parked/exited vehicles
				// so they stay visibly tinted red/blue/green without looking active or muddying on fuser.
				const int r = static_cast<int>(((col >> IM_COL32_R_SHIFT) & 0xFF) * 0.55f + 185.f * 0.45f);
				const int g = static_cast<int>(((col >> IM_COL32_G_SHIFT) & 0xFF) * 0.55f + 190.f * 0.45f);
				const int b = static_cast<int>(((col >> IM_COL32_B_SHIFT) & 0xFF) * 0.55f + 198.f * 0.45f);
				return IM_COL32(r, g, b, 255);
			};

			ImU32 vehCol = IM_COL32(210, 216, 224, 240);
			std::string remoteLabel;
			if (occ.occupied) {
				vehCol = occ.color;
			} else if (activeRemoteVeh) {
				if (remembered) {
					vehCol = GetInGameMapRoleColor(remembered->role, remembered->roleName, remembered->isTeammate);
					remoteLabel = remembered->nicknameUtf8;
				}
				if (remoteLabel.empty() && !mv.ownerNicknameUtf8.empty())
					remoteLabel = mv.ownerNicknameUtf8;
				if (!remembered && !mv.ownerRoleName.empty())
					vehCol = GetInGameMapRoleColor(0, mv.ownerRoleName, false);
			} else if (remembered || !mv.ownerRoleName.empty()) {
				// Recently exited or owned parked car: keep a soft role tint + name
				// so you can still tell whose car is parked there.
				ImU32 baseRole = IM_COL32(210, 216, 224, 255);
				if (remembered) {
					baseRole = GetInGameMapRoleColor(remembered->role, remembered->roleName, remembered->isTeammate);
					remoteLabel = remembered->nicknameUtf8;
				} else {
					baseRole = GetInGameMapRoleColor(0, mv.ownerRoleName, false);
				}
				if (remoteLabel.empty() && !mv.ownerNicknameUtf8.empty())
					remoteLabel = mv.ownerNicknameUtf8;
				vehCol = muteRoleColor(baseRole);
			}

			const bool isPoliceVeh =
				(occ.occupied && occ.isPolice) ||
				(!occ.occupied && remembered && RobRunReader::RoleLabel(remembered->role, remembered->roleName) == "Police Officer") ||
				(!occ.occupied && !remembered && !mv.ownerRoleName.empty() && RobRunReader::RoleLabel(0, mv.ownerRoleName) == "Police Officer");

			const float vehScale = scale * 0.90f;
			if (mv.kind == RobRunWorld::Kind::Helicopter) {
				if (!DrawMapSprite(dl, MapSpriteSlot::Helicopter, pt, 12.0f * vehScale, vehCol))
					DrawHeliIcon(dl, pt, vehScale, vehCol);
			} else {
				const MapSpriteSlot carSlot = isPoliceVeh ? MapSpriteSlot::PoliceCar : MapSpriteSlot::Car;
				const float carHalf = (isPoliceVeh ? 11.6f : 11.2f) * vehScale;
				if (!DrawMapSprite(dl, carSlot, pt, carHalf, vehCol))
					DrawCarIcon(dl, pt, vehScale, vehCol);
			}

			// Show occupant/owner name(s) on the fullscreen ('M') map above the car/heli icon.
			if (isFull && cfg.InGameMapPlayerNames && font) {
				std::string label;
				if (occ.occupied && !occ.occupantNames.empty()) {
					for (std::size_t ni = 0; ni < occ.occupantNames.size(); ++ni) {
						if (ni > 0) label += ", ";
						label += occ.occupantNames[ni];
					}
				} else if (!remoteLabel.empty()) {
					label = remoteLabel;
				}
				if (!label.empty()) {
					const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label.c_str());
					const ImVec2 textPos(pt.x - sz.x * 0.5f, pt.y - 11.0f * vehScale - sz.y - 2.f);
					dl->AddRectFilled(
						ImVec2(textPos.x - 5.f, textPos.y - 2.f),
						ImVec2(textPos.x + sz.x + 5.f, textPos.y + sz.y + 2.f),
						IM_COL32(26, 28, 32, 225), 3.5f);
					Esp::DrawOutlinedText(dl, textPos, vehCol, font, fontSize, label.c_str());
				}
			}
		}
	}

	// 3. Players (sleek directional chevron arrows; names on fullscreen map only)
	if (cfg.InGameMapPlayers)
	{
		for (std::size_t ei = 0; ei < snapshot->entities.size(); ++ei) {
			const auto& ent = snapshot->entities[ei];
			if (ent.entityId == localId || ent.isDead)
				continue;

			const bool isTeammate = ent.isPartyMember || ent.isClanMate;
			if (pcfg.HideTeammates && isTeammate)
				continue;

			float entYaw = ent.yaw;
			const float lookHorizSq = ent.lookDirection.x * ent.lookDirection.x + ent.lookDirection.z * ent.lookDirection.z;
			if (std::isfinite(lookHorizSq) && lookHorizSq > 0.01f)
				entYaw = std::atan2(ent.lookDirection.x, ent.lookDirection.z);

			ImVec2 pt{};
			if (!worldToMap(ent.position, pt))
				continue;

			const ImU32 col = GetInGameMapRoleColor(ent.role, ent.roleName, isTeammate);

			// When vehicle icons are enabled, players inside a vehicle are represented
			// cleanly by their role-colored car/heli icon (with occupant name(s) above)
			// instead of drawing an arrow on top of the vehicle.
			const bool inVeh = ent.inVehicle || (playerMatchedVeh[ei] >= 0);
			if (cfg.InGameMapVehicles && inVeh) {
				if (playerMatchedVeh[ei] < 0) {
					DrawCarIcon(dl, pt, scale, col);
					if (isFull && cfg.InGameMapPlayerNames && font && !ent.nicknameUtf8.empty()) {
						const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, ent.nicknameUtf8.c_str());
						const ImVec2 textPos(pt.x - sz.x * 0.5f, pt.y - 8.f * scale - sz.y - 2.f);
						dl->AddRectFilled(
							ImVec2(textPos.x - 5.f, textPos.y - 2.f),
							ImVec2(textPos.x + sz.x + 5.f, textPos.y + sz.y + 2.f),
							IM_COL32(26, 28, 32, 225), 3.5f);
						Esp::DrawOutlinedText(dl, textPos, col, font, fontSize, ent.nicknameUtf8.c_str());
					}
				}
				continue;
			}

			const float arrowR = (isFull ? 9.5f : 8.2f) * scale;
			if (std::isfinite(entYaw)) {
				DrawPlayerArrow(dl, pt, entYaw - rotRad, arrowR, col);
			} else {
				const float dotR = (isFull ? 6.0f : 5.2f) * scale;
				dl->AddCircleFilled(pt, dotR, col, 14);
				dl->AddCircle(pt, dotR, IM_COL32(26, 28, 32, 250), 14, 1.4f);
			}

			// Player names are only shown on the fullscreen ('M') map, never on the minimap.
			if (isFull && cfg.InGameMapPlayerNames && font && !ent.nicknameUtf8.empty()) {
				const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, ent.nicknameUtf8.c_str());
				const ImVec2 textPos(pt.x - sz.x * 0.5f, pt.y - arrowR - sz.y - 3.f);
				const float padX = 5.f;
				const float padY = 2.f;
				dl->AddRectFilled(
					ImVec2(textPos.x - padX, textPos.y - padY),
					ImVec2(textPos.x + sz.x + padX, textPos.y + sz.y + padY),
					IM_COL32(26, 28, 32, 225), 3.5f);
				Esp::DrawOutlinedText(dl, textPos, col, font, fontSize, ent.nicknameUtf8.c_str());
			}
		}
	}

	dl->PopClipRect();
}

void Shutdown()
{
	ReleaseMapSprites();
	if (g_mapSRV) { g_mapSRV->Release(); g_mapSRV = nullptr; }
	if (g_comInitialized) {
		CoUninitialize();
		g_comInitialized = false;
	}
}

const std::vector<std::string>& GetMapFiles()
{
	return g_mapFiles;
}

int GetCurrentMapIndex()
{
	std::lock_guard<std::mutex> lock(g_radarStateMutex);
	return g_currentMapIndex;
}

void RequestSetMap(int index)
{
	g_pendingMapIndex.store(index, std::memory_order_release);
}

void ProcessPendingMap()
{
	const int pending = g_pendingMapIndex.exchange(-1, std::memory_order_acq_rel);
	if (pending >= 0)
		SetMap(pending);
}

void SetMap(int index)
{
	if (index < 0 || index >= (int)g_mapFiles.size()) return;
	{
		std::lock_guard<std::mutex> lock(g_radarStateMutex);
		if (index == g_currentMapIndex) return;
	}

	if (LoadMapByName(g_mapFiles[index])) {
		{
			std::lock_guard<std::mutex> lock(g_radarStateMutex);
			g_currentMapIndex = index;
		}
		ConfigInstance.Radar.SelectedMap = g_mapFiles[index];
		if (ConfigInstance.Radar.AutoMapSelect) {
			CalibrateBoundsForMap(g_mapFiles[index]);
		}
	}
}

std::string GetCurrentMapName()
{
	std::lock_guard<std::mutex> lock(g_radarStateMutex);
	if (g_currentMapIndex >= 0 && g_currentMapIndex < (int)g_mapFiles.size())
		return g_mapFiles[g_currentMapIndex];
	return ConfigInstance.Radar.SelectedMap;
}

std::filesystem::path GetMapsDirectory()
{
	return g_mapsDir;
}

} // namespace Radar
