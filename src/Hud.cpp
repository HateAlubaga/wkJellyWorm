#include "Hud.h"
#include "Lua.h"
#include "Game.h"
#include "renderer/Bitmap.h"
#include <sol/sol.hpp>
#include <vector>
#include <cstring>

namespace {
	enum class DrawKind { Text, Rect };

	struct DrawCmd {
		DrawKind kind;
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
		int color = 0;
		std::string text;
	};

	std::vector<DrawCmd> g_drawQueue;
}

void Hud::install() {
	auto *lua = Lua::getInstance().getState();
	lua->set_function("drawTextLocal", [](int x, int y, const std::string &text, sol::optional<int> color) {
		drawTextLocal(x, y, text, color.value_or(0));
	});
	lua->set_function("drawRectLocal", [](int x, int y, int w, int h, sol::optional<int> color) {
		drawRectLocal(x, y, w, h, color.value_or(0));
	});
}

void Hud::drawTextLocal(int x, int y, const std::string &text, int color) {
	DrawCmd cmd;
	cmd.kind = DrawKind::Text;
	cmd.x = x;
	cmd.y = y;
	cmd.color = color;
	cmd.text = text;
	g_drawQueue.push_back(std::move(cmd));
}

void Hud::drawRectLocal(int x, int y, int w, int h, int color) {
	DrawCmd cmd;
	cmd.kind = DrawKind::Rect;
	cmd.x = x;
	cmd.y = y;
	cmd.w = w;
	cmd.h = h;
	cmd.color = color;
	g_drawQueue.push_back(std::move(cmd));
}

void Hud::flushDrawQueue() {
	if(g_drawQueue.empty())
		return;

	DWORD ddDisplay = Game::getAddrDdDisplay();
	if(!ddDisplay || !Game::isValidGameGlobal()) {
		g_drawQueue.clear();
		return;
	}
	DWORD gameGlobal = Game::getAddrGameGlobal();
	int *gamescene = *(int **)(gameGlobal + 0x524);
	if(!gamescene) {
		g_drawQueue.clear();
		return;
	}

	for(auto &cmd : g_drawQueue) {
		if(cmd.kind == DrawKind::Text) {
			Bitmap::BitmapTextbox box{};
			std::string text = cmd.text;
			if(text.size() > 255)
				text.resize(255);
			Bitmap::callConstructTextbox(ddDisplay, &box, (int)text.size() + 1, 0);
			int dummyA6 = 0, dummyA7 = 0;
			Bitmap::callSetTextboxText(&box, (char *)text.c_str(), cmd.color, 0, 0, &dummyA6, &dummyA7, 0xFFFFFFFF);
			int posX = cmd.x * 0xFFFF;
			int posY = cmd.y * 0xFFFF;
			Bitmap::callDrawTextboxLocal(gamescene, posY, 0, posX, &box, 0, 0, 0);
		}
		// Rect: minimal stub — queued for API completeness; no OpenGL fill.
		(void)cmd;
	}
	g_drawQueue.clear();
}
