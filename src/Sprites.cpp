
#include <cmath>
#include "Sprites.h"
#include "Hooks.h"
#include "Game.h"
#include "Utils.h"
#include "renderer/Bitmap.h"
#include "renderer/Renderer.h"
#include "renderer/BitmapExtension.h"
#include "WaLibc.h"
#include "Config.h"
#include "Lua.h"
#include "MemAccess.h"
#include <sol/sol.hpp>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstring>

// Temporary diagnostic: mirrors printf (which only reaches the dev console,
// not capturable from outside the process) into a plain file so it can be
// read back after a run.
static void spriteLog(const char *fmt, ...) {
	static bool firstWriteThisProcess = true;
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	va_start(args, fmt);
	FILE *f = nullptr;
	const char *mode = firstWriteThisProcess ? "w" : "a";
	if(fopen_s(&f, "jelly\\sprites_probe.log", mode) == 0 && f) {
		if(firstWriteThisProcess)
			fprintf(f, "=== sprite session pid=%lu tick=%lu ===\n", GetCurrentProcessId(), GetTickCount());
		vfprintf(f, fmt, args);
		fclose(f);
		firstWriteThisProcess = false;
	}
	va_end(args);
}

// ---------------------------------------------------------------------------
// Custom sprites from plain image files
//
// WA's native sprite loader parses its own proprietary container ("SPR\x1a"
// followed by a compressed stream). Every px2jelly-ported package instead
// ships plain .png art and registers it via registerCustomSprite(), because
// that is what ProjectX did -- ProjectX decoded PNGs itself. Feeding those
// raw PNG bytes to WA's parser makes it read frame counts and offsets out of
// PNG header bytes and scribble over the heap, which is what crashed the
// match on "start game" whenever CustomSprites was on.
//
// A native sprite is really just a BitmapImage atlas plus an array of frame
// rectangles (see Sprites.h). We already decode PNG into a native
// BitmapImage for weapon icons (renderer/Bitmap.cpp, via stb_image), so
// there is no need to reverse the compressed container at all:
//   1. let the native loader build+register a real sprite object at `id`
//      from a known-good template .spr, so all its internal bookkeeping is
//      done by WA itself, then
//   2. swap that sprite's atlas and frame table for our decoded PNG.
// ---------------------------------------------------------------------------

// WA's own bound check in LoadSprite: lea ecx,[id-1]; cmp ecx,0x3FE; ja fail
static const int kMinSpriteId = 1;
static const int kMaxSpriteId = 0x3FF;
// Sprite table lives at DD_Display + 0x1008, indexed by id (LoadSprite @0x5234AD:
// mov dword ptr [esi + ebp*4 + 0x1008], edi).
static const DWORD kSpriteTableOffset = 0x1008;
static const char *kTemplateSpritePath = "jelly\\placeholders\\template.spr";
// Frame indices are supplied by package Lua (drawSpriteLocal(..., frame)) and
// are not validated against our real frame count anywhere we control, so the
// table is over-allocated and padded with copies of frame 0. An out-of-range
// frame then draws something harmless instead of reading past the array.
static const int kFrameTableSlots = 64;

static bool hasImageFileExtension(const std::string &filename) {
	auto dot = filename.find_last_of('.');
	if(dot == std::string::npos)
		return false;
	std::string ext = filename.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif";
}

// ProjectX sprite sheets are overwhelmingly vertical strips of square frames
// (an N*w tall, w wide image is N frames). Anything else is treated as a
// single frame, which is always safe to draw.
static int deduceFrameCount(int width, int height) {
	if(width <= 0 || height <= 0)
		return 1;
	if(height > width && (height % width) == 0) {
		int frames = height / width;
		if(frames >= 1 && frames <= kFrameTableSlots)
			return frames;
	}
	return 1;
}

static bool patchSpriteFromImageFile(Sprites *sprite, const std::string &file, int frameHint, int flagHint) {
	if(!sprite)
		return false;

	int palette = 0;
	DWORD ddDisplay = Game::getAddrDdDisplay();
	if(ddDisplay && jellyProbeRead(ddDisplay + 0x3120, sizeof(DWORD)))
		palette = (int)*(DWORD *)(ddDisplay + 0x3120);

	auto *bitmap = Bitmap::loadImageFile(0, palette, (char *)file.c_str());
	if(!bitmap) {
		spriteLog("custom sprite '%s': image decode failed; sprite left as template\n", file.c_str());
		return false;
	}

	int width = bitmap->current_width_dword24;
	int height = bitmap->current_height_dword28;
	if(width <= 0 || height <= 0 || width > 4096 || height > 4096) {
		spriteLog("custom sprite '%s': implausible size %dx%d; skipping\n", file.c_str(), width, height);
		return false;
	}

	// The package's own frame count (ProjectX LoadSprite argument) wins over
	// the square-frame guess: e.g. 40x680 golf flags are 5 frames, not 17.
	int frames = (frameHint >= 1 && frameHint <= kFrameTableSlots && frameHint <= height)
		? frameHint : deduceFrameCount(width, height);
	int frameHeight = height / frames;
	int rowsize = bitmap->rowsize_dword10;

	auto *table = (Sprites::SpriteFrame *)WaLibc::waMalloc(sizeof(Sprites::SpriteFrame) * kFrameTableSlots);
	if(!table) {
		spriteLog("custom sprite '%s': could not allocate frame table\n", file.c_str());
		return false;
	}
	// bitmapoffset selects the frame's pixels; start/end are the drawn rect
	// INSIDE the width x frameHeight frame box (WA.exe 0x4FAD30 hands them to
	// the blitter as the offset from the box corner). Putting the strip row in
	// starty drew frame N N*frameHeight pixels too low, so every animated
	// sprite slid downwards as its frame advanced.
	for(int i = 0; i < kFrameTableSlots; i++) {
		int frame = (i < frames) ? i : 0;
		int top = frame * frameHeight;
		table[i].bitmapoffset = (DWORD)(top * rowsize);
		table[i].startx = 0;
		table[i].starty = 0;
		table[i].endx = (WORD)width;
		table[i].endy = (WORD)frameHeight;
	}

	Bitmap::BitmapImage *inlineBitmap =
		(Bitmap::BitmapImage *)&sprite->bitmap_dword34;
	BitmapExtension::destroyBitmapImageExtension(bitmap);
	// Only the verified native bitmap core (vtable through current_height) is
	// embedded at sprite offsets 0x34..0x5F. sizeof(BitmapImage) also includes
	// speculative trailing fields and overwrote the sprite fields after 0x60.
	constexpr size_t kEmbeddedBitmapBytes = 0x2C;
	memcpy(inlineBitmap, bitmap, kEmbeddedBitmapBytes);
	sprite->probably_sprite_bitmap_data_dword64 = bitmap->data_dword8;
	sprite->maybe_palette_data_dword68 = (DWORD)palette;
	WaLibc::waFree(bitmap);

	sprite->frame_meta_ptr_dword28 = table;
	// WA picks the frame as (phase * max_frames) >> 16, clamped or wrapped by
	// flag bit 0 and mirrored by bit 1 (WA.exe 0x4FAD30). A non-zero dword24
	// switches to a range mode that always shows frame 0, which is wrong
	// for a vertical strip, so clear it.
	spriteLog("custom sprite '%s': template flags=0x%X dword1C=%d dword20=%d dword24=%d\n",
		file.c_str(), sprite->flags_word10, sprite->dword1C, sprite->dword20, sprite->dword24);
	sprite->dword24 = 0;
	WORD animBits = (WORD)(flagHint >= 0 ? (flagHint & 3) : 1);
	sprite->flags_word10 = (WORD)((sprite->flags_word10 & ~3) | animBits);
	sprite->width_wordC = (WORD)width;
	sprite->height_wordE = (WORD)frameHeight;
	sprite->max_frames_word16 = (WORD)frames;
	sprite->max_frames_copy_word12 = (WORD)frames;

	spriteLog("custom sprite '%s': %dx%d as %d frame(s) of %dpx height\n",
		file.c_str(), width, height, frames, frameHeight);
	return true;
}

// Loads one custom sprite into table slot `id` (which must be empty) and
// returns the sprite object, or nullptr if the native loader refused it.
static Sprites *loadCustomSpriteIntoSlot(DWORD addrDD_Display, DWORD addrPC_FileArchive, int id, const std::string &file) {
	bool isPlainImage = hasImageFileExtension(file);
	// A plain image cannot go through the native parser, so bootstrap the
	// sprite object from the template and replace its contents after.
	std::string loadName = isPlainImage ? std::string(kTemplateSpritePath) : file;
	spriteLog("loading custom sprite: %s = %d%s\n", file.c_str(), id, isPlainImage ? " (via image decode)" : "");

	int loaded = 0;
	Sprites::beginVfsHook(loadName);
	// A missing/broken optional sprite must not abort global-context
	// construction. The weapon package and its logic are independent
	// from the visual resource.
	try {
		loaded = Sprites::loadSpriteNative(addrDD_Display, 1, id, addrPC_FileArchive, loadName.c_str());
	} catch(const std::exception & e) {
		spriteLog("custom sprite '%s' failed, continuing without it: %s\n", file.c_str(), e.what());
	} catch(...) {
		spriteLog("custom sprite '%s' failed, continuing without it\n", file.c_str());
	}
	Sprites::endVfsHook();

	if(!loaded) {
		spriteLog("custom sprite '%s': native loader rejected it\n", file.c_str());
		return nullptr;
	}
	DWORD slot = addrDD_Display + kSpriteTableOffset + (DWORD)id * 4;
	Sprites *sprite = jellyProbeRead(slot, sizeof(DWORD)) ? *(Sprites **)slot : nullptr;
	if(!isPlainImage)
		return sprite;
	spriteLog("custom sprite '%s': id=%d slot=0x%X spritePtr=0x%X\n", file.c_str(), id, slot, (DWORD)sprite);
	if(sprite && jellyProbeRead((DWORD)sprite, sizeof(Sprites)))
		patchSpriteFromImageFile(sprite, file, Sprites::frameHintFor(file), Sprites::flagHintFor(file));
	else
		spriteLog("custom sprite '%s': sprite slot %d unreadable after load\n", file.c_str(), id);
	return sprite;
}

void Sprites::aliasIntoSlot(DWORD ddDisplay, Sprites *sprite, int targetId) {
	DWORD targetSlot = ddDisplay + kSpriteTableOffset + (DWORD)targetId * 4;
	if(!jellyProbeRead(targetSlot, sizeof(DWORD)))
		return;
	Sprites *original = *(Sprites **)targetSlot;
	if(original == sprite)
		return;
	*(Sprites **)targetSlot = sprite;
	aliases[sprite].slots++;
	if(original) {
		auto previous = aliases.find(original);
		if(previous != aliases.end()) {
			// The slot held another alias of ours, not a native sprite: it just
			// loses one slot, and nothing extra must be freed for it.
			previous->second.slots--;
		} else {
			aliases[sprite].originals.push_back(original);
		}
	}
	spriteLog("SetSpriteOverride: target %d -> ptr=0x%X (displaced 0x%X)\n", targetId, (DWORD)sprite, (DWORD)original);
}

void Sprites::onConstructGlobalContext(int a1) {
	if(!Config::customSpritesEnabled || (customSprites.empty() && fileOverrides.empty()) || !Game::isValidDdGame())
		return;

	auto addrDD_Display = Game::getAddrDdDisplay();
	auto addrPC_FileArchive = *(DWORD *)(Game::getAddrDDGame() + 0x4C0);
	int id = customSpriteStartingId;
	aliases.clear();

	for(auto & it : customSprites) {
		if(id < kMinSpriteId || id > kMaxSpriteId) {
			spriteLog("custom sprite '%s': id %d out of WA's 1..1023 range; skipping\n", it.c_str(), id);
			id++;
			continue;
		}
		loadCustomSpriteIntoSlot(addrDD_Display, addrPC_FileArchive, id, it);
		id++;
	}

	// ProjectX weapons keep fixed sprite slot numbers in WeaponStruct. Redirect
	// those slots to the decoded package art after all custom sprites exist.
	for(const auto &overridePair : spriteOverrides) {
		int targetId = overridePair.first;
		int sourceId = overridePair.second;
		if(targetId < kMinSpriteId || targetId > kMaxSpriteId ||
		   sourceId < kMinSpriteId || sourceId > kMaxSpriteId) {
			spriteLog("SetSpriteOverride: invalid target=%d source=%d\n", targetId, sourceId);
			continue;
		}
		DWORD sourceSlot = addrDD_Display + kSpriteTableOffset + (DWORD)sourceId * 4;
		Sprites *source = jellyProbeRead(sourceSlot, sizeof(DWORD)) ? *(Sprites **)sourceSlot : nullptr;
		if(!source || !jellyProbeRead((DWORD)source, sizeof(Sprites))) {
			spriteLog("SetSpriteOverride: source %d unreadable for target %d\n", sourceId, targetId);
			continue;
		}
		if(aliases.find(source) == aliases.end())
			aliases[source].slots = 1; // the source keeps its own slot too
		aliasIntoSlot(addrDD_Display, source, targetId);
	}

	// Image-file overrides (worm skins and the like). WA's LoadSprite refuses
	// an occupied id, so each image is loaded into a free scratch slot, moved
	// into its target slots, and the scratch slot is emptied again.
	for(const auto &entry : fileOverrides) {
		const std::string &file = entry.first;
		if(entry.second.empty())
			continue;
		int scratch = 0;
		for(int candidate = kMaxSpriteId; candidate >= kMinSpriteId; candidate--) {
			if(candidate >= customSpriteStartingId && candidate < customSpriteStartingId + (int)customSprites.size())
				continue;
			DWORD a = addrDD_Display + kSpriteTableOffset + (DWORD)candidate * 4;
			DWORD b = addrDD_Display + kSpriteTableOffset + 0x1000 + (DWORD)candidate * 4;
			if(jellyProbeRead(a, 4) && jellyProbeRead(b, 4) && !*(DWORD *)a && !*(DWORD *)b) {
				scratch = candidate;
				break;
			}
		}
		if(!scratch) {
			spriteLog("SetSpriteOverride '%s': no free scratch slot; skipping\n", file.c_str());
			continue;
		}
		DWORD paletteEntry = addrDD_Display + 8 + (DWORD)scratch * 4;
		DWORD savedPalette = jellyProbeRead(paletteEntry, 4) ? *(DWORD *)paletteEntry : 0;
		Sprites *sprite = loadCustomSpriteIntoSlot(addrDD_Display, addrPC_FileArchive, scratch, file);
		*(DWORD *)(addrDD_Display + kSpriteTableOffset + (DWORD)scratch * 4) = 0;
		*(DWORD *)paletteEntry = savedPalette;
		if(!sprite || !jellyProbeRead((DWORD)sprite, sizeof(Sprites)))
			continue;
		aliases[sprite].slots = 0;
		if(flagHintFor(file) < 0) {
			// No ProjectX flags: animate like the sprite being replaced.
			DWORD firstSlot = addrDD_Display + kSpriteTableOffset + (DWORD)entry.second.front() * 4;
			Sprites *native = jellyProbeRead(firstSlot, 4) ? *(Sprites **)firstSlot : nullptr;
			if(native && jellyProbeRead((DWORD)native, sizeof(Sprites))) {
				sprite->flags_word10 = (WORD)((sprite->flags_word10 & ~3) | (native->flags_word10 & 3));
				if(native->fps_wordA)
					sprite->fps_wordA = native->fps_wordA;
			}
		}
		for(int targetId : entry.second) {
			DWORD targetSlot = addrDD_Display + kSpriteTableOffset + (DWORD)targetId * 4;
			Sprites *original = jellyProbeRead(targetSlot, 4) ? *(Sprites **)targetSlot : nullptr;
			if(original && jellyProbeRead((DWORD)original, sizeof(Sprites)))
				spriteLog("SetSpriteOverride '%s' -> %d: native %d frames %dx%d flags=0x%X fps=%d, new %d frames %dx%d\n",
					file.c_str(), targetId, original->max_frames_word16, original->width_wordC, original->height_wordE,
					original->flags_word10, original->fps_wordA, sprite->max_frames_word16, sprite->width_wordC, sprite->height_wordE);
			aliasIntoSlot(addrDD_Display, sprite, targetId);
		}
		if(aliases[sprite].slots == 0) {
			// No target took it. Nothing reaches it through a slot any more.
			aliases.erase(sprite);
			spriteLog("SetSpriteOverride '%s': no valid target, sprite dropped\n", file.c_str());
		}
	}
}

void Sprites::setSpriteOverride(int targetId, int sourceId) {
	for(auto &entry : spriteOverrides) {
		if(entry.first == targetId) {
			entry.second = sourceId;
			return;
		}
	}
	spriteOverrides.emplace_back(targetId, sourceId);
}

void Sprites::setSpriteOverrideFile(int targetId, std::string filename, int frames, int flags) {
	if(targetId < kMinSpriteId || targetId > kMaxSpriteId) {
		spriteLog("SetSpriteOverride: invalid target=%d for '%s'\n", targetId, filename.c_str());
		return;
	}
	// Resolve now, while the calling package's directory is still active.
	std::string resolved = Utils::resolveAssetPath(filename);
	if(frames > 0)
		frameHints[resolved] = frames;
	if(flags >= 0)
		flagHints[resolved] = flags;
	for(auto &entry : fileOverrides) {
		auto &targets = entry.second;
		targets.erase(std::remove(targets.begin(), targets.end(), targetId), targets.end());
	}
	for(auto &entry : fileOverrides) {
		if(entry.first == resolved) {
			entry.second.push_back(targetId);
			return;
		}
	}
	fileOverrides.push_back({resolved, {targetId}});
}

int Sprites::registerCustomSprite(std::string filename, int frames, int flags) {
	if(!Config::customSpritesEnabled) {
		// Keep Lua initialization successful. ID 0 is the native no-op/safe
		// fallback; no custom file is queued or loaded.
		spriteLog("registerCustomSprite: disabled by CustomSprites=0; using safe dummy id for '%s'\n",
			filename.c_str());
		return 0;
	}
	// Resolve against the active package's own directory (assets/) NOW,
	// while Package::getActivePackageDir() is still valid -- the actual load
	// happens later from onConstructGlobalContext, by which point no
	// specific package's directory is active any more, so a bare filename
	// ("buffalo.png") would otherwise be looked up relative to WA's process
	// directory and never found. This was why custom projectiles/sprites
	// were invisible even for packages that shipped real art.
	std::string resolved = Utils::resolveAssetPath(filename);
	if(resolved != filename)
		spriteLog("registerCustomSprite: resolved '%s' -> '%s'\n", filename.c_str(), resolved.c_str());

	if(frames > 0)
		frameHints[resolved] = frames;
	if(flags >= 0)
		flagHints[resolved] = flags;
	auto existing = resolvedSpriteIds.find(resolved);
	if(existing != resolvedSpriteIds.end())
		return existing->second;

	if(customSpriteIdsLeft() <= 0) {
		// An id past 1023 is outside WA's sprite table; handing it out would
		// make WA index past the table when something draws it.
		spriteLog("registerCustomSprite: no free id left in %d..%d for '%s'; using 0\n",
			customSpriteStartingId, kMaxSpriteId, resolved.c_str());
		return 0;
	}
	customSprites.push_back(resolved);
	int id = customSpriteStartingId + (int)customSprites.size() - 1;
	resolvedSpriteIds[resolved] = id;
	return id;
}

int Sprites::customSpriteIdsLeft() {
	return kMaxSpriteId - (customSpriteStartingId + (int)customSprites.size()) + 1;
}

int Sprites::registerCustomSpriteWithReserve(const std::string &filename, int reserve, int frames) {
	if(!Config::customSpritesEnabled)
		return 0;
	std::string resolved = Utils::resolveAssetPath(filename);
	auto existing = resolvedSpriteIds.find(resolved);
	if(existing != resolvedSpriteIds.end())
		return existing->second;
	if(customSpriteIdsLeft() <= reserve)
		return 0;
	return registerCustomSprite(filename, frames);
}

void Sprites::resetConfig() {
	customSprites.clear();
	resolvedSpriteIds.clear();
	spriteOverrides.clear();
	fileOverrides.clear();
	frameHints.clear();
	flagHints.clear();
}

int (__fastcall *origLoadSprite)(DWORD DD_Display, int EDX, int palette, int id, int a4, int a5, const char *filename);
int __fastcall Sprites::hookLoadSprite(DWORD DD_Display, int EDX, int palette, int id, int a4, int a5, const char *filename) {
	int ret = origLoadSprite(DD_Display, EDX, palette, id, a4, a5, filename);
//	printf("loadSprite: %s = %d  (%X), palette: %d a4: %X a5: %X\n", filename, id, ret, palette, a4, a5);
	return ret;
}



//signed int __userpurge process_sprite_sub_4FA7E0@<eax>(int a1@<eax>, int palette_a2@<ecx>, int a3)
DWORD origProcessSprite;
DWORD __stdcall hookProcessSprite(int a3) {
	DWORD retv;
	Sprites * sprite;
	GamePalette * palette;
	_asm mov sprite, eax
	_asm mov palette, ecx

	_asm push a3
	_asm mov ecx, palette
	_asm mov eax, sprite
	_asm call origProcessSprite
	_asm mov retv, eax

	if(Config::rendererEnabled && Renderer::isActive()) {
		BitmapExtension::convertSprite(sprite, palette);
	}

	return retv;
}

// readFileToWaBuffer THROWS on any failure (missing file, short read, alloc
// failure) rather than returning null. Must be called only from a normally-
// invoked function -- see the comment on hookLoadSprite_OpenVfsReader below
// for why the try/catch cannot live there directly.
static void *safeReadFileToWaBuffer(const std::string &filename) {
	try {
		return Utils::readFileToWaBuffer(filename);
	} catch(const std::exception & e) {
		spriteLog("custom sprite '%s' failed to load, skipping: %s\n", filename.c_str(), e.what());
	} catch(...) {
		spriteLog("custom sprite '%s' failed to load, skipping (unknown error)\n", filename.c_str());
	}
	return nullptr;
}

//DWORD addrLoadSprite_ProcessSprite;
DWORD origLoadSprite_OpenVfsReader;
DWORD __stdcall Sprites::hookLoadSprite_OpenVfsReader(int a3, int a4) {
	int seax, secx, retv; //filename, PC_Filearchive
	_asm mov seax, eax
	_asm mov secx, ecx

	if(!hookingVfs) {
		_asm mov eax, seax
		_asm mov ecx, secx
		_asm push a4
		_asm push a3
		_asm call origLoadSprite_OpenVfsReader
		_asm mov retv, eax
	} else {
		// This hook is called directly from WA's own native code via an asm
		// trampoline (see the eax/ecx capture above -- the real calling
		// convention doesn't match any C++ one, so the compiler is lied to
		// and the true args are grabbed from registers by hand before
		// anything else runs). A try/catch anywhere in a function forces the
		// compiler to emit an exception-handling prologue for the WHOLE
		// function, which runs before that eax/ecx capture and clobbers both
		// registers -- silently corrupting every call through this hook,
		// including plain native sprite loads (e.g. worms), not just the
		// custom-sprite branch that actually throws. That is what made worms
		// go invisible the moment this fix first landed. The try/catch must
		// live in a normally-called helper (safeReadFileToWaBuffer above),
		// never directly in this function.
		void * data = safeReadFileToWaBuffer(hookingFilename);
		if(!data) {
			// Do not pass a null buffer into WA's sprite processor. This is an
			// optional visual resource; its failure must not affect packages,
			// weapon registration, ammo, or fire hooks.
			spriteLog("custom sprite '%s' could not be read; skipping visual load\n",
				hookingFilename.c_str());
			retv = 0;
			return retv;
		}
		if(hasImageFileExtension(hookingFilename)) {
			// origProcessSprite (WA's native sprite-sheet parser) expects WA's
			// own proprietary multi-frame binary format, not a plain image
			// file. Every ported package registers raw .png sprites via
			// registerCustomSprite -- handing those bytes straight to the
			// native parser makes it read frame-count/offset fields out of
			// PNG header bytes, walk/allocate using that garbage, and corrupt
			// the heap. That corruption doesn't fault here; it silently
			// stomps memory and crashes later, deep in ntdll, whenever that
			// memory is next touched -- which is exactly the deterministic
			// "press start game -> loading -> crash" reported for every
			// package set once CustomSprites=1. There is no PNG-to-native
			// sprite-sheet converter yet (unlike weapon icons, which already
			// go through Bitmap.cpp's stb_image decoder), so for now skip the
			// visual cleanly instead of corrupting memory.
			spriteLog("custom sprite '%s' is a plain image file; native sprite loader needs its own binary "
				"format and would corrupt memory on this -- skipping visual load (not a crash)\n",
				hookingFilename.c_str());
			WaLibc::waFree(data);
			retv = 0;
			return retv;
		}
		*(void **)(a3 + 108) = data;
 		_asm push data
		_asm mov eax, a3
		_asm mov ecx, a4
		_asm call hookProcessSprite
		_asm mov retv, eax
	}
	return retv;
}


DWORD origDrawSpriteGlobal;
void Sprites::callDrawSpriteGlobal(int posy, int layer, int posx, int sprite, int frame) {
	DWORD gameGlobal = Game::getAddrGameGlobal();
	if(!gameGlobal)
		return;
	DWORD gamescene = *(DWORD*)(gameGlobal + 0x524);
	if(!gamescene)
		return;
	_asm push frame
	_asm push sprite
	_asm push posx
	_asm push layer
	_asm mov ecx, gamescene
	_asm mov eax, posy
	_asm call origDrawSpriteGlobal
}

DWORD origDrawSpriteLocal;
void Sprites::callDrawSpriteLocal(int posy, int layer, int posx, int sprite, int frame) {
	DWORD gameGlobal = Game::getAddrGameGlobal();
	if(!gameGlobal)
		return;
	DWORD gamescene = *(DWORD*)(gameGlobal + 0x524);
	if(!gamescene)
		return;
	_asm push frame
	_asm push sprite
	_asm push posx
	_asm push layer
	_asm mov ecx, gamescene
	_asm mov eax, posy
	_asm call origDrawSpriteLocal
}


// Reads the sprite object currently drawn for `spriteIndex`, or nullptr.
static Sprites *spriteForDraw(int spriteIndex) {
	DWORD ddDisplay = Game::getAddrDdDisplay();
	if(!ddDisplay)
		return nullptr;
	const DWORD tables[] = {(DWORD)kSpriteTableOffset, (DWORD)kSpriteTableOffset + 0x1000};
	for(DWORD table : tables) {
		DWORD slot = ddDisplay + table + (DWORD)spriteIndex * 4;
		if(!jellyProbeRead(slot, sizeof(DWORD)))
			continue;
		auto *sprite = *(Sprites **)slot;
		if(sprite && jellyProbeRead((DWORD)sprite, sizeof(Sprites)))
			return sprite;
	}
	return nullptr;
}

// WA takes 16.16 fixed positions and layers (the low 16 bits are masked
// off), and the frame as an animation phase 0..0xFFFF, not an index.
// Package code converted from ProjectX often passes pixels, small layer
// numbers and frame indices, which all end up as 0 on WA's side. Values that
// cannot be valid fixed-point/phase input are converted here.
static int toFixed(double v) {
	if(v > -65536.0 && v < 65536.0)
		v *= 65536.0;
	if(v > 2147483647.0)
		v = 2147483647.0;
	if(v < -2147483648.0)
		v = -2147483648.0;
	return (int)v;
}

static int toLayer(double v) {
	return (v > -256.0 && v < 256.0) ? (int)(v * 65536.0) : (int)v;
}

static int toPhase(double f, int spriteIndex) {
	if(!(f >= 0.0))
		return 0;
	if(f >= 256.0)
		return (int)fmod(f, 65536.0); // already a phase
	double whole = floor(f);
	if(f != whole)
		return (int)((f - whole) * 65536.0); // ProjectX style 0..1 animation fraction
	int index = (int)whole;
	Sprites *sprite = spriteForDraw(spriteIndex);
	int frames = sprite ? sprite->max_frames_word16 : 0;
	if(frames <= 1)
		return 0;
	index %= frames;
	// A quarter into the frame's phase range picks it with and without
	// WA's rounding (+0x8000 before the >> 16).
	double phase = (index + 0.25) * 65536.0 / frames;
	if(sprite->flags_word10 & 2)
		phase /= 2; // ping-pong doubles the phase before the lookup
	return (int)phase;
}

static void drawSpriteChecked(const char *name, sol::variadic_args &va,
                              void (*draw)(int, int, int, int, int)) {
	static std::unordered_map<std::string, bool> reported;
	int count = (int)va.size();
	bool ok = count >= 5;
	for(int i = 0; ok && i < 5; i++)
		ok = va[i].get_type() == sol::type::number;
	int sprite = ok ? (int)va[3].get<double>() : 0;
	int spriteIndex = sprite & 0xFFFF;
	if(ok && (spriteIndex < kMinSpriteId || spriteIndex > kMaxSpriteId))
		ok = false;
	if(!ok) {
		std::string key = std::string(name) + "/" + std::to_string(count) + "/" + std::to_string(sprite);
		if(!reported[key]) {
			reported[key] = true;
			std::string types;
			for(int i = 0; i < count; i++)
				types += std::string(i ? "," : "") + sol::type_name(va.lua_state(), va[i].get_type());
			spriteLog("%s: skipped non-canonical call (%d args: %s, sprite=%d); expected (posy, layer, posx, sprite, frame)\n",
				name, count, types.c_str(), sprite);
		}
		return;
	}
	double frame = va[4].get<double>();
	draw(toFixed(va[0].get<double>()), toLayer(va[1].get<double>()), toFixed(va[2].get<double>()),
		sprite, toPhase(frame, spriteIndex));
}

// Sprite destructor is thiscall(ecx=sprite, flags) with `ret 4`. Modelled as
// __fastcall with a dummy edx so the hook also pops the flags argument; the
// old two-argument form returned with plain `ret` and unbalanced the stack of
// every caller, which crashed WA while tearing sprites down on exit.
DWORD (__fastcall *origDestroySprite)(Sprites* sprite, int edx, int flags);
static DWORD destroyOneSprite(Sprites *sprite, int flags) {
	if(Config::rendererEnabled && Renderer::isActive()) {
		BitmapExtension::onDestroySprite(sprite);
	}
	return origDestroySprite(sprite, 0, flags);
}

DWORD __fastcall hookDestroySprite(Sprites* sprite, int, int flags) {
	auto alias = Sprites::aliases.find(sprite);
	if(alias == Sprites::aliases.end())
		return destroyOneSprite(sprite, flags);

	// First destroy frees the shared object; every later one belongs to a
	// slot that only borrowed it, so free that slot's original instead.
	auto &info = alias->second;
	int index = info.destroyed++;
	Sprites *victim = sprite;
	if(index > 0) {
		victim = nullptr;
		if(!info.originals.empty()) {
			victim = info.originals.back();
			info.originals.pop_back();
		}
	}
	std::vector<Sprites *> leftovers;
	if(info.destroyed >= info.slots) {
		leftovers.swap(info.originals);
		Sprites::aliases.erase(alias);
	}
	DWORD ret = victim ? destroyOneSprite(victim, flags) : 0;
	for(auto *original : leftovers)
		destroyOneSprite(original, flags);
	return ret;
}

int Sprites::install(SignatureScanner &, module) {
	DWORD addrLoadSprite = Hooks::scanPattern("LoadSprite", "\x55\x8B\x6C\x24\x0C\xF7\xC5\x00\x00\x00\x00\x56\x8B\xF1\x74\x0A\x5E\xB8\x00\x00\x00\x00\x5D\xC2\x14\x00\x53\x8B\x5C\x24\x10\x8D\x43\xFF\x83\xF8\x02\x0F\x87\x00\x00\x00\x00\x83\xBC\x9E\x00\x00\x00\x00\x00", "??????x????xxxxxxx????xxxxxxxxxxxxxxxxx????xxx?????", 0x523400);
	DWORD addrLoadSprite_OpenVfsReader = Hooks::scanPattern("LoadSprite_OpenVfsReader", "\x55\x8B\x6C\x24\x08\x56\x57\x8B\xF1\x56\x8B\xF8\xE8\x00\x00\x00\x00\x85\xC0\x74\x11\x8B\x16\x8B\x40\x04\x8B\x52\x08\x50\x8B\xCE\xFF\xD2\x85\xC0\x75\x57", "??????xxxxxxx????xxxxxxxxxxxxxxxxxxxxx", 0x4FAAF0);
	origDrawSpriteGlobal = Hooks::scanPattern("DrawSpriteGlobal", "\x8B\x91\x00\x00\x00\x00\x81\xFA\x00\x00\x00\x00\x56\x57\x8B\x7C\x24\x10\x8B\xF0\x7D\x5B\x8B\x01\x83\xC0\xE8\x78\x54\x89\x01\x8D\x44\x08\x04\x89\x84\x91\x00\x00\x00\x00\x8B\x91\x00\x00\x00\x00", "??????xx????xxxxxxxxxxxxxxxxxxxxxxxxxx????xx????", 0x541FE0);
	origDrawSpriteLocal = origDrawSpriteGlobal + (0x541BA0 - 0x541B20);

	DWORD addrProcessSprite = Hooks::scanPattern("ProcessSprite", "\x51\x8B\x54\x24\x08\x53\x55\x8B\x2D\x00\x00\x00\x00\x56\x8D\x72\x04\x83\xC6\x04\x57\x8B\xF8\x8B\x46\xFC\x01\x05\x00\x00\x00\x00\x89\x77\x60\x0F\xB7\x1E\x83\xC6\x02\x89\x77\x68\x66\x89\x5F\x14", "??????xxx????xxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxx", 0x4FAB80);

	DWORD addrConstructSprite = Hooks::scanPattern("ConstructSprite", "\x89\x48\x04\x33\xC9\xC7\x00\x00\x00\x00\x00\xC7\x40\x00\x00\x00\x00\x00\x89\x48\x48\x89\x48\x4C\x89\x48\x44\x89\x48\x3C\x89\x48\x50\x89\x48\x54\x89\x48\x58\x89\x48\x5C\xC7\x40\x00\x00\x00\x00\x00", "??????x????xx?????xxxxxxxxxxxxxxxxxxxxxxxxxx?????", 0x4FAA30);
	DWORD *addrSpriteVTable = *(DWORD**)(addrConstructSprite + 0x7);
	DWORD addrDestroySprite = addrSpriteVTable[0];

	Hooks::polyhook("LoadSprite", addrLoadSprite, (DWORD *) &hookLoadSprite, (DWORD *) &origLoadSprite);
	Hooks::polyhook("ProcessSprite", addrProcessSprite, (DWORD *) &hookProcessSprite, (DWORD *) &origProcessSprite);
	Hooks::polyhook("DestroySprite", addrDestroySprite, (DWORD *) &hookDestroySprite, (DWORD *) &origDestroySprite);
	Hooks::polyhook("LoadSprite_OpenVfsReader", addrLoadSprite_OpenVfsReader, (DWORD *) &hookLoadSprite_OpenVfsReader, (DWORD *) &origLoadSprite_OpenVfsReader);

	auto * lua = Lua::getInstance().getState();
	lua->set_function("registerCustomSprite", [](std::string filename, sol::optional<int> frames, sol::optional<int> flags) {
		return registerCustomSprite(filename, frames.value_or(0), flags.value_or(-1));
	});
	auto spriteOverrideLua = [](int targetId, sol::object source, sol::optional<int> frames, sol::optional<int> flags) {
		if(source.is<std::string>())
			setSpriteOverrideFile(targetId, source.as<std::string>(), frames.value_or(0), flags.value_or(-1));
		else if(source.is<int>())
			setSpriteOverride(targetId, source.as<int>());
		else
			spriteLog("SetSpriteOverride: target %d: source is neither sprite id nor file name\n", targetId);
	};
	lua->set_function("SetSpriteOverride", spriteOverrideLua);
	lua->set_function("setSpriteOverride", spriteOverrideLua);
	// Ported packages call these with 3..10 arguments in several orders. A
	// strict int binding raises a Lua error on a short call, which aborts the
	// rest of the package callback (weapon logic included). Draw only the
	// canonical (posy, layer, posx, sprite, frame[, ignored...]) form and log
	// every other shape once instead of throwing.
	lua->set_function("drawSpriteLocal", [](sol::variadic_args va) { drawSpriteChecked("drawSpriteLocal", va, &callDrawSpriteLocal); });
	lua->set_function("drawSpriteGlobal", [](sol::variadic_args va) { drawSpriteChecked("drawSpriteGlobal", va, &callDrawSpriteGlobal); });
	return 0;
}




