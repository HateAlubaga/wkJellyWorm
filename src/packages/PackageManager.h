#ifndef WKJELLYWORM_PACKAGEMANAGER_H
#define WKJELLYWORM_PACKAGEMANAGER_H


#include <string>
#include <map>
#include <set>
#include <vector>
#include "../entities/CGameTask.h"
#include "../entities/gametasks/CTaskWorm.h"
#include "../CustomWeapons.h"
#include "../Weapons.h"
#include "Package.h"


class PackageManager {
private:
	std::map<std::string, std::shared_ptr<Package>> packages;
	std::map<std::string, std::set<std::shared_ptr<Package>>> allversions;
	std::map<std::string, std::shared_ptr<Package>> jpxPackages;
	std::map<std::string, std::set<std::shared_ptr<Package>>> jpxAllversions;
	std::map<std::string, std::shared_ptr<Package>> enabledPackagesAll;
	std::map<std::string, std::shared_ptr<Package>> enabledPackagesExplicit;
	std::set<std::string> initializedPackages;
	bool gameInitialized = false;

	void ensureAutostartDefaultsInIni();
	bool tryAddPackageFromDisk(const std::string &name, const std::string &version = "latest");

	PackageManager();
	void addPackage(std::string path, bool showError = true);
	void addJpxPackage(std::string path);
	void handleLobbyListPackages(void * lobbyObj, bool all, bool manual, bool dependencies);
	bool autoEnableForCommand(const std::string &command, bool manual);
	void ensurePackageInitialized(const std::shared_ptr<Package> &pack);
	void ensureAllEnabledInitialized();
	static std::string extractChatCommand(const char *msg);
	std::shared_ptr<Package> findPackageVersion(const std::string &name, const std::string &version, bool includeJpx) const;
	std::vector<std::string> validateJpxDependencies(const std::string &name) const;

public:
	static std::string commandNameFromMessage(const char *msg);
	static bool isChatRoutableLobbyCommand(const std::string &command);

	struct PackageGuiEntry {
		std::string name;
		std::string version;
		std::string description;
		std::string author;
		bool enabled = false;
	};

	struct CommandPackageRef {
		std::string name;
		std::string version;
	};

	// package callbacks - packages can register their functions as callbacks for various game events
	template<auto value> struct Callbacks;
	template<typename... Args, int(*ptr)(Args...)>
	struct Callbacks<ptr> {
		using callback_t = std::function<int(Args...)>;
		static inline std::vector<callback_t> callbacks;
		static void registerCallback(callback_t cb) {
			callbacks.push_back(cb);
		}
		static void clearCallbacks() {
			callbacks.clear();
		}
		template<typename ...Args>
		static int callCallbacks(Args... args) {
			int ret = 0;
			for(auto & cb : callbacks) {
				try {
					ret |= cb(args...);
				} catch (const std::exception & e) {
					reportLuaError("callCallbacks", e.what());
				}
			}
			return ret;
		}
	};
	// package interface - all packages must implement specified functions
	template<auto value> struct Interface;
	template<typename... Args, int(*ptr)(Args...)>
	struct Interface<ptr> {
		using callback_t = std::function<int(Args...)>;
		template<typename ...Args>
		static int callAllPackages(std::string name, Args... args) {
			int ret=0;
			for(auto & package : getInstance().enabledPackagesAll) {
				ret |= package.second->callInterface<int, callback_t>(name, args...);
			}
			return ret;
		}
	};


	static PackageManager & getInstance();
	// Appends to jelly\selftest.log; the first write of each process
	// truncates it. Always on: it is the only record of which package was
	// loading when WA died, and costs one small file write per package.
	static void loadLog(const char *fmt, ...);
	void scanPackages();
	void scanJpxPackages();
	void enablePackage(std::string name, std::string version, bool manual);
	void disablePackage(std::string name, bool manual);
	void checkDependenciesAdd();
	void checkDependenciesRemove();
	std::shared_ptr<Package> getLatestVersion(std::string package) const;
	std::vector<std::string> listEnabledPackageIds() const;
	std::vector<PackageGuiEntry> listPackagesForGui() const;
	void togglePackageFromGui(void *lobbyObj, const std::string &name, const std::string &version, bool enable);
	void markAllEnabledExplicit();



	static int handleCGameTaskPhysics_t(int, CGameTask*, int);
	static int gameGlobalInit_t();
	static int createExplosion_t(int posx, int posy, CGameTask * sender, int pushForce, int power, int a6, int team);
	static int specialImpact_t(CGameTask *This, int posX, int posY, int radiusX, int radiusY, int power, int pushX, int pushY, int type_a9, Constants::SoundIds soundOnHit, Constants::Sprites spriteOnHit, int collisionMask, int a13);

	static int writeLandRaw_t(DWORD bitmap, DWORD PCLandscape, int posX, int posY, int layer);
	static int writeLandRadius_t(DWORD PCLandscape, int radius, int posX, int posY);
	static int writeLandMaskID_t(DWORD PCLandscape, int maskid, int posX, int posY);

	static int weaponRelease_t(CGameTask * This, int posX, int posY, int angleX, int angleY);
	static int wormStartFiringWeapon_t(CTaskWorm * worm);
	static int fireWeapon_t(CTaskWorm * worm, CustomWeapons::WeaponStruct * weaponStruct, Weapons::WeaponLaunchParams * launchParams);
	static int createWeaponProjectile_t(CGameTask * This, Weapons::WeaponProjectileParams * projectileParams, Weapons::WeaponLaunchParams * launchParams);
	static int onFrame_t();
	static int onTurnStart_t();


	int handleCGameTaskPhysics(int a1, CGameTask * object, int a3);
	int handleGameGlobalInit();
	int handleCreateExplosion(int posx, int posy, CGameTask * sender, int pushForce, int power, int a6, int team);
	int handleSpecialImpact(CGameTask *This, int posX, int posY, int radiusX, int radiusY, int power, int pushX, int pushY, int type_a9, Constants::SoundIds soundOnHit, Constants::Sprites spriteOnHit, int collisionMask, int a13);
	int handleWriteLandRaw(DWORD bitmap, DWORD PCLandscape, int posX, int posY, int layer);
	int handleWriteLandRadius(DWORD PCLandscape, int radius, int posX, int posY);
	int handleWriteLandMaskID(DWORD PCLandscape, int maskid, int posX, int posY);

	int handleWeaponRelease(CGameTask * This, int posX, int posY, int angleX, int angleY);
	int handleWormStartFiringWeapon(CTaskWorm * worm);
	int handleFireWeapon(CTaskWorm * worm, CustomWeapons::WeaponStruct * weaponStruct, Weapons::WeaponLaunchParams * launchParams);
	int handleCreateWeaponProjectile(CGameTask * This, Weapons::WeaponProjectileParams * projectileParams, Weapons::WeaponLaunchParams * launchParams);


	void clearCallbacks();
	int reloadPackages(void *lobbyObj = nullptr);
	int applyAutostart();
	int handleAutostartCommand(void *lobbyObj, const std::string &args);
	int handleJpxCommand(void *lobbyObj, const std::string &args);


	static int lobbyHostCli_t(void *, std::string&, std::string&);
	static int lobbyClientCli_t(void *, std::string&, std::string&);
	static int readConfig_t(std::string&);
	static int writeConfig_t();
	static int resetConfig_t();
	static int printLobbyHelpCommand_t(void *);
	static int onChatInput_t(char *msg);
	static int initialize_t();

	int handleLobbyHostCommand(void *lobbyObj, std::string &command, std::string &args);
	int handleLobbyClientCommand(void *lobbyObj, std::string &command, std::string &args);
	int handlePackageChatCommand(void *lobbyObj, const std::string &command, const std::string &args);

	int handleReadConfig(nlohmann::json & config);
	int handleWriteConfig(nlohmann::json & config);
	int handleResetConfig();
	int handleInitialize();
	int handleOnChatInput(char * msg);

	static void writeFeatureFlags(nlohmann::json & config);
	bool findCommandPackage(const std::string &command, CommandPackageRef &out) const;
	bool resolveNameAlias(std::string &name, std::string &version) const;
};

#endif //WKJELLYWORM_PACKAGEMANAGER_H
