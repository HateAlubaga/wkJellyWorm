#include "Input.h"
#include "Game.h"
#include "Lua.h"
#include <Windows.h>
#include <sol/sol.hpp>

namespace {
	// Provisional offset for DD_Keyboard key-state buffer in W:A 3.8.x.
	// TODO: confirm with Cheat Engine on Game::getAddrDdKeyboard() at runtime.
	constexpr int kKeyboardStateOffset = 0x84;
	constexpr int kKeyboardStateSize = 256;
}

bool Input::isKeyDown(int vk) {
	if (vk <= 0 || vk > 0xFF) {
		return false;
	}
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool Input::isGameKeyDown(int gameKeyCode) {
	if (gameKeyCode < 0 || gameKeyCode >= kKeyboardStateSize) {
		return false;
	}

	const DWORD keyboard = Game::getAddrDdKeyboard();
	if (!keyboard) {
		return false;
	}

	// TODO: replace offset once DD_Keyboard layout is confirmed in runtime.
	const auto *states = reinterpret_cast<const unsigned char *>(keyboard + kKeyboardStateOffset);
	return states[gameKeyCode] != 0;
}

unsigned int Input::getTickCount() {
	return ::GetTickCount();
}

void Input::registerLuaBindings() {
	auto *lua = Lua::getInstance().getState();
	lua->set_function("isKeyDown", &Input::isKeyDown);
	lua->set_function("isGameKeyDown", &Input::isGameKeyDown);
	lua->set_function("getTickCount", &Input::getTickCount);
}
