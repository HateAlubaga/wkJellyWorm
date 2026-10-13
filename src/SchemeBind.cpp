#include "SchemeBind.h"
#include "Config.h"
#include "Game.h"
#include "Hooks.h"
#include "Lobby.h"
#include "Lua.h"
#include "MemAccess.h"
#include "packages/PackageManager.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <sol/sol.hpp>

namespace fs = std::filesystem;

namespace {
	const char *kSectionModules = "scheme_modules";
	const char *kLogFile = "jelly\\scheme_hook.log";

	// Layout of WA's global scheme struct (from wkTerrainSync):
	//   +0x08 builtin scheme id (4 = custom scheme)
	//   +0x0C CString: char* to the scheme name
	//   +0x14 raw scheme bytes (the .wsc bytes after the 5 byte "SCHM"+version header)
	constexpr DWORD kStructBuiltinOff = 0x08;
	constexpr DWORD kStructNameOff = 0x0C;
	constexpr DWORD kStructDataOff = 0x14;
	// First 0x129-5 bytes = settings block + weapon table, the part every scheme version has.
	constexpr size_t kHashedBytes = 0x129 - 5;

	// WA's scheme selection handlers (offline lobby, host lobby, third lobby): each reads the combo
	// index, then calls SetBuiltinScheme(struct, id) or, for a file (index 1000), SetWscScheme.
	// SetBuiltinScheme has 23 callers in 3.8.1, most of them internal resets, so a built-in
	// scheme only counts as a user selection when it comes from one of these call sites.
	// The addresses are for the exe at its preferred base 0x400000. WA.exe is relocated (ASLR), so a
	// runtime return address has to be rebased with g_aslrDelta before it is compared with them.
	constexpr DWORD kBuiltinUiCallers[] = {0x4A1B56, 0x4B491A, 0x4CC239};
	constexpr DWORD kPreferredBase = 0x400000;
	constexpr DWORD kStaticSetBuiltinScheme = 0x4D4840;
	DWORD g_aslrDelta = 0;

	DWORD g_schemeStruct = 0;
	bool g_hooked = false;
	int g_enabled = 1;

	int (__stdcall *origSetWscScheme)(DWORD schemeStruct, char *path, char flag, bool *out) = nullptr;
	int (__stdcall *origSetBuiltinScheme)(DWORD schemeStruct, int id) = nullptr;

	struct Selection {
		bool valid = false;
		std::string source; // "wsc" | "builtin" | "screen"
		std::string name;   // file stem, or "builtinN"
		std::string path;
		std::string structName;
		uint32_t hash = 0;
		bool hashValid = false;
		int builtinId = -1;
	};
	Selection g_last;

	// State of the last apply, so the same scheme is not re-applied on every call.
	bool g_appliedValid = false;
	std::string g_appliedKey;
	// Packages this module enabled. Persists across config resets because the
	// session config restore can bring them back as ordinary explicit packages.
	std::set<std::string> g_schemeAdded;

	void slog(const char *fmt, ...) {
		char buf[1536];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		printf("SchemeBind: %s\n", buf);
		std::error_code ec;
		fs::create_directories(Config::jellyDir, ec);
		std::ofstream out(kLogFile, std::ios::app);
		if(out.good())
			out << "[" << GetTickCount() << " screen=" << Lobby::getFrontendScreen() << "] " << buf << "\n";
	}

	std::string toLower(std::string s) {
		for(auto &c : s)
			c = (char)tolower((unsigned char)c);
		return s;
	}

	std::string trim(const std::string &s) {
		size_t b = 0, e = s.size();
		while(b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
		while(e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
		return s.substr(b, e - b);
	}

	std::string hex8(uint32_t v) {
		char b[16];
		snprintf(b, sizeof(b), "%08X", v);
		return b;
	}

	// "User\Schemes\Foo.wsc" -> "Foo"
	std::string stemOf(const std::string &path) {
		size_t slash = path.find_last_of("\\/");
		std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
		size_t dot = name.rfind('.');
		if(dot != std::string::npos && toLower(name.substr(dot)) == ".wsc")
			name.erase(dot);
		return name;
	}

	// "{{05}} Pro" -> "Pro" (Data\Schemes uses a numeric sort prefix)
	std::string stripPrefix(const std::string &name) {
		if(name.rfind("{{", 0) == 0) {
			size_t end = name.find("}}");
			if(end != std::string::npos)
				return trim(name.substr(end + 2));
		}
		return name;
	}

	uint32_t fnv1a(const unsigned char *p, size_t n) {
		uint32_t h = 2166136261u;
		for(size_t i = 0; i < n; ++i) {
			h ^= p[i];
			h *= 16777619u;
		}
		return h;
	}

	bool coreHash(uint32_t &out) {
		if(!g_schemeStruct || !jellyProbeRead(g_schemeStruct + kStructDataOff, kHashedBytes))
			return false;
		out = fnv1a((const unsigned char *)(g_schemeStruct + kStructDataOff), kHashedBytes);
		return true;
	}

	std::string readStructName() {
		if(!g_schemeStruct || !jellyProbeRead(g_schemeStruct + kStructNameOff, 4))
			return "";
		DWORD p = *(DWORD *)(g_schemeStruct + kStructNameOff);
		if(p < 0x10000 || !jellyProbeRead(p, 1))
			return "";
		std::string s;
		for(int i = 0; i < 128; ++i) {
			if(!jellyProbeRead(p + i, 1))
				break;
			char c = *(char *)(p + i);
			if(!c)
				break;
			if((unsigned char)c < 0x20)
				return "";
			s += c;
		}
		return s;
	}

	int structBuiltinId() {
		if(!g_schemeStruct || !jellyProbeRead(g_schemeStruct + kStructBuiltinOff, 4))
			return -1;
		return *(int *)(g_schemeStruct + kStructBuiltinOff);
	}

	std::vector<std::pair<std::string, std::string>> readSection(const char *section) {
		std::vector<std::pair<std::string, std::string>> result;
		std::vector<char> buf(131072, 0);
		DWORD n = GetPrivateProfileSectionA(section, buf.data(), (DWORD)buf.size(), Config::iniFile.c_str());
		if(n == 0 || n >= buf.size() - 2)
			return result;
		const char *p = buf.data();
		while(*p) {
			std::string line(p);
			p += line.size() + 1;
			if(line.empty() || line[0] == ';')
				continue;
			size_t eq = line.find('=');
			if(eq == std::string::npos)
				continue;
			result.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
		}
		return result;
	}

	// "a, b=1.2.3 c" -> [(a,""), (b,"1.2.3"), (c,"")]
	std::vector<std::pair<std::string, std::string>> parseModules(const std::string &value) {
		std::vector<std::pair<std::string, std::string>> items;
		std::string cur;
		auto flush = [&]() {
			std::string t = trim(cur);
			cur.clear();
			if(t.empty())
				return;
			size_t eq = t.find('=');
			if(eq == std::string::npos)
				items.emplace_back(t, "");
			else
				items.emplace_back(trim(t.substr(0, eq)), trim(t.substr(eq + 1)));
		};
		std::string v = value;  // an inline "; comment" is not part of the module list
		const size_t comment = v.find(';');
		if(comment != std::string::npos)
			v.erase(comment);
		for(char c : v) {
			if(c == ',' || c == ' ' || c == '\t')
				flush();
			else
				cur += c;
		}
		flush();
		return items;
	}

	// Name match wins over hash match.
	bool findBinding(const Selection &sel, std::string &keyOut, std::string &valueOut) {
		const auto entries = readSection(kSectionModules);
		std::vector<std::string> nameKeys;
		if(!sel.name.empty()) {
			nameKeys.push_back(toLower(sel.name));
			nameKeys.push_back(toLower(stripPrefix(sel.name)));
		}
		if(!sel.structName.empty())
			nameKeys.push_back(toLower(sel.structName));
		if(sel.builtinId >= 0)
			nameKeys.push_back("#" + std::to_string(sel.builtinId));
		for(auto &kv : entries) {
			std::string k = toLower(kv.first);
			if(std::find(nameKeys.begin(), nameKeys.end(), k) != nameKeys.end()) {
				keyOut = kv.first;
				valueOut = kv.second;
				return true;
			}
		}
		if(sel.hashValid) {
			const std::string hk = "hash:" + toLower(hex8(sel.hash));
			for(auto &kv : entries) {
				if(toLower(kv.first) == hk) {
					keyOut = kv.first;
					valueOut = kv.second;
					return true;
				}
			}
		}
		return false;
	}

	void notifyLobby(const std::string &msg) {
		if(Lobby::getFrontendScreen() == 1704) {
			void *host = Lobby::getHostObj();
			if(host)
				Lobby::showGreenText(host, "scheme: " + msg);
		}
	}

	void syncToGuests() {
		if(Lobby::getFrontendScreen() != 1704)
			return;
		void *host = Lobby::getHostObj();
		if(host)
			Lobby::sendConfigString(host);
	}

	std::string describe(const Selection &s) {
		std::stringstream ss;
		ss << "source=" << s.source << " name='" << s.name << "' structName='" << s.structName << "'";
		if(s.hashValid)
			ss << " hash=" << hex8(s.hash);
		if(s.builtinId >= 0)
			ss << " builtinId=" << s.builtinId;
		return ss.str();
	}

	std::string addedFile() {
		return Config::jellyDir + "\\scheme_added.txt";
	}

	void loadAdded() {
		std::ifstream in(addedFile());
		std::string line;
		while(std::getline(in, line)) {
			line = trim(line);
			if(!line.empty())
				g_schemeAdded.insert(line);
		}
	}

	void saveAdded() {
		std::error_code ec;
		fs::create_directories(Config::jellyDir, ec);
		std::ofstream out(addedFile(), std::ios::trunc);
		for(auto &n : g_schemeAdded)
			out << n << "\n";
	}

	// Enable the modules bound to `sel`, disable the ones the previous scheme brought in.
	void applyFor(const Selection &sel, const char *why) {
		if(!g_enabled)
			return;
		std::string key, value;
		const bool bound = findBinding(sel, key, value);
		const std::string state = bound ? (key + "=" + value) : std::string();
		if(g_appliedValid && state == g_appliedKey) {
			slog("apply(%s): unchanged (%s)", why, bound ? key.c_str() : "no binding");
			return;
		}

		auto &pm = PackageManager::getInstance();
		std::set<std::string> keep;
		const auto autostart = PackageManager::autostartEnabledNames();
		std::stringstream report;
		int enabled = 0, failed = 0, removed = 0;

		if(bound) {
			for(auto &item : parseModules(value)) {
				std::string name = pm.resolveModuleName(item.first);
				keep.insert(name);
				const bool already = pm.isExplicitlyEnabled(name);
				if(already) {
					// Enabled by the user/autostart, or restored from the session. Keep tracking
					// it only if this module brought it in earlier.
					continue;
				}
				std::string err = pm.enableForScheme(name, item.second);
				if(err.empty()) {
					g_schemeAdded.insert(name);
					++enabled;
					report << " +" << name;
				} else {
					++failed;
					report << " !" << name << "(" << err << ")";
					slog("apply(%s): enable %s failed: %s", why, name.c_str(), err.c_str());
				}
			}
		}

		for(auto it = g_schemeAdded.begin(); it != g_schemeAdded.end();) {
			if(keep.count(*it)) {
				++it;
				continue;
			}
			if(pm.isExplicitlyEnabled(*it) && !autostart.count(*it)) {
				std::string err = pm.disableForScheme(*it);
				if(err.empty()) {
					++removed;
					report << " -" << *it;
				} else {
					slog("apply(%s): disable %s failed: %s", why, it->c_str(), err.c_str());
				}
			}
			it = g_schemeAdded.erase(it);
		}

		g_appliedValid = true;
		g_appliedKey = state;
		saveAdded();
		slog("apply(%s): %s -> %s: enabled=%d removed=%d failed=%d%s", why, describe(sel).c_str(),
		     bound ? (key + "=" + value).c_str() : "no binding", enabled, removed, failed, report.str().c_str());
		if(enabled || removed || failed) {
			std::string label = sel.name.empty() ? ("hash " + hex8(sel.hash)) : sel.name;
			notifyLobby("'" + label + "' modules:" + report.str());
			syncToGuests();
		}
	}

	bool screenAllowsApply(int screen) {
		// 11 local multiplayer, 21 single player, 1704 network host lobby. A client
		// (1706) gets the host's config string instead.
		return screen == 11 || screen == 21 || screen == 1704;
	}

	void onSelected(const char *source, const char *path, int builtinId, DWORD caller) {
		if(builtinId >= 0) {
			const DWORD staticCaller = caller - g_aslrDelta;
			const bool ui = std::find(std::begin(kBuiltinUiCallers), std::end(kBuiltinUiCallers), staticCaller) !=
			                std::end(kBuiltinUiCallers);
			if(!ui) {
				slog("SetBuiltinScheme(%d) from internal caller 0x%X (static 0x%X), ignored", builtinId, caller, staticCaller);
				return;
			}
		}
		Selection s;
		s.valid = true;
		s.source = source;
		s.path = path ? path : "";
		s.builtinId = builtinId;
		if(builtinId >= 0)
			s.name = "builtin" + std::to_string(builtinId);
		else
			s.name = stemOf(s.path);
		s.structName = readStructName();
		s.hashValid = coreHash(s.hash);
		g_last = s;

		const int screen = Lobby::getFrontendScreen();
		const bool inMatch = Game::getAddrDDGame() != 0;
		slog("selected: %s path='%s' structBuiltin=%d inMatch=%d caller=0x%X", describe(s).c_str(), s.path.c_str(),
		     structBuiltinId(), inMatch ? 1 : 0, caller);
		if(inMatch || !screenAllowsApply(screen)) {
			slog("not applying (inMatch=%d screen=%d)", inMatch ? 1 : 0, screen);
			return;
		}
		try {
			applyFor(s, "select");
		} catch(const std::exception &e) {
			slog("apply failed: %s", e.what());
		}
	}

	int __stdcall hookSetWscScheme(DWORD schemeStruct, char *path, char flag, bool *out) {
		const DWORD caller = (DWORD)_ReturnAddress();
		int ret = origSetWscScheme(schemeStruct, path, flag, out);
		try {
			onSelected("wsc", path, -1, caller);
		} catch(...) {
		}
		return ret;
	}

	int __stdcall hookSetBuiltinScheme(DWORD schemeStruct, int id) {
		const DWORD caller = (DWORD)_ReturnAddress();
		int ret = origSetBuiltinScheme(schemeStruct, id);
		try {
			onSelected("builtin", nullptr, id, caller);
		} catch(...) {
		}
		return ret;
	}

	std::string currentValueFromPackages() {
		auto &pm = PackageManager::getInstance();
		const auto autostart = PackageManager::autostartEnabledNames();
		std::string value;
		for(auto &name : pm.listExplicitPackageNames()) {
			if(autostart.count(name))
				continue;
			if(!value.empty())
				value += ",";
			value += name;
		}
		return value;
	}

	// The scheme WA holds right now: last hook call if the struct still matches it, else by hash.
	Selection currentSelection() {
		Selection s = g_last;
		uint32_t h = 0;
		const bool ok = coreHash(h);
		if(!s.valid || (ok && s.hashValid && s.hash != h)) {
			Selection fresh;
			fresh.valid = ok;
			fresh.source = "screen";
			fresh.structName = readStructName();
			fresh.hashValid = ok;
			fresh.hash = h;
			return fresh;
		}
		return s;
	}

	const char *kHelp =
		"/scheme - status of the scheme binding\n"
		"/scheme bind [pkg,pkg=ver,...] - bind modules to the selected scheme by name (no list = modules you enabled in the lobby)\n"
		"/scheme bindhash [pkg,...] - same, but by the hash of the scheme settings\n"
		"/scheme unbind - remove the binding of the selected scheme\n"
		"/scheme list - all bindings\n"
		"/scheme apply - apply the binding now\n"
		"Log: jelly\\scheme_hook.log. Bindings live in [scheme_modules] of wkJellyWorm.ini";
}

int SchemeBind::install(SignatureScanner &, module) {
	g_enabled = GetPrivateProfileIntA("scheme_bind", "Enabled", 1, Config::iniFile.c_str());
	slog("install: Enabled=%d", g_enabled);
	loadAdded();

	auto *lua = Lua::getInstance().getState();
	lua->set_function("getSchemeName", []() { return currentSelection().name; });
	lua->set_function("getSchemeHash", []() { return currentSelection().hashValid ? hex8(currentSelection().hash) : std::string(); });
	lua->set_function("schemeCommand", &SchemeBind::handleCommand);

	try {
		// Patterns from nizikawa-worms/wkTerrainSync (src/Scheme.cpp), WA 3.8.1.
		DWORD addrGetSchemeSettingsFromWam = Hooks::scanPattern("GetSchemeSettingsFromWam", "\x57\x6A\x04\x68\x00\x00\x00\x00\x8B\xF8\xE8\x00\x00\x00\x00\x83\xF8\xFF\x75\x04\x0B\xC0\x5F\xC3\x8B\x47\x0C\x56\x8B\x35\x00\x00\x00\x00\x50\x6A\x00\x68\x00\x00\x00\x00\x68\x00\x00\x00\x00\xFF\xD6", "????????xxx????xxxxxxxxxxxxxxx????xxxx????x????xx");
		DWORD addrSetWscScheme = Hooks::scanPattern("SetWscScheme", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x55\x8B\xAC\x24\x00\x00\x00\x00\x56\x8B\xB4\x24\x00\x00\x00\x00\x57\x8D\x4C\x24\x20", "???????xx????xxxx????xx????xxxxx????xxxx????xxxxx");
		g_schemeStruct = *(DWORD *)(addrGetSchemeSettingsFromWam + 0x4);
		DWORD addrSetBuiltinScheme = addrGetSchemeSettingsFromWam + 0xF + *(DWORD *)(addrGetSchemeSettingsFromWam + 0xB);
		g_aslrDelta = (DWORD)GetModuleHandleA(nullptr) - kPreferredBase;
		slog("install: schemeStruct=0x%X SetWscScheme=0x%X SetBuiltinScheme=0x%X aslrDelta=0x%X", g_schemeStruct,
		     addrSetWscScheme, addrSetBuiltinScheme, g_aslrDelta);
		if(addrSetBuiltinScheme - g_aslrDelta != kStaticSetBuiltinScheme)
			slog("install: WARNING SetBuiltinScheme static address is 0x%X, expected 0x%X: built-in scheme picks may be ignored",
			     addrSetBuiltinScheme - g_aslrDelta, kStaticSetBuiltinScheme);
		Hooks::polyhook("SetWscScheme", addrSetWscScheme, (DWORD *)&hookSetWscScheme, (DWORD *)&origSetWscScheme);
		Hooks::polyhook("SetBuiltinScheme", addrSetBuiltinScheme, (DWORD *)&hookSetBuiltinScheme, (DWORD *)&origSetBuiltinScheme);
		g_hooked = true;
	} catch(const std::exception &e) {
		// The scheme binding is optional: a pattern miss must not stop the whole module.
		slog("install FAILED, scheme binding is off: %s", e.what());
		return 1;
	}
	return 0;
}

void SchemeBind::onConfigReset() {
	g_appliedValid = false;
	g_appliedKey.clear();
}

void SchemeBind::onScreenEnter(int screen) {
	if(!g_hooked || !g_enabled || !screenAllowsApply(screen))
		return;
	try {
		Selection s = currentSelection();
		slog("screen %d opened, current scheme: %s", screen, describe(s).c_str());
		if(s.valid)
			applyFor(s, "screen");
	} catch(const std::exception &e) {
		slog("onScreenEnter failed: %s", e.what());
	}
}

std::string SchemeBind::handleCommand(const std::string &argsIn) {
	std::string args = trim(argsIn);
	std::string cmd = args, rest;
	size_t sp = args.find_first_of(" \t");
	if(sp != std::string::npos) {
		cmd = args.substr(0, sp);
		rest = trim(args.substr(sp + 1));
	}
	cmd = toLower(cmd);
	const std::string ini = Config::iniFile;

	if(!g_hooked && cmd != "help")
		return "scheme hook is not installed (see jelly\\scheme_hook.log)";
	try {
		if(cmd == "help")
			return kHelp;
		const Selection s = currentSelection();
		if(cmd.empty() || cmd == "status") {
			std::string key, value;
			const bool bound = s.valid && findBinding(s, key, value);
			std::stringstream ss;
			ss << "scheme: " << (s.valid ? describe(s) : "unknown (pick a scheme in the lobby)") << "\n";
			ss << "binding: " << (bound ? key + " = " + value : "none") << "\n";
			ss << "enabled by scheme:";
			for(auto &n : g_schemeAdded)
				ss << " " << n;
			if(g_schemeAdded.empty())
				ss << " -";
			return ss.str();
		}
		if(cmd == "list") {
			std::stringstream ss;
			const auto entries = readSection(kSectionModules);
			for(auto &kv : entries)
				ss << kv.first << " = " << kv.second << "\n";
			return entries.empty() ? "no bindings in [scheme_modules]" : ss.str();
		}
		if(cmd == "apply") {
			if(!s.valid)
				return "no scheme selected yet";
			if(Game::getAddrDDGame() || !screenAllowsApply(Lobby::getFrontendScreen()))
				return "apply works in the lobby only, not in a match";
			g_appliedValid = false;
			applyFor(s, "command");
			return "applied, see /scheme";
		}
		if(cmd == "bind" || cmd == "bindhash" || cmd == "unbind") {
			if(!s.valid)
				return "no scheme selected yet";
			std::string key;
			if(cmd == "bindhash") {
				if(!s.hashValid)
					return "scheme hash unavailable";
				key = "hash:" + hex8(s.hash);
			} else {
				key = s.name.empty() ? s.structName : s.name;
				if(key.empty())
					return "scheme has no name, use /scheme bindhash";
			}
			if(cmd == "unbind") {
				std::string k2, v2;
				if(!findBinding(s, k2, v2))
					return "no binding for this scheme";
				WritePrivateProfileStringA(kSectionModules, k2.c_str(), nullptr, ini.c_str());
				g_appliedValid = false;
				return "removed binding " + k2;
			}
			std::string value = rest.empty() ? currentValueFromPackages() : rest;
			if(value.empty())
				return "no modules to bind: /enable some modules first, or /scheme bind pkg1,pkg2";
			WritePrivateProfileStringA(kSectionModules, key.c_str(), value.c_str(), ini.c_str());
			g_appliedValid = false;
			slog("bound %s = %s", key.c_str(), value.c_str());
			return "bound '" + key + "' = " + value;
		}
		return std::string("unknown /scheme command '") + cmd + "'\n" + kHelp;
	} catch(const std::exception &e) {
		slog("/scheme %s failed: %s", args.c_str(), e.what());
		return std::string("scheme error: ") + e.what();
	}
}
