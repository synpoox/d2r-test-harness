#include "pad.h"

#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pad {
namespace {

constexpr DWORD NotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED
constexpr DWORD Empty = 4306;         // ERROR_EMPTY
constexpr char FileName[] = "d2r-testharness-pad.txt";

struct XState {  // XINPUT_STATE
	DWORD packet;
	uint16_t buttons;
	uint8_t lt, rt;
	int16_t lx, ly, rx, ry;
};

wchar_t g_path[MAX_PATH] = L"";
char g_text[512] = "";
State g_state;
std::atomic<bool> g_connected{false};
std::atomic<DWORD> g_packet{0};
std::atomic<ULONGLONG> g_changed{0};
std::atomic<uint32_t> g_calls[5] = {};  // GetState, GetStateEx, GetCapabilities, SetState, GetKeystroke

void* g_real[Modules][5] = {};  // per module: GetState, GetStateEx, GetCapabilities, SetState, GetKeystroke

auto Fill(XState* out) noexcept -> DWORD {
	out->buttons = g_state.buttons;
	out->lt = g_state.lt;
	out->rt = g_state.rt;
	out->lx = g_state.lx;
	out->ly = g_state.ly;
	out->rx = g_state.rx;
	out->ry = g_state.ry;
	out->packet = g_packet.load();
	return ERROR_SUCCESS;
}

template <int M>
auto WINAPI HookGetState(DWORD index, XState* out) -> DWORD {
	g_calls[0].fetch_add(1, std::memory_order_relaxed);
	if (!g_connected.load()) {
		return g_real[M][0] != nullptr ? reinterpret_cast<DWORD(WINAPI*)(DWORD, XState*)>(g_real[M][0])(index, out) : NotConnected;
	}
	return index == 0 && out != nullptr ? Fill(out) : NotConnected;
}

template <int M>
auto WINAPI HookGetStateEx(DWORD index, XState* out) -> DWORD {
	g_calls[1].fetch_add(1, std::memory_order_relaxed);
	if (!g_connected.load()) {
		return g_real[M][1] != nullptr ? reinterpret_cast<DWORD(WINAPI*)(DWORD, XState*)>(g_real[M][1])(index, out) : NotConnected;
	}
	return index == 0 && out != nullptr ? Fill(out) : NotConnected;
}

template <int M>
auto WINAPI HookCaps(DWORD index, DWORD flags, uint8_t* caps) -> DWORD {  // XINPUT_CAPABILITIES, 20 bytes
	g_calls[2].fetch_add(1, std::memory_order_relaxed);
	if (!g_connected.load()) {
		return g_real[M][2] != nullptr ? reinterpret_cast<DWORD(WINAPI*)(DWORD, DWORD, uint8_t*)>(g_real[M][2])(index, flags, caps) : NotConnected;
	}
	if (index != 0 || caps == nullptr) {
		return NotConnected;
	}
	std::memset(caps, 0, 20);
	caps[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
	caps[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
	const uint16_t all = 0xF3FF;
	std::memcpy(caps + 4, &all, 2);  // every button
	caps[6] = 255;
	caps[7] = 255;
	const int16_t full = -1;  // thumb sticks: all bits
	for (int i = 0; i < 4; ++i) {
		std::memcpy(caps + 8 + 2 * i, &full, 2);
	}
	std::memset(caps + 16, 0xFF, 4);  // vibration motors
	return ERROR_SUCCESS;
}

template <int M>
auto WINAPI HookSetState(DWORD index, void* vibration) -> DWORD {
	g_calls[3].fetch_add(1, std::memory_order_relaxed);
	if (!g_connected.load()) {
		return g_real[M][3] != nullptr ? reinterpret_cast<DWORD(WINAPI*)(DWORD, void*)>(g_real[M][3])(index, vibration) : NotConnected;
	}
	return index == 0 ? ERROR_SUCCESS : NotConnected;  // rumble is dropped
}

template <int M>
auto WINAPI HookKeystroke(DWORD index, DWORD reserved, void* keystroke) -> DWORD {
	g_calls[4].fetch_add(1, std::memory_order_relaxed);
	if (!g_connected.load()) {
		return g_real[M][4] != nullptr ? reinterpret_cast<DWORD(WINAPI*)(DWORD, DWORD, void*)>(g_real[M][4])(index, reserved, keystroke) : NotConnected;
	}
	return Empty;
}

template <int M>
void MakeExports(Export (&e)[5]) noexcept {
	e[0] = {"XInputGetState", reinterpret_cast<void*>(&HookGetState<M>), &g_real[M][0]};
	e[1] = {"#100", reinterpret_cast<void*>(&HookGetStateEx<M>), &g_real[M][1]};
	e[2] = {"XInputGetCapabilities", reinterpret_cast<void*>(&HookCaps<M>), &g_real[M][2]};
	e[3] = {"XInputSetState", reinterpret_cast<void*>(&HookSetState<M>), &g_real[M][3]};
	e[4] = {"XInputGetKeystroke", reinterpret_cast<void*>(&HookKeystroke<M>), &g_real[M][4]};
}

Export g_exports[Modules][5];

// Value of "key=<number>" as a whole word in the text.
auto Number(const char* text, const char* key, long* out) noexcept -> bool {
	char needle[16];
	std::snprintf(needle, sizeof(needle), "%s=", key);
	const char* at = std::strstr(text, needle);
	while (at != nullptr && at != text && at[-1] != ' ') {
		at = std::strstr(at + 1, needle);
	}
	if (at == nullptr) {
		return false;
	}
	const char* digits = at + std::strlen(needle);
	char* end = nullptr;
	*out = std::strtol(digits, &end, 0);
	return end != digits;
}

}  // namespace

auto Parse(const char* text, State* out) noexcept -> bool {
	State s;
	long v = 0;
	if (!Number(text, "buttons", &v)) {
		return false;
	}
	s.buttons = static_cast<uint16_t>(v);
	if (Number(text, "lt", &v)) s.lt = static_cast<uint8_t>(v);
	if (Number(text, "rt", &v)) s.rt = static_cast<uint8_t>(v);
	if (Number(text, "lx", &v)) s.lx = static_cast<int16_t>(v);
	if (Number(text, "ly", &v)) s.ly = static_cast<int16_t>(v);
	if (Number(text, "rx", &v)) s.rx = static_cast<int16_t>(v);
	if (Number(text, "ry", &v)) s.ry = static_cast<int16_t>(v);
	const char* keys = std::strstr(text, "keys=");
	if (keys != nullptr) {
		for (const char* p = keys + 5; *p != '\0' && *p != ' ' && *p != '\n' && *p != '\r';) {
			char* end = nullptr;
			const long vk = std::strtol(p, &end, 10);
			if (end == p) {
				break;
			}
			if (vk > 0 && vk < 256) {
				s.keys[vk] = true;
			}
			p = *end == ',' ? end + 1 : end;
		}
	}
	*out = s;
	return true;
}

void Init(const wchar_t* folder) noexcept {
	std::swprintf(g_path, MAX_PATH, L"%ls\\%hs", folder, FileName);
	DeleteFileW(g_path);  // a leftover file from an earlier run would boot the game into controller UI with stale keys
}

void Poll() noexcept {
	if (g_path[0] == L'\0') {
		return;
	}
	const HANDLE file = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		if (g_connected.exchange(false)) {
			g_state = State {};
			plog::Info("pad: state file gone, virtual pad off");
		}
		return;
	}
	char text[sizeof(g_text)] = "";
	DWORD got = 0;
	ReadFile(file, text, sizeof(text) - 1, &got, nullptr);
	CloseHandle(file);
	text[got] = '\0';
	if (std::strcmp(text, g_text) == 0 && g_connected.load()) {
		return;
	}
	State s;
	if (!Parse(text, &s)) {
		return;  // half-written: keep the last state
	}
	std::memcpy(g_text, text, sizeof(g_text));
	const bool was = g_connected.load();
	g_state = s;  // a stale read for a frame is fine for a test tool
	g_packet.fetch_add(1);
	g_changed.store(GetTickCount64());
	g_connected.store(true);
	if (!was) {
		plog::Info("pad: virtual pad on (state file seen)");
	}
}

auto Connected() noexcept -> bool {
	return g_connected.load();
}

auto ChangedTick() noexcept -> ULONGLONG {
	return g_changed.load();
}

auto Exports(int module, size_t* count) noexcept -> Export* {
	if (g_exports[0][0].hook == nullptr) {
		MakeExports<0>(g_exports[0]);
		MakeExports<1>(g_exports[1]);
		MakeExports<2>(g_exports[2]);
		MakeExports<3>(g_exports[3]);
		MakeExports<4>(g_exports[4]);
		MakeExports<5>(g_exports[5]);
	}
	*count = 5;
	return g_exports[module];
}

auto KeyHeld(int vk, SHORT* state) noexcept -> bool {
	if (!g_connected.load() || vk <= 0 || vk > 255) {
		return false;
	}
	const State& s = g_state;
	bool held = s.keys[vk];
	switch (vk) {  // the generic and the left/right forms answer for each other
	case VK_CONTROL: held = s.keys[VK_CONTROL] || s.keys[VK_LCONTROL] || s.keys[VK_RCONTROL]; break;
	case VK_SHIFT: held = s.keys[VK_SHIFT] || s.keys[VK_LSHIFT] || s.keys[VK_RSHIFT]; break;
	case VK_MENU: held = s.keys[VK_MENU] || s.keys[VK_LMENU] || s.keys[VK_RMENU]; break;
	case VK_LCONTROL: case VK_RCONTROL: held = held || s.keys[VK_CONTROL]; break;
	case VK_LSHIFT: case VK_RSHIFT: held = held || s.keys[VK_SHIFT]; break;
	case VK_LMENU: case VK_RMENU: held = held || s.keys[VK_MENU]; break;
	default: break;
	}
	if (!held) {
		return false;  // not held: the real key (or the harness block) answers
	}
	*state = static_cast<SHORT>(0x8000);
	return true;
}

void Describe(char* out, size_t size) noexcept {
	std::snprintf(out, size, "pad %s | buttons 0x%04X | XInput calls: state %u, stateEx %u, caps %u, set %u, keystroke %u", g_connected.load() ? "on" : "off",
	              g_state.buttons, g_calls[0].load(), g_calls[1].load(), g_calls[2].load(), g_calls[3].load(), g_calls[4].load());
}

}  // namespace pad
