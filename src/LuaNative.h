#ifndef WKJELLYWORM_LUANATIVE_H
#define WKJELLYWORM_LUANATIVE_H

// Low-level primitives for the Lua side of the ProjectX runtime (pxrt.lua):
// typed memory access, WA-heap allocation and calling arbitrary game
// functions by address. pxrt maps PX engine methods onto these using the
// offsets in px/pxmap.json, so most new PX API needs no DLL rebuild.
class LuaNative {
public:
	static void install();
};

#endif //WKJELLYWORM_LUANATIVE_H
