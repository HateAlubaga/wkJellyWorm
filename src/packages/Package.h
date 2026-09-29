#ifndef WKJELLYWORM_PACKAGE_H
#define WKJELLYWORM_PACKAGE_H


#include <string>
#include <vector>
#include <json.hpp>
#include <tuple>
#include "../Lua.h"
#include "../Utils.h"
#include <filesystem>

class Package {
public:
	struct Version {
		int major = 0, minor = 0, revision = 0;
		bool operator<(const Version &other) const {
			if(major != other.major) return major < other.major;
			if(minor != other.minor) return minor < other.minor;
			return revision < other.revision;
		}
		void parse(std::string version) {
			std::sscanf(version.c_str(), "%d.%d.%d", &major, &minor, &revision);
		}
		bool operator==(const Version &other) const {
			return major == other.major && minor == other.minor && revision == other.revision;
		}
	};

	bool operator<(const Package &other) const {
		return getVersion() < other.getVersion();
	}
	bool operator == (const Package &other) const {
		return other.getName() == this->getName() && other.getVersionstr() == this->getVersionstr();
	}

	template<typename ReturnType, typename type_t, typename... Args>
	ReturnType callInterface(std::string luaname, Args ... args) {
		try {
			if(!luaEnv)
				throw std::runtime_error("luaEnv is null");
			setActivePackageDir(parentpath);
			auto func = luaEnv->get<type_t>(luaname);
			if(!func)
				throw std::runtime_error("missing lua function: " + luaname);
			ReturnType result = func(args...);
			clearActivePackageDir();
			return result;
		} catch(std::exception & e) {
			clearActivePackageDir();
			reportLuaError(getIdentifier(), e.what());
			return ReturnType{};
		}
	}


private:
	std::filesystem::path parentpath;
	std::filesystem::path mainscript;

	nlohmann::json manifest;
	std::string name;
	std::string versionstr;
	Version version;
	std::shared_ptr<sol::environment> luaEnv;

public:
	Package(std::string path);
	nlohmann::json &getManifest();

	Version &getVersion();
	const Version &getVersion() const;

	const std::string &getName() const;
	const std::string &getVersionstr() const;
	std::string getIdentifier();
	std::string getDescription() const;
	std::string getAuthor() const;
	std::string getMinJellyVersion() const;
	std::vector<std::string> getChatCommands() const;
	std::vector<std::string> getCapabilities() const;

	void enable();
	void disable();
	bool isLoaded() const;
	const std::filesystem::path &getDir() const { return parentpath; }

	static void setActivePackageDir(const std::filesystem::path &dir);
	static void clearActivePackageDir();
	static std::filesystem::path getActivePackageDir();

	bool saveData(const std::string &jsonStr);
	std::string loadData();

private:
	std::filesystem::path dataFilePath() const;
	inline static thread_local std::filesystem::path activePackageDir;
};


#endif //WKJELLYWORM_PACKAGE_H
