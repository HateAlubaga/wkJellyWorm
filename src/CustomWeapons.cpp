#include <MinHook.h>
#include "CustomWeapons.h"
#include "Game.h"
#include "Chat.h"
#include "Hooks.h"
#include "Lua.h"
// NB: do not include <fstream>/<cstddef>/Config.h here. They pull in the C++
// alternative-operator tokens, which turn `xor` into an operator and break the
// existing `_asm xor edx,edx` blocks further down this file (error C2400).
// Vanilla-template dumping below therefore uses stdio and a literal path.
#include "MemAccess.h"
#include "renderer/Bitmap.h"
#include "Utils.h"
#include "PxWeaponArt.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include "sol/sol.hpp"

namespace fs = std::filesystem;
namespace JellyConfig {
	bool customWeaponIconsEnabled();
}

// offsets refer to WA v.3.8.0

static bool fileLooksReadable(const std::string &path) {
	std::error_code ec;
	return fs::is_regular_file(path, ec) && !ec;
}

static bool bitmapImageLooksUsable(DWORD rawBitmap) {
	if(!rawBitmap || !jellyProbeRead(rawBitmap, sizeof(Bitmap::BitmapImage)))
		return false;

	auto *bitmap = (Bitmap::BitmapImage *)rawBitmap;
	DWORD vtable = *(DWORD *)rawBitmap;
	DWORD expectedBase = Bitmap::getAddrBitmapImageVtable();
	DWORD expected8 = Bitmap::getAddrBitmapImage8Vtable();
	DWORD expected1 = Bitmap::getAddrBitmapImage1Vtable();
	if(expectedBase && vtable != expectedBase && vtable != expected8 && vtable != expected1)
		return false;

	int width = bitmap->max_width_dword14;
	int height = bitmap->max_height_dword18;
	int stride = bitmap->rowsize_dword10;
	if(bitmap->bitdepth_dwordC != 8 || width <= 0 || height <= 0
		|| width > 4096 || height > 4096 || stride < width || stride > width * 16)
		return false;
	if(!bitmap->data_dword8)
		return false;
	size_t dataSize = (size_t)stride * (size_t)height;
	return dataSize <= 64 * 1024 * 1024
		&& jellyProbeRead((DWORD)bitmap->data_dword8, dataSize);
}

// Loading a weapon icon (whether a real package .png or our own placeholder)
// was previously confirmed to crash somewhere in WA's own bitmap
// prepare/remap machinery, hard enough that it took the whole process down --
// that is why icon loading stayed disabled. A raw hardware exception like
// that is not something a C++ try/catch can stop; SEH can. No object with a
// destructor may live in this function (MSVC forbids mixing __try with C++
// unwinding), which is also why this is split out rather than inlined into
// the loop below.
static DWORD safeLoadWeaponIcon(const char *path) {
	__try {
		return (DWORD) Bitmap::callLoadImgFromFile(0, 0, (char *)path);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		printf("CustomWeapons: loading icon '%s' raised exception 0x%X; using vanilla fallback\n",
			path, GetExceptionCode());
		return 0;
	}
}

static std::string resolveWeaponIconPath(const std::string &img) {
	if(img.empty())
		return img;

	// Packages pass a bare filename ("buffalo.png") that lives in their own
	// package directory (assets/), not WA's working directory -- that bare
	// name almost never resolved before, which is why nearly every custom
	// weapon fell back to the vanilla bazooka icon regardless of whether the
	// art actually existed.
	std::string resolved = Utils::resolveAssetPath(img);
	if(img.find("jelly/placeholders/weapon0.img") != std::string::npos) {
		std::string discovered = Utils::findUniquePackageIcon();
		if(!discovered.empty()) {
			printf("registerCustomWeapon: replacing placeholder icon '%s' with '%s'\n",
				img.c_str(), discovered.c_str());
			resolved = discovered;
		}
	}
	if(fileLooksReadable(resolved)) {
		if(resolved != img)
			printf("registerCustomWeapon: resolved icon '%s' -> '%s'\n", img.c_str(), resolved.c_str());
		return resolved;
	}
	return img;
}

static std::string lowerAscii(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// px2jelly registered most weapons with the placeholder icon, and in
// multi-weapon packages often reused the first weapon's icon for all of them
// (every Ascender variant showed ico_ascender). The matched .bin trailer names
// the icon ProjectX actually used, so prefer it when the registered icon is a
// placeholder, missing, or a different picture for what is plainly the same
// weapon. A registered icon that is the same picture in another format
// (ico_magnet.img vs ico_magnet.png) is kept.
static std::string pickWeaponIcon(const std::string &img, const std::string &name1, const PxWeaponArt::Art *art) {
	bool placeholder = img.empty() || img.find("jelly/placeholders/weapon0.img") != std::string::npos;
	if(art && !art->iconFile.empty() && fileLooksReadable(art->iconFile)) {
		std::string registered = placeholder ? std::string() : resolveWeaponIconPath(img);
		bool usable = !registered.empty() && fileLooksReadable(registered);
		bool sameWeapon = !art->trailerName.empty() && lowerAscii(name1).find(lowerAscii(art->trailerName)) == 0;
		bool samePicture = usable && lowerAscii(fs::path(registered).stem().string())
			== lowerAscii(fs::path(art->iconFile).stem().string());
		if(!usable || (sameWeapon && !samePicture)) {
			PxWeaponArt::log("%s: icon '%s' -> ProjectX icon '%s'\n", name1.c_str(), img.c_str(), art->iconFile.c_str());
			return art->iconFile;
		}
		return registered;
	}
	return resolveWeaponIconPath(img);
}

void CustomWeapons::onConstructGlobalContext(int a1) {
	customWeaponsEnabled = weaponStructInjectionList.size() > 0;
	numAllWeapons = maxStandardWeapons + weaponStructInjectionList.size();
        if(numAllWeapons > maxCustomWeapons)
                numAllWeapons = maxCustomWeapons;

	if(Game::isValidDdGame()) {
		DWORD origLoadImgFromVfs_sesi = *(DWORD*)(Game::getAddrDDGame() + 0x4C0);
		(void)origLoadImgFromVfs_sesi;
	}

	for(size_t i = 0; i < weaponImgInjectionList.size(); i++) {
		auto & inject = weaponImgInjectionList[i];
		if(!JellyConfig::customWeaponIconsEnabled()) {
			// Do not call LoadImgFromFile at all. Even a valid placeholder
			// creates a bitmap which WA later prepares/remaps at the crash site.
			printf("CustomWeapons: icon loading disabled; '%s' uses vanilla weapon 1\n",
				inject.first.c_str());
			inject.second = 0;
			continue;
		}
		// inject.first already holds the resolved path from registerCustomWeapon
		// (resolveWeaponIconPath) -- a real package icon when one exists, else
		// the "jelly/placeholders/weapon0.img" stub. Earlier this always
		// substituted a hardcoded placeholder here regardless of what the
		// package actually registered, which is why no custom icon was ever
		// shown even before the crash.
		printf("CustomWeapons: loading icon '%s'\n", inject.first.c_str());
		inject.second = safeLoadWeaponIcon(inject.first.c_str());
		if(!bitmapImageLooksUsable(inject.second)) {
			printf("CustomWeapons: icon '%s' was rejected (%X); using vanilla fallback\n",
				inject.first.c_str(), inject.second);
			inject.second = 0;
		}
	}

//	DWORD addrGameGlobal = Game::getAddrGameGlobal();
//	*(Bitmap::BitmapImage**)(addrGameGlobal +0x538) = Bitmap::callLoadImgFromFile(0, 0, "jelly/fire-red.img");
//	*(Bitmap::BitmapImage**)(addrGameGlobal +0x53C) = Bitmap::callLoadImgFromFile(0, 0, "jelly/fire-blu.img");


	if(customWeaponsEnabled) {
		for(int team=0; team < numTeams; team++) {
			for(int weapon=0; weapon < numAllWeapons; weapon++) {
				ammoTable[team][weapon] = 10;
				delayTable[team][weapon] = 0;
			}
		}
		size_t i = 0;
		for(auto & it : weaponStructInjectionList) {
			auto wpnid = maxStandardWeapons + i;
			it.name1 = (char*)weaponNameStorage[i].first.c_str();
			it.name2 = (char*)weaponNameStorage[i].second.c_str();
			printf("CustomWeapons::onConstructGlobalContext: inject weapon: %s = %d\n", it.name1, i);
			memcpy((void*)&weaponTable[wpnid], (void*)&it, sizeof(WeaponStruct));
			i++;
		}
	}

}

DWORD originalWeaponTablePtr = 0;
int (__stdcall *origInitializeWeaponTable)(DWORD addrDDGame);
int __stdcall CustomWeapons::hookInitializeWeaponTable(DWORD addrDDGame) {
	printf("initialize weapon table\n");
	if(!addrDDGame || !jellyProbeRead(addrDDGame, 0x54C))
		return origInitializeWeaponTable(addrDDGame);
	DWORD addrGameGlobal = *(DWORD*)(addrDDGame + 0x488);
	if(!addrGameGlobal || !jellyProbeRead(addrGameGlobal, 0x550))
		return origInitializeWeaponTable(addrDDGame);
	DWORD addrWeaponPanel = *(DWORD*)(addrGameGlobal + 0x548);

	originalWeaponTablePtr = *(DWORD*)(addrGameGlobal + 0x510);
 	*(DWORD*)(addrGameGlobal + 0x510) = (DWORD)&weaponTable;

	variablePanelColumns = standardNumColumns;
	variablePanelWidthAdd = 144;
	int ret = origInitializeWeaponTable(addrDDGame);

	// The original call has just filled entries 0..maxStandardWeapons-1 of our
	// table with the real vanilla weapons. Snapshot them once so Lua packages
	// can build custom weapons on a correct WA template instead of on ported
	// ProjectX struct bytes. Custom weapons live at >= maxStandardWeapons and
	// are injected separately, so this range stays vanilla.
	if(!vanillaWeaponsCaptured) {
		bool looksFilled = false;
		for(int i = 0; i < maxStandardWeapons; i++) {
			if(weaponTable[i].name1) {
				looksFilled = true;
				break;
			}
		}
		if(looksFilled) {
			for(int i = 0; i < maxStandardWeapons; i++)
				vanillaWeapons[i] = weaponTable[i];
			vanillaWeaponsCaptured = true;
			printf("CustomWeapons: captured %d vanilla weapon templates\n", maxStandardWeapons);
			// relative to the WA working directory, same as jelly/pxcompat.log
			dumpVanillaWeapons("jelly\\vanilla_weapons.json");
		} else {
			printf("CustomWeapons: vanilla weapon table looked empty; templates not captured\n");
		}
	}
	return ret;
}

bool CustomWeapons::vanillaWeaponsReady() {
	return vanillaWeaponsCaptured;
}

// Returns a copy of vanilla weapon `id`'s struct. Lua receives it as a
// WeaponStruct userdata it can then override (name/panelRow/etc) and hand to
// registerCustomWeapon. Out-of-range or not-yet-captured returns a zeroed
// struct, which registers as an inert weapon rather than crashing.
CustomWeapons::WeaponStruct CustomWeapons::cloneVanillaWeapon(int id) {
	if(!vanillaWeaponsCaptured || id < 0 || id >= maxStandardWeapons) {
		printf("cloneVanillaWeapon(%d): unavailable (captured=%d)\n", id, (int)vanillaWeaponsCaptured);
		return WeaponStruct{};
	}
	return vanillaWeapons[id];
}

// Writes every vanilla template as raw field values, so the porting tools can
// diff them offline and pick a sensible base weapon per package.
int CustomWeapons::dumpVanillaWeapons(std::string path) {
	if(!vanillaWeaponsCaptured)
		return 0;
	FILE *out = nullptr;
	if(fopen_s(&out, path.c_str(), "w") != 0 || !out)
		return 0;
	// panelRow onwards is a flat run of ints; the two leading name pointers are
	// emitted separately as strings.
	const int fieldCount = (int)((sizeof(WeaponStruct) - 2 * sizeof(char *)) / sizeof(int));
	fprintf(out, "{\n  \"fieldCount\": %d,\n  \"weapons\": [\n", fieldCount);
	for(int i = 0; i < maxStandardWeapons; i++) {
		const WeaponStruct &w = vanillaWeapons[i];
		fprintf(out, "    {\"id\": %d, \"name1\": \"%s\", \"name2\": \"%s\", \"fields\": [",
			i, w.name1 ? w.name1 : "", w.name2 ? w.name2 : "");
		const int *fields = (const int *)&w.panelRow;
		for(int f = 0; f < fieldCount; f++)
			fprintf(out, f ? ", %d" : "%d", fields[f]);
		fprintf(out, "]}%s\n", i == maxStandardWeapons - 1 ? "" : ",");
	}
	fprintf(out, "  ]\n}\n");
	fclose(out);
	printf("CustomWeapons: wrote vanilla weapon templates to %s\n", path.c_str());
	return maxStandardWeapons;
}

void CustomWeapons::onDestroyGlobalContext() {
	clearShotReloadDelays();
	if(originalWeaponTablePtr && Game::isValidGameGlobal()) {
		DWORD addrGameGlobal = Game::getAddrGameGlobal();
		*(DWORD *) (addrGameGlobal + 0x510) = originalWeaponTablePtr;
		originalWeaponTablePtr = 0;
	}
}

void CustomWeapons::clearShotReloadDelays() {
	lastShotReloadDelay.clear();
	pendingShotReloadCaptures.clear();
	dbgPendingCaptureCount = 0;
}

int CustomWeapons::getTeamWeaponLastShotDelay(CTaskTeam *team, int weapon_id) {
	if(!team || weapon_id < 0 || weapon_id >= maxCustomWeapons)
		return 0;
	int teamSlot = getTeamSlotFromTeam(team);
	int best = 0;
	auto tryKey = [&](DWORD ctx) {
		if(!ctx)
			return;
		auto it = lastShotReloadDelay.find({ctx, teamSlot});
		if(it == lastShotReloadDelay.end())
			return;
		int d = it->second[weapon_id];
		if(d > best)
			best = d;
	};
	tryKey(getGameGlobalFromTeam(team));
	tryKey(Game::getAddrGameGlobal());
	// v1 subtractAmmo passes DDGame+0x488 ctx; Lua reads team->unknown2C — match by team slot.
	for(auto &entry : lastShotReloadDelay) {
		if(entry.first.second != teamSlot)
			continue;
		int d = entry.second[weapon_id];
		if(d > best)
			best = d;
	}
	return best;
}

CustomWeapons::ShotReloadKey CustomWeapons::makeShotReloadKey(CTaskTeam *team) {
	return makeShotReloadKeyFromContext(getGameGlobalFromTeam(team), getTeamSlotFromTeam(team));
}

CustomWeapons::ShotReloadKey CustomWeapons::makeShotReloadKeyFromContext(DWORD gameCtx, int teamSlot) {
	return {gameCtx, teamSlot};
}


DWORD addrPrepareWeaponPanelHookLoopEnd;
int __stdcall CustomWeapons::hookWeaponPanelLoop(int team, int counter, int row, int weapon_panel_obj) {
	int minWeaponId = 1;
	int maxWeaponId = numAllWeapons;
			//customWeaponsEnabled ? maxCustomWeapons : maxStandardWeapons;
	int weapon_added_count = 0;
	// The draw hooks index weaponPanelSlots by row * variablePanelColumns.
	// The legacy game counter is a vanilla-panel cursor and is not a safe
	// storage offset after the custom column width changes.
	(void)team;
	(void)counter;
	if(!weapon_panel_obj || row < 0 || row >= 13)
		return 0;
	int addrGameGlobal = *(DWORD*)weapon_panel_obj;
	// calculate variable column count on first iteration, so it stays consistent throughout later iterations
	if(row == 0) {
		variablePanelColumns = standardNumColumns;
		variablePanelWidthAdd = 144;
		for(int lrow = 0; lrow < 13; lrow++) {
			int count_found = 0;
			for(int weapon_id = minWeaponId; weapon_id < maxWeaponId; weapon_id++) {
				WeaponStruct * wpn = &weaponTable[weapon_id];
				if(wpn->panelRow == lrow) {
					int ammo = *(int*) getAmmoAddr_v2(weapon_panel_obj, weapon_id, 0);
					int delay = *(int*) getAmmoAddr_v2(weapon_panel_obj, weapon_id, 1);
					int unk = *(DWORD *)(*(DWORD *)(addrGameGlobal + 36) + 55160) < 406;
					if((ammo && delay >= -1) || unk) {
						count_found += 1;
						variablePanelColumns = max(variablePanelColumns, count_found);
						variablePanelWidthAdd = 29 * variablePanelColumns;
					}
				}
			}
		}
	}
	int row_base = row * variablePanelColumns;
	for(int slot_id = row_base; slot_id < row_base + variablePanelColumns
		&& slot_id < (int)weaponPanelSlots.size(); slot_id++) {
		weaponPanelSlots[slot_id] = 0;
	}

	for(int weapon_id = minWeaponId; weapon_id < maxWeaponId; weapon_id++) {
		WeaponStruct * wpn = &weaponTable[weapon_id];
		if(wpn->panelRow == row) {
			int ammo = *(int*) getAmmoAddr_v2(weapon_panel_obj, weapon_id, 0);
			int delay = *(int*) getAmmoAddr_v2(weapon_panel_obj, weapon_id, 1);
			int unk = *(DWORD *)(*(DWORD *)(addrGameGlobal + 36) + 55160) < 406;
			if((ammo && delay >= -1) || unk) {
				int slot = row_base + weapon_added_count;
				if(slot >= row_base + variablePanelColumns || slot >= (int)weaponPanelSlots.size())
					break;
				weaponPanelSlots[slot] = weapon_id;
				weapon_added_count += 1;
			}
		}
	}
	return weapon_added_count;
}


DWORD addrPrepareWeaponPanelHookLoopEndEarly;
void __declspec(naked) hookWeaponPanelLoop_wrapper() {
	//EBP is 0x0 right now
	_asm cmp [CustomWeapons::customWeaponsEnabled], 0
	_asm jnz callhook
	_asm xor edx,edx
	_asm LEA EAX,[EDX+1]
	_asm MOV EBX,0x1D0
	_asm CMP EDX,0x5
	_asm jmp addrPrepareWeaponPanelHookLoopEndEarly
//	00567EBD  |.  0F8D B9010000 JGE 0056807C

	_asm callhook:
	_asm push esi
	_asm mov eax, [esp+0x50 - 0x30] // row
	_asm push eax
	_asm mov eax, [esp+0x54 - 0x2c] // counter
	_asm push eax
	_asm mov eax, [esp+0x58 - 0x1c] // team
	_asm push eax
	_asm call CustomWeapons::hookWeaponPanelLoop
	_asm mov edx, eax // weapon added count
	_asm jmp addrPrepareWeaponPanelHookLoopEnd
}


int __stdcall CustomWeapons::hookWeaponPanelDrawTile_GetSlotAddr(int weapon_panel_obj, int column, int row) {
	if(customWeaponsEnabled)
		return (int)&weaponPanelSlots[row * variablePanelColumns + column];
	else {
		int offset = row + 6 + column + 4 * (row + 6);
		return weapon_panel_obj + 4 * offset;
	}
}

DWORD addrWeaponPanelDrawTile_GetSlot_patch1_ret;
void __declspec(naked) hookWeaponPanelDrawTile_GetSlot_patch1() {
	//00567826
//	_asm LEA EAX,[EBP+0x6]
//	_asm LEA EDX,[EAX*4+ECX]
//	_asm ADD EDX,EAX
//	_asm CMP DWORD PTR DS:[EDX*4+EDI],0
//	_asm LEA EAX,[EDX*4+EDI]

	_asm mov eax, [esp + 0x28 + 0x8] // row
	_asm push eax
	_asm mov eax, [esp + 0x2C + 0x4] // column
	_asm push eax
	_asm push edi // weapon panel
	_asm call CustomWeapons::hookWeaponPanelDrawTile_GetSlotAddr
	_asm cmp  dword ptr [eax], 0

	_asm jmp addrWeaponPanelDrawTile_GetSlot_patch1_ret
}


DWORD addrWeaponPanelDrawTile_GetSlot_patch3_ret;
void __declspec(naked) hookWeaponPanelDrawTile_GetSlot_patch3() {
	//0056774A
//	_asm LEA ECX,[EAX*4+ECX]
//	_asm ADD ECX,EAX
//	_asm MOV ESI,DWORD PTR DS:[ECX*4+EDI]
//	_asm TEST ESI,ESI

	_asm mov eax, [esp + 0x28 + 0x8] // row
	_asm push eax
	_asm mov eax, [esp + 0x2C + 0x4] // column
	_asm push eax
	_asm push edi // weapon panel
	_asm call CustomWeapons::hookWeaponPanelDrawTile_GetSlotAddr
	_asm mov esi,[eax]
	_asm test esi, esi

	_asm jmp addrWeaponPanelDrawTile_GetSlot_patch3_ret
}

DWORD addrWeaponPanelDescription_GetSlot_patch1_ret;
void __declspec(naked) hookWeaponPanelDescription_GetSlot_patch1() {
	//00567C11
//	_asm ADD EAX,6
//	_asm LEA ECX,[EAX*4+ECX]
//	_asm ADD ECX,EAX
//	_asm MOV EAX,DWORD PTR DS:[ECX*4+ESI]
//	_asm TEST EAX,EAX

	_asm mov eax, [esp + 0x1C + 0x8] // row
	_asm push eax
	_asm mov eax, [esp + 0x20 + 0x4] // column
	_asm push eax
	_asm push esi // weapon panel
	_asm call CustomWeapons::hookWeaponPanelDrawTile_GetSlotAddr
	_asm mov eax, [eax]
	_asm test eax, eax

	_asm jmp addrWeaponPanelDescription_GetSlot_patch1_ret
}


void __stdcall CustomWeapons::hookWeaponPanelCheckClick_patch1_c(int v5, int a1, int weapon_panel_obj, int a3, int column, int row) {
	int addr = hookWeaponPanelDrawTile_GetSlotAddr(weapon_panel_obj, column, row);
	*(DWORD*)a1 = column;
	*(DWORD*)a3 = *(DWORD*)addr;
	*(DWORD*)v5 = *(DWORD *)(weapon_panel_obj + 4 * row + 380);
}

DWORD addrWeaponPanelCheckClick_patch1_ret;
void __declspec(naked) hookWeaponPanelCheckClick_patch1() {
	//00568C98
//	_asm MOV EDX,DWORD PTR SS:[ESP+4]
//	_asm LEA EAX,[ECX+6]
//	_asm PUSH EBP
//	_asm LEA EBP,[EAX*4+EDX]
//	_asm ADD EBP,EAX
//	_asm MOV EAX,DWORD PTR DS:[EBP*4+ESI]
//	_asm MOV EBP,DWORD PTR SS:[ESP+0x10]
//	_asm MOV DWORD PTR SS:[EBP],EAX
//	_asm MOV ECX,DWORD PTR DS:[ECX*4+ESI+0x17C]
//	_asm MOV DWORD PTR DS:[EBX],ECX

	_asm mov eax, [esp + 0x8 + 0x8] // row
	_asm push eax
	_asm mov eax, [esp + 0xC - 0x4] // column
	_asm push eax
	_asm mov eax, [esp + 0x10 + 0x4] // y
	_asm push eax
	_asm push esi // weapon panel
	_asm push edi // a1
	_asm push ebx
	_asm call CustomWeapons::hookWeaponPanelCheckClick_patch1_c

	_asm jmp addrWeaponPanelCheckClick_patch1_ret
}

//void * (__cdecl *waNew)(size_t Size) = 0;
//FILE * (__cdecl *waFopen)(const char *Filename, const char *Mode) = 0;
//	waNew = (void *(__cdecl *)(size_t)) 0x005C034C;
//	waFopen = (FILE *(__cdecl *)(const char *, const char *)) 0x005D3082;

//bool grabLoadImgFromVfsParams = false;
//int (__stdcall *origVfsCreateGfxDirReader)(char *x) = 0;
//int __stdcall hookVfsCreateGfxDirReader(char *x) {
//	int sesi, retv;
//	_asm mov sesi, esi
//
//	if(!strcmp(x, "hi\\gravity.img")) {
//		grabLoadImgFromVfsParams = true;
//	}
//
//	_asm mov esi, sesi
//	_asm push x
//	_asm call origVfsCreateGfxDirReader
//	_asm mov retv, eax
//
//	return retv;
//}


DWORD getStandardImgPtr(int weapon_panel_obj, int weapon_id, int a4) {
	return *(DWORD*)(*(DWORD *)weapon_panel_obj + 4 * (a4 + 2 * weapon_id) + 324);
}

DWORD __stdcall CustomWeapons::getImgPtr(int weapon_panel_obj, int weapon_id, int a4) {
	if(weapon_id == 69 || weapon_id == 70) {
		return getStandardImgPtr(weapon_panel_obj, 1, a4);
	}
	if(weapon_id < maxStandardWeapons) {
		return getStandardImgPtr(weapon_panel_obj, weapon_id, a4);
	}
	int custom_weapon_id = weapon_id - maxStandardWeapons;
	if(custom_weapon_id >= weaponImgInjectionList.size()
	|| custom_weapon_id < 0
		|| !JellyConfig::customWeaponIconsEnabled()
		|| weaponImgInjectionList[custom_weapon_id].second == 0) {
		return getStandardImgPtr(weapon_panel_obj, 1, a4);
	}
	return weaponImgInjectionList[custom_weapon_id].second;
}

// Called from the naked hookWormSwitchingWeapon_GetImgPtr_patch1, which pushes
// three arguments and never pops them: must be __stdcall (callee cleans), or
// the worm frame function 0x515650 returns with esp 12 bytes off and WA later
// crashes writing team stats at 0x515724.
static DWORD __stdcall getWormSwitchingWeaponImgPtr(int weapon_panel_obj, int weapon_id, int a4) {
	if(weapon_id >= CustomWeapons::maxStandardWeapons)
		return getStandardImgPtr(weapon_panel_obj, 1, a4);
	return getStandardImgPtr(weapon_panel_obj, weapon_id, a4);
}

DWORD addrWeaponPanelDrawTile_GetImgPtr_patch1_ret;
void __declspec(naked) CustomWeapons::hookWeaponPanelDrawTile_GetImgPtr_patch1() {
	//00567A00
//	_asm mov     edx, [esp + 0x28 - 0x4]
//	_asm mov     ebx, [edx]
//	_asm mov     eax, [esp + 0x28 + 0x8]
//	_asm mov     esi, [edi]
//	_asm lea     ecx, [eax+ebx*2]
//	_asm xor     ebp, ebp
//	_asm cmp     [esi+ecx*4+0x144], ebp

	_asm mov     edx, [esp + 0x28 - 0x4] //weapon_id_ptr
	_asm mov     ebx, [edx] // ebx = weapon id,
	_asm mov     eax, [esp + 0x28 + 0x8] // a4

	_asm push ebx // save
	_asm push edi

	_asm push eax
	_asm push ebx
	_asm push edi
	_asm call getImgPtr
	_asm xor ebp, ebp
	_asm cmp [eax], ebp

	_asm pop edi
	_asm mov esi, [edi]
	_asm pop ebx

	_asm jmp addrWeaponPanelDrawTile_GetImgPtr_patch1_ret
}

DWORD addrWeaponPanelDrawTile_GetImgPtr_patch2_ret;
void __declspec(naked) CustomWeapons::hookWeaponPanelDrawTile_GetImgPtr_patch2() {
	//00567A38
	_asm push edx
	_asm push ebx
	_asm push edi
	_asm call getImgPtr
	_asm mov ecx, eax

	_asm MOV EDX,DWORD PTR SS:[ESP+0x20]
	_asm MOV EAX,DWORD PTR SS:[ESP+0x24]
	_asm MOV ESI,DWORD PTR DS:[ESI+0x138]
	_asm jmp addrWeaponPanelDrawTile_GetImgPtr_patch2_ret
}

DWORD addrWormSwitchingWeapon_GetImgPtr_patch1_ret;
void __declspec(naked) CustomWeapons::hookWormSwitchingWeapon_GetImgPtr_patch1() {
	//00515844
//	_asm mov     edx, [ebp+0x2C]
//	_asm mov     ecx, [edx+ecx*0x8+0x144]
//	_asm shl     eax, 0x10
//	_asm cmp     [ebp+0x374], ecx

	_asm push eax

	_asm lea edx, [ebp+0x2C]
	_asm push 0
	_asm push ecx
	_asm push edx
	_asm call getWormSwitchingWeaponImgPtr
	_asm mov ecx, eax

	_asm mov edx, [ebp+0x2C]
	_asm pop eax
	_asm shl     eax, 0x10
	_asm cmp     [ebp+0x374], ecx

	_asm jmp addrWormSwitchingWeapon_GetImgPtr_patch1_ret
}



DWORD addrWeaponPanel_incrementCounter_patch1_ret;
void __declspec(naked) CustomWeapons::hookWeaponPanel_incrementCounter_patch1() {
	// 005680A2
	_asm push eax
	_asm mov eax, variablePanelColumns
	_asm add     [esp + 0x50 - 0x2C], eax
	_asm pop eax
	_asm mov     [esp + 0x4C - 0x34], ebp
	_asm mov     [esp + 0x4C - 0x20], eax
	_asm jmp addrWeaponPanel_incrementCounter_patch1_ret
}

DWORD addrWeaponPanel_incrementCounter_patch2_ret;
void __declspec(naked) CustomWeapons::hookWeaponPanel_incrementCounter_patch2() {
	//005680D5

	_asm add eax, [variablePanelWidthAdd]
	_asm push eax

	_asm 	cmp     byte ptr [esi + 0x1C4], 0
//	_asm   mov     dword ptr [esi +0x1B0], 5
	_asm mov eax, variablePanelColumns
	_asm   mov     dword ptr [esi +0x1B0], eax
	_asm pop eax
	_asm   lea     edx, [ecx+ebp+0x6]
	_asm jmp addrWeaponPanel_incrementCounter_patch2_ret
}

#pragma optimize( "", off )
int (__stdcall *origGetAmmo)();
int __stdcall CustomWeapons::hookGetAmmo() {
	int team_a1, weapon_a2, team_info_obj, retv;
	_asm mov team_a1, eax
	_asm mov weapon_a2, edx
	_asm mov team_info_obj, esi

	if(weapon_a2 < maxStandardWeapons) {
		_asm mov eax, team_a1
		_asm mov edx, weapon_a2
		_asm mov esi, team_info_obj
		_asm call origGetAmmo
		_asm mov retv, eax
		return retv;
	} else {
		if(team_a1 < 0 || team_a1 > numTeams || weapon_a2 < 0 || weapon_a2 > maxCustomWeapons) return 0;
		int alliance_id = *(DWORD *)(1308 * team_a1 + team_info_obj + 4);
		//hex-rays intensifies
		if(delayTable[alliance_id][weapon_a2] != 0
			|| (*(DWORD *)(team_info_obj + 11276) && *(DWORD *)(team_info_obj + 11304) < 484 && *(DWORD *)(team_info_obj + 11304) < -2)
//			|| (LOBYTE(v5) = sub_522480(alliance_id, team_info_obj), v5))
			)
			return 0;
		else
			return ammoTable[alliance_id][weapon_a2];
	}
}

DWORD (__stdcall *origAddAmmo)(DWORD team_info_obj, int weapon_id);
DWORD __stdcall CustomWeapons::hookAddAmmo(DWORD team_info_obj, int weapon_id) {
	int seax, sedx, retv;
	_asm mov seax, eax //team
	_asm mov sedx, edx //amount
	if(weapon_id < maxStandardWeapons) {
		_asm mov eax, seax
		_asm mov edx, sedx
		_asm push weapon_id
		_asm push team_info_obj
		_asm call origAddAmmo
		_asm mov retv, eax
		return retv;
	}
	int alliance_id = *(DWORD *)(1308 * seax + team_info_obj + 4);
	auto & entry = ammoTable[alliance_id][weapon_id];
	if(entry >= 0) {
		if(sedx >= 0)
			entry += sedx;
		else
			entry = -1;
	}
	return (DWORD)&ammoTable[alliance_id][weapon_id];
}

//_DWORD *__userpurge subtract_ammo_sub_522560@<eax>(int a1@<eax>, int x@<ecx>, int y)
DWORD *(__stdcall * origSubtractAmmo)(int weapon_id);
DWORD *__stdcall CustomWeapons::hookSubtractAmmo(int weapon_id) {
	int seax, secx, retv;
	DWORD *ret;
	_asm mov seax, eax //team_id
	_asm mov secx, ecx //teaminfo obj (game global ctx)
	dbgSubtractAmmoV1Calls++;
	if(weapon_id < maxStandardWeapons) {
		dbgSubtractAmmoV1StandardCalls++;
		_asm mov eax, seax
		_asm mov ecx, secx
		_asm push weapon_id
		_asm call origSubtractAmmo
		_asm mov retv, eax
		ret = (DWORD*)retv;
	} else {
		int alliance_id = *(DWORD *)(1308 * seax + secx + 4);
		auto & entry = ammoTable[alliance_id][weapon_id];
		if(entry > 0)
			entry--;
		ret = (DWORD*)&ammoTable[alliance_id][weapon_id];
	}
	if(weapon_id >= 0 && weapon_id < maxCustomWeapons) {
		recordShotReloadDelayFromContext(secx, seax, weapon_id);
		DWORD altCtx = Game::getAddrGameGlobal();
		if(altCtx && altCtx != (DWORD)secx)
			recordShotReloadDelayFromContext(altCtx, seax, weapon_id);
	}
	return ret;
}
#pragma optimize( "", on )


//_DWORD *__fastcall subtract_ammo_missile_sub_5589A0(int a1, CTeam *x)
DWORD *(__fastcall *origSubtractAmmo_v2)(int a1, CTaskTeam *a2);
DWORD *__fastcall CustomWeapons::hookSubtractAmmo_v2(int a1, CTaskTeam *a2) {
	int weaponShot = a2 ? a2->lastLaunchedWeapon_dword60 : -1;
	DWORD *ret;
	dbgSubtractAmmoV2Calls++;
	dbgLastWeaponShot = weaponShot;
	if(a2 && weaponShot >= 0 && weaponShot < maxStandardWeapons)
		dbgSubtractAmmoV2StandardCalls++;
	if(a2 && weaponShot >= maxStandardWeapons) {
		int alliance_id = *(int*)(1308 * a2->team_number_dword38 + a2->unknown2C + 17964);
		int weapon = weaponShot;
		auto & entry = ammoTable[alliance_id][weapon];
		if(entry > 0)
			entry--;
		a2->unknown4C = 0;
		a2->lastLaunchedWeapon_dword60 = 0;
		DWORD v2 = a2->unknown2C;
		DWORD v3 = *(DWORD *)(v2 + 1352); //weapon panel
		if (v3)
			*(DWORD *)(v3 + 460) = 1; // redraw weapon panel
		ret = (DWORD*)&ammoTable[alliance_id][weapon];
	} else {
		ret = origSubtractAmmo_v2(a1, a2);
	}
	if(a2 && weaponShot >= 0 && weaponShot < maxCustomWeapons)
		recordShotReloadDelayFromContext(getGameGlobalFromTeam(a2), getTeamSlotFromTeam(a2), weaponShot);
	return ret;
}

//weapon panel variant
DWORD __stdcall CustomWeapons::getAmmoAddr_v2(int weapon_panel_obj, int weapon_id, int tabletype) {
	int team_id = *(DWORD *)(weapon_panel_obj + 4 * *(DWORD *)(weapon_panel_obj + 4) + 32);
	if(weapon_id < maxStandardWeapons) {
		int offset = tabletype ? 26104 : 25820;
		return (*(DWORD *)weapon_panel_obj + 4 * (142 * team_id + weapon_id) + offset);
	}
	if(!tabletype)
		return (DWORD)&ammoTable[team_id][weapon_id];
	else
		return (DWORD)&delayTable[team_id][weapon_id];
}

DWORD CustomWeapons::getGameGlobalFromTeam(CTaskTeam *team) {
	// В матче живой контекст — team->unknown2C (как hookReduceDelayOnTurnStart),
	// а не DDGame+0x488, который может не совпадать с panel/alliance таблицами.
	if(team && team->unknown2C)
		return team->unknown2C;
	return Game::getAddrGameGlobal();
}

DWORD CustomWeapons::getWeaponPanelFromTeam(CTaskTeam *team) {
	DWORD ctx = getGameGlobalFromTeam(team);
	if(!ctx)
		return 0;
	return *(DWORD *)(ctx + 1352);
}

int CustomWeapons::getAllianceIdFromTeam(CTaskTeam *team) {
	DWORD ctx = getGameGlobalFromTeam(team);
	if(!ctx || !team)
		return -1;
	int alliance = *(int *)(1308 * team->team_number_dword38 + ctx + 17964);
	if(alliance < 0 || alliance >= numTeams)
		return -1;
	return alliance;
}

static int getPanelActiveTeamSlot(DWORD panel) {
	if(!panel)
		return -1;
	return *(int *)(panel + 4 * *(DWORD *)(panel + 4) + 32);
}

static int getAllianceSlotFromContext(DWORD gameCtx, int teamSlot) {
	if(!gameCtx || teamSlot < 0 || teamSlot >= CustomWeapons::numTeams)
		return -1;
	int alliance = *(int *)(1308 * teamSlot + gameCtx + 17964);
	if(alliance < 0 || alliance >= CustomWeapons::numTeams)
		alliance = *(int *)(1308 * teamSlot + gameCtx + 4);
	if(alliance < 0 || alliance >= CustomWeapons::numTeams)
		return -1;
	return alliance;
}

int CustomWeapons::getPanelTeamSlotFromTeam(CTaskTeam *team) {
	DWORD panel = getWeaponPanelFromTeam(team);
	return getPanelActiveTeamSlot(panel);
}

int CustomWeapons::getTeamSlotFromTeam(CTaskTeam *team) {
	if(!team)
		return -1;
	int slot = team->team_number_dword38;
	if(slot < 0 || slot >= numTeams)
		return -1;
	return slot;
}

static DWORD getStandardWeaponFieldAddr(DWORD panel, int slot, int weapon_id, int tabletype) {
	if(!panel || slot < 0 || slot >= CustomWeapons::numTeams || weapon_id < 0)
		return 0;
	int offset = tabletype ? 26104 : 25820;
	return (*(DWORD *)panel + 4 * (142 * slot + weapon_id) + offset);
}

DWORD CustomWeapons::getDelayAddrFromContext(DWORD gameCtx, int teamSlot, int weapon_id) {
	if(!gameCtx || teamSlot < 0 || teamSlot >= numTeams || weapon_id < 0)
		return 0;
	if(weapon_id < maxStandardWeapons) {
		DWORD panel = *(DWORD *)(gameCtx + 1352);
		if(!panel)
			return 0;
		return getStandardWeaponFieldAddr(panel, teamSlot, weapon_id, 1);
	}
	int alliance = -1;
	alliance = *(int *)(1308 * teamSlot + gameCtx + 17964);
	if(alliance < 0 || alliance >= numTeams)
		alliance = *(int *)(1308 * teamSlot + gameCtx + 4);
	if(alliance < 0 || alliance >= numTeams)
		return 0;
	return (DWORD)&delayTable[alliance][weapon_id];
}

// Адрес, который панель читает при отрисовке (точная копия getAmmoAddr_v2 для стандартного оружия).
DWORD CustomWeapons::getPanelWeaponFieldAddr(CTaskTeam *team, int weapon_id, int tabletype) {
	if(!team || weapon_id < 0)
		return 0;
	DWORD panel = getWeaponPanelFromTeam(team);
	if(!panel)
		return 0;
	if(weapon_id < maxStandardWeapons)
		return getAmmoAddr_v2(panel, weapon_id, tabletype);
	int slot = getPanelActiveTeamSlot(panel);
	if(slot < 0 || slot >= numTeams)
		slot = getAllianceIdFromTeam(team);
	if(slot < 0)
		return 0;
	if(!tabletype)
		return (DWORD)&ammoTable[slot][weapon_id];
	return (DWORD)&delayTable[slot][weapon_id];
}

// Адрес данных конкретной команды (team_number_dword38), не зависит от того, чей ход на панели.
DWORD CustomWeapons::getTeamWeaponFieldAddr(CTaskTeam *team, int weapon_id, int tabletype) {
	if(!team)
		return 0;
	if(weapon_id < maxStandardWeapons) {
		DWORD panel = getWeaponPanelFromTeam(team);
		if(!panel)
			return 0;
		int slot = getTeamSlotFromTeam(team);
		if(slot < 0)
			return 0;
		return getStandardWeaponFieldAddr(panel, slot, weapon_id, tabletype);
	}
	int alliance = getAllianceIdFromTeam(team);
	if(alliance < 0)
		return 0;
	if(!tabletype)
		return (DWORD)&ammoTable[alliance][weapon_id];
	return (DWORD)&delayTable[alliance][weapon_id];
}

void CustomWeapons::writeStandardWeaponField(CTaskTeam *team, int weapon_id, int tabletype, int value) {
	DWORD panel = getWeaponPanelFromTeam(team);
	if(!panel || weapon_id < 0 || weapon_id >= maxStandardWeapons)
		return;
	int teamSlot = getTeamSlotFromTeam(team);
	int panelSlot = getPanelActiveTeamSlot(panel);
	int alliance = getAllianceIdFromTeam(team);
	// Собственная строка команды в таблице оружия — обязательна для API и regen.
	if(teamSlot >= 0) {
		DWORD addr = getStandardWeaponFieldAddr(panel, teamSlot, weapon_id, tabletype);
		if(addr)
			*(int *)addr = value;
	}
	// Строка, которую панель рисует прямо сейчас (getAmmoAddr_v2), если отличается.
	if(panelSlot >= 0 && panelSlot != teamSlot) {
		DWORD addr = getStandardWeaponFieldAddr(panel, panelSlot, weapon_id, tabletype);
		if(addr)
			*(int *)addr = value;
	}
	// Alliance-строка для режимов с общим арсеналом.
	if(alliance >= 0 && alliance != teamSlot && alliance != panelSlot) {
		DWORD addr = getStandardWeaponFieldAddr(panel, alliance, weapon_id, tabletype);
		if(addr)
			*(int *)addr = value;
	}
}

int CustomWeapons::readBestShotDelayFromContext(DWORD gameCtx, int teamSlot, int weapon_id, DWORD fixedDelayAddr) {
	int delay = 0;
	if(fixedDelayAddr) {
		int d = *(int *)fixedDelayAddr;
		if(d > delay)
			delay = d;
	}
	if(weapon_id < maxStandardWeapons && gameCtx) {
		DWORD panel = *(DWORD *)(gameCtx + 1352);
		if(panel) {
			auto considerSlot = [&](int slot) {
				if(slot < 0 || slot >= numTeams)
					return;
				DWORD addr = getStandardWeaponFieldAddr(panel, slot, weapon_id, 1);
				if(!addr)
					return;
				int d = *(int *)addr;
				if(d > delay)
					delay = d;
			};
			considerSlot(teamSlot);
			considerSlot(getPanelActiveTeamSlot(panel));
			considerSlot(getAllianceSlotFromContext(gameCtx, teamSlot));
		}
	}
	return delay;
}

int CustomWeapons::readBestShotDelay(CTaskTeam *team, int weapon_id, DWORD fixedDelayAddr) {
	int delay = readBestShotDelayFromContext(getGameGlobalFromTeam(team), getTeamSlotFromTeam(team), weapon_id, fixedDelayAddr);
	int apiDelay = getTeamWeaponDelay(team, weapon_id);
	if(apiDelay > delay)
		delay = apiDelay;
	return delay;
}

void CustomWeapons::storeShotReloadDelay(ShotReloadKey key, int weapon_id, int delay) {
	if(key.second < 0 || weapon_id < 0 || weapon_id >= maxCustomWeapons || delay <= 0)
		return;
	auto storeOne = [&](ShotReloadKey k) {
		if(!k.first)
			return false;
		auto &slot = lastShotReloadDelay[k];
		if(delay > slot[weapon_id]) {
			slot[weapon_id] = delay;
			return true;
		}
		return false;
	};
	bool stored = storeOne(key);
	DWORD altCtx = Game::getAddrGameGlobal();
	if(altCtx && altCtx != key.first)
		stored = storeOne({altCtx, key.second}) || stored;
	if(stored) {
		dbgLastRecordedDelay = delay;
		dbgRecordShotStoredCalls++;
	}
}

void CustomWeapons::recordShotReloadDelayFromContext(DWORD gameCtx, int teamSlot, int weapon_id) {
	if(!gameCtx || teamSlot < 0 || weapon_id < 0 || weapon_id >= maxCustomWeapons)
		return;
	dbgRecordShotCalls++;
	dbgLastWeaponShot = weapon_id;
	ShotReloadKey key = makeShotReloadKeyFromContext(gameCtx, teamSlot);
	DWORD delayAddr = getDelayAddrFromContext(gameCtx, teamSlot, weapon_id);
	int delay = readBestShotDelayFromContext(gameCtx, teamSlot, weapon_id, delayAddr);
	dbgLastCaptureDelay = delay;
	// Fires on every shot for every player; use getCustomWeaponsDebugCounters() from Lua
	// (jellydelay /delay probe) instead of printf-spamming the console for all users.
	if(delay > 0) {
		storeShotReloadDelay(key, weapon_id, delay);
		return;
	}
	for(auto & pending : pendingShotReloadCaptures) {
		if(pending.gameCtx == gameCtx && pending.team_slot == teamSlot && pending.weapon_id == weapon_id) {
			pending.delay_addr = delayAddr;
			pending.peak_delay = 0;
			pending.retries = 120;
			dbgPendingCaptureCount = (int) pendingShotReloadCaptures.size();
			return;
		}
	}
	pendingShotReloadCaptures.push_back({gameCtx, teamSlot, weapon_id, delayAddr, 0, 120});
	dbgPendingCaptureCount = (int) pendingShotReloadCaptures.size();
}

void CustomWeapons::recordShotReloadDelay(CTaskTeam *team, int weapon_id) {
	if(!team)
		return;
	recordShotReloadDelayFromContext(getGameGlobalFromTeam(team), getTeamSlotFromTeam(team), weapon_id);
}

void CustomWeapons::pollPendingShotReloadCaptures() {
	for(size_t i = 0; i < pendingShotReloadCaptures.size(); ) {
		auto & pending = pendingShotReloadCaptures[i];
		if(!pending.gameCtx || pending.team_slot < 0) {
			pendingShotReloadCaptures.erase(pendingShotReloadCaptures.begin() + (ptrdiff_t) i);
			continue;
		}
		ShotReloadKey key = makeShotReloadKeyFromContext(pending.gameCtx, pending.team_slot);
		int live = readBestShotDelayFromContext(pending.gameCtx, pending.team_slot, pending.weapon_id, pending.delay_addr);
		DWORD altCtx = Game::getAddrGameGlobal();
		if(altCtx && altCtx != pending.gameCtx) {
			DWORD altAddr = getDelayAddrFromContext(altCtx, pending.team_slot, pending.weapon_id);
			int liveAlt = readBestShotDelayFromContext(altCtx, pending.team_slot, pending.weapon_id, altAddr);
			if(liveAlt > live)
				live = liveAlt;
		}
		dbgLastCaptureDelay = live;
		if(live > pending.peak_delay)
			pending.peak_delay = live;
		int best = pending.peak_delay;
		if(best > 0) {
			storeShotReloadDelay(key, pending.weapon_id, best);
			pendingShotReloadCaptures.erase(pendingShotReloadCaptures.begin() + (ptrdiff_t) i);
			continue;
		}
		if(--pending.retries <= 0) {
			pendingShotReloadCaptures.erase(pendingShotReloadCaptures.begin() + (ptrdiff_t) i);
			continue;
		}
		i++;
	}
	dbgPendingCaptureCount = (int) pendingShotReloadCaptures.size();
}

static bool isMemReadable(DWORD addr, size_t size) {
	return jellyIsMemReadable(addr, size);
}

static bool isMemWritable(DWORD addr, size_t size) {
	return jellyIsMemWritable(addr, size);
}

int __stdcall CustomWeapons::readMemInt(DWORD addr) {
	if(!addr || !isMemReadable(addr, sizeof(int)))
		return 0;
	return *(int *)addr;
}

void __stdcall CustomWeapons::writeMemInt(DWORD addr, int value) {
	if(!addr || !isMemWritable(addr, sizeof(int)))
		return;
	*(int *)addr = value;
}

std::string CustomWeapons::getDebugCounters() {
	char buff[512];
	sprintf_s(buff, "subtractAmmoV1=%d subtractAmmoV1std=%d subtractAmmoV2=%d subtractAmmoV2std=%d recordShot=%d recordShotStored=%d lastWeaponShot=%d lastCaptureDelay=%d lastRecordedDelay=%d pendingCaptures=%d",
		dbgSubtractAmmoV1Calls, dbgSubtractAmmoV1StandardCalls, dbgSubtractAmmoV2Calls, dbgSubtractAmmoV2StandardCalls,
		dbgRecordShotCalls, dbgRecordShotStoredCalls, dbgLastWeaponShot, dbgLastCaptureDelay, dbgLastRecordedDelay, dbgPendingCaptureCount);
	return std::string(buff);
}

void CustomWeapons::markWeaponPanelDirty(CTaskTeam *team) {
	DWORD panel = getWeaponPanelFromTeam(team);
	if(panel)
		*(DWORD *)(panel + 460) = 1;
}

int CustomWeapons::getTeamWeaponAmmo(CTaskTeam *team, int weapon_id) {
	DWORD addr = getTeamWeaponFieldAddr(team, weapon_id, 0);
	if(!addr)
		return 0;
	return *(int *)addr;
}

int CustomWeapons::getTeamWeaponDelay(CTaskTeam *team, int weapon_id) {
	DWORD addr = getTeamWeaponFieldAddr(team, weapon_id, 1);
	if(!addr)
		return 0;
	return *(int *)addr;
}

void CustomWeapons::setTeamWeaponAmmo(CTaskTeam *team, int weapon_id, int value) {
	if(!team)
		return;
	if(weapon_id < maxStandardWeapons) {
		writeStandardWeaponField(team, weapon_id, 0, value);
		markWeaponPanelDirty(team);
		return;
	}
	DWORD addr = getTeamWeaponFieldAddr(team, weapon_id, 0);
	if(!addr)
		return;
	*(int *)addr = value;
	markWeaponPanelDirty(team);
}

void CustomWeapons::setTeamWeaponDelay(CTaskTeam *team, int weapon_id, int value) {
	if(!team)
		return;
	if(weapon_id < maxStandardWeapons) {
		writeStandardWeaponField(team, weapon_id, 1, value);
		markWeaponPanelDirty(team);
		return;
	}
	DWORD addr = getTeamWeaponFieldAddr(team, weapon_id, 1);
	if(!addr)
		return;
	*(int *)addr = value;
	markWeaponPanelDirty(team);
}

void CustomWeapons::adjustTeamDelays(CTaskTeam *team, int amount) {
	if(!team || amount <= 0)
		return;
	int modified = 0;
	for(int w = 0; w < numAllWeapons; w++) {
		DWORD addr = getTeamWeaponFieldAddr(team, w, 1);
		if(!addr)
			continue;
		int &delay = *(int *)addr;
		if(delay > 0) {
			delay = delay > amount ? delay - amount : 0;
			modified = 1;
		}
	}
	if(modified)
		markWeaponPanelDirty(team);
}

// shadows delayed weapon tiles
DWORD addrWeaponPanelUnknown1_patch1_ret;
void __declspec(naked) CustomWeapons::hookWeaponPanelUnknown1_patch1() {
	//00567550
	_asm push ebx
	_asm mov ebx, [esp+0x8]
	_asm cmp ebx, maxStandardWeapons
	_asm pop ebx
	_asm jge hookWeaponPanelUnknown1_patch1_hook
	// use normal logic for < maxStandardWeapons
	_asm MOV ECX,DWORD PTR DS:[EAX+0x4]
	_asm MOV EDX,DWORD PTR DS:[ECX*4+EAX+0x20]
	_asm IMUL EDX,EDX,0x8E
	_asm PUSH EBX
	_asm MOV EBX,DWORD PTR SS:[ESP+0x8]
	_asm push    esi
	_asm mov     esi, [eax]
	_asm add     edx, ebx
	_asm cmp     dword ptr [esi+edx*4 + 0x65F8], 0

	_asm jmp addrWeaponPanelUnknown1_patch1_ret

_asm hookWeaponPanelUnknown1_patch1_hook:
	_asm push ebx
	_asm mov ebx, [esp + 0x8] //weapon id
	_asm push    esi
	_asm mov     esi, [eax] // *weapon_panel_obj

	_asm push eax //save eax
	_asm push 1
	_asm push ebx //weapon id
	_asm push eax //weapon panel obj
	_asm call getAmmoAddr_v2
	_asm cmp [eax], 0
	_asm pop eax
	_asm jmp addrWeaponPanelUnknown1_patch1_ret
}


DWORD addrWeaponPanelDescription_getAmmoDelay_patch1_ret;
void __declspec(naked) CustomWeapons::hookWeaponPanelDescription_getAmmoDelay_patch1() {
	//00567C24
//	_asm mov     edx, [esi+0x4]
//	_asm mov     ecx, [esi + edx*0x4 + 0x20]
//	_asm mov     edx, [esi]
//	_asm imul    ecx, 0x8E
//	_asm add     ecx, eax
//	_asm mov     edi, [edx + ecx*0x4 + 0x64DC]
//	_asm mov     ecx, [edx + ecx*0x4 + 0x65F8]
//	_asm test    ecx, ecx
//
	// eax = weapon number
	_asm mov     edx, [esi+0x4]
	_asm mov     ecx, [esi + edx*0x4 + 0x20]
	_asm mov     edx, [esi]

	_asm cmp eax, maxStandardWeapons
	_asm jge hookWeaponPanelDescription_getAmmoDelay_hook

	_asm imul    ecx, 0x8E
	_asm add     ecx, eax
	_asm mov     edi, [edx + ecx*0x4 + 0x64DC]
	_asm mov     ecx, [edx + ecx*0x4 + 0x65F8]
	_asm test ecx, ecx
	_asm jmp addrWeaponPanelDescription_getAmmoDelay_patch1_ret
_asm hookWeaponPanelDescription_getAmmoDelay_hook:
	_asm push edx // save edx
	_asm push eax // save eax
	_asm push eax // save eax

	//get ammo
	_asm push 0
	_asm push eax
	_asm push esi
	_asm call getAmmoAddr_v2
	_asm mov edi, [eax]
	_asm pop eax
	//get delay
	_asm push 1
	_asm push eax
	_asm push esi
	_asm call getAmmoAddr_v2
	_asm mov ecx, [eax]
	//restore eax,edx
	_asm pop eax
	_asm pop edx

	_asm test    ecx, ecx
	//ammo = edi
	//delay = ecx
	_asm jmp addrWeaponPanelDescription_getAmmoDelay_patch1_ret
}


int (__stdcall *origReduceDelayOnTurnStart)();
int __stdcall CustomWeapons::hookReduceDelayOnTurnStart() {
	DWORD sesi, retv;
	_asm mov sesi, esi

	_asm mov esi, sesi
	_asm call origReduceDelayOnTurnStart
	_asm mov retv,eax

	if(customWeaponsEnabled) {
		CTaskTeam * team = (CTaskTeam*) sesi;
		DWORD addrGameGlobal = team->unknown2C;
		DWORD v2 = 0;
		DWORD found = 0;
		if(*(DWORD *)(*(DWORD *)(addrGameGlobal + 36) + 55160) < 301 )
			v2 = team->unknown5C;
		else
			v2 = *(DWORD *)(addrGameGlobal + 4 * *(DWORD *)(1308 * team->team_number_dword38 + addrGameGlobal + 17964) + 29240);
		if(v2 > 1) {
			int alliance = *(DWORD *)(1308 * team->team_number_dword38 + addrGameGlobal + 17964);
			for(int weapon=maxStandardWeapons; weapon < maxCustomWeapons; weapon++) {
				auto & delay = delayTable[alliance][weapon];
				if(delay > 0) {
					delay--;
					found = 1;
					if(delay == 0) {
						if (*(DWORD *)(*(DWORD *)(addrGameGlobal + 36) + 55160) < 413 || ammoTable[alliance][weapon]) {
							DWORD weapon_panel = *(DWORD *)(addrGameGlobal + 1352);
							if (weapon_panel)
								*(DWORD *)(weapon_panel + 460) = 1; // redraw
						}
					}
				}
			}
		}
		retv |= found;
	}
	return retv;
}

void CustomWeapons::printDebugAddrs() {
	char buff[1024];
	DWORD addrGameGlobal = Game::getAddrGameGlobal();
	DWORD addrWeaponPanel = *(DWORD*)(addrGameGlobal + 0x548);
	DWORD addrWeaponTable = *(DWORD*)(addrGameGlobal + 0x510);
	sprintf_s(buff, "weaponPanel: %X weaponTable(current): %X weaponTable(original): %X", addrWeaponPanel, addrWeaponTable, originalWeaponTablePtr);
	Chat::callShowChatMessage(buff, 6);
}

// Takes sol::object rather than WeaponStruct so a package passing the WRONG
// TYPE fails loudly instead of corrupting memory. 18 of the ported packages
// call this with a plain Lua table literal instead of a WeaponStruct; binding
// the parameter by value let sol read that table as a struct, which took the
// whole process down at load time with no exception and no crash dump.
int CustomWeapons::registerCustomWeaponLua(sol::object weaponObj, std::string img, std::string name1, std::string name2) {
	if(!weaponObj.is<WeaponStruct>()) {
		printf("registerCustomWeapon('%s'): expected a WeaponStruct, got %s -- weapon not registered\n",
			name1.c_str(), sol::type_name(weaponObj.lua_state(), weaponObj.get_type()).c_str());
		return -1;
	}
	return registerCustomWeapon(weaponObj.as<WeaponStruct>(), img, name1, name2);
}

int CustomWeapons::registerCustomWeapon(CustomWeapons::WeaponStruct weaponStruct, std::string img, std::string name1, std::string name2) {
	// Weapon type 0 (0x30) is not a weapon WA can fire. The ported packages
	// make these from hand-written draft code (a zeroed WeaponStruct, or a
	// ProjectX "-- Sounds --" dummy); they show up as broken panel entries,
	// and a draft re-registering an existing name would wipe the real struct.
	if(weaponStruct.unknown30 == 0 || name1.empty()) {
		auto same = customWeaponIdByKey.find(name1 + '\x01' + name2);
		PxWeaponArt::log("registerCustomWeapon: skipped '%s':'%s' (weapon type 0 / no name)%s\n",
			name1.c_str(), name2.c_str(), same != customWeaponIdByKey.end() ? ", kept existing" : "");
		return same != customWeaponIdByKey.end() ? same->second : -1;
	}

	std::string key = name1 + '\x01' + name2;
	PxWeaponArt::Art art;
	bool hasArt = PxWeaponArt::find(reinterpret_cast<const int *>(&weaponStruct), sizeof(weaponStruct), name1, art);
	std::string resolved = pickWeaponIcon(img, name1, hasArt ? &art : nullptr);

	auto existing = customWeaponIdByKey.find(key);
	if(existing != customWeaponIdByKey.end()) {
		int idx = existing->second - maxStandardWeapons;
		weaponImgInjectionList[idx] = {resolved, 0};
		weaponNameStorage[idx] = {name1, name2};
		weaponStructInjectionList[idx] = weaponStruct;
		customAimSprites.erase(existing->second);
		if(hasArt)
			pxArt[existing->second] = art;
		else
			pxArt.erase(existing->second);
		printf("registerCustomWeapon: re-registered weapon %s:%s id: %d icon: %s\n",
			name1.c_str(), name2.c_str(), existing->second, resolved.c_str());
		return existing->second;
	}

	if(weaponStructInjectionList.size() >= (size_t)(maxCustomWeapons - maxStandardWeapons)) {
                printf("registerCustomWeapon: capacity exhausted for %s:%s id=%zu\n", name1.c_str(), name2.c_str(), weaponStructInjectionList.size());
                return -1;
        }
        weaponImgInjectionList.emplace_back(resolved, 0);
	weaponNameStorage.emplace_back(name1, name2);
	weaponStructInjectionList.push_back(weaponStruct);
	int id = maxStandardWeapons + (int)weaponStructInjectionList.size() - 1;
	customWeaponIdByKey[key] = id;
	if(hasArt)
		pxArt[id] = art;
	printf("registerCustomWeapon: registered weapon %s:%s id: %d icon: %s\n", name1.c_str(), name2.c_str(), id, resolved.c_str());
	return id;
}

void CustomWeapons::resetConfig() {
//	for(int i=maxStandardWeapons; i < weaponStructInjectionList.size(); i++)
//		memset(&weaponTable[i], 0, sizeof(WeaponStruct));
	weaponNameStorage.clear();
	weaponImgInjectionList.clear();
	weaponStructInjectionList.clear();
	customWeaponIdByKey.clear();
	customAimSprites.clear();
	pxArt.clear();
	customWeaponsEnabled = 0;
	numAllWeapons = maxStandardWeapons;
}


int __stdcall CustomWeapons::aimSpriteFor(WeaponStruct *ws) {
	if(!customWeaponsEnabled || !ws)
		return 0;
	int idx = -1;
	if(ws >= &weaponTable[0] && ws < &weaponTable[0] + maxCustomWeapons)
		idx = (int)(ws - &weaponTable[0]);
	else {
		// Not a pointer into our table: identify the weapon by its name string,
		// which every custom entry owns uniquely (weaponNameStorage).
		for(int i = maxStandardWeapons; i < numAllWeapons; i++) {
			if(ws->name1 && ws->name1 == weaponTable[i].name1) {
				idx = i;
				break;
			}
		}
	}
	if(idx < maxStandardWeapons || idx >= numAllWeapons)
		return 0;
	auto it = customAimSprites.find(idx);
	if(it != customAimSprites.end() && it->second > 0)
		return it->second;
	return -1;
}

// WA's worm aim sprite lookup (0x51C060): eax = WeaponStruct*, one stack
// argument (the worm), `ret 4`, sprite id in eax. For ported ProjectX weapons
// it falls off its tables (0x60 holds 100000 or a custom sprite id) and
// returns 0, and the worm animator then draws sprite 0: an invisible worm.
// Custom weapons get their own aim art when the package has it, otherwise
// whatever WA computes, with the bazooka pose (0x20D) instead of nothing.
DWORD addrWormAimSprite_resume;
void __declspec(naked) hookWormAimSprite() {
	_asm push edx
	_asm push eax
	_asm push eax
	_asm call CustomWeapons::aimSpriteFor
	_asm test eax, eax
	_asm jg have_custom
	_asm mov ecx, eax // 0 = vanilla weapon, -1 = custom weapon without art
	_asm pop eax // WeaponStruct*
	_asm push ecx
	_asm push dword ptr [esp + 0xC] // worm argument for the original body
	_asm call original_body
	_asm pop ecx
	_asm test eax, eax
	_asm jnz done
	_asm test ecx, ecx
	_asm jz done
	_asm mov eax, 0x20D
	_asm done:
	_asm pop edx
	_asm ret 4

	_asm have_custom:
	_asm add esp, 4
	_asm pop edx
	_asm ret 4

	// Replays the 6 bytes our jump overwrote, then continues in WA's body,
	// which returns to our call with `ret 4` (popping the pushed worm).
	_asm original_body:
	_asm mov ecx, dword ptr [eax + 0x30]
	_asm add ecx, -1
	_asm jmp addrWormAimSprite_resume
}

int CustomWeapons::install(SignatureScanner & signatureScanner, module mod) {
	DWORD addrInitializeWeaponTable = Hooks::scanPattern("InitializeWeaponTable", "\x53\x55\x8B\x6C\x24\x0C\x8B\x85\x00\x00\x00\x00\x8B\x88\x00\x00\x00\x00\x56\x57\x68\x00\x00\x00\x00\x33\xDB\x53\x51\xE8\x00\x00\x00\x00\x83\xC4\x0C", "??????xx????xx????xxx????xxxxx????xxx", 0x53CAB0);
	DWORD addrPrepareWeaponPanel = Hooks::scanPattern("PrepareWeaponPanel", "\x83\xEC\x3C\x53\x55\x56\x57\x8B\xF0\x8D\x44\x24\x38\x50\x8D\x4C\x24\x38\x51\x8D\x7C\x24\x50\xE8\x00\x00\x00\x00\xE8\x00\x00\x00\x00", "??????xxxxxxxxxxxxxxxxxx????x????", 0x568220);

	DWORD addrWormAimSprite = Hooks::scanPattern("WormAimSprite", "\x8B\x48\x30\x83\xC1\xFF\x83\xF9\x03\x0F\x87\xCA\x01\x00\x00\xFF\x24\x8D\x40\xC2\x51\x00\x8B\x48\x34\x83\xC1\xFF\x83\xF9\x0B\x0F\x87", "??????xxxxx????xxx????xxxxxxxxxxx", 0x51C060);
	addrWormAimSprite_resume = addrWormAimSprite + 6;
	Hooks::hookAsm(addrWormAimSprite, (DWORD)&hookWormAimSprite);

	DWORD addrPrepareWeaponPanelHookLoop = addrPrepareWeaponPanel + 0x100;
	addrPrepareWeaponPanelHookLoopEndEarly = addrPrepareWeaponPanel + 0x10D;
	addrPrepareWeaponPanelHookLoopEnd = addrPrepareWeaponPanel + 0x2CC;

	DWORD addrWeaponPanelDrawTile = Hooks::scanPattern("WeaponPanelDrawTile", "\x8B\x44\x24\x08\x83\xEC\x1C\x80\xBF\x00\x00\x00\x00\x00\x53\x55\x56\x0F\x84\x00\x00\x00\x00\x83\xF8\x10\x0F\x85\x00\x00\x00\x00\x8B\x44\x24\x2C\x83\xF8\x05\x0F\x87\x00\x00\x00\x00", "??????xxx?????xxxxx????xxxxx????xxxxxxxxx????", 0x567AA0);
	DWORD addrWeaponPanelDrawTile_GetSlot_patch1 = addrWeaponPanelDrawTile + 0x1F6; //00567826
	addrWeaponPanelDrawTile_GetSlot_patch1_ret = addrWeaponPanelDrawTile + 0x202; //00567832

	DWORD addrWeaponPanelDrawTile_GetSlot_patch3 = addrWeaponPanelDrawTile + 0x11A; // 0056774A
	addrWeaponPanelDrawTile_GetSlot_patch3_ret = addrWeaponPanelDrawTile + 0x124; // 00567754

	DWORD addrWeaponPanelDescription= Hooks::scanPattern("WeaponPanelDescription", "\x8B\x46\x74\x83\xEC\x10\x53\x8B\x5E\x50\x2B\xD8\x55\x83\xEB\x03\x80\xBE\x00\x00\x00\x00\x00\x57", "??????xxxxxxxxxxxx?????x", 0x567F80);
	DWORD addrWeaponPanelDescription_GetSlot_patch1 = addrWeaponPanelDescription + 0x101; // 00567C11
	addrWeaponPanelDescription_GetSlot_patch1_ret = addrWeaponPanelDescription + 0x10E; // 00567C1E

	DWORD addrWeaponPanelCheckClick= Hooks::scanPattern("WeaponPanelCheckClick", "\x51\x8B\x44\x24\x08\x53\x8B\x5C\x24\x10\xC7\x00\x00\x00\x00\x00\x83\xBE\x00\x00\x00\x00\x00\xC7\x03\x00\x00\x00\x00", "??????xxxxxx????xx?????xx????", 0x5690B0);
	DWORD addrWeaponPanelCheckClick_patch1 = addrWeaponPanelCheckClick + 0x58; // 00568C98
	addrWeaponPanelCheckClick_patch1_ret = addrWeaponPanelCheckClick + 0x7B; // 00568CBB

	DWORD addrWeaponPanelUnknown1= Hooks::scanPattern("WeaponPanelUnknown1", "\x8B\x48\x04\x8B\x54\x88\x20\x69\xD2\x00\x00\x00\x00\x53\x8B\x5C\x24\x08\x56\x8B\x30\x03\xD3\x83\xBC\x96\x00\x00\x00\x00\x00\x74\x0A", "??????xxx????xxxxxxxxxxxxx?????xx", 0x5679C0);
	addrWeaponPanelUnknown1_patch1_ret = addrWeaponPanelUnknown1 + 0x1F; 	//0056756F

	Hooks::hookAsm(addrPrepareWeaponPanelHookLoop, (DWORD)&hookWeaponPanelLoop_wrapper);
	Hooks::hookAsm(addrWeaponPanelDrawTile_GetSlot_patch1, (DWORD)&hookWeaponPanelDrawTile_GetSlot_patch1);
	Hooks::hookAsm(addrWeaponPanelDrawTile_GetSlot_patch3, (DWORD)&hookWeaponPanelDrawTile_GetSlot_patch3);
	Hooks::hookAsm(addrWeaponPanelDescription_GetSlot_patch1, (DWORD)&hookWeaponPanelDescription_GetSlot_patch1);
	Hooks::hookAsm(addrWeaponPanelCheckClick_patch1, (DWORD)&hookWeaponPanelCheckClick_patch1);
	Hooks::hookAsm(addrWeaponPanelUnknown1, (DWORD)&hookWeaponPanelUnknown1_patch1);

	//// weapon imgs
	DWORD addrLoadWeaponPanelImgs = Hooks::scanPattern("LoadWeaponPanelImgs","\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x55\x56\x57\x8B\xBC\x24\x00\x00\x00\x00\x8B\xF1", "???????xx????xxxx????xx????xxxxxxx????xx", 0x53D0E0);
	DWORD addrWormSwitchingWeapon = Hooks::scanPattern("WormSwitchingWeapon", "\x81\xEC\x00\x00\x00\x00\x53\x55\x8B\xAC\x24\x00\x00\x00\x00\x8B\x85\x00\x00\x00\x00\x8B\x9D\x00\x00\x00\x00\x56\x57\x8B\xBD\x00\x00\x00\x00\x8B\xCD\x89\x7C\x24\x10", "??????xxxxx????xx????xx????xxxx????xxxxxx", 0x515650);

	DWORD addrWeaponPanelDrawTile_GetImgPtr_patch1 = addrWeaponPanelDrawTile + 0x3D0; // 00567A00
	addrWeaponPanelDrawTile_GetImgPtr_patch1_ret = addrWeaponPanelDrawTile + 0x3E8; // 00567A18
	Hooks::hookAsm(addrWeaponPanelDrawTile_GetImgPtr_patch1, (DWORD)&hookWeaponPanelDrawTile_GetImgPtr_patch1);

	DWORD addrWeaponPanelDrawTile_GetImgPtr_patch2 = addrWeaponPanelDrawTile + 0x408; // 00567A38
	addrWeaponPanelDrawTile_GetImgPtr_patch2_ret = addrWeaponPanelDrawTile + 0x420; //  00567A50
	Hooks::hookAsm(addrWeaponPanelDrawTile_GetImgPtr_patch2, (DWORD)&hookWeaponPanelDrawTile_GetImgPtr_patch2);

	DWORD addrWormSwitchingWeapon_GetImgPtr_patch1 = addrWormSwitchingWeapon + 0x324; //00515844
	addrWormSwitchingWeapon_GetImgPtr_patch1_ret = addrWormSwitchingWeapon + 0x337; //00515857
	Hooks::hookAsm(addrWormSwitchingWeapon_GetImgPtr_patch1, (DWORD)&hookWormSwitchingWeapon_GetImgPtr_patch1);

	// weapon panel width & iteration
	DWORD addrConstructDDGame = Hooks::scanPattern("ConstructDDGame", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\xB8\x00\x00\x00\x00\x64\x89\x25\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x53\x55\x8B\xAC\x24\x00\x00\x00\x00\x56\x57", "???????xx????xx????xxx????x????xxxxx????xx", 0x56E220);
	DWORD addConstrucDDGame_WeaponPanelWidth_patch = addrConstructDDGame + 0xCF5; //0056E865

	unsigned char weapon_panel_box_width_op[] = {0x68, 0x00, 0x08, 0x00, 0x00};
	Hooks::patchAsm(addConstrucDDGame_WeaponPanelWidth_patch, weapon_panel_box_width_op, sizeof(weapon_panel_box_width_op));


	DWORD addrWeaponPanel_incrementCounter_patch1 = addrPrepareWeaponPanel + 0x2F2; //005680A2
	addrWeaponPanel_incrementCounter_patch1_ret = addrPrepareWeaponPanel + 0x2FF; //05680AF
	Hooks::hookAsm(addrWeaponPanel_incrementCounter_patch1, (DWORD)&hookWeaponPanel_incrementCounter_patch1);

	DWORD addrWeaponPanel_incrementCounter_patch2 = addrPrepareWeaponPanel + 0x325; //005680D5
	addrWeaponPanel_incrementCounter_patch2_ret = addrPrepareWeaponPanel + 0x33F; //005680EF
	Hooks::hookAsm(addrWeaponPanel_incrementCounter_patch2, (DWORD)&hookWeaponPanel_incrementCounter_patch2);


	// ammo/delay inline patches
	DWORD addrWeaponPanelDescription_getAmmoDelay_patch1 = addrWeaponPanelDescription + 0x114; // 00567C24
	addrWeaponPanelDescription_getAmmoDelay_patch1_ret = addrWeaponPanelDescription + 0x135; // 0567C45
	Hooks::hookAsm(addrWeaponPanelDescription_getAmmoDelay_patch1, (DWORD)&hookWeaponPanelDescription_getAmmoDelay_patch1);

	DWORD addrGetAmmo = Hooks::scanPattern("GetAmmo", "\x8B\xC8\x69\xC9\x00\x00\x00\x00\x8B\x4C\x31\x04\x69\xC9\x00\x00\x00\x00\x57\x8D\x3C\x11\x83\xBC\xBE\x00\x00\x00\x00\x00\x74\x1E\x83\xBE\x00\x00\x00\x00\x00", "????????xxxxxx????xxxxxxx?????xxxx?????", 0x5225E0);
	DWORD addrAddAmmo = Hooks::scanPattern("AddAmmo", "\x8B\x4C\x24\x04\x69\xC0\x00\x00\x00\x00\x8B\x44\x08\x04\x69\xC0\x00\x00\x00\x00\x03\x44\x24\x08\x8D\x84\x81\x00\x00\x00\x00\x8B\x08\x85\xC9\x7C\x11\x85\xD2", "??????????xxxxxx????xxxxxxx????xxxxxxxx", 0x522640);
	DWORD addrSubtractAmmo = Hooks::scanPattern("SubtractAmmo", "\x69\xC0\x00\x00\x00\x00\x8B\x44\x08\x04\x69\xC0\x00\x00\x00\x00\x03\x44\x24\x04\x8D\x84\x81\x00\x00\x00\x00\x8B\x08\x85\xC9\x7E\x05\x83\xC1\xFF\x89\x08", "??????xxxxxx????xxxxxxx????xxxxxxxxxxx", 0x522680);
	DWORD addrSubtractAmmo_v2 = Hooks::scanPattern("SubtractAmmo_v2", "\x8B\x42\x2C\xC7\x42\x00\x00\x00\x00\x00\x8B\x80\x00\x00\x00\x00\x85\xC0\x74\x0A\xC7\x80\x00\x00\x00\x00\x00\x00\x00\x00\x8B\x4A\x38", "??????????xx????xxxxxx????????xxx", 0x558E80);
	DWORD addrReduceDelayOnTurnStart = Hooks::scanPattern("ReduceDelayOnTurnStart", "\x8B\x46\x2C\x8B\x48\x24\x81\xB9\x00\x00\x00\x00\x00\x00\x00\x00\x53\x55\x57\x7C\x19\x8B\x56\x38\x69\xD2\x00\x00\x00\x00\x8B\x8C\x02\x00\x00\x00\x00", "??????xx????????xxxxxxxxxx????xxx????", 0x556E90);
	DWORD addrGetAmmoFkeys = Hooks::scanPattern("GetAmmoFkeys", "\x51\x8B\x44\x24\x10\xC7\x00\x00\x00\x00\x00\x8B\x41\x38\x69\xC0\x00\x00\x00\x00\x53\x55\x56\x8B\x71\x2C", "??????x????xxxxx????xxxxxx", 0x55AAC0);

	Hooks::polyhook("initializeWeaponTable", addrInitializeWeaponTable, (DWORD *) &hookInitializeWeaponTable, (DWORD *) &origInitializeWeaponTable);
	Hooks::polyhook("getAmmo", addrGetAmmo, (DWORD *) &hookGetAmmo, (DWORD *) &origGetAmmo);
	Hooks::polyhook("addAmmo", addrAddAmmo, (DWORD *) &hookAddAmmo, (DWORD *) &origAddAmmo);
	Hooks::polyhook("subtractAmmo_v1", addrSubtractAmmo, (DWORD *) &hookSubtractAmmo, (DWORD *) &origSubtractAmmo);
	Hooks::polyhook("subtractAmmo_v2", addrSubtractAmmo_v2, (DWORD *) &hookSubtractAmmo_v2, (DWORD *) &origSubtractAmmo_v2);
//	Hooks::minhook("loadWeaponPanelImgs", addrLoadWeaponPanelImgs, (DWORD*)&hookLoadWeaponPanelImgs, (DWORD*)&origLoadWeaponPanelImgs);
//	Hooks::minhook("vfsCreateGfxDirReader", addrVfsCreateGfxDirReader, (DWORD*)&hookVfsCreateGfxDirReader, (DWORD*)&origVfsCreateGfxDirReader);
//	Hooks::minhook("loadImgFromVfs", addrLoadImgFromVfs, (DWORD*) &hookLoadImgFromVfs, (DWORD*)&origLoadImgFromVfs);
	Hooks::polyhook("reduceDelayOnTurnStart", addrReduceDelayOnTurnStart, (DWORD *) &hookReduceDelayOnTurnStart, (DWORD *) &origReduceDelayOnTurnStart);


	auto * lua = Lua::getInstance().getState();
	// Ported packages routinely write/read struct field names that don't
	// exist on this usertype at all -- px2jelly dumped ProjectX's struct
	// byte-by-byte (unknownD, unknown4, unknown8, ...) while WeaponStruct
	// here is only bound at the real DWORD-aligned offsets, so those names
	// land on nothing. Without a fallback, sol hard-errors ("no defined
	// new_index operation") and aborts the whole package's load over what is
	// always dead/duplicate code (the same bytes are already covered by a
	// correctly-named neighboring field, e.g. unknownC). Accept unknown
	// writes as no-ops and unknown reads as nil instead.
	sol::usertype <WeaponStruct> ut = lua->new_usertype<WeaponStruct> ("WeaponStruct",
		sol::meta_function::new_index, [](WeaponStruct &, std::string, sol::object) {},
		sol::meta_function::index, [](WeaponStruct &, std::string) -> sol::object { return sol::lua_nil; });
	lua->set_function("registerCustomWeapon", &registerCustomWeaponLua);
	lua->set_function("cloneVanillaWeapon", &cloneVanillaWeapon);
	lua->set_function("vanillaWeaponsReady", &vanillaWeaponsReady);
	lua->set_function("dumpVanillaWeapons", &dumpVanillaWeapons);
	lua->set_function("getTeamWeaponAmmo", &getTeamWeaponAmmo);
	lua->set_function("getTeamWeaponDelay", &getTeamWeaponDelay);
	lua->set_function("getTeamWeaponLastShotDelay", &getTeamWeaponLastShotDelay);
	lua->set_function("pollPendingShotReloadCaptures", &pollPendingShotReloadCaptures);
	lua->set_function("setTeamWeaponAmmo", &setTeamWeaponAmmo);
	lua->set_function("setTeamWeaponDelay", &setTeamWeaponDelay);
	lua->set_function("adjustTeamDelays", &adjustTeamDelays);
	lua->set_function("getWeaponPanelFromTeam", &getWeaponPanelFromTeam);
	lua->set_function("getAllianceIdFromTeam", &getAllianceIdFromTeam);
	lua->set_function("getPanelTeamSlotFromTeam", &getPanelTeamSlotFromTeam);
	lua->set_function("getTeamSlotFromTeam", &getTeamSlotFromTeam);

	// diagnostics + raw-memory access for /delay probe и для обхода "slot успел смениться" race
	lua->set_function("getTeamWeaponAddr", &getTeamWeaponFieldAddr);
	lua->set_function("getPanelWeaponAddr", &getPanelWeaponFieldAddr);
	lua->set_function("readMemInt", &readMemInt);
	lua->set_function("writeMemInt", &writeMemInt);
	lua->set_function("markWeaponPanelDirtyForTeam", &markWeaponPanelDirty);
	lua->set_function("getCustomWeaponsDebugCounters", &getDebugCounters);

	// Ported packages routinely do `ws.name1 = "Foo"` before calling
	// registerCustomWeapon(ws, img, "Foo", "Foo") with the real name as an
	// explicit argument -- the field write is always dead code (name1/name2
	// are raw char* into WA's native struct and can't safely own a Lua
	// string), but making it a hard error broke ~400 packages that carry
	// this harmless leftover from the ProjectX source. Accept and ignore it.
	ut["name1"] = sol::property(
		[](WeaponStruct & w) -> const char * { return w.name1 ? w.name1 : ""; },
		[](WeaponStruct &, const std::string &) {});
	ut["name2"] = sol::property(
		[](WeaponStruct & w) -> const char * { return w.name2 ? w.name2 : ""; },
		[](WeaponStruct &, const std::string &) {});
	ut["panelRow"] = &WeaponStruct::panelRow;
	ut["unknownC"] = &WeaponStruct::unknownC;
	ut["unknown10"] = &WeaponStruct::unknown10;
	ut["unknown14"] = &WeaponStruct::unknown14;
	ut["unknown18"] = &WeaponStruct::unknown18;
	ut["unknown1C"] = &WeaponStruct::unknown1C;
	ut["unknown20"] = &WeaponStruct::unknown20;
	ut["unknown24"] = &WeaponStruct::unknown24;
	ut["unknown28"] = &WeaponStruct::unknown28;
	ut["unknown2C"] = &WeaponStruct::unknown2C;
	ut["unknown30"] = &WeaponStruct::unknown30;
	ut["unknown34"] = &WeaponStruct::unknown34;
	ut["unknown38"] = &WeaponStruct::unknown38;
	ut["unknown3C"] = &WeaponStruct::unknown3C;
	ut["unknown40"] = &WeaponStruct::unknown40;
	ut["unknown44"] = &WeaponStruct::unknown44;
	ut["unknown48"] = &WeaponStruct::unknown48;
	ut["unknown4C"] = &WeaponStruct::unknown4C;
	ut["unknown50"] = &WeaponStruct::unknown50;
	ut["unknown54"] = &WeaponStruct::unknown54;
	ut["unknown58"] = &WeaponStruct::unknown58;
	ut["unknown5C"] = &WeaponStruct::unknown5C;
	ut["unknown60"] = &WeaponStruct::unknown60;
	ut["unknown64"] = &WeaponStruct::unknown64;
	ut["unknown68"] = &WeaponStruct::unknown68;
	ut["unknown6C"] = &WeaponStruct::unknown6C;
	ut["unknown70"] = &WeaponStruct::unknown70;
	ut["unknown74"] = &WeaponStruct::unknown74;
	ut["unknown78"] = &WeaponStruct::unknown78;
	ut["unknown7C"] = &WeaponStruct::unknown7C;
	ut["unknown80"] = &WeaponStruct::unknown80;
	ut["unknown84"] = &WeaponStruct::unknown84;
	ut["unknown88"] = &WeaponStruct::unknown88;
	ut["unknown8C"] = &WeaponStruct::unknown8C;
	ut["unknown90"] = &WeaponStruct::unknown90;
	ut["unknown94"] = &WeaponStruct::unknown94;
	ut["unknown98"] = &WeaponStruct::unknown98;
	ut["unknown9C"] = &WeaponStruct::unknown9C;
	ut["unknownA0"] = &WeaponStruct::unknownA0;
	ut["unknownA4"] = &WeaponStruct::unknownA4;
	ut["unknownA8"] = &WeaponStruct::unknownA8;
	ut["unknownAC"] = &WeaponStruct::unknownAC;
	ut["unknownB0"] = &WeaponStruct::unknownB0;
	ut["unknownB4"] = &WeaponStruct::unknownB4;
	ut["unknownB8"] = &WeaponStruct::unknownB8;
	ut["unknownBC"] = &WeaponStruct::unknownBC;
	ut["unknownC0"] = &WeaponStruct::unknownC0;
	ut["unknownC4"] = &WeaponStruct::unknownC4;
	ut["unknownC8"] = &WeaponStruct::unknownC8;
	ut["unknownCC"] = &WeaponStruct::unknownCC;
	ut["unknownD0"] = &WeaponStruct::unknownD0;
	ut["unknownD4"] = &WeaponStruct::unknownD4;
	ut["unknownD8"] = &WeaponStruct::unknownD8;
	ut["unknownDC"] = &WeaponStruct::unknownDC;
	ut["unknownE0"] = &WeaponStruct::unknownE0;
	ut["unknownE4"] = &WeaponStruct::unknownE4;
	ut["unknownE8"] = &WeaponStruct::unknownE8;
	ut["unknownEC"] = &WeaponStruct::unknownEC;
	ut["unknownF0"] = &WeaponStruct::unknownF0;
	ut["unknownF4"] = &WeaponStruct::unknownF4;
	ut["unknownF8"] = &WeaponStruct::unknownF8;
	ut["unknownFC"] = &WeaponStruct::unknownFC;
	ut["unknown100"] = &WeaponStruct::unknown100;
	ut["unknown104"] = &WeaponStruct::unknown104;
	ut["unknown108"] = &WeaponStruct::unknown108;
	ut["unknown10C"] = &WeaponStruct::unknown10C;
	ut["unknown110"] = &WeaponStruct::unknown110;
	ut["unknown114"] = &WeaponStruct::unknown114;
	ut["unknown118"] = &WeaponStruct::unknown118;
	ut["unknown11C"] = &WeaponStruct::unknown11C;
	ut["unknown120"] = &WeaponStruct::unknown120;
	ut["unknown124"] = &WeaponStruct::unknown124;
	ut["unknown128"] = &WeaponStruct::unknown128;
	ut["unknown12C"] = &WeaponStruct::unknown12C;
	ut["unknown130"] = &WeaponStruct::unknown130;
	ut["unknown134"] = &WeaponStruct::unknown134;
	ut["unknown138"] = &WeaponStruct::unknown138;
	ut["unknown13C"] = &WeaponStruct::unknown13C;
	ut["unknown140"] = &WeaponStruct::unknown140;
	ut["unknown144"] = &WeaponStruct::unknown144;
	ut["unknown148"] = &WeaponStruct::unknown148;
	ut["unknown14C"] = &WeaponStruct::unknown14C;
	ut["unknown150"] = &WeaponStruct::unknown150;
	ut["unknown154"] = &WeaponStruct::unknown154;
	ut["unknown158"] = &WeaponStruct::unknown158;
	ut["unknown15C"] = &WeaponStruct::unknown15C;
	ut["unknown160"] = &WeaponStruct::unknown160;
	ut["unknown164"] = &WeaponStruct::unknown164;
	ut["unknown168"] = &WeaponStruct::unknown168;
	ut["unknown16C"] = &WeaponStruct::unknown16C;
	ut["unknown170"] = &WeaponStruct::unknown170;
	ut["unknown174"] = &WeaponStruct::unknown174;
	ut["unknown178"] = &WeaponStruct::unknown178;
	ut["unknown17C"] = &WeaponStruct::unknown17C;
	ut["unknown180"] = &WeaponStruct::unknown180;
	ut["unknown184"] = &WeaponStruct::unknown184;
	ut["unknown188"] = &WeaponStruct::unknown188;
	ut["unknown18C"] = &WeaponStruct::unknown18C;
	ut["unknown190"] = &WeaponStruct::unknown190;
	ut["unknown194"] = &WeaponStruct::unknown194;
	ut["unknown198"] = &WeaponStruct::unknown198;
	ut["unknown19C"] = &WeaponStruct::unknown19C;
	ut["unknown1A0"] = &WeaponStruct::unknown1A0;
	ut["unknown1A4"] = &WeaponStruct::unknown1A4;
	ut["unknown1A8"] = &WeaponStruct::unknown1A8;
	ut["unknown1AC"] = &WeaponStruct::unknown1AC;
	ut["unknown1B0"] = &WeaponStruct::unknown1B0;
	ut["unknown1B4"] = &WeaponStruct::unknown1B4;
	ut["unknown1B8"] = &WeaponStruct::unknown1B8;
	ut["unknown1BC"] = &WeaponStruct::unknown1BC;
	ut["unknown1C0"] = &WeaponStruct::unknown1C0;
	ut["unknown1C4"] = &WeaponStruct::unknown1C4;
	ut["unknown1C8"] = &WeaponStruct::unknown1C8;
	ut["unknown1CC"] = &WeaponStruct::unknown1CC;

	return 0;
}

//void CustomWeapons::switchWeaponTable() {
//	DWORD addrDDGame = Game::getAddrGameGlobal();
//	DWORD addrWeaponPanel = *(DWORD*)(addrDDGame + 0x548);
//	*(DWORD*)(addrWeaponPanel + 0x1CC) = 1;
//}



