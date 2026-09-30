#ifndef WKJELLYWORM_CUSTOMWEAPONS_H
#define WKJELLYWORM_CUSTOMWEAPONS_H

#include <sigscanner.h>
#include <array>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <sol/forward.hpp>
#include "entities/tasks/CTaskTeam.h"
#include "Constants.h"
#include "entities/Entities.h"
#include "PxWeaponArt.h"

class CustomWeapons {
public:
	inline static int customWeaponsEnabled = 0;
	static const int maxCustomWeapons = 256;
	static const int maxStandardWeapons = 71;
	static inline int numAllWeapons = maxStandardWeapons;
	static const int standardNumColumns = 5;
	static const int customNumColumns = 40;
	static const int numTeams = 6;

	struct WeaponStruct {
		char * name1; // 0x0
		char * name2; // 0x4
		int panelRow; // 0x8
		int unknownC; // 0xC
		int unknown10; // 0x10
		int unknown14; // 0x14
		int unknown18; // 0x18
		int unknown1C; // 0x1C
		int unknown20; // 0x20
		int unknown24; // 0x24
		int unknown28; // 0x28
		int unknown2C; // 0x2C
		int unknown30; // 0x30
		int unknown34; // 0x34
		int unknown38; // 0x38
		int unknown3C; // 0x3C
		int unknown40; // 0x40
		int unknown44; // 0x44
		int unknown48; // 0x48
		int unknown4C; // 0x4C
		int unknown50; // 0x50
		int unknown54; // 0x54
		int unknown58; // 0x58
		int unknown5C; // 0x5C
		int unknown60; // 0x60
		int unknown64; // 0x64
		int unknown68; // 0x68
		int unknown6C; // 0x6C
		int unknown70; // 0x70
		int unknown74; // 0x74
		int unknown78; // 0x78
		int unknown7C; // 0x7C
		int unknown80; // 0x80
		int unknown84; // 0x84
		int unknown88; // 0x88
		int unknown8C; // 0x8C
		int unknown90; // 0x90
		int unknown94; // 0x94
		int unknown98; // 0x98
		int unknown9C; // 0x9C
		int unknownA0; // 0xA0
		int unknownA4; // 0xA4
		int unknownA8; // 0xA8
		int unknownAC; // 0xAC
		int unknownB0; // 0xB0
		int unknownB4; // 0xB4
		int unknownB8; // 0xB8
		int unknownBC; // 0xBC
		int unknownC0; // 0xC0
		int unknownC4; // 0xC4
		int unknownC8; // 0xC8
		int unknownCC; // 0xCC
		int unknownD0; // 0xD0
		int unknownD4; // 0xD4
		int unknownD8; // 0xD8
		int unknownDC; // 0xDC
		int unknownE0; // 0xE0
		int unknownE4; // 0xE4
		int unknownE8; // 0xE8
		int unknownEC; // 0xEC
		int unknownF0; // 0xF0
		int unknownF4; // 0xF4
		int unknownF8; // 0xF8
		int unknownFC; // 0xFC
		int unknown100; // 0x100
		int unknown104; // 0x104
		int unknown108; // 0x108
		int unknown10C; // 0x10C
		int unknown110; // 0x110
		int unknown114; // 0x114
		int unknown118; // 0x118
		int unknown11C; // 0x11C
		int unknown120; // 0x120
		int unknown124; // 0x124
		int unknown128; // 0x128
		int unknown12C; // 0x12C
		int unknown130; // 0x130
		int unknown134; // 0x134
		int unknown138; // 0x138
		int unknown13C; // 0x13C
		int unknown140; // 0x140
		int unknown144; // 0x144
		int unknown148; // 0x148
		int unknown14C; // 0x14C
		int unknown150; // 0x150
		int unknown154; // 0x154
		int unknown158; // 0x158
		int unknown15C; // 0x15C
		int unknown160; // 0x160
		int unknown164; // 0x164
		int unknown168; // 0x168
		int unknown16C; // 0x16C
		int unknown170; // 0x170
		int unknown174; // 0x174
		int unknown178; // 0x178
		int unknown17C; // 0x17C
		int unknown180; // 0x180
		int unknown184; // 0x184
		int unknown188; // 0x188
		int unknown18C; // 0x18C
		int unknown190; // 0x190
		int unknown194; // 0x194
		int unknown198; // 0x198
		int unknown19C; // 0x19C
		int unknown1A0; // 0x1A0
		int unknown1A4; // 0x1A4
		int unknown1A8; // 0x1A8
		int unknown1AC; // 0x1AC
		int unknown1B0; // 0x1B0
		int unknown1B4; // 0x1B4
		int unknown1B8; // 0x1B8
		int unknown1BC; // 0x1BC
		int unknown1C0; // 0x1C0
		int unknown1C4; // 0x1C4
		int unknown1C8; // 0x1C8
		int unknown1CC; // 0x1CC
	};

private:
	inline static int currentWeaponTable;

	static int __stdcall hookInitializeWeaponTable(DWORD addrDDGame);
	static int __stdcall hookWeaponPanelLoop(int team, int counter, int row, int weapon_panel_obj);
	static int __stdcall hookGetAmmo();

	inline static std::array<WeaponStruct, maxCustomWeapons> weaponTable;
	// Snapshot of the vanilla entries, taken once after WA has filled them.
	inline static std::array<WeaponStruct, maxStandardWeapons> vanillaWeapons;
	inline static bool vanillaWeaponsCaptured = false;
	inline static std::array<DWORD, maxCustomWeapons> weaponPanelSlots;
	inline static std::array<std::array<int, maxCustomWeapons>, numTeams> ammoTable;
	inline static std::array<std::array<int, maxCustomWeapons>, numTeams> delayTable;
	using ShotReloadKey = std::pair<DWORD, int>; // (gameCtx, teamSlot)
	inline static std::map<ShotReloadKey, std::array<int, maxCustomWeapons>> lastShotReloadDelay;

	struct PendingShotReloadCapture {
		DWORD gameCtx;
		int team_slot;
		int weapon_id;
		DWORD delay_addr;
		int peak_delay;
		int retries;
	};
	inline static std::vector<PendingShotReloadCapture> pendingShotReloadCaptures;

	inline static std::vector<WeaponStruct> weaponStructInjectionList;
	inline static std::vector<std::pair<std::string, DWORD>> weaponImgInjectionList;
	inline static std::vector<std::pair<std::string, std::string>> weaponNameStorage;
	// A package's "initialize" Lua entrypoint can legitimately run more than
	// once per match. Without this, every rerun would append a fresh
	// duplicate weapon and hand out a NEW id, leaving whichever id the
	// package's Lua actually captured (e.g. for firing/collision logic)
	// pointing at a stale or never-populated slot. Keyed by name1+"\x01"+name2.
	inline static std::unordered_map<std::string, int> customWeaponIdByKey;
	// Custom worm aiming sprite per weapon id, from the ProjectX .bin trailer.
	inline static std::unordered_map<int, int> customAimSprites;
	// ProjectX art found at registration, turned into sprite ids by assignPxArt.
	inline static std::map<int, PxWeaponArt::Art> pxArt;

	inline static int variablePanelColumns = standardNumColumns;
	inline static int variablePanelWidthAdd = 144;

	static int __stdcall hookWeaponPanelDrawTile_GetSlotAddr(int weapon_panel_obj, int column, int row);
	static void __stdcall hookWeaponPanelCheckClick_patch1_c(int v5, int a1, int weapon_panel_obj, int a3, int column, int row);

//	static DWORD __stdcall hookLoadImgFromVfs(int x, int y, int a4);
//	static int __fastcall hookLoadWeaponPanelImgs(int This, int EDX, int x, int y);

	static DWORD __stdcall getImgPtr(int weapon_panel_obj, int weapon_id, int a4);
	static void hookWeaponPanelDrawTile_GetImgPtr_patch1();
	static void hookWeaponPanelDrawTile_GetImgPtr_patch2();
	static void hookWormSwitchingWeapon_GetImgPtr_patch1();
	static void hookWeaponPanel_incrementCounter_patch1();
	static void hookWeaponPanel_incrementCounter_patch2();
	static void hookWeaponPanelUnknown1_patch1();
	static void hookWeaponPanelDescription_getAmmoDelay_patch1();

//	static DWORD __stdcall getAmmoAddr_v1(int team_id, int weapon_id, int teaminfo_obj, int tabletype);
	static DWORD __stdcall getAmmoAddr_v2(int weapon_panel_obj, int weapon_id, int tabletypw);

	static DWORD __stdcall hookAddAmmo(DWORD team_info_obj, int weapon_id);
	static DWORD *__stdcall hookSubtractAmmo(int weapon_id);
	static DWORD *__fastcall hookSubtractAmmo_v2(int a1, CTaskTeam *a2);
	static int hookGetAmmo_wrapped(int team_id, int weapon_id, DWORD teaminfo_obj);
	static DWORD __stdcall getWeaponDelayAddr(DWORD weapon_panel_obj, int weapon_id);
	static int __stdcall hookReduceDelayOnTurnStart();

	static void test(int team, int alliance, int weapon);
	static void test2(int team, int weapon);
	static int getTeamWeaponAmmo(CTaskTeam *team, int weapon_id);
	static int getTeamWeaponDelay(CTaskTeam *team, int weapon_id);
	static int getTeamWeaponLastShotDelay(CTaskTeam *team, int weapon_id);
	static void recordShotReloadDelay(CTaskTeam *team, int weapon_id);
	static void recordShotReloadDelayFromContext(DWORD gameCtx, int teamSlot, int weapon_id);
	static void pollPendingShotReloadCaptures();
	static void clearShotReloadDelays();
	static ShotReloadKey makeShotReloadKey(CTaskTeam *team);
	static ShotReloadKey makeShotReloadKeyFromContext(DWORD gameCtx, int teamSlot);
	static DWORD getDelayAddrFromContext(DWORD gameCtx, int teamSlot, int weapon_id);
	static int readBestShotDelayFromContext(DWORD gameCtx, int teamSlot, int weapon_id, DWORD fixedDelayAddr);
	static int readBestShotDelay(CTaskTeam *team, int weapon_id, DWORD fixedDelayAddr);
	static void storeShotReloadDelay(ShotReloadKey key, int weapon_id, int delay);
	static void setTeamWeaponAmmo(CTaskTeam *team, int weapon_id, int value);
	static void setTeamWeaponDelay(CTaskTeam *team, int weapon_id, int value);
	static void adjustTeamDelays(CTaskTeam *team, int amount);

	static DWORD getGameGlobalFromTeam(CTaskTeam *team);
	static DWORD getWeaponPanelFromTeam(CTaskTeam *team);
	static int getAllianceIdFromTeam(CTaskTeam *team);
	static int getPanelTeamSlotFromTeam(CTaskTeam *team);
	static int getTeamSlotFromTeam(CTaskTeam *team);
	static DWORD getTeamWeaponFieldAddr(CTaskTeam *team, int weapon_id, int tabletype);
	static DWORD getPanelWeaponFieldAddr(CTaskTeam *team, int weapon_id, int tabletype);
	static void writeStandardWeaponField(CTaskTeam *team, int weapon_id, int tabletype, int value);
	static void markWeaponPanelDirty(CTaskTeam *team);

	// --- diagnostics / raw-memory helpers (для /delay probe и обхода гонки "slot успел смениться") ---
	inline static int dbgSubtractAmmoV1Calls = 0;
	inline static int dbgSubtractAmmoV1StandardCalls = 0;
	inline static int dbgSubtractAmmoV2Calls = 0;
	inline static int dbgSubtractAmmoV2StandardCalls = 0;
	inline static int dbgRecordShotCalls = 0;
	inline static int dbgRecordShotStoredCalls = 0;
	inline static int dbgLastWeaponShot = -1;
	inline static int dbgLastCaptureDelay = 0;
	inline static int dbgLastRecordedDelay = 0;
	inline static int dbgPendingCaptureCount = 0;

	static int __stdcall readMemInt(DWORD addr);
	static void __stdcall writeMemInt(DWORD addr, int value);
	static std::string getDebugCounters();

public:
//	static int onConstructGlobalContext(int a1);
	static int install(SignatureScanner &, module);
//	static int onSendGamePacketWrapped(int a1, int x, Constants::TaskMessage y, void *data);
	static void printDebugAddrs();
//	static void switchWeaponTable();
	static void onDestroyGlobalContext();

	static void onConstructGlobalContext(int a1);

	static int registerCustomWeapon(WeaponStruct weaponStruct, std::string img, std::string name1, std::string name2);
	// Lua-facing wrapper: type-checks before touching the struct.
	static int registerCustomWeaponLua(sol::object weaponObj, std::string img, std::string name1, std::string name2);
	static void resetConfig();
	// Worm aim sprite for a custom weapon: >0 sprite id, 0 = not a custom
	// weapon, -1 = custom weapon without its own aim art.
	static int __stdcall aimSpriteFor(WeaponStruct *ws);
	// Call after every package initialize() ran, before sprites are loaded.
	static void assignPxArt();

	// Vanilla weapon templates.
	//
	// hookInitializeWeaponTable points the game's weapon table at our own
	// weaponTable BEFORE calling the original, so WA itself fills entries
	// 0..maxStandardWeapons-1 with genuine vanilla WeaponStructs. We snapshot
	// those once, which gives packages a correct WA-semantics base to build a
	// custom weapon on.
	//
	// This exists because px2jelly ported ProjectX's own weapon-struct bytes
	// into WA's struct layout. The two layouts are unrelated and all but three
	// fields here are still "unknownXX", so those bytes decode to whatever WA
	// happens to read at that offset -- which is why ported weapons fire as
	// sheep or dynamite. Basing a custom weapon on a vanilla template and
	// adding behaviour from Lua is the only approach that can actually work.
	static WeaponStruct cloneVanillaWeapon(int id);
	static bool vanillaWeaponsReady();
	static int dumpVanillaWeapons(std::string path);
};

#endif //WKJELLYWORM_CUSTOMWEAPONS_H

