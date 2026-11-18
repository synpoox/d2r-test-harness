// Virtual game pad and held keys for the test harness. Test-only, like the rest of this plugin.
// State comes from d2r-testharness-pad.txt next to the DLL (tools/pad.py writes it); see the README.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace pad {

struct Export {
	const char* name;  // XInput export name; "#100" is ordinal 100 (XInputGetStateEx)
	void* hook;
	void** real;       // the first real function found for this export, or nullptr
};

void Init(const wchar_t* pluginFolder) noexcept;
void Poll() noexcept;  // re-reads the file when its text changed
auto Connected() noexcept -> bool;
auto ChangedTick() noexcept -> ULONGLONG;  // GetTickCount64 of the last pad state change
constexpr int Modules = 6;  // xinput1_4, 1_3, 9_1_0, uap, 1_2, 1_1: each has its own hooks, so each forwards to its own real function
auto Exports(int module, size_t* count) noexcept -> Export*;
auto KeyHeld(int vk, SHORT* state) noexcept -> bool;  // true when a held key answers for vk
void Describe(char* out, size_t size) noexcept;

// The file's one line -> state. Exposed so the format has a single reader.
struct State {
	uint16_t buttons = 0;
	uint8_t lt = 0, rt = 0;
	int16_t lx = 0, ly = 0, rx = 0, ry = 0;
	bool keys[256] = {};
};
auto Parse(const char* text, State* out) noexcept -> bool;

}  // namespace pad
