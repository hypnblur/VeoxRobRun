#include "pch.h"
#include "Radar.h"
#include "ConfigInstance.h"
#include "RobRunReader.h"
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

	if (rotate) {
		float s = std::sin(-yaw);
		float c = std::cos(-yaw);
		float rx = dx * c - dz * s;
		float rz = dx * s + dz * c;
		dx = rx;
		dz = rz;
	}

	float scale = radarSize / std::max(worldW, worldH) * zoom;
	return ImVec2(radarCenter.x + dx * scale, radarCenter.y + dz * scale);
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

void Draw()
{
	if (!ConfigInstance.Radar.Enabled) return;
	if (!g_mapSRV) return;

	const auto snapshot = RobRunReader::GetSnapshot();
	const Vector3 localPos = snapshot ? snapshot->localPosition : Vector3{};
	const bool haveLocal = snapshot && !localPos.IsZero();

	float localYaw = 0.f;
	if (snapshot) {
		for (const auto& ent : snapshot->entities) {
			if (ent.entityId == snapshot->localEntityId) {
				localYaw = ent.yaw;
				break;
			}
		}
	}

	const auto& cfg = ConfigInstance.Radar;
	const float radarSize = cfg.Size;

	// Restore the last on-screen placement, and follow the Size setting when
	// the user changes it (the window itself is still freely resizable).
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
		ImGui::SetNextWindowSize(ImVec2(radarSize + 16.f, radarSize + 36.f),
			ImGuiCond_FirstUseEver);
	}
	if (s_lastSizeSetting >= 0.f && s_lastSizeSetting != cfg.Size)
		ImGui::SetNextWindowSize(ImVec2(radarSize + 16.f, radarSize + 36.f), ImGuiCond_Always);
	s_lastSizeSetting = cfg.Size;

	ImGui::SetNextWindowBgAlpha(cfg.Opacity);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 12, 18, 220));
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(40, 80, 160, 120));
	ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(18, 20, 30, 255));
	ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(30, 120, 255, 200));

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
			uvA, ImVec2(uvB.x, uvA.y), uvB, ImVec2(uvA.x, uvB.y));
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

			dl->AddImage((ImTextureID)g_mapSRV, radarMin, radarMax, uvA, uvB);
		}
		else {
			dl->AddImage((ImTextureID)g_mapSRV, radarMin, radarMax,
				ImVec2(0.f, 0.f), ImVec2(1.f, 1.f));
		}
	}

	dl->AddCircle(radarCenter, side * 0.5f, IM_COL32(40, 80, 160, 180), 64, 2.f);

	if (haveLocal && snapshot) {
		if (snapshot->world && snapshot->world->valid) {
			for (const auto& ent : snapshot->world->entities) {
				bool show = false;
				ImU32 dotCol = IM_COL32(255, 255, 255, 200);
				float dotSize = 3.f;

				switch (ent.kind) {
				case RobRunWorld::Kind::Car:
				case RobRunWorld::Kind::Helicopter:
				case RobRunWorld::Kind::Vehicle:
					if (cfg.ShowVehicles) {
						show = true;
						dotCol = IM_COL32(120, 190, 255, 230);
						dotSize = 4.f;
					}
					break;
				default:
					if (cfg.ShowLoot) {
						show = true;
						dotCol = IM_COL32(255, 190, 80, 180);
						dotSize = 2.5f;
					}
					break;
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

				ImVec2 pos = WorldToRadar(ent.position, localPos, localYaw,
					cfg.RotateWithCamera, side, radarCenter, cfg.Zoom);

				float ddx = pos.x - radarCenter.x;
				float ddy = pos.y - radarCenter.y;
				if (ddx * ddx + ddy * ddy > (side * 0.5f) * (side * 0.5f)) continue;

				dl->AddCircleFilled(pos, 4.f, IM_COL32(255, 50, 50, 255));
				dl->AddCircle(pos, 4.f, IM_COL32(0, 0, 0, 200), 0, 1.f);
			}
		}

		dl->AddCircleFilled(radarCenter, 5.f, IM_COL32(50, 220, 100, 255));
		dl->AddCircle(radarCenter, 5.f, IM_COL32(0, 0, 0, 200), 0, 1.5f);

		if (!cfg.RotateWithCamera) {
			float dirLen = 12.f;
			float s = std::sin(localYaw);
			float c = std::cos(localYaw);
			ImVec2 dirEnd(radarCenter.x + s * dirLen, radarCenter.y - c * dirLen);
			dl->AddLine(radarCenter, dirEnd, IM_COL32(50, 220, 100, 255), 2.f);
		}
		else {
			ImVec2 dirEnd(radarCenter.x, radarCenter.y - 12.f);
			dl->AddLine(radarCenter, dirEnd, IM_COL32(50, 220, 100, 255), 2.f);
		}
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
	}
}

} // namespace Radar
