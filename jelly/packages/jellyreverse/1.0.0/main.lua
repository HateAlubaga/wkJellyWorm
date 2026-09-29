-- ============================================================
--  jellyreverse — реальный мод для wkJellyWorm (W:A 3.8.x)
--  Только настоящее API из исходников nizikawa-worms/wkJellyWorm.
--
--  Установка: /enable jellyreverse  (пакет в jelly/packages/jellyreverse/1.0.0/)
--
--  Фичи:
--   1) Реверс скорости объектов + перевод червяка в "полётное"
--      состояние (как после верёвки) — оффсет состояния калибруется.
--   2) Режимы реверса:
--        mode 1 — только активный червяк
--        mode 2 — все физические объекты (черви + снаряды и т.п.)
--        mode 3 — Shift-режим: пока зажат Shift червяк смотрит в
--                 другую сторону; отпустил без стрелок — обычный
--                 реверс скорости; Shift+стрелка — реверс только
--                 одной компоненты (влево = гасит положительный X).
--                 Работает и по снарядам (см. mode3_projectiles).
--   3) Два типа реген-задержки оружия (значения берутся из схемы,
--      т.е. из таблицы команды, которую игра заполняет схемой):
--        regen 1 — после КАЖДОГО использования задержка
--                  восстанавливается, боезапас не трогаем
--        regen 2 — после полного израсходования боезапаса
--                  восстанавливаются И задержка, И боезапас
--
--  ЧЕСТНОЕ ЗАМЕЧАНИЕ: wkJellyWorm не именует поля червяка/команды —
--  всё, что ниже в CALIB, это оффсеты, которые надо один раз найти
--  на своей сборке встроенными командами /jr scan и /jr team.
--  Без калибровки мод работает, но: реверс не включает полётное
--  состояние, а реген-логика работает в "теневом" режиме (пишет в
--  чат, что бы она сделала, не трогая память).
-- ============================================================

json = require "json"

-- ---------------- CALIB: калибруемые оффсеты ----------------
local CALIB = {
  -- CTaskWorm: dword-оффсет поля состояния червяка (nil = не откалибровано)
  worm_state_offset = nil,      -- пример: 0x1A4
  worm_state_ropeflight = nil,  -- значение состояния "полёт с верёвки"
  worm_facing_offset = nil,     -- поле направления взгляда (0/1)

  -- CTaskTeam: базы массивов по оружию (dword на слот оружия)
  team_ammo_offset  = nil,      -- база массива боезапаса
  team_delay_offset = nil,      -- база массива задержек
  weapon_slots      = 64,       -- сколько слотов читать при снапшоте
}

-- ---------------- Константы (из Constants.h) ----------------
local MSG = {
  RenderScene = 3,
  MoveLeft = 30, MoveRight = 31, MoveUp = 32, MoveDown = 33,
  FaceLeft = 34, FaceRight = 35,
  Jump = 36, JumpUp = 37,
  FireWeapon = 38, ReleaseWeapon = 39,
  StartTurn = 52, TurnStarted = 56, TurnFinished = 57,
  RetreatStarted = 60,
}

local CLASS = { -- enum ClassType
  Mine = 8, Canister = 9, Team = 10, Missile = 11, Arrow = 12,
  Crate = 15, Flame = 16, Worm = 18, OldWorm = 19,
  FireBall = 26, OilDrum = 30,
}

-- какие классы считаем "объектами" для mode 2
local AFFECT_ALL = {
  [CLASS.Worm]=true, [CLASS.OldWorm]=true, [CLASS.Missile]=true,
  [CLASS.Arrow]=true, [CLASS.Mine]=true, [CLASS.Canister]=true,
  [CLASS.Crate]=true, [CLASS.Flame]=true, [CLASS.FireBall]=true,
  [CLASS.OilDrum]=true,
}
-- "любой выпущенный снаряд" для mode 3
local AFFECT_PROJ = {
  [CLASS.Missile]=true, [CLASS.OldWorm]=true, [CLASS.Arrow]=true,
  [CLASS.FireBall]=true, [CLASS.Flame]=true,
}

-- Win32 VK-коды для isKeyDown (GetAsyncKeyState). Не DIK/scancode.
-- Опрос каждый кадр в RenderScene: видим и зажатие, и отпускание (edge по shiftHeld).
local walib = require("walib")
local VK = {
  SHIFT = walib.VK_SHIFT,
  LEFT  = walib.VK_LEFT,
  RIGHT = walib.VK_RIGHT,
  UP    = walib.VK_UP,
  DOWN  = walib.VK_DOWN,
}

-- ---------------- Состояние мода ----------------
local S = {
  mode = 1,            -- 1 | 2 | 3
  regen = 0,           -- 0=выкл, 1, 2
  mode3_projectiles = true,

  activeWorm = nil,    -- CTaskWorm активного червяка (по входным сообщениям)
  activeAddr = 0,

  -- триггер реверса: "поколение" + счётчик кадров, за которые он живёт
  gen = 0,
  armedAxis = nil,     -- nil = обе оси; "xpos","xneg","ypos","yneg"
  armedFrames = 0,     -- сколько кадров триггер активен (>0 = активен)
  doneGen = {},        -- addr -> gen (кому уже реверснули в этом поколении)

  -- mode 3 (только с isKeyDown)
  shiftHeld = false,
  facingSaved = nil,

  -- реген
  teams = {},          -- addr -> {task=CTaskTeam, ammo0={}, delay0={}, snap=false}
  shadowAmmo = {},     -- теневое: teamAddr -> {[wpn]=осталось}
}

local canPoll = (type(isKeyDown) == "function") -- нет в стоковом wkJellyWorm

local function chat(msg) showChatMessage("[jr] " .. msg, 7) end
local function log(fmt, ...) io.write(string.format("[jr] " .. fmt .. "\n", ...)) end

-- ---------------- Реверс ----------------
-- скорости в fixed-point 16.16 (пример из исходников: speedY = -50*65536)
local function reverseObj(obj, axis)
  local vx, vy = obj.speedX, obj.speedY
  if axis == nil then
    obj.speedX, obj.speedY = -vx, -vy
  elseif axis == "xpos" then      -- стрелка ВЛЕВО: гасим движение вправо
    if vx > 0 then obj.speedX = -vx end
  elseif axis == "xneg" then      -- стрелка ВПРАВО: гасим движение влево
    if vx < 0 then obj.speedX = -vx end
  elseif axis == "ypos" then      -- стрелка ВВЕРХ: гасим падение (Y растёт вниз)
    if vy > 0 then obj.speedY = -vy end
  elseif axis == "yneg" then      -- стрелка ВНИЗ: гасим взлёт
    if vy < 0 then obj.speedY = -vy end
  end
end

local function setRopeFlight(obj)
  if obj.classtype ~= CLASS.Worm then return end
  if not (CALIB.worm_state_offset and CALIB.worm_state_ropeflight) then return end
  obj:setOffset(CALIB.worm_state_offset, CALIB.worm_state_ropeflight)
end

local function trigger(axis, frames)
  S.gen = S.gen + 1
  S.armedAxis = axis
  S.armedFrames = frames or 2   -- живёт 2 кадра — каждый объект успеет попасть в physics
  S.doneGen = {}
end

local function shouldAffect(obj)
  local ct = obj.classtype
  if S.mode == 1 then
    return ct == CLASS.Worm and obj:getAddr() == S.activeAddr
  elseif S.mode == 2 then
    return AFFECT_ALL[ct] or false
  else -- mode 3
    if ct == CLASS.Worm and obj:getAddr() == S.activeAddr then return true end
    return S.mode3_projectiles and AFFECT_PROJ[ct] or false
  end
end

-- ---------------- Физический тик (реальный колбэк) ----------------
-- registerCallback_handleCGameTaskPhysics: (a1, object CGameTask, a3),
-- вызывается для каждого объекта каждый тик; return 0 = не глотать.
local function onPhysics(a1, object, a3)
  if S.armedFrames > 0 and object ~= nil then
    local ok, addr = pcall(function() return object:getAddr() end)
    if ok and S.doneGen[addr] ~= S.gen and shouldAffect(object) then
      S.doneGen[addr] = S.gen
      reverseObj(object, S.armedAxis)
      setRopeFlight(object)
    end
  end
  return 0
end

-- ---------------- Mode 3: опрос клавиш (если биндинг есть) ----------------
local function isActiveWormValid()
  if S.activeWorm == nil or S.activeAddr == 0 then return false end
  local ok, addr = pcall(function() return S.activeWorm:getAddr() end)
  return ok and addr == S.activeAddr
end

local function clearActiveWorm()
  S.activeWorm, S.activeAddr = nil, 0
  S.shiftHeld, S.facingSaved, S.lastAxisFired = false, nil, nil
end

local function pollMode3()
  if not canPoll or S.mode ~= 3 or not isActiveWormValid() then return end
  local shift = isKeyDown(VK.SHIFT)

  if shift and not S.shiftHeld then
    S.shiftHeld = true
    -- червяк "смотрит в другую сторону", пока Shift зажат
    if CALIB.worm_facing_offset then
      local f = S.activeWorm:getOffset(CALIB.worm_facing_offset)
      S.facingSaved = f
      S.activeWorm:setOffset(CALIB.worm_facing_offset, f == 0 and 1 or 0)
    end
  elseif shift and S.shiftHeld then
    -- Shift+стрелка = осевой реверс (одноразово на нажатие оси)
    local axis = nil
    if     isKeyDown(VK.LEFT)  then axis = "xpos"
    elseif isKeyDown(VK.RIGHT) then axis = "xneg"
    elseif isKeyDown(VK.UP)    then axis = "ypos"
    elseif isKeyDown(VK.DOWN)  then axis = "yneg" end
    if axis and axis ~= S.lastAxisFired then
      S.lastAxisFired = axis
      trigger(axis)
    elseif not axis then
      S.lastAxisFired = nil
    end
  elseif not shift and S.shiftHeld then
    S.shiftHeld = false
    -- вернуть взгляд
    if CALIB.worm_facing_offset and S.facingSaved ~= nil then
      S.activeWorm:setOffset(CALIB.worm_facing_offset, S.facingSaved)
      S.facingSaved = nil
    end
    -- отпустил без стрелок — обычный полный реверс
    if S.lastAxisFired == nil then trigger(nil) end
    S.lastAxisFired = nil
  end
end

-- ---------------- Колбэк червяка (vtable8, реальная сигнатура) ----------------
local function wormcallback(This, sender, messagetype, psize, params)
  -- любое входное сообщение управления = этот червяк активен
  if messagetype >= MSG.MoveLeft and messagetype <= MSG.ReleaseWeapon then
    S.activeWorm = This
    S.activeAddr = This:getAddr()
  end

  if messagetype == MSG.RenderScene then
    -- кадр: гасим триггер и опрашиваем клавиши (по одному разу за кадр —
    -- только на активном червяке)
    if This:getAddr() == S.activeAddr then
      if S.armedFrames > 0 then S.armedFrames = S.armedFrames - 1 end
      pollMode3()
    end
  elseif messagetype == MSG.TurnStarted or messagetype == MSG.TurnFinished then
    clearActiveWorm()
    if messagetype == MSG.TurnStarted then snapshotTeams() end
  end
  return 0
end

-- ---------------- Реген задержек ----------------
-- Атрибуция стрелявшей команды: registerCallback_fireWeapon даёт червяка,
-- но связь червяк->команда не именована. Реальный способ: после выстрела
-- у команды стрелявшего меняется lastLaunchedWeapon_dword60 — ловим
-- именно это изменение в teamcallback, а fireWeapon лишь "взводит" флаг.
local function applyRegen(addr, t, wpn)
  if wpn == nil or wpn < 0 then return end
  local calibrated = CALIB.team_ammo_offset and CALIB.team_delay_offset and t.snap

  if S.regen == 1 then
    -- тип 1: задержка восстанавливается после каждого использования
    if calibrated then
      t.task:setOffset(CALIB.team_delay_offset + wpn * 4, t.delay0[wpn] or 0)
    else
      chat(string.format("regen1 (тень): сбросил бы задержку оружия %d", wpn))
    end
  elseif S.regen == 2 then
    -- тип 2: когда боезапас кончился — вернуть и задержку, и боезапас
    if calibrated then
      local ammo = t.task:getOffset(CALIB.team_ammo_offset + wpn * 4)
      if ammo == 0 then
        t.task:setOffset(CALIB.team_ammo_offset  + wpn * 4, t.ammo0[wpn] or 0)
        t.task:setOffset(CALIB.team_delay_offset + wpn * 4, t.delay0[wpn] or 0)
        chat(string.format("regen2: оружие %d восстановлено (ammo=%d)", wpn, t.ammo0[wpn] or 0))
      end
    else
      local sa = S.shadowAmmo[addr] or {}
      S.shadowAmmo[addr] = sa
      sa[wpn] = (sa[wpn] or 3) - 1
      if sa[wpn] <= 0 then
        sa[wpn] = 3
        chat(string.format("regen2 (тень): оружие %d восстановил бы", wpn))
      end
    end
  end
end

-- Команды собираем реальным способом: CTaskTeam тоже получает сообщения,
-- регистрируемся на его vtable8 и складываем объекты в реестр.
local function teamcallback(This, sender, messagetype, psize, params)
  local addr = This:getAddr()
  local t = S.teams[addr]
  if t == nil then
    t = { task = This, snap = false, ammo0 = {}, delay0 = {},
          prevWpn = This.lastLaunchedWeapon_dword60 }
    S.teams[addr] = t
    log("team registered addr=%X number=%d", addr, This.team_number_dword38)
  end

  -- изменение lastLaunchedWeapon после взведённого fireWeapon =
  -- именно ЭТА команда стреляла
  local cur = This.lastLaunchedWeapon_dword60
  if cur ~= t.prevWpn then
    if S.fireArmed and S.regen ~= 0 then
      S.fireArmed = false
      applyRegen(addr, t, cur)
    end
    t.prevWpn = cur
  end
  return 0
end

-- Снапшот "значений из схемы": игра заполняет таблицы команды из схемы,
-- читаем их на первом ходу (нужны откалиброванные оффсеты).
function snapshotTeams()
  if not (CALIB.team_ammo_offset and CALIB.team_delay_offset) then return end
  for addr, t in pairs(S.teams) do
    if not t.snap then
      for w = 0, CALIB.weapon_slots - 1 do
        t.ammo0[w]  = t.task:getOffset(CALIB.team_ammo_offset  + w * 4)
        t.delay0[w] = t.task:getOffset(CALIB.team_delay_offset + w * 4)
      end
      t.snap = true
      log("team %X: scheme snapshot taken", addr)
    end
  end
end

-- Реальный колбэк выстрела: registerCallback_fireWeapon.
-- Только взводит флаг — атрибуция команды делается в teamcallback по
-- изменению lastLaunchedWeapon_dword60 у команды стрелявшего.
local function onFireWeapon(worm, weaponStruct, launchParams)
  if S.regen ~= 0 then S.fireArmed = true end
  return 0
end

-- ---------------- Калибровочные команды ----------------
-- /jr scan <hexoff> <count>  — дамп dword-полей активного червяка
--   метод: встань на верёвку/слезь и сравни дампы — поле состояния
--   меняется. Найденное: /jr set state <off> <val>
-- /jr team <hexoff> <count>  — то же для первой команды (ищем массивы
--   боезапаса: значения совпадут с настройками схемы)
local function hex(s) return tonumber(s, 16) end

local function cmdScan(obj, off, cnt, label)
  if obj == nil then chat("нет объекта (сначала подвигай червяком)") return end
  off, cnt = off or 0x30, math.min(cnt or 32, 128)
  for i = 0, cnt - 1 do
    local o = off + i * 4
    log("%s +0x%X = %d (0x%X)", label, o, obj:getOffset(o), obj:getOffset(o))
  end
  chat(string.format("%s: дамп 0x%X..0x%X в консоли", label, off, off + (cnt-1)*4))
end

function onChatInput(msg)
  local args = {}
  for a in msg:gmatch("%S+") do args[#args+1] = a end
  if args[1] ~= "/jr" then return 0 end

  local c = args[2]
  if c == "mode" and tonumber(args[3]) then
    S.mode = tonumber(args[3])
    chat("mode = " .. S.mode .. (S.mode == 3 and not canPoll
      and " (isKeyDown нет — Shift-режим недоступен, юзай /jr rev [l|r|u|d])" or ""))
  elseif c == "regen" and tonumber(args[3]) then
    S.regen = tonumber(args[3])
    chat("regen = " .. S.regen)
  elseif c == "rev" then
    -- ручной триггер (и единственный способ осевого реверса без isKeyDown)
    local m = { l = "xpos", r = "xneg", u = "ypos", d = "yneg" }
    trigger(m[args[3] or ""], 2)
    chat("reverse!" .. (args[3] and (" axis=" .. args[3]) or ""))
  elseif c == "scan" then
    cmdScan(S.activeWorm, hex(args[3] or "30"), tonumber(args[4]), "worm")
  elseif c == "team" then
    local first = next(S.teams)
    cmdScan(first and S.teams[first].task or nil, hex(args[3] or "30"), tonumber(args[4]), "team")
  elseif c == "set" then
    -- /jr set state <off> <val> | facing <off> | ammo <off> | delay <off>
    local what, a, b = args[3], hex(args[4] or ""), tonumber(args[5])
    if what == "state" and a and b then
      CALIB.worm_state_offset, CALIB.worm_state_ropeflight = a, b
      chat(string.format("state: +0x%X = %d", a, b))
    elseif what == "facing" and a then
      CALIB.worm_facing_offset = a; chat(string.format("facing: +0x%X", a))
    elseif what == "ammo" and a then
      CALIB.team_ammo_offset = a; chat(string.format("ammo base: +0x%X", a))
    elseif what == "delay" and a then
      CALIB.team_delay_offset = a; chat(string.format("delay base: +0x%X", a))
    else
      chat("сохранение: /jr set state|facing|ammo|delay <hexoff> [val]")
    end
  else
    chat("/jr mode 1|2|3, regen 0|1|2, rev [l|r|u|d], scan, team, set")
  end
  return 1 -- сообщение съедено, в чат не уходит
end

-- ---------------- Конфиг пакета (реальный механизм JellyWorm) ----------------
function writeConfig()
  return json.encode({ mode = S.mode, regen = S.regen, calib = CALIB })
end

function readConfig(config)
  local ok, cfg = pcall(json.decode, config)
  if ok and type(cfg) == "table" then
    if cfg.mode  ~= nil then S.mode  = cfg.mode  end
    if cfg.regen ~= nil then S.regen = cfg.regen end
    if type(cfg.calib) == "table" then
      for k, v in pairs(cfg.calib) do CALIB[k] = v end
    end
  end
  return 0
end

function resetConfig() return 0 end

function lobbyHostCli(lobbyObj, command, args) return 0 end
function lobbyClientCli(lobbyObj, command, args) return 0 end

-- ---------------- Инициализация ----------------
function initialize()
  CTaskWorm_RegisterCallback_vtable8(wormcallback)
  CTaskTeam_RegisterCallback_vtable8(teamcallback)
  registerCallback_handleCGameTaskPhysics(onPhysics)
  registerCallback_fireWeapon(onFireWeapon)

  log("jellyreverse loaded. isKeyDown=%s", tostring(canPoll))
  return 0
end
