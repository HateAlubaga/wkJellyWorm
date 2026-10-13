#include "Config.h"
#include <MinHook.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <git.h>
#include "packages/PackageManager.h"

#include "Game.h"
#include "Chat.h"
#include "CustomWeapons.h"
#include "Sounds.h"
#include "Sprites.h"
#include "Music.h"
#include "SchemeBind.h"
#include "JellyRNG.h"

namespace fs = std::filesystem;

const std::string Config::jellyBuildTime = __DATE__ " " __TIME__;

void Config::onConstructGlobalContext(int a1) {
	DWORD addrGameinfoObject = Game::getAddrGameinfoObject();
	char * outputReplayPath = (char*)(addrGameinfoObject + 0xDF60);
	char * inputReplayPath = (char*)(addrGameinfoObject + 0xDB60);
	if(strlen(outputReplayPath) && !strlen(inputReplayPath)) saveConfigReplay(outputReplayPath);
	if(!strlen(outputReplayPath) && strlen(inputReplayPath)) loadConfigReplay(inputReplayPath);
}

void Config::createJellyDirs() {
	auto dirs = {jellyDir, schemeDir, luaLibDir, packageDir, jpxDir, packageDataDir, musicDir, musicCacheDir};
	for (auto & dir : dirs) {
		fs::create_directories(dir);
	}
}

void Config::loadConfigReplay(char *basepath) {
	std::string path(basepath);
	path += schemeExt;
	std::ifstream in(path);
	if (in.good()) {
		std::stringstream buffer;
		buffer << in.rdbuf();
		in.close();
		parseConfigString((char*)buffer.str().c_str());
	}
}

void Config::saveConfigReplay(char *basepath) {
	if(currentConfig.empty()) return;
	std::string path(basepath);
	path += schemeExt;
	std::ofstream out(path);
	if (out.good()) {
		std::string config = getConfigString(4);
		out << config;
		out.close();
	}
}

int Config::saveConfigToFile(std::string & filename) {
	std::string config = getConfigString(4);
	std::string path = schemeDir + "/" + filename + schemeExt;
	std::ofstream out(path);
	if (out.good()) {
		out << config;
		out.close();
		return 0;
	}
	return -1;
}

int Config::loadConfigFromFile(std::string & filename) {
	std::string path = schemeDir + "/" + filename + schemeExt;
	std::ifstream in(path);
	if (in.good()) {
		std::stringstream buffer;
		buffer << in.rdbuf();
		in.close();
		parseConfigString((char*)buffer.str().c_str());
		PackageManager::getInstance().markAllEnabledExplicit();
		SchemeBind::onConfigReset();
		return 0;
	}
	return -1;
}

std::string Config::listConfigFiles() {
	std::string files;
	for (const auto & entry : fs::directory_iterator(schemeDir)) {
		if(!entry.is_regular_file()) continue;
		if(entry.path().extension().string() == schemeExt) {
			files += entry.path().filename().stem().string() + " ";
		}
	}
	return files;
}


bool Config::parseConfigString(char *config) {
	try {
		currentConfig = nlohmann::json::parse(config);
		printf("configstr: %s\n", config);
		if(currentConfig.contains("version") && currentConfig["version"].contains("host")
			&& currentConfig["version"]["host"].contains("versionInt")) {
			int hostVersionInt = currentConfig["version"]["host"]["versionInt"].get<int>();
			if(hostVersionInt != getJellyVersionInt()) {
				std::string hostStr = currentConfig["version"]["host"].value("versionstr", "?");
				std::string msg = "wkJellyWorm: host DLL " + hostStr + ", you have " + getJellyVersionStr();
				if(Game::isValidDdGame())
					Chat::callShowChatMessage(msg, 7);
				printf("%s\n", msg.c_str());
			}
		}
		if(currentConfig.contains("features") && currentConfig["features"].is_object()) {
			auto &feat = currentConfig["features"];
			if(feat.value("supportsHandshakeV2", false)) {
				printf("wkJellyWorm: host handshake v2 features present\n");
			}
		}
		JellyRNG::readFromJson(currentConfig);
		PackageManager::getInstance().handleReadConfig(currentConfig);
	} catch(std::exception & e) {
		printf("parseConfigString:: failed to parse config string: %s\njson config: %s\n", e.what(), config);
		return false;
	}
	return true;
}

std::string Config::buildClientCapsJson() {
	nlohmann::json caps;
	writeVersionToJson(caps, "client");
	PackageManager::writeFeatureFlags(caps);
	if(currentConfig.contains("missingPackages"))
		caps["missingPackages"] = currentConfig["missingPackages"];
	else
		caps["missingPackages"] = nlohmann::json::array();
	caps["rngSeeded"] = JellyRNG::isSeeded();
	caps["rngSeed"] = JellyRNG::getSeed();
	return caps.dump();
}

void Config::handleClientCaps(const std::string &json, const std::string &playerHint) {
	try {
		auto caps = nlohmann::json::parse(json);
		std::string who = playerHint.empty() ? "a client" : playerHint;
		if(caps.contains("version") && caps["version"].contains("client")) {
			std::string ver = caps["version"]["client"].value("versionstr", "?");
			int clientInt = caps["version"]["client"].value("versionInt", 0);
			if(clientInt != getJellyVersionInt()) {
				std::string msg = "wkJellyWorm: " + who + " has DLL " + ver + " (you " + getJellyVersionStr() + ")";
				if(Game::isValidDdGame())
					Chat::callShowChatMessage(msg, 7);
				printf("%s\n", msg.c_str());
			}
		}
		if(caps.contains("missingPackages") && caps["missingPackages"].is_array()) {
			for(auto &m : caps["missingPackages"]) {
				std::string name = m.value("name", "?");
				std::string version = m.value("version", "?");
				std::string msg = "wkJellyWorm: " + who + " is missing " + name + " " + version
					+ " — feature disabled for them";
				if(Game::isValidDdGame())
					Chat::callShowChatMessage(msg, 7);
				printf("%s\n", msg.c_str());
			}
		}
		if(caps.contains("packagesHash") && currentConfig.contains("packagesHash")) {
			try {
				unsigned clientHash = caps["packagesHash"].get<unsigned>();
				unsigned hostHash = currentConfig["packagesHash"].get<unsigned>();
				if(clientHash != hostHash) {
					std::string msg = "wkJellyWorm: " + who + " package set differs from host";
					if(Game::isValidDdGame())
						Chat::callShowChatMessage(msg, 7);
					printf("%s (client=%u host=%u)\n", msg.c_str(), clientHash, hostHash);
				}
			} catch(...) {}
		}
		if(caps.contains("features") && caps["features"].is_object()) {
			auto &feat = caps["features"];
			auto check = [&](const char *flag, const char *label) {
				if(!feat.value(flag, false)) {
					std::string msg = "wkJellyWorm: " + who + " lacks " + label;
					if(Game::isValidDdGame())
						Chat::callShowChatMessage(msg, 7);
					printf("%s\n", msg.c_str());
				}
			};
			check("supportsRNG", "synced RNG");
			check("supportsHandshakeV2", "handshake v2");
			check("supportsEventBus", "event bus");
			check("supportsTimers", "timers");
			check("supportsTaskInspector", "task inspector");
		}
	} catch(std::exception &e) {
		printf("handleClientCaps: %s\n", e.what());
	}
}

void Config::saveSessionConfig() {
	nlohmann::json cfg;
	PackageManager::getInstance().handleWriteConfig(cfg);
	if(!cfg.contains("packages") || cfg["packages"].empty())
		return;
	writeVersionToJson(cfg, "host");
	JellyRNG::writeToJson(cfg);
	std::ofstream out(sessionConfigFile);
	if(out.good()) {
		out << cfg.dump(4);
		out.close();
	}
}

void Config::loadSessionConfig() {
	std::ifstream in(sessionConfigFile);
	if(!in.good())
		return;
	std::stringstream buffer;
	buffer << in.rdbuf();
	in.close();
	std::string content = buffer.str();
	if(content.empty())
		return;
	parseConfigString((char *)content.c_str());
	PackageManager::getInstance().markAllEnabledExplicit();
}

void Config::resetConfig() {
	currentConfig = {};
	PackageManager::getInstance().handleResetConfig();
	JellyRNG::reset();
	CustomWeapons::resetConfig();
	Sounds::resetConfig();
	Sprites::resetConfig();
	Music::resetConfig();
	SchemeBind::onConfigReset();
}

std::string Config::getConfigString(int indent, bool host) {
	if(host) {
		currentConfig = {};
		PackageManager::getInstance().handleWriteConfig(currentConfig);
		writeVersionToJson(currentConfig, "host");
		JellyRNG::writeToJson(currentConfig);
	}
	if(!host)
		writeVersionToJson(currentConfig, "local");
	return currentConfig.dump(indent);
}

int Config::install(SignatureScanner & signatureScanner, module mod) {
	return 0;
}

std::string Config::getJellyVersionMessage() {
	std::stringstream ss;
	ss << "wkJellyWorm v. " << jellyVersionStr << " (built: " << jellyBuildTime << ")";
	if(GitMetadata::Populated()) {
		ss << "\nGit commit: " << GitMetadata::CommitSubject() << " (" << GitMetadata::Describe() << ") " << GitMetadata::CommitDate() << " [ " << GitMetadata::AuthorName() << " - " << GitMetadata::AuthorEmail() << " ]";
		ss << "\n" << (GitMetadata::AnyUncommittedChanges() ? "[Uncommitted changes are present]" : "");
	}
	return ss.str();
}

int Config::getJellyVersionInt() {
	return jellyVersionInt;
}

std::string Config::getJellyVersionStr() {
	return jellyVersionStr;
}

int Config::writeVersionToJson(nlohmann::json &config, std::string type) {
	auto & ver = config["version"][type];
	ver = {
			{"versionstr", jellyVersionStr},
			{"versionInt", jellyVersionInt},
			{"built", jellyBuildTime},
			{"git", {
				{"populated",GitMetadata::Populated()},
				{"commitSubject",GitMetadata::CommitSubject()},
				{"commitDescribe",GitMetadata::Describe()},
				{"commitSha1",GitMetadata::CommitSHA1()},
				{"commitBody",GitMetadata::CommitBody()},
				{"commitDate",GitMetadata::CommitDate()},
				{"authorName",GitMetadata::AuthorName()},
				{"authorEmail",GitMetadata::AuthorEmail()},
				{"uncommittedChanges",GitMetadata::AnyUncommittedChanges()},
			}}
	};
	return 0;
}

void Config::readPrivateProfile() {
	moduleEnabled = GetPrivateProfileIntA("general", "EnableModule", 1, iniFile.c_str());
	useOffsetCache = GetPrivateProfileIntA("general", "UseOffsetCache", 0, iniFile.c_str());
	// 1 = scan jelly/packages recursively on every startup/reload (slower with lots of packages).
	// 0 (default) = only load packages with value != 0 in [autostart];
	// /enable <name> discovers that package from jelly/packages on demand.
	scanAllPackages = GetPrivateProfileIntA("general", "ScanAllPackages", 0, iniFile.c_str());

	lobbyGuiEnabled = GetPrivateProfileIntA("lobby_gui", "EnablePackagePicker", 1, iniFile.c_str());
	logFrontendScreens = GetPrivateProfileIntA("lobby_gui", "LogFrontendScreens", 0, iniFile.c_str());
	interceptSchemeScreen = GetPrivateProfileIntA("lobby_gui", "InterceptSchemeScreen", 0, iniFile.c_str());
	interceptMapScreen = GetPrivateProfileIntA("lobby_gui", "InterceptMapScreen", 0, iniFile.c_str());
	autoInterceptFrontend = GetPrivateProfileIntA("lobby_gui", "AutoInterceptFrontend", 1, iniFile.c_str());

	devConsoleEnabled = GetPrivateProfileIntA("debug", "EnableDevConsole", 0, iniFile.c_str());
	signatureMismatchWarning = GetPrivateProfileIntA("debug", "SignatureMismatchWarning", 0, iniFile.c_str());

	rendererEnabled = GetPrivateProfileIntA("renderer", "EnableCustomRenderer", 0, iniFile.c_str());
	quadRenderEnabled = GetPrivateProfileIntA("renderer", "EnableQuadRender", 1, iniFile.c_str());
	rendererDebugContext = GetPrivateProfileIntA("renderer", "DebugContext", 0, iniFile.c_str());
	rendererGLMajor = GetPrivateProfileIntA("renderer", "GLVersionMajor", 3, iniFile.c_str());
	rendererGLMinor = GetPrivateProfileIntA("renderer", "GLVersionMinor", 2, iniFile.c_str());
	rendererDebugPoints = GetPrivateProfileIntA("renderer", "EnableDebugPoints", 0, iniFile.c_str());
	rendererSkipNativeBlit = GetPrivateProfileIntA("renderer", "SkipNativeBlit", 0, iniFile.c_str());
	rendererFallbackOnContextError = GetPrivateProfileIntA("renderer", "FallbackOnContextError", 1, iniFile.c_str());
	customWeaponIconsEnabled = GetPrivateProfileIntA("general", "CustomWeaponIcons", 0, iniFile.c_str());
	customSpritesEnabled = GetPrivateProfileIntA("general", "CustomSprites", 0, iniFile.c_str());
	pngBitmapLoaderEnabled = GetPrivateProfileIntA("general", "PngBitmapLoader", 0, iniFile.c_str());
	selfTestEnabled = GetPrivateProfileIntA("general", "SelfTest", 0, iniFile.c_str());
	Music::readConfig();
}

namespace JellyConfig {
	bool customWeaponIconsEnabled() {
		return Config::customWeaponIconsEnabled != 0;
	}
}
