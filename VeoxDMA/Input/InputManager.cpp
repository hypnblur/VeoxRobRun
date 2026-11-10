#include "pch.h"
#include "InputManager.h"
#include "Memory.h"
#include <atomic>
#include <chrono>
#include <mutex>

#ifdef NDEBUG
#define INPUT_LOG(...) ((void)0)
#else
#define INPUT_LOG(...) printf(__VA_ARGS__)
#endif

namespace Keyboard
{
	uint64_t gafAsyncKeyStateExport = 0;
	uint8_t state_bitmap[64]{ };
	uint8_t previous_state_bitmap[256 / 8]{ };
	uint64_t win32kbase = 0;
	int win_logon_pid = 0;
	uint64_t cursorSessionBase = 0; // Win11 user session state, 0 on Win10
}

// gafAsyncKeyState discovery, ported from Pure-H1Z1 (InputManager.cpp). Win11
// moved it into per-session state whose layout shifts between builds, so it is
// found by signature in win32k / win32kbase rather than by fixed offsets.
namespace
{
	bool IsKernelVa(uint64_t va) { return va > 0x7FFFFFFFFFFF; }

	template<typename T>
	bool ReadExact(int pid, uint64_t va, T& value)
	{
		DWORD cb = 0;
		return VMMDLL_MemReadEx(TargetProcess.vHandle, pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY, va,
			(PBYTE)&value, sizeof(T), &cb, VMMDLL_FLAG_NOCACHE) && cb == sizeof(T);
	}

	uint8_t HexByte(const char* hex)
	{
		char b[3] = { hex[0], hex[1], 0 };
		return (uint8_t)strtoul(b, nullptr, 16);
	}

	// Chunked and zero-padded: win32k has paged-out pages, so a single
	// whole-module read (Memory::FindSignature) fails outright.
	uint64_t ScanKernelModule(const char* signature, uint64_t start, uint64_t end, int pid)
	{
		std::vector<int> pattern;
		for (const char* p = signature; *p;)
		{
			while (*p == ' ') ++p;
			if (!*p) break;
			if (*p == '?') { pattern.push_back(-1); while (*p == '?') ++p; }
			else { pattern.push_back(HexByte(p)); p += 2; }
		}
		auto matches = [&](const uint8_t* bytes) {
			for (size_t i = 0; i < pattern.size(); ++i)
				if (pattern[i] >= 0 && bytes[i] != (uint8_t)pattern[i])
					return false;
			return true;
		};

		constexpr uint64_t kChunk = 64 * 1024;
		const uint64_t overlap = pattern.size() - 1;
		std::vector<uint8_t> buffer((size_t)(kChunk + overlap));
		std::vector<uint8_t> verified(pattern.size());
		for (uint64_t chunk = start; chunk < end; chunk += kChunk)
		{
			const DWORD requested = (DWORD)(std::min)(end - chunk, kChunk + overlap);
			std::fill(buffer.begin(), buffer.end(), 0);
			DWORD cb = 0;
			if (VMMDLL_MemReadEx(TargetProcess.vHandle, pid, chunk, buffer.data(), requested, &cb,
				VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_ZEROPAD_ON_FAIL) && requested >= pattern.size())
			{
				for (size_t off = 0; off + pattern.size() <= requested; ++off)
				{
					if (!matches(buffer.data() + off))
						continue;
					// Re-read exactly so zero padding cannot fake a match.
					DWORD vcb = 0;
					if (VMMDLL_MemReadEx(TargetProcess.vHandle, pid, chunk + off, verified.data(), (DWORD)verified.size(),
						&vcb, VMMDLL_FLAG_NOCACHE) && vcb == verified.size() && matches(verified.data()))
						return chunk + off;
				}
			}
		}
		return 0;
	}

	bool FindModule(int pid, const char* name, uint64_t& base, uint32_t& size)
	{
		PVMMDLL_MAP_MODULEENTRY m = nullptr;
		if (!VMMDLL_Map_GetModuleFromNameU(TargetProcess.vHandle, pid, (LPSTR)name, &m, VMMDLL_MODULE_FLAG_NORMAL))
			return false;
		base = m->vaBase;
		size = m->cbImageSize;
		VMMDLL_MemFree(m);
		return true;
	}

	// A resolved address only counts once the 64-byte bitmap reads back whole.
	bool PrimeBitmap()
	{
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			DWORD cb = 0;
			if (VMMDLL_MemReadEx(TargetProcess.vHandle, Keyboard::win_logon_pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY,
				Keyboard::gafAsyncKeyStateExport, Keyboard::state_bitmap, 64, &cb, VMMDLL_FLAG_NOCACHE) && cb == 64)
				return true;
			Sleep(5);
		}
		return false;
	}

	bool InitFromEat()
	{
		PVMMDLL_MAP_EAT eat_map = NULL;
		if (VMMDLL_Map_GetEATU(TargetProcess.vHandle, Keyboard::win_logon_pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY, (LPSTR)"win32kbase.sys", &eat_map)
			&& eat_map->dwVersion == VMMDLL_MAP_EAT_VERSION)
		{
			for (DWORD i = 0; i < eat_map->cMap; i++)
			{
				if (strcmp(eat_map->pMap[i].uszFunction, "gafAsyncKeyState") == 0)
				{
					Keyboard::gafAsyncKeyStateExport = eat_map->pMap[i].vaFunction;
					break;
				}
			}
		}
		if (eat_map)
			VMMDLL_MemFree(eat_map);
		if (IsKernelVa(Keyboard::gafAsyncKeyStateExport) && PrimeBitmap())
			return true;
		INPUT_LOG("[Input] win32kbase.sys EAT: gafAsyncKeyState %s\n",
			IsKernelVa(Keyboard::gafAsyncKeyStateExport) ? "found but unreadable" : "not found");
		Keyboard::gafAsyncKeyStateExport = 0;
		return false;
	}

	bool InitFromSession(ULONG64 build)
	{
		DWORD session = 0;
		{
			VMMDLL_PROCESS_INFORMATION info{};
			info.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
			info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
			SIZE_T cb = sizeof(info);
			if (VMMDLL_ProcessGetInformation(TargetProcess.vHandle, Keyboard::win_logon_pid, &info, &cb))
				session = info.win.dwSessionId;
		}

		const bool is24H2 = build >= 26100;
		const char* mods[2] = { is24H2 ? "win32k.sys" : "win32ksgd.sys", is24H2 ? "win32ksgd.sys" : "win32k.sys" };
		const char* failure = "no csrss.exe";

		for (int pid : TargetProcess.GetPidListFromName("csrss.exe"))
		{
			VMMDLL_PROCESS_INFORMATION ci{};
			ci.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
			ci.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
			SIZE_T ccb = sizeof(ci);
			DWORD csrssSession = 0;
			if (VMMDLL_ProcessGetInformation(TargetProcess.vHandle, pid, &ci, &ccb))
				csrssSession = ci.win.dwSessionId;
			INPUT_LOG("[Input] csrss pid %d session %lu\n", pid, csrssSession);

			for (const char* mod : mods)
			{
				uint64_t modBase = 0;
				uint32_t modSize = 0;
				if (!FindModule(pid, mod, modBase, modSize))
				{
					INPUT_LOG("[Input]   %s not found in csrss %d\n", mod, pid);
					failure = "win32k module not mapped in csrss";
					continue;
				}
				INPUT_LOG("[Input]   %s found at 0x%llX size 0x%X in csrss %d\n", mod, modBase, modSize, pid);

				// Instruction loading the session-global slot table.
				struct SessionSig { const char* pat; bool decrementSession; };
				const SessionSig sigs[] = {
					{ "48 8B 05 ? ? ? ? FF C9 48 8B 04 C8", true },  // 24H2
					{ "48 8B 05 ? ? ? ? 48 8B 04 C8", false },       // 23H2
					{ "48 8B 05 ? ? ? ? FF C9", true },               // 24H2 short
				};
				std::vector<std::pair<uint64_t, bool>> slotTables;
				for (int si = 0; si < 3; ++si)
				{
					const auto& s = sigs[si];
					const uint64_t insn = ScanKernelModule(s.pat, modBase, modBase + modSize, pid);
					int32_t rel = 0;
					INPUT_LOG("[Input]   sig%d: %s at 0x%llX\n", si, insn ? "HIT" : "miss", insn);
					if (insn && ReadExact(pid, insn + 3, rel))
					{
						INPUT_LOG("[Input]   slot table at 0x%llX\n", insn + 7 + rel);
						slotTables.push_back({ insn + 7 + rel, s.decrementSession });
						break;
					}
				}
				if (slotTables.empty() && is24H2 && strcmp(mod, "win32k.sys") == 0)
				{
					// Known 24H2 sites, either the load instruction or the table itself.
					for (uint32_t rva : { 0x824F0u, 0x82538u })
					{
						const uint64_t site = modBase + rva;
						uint8_t head[3]{};
						int32_t rel = 0;
						if (!ReadExact(pid, site, head))
							continue;
						if (head[0] == 0x48 && head[1] == 0x8B && head[2] == 0x05)
						{
							if (ReadExact(pid, site + 3, rel))
								slotTables.push_back({ site + 7 + rel, true });
						}
						else
							slotTables.push_back({ site, true });
					}
				}

				// Session state candidates: winlogon's session first, then 0..3.
				std::vector<uint64_t> states;
				for (const auto& [slots, dec] : slotTables)
				{
					uint64_t slotPtr = 0;
					if (!IsKernelVa(slots) || !ReadExact(pid, slots, slotPtr) || !IsKernelVa(slotPtr))
						continue;
					INPUT_LOG("[Input]   slotPtr=0x%llX\n", slotPtr);

					// Standard: slotPtr -> array of pointers -> each deref to session state
					for (uint64_t i = 0; i < 8; ++i)
					{
						uint64_t slotValue = 0, state = 0;
						if (!ReadExact(pid, slotPtr + 8 * i, slotValue) || !IsKernelVa(slotValue))
							continue;
						if (ReadExact(pid, slotValue, state) && IsKernelVa(state))
						{
							INPUT_LOG("[Input]   slot[%llu] -> 0x%llX -> state 0x%llX\n", i, slotValue, state);
							if (std::find(states.begin(), states.end(), state) == states.end())
								states.push_back(state);
						}
						else
						{
							// 25H2: slotValue might BE the session state directly (no double deref)
							INPUT_LOG("[Input]   slot[%llu] -> 0x%llX (trying as direct state)\n", i, slotValue);
							if (std::find(states.begin(), states.end(), slotValue) == states.end())
								states.push_back(slotValue);
						}
					}

					// Also try slotPtr itself as session state (no array at all)
					if (states.empty())
					{
						INPUT_LOG("[Input]   trying slotPtr itself as session state\n");
						states.push_back(slotPtr);
					}
				}
				INPUT_LOG("[Input]   slotTables=%zu states=%zu\n", slotTables.size(), states.size());
				if (states.empty())
				{
					failure = slotTables.empty() ? "session signature not found" : "no readable session slot";
					continue;
				}

				// gafAsyncKeyState offset inside the session state.
				uint64_t kbBase = 0;
				uint32_t kbSize = 0;
				if (!FindModule(pid, "win32kbase.sys", kbBase, kbSize))
				{
					failure = "win32kbase.sys not mapped in csrss";
					continue;
				}
				struct OffSig { const char* pat; int dispAt; };
				const OffSig offSigs[] = {
					{ "48 8D 90 ? ? ? ? E8 ? ? ? ? 0F 57 C0", 3 },
					{ "B9 00 80 FF FF ? 22 B4 ? ? ? ? ? 41", 9 },
					{ "40 0F B6 ? 48 C1 E9 02 8A 94 08 ? ? ? ?", 11 },
				};
				uint32_t gafOffset = 0;
				for (const auto& s : offSigs)
				{
					const uint64_t hit = ScanKernelModule(s.pat, kbBase, kbBase + kbSize, pid);
					uint32_t off = 0;
					if (hit && ReadExact(pid, hit + s.dispAt, off) && off >= 0x2000 && off <= 0x8000)
					{
						gafOffset = off;
						break;
					}
				}
				if (!gafOffset)
				{
					failure = "gafAsyncKeyState offset signature not found";
					continue;
				}

				for (uint64_t state : states)
				{
					Keyboard::gafAsyncKeyStateExport = state + gafOffset;
					if (PrimeBitmap())
					{
						Keyboard::cursorSessionBase = state;
						INPUT_LOG("[Input] gafAsyncKeyState 0x%llX (session state 0x%llX + 0x%X, %s)\n",
							Keyboard::gafAsyncKeyStateExport, state, gafOffset, mod);
						return true;
					}
				}
				Keyboard::gafAsyncKeyStateExport = 0;
				failure = "bitmap unreadable at every session candidate";
			}
		}
		INPUT_LOG("[Input] Win11 gafAsyncKeyState lookup failed: %s (session %lu)\n", failure, session);
		return false;
	}
}

bool Keyboard::InitKeyboard()
{
	gafAsyncKeyStateExport = 0;
	cursorSessionBase = 0;

	ULONG64 build = 0;
	if (!VMMDLL_ConfigGet(TargetProcess.vHandle, VMMDLL_OPT_WIN_VERSION_BUILD, &build))
	{
		INPUT_LOG("[Input] could not read the game PC Windows build\n");
		return false;
	}

	// Find the interactive session by checking the game process first,
	// then pick a winlogon in that session. On some 25H2 setups the only
	// winlogon is session 0 so we fall back to the game PID for kernel reads.
	DWORD targetSession = 0;
	{
		VMMDLL_PROCESS_INFORMATION gi{};
		gi.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
		gi.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
		SIZE_T gcb = sizeof(gi);
		if (VMMDLL_ProcessGetInformation(TargetProcess.vHandle, TargetProcess.GetPID(), &gi, &gcb))
			targetSession = gi.win.dwSessionId;
		INPUT_LOG("[Input] game process session %lu\n", targetSession);
	}

	win_logon_pid = 0;
	for (int pid : TargetProcess.GetPidListFromName("winlogon.exe"))
	{
		VMMDLL_PROCESS_INFORMATION info{};
		info.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
		info.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
		SIZE_T cb = sizeof(info);
		if (VMMDLL_ProcessGetInformation(TargetProcess.vHandle, pid, &info, &cb))
		{
			INPUT_LOG("[Input] winlogon pid %d session %lu\n", pid, info.win.dwSessionId);
			if (info.win.dwSessionId == targetSession && targetSession > 0)
			{
				win_logon_pid = pid;
				break;
			}
			if (info.win.dwSessionId > 0 && !win_logon_pid)
				win_logon_pid = pid;
		}
	}
	// If no interactive winlogon, use the game PID for kernel reads
	if (!win_logon_pid)
	{
		win_logon_pid = TargetProcess.GetPID();
		INPUT_LOG("[Input] no interactive winlogon, using game pid %d\n", win_logon_pid);
	}
	INPUT_LOG("[Input] game PC Windows build %llu, winlogon pid %d\n", build, win_logon_pid);
	if (!win_logon_pid)
		return false;

	// 22000 (Win11 21H2) can go either way, so it tries the Win10 export first.
	if (build <= 22000 && InitFromEat())
		return true;
	if (build < 22000)
		return false;
	// Try session method first, fall back to EAT on failure.
	if (InitFromSession(build))
		return true;
	return InitFromEat();
}

namespace
{
	// The overlay and game threads both poll keys. One of them refreshes the
	// bitmap (a single DMA read); the other keeps using the last copy instead
	// of waiting on the read.
	std::mutex g_keyMutex;      // held for the DMA read
	std::mutex g_keySnapMutex;  // held only to copy the snapshot
	std::atomic<int64_t> g_keyLastUpdateUs{ 0 };
	uint8_t g_keySnapshot[64]{};

	int64_t KeyNowUs()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}

void Keyboard::UpdateKeys()
{
	uint8_t previous_key_state_bitmap[64] = { 0 };
	memcpy(previous_key_state_bitmap, state_bitmap, 64);

	uint8_t fresh[64] = {};
	DWORD read = 0;
	if (!VMMDLL_MemReadEx(TargetProcess.vHandle, win_logon_pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY, gafAsyncKeyStateExport, fresh, 64, &read, VMMDLL_FLAG_NOCACHE) || read != 64)
		return; // keep the previous state rather than reporting every key released
	memcpy(state_bitmap, fresh, 64);
	for (int vk = 0; vk < 256; ++vk)
		if ((state_bitmap[(vk * 2 / 8)] & 1 << vk % 4 * 2) && !(previous_key_state_bitmap[(vk * 2 / 8)] & 1 << vk % 4 * 2))
			previous_state_bitmap[vk / 8] |= 1 << vk % 8;
}

bool Keyboard::IsKeyDown(uint32_t virtual_key_code)
{
	if (gafAsyncKeyStateExport < 0x7FFFFFFFFFFF || virtual_key_code >= 256)
		return false;
	const int64_t now = KeyNowUs();
	if (now - g_keyLastUpdateUs.load(std::memory_order_relaxed) > 2000)
	{
		std::unique_lock<std::mutex> lock(g_keyMutex, std::try_to_lock);
		if (lock.owns_lock())
		{
			UpdateKeys();
			std::lock_guard<std::mutex> snap(g_keySnapMutex);
			memcpy(g_keySnapshot, state_bitmap, sizeof(g_keySnapshot));
			g_keyLastUpdateUs.store(KeyNowUs(), std::memory_order_relaxed);
		}
	}
	std::lock_guard<std::mutex> lock(g_keySnapMutex);
	return (g_keySnapshot[(virtual_key_code * 2 / 8)] & 1 << virtual_key_code % 4 * 2) != 0;
}

bool Keyboard::IsReady()
{
	return gafAsyncKeyStateExport > 0x7FFFFFFFFFFF;
}

// Game-PC desktop cursor, ported from Pure-H1Z1 (InputManager.cpp, c_keys).
//
// gafAsyncKeyState carries buttons but no position, so the cursor POINT is
// learned: re-read a kernel window every ~120 ms and drop candidates that stop
// behaving like a cursor. Win11 scans the user session state; Win10 has no
// session base, so the window is centred on gafAsyncKeyState instead.
namespace
{
	constexpr uint32_t kCursorSessionScanSize = 0x100000; // 1 MiB
	constexpr uint32_t kCursorModuleScanBack = 0x40000;
	constexpr uint32_t kCursorModuleScanSize = 0x80000;
	constexpr int      kCursorSampleIntervalMs = 120;
	constexpr int32_t  kCursorMaxStep = 8000;  // px between samples
	// Reversals are what reject counters; range only proves the value moves.
	constexpr int      kCursorMinReversals = 2; // per axis, per direction
	constexpr int      kCursorMinSamples = 15;
	constexpr int32_t  kCursorMinRangePx = 80;
	// Not bounded by the configured screen: the game PC desktop can be bigger.
	constexpr int32_t  kCursorDiscoverBound = 32768;
	constexpr int      kCursorMaxReadFailStreak = 30;

	struct CursorCandidate
	{
		uint32_t off;
		int32_t prevX, prevY;
		int32_t minX, maxX, minY, maxY;
		uint16_t xUp, xDown, yUp, yDown;
	};

	uint64_t cursorScanBase = 0;
	uint32_t cursorScanSize = 0;
	uint64_t cursorAddress = 0;
	bool cursorLocked = false;
	int32_t cursorX = 0;
	int32_t cursorY = 0;
	bool cursorValid = false;
	int cursorReadFailStreak = 0;

	Keyboard::CursorCalStep cursorCalStep = Keyboard::CursorCalStep::Idle;
	std::string cursorCalMessage;
	std::vector<uint8_t> cursorScanBuf;
	std::vector<CursorCandidate> cursorCandidates;
	bool cursorSeeded = false;
	int cursorSamples = 0;
	std::chrono::steady_clock::time_point cursorLastSample{};

	bool CursorPointInBounds(int32_t x, int32_t y, int32_t w, int32_t h)
	{
		return x >= 0 && y >= 0 && x < w && y < h;
	}

	bool ReadKernel(uint64_t va, void* out, DWORD size)
	{
		DWORD cb = 0;
		return VMMDLL_MemReadEx(TargetProcess.vHandle, Keyboard::win_logon_pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY,
			va, (PBYTE)out, size, &cb, VMMDLL_FLAG_NOCACHE) && cb == size;
	}

	uint32_t SampleCursorScanWindow()
	{
		if (cursorScanBase == 0)
		{
			if (Keyboard::cursorSessionBase > 0x7FFFFFFFFFFF)
			{
				cursorScanBase = Keyboard::cursorSessionBase;
				cursorScanSize = kCursorSessionScanSize;
			}
			else
			{
				cursorScanBase = Keyboard::gafAsyncKeyStateExport - kCursorModuleScanBack;
				cursorScanSize = kCursorModuleScanSize;
			}
		}
		if (cursorScanBuf.size() != cursorScanSize)
			cursorScanBuf.assign(cursorScanSize, 0);

		DWORD cb = 0;
		VMMDLL_MemReadEx(TargetProcess.vHandle, Keyboard::win_logon_pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY,
			cursorScanBase, cursorScanBuf.data(), cursorScanSize, &cb, VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_ZEROPAD_ON_FAIL);
		return cb;
	}

	bool CursorPageHasData(uint32_t off)
	{
		const uint32_t page = off & ~0xFFFu;
		if ((size_t)page + 0x1000 > cursorScanBuf.size())
			return false;
		const uint8_t* p = cursorScanBuf.data() + page;
		for (uint32_t i = 0; i < 0x1000; ++i)
			if (p[i] != 0)
				return true;
		return false;
	}

	void RestartCursorLearning()
	{
		cursorCandidates.clear();
		cursorCandidates.shrink_to_fit();
		cursorSeeded = false;
		cursorSamples = 0;
	}

	void SeedCursorCandidates(uint32_t cb)
	{
		cursorCandidates.clear();
		cursorCandidates.reserve(1 << 16);
		for (uint32_t off = 0; off + 8 <= cb; off += 4)
		{
			int32_t x = 0, y = 0;
			memcpy(&x, cursorScanBuf.data() + off, sizeof(int32_t));
			memcpy(&y, cursorScanBuf.data() + off + sizeof(int32_t), sizeof(int32_t));
			if (!CursorPointInBounds(x, y, kCursorDiscoverBound, kCursorDiscoverBound) || !CursorPageHasData(off))
				continue;
			CursorCandidate c{};
			c.off = off;
			c.prevX = c.minX = c.maxX = x;
			c.prevY = c.minY = c.maxY = y;
			cursorCandidates.push_back(c);
		}
	}

	void UpdateCursorCandidates(uint32_t cb)
	{
		std::vector<CursorCandidate> keep;
		keep.reserve(cursorCandidates.size());
		for (CursorCandidate c : cursorCandidates)
		{
			if ((size_t)c.off + 8 > cb)
				continue;
			int32_t x = 0, y = 0;
			memcpy(&x, cursorScanBuf.data() + c.off, sizeof(int32_t));
			memcpy(&y, cursorScanBuf.data() + c.off + sizeof(int32_t), sizeof(int32_t));
			if (!CursorPointInBounds(x, y, kCursorDiscoverBound, kCursorDiscoverBound))
				continue;
			if (std::abs(x - c.prevX) > kCursorMaxStep || std::abs(y - c.prevY) > kCursorMaxStep)
				continue;

			if (x > c.prevX) { if (c.xUp < 60000) ++c.xUp; }
			else if (x < c.prevX) { if (c.xDown < 60000) ++c.xDown; }
			if (y > c.prevY) { if (c.yUp < 60000) ++c.yUp; }
			else if (y < c.prevY) { if (c.yDown < 60000) ++c.yDown; }

			c.prevX = x;
			c.prevY = y;
			c.minX = (std::min)(c.minX, x);
			c.maxX = (std::max)(c.maxX, x);
			c.minY = (std::min)(c.minY, y);
			c.maxY = (std::max)(c.maxY, y);
			keep.push_back(c);
		}
		cursorCandidates.swap(keep);
	}

	bool TryLockCursor()
	{
		if (cursorSamples < kCursorMinSamples)
			return false;

		const CursorCandidate* best = nullptr;
		long long bestScore = -1;
		for (const CursorCandidate& c : cursorCandidates)
		{
			if (c.maxX - c.minX < kCursorMinRangePx || c.maxY - c.minY < kCursorMinRangePx)
				continue;
			if (c.xUp < kCursorMinReversals || c.xDown < kCursorMinReversals
				|| c.yUp < kCursorMinReversals || c.yDown < kCursorMinReversals)
				continue;
			const long long score = (long long)(c.maxX - c.minX) + (c.maxY - c.minY);
			if (score > bestScore)
			{
				bestScore = score;
				best = &c;
			}
		}
		if (!best)
			return false;

		cursorAddress = cursorScanBase + best->off;
		cursorLocked = true;
		cursorValid = false;
		cursorReadFailStreak = 0;
		cursorCalStep = Keyboard::CursorCalStep::Done;
		cursorCalMessage = "Locked on to the game PC cursor.";
		printf("[Cursor] locked va=0x%llX after %d samples\n", cursorAddress, cursorSamples);

		RestartCursorLearning();
		cursorScanBuf.clear();
		cursorScanBuf.shrink_to_fit();
		return true;
	}
}

void Keyboard::BeginCursorCalibration()
{
	cursorLocked = false;
	cursorValid = false;
	cursorAddress = 0;
	cursorReadFailStreak = 0;
	cursorLastSample = {};
	RestartCursorLearning();

	if (!IsReady())
	{
		cursorCalStep = CursorCalStep::Failed;
		cursorCalMessage = "Game PC keyboard state is not available over DMA, so the cursor cannot be read.";
		return;
	}
	cursorCalStep = CursorCalStep::Learning;
	cursorCalMessage = "Learning. Move the game PC mouse around its screen.";
}

void Keyboard::CancelCursorCalibration()
{
	cursorCalStep = CursorCalStep::Idle;
	cursorCalMessage.clear();
	RestartCursorLearning();
	cursorScanBuf.clear();
	cursorScanBuf.shrink_to_fit();
}

Keyboard::CursorCalStep Keyboard::GetCursorCalStep() { return cursorCalStep; }
const std::string& Keyboard::GetCursorCalMessage() { return cursorCalMessage; }
bool Keyboard::IsCursorLocked() { return cursorLocked; }

bool Keyboard::GetCursor(int32_t& x, int32_t& y)
{
	if (!cursorLocked || !cursorValid)
		return false;
	x = cursorX;
	y = cursorY;
	return true;
}

bool Keyboard::GetCursorGafDelta(int64_t& delta)
{
	if (!cursorLocked || !cursorAddress || !IsReady())
		return false;
	delta = (int64_t)cursorAddress - (int64_t)gafAsyncKeyStateExport;
	return true;
}

bool Keyboard::RestoreCursorFromGafDelta(int64_t delta, int32_t screenW, int32_t screenH)
{
	if (delta == 0 || screenW <= 0 || screenH <= 0 || !IsReady())
		return false;
	const uint64_t va = (uint64_t)((int64_t)gafAsyncKeyStateExport + delta);
	if (va <= 0x7FFFFFFFFFFF)
		return false;

	struct { int32_t x, y; } pt{};
	if (!ReadKernel(va, &pt, sizeof(pt)) || !CursorPointInBounds(pt.x, pt.y, screenW, screenH))
		return false;

	cursorAddress = va;
	cursorX = pt.x;
	cursorY = pt.y;
	cursorValid = true;
	cursorLocked = true;
	cursorReadFailStreak = 0;
	cursorCalStep = CursorCalStep::Done;
	cursorCalMessage = "Cursor address restored from the saved calibration.";
	return true;
}

void Keyboard::TickCursor(int32_t screenW, int32_t screenH)
{
	if (!IsReady() || screenW <= 0 || screenH <= 0)
		return;

	if (cursorLocked)
	{
		struct { int32_t x, y; } pt{};
		if (!ReadKernel(cursorAddress, &pt, sizeof(pt)))
		{
			// Ride out transient DMA misses instead of dropping the lock.
			if (++cursorReadFailStreak > kCursorMaxReadFailStreak)
				cursorValid = false;
			return;
		}
		cursorReadFailStreak = 0;
		cursorValid = CursorPointInBounds(pt.x, pt.y, kCursorDiscoverBound, kCursorDiscoverBound);
		if (cursorValid)
		{
			cursorX = pt.x;
			cursorY = pt.y;
		}
		return;
	}

	if (cursorCalStep != CursorCalStep::Learning)
		return;

	const auto now = std::chrono::steady_clock::now();
	if (now - cursorLastSample < std::chrono::milliseconds(kCursorSampleIntervalMs))
		return;
	cursorLastSample = now;

	const uint32_t cb = SampleCursorScanWindow();
	if (cb < sizeof(int32_t) * 2)
	{
		cursorCalStep = CursorCalStep::Failed;
		cursorCalMessage = "Could not read kernel memory on the game PC. Check the DMA link, then try again.";
		return;
	}

	if (!cursorSeeded)
	{
		SeedCursorCandidates(cb);
		cursorSeeded = true;
		cursorSamples = 1;
		if (cursorCandidates.empty())
			RestartCursorLearning();
		cursorCalMessage = "Learning. Keep the game PC mouse moving.";
		return;
	}

	UpdateCursorCandidates(cb);
	++cursorSamples;
	if (cursorCandidates.empty())
	{
		RestartCursorLearning();
		cursorCalMessage = "Learning. Keep the game PC mouse moving.";
		return;
	}
	if (TryLockCursor())
		return;

	char msg[192];
	sprintf_s(msg, "Learning: %zu candidates, %d samples. Keep moving the game PC mouse across its screen.",
		cursorCandidates.size(), cursorSamples);
	cursorCalMessage = msg;
}
