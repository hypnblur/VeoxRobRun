#include "pch.h"
#include "IconSprites.h"
#include "IconIndex.h"
#include "../Overlay/Overlay.h"
#include <d3d11.h>
#include <wincodec.h>
#include <unordered_map>
#include <string>
#include <vector>

namespace {

typedef BOOL(WINAPI* PFN_OrbitGetIcon)(const char*, const void**, DWORD*);
typedef DWORD(WINAPI* PFN_OrbitGetIconCount)(void);

HMODULE            g_dll      = nullptr;
PFN_OrbitGetIcon   g_getIcon  = nullptr;
DWORD              g_iconCount = 0;
std::wstring       g_root;      // icon root with a trailing separator, empty if none
bool               g_ready    = false;

struct CachedSprite {
    ID3D11ShaderResourceView* srv = nullptr;
    int width  = 0;
    int height = 0;
    bool tried = false;   // a failure is cached as hard as a success
};

std::unordered_map<std::string, CachedSprite> g_cache;

static ID3D11ShaderResourceView* DecodePng(
    const void* pngData, DWORD pngSize, ID3D11Device* device, int& w, int& h)
{
    w = 0; h = 0;

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, pngSize);
    if (!hMem) { factory->Release(); return nullptr; }
    void* pMem = GlobalLock(hMem);
    if (!pMem) { GlobalFree(hMem); factory->Release(); return nullptr; }
    std::memcpy(pMem, pngData, pngSize);
    GlobalUnlock(hMem);

    IStream* stream = nullptr;
    hr = CreateStreamOnHGlobal(hMem, TRUE, &stream);
    if (FAILED(hr)) { GlobalFree(hMem); factory->Release(); return nullptr; }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromStream(stream, nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        stream->Release(); factory->Release();
        return nullptr;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        decoder->Release(); stream->Release(); factory->Release();
        return nullptr;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        frame->Release(); decoder->Release();
        stream->Release(); factory->Release();
        return nullptr;
    }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        converter->Release(); frame->Release();
        decoder->Release(); stream->Release(); factory->Release();
        return nullptr;
    }

    UINT pw = 0, ph = 0;
    hr = converter->GetSize(&pw, &ph);
    if (FAILED(hr) || pw == 0 || ph == 0 || pw > 16384 || ph > 16384) {
        converter->Release(); frame->Release();
        decoder->Release(); stream->Release(); factory->Release();
        return nullptr;
    }

    const std::size_t pixelBytes = static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph) * 4u;
    std::vector<BYTE> pixels(pixelBytes);
    hr = converter->CopyPixels(nullptr, pw * 4,
        static_cast<UINT>(pixels.size()), pixels.data());
    converter->Release(); frame->Release();
    decoder->Release(); stream->Release(); factory->Release();
    if (FAILED(hr)) return nullptr;

    w = static_cast<int>(pw);
    h = static_cast<int>(ph);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = pw;
    desc.Height           = ph;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem     = pixels.data();
    sub.SysMemPitch = pw * 4;

    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&desc, &sub, &tex);
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                 = desc.Format;
    srvDesc.ViewDimension          = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels    = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(tex, &srvDesc, &srv);
    tex->Release();
    if (FAILED(hr)) return nullptr;

    return srv;
}

// Index paths use forward slashes and are relative to the icon root.
static bool LoadPngFile(const std::string& relPath, std::vector<BYTE>& out)
{
    std::wstring full = g_root;
    for (char c : relPath)
        full.push_back(c == '/' ? L'\\' : (wchar_t)(unsigned char)c);

    HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 22)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

static std::wstring ExeDir()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    const auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos + 1);
    return dir;
}

static bool HasIcons(const std::wstring& root)
{
    const std::wstring probe = root + L"Assets\\Content\\UI\\Sprites";
    return GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The DLL stores each icon under an RC-safe version of the sprite stem. rc.exe
// cannot parse a quoted name at all (it keeps the quotes, and a space breaks
// the line), so the generator uses bare identifiers: only [A-Za-z0-9_] survives,
// everything else becomes '_', and RC uppercases the result. This must stay in
// step with tools/loot/gen_orbit_icons.py.
static void SanitizeName(const char* in, char* out, std::size_t cap)
{
    if (cap == 0) return;
    std::size_t i = 0;
    for (; in[i] && i + 1 < cap; ++i) {
        unsigned char c = (unsigned char)in[i];
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_';
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        out[i] = safe ? (char)c : '_';
    }
    out[i] = 0;
}

} // namespace

namespace IconSprites {

void Init()
{
    if (g_ready) return;
    g_ready = true;

    const std::wstring exeDir = ExeDir();
    const std::wstring candidates[] = {
        exeDir + L"icons\\",
        exeDir + L"..\\..\\..\\tools\\loot\\icons\\",   // running from Build\<cfg>\VeoxDMA
    };
    for (const auto& c : candidates) {
        if (HasIcons(c)) { g_root = c; break; }
    }

    g_dll = LoadLibraryW((exeDir + L"OrbitResources.dll").c_str());
    if (g_dll) {
        g_getIcon = reinterpret_cast<PFN_OrbitGetIcon>(
            GetProcAddress(g_dll, "OrbitGetIcon"));
        const auto getCount = reinterpret_cast<PFN_OrbitGetIconCount>(
            GetProcAddress(g_dll, "OrbitGetIconCount"));
        if (getCount) g_iconCount = getCount();
        if (!g_getIcon) {
            FreeLibrary(g_dll);
            g_dll = nullptr;
            g_iconCount = 0;
        }
    }

    LOG("[IconSprites] dll=%d embedded=%lu root=%ls\n",
        g_dll ? 1 : 0, g_iconCount, g_root.empty() ? L"<none>" : g_root.c_str());
}

int EmbeddedCount()
{
    return (int)g_iconCount;
}

bool UsingEmbedded()
{
    return g_getIcon != nullptr && g_iconCount > 0;
}

void Shutdown()
{
    for (auto& [_, c] : g_cache)
        if (c.srv) c.srv->Release();
    g_cache.clear();

    if (g_dll) {
        FreeLibrary(g_dll);
        g_dll     = nullptr;
        g_getIcon = nullptr;
    }
    g_root.clear();
    g_ready = false;
}

const wchar_t* Root()
{
    return g_root.empty() ? L"" : g_root.c_str();
}

SpriteInfo Get(const char* spriteName)
{
    if (!spriteName || !spriteName[0])
        return {};

    auto it = g_cache.find(spriteName);
    if (it != g_cache.end())
        return { it->second.srv, it->second.width, it->second.height };

    CachedSprite cached{};

    auto* device = Overlay::GetDevice();
    if (!device) return {};   // not a permanent failure: retry once the device exists

    // Primary source: the embedded icon set in OrbitResources.dll.
    if (g_getIcon) {
        char name[256]{};
        SanitizeName(spriteName, name, std::size(name));

        const void* data = nullptr;
        DWORD size = 0;
        if (g_getIcon(name, &data, &size) && data && size) {
            int w = 0, h = 0;
            cached.srv = DecodePng(data, size, device, w, h);
            cached.width = w;
            cached.height = h;
        }
    }

    // Fallback for a build that ran without the DLL: read the exported sprites
    // from the icons root, addressed by the generated index. A sprite can
    // appear several times there; take the first row naming a real file.
    if (!cached.srv && !g_root.empty()) {
        const char* relPath = nullptr;
        for (const auto& e : robrun::icons::kIcons) {
            if (e.sprite == spriteName && !e.path.empty()) {
                relPath = e.path.data();
                break;
            }
        }
        if (relPath) {
            std::vector<BYTE> file;
            if (LoadPngFile(relPath, file)) {
                int w = 0, h = 0;
                cached.srv = DecodePng(file.data(), (DWORD)file.size(), device, w, h);
                cached.width = w;
                cached.height = h;
            }
        }
    }

    cached.tried = true;
    g_cache[spriteName] = cached;
    return { cached.srv, cached.width, cached.height };
}

} // namespace IconSprites
