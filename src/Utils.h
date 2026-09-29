#ifndef WKJELLYWORM_UTILS_H
#define WKJELLYWORM_UTILS_H


#include <vector>
#include <string>

class Utils {
public:
	static int split_in_args(std::vector<std::string> &qargs, std::string command);
	static void * readFileToWaBuffer(std::string filename);
	// Ported packages pass bare asset filenames ("buffalo.png") that only
	// exist inside their own package directory (typically an assets/
	// subfolder), not relative to WA's process working directory. Returns
	// the first candidate that actually exists on disk, checked in order:
	// the path as given, <active package dir>/<filename>, then
	// <active package dir>/assets/<filename>. Falls back to the input
	// unchanged if none exist (matching prior behavior exactly, callers
	// already handle a missing file). Only meaningful while a package's Lua
	// is running (Package::enable/callInterface set the active package dir).
	static std::string resolveAssetPath(const std::string &filename);
	// Returns the only icon-like PNG/IMG/BMP in the active package's assets
	// directory, or an empty string when there is none or the choice is ambiguous.
	static std::string findUniquePackageIcon();
};

// printf + rate-limited in-game chat (max 1 msg / 10s per source)
void reportLuaError(const std::string &source, const std::string &what);

#endif //WKJELLYWORM_UTILS_H
