#ifndef WKJELLYWORM_INPUT_H
#define WKJELLYWORM_INPUT_H

class Input {
public:
	// Win32 virtual-key check (VK_SHIFT, VK_SPACE, etc.)
	static bool isKeyDown(int vk);

	// Game-internal key code (Constants::KeyCodes). Uses addrDDKeyboard when layout is known.
	static bool isGameKeyDown(int gameKeyCode);

	// Win32 GetTickCount (ms since boot) for Lua timers.
	static unsigned int getTickCount();

	static void registerLuaBindings();
};

#endif //WKJELLYWORM_INPUT_H
