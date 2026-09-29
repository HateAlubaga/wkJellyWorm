#include "Lobby.h"
#include <cstdlib>
#include <MinHook.h>
#include <json.hpp>
#include "packages/PackageManager.h"
#include "LobbyGui.h"
#include "Game.h"
#include "MemAccess.h"

#include "Config.h"
#include "Hooks.h"
#include "Lua.h"

namespace {
	constexpr int kLobbyDisplayMsgOffset = 0x10318;
	std::string pendingClientCapsJson;
	// KNOWN ISSUE (do not "fix" by chunking -- that was tried and made things
	// worse): addrLobbySendGreentext (WA @0x4AA990) memcpy()s the whole
	// "nickname + sep + msg" string into a fixed global buffer at 0x8C22CA
	// (WA @0x4AA930 -> CRT memcpy @0x5DC530) with no bounds check. The next
	// referenced global sits at 0x8C2424, so usable capacity is only ~346
	// bytes. sendConfigString's JSON exceeds that once enough packages are
	// enabled, smearing adjacent globals -- that is the 0xc0000005 crash seen
	// during multiplayer lobby team setup, where the faulting pointer decoded
	// to ASCII "auth" (bytes from the config's own "authorName" field).
	// Splitting it into small reassembled chunks avoided the overflow but
	// broke sprite rendering game-wide for reasons not yet understood, so the
	// original single-message behaviour is kept until there is a transport
	// that is actually understood. Affects multiplayer lobby only.
}


void (__fastcall *addrLobbySendGreentext)(char * msg, void * EDX, void* This, int a3, int a4);
int (__fastcall *origLobbyHostCommands)(void* This, void* EDX, char **commandstrptr, char **argstrptr);
int __fastcall Lobby::hookLobbyHostCommands(void *This, void *EDX, char **commandstrptr, char **argstrptr) {
	std::string command = std::string(commandstrptr[0]);
	std::string args = std::string(argstrptr[0]);

	addrLobbyHostObj = (DWORD)This;

	if(command == "help") {
		showGreenText(This,
		"wkJellyWorm commands:\n" \
			"/help - display this help message\n" \
			"/save <name> - save current configuration as a scheme\n" \
			"/load <name> - load existing scheme\n" \
			"/reset - reset configuration\n" \
			"/reload - reload enabled packages from disk\n" \
			"/enable <package> - enable wkJellyWorm lua package (/enable rev = jellyreverse v2)\n" \
			"/disable <package> - disable wkJellyWorm lua package (/disable rev)\n" \
			"/jpx [list|enable <sbornik>|disable <sbornik>|reload] - PX scheme bundles from jelly/jpx (93 schemes)\n" \
			"/packages gui - open package picker overlay in lobby (same as /enable via list)\n" \
			"/autostart [scan|list|add <pkg> [-v version]|remove <pkg>|clear] - packages auto-enabled in single player / on lobby entry\n" \
			"  scan quickly writes package directory names as name=0 (no manifest parsing; not done automatically on startup)\n" \
			"  edit [autostart] section in wkJellyWorm.ini directly: name=0 off, name=1 latest, name=1.2.3 pinned\n" \
			"  [general] ScanAllPackages=0 in wkJellyWorm.ini: skip the full jelly/packages scan on startup,\n" \
			"    load only [autostart] entries with value != 0; /enable <name> loads that package by folder name\n" \
			"/jelly - show your wkJellyWorm version\n" \
			"/jelly api - dump Lua API to jelly/api.txt\n" \
			"/vt - list CTask tree (dev console only)"
			);
		return 1;
	}
	if(command == "jelly") {
		while(!args.empty() && (args[0] == ' ' || args[0] == '\t'))
			args.erase(0, 1);
		if(args == "api" || args.rfind("api ", 0) == 0) {
			int n = Lua::getInstance().dumpApiToFile("jelly\\api.txt");
			std::string msg;
			if(n < 0)
				msg = "failed to write jelly/api.txt";
			else
				msg = "API dump: jelly/api.txt (" + std::to_string(n) + " functions)";
			showGreenText(This, msg);
			return 1;
		}
		sendGreenTextMe(This, Config::getJellyVersionMessage());
		return 1;
	}
	else if(command == "reset") {
		Config::resetConfig();
		sendConfigString(This);
		sendGreenTextSystem(This, "Cleared wkJellyWorm config");
		return 1;
	}
	else if(command == "save" || command == "load") {
		args.erase(std::remove_if(args.begin(), args.end(), []( auto const& c ) -> bool { return !std::isalnum(c); } ), args.end());
		if(args.length()) {
			if(command == "save") {
				if(!Config::saveConfigToFile(args)) {
					std::string msg = "Saved current config as: " + args;
					showGreenText(This, msg);
				} else {
					std::string msg = "Failed to save current config as: " + args;
					showGreenText(This, msg);
				}
			}
			if(command == "load") {
				if(!Config::loadConfigFromFile(args)) {
					std::string msg = "Loaded config: " + args;
					sendConfigString(This);
					showGreenText(This, msg);
				} else {
					std::string msg = "Failed to load config: " + args;
					showGreenText(This, msg);
				}
			}
		} else {
			std::string usage = "Usage: /" + command + " scheme\n";
			usage += "\tList of available schemes: " + Config::listConfigFiles();
			showGreenText(This, usage);
		}
		return 1;
	}
	if(PackageManager::getInstance().handleLobbyHostCommand(This, command, args))
		return 1;

	return origLobbyHostCommands(This, EDX, commandstrptr, argstrptr);
}

int (__fastcall *origLobbyClientCommands)(void* This, void* EDX, char **commandstrptr, char **argstrptr);
int __fastcall Lobby::hookLobbyClientCommands(void *This, void *EDX, char **commandstrptr, char **argstrptr) {
	std::string command = std::string(commandstrptr[0]);
	std::string args = std::string(argstrptr[0]);
	if(isValidLobbyObj(This))
		addrLobbyClientObj = (DWORD)This;
	flushPendingClientCaps();

	if(command == "jelly") {
		while(!args.empty() && (args[0] == ' ' || args[0] == '\t'))
			args.erase(0, 1);
		if(args == "api" || args.rfind("api ", 0) == 0) {
			int n = Lua::getInstance().dumpApiToFile("jelly\\api.txt");
			std::string msg;
			if(n < 0)
				msg = "failed to write jelly/api.txt";
			else
				msg = "API dump: jelly/api.txt (" + std::to_string(n) + " functions)";
			showGreenText(This, msg);
			return 1;
		}
		sendGreenTextMe(This, Config::getJellyVersionMessage());
		return 1;
	}
	if(PackageManager::getInstance().handleLobbyClientCommand(This, command, args))
		return 1;
	if(PackageManager::getInstance().handlePackageChatCommand(This, command, args))
		return 1;
	return origLobbyClientCommands(This, EDX, commandstrptr, argstrptr);
}

bool Lobby::isValidLobbyObj(void *This) {
	if(!This)
		return false;
	DWORD addr = (DWORD)This;
	return addr > 0x10000 && jellyProbeRead(addr, 0x40);
}

void Lobby::sendGreenTextMe(void *This, std::string msg) {
	if(!isValidLobbyObj(This)) {
		printf("sendGreenTextMe: invalid lobby obj %p\n", This);
		return;
	}
	std::string buff = (char*)addrMyNickname;
	buff += " ";
	buff += msg;
	addrLobbySendGreentext((char*)buff.c_str(), 0, This, 0, 0);
}

void Lobby::sendGreenTextSystem(void *This, std::string msg) {
	if(!isValidLobbyObj(This)) {
		printf("sendGreenTextSystem: invalid lobby obj %p (msg len=%zu)\n", This, msg.size());
		return;
	}
	addrLobbySendGreentext((char*)msg.c_str(), 0, This, 0, 0);
}

void Lobby::showGreenText(void *This, std::string msg) {
	std::string buff = "SYS:wkJellyWorm:ALL:" + msg;
	hookLobbyDisplayMessage((int)This + 0x10318, (char*)buff.c_str());
}

int Lobby::sendConfigString(void * This) {
	std::string config = "wkJellyWormConfigStr:" + Config::getConfigString(-1, true);
	sendGreenTextSystem(This, config);
	return 0;
}

bool Lobby::isHost() {
	return addrLobbyHostObj != 0;
}

void *Lobby::getHostObj() {
	return addrLobbyHostObj ? (void *)addrLobbyHostObj : nullptr;
}

void Lobby::captureHostLobbyObj(void *This) {
	if(This && (currentFrontendScreen == 1704 || currentFrontendScreen == 0))
		addrLobbyHostObj = (DWORD)This;
}

void Lobby::captureClientLobbyObjFromDisplay(int displayMsgThis) {
	if(addrLobbyHostObj || currentFrontendScreen != 1706)
		return;
	if(displayMsgThis < 0x10000)
		return;
	void *candidate = (void *)(displayMsgThis - kLobbyDisplayMsgOffset);
	if(isValidLobbyObj(candidate))
		addrLobbyClientObj = (DWORD)candidate;
}

void *Lobby::resolveClientLobbyObj(int displayMsgThis) {
	captureClientLobbyObjFromDisplay(displayMsgThis);
	if(addrLobbyClientObj && isValidLobbyObj((void *)addrLobbyClientObj))
		return (void *)addrLobbyClientObj;
	if(displayMsgThis >= 0x10000) {
		void *fallback = (void *)(displayMsgThis - kLobbyDisplayMsgOffset);
		if(isValidLobbyObj(fallback))
			return fallback;
	}
	return nullptr;
}

void Lobby::flushPendingClientCaps() {
	if(pendingClientCapsJson.empty() || !addrLobbyClientObj)
		return;
	if(!isValidLobbyObj((void *)addrLobbyClientObj))
		return;
	std::string caps = "wkJellyWormClientCaps:" + pendingClientCapsJson;
	sendGreenTextSystem((void *)addrLobbyClientObj, caps);
	pendingClientCapsJson.clear();
}

int Lobby::getFrontendScreen() {
	return currentFrontendScreen;
}


int (__stdcall *origLobbyDisplayMessage)(int a1, char *msg);
int __stdcall Lobby::hookLobbyDisplayMessage(int a1, char *msg) {
	if(!addrLobbyHostObj) {
		captureClientLobbyObjFromDisplay(a1);
		flushPendingClientCaps();
	}

	char * pos[6];
	pos[0] = msg;
	for(size_t i=1; i < 6; i++) {
		pos[i] = strstr(pos[i-1] + 1, ":");
		if(!pos[i]) break;
		if(i==1 && strncmp(pos[i-1], "SYS:", 4)) break;
		else if(i==3 && strncmp(pos[i-1], ":ALL:", 5)) break;
		else if(i==4) {
			bool isConfig = !strncmp(pos[i-1], ":wkJellyWormConfigStr:", 22);
			bool isCaps = !strncmp(pos[i-1], ":wkJellyWormClientCaps:", 23);
			if(!isConfig && !isCaps) break;
		}
		else if(i==5) {
			bool isConfig = !strncmp(pos[3], ":wkJellyWormConfigStr:", 22);
			bool isCaps = !strncmp(pos[3], ":wkJellyWormClientCaps:", 23);
			if(isConfig) {
				if(!addrLobbyHostObj) {
					Config::parseConfigString(pos[i-1] + 1);
					void *lobbyObj = resolveClientLobbyObj(a1);
					if(lobbyObj) {
						addrLobbyClientObj = (DWORD)lobbyObj;
						std::string caps = "wkJellyWormClientCaps:" + Config::buildClientCapsJson();
						sendGreenTextSystem(lobbyObj, caps);
					} else {
						pendingClientCapsJson = Config::buildClientCapsJson();
						printf("wkJellyWorm: deferred client caps (no lobby obj yet)\n");
					}
				}
				return 1;
			}
			if(isCaps) {
				if(addrLobbyHostObj) {
					std::string hint;
					if(pos[1] && pos[2] && pos[2] > pos[1] + 1)
						hint.assign(pos[1] + 1, (size_t)(pos[2] - pos[1] - 1));
					Config::handleClientCaps(pos[i-1] + 1, hint);
				}
				return 1;
			}
		}
	}
	return origLobbyDisplayMessage(a1, msg);
}
void (__stdcall *origFrontendChangeScreen)(int screen);
void __stdcall Lobby::hookFrontendChangeScreen(int screen) {
	int sesi;
	_asm mov sesi, esi

	Game::probeAndInvalidateStalePointers();

	LobbyGui::onFrontendScreen(screen);
	currentFrontendScreen = screen;

	int redirectTo = screen;
	if(LobbyGui::shouldInterceptFrontendScreen(screen, redirectTo))
		screen = redirectTo;

	switch(screen) {
		case 10:	// main screen
		case 11:	// local multiplayer
		case 21:	// single player
		case 1700:	// network
		case 1702:	// lan network
		case 1703:  // wormnet
		case 1704: 	// lobby host
		case 1706:	// lobby client
			Config::saveSessionConfig();
			addrLobbyHostObj = 0;
			addrLobbyClientObj = 0;
			pendingClientCapsJson.clear();
			Config::resetConfig();
			// Autostart normally only applies when a match is actually being
			// set up, which makes the package layer impossible to exercise
			// from the main menu -- and therefore impossible to verify without
			// a human clicking into single player. [general] SelfTest=1 also
			// applies it on the main screen so an automated launch can load
			// every package, produce the usual logs, and be checked headlessly.
			if(screen == 21 || screen == 1704
				|| (Config::selfTestEnabled && screen == 10)) {
				int applied = PackageManager::getInstance().applyAutostart();
				if(Config::selfTestEnabled) {
					// printf goes to the dev console, which cannot be captured
					// from outside the process; a file can be.
					printf("SelfTest: applyAutostart(screen %d) applied %d package(s)\n", screen, applied);
					FILE *f = nullptr;
					if(fopen_s(&f, "jelly\\selftest.log", "a") == 0 && f) {
						fprintf(f, "applyAutostart screen=%d applied=%d\n", screen, applied);
						fclose(f);
					}
				}
			}
			if(screen == 1704)
				Config::loadSessionConfig();
			break;
		default:
			break;
	}

	_asm mov esi, sesi
	_asm push screen
	_asm call origFrontendChangeScreen
}


// LobbyPrintUsedVersion runs during host lobby init — capture This before first chat command
int flagCwndEnableWindow = 0;
int (__fastcall *origLobbyPrintUsedVersion)(void * This, int EDX);
int __fastcall hookLobbyPrintUsedVersion(void * This, int EDX) {
	flagCwndEnableWindow = 1;
	Lobby::captureHostLobbyObj(This);
	int ret = origLobbyPrintUsedVersion(This, EDX);
	flagCwndEnableWindow = 0;
	return ret;
}

int (__fastcall *origCwndEnableWindow)(void * This, int EDX, int enable);
int __fastcall Lobby::hookCwndEnableWindow(void * This, int EDX, int enable) {
	if (flagCwndEnableWindow && enable && addrLobbyHostObj) {
		sendConfigString((void*)addrLobbyHostObj);
	}
	return origCwndEnableWindow(This, EDX, enable);
}



int Lobby::install(SignatureScanner & signatureScanner, module mod) {
	DWORD addrLobbyHostCommands = Hooks::scanPattern("LobbyHostCommands", "\x55\x8B\xEC\x83\xE4\xF8\x64\xA1\x00\x00\x00\x00\x6A\xFF\x68\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x56\x8B\x75\x08\x8B\x06\x57\x68\x00\x00\x00\x00\x50\x8B\xF9\xE8\x00\x00\x00\x00\x83\xC4\x08\x85\xC0\x0F\x84\x00\x00\x00\x00\x8B\x06\x68\x00\x00\x00\x00\x50\xE8\x00\x00\x00\x00", "??????xx????xxx????xxxx????xx????xxxxxxxxx????xxxx????xxxxxxx????xxx????xx????", 0x4B9B00);
	DWORD addrLobbyClientCommands = Hooks::scanPattern("LobbyClientCommands", "\x55\x8B\xEC\x83\xE4\xF8\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x40\x53\x56\x8B\x75\x08\x8B\x06\x57\x8B\xD9\x68\x00\x00\x00\x00\x50\x89\x5C\x24\x1C\xE8\x00\x00\x00\x00\x83\xC4\x08\x85\xC0", "??????xxx????xx????xxxx????xxxxxxxxxxxxxx????xxxxxx????xxxxx", 0x4AABB0);
	DWORD addrLobbyDisplayMessage = Hooks::scanPattern("LobbyDisplayMessage", "\x55\x8B\xEC\x83\xE4\xF8\x64\xA1\x00\x00\x00\x00\x6A\xFF\x68\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x30\x53\x56\x57\xE8\x00\x00\x00\x00\x33\xC9\x85\xC0\x0F\x95\xC1\x85\xC9\x75\x0A\x68\x00\x00\x00\x00\xE8\x00\x00\x00\x00", "??????xx????xxx????xxxx????xxxxxxx????xxxxxxxxxxxx????x????", 0x493CB0);
	DWORD addrFrontendChangeScreen = Hooks::scanPattern("FrontendChangeScreen", "\x83\x3D\x00\x00\x00\x00\x00\x53\x8B\x5C\x24\x08\x75\x14\x8B\x46\x3C\xA8\x18\x74\x59\x83\xE0\xEF\x89\x5E\x44\x89\x46\x3C\x5B\xC2\x04\x00\x6A\x00\x8B\xCE\xE8\x00\x00\x00\x00\x8B\x86\x00\x00\x00\x00\x50\x8B\x86\x00\x00\x00\x00\x68\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x33\xC0\x57", "???????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xx????xxx????x????x????xxx", 0x447A20);
	DWORD addrLobbyPrintUsedVersion = Hooks::scanPattern("LobbyPrintUsedVersion", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x55\x56\x57\xBF\x00\x00\x00\x00\x89\x4C\x24\x70\xC7\x44\x24\x00\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x85\xC0\x89\x84\x24\x00\x00\x00\x00", "???????xx????xxxx????xx????xxxxx????xxxxxxx?????x????xxxxx????", 0x4B7E20);
	DWORD addrCwndEnableWindow = Hooks::scanPattern("CwndEnableWindow", "\x83\x79\x50\x00\x75\x10\xFF\x74\x24\x04\xFF\x71\x20\xFF\x15\x00\x00\x00\x00\xC2\x04\x00\x8B\x49\x50\x8B\x01\xFF\xA0\xA8\x00\x00\x00", "??????xxxxxxxxx????xxxxxxxxxxxxxx", 0x5C647A);

	addrLobbySendGreentext =
			(void (__fastcall *)(char*,void *,void *,int, int))
			Hooks::scanPattern("LobbySendGreentext", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x51\x56\x57\x8B\xF1\x68\x00\x00\x00\x00\x8D\x4C\x24\x0C\xE8\x00\x00\x00\x00\x80\x7C\x24\x00\x00", "???????xx????xxxx????xxxxxx????xxxxx????xxx??", 0x4AA990);
	addrMyNickname = *(DWORD*)((DWORD)addrLobbySendGreentext + 0x4D);

	Hooks::polyhook("LobbyHostCommands", addrLobbyHostCommands, (DWORD *) &hookLobbyHostCommands, (DWORD *) &origLobbyHostCommands);
	Hooks::polyhook("LobbyClientCommands", addrLobbyClientCommands, (DWORD *) &hookLobbyClientCommands, (DWORD *) &origLobbyClientCommands);
	Hooks::polyhook("LobbyDisplayMessage", addrLobbyDisplayMessage, (DWORD *) &hookLobbyDisplayMessage, (DWORD *) &origLobbyDisplayMessage);
	Hooks::polyhook("FrontendChangeScreen", addrFrontendChangeScreen, (DWORD *) &hookFrontendChangeScreen, (DWORD *) &origFrontendChangeScreen);
	Hooks::polyhook("LobbyPrintUsedVersion", addrLobbyPrintUsedVersion, (DWORD *) &hookLobbyPrintUsedVersion, (DWORD *) &origLobbyPrintUsedVersion);
	Hooks::polyhook("CwndEnableWindow", addrCwndEnableWindow, (DWORD *) &hookCwndEnableWindow, (DWORD *) &origCwndEnableWindow);

	return 0;
}
