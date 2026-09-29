
#ifndef WKJELLYWORM_RENDERER_H
#define WKJELLYWORM_RENDERER_H


#include <sigscanner.h>
#include "../Constants.h"
#include "Palette.h"
#include "Bitmap.h"
#include "BitmapExtension.h"

#include <array>
#include <GL/glew.h>
#include <map>
#include <windows.h>
#include "../Sprites.h"


class CTaskTurnGame;
class CTask;
class GamePalette;
class Drawing;

class Renderer {
public:
	struct OpenGLRenderer
	{
		DWORD dword0;
		DWORD dword4;
		DWORD dword8;
		DWORD dwordC;
		DWORD dword10;
		DWORD dword14;
		DWORD dword18;
		DWORD dword1C;
		DWORD dword20;
		DWORD dword24;
		DWORD dword28;
		DWORD width_dword2C;
		DWORD height_dword30;
		DWORD dword34;
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
		DWORD dword60;
		DWORD dword64;
		DWORD dword68;
		DWORD dword6C;
		DWORD dword70;
		DWORD dword74;
		DWORD dword78;
		DWORD dword7C;
	};

	struct BitmapDescriptor {
		DWORD vtable;
		DWORD width_dword4;
		DWORD height_dword8;
		DWORD data_dwordC;
		DWORD dword10;
		// hax
		Bitmap::BitmapImage * bitmap;
	};



private:
	static int CTaskTurnGame_vt8(CTaskTurnGame *, CTask * sender, Constants::TaskMessage mtype, size_t size, void * data);

	static DWORD __fastcall hookDrawBitmapOnBitmapAnyDepth(int flags, int srcOffsetY, int posX, int posY, int endX, int endY, int srcOffsetX, Bitmap::BitmapImage *Src, Bitmap::BitmapImage *Dst, int a10);
	// WA OpenGLInit is thiscall: ECX=OpenGLRenderer*, stack width/height.
	static DWORD __fastcall hookOpenGLInit(OpenGLRenderer *This, int EDX, int width, int height);

	static DWORD __fastcall hookDestroyBitmapDescriptor(Renderer::BitmapDescriptor * This, int EDX, int a2);
	static BitmapDescriptor* __stdcall hookConstructBitmapDescriptor();

	static HGLRC __stdcall hookOpenGLCreateContext_cpp(HDC dc);
	static DWORD __fastcall hookOpenGLSwapBuffers(int a1, DWORD a2);
	static void __stdcall hookRenderDrawingQueue(DWORD dddisplay, DWORD a3);
	static DWORD __stdcall hookWriteScreenPalette(DWORD a2);

	static void disableRenderer(const char * reason);
	// Per-frame GL override: clear/setup backbuffer at RenderScene (before bitmap blits).
	static void frameOverride();

	static inline int screenWidth, screenHeight;
	static inline bool glewInitialized = false;
	static inline bool glContextCreated = false;
	static inline bool shadersCompiled = false;
	static inline HDC glHdc = nullptr;
	static inline HGLRC glHglrc = nullptr;
	static inline DWORD * addrBitmapDescriptorVTable = nullptr;
	static inline DWORD addrScreenPalette = 0;
	static inline bool forceDisableNativeDrawing = false;

	static inline bool insideRenderDrawingQueue = false;
	static inline float rot = 0;
public:
	static int onConstructGlobalContext(int a1);
	static int install(SignatureScanner &, module);

	static bool ensureGLContext();
	// True only when custom GL path is actually live (safe to alter native draw).
	static bool isActive();
	// If the game already created a GL context (e.g. before our DLL hooked OpenGLInit), attach to it.
	static bool tryLateInitGL();
	static void toggleNativeDrawing();
	friend class Drawing;
	friend class BitmapExtension;
	friend class GLDraw;
};


#endif //WKJELLYWORM_RENDERER_H
