
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
	static inline bool hookingVfs = false;
	static inline std::string hookingFilename;

	static DWORD __stdcall hookLoadSprite_OpenVfsReader(int a3, int a4);
	static int __fastcall hookLoadSprite(DWORD DD_Display, int EDX, int palette, int id, int a4, int a5, const char *filename);
public:

	// ProjectX reserves override targets through 850; generated handles must not collide.
	static const int customSpriteStartingId = 851;

	static void onConstructGlobalContext(int a1);
	static int install(SignatureScanner &, module);
	static int registerCustomSprite(std::string filename);
	static void setSpriteOverride(int targetId, int sourceId);
	static void resetConfig();
	static void callDrawSpriteLocal(int posy, int layer, int posx, int sprite, int frame);
	static void callDrawSpriteGlobal(int posy, int layer, int posx, int sprite, int frame);

	// An override makes two table slots point at the same sprite object. WA
	// destroys every slot on teardown, so each extra destroy of an aliased
	// source is redirected to the target slot's displaced original instead.
	static inline std::unordered_map<Sprites *, std::vector<Sprites *>> aliasedOriginals;
	static inline std::unordered_map<Sprites *, int> aliasedDestroyCount;
};


#endif //WKJELLYWORM_SPRITES_H

