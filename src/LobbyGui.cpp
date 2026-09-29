#include "LobbyGui.h"
#include "MemAccess.h"
#include "Config.h"
#include "Game.h"
#include "Lobby.h"
#include "Input.h"
#include "Hooks.h"
#include "packages/PackageManager.h"
#include "renderer/Bitmap.h"
#include <Windows.h>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <set>

namespace {
	struct GuiRow {
		std::string name;
		std::string version;
		std::string description;
		std::string author;
		bool enabled = false;
	};

	constexpr int kPanelX = 48;
	constexpr int kPanelY = 36;
	constexpr int kRowH = 14;
	constexpr int kVisibleRows = 12;
	constexpr int kHeaderLines = 5;
	constexpr int kPanelW = 560;

	bool pickerOpen = false;
	int currentScreen = 0;
	int lastLobbyScreen = 0;
	int scrollOffset = 0;
	int selectedIndex = 0;
	bool prevLmb = false;
	bool prevUp = false;
	bool prevDown = false;
	bool prevEnter = false;
	bool prevEsc = false;
	bool prevPgUp = false;
	bool prevPgDn = false;
	std::string filterText;
	int learnedSchemeScreen = 0;
	int learnedMapScreen = 0;
	std::set<int> seenFromLobby;

	bool prevShiftLmb = false;

	bool isKnownLobbyScreen(int screen) {
		return screen == 1704 || screen == 1706 || screen == 21
			|| screen == 10 || screen == 11 || screen == 1700 || screen == 1702 || screen == 1703;
	}

	// LobbyGui draws via Bitmap in the render hook — skip client lobby (1706) and
	// network menu screens where DDGame/gameGlobal may be transitional (crash at +0x488).
	bool isLobbyGuiDrawScreen(int screen) {
		return screen == 1704 || screen == 21;
	}

	bool isNetworkMenuScreen(int screen) {
		return screen == 1700 || screen == 1702 || screen == 1703;
	}

	bool canEditPackages() {
		// Host lobby (1704) and single player (21) may toggle packages.
		// Client lobby (1706) is read-only — host drives config sync.
		return currentScreen == 1704 || currentScreen == 21;
	}

	void drawText(int *gamescene, int x, int y, const std::string &text, int color) {
		DWORD ddDisplay = Game::getAddrDdDisplay();
		if(!ddDisplay || !jellyProbeRead(ddDisplay, 0x40))
			return;
		if(!gamescene || text.empty())
			return;
		Bitmap::BitmapTextbox box{};
		std::string line = text;
		if(line.size() > 255)
			line.resize(255);
		Bitmap::callConstructTextbox(ddDisplay, &box, (int)line.size() + 1, 0);
		int dummyA6 = 0, dummyA7 = 0;
		Bitmap::callSetTextboxText(&box, (char *)line.c_str(), color, 0, 0, &dummyA6, &dummyA7, 0xFFFFFFFF);
		Bitmap::callDrawTextboxLocal(gamescene, y * 0xFFFF, 0, x * 0xFFFF, &box, 0, 0, 0);
	}

	std::string toLower(std::string s) {
		for(char &c : s)
			c = (char)std::tolower((unsigned char)c);
		return s;
	}

	std::vector<GuiRow> buildRows() {
		std::vector<GuiRow> rows;
		auto &pm = PackageManager::getInstance();
		std::string filter = toLower(filterText);
		for(auto & entry : pm.listPackagesForGui()) {
			GuiRow row{entry.name, entry.version, entry.description, entry.author, entry.enabled};
			if(!filter.empty()) {
				std::string hay = toLower(row.name + " " + row.description + " " + row.author);
				if(hay.find(filter) == std::string::npos)
					continue;
			}
			rows.push_back(std::move(row));
		}
		return rows;
	}

	void clampSelection(const std::vector<GuiRow> &rows) {
		if(rows.empty()) {
			selectedIndex = 0;
			scrollOffset = 0;
			return;
		}
		if(selectedIndex < 0)
			selectedIndex = 0;
		if(selectedIndex >= (int)rows.size())
			selectedIndex = (int)rows.size() - 1;
		if(selectedIndex < scrollOffset)
			scrollOffset = selectedIndex;
		if(selectedIndex >= scrollOffset + kVisibleRows)
			scrollOffset = selectedIndex - kVisibleRows + 1;
	}

	void toggleSelected(void *lobbyObj) {
		if(!canEditPackages())
			return;
		auto rows = buildRows();
		if(selectedIndex < 0 || selectedIndex >= (int)rows.size())
			return;
		auto &row = rows[selectedIndex];
		PackageManager::getInstance().togglePackageFromGui(lobbyObj, row.name, row.version, !row.enabled);
	}

	void handleInput(void *lobbyObj) {
		auto rows = buildRows();
		clampSelection(rows);

		bool up = Input::isKeyDown(VK_UP);
		bool down = Input::isKeyDown(VK_DOWN);
		bool enter = Input::isKeyDown(VK_RETURN);
		bool esc = Input::isKeyDown(VK_ESCAPE);
		bool pgUp = Input::isKeyDown(VK_PRIOR);
		bool pgDn = Input::isKeyDown(VK_NEXT);
		bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		bool backspace = Input::isKeyDown(VK_BACK);

		if(up && !prevUp && !rows.empty())
			selectedIndex--;
		if(down && !prevDown && !rows.empty())
			selectedIndex++;
		if(pgUp && !prevPgUp) {
			selectedIndex -= kVisibleRows;
			scrollOffset -= kVisibleRows;
			if(scrollOffset < 0) scrollOffset = 0;
		}
		if(pgDn && !prevPgDn) {
			selectedIndex += kVisibleRows;
			scrollOffset += kVisibleRows;
		}
		if(enter && !prevEnter)
			toggleSelected(lobbyObj);
		if(esc && !prevEsc) {
			if(!filterText.empty())
				filterText.clear();
			else
				pickerOpen = false;
		}

		// Type-to-filter (letters/digits/space)
		for(int vk = 'A'; vk <= 'Z'; vk++) {
			static bool prevLetter[26] = {};
			bool downL = Input::isKeyDown(vk);
			int idx = vk - 'A';
			if(downL && !prevLetter[idx] && filterText.size() < 32)
				filterText.push_back((char)std::tolower(vk));
			prevLetter[idx] = downL;
		}
		for(int vk = '0'; vk <= '9'; vk++) {
			static bool prevDigit[10] = {};
			bool downD = Input::isKeyDown(vk);
			int idx = vk - '0';
			if(downD && !prevDigit[idx] && filterText.size() < 32)
				filterText.push_back((char)vk);
			prevDigit[idx] = downD;
		}
		static bool prevBk = false;
		if(backspace && !prevBk && !filterText.empty())
			filterText.pop_back();
		prevBk = backspace;

		if(lmb && !prevLmb) {
			POINT pt{};
			GetCursorPos(&pt);
			HWND hwnd = GetForegroundWindow();
			if(hwnd)
				ScreenToClient(hwnd, &pt);
			int listY = kPanelY + kHeaderLines * kRowH;
			int listBottom = listY + kVisibleRows * kRowH;
			// scrollbar click
			int barX = kPanelX + kPanelW - 12;
			if(pt.x >= barX && pt.x <= barX + 10 && pt.y >= listY && pt.y < listBottom && !rows.empty()) {
				float rel = (float)(pt.y - listY) / (float)(kVisibleRows * kRowH);
				selectedIndex = (int)(rel * (float)rows.size());
				clampSelection(rows);
			} else if(pt.x >= kPanelX && pt.x <= kPanelX + kPanelW
				&& pt.y >= listY && pt.y < listBottom) {
				int row = (pt.y - listY) / kRowH + scrollOffset;
				if(row >= 0 && row < (int)rows.size()) {
					selectedIndex = row;
					toggleSelected(lobbyObj);
				}
			}
		}

		prevUp = up;
		prevDown = down;
		prevEnter = enter;
		prevEsc = esc;
		prevLmb = lmb;
		prevPgUp = pgUp;
		prevPgDn = pgDn;
		clampSelection(buildRows());
	}

	void drawPicker() {
		if(!pickerOpen)
			return;
		if(!isLobbyGuiDrawScreen(currentScreen) || !Game::hasValidFrontendDrawContext())
			return;

		int *gamescene = Game::getAddrGameScene();
		if(!gamescene)
			return;

		auto rows = buildRows();
		clampSelection(rows);

		int y = kPanelY;
		drawText(gamescene, kPanelX, y, "=== wkJellyWorm packages ===", 1);
		y += kRowH;
		if(!canEditPackages()) {
			drawText(gamescene, kPanelX, y, "Read-only (host controls packages in online lobby)", 2);
			y += kRowH;
		}
		drawText(gamescene, kPanelX, y, "Up/Down/PgUp/PgDn + Enter/click, type=filter, Esc=clear/close", 2);
		y += kRowH;
		std::string filterLine = filterText.empty() ? "Filter: (none)" : ("Filter: " + filterText);
		drawText(gamescene, kPanelX, y, filterLine, 2);
		y += kRowH;

		if(!rows.empty() && selectedIndex >= 0 && selectedIndex < (int)rows.size()) {
			auto &sel = rows[selectedIndex];
			std::string meta = sel.author.empty() ? sel.description : (sel.author + " — " + sel.description);
			if(meta.size() > 80) meta = meta.substr(0, 77) + "...";
			if(meta.empty()) meta = "(no description)";
			drawText(gamescene, kPanelX, y, meta, 1);
		} else {
			drawText(gamescene, kPanelX, y, "(no packages)", 2);
		}
		y += kRowH;

		int listTop = y;
		for(int i = 0; i < kVisibleRows; i++) {
			int idx = scrollOffset + i;
			if(idx >= (int)rows.size())
				break;
			auto &row = rows[idx];
			std::string line = (row.enabled ? "[x] " : "[ ] ");
			line += row.name + " " + row.version;
			if(idx == selectedIndex)
				line = "> " + line;
			// listPackagesForGui groups enabled then disabled; mark the boundary
			if(idx > 0 && rows[idx - 1].enabled && !row.enabled && filterText.empty())
				line = "- " + line;
			drawText(gamescene, kPanelX, y + i * kRowH, line, row.enabled ? 1 : 2);
		}

		// scrollbar
		if((int)rows.size() > kVisibleRows) {
			int barX = kPanelX + kPanelW - 10;
			int barH = kVisibleRows * kRowH;
			drawText(gamescene, barX, listTop, "|", 2);
			int thumbH = (std::max)(8, barH * kVisibleRows / (int)rows.size());
			int maxScroll = (int)rows.size() - kVisibleRows;
			int thumbY = listTop;
			if(maxScroll > 0)
				thumbY = listTop + (barH - thumbH) * scrollOffset / maxScroll;
			drawText(gamescene, barX, thumbY, "#", 1);
		}
	}

}

int LobbyGui::install(SignatureScanner &signatureScanner, module mod) {
	(void)signatureScanner;
	(void)mod;
	if(!Config::lobbyGuiEnabled)
		return 0;

	// LobbyGui::tick() is invoked from Renderer::hookRenderDrawingQueue (single hook).
	learnedSchemeScreen = Config::interceptSchemeScreen;
	learnedMapScreen = Config::interceptMapScreen;
	return 0;
}

void LobbyGui::onFrontendScreen(int screen) {
	int prev = currentScreen;
	currentScreen = screen;
	if(LobbyGui::isLobbyDrawScreen(screen))
		lastLobbyScreen = screen;

	if(isNetworkMenuScreen(screen))
		pickerOpen = false;

	if(Config::logFrontendScreens) {
		std::ofstream log(Config::jellyDir + "\\frontend_screens.log", std::ios::app);
		if(log.good())
			log << screen << "\n";
		printf("FrontendChangeScreen: %d\n", screen);
	}

	// Auto-learn scheme/map screen IDs when leaving lobby to an unknown screen
	if(Config::autoInterceptFrontend
		&& (prev == 1704 || prev == 1706)
		&& !isKnownLobbyScreen(screen)
		&& screen > 0) {
		seenFromLobby.insert(screen);
		if(!learnedSchemeScreen) {
			learnedSchemeScreen = screen;
			printf("LobbyGui: auto-learned InterceptSchemeScreen=%d\n", screen);
			if(Config::logFrontendScreens) {
				std::ofstream log(Config::jellyDir + "\\frontend_screens.log", std::ios::app);
				if(log.good())
					log << "suggested InterceptSchemeScreen=" << screen << "\n";
			}
		} else if(!learnedMapScreen && screen != learnedSchemeScreen) {
			learnedMapScreen = screen;
			printf("LobbyGui: auto-learned InterceptMapScreen=%d\n", screen);
			if(Config::logFrontendScreens) {
				std::ofstream log(Config::jellyDir + "\\frontend_screens.log", std::ios::app);
				if(log.good())
					log << "suggested InterceptMapScreen=" << screen << "\n";
			}
		}
	}
}

bool LobbyGui::shouldInterceptFrontendScreen(int screen, int &redirectTo) {
	if(!Config::lobbyGuiEnabled)
		return false;

	int schemeId = Config::interceptSchemeScreen ? Config::interceptSchemeScreen : learnedSchemeScreen;
	int mapId = Config::interceptMapScreen ? Config::interceptMapScreen : learnedMapScreen;

	if(schemeId && screen == schemeId) {
		open();
		redirectTo = lastLobbyScreen ? lastLobbyScreen : 1704;
		return true;
	}
	if(mapId && screen == mapId) {
		open();
		redirectTo = lastLobbyScreen ? lastLobbyScreen : 1704;
		return true;
	}
	return false;
}

void LobbyGui::open() {
	pickerOpen = true;
	selectedIndex = 0;
	scrollOffset = 0;
}

void LobbyGui::close() {
	pickerOpen = false;
}

void LobbyGui::toggle() {
	pickerOpen = !pickerOpen;
	if(pickerOpen) {
		selectedIndex = 0;
		scrollOffset = 0;
		filterText.clear();
	}
}

bool LobbyGui::isOpen() {
	return pickerOpen;
}

void LobbyGui::tick() {
	if(!Config::lobbyGuiEnabled)
		return;

	if(!Game::hasValidFrontendDrawContext() || !isLobbyGuiDrawScreen(currentScreen)) {
		if(pickerOpen)
			pickerOpen = false;
		prevShiftLmb = false;
		return;
	}

	// Shift+click opens picker on host lobby / single player only
	if(!pickerOpen) {
		int screen = currentScreen;
		if(screen == 1704 || screen == 21) {
			bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			bool shift = Input::isKeyDown(VK_SHIFT);
			if(lmb && shift && !prevShiftLmb)
				open();
			prevShiftLmb = lmb && shift;
		} else {
			prevShiftLmb = false;
		}
	}

	if(!pickerOpen)
		return;
	void *lobbyObj = Lobby::getHostObj();
	handleInput(lobbyObj);
	drawPicker();
}

bool LobbyGui::isLobbyDrawScreen(int screen) {
	return screen == 1704 || screen == 1706 || screen == 21;
}

bool LobbyGui::shouldTickFromRenderHook() {
	if(!Config::lobbyGuiEnabled)
		return false;
	int screen = Lobby::getFrontendScreen();
	return isLobbyGuiDrawScreen(screen) && Game::hasValidFrontendDrawContext();
}
