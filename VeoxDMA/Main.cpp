#include "pch.h"
#include "Memory.h"
#include "CheatFunction.h"
#include "CameraFinder.h"
#include "RobRunReader.h"
#include "PlayerEsp.h"
#include "WorldEsp.h"
#include "Aim/AimController.h"
#include "Input/InputManager.h"
#include "UI/VeoxMenu.h"
#include "Overlay/Overlay.h"
#include <atomic>
#include <mutex>

static std::atomic<bool> g_attached{ false };
static std::atomic<bool> g_stopAttach{ false };
static std::atomic<bool> g_pendingAttachToast{ false };
static std::thread g_attachThread;

static std::mutex g_statusMutex;
static std::string g_attachStatus = "Waiting for DMA card...";

static void SetAttachStatus(const std::string& text)
{
	std::lock_guard<std::mutex> lock(g_statusMutex);
	g_attachStatus = text;
}

static std::string GetAttachStatus()
{
	std::lock_guard<std::mutex> lock(g_statusMutex);
	return g_attachStatus;
}

static bool AttachGame()
{
	if (!TargetProcess.Init("ROB RUN.exe"))
	{
		SetAttachStatus("Waiting for ROB RUN...");
		return false;
	}

	size_t gaBase = TargetProcess.GetBaseAddress("GameAssembly.dll");
	size_t unityBase = TargetProcess.GetBaseAddress("UnityPlayer.dll");
	if (!gaBase || !unityBase)
	{
		SetAttachStatus("Failed to find game modules.");
		return false;
	}

	CameraFinder::Init(unityBase);
	RobRunReader::Init(gaBase, unityBase);
	printf("GameAssembly @ 0x%llX | UnityPlayer @ 0x%llX\n", gaBase, unityBase);
	// Resolve the game PC's gafAsyncKeyState once the DMA process context is
	// live. This can take a few seconds (kernel module scans), so it belongs on
	// the attach thread. Without it every aim key read returns false.
	const bool keyboardReady = Keyboard::InitKeyboard();
	printf("[Input] game PC keyboard %s\n",
		keyboardReady ? "ready" : "unavailable (host/device input only)");
	SetAttachStatus("Attached.");
	g_attached.store(true, std::memory_order_release);
	g_pendingAttachToast.store(true);
	return true;
}

static void StartAttachLoop()
{
	g_stopAttach.store(false);
	g_attachThread = std::thread([] {
		while (!g_stopAttach.load()) {
			if (AttachGame()) return;
			for (int i = 0; i < 30 && !g_stopAttach.load(); ++i)
				Sleep(100);
		}
	});
}

static void StopAttachLoop()
{
	g_stopAttach.store(true);
	if (g_attachThread.joinable()) g_attachThread.join();
}

static void TickOverlay()
{
	if (g_pendingAttachToast.exchange(false))
		Overlay::Notify("Attached to ROB RUN", 0xFF8CDC64u, 3.f); // IM_COL32(100,220,140,255)

	if (g_attached.load(std::memory_order_acquire))
		RobRunReader::RefreshCamera();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
	AllocConsole();
	FILE* fDummy;
	freopen_s(&fDummy, "CONIN$", "r", stdin);
	freopen_s(&fDummy, "CONOUT$", "w", stderr);
	freopen_s(&fDummy, "CONOUT$", "w", stdout);
	printf("VeoxDMA - Starting...\n");

	StartAttachLoop();

	Overlay::Callbacks callbacks;
	callbacks.init = [] {
		VeoxMenu::Init();
	};
	callbacks.tick = [] {
		TickOverlay();
	};
	callbacks.draw = [] {
		Aim::Tick();
		DrawPlayers();
		DrawWorldEntities();
		Aim::Draw();
		Aim::DrawDebug();
		if (Overlay::MenuOpen())
			VeoxMenu::Draw(g_attached.load(std::memory_order_acquire), GetAttachStatus());
	};
	callbacks.shutdown = [] {
		StopAttachLoop();
		RobRunReader::Shutdown();
	};
	callbacks.status = [] {
		return g_attached.load(std::memory_order_acquire) ? std::string("Attached") : GetAttachStatus();
	};

	const int result = Overlay::Run(hInstance, L"VeoxDMA", L"VeoxDMA", callbacks);
	// Run only invokes the shutdown callback on its normal teardown path. Join
	// here too so an early startup failure cannot leave a joinable thread for
	// the static destructor to terminate on.
	StopAttachLoop();
	RobRunReader::Shutdown();
	return result;
}
