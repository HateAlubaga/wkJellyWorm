#ifndef WKJELLYWORM_LOBBYGUI_H
#define WKJELLYWORM_LOBBYGUI_H

#include <sigscanner.h>

class LobbyGui {
public:
	static int install(SignatureScanner &, module);

	static void onFrontendScreen(int screen);
	static void open();
	static void close();
	static void toggle();
	static bool isOpen();
	static void tick();

	static bool shouldInterceptFrontendScreen(int screen, int &redirectTo);

	static bool isLobbyDrawScreen(int screen);
	static bool shouldTickFromRenderHook();
};

#endif //WKJELLYWORM_LOBBYGUI_H
