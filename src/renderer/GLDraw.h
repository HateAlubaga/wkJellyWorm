
#ifndef WKJELLYWORM_GLDRAW_H
#define WKJELLYWORM_GLDRAW_H

#include <GL/glew.h>

class GLDraw {
public:
	// Draw an indexed (palette-index) texture quad into screen (FBO 0) or an offscreen FBO.
	// Uses Compatibility-profile fixed-function matrix stack + glBegin(GL_QUADS).
	static void drawIndexedQuad(GLuint dstFbo, int dstW, int dstH,
	                            GLuint srcTex,
	                            int posX, int posY, int width, int height,
	                            float u0, float v0, float u1, float v1,
	                            bool toScreen, bool transparent, bool flipH,
	                            GLuint paletteProgram, GLuint indexedProgram,
	                            GLint paletteIndexedLoc, GLint palettePaletteLoc, GLint paletteTransparentLoc,
	                            GLint indexedSamplerLoc,
	                            GLuint paletteTexture,
	                            bool debugPoints);
};

#endif //WKJELLYWORM_GLDRAW_H
