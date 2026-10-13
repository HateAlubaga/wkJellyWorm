#ifndef WKJELLYWORM_QUADRENDER_H
#define WKJELLYWORM_QUADRENDER_H

// ProjectX quad rendering (RenderQuad / AddSpriteQ / AddCustomRender, rotated and scaled sprites) for the 8bpp
// software renderer. Lua hands over batches of quads; each batch becomes one type-3 item ("call obj->vtbl[2]") in
// WA's draw queue, so it is sorted by layer with WA's own sprites. At dispatch time the batch is rasterised straight
// into the screen bitmap: textured from a sprite frame or flat, gouraud ARGB vertex colours, normal / additive /
// multiply blending through the screen palette.
class QuadRender {
public:
	static void install();
};

#endif //WKJELLYWORM_QUADRENDER_H
