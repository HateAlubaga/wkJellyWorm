#include "QuadRender.h"
#include "Lua.h"
#include "Game.h"
#include "Sprites.h"
#include "Config.h"
#include "renderer/Renderer.h"
#include <sol/sol.hpp>
#include <windows.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
	// One vertex as PX's CQuad keeps it: world pixels, texture coordinates over the sprite cell (0..1, wrapped
	// outside), ARGB colour.
	struct QV {
		float x, y, tx, ty;
		uint32_t c;
	};
	struct Quad {
		int sprite;   // 0 = flat colour
		int phase;    // WA frame phase of the sprite
		int blend;    // 0 normal (alpha), 1 additive, 64 multiply
		QV v[4];
	};
	struct Batch {
		uint32_t gen = 0;
		std::vector<Quad> quads;
	};

	// Batches live in a ring: the queue item carries (slot, generation) and a slot is only drawn while its
	// generation still matches, so a reused slot or a frame that was never dispatched cannot draw stale data.
	const int kPool = 2048;
	Batch g_pool[kPool];
	uint32_t g_nextSlot = 0;
	uint32_t g_gen = 0;

	const int kQuadFloats = 24;   // Lua layout per quad: sprite, frame, blend, flags, 4 x (x, y, tx, ty, colour)

	// statistics for pxQuadStats()
	uint32_t g_batches = 0, g_quads = 0, g_rejected = 0, g_faults = 0;
	double g_drawMs = 0;
	bool g_rendererWarned = false;

	void quadLog(const char *fmt, ...) {
		va_list args;
		va_start(args, fmt);
		FILE *f = nullptr;
		if(fopen_s(&f, "jelly\\quadrender.log", "a") == 0 && f) {
			vfprintf(f, fmt, args);
			fclose(f);
		}
		va_end(args);
	}

	// ---- screen palette and nearest-colour lookup (RGB555 -> index, filled on demand)
	uint8_t palR[256], palG[256], palB[256];
	uint32_t palHash = 0;
	bool palValid = false;
	uint16_t lut[32768];

	void refreshPalette(DWORD dd) {
		// PALETTEENTRY[256] (R, G, B, flags) the screen palette is set from (see 0x56A610)
		const uint8_t *p = (const uint8_t *)(dd + 0x358D);
		uint32_t h = 2166136261u;
		for(int i = 0; i < 1024; i++)
			h = (h ^ p[i]) * 16777619u;
		if(palValid && h == palHash)
			return;
		for(int i = 0; i < 256; i++) {
			palR[i] = p[i * 4];
			palG[i] = p[i * 4 + 1];
			palB[i] = p[i * 4 + 2];
		}
		memset(lut, 0xFF, sizeof(lut));
		palHash = h;
		palValid = true;
	}

	inline uint8_t nearest(int r, int g, int b) {
		int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
		uint16_t e = lut[key];
		if(e != 0xFFFF)
			return (uint8_t)e;
		int cr = (r & 0xF8) | 4, cg = (g & 0xF8) | 4, cb = (b & 0xF8) | 4;
		int best = 1, bestD = 0x7FFFFFFF;
		for(int i = 1; i < 256; i++) {   // index 0 is the colour key
			int dr = cr - palR[i], dg = cg - palG[i], db = cb - palB[i];
			int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
			if(d < bestD) {
				bestD = d;
				best = i;
			}
		}
		lut[key] = (uint16_t)best;
		return (uint8_t)best;
	}

	// ---- sprite frames
	// DDDisplay vtbl +0x84 (0x5237C0): thiscall (id, phase, &cellW, &cellH, &x1, &y1, &x2, &y2, &frac) -> BitmapImage*
	// of the frame's non-empty rect (x1..x2, y1..y2 inside the cell), pixels packed with stride x2 - x1.
	typedef uint8_t *(__fastcall *GetSpriteFrameFn)(DWORD dd, int edx, int id, int phase, int *W, int *H,
		int *x1, int *y1, int *x2, int *y2, int *frac);

	struct Tex {
		const uint8_t *data;
		int W, H, x1, y1, w, h;
	};

	bool getTex(DWORD dd, int sprite, int phase, Tex &t) {
		if(sprite <= 0 || sprite > 0x3FF)
			return false;
		auto fn = (GetSpriteFrameFn)(*(DWORD **)dd)[0x84 / 4];
		int W = 0, H = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0, frac = 0;
		uint8_t *bmp = fn(dd, 0, sprite, phase, &W, &H, &x1, &y1, &x2, &y2, &frac);
		if(!bmp || !*(DWORD *)(bmp + 4))
			return false;
		t.data = *(const uint8_t **)(bmp + 8);
		t.W = W;
		t.H = H;
		t.x1 = x1;
		t.y1 = y1;
		t.w = x2 - x1;
		t.h = y2 - y1;
		return t.data && W > 0 && H > 0 && t.w > 0 && t.h > 0;
	}

	struct Target {
		uint8_t *data;
		int stride;
		int cx1, cy1, cx2, cy2;   // clip rect, x2/y2 exclusive
	};

	struct SV {
		float x, y, tx, ty, a, r, g, b;
	};

	inline SV toScreen(const QV &v, float ox, float oy, bool alphaOne) {
		SV s;
		s.x = ox + v.x;
		s.y = oy + v.y;
		s.tx = v.tx;
		s.ty = v.ty;
		s.a = alphaOne ? 255.0f : (float)((v.c >> 24) & 255);
		s.r = (float)((v.c >> 16) & 255);
		s.g = (float)((v.c >> 8) & 255);
		s.b = (float)(v.c & 255);
		return s;
	}

	inline bool tieIn(float dx, float dy) {
		return dy > 0 || (dy == 0 && dx > 0);
	}

	// Texture coordinates repeat outside 0..1 (tiled quads), but rounding just past an edge of a 0..1 quad must
	// clamp: wrapping 1.0000001 to 0 put the opposite edge's texels on the border of rotated sprites.
	inline float wrap01(float u) {
		if(u < 0.0f || u > 1.0f) {
			if(u > -1e-3f && u < 1.001f)
				return u < 0.0f ? 0.0f : 1.0f;
			u -= floorf(u);
		}
		return u;
	}

	inline int clamp255(float v) {
		int i = (int)v;
		return i < 0 ? 0 : (i > 255 ? 255 : i);
	}

	void rasterTri(const Target &T, SV a, SV b, SV c, int blend, const Tex *tex) {
		float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
		if(area > -1e-6f && area < 1e-6f)
			return;
		if(area < 0) {
			SV t = b;
			b = c;
			c = t;
			area = -area;
		}
		int minx = (int)floorf(fminf(a.x, fminf(b.x, c.x)));
		int maxx = (int)ceilf(fmaxf(a.x, fmaxf(b.x, c.x)));
		int miny = (int)floorf(fminf(a.y, fminf(b.y, c.y)));
		int maxy = (int)ceilf(fmaxf(a.y, fmaxf(b.y, c.y)));
		if(minx < T.cx1) minx = T.cx1;
		if(miny < T.cy1) miny = T.cy1;
		if(maxx > T.cx2 - 1) maxx = T.cx2 - 1;
		if(maxy > T.cy2 - 1) maxy = T.cy2 - 1;
		if(minx > maxx || miny > maxy)
			return;
		// edge functions; e0 weights a (edge b->c), e1 weights b (c->a), e2 weights c (a->b)
		float d0x = c.x - b.x, d0y = c.y - b.y;
		float d1x = a.x - c.x, d1y = a.y - c.y;
		float d2x = b.x - a.x, d2y = b.y - a.y;
		bool t0 = tieIn(d0x, d0y), t1 = tieIn(d1x, d1y), t2 = tieIn(d2x, d2y);
		float inv = 1.0f / area;
		float px0 = minx + 0.5f, py0 = miny + 0.5f;
		float e0row = d0x * (py0 - b.y) - d0y * (px0 - b.x);
		float e1row = d1x * (py0 - c.y) - d1y * (px0 - c.x);
		float e2row = d2x * (py0 - a.y) - d2y * (px0 - a.x);
		bool flat = !tex;
		for(int y = miny; y <= maxy; y++) {
			float e0 = e0row, e1 = e1row, e2 = e2row;
			uint8_t *row = T.data + y * T.stride;
			for(int x = minx; x <= maxx; x++, e0 -= d0y, e1 -= d1y, e2 -= d2y) {
				if(e0 < 0 || e1 < 0 || e2 < 0)
					continue;
				if((e0 == 0 && !t0) || (e1 == 0 && !t1) || (e2 == 0 && !t2))
					continue;
				float wa = e0 * inv, wb = e1 * inv, wc = 1.0f - wa - wb;
				float A = wa * a.a + wb * b.a + wc * c.a;
				if(blend != 64 && A < 0.5f)
					continue;
				float R = wa * a.r + wb * b.r + wc * c.r;
				float G = wa * a.g + wb * b.g + wc * c.g;
				float B = wa * a.b + wb * b.b + wc * c.b;
				uint8_t src = 0;
				if(!flat) {
					float u = wrap01(wa * a.tx + wb * b.tx + wc * c.tx);
					float v = wrap01(wa * a.ty + wb * b.ty + wc * c.ty);
					int ix = (int)(u * tex->W), iy = (int)(v * tex->H);
					if(ix >= tex->W) ix = tex->W - 1;
					if(iy >= tex->H) iy = tex->H - 1;
					ix -= tex->x1;
					iy -= tex->y1;
					if(ix < 0 || iy < 0 || ix >= tex->w || iy >= tex->h)
						continue;
					src = tex->data[iy * tex->w + ix];
					if(!src)
						continue;
					if(blend == 0 && A >= 254.5f && R >= 254.5f && G >= 254.5f && B >= 254.5f) {
						row[x] = src;   // untinted opaque sprite pixel: exact colour
						continue;
					}
					R = R * palR[src] * (1.0f / 255.0f);
					G = G * palG[src] * (1.0f / 255.0f);
					B = B * palB[src] * (1.0f / 255.0f);
				}
				uint8_t d = row[x];
				float dr = palR[d], dg = palG[d], db = palB[d];
				float nr, ng, nb;
				if(blend == 64) {
					nr = dr * R * (1.0f / 255.0f);
					ng = dg * G * (1.0f / 255.0f);
					nb = db * B * (1.0f / 255.0f);
				} else if(blend == 1) {
					float k = A * (1.0f / 255.0f);
					nr = dr + R * k;
					ng = dg + G * k;
					nb = db + B * k;
				} else {
					float k = A * (1.0f / 255.0f);
					nr = dr + (R - dr) * k;
					ng = dg + (G - dg) * k;
					nb = db + (B - db) * k;
				}
				row[x] = nearest(clamp255(nr), clamp255(ng), clamp255(nb));
			}
			e0row += d0x;
			e1row += d1x;
			e2row += d2x;
		}
	}

	void drawBatch(DWORD dd, uint8_t *bmp, int ox, int oy, Batch &batch) {
		if(*(int *)(bmp + 0xC) != 8)
			return;
		Target T;
		T.data = *(uint8_t **)(bmp + 8);
		T.stride = *(int *)(bmp + 0x10);
		T.cx1 = *(int *)(bmp + 0x1C);
		T.cy1 = *(int *)(bmp + 0x20);
		T.cx2 = *(int *)(bmp + 0x24);
		T.cy2 = *(int *)(bmp + 0x28);
		if(!T.data || T.cx2 <= T.cx1 || T.cy2 <= T.cy1)
			return;
		refreshPalette(dd);
		Tex tex;
		int texSprite = -1, texPhase = -1;
		bool texOk = false;
		for(const Quad &q : batch.quads) {
			const Tex *tp = nullptr;
			if(q.sprite > 0) {
				if(q.sprite != texSprite || q.phase != texPhase) {
					texSprite = q.sprite;
					texPhase = q.phase;
					texOk = getTex(dd, q.sprite, q.phase, tex);
				}
				if(!texOk)
					continue;
				tp = &tex;
			}
			// a quad whose four colours all have alpha 0 was built with RGB(): draw it opaque
			bool alphaOne = ((q.v[0].c | q.v[1].c | q.v[2].c | q.v[3].c) >> 24) == 0;
			SV s0 = toScreen(q.v[0], (float)ox, (float)oy, alphaOne);
			SV s1 = toScreen(q.v[1], (float)ox, (float)oy, alphaOne);
			SV s2 = toScreen(q.v[2], (float)ox, (float)oy, alphaOne);
			SV s3 = toScreen(q.v[3], (float)ox, (float)oy, alphaOne);
			rasterTri(T, s0, s1, s2, q.blend, tp);
			rasterTri(T, s0, s2, s3, q.blend, tp);
		}
	}

	bool drawGuarded(DWORD dd, uint8_t *bmp, int ox, int oy, Batch *batch) {
		__try {
			drawBatch(dd, bmp, ox, oy, *batch);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// WA's type-3 queue item calls obj->vtbl[2](screenBitmap, px, py, p, q) as thiscall with 5 stack args
	// (DrawObject 0x56B7C0). px/py = screen position of the item's world point, here world (0, 0).
	struct QuadDrawer {
		virtual void unused0() {}
		virtual void unused1() {}
		virtual void draw(uint8_t *bmp, int px, int py, int slot, uint32_t gen) {
			if(slot < 0 || slot >= kPool || g_pool[slot].gen != gen || gen == 0)
				return;
			Batch *batch = &g_pool[slot];
			batch->gen = 0;
			if(Config::rendererEnabled && Renderer::isActive()) {
				if(!g_rendererWarned) {
					g_rendererWarned = true;
					quadLog("quad render: the GL renderer is active, quads are not drawn\n");
				}
				return;
			}
			DWORD dd = Game::getAddrDdDisplay();
			if(!dd || !bmp)
				return;
			LARGE_INTEGER t0, t1, fq;
			QueryPerformanceCounter(&t0);
			if(!drawGuarded(dd, bmp, px, py, batch)) {
				if(g_faults++ < 10)
					quadLog("quad render: fault while drawing a batch of %u quads\n", (unsigned)batch->quads.size());
			}
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&fq);
			g_drawMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
		}
	};
	QuadDrawer g_drawer;

	int toLayer(double v) {
		return (v > -256.0 && v < 256.0) ? (int)(v * 65536.0) : (int)v;
	}

	// hand-written type-3 item, same allocator as WA's enqueue functions (queue Q = *(G + 0x524))
	bool enqueue(int layer, int slot, uint32_t gen) {
		DWORD G = Game::getAddrGameGlobal();
		if(!G)
			return false;
		uint8_t *Q = *(uint8_t **)(G + 0x524);
		if(!Q)
			return false;
		int count = *(int *)(Q + 0x10004);
		if(count < 0 || count >= 0x800)
			return false;
		int left = *(int *)Q - 0x24;
		if(left < 0)
			return false;
		*(int *)Q = left;
		DWORD *item = (DWORD *)(Q + left + 4);
		((DWORD **)(Q + 0x10008))[count] = item;
		*(int *)(Q + 0x10004) = count + 1;
		item[0] = 3;
		item[1] = (DWORD)layer;
		item[2] = 0;   // world x
		item[3] = 0;   // world y
		item[4] = 0;   // z (perspective scale 1)
		item[5] = 0;
		item[6] = (DWORD)&g_drawer;
		item[7] = (DWORD)slot;
		item[8] = gen;
		return true;
	}

	// pxQuadSubmit(layer, data, n): data = flat array, 24 numbers per quad. Returns the number of quads queued.
	int luaSubmit(lua_State *L) {
		double layer = luaL_checknumber(L, 1);
		luaL_checktype(L, 2, LUA_TTABLE);
		lua_Integer n = luaL_checkinteger(L, 3);
		if(!Config::quadRenderEnabled || n <= 0 || n > 200000) {
			lua_pushinteger(L, 0);
			return 1;
		}
		int slot = (int)(g_nextSlot++ % kPool);
		Batch &b = g_pool[slot];
		b.gen = 0;
		b.quads.resize((size_t)n);
		int lastSprite = -1;
		double lastFrame = -1;
		int lastPhase = 0;
		for(lua_Integer k = 0; k < n; k++) {
			lua_Integer base = k * kQuadFloats;
			double f[kQuadFloats];
			for(int i = 0; i < kQuadFloats; i++) {
				lua_rawgeti(L, 2, base + i + 1);
				f[i] = lua_tonumber(L, -1);
				lua_pop(L, 1);
			}
			Quad &q = b.quads[(size_t)k];
			q.sprite = (int)f[0] & 0xFFFF;
			q.blend = (int)f[2];
			if(q.sprite > 0) {
				if(q.sprite != lastSprite || f[1] != lastFrame) {
					lastSprite = q.sprite;
					lastFrame = f[1];
					lastPhase = Sprites::framePhase(f[1], q.sprite);
				}
				q.phase = lastPhase;
			} else {
				q.phase = 0;
			}
			for(int i = 0; i < 4; i++) {
				const double *v = f + 4 + i * 5;
				q.v[i].x = (float)v[0];
				q.v[i].y = (float)v[1];
				q.v[i].tx = (float)v[2];
				q.v[i].ty = (float)v[3];
				q.v[i].c = (uint32_t)(int64_t)v[4];
			}
		}
		if(++g_gen == 0)
			g_gen = 1;
		if(!enqueue(toLayer(layer), slot, g_gen)) {
			g_rejected++;
			lua_pushinteger(L, 0);
			return 1;
		}
		b.gen = g_gen;
		g_batches++;
		g_quads += (uint32_t)n;
		lua_pushinteger(L, n);
		return 1;
	}

	// pxSpriteCell(id) -> cell width, height (0, 0 for an empty slot)
	int luaSpriteCell(lua_State *L) {
		int id = (int)luaL_checkinteger(L, 1) & 0xFFFF;
		int w = 0, h = 0;
		if(id > 0 && !Sprites::cellSize(id, w, h))
			w = h = 0;
		lua_pushinteger(L, w);
		lua_pushinteger(L, h);
		return 2;
	}

	// pxQuadStats() -> batches, quads, rejected, faults, draw ms since the last call
	int luaStats(lua_State *L) {
		lua_pushinteger(L, g_batches);
		lua_pushinteger(L, g_quads);
		lua_pushinteger(L, g_rejected);
		lua_pushinteger(L, g_faults);
		lua_pushnumber(L, g_drawMs);
		g_batches = g_quads = g_rejected = 0;
		g_drawMs = 0;
		return 5;
	}
}

void QuadRender::install() {
	auto *lua = Lua::getInstance().getState();
	lua_State *L = lua->lua_state();
	lua_register(L, "pxQuadSubmit", luaSubmit);
	lua_register(L, "pxSpriteCell", luaSpriteCell);
	lua_register(L, "pxQuadStats", luaStats);
}
