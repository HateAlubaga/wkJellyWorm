#include "Package.h"

#include <fstream>
#include <json.hpp>
#include <filesystem>
#include "../Lua.h"
#include "../Config.h"
#include <sol/sol.hpp>

namespace fs = std::filesystem;

static constexpr size_t kMaxPackageDataBytes = 256 * 1024;

Package::Package(std::string manifestname) : version({0, 0, 0}){
	std::ifstream ifs(manifestname);
	manifest = nlohmann::json::parse(ifs);

	static std::vector<std::string> requiredFields = {"name", "description", "version", "author", "scripts", "sprites", "imgs", "dependencies"};
	std::string missingFields;
	for(auto & field : requiredFields) {
		if(manifest.find(field) == manifest.end())
			missingFields+= field + " ";
	}
	if(!missingFields.empty())
		throw std::runtime_error("Package manifest is missing fields: " + missingFields);

	name = manifest["name"];
	versionstr= manifest["version"];
	version.parse(versionstr);

	if(manifest["scripts"].empty())
		throw std::runtime_error("Package " + name + " does not include any lua scripts");

	std::string missingFiles;
	parentpath = fs::path(manifestname).parent_path();
	static std::vector<std::string> checkFiles = {"scripts", "sprites", "imgs"};
	for(auto & check : checkFiles) {
		for(std::string filename : manifest[check]) {
			auto path = parentpath / filename;
			if(!fs::exists(path))
				missingFiles += "\n" + filename;
		}
	}
	if(!missingFiles.empty())
		throw std::runtime_error("Package " + name + " is missing files: " + missingFiles);

	mainscript = parentpath / std::string(manifest["scripts"][0]);
}

nlohmann::json &Package::getManifest() {
	return manifest;
}

Package::Version &Package::getVersion()  {
	return version;
}

const Package::Version &Package::getVersion() const {
	return version;
}

const std::string &Package::getName() const {
	return name;
}

const std::string &Package::getVersionstr() const {
	return versionstr;
}

std::string Package::getIdentifier() {
	return name + ":" + versionstr;
}

std::string Package::getDescription() const {
	try {
		if(manifest.contains("description"))
			return manifest["description"].get<std::string>();
	} catch(...) {}
	return "";
}

std::string Package::getAuthor() const {
	try {
		if(manifest.contains("author"))
			return manifest["author"].get<std::string>();
	} catch(...) {}
	return "";
}

std::string Package::getMinJellyVersion() const {
	try {
		if(manifest.contains("minJellyVersion"))
			return manifest["minJellyVersion"].get<std::string>();
	} catch(...) {}
	return "";
}

std::vector<std::string> Package::getChatCommands() const {
	std::vector<std::string> cmds;
	try {
		if(!manifest.contains("chatCommands") || !manifest["chatCommands"].is_array())
			return cmds;
		for(auto &c : manifest["chatCommands"]) {
			if(c.is_string()) {
				std::string s = c.get<std::string>();
				if(!s.empty())
					cmds.push_back(s);
			}
		}
	} catch(...) {}
	return cmds;
}

std::vector<std::string> Package::getCapabilities() const {
	std::vector<std::string> caps;
	try {
		if(!manifest.contains("capabilities") || !manifest["capabilities"].is_array())
			return caps;
		for(auto &c : manifest["capabilities"]) {
			if(c.is_string()) {
				std::string s = c.get<std::string>();
				if(!s.empty())
					caps.push_back(s);
			}
		}
	} catch(...) {}
	return caps;
}

fs::path Package::dataFilePath() const {
	return fs::path(Config::packageDataDir) / (name + ".json");
}

bool Package::saveData(const std::string &jsonStr) {
	if(jsonStr.size() > kMaxPackageDataBytes) {
		printf("Package(%s)::saveData: payload too large (%zu > %zu)\n",
			name.c_str(), jsonStr.size(), kMaxPackageDataBytes);
		return false;
	}
	try {
		fs::create_directories(Config::packageDataDir);
		auto path = dataFilePath();
		auto tmp = path;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if(!out)
				return false;
			out.write(jsonStr.data(), (std::streamsize)jsonStr.size());
			if(!out)
				return false;
		}
		std::error_code ec;
		fs::rename(tmp, path, ec);
		if(ec) {
			fs::remove(path, ec);
			fs::rename(tmp, path, ec);
			if(ec)
				return false;
		}
		return true;
	} catch(std::exception & e) {
		printf("Package(%s)::saveData exception: %s\n", name.c_str(), e.what());
		return false;
	}
}

std::string Package::loadData() {
	try {
		auto path = dataFilePath();
		if(!fs::exists(path))
			return "";
		auto size = fs::file_size(path);
		if(size > kMaxPackageDataBytes) {
			printf("Package(%s)::loadData: file too large (%llu)\n",
				name.c_str(), (unsigned long long)size);
			return "";
		}
		std::ifstream in(path, std::ios::binary);
		if(!in)
			return "";
		std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		return data;
	} catch(std::exception & e) {
		printf("Package(%s)::loadData exception: %s\n", name.c_str(), e.what());
		return "";
	}
}

void Package::enable() {
	auto * state = Lua::getInstance().getState();
	luaEnv = std::make_shared<sol::environment>(*state, sol::create, state->globals());
	auto & env = *luaEnv;
	static std::vector<std::string> unsafeFuncs = {
		"collectgarbage", "dofile", "getfenv", "getmetatable", "load", "loadfile", "loadstring",
		"rawequal", "rawget", "rawset", "setfenv", "setmetatable", "coroutine", "module", "package",
		"require", "package.loaded", "package.loaders", "package.loadlib", "package.path",
		"package.cpath", "package.preload", "package.seeall", "table", "io", "os", "os.date",
		"os.execute", "os.exit", "os.getenv", "os.remove", "os.rename", "os.setlocale",
		"os.tmpname", "debug", "newproxy"
	};
	for(auto & func : unsafeFuncs) {
		env[func] = nullptr;
	}
	env["saveData"] = [this](const std::string &jsonStr) { return saveData(jsonStr); };
	env["loadData"] = [this]() { return loadData(); };
	setActivePackageDir(parentpath);
	try {
		state->script_file(mainscript.string(), env);
	} catch(...) {
		clearActivePackageDir();
		throw;
	}
	clearActivePackageDir();
}

void Package::setActivePackageDir(const std::filesystem::path &dir) {
	activePackageDir = dir;
}

void Package::clearActivePackageDir() {
	activePackageDir.clear();
}

std::filesystem::path Package::getActivePackageDir() {
	return activePackageDir;
}

void Package::disable() {
	luaEnv = nullptr;
}

bool Package::isLoaded() const {
	return luaEnv != nullptr;
}
