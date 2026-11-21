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
#include "DebugLog.h"
#include "Esp/WeaponSprites.h"
#include "Aim/AimController.h"
#include "SDK/SkinChanger.h"
#include "Input/InputManager.h"
#include "Input/DeviceSession.h"
#include "UI/VeoxMenu.h"
#include "ImGUI/imgui.h"
#include "Overlay/Overlay.h"
#include "Overlay/Backdrop.h"
#include <atomic>
#include <mutex>

static std::atomic<bool> g_attached{ false };
static std::atomic<bool> g_stopAttach{ false };
static std::atomic<bool> g_pendingAttachToast{ false };
static std::atomic<bool> g_pendingReattach{ false };
static std::thread g_attachThread;
static HINSTANCE g_hInstance = nullptr;

static std::mutex g_statusMutex;
static std::string g_attachStatus = "Waiting for DMA card...";

// Serialises reader lifetime: Init() (attach thread) and Shutdown() (render
// thread on reattach/exit) reset the same Il2Cpp/RobRunCamera/CameraFinder
// statics, and the render thread must never be reading them while that runs.
static std::mutex g_lifecycleMutex;

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

	{
		std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
		CameraFinder::Init(unityBase);
		RobRunReader::Init(gaBase, unityBase);
		SkinChanger::Start();
	}
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
	Aim::StartAimWorker();
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

void RequestReattach()
{
	g_pendingReattach.store(true);
}

static void TickOverlay()
{
	if (g_pendingReattach.exchange(false))
	{
		Overlay::Notify("Re-attaching...", 0xFFFFCC44u, 2.f);
		// Drop the render thread's reader access before tearing anything down:
		// RefreshCamera() and the draw callbacks are gated on this flag.
		g_attached.store(false, std::memory_order_release);
		Aim::StopAimWorker();
		// Join the attach thread before taking the lifecycle lock. It may be
		// inside AttachGame holding that lock, so joining while holding it
		// would deadlock.
		StopAttachLoop();
		// An attach that completed during that join may have flipped the flag
		// back on and restarted the aim worker; undo both before shutdown.
		g_attached.store(false, std::memory_order_release);
		Aim::StopAimWorker();
		{
			std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
			SkinChanger::Stop();
			RobRunReader::Shutdown();
			TargetProcess.ResetProcess();
		}
		SetAttachStatus("Re-attaching...");
		StartAttachLoop();
	}

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	Aim::SetDisplaySize(display.x, display.y);
	if (g_attached.load(std::memory_order_acquire)) {
		RobRunReader::RefreshCamera();
	}

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
	DLOG("=== WinMain entry ===");

	StartAttachLoop();
	DLOG("AttachLoop started");

	// Apply saved settings before the menu is first drawn.
	ConfigFile::Load();
	DLOG("Config loaded");

	// Auto-connect saved aim device so it's ready before the first frame.
	{
		using Base::Hardware::DeviceKind;
		constexpr DeviceKind deviceKinds[] = { DeviceKind::Off, DeviceKind::KmboxUsb, DeviceKind::KmboxNet,
			DeviceKind::Makcu, DeviceKind::Controller, DeviceKind::Ferrum, DeviceKind::Medius,
			DeviceKind::KmboxNbPro };
		constexpr int baudRates[] = { 115200, 921600, 1500000, 2000000, 3000000, 4000000 };
		const auto& saved = ConfigInstance.Aim;
		if (saved.DeviceIndex > 0 && static_cast<unsigned>(saved.DeviceIndex) < std::size(deviceKinds)
			&& static_cast<unsigned>(saved.BaudIndex) < std::size(baudRates)) {
			Base::Hardware::DeviceConfig cfg;
			cfg.kind = deviceKinds[saved.DeviceIndex];
			cfg.port = saved.ComPort;
			cfg.ip = saved.IpAddr;
			cfg.networkPort = saved.NetPort;
			cfg.mac = saved.MacId;
			cfg.baud = baudRates[saved.BaudIndex];
			cfg.manualPort = saved.ManualPort || cfg.kind == DeviceKind::Ferrum;
			cfg.ferrumController = saved.FerrumController;
			Base::Hardware::Session().Connect(cfg);
		}
	}

	Overlay::Callbacks callbacks;
	callbacks.init = [] {
		DLOG("Overlay init callback");
		VeoxMenu::Init();
		Radar::Init();
		WeaponSprites::Init();
		Backdrop::Create(g_hInstance);
		Aim::StartAimWorker();
		DLOG("Overlay init callback done");
	};
	callbacks.tick = [] {
		TickOverlay();
	};
	callbacks.draw = [] {
		static int frameCount = 0;
		++frameCount;
		if (frameCount <= 3 || (frameCount % 500) == 0)
			DLOG("Draw frame #%d", frameCount);
		try {
			// Only draw game state while attached: during a reattach the reader
			// is being shut down and the last snapshot is stale.
			if (g_attached.load(std::memory_order_acquire)) {
				DrawPlayers();
				DrawBvhDebug();
				if (!ConfigInstance.Overlay.BattleMode) {
					DrawWorldEntities();
					DrawHouseInfo();
					Radar::Draw();
					Radar::DrawFullMap();
				}
			}
			Aim::Draw();
			Aim::DrawDebug();
			if (Overlay::MenuOpen())
				VeoxMenu::Draw(g_attached.load(std::memory_order_acquire), GetAttachStatus());
		} catch (const std::exception& e) {
			DLOG("Draw EXCEPTION frame #%d: %s", frameCount, e.what());
		} catch (...) {
			DLOG("Draw UNKNOWN EXCEPTION frame #%d", frameCount);
		}
	};
	callbacks.shutdown = [] {
		Aim::StopAimWorker();
		ConfigFile::Save();
		WeaponSprites::Shutdown();
		Radar::Shutdown();
		Backdrop::Destroy();
		StopAttachLoop();
		{
			std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
			SkinChanger::Stop();
			RobRunReader::Shutdown();
		}
	};
	callbacks.status = [] {
		return g_attached.load(std::memory_order_acquire) ? std::string("Attached") : GetAttachStatus();
	};

	const int result = Overlay::Run(hInstance, L"VeoxDMA", L"VeoxDMA", callbacks);
	// Run only invokes the shutdown callback on its normal teardown path. Join
	// here too so an early startup failure cannot leave a joinable thread for
	// the static destructor to terminate on.
	Aim::StopAimWorker();
	StopAttachLoop();
	RobRunReader::Shutdown();
	return result;
}
