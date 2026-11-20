#include "pch.h"
#include "WeaponSprites.h"
#include "../Overlay/Overlay.h"
#include <d3d11.h>
#include <wincodec.h>
#include <filesystem>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace {

struct CachedSprite {
	ID3D11ShaderResourceView* srv = nullptr;
	int width = 0;
	int height = 0;
};

std::unordered_map<std::string, CachedSprite> g_cache;
std::filesystem::path g_spritesDir;
bool g_scanned = false;

static std::string ToLower(const std::string& s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return out;
}

static ID3D11ShaderResourceView* LoadPng(const std::wstring& path,
	ID3D11Device* device, int& w, int& h)
{
	w = 0; h = 0;
	IWICImagingFactory* factory = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
		CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if (FAILED(hr)) return nullptr;

	IWICBitmapDecoder* decoder = nullptr;
	hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr,
		GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
	if (FAILED(hr)) { factory->Release(); return nullptr; }

	IWICBitmapFrameDecode* frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr)) { decoder->Release(); factory->Release(); return nullptr; }

	IWICFormatConverter* converter = nullptr;
	hr = factory->CreateFormatConverter(&converter);
	if (FAILED(hr)) {
		frame->Release(); decoder->Release(); factory->Release();
		return nullptr;
	}

	hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut);
	if (FAILED(hr)) {
		converter->Release(); frame->Release();
		decoder->Release(); factory->Release();
		return nullptr;
	}

	UINT pw = 0, ph = 0;
	converter->GetSize(&pw, &ph);
	w = (int)pw; h = (int)ph;

	std::vector<BYTE> pixels(pw * ph * 4);
	hr = converter->CopyPixels(nullptr, pw * 4, (UINT)pixels.size(), pixels.data());
	converter->Release(); frame->Release();
	decoder->Release(); factory->Release();
	if (FAILED(hr)) return nullptr;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = pw; desc.Height = ph;
	desc.MipLevels = 1; desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA sub{};
	sub.pSysMem = pixels.data();
	sub.SysMemPitch = pw * 4;

	ID3D11Texture2D* tex = nullptr;
	hr = device->CreateTexture2D(&desc, &sub, &tex);
	if (FAILED(hr)) return nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView* srv = nullptr;
	hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
	tex->Release();
	if (FAILED(hr)) return nullptr;

	return srv;
}

static void ScanDirectory(ID3D11Device* device)
{
	if (g_scanned) return;
	g_scanned = true;

	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	g_spritesDir = std::filesystem::path(exePath).parent_path() / "weapons";

	if (!std::filesystem::exists(g_spritesDir)) {
		printf("[WeaponSprites] No weapons/ folder found\n");
		return;
	}

	int count = 0;
	for (const auto& entry : std::filesystem::directory_iterator(g_spritesDir)) {
		if (!entry.is_regular_file()) continue;
		auto ext = entry.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		if (ext != ".png") continue;

		std::string stem = entry.path().stem().string();
		std::string key = ToLower(stem);

		int w = 0, h = 0;
		auto* srv = LoadPng(entry.path().wstring(), device, w, h);
		if (srv) {
			g_cache[key] = { srv, w, h };
			++count;
		}
	}
	if (count > 0)
		printf("[WeaponSprites] Loaded %d weapon sprites\n", count);
}

} // anonymous namespace

namespace WeaponSprites {

void Init()
{
	auto* device = Overlay::GetDevice();
	if (device) ScanDirectory(device);
}

void Shutdown()
{
	for (auto& [k, v] : g_cache)
		if (v.srv) v.srv->Release();
	g_cache.clear();
	g_scanned = false;
}

SpriteInfo Get(const std::string& weaponName)
{
	if (!g_scanned) {
		auto* device = Overlay::GetDevice();
		if (device) ScanDirectory(device);
	}

	std::string key = ToLower(weaponName);
	auto it = g_cache.find(key);
	if (it != g_cache.end())
		return { it->second.srv, it->second.width, it->second.height };

	// Try matching with spaces replaced by underscores
	std::string alt = key;
	std::replace(alt.begin(), alt.end(), ' ', '_');
	it = g_cache.find(alt);
	if (it != g_cache.end())
		return { it->second.srv, it->second.width, it->second.height };

	return {};
}

} // namespace WeaponSprites
