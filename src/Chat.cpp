#include <MinHook.h>
#include "Chat.h"
#include "Game.h"
#include "CustomWeapons.h"
#include "Hooks.h"
#include "Lua.h"
#include "Config.h"
#include "Music.h"
#include "SchemeBind.h"
#include <sol/sol.hpp>
#include "packages/PackageManager.h"
#include "renderer/Renderer.h"
#include "entities/tasks/CTaskTurnGame.h"
#include <cstring>
#include <sstream>
#include <vector>

namespace {
	const char kSyncPrefix[] = "wkJellySync:";

	void (__stdcall *origShowChatMessage)(DWORD addrResourceObject, int color, char * msg, int unk) = nullptr;
	DWORD origOnChatInput = 0;

	bool g_incomingSyncChat = false;
	int g_incomingSyncGen = 0;

	int __stdcall callOriginalOnChatInput(int a1, char * msg, int a3) {
		_asm mov ecx, a1
		_asm mov eax, msg
		_asm push a3
		_asm call origOnChatInput
	}

	bool tryHandleIncomingSyncChat(const char *msg) {
		if(!msg)
			return false;
		const char *pos = strstr(msg, kSyncPrefix);
		if(!pos)
			return false;

		const char *payload = pos + strlen(kSyncPrefix);
		int gen = 0;
		if(payload[0] == 'g') {
			gen = atoi(payload + 1);
			const char *colon = strchr(payload, ':');
			if(!colon)
				return false;
			payload = colon + 1;
		}

		std::string cmd = payload;
		while(!cmd.empty() && (cmd[0] == ' ' || cmd[0] == '\t'))
			cmd.erase(0, 1);
		if(cmd.empty())
			return false;
		if(cmd[0] != '/')
			cmd = "/" + cmd;

		std::vector<char> buf(cmd.begin(), cmd.end());
		buf.push_back('\0');

		g_incomingSyncGen = gen;
		g_incomingSyncChat = true;
		PackageManager::getInstance().handleOnChatInput(buf.data());
		g_incomingSyncChat = false;
		g_incomingSyncGen = 0;
		return true;
	}

	void __stdcall hookShowChatMessage(DWORD addrResourceObject, int color, char * msg, int unk) {
		if(msg && tryHandleIncomingSyncChat(msg))
			return;
		origShowChatMessage(addrResourceObject, color, msg, unk);
	}
}

void Chat::callShowChatMessage(std::string msg, int color) {
	if(!Game::isValidDdGame() || !origShowChatMessage)
		return;
	origShowChatMessage(Game::getAddrDDGame(), color, (char*)msg.c_str(), 1);
}

bool Chat::sendSyncChat(const std::string &payload) {
	if(!Game::isValidDdGame() || !Game::getAddrWsGameNet() || !origOnChatInput)
		return false;
	if(payload.empty() || payload.size() > 200)
		return false;

	std::string msg = std::string(kSyncPrefix) + payload;
	std::vector<char> buf(msg.begin(), msg.end());
	buf.push_back('\0');
	callOriginalOnChatInput(Game::getAddrDDGame(), buf.data(), 1);
	return true;
}

bool Chat::isIncomingSyncChat() {
	return g_incomingSyncChat;
}

int Chat::getIncomingSyncGen() {
	return g_incomingSyncGen;
}

int Chat::onChatInput(int a1, char * msg, int a3) {
	size_t len = strlen(msg);
	if(!strcmp(msg, "/test")) {
		callShowChatMessage("wkJellyWorm test", 6);
		return 1;
	}

	std::string norm;
	bool hadSlash = false;  // a chat line that merely starts with the word "music" is not a command
	if(msg && msg[0]) {
		norm = msg;
		while(!norm.empty() && norm[0] == ' ') norm.erase(0, 1);
		hadSlash = !norm.empty() && norm[0] == '/';
		if(!norm.empty() && norm[0] != '/') norm = "/" + norm;
	}

	if(Config::devConsoleEnabled && !norm.empty()) {
		if(norm == "/reload" || norm.rfind("/reload ", 0) == 0) {
			PackageManager::getInstance().reloadPackages(nullptr);
			return 1;
		}
		if(norm.rfind("/lua ", 0) == 0 || norm.rfind("/lua\t", 0) == 0) {
			std::string code = norm.substr(5);
			while(!code.empty() && (code[0] == ' ' || code[0] == '\t'))
				code.erase(0, 1);
			if(code.size() > 512) {
				callShowChatMessage("wkJellyWorm: /lua input too long (max 512)", 7);
				return 1;
			}
			std::string result = Lua::getInstance().evalRepl(code);
			if(result.size() > 200)
				result = result.substr(0, 200) + "...";
			callShowChatMessage(result.empty() ? "ok" : result, 6);
			return 1;
		}
	}

	if(!norm.empty() && (norm == "/autostart" || norm.rfind("/autostart ", 0) == 0)) {
		std::string args = norm.size() > 10 ? norm.substr(10) : "";
		while(!args.empty() && (args[0] == ' ' || args[0] == '\t'))
			args.erase(0, 1);
		PackageManager::getInstance().handleAutostartCommand(nullptr, args);
		return 1;
	}

	if(hadSlash && (norm == "/scheme" || norm.rfind("/scheme ", 0) == 0)) {
		std::string reply = SchemeBind::handleCommand(norm.size() > 7 ? norm.substr(7) : "");
		std::stringstream lines(reply);
		std::string line;
		while(std::getline(lines, line)) {
			if(!line.empty())
				callShowChatMessage(line, 6);
		}
		return 1;
	}

	if(hadSlash && (norm == "/music" || norm.rfind("/music ", 0) == 0)) {
		std::string reply = Music::handleCommand(norm.size() > 6 ? norm.substr(6) : "");
		std::stringstream lines(reply);
		std::string line;
		while(std::getline(lines, line)) {
			if(!line.empty())
				callShowChatMessage(line, 6);
		}
		return 1;
	}

	if(!norm.empty() && (norm == "/jelly api" || norm.rfind("/jelly api ", 0) == 0)) {
		int n = Lua::getInstance().dumpApiToFile("jelly\\api.txt");
		if(n < 0)
			callShowChatMessage("wkJellyWorm: failed to write jelly/api.txt", 7);
		else
			callShowChatMessage("API dump: jelly/api.txt (" + std::to_string(n) + " functions)", 6);
		return 1;
	}

	if(!norm.empty()) {
		std::string withSlash = norm;
		if(withSlash[0] != '/')
			withSlash = "/" + withSlash;
		std::string cmdName = PackageManager::commandNameFromMessage(withSlash.c_str());
		if(!cmdName.empty() && PackageManager::isChatRoutableLobbyCommand(cmdName)) {
			std::vector<char> buf(withSlash.begin(), withSlash.end());
			buf.push_back('\0');
			PackageManager::getInstance().handleOnChatInput(buf.data());
			return 1;
		}
	}
	if(Config::devConsoleEnabled && !norm.empty() && (norm == "/vt" || norm == "/listtasks")) {
		std::string dump = Game::listAllTasks();
		if(dump.size() > 180)
			dump = dump.substr(0, 180) + "...";
		callShowChatMessage(dump.empty() ? "listTasks ok" : dump, 6);
		return 1;
	}
	if(!strcmp(msg, "/arma")) {
		CTaskTurnGame::triggerArmageddon(100, 166);
		return 1;
	}
	if(!strcmp(msg, "/quake")) {
		CTaskTurnGame::triggerQuake(10);
		return 1;
	}
//	if(!strcmp(msg, "/spawn")) {
//		SpawnObjectParams params = {0};
//		params.type = SpawnObjectParams::Mine;
//		Game::spawnObject(&params);
//		return 1;
//	}
	if(!strcmp(msg, "/weapons")) {
		CustomWeapons::printDebugAddrs();
		return 1;
	}
//	if(!strcmp(msg, "/table")) {
//		CustomWeapons::switchWeaponTable();
//		return 1;
//	}
	if(!strcmp(msg, "/crash")) {
		*(DWORD*)0 += 1;
	}
	if(!strcmp(msg, "/drawing")) {
		printf("toggling native renderer");
		Renderer::toggleNativeDrawing();
		return 1;
	}

	if(PackageManager::getInstance().handleOnChatInput(msg))
		return 1;
	return 0;
}

#pragma optimize( "", off )
char* onchat_eax;
int onchat_ecx;
void __stdcall Chat::hookOnChatInput(int a3) {
	_asm mov onchat_eax, eax
	_asm mov onchat_ecx, ecx
	if(!Chat::onChatInput(onchat_ecx, (char*)onchat_eax, a3)) {
		callOriginalOnChatInput(onchat_ecx, onchat_eax, a3);
	}
}
#pragma optimize( "", on )

int Chat::install(SignatureScanner & signatureScanner, module mod) {
	DWORD addrShowChatMessage = Hooks::scanPattern("ShowChatMessage", "\x81\xEC\x00\x00\x00\x00\x53\x55\x8B\xAC\x24\x00\x00\x00\x00\x80\xBD\x00\x00\x00\x00\x00\x8B\x85\x00\x00\x00\x00\x8B\x48\x24\x56\x8B\xB1\x00\x00\x00\x00\x57", "??????xxxxx????xx?????xx????xxxxxx????x", 0x52ACB0);
	DWORD addrOnChatInput = Hooks::scanPattern("OnChatInput", "\x81\xEC\x00\x00\x00\x00\x55\x56\x57\x8B\xF8\x8A\x07\x84\xC0\x8B\xF1\x0F\x84\x00\x00\x00\x00\x3C\x2F\x0F\x85\x00\x00\x00\x00\x8D\x44\x24\x40", "??????xxxxxxxxxxxxx????xxxx????xxxx", 0x52B730);

	Hooks::polyhook("ShowChatMessage", addrShowChatMessage, (DWORD *) &hookShowChatMessage, (DWORD *) &origShowChatMessage);
	Hooks::polyhook("onChatInput", addrOnChatInput, (DWORD *) &hookOnChatInput, (DWORD *) &origOnChatInput);

	auto * lua = Lua::getInstance().getState();
	lua->set_function("showChatMessage", &callShowChatMessage);
	lua->set_function("sendSyncChat", &Chat::sendSyncChat);
	lua->set_function("isIncomingSyncChat", &Chat::isIncomingSyncChat);
	lua->set_function("getIncomingSyncGen", &Chat::getIncomingSyncGen);
	return 0;
}
