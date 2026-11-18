// test-harness: the plugin side of d2rAI-toolkit's harness protocol (docs/testing.md there). Test-only: it lets the
// toolkit's game_ctl.py click, hold modifiers and send console lines in a game slot without the real mouse or keyboard.
// It is never shipped to players, and it does nothing unless d2r-testharness.txt sits next to
// the DLL (game_ctl.py creates that file before a start and removes it at stop).
//
// With the word file present it:
//  - creates the shared block Local\d2r-testharness-block-<pid> and sets its magic first;
//  - swaps a few USER32 entries in the import tables of the game image and D2RCore.dll. D2R asks these for the cursor,
//    the keys and the focus, so the swap needs no game address and works on any build. It writes no game code;
//  - answers those calls from the block, and always reports the game window as focused, so a slot works behind others;
//  - mutes this game's own audio sessions (Windows volume mixer), so a test run is silent; other programs are untouched;
//  - runs console lines from the block on the UI thread (D2RCore's ExecuteConsoleCommand) and appends what happened to
//    d2r-testharness-console.txt, acknowledging last so the tools never read a result that is not ready.
//
// Console command "testharness" prints the state.
#include <D2RLPlugin/api.h>
#include <D2RLPlugin/core_exports.h>
#include <D2RLPlugin/threads.h>

#include "crash_guard.h"
#include "log.h"
#include "pad.h"

#include <windows.h>

#include <audiopolicy.h>
#include <psapi.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <type_traits>
#include <utility>

namespace {

constexpr const char* PluginName = "test-harness";
constexpr const char* PluginVersion = "0.1.0";

// The block: testing.example.toml's [harness] defaults. Change them there and here together.
constexpr int32_t Magic = 0x4D4B5448;
constexpr DWORD BlockBytes = 576;
constexpr int CommandOffset = 64;
constexpr int CommandMax = 500;
enum Field { IdxMagic = 0, IdxActive = 1, IdxX = 2, IdxY = 3, IdxButtons = 4, IdxMods = 5, IdxCmdSeq = 8, IdxCmdAck = 9, IdxCmdResult = 10 };
constexpr int32_t ButtonLeft = 1, ButtonRight = 2;
constexpr int32_t ModShift = 1, ModCtrl = 2, ModAlt = 4;

constexpr const wchar_t* WordFile = L"d2r-testharness.txt";
constexpr const wchar_t* ConsoleFile = L"d2r-testharness-console.txt";
constexpr int MaxPatches = 256;
constexpr DWORD PollMs = 20;
constexpr int RescanPolls = 100;  // about every 2 s
constexpr int MutePolls = 50;  // sessions appear and return while the game runs: look again about once a second

constexpr D2RL::PluginInfo Info {
	.infoSize    = D2RL::PluginInfoSize,
	.abiVersion  = D2RL_PLUGIN_ABI_VERSION,
	.id          = "test-harness",
	.name        = "Test Harness",
	.version     = PluginVersion,
	.author      = "Synpoo",
	.description = "Test-only: lets d2rAI-toolkit drive a game slot. Never released.",
	.flags       = D2RL::PluginFlags::Client,
};

using ExecuteConsoleCommandFn = D2RL::CoreExports::ExecuteConsoleCommandFn;

const D2RL::PluginContext* g_context = nullptr;
const D2RL::ThreadService* g_threads = nullptr;
ExecuteConsoleCommandFn g_execute = nullptr;
volatile int32_t* g_block = nullptr;
HANDLE g_mapping = nullptr;
HANDLE g_worker = nullptr;
HANDLE g_stop = nullptr;
std::atomic<HWND> g_window{nullptr};
std::atomic<int32_t> g_queued{0};  // the cmd_seq a UI callback is on its way for
wchar_t g_consolePath[MAX_PATH] = L"";
int g_patchedEntries = 0;
int g_commands = 0;
int g_mutedSessions = 0;
uintptr_t g_exeBase = 0;
wchar_t g_folder[MAX_PATH] = L"";

struct Patch {
	void** slot;
	void* original;
};
Patch g_patches[MaxPatches] = {};
int g_patchCount = 0;

std::atomic<uint32_t> g_calls[7] = {};  // cursor, physical cursor, key state, async key state, foreground, active, focus

auto Active() noexcept -> bool {
	return g_block != nullptr && g_block[IdxActive] != 0;
}

auto Held(bool down) noexcept -> SHORT {
	return down ? static_cast<SHORT>(0x8000) : static_cast<SHORT>(0);
}

// Answers for the mouse buttons and modifiers while the tools drive the game; false for every other key.
auto VirtualKey(int vk, SHORT* state) noexcept -> bool {
	const int32_t buttons = g_block[IdxButtons];
	const int32_t mods = g_block[IdxMods];
	switch (vk) {
	case VK_LBUTTON: *state = Held((buttons & ButtonLeft) != 0); return true;
	case VK_RBUTTON: *state = Held((buttons & ButtonRight) != 0); return true;
	case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: *state = Held((mods & ModShift) != 0); return true;
	case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: *state = Held((mods & ModCtrl) != 0); return true;
	case VK_MENU: case VK_LMENU: case VK_RMENU: *state = Held((mods & ModAlt) != 0); return true;
	default: return false;
	}
}

decltype(&GetCursorPos) g_realCursor = nullptr;
decltype(&GetPhysicalCursorPos) g_realPhysicalCursor = nullptr;
decltype(&GetKeyState) g_realKeyState = nullptr;
decltype(&GetAsyncKeyState) g_realAsyncKeyState = nullptr;
decltype(&GetForegroundWindow) g_realForeground = nullptr;
decltype(&GetActiveWindow) g_realActive = nullptr;
decltype(&GetFocus) g_realFocus = nullptr;

auto VirtualCursor(LPPOINT point) noexcept -> bool {
	const HWND window = g_window.load();
	if (point == nullptr || window == nullptr || !Active()) {
		return false;
	}
	POINT p {g_block[IdxX], g_block[IdxY]};
	if (!ClientToScreen(window, &p)) {
		return false;
	}
	*point = p;
	return true;
}

auto WINAPI HookCursor(LPPOINT point) -> BOOL {
	g_calls[0].fetch_add(1, std::memory_order_relaxed);
	return VirtualCursor(point) ? TRUE : g_realCursor(point);
}

auto WINAPI HookPhysicalCursor(LPPOINT point) -> BOOL {
	g_calls[1].fetch_add(1, std::memory_order_relaxed);
	return VirtualCursor(point) ? TRUE : g_realPhysicalCursor(point);
}

auto WINAPI HookKeyState(int vk) -> SHORT {
	g_calls[2].fetch_add(1, std::memory_order_relaxed);
	SHORT state = 0;
	if (pad::KeyHeld(vk, &state)) {
		return state;
	}
	return Active() && VirtualKey(vk, &state) ? state : g_realKeyState(vk);
}

auto WINAPI HookAsyncKeyState(int vk) -> SHORT {
	g_calls[3].fetch_add(1, std::memory_order_relaxed);
	SHORT state = 0;
	if (pad::KeyHeld(vk, &state)) {
		return state;
	}
	return Active() && VirtualKey(vk, &state) ? state : g_realAsyncKeyState(vk);
}

auto WINAPI HookForeground() -> HWND {
	g_calls[4].fetch_add(1, std::memory_order_relaxed);
	const HWND window = g_window.load();
	return window != nullptr ? window : g_realForeground();
}

auto WINAPI HookActive() -> HWND {
	g_calls[5].fetch_add(1, std::memory_order_relaxed);
	const HWND window = g_window.load();
	return window != nullptr ? window : g_realActive();
}

auto WINAPI HookFocus() -> HWND {
	g_calls[6].fetch_add(1, std::memory_order_relaxed);
	const HWND window = g_window.load();
	return window != nullptr ? window : g_realFocus();
}

// Windows would report the real mouse leaving the game window, which ends hover for the virtual cursor.
auto WINAPI HookTrackMouse(LPTRACKMOUSEEVENT) -> BOOL {
	return TRUE;
}

// --- Import tables -------------------------------------------------------------------------------------------------
// The game image keeps its imports where the PE header does not show them, so this looks for the pointers themselves:
// every readable, aligned word in the module that holds a function's real address.
auto Readable(DWORD protect) noexcept -> bool {
	if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
		return false;
	}
	return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void SwapSlot(void** slot, void* hook) noexcept {
	if (g_patchCount >= MaxPatches) {
		return;
	}
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {  // keeps the page executable, as the old protection may be
		return;
	}
	g_patches[g_patchCount++] = Patch {slot, *slot};
	*slot = hook;
	VirtualProtect(slot, sizeof(void*), old, &old);
}

struct Swap {
	const char* name;
	void* real;
	void* hook;
};

void SwapInModule(uintptr_t base, Swap* swaps, int count) noexcept {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<uintptr_t>(dos->e_lfanew));
	const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
	uintptr_t at = base;
	while (at < end) {
		MEMORY_BASIC_INFORMATION region {};
		if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) == 0) {
			return;
		}
		const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
		if (region.State == MEM_COMMIT && Readable(region.Protect)) {
			const uintptr_t stop = (regionEnd < end ? regionEnd : end) - sizeof(void*);
			for (uintptr_t p = (at + 7) & ~uintptr_t{7}; p <= stop; p += sizeof(void*)) {
				void* const value = *reinterpret_cast<void* const*>(p);
				for (int i = 0; i < count; ++i) {
					if (value == swaps[i].real) {
						SwapSlot(reinterpret_cast<void**>(p), swaps[i].hook);
						break;
					}
				}
			}
		}
		at = regionEnd;
	}
}

// Swaps the entries in the game image, D2RCore.dll and the other DLLs of the plugins folder (a plugin then reads the
// same virtual input as the game). shortcut: only plugins loaded before this one; a later one keeps the real input.
void SwapEverywhere(uintptr_t exeBase, const wchar_t* folder, Swap* swaps, int count) noexcept {
	SwapInModule(exeBase, swaps, count);
	const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
	if (core != nullptr) {
		SwapInModule(reinterpret_cast<uintptr_t>(core), swaps, count);
	}
	HMODULE modules[512];
	DWORD bytes = 0;
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&SwapInModule), &self);
	if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
		const size_t folderLength = std::wcslen(folder);
		for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 512; ++i) {
			wchar_t path[MAX_PATH];
			if (modules[i] != self && GetModuleFileNameW(modules[i], path, MAX_PATH) != 0 && _wcsnicmp(path, folder, folderLength) == 0 &&
			    path[folderLength] == L'\\') {
				SwapInModule(reinterpret_cast<uintptr_t>(modules[i]), swaps, count);
			}
		}
	}
}

// XInput: every export of every XInput DLL the process has loaded, so the pad works whichever one the game picked.
// Resolved by export name, never by address. Returns the new number of entries in swaps.
auto AddPadSwaps(Swap* swaps, int at, int capacity) noexcept -> int {
	static const wchar_t* const modules[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinputuap.dll", L"xinput1_2.dll", L"xinput1_1.dll"};
	static_assert(sizeof(modules) / sizeof(modules[0]) == pad::Modules);
	for (int m = 0; m < pad::Modules; ++m) {
		size_t exportCount = 0;
		pad::Export* exports = pad::Exports(m, &exportCount);
		const HMODULE module = GetModuleHandleW(modules[m]);
		if (module == nullptr) {
			continue;
		}
		for (size_t i = 0; i < exportCount; ++i) {
			const char* exportName = exports[i].name;
			void* const real = reinterpret_cast<void*>(exportName[0] == '#' ? GetProcAddress(module, MAKEINTRESOURCEA(std::atoi(exportName + 1))) : GetProcAddress(module, exportName));
			if (real != nullptr && at < capacity) {
				*exports[i].real = real;
				swaps[at++] = Swap {exportName, real, exports[i].hook};
			}
		}
	}
	return at;
}

// folder: the plugins folder.
void SwapImports(uintptr_t exeBase, const wchar_t* folder) noexcept {
	const HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if (user32 == nullptr) {
		return;
	}
	const auto load = [user32](auto* real, const char* name) { *real = reinterpret_cast<std::remove_reference_t<decltype(*real)>>(GetProcAddress(user32, name)); };
	load(&g_realCursor, "GetCursorPos");
	load(&g_realPhysicalCursor, "GetPhysicalCursorPos");
	load(&g_realKeyState, "GetKeyState");
	load(&g_realAsyncKeyState, "GetAsyncKeyState");
	load(&g_realForeground, "GetForegroundWindow");
	load(&g_realActive, "GetActiveWindow");
	load(&g_realFocus, "GetFocus");
	Swap swaps[64] = {
		{"GetCursorPos", reinterpret_cast<void*>(g_realCursor), reinterpret_cast<void*>(&HookCursor)},
		{"GetPhysicalCursorPos", reinterpret_cast<void*>(g_realPhysicalCursor), reinterpret_cast<void*>(&HookPhysicalCursor)},
		{"GetKeyState", reinterpret_cast<void*>(g_realKeyState), reinterpret_cast<void*>(&HookKeyState)},
		{"GetAsyncKeyState", reinterpret_cast<void*>(g_realAsyncKeyState), reinterpret_cast<void*>(&HookAsyncKeyState)},
		{"GetForegroundWindow", reinterpret_cast<void*>(g_realForeground), reinterpret_cast<void*>(&HookForeground)},
		{"GetActiveWindow", reinterpret_cast<void*>(g_realActive), reinterpret_cast<void*>(&HookActive)},
		{"GetFocus", reinterpret_cast<void*>(g_realFocus), reinterpret_cast<void*>(&HookFocus)},
		{"TrackMouseEvent", reinterpret_cast<void*>(GetProcAddress(user32, "TrackMouseEvent")), reinterpret_cast<void*>(&HookTrackMouse)},
	};
	int initialised = 0;
	while (swaps[initialised].name != nullptr) {
		++initialised;
	}
	const int count = AddPadSwaps(swaps, initialised, 64);
	SwapEverywhere(exeBase, folder, swaps, count);
	g_patchedEntries = g_patchCount;
}

void RestoreImports() noexcept {
	for (int i = g_patchCount - 1; i >= 0; --i) {
		DWORD old = 0;
		if (VirtualProtect(g_patches[i].slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			*g_patches[i].slot = g_patches[i].original;
			VirtualProtect(g_patches[i].slot, sizeof(void*), old, &old);
		}
	}
	g_patchCount = 0;
}

// --- Console -------------------------------------------------------------------------------------------------------
void AppendConsole(const char* text) noexcept {
	const HANDLE file = CreateFileW(g_consolePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	DWORD written = 0;
	WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
	CloseHandle(file);
}

void __cdecl RunCommand(const D2RL::PluginContext*, void* userData) noexcept {
	const auto seq = static_cast<int32_t>(reinterpret_cast<intptr_t>(userData));
	char command[CommandMax + 1];
	const auto* text = reinterpret_cast<const volatile char*>(g_block) + CommandOffset;
	int length = 0;
	for (; length < CommandMax && text[length] != '\0'; ++length) {
		command[length] = text[length];
	}
	command[length] = '\0';
	const bool handled = g_execute != nullptr && g_execute(command);
	++g_commands;
	char line[CommandMax + 32];
	std::snprintf(line, sizeof(line), "> %s\n%s\n", command, handled ? "handled" : "refused");
	AppendConsole(line);
	g_block[IdxCmdResult] = handled ? 1 : 0;
	MemoryBarrier();
	g_block[IdxCmdAck] = seq;
}

// Mutes every audio session of this process on the default output. Returns how many it holds muted.
auto MuteSessions() noexcept -> int {
	IMMDeviceEnumerator* devices = nullptr;
	IMMDevice* device = nullptr;
	IAudioSessionManager2* manager = nullptr;
	IAudioSessionEnumerator* sessions = nullptr;
	int muted = 0;
	int count = 0;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) &&
	    SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
	    SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) &&
	    SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && SUCCEEDED(sessions->GetCount(&count))) {
		for (int i = 0; i < count; ++i) {
			IAudioSessionControl* control = nullptr;
			IAudioSessionControl2* control2 = nullptr;
			ISimpleAudioVolume* volume = nullptr;
			DWORD pid = 0;
			if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2))) &&
			    SUCCEEDED(control2->GetProcessId(&pid)) && pid == GetCurrentProcessId() && SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&volume))) &&
			    SUCCEEDED(volume->SetMute(TRUE, nullptr))) {
				++muted;
			}
			if (volume != nullptr) {
				volume->Release();
			}
			if (control2 != nullptr) {
				control2->Release();
			}
			if (control != nullptr) {
				control->Release();
			}
		}
	}
	if (sessions != nullptr) {
		sessions->Release();
	}
	if (manager != nullptr) {
		manager->Release();
	}
	if (device != nullptr) {
		device->Release();
	}
	if (devices != nullptr) {
		devices->Release();
	}
	return muted;
}

auto CALLBACK FindWindowProc(HWND window, LPARAM result) -> BOOL {
	DWORD pid = 0;
	GetWindowThreadProcessId(window, &pid);
	RECT rect {};
	if (pid == GetCurrentProcessId() && IsWindowVisible(window) && GetClientRect(window, &rect)) {
		const LONG area = rect.right * rect.bottom;
		auto* best = reinterpret_cast<std::pair<HWND, LONG>*>(result);
		if (area > best->second) {
			*best = {window, area};
		}
	}
	return TRUE;
}

// Logs each module that looks like an input path (XInput, HID, Windows.Gaming.Input, GameInput, Steam) the first time it
// is seen, so the log shows which one the game really uses.
void WatchModules() noexcept {
	static HMODULE seen[256];
	static int seenCount = 0;
	HMODULE modules[1024];
	DWORD bytes = 0;
	if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
		return;
	}
	for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
		wchar_t path[MAX_PATH];
		if (GetModuleFileNameW(modules[i], path, MAX_PATH) == 0) {
			continue;
		}
		wchar_t lower[MAX_PATH];
		wcscpy_s(lower, path);
		_wcslwr_s(lower);
		const wchar_t* file = std::wcsrchr(lower, L'\\');
		file = file != nullptr ? file + 1 : lower;
		const wchar_t* keys[] = {L"input", L"hid", L"gaming", L"steam", L"joy", L"gamepad", L"usb", L"bluetooth"};
		bool match = false;
		for (const wchar_t* k : keys) {
			match = match || std::wcsstr(file, k) != nullptr;
		}
		bool known = false;
		for (int j = 0; j < seenCount; ++j) {
			known = known || seen[j] == modules[i];
		}
		if (match && !known && seenCount < 256) {
			seen[seenCount++] = modules[i];
			plog::Info("module loaded: %ls", path);
		}
	}
}

auto WINAPI Worker(LPVOID) -> DWORD {
	bool logged = false;
	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	int untilMute = 0;
	int untilScan = 0;
	while (WaitForSingleObject(g_stop, PollMs) == WAIT_TIMEOUT) {
		pad::Poll();
		if (--untilScan <= 0) {
			untilScan = RescanPolls;
			WatchModules();
			if (pad::Connected() && g_exeBase != 0) {
				SwapImports(g_exeBase, g_folder);  // the game may take an XInput pointer after the first swap
			}
		}
		if (com && --untilMute <= 0) {
			untilMute = MutePolls;
			const int muted = MuteSessions();
			if (muted != g_mutedSessions) {
				plog::Info("sound: %d audio session(s) of this game muted", muted);
				g_mutedSessions = muted;
			}
		}
		const HWND known = g_window.load();
		if (known == nullptr || !IsWindow(known)) {
			std::pair<HWND, LONG> best {nullptr, 0};
			EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&best));
			g_window.store(best.first);
			if (best.first != nullptr && !logged) {
				logged = true;
				plog::Info("game window found (%ld px)", best.second);
			}
		}
		const int32_t seq = g_block[IdxCmdSeq];
		if (seq != g_block[IdxCmdAck] && seq != g_queued.load() && g_threads != nullptr) {
			if (g_threads->runOnUiThread(g_context, RunCommand, reinterpret_cast<void*>(static_cast<intptr_t>(seq))) == D2RL::Threads::Result::Success) {
				g_queued.store(seq);
			}
		}
	}
	if (com) {
		CoUninitialize();
	}
	return 0;
}

auto StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	if (command == nullptr || command->plugin == nullptr) {
		return D2RL::ConsoleCommandResult::Failed;
	}
	char line[320];
	std::snprintf(line, sizeof(line), "%s %s | %s | %d import entries | window %s | active %d | %d commands | %d sessions muted", PluginName, PluginVersion,
	              g_block != nullptr ? "on" : "off (no d2r-testharness.txt)", g_patchedEntries, g_window.load() != nullptr ? "found" : "none",
	              Active() ? 1 : 0, g_commands, g_mutedSessions);
	command->plugin->WriteConsoleMessage(line);
	plog::Info("%s", line);
	pad::Describe(line, sizeof(line));
	command->plugin->WriteConsoleMessage(line);
	plog::Info("%s", line);
	plog::Info("calls: cursor %u, physical %u, key %u, async key %u, foreground %u, active %u, focus %u", g_calls[0].load(), g_calls[1].load(), g_calls[2].load(),
	           g_calls[3].load(), g_calls[4].load(), g_calls[5].load(), g_calls[6].load());
	return D2RL::ConsoleCommandResult::Handled;
}

// Starts the harness when the word file sits next to the DLL. Returns what happened for the log.
auto Start(const D2RL::PluginContext* context, char* report, size_t size) noexcept -> bool {
	const wchar_t* self = D2RL::GetPluginPath(context);
	if (self == nullptr) {
		std::snprintf(report, size, "off: D2RLoader gave no plugin path");
		return false;
	}
	wchar_t folder[MAX_PATH];
	std::swprintf(folder, MAX_PATH, L"%ls", self);
	wchar_t* slash = std::wcsrchr(folder, L'\\');
	if (slash == nullptr) {
		std::snprintf(report, size, "off: no folder in the plugin path");
		return false;
	}
	*slash = L'\0';
	wchar_t word[MAX_PATH];
	std::swprintf(word, MAX_PATH, L"%ls\\%ls", folder, WordFile);
	if (GetFileAttributesW(word) == INVALID_FILE_ATTRIBUTES) {
		std::snprintf(report, size, "off: no d2r-testharness.txt next to the DLL");
		return false;
	}
	std::swprintf(g_consolePath, MAX_PATH, L"%ls\\%ls", folder, ConsoleFile);
	if (context->QueryService(&g_threads) != D2RL::ServiceQueryResult::Success ||
	    !D2RL::HasThreadServiceField(g_threads, D2RL::ThreadServiceRequiredSize) || g_threads->runOnUiThread == nullptr) {
		g_threads = nullptr;
		std::snprintf(report, size, "off: D2RLoader has no thread service");
		return false;
	}
	wchar_t name[80];
	std::swprintf(name, 80, L"Local\\d2r-testharness-block-%lu", GetCurrentProcessId());
	g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, BlockBytes, name);
	if (g_mapping == nullptr) {
		std::snprintf(report, size, "off: no shared block (error %lu)", GetLastError());
		return false;
	}
	g_block = static_cast<volatile int32_t*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, BlockBytes));
	if (g_block == nullptr) {
		std::snprintf(report, size, "off: no view of the shared block (error %lu)", GetLastError());
		CloseHandle(g_mapping);
		g_mapping = nullptr;
		return false;
	}
	g_block[IdxMagic] = Magic;
	const HMODULE core = GetModuleHandleW(L"D2RCore.dll");
	if (core != nullptr) {
		g_execute = reinterpret_cast<ExecuteConsoleCommandFn>(GetProcAddress(core, MAKEINTRESOURCEA(D2RL::CoreExports::ExecuteConsoleCommandInfo.ordinal)));
	}
	pad::Init(folder);
	pad::Poll();
	g_exeBase = context->exeBase;
	std::swprintf(g_folder, MAX_PATH, L"%ls", folder);
	SwapImports(context->exeBase, folder);
	WatchModules();
	g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_worker = g_stop != nullptr ? CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr) : nullptr;
	std::snprintf(report, size, "on: block ready, %d import entries swapped, console %s, worker %s", g_patchCount, g_execute != nullptr ? "yes" : "no",
	              g_worker != nullptr ? "yes" : "no");
	return true;
}

void Stop() noexcept {
	if (g_stop != nullptr) {
		SetEvent(g_stop);
	}
	if (g_worker != nullptr) {
		WaitForSingleObject(g_worker, INFINITE);  // RestoreImports must not race a scan
		CloseHandle(g_worker);
		g_worker = nullptr;
	}
	if (g_stop != nullptr) {
		CloseHandle(g_stop);
		g_stop = nullptr;
	}
	g_window.store(nullptr);
	RestoreImports();
	if (g_block != nullptr) {
		UnmapViewOfFile(const_cast<int32_t*>(g_block));
		g_block = nullptr;
	}
	if (g_mapping != nullptr) {
		CloseHandle(g_mapping);
		g_mapping = nullptr;
	}
}

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	g_context = context;
	plog::Init(context, PluginName);
	crash_guard::Install(PluginVersion, context->exeBase);
	char report[300];
	(void)Start(context, report, sizeof(report));
	plog::Info("%s %s", PluginVersion, report);
	return context->RegisterConsoleCommand("testharness", StatusCommand, "Test harness status.");
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
	Stop();
	crash_guard::Remove();
	g_context = nullptr;
}
