
#include "GLDraw.h"

void GLDraw::drawIndexedQuad(GLuint dstFbo, int dstW, int dstH,
                             GLuint srcTex,
                             int posX, int posY, int width, int height,
                             float u0, float v0, float u1, float v1,
                             bool toScreen, bool transparent, bool flipH,
                             GLuint paletteProgram, GLuint indexedProgram,
                             GLint paletteIndexedLoc, GLint palettePaletteLoc, GLint paletteTransparentLoc,
                             GLint indexedSamplerLoc,
                             GLuint paletteTexture,
                             bool debugPoints) {
	float startx = u0;
	float starty = v0;
	float endx = u1;
	float endy = v1;
	if(flipH) {
		float tmp = startx;
		startx = endx;
		endx = tmp;
	}

	glDisable(GL_DEPTH_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();

	glPushAttrib(GL_VIEWPORT_BIT);
	glViewport(0, 0, dstW, dstH);
	if(toScreen)
		glOrtho(0, dstW, dstH, 0, -1, 1);
	else
		glOrtho(0, dstW, 0, dstH, -1, 1);

	if(transparent) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glBlendEquation(GL_FUNC_ADD);
	}

	GLint prevTex = 0;
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, srcTex);

	if(toScreen) {
		glUseProgram(paletteProgram);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_1D, paletteTexture);
		glUniform1i(paletteIndexedLoc, 0);
		glUniform1i(palettePaletteLoc, 1);
		glUniform1i(paletteTransparentLoc, transparent ? 1 : 0);
		glActiveTexture(GL_TEXTURE0);
	} else {
		glUseProgram(indexedProgram);
		glUniform1i(indexedSamplerLoc, 0);
	}

	glBegin(GL_QUADS);
	glTexCoord2f(startx, starty);
	glVertex2i(posX, posY);
	glTexCoord2f(endx, starty);
	glVertex2i(posX + width, posY);
	glTexCoord2f(endx, endy);
	glVertex2i(posX + width, posY + height);
	glTexCoord2f(startx, endy);
	glVertex2i(posX, posY + height);
	glEnd();

	glBindTexture(GL_TEXTURE_2D, prevTex);
	glDisable(GL_TEXTURE_2D);

	if(debugPoints) {
		glPointSize(8.0f);
		glBegin(GL_POINTS);
		glColor3f(1.0f, 0.0f, 0.0f);
		glVertex2i(0, 0);
		glColor3f(0.0f, 1.0f, 0.0f);
		glVertex2i(posX, posY);
		glColor3f(0.0f, 1.0f, 1.0f);
		glVertex2i(posX + width, posY + height);
		glEnd();
		glColor3f(1.0f, 1.0f, 1.0f);
	}

	if(transparent)
		glDisable(GL_BLEND);

	glPopMatrix();
	glPopAttrib();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(0);
}
