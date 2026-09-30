#ifndef WKJELLYWORM_PXWEAPONART_H
#define WKJELLYWORM_PXWEAPONART_H

#include <cstddef>
#include <string>

// Recovers the art ProjectX kept outside the WeaponStruct.
//
// ProjectX stores the value 100000 in a projectile sprite slot to mean "use
// the image file named in this weapon's .bin trailer". px2jelly copied the
// struct bytes (100000 included) into main.lua but dropped the trailer, so WA
// ends up drawing sprite 100000, which does not exist: invisible projectiles.
// The trailer also names the worm's aiming animation; without it WA's aim
// sprite lookup returns 0 for these weapons and the worm is not drawn at all.
namespace PxWeaponArt {
	// WeaponStruct offsets of the three projectile sprite slots.
	const size_t kSlotOffsets[3] = {0x60, 0x124, 0x170};
	const int kSpriteFromFile = 100000;

	struct Art {
		std::string slotFiles[3]; // resolved image per kSlotOffsets entry, empty = keep
		std::string aimFile;      // resolved worm aim image, empty = none
	};

	// Must run while the registering package's directory is active (i.e. from
	// registerCustomWeapon during the package's Lua initialize()). `fields` is
	// the registered WeaponStruct viewed as ints. Returns false when no .bin in
	// the package's weapons_raw confidently matches the struct.
	bool find(const int *fields, size_t structSize, const std::string &name1, Art &art);

	// Appends to jelly\pxweapons.log (and stdout).
	void log(const char *fmt, ...);
}

#endif //WKJELLYWORM_PXWEAPONART_H
