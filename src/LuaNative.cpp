#include "LuaNative.h"
#include "Lua.h"
#include "WaLibc.h"
#include "MemAccess.h"
#include "entities/CTask.h"
#include "Weapons.h"
#include "CustomWeapons.h"
#include "Hooks.h"
#include <sol/sol.hpp>
#include <Windows.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

template<typename V>
V rd(DWORD addr) {
	V v{};
	if(!jellySafeRead(addr, &v, sizeof(V)))
		return V{};
	return v;
}

template<typename V>
void wr(DWORD addr, V value) {
	jellySafeWrite(addr, &value, sizeof(V));
}

struct Regs {
	DWORD eax = 0, ecx = 0, edx = 0, ebx = 0, esi = 0, edi = 0;
};

// Pushes `n` stack args (args[0] ends up at [esp] on entry, i.e. leftmost),
// loads the register set and calls `target`. ESP is restored from a saved
// copy afterwards, so cdecl, stdcall, thiscall and fastcall callees all work.
DWORD callRawUnsafe(DWORD target, const Regs * r, const DWORD * args, DWORD n) {
	DWORD result = 0;
	DWORD savedEsp = 0;
	DWORD vEax = r->eax, vEcx = r->ecx, vEdx = r->edx, vEbx = r->ebx, vEsi = r->esi, vEdi = r->edi;
	__asm {
		mov savedEsp, esp
		mov ecx, n
		mov esi, args
	pushloop:
		test ecx, ecx
		jz pushdone
		dec ecx
		push dword ptr [esi + ecx * 4]
		jmp pushloop
	pushdone:
		mov eax, vEax
		mov ecx, vEcx
		mov edx, vEdx
		mov ebx, vEbx
		mov esi, vEsi
		mov edi, vEdi
		call target
		mov result, eax
		mov esp, savedEsp
	}
	return result;
}

// Current CallInfo of a Lua thread, or null when no function runs in it.
void * luaCurrentCi(lua_State * L) {
	lua_Debug ar;
	return (L && lua_getstack(L, 0, &ar)) ? (void *)ar.i_ci : nullptr;
}

// Lua call depth of the calling thread and of the main state when a native call starts.
struct LuaMark {
	lua_State * caller = nullptr;
	lua_State * main = nullptr;
	void * callerCi = nullptr;
	void * mainCi = nullptr;
};

LuaMark luaMark(lua_State * caller) {
	LuaMark m;
	m.main = Lua::getInstance().getState()->lua_state();
	m.caller = caller ? caller : m.main;
	m.callerCi = luaCurrentCi(m.caller);
	m.mainCi = luaCurrentCi(m.main);
	return m;
}

// A fault raised while Lua code entered from a game hook is still running must not be
// handled here. Unwinding to this frame skips Lua's protected-call bookkeeping
// (L->errorJmp keeps pointing into the dead frames), so the next Lua error longjmps
// into them and WA dies with STATUS_INVALID_UNWIND_TARGET (crash of 2026-10-10).
int callRawFilter(const LuaMark * m) {
	if(luaCurrentCi(m->caller) != m->callerCi || luaCurrentCi(m->main) != m->mainCi)
		return EXCEPTION_CONTINUE_SEARCH;
	return EXCEPTION_EXECUTE_HANDLER;
}

bool callRaw(DWORD target, const Regs * r, const DWORD * args, DWORD n, DWORD * out, const LuaMark & mark) {
	__try {
		*out = callRawUnsafe(target, r, args, n);
		return true;
	} __except(callRawFilter(&mark)) {
		return false;
	}
}

DWORD toDword(const sol::object & o) {
	switch(o.get_type()) {
		case sol::type::number: {
			lua_State * L = o.lua_state();
			o.push();
			DWORD v;
			if(lua_isinteger(L, -1))
				v = (DWORD)(long long)lua_tointeger(L, -1);
			else
				v = (DWORD)(long long)lua_tonumber(L, -1);
			lua_pop(L, 1);
			return v;
		}
		case sol::type::boolean:
			return o.as<bool>() ? 1 : 0;
		case sol::type::lightuserdata:
			return (DWORD)o.as<void *>();
		case sol::type::string: {
			// Pointer into the Lua string buffer; valid for the duration of the call.
			lua_State * L = o.lua_state();
			o.push();
			DWORD v = (DWORD)lua_tostring(L, -1);
			lua_pop(L, 1);
			return v;
		}
		case sol::type::userdata:
			// Task usertypes all derive from CTask with single inheritance,
			// so the base pointer equals the object address.
			if(o.is<CTask *>())
				return (DWORD)o.as<CTask *>();
			// Weapon structures handed to Lua callbacks (fireWeapon etc.).
			if(o.is<CustomWeapons::WeaponStruct *>())
				return (DWORD)o.as<CustomWeapons::WeaponStruct *>();
			if(o.is<Weapons::WeaponLaunchParams *>())
				return (DWORD)o.as<Weapons::WeaponLaunchParams *>();
			if(o.is<Weapons::WeaponProjectileParams *>())
				return (DWORD)o.as<Weapons::WeaponProjectileParams *>();
			return 0;
		default:
			return 0;
	}
}

DWORD invoke(const char * name, DWORD target, const Regs & regs, sol::variadic_args va, size_t skip) {
	if(!target || !jellyIsMemReadable(target, 1))
		throw std::runtime_error(std::string(name) + ": bad target address");
	std::vector<DWORD> args;
	for(size_t i = skip; i < va.size(); i++)
		args.push_back(toDword(va[i]));
	DWORD out = 0;
	if(!callRaw(target, &regs, args.data(), (DWORD)args.size(), &out, luaMark(va.lua_state()))) {
		char buff[96];
		_snprintf_s(buff, _TRUNCATE, "%s: access violation in 0x%08X", name, target);
		throw std::runtime_error(buff);
	}
	return out;
}

std::string readString(DWORD addr, int maxLen) {
	std::string s;
	if(maxLen <= 0)
		maxLen = 4096;
	char buf[256];
	size_t done = 0, limit = (size_t)maxLen;
	while(done < limit) {
		size_t want = limit - done < sizeof(buf) ? limit - done : sizeof(buf);
		size_t got = jellySafeStrCopy(addr + done, buf, want);
		s.append(buf, got);
		done += got;
		if(got < want)
			break; // terminator or unreadable byte
	}
	return s;
}

// ---- generic function hooks -------------------------------------------------
// hookFunction(name, addr, {regs={"ecx",..}, nstack=N, cleanup=bytes}, fn)
// detours any WA function to a Lua callback. Each hook gets a small thunk:
//   pushad; mov eax,esp; lea edx,[esp+0x24]; push edx; push eax; push hook;
//   call luaHookDispatch; mov [esp+0x1c],eax; popad; ret cleanup
// The callback receives the register args (in `regs` order) followed by the
// stack args and returns the value for EAX. callHookOriginal(id, ...) takes
// args in the same layout; registers not named in `regs` keep their values
// from hook entry, so loosely understood usercall functions stay intact.

struct PushadRegs {
	DWORD edi, esi, ebp, esp, ebx, edx, ecx, eax;
};

enum RegId { R_EAX, R_ECX, R_EDX, R_EBX, R_ESI, R_EDI };

struct LuaHook {
	int id = 0;
	std::string name;
	DWORD target = 0;
	DWORD trampoline = 0;
	std::vector<RegId> regArgs;
	int nstack = 0;
	int cleanup = 0;
	sol::protected_function fn;
	bool enabled = true;
	int errors = 0;
	std::string lastError;
	unsigned char * thunk = nullptr;
};

struct HookFrame {
	LuaHook * hook;
	PushadRegs * regs;
	DWORD * stack;
	bool calledOriginal;
	DWORD originalResult;
};

std::vector<LuaHook *> g_hooks;
std::vector<HookFrame> g_frames;

DWORD & regRef(PushadRegs * p, RegId r) {
	switch(r) {
		case R_EAX: return p->eax;
		case R_ECX: return p->ecx;
		case R_EDX: return p->edx;
		case R_EBX: return p->ebx;
		case R_ESI: return p->esi;
		default: return p->edi;
	}
}

DWORD & regRef(Regs & r, RegId id) {
	switch(id) {
		case R_EAX: return r.eax;
		case R_ECX: return r.ecx;
		case R_EDX: return r.edx;
		case R_EBX: return r.ebx;
		case R_ESI: return r.esi;
		default: return r.edi;
	}
}

Regs regsFromPushad(const PushadRegs * p) {
	Regs r;
	r.eax = p->eax; r.ecx = p->ecx; r.edx = p->edx;
	r.ebx = p->ebx; r.esi = p->esi; r.edi = p->edi;
	return r;
}

DWORD callHookOriginalRaw(LuaHook * h, Regs & regs, const DWORD * stack, lua_State * caller) {
	DWORD out = 0;
	if(!callRaw(h->trampoline, &regs, stack, (DWORD)h->nstack, &out, luaMark(caller))) {
		char buff[128];
		_snprintf_s(buff, _TRUNCATE, "hook %s: access violation in original 0x%08X", h->name.c_str(), h->target);
		throw std::runtime_error(buff);
	}
	return out;
}

DWORD __stdcall luaHookDispatch(LuaHook * h, PushadRegs * regs, DWORD * stack) {
	g_frames.push_back({h, regs, stack, false, 0});
	size_t fi = g_frames.size() - 1;
	DWORD result = 0;
	bool done = false;
	if(h->enabled && h->fn.valid()) {
		std::vector<long long> args;
		args.reserve(h->regArgs.size() + h->nstack);
		for(RegId r : h->regArgs)
			args.push_back((long long)regRef(regs, r));
		for(int i = 0; i < h->nstack; i++)
			args.push_back((long long)stack[i]);
		try {
			sol::protected_function_result res = h->fn(sol::as_args(args));
			if(res.valid()) {
				done = true;
				sol::object o = res.return_count() > 0 ? res.get<sol::object>(0) : sol::object();
				// No return value after callHookOriginal passes the original's EAX through.
				if(o.valid() && o.get_type() != sol::type::lua_nil)
					result = toDword(o);
				else if(g_frames[fi].calledOriginal)
					result = g_frames[fi].originalResult;
			} else {
				sol::error err = res;
				h->lastError = err.what();
			}
		} catch(std::exception & e) {
			h->lastError = e.what();
		}
		if(!done) {
			h->errors++;
			printf("luahook %s error #%d: %s\n", h->name.c_str(), h->errors, h->lastError.c_str());
			if(h->errors >= 10)
				h->enabled = false;
		}
	}
	if(!done) {
		if(g_frames[fi].calledOriginal) {
			result = g_frames[fi].originalResult;
		} else {
			Regs r = regsFromPushad(regs);
			try {
				result = callHookOriginalRaw(h, r, stack, nullptr);
			} catch(std::exception & e) {
				printf("%s\n", e.what());
			}
		}
	}
	g_frames.pop_back();
	return result;
}

unsigned char * buildThunk(LuaHook * h) {
	auto * t = (unsigned char *)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if(!t)
		throw std::runtime_error("hookFunction: VirtualAlloc failed");
	int i = 0;
	auto put = [&](unsigned char b) { t[i++] = b; };
	auto put32 = [&](DWORD v) { memcpy(t + i, &v, 4); i += 4; };
	put(0x60);                                  // pushad
	put(0x89); put(0xE0);                       // mov eax, esp
	put(0x8D); put(0x54); put(0x24); put(0x24); // lea edx, [esp+0x24]
	put(0x52);                                  // push edx
	put(0x50);                                  // push eax
	put(0x68); put32((DWORD)h);                 // push hook
	put(0xE8);                                  // call luaHookDispatch
	put32((DWORD)&luaHookDispatch - ((DWORD)t + i + 4));
	put(0x89); put(0x44); put(0x24); put(0x1C); // mov [esp+0x1c], eax
	put(0x61);                                  // popad
	if(h->cleanup) {
		put(0xC2); put((unsigned char)(h->cleanup & 0xFF)); put((unsigned char)(h->cleanup >> 8));
	} else {
		put(0xC3);
	}
	FlushInstructionCache(GetCurrentProcess(), t, 64);
	return t;
}

RegId parseReg(const std::string & s) {
	if(s == "eax") return R_EAX;
	if(s == "ecx") return R_ECX;
	if(s == "edx") return R_EDX;
	if(s == "ebx") return R_EBX;
	if(s == "esi") return R_ESI;
	if(s == "edi") return R_EDI;
	throw std::runtime_error("hookFunction: unknown register " + s);
}

LuaHook * hookById(int id) {
	if(id <= 0 || id > (int)g_hooks.size())
		throw std::runtime_error("bad hook id");
	return g_hooks[id - 1];
}

int hookFunction(const std::string & name, DWORD addr, sol::table spec, sol::protected_function fn) {
	for(LuaHook * h : g_hooks) {
		if(h->name == name) {
			if(h->target != addr)
				throw std::runtime_error("hookFunction: name " + name + " reused for another address");
			h->fn = fn;
			h->enabled = true;
			h->errors = 0;
			return h->id;
		}
	}
	if(!addr || !jellyIsMemReadable(addr, 5))
		throw std::runtime_error("hookFunction: bad address for " + name);
	auto * h = new LuaHook();
	h->name = name;
	h->target = addr;
	sol::optional<sol::table> regs = spec["regs"];
	if(regs) {
		for(size_t i = 1; i <= regs->size(); i++)
			h->regArgs.push_back(parseReg((*regs)[i].get<std::string>()));
	}
	h->nstack = spec.get_or("nstack", 0);
	if(h->nstack < 0 || h->nstack > 32)
		throw std::runtime_error("hookFunction: nstack out of range");
	bool isCdecl = spec.get_or("cdecl", false);
	h->cleanup = spec.get_or("cleanup", isCdecl ? 0 : h->nstack * 4);
	h->fn = fn;
	h->thunk = buildThunk(h);
	try {
		Hooks::minhook("lua:" + name, addr, (DWORD *)h->thunk, &h->trampoline);
	} catch(...) {
		VirtualFree(h->thunk, 0, MEM_RELEASE);
		delete h;
		throw;
	}
	g_hooks.push_back(h);
	h->id = (int)g_hooks.size();
	return h->id;
}

long long callHookOriginal(int id, sol::variadic_args va) {
	LuaHook * h = hookById(id);
	// Index, not pointer: nested hooks fired by the original may grow g_frames.
	int fi = -1;
	for(int i = (int)g_frames.size() - 1; i >= 0; i--) {
		if(g_frames[i].hook == h) {
			fi = i;
			break;
		}
	}
	HookFrame * frame = fi >= 0 ? &g_frames[fi] : nullptr;
	Regs r = frame ? regsFromPushad(frame->regs) : Regs();
	size_t k = 0;
	for(RegId reg : h->regArgs) {
		if(k < va.size())
			regRef(r, reg) = toDword(va[k]);
		k++;
	}
	std::vector<DWORD> stack(h->nstack ? h->nstack : 1, 0);
	for(int i = 0; i < h->nstack; i++, k++) {
		if(k < va.size() && va[k].get_type() != sol::type::none)
			stack[i] = toDword(va[k]);
		else if(frame)
			stack[i] = frame->stack[i];
	}
	DWORD out = callHookOriginalRaw(h, r, stack.data(), va.lua_state());
	if(fi >= 0) {
		g_frames[fi].calledOriginal = true;
		g_frames[fi].originalResult = out;
	}
	return (long long)out;
}

} // namespace

void LuaNative::install() {
	auto * lua = Lua::getInstance().getState();

	lua->set_function("readMemU8", [](DWORD a) { return (int)rd<unsigned char>(a); });
	lua->set_function("readMemS8", [](DWORD a) { return (int)rd<signed char>(a); });
	lua->set_function("readMemU16", [](DWORD a) { return (int)rd<unsigned short>(a); });
	lua->set_function("readMemS16", [](DWORD a) { return (int)rd<short>(a); });
	lua->set_function("readMemU32", [](DWORD a) { return (long long)rd<DWORD>(a); });
	lua->set_function("readMemFloat", [](DWORD a) { return (double)rd<float>(a); });
	lua->set_function("readMemDouble", [](DWORD a) { return rd<double>(a); });
	lua->set_function("writeMemU8", [](DWORD a, long long v) { wr<unsigned char>(a, (unsigned char)v); });
	lua->set_function("writeMemU16", [](DWORD a, long long v) { wr<unsigned short>(a, (unsigned short)v); });
	lua->set_function("writeMemU32", [](DWORD a, long long v) { wr<DWORD>(a, (DWORD)v); });
	lua->set_function("writeMemFloat", [](DWORD a, double v) { wr<float>(a, (float)v); });
	lua->set_function("writeMemDouble", [](DWORD a, double v) { wr<double>(a, v); });

	lua->set_function("readMemString", [](DWORD a, sol::optional<int> maxLen) { return readString(a, maxLen.value_or(4096)); });
	lua->set_function("writeMemString", [](DWORD a, const std::string & s, sol::optional<int> maxLen) {
		size_t n = s.size() + 1;
		if(maxLen && *maxLen > 0 && n > (size_t)*maxLen)
			n = (size_t)*maxLen;
		if(!a || !jellyIsMemWritable(a, n))
			return false;
		memcpy((void *)a, s.c_str(), n);
		((char *)a)[n - 1] = 0;
		return true;
	});
	lua->set_function("readMemBytes", [](DWORD a, int n) {
		if(n <= 0 || !a || !jellyIsMemReadable(a, n))
			return std::string();
		return std::string((const char *)a, (size_t)n);
	});
	lua->set_function("writeMemBytes", [](DWORD a, const std::string & s) {
		if(!a || s.empty() || !jellyIsMemWritable(a, s.size()))
			return false;
		memcpy((void *)a, s.data(), s.size());
		return true;
	});
	lua->set_function("memCopy", [](DWORD dst, DWORD src, int n) {
		if(n <= 0 || !jellyIsMemReadable(src, n) || !jellyIsMemWritable(dst, n))
			return false;
		memmove((void *)dst, (const void *)src, (size_t)n);
		return true;
	});
	lua->set_function("memFill", [](DWORD dst, int byte, int n) {
		if(n <= 0 || !jellyIsMemWritable(dst, n))
			return false;
		memset((void *)dst, byte, (size_t)n);
		return true;
	});
	// WA heap, so memory handed to game code may be freed by the game itself.
	lua->set_function("memAlloc", [](int size) {
		if(size <= 0)
			return (DWORD)0;
		void * p = WaLibc::waMalloc((size_t)size);
		if(p)
			memset(p, 0, (size_t)size);
		return (DWORD)p;
	});
	lua->set_function("memFree", [](DWORD a) {
		if(a)
			WaLibc::waFree((void *)a);
	});
	lua->set_function("isMemReadable", [](DWORD a, sol::optional<int> n) { return a && jellyIsMemReadable(a, n.value_or(4)); });

	lua->set_function("ptrToInt", [](sol::object o) { return toDword(o); });
	lua->set_function("intToPtr", [](DWORD a) { return (void *)a; });
	lua->set_function("floatBits", [](double f) { float x = (float)f; DWORD d; memcpy(&d, &x, 4); return (long long)d; });
	lua->set_function("bitsFloat", [](long long v) { DWORD d = (DWORD)v; float x; memcpy(&x, &d, 4); return (double)x; });

	// callStdcall(addr, ...)       stdcall or cdecl
	// callThiscall(addr, this, ...)
	// callFastcall(addr, ecx, edx, ...)
	// callRegs(addr, {eax=,ecx=,edx=,ebx=,esi=,edi=}, ...)   WA usercall helpers
	// Stack args are DWORDs: numbers, booleans, nil, lightuserdata, strings
	// (pointer to the Lua buffer) or task usertypes (the task address).
	lua->set_function("callStdcall", [](DWORD target, sol::variadic_args va) {
		Regs r;
		return (long long)invoke("callStdcall", target, r, va, 0);
	});
	lua->set_function("callThiscall", [](DWORD target, sol::variadic_args va) {
		Regs r;
		if(va.size() > 0)
			r.ecx = toDword(va[0]);
		return (long long)invoke("callThiscall", target, r, va, 1);
	});
	lua->set_function("callFastcall", [](DWORD target, sol::variadic_args va) {
		Regs r;
		if(va.size() > 0)
			r.ecx = toDword(va[0]);
		if(va.size() > 1)
			r.edx = toDword(va[1]);
		return (long long)invoke("callFastcall", target, r, va, 2);
	});
	lua->set_function("callRegs", [](DWORD target, sol::variadic_args va) {
		Regs r;
		if(va.size() > 0 && va[0].get_type() == sol::type::table) {
			sol::table t = va[0];
			r.eax = toDword(t["eax"]);
			r.ecx = toDword(t["ecx"]);
			r.edx = toDword(t["edx"]);
			r.ebx = toDword(t["ebx"]);
			r.esi = toDword(t["esi"]);
			r.edi = toDword(t["edi"]);
		}
		return (long long)invoke("callRegs", target, r, va, 1);
	});

	lua->set_function("hookFunction", &hookFunction);
	lua->set_function("callHookOriginal", &callHookOriginal);
	lua->set_function("hookTrampoline", [](int id) { return (long long)hookById(id)->trampoline; });
	lua->set_function("setHookEnabled", [](int id, bool on) { LuaHook * h = hookById(id); h->enabled = on; if(on) h->errors = 0; });
	lua->set_function("getHookLastError", [](int id) { return hookById(id)->lastError; });
	lua->set_function("getHookErrorCount", [](int id) { return hookById(id)->errors; });
	// Entry registers of the innermost active hook frame (debugging usercall signatures).
	lua->set_function("hookCurrentRegs", [](sol::this_state s) {
		sol::state_view lua(s);
		sol::table t = lua.create_table();
		if(g_frames.empty())
			return t;
		PushadRegs * p = g_frames.back().regs;
		t["eax"] = (long long)p->eax; t["ecx"] = (long long)p->ecx; t["edx"] = (long long)p->edx;
		t["ebx"] = (long long)p->ebx; t["esi"] = (long long)p->esi; t["edi"] = (long long)p->edi;
		t["ebp"] = (long long)p->ebp; t["esp"] = (long long)p->esp;
		return t;
	});
}
