#ifndef WKJELLYWORM_CONFIG_H
#define WKJELLYWORM_CONFIG_H

#include <sigscanner.h>
#include <json.hpp>


class Config {
private:
	static void saveConfigReplay(char * basepath);
	static void loadConfigReplay(char * basepath);

	inline static nlohmann::json currentConfig;

	inline static const std::string jellyVersionStr = "1.2.0";
	inline static const int jellyVersionInt = 0x00010200;
	static const std::string jellyBuildTime;

public:
	inline static const std::string jellyDir = "jelly";
	inline static const std::string schemeDir = jellyDir + "\\schemes";
	inline static const std::string schemeExt = ".jelly";
	inline static const std::string luaLibDir = jellyDir + "\\lualibs";
	inline static const std::string packageDir = jellyDir + "\\packages";
	inline static const std::string jpxDir = jellyDir + "\\jpx";
	inline static const std::string packageDataDir = jellyDir + "\\data";
	inline static const std::string musicCacheDir = jellyDir + "\\cache\\music";
	inline static const std::string sessionConfigFile = jellyDir + "\\session.jelly";
	inline static const std::string iniFile = ".\\wkJellyWorm.ini";
	inline static const std::string offsetsFile = jellyDir +"\\offsetcache.json";



	static void onConstructGlobalContext(int a1);

	static std::string getConfigString(int indent=-1, bool host=false);
	static bool parseConfigString(char *config);
	static std::string buildClientCapsJson();
	static void handleClientCaps(const std::string &json, const std::string &playerHint = "");
	static int install(SignatureScanner &, module);
	static void resetConfig();
	static void saveSessionConfig();
	static void loadSessionConfig();

	static std::string listConfigFiles();
	static int saveConfigToFile(std::string & filename);
	static int loadConfigFromFile(std::string & filename);

	static std::string getJellyVersionMessage();
	static int getJellyVersionInt();
	static std::string getJellyVersionStr();
	static int writeVersionToJson(nlohmann::json & config, std::string type);

	inline static int devConsoleEnabled = 0;
	inline static int moduleEnabled = 0;
	inline static int useOffsetCache = 0;
	inline static int signatureMismatchWarning = 0;
	inline static int scanAllPackages = 1;

	inline static int lobbyGuiEnabled = 1;
	inline static int logFrontendScreens = 0;
	inline static int interceptSchemeScreen = 0;
	inline static int interceptMapScreen = 0;
	inline static int autoInterceptFrontend = 1;

	inline static int rendererEnabled = 0;
	inline static int rendererDebugContext = 0;
	inline static int rendererGLMajor = 0;
	inline static int rendererGLMinor = 0;
	inline static int rendererDebugPoints = 0;
	inline static int rendererSkipNativeBlit = 0;
	inline static int rendererFallbackOnContextError = 1;
	// Emergency crash isolation switches. Keep both disabled until bitmap paths
	// are proven safe against the WA prepare/remap code.
	inline static int customWeaponIconsEnabled = 0;
	inline static int customSpritesEnabled = 0;
	// The ported ProjectX packages ship 3371 PNGs and pass .png straight to
	// registerCustomSprite (1105 call sites), so the PNG branch of
	// Bitmap::loadImageFile is what makes that art usable at all. It used to be
	// a compile-time constant; as an ini switch it can be turned back off
	// without a rebuild if the WA prepare/remap path misbehaves.
	inline static int pngBitmapLoaderEnabled = 0;
	// Applies [autostart] on the main menu too, so an automated launch can
	// load and exercise the package layer without a human entering a match.
	inline static int selfTestEnabled = 0;

	static void readPrivateProfile();
	static void createJellyDirs();
};


#endif //WKJELLYWORM_CONFIG_H
