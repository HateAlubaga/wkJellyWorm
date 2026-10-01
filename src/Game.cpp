#include "Game.h"
#include <MinHook.h>
#include "packages/PackageManager.h"
#include "Config.h"
#include "entities/Entities.h"
#include "MemAccess.h"

#include "Chat.h"
#include "Landscape.h"
#include "CustomWeapons.h"
#include "Hooks.h"
#include "Lua.h"
#include <sol/sol.hpp>
#include "Sounds.h"
#include "Sprites.h"
#include "Music.h"
#include "renderer/Renderer.h"
#include "Lobby.h"
#include <sstream>
#include <ctime>
#include <cstdio>


static void collectTasks(CTask *task, int depth, int &count, std::stringstream &ss, int maxDepth = 8, int maxCount = 256) {
	if(!task || depth > maxDepth || count >= maxCount)
		return;
	count++;
	for(int i = 0; i < depth; i++)
		ss << "  ";
	ss << Game::getTaskTypeName(task->classtype)
		<< " @" << std::hex << (DWORD)task << std::dec
		<< " type=" << task->classtype
		<< " children=" << task->children.size()
		<< "\n";
	int n = task->children.size();
	for(int i = 0; i < n; i++) {
		CTask *child = task->children[i];
		if(child)
			collectTasks(child, depth + 1, count, ss, maxDepth, maxCount);
	}
}

std::string Game::getTaskTypeName(int classtype) {
	using CT = Constants::ClassType;
	switch(classtype) {
		case CT::ClassType_None: return "None";
		case CT::ClassType_Task: return "Task";
		case CT::ClassType_GameTask: return "GameTask";
		case CT::ClassType_GameCollisionTask: return "GameCollisionTask";
		case CT::ClassType_Task_Control: return "Control";
		case CT::ClassType_Task_Game: return "Game";
		case CT::ClassType_Task_TurnGame: return "TurnGame";
		case CT::ClassType_Task_Filter: return "Filter";
		case CT::ClassType_Task_Mine: return "Mine";
		case CT::ClassType_Task_Canister: return "Canister";
		case CT::ClassType_Task_Team: return "Team";
		case CT::ClassType_Task_Missile: return "Missile";
		case CT::ClassType_Task_Arrow: return "Arrow";
		case CT::ClassType_Task_Animation: return "Animation";
		case CT::ClassType_Task_Dirt: return "Dirt";
		case CT::ClassType_Task_Crate: return "Crate";
		case CT::ClassType_Task_Flame: return "Flame";
		case CT::ClassType_Task_AirStrike: return "AirStrike";
		case CT::ClassType_Task_Worm: return "Worm";
		case CT::ClassType_Task_OldWorm: return "OldWorm";
		case CT::ClassType_Task_Drill: return "Drill";
		case CT::ClassType_Task_Cross: return "Cross";
		case CT::ClassType_Task_Smoke: return "Smoke";
		case CT::ClassType_Task_Cloud: return "Cloud";
		case CT::ClassType_Task_Fire: return "Fire";
		case CT::ClassType_Task_Gass: return "Gass";
		case CT::ClassType_Task_FireBall: return "FireBall";
		case CT::ClassType_Task_SeaBubble: return "SeaBubble";
		case CT::ClassType_Task_Land: return "Land";
		case CT::ClassType_Task_ScoreBubble: return "ScoreBubble";
		case CT::ClassType_Task_OilDrum: return "OilDrum";
		case CT::ClassType_Task_CPU: return "CPU";
		case CT::ClassType_SpriteAnimation: return "SpriteAnimation";
		case CT::ClassType_CollisionManager: return "CollisionManager";
		default: return "Unknown";
	}
}

std::string Game::listAllTasks() {
	if(!Config::devConsoleEnabled)
		return "listTasks: enable [debug] EnableDevConsole=1";
	DWORD addr = getAddrTurnGameObject();
	if(!addr)
		return "listTasks: no TurnGame (not in match?)";
	std::stringstream ss;
	int count = 0;
	collectTasks(reinterpret_cast<CTask *>(addr), 0, count, ss);
	ss << "total=" << count << "\n";
	printf("%s", ss.str().c_str());
	return ss.str();
}

int (__stdcall *origConstructGlobalContext)(int a1);
int __stdcall Game::hookConstructGlobalContext(int a1) {
//	addrDDGame = a1;
	int ret = origConstructGlobalContext(a1);
	Config::onConstructGlobalContext(a1);
	auto pmanager = PackageManager::getInstance();
	pmanager.handleInitialize();
	CustomWeapons::assignPxArt();
	clearLiveTeams();
	pmanager.handleGameGlobalInit();
	Landscape::onConstructGlobalContext(a1);
	Sounds::onConstructGlobalContext(a1);
	Sprites::onConstructGlobalContext(a1);
	CustomWeapons::onConstructGlobalContext(a1);
	Renderer::onConstructGlobalContext(a1);


	auto addrGameGlobal = getAddrGameGlobal();
	if(addrGameGlobal)
		printf("addrGradient: 0x%X\n", addrGameGlobal + 0x30);
//	*(DWORD*)(addrGameGlobal + 0x30) = *(DWORD*)(addrGameGlobal + 0x34);
//	*(DWORD*)(addrGameGlobal + 0x30) = 0; //*(DWORD*)(addrGameGlobal + 0x30);

	return ret;
}

int (__fastcall *origDestroyGlobalContext)(int This, int EDX);
int __fastcall Game::hookDestroyGlobalContext(int This, int EDX) {
	Sprites::onDestroyGlobalContext();
	CustomWeapons::onDestroyGlobalContext();
	Music::shutdown();
	int ret = origDestroyGlobalContext(This, EDX);

	addrDDDisplay = addrDSSound = addrDDKeyboard = addrDDMouse = addrWavCDRom = addrWSGameNet = addrDDGame = 0;
	clearLiveTeams();
	return ret;
}

DWORD Game::getAddrDDGame() {
	return addrDDGame;
}

DWORD Game::getAddrGameinfoObject() {
	return addrGameinfoObject;
}

DWORD origConstructDDGameWrapper;
DWORD __stdcall Game::hookConstructDDGameWrapper(DWORD DD_Game_a2, DWORD DD_Display_a3, DWORD DS_Sound_a4, DWORD DD_Keyboard_a5, DWORD DD_Mouse_a6, DWORD WAV_CDrom_a7, DWORD WS_GameNet_a8) {
	DWORD DD_W2Wrapper, retv;
	_asm mov DD_W2Wrapper, edi

	addrDDGame = DD_Game_a2;
	printf("ddgame: %X\n", addrDDGame);
	addrDDDisplay = DD_Display_a3;
	addrDSSound = DS_Sound_a4;
	addrDDKeyboard = DD_Keyboard_a5;
	addrDDMouse = DD_Mouse_a6;
	addrWavCDRom = WAV_CDrom_a7;
	addrWSGameNet = WS_GameNet_a8;

	_asm push WS_GameNet_a8
	_asm push WAV_CDrom_a7
	_asm push DD_Mouse_a6
	_asm push DD_Keyboard_a5
	_asm push DS_Sound_a4
	_asm push DD_Display_a3
	_asm push DD_Game_a2
	_asm mov edi, DD_W2Wrapper
	_asm call origConstructDDGameWrapper
	_asm mov retv, eax

	return retv;
}


int Game::install(SignatureScanner & signatureScanner, module mod) {
	DWORD addrConstructGlobalContext =  Hooks::scanPattern("ConstructGlobalContext", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x24\x53\x55\x8B\x6C\x24\x3C\x8B\x85\x00\x00\x00\x00\x8B\x48\x24", "???????xx????xxxx????xxxxxxxxxxx????xxx", 0x526500);
	DWORD addrDestroyGlobalContext =  Hooks::scanPattern("DestroyGlobalContext","\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x51\x56\x8B\xF1\x89\x74\x24\x04\xC7\x06\x00\x00\x00\x00\x8B\xC6\xC7\x44\x24\x00\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xA1\x00\x00\x00\x00\x83\x78\x5C\x00\x75\x07\x8B\xC6\xE8\x00\x00\x00\x00\x8B\xC6\xE8\x00\x00\x00\x00\x8B\x86\x00\x00\x00\x00\x85\xC0\x74\x09\x50\xE8\x00\x00\x00\x00\x83\xC4\x04", "???????xx????xxxx????xxxxxxxxxx????xxxxx?????x????x????xxxxxxxxx????xxx????xx????xxxxxx????xxx", 0x56DFB0);
	DWORD addrReferencesGameinfo =  Hooks::scanPattern("ReferencesGameinfo", "\x80\x3D\x00\x00\x00\x00\x00\x75\x15\x56\x8B\x35\x00\x00\x00\x00\x6A\x01\xFF\xD6\x80\x3D\x00\x00\x00\x00\x00\x74\xF3\x5E\x8B\x44\x24\x04\x50\x68\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xC2\x04\x00", "???????xxxxx????xxxxxx?????xxxxxxxxx????x????xxx", 0x4E3B40);
	DWORD addrConstructDDGameWrapper = Hooks::scanPattern("ConstructDDGameWrapper", "\x6A\xFF\x64\xA1\x00\x00\x00\x00\x68\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x53\x8B\x5C\x24\x1C\x55\x8B\x6C\x24\x1C\x56\x8B\x74\x24\x1C\x33\xC0\x89\x86\x00\x00\x00\x00\x89\x44\x24\x14\x89\x86\x00\x00\x00\x00\x8B\xC7\xC7\x06\x00\x00\x00\x00\x89\x9E\x00\x00\x00\x00\x89\xAE\x00\x00\x00\x00\xE8\x00\x00\x00\x00", "????????x????xxxx????xxxxxxxxxxxxxxxxxxx????xxxxxx????xxxx????xx????xx????x????", 0x56DEF0);

	addrGameinfoObject =  *(DWORD*)(addrReferencesGameinfo + 0x24);

	Hooks::polyhook("constructGlobalContext", addrConstructGlobalContext, (DWORD *) &hookConstructGlobalContext, (DWORD *) &origConstructGlobalContext);
	Hooks::polyhook("destroyGlobalContext", addrDestroyGlobalContext, (DWORD *) &hookDestroyGlobalContext, (DWORD *) &origDestroyGlobalContext);
	Hooks::polyhook("ConstructDDGameWrapper", addrConstructDDGameWrapper, (DWORD *) &hookConstructDDGameWrapper, (DWORD *) &origConstructDDGameWrapper);

	CTask::CTask_vtableHooks<CTaskTeam>::registerBuiltinCallbackVt0(&onTeamVt0);
	CTask::CTask_vtableHooks<CTaskTeam>::registerBuiltinCallbackVt4(&onTeamVt4);

	auto * lua = Lua::getInstance().getState();
	lua->set_function("getTurnGameAddr", &Game::getAddrTurnGameObject);
	lua->set_function("getTurnGameObject", []() -> sol::optional<CTaskTurnGame *> {
		DWORD addr = Game::getAddrTurnGameObject();
		if (!addr) {
			return sol::nullopt;
		}
		return reinterpret_cast<CTaskTurnGame *>(addr);
	});
	lua->set_function("isHost", &Game::isHost);
	lua->set_function("isReplayPlayback", &Game::isReplayPlayback);
	lua->set_function("isMatchRunning", &Game::isMatchRunning);
	lua->set_function("getTeams", &Game::getTeams);
	lua->set_function("listTasks", []() -> std::string {
		return Game::listAllTasks();
	});
	lua->set_function("getTaskType", [](sol::optional<CTask *> task) -> std::string {
		if(!Config::devConsoleEnabled)
			return "getTaskType: EnableDevConsole=1 required";
		if(!task || !*task)
			return "null";
		return Game::getTaskTypeName((*task)->classtype);
	});
	return 0;
}

int Game::onTeamVt0(CTaskTeam *This, int a2) {
	(void)a2;
	if(This)
		liveTeams.insert(This);
	return 0;
}

int Game::onTeamVt4(CTaskTeam *This, int heap) {
	(void)heap;
	if(This)
		liveTeams.erase(This);
	return 0;
}

void Game::clearLiveTeams() {
	liveTeams.clear();
}

bool Game::isHost() {
	return Lobby::isHost();
}

bool Game::isReplayPlayback() {
	if(!addrGameinfoObject)
		return false;
	char * inputReplayPath = (char*)(addrGameinfoObject + 0xDB60);
	return inputReplayPath && strlen(inputReplayPath) > 0;
}

void Game::invalidateStaleGamePointers() {
	addrDDGame = addrDDDisplay = addrDSSound = addrDDKeyboard = addrDDMouse = addrWavCDRom = addrWSGameNet = 0;
}

void Game::probeAndInvalidateStalePointers() {
	if(!addrDDGame)
		return;
	if(!jellyProbeRead(addrDDGame, 0x48C))
		invalidateStaleGamePointers();
}

bool Game::isValidDdGame() {
	if(!addrDDGame)
		return false;
	if(!jellyProbeRead(addrDDGame, 0x48C)) {
		invalidateStaleGamePointers();
		return false;
	}
	return true;
}

bool Game::isValidGameGlobal() {
	if(!isValidDdGame())
		return false;
	DWORD gameGlobal = *(DWORD*)(addrDDGame + 0x488);
	if(!gameGlobal)
		return false;
	if(!jellyProbeRead(gameGlobal, 0x528))
		return false;
	DWORD gamescene = *(DWORD*)(gameGlobal + 0x524);
	if(!gamescene)
		return false;
	if(!jellyProbeRead(gamescene, sizeof(DWORD)))
		return false;
	return true;
}

bool Game::isMatchRunning() {
	return isValidDdGame();
}

std::vector<CTaskTeam*> Game::getTeams() {
	return std::vector<CTaskTeam*>(liveTeams.begin(), liveTeams.end());
}

DWORD Game::getAddrTurnGameObject() {
	if(!isValidDdGame())
		return 0;
	if(!jellyProbeRead(addrDDGame + 0x8, sizeof(DWORD)))
		return 0;
	DWORD turnGame = *(DWORD*)(addrDDGame + 0x8);
	if(turnGame && !jellyProbeRead(turnGame, sizeof(DWORD)))
		return 0;
	return turnGame;
}

DWORD Game::getAddrGameGlobal() {
	if(!isValidGameGlobal())
		return 0;
	return *(DWORD*)(addrDDGame + 0x488);
}

int *Game::getAddrGameScene() {
	if(!isValidGameGlobal())
		return nullptr;
	DWORD gameGlobal = *(DWORD*)(addrDDGame + 0x488);
	return *(int **)(gameGlobal + 0x524);
}

bool Game::hasValidFrontendDrawContext() {
	if(!addrDDDisplay)
		return false;
	return isValidGameGlobal();
}

void Game::printDebugAddrs() {
}

DWORD Game::getAddrDdDisplay() {
	return addrDDDisplay;
}

DWORD Game::getAddrDsSound() {
	return addrDSSound;
}

DWORD Game::getAddrDdKeyboard() {
	return addrDDKeyboard;
}

DWORD Game::getAddrDdMouse() {
	return addrDDMouse;
}

DWORD Game::getAddrWavCdRom() {
	return addrWavCDRom;
}

DWORD Game::getAddrWsGameNet() {
	return addrWSGameNet;
}

DWORD Game::getAddrPCLandscape() {
	if(!isValidDdGame())
		return 0;
	if(!jellyProbeRead(addrDDGame + 0x4CC, sizeof(DWORD)))
		return 0;
	DWORD landscape = *(DWORD*)(addrDDGame + 0x4CC);
	if(landscape && !jellyProbeRead(landscape, sizeof(DWORD)))
		return 0;
	return landscape;
}
