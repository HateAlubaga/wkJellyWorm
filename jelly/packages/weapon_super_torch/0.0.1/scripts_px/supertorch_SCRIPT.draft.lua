-- Source: supertorch_SCRIPT.pxc
-- Purpose: Loads a custom "wbloww3" sprite and modifies worm TurnSide when using Super Torch weapon.
-- When supertorch is active, TurnSide is set to ±3 (wider torch angle), then reset to ±1 when not firing.
-- PORT STATUS: PARTIAL (fire detection requires FireFinal hook not available in Lua API)

-- Register the custom sprite (original: LoadSprite(f, 15, 1))
local spr_wbloww3 = registerCustomSprite("wbloww3.png")

-- Persistent torch state; must be initialized because Lua globals start nil/false
local supertorch = false

local function wormcallback(This, sender, messagetype, psize, params)
    -- messagetype 3 = frame update (equivalent to M_FRAME)
    if messagetype == 3 then
        if supertorch then
            -- When using torch, widen the aim angle
            if This.turnside > 0 then
                This.turnside = 3
            elseif This.turnside < 0 then
                This.turnside = -3
            end

            -- Reset torch mode when not in firing states
            -- Note: ObjState values (WS_FIRED, WS_BLOWTORCH) are fixed-point enum values
            -- Adjust these constants to match your worm object's ObjState enum
            local is_firing = (This.objstate == WS_FIRED or This.objstate == WS_BLOWTORCH)
            if not is_firing then
                supertorch = false
                if This.turnside == 3 then
                    This.turnside = 1
                elseif This.turnside == -3 then
                    This.turnside = -1
                end
            end
        end
    end

    return 0
end

-- Register the frame-update callback
-- NOTE: Original code had a FireFinal hook to set supertorch=true when Super Torch fires.
-- Since Lua API does not expose FireFinal, you must detect the weapon elsewhere.
-- Possible approaches:
--   1. Check weapon ID/name in params on a different message type (if exposed)
--   2. Use a separate hook/callback mechanism if the wkJellyWorm runtime provides one
--   3. Poll the current weapon each frame (if worm object exposes weapon info)
--
-- Placeholder: uncomment below and adapt to your detected weapon-firing method
--
-- local function detect_torch_fire(This, ...)
--     -- Example: if SomeApi.GetCurrentWeapon(This) == "Super Torch" then
--     --     supertorch = true
--     -- end
-- end

-- --- Sprite override (commented in original) ---
-- SetSpriteOverride(140, wbloww3) -- TODO: not implemented; no Lua API equivalent found
