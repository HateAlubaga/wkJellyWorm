#include "PxWeaponArt.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "CustomWeapons.h"
#include "Sprites.h"
#include "Utils.h"
#include "packages/Package.h"

static_assert(sizeof(CustomWeapons::WeaponStruct) == 0x1D0, "WeaponStruct must match WA's 0x1D0-byte layout");

namespace {

// ProjectX .bin: 4-byte header, the WeaponStruct bytes, then the trailer.
const size_t kStructOffset = 4;
const size_t kTrailerOffset = 0x1E0;
} // namespace

void PxWeaponArt::log(const char *fmt, ...) {
	static bool firstWriteThisProcess = true;
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	FILE *f = nullptr;
	if(fopen_s(&f, "jelly\\pxweapons.log", firstWriteThisProcess ? "w" : "a") == 0 && f) {
		if(firstWriteThisProcess)
			fprintf(f, "=== pxweapons session pid=%lu tick=%lu ===\n", GetCurrentProcessId(), GetTickCount());
		va_start(args, fmt);
		vfprintf(f, fmt, args);
		va_end(args);
		fclose(f);
	}
	firstWriteThisProcess = false;
}

namespace {

struct Reader {
	const std::vector<unsigned char> &data;
	size_t pos;
	bool ok = true;

	unsigned int u32() {
		if(!ok || pos + 4 > data.size()) { ok = false; return 0; }
		unsigned int v;
		memcpy(&v, &data[pos], 4);
		pos += 4;
		return v;
	}
	unsigned char u8() {
		if(!ok || pos + 1 > data.size()) { ok = false; return 0; }
		return data[pos++];
	}
	std::string str() {
		unsigned int n = u32();
		if(!ok || n > 260 || pos + n > data.size()) { ok = false; return {}; }
		std::string s(data.begin() + pos, data.begin() + pos + n);
		pos += n;
		return s;
	}
};

struct Trailer {
	std::string name1;
	std::string sprites[3]; // for WeaponStruct 0x60, 0x124, 0x170
	std::string aim;        // first of the 9 animation names (aim, "e"/"p" pose)
	bool hasAnims = false;
};

// Two layouts occur between the sprite names and the 9 animation names: 17
// raw bytes (u32 u32 u8 u32 u32), or u32 u32 u8 followed by two strings. Both
// end with 4 flag bytes, which pins down the right one.
bool readAnims(const std::vector<unsigned char> &data, size_t start, bool stringsVariant, std::string &aim) {
	Reader r{data, start};
	r.u32(); r.u32(); r.u8();
	if(stringsVariant) { r.str(); r.str(); }
	else { r.u32(); r.u32(); }
	std::string first;
	for(int i = 0; i < 9 && r.ok; i++) {
		std::string s = r.str();
		if(i == 0) first = s;
	}
	if(!r.ok || r.pos + 4 != data.size())
		return false;
	aim = first;
	return true;
}

bool parseTrailer(const std::vector<unsigned char> &data, Trailer &out) {
	if(data.size() < kTrailerOffset + 8)
		return false;
	Reader r{data, kTrailerOffset};
	r.u32();
	out.name1 = r.str();
	r.str(); // name2
	r.str(); // icon
	r.str(); // gfx dir
	for(auto &s : out.sprites)
		s = r.str();
	if(!r.ok)
		return false;
	out.hasAnims = readAnims(data, r.pos, false, out.aim) || readAnims(data, r.pos, true, out.aim);
	return true;
}

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// How well a .bin's struct bytes explain the struct the Lua registered. Only
// fields that are non-zero on either side count, so two sparse weapons in one
// package do not look alike just because both are mostly zero.
double similarity(const std::vector<unsigned char> &data, const int *lua, size_t structSize) {
	if(data.size() < kStructOffset + structSize)
		return 0;
	int considered = 0, equal = 0;
	for(size_t off = 8; off < structSize; off += 4) {
		int bin;
		memcpy(&bin, &data[kStructOffset + off], 4);
		int mine = lua[off / 4];
		if(!bin && !mine)
			continue;
		considered++;
		if(bin == mine)
			equal++;
	}
	return considered ? (double)equal / considered : 0;
}

bool sameAssetName(const std::string &a, const std::string &b) {
	auto norm = [](const std::string &s) {
		std::string out;
		for(unsigned char c : s) {
			char n = std::isalnum(c) ? (char)std::tolower(c) : '_';
			if(n != '_' || out.empty() || out.back() != '_')
				out += n;
		}
		return out;
	};
	return norm(a) == norm(b);
}

// ProjectX libraries such as cweps keep their art in a companion "<name>_gfx"
// library (cweps_gfx), which px2jelly converted into a separate package.
std::string findInGfxCompanion(const std::filesystem::path &packageDir, const std::string &file) {
	namespace fs = std::filesystem;
	std::string bare = fs::path(file).filename().string();
	fs::path companion = packageDir.parent_path().parent_path() / (packageDir.parent_path().filename().string() + "_gfx");
	std::error_code ec;
	for(fs::directory_iterator ver(companion, fs::directory_options::skip_permission_denied, ec), end;
		!ec && ver != end; ver.increment(ec)) {
		std::error_code ec2;
		for(fs::directory_iterator it(ver->path() / "assets", fs::directory_options::skip_permission_denied, ec2), end2;
			!ec2 && it != end2; it.increment(ec2)) {
			if(it->is_regular_file(ec2) && !ec2 && sameAssetName(it->path().filename().string(), bare))
				return it->path().string();
		}
	}
	return {};
}

std::string resolveExisting(const std::filesystem::path &packageDir, const std::string &file) {
	if(file.empty())
		return {};
	std::string resolved = Utils::resolveAssetPath(file);
	std::error_code ec;
	if(std::filesystem::is_regular_file(resolved, ec) && !ec)
		return resolved;
	return findInGfxCompanion(packageDir, file);
}

} // namespace

bool PxWeaponArt::find(const int *fields, size_t structSize, const std::string &name1, Art &art) {
	namespace fs = std::filesystem;
	art = Art();
	auto packageDir = Package::getActivePackageDir();
	if(packageDir.empty())
		return false;
	fs::path rawDir = packageDir / "weapons_raw";
	std::error_code ec;
	if(!fs::is_directory(rawDir, ec) || ec)
		return false;

	fs::path bestPath;
	Trailer bestTrailer;
	double bestScore = -1;
	for(fs::directory_iterator it(rawDir, fs::directory_options::skip_permission_denied, ec), end;
		!ec && it != end; it.increment(ec)) {
		std::error_code ec2;
		if(!it->is_regular_file(ec2) || ec2 || lower(it->path().extension().string()) != ".bin")
			continue;
		std::ifstream in(it->path(), std::ios::binary);
		std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		Trailer trailer;
		if(!parseTrailer(data, trailer))
			continue;
		double score = similarity(data, fields, structSize);
		// Names differ slightly after px2jelly ("Baseball Bomb" vs "Baseball
		// Bomb Wep"); only use them to break near-ties.
		if(!trailer.name1.empty() && lower(name1).find(lower(trailer.name1)) == 0)
			score += 0.01;
		if(score > bestScore) {
			bestScore = score;
			bestPath = it->path();
			bestTrailer = trailer;
		}
	}
	if(bestScore < 0.85) {
		if(bestScore >= 0)
			PxWeaponArt::log("%s: no confident weapons_raw match (best %.2f '%s'); art left as registered\n",
				name1.c_str(), bestScore, bestPath.filename().string().c_str());
		return false;
	}
	PxWeaponArt::log("%s: matched %s (%.2f)\n", name1.c_str(), bestPath.filename().string().c_str(), bestScore);

	for(int i = 0; i < 3; i++) {
		if(fields[kSlotOffsets[i] / 4] != kSpriteFromFile)
			continue;
		const std::string &file = bestTrailer.sprites[i];
		art.slotFiles[i] = resolveExisting(packageDir, file);
		if(art.slotFiles[i].empty())
			PxWeaponArt::log("%s: slot 0x%X wants trailer sprite '%s', which is in neither the package nor a _gfx companion\n",
				name1.c_str(), (unsigned)kSlotOffsets[i], file.c_str());
	}
	if(bestTrailer.hasAnims) {
		art.aimFile = resolveExisting(packageDir, bestTrailer.aim);
		if(art.aimFile.empty() && !bestTrailer.aim.empty())
			PxWeaponArt::log("%s: aim image '%s' not found\n", name1.c_str(), bestTrailer.aim.c_str());
	}
	return true;
}

// Lives here rather than in CustomWeapons.cpp, which must not include
// <fstream> (see the note at the top of that file).
//
// Runs once all packages have registered their weapons and sprites, so the
// package Lua keeps first claim on the 173 custom sprite ids. Projectiles go
// first; aim poses only get ids that are left, because a missing aim pose
// still has the bazooka fallback while a missing projectile is invisible.
void CustomWeapons::assignPxArt() {
	const int reserve = 4; // SetSpriteOverride needs a free scratch id
	int projectiles = 0, aims = 0, skipped = 0;
	for(auto &entry : pxArt) {
		int index = entry.first - maxStandardWeapons;
		if(index < 0 || index >= (int)weaponStructInjectionList.size())
			continue;
		int *fields = reinterpret_cast<int *>(&weaponStructInjectionList[index]);
		for(int i = 0; i < 3; i++) {
			const std::string &file = entry.second.slotFiles[i];
			int &slot = fields[PxWeaponArt::kSlotOffsets[i] / 4];
			if(file.empty() || slot != PxWeaponArt::kSpriteFromFile)
				continue;
			int id = Sprites::registerCustomSpriteWithReserve(file, reserve);
			if(id > 0) {
				slot = id;
				projectiles++;
			} else
				skipped++;
			PxWeaponArt::log("weapon %d slot 0x%X -> sprite %d ('%s')\n",
				entry.first, (unsigned)PxWeaponArt::kSlotOffsets[i], id, file.c_str());
		}
	}
	for(auto &entry : pxArt) {
		const std::string &file = entry.second.aimFile;
		if(file.empty())
			continue;
		int id = Sprites::registerCustomSpriteWithReserve(file, reserve);
		if(id > 0) {
			customAimSprites[entry.first] = id;
			aims++;
		} else
			skipped++;
		PxWeaponArt::log("weapon %d aim -> sprite %d ('%s')\n", entry.first, id, file.c_str());
	}
	PxWeaponArt::log("assigned %d projectile and %d aim sprites, %d skipped for lack of ids; %d custom ids left\n",
		projectiles, aims, skipped, Sprites::customSpriteIdsLeft());
}
