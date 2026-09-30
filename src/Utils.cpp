#include <stdexcept>
#include "Utils.h"
#include "WaLibc.h"
#include "Chat.h"
#include "Game.h"
#include "packages/Package.h"
#include <Windows.h>
#include <cstdio>
#include <map>
#include <filesystem>
#include <algorithm>
#include <cctype>


//https://stackoverflow.com/questions/18675364/c-tokenize-a-string-with-spaces-and-quotes
int Utils::split_in_args(std::vector<std::string>& qargs, std::string command) {
	int len = command.length();
	bool qot = false, sqot = false;
	int arglen;
	for(int i = 0; i < len; i++) {
		int start = i;
		if(command[i] == '\"') {
			qot = true;
		}
		else if(command[i] == '\'') sqot = true;

		if(qot) {
			i++;
			start++;
			while(i<len && command[i] != '\"')
				i++;
			if(i<len)
				qot = false;
			arglen = i-start;
			i++;
		}
		else if(sqot) {
			i++;
			while(i<len && command[i] != '\'')
				i++;
			if(i<len)
				sqot = false;
			arglen = i-start;
			i++;
		}
		else{
			while(i<len && command[i]!=' ')
				i++;
			arglen = i-start;
		}
		qargs.push_back(command.substr(start, arglen));
	}
	if(qot || sqot) return -1;
	return 0;
}

std::string Utils::resolveAssetPath(const std::string &filename) {
	namespace fs = std::filesystem;
	if(filename.empty())
		return filename;
	std::error_code ec;
	if(fs::is_regular_file(filename, ec) && !ec)
		return filename;
	auto packageDir = Package::getActivePackageDir();
	if(packageDir.empty())
		return filename;
	// A package sometimes references a subfolder that only existed in the
	// original ProjectX source tree ("gfx/foo.png", "sprites/foo.png"), while
	// the extracted art actually sits flat in this package's own assets/ --
	// so also try just the bare filename there.
	fs::path bare = fs::path(filename).filename();
	fs::path candidates[] = {
		packageDir / filename,
		packageDir / "assets" / filename,
		packageDir / "assets" / bare,
	};
	for(auto & c : candidates) {
		ec.clear();
		if(fs::is_regular_file(c, ec) && !ec)
			return c.string();
	}
	// px2jelly sometimes kept the original attachment spelling in Lua while
	// sanitising the extracted filename: "a - baseball ( e ).png" was saved as
	// "a_-_baseball___e__.png", "hivefire(d).png" as "hivefire_d_.png". Every
	// non-alphanumeric byte therefore compares as '_'. Accept only one
	// unambiguous normalised match inside this package.
	auto normaliseAssetName = [](std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
			if(!std::isalnum(ch)) return '_';
			return (char)std::tolower(ch);
		});
		value.erase(std::unique(value.begin(), value.end(), [](char a, char b) {
			return a == '_' && b == '_';
		}), value.end());
		return value;
	};
	fs::path assetsDir = packageDir / "assets";
	std::string wanted = normaliseAssetName(bare.string());
	fs::path fuzzyMatch;
	int matches = 0;
	ec.clear();
	for(fs::directory_iterator it(assetsDir, fs::directory_options::skip_permission_denied, ec), end;
		!ec && it != end; it.increment(ec)) {
		if(!it->is_regular_file(ec) || ec) continue;
		if(normaliseAssetName(it->path().filename().string()) == wanted) {
			fuzzyMatch = it->path();
			matches++;
		}
	}
	if(matches == 1) return fuzzyMatch.string();
	return filename;
}

std::string Utils::findUniquePackageIcon() {
	namespace fs = std::filesystem;
	auto packageDir = Package::getActivePackageDir();
	if(packageDir.empty()) return {};
	fs::path match;
	int matches = 0;
	std::error_code ec;
	fs::path assetsDir = packageDir / "assets";
	for(fs::directory_iterator it(assetsDir, fs::directory_options::skip_permission_denied, ec), end;
		!ec && it != end; it.increment(ec)) {
		if(!it->is_regular_file(ec) || ec) continue;
		std::string name = it->path().filename().string();
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
			return (char)std::tolower(ch);
		});
		std::string ext = it->path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
			return (char)std::tolower(ch);
		});
		bool image = ext == ".png" || ext == ".img" || ext == ".bmp";
		bool iconLike = name.find("ico") != std::string::npos || name.find("icon") != std::string::npos;
		if(image && iconLike) {
			match = it->path();
			matches++;
		}
	}
	return matches == 1 ? match.string() : std::string();
}

void * Utils::readFileToWaBuffer(std::string filename) {
	FILE * pf = fopen(filename.c_str(), "rb");
	if(!pf) throw std::runtime_error("Failed to load file: " + filename);

	fseek(pf, 0, SEEK_END);
	size_t filesize = ftell(pf);
	rewind(pf);
	void * buffer = WaLibc::waMalloc(filesize);
	if(!buffer) { fclose(pf); throw std::runtime_error("Failed to load filename: " + filename + " - Failed to allocate memory: " + std::to_string(filesize)); }
	size_t readsize = fread(buffer, 1, filesize,pf);
	if(readsize != filesize) {
		fclose(pf);
		WaLibc::waFree(buffer);
		throw std::runtime_error("Failed to read filename completely: filename: " + filename + " size: " + std::to_string(filesize) + " read: " + std::to_string(readsize));
	}
	fclose(pf);

	return buffer;
}

void reportLuaError(const std::string &source, const std::string &what) {
	printf("Lua error [%s]: %s\n", source.c_str(), what.c_str());
	static std::map<std::string, DWORD> lastNotifyTick;
	DWORD now = GetTickCount();
	auto it = lastNotifyTick.find(source);
	if(it != lastNotifyTick.end() && (now - it->second) < 10000)
		return;
	lastNotifyTick[source] = now;
	if(Game::isValidDdGame()) {
		std::string msg = "wkJellyWorm Lua [" + source + "]: " + what;
		if(msg.size() > 200)
			msg = msg.substr(0, 200) + "...";
		Chat::callShowChatMessage(msg, 7);
	}
}