-- jellyreverse v2.0.0
-- mode 1 — активный червяк (+ отступление)
-- mode 2 — все физ. объекты (Shift = реверс всего)
-- mode 3 — свой червяк Shift+стрелки; враги реверсятся при уроне
-- mode 4 — как mode 1; без isKeyDown — Shift через isGameKeyDown или /rev в чате
-- online: Shift шлёт wkJellySync:… в сеть (скрыто), у всех клиентов срабатывает /rev
--
-- changelog (правки):
--  - flight-state хак (принудительный "полёт как после верёвки") теперь
--    ВЫКЛЮЧЕН по умолчанию (S.forceFlightState=false): именно он вызывал
--    "скольжение как при потере контроля" при приземлении после реверса
--    вне верёвки. Включается вручную: /jr flight 1 (сохраняется в конфиге).
--  - detachRope всё ещё выполняется всегда — это и есть "регенерация
--    контроля" (снятие attached/rope-mode/shooting-rope флагов), поэтому
--    верёвку можно использовать снова сразу после реверса.
--  - activeWorm больше не сбрасывается на RetreatFinished — держим ссылку
--    живой (валидность всё равно перепроверяется через getAddr()), чтобы
--    Shift продолжал работать во время "повторов" (instant replay) и при
--    просмотре .WAgame реплеев; следующий TurnStarted всё равно перезапишет
--    активного червяка на нового игрока.
--  - detachRope больше не обнуляет unknown48 и не трогает 250/258 без верёвки:
--    unknown48 — флаг активной фазы оружия (CGameTaskPhysics проверяет +0x48);
--    обнуление при kamikaze/suicide → R6025 pure virtual call.

json = require "json"

local PACKAGE_SOUND_PATHS = {
  "jelly/packages/jellyreverse/2.0.0/use.wav",
  "jelly/packages/jellyreverse/2.0.0/reverse.wav",
  "jelly/packages/jellyreverse/2.0.0/sound.wav",
}

local CALIB = {
  worm_state_offset = 0x44,
  worm_state_ropeflight = nil,
  worm_facing_offset = 0x1A8,
}

local MSG = {
  RenderScene = 3,
  MoveLeft = 30, MoveRight = 31, MoveUp = 32, MoveDown = 33,
  FaceLeft = 34, FaceRight = 35,
  Jump = 36, JumpUp = 37,
  FireWeapon = 38, ReleaseWeapon = 39,
  StartTurn = 52, FinishTurn = 55, TurnStarted = 56, TurnFinished = 57,
  RetreatStarted = 60, RetreatFinished = 61,
  WormDamaged = 72,
}

local CLASS = {
  Mine = 8, Canister = 9, Team = 10, Missile = 11, Arrow = 12,
  Animation = 13, Crate = 15, Flame = 16, Worm = 18, OldWorm = 19,
  Drill = 20, Cross = 21, Smoke = 22, Cloud = 23, Fire = 24,
  Gass = 25, FireBall = 26, SeaBubble = 27, OilDrum = 30,
}

local CLASS_SKIP = {
  [0]=true, [1]=true, [2]=true, [3]=true, [4]=true, [5]=true, [6]=true,
  [7]=true, [CLASS.Team]=true, [10]=true,
}

local AFFECT_PROJ = {
  [CLASS.Missile]=true, [CLASS.OldWorm]=true, [CLASS.Arrow]=true,
  [CLASS.FireBall]=true, [CLASS.Flame]=true, [CLASS.Drill]=true,
}

local walib = require("walib")
local VK = {
  SHIFT = walib.VK_SHIFT,
  LEFT  = walib.VK_LEFT,
  RIGHT = walib.VK_RIGHT,
  UP    = walib.VK_UP,
  DOWN  = walib.VK_DOWN,
}

-- Constants::KeyCodes (W:A 3.8.x) — для isGameKeyDown без Win32 isKeyDown
local GAMEKEY = {
  SHIFT = 11,
  LEFT  = 6, RIGHT = 7, UP = 8, DOWN = 9,
}

local S = {
  mode = 1,
  maxPerTurn = 0,
  usesThisTurn = 0,
  acquire = false,
  mode3_projectiles = true,
  forceFlightState = false,
  soundId = nil,
  activeWorm = nil,
  activeAddr = 0,
  gen = 0,
  armedAxis = nil,
  armedFrames = 0,
  doneGen = {},
  shiftHeld = false,
  facingSaved = nil,
  lastAxisFired = nil,
  inRetreat = false,
  pendingRopeJump = false,
  pending = {},
  blastReverseFrames = 0,
  pollMs = -1,
  syncGen = 0,
  processedSync = {},
  _syncSilent = false,
}

local canPollVK = (type(isKeyDown) == "function")
local canPollGame = (type(isGameKeyDown) == "function")
local canPoll = canPollVK or canPollGame

local function pollKey(vk, gameKey)
  if canPollVK then return isKeyDown(vk) end
  if canPollGame then return isGameKeyDown(gameKey) end
  return false
end

local function isModeWormOnly() return S.mode == 1 or S.mode == 4 end

local function setStatus(msg) S._statusMsg = msg end
local function chat(msg)
  setStatus(msg)
  if S._syncSilent then return end
  if type(showChatMessage) == "function" then
    pcall(function() showChatMessage("[rev] " .. msg, 7) end)
  end
end

local function canBroadcastSync()
  return type(sendSyncChat) == "function"
end

local function markSyncProcessed(gen)
  if not gen or gen <= 0 then return end
  S.processedSync[gen] = true
end

local function isSyncAlreadyProcessed(gen)
  return gen and gen > 0 and S.processedSync[gen] == true
end

local function beginIncomingSync()
  if type(isIncomingSyncChat) ~= "function" or not isIncomingSyncChat() then
    return false
  end
  local gen = type(getIncomingSyncGen) == "function" and getIncomingSyncGen() or 0
  if isSyncAlreadyProcessed(gen) then
    return "dup"
  end
  markSyncProcessed(gen)
  S._syncSilent = true
  return true
end

local function endIncomingSync()
  S._syncSilent = false
end

-- cmdBody: "rev" или "rev l" (без ведущего /)
local function broadcastSync(cmdBody)
  if not canBroadcastSync() then return false end
  S.syncGen = S.syncGen + 1
  markSyncProcessed(S.syncGen)
  local payload = string.format("g%d:%s", S.syncGen, cmdBody)
  local ok = pcall(function() sendSyncChat(payload) end)
  return ok
end

local AXIS_LETTER = { xpos = "l", xneg = "r", ypos = "u", yneg = "d" }

local function axisSyncBody(axis)
  local letter = axis and AXIS_LETTER[axis]
  return letter and ("rev " .. letter) or "rev"
end
local function log(fmt, ...) io.write(string.format("[rev] " .. fmt .. "\n", ...)) end

local function safeAddr(obj)
  if obj == nil then return nil end
  local ok, addr = pcall(function() return obj:getAddr() end)
  return ok and addr or nil
end

local function isActiveWormValid()
  if S.activeWorm == nil or S.activeAddr == 0 then return false end
  local ok, addr = pcall(function() return S.activeWorm:getAddr() end)
  return ok and addr == S.activeAddr
end

local function isEnemyWorm(obj)
  if obj == nil or obj.classtype ~= CLASS.Worm then return false end
  local addr = safeAddr(obj)
  return addr and addr ~= S.activeAddr
end

local function isPhysicsObject(obj)
  if obj == nil or obj.speedX == nil then return false end
  local ct = obj.classtype
  if CLASS_SKIP[ct] then return false end
  if ct == CLASS.Worm or ct == CLASS.OldWorm then return true end
  if ct >= CLASS.Mine and ct <= CLASS.OilDrum then return true end
  return false
end

local function reverseObj(obj, axis)
  local vx, vy = obj.speedX, obj.speedY
  if axis == nil then
    obj.speedX, obj.speedY = -vx, -vy
  elseif axis == "xpos" then
    if vx > 0 then obj.speedX = -vx end
  elseif axis == "xneg" then
    if vx < 0 then obj.speedX = -vx end
  elseif axis == "ypos" then
    if vy > 0 then obj.speedY = -vy end
  elseif axis == "yneg" then
    if vy < 0 then obj.speedY = -vy end
  end
end

local function setWormField(obj, off, val)
  if off == 0x44 and obj.unknown44 ~= nil then
    obj.unknown44 = val
  elseif off == 0x1A4 and obj.unknown1A4 ~= nil then
    obj.unknown1A4 = val
  else
    obj:setOffset(off, val)
  end
end

-- CGameTask::unknown48 (+0x48): non-zero while a weapon/subtask owns the worm.
-- CGameTaskPhysics branches on it; zeroing during kamikaze corrupts the task → R6025.
local function wormInWeaponPhase(worm)
  if worm == nil or worm.classtype ~= CLASS.Worm then return false end
  local ok, v = pcall(function() return worm.unknown48 end)
  return ok and v ~= nil and v ~= 0
end

local function detachRope(obj)
  if obj.classtype ~= CLASS.Worm then return end
  pcall(function()
    local onRope = obj.unknownBC ~= nil and obj.unknownBC ~= 0
    if not onRope then return end
    obj.unknownBC = 0
    if obj.unknown258 ~= nil and obj.unknown258 ~= 0 then obj.unknown258 = 0 end
    if obj.unknown250 ~= nil and obj.unknown250 ~= 0 then obj.unknown250 = 0 end
  end)
end

local function setRopeFlight(obj)
  if obj.classtype ~= CLASS.Worm then return end
  if wormInWeaponPhase(obj) then return end
  -- detachRope снимает "прицеплен к верёвке" только когда unknownBC указывает на верёвку.
  detachRope(obj)
  if not S.forceFlightState then return end
  -- Принудительный перевод в состояние "полёт после верёвки" — опционально
  -- (по умолчанию выключено): именно это состояние на приземлении ведёт
  -- себя как "потеря контроля" (скольжение), даже если верёвка не юзалась.
  local val = CALIB.worm_state_ropeflight
  if val then
    setWormField(obj, CALIB.worm_state_offset or 0x44, val)
    if obj.unknown164 ~= nil then obj.unknown164 = 0 end
  end
end

local function applyReverse(obj, axis)
  if obj == nil then return end
  pcall(function()
    reverseObj(obj, axis)
    setRopeFlight(obj)
  end)
end

local function queueReverse(obj, axis, frames)
  local addr = safeAddr(obj)
  if not addr then return end
  S.pending[addr] = { obj = obj, axis = axis, frames = frames or 2 }
end

local function onWormSetState(worm, state)
  if worm == nil or S.activeAddr == 0 then return 0 end
  local ok, addr = pcall(function() return worm:getAddr() end)
  if ok and addr == S.activeAddr and S.pendingRopeJump then
    CALIB.worm_state_ropeflight = state
    S.pendingRopeJump = false
    log("auto-calibrated rope-jump state=%d", state)
  end
  return 0
end

local function trigger(axis, frames)
  S.gen = S.gen + 1
  S.armedAxis = axis
  S.armedFrames = frames or 30
  S.doneGen = {}
end

local function tryTrigger(axis, frames)
  if S.maxPerTurn > 0 and S.usesThisTurn >= S.maxPerTurn then
    chat(string.format("limit per turn: %d/%d", S.usesThisTurn, S.maxPerTurn))
    return false
  end
  trigger(axis, frames)
  if S.maxPerTurn > 0 then S.usesThisTurn = S.usesThisTurn + 1 end
  if isActiveWormValid() and S.soundId and type(playSoundLocal) == "function" then
    pcall(function() playSoundLocal(0x10000, 0x10000, S.activeWorm, S.soundId, 8) end)
  end
  return true
end

local function dispatchRevChat(cmd)
  if S._inChatRelay then return end
  S._inChatRelay = true
  onChatInput(cmd)
  S._inChatRelay = false
end

local function triggerReverse(axis, frames, skipBroadcast)
  if not skipBroadcast then
    broadcastSync(axisSyncBody(axis))
  end
  return tryTrigger(axis, frames or 30)
end

local function triggerWormOnlyReverse(skipBroadcast)
  if S.mode == 4 and not canPollVK and not canBroadcastSync() then
    dispatchRevChat("/rev")
  else
    triggerReverse(nil, 30, skipBroadcast)
  end
end

local function revByLetter(letter, silent)
  local m = { l = "xpos", r = "xneg", u = "ypos", d = "yneg" }
  local axis = m[letter or ""]
  if not axis then return false end
  if tryTrigger(axis, 30) then
    if not silent then chat("reverse! axis=" .. letter) end
    return true
  end
  return false
end

local function shouldAffect(obj)
  local ct = obj.classtype
  local addr = safeAddr(obj)
  if isModeWormOnly() then
    return ct == CLASS.Worm and addr == S.activeAddr
  elseif S.mode == 2 then
    return isPhysicsObject(obj)
  elseif S.mode == 3 then
    if ct == CLASS.Worm and addr == S.activeAddr then return true end
    if S.blastReverseFrames > 0 and isEnemyWorm(obj) then return true end
    return S.mode3_projectiles and AFFECT_PROJ[ct] or false
  else
    return false
  end
end

local function reverseEnemy(obj, axis)
  if not isEnemyWorm(obj) then return end
  applyReverse(obj, axis)
  queueReverse(obj, axis, 2)
end

local function onCreateExplosion(posx, posy, sender, pushForce, power, a6, team)
  if S.mode == 3 then
    S.blastReverseFrames = 4
  end
  if S.acquire and S.mode == 2 then
    if type(createExplosion) == "function" then
      createExplosion(sender, posx, posy, -pushForce, power, a6, team)
      return 1
    end
  end
  return 0
end

local function onSpecialImpact(This, posX, posY, radiusX, radiusY, power, pushX, pushY,
                               type_a9, soundOnHit, spriteOnHit, collisionMask, a13)
  if S.mode == 3 and isEnemyWorm(This) then
    reverseEnemy(This, nil)
  end
  if S.acquire and S.mode == 2 and This ~= nil and isPhysicsObject(This) then
    if type(createSpecialImpact) == "function" then
      createSpecialImpact(This, posX, posY, radiusX, radiusY, power,
                          -pushX, -pushY, type_a9, soundOnHit, spriteOnHit, collisionMask, a13)
      return 1
    end
  end
  return 0
end

local function processPending(object, addr)
  local p = S.pending[addr]
  if not p then return end
  if p.obj ~= object then return end
  applyReverse(object, p.axis)
  p.frames = p.frames - 1
  if p.frames <= 0 then S.pending[addr] = nil end
end

local function onPhysics(a1, object, a3)
  if object == nil then return 0 end
  local addr = safeAddr(object)
  if not addr then return 0 end

  processPending(object, addr)

  if S.armedFrames > 0 and S.doneGen[addr] ~= S.gen and shouldAffect(object) then
    S.doneGen[addr] = S.gen
    applyReverse(object, S.armedAxis)
  end
  return 0
end

local function getFacing(worm)
  if worm.unknown1A8 ~= nil then return worm.unknown1A8 end
  if CALIB.worm_facing_offset then
    return worm:getOffset(CALIB.worm_facing_offset)
  end
  return nil
end

local function setFacing(worm, val)
  if worm.unknown1A8 ~= nil then worm.unknown1A8 = val
  elseif CALIB.worm_facing_offset then worm:setOffset(CALIB.worm_facing_offset, val) end
end

local function pollShift()
  if not canPoll then return end
  if isModeWormOnly() and not isActiveWormValid() then return end
  if S.mode == 3 and not isActiveWormValid() then return end

  local shift = pollKey(VK.SHIFT, GAMEKEY.SHIFT)

  if S.mode == 3 then
    if shift and not S.shiftHeld then
      S.shiftHeld = true
      local f = getFacing(S.activeWorm)
      if f ~= nil then
        S.facingSaved = f
        setFacing(S.activeWorm, f == 0 and 1 or 0)
      end
    elseif shift and S.shiftHeld then
      local axis = nil
      if     isKeyDown(VK.LEFT)  then axis = "xpos"
      elseif isKeyDown(VK.RIGHT) then axis = "xneg"
      elseif isKeyDown(VK.UP)    then axis = "ypos"
      elseif isKeyDown(VK.DOWN)  then axis = "yneg" end
      if axis and axis ~= S.lastAxisFired then
        S.lastAxisFired = axis
        triggerReverse(axis, 30)
      elseif not axis then
        S.lastAxisFired = nil
      end
    elseif not shift and S.shiftHeld then
      S.shiftHeld = false
      if S.facingSaved ~= nil then
        setFacing(S.activeWorm, S.facingSaved)
        S.facingSaved = nil
      end
      S.lastAxisFired = nil
    end
  elseif isModeWormOnly() or S.mode == 2 then
    if shift and not S.shiftHeld then
      S.shiftHeld = true
      if isModeWormOnly() then
        triggerWormOnlyReverse()
      else
        triggerReverse(nil, 30)
      end
    elseif not shift and S.shiftHeld then
      S.shiftHeld = false
    end
  end
end

local function tickFrame()
  local now = type(getTickCount) == "function" and getTickCount() or 0
  if now == S.pollMs and now ~= 0 then return end
  S.pollMs = now
  if S.armedFrames > 0 then S.armedFrames = S.armedFrames - 1 end
  if S.blastReverseFrames > 0 then S.blastReverseFrames = S.blastReverseFrames - 1 end
  pollShift()
end

local function clearActiveWorm()
  S.activeWorm, S.activeAddr = nil, 0
  S.shiftHeld, S.facingSaved, S.lastAxisFired = false, nil, nil
  S.inRetreat = false
  S.pendingRopeJump = false
end

local function markRetreatPhase() S.inRetreat = true end

local function wormcallback(This, sender, messagetype, psize, params)
  if messagetype >= MSG.MoveLeft and messagetype <= MSG.ReleaseWeapon then
    S.activeWorm = This
    S.activeAddr = This:getAddr()
  end
  if messagetype == MSG.Jump or messagetype == MSG.JumpUp then
    if This.unknownBC ~= nil and This.unknownBC ~= 0 then
      S.pendingRopeJump = true
    end
  end
  if messagetype == MSG.WormDamaged and S.mode == 3 and isEnemyWorm(This) then
    reverseEnemy(This, nil)
  end
  if messagetype == MSG.RenderScene then
    tickFrame()
  elseif messagetype == MSG.StartTurn or messagetype == MSG.TurnStarted then
    S.activeWorm = This
    S.activeAddr = This:getAddr()
    S.inRetreat = false
    S.shiftHeld = canPoll and pollKey(VK.SHIFT, GAMEKEY.SHIFT) or false
    S.facingSaved = nil
    S.lastAxisFired = nil
    S.usesThisTurn = 0
    S.pending = {}
  elseif messagetype == MSG.FinishTurn or messagetype == MSG.TurnFinished then
    if isModeWormOnly() or S.mode == 3 then
      markRetreatPhase()
      if S.activeAddr == 0 then
        S.activeWorm = This
        S.activeAddr = This:getAddr()
      end
    else
      clearActiveWorm()
      S.usesThisTurn = 0
    end
  elseif messagetype == MSG.RetreatStarted then
    markRetreatPhase()
    S.activeWorm = This
    S.activeAddr = This:getAddr()
  elseif messagetype == MSG.RetreatFinished then
    S.inRetreat = false
    -- Не сбрасываем activeWorm здесь: ход уже закончен, но игра ещё может
    -- крутить "повтор" (instant replay) этого же червяка — Shift должен
    -- продолжать работать. isActiveWormValid() сам вернёт false, как
    -- только объект реально станет невалидным; следующий TurnStarted
    -- всё равно переключит активного червяка на нового игрока.
    S.usesThisTurn = 0
  end
  return 0
end

local function normalizeRevCommand(msg)
  if not msg then return msg end
  msg = msg:gsub("^/(rev|reversal)(%d)", "/%1 %2")
  msg = msg:gsub("^/(rev|reversal)(%d)/", "/%1 %2/")
  return msg
end

local function parseReversal(msg)
  local mode, maxu = msg:match("^/reversal%s+(%d)/(%d+)%s*$")
  if not mode then mode, maxu = msg:match("^/reversal(%d)/(%d+)%s*$") end
  if not mode then return nil end
  mode, maxu = tonumber(mode), tonumber(maxu)
  if mode < 1 or mode > 4 or maxu < 0 then return nil end
  return mode, maxu
end

local function parseRevShortcut(msg)
  local mode, maxu = msg:match("^/rev%s+(%d)/(%d+)%s*$")
  if not mode then mode, maxu = msg:match("^/rev(%d)/(%d+)%s*$") end
  if not mode then mode, maxu = msg:match("^/rev%s+(%d+)%s+(%d+)%s*$") end
  if mode then
    mode, maxu = tonumber(mode), tonumber(maxu)
    if mode >= 1 and mode <= 4 and maxu >= 0 then return mode, maxu end
    return nil
  end
  local onlyMode = msg:match("^/rev%s+(%d+)%s*$") or msg:match("^/reversal%s+(%d+)%s*$")
  if onlyMode then
    mode = tonumber(onlyMode)
    if mode >= 1 and mode <= 4 then return mode, 0 end
  end
  return nil
end

local MODE_LABELS = {
  [1] = "worm only (+retreat)",
  [2] = "all physics objects",
  [3] = "self shift+arrows, enemies on hit",
  [4] = "worm only (+retreat), chat fallback",
}

function getStatusMessage()
  local m = S._statusMsg
  S._statusMsg = nil
  return m or ""
end

function getSettingsSummary()
  local lim = S.maxPerTurn > 0 and tostring(S.maxPerTurn) or "no limit"
  local poll = canPollVK and "vk" or (canPollGame and "game" or "manual /rev")
  local sync = canBroadcastSync() and "hidden-chat" or "local"
  local extra = (S.mode == 4 and not canPoll) and " (type /rev in chat)" or ""
  return string.format("mode=%d (%s) max/turn=%s acquire=%s flight=%s poll=%s sync=%s%s",
    S.mode, MODE_LABELS[S.mode] or "?", lim, S.acquire and "on" or "off",
    S.forceFlightState and "on" or "off", poll, sync, extra)
end

local function setupPackageSound()
  if type(registerCustomSound) ~= "function" then return end
  for _, path in ipairs(PACKAGE_SOUND_PATHS) do
    local ok, id = pcall(function() return registerCustomSound(path) end)
    if ok and type(id) == "number" and id > 0 then
      S.soundId = id
      return
    end
  end
end

function onChatInput(msg)
  if not msg or msg == "" then return 0 end
  if msg:sub(1, 1) ~= "/" then msg = "/" .. msg end
  msg = normalizeRevCommand(msg)

  local syncState = beginIncomingSync()
  if syncState == "dup" then return 1 end

  local mode, maxu = parseReversal(msg)
  if not mode then mode, maxu = parseRevShortcut(msg) end
  if mode then
    S.mode = mode
    S.maxPerTurn = maxu
    S.usesThisTurn = 0
    local lim = maxu > 0 and tostring(maxu) or "no limit"
    chat(string.format("mode %d: %s, max/turn=%s", mode, MODE_LABELS[mode] or "?", lim))
    if mode == 4 and not canPoll then
      chat("no key poll — type /rev in chat for full reverse")
    end
    endIncomingSync()
    return 1
  end

  local args = {}
  for a in msg:gmatch("%S+") do args[#args+1] = a end

  if args[1] == "/rev" then
    local axis = args[2]
    local fromSync = syncState == true
    if axis and tonumber(axis) then
      chat("hint: /rev <mode> or /rev l|r|u|d")
      endIncomingSync()
      return 1
    end
    if axis and axis ~= "" then
      if not fromSync then broadcastSync("rev " .. axis) end
      revByLetter(axis, fromSync)
    else
      if not fromSync then broadcastSync("rev") end
      if triggerReverse(nil, 30, true) and not fromSync then
        chat("reverse!")
      end
    end
    endIncomingSync()
    return 1
  end

  if args[1] ~= "/jr" then
    endIncomingSync()
    return 0
  end

  local c = args[2]
  if c == "mode" and tonumber(args[3]) then
    S.mode = tonumber(args[3])
    chat("mode = " .. S.mode)
  elseif c == "acquire" and tonumber(args[3]) then
    S.acquire = (tonumber(args[3]) ~= 0)
    chat("acquire = " .. (S.acquire and "on" or "off"))
  elseif c == "flight" and tonumber(args[3]) then
    S.forceFlightState = (tonumber(args[3]) ~= 0)
    chat("flight state hack = " .. (S.forceFlightState and "on (может скользить при посадке)" or "off"))
  elseif c == "rev" then
    revByLetter(args[3] or "", false)
  else
    chat("/reversal <mode>/<max>, /rev, /jr mode|acquire|flight|rev")
  end
  endIncomingSync()
  return 1
end

function writeConfig()
  return json.encode({
    mode = S.mode, maxPerTurn = S.maxPerTurn, acquire = S.acquire,
    forceFlightState = S.forceFlightState, calib = CALIB,
  })
end

function readConfig(config)
  local ok, cfg = pcall(json.decode, config)
  if ok and type(cfg) == "table" then
    if cfg.mode ~= nil then S.mode = cfg.mode end
    if cfg.maxPerTurn ~= nil then S.maxPerTurn = cfg.maxPerTurn end
    if cfg.acquire ~= nil then S.acquire = cfg.acquire end
    if cfg.forceFlightState ~= nil then S.forceFlightState = cfg.forceFlightState end
    if type(cfg.calib) == "table" then
      for k, v in pairs(cfg.calib) do CALIB[k] = v end
    end
  end
  return 0
end

function resetConfig()
  S.mode = 1
  S.maxPerTurn = 0
  S.acquire = false
  S.forceFlightState = false
  return 0
end

function lobbyHostCli(lobbyObj, command, args) return 0 end
function lobbyClientCli(lobbyObj, command, args) return 0 end

function initialize()
  local ok, err = pcall(function()
    CTaskWorm_RegisterCallback_vtable8(wormcallback)
    CTaskWorm_RegisterCallback_vtable38(onWormSetState)
    registerCallback_handleCGameTaskPhysics(onPhysics)
    registerCallback_handleCreateExplosion(onCreateExplosion)
    registerCallback_handleSpecialImpact(onSpecialImpact)
    setupPackageSound()
  end)
  if not ok then
    chat("init error: " .. tostring(err))
    return 1
  end
  local pollSrc = canPollVK and "isKeyDown" or (canPollGame and "isGameKeyDown" or "none")
  local syncSrc = canBroadcastSync() and "sendSyncChat" or "none"
  -- Хуки (vtable8/physics/RenderScene) не отключаются на "повторах" и при
  -- просмотре .WAgame — Shift-поллинг работает там так же, как вживую.
  local replayNote = ""
  if type(isReplayPlayback) == "function" then
    local ok2, rp = pcall(isReplayPlayback)
    if ok2 and rp then replayNote = " [replay]" end
  end
  log("jellyreverse v2.0.0 ready mode=%d poll=%s sync=%s%s", S.mode, pollSrc, syncSrc, replayNote)
  return 0
end
