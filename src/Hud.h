#ifndef WKJELLYWORM_HUD_H
#define WKJELLYWORM_HUD_H

#include <string>

class Hud {
public:
	static void install();
	static void drawTextLocal(int x, int y, const std::string &text, int color = 0);
	static void drawRectLocal(int x, int y, int w, int h, int color = 0);
	static void flushDrawQueue();
};

#endif //WKJELLYWORM_HUD_H
