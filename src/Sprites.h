
#ifndef WKJELLYWORM_SPRITES_H
#define WKJELLYWORM_SPRITES_H


#include <sigscanner.h>
#include <vector>
#include <string>
#include <unordered_map>
#include "renderer/Bitmap.h"
class Sprites {
public:
	class SpriteFrame {
	public:
		DWORD bitmapoffset;
		WORD startx;
		WORD starty;
		WORD endx;
		WORD endy;
	};


public:
	DWORD dword0;
	DWORD dword4;
	WORD word8;
	WORD fps_wordA;
	WORD width_wordC;
	WORD height_wordE;
	WORD flags_word10;
	WORD max_frames_copy_word12;
	WORD word14;
	WORD max_frames_word16;
	WORD word18;
	WORD word1A;
	DWORD dword1C;
	DWORD dword20;
	DWORD dword24;
	SpriteFrame * frame_meta_ptr_dword28;
	DWORD dword2C;
	WORD word30;
	WORD word32;


	DWORD bitmap_dword34;

	DWORD dword38;
	DWORD dword3C;
	DWORD dword40;
	DWORD dword44;
	DWORD dword48;
	DWORD dword4C;
	DWORD dword50;
	DWORD dword54;
	DWORD dword58;
	DWORD dword5C;

//	Bitmap::BitmapImage bitmap;
	DWORD dword60;
	DWORD probably_sprite_bitmap_data_dword64;
	DWORD maybe_palette_data_dword68;
	DWORD dword6C;

private:
	static inline std::vector<std::string> customSprites;
	// A package's "initialize" Lua entrypoint can legitimately run more than
	// once per match (re-applied autostart, re-entering the lobby, etc.).
	// Without this, every rerun would append a fresh duplicate to
	// customSprites and hand out a NEW id for the same file, leaving
	// whichever id the package's Lua actually captured pointing at a
	// never-loaded slot. Keyed by resolved path so the same asset always
	// gets the same id no matter how many times it's registered.
	static inline std::unordered_map<std::string, int> resolvedSpriteIds;
	static inline std::vector<std::pair<int, int>> spriteOverrides;
	static inline std::unordered_map<std::string, int> frameHints;
	static inline std::unordered_map<std::string, int> flagHints;
	// resolved image path -> target ids, in registration order
	static inline std::vector<std::pair<std::string, std::vector<int>>> fileOverrides;
	static void aliasIntoSlot(DWORD ddDisplay, Sprites *sprite, int targetId);
	static inline bool hookingVfs = false;
	static inline std::string hookingFilename;

	static DWORD __stdcall hookLoadSprite_OpenVfsReader(int a3, int a4);
	static int __fastcall hookLoadSprite(DWORD DD_Display, int EDX, int palette, int id, int a4, int a5, const char *filename);
public:

	// ProjectX reserves override targets through 850; generated handles must not collide.
	static const int customSpriteStartingId = 851;

	static void onConstructGlobalContext(int a1);
	static int install(SignatureScanner &, module);
	static int registerCustomSprite(std::string filename, int frames = 0, int flags = -1);
	// Like registerCustomSprite, but only takes a new id while more than
	// `reserve` ids of the 851..1023 range stay free. Returns 0 otherwise.
	static int registerCustomSpriteWithReserve(const std::string &filename, int reserve);
	static int customSpriteIdsLeft();
	static void setSpriteOverride(int targetId, int sourceId);
	static void resetConfig();
	static void callDrawSpriteLocal(int posy, int layer, int posx, int sprite, int frame);
	static void callDrawSpriteGlobal(int posy, int layer, int posx, int sprite, int frame);

	// ProjectX SetSpriteOverride(id, LoadSprite(file)): the image never needs a
	// custom id of its own, so it does not eat the small 851..1023 range.
	static void setSpriteOverrideFile(int targetId, std::string filename, int frames = 0, int flags = -1);
	// ProjectX LoadSprite(file, frames, ...) frame count, keyed by resolved path.
	static int frameHintFor(const std::string &resolved) {
		auto it = frameHints.find(resolved);
		return it == frameHints.end() ? 0 : it->second;
	}
	// ProjectX LoadSprite flags: bit 0 loops the animation, bit 1 ping-pongs.
	// They are the same bits WA keeps in the sprite's flags word.
	static int flagHintFor(const std::string &resolved) {
		auto it = flagHints.find(resolved);
		return it == flagHints.end() ? -1 : it->second;
	}

	static void beginVfsHook(const std::string &filename) { hookingFilename = filename; hookingVfs = true; }
	static void endVfsHook() { hookingVfs = false; }
	static int loadSpriteNative(DWORD ddDisplay, int palette, int id, DWORD fileArchive, const char *filename) {
		return hookLoadSprite(ddDisplay, 0, palette, id, 0, (int)fileArchive, filename);
	}

	// An override makes several table slots point at the same sprite object.
	// WA destroys every slot on teardown, so each extra destroy of an aliased
	// sprite is redirected to a displaced original, and whatever originals
	// remain after the last slot's destroy are freed then.
	struct AliasInfo {
		int slots = 0;
		int destroyed = 0;
		std::vector<Sprites *> originals;
	};
	static inline std::unordered_map<Sprites *, AliasInfo> aliases;
};


#endif //WKJELLYWORM_SPRITES_H

