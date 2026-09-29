#include "PackageManager.h"
#include <filesystem>
#include <Windows.h>
#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <vector>
#include "../Lobby.h"
#include "../LobbyGui.h"
#include "../Chat.h"
#include "../Game.h"
#include "../Utils.h"
#include <args.hxx>
#include "../Config.h"
#include "../Input.h"
#include "../Lua.h"
#include "../FrameScheduler.h"
#include <sol/sol.hpp>
#include <cctype>
#include "../entities/tasks/CTaskTeam.h"
#include "../entities/gametasks/CTaskWorm.h"

namespace fs = std::filesystem;

static void notifyGameChat(const std::string &msg) {
	if(Game::isValidDdGame())
		Chat::callShowChatMessage(msg, 7);
}

static int parseVersionTriple(const std::string &v, int &maj, int &min, int &rev) {
	maj = min = rev = 0;
	return std::sscanf(v.c_str(), "%d.%d.%d", &maj, &min, &rev);
}

static bool versionLess(const std::string &a, const std::string &b) {
	int amaj, amin, arev, bmaj, bmin, brev;
	parseVersionTriple(a, amaj, amin, arev);
	parseVersionTriple(b, bmaj, bmin, brev);
	if(amaj != bmaj) return amaj < bmaj;
	if(amin != bmin) return amin < bmin;
	return arev < brev;
}

static uint32_t fnv1aHash(const std::string &s) {
	uint32_t h = 2166136261u;
	for(unsigned char c : s) {
		h ^= c;
		h *= 16777619u;
	}
	return h;
}

static std::string normalizeChatMessage(const char *msg) {
	if(!msg || !msg[0])
		return "";
	std::string s(msg);
	while(!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
		s.erase(s.begin());
	while(!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
		s.pop_back();
	if(s.empty())
		return "";
	if(s[0] != '/')
		s = "/" + s;
	return s;
}

static const char *kAutostartSection = "autostart";

// [autostart] section of wkJellyWorm.ini: one line per package, "name=0" (disabled),
// "name=1" (enabled, latest version) or "name=1.2.3" (enabled, pinned version).
static std::vector<std::pair<std::string, std::string>> readIniSection(const char *section, const std::string &file) {
	std::vector<std::pair<std::string, std::string>> result;
	std::vector<char> buf(262144, 0);
	DWORD n = GetPrivateProfileSectionA(section, buf.data(), (DWORD) buf.size(), file.c_str());
	if(n == 0 || n >= buf.size() - 2)
		return result;
	const char *p = buf.data();
	while(*p) {
		std::string line(p);
		size_t eq = line.find('=');
		if(eq != std::string::npos)
			result.emplace_back(line.substr(0, eq), line.substr(eq + 1));
		p += line.size() + 1;
	}
	return result;
}

static void writeIniValue(const char *section, const std::string &key, const std::string &value, const std::string &file) {
	WritePrivateProfileStringA(section, key.c_str(), value.c_str(), file.c_str());
}

PackageManager::PackageManager() {
	auto lua = Lua::getInstance().getState();
	lua->set_function("registerCallback_gameGlobalInit", &Callbacks<&gameGlobalInit_t>::registerCallback);
	lua->set_function("registerCallback_handleCGameTaskPhysics", &Callbacks<&handleCGameTaskPhysics_t>::registerCallback);
	lua->set_function("registerCallback_handleCreateExplosion", &Callbacks<&createExplosion_t>::registerCallback);
	lua->set_function("registerCallback_handleSpecialImpact", &Callbacks<&specialImpact_t>::registerCallback);
	lua->set_function("registerCallback_writeLandRaw", &Callbacks<&writeLandRaw_t>::registerCallback);
	lua->set_function("registerCallback_writeLandMaskID", &Callbacks<&writeLandMaskID_t>::registerCallback);
	lua->set_function("registerCallback_weaponRelease", &Callbacks<&weaponRelease_t>::registerCallback);
	lua->set_function("registerCallback_wormStartFiringWeapon", &Callbacks<&wormStartFiringWeapon_t>::registerCallback);
	lua->set_function("registerCallback_fireWeapon", &Callbacks<&fireWeapon_t>::registerCallback);
	lua->set_function("registerCallback_createWeaponProjectile", &Callbacks<&createWeaponProjectile_t>::registerCallback);
	lua->set_function("registerCallback_onFrame", &Callbacks<&onFrame_t>::registerCallback);
	lua->set_function("registerCallback_onTurnStart", &Callbacks<&onTurnStart_t>::registerCallback);
	Input::registerLuaBindings();

}

void PackageManager::clearCallbacks() {
	Callbacks<&gameGlobalInit_t>::clearCallbacks();
	Callbacks<&handleCGameTaskPhysics_t>::clearCallbacks();
	Callbacks<&createExplosion_t>::clearCallbacks();
	Callbacks<&specialImpact_t>::clearCallbacks();
	Callbacks<&writeLandRaw_t>::clearCallbacks();
	Callbacks<&writeLandMaskID_t>::clearCallbacks();
	Callbacks<&weaponRelease_t>::clearCallbacks();
	Callbacks<&wormStartFiringWeapon_t>::clearCallbacks();
	Callbacks<&fireWeapon_t>::clearCallbacks();
	Callbacks<&createWeaponProjectile_t>::clearCallbacks();
	Callbacks<&onFrame_t>::clearCallbacks();
	Callbacks<&onTurnStart_t>::clearCallbacks();

	CTask::CTask_vtableHooks<CTaskWorm>::clearCallbacks();
	CTask::CTask_vtableHooks<CTaskTeam>::clearCallbacks();
	CGameTask::CGameTask_vtableHooks<CTaskWorm>::clearCallbacks();
}

int PackageManager::reloadPackages(void *lobbyObj) {
	std::map<std::string, std::shared_ptr<Package>> explicitSnapshot = enabledPackagesExplicit;
	nlohmann::json configSnapshot;
	handleWriteConfig(configSnapshot);

	clearCallbacks();
	FrameScheduler::clearTimers();

	std::vector<std::string> enabledNames;
	for(auto & it : enabledPackagesAll)
		enabledNames.push_back(it.first);
	for(auto & name : enabledNames) {
		try {
			disablePackage(name, false);
		} catch(std::exception & e) {
			printf("reloadPackages disable %s: %s\n", name.c_str(), e.what());
		}
	}
	enabledPackagesAll.clear();
	enabledPackagesExplicit.clear();
	initializedPackages.clear();

	scanPackages();
	scanJpxPackages();

	int reloaded = 0;
	if(configSnapshot.contains("packages")) {
		for(auto & entry : configSnapshot["packages"]) {
			try {
				std::string name = entry["name"];
				std::string version = entry["version"];
				bool manual = explicitSnapshot.find(name) != explicitSnapshot.end();
				enablePackage(name, version, manual);
				if(enabledPackagesAll.find(name) == enabledPackagesAll.end())
					throw std::runtime_error("enable failed");
				std::string configstr = entry["config"].dump();
				enabledPackagesAll[name]->callInterface<int, std::function<int(std::string)>>("readConfig", configstr);
				reloaded++;
			} catch(std::exception & e) {
				printf("reloadPackages re-enable exception: %s\n", e.what());
			}
		}
	} else {
		for(auto & it : explicitSnapshot) {
			try {
				enablePackage(it.first, it.second->getVersionstr(), true);
				reloaded++;
			} catch(std::exception & e) {
				printf("reloadPackages explicit re-enable %s: %s\n", it.first.c_str(), e.what());
			}
		}
	}

	if(gameInitialized)
		ensureAllEnabledInitialized();

	if(lobbyObj) {
		std::string msg = "reloaded " + std::to_string(reloaded) + " packages";
		Lobby::showGreenText(lobbyObj, (char *) msg.c_str());
		Lobby::sendConfigString(lobbyObj);
	} else if(Game::isValidDdGame()) {
		notifyGameChat("wkJellyWorm: reloaded " + std::to_string(reloaded) + " packages");
	}
	return reloaded;
}

PackageManager &PackageManager::getInstance() {
	static PackageManager instance;
	return instance;
}

int PackageManager::applyAutostart() {
	int applied = 0;
	auto entries = readIniSection(kAutostartSection, Config::iniFile);
	if(Config::selfTestEnabled) {
		FILE *f = nullptr;
		if(fopen_s(&f, "jelly\\selftest.log", "a") == 0 && f) {
			fprintf(f, "autostart section '%s' from '%s': %d entr(ies)\n",
				kAutostartSection, Config::iniFile.c_str(), (int)entries.size());
			int shown = 0;
			for(auto &kv : entries) {
				if(shown++ >= 5) break;
				fprintf(f, "  entry: '%s' = '%s'\n", kv.first.c_str(), kv.second.c_str());
			}
			fclose(f);
		}
	}
	for(auto & kv : entries) {
		std::string name = kv.first;
		std::string value = kv.second;
		while(!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
			value.pop_back();
		if(value.empty() || value == "0")
			continue;
		std::string version = (value == "1") ? "latest" : value;
		resolveNameAlias(name, version);
		// Write the name BEFORE loading and flush: a package that takes the
		// process down hard throws nothing and leaves no crash dump, so the
		// last name in this file is the culprit. This is the only way to
		// identify it without bisecting 643 packages by hand.
		if(Config::selfTestEnabled) {
			FILE *f = nullptr;
			if(fopen_s(&f, "jelly\\selftest.log", "a") == 0 && f) {
				fprintf(f, "loading %s\n", name.c_str());
				fflush(f);
				fclose(f);
			}
		}
		try {
			enablePackage(name, version, true);
			checkDependenciesAdd();
			applied++;
		} catch(std::exception & e) {
			// A package failing to load is the most common cause of "the mod
			// does nothing", and the reason only ever went to the dev console,
			// which cannot be read from outside the process. Record it.
			printf("applyAutostart(%s=%s): %s\n", name.c_str(), value.c_str(), e.what());
			if(Config::selfTestEnabled) {
				FILE *f = nullptr;
				if(fopen_s(&f, "jelly\\selftest.log", "a") == 0 && f) {
					fprintf(f, "FAILED %s=%s: %s\n", name.c_str(), value.c_str(), e.what());
					fclose(f);
				}
			}
		}
	}
	if(applied > 0 && gameInitialized)
		ensureAllEnabledInitialized();
	return applied;
}

int PackageManager::handleAutostartCommand(void *lobbyObj, const std::string &args) {
	auto feedback = [&](const std::string &msg) {
		if(lobbyObj)
			Lobby::showGreenText(lobbyObj, (char *) msg.c_str());
		else
			notifyGameChat("wkJellyWorm: " + msg);
	};

	std::vector<std::string> argsSplit;
	Utils::split_in_args(argsSplit, args);
	std::string sub = argsSplit.empty() ? "list" : argsSplit[0];

	if(sub == "scan" || sub == "refresh" || sub == "populate") {
		std::set<std::string> before;
		for(auto & kv : readIniSection(kAutostartSection, Config::iniFile))
			before.insert(kv.first);
		ensureAutostartDefaultsInIni();
		int added = 0;
		for(auto & kv : readIniSection(kAutostartSection, Config::iniFile)) {
			if(before.find(kv.first) == before.end())
				added++;
		}
		feedback("autostart scan: " + std::to_string(added) + " new package(s) written to ["
			+ kAutostartSection + "] in " + Config::iniFile
			+ " (directory names only; set to 1 to auto-enable)");
		return 1;
	}
	if(sub == "list") {
		auto entries = readIniSection(kAutostartSection, Config::iniFile);
		if(entries.empty()) {
			feedback("autostart list is empty. Run /autostart scan to list all installed packages, or /autostart add <pkg>");
		} else {
			std::stringstream ss;
			ss << "[" << kAutostartSection << "] in " << Config::iniFile << " (edit directly, 0=off, 1=latest, or pin a version):\n";
			for(auto & kv : entries)
				ss << "  " << kv.first << "=" << (kv.second.empty() ? "0" : kv.second) << "\n";
			feedback(ss.str());
		}
		return 1;
	}
	if(sub == "clear") {
		for(auto & kv : readIniSection(kAutostartSection, Config::iniFile))
			writeIniValue(kAutostartSection, kv.first, "0", Config::iniFile);
		feedback("autostart: all packages set to 0");
		return 1;
	}
	if(sub == "add" || sub == "enable") {
		if(argsSplit.size() < 2) {
			feedback("Usage: /autostart add <package> [-v version]");
			return 1;
		}
		std::string name = argsSplit[1];
		std::string version = "latest";
		if(argsSplit.size() >= 4 && (argsSplit[2] == "-v" || argsSplit[2] == "--version"))
			version = argsSplit[3];
		resolveNameAlias(name, version);
		try {
			enablePackage(name, version, true);
			writeIniValue(kAutostartSection, name, version, Config::iniFile);
			checkDependenciesAdd();
			ensureAllEnabledInitialized();
			feedback("autostart: " + name + "=" + version + " (enabled now too)");
			if(lobbyObj)
				Lobby::sendConfigString(lobbyObj);
		} catch(std::exception & e) {
			feedback(std::string("autostart add failed: ") + e.what());
		}
		return 1;
	}
	if(sub == "remove" || sub == "disable" || sub == "del" || sub == "rm") {
		if(argsSplit.size() < 2) {
			feedback("Usage: /autostart remove <package>");
			return 1;
		}
		std::string name = argsSplit[1];
		std::string dummyVersion = "latest";
		resolveNameAlias(name, dummyVersion);
		writeIniValue(kAutostartSection, name, "0", Config::iniFile);
		try {
			if(enabledPackagesAll.find(name) != enabledPackagesAll.end()) {
				disablePackage(name, true);
				checkDependenciesRemove();
			}
		} catch(std::exception & e) {
			printf("autostart remove disable %s: %s\n", name.c_str(), e.what());
		}
		feedback("autostart: " + name + "=0");
		return 1;
	}
	feedback("Usage: /autostart [list|add <pkg> [-v version]|remove <pkg>|clear]");
	return 1;
}

int PackageManager::handleJpxCommand(void *lobbyObj, const std::string &args) {
	auto feedback = [&](const std::string &msg) {
		if(lobbyObj)
			Lobby::showGreenText(lobbyObj, (char *) msg.c_str());
		else
			notifyGameChat("wkJellyWorm: " + msg);
	};

	std::vector<std::string> argsSplit;
	Utils::split_in_args(argsSplit, args);
	std::string sub = argsSplit.empty() ? "list" : argsSplit[0];

	if(sub == "reload") {
		scanJpxPackages();
		feedback("jpx reload: " + std::to_string(jpxAllversions.size()) + " scheme bundles in " + Config::jpxDir);
		return 1;
	}

	if(sub == "list" || sub == "ls") {
		if(jpxAllversions.empty()) {
			feedback("jpx: no scheme bundles in " + Config::jpxDir + " (put sbornik_* packages there, then /jpx reload)");
			return 1;
		}
		std::stringstream ss;
		ss << "jpx schemes (" << jpxAllversions.size() << ") — /jpx enable <name>:\n";
		int okCount = 0;
		int missCount = 0;
		for(auto & it : jpxAllversions) {
			std::shared_ptr<Package> pack;
			for(auto & p : it.second) {
				if(!pack || versionLess(pack->getVersionstr(), p->getVersionstr()))
					pack = p;
			}
			auto missing = validateJpxDependencies(it.first);
			bool enabled = enabledPackagesAll.find(it.first) != enabledPackagesAll.end();
			size_t depCount = 0;
			std::string desc;
			if(pack) {
				auto & deps = pack->getManifest()["dependencies"];
				depCount = deps.size();
				desc = pack->getDescription();
			}
			if(missing.empty())
				okCount++;
			else
				missCount++;
			ss << (enabled ? "* " : "  ") << it.first;
			ss << " [" << depCount << " deps]";
			if(missing.empty())
				ss << " OK";
			else
				ss << " MISSING " << missing.size();
			if(!desc.empty())
				ss << " — " << desc;
			ss << "\n";
		}
		ss << "OK=" << okCount << " MISSING=" << missCount << " (* = enabled)";
		feedback(ss.str());
		return 1;
	}

	if(sub == "enable") {
		if(argsSplit.size() < 2) {
			feedback("Usage: /jpx enable <sbornik_name>");
			return 1;
		}
		std::string name = argsSplit[1];
		if(jpxAllversions.find(name) == jpxAllversions.end()) {
			feedback("jpx: unknown scheme bundle '" + name + "' (try /jpx list)");
			return 1;
		}
		auto missing = validateJpxDependencies(name);
		if(!missing.empty()) {
			std::stringstream ss;
			ss << "jpx: cannot enable " << name << " — missing " << missing.size() << " package(s):\n";
			for(size_t i = 0; i < missing.size() && i < 20; i++)
				ss << "  " << missing[i] << "\n";
			if(missing.size() > 20)
				ss << "  ... and " << (missing.size() - 20) << " more\n";
			ss << "Install them into jelly/packages first.";
			feedback(ss.str());
			return 1;
		}
		try {
			enablePackage(name, "latest", true);
			checkDependenciesAdd();
			ensureAllEnabledInitialized();
			size_t depCount = 0;
			auto pack = findPackageVersion(name, "latest", true);
			if(pack)
				depCount = pack->getManifest()["dependencies"].size();
			feedback("jpx: enabled " + name + " (+" + std::to_string(depCount) + " deps)");
			if(lobbyObj) {
				handleLobbyListPackages(lobbyObj, false, true, true);
				Lobby::sendConfigString(lobbyObj);
			}
		} catch(const std::exception &e) {
			feedback(std::string("jpx enable failed: ") + e.what());
		}
		return 1;
	}

	if(sub == "disable") {
		if(argsSplit.size() < 2) {
			feedback("Usage: /jpx disable <sbornik_name>");
			return 1;
		}
		std::string name = argsSplit[1];
		try {
			disablePackage(name, true);
			checkDependenciesRemove();
			feedback("jpx: disabled " + name);
			if(lobbyObj) {
				handleLobbyListPackages(lobbyObj, false, true, true);
				Lobby::sendConfigString(lobbyObj);
			}
		} catch(const std::exception &e) {
			feedback(std::string("jpx disable failed: ") + e.what());
		}
		return 1;
	}

	feedback("Usage: /jpx [list|enable <name>|disable <name>|reload]");
	return 1;
}

std::vector<std::string> PackageManager::listEnabledPackageIds() const {
	std::vector<std::string> ids;
	ids.reserve(enabledPackagesAll.size());
	for(auto & it : enabledPackagesAll)
		ids.push_back(it.second->getIdentifier());
	return ids;
}

std::vector<PackageManager::PackageGuiEntry> PackageManager::listPackagesForGui() const {
	std::vector<PackageGuiEntry> enabledEntries;
	std::vector<PackageGuiEntry> disabledEntries;
	for(auto & it : allversions) {
		PackageGuiEntry entry;
		entry.name = it.first;
		std::shared_ptr<Package> pack;
		try {
			pack = getLatestVersion(it.first);
			entry.version = pack->getVersionstr();
		} catch(...) {
			entry.version = "?";
		}
		auto enabled = enabledPackagesAll.find(it.first);
		entry.enabled = enabled != enabledPackagesAll.end();
		if(entry.enabled) {
			entry.version = enabled->second->getVersionstr();
			pack = enabled->second;
		}
		if(pack) {
			entry.description = pack->getDescription();
			entry.author = pack->getAuthor();
		}
		if(entry.enabled)
			enabledEntries.push_back(std::move(entry));
		else
			disabledEntries.push_back(std::move(entry));
	}
	std::vector<PackageGuiEntry> entries;
	entries.reserve(enabledEntries.size() + disabledEntries.size());
	for(auto &e : enabledEntries) entries.push_back(std::move(e));
	for(auto &e : disabledEntries) entries.push_back(std::move(e));
	return entries;
}

void PackageManager::markAllEnabledExplicit() {
	for(auto & it : enabledPackagesAll)
		enabledPackagesExplicit[it.first] = it.second;
}

void PackageManager::togglePackageFromGui(void *lobbyObj, const std::string &name, const std::string &version, bool enable) {
	std::string pkgName = name;
	std::string ver = version;
	resolveNameAlias(pkgName, ver);
	try {
		if(enable) {
			enablePackage(pkgName, ver == "?" ? "latest" : ver, true);
			checkDependenciesAdd();
		} else {
			disablePackage(pkgName, true);
			checkDependenciesRemove();
		}
		if(lobbyObj)
			Lobby::sendConfigString(lobbyObj);
		else if(Lobby::getFrontendScreen() == 1704) {
			void *host = Lobby::getHostObj();
			if(host)
				Lobby::sendConfigString(host);
		}
	} catch(std::exception & e) {
		if(lobbyObj)
			Lobby::showGreenText(lobbyObj, (char *)e.what());
		printf("togglePackageFromGui(%s): %s\n", pkgName.c_str(), e.what());
	}
}

void PackageManager::scanPackages() {
	packages.clear();
	allversions.clear();

	if(Config::scanAllPackages) {
		// full scan: walk every manifest.json under jelly/packages (can get slow with lots of packages)
		for (const auto & entry : fs::recursive_directory_iterator(Config::packageDir)) {
			if(!entry.is_regular_file()) continue;
			if(entry.path().filename() == "manifest.json") {
				addPackage(entry.path().string());
			}
		}
	} else {
		// fast mode: only load enabled [autostart] entries. Disabled entries remain
		// in the INI as the persistent package base, but do not parse manifests.
		std::set<std::string> knownNames;
		for(auto kv : readIniSection(kAutostartSection, Config::iniFile)) {
			while(!kv.second.empty() && (kv.second.front() == ' ' || kv.second.front() == '\t'))
				kv.second.erase(kv.second.begin());
			while(!kv.second.empty() && (kv.second.back() == ' ' || kv.second.back() == '\t' || kv.second.back() == '\r'))
				kv.second.pop_back();
			if(!kv.second.empty() && kv.second != "0")
				knownNames.insert(kv.first);
		}
		std::error_code ec;
		for(auto & name : knownNames) {
			fs::path pkgDir = fs::path(Config::packageDir) / name;
			if(!fs::is_directory(pkgDir, ec))
				continue;
			std::string requestedVersion = "latest";
			for(auto kv : readIniSection(kAutostartSection, Config::iniFile)) {
				if(kv.first != name)
					continue;
				while(!kv.second.empty() && (kv.second.front() == ' ' || kv.second.front() == '\t'))
					kv.second.erase(kv.second.begin());
				while(!kv.second.empty() && (kv.second.back() == ' ' || kv.second.back() == '\t' || kv.second.back() == '\r'))
					kv.second.pop_back();
				if(kv.second != "1")
					requestedVersion = kv.second;
			}
			for(const auto & verEntry : fs::directory_iterator(pkgDir, ec)) {
				if(ec || !verEntry.is_directory())
					continue;
				if(requestedVersion != "latest" && verEntry.path().filename().string() != requestedVersion)
					continue;
				fs::path manifestPath = verEntry.path() / "manifest.json";
				if(fs::exists(manifestPath))
					addPackage(manifestPath.string());
			}
		}
	}

	// In fast mode dependencies are intentionally resolved lazily by
	// checkDependenciesAdd() -> enablePackage(). Do not discard the enabled
	// roots just because their dependency manifests were not scanned yet.
	std::vector<std::shared_ptr<Package>> brokenPackages;
	std::string brokenPackagesStr = "The following packages were disabled due to unmet dependencies:\n";
	for(auto & it : packages) {
		if(!Config::scanAllPackages)
			break;
		auto & manifest = it.second->getManifest();
		auto & dependencies = manifest["dependencies"];
		for(std::string dependency : dependencies) {
			if(allversions.find(dependency) == allversions.end()) {
				brokenPackages.push_back(it.second);
				brokenPackagesStr += it.second->getIdentifier() + " ";
				printf("unmet package dependencies: %s (%s)\n", it.second->getIdentifier().c_str(), dependency.c_str());
			}
		}
	}
	if(!brokenPackages.empty()) {
		MessageBoxA(0, brokenPackagesStr.c_str(), "wkJellyWorm", MB_ICONWARNING);
	}
	for(auto & package : brokenPackages) {
		packages.erase(package->getIdentifier());
		auto name = package->getName();
		allversions[name].erase(package);
		if(allversions[name].empty())
			allversions.erase(name);
	}

	// debug - determine latest verions of packages
	for(auto & it : allversions) {
		auto & name = it.first;
		auto latest = getLatestVersion(name);
		printf("latest: %s\n", latest->getIdentifier().c_str());
	}
	// autostart [ini] defaults are NOT populated here on purpose - this runs at game
	// startup (and on /reload) and we don't want to add disk writes to that path.
	// Run "/autostart scan" once to write missing "package=0" lines into wkJellyWorm.ini.
}

void PackageManager::ensureAutostartDefaultsInIni() {
	std::set<std::string> haveKeys;
	for(auto & kv : readIniSection(kAutostartSection, Config::iniFile))
		haveKeys.insert(kv.first);
	std::error_code ec;
	if(!fs::is_directory(Config::packageDir, ec))
		return;
	for(const auto & entry : fs::directory_iterator(Config::packageDir, ec)) {
		if(ec)
			break;
		if(!entry.is_directory())
			continue;
		std::string name = entry.path().filename().string();
		if(!name.empty() && haveKeys.find(name) == haveKeys.end()) {
			writeIniValue(kAutostartSection, name, "0", Config::iniFile);
			haveKeys.insert(name);
		}
	}
}

bool PackageManager::tryAddPackageFromDisk(const std::string &name, const std::string &version) {
	if(name.empty())
		return false;
	auto known = allversions.find(name);
	if(version == "latest" && known != allversions.end() && !known->second.empty())
		return true;
	if(version != "latest" && known != allversions.end()) {
		for(auto & pack : known->second)
			if(pack->getVersionstr() == version)
				return true;
	}
	std::error_code ec;
	fs::path packagePath = fs::path(Config::packageDir) / name;
	if(!fs::is_directory(packagePath, ec))
		return false;
	bool added = false;
	if(version != "latest") {
		fs::path manifestPath = packagePath / version / "manifest.json";
		if(fs::is_regular_file(manifestPath, ec)) {
			addPackage(manifestPath.string(), false);
			added = allversions.find(name) != allversions.end();
		}
		return added;
	}
	for(const auto & entry : fs::directory_iterator(packagePath, ec)) {
		if(ec)
			break;
		if(!entry.is_directory())
			continue;
		fs::path manifestPath = entry.path() / "manifest.json";
		if(fs::is_regular_file(manifestPath, ec)) {
			addPackage(manifestPath.string(), false);
			added = true;
		}
	}
	return added;
}

void PackageManager::addPackage(std::string path, bool showError) {
	try {
		printf("addPackage: %s\n", path.c_str());
		auto package = std::make_shared<Package>(path);
		auto name = package->getName();
		auto versionstr =  package->getVersionstr();
		auto identifier = package->getIdentifier();

		packages[identifier] = package;
		allversions[name].insert(package);

	} catch(std::exception & e) {
		std::string msg = "Failed to load package:\n" + path + "\n\n";
		msg += e.what();
		if(showError)
			MessageBoxA(0, msg.c_str(), "wkJellyWorm - Failed to load package manifest", MB_ICONWARNING);
		else
			printf("%s\n", msg.c_str());
	}
}

void PackageManager::addJpxPackage(std::string path) {
	try {
		printf("addJpxPackage: %s\n", path.c_str());
		auto package = std::make_shared<Package>(path);
		auto name = package->getName();
		auto identifier = package->getIdentifier();
		jpxPackages[identifier] = package;
		jpxAllversions[name].insert(package);
	} catch(std::exception & e) {
		std::string msg = "Failed to load jpx package:\n" + path + "\n\n";
		msg += e.what();
		MessageBoxA(0, msg.c_str(), "wkJellyWorm - Failed to load jpx manifest", MB_ICONWARNING);
	}
}

void PackageManager::scanJpxPackages() {
	jpxPackages.clear();
	jpxAllversions.clear();
	std::error_code ec;
	if(!fs::is_directory(Config::jpxDir, ec)) {
		printf("scanJpxPackages: %s missing, skip\n", Config::jpxDir.c_str());
		return;
	}
	for(const auto & entry : fs::recursive_directory_iterator(Config::jpxDir, ec)) {
		if(ec)
			break;
		if(!entry.is_regular_file())
			continue;
		if(entry.path().filename() == "manifest.json")
			addJpxPackage(entry.path().string());
	}
	printf("scanJpxPackages: %zu scheme bundles\n", jpxAllversions.size());
}

std::shared_ptr<Package> PackageManager::findPackageVersion(const std::string &name, const std::string &version, bool includeJpx) const {
	auto findIn = [&](const std::map<std::string, std::set<std::shared_ptr<Package>>> &registry) -> std::shared_ptr<Package> {
		auto it = registry.find(name);
		if(it == registry.end() || it->second.empty())
			return nullptr;
		if(version == "latest") {
			std::shared_ptr<Package> best;
			for(auto & pack : it->second) {
				if(!best || versionLess(best->getVersionstr(), pack->getVersionstr()))
					best = pack;
			}
			return best;
		}
		for(auto & packver : it->second) {
			if(packver->getVersionstr() == version)
				return packver;
		}
		return nullptr;
	};
	auto pack = findIn(allversions);
	if(pack || !includeJpx)
		return pack;
	return findIn(jpxAllversions);
}

std::vector<std::string> PackageManager::validateJpxDependencies(const std::string &name) const {
	std::vector<std::string> missing;
	auto it = jpxAllversions.find(name);
	if(it == jpxAllversions.end() || it->second.empty())
		return missing;
	std::shared_ptr<Package> pack;
	for(auto & p : it->second) {
		if(!pack || versionLess(pack->getVersionstr(), p->getVersionstr()))
			pack = p;
	}
	if(!pack)
		return missing;
	auto & deps = pack->getManifest()["dependencies"];
	for(std::string dependency : deps) {
		if(allversions.find(dependency) == allversions.end())
			missing.push_back(dependency);
	}
	return missing;
}

void PackageManager::enablePackage(std::string name, std::string version, bool manual) {
	if(allversions.find(name) == allversions.end())
		tryAddPackageFromDisk(name, version);
	auto pack = findPackageVersion(name, version, true);
	if(pack == nullptr) {
		if(jpxAllversions.find(name) != jpxAllversions.end() || allversions.find(name) != allversions.end())
			throw std::runtime_error("Package version " + name + "(" + version + ") not found");
		throw std::runtime_error("Package " + name + " not found");
	}

	std::string minVer = pack->getMinJellyVersion();
	if(!minVer.empty() && versionLess(Config::getJellyVersionStr(), minVer)) {
		throw std::runtime_error("Package " + pack->getIdentifier()
			+ " requires wkJellyWorm >= " + minVer
			+ " (you have " + Config::getJellyVersionStr() + ")");
	}

	auto caps = pack->getCapabilities();
	if(!caps.empty()) {
		std::string capList;
		for(size_t i = 0; i < caps.size(); i++) {
			if(i) capList += ", ";
			capList += caps[i];
		}
		std::string warn = "wkJellyWorm: enabling " + pack->getIdentifier()
			+ " requests capabilities: " + capList;
		printf("%s\n", warn.c_str());
		notifyGameChat(warn);
	}

	auto existing = enabledPackagesAll.find(name);
	if(existing != enabledPackagesAll.end() && existing->second == pack) {
		if(manual)
			enabledPackagesExplicit[name] = pack;
		ensurePackageInitialized(pack);
		return;
	}
	if(existing != enabledPackagesAll.end())
		existing->second->disable();

	pack->enable();
	ensurePackageInitialized(pack);
	enabledPackagesAll[name] = pack;
	if(manual)
		enabledPackagesExplicit[name] = pack;
}

void PackageManager::disablePackage(std::string name, bool manual) {
	if(enabledPackagesAll.find(name) == enabledPackagesAll.end())
		throw std::runtime_error("Package " + name + " is not enabled");
	if(manual) {
		if(enabledPackagesExplicit.find(name) != enabledPackagesExplicit.end())
			enabledPackagesExplicit.erase(name);
		else
			throw std::runtime_error("disablePackage: Package " + name + " is not found in enabledPackagesExplicit");
	}
	else{
		auto package = enabledPackagesAll[name];
		package->disable();
		initializedPackages.erase(package->getIdentifier());
		if(enabledPackagesAll.find(name) != enabledPackagesAll.end())
			enabledPackagesAll.erase(name);
		else
			throw std::runtime_error("disablePackage: Package " + name + " is not found in enabledPackagesAll");
	}
}

void PackageManager::checkDependenciesAdd() {
	std::set<std::string> added;
	bool changed;
	do {
		changed = false;
		for (auto &it : enabledPackagesAll) {
			auto & manifest = it.second->getManifest();
			auto & dependencies = manifest["dependencies"];
			for(std::string dependency : dependencies) {
				if(enabledPackagesAll.find(dependency) == enabledPackagesAll.end()) {
					if(added.find(dependency) != added.end())
						continue; // already failed once this call; do not retry forever
					added.insert(dependency);
					try {
						enablePackage(dependency, "latest", false);
						changed = true;
						printf("checkDependenciesAdd: added: %s\n", dependency.c_str());
					} catch(std::exception & e) {
						// A missing/broken dependency must not take down the
						// unrelated package whose own enablePackage() call
						// happened to trigger this check. Uncaught, this
						// exception used to propagate out of
						// checkDependenciesAdd() and get misattributed by
						// applyAutostart()'s catch block to whatever package
						// was currently loading -- one broken dependency
						// (e.g. weapon_tazer) made every package loaded after
						// it falsely appear broken for the rest of the run.
						printf("checkDependenciesAdd: failed to add dependency %s: %s\n",
							dependency.c_str(), e.what());
					}
				}
			}
		}
	} while(changed);
}


void PackageManager::checkDependenciesRemove() {
	std::map<std::string, std::shared_ptr<Package>> newEnabled;
	for(auto & it : enabledPackagesExplicit) {
		newEnabled[it.first] = it.second;
	}
	for(auto & it : enabledPackagesAll) {
		if(newEnabled.find(it.first) == newEnabled.end()) {
			it.second->disable();
			initializedPackages.erase(it.second->getIdentifier());
		}
	}
	enabledPackagesAll = std::move(newEnabled);
	checkDependenciesAdd();
}

void PackageManager::handleLobbyListPackages(void * lobbyObj, bool all, bool manual, bool dependencies) {
	std::stringstream ss;
	if(all) {
		ss << "All packages:\n";
		for (auto &it : allversions) {
			ss << it.first << " : ";
			for (auto &ver : it.second) {
				ss << ver->getVersionstr() << " ";
			}
			ss << "\n";
		}
		ss << "\n";
	}

	if(manual) {
		ss << "Explicitly enabled packages:\n";
		for(auto & it : enabledPackagesExplicit) {
			ss << it.first << " " << it.second->getVersionstr() << "\n";
		}
		ss << "\n";
	}

	if(dependencies) {
		ss << "All enabled packages:\n";
		for(auto & it : enabledPackagesAll) {
			ss << it.first << " " << it.second->getVersionstr() << "\n";
		}
		ss << "\n";
	}
	Lobby::showGreenText(lobbyObj, (char *) ss.str().c_str());
}


int PackageManager::handleLobbyHostCommand(void *lobbyObj, std::string &command, std::string &args) {
	if(command == "jpx") {
		return handleJpxCommand(lobbyObj, args);
	}
	if(command == "packages") {
		while(!args.empty() && (args[0] == ' ' || args[0] == '\t'))
			args.erase(0, 1);
		if(args == "gui") {
			LobbyGui::toggle();
			std::string msg = LobbyGui::isOpen()
				? "package picker open (Up/Down, Enter, Esc)"
				: "package picker closed";
			Lobby::showGreenText(lobbyObj, (char *)msg.c_str());
			return 1;
		}
		handleLobbyListPackages(lobbyObj, true, true, true);
		return 1;
	}
	if(command == "reload") {
		reloadPackages(lobbyObj);
		return 1;
	}
	if(command == "autostart") {
		return handleAutostartCommand(lobbyObj, args);
	}
	if(command == "enable") {
		std::vector<std::string> argsSplit;
		Utils::split_in_args(argsSplit, args);

		args::ArgumentParser parser("Enable specified package");
		args::HelpFlag help(parser, "help", "Display this help menu", {"help"});
		args::Positional<std::string> pathsList(parser, "name", "Package name");
		args::ValueFlag<std::string> packversion(parser, "version", "Specify package version", {"version", 'v'}, "latest");

		try {
			if (argsSplit.empty()) throw args::Help("");
			parser.ParseCLI(argsSplit);
			auto & name = pathsList.Get();
			auto & version = packversion.Get();
			resolveNameAlias(name, version);
			enablePackage(name, version, true);
			checkDependenciesAdd();
		}
		catch (const args::Help &) {
			std::string help = "/" + command + parser.Help();
			Lobby::showGreenText(lobbyObj, (char *) help.c_str());
		}
		catch (const std::exception &e) {
			Lobby::showGreenText(lobbyObj, (char *) e.what());
		}
		handleLobbyListPackages(lobbyObj, false, true, true);
		Lobby::sendConfigString(lobbyObj);
		return 1;
	}
	if(command == "disable") {
		std::vector<std::string> argsSplit;
		Utils::split_in_args(argsSplit, args);

		args::ArgumentParser parser("Disable specified package");
		args::HelpFlag help(parser, "help", "Display this help menu", {"help"});
		args::Positional<std::string> pathsList(parser, "name", "Package name");

		try {
			if (argsSplit.empty()) throw args::Help("");
			parser.ParseCLI(argsSplit);
			auto & name = pathsList.Get();
			std::string version = "latest";
			resolveNameAlias(name, version);
			disablePackage(name, true);
			checkDependenciesRemove();
		}
		catch (const args::Help &) {
			std::string help = "/" + command + parser.Help();
			Lobby::showGreenText(lobbyObj, (char *) help.c_str());
		}
		catch (const std::exception &e) {
			Lobby::showGreenText(lobbyObj, (char *) e.what());
		}
		handleLobbyListPackages(lobbyObj, false, true, true);
		Lobby::sendConfigString(lobbyObj);
		return 1;
	}

	int ret = Interface<&lobbyHostCli_t>::callAllPackages("lobbyHostCli", lobbyObj, command, args);
	if(ret)
		return ret;

	return handlePackageChatCommand(lobbyObj, command, args);
}

bool PackageManager::autoEnableForCommand(const std::string &command, bool manual) {
	CommandPackageRef pref;
	if(!findCommandPackage(command, pref))
		return false;
	auto tryEnable = [&](const std::string &version) {
		enablePackage(pref.name, version, manual);
		checkDependenciesAdd();
		ensureAllEnabledInitialized();
	};
	try {
		tryEnable(pref.version);
		return true;
	} catch(std::exception & e) {
		if(pref.version != "latest") {
			try {
				tryEnable("latest");
				return true;
			} catch(std::exception & e2) {
				printf("autoEnableForCommand(%s): %s; latest: %s\n", command.c_str(), e.what(), e2.what());
			}
		} else {
			printf("autoEnableForCommand(%s): %s\n", command.c_str(), e.what());
		}
		return false;
	}
}

void PackageManager::ensurePackageInitialized(const std::shared_ptr<Package> &pack) {
	if(!pack || !pack->isLoaded() || !gameInitialized)
		return;
	auto identifier = pack->getIdentifier();
	if(initializedPackages.find(identifier) != initializedPackages.end())
		return;
	pack->callInterface<int, std::function<int()>>("initialize");
	initializedPackages.insert(identifier);
}

std::string PackageManager::extractChatCommand(const char *msg) {
	if(!msg || msg[0] != '/')
		return "";
	std::string s(msg);
	size_t end = 1;
	while(end < s.size() && std::isalnum(static_cast<unsigned char>(s[end])))
		++end;
	return s.substr(0, end);
}

std::string PackageManager::commandNameFromMessage(const char *msg) {
	if(!msg || !msg[0])
		return "";
	std::string cmd = extractChatCommand(msg);
	if(cmd.size() > 1)
		return cmd.substr(1);
	std::string s(msg);
	size_t end = 0;
	while(end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])))
		++end;
	return s.substr(0, end);
}

bool PackageManager::isChatRoutableLobbyCommand(const std::string &command) {
	CommandPackageRef unused;
	return getInstance().findCommandPackage(command, unused);
}

void PackageManager::ensureAllEnabledInitialized() {
	for(auto & package : enabledPackagesAll)
		ensurePackageInitialized(package.second);
}

static std::string getPackageLobbyFeedback(const std::shared_ptr<Package> &pack, bool justEnabled) {
	if(!pack) return "";
	std::stringstream ss;
	if(justEnabled)
		ss << pack->getName() << " " << pack->getVersionstr() << " enabled (auto)\n";
	try {
		std::string status = pack->callInterface<std::string, std::function<std::string()>>("getStatusMessage");
		std::string summary = pack->callInterface<std::string, std::function<std::string()>>("getSettingsSummary");
		if(!status.empty())
			ss << status << "\n";
		if(!summary.empty())
			ss << summary;
	} catch(...) {}
	return ss.str();
}

int PackageManager::handlePackageChatCommand(void *lobbyObj, const std::string &command, const std::string &args) {
	if(!isChatRoutableLobbyCommand(command))
		return 0;

	CommandPackageRef cmdRef;
	if(!findCommandPackage(command, cmdRef))
		return 0;
	const std::string &pkgName = cmdRef.name;

	bool wasEnabled = enabledPackagesAll.find(pkgName) != enabledPackagesAll.end();

	if(!autoEnableForCommand(command, false)) {
		if(lobbyObj) {
			std::string err = "wkJellyWorm: failed to auto-enable " + cmdRef.name + " (" + cmdRef.version + ")";
			Lobby::showGreenText(lobbyObj, (char *) err.c_str());
		}
		return 1;
	}

	bool justEnabled = !wasEnabled && enabledPackagesAll.find(pkgName) != enabledPackagesAll.end();

	std::string msg = "/" + command;
	if(!args.empty())
		msg += " " + args;
	std::vector<char> buf(msg.begin(), msg.end());
	buf.push_back('\0');
	int ret = handleOnChatInput(buf.data());

	if(lobbyObj) {
		std::shared_ptr<Package> pack;
		auto it = enabledPackagesAll.find(pkgName);
		if(it != enabledPackagesAll.end())
			pack = it->second;
		std::string feedback = getPackageLobbyFeedback(pack, justEnabled);
		if(feedback.empty() && pack) {
			try {
				feedback = pack->callInterface<std::string, std::function<std::string()>>("getSettingsSummary");
			} catch(...) {}
		}
		if(feedback.empty() && !ret)
			feedback = "wkJellyWorm: /" + command + " not handled";
		if(!feedback.empty()) {
			Lobby::showGreenText(lobbyObj, (char *) feedback.c_str());
			Lobby::sendGreenTextMe(lobbyObj, feedback);
		}
		Lobby::sendConfigString(lobbyObj);
		return 1;
	}
	return ret;
}

int PackageManager::handleLobbyClientCommand(void *lobbyObj, std::string &command, std::string &args) {
	return Interface<&lobbyClientCli_t>::callAllPackages("lobbyClientCli", lobbyObj, command, args);
}

std::shared_ptr<Package> PackageManager::getLatestVersion(std::string package) const {
	auto pack = findPackageVersion(package, "latest", true);
	if(!pack)
		throw std::runtime_error("getLatestVersion: not found " + package);
	return pack;
}


int PackageManager::handleCGameTaskPhysics(int a1, CGameTask *object, int a3) {
	return Callbacks<&handleCGameTaskPhysics_t>::callCallbacks(a1, object, a3);
}

int PackageManager::handleGameGlobalInit() {
	return Callbacks<&gameGlobalInit_t>::callCallbacks();
}

int PackageManager::handleReadConfig(nlohmann::json & config) {
	for(auto & package : enabledPackagesAll) {
		package.second->disable();
	}
	enabledPackagesAll.clear();
	enabledPackagesExplicit.clear();
	initializedPackages.clear();

	nlohmann::json missing = nlohmann::json::array();
	if(config.contains("packages")) {
		for(auto & entry : config["packages"]) {
			try {
				std::string name = entry["name"];
				std::string version = entry["version"];
				printf("enabling package: %s\n", name.c_str());
				enablePackage(name, version, 0);
				if(enabledPackagesAll.find(name) == enabledPackagesAll.end())
					throw std::runtime_error("enabling package failed");

				nlohmann::json packConfig = entry.value("config", nlohmann::json::object());
				std::string configstr = packConfig.dump();
				auto package = enabledPackagesAll[name];
				package->callInterface<int, std::function<int(std::string)>>("readConfig", configstr);
			} catch(std::exception & e) {
				printf("handleReadConfig::%s exception: %s\n", entry.dump().c_str(), e.what());
				std::string name = "?";
				std::string version = "?";
				try {
					if(entry.contains("name")) name = entry["name"].get<std::string>();
					if(entry.contains("version")) version = entry["version"].get<std::string>();
				} catch(...) {}
				missing.push_back({{"name", name}, {"version", version}});
				notifyGameChat("wkJellyWorm: host uses " + name + " " + version
					+ " — you don't have it, feature disabled for you");
			}
		}
	}
	config["missingPackages"] = missing;
	if(gameInitialized)
		ensureAllEnabledInitialized();
	return 0;
}

int PackageManager::handleWriteConfig(nlohmann::json & config) {
	for(auto & package : enabledPackagesAll) {
		try {
			auto packstr = package.second->callInterface<std::string, std::function<std::string()>>("writeConfig");
			nlohmann::json packjson = nlohmann::json::object();
			if(!packstr.empty()) {
				try {
					packjson = nlohmann::json::parse(packstr);
				} catch(std::exception & e) {
					printf("handleWriteConfig::%s parse: %s\n", package.second->getIdentifier().c_str(), e.what());
				}
			}
			nlohmann::json entry = {{"name",package.second->getName()}, {"version",package.second->getVersionstr()}, {"config", packjson}};
			config["packages"][package.second->getIdentifier()] = entry;
		} catch(std::exception & e) {
			printf("handleWriteConfig::%s exception: %s\n", package.second->getIdentifier().c_str(), e.what());
		}
	}
	writeFeatureFlags(config);
	return 0;
}

void PackageManager::writeFeatureFlags(nlohmann::json & config) {
	config["features"] = {
		{"supportsRNG", true},
		{"supportsProfiles", true},
		{"supportsEventBus", true},
		{"supportsTimers", true},
		{"supportsManifestV2", true},
		{"supportsTaskInspector", true},
		{"supportsHandshakeV2", true}
	};
	// Stable fingerprint of enabled package ids for clients / host diagnostics
	std::vector<std::string> ids = getInstance().listEnabledPackageIds();
	std::sort(ids.begin(), ids.end());
	std::string joined;
	for(size_t i = 0; i < ids.size(); i++) {
		if(i) joined += ";";
		joined += ids[i];
	}
	config["packagesHash"] = fnv1aHash(joined);
}

bool PackageManager::findCommandPackage(const std::string &command, CommandPackageRef &out) const {
	if(command.empty())
		return false;
	out = {};
	auto consider = [&](const std::shared_ptr<Package> &pack) {
		for(auto & cmd : pack->getChatCommands()) {
			if(cmd != command)
				continue;
			if(out.name.empty() || versionLess(out.version, pack->getVersionstr())) {
				out.name = pack->getName();
				out.version = pack->getVersionstr();
			}
		}
	};
	for(auto & it : enabledPackagesAll)
		consider(it.second);
	if(out.name.empty()) {
		for(auto & it : allversions) {
			for(auto & pack : it.second)
				consider(pack);
		}
	}
	return !out.name.empty();
}

bool PackageManager::resolveNameAlias(std::string &name, std::string &version) const {
	CommandPackageRef ref;
	if(findCommandPackage(name, ref)) {
		name = ref.name;
		if(version == "latest" || version.empty())
			version = ref.version;
		return true;
	}
	return false;
}

int PackageManager::handleResetConfig() {
	int ret = Interface<&resetConfig_t>::callAllPackages("resetConfig");
	clearCallbacks();
	FrameScheduler::clearTimers();
	for(auto & package : enabledPackagesAll) {
		package.second->disable();
	}
	enabledPackagesAll.clear();
	enabledPackagesExplicit.clear();
	initializedPackages.clear();
	gameInitialized = false;
	return ret;
}

int PackageManager::handleInitialize() {
	gameInitialized = true;
	int ret = 0;
	for(auto & package : enabledPackagesAll) {
		if(!package.second->isLoaded())
			continue;
		auto identifier = package.second->getIdentifier();
		if(initializedPackages.find(identifier) != initializedPackages.end())
			continue;
		ret |= package.second->callInterface<int, std::function<int()>>("initialize");
		initializedPackages.insert(identifier);
	}
	return ret;
}

int PackageManager::handleOnChatInput(char *msg) {
	std::string normalized = normalizeChatMessage(msg);
	if(normalized.empty())
		return 0;
	std::vector<char> buf(normalized.begin(), normalized.end());
	buf.push_back('\0');
	char *normMsg = buf.data();

	std::string cmdName = commandNameFromMessage(normMsg);
	if(!cmdName.empty() && isChatRoutableLobbyCommand(cmdName)) {
		if(!autoEnableForCommand(cmdName, false)) {
			CommandPackageRef cmdRef;
			if(findCommandPackage(cmdName, cmdRef) && Game::isValidDdGame()) {
				std::string err = "wkJellyWorm: failed to auto-enable " + cmdRef.name
					+ " (" + cmdRef.version + "). Copy jelly/packages/"
					+ cmdRef.name + "/" + cmdRef.version + "/ into W:A, or /enable "
					+ cmdRef.name;
				notifyGameChat(err);
			}
			return Game::isValidDdGame() ? 1 : 0;
		}
	} else {
		ensureAllEnabledInitialized();
	}
	return Interface<&onChatInput_t>::callAllPackages("onChatInput", normMsg);
}

// Sentinel definitions for Callbacks<&..._t> template keys (address identity only).
int PackageManager::onFrame_t() { return 0; }
int PackageManager::onTurnStart_t() { return 0; }
int PackageManager::gameGlobalInit_t() { return 0; }
int PackageManager::handleCGameTaskPhysics_t(int, CGameTask*, int) { return 0; }
int PackageManager::createExplosion_t(int, int, CGameTask*, int, int, int, int) { return 0; }
int PackageManager::specialImpact_t(CGameTask*, int, int, int, int, int, int, int, int, Constants::SoundIds, Constants::Sprites, int, int) { return 0; }
int PackageManager::writeLandRaw_t(DWORD, DWORD, int, int, int) { return 0; }
int PackageManager::writeLandRadius_t(DWORD, int, int, int) { return 0; }
int PackageManager::writeLandMaskID_t(DWORD, int, int, int) { return 0; }
int PackageManager::weaponRelease_t(CGameTask*, int, int, int, int) { return 0; }
int PackageManager::wormStartFiringWeapon_t(CTaskWorm*) { return 0; }
int PackageManager::fireWeapon_t(CTaskWorm*, CustomWeapons::WeaponStruct*, Weapons::WeaponLaunchParams*) { return 0; }
int PackageManager::createWeaponProjectile_t(CGameTask*, Weapons::WeaponProjectileParams*, Weapons::WeaponLaunchParams*) { return 0; }

int PackageManager::handleCreateExplosion(int posx, int posy, CGameTask *sender, int pushForce, int power, int a6, int team) {
	return Callbacks<&createExplosion_t>::callCallbacks(posx, posy, sender, pushForce, power, a6, team);
}

int PackageManager::handleSpecialImpact(CGameTask *This, int posX, int posY, int radiusX, int radiusY, int power, int pushX, int pushY, int type_a9, Constants::SoundIds soundOnHit, Constants::Sprites spriteOnHit, int collisionMask, int a13) {
	return Callbacks<&specialImpact_t>::callCallbacks(This, posX, posY, radiusX, radiusY, power, pushX, pushY, type_a9, soundOnHit, spriteOnHit, collisionMask, a13);
}

int PackageManager::handleWriteLandRaw(DWORD bitmap, DWORD PCLandscape, int posX, int posY, int layer) {
	return Callbacks<&writeLandRaw_t>::callCallbacks(bitmap, PCLandscape, posX, posY, layer);
}

int PackageManager::handleWriteLandRadius(DWORD PCLandscape, int radius, int posX, int posY) {
	return Callbacks<&writeLandRadius_t>::callCallbacks(PCLandscape, radius, posX, posY);
}

int PackageManager::handleWriteLandMaskID(DWORD PCLandscape, int maskid, int posX, int posY) {
	return Callbacks<&writeLandMaskID_t>::callCallbacks(PCLandscape, maskid, posX, posY);
}

int PackageManager::handleWeaponRelease(CGameTask *This, int posX, int posY, int angleX, int angleY) {
	return Callbacks<&weaponRelease_t>::callCallbacks(This, posX, posY, angleX, angleY);
}

int PackageManager::handleWormStartFiringWeapon(CTaskWorm *worm) {
	return Callbacks<&wormStartFiringWeapon_t>::callCallbacks(worm);
}

int PackageManager::handleFireWeapon(CTaskWorm *worm, CustomWeapons::WeaponStruct *weaponStruct, Weapons::WeaponLaunchParams *launchParams) {
	return Callbacks<&fireWeapon_t>::callCallbacks(worm, weaponStruct, launchParams);
}

int PackageManager::handleCreateWeaponProjectile(CGameTask *This, Weapons::WeaponProjectileParams *projectileParams, Weapons::WeaponLaunchParams *launchParams) {
	return Callbacks<&createWeaponProjectile_t>::callCallbacks(This, projectileParams, launchParams);
}
