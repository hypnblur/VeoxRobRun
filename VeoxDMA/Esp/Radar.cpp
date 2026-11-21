#include "pch.h"
#include "Radar.h"
#include "EspDraw.h"
#include "IconSprites.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
#include "RobRunMemory.h"
#include "../Overlay/Overlay.h"
#include "../ImGUI/imgui.h"
#include <d3d11.h>
#include <wincodec.h>
#include <cmath>
#include <filesystem>

namespace {

ID3D11ShaderResourceView* g_mapSRV = nullptr;
int g_mapWidth = 0;
int g_mapHeight = 0;

std::vector<std::string> g_mapFiles;
int g_currentMapIndex = -1;
std::filesystem::path g_mapsDir;

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
	converter->GetSize(&w, &h);
	g_mapWidth = (int)w;
	g_mapHeight = (int)h;

	std::vector<BYTE> pixels(w * h * 4);
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
	(void)hr;

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
		if (cfg.RotateWithCamera && haveLocal) {
			float worldW = cfg.WorldMaxX - cfg.WorldMinX;
			float worldH = cfg.WorldMaxZ - cfg.WorldMinZ;
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
			if (haveLocal) {
				float worldW = cfg.WorldMaxX - cfg.WorldMinX;
				float worldH = cfg.WorldMaxZ - cfg.WorldMinZ;
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

void Shutdown()
{
	if (g_mapSRV) { g_mapSRV->Release(); g_mapSRV = nullptr; }
}

const std::vector<std::string>& GetMapFiles()
{
	return g_mapFiles;
}

int GetCurrentMapIndex()
{
	return g_currentMapIndex;
}

void SetMap(int index)
{
	if (index < 0 || index >= (int)g_mapFiles.size()) return;
	if (index == g_currentMapIndex) return;

	if (LoadMapByName(g_mapFiles[index])) {
		g_currentMapIndex = index;
		ConfigInstance.Radar.SelectedMap = g_mapFiles[index];
		if (ConfigInstance.Radar.AutoMapSelect) {
			CalibrateBoundsForMap(g_mapFiles[index]);
		}
	}
}

const std::string& GetCurrentMapName()
{
	static const std::string s_empty = "";
	if (g_currentMapIndex >= 0 && g_currentMapIndex < (int)g_mapFiles.size())
		return g_mapFiles[g_currentMapIndex];
	return ConfigInstance.Radar.SelectedMap.empty() ? s_empty : ConfigInstance.Radar.SelectedMap;
}

std::filesystem::path GetMapsDirectory()
{
	return g_mapsDir;
}

} // namespace Radar
