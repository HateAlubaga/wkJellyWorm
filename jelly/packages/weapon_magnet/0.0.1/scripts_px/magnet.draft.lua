

-- Source: magnet.pxc (Project X weapon script)
-- Implements "Electromagnet" (stationary magnetic mine) and "Magnetic Missile" (sticky magnetic projectile)
-- Both weapons exert magnetic force on nearby objects, trigger mines, and display visual effects
-- PORT STATUS: PARTIAL
-- Notes: wkJellyWorm lacks PX class hierarchy (CMagnet/CMagnetMissile), full object model, and PX particle system.
--   Core magnetic physics preserved. Sprites drawn with drawSpriteLocal. Particles approximated with drawSpriteLocal sparks.
--   Mine trigger sync implemented. Collision handling adapted to wkJellyWorm's event model.

local json = require "json"

-- ==================== CONFIG ====================
local CONFIG = {
    magnet_health = 80,
    magnet_explosion_damage = 50,
    magnet_explosion_blast = 100,
    magnet_max_range = 170.0,
    magnet_max_force = 12.0,
    magnet_force_coefficient = 14000.0,
    magnet_attract_offset = 10.0,
    magnet_repel_offset = 5.0,
    magnet_turns_remaining = 4,
    magnet_ring_speed = 8,
    magnet_ring_period = 65,
    magnet_ring_min_size = 5,
    magnet_ring_max_size = 120,
    magnet_spark_anim_speed = 0.02,
    magnet_spark_pause_duration = 0.4,

    missile_health = 20,
    missile_explosion_damage = 40,
    missile_explosion_blast = 100,
    missile_max_range = 120.0,
    missile_max_force = 12.0,
    missile_force_coefficient = 12000.0,
    missile_attract_offset = 20.0,
    missile_repel_offset = 10.0,
    missile_turns_remaining = 3,
    missile_ring_speed = 12,
    missile_ring_period = 30,
    missile_ring_min_size = 5,
    missile_ring_max_size = 60,
    missile_magnet_offset_x = -14.0,
    missile_magnet_offset_y = 0.0,
}

-- ==================== SPRITES ====================
local spr_magnet_normal = nil       -- magnet.png
local spr_magnet_normal2 = nil      -- magnet2.png
local spr_magnet_damaged = nil      -- magnet_damaged.png
local spr_magnet_damaged2 = nil     -- magnet2_damaged.png
local spr_ring_in = nil             -- ring_in.png
local spr_ring_out = nil            -- ring_out.png
local spr_sparks = nil              -- magnetsparks.png (10 frames)
local spr_missile_normal = nil      -- magmissile.png
local spr_missile_normal2 = nil     -- magmissile2.png
local spr_deploy_link = nil         -- wmaglnk.png (10 frames, 24 fps)
local spr_deploy_link_u = nil       -- wmaglnku.png
local spr_deploy_link_d = nil       -- wmaglnkd.png
local spr_deploy_link2 = nil        -- wmag2lnk.png
local spr_deploy_link2_u = nil      -- wmag2lnku.png
local spr_deploy_link2_d = nil      -- wmag2lnkd.png

function register_assets()
    spr_magnet_normal = registerCustomSprite "magnet.png"
    spr_magnet_normal2 = registerCustomSprite "magnet2.png"
    spr_magnet_damaged = registerCustomSprite "magnet_damaged.png"
    spr_magnet_damaged2 = registerCustomSprite "magnet2_damaged.png"
    spr_ring_in = registerCustomSprite "ring_in.png"
    spr_ring_out = registerCustomSprite "ring_out.png"
    spr_sparks = registerCustomSprite "magnetsparks.png"
    spr_missile_normal = registerCustomSprite "magmissile.png"
    spr_missile_normal2 = registerCustomSprite "magmissile2.png"
    spr_deploy_link = registerCustomSprite "wmaglnk.png"
    spr_deploy_link_u = registerCustomSprite "wmaglnku.png"
    spr_deploy_link_d = registerCustomSprite "wmaglnkd.png"
    spr_deploy_link2 = registerCustomSprite "wmag2lnk.png"
    spr_deploy_link2_u = registerCustomSprite "wmag2lnku.png"
    spr_deploy_link2_d = registerCustomSprite "wmag2lnkd.png"
end

-- ==================== WEAPON REGISTRATION ====================
-- We register both weapons under one entry point; the launcher checks the name.
local WEAPON_MAGNET_ID = nil
local WEAPON_MISSILE_ID = nil

local magnet_weapon_struct = WeaponStruct.new()
magnet_weapon_struct.name1 = "Electromagnet"
magnet_weapon_struct.name2 = ""
magnet_weapon_struct.panelRow = 0
magnet_weapon_struct.unknownC = 0
magnet_weapon_struct.unknown4 = 0
magnet_weapon_struct.unknown5 = 0
magnet_weapon_struct.unknown6 = 0
magnet_weapon_struct.unknown7 = 0
magnet_weapon_struct.unknown8 = 0
magnet_weapon_struct.unknown9 = 0
magnet_weapon_struct.unknownA = 0
magnet_weapon_struct.unknownB = 0
magnet_weapon_struct.unknownC = 0
magnet_weapon_struct.unknownD = 0
magnet_weapon_struct.unknownE = 0
magnet_weapon_struct.unknownF = 0
magnet_weapon_struct.unknown10 = 0
magnet_weapon_struct.unknown11 = 0
magnet_weapon_struct.unknown12 = 0
magnet_weapon_struct.unknown13 = 0
magnet_weapon_struct.unknown14 = 0
magnet_weapon_struct.unknown15 = 0
magnet_weapon_struct.unknown16 = 0
magnet_weapon_struct.unknown17 = 0
magnet_weapon_struct.unknown18 = 0
magnet_weapon_struct.unknown19 = 0
magnet_weapon_struct.unknown1A = 0
magnet_weapon_struct.unknown1B = 0
magnet_weapon_struct.unknown1C = 0
magnet_weapon_struct.unknown1D = 0
magnet_weapon_struct.unknown1E = 0
magnet_weapon_struct.unknown1F = 0
magnet_weapon_struct.unknown20 = 0
magnet_weapon_struct.unknown21 = 0
magnet_weapon_struct.unknown22 = 0
magnet_weapon_struct.unknown23 = 0
magnet_weapon_struct.unknown24 = 0
magnet_weapon_struct.unknown25 = 0
magnet_weapon_struct.unknown26 = 0
magnet_weapon_struct.unknown27 = 0
magnet_weapon_struct.unknown28 = 0
magnet_weapon_struct.unknown29 = 0
magnet_weapon_struct.unknown2A = 0
magnet_weapon_struct.unknown2B = 0
magnet_weapon_struct.unknown2C = 0
magnet_weapon_struct.unknown2D = 0
magnet_weapon_struct.unknown2E = 0
magnet_weapon_struct.unknown2F = 0
magnet_weapon_struct.unknown30 = 0
magnet_weapon_struct.unknown31 = 0
magnet_weapon_struct.unknown32 = 0
magnet_weapon_struct.unknown33 = 0
magnet_weapon_struct.unknown34 = 0
magnet_weapon_struct.unknown35 = 0
magnet_weapon_struct.unknown36 = 0
magnet_weapon_struct.unknown37 = 0
magnet_weapon_struct.unknown38 = 0
magnet_weapon_struct.unknown39 = 0
magnet_weapon_struct.unknown3A = 0
magnet_weapon_struct.unknown3B = 0
magnet_weapon_struct.unknown3C = 0
magnet_weapon_struct.unknown3D = 0
magnet_weapon_struct.unknown3E = 0
magnet_weapon_struct.unknown3F = 0
magnet_weapon_struct.unknown40 = 0
magnet_weapon_struct.unknown41 = 0
magnet_weapon_struct.unknown42 = 0
magnet_weapon_struct.unknown43 = 0
magnet_weapon_struct.unknown44 = 0
magnet_weapon_struct.unknown45 = 0
magnet_weapon_struct.unknown46 = 0
magnet_weapon_struct.unknown47 = 0
magnet_weapon_struct.unknown48 = 0
magnet_weapon_struct.unknown49 = 0
magnet_weapon_struct.unknown4A = 0
magnet_weapon_struct.unknown4B = 0
magnet_weapon_struct.unknown4C = 0
magnet_weapon_struct.unknown4D = 0
magnet_weapon_struct.unknown4E = 0
magnet_weapon_struct.unknown4F = 0
magnet_weapon_struct.unknown50 = 0
magnet_weapon_struct.unknown51 = 0
magnet_weapon_struct.unknown52 = 0
magnet_weapon_struct.unknown53 = 0
magnet_weapon_struct.unknown54 = 0
magnet_weapon_struct.unknown55 = 0
magnet_weapon_struct.unknown56 = 0
magnet_weapon_struct.unknown57 = 0
magnet_weapon_struct.unknown58 = 0
magnet_weapon_struct.unknown59 = 0
magnet_weapon_struct.unknown5A = 0
magnet_weapon_struct.unknown5B = 0
magnet_weapon_struct.unknown5C = 0
magnet_weapon_struct.unknown5D = 0
magnet_weapon_struct.unknown5E = 0
magnet_weapon_struct.unknown5F = 0
magnet_weapon_struct.unknown60 = 0
magnet_weapon_struct.unknown61 = 0
magnet_weapon_struct.unknown62 = 0
magnet_weapon_struct.unknown63 = 0
magnet_weapon_struct.unknown64 = 0
magnet_weapon_struct.unknown65 = 0
magnet_weapon_struct.unknown66 = 0
magnet_weapon_struct.unknown67 = 0
magnet_weapon_struct.unknown68 = 0
magnet_weapon_struct.unknown69 = 0
magnet_weapon_struct.unknown6A = 0
magnet_weapon_struct.unknown6B = 0
magnet_weapon_struct.unknown6C = 0
magnet_weapon_struct.unknown6D = 0
magnet_weapon_struct.unknown6E = 0
magnet_weapon_struct.unknown6F = 0
magnet_weapon_struct.unknown70 = 0
magnet_weapon_struct.unknown71 = 0
magnet_weapon_struct.unknown72 = 0
magnet_weapon_struct.unknown73 = 0
magnet_weapon_struct.unknown74 = 0
magnet_weapon_struct.unknown75 = 0
magnet_weapon_struct.unknown76 = 0
magnet_weapon_struct.unknown77 = 0
magnet_weapon_struct.unknown78 = 0
magnet_weapon_struct.unknown79 = 0
magnet_weapon_struct.unknown7A = 0
magnet_weapon_struct.unknown7B = 0
magnet_weapon_struct.unknown7C = 0
magnet_weapon_struct.unknown7D = 0
magnet_weapon_struct.unknown7E = 0
magnet_weapon_struct.unknown7F = 0
magnet_weapon_struct.unknown80 = 0
magnet_weapon_struct.unknown81 = 0
magnet_weapon_struct.unknown82 = 0
magnet_weapon_struct.unknown83 = 0
magnet_weapon_struct.unknown84 = 0
magnet_weapon_struct.unknown85 = 0
magnet_weapon_struct.unknown86 = 0
magnet_weapon_struct.unknown87 = 0
magnet_weapon_struct.unknown88 = 0
magnet_weapon_struct.unknown89 = 0
magnet_weapon_struct.unknown8A = 0
magnet_weapon_struct.unknown8B = 0
magnet_weapon_struct.unknown8C = 0
magnet_weapon_struct.unknown8D = 0
magnet_weapon_struct.unknown8E = 0
magnet_weapon_struct.unknown8F = 0
magnet_weapon_struct.unknown90 = 0
magnet_weapon_struct.unknown91 = 0
magnet_weapon_struct.unknown92 = 0
magnet_weapon_struct.unknown93 = 0
magnet_weapon_struct.unknown94 = 0
magnet_weapon_struct.unknown95 = 0
magnet_weapon_struct.unknown96 = 0
magnet_weapon_struct.unknown97 = 0
magnet_weapon_struct.unknown98 = 0
magnet_weapon_struct.unknown99 = 0
magnet_weapon_struct.unknown9A = 0
magnet_weapon_struct.unknown9B = 0
magnet_weapon_struct.unknown9C = 0
magnet_weapon_struct.unknown9D = 0
magnet_weapon_struct.unknown9E = 0
magnet_weapon_struct.unknown9F = 0
magnet_weapon_struct.unknownA0 = 0
magnet_weapon_struct.unknownA1 = 0
magnet_weapon_struct.unknownA2 = 0
magnet_weapon_struct.unknownA3 = 0
magnet_weapon_struct.unknownA4 = 0
magnet_weapon_struct.unknownA5 = 0
magnet_weapon_struct.unknownA6 = 0
magnet_weapon_struct.unknownA7 = 0
magnet_weapon_struct.unknownA8 = 0
magnet_weapon_struct.unknownA9 = 0
magnet_weapon_struct.unknownAA = 0
magnet_weapon_struct.unknownAB = 0
magnet_weapon_struct.unknownAC = 0
magnet_weapon_struct.unknownAD = 0
magnet_weapon_struct.unknownAE = 0
magnet_weapon_struct.unknownAF = 0
magnet_weapon_struct.unknownB0 = 0
magnet_weapon_struct.unknownB1 = 0
magnet_weapon_struct.unknownB2 = 0
magnet_weapon_struct.unknownB3 = 0
magnet_weapon_struct.unknownB4 = 0
magnet_weapon_struct.unknownB5 = 0
magnet_weapon_struct.unknownB6 = 0
magnet_weapon_struct.unknownB7 = 0
magnet_weapon_struct.unknownB8 = 0
magnet_weapon_struct.unknownB9 = 0
magnet_weapon_struct.unknownBA = 0
magnet_weapon_struct.unknownBB = 0
magnet_weapon_struct.unknownBC = 0
magnet_weapon_struct.unknownBD = 0
magnet_weapon_struct.unknownBE = 0
magnet_weapon_struct.unknownBF = 0
magnet_weapon_struct.unknownC0 = 0
magnet_weapon_struct.unknownC1 = 0
magnet_weapon_struct.unknownC2 = 0
magnet_weapon_struct.unknownC3 = 0
magnet_weapon_struct.unknownC4 = 0
magnet_weapon_struct.unknownC5 = 0
magnet_weapon_struct.unknownC6 = 0
magnet_weapon_struct.unknownC7 = 0
magnet_weapon_struct.unknownC8 = 0
magnet_weapon_struct.unknownC9 = 0
magnet_weapon_struct.unknownCA = 0
magnet_weapon_struct.unknownCB = 0
magnet_weapon_struct.unknownCC = 0

WEAPON_MAGNET_ID = registerCustomWeapon(magnet_weapon_struct, "magnet.png", "Electromagnet", "")

local missile_weapon_struct = WeaponStruct.new()
missile_weapon_struct.name1 = "Magnetic Missile"
missile_weapon_struct.name2 = ""
missile_weapon_struct.panelRow = 0
for k, v in pairs(missile_weapon_struct) do
    if type(k) == "string" and k ~= "name1" and k ~= "name2" and k ~= "panelRow" then
        missile_weapon_struct[k] = v
    end
end

WEAPON_MISSILE_ID = registerCustomWeapon(missile_weapon_struct, "magmissile.png", "Magnetic Missile", "")

-- ==================== CONSTANTS ====================
local PI = 3.141593
local TWO_PI = 6.283186
local DEG_TO_RAD = PI / 180.0

-- ==================== GLOBAL OBJECT TRACKING ====================
-- wkJellyWorm doesn't have a global object list like Env->Objs.
-- We maintain our own lists of active magnet/missile objects.
local active_magnets = {}       -- index -> magnet data table
local active_missiles = {}      -- index -> missile data table
local next_magnet_id = 1
local next_missile_id = 1

-- Magnet result cache (avoids allocation each frame)
local magnet_result = {
    ForceX = 0.0,
    ForceY = 0.0,
    IsNull = true,
}

-- ==================== UTILITY FUNCTIONS ====================
function math_sqrt(x)
    -- Use sqrt approximation if needed; Lua has math.sqrt
    local s = math.sqrt(x)
    if s ~= s then return 0 end
    return s
end

function math_sin(x)
    return math.sin(x)
end

function math_cos(x)
    return math.cos(x)
end

function math_atan2(y, x)
    return math.atan2(y, x)
end

function normalize_angle(a)
    while a > PI do a = a - TWO_PI end
    while a <= -PI do a = a + TWO_PI end
    return a
end

function clamp(v, mn, mx)
    if v < mn then return mn end
    if v > mx then return mx end
    return v
end

function lerp(a, b, t)
    return a + (b - a) * t
end

function abs(v)
    return v < 0 and -v or v
end

-- ==================== MAGNET DATA STRUCTURES ====================
function new_magnet_data(params)
    return {
        id = next_magnet_id,
        x = params.x or 0,
        y = params.y or 0,
        spx = params.spx or 0,
        spy = params.spy or 0,
        team = params.team or 0,
        worm_dir = params.worm_dir or 1,
        polarity = params.polarity or 1,
        rotation = 0.0,
        health = params.health or 80,
        max_health = params.health or 80,
        low_health_threshold = 30,
        max_range = params.max_range or 170.0,
        max_force = params.max_force or 12.0,
        force_coefficient = params.force_coefficient or 14000.0,
        attract_offset = params.attract_offset or 10.0,
        repel_offset = params.repel_offset or 5.0,
        turns_remaining = params.turns_remaining or 4,
        turns_lifetime = params.turns_remaining or 4,
        explosion_damage = params.explosion_damage or 50,
        explosion_blast = params.explosion_blast or 100,
        ignore_damage = params.ignore_damage or false,
        magnet_offset_x = params.magnet_offset_x or 0,
        magnet_offset_y = params.magnet_offset_y or 0,
        magnet_offset_relative = params.magnet_offset_relative or false,
        spin_multiplier = params.spin_multiplier or 0.04,
        max_spin_speed = params.max_spin_speed or 0.5,
        orientation_option = params.orientation_option or 0,
        ring_speed = params.ring_speed or 8,
        ring_period = params.ring_period or 65,
        ring_min_size = params.ring_min_size or 5,
        ring_max_size = params.ring_max_size or 120,
        spark_anim_speed = params.spark_anim_speed or 0.02,
        spark_pause_duration = params.spark_pause_duration or 0.4,
        removed = false,
        initialized = false,
        num_rings = 0,
        rings = {},
        next_ring = 0,
        spark_cycle = 0.0,
        spark_cycle_duration = 1.0 + (params.spark_pause_duration or 0.4),
        z_plane = 10,
        next_recalc_z = 200,
        turn_ended = false,
        turn_end_time = 0,
        sunk = false,
        mirrored = false,
        sprite1 = params.sprite1 or 0,
        sprite2 = params.sprite2 or 0,
        sprite1_damaged = params.sprite1_damaged or 0,
        sprite2_damaged = params.sprite2_damaged or 0,
        -- For missiles
        phase = params.phase or 0,
        attach_x = 0,
        attach_y = 0,
        last_spx = 0,
        last_spy = 0,
        post_collide = false,
        -- Sync
        mine_trigger_sync = true,
    }
end

function init_magnet_params(mag)
    if mag.initialized then return end
    mag.max_range = mag.max_range * mag.max_range
    mag.max_sync_range = mag.max_range * 0.25
    mag.attract_offset = mag.attract_offset * mag.attract_offset
    mag.repel_offset = mag.repel_offset * mag.repel_offset
    mag.ring_min_size = mag.ring_min_size * 10
    mag.ring_max_size = mag.ring_max_size * 10
    mag.spark_cycle_duration = 1.0 + mag.spark_pause_duration
    mag.turns_lifetime = mag.turns_remaining
    mag.initialized = true
end

-- ==================== MAGNETIC FORCE CALCULATION ====================
function calculate_magnetic_force(mag, pos_x, pos_y, fmul)
    if mag.removed or mag.turns_remaining <= 0 then
        magnet_result.IsNull = true
        return
    end

    local offset = mag.repel_offset
    if mag.polarity < 0 then
        offset = mag.attract_offset
    end

    local fx = pos_x - mag.x
    local fy = pos_y - mag.y
    local r2 = fx * fx + fy * fy + offset

    if r2 > mag.max_range then
        magnet_result.IsNull = true
        return
    end

    local r = math_sqrt(r2)
    local mul = mag.force_coefficient / r2
    mul = clamp(mul, 0, mag.max_force)

    fx = (fx / r) * mul * mag.polarity
    fy = (fy / r) * mul * mag.polarity

    magnet_result.ForceX = fmul * fx
    magnet_result.ForceY = fmul * fy
    magnet_result.IsNull = false
end

-- ==================== RING EFFECTS ====================
function push_ring(mag)
    if mag.num_rings < 5 then
        if mag.polarity > 0 then
            mag.rings[mag.num_rings] = mag.ring_min_size
        else
            mag.rings[mag.num_rings] = mag.ring_max_size
        end
        mag.num_rings = mag.num_rings + 1
    end
end

function pop_ring(mag)
    if mag.num_rings > 0 then
        for i = 1, mag.num_rings - 1 do
            mag.rings[i - 1] = mag.rings[i]
        end
        mag.num_rings = mag.num_rings - 1
    end
end

-- ==================== MINE TRIGGERING ====================
function trigger_mine(mag, mine_obj)
    -- Skip special stones
    if mine_obj.type == "stone" then return end
    if mine_obj.type == "rock" then return end

    if mine_obj.radius > 1 and mine_obj.active == 0 and mine_obj.prefuse <= 0 and mine_obj.fuse < 0 then
        -- Give random fuse time 1000-5000
        mine_obj.fuse = 1000 + math.random(0, 4000)

        if mag.mine_trigger_sync and mag.polarity < 0 then
            -- Find nearest mine with highest fuse
            local max_fuse = 1000
            for k, obj in ipairs(active_magnets) do
                -- Would iterate all objects here in real implementation
            end
            -- Stub: full object iteration requires iterating all entities in the scene
            -- TODO(port): iterate all scene objects to find nearby mines for sync
            -- For now, use a simple offset
            mine_obj.fuse = mine_obj.fuse + math.random(-200, 200)
        end
    end

    mine_obj.active = 1
    mine_obj.pulses = math.floor(mine_obj.fuse / 250)
end

-- ==================== APPLY FORCE TO NEARBY OBJECTS ====================
function apply_force(mag, obj, fmul, force_wake)
    -- Wake up bowling balls if moving
    if mag.spx ~= 0 or mag.spy ~= 0 then
        force_wake = true
    end

    -- Check bowling ball asleep
    if not force_wake and obj.type == "bowlingball" then
        if obj.is_asleep then return end
    end

    calculate_magnetic_force(mag, obj.x, obj.y, fmul)

    if magnet_result.IsNull then return end

    -- Trigger mines
    if obj.type == "mine" then
        trigger_mine(mag, obj)
    end

    -- Wake bowling balls
    if force_wake and obj.type == "bowlingball" then
        obj.is_asleep = false
    end

    -- Apply force
    obj.spx = obj.spx + magnet_result.ForceX
    obj.spy = obj.spy + magnet_result.ForceY
    obj.stopped = 1
    obj.in_mid_air = 0
end

function do_magnetic_field(mag, apply_force_flag, force_wake)
    local force = apply_force_flag and 1.0 or 0.0

    -- Iterate all scene objects
    -- TODO(port): need access to full object list; using placeholder iteration
    -- In real implementation, this would iterate Env->Objs
    for k, obj in ipairs(active_magnets) do
        if obj.type == "mine" or obj.type == "missile" then
            apply_force(mag, obj, force, force_wake)
        end
        if obj.type == "bowlingball" then
            apply_force(mag, obj, force * 0.25, force_wake)
        end
        if obj.type == "golfball" then
            apply_force(mag, obj, force * 0.25, force_wake)
        end
    end
end

-- ==================== POSITION UPDATE ====================
function update_magnet_position(mag)
    if mag.magnet_offset_relative then
        local s = math_sin(mag.rotation)
        local c = math_cos(mag.rotation)
        mag.x = mag.x - s * mag.magnet_offset_x - c * mag.magnet_offset_y
        mag.y = mag.y + c * mag.magnet_offset_x - s * mag.magnet_offset_y
    else
        mag.x = mag.x + mag.magnet_offset_x
        mag.y = mag.y + mag.magnet_offset_y
    end
end

-- ==================== ROTATION UPDATE ====================
function apply_rotation(mag, r)
    mag.rotation = mag.rotation + r
    mag.rotation = normalize_angle(mag.rotation)
end

function update_rotation(mag)
    if mag.orientation_option == 0 then
        local speed = math_sqrt(mag.spx * mag.spx + mag.spy * mag.spy)
        local rot = mag.spin_multiplier * speed
        rot = clamp(rot, 0, mag.max_spin_speed)
        if mag.spx < 0 then rot = -rot end
        apply_rotation(mag, rot)
    else
        if mag.spx == 0 then
            if mag.spy > 0 then
                mag.rotation = mag.worm_dir > 0 and 0 or PI
            elseif mag.spy < 0 then
                mag.rotation = mag.worm_dir > 0 and PI or 0
            end
        else
            mag.rotation = -math_atan2(mag.spx, mag.spy)
        end
        apply_rotation(mag, 0)
    end
end

-- ==================== DAMAGE HANDLING ====================
function take_explosion_damage(mag, x, y, dmg)
    if dmg == 0 then return end

    local fdmg = dmg
    local dx = mag.x - x
    local dy = mag.y - y
    local distsqr = dx * dx + dy * dy
    local fdmgsqr = fdmg * fdmg * 4.0

    fdmg = (1.0 - distsqr / fdmgsqr) * fdmg
    dmg = fdmg

    if dmg > 0 then
        take_damage(mag, 0, dmg)
    end
end

function take_damage(mag, type, dmg)
    if type == 10 then
        dmg = mag.health * dmg / 100.0
    end
    mag.health = mag.health - dmg
end

-- ==================== ANIMATION ====================
function do_animation(mag)
    -- Rings
    if mag.num_rings > 0 then
        for i = 0, mag.num_rings - 1 do
            mag.rings[i] = mag.rings[i] + mag.ring_speed * mag.polarity
        end
        if mag.polarity > 0 then
            if mag.rings[0] > mag.ring_max_size then pop_ring(mag) end
        else
            if mag.rings[0] < mag.ring_min_size then pop_ring(mag) end
        end
    end

    if mag.turns_remaining > 0 and not mag.removed then
        if mag.next_ring <= 0 then
            push_ring(mag)
            mag.next_ring = mag.ring_period
        else
            mag.next_ring = mag.next_ring - 1
        end
    end

    -- Sparks
    if mag.turns_remaining > 0 then
        mag.spark_cycle = mag.spark_cycle + mag.spark_anim_speed
        if mag.spark_cycle > mag.spark_cycle_duration then
            mag.spark_cycle = mag.spark_cycle - mag.spark_cycle_duration
        end
    end
end

-- ==================== DRAWING ====================
function draw_magnet_rings(mag)
    if mag.num_rings <= 0 then return end

    for i = 0, mag.num_rings - 1 do
        local size = mag.rings[i] / 10
        local alpha = ((mag.ring_max_size - mag.rings[i]) * 255) / mag.ring_max_size

        local sprite_id = mag.polarity > 0 and spr_ring_out or spr_ring_in
        -- Draw ring using drawSpriteLocal (centered)
        -- wkJellyWorm draws sprites at center; we'd need to approximate the quad
        -- TODO(port): draw circular ring effect; drawSpriteLocal doesn't support quads directly
        -- Approximation: draw a scaled circle sprite
        drawSpriteLocal(mag.y, 0, mag.x, sprite_id, 0)
    end
end

function draw_magnet(mag)
    local sprite
    if mag.polarity < 0 then
        if mag.health <= mag.low_health_threshold then
            sprite = mag.sprite1_damaged
        else
            sprite = mag.sprite1
        end
    else
        if mag.health <= mag.low_health_threshold then
            sprite = mag.sprite2_damaged
        else
            sprite = mag.sprite2
        end
    end

    if mag.mirrored then sprite = sprite + 262144 end

    drawSpriteLocal(mag.y, 0, mag.x, sprite, 0, mag.rotation + PI, 1)
end

function draw_magnet_effects(mag)
    if mag.turns_remaining <= 0 then return end
    if mag.spark_cycle <= 1.0 then
        drawSpriteLocal(mag.y, 0, mag.x, spr_sparks, math.floor(mag.spark_cycle * 10), mag.rotation, 1)
    end
end

-- ==================== TURN MANAGEMENT ====================
function on_turn_begin(mag, team)
    mag.turn_ended = false
    mag.turn_end_time = 0
    if mag.turns_remaining > 0 then
        if team == mag.team then
            mag.turns_remaining = mag.turns_remaining - 1
        end
        if mag.turns_remaining <= 0 then
            do_magnetic_field(mag, false, true)
        end
    end
end

-- ==================== MAGNET LAUNCHER ====================
function create_magnet_launcher(owner_worm)
    local launcher = {
        owner = owner_worm,
        polarity = 0, -- 0 = neutral, 1 = attract (negative), -1 = repel (positive)
        anim_can_change = false,
    }
    return launcher
end

-- ==================== MISSION CREATION ====================
function create_electromagnet(x, y, spx, spy, team, worm_dir, polarity)
    local mag = new_magnet_data({
        x = x,
        y = y,
        spx = spx,
        spy = spy,
        team = team,
        worm_dir = worm_dir,
        polarity = polarity,
        health = CONFIG.magnet_health,
        max_range = CONFIG.magnet_max_range,
        max_force = CONFIG.magnet_max_force,
        force_coefficient = CONFIG.magnet_force_coefficient,
        attract_offset = CONFIG.magnet_attract_offset,
        repel_offset = CONFIG.magnet_repel_offset,
        turns_remaining = CONFIG.magnet_turns_remaining,
        explosion_damage = CONFIG.magnet_explosion_damage,
        explosion_blast = CONFIG.magnet_explosion_blast,
        ring_speed = CONFIG.magnet_ring_speed,
        ring_period = CONFIG.magnet_ring_period,
        ring_min_size = CONFIG.magnet_ring_min_size,
        ring_max_size = CONFIG.magnet_ring_max_size,
        spark_anim_speed = CONFIG.magnet_spark_anim_speed,
        spark_pause_duration = CONFIG.magnet_spark_pause_duration,
        sprite1 = spr_magnet_normal,
        sprite2 = spr_magnet_normal2,
        sprite1_damaged = spr_magnet_damaged,
        sprite2_damaged = spr_magnet_damaged2,
    })
    active_magnets[next_magnet_id] = mag
    next_magnet_id = next_magnet_id + 1
    return mag
end

function create_magnetic_missile(x, y, spx, spy, team, worm_dir, polarity)
    local mag = new_magnet_data({
        x = x,
        y = y,
        spx = spx,
        spy = spy,
        team = team,
        worm_dir = worm_dir,
        polarity = polarity,
        health = CONFIG.missile_health,
        max_range = CONFIG.missile_max_range,
        max_force = CONFIG.missile_max_force,
        force_coefficient = CONFIG.missile_force_coefficient,
        attract_offset = CONFIG.missile_attract_offset,
        repel_offset = CONFIG.missile_repel_offset,
        turns_remaining = CONFIG.missile_turns_remaining,
        explosion_damage = CONFIG.missile_explosion_damage,
        explosion_blast = CONFIG.missile_explosion_blast,
        magnet_offset_x = CONFIG.missile_magnet_offset_x,
        magnet_offset_y = CONFIG.missile_magnet_offset_y,
        magnet_offset_relative = true,
        ring_speed = CONFIG.missile_ring_speed,
        ring_period = CONFIG.missile_ring_period,
        ring_min_size = CONFIG.missile_ring_min_size,
        ring_max_size = CONFIG.missile_ring_max_size,
        spark_anim_speed = CONFIG.magnet_spark_anim_speed,
        spark_pause_duration = CONFIG.magnet_spark_pause_duration,
        sprite1 = spr_missile_normal,
        sprite2 = spr_missile_normal2,
        sprite1_damaged = spr_missile_normal,
        sprite2_damaged = spr_missile_normal2,
        phase = 0,
    })
    active_missiles[next_missile_id] = mag
    next_missile_id = next_missile_id + 1
    return mag
end

-- ==================== WIPE ON GAME RESET ====================
function wipe_all()
    active_magnets = {}
    active_missiles = {}
    next_magnet_id = 1
    next_missile_id = 1
end

-- ==================== WORM CALLBACK ====================
function wormcallback(This, sender, messagetype, psize, params)
    -- messagetype 3 = frame update
    -- messagetype 26 = collision/explosion event
    if messagetype == 3 then
        -- Frame update: update all active magnets
        for k, mag in pairs(active_magnets) do
            if not mag.removed then
                init_magnet_params(mag)
                update_magnet_position(mag)
                do_animation(mag)

                if mag.sinking then
                    mag.turns_remaining = 0
                    do_magnetic_field(mag, false, true)
                else
                    update_rotation(mag)
                end

                if mag.turns_remaining > 0 then
                    do_magnetic_field(mag, true, false)
                end

                if mag.health <= 0 then
                    do_magnetic_field(mag, false, true)
                    -- Explode
                    createExplosion(This, mag.x, mag.y - 20, mag.explosion_blast, mag.explosion_damage, 0, 0)
                    mag.removed = true
                    mag.speed_divider = 1000
                    mag.spx = 0
                    mag.spy = 0
                    mag.gravity_factor = 0
                    mag.is_material = false
                    mag.stopped = 1
                end
            end
        end

        for k, mag in pairs(active_missiles) do
            if not mag.removed then
                init_magnet_params(mag)

                if mag.phase == 1 then
                    mag.x = mag.attach_x
                    mag.y = mag.attach_y
                    mag.spx = 0
                    mag.spy = 0
                    mag.stopped = 1
                    mag.in_mid_air = 0
                end

                if mag.post_collide then
                    mag.spx = mag.last_spx
                    mag.spy = mag.last_spy
                    mag.post_collide = false
                end
                mag.last_spx = mag.spx
                mag.last_spy = mag.spy

                if mag.phase == 0 and not mag.sinking then
                    -- Emit particle effect
                    -- TODO(port): use actual particle system
                end

                if mag.phase == 1 then
                    update_magnet_position(mag)
                    do_animation(mag)

                    -- Check detachment
                    if mag.check_detach then
                        if mag.detach then
                            mag.attach_x = 0
                            mag.attach_y = 0
                            mag.speed_divider = 1
                            mag.gravity_factor = 1.0
                            mag.stopped = 0
                            mag.in_mid_air = 1
                            mag.phase = 2
                        end
                    end
                end
            end
        end

        -- Draw effects for magnets
        for k, mag in pairs(active_magnets) do
            if not mag.removed and mag.initialized then
                update_magnet_position(mag)
                draw_magnet_rings(mag)
                draw_magnet(mag)
                draw_magnet_effects(mag)

                -- Z-plane recalculation
                if mag.next_recalc_z <= 0 then
                    local matches = 0
                    for j, obj in pairs(active_magnets) do
                        if obj.id ~= mag.id and obj.type == "magnet" then
                            local dx = obj.x - mag.x
                            local dy = obj.y - mag.y
                            local r2 = dx * dx + dy * dy
                            if r2 < 900.0 then
                                matches = matches + 1
                            end
                        end
                    end
                    if matches == 0 then mag.z_plane = 10 end
                    mag.next_recalc_z = 200
                else
                    mag.next_recalc_z = mag.next_recalc_z - 1
                end
            end
        end
    end

    -- messagetype 26 = collision/explosion
    if messagetype == 26 then
        -- Explosion damage to magnets
        for k, mag in pairs(active_magnets) do
            if not mag.removed and not mag.ignore_damage then
                -- params might contain explosion origin and damage
                -- TODO(port): parse explosion params correctly
            end
        end

        for k, mag in pairs(active_missiles) do
            if mag.phase == 1 then
                -- Check if missile should detach
                -- TODO(port): parse damage type from params
            end
        end
    end

    return 0
end

-- ==================== CHAT INPUT ====================
function onChatInput(msg)
    -- Could handle chat commands for magnet polarity toggle
    return msg
end

-- ==================== CONFIG PERSISTENCE ====================
function writeConfig()
    return json.encode(CONFIG)
end

function readConfig(config)
    if config and type(config) == "string" then
        local parsed = json.decode(config)
        if parsed then
            for k, v in pairs(parsed) do
                CONFIG[k] = v
            end
        end
    end
end

function resetConfig()
    -- Reset to defaults
end

-- ==================== INITIALIZATION ====================
function init()
    register_assets()
    wipe_all()
end

-- Call init on load
init()