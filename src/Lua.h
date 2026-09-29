#ifndef WKJELLYWORM_LUA_H
#define WKJELLYWORM_LUA_H

#include <sol/forward.hpp>
#include <memory>
#include <map>
#include <string>


class Lua {
private:
	sol::state * state;
	Lua();
	static inline std::map<std::string, std::string> knownLibraries;
	static int luaRequire(lua_State * L);
public:
	static void install();
	static Lua & getInstance();

	void resetState();
	sol::state *getState() const;

	std::shared_ptr<sol::environment> spawnEnv(std::string path);

	std::string evalRepl(const std::string &code);

	// Dump globals (functions + usertypes) to jelly/api.txt; returns count of entries written
	int dumpApiToFile(const std::string &path = "jelly\\api.txt");

	static void onConstructGlobalContext(int a1);
};


#endif //WKJELLYWORM_LUA_H
