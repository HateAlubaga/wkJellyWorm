
#include "../Hooks.h"
#include "../Config.h"
#include "../Game.h"
#include "../WaLibc.h"
#include "Drawing.h"
#include "Bitmap.h"
#include "Renderer.h"
#include "GLDraw.h"
#include "DebugLog.h"



DWORD (__fastcall *origDrawLandscape)(int a1, DWORD *a2, Renderer::BitmapDescriptor * Src, int a4, int a5, int a6, int a7, int a8, int a9, int a10);
DWORD __fastcall Drawing::hookDrawLandscape(int a1, DWORD *a2, Renderer::BitmapDescriptor * Src, int posX, int posY, int offsetX, int offsetY, int width, int height, int flag) {
	if(Renderer::isActive()) {
		if(Src->vtable == (DWORD)Renderer::addrBitmapDescriptorVTable) {
			if(!Src->bitmap) {
				//create a fake bitmap entry so it works with the drawing func
				Src->bitmap = (Bitmap::BitmapImage*) WaLibc::waMalloc(sizeof(Bitmap::BitmapImage));
				memset(Src->bitmap, 0, sizeof(Bitmap::BitmapImage));
				Src->bitmap->max_width_dword14 = Src->width_dword4;
				Src->bitmap->max_height_dword18 = Src->height_dword8;
				Src->bitmap->current_width_dword24 = Src->width_dword4;
				Src->bitmap->current_height_dword28 = Src->height_dword8;
				Src->bitmap->rowsize_dword10 = Src->width_dword4;
				Src->bitmap->data_dword8 = Src->data_dwordC;
				Src->bitmap->bitdepth_dwordC = 8;
				*(DWORD*)Src->bitmap = Bitmap::getAddrBitmapImageVtable();

				auto extension = BitmapExtension::constructBitmapImageExtension(Src->bitmap);
				extension->align = 1;
				BitmapExtension::populateBitmapImageExtension(Src->bitmap, (unsigned char*)Src->data_dwordC);
			}
			callDrawBitmapOnBitmap(0, posX, posY, width, height, Src->bitmap, offsetX, offsetY, 0, flag);
		}
	}
	if(!Renderer::forceDisableNativeDrawing)
		return origDrawLandscape(a1, a2, Src, posX, posY, offsetX, offsetY, width, height, flag);
	return 0;
}

DWORD (__fastcall *origRedrawFrontendBitmap)(Renderer::BitmapDescriptor *  a1, DWORD a2, int a3, int a4, Renderer::BitmapDescriptor *  a5, int a6, int a7, int a8, int a9, int a10);
DWORD __fastcall Drawing::hookRedrawFrontendBitmap(Renderer::BitmapDescriptor * a1, DWORD a2, int a3, int a4, Renderer::BitmapDescriptor *  a5, int a6, int a7, int a8, int a9, int a10) {
	if(!Renderer::forceDisableNativeDrawing)
		return origRedrawFrontendBitmap(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
	return 0;
}


void Drawing::callDrawBitmapOnBitmap(Bitmap::BitmapImage *Dst, int posX, int posY, int width, int height, Bitmap::BitmapImage *Src, int srcOffsetX, int srcOffsetY, int a9, int flags) {
	_asm push flags
	_asm push a9
	_asm push srcOffsetY
	_asm push srcOffsetX
	_asm push Src
	_asm push height
	_asm push width
	_asm push posY
	_asm push posX
	_asm mov esi, Dst
	_asm call hookDrawBitmapOnBitmap
}


DWORD origDrawBitmapOnBitmap;
int __stdcall Drawing::hookDrawBitmapOnBitmap(int posX, int posY, int width, int height, Bitmap::BitmapImage *Src, int srcOffsetX, int srcOffsetY, int a9, int flags) {
	Bitmap::BitmapImage * Dst;
	int retv;
	_asm mov Dst, esi

	// Until custom GL is live, do not touch draw semantics — pure native trampoline.
	// (Previously default drawToScreen=true + early returns could drop non-landscape blits.)
	if(!Renderer::isActive()) {
		Renderer::tryLateInitGL();
		if(!Renderer::isActive()) {
			// #region agent log
			{
				static unsigned int n = 0;
				n++;
				if(n <= 8) {
					char data[128];
					sprintf_s(data, "{\"n\":%u,\"depth\":%d,\"dstNull\":%d}",
						n, Src ? Src->bitdepth_dwordC : -1, Dst ? 0 : 1);
					agentDebugLog("H12", "Drawing.cpp:hookDrawBitmapOnBitmap", "pure_native", data);
				}
			}
			// #endregion
			_asm mov esi, Dst
			_asm push flags
			_asm push a9
			_asm push srcOffsetY
			_asm push srcOffsetX
			_asm push Src
			_asm push height
			_asm push width
			_asm push posY
			_asm push posX
			_asm call origDrawBitmapOnBitmap
			_asm mov retv, eax
			return retv;
		}
	}

	bool drawToScreen = true;
	bool didGlDraw = false;
	ExtendedBitmap * srcExtension = nullptr, * dstExtension = nullptr;
	if(Renderer::isActive() && Renderer::ensureGLContext()) {
		auto dddisplay = Game::getAddrDdDisplay();
		drawToScreen = (!Dst || (dddisplay && ((DWORD) Dst == *(DWORD *) (dddisplay + 0x3D9C))));

		if (Src && Src->bitdepth_dwordC == 8 && (drawToScreen || (Dst && Dst->bitdepth_dwordC == 8))) {

			srcExtension = BitmapExtension::findBitmapExtension(Src);
			if (!srcExtension) {
				printf("hookDrawBitmapOnBitmap: creating Src BitmapExtension: %X %dx%d (rowsize: %d, depth: %d)\n", Src, Src->max_width_dword14, Src->max_height_dword18, Src->rowsize_dword10, Src->bitdepth_dwordC);
				srcExtension = BitmapExtension::constructBitmapImageExtension(Src);
			}
			if (!srcExtension->populated) {
				printf("hookDrawBitmapOnBitmap: populating Src BitmapExtension: %X %dx%d (rowsize: %d, depth: %d)\n", Src, Src->max_width_dword14, Src->max_height_dword18, Src->rowsize_dword10, Src->bitdepth_dwordC);
				auto data = (unsigned char *) Src->data_dword8;
				BitmapExtension::populateBitmapImageExtension(Src, data);
			}
			if(srcExtension->redraw) {
				srcExtension->redraw = false;
				BitmapExtension::redrawBitmapImageExtension(Src);
			}

			dstExtension = BitmapExtension::findBitmapExtension(Dst);
			if (!drawToScreen && Dst->bitdepth_dwordC == 8) {
				if (!dstExtension) {
					printf("hookDrawBitmapOnBitmap: creating Dst BitmapExtension: %X %dx%d (rowsize: %d, depth: %d)\n", Dst, Dst->max_width_dword14, Dst->max_height_dword18, Dst->rowsize_dword10, Dst->bitdepth_dwordC);
					dstExtension = BitmapExtension::constructBitmapImageExtension(Dst);
				}
				if (!dstExtension->populated) {
					printf("hookDrawBitmapOnBitmap: populating Dst BitmapExtension: %X %dx%d (rowsize: %d, depth: %d)\n", Dst, Dst->max_width_dword14, Dst->max_height_dword18, Dst->rowsize_dword10, Dst->bitdepth_dwordC);
					auto data = (unsigned char *) Dst->data_dword8;
					BitmapExtension::populateBitmapImageExtension(Dst, data);
				}
			}

			GLuint dstBuffer;
			int dstWidth, dstHeight;

			int srcWidth = Src->max_width_dword14;
			int srcHeight = Src->max_height_dword18;
			int glOffsetX = srcOffsetX;
			int glOffsetY = srcOffsetY;
			int glWidth = width;
			int glHeight = height;
			if (srcExtension->sprite) {
				srcWidth = srcExtension->spriteMaxW;
				glOffsetY += srcExtension->spriteFrames[Src->data_dword8] * srcExtension->spriteMaxH;
				srcHeight = srcExtension->spriteMaxH * srcExtension->sprite->max_frames_word16;
			}
			GLuint srcTexture = srcExtension->texture;
			if (drawToScreen) {
				dstWidth = Renderer::screenWidth;
				dstHeight = Renderer::screenHeight;
				dstBuffer = 0;
			} else {
				dstBuffer = dstExtension->framebuffer;
				dstWidth = Dst->current_width_dword24;
				dstHeight = Dst->current_height_dword28;
			}

			GLfloat startx = (float) glOffsetX / (float) srcWidth;
			GLfloat starty = (float) glOffsetY / (float) srcHeight;
			GLfloat endx = (float) (glOffsetX + width) / (float) srcWidth;
			GLfloat endy = (float) (glOffsetY + height) / (float) srcHeight;

			GLDraw::drawIndexedQuad(
				dstBuffer, dstWidth, dstHeight,
				srcTexture,
				posX, posY, glWidth, glHeight,
				startx, starty, endx, endy,
				drawToScreen,
				(flags & 0x1) != 0,
				(flags & 0x10000) != 0,
				(GLuint)paletteShaderProgram, (GLuint)indexedShaderProgram,
				palette_IndexedTextureSamplerLocation, palette_PaletteTextureSamplerLocation, palette_TransparentFlagLocation,
				indexed_TextureSamplerLocation,
				palette_texture,
				Config::rendererDebugPoints != 0
			);
			didGlDraw = true;

			// #region agent log
			{
				static unsigned int n = 0;
				n++;
				if(n <= 8) {
					char data[256];
					sprintf_s(data,
						"{\"n\":%u,\"screen\":%d,\"palTex\":%u,\"srcTex\":%u,\"w\":%d,\"h\":%d,\"depth\":%d}",
						n, (int)drawToScreen, (unsigned)palette_texture, (unsigned)srcTexture,
						width, height, Src->bitdepth_dwordC);
					agentDebugLog(drawToScreen ? "H4" : "H1", "Drawing.cpp:hookDrawBitmapOnBitmap", "gl_draw", data);
				}
			}
			// #endregion
		}
	}
	if(!Dst) {
		// #region agent log
		if(!didGlDraw) {
			static unsigned int nullDstN = 0;
			nullDstN++;
			if(nullDstN <= 8) {
				char data[128];
				sprintf_s(data, "{\"n\":%u,\"didGl\":0,\"active\":%d}", nullDstN, (int)Renderer::isActive());
				agentDebugLog("H8", "Drawing.cpp:hookDrawBitmapOnBitmap", "null_dst_try_native", data);
			}
		}
		// #endregion
		// Only skip native when GL already drew to the default framebuffer.
		if(didGlDraw)
			return 0;
	}
	// Only skip native screen blit when GL actually drew. Otherwise frontend/DD path
	// would get a blank frame (rendererEnabled but no OpenGLInit/context yet).
	if(Config::rendererEnabled && drawToScreen && didGlDraw) {
		return 0;
	}
	// #region agent log
	if(Config::rendererEnabled && drawToScreen && !didGlDraw) {
		static unsigned int skipN = 0;
		skipN++;
		if(skipN <= 10) {
			char data[192];
			int depth = Src ? Src->bitdepth_dwordC : -1;
			sprintf_s(data, "{\"n\":%u,\"didGl\":0,\"depth\":%d,\"active\":%d,\"fallbackNative\":1}",
				skipN, depth, (int)Renderer::isActive());
			agentDebugLog("H6", "Drawing.cpp:hookDrawBitmapOnBitmap", "inactive_or_no_gl_native", data);
		}
	}
	// #endregion



	_asm mov esi, Dst
	_asm push flags
	_asm push a9
	_asm push srcOffsetY
	_asm push srcOffsetX
	_asm push Src
	_asm push height
	_asm push width
	_asm push posY
	_asm push posX
	_asm call origDrawBitmapOnBitmap
	_asm mov retv, eax

	if(Config::rendererEnabled && !drawToScreen && dstExtension) {
		dstExtension->redraw = true;
	}

	return retv;
}


bool Drawing::compileShaders() {
	try {
		if(!Config::rendererEnabled || !Renderer::glContextCreated || !Renderer::glewInitialized || !wglGetCurrentContext()) {
			printf("compileShaders: no GL context\n");
			return false;
		}

		const GLubyte * glVersion = glGetString(GL_VERSION);
		const GLubyte * glslVersion = glGetString(GL_SHADING_LANGUAGE_VERSION);
		printf("compileShaders: GL_VERSION=%s GLSL=%s\n",
			   glVersion ? (const char*)glVersion : "?",
			   glslVersion ? (const char*)glslVersion : "?");

		unsigned int vertex, fragment;
		int success;
		char infoLog[512];

		vertex = glCreateShader(GL_VERTEX_SHADER);
		const char *vs_c_str = vertexShaderSource.c_str();
		glShaderSource(vertex, 1, (const GLchar *const *) &vs_c_str, NULL);
		glCompileShader(vertex);
		glGetShaderiv(vertex, GL_COMPILE_STATUS, &success);
		if (!success) {
			glGetShaderInfoLog(vertex, sizeof(infoLog), NULL, infoLog);
			throw std::runtime_error("Failed to compile vertex shader, reason:\n" + std::string(infoLog));
		};

		fragment = glCreateShader(GL_FRAGMENT_SHADER);
		const char *fs_c_str = indexedFragmentShaderSource.c_str();
		glShaderSource(fragment, 1, (const GLchar *const *) &fs_c_str, NULL);
		glCompileShader(fragment);
		glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
		if (!success) {
			glGetShaderInfoLog(fragment, sizeof(infoLog), NULL, infoLog);
			throw std::runtime_error("Failed to compile fragment shader (indexed), reason:\n" + std::string(infoLog));
		};

		GLint ID = glCreateProgram();
		glAttachShader(ID, vertex);
		glAttachShader(ID, fragment);
		glLinkProgram(ID);
		glGetProgramiv(ID, GL_LINK_STATUS, &success);
		if (!success) {
			glGetProgramInfoLog(ID, sizeof(infoLog), NULL, infoLog);
			throw std::runtime_error("Failed to link shader program, reason:\n" + std::string(infoLog));
		}

		glDeleteShader(fragment);
		indexedShaderProgram = ID;

		indexed_TextureSamplerLocation = glGetUniformLocation(indexedShaderProgram, "indexed_text");


		fragment = glCreateShader(GL_FRAGMENT_SHADER);
		fs_c_str = paletteFragmentShaderSource.c_str();
		glShaderSource(fragment, 1, (const GLchar *const *) &fs_c_str, NULL);
		glCompileShader(fragment);
		glGetShaderiv(fragment, GL_COMPILE_STATUS, &success);
		if (!success) {
			glGetShaderInfoLog(fragment, sizeof(infoLog), NULL, infoLog);
			throw std::runtime_error("Failed to compile fragment shader (palette), reason:\n" + std::string(infoLog));
		};

		ID = glCreateProgram();
		glAttachShader(ID, vertex);
		glAttachShader(ID, fragment);
		glLinkProgram(ID);
		glGetProgramiv(ID, GL_LINK_STATUS, &success);
		if (!success) {
			glGetProgramInfoLog(ID, sizeof(infoLog), NULL, infoLog);
			throw std::runtime_error("Failed to link shader program (palette), reason:\n" + std::string(infoLog));
		}
		glDeleteShader(vertex);
		glDeleteShader(fragment);

		paletteShaderProgram = ID;
		palette_IndexedTextureSamplerLocation = glGetUniformLocation(paletteShaderProgram, "indexed_text");
		palette_PaletteTextureSamplerLocation = glGetUniformLocation(paletteShaderProgram, "palette_text");
		palette_TransparentFlagLocation = glGetUniformLocation(paletteShaderProgram, "transparent_flag");

		printf("compileShaders: indexed program=%d palette program=%d\n", indexedShaderProgram, paletteShaderProgram);
		return true;
	}
	catch(std::exception & e) {
		printf("compileShaders failed: %s\n", e.what());
		MessageBoxA(0, e.what(), "Failed to compile shader", 0);
		return false;
	}
}

void Drawing::genPaletteTexture(DWORD palette) {
	if(!Renderer::ensureGLContext())
		return;

	auto screenpal = (ScreenPalette *) palette;
	static auto data = (DWORD*)malloc(sizeof(DWORD) * 256);

	if(!palette) {
		for(int i=0; i < 256; i++) {
			data[i] = 0xFF000000 | (rand() & 0xFF) << 16 | (rand() & 0xFF) << 8 | (rand() & 0xFF);
		}
	} else {
		for(int i=0; i < 256; i++) {
			data[i] = screenpal->colors[i].toRGBA();
		}
	}

	if(!palette_texture) {
		glGenTextures(1, &palette_texture);
	}
	GLint oldbind;
	glGetIntegerv(GL_TEXTURE_BINDING_1D, &oldbind);
	glBindTexture(GL_TEXTURE_1D, palette_texture);

	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_1D, oldbind);
}


DWORD (__fastcall *origSwitchScreenPalette)(DWORD This, int EDX, DWORD palette, int commit);
DWORD __fastcall Drawing::hookSwitchScreenPalette(DWORD This, int EDX, DWORD palette, int commit) {
	auto ret = origSwitchScreenPalette(This, EDX, palette, commit);
	if(Config::rendererEnabled && Renderer::ensureGLContext()) {
		genPaletteTexture(palette);
	}
	return ret;
}

int Drawing::install(SignatureScanner &, module) {
	if(!Config::rendererEnabled || !Config::rendererSkipNativeBlit) {
		printf("Drawing::install: custom draw overrides disabled (Enable=%d SkipNativeBlit=%d)\n",
			Config::rendererEnabled, Config::rendererSkipNativeBlit);
		// #region agent log
		agentDebugLog("H19", "Drawing.cpp:install", "skip_draw_hooks_safe_mode",
			Config::rendererEnabled ? "{\"SkipNativeBlit\":0}" : "{\"EnableCustomRenderer\":0}");
		// #endregion
		return 0;
	}

	DWORD addrDrawLandscape = Hooks::scanPattern("DrawLandscape", "\x55\x8B\xEC\x83\xE4\xF8\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x30\x53\x56\x57\x8B\xD9\x8B\x4D\x08\x8B\xFA", "??????xxx????xx????xxxx????xxxxxxxxxxxxx", 0x5A2790);
	DWORD addrDrawBitmapOnBitmap = Hooks::scanPattern("DrawBitmapOnBitmap", "\x55\x8B\xEC\x83\xE4\xF8\x8B\x4D\x10\x8B\x45\x18\x83\xEC\x38\x85\xC9\x53\x57\x0F\x84\x00\x00\x00\x00\x83\x7D\x14\x00\x0F\x84\x00\x00\x00\x00\x8B\x50\x08\x8B\x78\x10\x8B\x46\x20\x8B\x5E\x1C", "??????xxxxxxxxxxxxxxx????xxxxxx????xxxxxxxxxxxx", 0x4F6910);
	DWORD addrRedrawFrontendBitmap = Renderer::addrBitmapDescriptorVTable[11]; //5A0BD0

	DWORD addrSwitchScreenPalette = Hooks::scanPattern("SwitchScreenPalette", "\x51\x8B\x44\x24\x08\x66\x83\xB8\x00\x00\x00\x00\x00\x56\x8B\xF1\x66\xC7\x80\x00\x00\x00\x00\x00\x00\x0F\x8E\x00\x00\x00\x00\x53\x0F\xBF\x58\x02\x55\x0F\xBF\x28\x57\x0F\xB6\xB8\x00\x00\x00\x00\x89\x5C\x24\x10", "??????xx?????xxxxxx??????xx????xxxxxxxxxxxxx????xxxx", 0x56A610);

	Hooks::polyhook("DrawBitmapOnBitmap", addrDrawBitmapOnBitmap, (DWORD *) &hookDrawBitmapOnBitmap, (DWORD *) &origDrawBitmapOnBitmap);
	Hooks::polyhook("DrawLandscape", addrDrawLandscape, (DWORD *) &hookDrawLandscape, (DWORD *) &origDrawLandscape);
	Hooks::polyhook("RedrawFrontendBitmap", addrRedrawFrontendBitmap, (DWORD *) &hookRedrawFrontendBitmap, (DWORD *) &origRedrawFrontendBitmap);
	Hooks::polyhook("SwitchScreenPalette", addrSwitchScreenPalette, (DWORD *) &hookSwitchScreenPalette, (DWORD *) &origSwitchScreenPalette);

	return 0;
}
