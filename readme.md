# wkJellyWorm
_A dead ProjectX clone for Worms Armageddon 3.8.1_

This module hooks into most gameplay classes and exposes their functionality and internal structure to LUA scripts. It enables player to alter gameplay physics, object behaviour and add custom weapons.

### Features:
- Hooked all CTask and CGameTask classes - both member variables and virtual table methods
- Basic LUA scripting
- Support for adding more weapons to the game
- An experimental OpenGL renderer (Compatibility 3.2) — improved, still optional
- Basic support for online play

## Usage:
Host a LAN lobby and type: /enable test 
This will enable a test package with custom airstrike weapons. Type /help for more commands.

## Experimental OpenGL renderer

Disabled by default. Enable in `wkJellyWorm.ini`:

```ini
[renderer]
EnableCustomRenderer=1
DebugContext=1
GLVersionMajor=3
GLVersionMinor=2
EnableDebugPoints=0
SkipNativeBlit=0
FallbackOnContextError=1
```

| Option | Default | Meaning |
|--------|---------|---------|
| `EnableCustomRenderer` | 0 | Hook OpenGL path and draw 8-bit bitmaps via GL shaders |
| `DebugContext` | 0 | Request a debug GL context and log GL errors |
| `GLVersionMajor` / `GLVersionMinor` | 3 / 2 | Requested Compatibility profile version |
| `EnableDebugPoints` | 0 | Draw colored debug points at blit corners |
| `SkipNativeBlit` | 0 | Skip native CPU blit (enable only after GL path is verified) |
| `FallbackOnContextError` | 1 | Auto-disable custom renderer if context/shader init fails |

**What works:** WGL context creation with fallback, GLEW init guards, palette + indexed fragment shaders, 8-bit bitmap/sprite blits to screen and offscreen FBOs, palette updates on write/switch, per-frame `frameOverride` on `RenderScene` (clear/setup backbuffer before GL blits).

**Known limits:** Textboxes / HUD still use native drawing; full Core-profile / VBO path is not used (Compat 3.2 + `glBegin` quads). Keep `SkipNativeBlit=0` until in-game visuals look correct.

**Manual test plan:**
1. Set `EnableCustomRenderer=1`, `DebugContext=1` — game starts without crash; console shows GL/GLSL version.
2. Frontend landscape — colours look correct; palette changes apply.
3. In-game terrain — deformations remain visible.
4. Worms/sprites — frame atlas looks correct.
5. Chat `/drawing` — compare native vs GL path.
6. Exit game cleanly (no GL crash on teardown).

## Building:
Use cmake Release or RelWithDebInfo build types with MSVC compiler. Debug build types will not work due to custom calling conventions in some hooked functions.
