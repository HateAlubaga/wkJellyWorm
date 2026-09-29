
#include <functional>
#include <cassert>
#include "Renderer.h"
#include "Bitmap.h"
#include "../Game.h"
#include "../WaLibc.h"
#include "../Hooks.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <set>
#include "Palette.h"
#include "../Config.h"
#include "../LobbyGui.h"
#include "../Lobby.h"
#include "Drawing.h"
#include "DebugLog.h"


int Renderer::onConstructGlobalContext(int a1) {
	return 0;
}

int Renderer::CTaskTurnGame_vt8(CTaskTurnGame *This, CTask * sender, Constants::TaskMessage mtype, size_t size, void * data) {
	(void)This;
	(void)sender;
	(void)size;
	(void)data;
	if(mtype == Constants::TaskMessage_RenderScene && Config::rendererEnabled && Config::rendererSkipNativeBlit) {
		frameOverride();
	}
	return 0;
}

void Renderer::frameOverride() {
	static unsigned int frameCount = 0;
	tryLateInitGL();
	if(!ensureGLContext()) {
		// #region agent log
		{
			static unsigned int skipN = 0;
			skipN++;
			if(skipN <= 5) {
				char data[128];
				sprintf_s(data, "{\"n\":%u,\"created\":%d,\"glew\":%d,\"ctx\":%d}",
					skipN, (int)glContextCreated, (int)glewInitialized, (int)(wglGetCurrentContext() != nullptr));
				agentDebugLog("H2", "Renderer.cpp:frameOverride", "skipped_no_context", data);
			}
		}
		// #endregion
		return;
	}

	// Own the backbuffer for this frame: reset default FBO, viewport, then clear.
	// Bitmap blits during the frame redraw the scene; SwapBuffers presents it.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, screenWidth, screenHeight);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glUseProgram(0);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	// #region agent log
	frameCount++;
	if(frameCount <= 5 || (frameCount % 120) == 0) {
		char data[160];
		sprintf_s(data, "{\"n\":%u,\"w\":%d,\"h\":%d}", frameCount, screenWidth, screenHeight);
		agentDebugLog("H2", "Renderer.cpp:frameOverride", "cleared", data);
	}
	// #endregion

	if(Config::rendererDebugPoints)
		rot += 0.3f;
}

void Renderer::disableRenderer(const char * reason) {
	printf("Renderer: disabling custom renderer: %s\n", reason ? reason : "unknown");
	// #region agent log
	{
		char data[256];
		sprintf_s(data, "{\"reason\":\"%.180s\"}", reason ? reason : "unknown");
		agentDebugLog("H3", "Renderer.cpp:disableRenderer", "disabled", data);
	}
	// #endregion
	if(Config::rendererFallbackOnContextError) {
		Config::rendererEnabled = 0;
	}
	glContextCreated = false;
	glewInitialized = false;
	shadersCompiled = false;
}

bool Renderer::ensureGLContext() {
	if(!Config::rendererEnabled)
		return false;
	if(!glContextCreated || !glewInitialized)
		return false;
	if(!wglGetCurrentContext()) {
		if(glHdc && glHglrc) {
			if(!wglMakeCurrent(glHdc, glHglrc)) {
				printf("Renderer::ensureGLContext: wglMakeCurrent failed\n");
				return false;
			}
		} else {
			return false;
		}
	}
	return true;
}

bool Renderer::isActive() {
	// SkipNativeBlit gates destructive overrides. With it off we still init GL/shaders
	// for diagnostics, but leave native OpenGL CPU draw/present untouched.
	return Config::rendererEnabled && Config::rendererSkipNativeBlit
		&& glContextCreated && glewInitialized && shadersCompiled;
}

bool Renderer::tryLateInitGL() {
	if(!Config::rendererEnabled || !Config::rendererSkipNativeBlit)
		return false;
	if(shadersCompiled)
		return true;

	HGLRC ctx = wglGetCurrentContext();
	HDC dc = wglGetCurrentDC();

	// #region agent log
	{
		static unsigned int n = 0;
		n++;
		if(n <= 8) {
			char data[160];
			sprintf_s(data, "{\"n\":%u,\"ctx\":%u,\"dc\":%u,\"created\":%d,\"glew\":%d}",
				n, (unsigned)(uintptr_t)ctx, (unsigned)(uintptr_t)dc,
				(int)glContextCreated, (int)glewInitialized);
			agentDebugLog("H10", "Renderer.cpp:tryLateInitGL", "probe", data);
		}
	}
	// #endregion

	if(!ctx || !dc)
		return false;

	glHglrc = ctx;
	glHdc = dc;
	glContextCreated = true;

	if(!glewInitialized) {
		GLenum glewErr = glewInit();
		if(glewErr != GLEW_OK) {
			printf("tryLateInitGL: glewInit failed: %s\n", glewGetErrorString(glewErr));
			// #region agent log
			agentDebugLog("H10", "Renderer.cpp:tryLateInitGL", "glew_fail", "{}");
			// #endregion
			return false;
		}
		glewInitialized = true;
	}

	if(!shadersCompiled) {
		if(!Drawing::compileShaders()) {
			// #region agent log
			agentDebugLog("H10", "Renderer.cpp:tryLateInitGL", "shader_fail", "{}");
			// #endregion
			return false;
		}
		shadersCompiled = true;
		Drawing::genPaletteTexture(addrScreenPalette ? addrScreenPalette : 0);
		printf("tryLateInitGL: attached to existing GL context, shaders ready\n");
		// #region agent log
		agentDebugLog("H10", "Renderer.cpp:tryLateInitGL", "success", "{}");
		// #endregion
	}
	return true;
}


DWORD origBlitScreen;
DWORD __stdcall hookBlitScreen(int width_a3, int a4, DWORD * output_buffer, int a6, DWORD * palette_a7) {
	if(Renderer::isActive() && Config::rendererSkipNativeBlit)
		return 0;

	DWORD height_a1, retv;
	DWORD * source_buffer;
	_asm mov height_a1, eax
	_asm mov source_buffer, ecx

	_asm mov eax, height_a1
	_asm mov ecx, source_buffer
	_asm push palette_a7
	_asm push a6
	_asm push output_buffer
	_asm push a4
	_asm push width_a3
	_asm call origBlitScreen
	_asm mov retv, eax
	return retv;
}

Renderer::BitmapDescriptor* (__stdcall *origConstructBitmapDescriptor)();
Renderer::BitmapDescriptor* __stdcall Renderer::hookConstructBitmapDescriptor() {
	// Use native descriptor layout until GL is live — extended structs break DD/frontend.
	if(!isActive()) return origConstructBitmapDescriptor();
	auto ret = (BitmapDescriptor*) WaLibc::waMalloc(sizeof(BitmapDescriptor));
	if(!ret) return ret;
	memset(ret, 0, sizeof(BitmapDescriptor));
	ret->vtable = (DWORD)addrBitmapDescriptorVTable;
	return ret;
}

DWORD (__fastcall *origDestroyBitmapDescriptor)(Renderer::BitmapDescriptor * This, int EDX, int a2);
DWORD __fastcall Renderer::hookDestroyBitmapDescriptor(Renderer::BitmapDescriptor * This, int EDX, int a2) {
	if(isActive() && This->vtable == (DWORD)addrBitmapDescriptorVTable && This->bitmap) {
		BitmapExtension::destroyBitmapImageExtension(This->bitmap);
		WaLibc::waFree((void*)This->bitmap);
		This->bitmap = 0;
	}
	return origDestroyBitmapDescriptor(This, EDX, a2);
}

DWORD (__fastcall *origDrawBitmapOnBitmapAnyDepth)(int flags, int srcOffsetY, int posX, int posY, int endX, int endY, int srcOffsetX, Bitmap::BitmapImage *Src, Bitmap::BitmapImage *Dst, int a10);
DWORD __fastcall Renderer::hookDrawBitmapOnBitmapAnyDepth(int flags, int srcOffsetY, int posX, int posY, int endX, int endY, int srcOffsetX, Bitmap::BitmapImage *Src, Bitmap::BitmapImage *Dst, int a10) {
	if(isActive() && Src && Dst
	   && Src->bitdepth_dwordC == 8 && Dst->bitdepth_dwordC == 8
	   && ensureGLContext()) {
		int width = endX - posX;
		int height = endY - posY;
		if(width > 0 && height > 0) {
			Drawing::callDrawBitmapOnBitmap(Dst, posX, posY, width, height, Src, srcOffsetX, srcOffsetY, a10, flags);
			return 0;
		}
	}
	return origDrawBitmapOnBitmapAnyDepth(flags, srcOffsetY, posX, posY, endX, endY, srcOffsetX, Src, Dst, a10);
}


DWORD (__fastcall *origMarkScreenAsReady)(Renderer::OpenGLRenderer * a1, DWORD a2);
int (__fastcall *origOpenGLDraw)(Renderer::OpenGLRenderer * a1, int a2, int a3);
int __fastcall hookOpenGLDraw(Renderer::OpenGLRenderer * a1, int a2, int a3) {
	Renderer::tryLateInitGL();
	// Custom GL currently only blits some 8-bit bitmaps (often offscreen FBOs).
	// Skipping native OpenGLDraw blanks the frontend/backbuffer (black screen).
	// Only replace native present when SkipNativeBlit is explicitly enabled (via isActive).
	if(!Renderer::isActive()) {
		// #region agent log
		{
			static unsigned int n = 0;
			n++;
			if(n <= 5) {
				char data[128];
				sprintf_s(data, "{\"n\":%u,\"active\":0,\"skipBlit\":%d,\"passthrough\":1}",
					n, Config::rendererSkipNativeBlit);
				agentDebugLog("H6", "Renderer.cpp:hookOpenGLDraw", "native_passthrough", data);
			}
		}
		// #endregion
		return origOpenGLDraw(a1, a2, a3);
	}
	// #region agent log
	{
		static unsigned int n = 0;
		n++;
		if(n <= 3) {
			char data[64];
			sprintf_s(data, "{\"n\":%u,\"skipNative\":1}", n);
			agentDebugLog("H5", "Renderer.cpp:hookOpenGLDraw", "skip_native_mark_ready", data);
		}
	}
	// #endregion
	return origMarkScreenAsReady(a1, a2);
}

DWORD (__fastcall *origOpenGLSwapBuffers)(int a1, DWORD a2);
DWORD __fastcall Renderer::hookOpenGLSwapBuffers(int a1, DWORD a2) {
	tryLateInitGL();
	// Present only — do not clear after swap (that would wipe the just-presented frame).
	// #region agent log
	{
		static unsigned int n = 0;
		n++;
		if(n <= 3 || (n % 120) == 0) {
			char data[96];
			sprintf_s(data, "{\"n\":%u,\"active\":%d}", n, (int)isActive());
			agentDebugLog("H5", "Renderer.cpp:hookOpenGLSwapBuffers", "present", data);
		}
	}
	// #endregion
	return origOpenGLSwapBuffers(a1, a2);
}

void GLAPIENTRY GLErrorCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
	if (severity != GL_DEBUG_SEVERITY_NOTIFICATION) {
		printf("!!!!!!!!!!!!!!!!!! GLErrorCallback: %s\n", message);
	}
}

DWORD (__fastcall *origOpenGLInit)(Renderer::OpenGLRenderer *This, int EDX, int width, int height);
DWORD __fastcall Renderer::hookOpenGLInit(OpenGLRenderer *This, int EDX, int width, int height) {
	// #region agent log
	{
		char data[160];
		sprintf_s(data, "{\"this\":%u,\"w\":%d,\"h\":%d,\"enabled\":%d}",
			(unsigned)(uintptr_t)This, width, height, Config::rendererEnabled);
		agentDebugLog("H13", "Renderer.cpp:hookOpenGLInit", "before_orig", data);
	}
	// #endregion

	if(!This) {
		// #region agent log
		agentDebugLog("H13", "Renderer.cpp:hookOpenGLInit", "null_this", "{}");
		// #endregion
		return 0;
	}

	auto ret = origOpenGLInit(This, EDX, width, height);

	// #region agent log
	{
		char data[160];
		sprintf_s(data, "{\"ret\":%u,\"curCtx\":%u,\"curDC\":%u}",
			(unsigned)ret,
			(unsigned)(uintptr_t)wglGetCurrentContext(),
			(unsigned)(uintptr_t)wglGetCurrentDC());
		agentDebugLog("H13", "Renderer.cpp:hookOpenGLInit", "after_orig", data);
	}
	// #endregion

	if(Config::rendererEnabled) {
		printf("OpenGL init\n");
		screenWidth = width;
		screenHeight = height;

		// Prefer the context the game just made (no mid-function ASM patch).
		if(!glHglrc)
			glHglrc = wglGetCurrentContext();
		if(!glHdc)
			glHdc = wglGetCurrentDC();
		if(glHglrc)
			glContextCreated = true;

		// #region agent log
		{
			char data[192];
			sprintf_s(data, "{\"w\":%d,\"h\":%d,\"hdc\":%u,\"hglrc\":%u,\"skipBlit\":%d}",
				width, height, (unsigned)(uintptr_t)glHdc, (unsigned)(uintptr_t)glHglrc,
				Config::rendererSkipNativeBlit);
			agentDebugLog("H3", "Renderer.cpp:hookOpenGLInit", "captured_context", data);
		}
		// #endregion

		// Do not touch GL state (glew/shaders/debug) unless we are replacing native draw.
		// glewInit + program binds were leaving the frontend black even with native OpenGLDraw.
		if(!Config::rendererSkipNativeBlit) {
			// #region agent log
			agentDebugLog("H18", "Renderer.cpp:hookOpenGLInit", "defer_glew_until_skip_native",
				"{\"reason\":\"preserve_native_frontend\"}");
			// #endregion
			return ret;
		}

		if(!glHglrc || !glHdc) {
			disableRenderer("OpenGLInit: missing GL context (null HGLRC/HDC)");
			return ret;
		}

		if(!wglMakeCurrent(glHdc, glHglrc)) {
			disableRenderer("OpenGLInit: wglMakeCurrent failed");
			return ret;
		}

		GLenum glewErr = glewInit();
		if(glewErr != GLEW_OK) {
			printf("OpenGLInit: glewInit failed: %s\n", glewGetErrorString(glewErr));
			disableRenderer("OpenGLInit: glewInit failed");
			return ret;
		}
		glewInitialized = true;

		const GLubyte * glVersion = glGetString(GL_VERSION);
		const GLubyte * glslVersion = glGetString(GL_SHADING_LANGUAGE_VERSION);
		printf("OpenGLInit: GL_VERSION=%s GLSL=%s\n",
			   glVersion ? (const char*)glVersion : "?",
			   glslVersion ? (const char*)glslVersion : "?");

		// #region agent log
		{
			char data[320];
			sprintf_s(data, "{\"gl\":\"%.80s\",\"glsl\":\"%.80s\"}",
				glVersion ? (const char*)glVersion : "?",
				glslVersion ? (const char*)glslVersion : "?");
			agentDebugLog("H3", "Renderer.cpp:hookOpenGLInit", "glew_ok", data);
		}
		// #endregion

		if(Config::rendererDebugContext) {
			glDebugMessageCallback(GLErrorCallback, nullptr);
			glEnable(GL_DEBUG_OUTPUT);
			glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		}
		if(!Drawing::compileShaders()) {
			disableRenderer("OpenGLInit: shader compilation failed");
			return ret;
		}
		shadersCompiled = true;
		Drawing::genPaletteTexture(addrScreenPalette ? addrScreenPalette : 0);
		// #region agent log
		agentDebugLog("H4", "Renderer.cpp:hookOpenGLInit", "shaders_and_palette_ready",
			addrScreenPalette ? "{\"palette\":\"addr\"}" : "{\"palette\":\"fallback_rand\"}");
		// #endregion
	}

	return ret;
}

HGLRC __stdcall Renderer::hookOpenGLCreateContext_cpp(HDC dc) {
	glHdc = dc;
	glContextCreated = false;
	glHglrc = nullptr;

	// #region agent log
	{
		char data[96];
		sprintf_s(data, "{\"enabled\":%d,\"dc\":%u}", Config::rendererEnabled, (unsigned)(uintptr_t)dc);
		agentDebugLog("H3", "Renderer.cpp:createContext", "enter", data);
	}
	// #endregion

	if(!Config::rendererEnabled) {
		printf("hookCallOpenGLCreateContext: creating default opengl context\n");
		HGLRC fallback = wglCreateContext(dc);
		if(fallback) {
			glHglrc = fallback;
			glContextCreated = true;
		}
		return fallback;
	}

#define WGL_CONTEXT_DEBUG_BIT_ARB 0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define setAttrib(a, v) \
{ \
    assert(((size_t) index + 1) < sizeof(attribs) / sizeof(attribs[0])); \
    attribs[index++] = a; \
    attribs[index++] = v; \
}

	auto origcontext = wglGetCurrentContext();
	auto temprc = wglCreateContext(dc);
	if(!temprc) {
		printf("Creating custom OpenGL context: failed to create temporary context, falling back\n");
		HGLRC fallback = wglCreateContext(dc);
		if(fallback) {
			glHglrc = fallback;
			glContextCreated = true;
			wglMakeCurrent(dc, fallback);
		} else {
			disableRenderer("failed to create any OpenGL context");
		}
		return fallback;
	}
	wglMakeCurrent(dc, temprc);

	HGLRC (__stdcall *imp_wglCreateContextAttribsARB)(HDC hDC, HGLRC hShareContext, const int *attribList);
	imp_wglCreateContextAttribsARB = (HGLRC (__stdcall *)(HDC, HGLRC, const int *)) wglGetProcAddress("wglCreateContextAttribsARB");

	HGLRC ret = nullptr;
	if(imp_wglCreateContextAttribsARB) {
		int attribs[40];
		memset(attribs, 0, sizeof(attribs));
		int mask = 0, flags = 0, index = 0;

		mask |= WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
		if(Config::rendererDebugContext)
			flags |= WGL_CONTEXT_DEBUG_BIT_ARB;
		setAttrib(WGL_CONTEXT_MAJOR_VERSION_ARB, Config::rendererGLMajor);
		setAttrib(WGL_CONTEXT_MINOR_VERSION_ARB, Config::rendererGLMinor);
		setAttrib(WGL_CONTEXT_FLAGS_ARB, flags);
		setAttrib(WGL_CONTEXT_PROFILE_MASK_ARB, mask);

		HGLRC share = nullptr;
		ret = imp_wglCreateContextAttribsARB(dc, share, attribs);
	} else {
		printf("Creating custom OpenGL context: wglCreateContextAttribsARB unavailable\n");
	}

	wglMakeCurrent(dc, origcontext);
	wglDeleteContext(temprc);

	if(!ret) {
		printf("Creating custom OpenGL context failed (version %d.%d), falling back to default\n",
			   Config::rendererGLMajor, Config::rendererGLMinor);
		ret = wglCreateContext(dc);
		if(!ret) {
			disableRenderer("wglCreateContextAttribsARB and wglCreateContext both failed");
			return nullptr;
		}
		if(Config::rendererFallbackOnContextError) {
			printf("Renderer: using legacy context; custom renderer features may be limited\n");
		}
	}

	glHglrc = ret;
	glContextCreated = true;
	// Activate the new context so subsequent GL calls (glewInit in OpenGLInit) see a current context.
	if(!wglMakeCurrent(dc, ret)) {
		printf("Creating custom OpenGL context: wglMakeCurrent failed after create\n");
	}

	printf("Creating custom OpenGL context, version: %d.%d, result: %X\n", Config::rendererGLMajor, Config::rendererGLMinor, (DWORD)ret);
	return ret;
}

DWORD addrCallOpenGLCreateContext_ret;
DWORD addrWglCreateContext;
void __declspec(naked) hookCallOpenGLCreateContext() {
	_asm push esi
	_asm push edi
	_asm mov ecx,dword ptr ds:[esi+0x18]
	_asm push ecx
	_asm call Renderer::hookOpenGLCreateContext_cpp
	_asm pop edi
	_asm pop esi
	_asm mov dword ptr ds:[esi+0x1C],eax
	_asm test eax,eax

	_asm jmp addrCallOpenGLCreateContext_ret
}


void (__stdcall *origRenderDrawingQueue)(DWORD dddisplay, DWORD a3);
void __stdcall Renderer::hookRenderDrawingQueue(DWORD dddisplay, DWORD a3) {
	DWORD GameScene;
	_asm mov GameScene, eax

	insideRenderDrawingQueue = true;
	tryLateInitGL();

	if(origRenderDrawingQueue) {
		_asm mov eax, GameScene
		_asm push a3
		_asm push dddisplay
		_asm call origRenderDrawingQueue
	}

	insideRenderDrawingQueue = false;
	if(LobbyGui::shouldTickFromRenderHook())
		LobbyGui::tick();
}


DWORD origWriteScreenPalette;
DWORD __stdcall Renderer::hookWriteScreenPalette(DWORD a2) {
	DWORD sedi, retv;
	_asm mov sedi, edi


	_asm mov edi, sedi
	_asm push a2
	_asm call origWriteScreenPalette
	_asm mov retv, eax

	if(Config::rendererEnabled && ensureGLContext()) {
		Drawing::genPaletteTexture(sedi);
	}

	return retv;
}


int Renderer::install(SignatureScanner &, module) {
	if(Config::lobbyGuiEnabled || Config::rendererEnabled) {
		DWORD addrRenderDrawingQueue = Hooks::scanPattern("RenderDrawingQueue", "\x83\xEC\x1C\x53\x55\x8B\x6C\x24\x28\x56\x57\x8B\xF0\x8B\x86\x00\x00\x00\x00\x68\x00\x00\x00\x00\x6A\x04\x50\x8D\x8E\x00\x00\x00\x00\x51\xE8\x00\x00\x00\x00", "??????xxxxxxxxx????x????xxxxx????xx????", 0x542350);
		Hooks::polyhook("RenderDrawingQueue", addrRenderDrawingQueue, (DWORD *) &hookRenderDrawingQueue, (DWORD *) &origRenderDrawingQueue);
	}

	if(!Config::rendererEnabled) {
		printf("Renderer::install: custom renderer disabled, skipping renderer-only hooks\n");
		return 0;
	}

	// Safe mode: keep native OpenGL CPU frontend intact until SkipNativeBlit=1.
	// Installing OpenGL/draw hooks alone was enough to blank the menu (avgLuma=0).
	if(!Config::rendererSkipNativeBlit) {
		// #region agent log
		agentDebugLog("H19", "Renderer.cpp:install", "skip_all_hooks_safe_mode",
			"{\"EnableCustomRenderer\":1,\"SkipNativeBlit\":0}");
		// #endregion
		printf("Renderer::install: SkipNativeBlit=0, not installing OpenGL override hooks\n");
		return 0;
	}

	// #region agent log
	agentDebugLog("H6", "Renderer.cpp:install", "installing_hooks", "{\"enabled\":1}");
	// #endregion

	CTaskTurnGame::registerBuiltinCallbackVt8(&CTaskTurnGame_vt8);

	DWORD addrBlitScreen = Hooks::scanPattern("BlitScreen", "\x85\xC0\x8B\x54\x24\x04\x53\x8B\x5C\x24\x18\x56\x57\x8B\x7C\x24\x18\x8B\xF1\x76\x3A\x89\x44\x24\x20\x55\x8D\x9B\x00\x00\x00\x00\x85\xD2\x8B\xCE\x8B\xC7", "??????xxxxxxxxxxxxxxxxxxxxxx????xxxxxx", 0x5A2020);
	DWORD addrConstructFrameBuffer = Hooks::scanPattern("ConstructFrameBuffer", "\x51\x83\x3D\x00\x00\x00\x00\x00\x53\x55\x56\x57\x8B\xEA\x8B\xF1\x74\x0D\x8B\x06\x8B\x40\x1C\x6A\x00\x8D\x54\x24\x14\xFF\xD0\x8B\x7C\x24\x18", "????????xxxxxxxxxxxxxxxxxxxxxxxxxxx", 0x5A2430);
	DWORD addrConstructDDDisplay = Hooks::scanPattern("ConstructDDDisplay", "\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x8B\x44\x24\x18\x83\xEC\x08\x53\x55\x8B\x6C\x24\x20\x89\x85\x00\x00\x00\x00\x89\x85\x00\x00\x00\x00\x99\x2B\xC2", "???????xx????xxxx????xxxxxxxxxxxxxxx????xx????xxx", 0x569D00);
	DWORD addrDrawBitmapOnBitmapAnyDepth = Hooks::scanPattern("DrawBitmapOnBitmapAnyDepth", "\x8B\x44\x24\x08\x53\x55\x8B\x6C\x24\x28\x8B\xD9\x56\x8B\x74\x24\x28\x81\xE3\x00\x00\x00\x00\x83\xFB\x05\x57\x8B\x7C\x24\x28\x77\x27\xFF\x24\x9D\x00\x00\x00\x00", "??????xxxxxxxxxxxxx????xxxxxxxxxxxxx????", 0x4F80C0);


	DWORD addrConstructOpenGLCpu = Hooks::scanPattern("ConstructOpenGLCPU", "\x6A\x48\xE8\x00\x00\x00\x00\x33\xC9\x83\xC4\x04\x3B\xC1\x74\x29\x8B\x54\x24\x08\x89\x50\x24\x8B\x54\x24\x04\x89\x48\x18\x89\x48\x1C\x89\x48\x20\x89\x48\x40\x89\x50\x10\x89\x48\x14\xC7\x00\x00\x00\x00\x00\x89\x48\x44\xC2\x08\x00", "???????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxx", 0x5A0850);
	DWORD* addrOpenGLCpuVtable = *(DWORD**)(addrConstructOpenGLCpu + 0x2F);

	DWORD addrOpenGLDraw = addrOpenGLCpuVtable[12];
	DWORD addrOpenGLSwapBuffers = addrOpenGLCpuVtable[13];
	origMarkScreenAsReady = (DWORD (__fastcall *)(Renderer::OpenGLRenderer*,DWORD))addrOpenGLCpuVtable[1];
	DWORD addrOpenGLInit = Hooks::scanPattern("OpenGLInit", "\x55\x8B\xEC\x6A\xFF\x68\x00\x00\x00\x00\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x56\x57\x89\x65\xE8\x8B\xFA\x89\x7D\xE4\x8B\xF1\x8B\x5D\x08", "??????????x????xx????xxxx????xx????xxxxxxxxxxxxxxxx", 0x59F000);

	DWORD addrConstructBitmapDescriptor = Hooks::scanPattern("ConstructBitmapDescriptor", "\x6A\x14\xE8\x00\x00\x00\x00\x33\xC9\x83\xC4\x04\x3B\xC1\x74\x13\x89\x48\x04\x89\x48\x08\xC7\x00\x00\x00\x00\x00\x89\x48\x0C\x88\x48\x10\xC3", "???????xxxxxxxxxxxxxxxxx????xxxxxxx", 0x5A2760);
	addrBitmapDescriptorVTable = *(DWORD**)(addrConstructBitmapDescriptor + 0x18);
	DWORD addrDestroyBitmapDescriptor = addrBitmapDescriptorVTable[0];



	DWORD addrWriteScreenPalette = Hooks::scanPattern("WriteScreenPalette", "\x53\x8B\x5C\x24\x08\x55\xBA\x00\x00\x00\x00\x8B\xC7\xB9\x00\x00\x00\x00\x56\x8B\x31\x3B\x30\x75\x12\x83\xEA\x04\x83\xC0\x04\x83\xC1\x04\x83\xFA\x04\x73\xEC", "??????x????xxx????xxxxxxxxxxxxxxxxxxxxx", 0x5A1110);
	addrScreenPalette = *(DWORD*)(addrWriteScreenPalette + 0xE);

	Hooks::polyhook("BlitScreen", addrBlitScreen, (DWORD *) &hookBlitScreen, (DWORD *) &origBlitScreen);
	Hooks::polyhook("DrawBitmapOnBitmapAnyDepth", addrDrawBitmapOnBitmapAnyDepth, (DWORD *) &hookDrawBitmapOnBitmapAnyDepth, (DWORD *) &origDrawBitmapOnBitmapAnyDepth);
	Hooks::polyhook("OpenGLDraw", addrOpenGLDraw, (DWORD *) &hookOpenGLDraw, (DWORD *) &origOpenGLDraw);
	Hooks::polyhook("OpenGLInit", addrOpenGLInit, (DWORD *) &hookOpenGLInit, (DWORD *) &origOpenGLInit);
	Hooks::polyhook("OpenGLSwapBuffers", addrOpenGLSwapBuffers, (DWORD *) &hookOpenGLSwapBuffers, (DWORD *) &origOpenGLSwapBuffers);

	Hooks::polyhook("DestroyBitmapDescriptor", addrDestroyBitmapDescriptor, (DWORD *) &hookDestroyBitmapDescriptor, (DWORD *) &origDestroyBitmapDescriptor);
	Hooks::polyhook("ConstructBitmapDescriptor", addrConstructBitmapDescriptor, (DWORD *) &hookConstructBitmapDescriptor, (DWORD *) &origConstructBitmapDescriptor);
	Hooks::polyhook("WriteScreenPalette", addrWriteScreenPalette, (DWORD *) &hookWriteScreenPalette, (DWORD *) &origWriteScreenPalette);

	// Do NOT mid-patch OpenGLInit+0x27C for wglCreateContext — that offset is fragile and
	// can break the game's own OpenGL CPU presenter. We capture the context after OpenGLInit.
	// #region agent log
	agentDebugLog("H7", "Renderer.cpp:install", "skip_asm_create_context_hook", "{}");
	// #endregion
	return 0;
}

void Renderer::toggleNativeDrawing() {
	forceDisableNativeDrawing = !forceDisableNativeDrawing;
	rot = 0;
}
