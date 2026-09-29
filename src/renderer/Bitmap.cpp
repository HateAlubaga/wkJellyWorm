#include "Bitmap.h"
#include "../Hooks.h"
#include "../Game.h"
#include "../MemAccess.h"
#include "../Lua.h"
#include <sol/sol.hpp>
#include "../Config.h"
#include "../WaLibc.h"
#include "Renderer.h"
#include "Palette.h"
#include <stb_image.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <Windows.h>

DWORD origConstructBitmapImage;

Bitmap::BitmapImage * __stdcall hookConstructBitmapImage(int width) {
	int bitdepth, height;
	Bitmap::BitmapImage * This, * retv;
	_asm mov bitdepth, ecx
	_asm mov height, edi
	_asm mov This, esi

	_asm push width
	_asm mov ecx, bitdepth
	_asm mov edi, height
	_asm mov esi, This
	_asm call origConstructBitmapImage
	_asm mov retv, eax

	if(Config::rendererEnabled && This->bitdepth_dwordC == 8 && Renderer::isActive()) {
		BitmapExtension::constructBitmapImageExtension(This);
	}

	return retv;
}

DWORD (__fastcall *origDestroyBitmapImage)(Bitmap::BitmapImage * This, int EDX, char a2);
DWORD __fastcall hookDestroyBitmapImage(Bitmap::BitmapImage * This, int EDX, char a2) {
//	if(a2) Renderer::onDestroyBitmapImage(This);
	if(Config::rendererEnabled && This->bitdepth_dwordC == 8 && Renderer::isActive()) {
		BitmapExtension::destroyBitmapImageExtension(This);
	}
	return origDestroyBitmapImage(This, EDX, a2);
}

Bitmap::BitmapTextbox *(__fastcall *origConstructTextbox)(DWORD DDDisplay, int EDX, Bitmap::BitmapTextbox *Dst, int length, int fontid);
Bitmap::BitmapTextbox *__fastcall hookConstructTextbox(DWORD DDDisplay, int EDX, Bitmap::BitmapTextbox *Dst, int length, int fontid) {
//	printf("constructTextbox: length: %d fontid: %d\n", length, fontid);
	return origConstructTextbox(DDDisplay, EDX, Dst, length, fontid);
}

int (__stdcall *origSetTextboxText)(Bitmap::BitmapTextbox *textbox, char *msg, int textcolor, int color1, int color2, int* a6, int* a7, int opacity);
int __stdcall hookSetTextboxText(Bitmap::BitmapTextbox *textbox, char *msg, int textcolor, int color1, int color2, int* a6, int* a7, int opacity) {
//	printf("setTextboxText: %X msg: %s textcolor: %d color1: %d color2: %d a6: %d a7: %d opacity: %X\n",
//		   (DWORD)textbox, msg, textcolor, color1, color2, *a6, *a7, opacity);
	return origSetTextboxText(textbox, msg, textcolor, color1, color2, a6, a7, opacity);
}

int (__fastcall *origDrawBitmapGlobal)(DWORD gamescene, int a2, int a3, int a4, Bitmap::BitmapImage *bitmap, int a6, int a7, int a8, int a9);
int __fastcall hookDrawBitmapGlobal(DWORD gamescene, int posY, int a3, int posX, Bitmap::BitmapImage *bitmap, int a6, int a7, int a8, int a9) {
//	printf("DrawBitmapGlobal: gamescene: %X posY: %d y: %d posX: %d bitmap: %X a6: %d a7: %d a8: %d a9: %d\n", gamescene, posY / 0xFFFF, y, posX / 0xFFFF, (DWORD)bitmap, a6, a7, a8, a9);
	return origDrawBitmapGlobal(gamescene, posY, a3, posX, bitmap, a6, a7, a8, a9);
}

void Bitmap::callDrawBitmapGlobal(int posY, int a3, int posX, Bitmap::BitmapImage *bitmap, int a6, int a7, int a8, int a9) {
	int gamescene = (int)Game::getAddrGameScene();
	if(!gamescene)
		return;
	hookDrawBitmapGlobal(gamescene, posY, a3, posX, bitmap, a6, a7, a8, a9);
}

int (__fastcall *origDrawTextboxLocal)(int *gamescene, int a2, int a3, int a4, Bitmap::BitmapTextbox *textbox, int a6, int a7, int a8);
int __fastcall hookDrawTextboxLocal(int *gamescene, int posY, int a3, int posX, Bitmap::BitmapTextbox *textbox, int a6, int a7, int a8) {
//	printf("drawTextboxLocal: posY: %d y: %d posX: %d textbox: %X a6: %d a7: %d a8: %d\n", posY / 0xFFFF, y, posX / 0xFFFF, (DWORD)textbox, a6, a7, a8);
	return origDrawTextboxLocal(gamescene, posY, a3, posX, textbox, a6, a7, a8);
}

Bitmap::BitmapTextbox *Bitmap::callConstructTextbox(DWORD DDDisplay, BitmapTextbox *Dst, int length, int fontid) {
	if(!origConstructTextbox || !DDDisplay || !Dst)
		return Dst;
	return origConstructTextbox(DDDisplay, 0, Dst, length, fontid);
}

int Bitmap::callSetTextboxText(BitmapTextbox *textbox, char *msg, int textcolor, int color1, int color2, int *a6, int *a7, int opacity) {
	if(!origSetTextboxText || !textbox || !msg)
		return 0;
	return origSetTextboxText(textbox, msg, textcolor, color1, color2, a6, a7, opacity);
}

int Bitmap::callDrawTextboxLocal(int *gamescene, int posY, int a3, int posX, BitmapTextbox *textbox, int a6, int a7, int a8) {
	if(!origDrawTextboxLocal || !gamescene || !textbox)
		return 0;
	return origDrawTextboxLocal(gamescene, posY, a3, posX, textbox, a6, a7, a8);
}

//(*(*a1_v8->dword14 + 36))(a1_v8->dword14, font_v29, msg_a2[v17], &v31, &v32)
int (__fastcall *origDrawTextOnBitmap)(DWORD * DDDisplay, int EDX, int font, Bitmap::BitmapImage * bitmap, int horizontalAlign, int verticalAlign, char * msg, int *a7, int *a8);
int __fastcall hookDrawTextOnBitmap(DWORD * DDDisplay, int EDX, int font, Bitmap::BitmapImage * bitmap, int horizontalAlign, int verticalAlign, char * msg, int* a7, int* a8) {
//	printf("hookDrawTextOnBitmap: font: %d ha: %d va: %d msg: %s a7: %d a8: %d\n", font, horizontalAlign, verticalAlign, msg, a7, a8);
	auto ret = origDrawTextOnBitmap(DDDisplay, EDX, font, bitmap, horizontalAlign, verticalAlign, msg, a7, a8);
	if(Config::rendererEnabled && Renderer::isActive()) {
		auto extension = BitmapExtension::findBitmapExtension(bitmap);
		if(extension)
			extension->redraw = true;
	}
	return ret;
}


DWORD addrLoadImgFromFile;
static Bitmap::BitmapImage* callNativeLoadImgFromFile(DWORD a1, int a2, char * path) {
	if(!a1 && Game::isValidDdGame())
		a1 = *(DWORD*)(Game::getAddrDDGame() + 0x4C0); // dd display
	DWORD ddDisplay = Game::getAddrDdDisplay();
	if(!a2 && ddDisplay && jellyProbeRead(ddDisplay + 0x3120, sizeof(DWORD)))
		a2 = *(DWORD*)(ddDisplay + 0x3120); // img palette
	Bitmap::BitmapImage* retv;
	_asm mov esi, a1
	_asm push path
	_asm push a2
	_asm call addrLoadImgFromFile
	_asm mov retv, eax
	return retv;
}

static bool hasPngExtension(const char *path) {
	if(!path)
		return false;
	const char *dot = strrchr(path, '.');
	if(!dot)
		return false;
	std::string extension(dot);
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });
	return extension == ".png";
}

static unsigned char nearestPaletteIndex(const unsigned char *rgba, const GamePalette *palette) {
	if(rgba[3] < 128)
		return 0;
	if(!palette)
		return (unsigned char)(std::max)(1, (int)(0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2]));

	int bestIndex = 1;
	unsigned int bestDistance = ~0u;
	for(int i = 1; i < 256; i++) {
		int dr = (int)rgba[0] - palette->colors[i].r;
		int dg = (int)rgba[1] - palette->colors[i].g;
		int db = (int)rgba[2] - palette->colors[i].b;
		unsigned int distance = (unsigned int)(dr * dr + dg * dg + db * db);
		if(distance < bestDistance) {
			bestDistance = distance;
			bestIndex = i;
		}
	}
	return (unsigned char)bestIndex;
}

// The WA bitmap constructor owns the pixel allocation.  Do not assume that
// width * height is the allocation size: rows are padded to rowsize.
static bool bitmapLayoutIsValid(const Bitmap::BitmapImage *image, int width, int height) {
	if(!image || image->bitdepth_dwordC != 8
		|| width <= 0 || height <= 0
		|| width > 4096 || height > 4096
		|| image->max_width_dword14 < width
		|| image->max_height_dword18 < height
		|| image->rowsize_dword10 < width
		|| image->rowsize_dword10 > width * 16
		|| !image->data_dword8)
		return false;

	size_t stride = (size_t)image->rowsize_dword10;
	size_t bytes = stride * (size_t)height;
	if(bytes / stride != (size_t)height || bytes > 64u * 1024u * 1024u)
		return false;
	return jellyProbeRead((DWORD)image->data_dword8, bytes);
}

static bool bitmapStorageIsCommitted(const Bitmap::BitmapImage *image) {
	if(!image || !image->data_dword8 || image->rowsize_dword10 <= 0
		|| image->max_height_dword18 <= 0)
		return false;

	size_t bytes = (size_t)image->rowsize_dword10
		* (size_t)image->max_height_dword18;
	if(bytes / (size_t)image->rowsize_dword10
		!= (size_t)image->max_height_dword18)
		return false;

	MEMORY_BASIC_INFORMATION mbi{};
	if(!VirtualQuery((LPCVOID)(DWORD)image->data_dword8, &mbi, sizeof(mbi))
		|| mbi.State != MEM_COMMIT)
		return false;
	ULONG_PTR dataEnd = (ULONG_PTR)(DWORD)image->data_dword8 + bytes;
	ULONG_PTR regionEnd = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
	return dataEnd >= (ULONG_PTR)(DWORD)image->data_dword8
		&& dataEnd <= regionEnd;
}

static Bitmap::BitmapImage *constructNativeBitmapImage(int width, int height) {
	if(!origConstructBitmapImage || width <= 0 || height <= 0)
		return nullptr;
	auto *image = (Bitmap::BitmapImage *)WaLibc::waMalloc(sizeof(Bitmap::BitmapImage));
	if(!image)
		return nullptr;
	memset(image, 0, sizeof(Bitmap::BitmapImage));
	Bitmap::BitmapImage *retv;
	_asm mov ecx, 8
	_asm mov edi, height
	_asm mov esi, image
	_asm push width
	_asm call origConstructBitmapImage
	_asm mov retv, eax
	if(!retv) {
		WaLibc::waFree(image);
		return nullptr;
	}
	return retv;
}

static Bitmap::BitmapImage *loadPngImageFile(int a2, char *path) {
	if(!path)
		return nullptr;
	int width = 0, height = 0, channels = 0;
	unsigned char *rgba = stbi_load(path, &width, &height, &channels, 4);
	if(!rgba || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
		if(rgba)
			stbi_image_free(rgba);
		printf("Bitmap: failed to load PNG '%s'\n", path);
		return nullptr;
	}

	auto *image = constructNativeBitmapImage(width, height);
	if(!bitmapLayoutIsValid(image, width, height)) {
		if(image && origDestroyBitmapImage)
			origDestroyBitmapImage(image, 0, 1);
		stbi_image_free(rgba);
		printf("Bitmap: rejected PNG '%s': invalid WA 8-bit bitmap layout\n", path);
		return nullptr;
	}

	auto *palette = (GamePalette *)(DWORD)a2;
	for(int y = 0; y < height; y++) {
		auto *dst = (unsigned char *)image->data_dword8 + y * image->rowsize_dword10;
		for(int x = 0; x < width; x++) {
			dst[x] = nearestPaletteIndex(rgba + 4 * (y * width + x), palette);
		}
		// WA may read the padded tail while preparing/remapping an image.
		if(image->rowsize_dword10 > width)
			memset(dst + width, 0, (size_t)image->rowsize_dword10 - width);
	}
	image->current_width_dword24 = width;
	image->current_height_dword28 = height;
	if(Config::rendererEnabled && !BitmapExtension::findBitmapExtension(image) && Renderer::isActive())
		BitmapExtension::constructBitmapImageExtension(image);
	stbi_image_free(rgba);
	printf("Bitmap: loaded PNG '%s' as 8-bit indexed %dx%d\n", path, width, height);
	return image;
}

Bitmap::BitmapImage* Bitmap::loadImageFile(DWORD a1, int a2, char * path) {
	// Switchable from the ini ([general] PngBitmapLoader) rather than being a
	// compile-time constant, so it can be turned off again without a rebuild if
	// the WA prepare/remap contract misbehaves. Still off by default: the
	// ported ProjectX packages are what need it, since they pass .png straight
	// to registerCustomSprite.
	const bool kEnablePngBitmapLoader = Config::pngBitmapLoaderEnabled != 0;
	if(!a2) {
		DWORD ddDisplay = Game::getAddrDdDisplay();
		if(ddDisplay && jellyProbeRead(ddDisplay + 0x3120, sizeof(DWORD)))
			a2 = *(DWORD*)(ddDisplay + 0x3120);
	}
	if(hasPngExtension(path)) {
		if(!kEnablePngBitmapLoader) {
			printf("Bitmap: PNG loader disabled; refusing '%s'\n", path ? path : "(null)");
			return nullptr;
		}
		return loadPngImageFile(a2, path);
	}
	auto *image = callNativeLoadImgFromFile(a1, a2, path);
	if(!image || !jellyProbeRead((DWORD)image, sizeof(Bitmap::BitmapImage))) {
		printf("Bitmap: native IMG '%s' returned no readable bitmap\n",
			path ? path : "(null)");
		return nullptr;
	}
	return image;
}

Bitmap::BitmapImage* Bitmap::callLoadImgFromFile(DWORD a1, int a2, char * path) {
	return loadImageFile(a1, a2, path);
}

DWORD addrCreatePalette;
int Bitmap::callCreatePalette(void *data) {
	int retv;
	_asm mov eax, data
	_asm call addrCreatePalette
	_asm mov retv, eax
	return retv;
}

//BitmapImage *__userpurge prepare_img_sub_4F5C00@<eax>(, int palette_a2, int *a3, char a4)
DWORD origPrepareImg;
Bitmap::BitmapImage * __stdcall hookPrepareImg(GamePalette * palette, int *a3, int a4) {
	DWORD a1;
	Bitmap::BitmapImage * bitmap;
	_asm mov a1, eax

	_asm mov eax, a1
	_asm push a4
	_asm push a3
	_asm push palette
	_asm call origPrepareImg
	_asm mov bitmap, eax

	if(Config::rendererEnabled && Renderer::isActive() && bitmap->bitdepth_dwordC == 8) {
//		auto * data = BitmapExtension::convertBitmapImageToGLRGBA(bitmap, palette, nullptr);
		auto data = (unsigned char*)bitmap->data_dword8;
		BitmapExtension::populateBitmapImageExtension(bitmap, data);
	}

	return bitmap;
}


int Bitmap::install(SignatureScanner &, module) {
	DWORD addrConstructBitmapImage = Hooks::scanPattern("ConstructBitmapImage", "\x53\x55\x8B\x6C\x24\x0C\x8B\xC5\x0F\xAF\xC1\x83\xC0\x07\x99\x83\xE2\x07\x03\xC2\xC1\xF8\x03\x83\xC0\x03\x83\xE0\xFC\x89\x46\x10\x0F\xAF\xC7", "??????xxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 0x4F6370);
	addrBitmapImageVtable = *(DWORD*)(addrConstructBitmapImage + 0x33);
	addrBitmapImage1Vtable = addrBitmapImageVtable + 0x2C;
	addrBitmapImage8Vtable = addrBitmapImage1Vtable + 0x2C;
	DWORD addrDestroyBitmapImage = *(DWORD*)(addrBitmapImageVtable + 0xC);
	printf("addrDestroyBitmapImage: %X\n", addrDestroyBitmapImage);
	DWORD addrConstructTextbox = Hooks::scanPattern("ConstructTextbox", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x51\x8B\x44\x24\x1C\x53\x8B\x5C\x24\x1C\x55\x8B\x6C\x24\x1C\x56\x57\x8B\xF1\x89\x45\x08\x8D\x7D\x04\x57\x8D\x4C\x24\x30\x51", "???????xx????xxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", 0x4FAF00);
	DWORD addrSetTextboxText = Hooks::scanPattern("SetTextboxText", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x8B\x44\x24\x18\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x10\x53\x8B\x5C\x24\x24\x3B\x83\x00\x00\x00\x00\x55\x8B\x6C\x24\x3C\x56\x8B\x74\x24\x3C", "???????xx????xxxxxxxx????xxxxxxxxxx????xxxxxxxxxx", 0x4FB070);
	DWORD addrDrawBitmapGlobal = Hooks::scanPattern("DrawBitmapGlobal", "\x8B\x81\x00\x00\x00\x00\x3D\x00\x00\x00\x00\x56\x8B\x74\x24\x0C\x7D\x79\x57\x8B\x39\x83\xC7\xD8\x78\x70\x89\x39\x8D\x7C\x0F\x04\x89\xBC\x81\x00\x00\x00\x00\x8B\xB9\x00\x00\x00\x00\x8B\x84\xB9\x00\x00\x00\x00\x83\xC7\x01\x85\xC0\x89\xB9\x00\x00\x00\x00\x74\x49", "??????x????xxxxxxxxxxxxxxxxxxxxxxxx????xx????xxx????xxxxxxx????x", 0x542170);
	DWORD addrDrawTextboxLocal = Hooks::scanPattern("DrawTextboxLocal", "\x8B\x81\x00\x00\x00\x00\x3D\x00\x00\x00\x00\x56\x8B\x74\x24\x0C\x7D\x7D\x53\x57\x8B\x39\x83\xC7\xCC\x33\xDB\x3B\xFB\x7C\x6E\x89\x39\x8D\x7C\x0F\x04\x89\xBC\x81\x00\x00\x00\x00\x8B\xB9\x00\x00\x00\x00\x8B\x84\xB9\x00\x00\x00\x00\x83\xC7\x01\x3B\xC3\x89\xB9\x00\x00\x00\x00\x74\x47\x8B\x4C\x24\x10", "??????x????xxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xx????xxx????xxxxxxx????xxxxxx", 0x542200);
	DWORD addrDrawTextOnBitmap = Hooks::scanPattern("DrawTextOnBitmap", "\x8B\x54\x24\x04\x8B\xC2\x81\xE2\x00\x00\x00\x00\x56\x8D\x72\xFF\x25\x00\x00\x00\x00\x83\xFE\x1E\x77\x34\x8B\xB4\x91\x00\x00\x00\x00\x85\xF6\x74\x29\x8B\x4C\x24\x18\x8B\x54\x24\x14\xC1\xF8\x10", "??????xx????xxxxx????xxxxxxxx????xxxxxxxxxxxxxxx", 0x5236B0);
	addrCreatePalette = Hooks::scanPattern("CreatePalette", "\x66\x8B\x48\x02\x66\x2B\x08\x33\xD2\x66\x83\xC1\x01\x66\x85\xC9\x66\xC7\x80\x00\x00\x00\x00\x00\x00\x66\x89\x88\x00\x00\x00\x00\x7E\x1A\x8A\x48\x02\x2A\xCA\x88\x8C\x10\x00\x00\x00\x00", "??????xxxxxxxxxxxxx??????xxx????xxxxxxxxxx????", 0x5411A0);

	DWORD addrPrepareImg = Hooks::scanPattern("PrepareImg", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x8B\x9C\x24\x00\x00\x00\x00\x55\x8B\xAC\x24\x00\x00\x00\x00\x8B\x45\x00\x8B\x50\x08\x8B\xCD\xFF\xD2", "???????xx????xxxx????xx????xxxx????xxxx????xxxxxxxxxx", 0x4F5F80);

//	DWORD addrVfsCreateGfxDirReader = Hooks::scanPattern("VfsCreateGfxDirReader","\x83\xBE\x00\x00\x00\x00\x00\x75\x05\x33\xC0\xC2\x04\x00\x8B\x44\x24\x04\x53\x56\xE8\x00\x00\x00\x00", "???????xxxxxxxxxxxxxx????", 0x566260);
//	DWORD addrLoadImgFromVfs = Hooks::scanPattern("LoadImgFromVfs", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x8B\x9C\x24\x00\x00\x00\x00\x55\x8B\xAC\x24\x00\x00\x00\x00\x8B\x45\x00", "???????xx????xxxx????xx????xxxx????xxxx????xxx", 0x4F5C00);
	addrLoadImgFromFile = Hooks::scanPattern("LoadImgFromFile", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x0C\x8B\x44\x24\x20\x56\x68\x00\x00\x00\x00\x50\xC7\x44\x24\x00\x00\x00\x00\x00", "???????xx????xxxx????xxxxxxxxx????xxxx?????", 0x57A900);


	Hooks::polyhook("ConstructBitmapImage", addrConstructBitmapImage, (DWORD *) &hookConstructBitmapImage, (DWORD *) &origConstructBitmapImage);
	Hooks::polyhook("DestroyBitmapImage", addrDestroyBitmapImage, (DWORD *) &hookDestroyBitmapImage, (DWORD *) &origDestroyBitmapImage);
	Hooks::polyhook("ConstructTextbox", addrConstructTextbox, (DWORD *) &hookConstructTextbox, (DWORD *) &origConstructTextbox);
	Hooks::polyhook("SetTextboxText", addrSetTextboxText, (DWORD *) &hookSetTextboxText, (DWORD *) &origSetTextboxText);
	Hooks::polyhook("DrawBitmapGlobal", addrDrawBitmapGlobal, (DWORD *) &hookDrawBitmapGlobal, (DWORD *) &origDrawBitmapGlobal);
	Hooks::polyhook("DrawTextboxLocal", addrDrawTextboxLocal, (DWORD *) &hookDrawTextboxLocal, (DWORD *) &origDrawTextboxLocal);
	Hooks::polyhook("DrawTextOnBitmap", addrDrawTextOnBitmap, (DWORD *) &hookDrawTextOnBitmap, (DWORD *) &origDrawTextOnBitmap);
	Hooks::polyhook("PrepareImg", addrPrepareImg, (DWORD *) &hookPrepareImg, (DWORD *) &origPrepareImg);



	auto * lua = Lua::getInstance().getState();
	return 0;
}

DWORD Bitmap::getAddrBitmapImageVtable() {
	return addrBitmapImageVtable;
}

DWORD Bitmap::getAddrBitmapImage8Vtable() {
	return addrBitmapImage8Vtable;
}

DWORD Bitmap::getAddrBitmapImage1Vtable() {
	return addrBitmapImage1Vtable;
}

