#include "pch.h"
#include "Memory.h"
#include "CheatFunction.h"
#include "CameraFinder.h"
#include "RobRunReader.h"
#include "ConfigFile.h"
#include "ConfigInstance.h"
#include "PlayerEsp.h"
#include "WorldEsp.h"
#include "Esp/Radar.h"
#include "Esp/WeaponSprites.h"
#include "Aim/AimController.h"
#include "Input/InputManager.h"
#include "UI/VeoxMenu.h"
#include "Overlay/Overlay.h"
#include "Overlay/Backdrop.h"
#include <atomic>
#include <mutex>

static std::atomic<bool> g_attached{ false };
static std::atomic<bool> g_stopAttach{ false };
static std::atomic<bool> g_pendingAttachToast{ false };
static std::thread g_attachThread;
static HINSTANCE g_hInstance = nullptr;

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
		Overlay::Notify("Attached to ROB RUN", 0xFF8CDC64u, 3.f);

	// Battle mode suppresses world ESP/radar/money HUD so only player ESP is
	// shown. Toggle on the rising edge of the selected key ("Always" is not a
	// toggle key). Ignore while the menu is open to avoid accidental toggles
	// when changing the binding.
	static bool battleKeyWasHeld = false;
	if (!Overlay::MenuOpen()) {
		const int key = ConfigInstance.Overlay.BattleModeKey;
		const bool held = key >= 0 && key < Aim::KeyCount() - 1 && Aim::IsKeyHeld(key);
		if (held && !battleKeyWasHeld) {
			ConfigInstance.Overlay.BattleMode = !ConfigInstance.Overlay.BattleMode;
			Overlay::Notify(ConfigInstance.Overlay.BattleMode
				? "Battle Mode: ON (player ESP only)"
				: "Battle Mode: OFF", 0xFF8CDC64u, 2.f);
		}
		battleKeyWasHeld = held;
	} else {
		battleKeyWasHeld = false;
	}

	// Persist settings whenever the menu closes, so a user does not have to
	// remember a separate save step. Shutdown saves again as a backstop.
	static bool wasMenuOpen = false;
	const bool menuOpen = Overlay::MenuOpen();
	if (wasMenuOpen && !menuOpen)
		ConfigFile::Save();
	wasMenuOpen = menuOpen;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
	g_hInstance = hInstance;
	AllocConsole();
	FILE* fDummy;
	freopen_s(&fDummy, "CONIN$", "r", stdin);
	freopen_s(&fDummy, "CONOUT$", "w", stderr);
	freopen_s(&fDummy, "CONOUT$", "w", stdout);
	printf("VeoxDMA - Starting...\n");

	StartAttachLoop();

	// Apply saved settings before the menu is first drawn.
	ConfigFile::Load();

	Overlay::Callbacks callbacks;
	callbacks.init = [] {
		VeoxMenu::Init();
		Radar::Init();
		WeaponSprites::Init();
	};
	callbacks.tick = [] {
		TickOverlay();
	};
	callbacks.draw = [] {
		Aim::Tick();
		Aim::TickRecoilSpread();
		// Read the camera after the aim has written its angles. The ESP must use
		// the same camera state the game will render this frame; reading before
		// the write projected with last frame's angle, which made skeletons drift
		// while memory aim was active.
		if (g_attached.load(std::memory_order_acquire))
			RobRunReader::RefreshCamera();
		DrawPlayers();
		if (!ConfigInstance.Overlay.BattleMode) {
			DrawWorldEntities();
			Radar::Draw();
		}
		Aim::Draw();
		Aim::DrawDebug();
		if (Overlay::MenuOpen())
			VeoxMenu::Draw(g_attached.load(std::memory_order_acquire), GetAttachStatus());
	};
	callbacks.shutdown = [] {
		ConfigFile::Save();
		WeaponSprites::Shutdown();
		Radar::Shutdown();
		Backdrop::Destroy();
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
