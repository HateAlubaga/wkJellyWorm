#ifndef WKJELLYWORM_SCHEMEBIND_H
#define WKJELLYWORM_SCHEMEBIND_H

#include <sigscanner.h>
#include <string>

// Binds wkJellyWorm packages to the scheme picked in the normal WA lobby GUI.
//
// A .wsc file has no name inside, the name is the file name. WA reads a scheme
// into one global scheme struct from SetWscScheme(struct, path, flag, out) (and
// SetBuiltinScheme(struct, id) for the built-in ones). We hook both (patterns
// from wkTerrainSync, same WA 3.8.1), take the name from the path, hash the raw
// settings bytes in the struct, and look both up in [scheme_modules] of
// wkJellyWorm.ini. Matching modules are enabled, modules of the previous scheme
// are disabled, and the host's config string syncs the result to the guests.
//
//   [scheme_modules]
//   Pro=glider,weapon_tazer          ; by file name, without .wsc
//   hash:1A2B3C4D=jellyreverse       ; by hash of the settings bytes
//
// Everything that happens is logged to jelly\scheme_hook.log.
class SchemeBind {
public:
	static int install(SignatureScanner &signatureScanner, module mod);
	// The package layer was reset (lobby entry, /reset, /load): applied state is stale.
	static void onConfigReset();
	// Called when a frontend screen with a scheme selector opens, after autostart
	// and session restore, to re-apply the binding for the scheme WA already holds.
	static void onScreenEnter(int screen);
	// /scheme ... Local only, returns the text to show.
	static std::string handleCommand(const std::string &args);
};

#endif //WKJELLYWORM_SCHEMEBIND_H
