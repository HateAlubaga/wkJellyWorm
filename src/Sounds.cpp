#include "Sounds.h"
#include "Hooks.h"
#include "Game.h"
#include "Lua.h"
#include "Utils.h"
#include "MemAccess.h"
#include "Constants.h"
#include <sol/sol.hpp>


DWORD (__fastcall *origLoadGameSoundFromFile)(DWORD This, int ECX, int index, const char * filename);
DWORD __fastcall hookLoadGameSoundFromFile(DWORD This, int EDX, int index, const char *filename) {
	auto ret = origLoadGameSoundFromFile(This, 0, index, filename);
//	printf("hookLoadGameSoundFromFile: %s = %d ret: %X\n", filename, index);
	return ret;
}

DWORD callLoadGameSoundFromFile(int index, std::string filename) {
	return hookLoadGameSoundFromFile(Game::getAddrDsSound(), 0, index, filename.c_str());
}

void Sounds::onConstructGlobalContext(int a1) {
	int id = customSoundStartingId;
	for(auto & it : customSounds) {
		printf("Loading custom sound: %s = %d\n", it.c_str(), id);
		callLoadGameSoundFromFile(id, it);
		id++;
	}
}

int Sounds::registerCustomSound(std::string filename) {
	// Resolve against the active package's own directory now -- the actual
	// load happens later from onConstructGlobalContext, by which point no
	// package's directory is active, so a bare filename would be looked up
	// relative to WA's process directory and silently fail to load.
	std::string resolved = Utils::resolveAssetPath(filename);
	if(resolved != filename)
		printf("registerCustomSound: resolved '%s' -> '%s'\n", filename.c_str(), resolved.c_str());
	customSounds.push_back(resolved);
	return customSoundStartingId + customSounds.size() - 1;
}

void Sounds::resetConfig() {
	customSounds.clear();
}

DWORD addrPlaySoundLocal;
int Sounds::callPlaySoundLocal(int a1, int a2, CTask * object, Constants::SoundIds id, int a5) {
	int retv;
	_asm push a5
	_asm push id
	_asm mov edi, object
	_asm mov ecx, a2
	_asm mov eax, a1
	_asm call addrPlaySoundLocal
	_asm mov retv, eax
	return retv;
}

DWORD addrPlaySoundGlobal;
static int callPlaySoundGlobal(DWORD task, int a1, int a2, int id, int a5) {
	typedef int (__fastcall *PlaySoundGlobalFn)(DWORD, DWORD, int, int, int, int);	// thiscall, args id, a5, a2, a1
	return ((PlaySoundGlobalFn)addrPlaySoundGlobal)(task, 0, id, a5, a2, a1);
}

// Lua numbers -> WA 16.16 fixed. Packages pass ProjectX-style floats (1.0, 0.8),
// which the old int binding truncated to 1/0 = silent.
static int soundVolumeFixed(const sol::object &o) {
	if(o.get_type() != sol::type::number)
		return 0x10000;
	double v = o.as<double>();
	if(v <= 0)
		return 0;
	if(v <= 64)
		v *= 65536.0;
	else if(v < 4096)
		v = v / 100.0 * 65536.0;	// percent
	if(v > 4 * 65536.0)
		v = 4 * 65536.0;
	return (int)v;
}

static int soundPitchFixed(const sol::object &o) {
	if(o.get_type() != sol::type::number)
		return 0x10000;
	double v = o.as<double>();
	if(v >= 0.25 && v <= 4)
		return (int)(v * 65536.0);
	if(v >= 0x4000 && v <= 0x40000)
		return (int)v;
	return 0x10000;	// 0 / pan-style values: keep normal pitch
}

static bool soundTaskValid(DWORD task) {
	if(task < 0x10000 || !jellyProbeRead(task, 0x100))
		return false;
	DWORD vt = *(DWORD*)task;
	if(vt < 0x400000 || vt >= 0x800000)
		return false;
	DWORD ddgame = *(DWORD*)(task + 0x2C);
	return ddgame && jellyProbeRead(ddgame, 0x8144);
}

static DWORD soundTaskFrom(const sol::object &o) {
	DWORD addr = 0;
	if(o.get_type() == sol::type::number) {
		double d = o.as<double>();
		if(d >= 0x10000 && d < 4294967296.0)
			addr = (DWORD)d;
	} else if(o.get_type() == sol::type::userdata) {
		// sol keeps the object pointer at the start of the userdata block
		const void *block = o.pointer();
		if(block && jellyProbeRead((DWORD)block, sizeof(DWORD)))
			addr = *(const DWORD*)block;
	}
	return soundTaskValid(addr) ? addr : 0;
}

// Only GameTask-derived objects own the sound emitter at +0xE8.
static bool soundTaskHasEmitter(DWORD task) {
	using CT = Constants::ClassType;
	int ct = *(int*)(task + 0x20);
	switch(ct) {
		case CT::ClassType_GameTask: case CT::ClassType_GameCollisionTask:
		case CT::ClassType_Task_Mine: case CT::ClassType_Task_Canister:
		case CT::ClassType_Task_Missile: case CT::ClassType_Task_Arrow:
		case CT::ClassType_Task_Crate: case CT::ClassType_Task_Worm:
		case CT::ClassType_Task_OldWorm: case CT::ClassType_Task_Drill:
		case CT::ClassType_Task_FireBall: case CT::ClassType_Task_OilDrum:
			return true;
		default:
			return false;
	}
}

// playSoundLocal(volume, pitch, object, soundId, priority)
// volume/pitch: 1.0 = normal (16.16 raw also accepted); object: task userdata,
// task address or nil (plays non-positional through the turn game).
static int luaPlaySoundLocal(sol::object vol, sol::object pitch, sol::object obj, sol::object id, sol::object prio) {
	if(id.get_type() != sol::type::number)
		return 0;
	int soundId = (int)id.as<double>();
	if(soundId <= 0)
		return 0;
	int a1 = soundVolumeFixed(vol);
	if(a1 <= 0)
		return 0;
	int a2 = soundPitchFixed(pitch);
	int a5 = 0;
	if(prio.get_type() == sol::type::number) {
		a5 = (int)prio.as<double>();
		if(a5 < 0 || a5 > 15)
			a5 = 0;
	}
	DWORD task = soundTaskFrom(obj);
	if(task && soundTaskHasEmitter(task))
		return Sounds::callPlaySoundLocal(a1, a2, (CTask *)task, (Constants::SoundIds)soundId, a5);
	if(!task)
		task = Game::getAddrTurnGameObject();
	if(!soundTaskValid(task))
		return 0;
	return callPlaySoundGlobal(task, a1, a2, soundId, a5);
}


int Sounds::install(SignatureScanner &, module) {
	DWORD addrConstructDSSound = Hooks::scanPattern("ConstructDSSound", "\x53\x56\x8B\xF0\x33\xDB\x57\x33\xC0\xC7\x06\x00\x00\x00\x00\x89\x5E\x08\x89\x5E\x10\x89\x5E\x0C\x89\x9E\x00\x00\x00\x00\xC7\x86\x00\x00\x00\x00\x00\x00\x00\x00\x8D\xBE\x00\x00\x00\x00\xB9\x00\x00\x00\x00\xF3\xAB\x8B\xC6\xE8\x00\x00\x00\x00\xE8\x00\x00\x00\x00\xB8\x00\x00\x00\x00\x89\x86\x00\x00\x00\x00", "??????xxxxx????xxxxxxxxxxx????xx????????xx????x????xxxxx????x????x????xx????", 0x573D50);
	addrPlaySoundLocal = Hooks::scanPattern("PlaySoundLocal", "\x8B\x54\x24\x08\x56\x50\x8B\x44\x24\x0C\x51\x52\x50\x8B\xCF\x8D\xB7\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x85\xC0\x74\x50\x8B\x57\x2C\x8B\x82\x00\x00\x00\x00\x83\xE8\x01\x8D\x0C\xC0\x03\xC9", "??????xxxxxxxxxxx????x????xxxxxxxxx????xxxxxxxx", 0x4FDFE0);
	addrPlaySoundGlobal = Hooks::scanPattern("addrPlaySoundGlobal", "\x8B\x51\x2C\x8B\x82\x00\x00\x00\x00\x83\xF8\x10\x7C\x05\x33\xC0\xC2\x10\x00\x83\x7A\x08\x00\x74\xF5\x8D\x04\xC0\x56\x8B\x74\x24\x08\x03\xC0\x03\xC0\x89\xB4\x10\x00\x00\x00\x00", "?????????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????", 0x546E20);
	DWORD addrDSSound_vtable = *(DWORD*)(addrConstructDSSound + 0xB);
	DWORD addrLoadGameSoundFromFile = *(DWORD*)(addrDSSound_vtable + 0x30);
	Hooks::polyhook("LoadGameSoundFromFile", addrLoadGameSoundFromFile, (DWORD *) &hookLoadGameSoundFromFile, (DWORD *) &origLoadGameSoundFromFile);

	auto * lua = Lua::getInstance().getState();
	lua->set_function("registerCustomSound", &registerCustomSound);
	lua->set_function("playSoundLocal", &luaPlaySoundLocal);

	return 0;
}
