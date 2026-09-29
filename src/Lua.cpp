#include "Lua.h"
#include "Config.h"
#include <lua/lua.hpp>
#include <sol/sol.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <utility>

std::vector<std::string> unsafeFuncs = {"collectgarbage", "dofile", "getfenv", "getmetatable", "load", "loadfile", "loadstring", "rawequal", "rawget", "rawset", "setfenv", "setmetatable", "coroutine", "module", "package", "require", "package.loaded", "package.loaders", "package.loadlib", "package.path", "package.cpath", "package.preload", "package.seeall", "table", "io", "os", "os.date", "os.execute", "os.exit", "os.getenv", "os.remove", "os.rename", "os.setlocale", "os.tmpname", "debug", "newproxy"};

namespace fs = std::filesystem;

Lua::Lua() : state(new sol::state()) {
	state->open_libraries(sol::lib::base,
					   sol::lib::package,
					   sol::lib::string,
					   sol::lib::math,
					   sol::lib::io,
					   sol::lib::table,
					   sol::lib::os,
					   sol::lib::bit32,
					   sol::lib::debug);

	(*state)["package"]["searchers"] = state->create_table_with(1, luaRequire);

	for (const auto & entry : fs::directory_iterator(Config::luaLibDir)) {
		if(!entry.is_regular_file()) continue;
		if(entry.path().extension().string() == ".lua") {
			std::string name = entry.path().stem().string();
			// Ported packages require() lualibs with inconsistent casing
			// (e.g. "Utils" vs "utils.lua" on disk); match case-insensitively.
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			knownLibraries[name] = entry.path().string();
		}
	}

}

Lua &Lua::getInstance() {
	static Lua instance;
	return instance;
}

sol::state *Lua::getState() const {
	return state;
}

void Lua::install() {

}

void Lua::onConstructGlobalContext(int a1) {

}

std::shared_ptr<sol::environment> Lua::spawnEnv(std::string path) {
	auto ptr = std::make_shared<sol::environment>(*state, sol::create, state->globals());
	auto & env = *ptr;
	for(auto & func : unsafeFuncs) {
		env[func] = nullptr;
	}
	state->script_file(path, env);
	return ptr;
}

std::string Lua::evalRepl(const std::string &code) {
	if(code.empty())
		return "";
	sol::protected_function_result result = state->safe_script(code, sol::script_pass_on_error);
	if(!result.valid()) {
		sol::error err = result;
		return std::string(err.what());
	}
	if(result.return_count() == 0)
		return "ok";
	sol::object obj = result.get<sol::object>(0);
	if(obj.is<std::string>())
		return obj.as<std::string>();
	if(obj.is<bool>())
		return obj.as<bool>() ? "true" : "false";
	if(obj.is<double>()) {
		std::ostringstream ss;
		ss << obj.as<double>();
		return ss.str();
	}
	if(obj.is<int>())
		return std::to_string(obj.as<int>());
	if(obj == sol::nil)
		return "nil";
	return "ok";
}

int Lua::dumpApiToFile(const std::string &path) {
	std::ofstream out(path, std::ios::trunc);
	if(!out)
		return -1;
	int count = 0;
	out << "wkJellyWorm Lua API dump\n";
	out << "version: " << Config::getJellyVersionStr() << "\n\n";

	sol::table globals = state->globals();
	std::vector<std::pair<std::string, std::string>> entries;
	for(auto & kv : globals) {
		sol::object key = kv.first;
		sol::object value = kv.second;
		if(!key.is<std::string>())
			continue;
		std::string name = key.as<std::string>();
		if(name.empty() || name[0] == '_')
			continue;
		std::string kind;
		if(value.is<sol::function>())
			kind = "function";
		else if(value.get_type() == sol::type::userdata)
			kind = "userdata";
		else if(value.is<sol::table>()) {
			// usertype registries often appear as tables with metatable
			sol::table t = value.as<sol::table>();
			sol::object mt = t[sol::metatable_key];
			if(mt.valid())
				kind = "table/usertype";
			else
				continue;
		} else {
			continue;
		}
		entries.emplace_back(name, kind);
	}
	std::sort(entries.begin(), entries.end());
	for(auto & e : entries) {
		out << e.second << "\t" << e.first << "\n";
		count++;
	}
	out << "\nTotal: " << count << "\n";
	return count;
}

int Lua::luaRequire(lua_State *L) {
	std::string name = sol::stack::get<std::string>(L);
	std::transform(name.begin(), name.end(), name.begin(), ::tolower);
	std::string path;
	if (knownLibraries.find(name) != knownLibraries.end()) {
		path = knownLibraries[name];
	} else {
		//todo: find in enabled packages
	}
	if(!path.empty()) {
		std::ifstream in(path);
		std::stringstream buffer;
		buffer << in.rdbuf();
		in.close();
		std::string script = std::move(buffer.str());
		luaL_loadbuffer(L, script.data(), script.size(), path.c_str());
		return 1;
	} else {
		sol::stack::push(L, "These are not the modules you're looking for!");
		return 1;
	}
}
