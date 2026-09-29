local walib = { _version = "0.0.6" }

-- Win32 virtual-key helpers
walib.VK_SHIFT = 0x10
walib.VK_CONTROL = 0x11
walib.VK_MENU = 0x12
walib.VK_SPACE = 0x20
walib.VK_LEFT = 0x25
walib.VK_UP = 0x26
walib.VK_RIGHT = 0x27
walib.VK_DOWN = 0x28

local keyBinds = {}
local gameKeyBinds = {}
local keyPrev = {}
local gameKeyPrev = {}
local keyPollInstalled = false

local function ensureKeyPoll()
	if keyPollInstalled then
		return
	end
	keyPollInstalled = true
	walib.onFrame(function()
		for vk, fn in pairs(keyBinds) do
			local down = walib.isKeyDown(vk)
			if down and not keyPrev[vk] then
				local ok, err = pcall(fn)
				if not ok then
					print("walib.bindKey error: " .. tostring(err))
				end
			end
			keyPrev[vk] = down
		end
		for key, fn in pairs(gameKeyBinds) do
			local down = walib.isGameKeyDown(key)
			if down and not gameKeyPrev[key] then
				local ok, err = pcall(fn)
				if not ok then
					print("walib.bindGameKey error: " .. tostring(err))
				end
			end
			gameKeyPrev[key] = down
		end
	end)
end

function walib.isKeyDown(vk)
	if isKeyDown then
		return isKeyDown(vk)
	end
	return false
end

function walib.isGameKeyDown(keyCode)
	if isGameKeyDown then
		return isGameKeyDown(keyCode)
	end
	return false
end

-- Per-frame callback (CTaskTurnGame RenderScene)
function walib.onFrame(fn)
	if registerCallback_onFrame and type(fn) == "function" then
		registerCallback_onFrame(fn)
		return true
	end
	return false
end

-- High-level event bus (thin wrappers over registerCallback_*)
function walib.onTurnStart(fn)
	if type(fn) ~= "function" then
		return false
	end
	if not registerCallback_onTurnStart then
		return false
	end
	registerCallback_onTurnStart(function()
		local ok, err = pcall(fn)
		if not ok then
			print("walib.onTurnStart error: " .. tostring(err))
		end
		return 0
	end)
	return true
end

function walib.onWeaponFired(fn)
	if type(fn) ~= "function" then
		return false
	end
	if not registerCallback_fireWeapon then
		return false
	end
	registerCallback_fireWeapon(function(worm, weaponStruct, launchParams)
		local ok, err = pcall(fn, worm, weaponStruct, launchParams)
		if not ok then
			print("walib.onWeaponFired error: " .. tostring(err))
		end
		return 0
	end)
	return true
end

function walib.onExplosion(fn)
	if type(fn) ~= "function" then
		return false
	end
	if not registerCallback_handleCreateExplosion then
		return false
	end
	registerCallback_handleCreateExplosion(function(posx, posy, sender, pushForce, power, a6, team)
		local ok, err = pcall(fn, posx, posy, sender, pushForce, power, a6, team)
		if not ok then
			print("walib.onExplosion error: " .. tostring(err))
		end
		return 0
	end)
	return true
end

-- Edge-triggered Win32 VK bind (fires once on press)
function walib.bindKey(vk, fn)
	if type(fn) ~= "function" then
		return false
	end
	keyBinds[vk] = fn
	keyPrev[vk] = walib.isKeyDown(vk)
	ensureKeyPoll()
	return true
end

-- Edge-triggered game key bind (Constants::KeyCodes)
function walib.bindGameKey(key, fn)
	if type(fn) ~= "function" then
		return false
	end
	gameKeyBinds[key] = fn
	gameKeyPrev[key] = walib.isGameKeyDown(key)
	ensureKeyPoll()
	return true
end

function walib.unbindAll()
	keyBinds = {}
	gameKeyBinds = {}
	keyPrev = {}
	gameKeyPrev = {}
end

-- One-shot or repeating timer via FrameScheduler (jellySchedule/jellyCancel).
-- Returns timer id (number) for walib.cancel, or false on failure.
-- walib.schedule(ms, fn)           — fire once after ms
-- walib.schedule(ms, fn, true)     — fire every ms (first after ms)
function walib.schedule(ms, fn, repeating)
	if type(fn) ~= "function" then
		return false
	end
	local delay = tonumber(ms) or 0
	if delay < 0 then
		delay = 0
	end
	local interval = 0
	if repeating then
		interval = delay
		if interval < 1 then
			interval = 1
		end
	end
	if jellySchedule then
		local id = jellySchedule(delay, interval, fn)
		if id and id ~= 0 then
			return id
		end
		return false
	end
	-- Fallback without FrameScheduler Lua bindings
	if not repeating then
		local fired = false
		local start = getTickCount and getTickCount() or 0
		walib.onFrame(function()
			if fired then
				return
			end
			local now = getTickCount and getTickCount() or 0
			if (now - start) >= delay then
				fired = true
				fn()
			end
		end)
		return true
	end
	return walib.every(delay, fn)
end

function walib.cancel(id)
	if type(id) ~= "number" or id <= 0 then
		return false
	end
	if jellyCancel then
		return jellyCancel(id)
	end
	return false
end

-- Call fn at most once every `ms` milliseconds.
-- Prefer FrameScheduler (returns cancelable id); falls back to onFrame.
function walib.every(ms, fn)
	if type(fn) ~= "function" then
		return false
	end
	local interval = tonumber(ms) or 0
	if interval < 0 then
		interval = 0
	end
	if jellySchedule then
		-- delay=0 fires this frame; interval>0 repeats (matches prior onFrame semantics)
		local id = jellySchedule(0, interval, fn)
		if id and id ~= 0 then
			return id
		end
		return false
	end
	local last = nil
	return walib.onFrame(function()
		local now = getTickCount and getTickCount() or 0
		if last == nil or (now - last) >= interval then
			last = now
			fn()
		end
	end)
end

-- Host-seeded synced RNG (online-safe when seed comes from wkJellyWormConfigStr)
function walib.random()
	if jellyRandom then
		return jellyRandom()
	end
	return math.random()
end

function walib.randomInt(min, max)
	min = tonumber(min) or 0
	max = tonumber(max) or min
	if jellyRandomInt then
		return jellyRandomInt(min, max)
	end
	if max < min then
		min, max = max, min
	end
	return math.random(min, max)
end

function walib.rngSeed()
	if jellyRngSeed then
		return jellyRngSeed()
	end
	return 0
end

function walib.rngIsSeeded()
	if jellyRngIsSeeded then
		return jellyRngIsSeeded()
	end
	return false
end

-- Tracker music (libxmp): MOD/XM/IT/S3M etc.
-- pathOrUrl: local path or http(s) URL (URLs cached in jelly/cache/music/)
function walib.loadTrackerMusic(pathOrUrl)
	if loadTrackerMusic then
		return loadTrackerMusic(pathOrUrl)
	end
	return -1
end

function walib.playTrackerMusic(id, loop, volume)
	if playTrackerMusic then
		return playTrackerMusic(id, loop, volume)
	end
	return -1
end

function walib.stopTrackerMusic(id)
	if stopTrackerMusic then
		return stopTrackerMusic(id)
	end
	return -1
end

function walib.pauseTrackerMusic(id, paused)
	if pauseTrackerMusic then
		return pauseTrackerMusic(id, paused)
	end
	return -1
end

function walib.setTrackerMusicVolume(id, volume)
	if setTrackerMusicVolume then
		return setTrackerMusicVolume(id, volume)
	end
	return -1
end

function walib.unloadTrackerMusic(id)
	if unloadTrackerMusic then
		return unloadTrackerMusic(id)
	end
	return -1
end

function walib.listTrackerMusicCache()
	if listTrackerMusicCache then
		return listTrackerMusicCache()
	end
	return ""
end

return walib
