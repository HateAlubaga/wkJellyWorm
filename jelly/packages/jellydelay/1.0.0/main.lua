-- jellydelay v1.0.0 — regen / rate / down (отдельный пакет)
-- /enable jellydelay в лобби
--
-- БЫСТРЫЙ СТАРТ (хост):
--   1) /enable jellydelay
--   2) /delay rt                   (или /delay для regen1)
--   На 1-м ходу: ammo из памяти матча → авто-поиск .wsc в User/Schemes по ammo-сигнатуре
--   → delay из найденного файла (точные с 1-го выстрела). Ручной /delay scheme не обязателен.
--   Опционально: /delay scheme <file> — форс-override + sync клиентам в лобби.
--
-- regen baseline (порядок):
--   1) хост: /delay scheme <path.wsc> в лобби → таблицы в wkJellyWormConfigStr (онлайн)
--   2) клиент: readConfig получает schemeAmmo/schemeDelay от хоста
--   3) авто: memory ammo + индекс .wsc (User/Schemes) → delay из файла (source=auto)
--   4) fallback: только память матча (delay self-learn после 1-го выстрела)
--   5) опционально /delay scheme — ручной override с диска
--
--   /delay              — regen 1: сброс delay после выстрела
--   /delay rt           — regen 2: ammo+delay при ammo=0
--   /delay scheme <path>— загрузить .wsc (SCHM) поверх memory baseline
--   /delay schemes      — список .wsc из индекса User/Schemes
--   /delay off          — выкл regen
--
-- rate (напрямую в память, не схема):
--   /delay rate      — −1 ко всем delay каждые 1 сек (по умолчанию)
--   /delay rate 5    — −1 каждые 5 сек
--   /delay rate off  — выкл
--
-- down (напрямую на старте матча, не схема):
--   /delay down      — один раз −10 (по умолчанию)
--   /delay down N    — один раз −N
--   /delay down off  — выкл

json = require "json"

local WEAPON_SLOTS = 128
-- Panel / scheme-memory IDs (WKB .wsc order + 1; Weapon_None=0). Not Constants.h enum order.
-- scheme index 0=Bazooka → game 1; index 3=Grenade → game 4.
local WEAPON_HINTS = {
  [1] = "Bazooka", [2] = "HomingMissile", [3] = "Mortar", [4] = "Grenade",
  [14] = "Airstrike", [15] = "Napalm", [25] = "NinjaRope", [31] = "Mine",
}
local WSC_MAGIC = "SCHM"
local WSC_WEAPONS_OFF = 0x29
local WSC_SUPER_OFF = 0xDD
local WSC_STD_COUNT = 45
local WSC_SUPER_COUNT = 19
-- Standard panel weapon ids (Weapon_None=0 .. custom). Match-memory capture range.
local MATCH_SCHEME_WPN_MAX = 71
local CLASS = { Team = 10 }
local MSG = {
  RenderScene = 3,
  MoveLeft = 30, MoveRight = 31, MoveUp = 32, MoveDown = 33,
  FaceLeft = 34, FaceRight = 35,
  Jump = 36, JumpUp = 37,
  FireWeapon = 38, ReleaseWeapon = 39,
  StartTurn = 52,
  TurnStarted = 56,
  TurnFinished = 57,
  WeaponFinished = 118,
}

local S = {
  regen = 0,
  delayRateSec = 0,
  delayDown = 0,
  teams = {},
  fireArmed = false,
  pendingWpn = nil,
  pendingTeam = nil,
  pendingRegen = nil,
  apiOk = false,
  lastRateTick = 0,
  discoverTick = 0,
  regenDebug = false, -- verbose per-shot chat log; off by default, /delay debug on to enable
  activeWorm = nil,
  activeAddr = 0,
  schemeReload = {},
  shotProbe = nil,
  -- Baseline: match-memory (primary) or .wsc file (manual). Per-team TurnStarted snap = probe only.
  schemeAmmo = {},
  schemeDelay = {},
  schemePath = nil,
  schemeLoaded = false,
  schemeVersion = 0,
  schemeSource = nil,       -- "memory" | "sync" | "file" | "auto"
  schemePendingMemory = false,
  schemeMemorySnapshot = nil, -- ammo/delay from match-memory before manual .wsc override
  schemeAutoDetectDone = false,
  -- Cached parse of all User/Schemes/*.wsc (rebuilt when dir fingerprint changes).
  schemeIndex = { fileListKey = nil, entries = {} },
  -- RenderScene/StartTurn callbacks fire once PER WORM (broadcast), not once per frame/turn —
  -- dedupe by tick so per-frame/per-turn work runs exactly once regardless of worm count.
  lastRenderTick = nil,
  schemeCaptureFirstTick = nil,
  schemeCaptureAttempts = 0,
}

local SCHEME_CAPTURE_GIVEUP_MS = 20000  -- stop polling match-memory after 20s of real turns
local SCHEME_CAPTURE_GIVEUP_ATTEMPTS = 60

local REGEN_WAIT_FRAMES = 2
local REGEN_RETRY_FRAMES = 120
local SHOT_PROBE_FRAMES = 120

local function weaponHint(wpn)
  return WEAPON_HINTS[wpn] and (" (" .. WEAPON_HINTS[wpn] .. ")") or ""
end

local function lastShotWeaponFromCounters()
  if type(getCustomWeaponsDebugCounters) ~= "function" then return nil end
  local ok, c = pcall(getCustomWeaponsDebugCounters)
  if not ok or not c then return nil end
  local wpn = c:match("lastWeaponShot=([%-%d]+)")
  wpn = wpn and tonumber(wpn) or nil
  if wpn ~= nil and wpn >= 1 then return wpn end
  return nil
end
local function setStatus(msg) S._statusMsg = msg end

-- File log survives chat truncation; DevConsole also shows stdout when EnableDevConsole=1.
local function log(fmt, ...)
  local line = string.format("[delay] " .. fmt, ...)
  pcall(function() io.write(line .. "\n") end)
  pcall(function()
    local f = io.open("jelly/packages/jellydelay/1.0.0/delay.log", "a")
    if f then
      f:write(line .. "\n")
      f:close()
    end
  end)
end

local function chat(msg)
  setStatus(msg)
  log("%s", msg)
  if type(showChatMessage) == "function" then
    pcall(function() showChatMessage("[delay] " .. msg, 7) end)
  end
end

-- .wsc scheme index → in-game / panel weapon id (Weapon_None=0, Bazooka=1).
local function schemeIndexToGameId(schemeIndex)
  if schemeIndex == nil or schemeIndex < 0 then return nil end
  return schemeIndex + 1
end

local function normalizeSchemeAmmo(b)
  if b == nil then return 0 end
  if b == 10 or b >= 0x80 then return -1 end
  return b
end

-- Delay 0x80..0xFF = locked/unavailable in scheme — not a reload target.
local function normalizeSchemeDelay(b)
  if b == nil or b >= 0x80 then return 0 end
  return b
end

local function clearSchemeTables()
  S.schemeAmmo = {}
  S.schemeDelay = {}
  S.schemeLoaded = false
  S.schemeVersion = 0
  S.schemeSource = nil
end

local function applySchemeBaseline(ammo, delay, meta)
  meta = meta or {}
  S.schemeAmmo = ammo or {}
  S.schemeDelay = delay or {}
  S.schemeLoaded = true
  S.schemeSource = meta.source or "memory"
  S.schemePath = meta.path or "?"
  S.schemeVersion = meta.version or 0
  S.schemePendingMemory = false
  S.schemeReload = {}
  for w, d in pairs(S.schemeDelay) do
    if d and d > 0 then S.schemeReload[w] = d end
  end
end

local function schemeTablesForJson()
  local ammoOut, delayOut = {}, {}
  for w = 1, MATCH_SCHEME_WPN_MAX - 1 do
    local a = S.schemeAmmo[w]
    local d = S.schemeDelay[w]
    if a ~= nil and a ~= 0 then ammoOut[tostring(w)] = a end
    if d ~= nil and d > 0 then delayOut[tostring(w)] = d end
  end
  return ammoOut, delayOut
end

local function applySchemeFromJsonTables(cfg)
  if type(cfg) ~= "table" or type(cfg.schemeAmmo) ~= "table" or type(cfg.schemeDelay) ~= "table" then
    return false
  end
  local ammo, delay = {}, {}
  for k, v in pairs(cfg.schemeAmmo) do
    local w = tonumber(k)
    if w and w > 0 and w < WEAPON_SLOTS then ammo[w] = v end
  end
  for k, v in pairs(cfg.schemeDelay) do
    local w = tonumber(k)
    if w and w > 0 and w < WEAPON_SLOTS then delay[w] = v end
  end
  if next(ammo) == nil and next(delay) == nil then return false end
  applySchemeBaseline(ammo, delay, {
    source = "sync",
    path = cfg.schemePath or "host-sync",
    version = cfg.schemeVersion or 0,
  })
  log("scheme sync: bazooka ammo=%s delay=%s path=%s",
    tostring(ammo[1]), tostring(delay[1]), tostring(cfg.schemePath))
  return true
end

local function schemeBaselinePersists()
  return S.schemeLoaded and (S.schemeSource == "sync" or S.schemeSource == "file" or S.schemeSource == "auto")
end

local captureMatchScheme -- forward decl (needs getAmmo/getDelay later)
local checkSchemeMismatch -- forward decl (defined after resolveSchemePath helpers)

-- Parse binary SCHM .wsc bytes → ammo/delay tables (no side effects on S).
-- Weapons @0x29: 45 × (Ammo, Power, Delay, Crate). Super @0xDD for v2+.
local function parseSchemeBytes(data)
  if not data or #data < (WSC_WEAPONS_OFF + WSC_STD_COUNT * 4) then
    return nil, nil, nil, string.format("file too short (%d bytes)", data and #data or 0)
  end
  if data:sub(1, 4) ~= WSC_MAGIC then
    return nil, nil, nil, "not SCHM (bad magic)"
  end
  local version = data:byte(5)
  if version < 1 or version > 3 then
    return nil, nil, nil, string.format("unsupported SCHM version %d", version or -1)
  end
  local ammo, delay = {}, {}
  local function store(schemeIndex, ammoByte, delayByte)
    local gameId = schemeIndexToGameId(schemeIndex)
    if not gameId or gameId >= WEAPON_SLOTS then return end
    ammo[gameId] = normalizeSchemeAmmo(ammoByte)
    delay[gameId] = normalizeSchemeDelay(delayByte)
  end
  for i = 0, WSC_STD_COUNT - 1 do
    local base = WSC_WEAPONS_OFF + i * 4
    store(i, data:byte(base + 1), data:byte(base + 3))
  end
  if version >= 2 and #data >= (WSC_SUPER_OFF + WSC_SUPER_COUNT * 4) then
    for i = 0, WSC_SUPER_COUNT - 1 do
      local base = WSC_SUPER_OFF + i * 4
      store(WSC_STD_COUNT + i, data:byte(base + 1), data:byte(base + 3))
    end
  end
  return ammo, delay, version, nil
end

local function loadSchemeFile(path)
  if not path or path == "" then
    return false, "empty path"
  end
  local f, err = io.open(path, "rb")
  if not f then
    return false, "open failed: " .. tostring(err or path)
  end
  local data = f:read("*a")
  f:close()
  local ammo, delay, version, perr = parseSchemeBytes(data)
  if not ammo then
    return false, perr or "parse failed"
  end
  checkSchemeMismatch(ammo, delay)
  applySchemeBaseline(ammo, delay, {
    source = "file",
    path = path,
    version = version,
  })
  local bazA = S.schemeAmmo[1]
  local bazD = S.schemeDelay[1]
  log("scheme loaded path=%s ver=%d bazooka ammo=%s delay=%s",
    path, version, tostring(bazA), tostring(bazD))
  return true, string.format("loaded v%d bazooka ammo=%s delay=%s (host: clients sync on lobby send)", version, tostring(bazA), tostring(bazD))
end

local function fileExists(path)
  if not path or path == "" then return false end
  local f = io.open(path, "rb")
  if not f then return false end
  f:close()
  return true
end

local SCHEME_DIR_CANDIDATES = {
  "User/Schemes",
}
-- Minimum non-zero ammo slots before we trust a match-memory capture (avoids accepting
-- regen2 fallback ammo=1 as a "scheme" and breaking auto-detect).
local MIN_CAPTURE_AMMO_WEAPONS = 5

local function resolveSchemePath(argPath)
  if not argPath or argPath == "" then return nil end
  if fileExists(argPath) then return argPath end
  local underSchemes = "User/Schemes/" .. argPath
  if fileExists(underSchemes) then return underSchemes end
  underSchemes = "User\\Schemes\\" .. argPath
  if fileExists(underSchemes) then return underSchemes end
  if not argPath:lower():match("%.wsc$") then
    local withExt = argPath .. ".wsc"
    if fileExists(withExt) then return withExt end
    if fileExists("User/Schemes/" .. withExt) then return "User/Schemes/" .. withExt end
    if fileExists("User\\Schemes\\" .. withExt) then return "User\\Schemes\\" .. withExt end
  end
  return argPath
end

local function shallowCopyWeaponTable(src)
  local out = {}
  if type(src) ~= "table" then return out end
  for k, v in pairs(src) do out[k] = v end
  return out
end

-- Key weapons for sanity when comparing memory baseline vs manual .wsc load.
local SCHEME_MISMATCH_PROBE = {
  { w = 1,  label = "Bazooka" },
  { w = 2,  label = "HomingMissile" },
  { w = 4,  label = "Grenade" },
  { w = 14, label = "Airstrike" },
  { w = 15, label = "Napalm" },
  { w = 25, label = "NinjaRope" },
  { w = 31, label = "Mine" },
}

checkSchemeMismatch = function(newAmmo, newDelay)
  local snap = S.schemeMemorySnapshot
  if not snap or snap.source ~= "memory" then return end
  local oldAmmo = snap.ammo or {}
  local oldDelay = snap.delay or {}
  local mismatches = {}
  for _, probe in ipairs(SCHEME_MISMATCH_PROBE) do
    local w = probe.w
    local oldA, newA = oldAmmo[w], newAmmo[w]
    local oldD, newD = oldDelay[w], newDelay[w]
    if oldA ~= nil and newA ~= nil and oldA ~= newA then
      mismatches[#mismatches + 1] = string.format("%s ammo %s→%s", probe.label, tostring(oldA), tostring(newA))
    end
    if oldD ~= nil and newD ~= nil and oldD > 0 and newD > 0 and oldD ~= newD then
      mismatches[#mismatches + 1] = string.format("%s delay %s→%s", probe.label, tostring(oldD), tostring(newD))
    end
  end
  if #mismatches == 0 then return end
  chat(string.format(
    "scheme WARN: loaded .wsc differs from match-memory (%s) — wrong file? %s",
    snap.path or "match-memory", table.concat(mismatches, "; ")))
  log("scheme mismatch vs memory: %s", table.concat(mismatches, "; "))
end

local function listSchemeFiles()
  -- Cache dir listing: io.popen("dir") is expensive and causes hitching mid-match.
  if S._schemeListCache and S._schemeListCache.names then
    return S._schemeListCache.names
  end
  local seen, names = {}, {}
  local function addName(name)
    if not name or name == "" then return end
    local key = name:lower()
    if seen[key] then return end
    seen[key] = true
    names[#names + 1] = name
  end
  local function scanDir(dir)
    if not dir or dir == "" then return end
    local cmd = string.format('dir /b "%s\\*.wsc" 2>nul', dir)
    local pipe = io.popen(cmd)
    if not pipe then return end
    for line in pipe:lines() do
      line = line:match("^%s*(.-)%s*$")
      if line and line ~= "" then addName(line) end
    end
    pipe:close()
  end
  for _, dir in ipairs(SCHEME_DIR_CANDIDATES) do
    scanDir(dir)
  end
  table.sort(names, function(a, b) return a:lower() < b:lower() end)
  S._schemeListCache = { names = names }
  return names
end

local function invalidateSchemeListCache()
  S._schemeListCache = nil
end

local function schemeFullPath(name)
  if not name or name == "" then return nil end
  local p1 = "User/Schemes/" .. name
  if fileExists(p1) then return p1 end
  local p2 = "User\\Schemes\\" .. name
  if fileExists(p2) then return p2 end
  return p1
end

-- Names-only key (one dir listing). Size checked only while reading during rebuild.
local function schemeDirNameKey()
  local names = listSchemeFiles()
  local parts = {}
  for _, name in ipairs(names) do
    parts[#parts + 1] = name:lower()
  end
  return table.concat(parts, "|"), names
end

local function buildSchemeIndex(force)
  if not S.schemeIndex then
    S.schemeIndex = { fileListKey = nil, entries = {} }
  end
  if not force and S.schemeIndex.entries and #S.schemeIndex.entries > 0 then
    -- Fast path: never touch disk during match if index already warm.
    return S.schemeIndex.entries, false
  end
  if force then
    invalidateSchemeListCache()
  end
  local nameKey, names = schemeDirNameKey()
  if not force and S.schemeIndex.fileListKey == nameKey and S.schemeIndex.entries and #S.schemeIndex.entries > 0 then
    return S.schemeIndex.entries, false
  end
  local entries = {}
  local sizeParts = {}
  for _, name in ipairs(names) do
    local path = schemeFullPath(name)
    local f = io.open(path, "rb")
    if f then
      local data = f:read("*a")
      f:close()
      sizeParts[#sizeParts + 1] = string.format("%s:%d", name:lower(), data and #data or 0)
      local ammo, delay, version, perr = parseSchemeBytes(data)
      if ammo then
        entries[#entries + 1] = {
          path = path,
          name = name,
          ammo = ammo,
          delay = delay,
          version = version or 0,
        }
      else
        log("scheme index skip %s: %s", tostring(name), tostring(perr))
      end
    end
  end
  S.schemeIndex.fileListKey = nameKey
  S.schemeIndex.sizeKey = table.concat(sizeParts, "|")
  S.schemeIndex.entries = entries
  log("scheme index: rebuilt %d entries", #entries)
  return entries, true
end

-- Light check for new/removed .wsc names (one dir listing). Call from lobby/schemes cmd, not every frame.
local function refreshSchemeIndexIfDirChanged()
  invalidateSchemeListCache()
  local nameKey = schemeDirNameKey()
  if S.schemeIndex and S.schemeIndex.fileListKey == nameKey and S.schemeIndex.entries and #S.schemeIndex.entries > 0 then
    return S.schemeIndex.entries, false
  end
  return buildSchemeIndex(true)
end

-- Compare file ammo vs live match ammo. Only non-zero captured slots (weapons present in match).
-- Live panel may keep infinite as 10 or -1; file parser always stores -1.
local function normalizeLiveAmmo(a)
  if a == nil then return 0 end
  if a == 10 or a >= 0x80 or a == -1 then return -1 end
  return a
end

local function scoreSchemeMatch(fileAmmo, capturedAmmo)
  local matches, mismatches = 0, 0
  if type(fileAmmo) ~= "table" or type(capturedAmmo) ~= "table" then
    return 0, 999
  end
  for w, capA in pairs(capturedAmmo) do
    if type(w) == "number" and capA ~= nil and capA ~= 0 then
      local live = normalizeLiveAmmo(capA)
      local fileA = fileAmmo[w]
      if fileA == nil then fileA = 0 end
      if fileA == -1 then
        -- file infinite matches live infinite (10 or -1)
        if live == -1 then
          matches = matches + 1
        else
          mismatches = mismatches + 1
        end
      elseif fileA == live then
        matches = matches + 1
      else
        mismatches = mismatches + 1
      end
    end
  end
  return matches, mismatches
end

-- Find unique .wsc in index whose ammo signature matches captured match ammo.
local function autoDetectScheme(capturedAmmo)
  local entries = buildSchemeIndex(false)
  if not entries or #entries == 0 then
    return nil, "no .wsc in User/Schemes"
  end
  local bestMatches = -1
  local winners = {}
  for _, e in ipairs(entries) do
    local matches, mismatches = scoreSchemeMatch(e.ammo, capturedAmmo)
    if mismatches == 0 and matches > 0 then
      if matches > bestMatches then
        bestMatches = matches
        winners = { e }
      elseif matches == bestMatches then
        winners[#winners + 1] = e
      end
    end
  end
  if #winners == 1 then
    local e = winners[1]
    return e, nil
  end
  if #winners > 1 then
    local names = {}
    for _, e in ipairs(winners) do names[#names + 1] = e.name end
    return nil, string.format("%d candidates tie by ammo: %s", #winners, table.concat(names, ", "))
  end
  return nil, "no ammo signature match"
end

local function chatSchemeFileList()
  local entries = refreshSchemeIndexIfDirChanged()
  if not entries or #entries == 0 then
    chat("schemes: none found in User/Schemes — put .wsc there or use /delay scheme <full path>")
    return
  end
  local maxShow = 20
  local shown = {}
  for i = 1, math.min(#entries, maxShow) do
    shown[#shown + 1] = entries[i].name
  end
  local tail = (#entries > maxShow) and string.format(" … +%d more", #entries - maxShow) or ""
  chat(string.format("schemes (%d indexed): %s%s", #entries, table.concat(shown, ", "), tail))
  chat("usage: /delay scheme <name.wsc>  (or rely on auto-detect at match start)")
end

local function schemeStatusLine()
  local src = S.schemeSource or "?"
  local path = S.schemePath or "?"
  return string.format("scheme[%s]: %s bazooka ammo=%s delay=%s",
    src, path, tostring(S.schemeAmmo[1]), tostring(S.schemeDelay[1]))
end

local function ensureSchemeLoaded(announce)
  if S.schemeLoaded then
    if announce then chat(schemeStatusLine()) end
    return true
  end
  -- Prefer live match tables (works online). Capture when a team is available.
  if S.schemePendingMemory then
    local team = nil
    if type(activeTeam) == "function" then
      -- activeTeam defined later; try registered teams via pairs if needed below
    end
    for _, entry in pairs(S.teams) do
      if entry.task and isValidTask and isValidTask(entry.task) then
        team = entry.task
        break
      elseif entry.task then
        team = entry.task
        break
      end
    end
    if team and captureMatchScheme then
      captureMatchScheme(team)
    end
  end
  if S.schemeLoaded then
    if announce then chat(schemeStatusLine()) end
    return true
  end
  -- Optional: last explicit .wsc path (never guess John_Wick / random schemes).
  if S.schemePath and S.schemePath ~= "match-memory" and fileExists(S.schemePath) then
    local ok, msg = loadSchemeFile(S.schemePath)
    if ok then
      if announce then chat(schemeStatusLine() .. " | " .. tostring(msg)) end
      return true
    end
  end
  if announce then
    if S.schemePendingMemory then
      chat("scheme: waiting match start (reads live ammo/delay) or /delay scheme <path.wsc>")
    else
      chat("scheme not loaded — start a match or /delay scheme <path.wsc>")
    end
  end
  return false
end

-- Sol2: nullptr — userdata (не nil); .children = [this+0x8] → AV. pcall не ловит C++ AV.
local function isValidTask(task)
  if task == nil then return false end
  if type(isTaskAlive) == "function" then
    return isTaskAlive(task)
  end
  local ok, addr = pcall(function() return task:getAddr() end)
  return ok and addr and addr ~= 0
end

local function safeAddr(obj)
  if not isValidTask(obj) then return nil end
  local ok, addr = pcall(function() return obj:getAddr() end)
  if not ok or not addr or addr == 0 then return nil end
  return addr
end

local function taskChildrenCount(task)
  if not isValidTask(task) then return 0 end
  if type(taskChildCount) == "function" then
    local ok, n = pcall(taskChildCount, task)
    return ok and n or 0
  end
  local ok, n = pcall(function() return task.children:size() end)
  return ok and n or 0
end

local function taskChild(task, index)
  if not isValidTask(task) then return nil end
  if type(taskChildAt) == "function" then
    local ok, child = pcall(taskChildAt, task, index)
    return ok and child or nil
  end
  local ok, child = pcall(function() return task.children:get(index) end)
  return ok and child or nil
end

local function hasTurnGame()
  if type(getTurnGameAddr) == "function" then
    local ok, addr = pcall(getTurnGameAddr)
    return ok and addr and addr ~= 0
  end
  if type(getTurnGameObject) ~= "function" then return false end
  local ok, root = pcall(getTurnGameObject)
  return ok and isValidTask(root)
end

local function hasWeaponApi()
  return type(getTeamWeaponAmmo) == "function"
      and type(getTeamWeaponDelay) == "function"
      and type(setTeamWeaponAmmo) == "function"
      and type(setTeamWeaponDelay) == "function"
      and type(adjustTeamDelays) == "function"
end

local function teamFromWorm(worm)
  if not isValidTask(worm) then return nil end
  local ok, p = pcall(function() return worm.parent end)
  if not ok then return nil end
  while isValidTask(p) do
    local okCt, ct = pcall(function() return p.classtype end)
    if okCt and ct == CLASS.Team then return p end
    local okParent, nextP = pcall(function() return p.parent end)
    if not okParent then break end
    p = nextP
  end
  return nil
end

local function walkTasks(task, fn)
  if not isValidTask(task) then return end
  pcall(fn, task)
  local n = taskChildrenCount(task)
  if not n or n <= 0 then return end
  for i = 1, n do
    local child = taskChild(task, i)
    if isValidTask(child) then walkTasks(child, fn) end
  end
end

local function discoverTeams()
  if not hasTurnGame() then return 0 end
  if type(getTurnGameObject) ~= "function" then return 0 end
  local ok, root = pcall(getTurnGameObject)
  if not ok or not isValidTask(root) then return 0 end
  local found = 0
  walkTasks(root, function(task)
    local okCt, ct = pcall(function() return task.classtype end)
    if okCt and ct == CLASS.Team then
      registerTeam(task)
      found = found + 1
    end
  end)
  return found
end

local function teamCount()
  local n = 0
  for _ in pairs(S.teams) do n = n + 1 end
  return n
end

local function debugStatus(team)
  if not S.apiOk then return "api=missing" end
  local n = teamCount()
  local panel = "?"
  local alliance = "?"
  local sample = "?"
  local probes = ""
  if team and type(getWeaponPanelFromTeam) == "function" then
    local ok, p = pcall(function() return getWeaponPanelFromTeam(team) end)
    panel = ok and tostring(p) or "err"
  end
  if team and type(getAllianceIdFromTeam) == "function" then
    local ok, a = pcall(function() return getAllianceIdFromTeam(team) end)
    alliance = ok and tostring(a) or "err"
  end
  if team then
    local bits = {}
    for _, wpn in ipairs({0, 1, 2, 3, 4, 5}) do
      local ok, d = pcall(function() return getDelay(team, wpn) end)
      if ok and d and d > 0 then
        bits[#bits + 1] = string.format("w%d=%d", wpn, d)
      end
    end
    probes = (#bits > 0) and table.concat(bits, " ") or "delays=0"
    local ok, d0 = pcall(function() return getDelay(team, 0) end)
    sample = ok and tostring(d0) or "err"
  end
  return string.format("teams=%d panel=%s alliance=%s delay[0]=%s %s",
    n, panel, alliance, sample, probes)
end

local function getAmmo(team, wpn) return getTeamWeaponAmmo(team, wpn) end
local function getDelay(team, wpn) return getTeamWeaponDelay(team, wpn) end
local function setAmmo(team, wpn, v) setTeamWeaponAmmo(team, wpn, v) end
local function setDelay(team, wpn, v) setTeamWeaponDelay(team, wpn, v) end

-- Capture the scheme WA already applied into the match (local + online).
-- Must run once ASAP on match start — before delay-down and before delays tick to 0.
-- Scheme Delay=N starts as panel delay=N; after unlock panel shows 0 but reload is still N.
captureMatchScheme = function(team)
  if not S.schemePendingMemory then return false end
  if schemeBaselinePersists() then
    S.schemePendingMemory = false
    return false
  end
  if not S.apiOk or not team then return false end
  local okAlive = true
  if type(isValidTask) == "function" then
    okAlive = isValidTask(team)
  end
  if not okAlive then return false end

  -- Bound worst case: if the weapon panel never fills in (e.g. odd game state), don't keep
  -- re-scanning every real turn for the whole match — that's the recurring "lag" complaint.
  local now = type(getTickCount) == "function" and getTickCount() or nil
  if now and not S.schemeCaptureFirstTick then S.schemeCaptureFirstTick = now end
  S.schemeCaptureAttempts = S.schemeCaptureAttempts + 1
  local elapsed = (now and S.schemeCaptureFirstTick) and (now - S.schemeCaptureFirstTick) or 0
  if S.schemeCaptureAttempts > SCHEME_CAPTURE_GIVEUP_ATTEMPTS or elapsed > SCHEME_CAPTURE_GIVEUP_MS then
    S.schemePendingMemory = false
    S.schemeAutoDetectDone = true
    log("match-memory capture gave up after %d attempts / %dms — self-learning only, /delay scheme <file> for accurate reload",
      S.schemeCaptureAttempts, elapsed)
    return false
  end

  local ammo = {}
  local delay = {}
  local nAmmo, nDelay = 0, 0
  local function readInto(t)
    local a, d, na, nd = {}, {}, 0, 0
    for w = 1, MATCH_SCHEME_WPN_MAX - 1 do
      local okA, av = pcall(getAmmo, t, w)
      local okD, dv = pcall(getDelay, t, w)
      if okA and av ~= nil then
        a[w] = av
        if av ~= 0 then na = na + 1 end
      end
      if okD and dv ~= nil then
        local v = dv
        if v < 0 then v = 0 end
        d[w] = v
        if v > 0 then nd = nd + 1 end
      end
    end
    return a, d, na, nd
  end
  ammo, delay, nAmmo, nDelay = readInto(team)
  -- Panel for this team may be empty early — try other registered teams.
  if nAmmo < MIN_CAPTURE_AMMO_WEAPONS then
    for _, entry in pairs(S.teams) do
      local other = entry.task
      if other and other ~= team then
        local ok = true
        if type(isValidTask) == "function" then ok = isValidTask(other) end
        if ok then
          local a2, d2, na, nd = readInto(other)
          if na > nAmmo then
            ammo, delay, nAmmo, nDelay = a2, d2, na, nd
          end
        end
      end
    end
  end
  -- Sanity: empty / too-weak panel → retry (do NOT accept regen2 ammo=1 as scheme).
  if nAmmo < MIN_CAPTURE_AMMO_WEAPONS then
    log("match-memory capture weak nAmmo=%d nDelay=%d — retry later", nAmmo, nDelay)
    return false
  end

  S.schemeMemorySnapshot = {
    source = "memory",
    path = "match-memory",
    ammo = shallowCopyWeaponTable(ammo),
    delay = shallowCopyWeaponTable(delay),
  }

  -- Auto-detect .wsc by ammo signature against cached User/Schemes index.
  if not S.schemeAutoDetectDone then
    local hit, why = autoDetectScheme(ammo)
    if hit then
      S.schemeAutoDetectDone = true
      applySchemeBaseline(hit.ammo, hit.delay, {
        source = "auto",
        path = hit.path or hit.name,
        version = hit.version or 0,
      })
      chat(string.format(
        "scheme[auto]: detected %s (ammo signature match=%d) — delay accurate from turn 1 | bazooka ammo=%s delay=%s",
        hit.name or hit.path, select(1, scoreSchemeMatch(hit.ammo, ammo)),
        tostring(S.schemeAmmo[1]), tostring(S.schemeDelay[1])))
      log("scheme auto: %s bazooka ammo=%s delay=%s", tostring(hit.name),
        tostring(S.schemeAmmo[1]), tostring(S.schemeDelay[1]))
      return true
    end
    -- Mark done only after a *strong* capture failed to match — don't keep re-scanning.
    S.schemeAutoDetectDone = true
    if why and why:find("tie") then
      chat(string.format("scheme auto-detect: %s — use /delay scheme <file>", why))
    else
      log("scheme auto-detect failed: %s — using memory baseline", tostring(why))
      chat("scheme auto-detect: no match in User/Schemes — delay from memory only; /delay scheme <file> for accurate reload")
    end
  end

  applySchemeBaseline(ammo, delay, { source = "memory", path = "match-memory", version = 0 })
  chat(string.format(
    "scheme[memory]: match ammo/delay captured (weapons with ammo=%d delay>0=%d) bazooka ammo=%s delay=%s",
    nAmmo, nDelay, tostring(ammo[1]), tostring(delay[1])))
  log("match-memory bazooka ammo=%s delay=%s", tostring(ammo[1]), tostring(delay[1]))
  return true
end

-- Raw-memory путь. ПОЧЕМУ он нужен: getTeamWeaponAddr/getTeamWeaponDelay для стандартного
-- оружия резолвят "слот" через панель (*(panel+4*(*(panel+4)))+0x20) КАЖДЫЙ раз заново —
-- то есть через "чей ход активен на панели прямо сейчас". regen2 срабатывает при ammo=0
-- (последний выстрел патроном) и часто дописывает delay через много retry-кадров (до ~90)
-- ПОСЛЕ выстрела — а именно после последнего патрона игроки чаще всего быстро завершают
-- ход. Если к моменту фактической записи delay активный слот на панели уже сменился на
-- другую команду (ход передался), setTeamWeaponDelay(team,...) пишет по СВЕЖЕ пересчитанному
-- (уже неверному) адресу — команда, которая стреляла, не видит обновления, хотя ammo
-- (который пишется сразу же, на первом тике) успевает записаться в верный адрес.
-- Фикс: адрес ammo/delay фиксируется ОДИН РАЗ в момент выстрела (когда слот точно верный)
-- и используется для всех последующих чтений/записей этого regen-цикла.
local function hasRawMem()
  return type(getTeamWeaponAddr) == "function"
      and type(getPanelWeaponAddr) == "function"
      and type(readMemInt) == "function"
      and type(writeMemInt) == "function"
end

local function snapshotAddrs(team, wpn)
  if not hasRawMem() then return nil, nil end
  local okA, a = pcall(getTeamWeaponAddr, team, wpn, 0)
  local okD, d = pcall(getTeamWeaponAddr, team, wpn, 1)
  return (okA and a and a ~= 0) and a or nil, (okD and d and d ~= 0) and d or nil
end

local function snapshotPanelAddrs(team, wpn)
  if not hasRawMem() then return nil, nil end
  local okA, a = pcall(getPanelWeaponAddr, team, wpn, 0)
  local okD, d = pcall(getPanelWeaponAddr, team, wpn, 1)
  return (okA and a and a ~= 0) and a or nil, (okD and d and d ~= 0) and d or nil
end

local function rawRead(addr, fallback)
  if addr and addr ~= 0 and type(readMemInt) == "function" then
    local ok, v = pcall(readMemInt, addr)
    if ok and v ~= nil then return v end
  end
  return fallback
end

-- Пишет по фиксированному адресу если он есть и валиден, иначе — обычный API-фолбэк.
-- markWeaponPanelDirtyForTeam нужен, потому что запись через writeMemInt мимо
-- setTeamWeaponDelay/Ammo не ставит флаг redraw=1 на панели (panel+460) сама.
local function rawWrite(team, addr, value, fallbackFn)
  if addr and addr ~= 0 and type(writeMemInt) == "function" then
    local ok = pcall(writeMemInt, addr, value)
    if ok then
      if type(markWeaponPanelDirtyForTeam) == "function" then
        pcall(markWeaponPanelDirtyForTeam, team)
      end
      return true
    end
  end
  if fallbackFn then fallbackFn() end
  return false
end

local forEachTeam
local applyRegen

-- Baseline только на старте хода (до delay down). Не переснимать при /delay или после выстрела.
local function snapshotScheme(team, t)
  if not S.apiOk then return end
  if t.snap then return end
  for w = 0, WEAPON_SLOTS - 1 do
    t.ammo0[w] = getAmmo(team, w)
    t.delay0[w] = getDelay(team, w)
  end
  t.snap = true
  log("scheme snapshot team=%X", team:getAddr())
end

local function regenDebugMsg(fmt, ...)
  if not S.regenDebug then return end
  chat(string.format(fmt, ...))
end

local function teamSlotOf(team)
  if type(getTeamSlotFromTeam) == "function" then
    local ok, s = pcall(getTeamSlotFromTeam, team)
    return ok and tostring(s) or "err"
  end
  return "?"
end

local function panelSlotOf(team)
  if type(getPanelTeamSlotFromTeam) ~= "function" then return "?" end
  local ok, s = pcall(getPanelTeamSlotFromTeam, team)
  return ok and tostring(s) or "err"
end

local function allianceOf(team)
  if type(getAllianceIdFromTeam) ~= "function" then return "?" end
  local ok, s = pcall(getAllianceIdFromTeam, team)
  return ok and tostring(s) or "err"
end

local function pollShotReloadCapture()
  if type(pollPendingShotReloadCaptures) == "function" then
    pcall(pollPendingShotReloadCaptures)
  end
end

local function parseCounter(counters, key)
  local v = counters:match(key .. "=([%-%d]+)")
  return v and tonumber(v) or nil
end

local function explainCounters(counters)
  if not counters or counters == "?" or counters == "err" then
    return "counters unavailable (rebuild wkJellyWorm.dll)"
  end
  local lines = {}
  local recordShot = parseCounter(counters, "recordShot") or 0
  local recordStored = parseCounter(counters, "recordShotStored") or 0
  local lastCapture = parseCounter(counters, "lastCaptureDelay") or 0
  local lastRecorded = parseCounter(counters, "lastRecordedDelay") or 0
  local pending = parseCounter(counters, "pendingCaptures") or 0
  local v1 = parseCounter(counters, "subtractAmmoV1") or 0
  local v1std = parseCounter(counters, "subtractAmmoV1std") or 0
  local v2 = parseCounter(counters, "subtractAmmoV2") or 0
  local v2std = parseCounter(counters, "subtractAmmoV2std") or 0
  local lastWpn = parseCounter(counters, "lastWeaponShot")

  if v1 == 0 and v1std == 0 and v2 == 0 and v2std == 0 then
    lines[#lines + 1] = "subtractAmmo hook not fired yet - shoot a weapon"
  elseif v1std > 0 and recordShot == 0 then
    lines[#lines + 1] = "v1std>0 but recordShot=0: rebuild wkJellyWorm.dll (v1 capture fix)"
  elseif v2std > 0 and v1std == 0 and recordShot == 0 then
    lines[#lines + 1] = "only v2 fired - standard weapons may use subtractAmmo v1"
  end
  if recordShot == 0 then
    lines[#lines + 1] = "recordShot=0: post-shot reload not captured yet"
  elseif recordStored == 0 then
    lines[#lines + 1] = string.format(
      "recordShot=%d stored=0: delay<=0 at hook (live=%d) - game sets reload later; polling (~90 frames)",
      recordShot, lastCapture)
  end
  if pending > 0 then
    lines[#lines + 1] = string.format("pendingCaptures=%d: deferred reload capture in progress", pending)
  end
  if lastRecorded > 0 then
    lines[#lines + 1] = string.format("lastRecordedDelay=%d: last saved reload", lastRecorded)
  elseif lastRecorded == 0 then
    lines[#lines + 1] = "lastRecordedDelay=0: waiting for game reload write (normal)"
  end
  if lastCapture <= 0 and recordShot > 0 then
    lines[#lines + 1] = "lastCaptureDelay<=0: delay=0/-1 in memory - weapon ready or reload not written yet"
  end
  if lastWpn ~= nil and lastWpn >= 0 then
    lines[#lines + 1] = string.format("lastWeaponShot=%d - last weapon in subtractAmmo_v2", lastWpn)
  end
  if #lines == 0 then
    lines[#lines + 1] = "counters look OK"
  end
  return table.concat(lines, " | ")
end

local function captureKnownReload(t, wpn, live)
  if not t or wpn == nil or wpn < 0 or not live or live <= 0 then return end
  if not t.knownReload then t.knownReload = {} end
  if live > (t.knownReload[wpn] or 0) then
    t.knownReload[wpn] = live
  end
  if live > (S.schemeReload[wpn] or 0) then
    S.schemeReload[wpn] = live
  end
  -- If match-memory captured late (delay already ticked down), raise from live post-shot reload.
  if live > (S.schemeDelay[wpn] or 0) then
    S.schemeDelay[wpn] = live
  end
end

local function configuredReload(wpn)
  local n = S.schemeReload[wpn]
  return n and n > 0 and n or 0
end

local function lastShotDelay(team, wpn)
  if type(getTeamWeaponLastShotDelay) ~= "function" then return 0 end
  local ok, d = pcall(getTeamWeaponLastShotDelay, team, wpn)
  return ok and d or 0
end

-- Priority: /delay reload override > .wsc schemeDelay > knownReload > shot/peak/live.
-- TurnStarted delay0 is usually 0 for ready weapons — never primary baseline.
local function regenDelayTarget(team, t, wpn, pending)
  local peak = t.postShotDelay and t.postShotDelay[wpn] or 0
  if pending and pending.peakMax and pending.peakMax > peak then
    peak = pending.peakMax
  end
  local shot = lastShotDelay(team, wpn)
  local live = rawRead(pending and pending.delayAddr, getDelay(team, wpn))
  local known = (t.knownReload and t.knownReload[wpn]) or 0
  local cfg = configuredReload(wpn)
  local schemeD = (S.schemeLoaded and S.schemeDelay[wpn]) or 0
  local target = 0
  if cfg > 0 then
    target = cfg
  elseif schemeD > 0 then
    target = schemeD
  elseif known > 0 then
    target = known
  else
    if shot > target then target = shot end
    if peak > target then target = peak end
    if live > target then target = live end
  end
  return target, shot, peak
end

-- Write delay via C++ API only (dual-slot). Raw writes previously risked hitting ammo cells.
local function writeDelayEverywhere(team, wpn, value, pending)
  if value == nil or value < 0 then return "skip" end
  setDelay(team, wpn, value)
  if type(markWeaponPanelDirtyForTeam) == "function" then
    pcall(markWeaponPanelDirtyForTeam, team)
  end
  return "api"
end

-- Never restore ammo to 0 — that deletes the weapon from the panel.
-- Prefer .wsc schemeAmmo over TurnStarted panel snapshot.
local function resolveRestoreAmmo(t, wpn)
  local schemeA = S.schemeLoaded and S.schemeAmmo[wpn] or nil
  local a0 = t.ammo0 and t.ammo0[wpn]
  local atFire = t.ammoAtFire and t.ammoAtFire[wpn]
  if schemeA == -1 or a0 == -1 or atFire == -1 then return -1 end
  if type(schemeA) == "number" and schemeA > 0 then return schemeA end
  if type(a0) == "number" and a0 > 0 then return a0 end
  if type(atFire) == "number" and atFire > 0 then return atFire end
  -- Last resort: 1 round so weapon stays usable (better than wiping to 0)
  return 1
end

local function writeAmmoSafe(team, wpn, value)
  if value == nil then return false end
  if value == 0 then
    log("refuse write ammo=0 wpn=%d (would remove weapon)", wpn)
    return false
  end
  setAmmo(team, wpn, value)
  if type(markWeaponPanelDirtyForTeam) == "function" then
    pcall(markWeaponPanelDirtyForTeam, team)
  end
  return true
end

local function scheduleNextTurnDelay(team, t, wpn, target)
  if not t or wpn == nil or target == nil or target <= 0 then return end
  if not t.nextTurnDelay then t.nextTurnDelay = {} end
  t.nextTurnDelay[wpn] = target
  captureKnownReload(t, wpn, target)
  log("schedule next-turn delay wpn=%d tgt=%d team=%X", wpn, target, team:getAddr())
end

local function applyNextTurnDelays(team, t)
  if not t or not t.nextTurnDelay then return end
  for wpn, target in pairs(t.nextTurnDelay) do
    if target and target > 0 then
      local wrote = writeDelayEverywhere(team, wpn, target, nil)
      local after = getDelay(team, wpn)
      chat(string.format("next-turn delay wpn=%d%s tgt=%d after=%d via=%s",
        wpn, weaponHint(wpn), target, after, wrote))
      if after == target then
        t.nextTurnDelay[wpn] = nil
      end
    end
  end
end

local function notePostShotDelay(team, t, wpn, delayAddr)
  if not t.postShotDelay then t.postShotDelay = {} end
  local live = rawRead(delayAddr, getDelay(team, wpn))
  if live > (t.postShotDelay[wpn] or 0) then
    t.postShotDelay[wpn] = live
  end
  captureKnownReload(t, wpn, live)
end

local function scheduleShotProbe(team, wpn)
  if not team or wpn == nil or wpn < 0 then return end
  -- No noisy probe when scheme already has a reload target for this weapon.
  local schemeD = (S.schemeLoaded and S.schemeDelay and S.schemeDelay[wpn]) or 0
  if schemeD > 0 then return end
  S.shotProbe = { team = team, wpn = wpn, ttl = SHOT_PROBE_FRAMES, shown = false }
end

local function tickShotProbe()
  local p = S.shotProbe
  if not p or not isValidTask(p.team) then
    S.shotProbe = nil
    return
  end
  pollShotReloadCapture()
  p.ttl = p.ttl - 1
  local due = (p.ttl <= 0) or (p.ttl == SHOT_PROBE_FRAMES - 30) or (p.ttl == SHOT_PROBE_FRAMES - 60)
  if due and not p.shown then
    p.shown = true
    local counters = "?"
    if type(getCustomWeaponsDebugCounters) == "function" then
      local ok, c = pcall(getCustomWeaponsDebugCounters)
      counters = ok and c or "err"
    end
    local t = getTeamEntry(p.team)
    local peak = (t and t.postShotDelay and t.postShotDelay[p.wpn]) or 0
    -- File log only — chat spam ("reload not saved yet") was confusing and not an error.
    log("post-shot probe wpn=%d delay=%d lastShot=%d peak=%d | %s",
      p.wpn, getDelay(p.team, p.wpn), lastShotDelay(p.team, p.wpn), peak, counters)
  end
  if p.ttl <= 0 then S.shotProbe = nil end
end

local function applyRegenWrite(team, t, wpn, pending)
  if S.regen == 0 or wpn == nil or wpn < 0 then return true, false end
  if not S.apiOk then
    chat("regen: wkJellyWorm weapon API missing")
    return true, false
  end
  if not S.schemeLoaded and not t.snap then
    regenDebugMsg("regen skip wpn=%d: scheme not loaded ( /delay scheme <path.wsc> )", wpn)
    return true, false
  end

  local before = getDelay(team, wpn)
  local ammoBefore = getAmmo(team, wpn)
  local shotD = lastShotDelay(team, wpn)
  local schemeD = (S.schemeLoaded and S.schemeDelay[wpn]) or 0

  if S.regen == 1 then
    -- regen1: put .wsc / configured reload delay back after shot (do NOT touch ammo).
    local target = select(1, regenDelayTarget(team, t, wpn, pending))
    if target <= 0 then
      if pending and not pending._pendingMsg then
        pending._pendingMsg = true
        regenDebugMsg("regen1 wpn=%d%s pending: no reload target — /delay scheme or /delay reload %d <N>",
          wpn, weaponHint(wpn), wpn)
      end
      return false, false
    end
    local wrote = writeDelayEverywhere(team, wpn, target, pending)
    local after = getDelay(team, wpn)
    regenDebugMsg("regen1 wpn=%d%s schemeD=%d shot=%d live=%d->%d tgt=%d via=%s",
      wpn, weaponHint(wpn), schemeD, shotD, before, after, target, wrote)
    scheduleNextTurnDelay(team, t, wpn, target)
    if after == target then return true, true end
    return false, false
  elseif S.regen == 2 then
    local ammoAtFire = t.ammoAtFire and t.ammoAtFire[wpn] or nil
    local shouldRestore = ammoBefore == 0
    if not shouldRestore and ammoAtFire ~= nil and ammoAtFire > 0 and ammoBefore < ammoAtFire then
      shouldRestore = true
    end
    if not shouldRestore and ammoAtFire == 1 then
      shouldRestore = true
    end
    if not shouldRestore then
      regenDebugMsg("regen2 skip wpn=%d ammo=%d (fire=%s)", wpn, ammoBefore,
        ammoAtFire ~= nil and tostring(ammoAtFire) or "?")
      return true, false
    end
    if pending and not pending.ammoDone then
      local restore = resolveRestoreAmmo(t, wpn)
      if writeAmmoSafe(team, wpn, restore) then
        pending.ammoDone = true
        chat(string.format("regen2 ammo wpn=%d%s %d -> %d", wpn, weaponHint(wpn), ammoBefore, restore))
      else
        chat(string.format("regen2 ammo SKIP wpn=%d (would write 0; ammo0=%s fire=%s)",
          wpn, tostring(t.ammo0 and t.ammo0[wpn]), tostring(ammoAtFire)))
        return true, false
      end
    end
    local target, shotNow, peakD = regenDelayTarget(team, t, wpn, pending)
    if target > 0 then
      local wrote = writeDelayEverywhere(team, wpn, target, pending)
      scheduleNextTurnDelay(team, t, wpn, target)
      local after = getDelay(team, wpn)
      regenDebugMsg(
        "regen2 wpn=%d%s ammo=%d->%d schemeD=%d shot=%d peak=%d live=%d->%d tgt=%d via=%s nextTurn=1",
        wpn, weaponHint(wpn), ammoBefore, getAmmo(team, wpn),
        schemeD, shotNow, peakD, before, after, target, wrote)
      if after == target then return true, true end
      return true, pending and pending.ammoDone
    end
    if pending and not pending._pendingMsg then
      pending._pendingMsg = true
      regenDebugMsg("regen2 wpn=%d%s ammo restored, delay pending — /delay reload %d <N>",
        wpn, weaponHint(wpn), wpn)
    end
    return false, false
  end
  return true, false
end
applyRegen = applyRegenWrite

local function rememberAmmoBeforeShot(team, wpn)
  if not isValidTask(team) or wpn == nil or wpn < 0 then return end
  registerTeam(team)
  local t = getTeamEntry(team)
  if not t then return end
  if not t.ammoAtFire then t.ammoAtFire = {} end
  -- Only capture if we still see ammo (before subtract). Do not overwrite with 0.
  local live = getAmmo(team, wpn)
  if live ~= nil and live ~= 0 then
    t.ammoAtFire[wpn] = live
  end
  if (not t.ammo0[wpn] or t.ammo0[wpn] == 0) and live ~= nil and live ~= 0 then
    t.ammo0[wpn] = live
  end
end

local function queueRegen(team, wpn)
  if S.regen == 0 or not team or wpn == nil or wpn < 0 then return end
  registerTeam(team)
  local t = getTeamEntry(team)
  if t then
    if not t.postShotDelay then t.postShotDelay = {} end
    t.postShotDelay[wpn] = 0
    -- Do NOT overwrite ammoAtFire here — shot may already have consumed ammo.
  end
  local ammoAddr, delayAddr = snapshotAddrs(team, wpn)
  local _, panelDelayAddr = snapshotPanelAddrs(team, wpn)
  S.pendingRegen = {
    team = team,
    wpn = wpn,
    wait = REGEN_WAIT_FRAMES,
    retry = REGEN_RETRY_FRAMES,
    ammoDone = false,
    peakMax = 0,
    ammoAddr = ammoAddr,
    delayAddr = delayAddr,
    panelDelayAddr = panelDelayAddr,
  }
  scheduleShotProbe(team, wpn)
end

local function tickPendingRegen()
  local p = S.pendingRegen
  if not p then return end
  if not isValidTask(p.team) then
    S.pendingRegen = nil
    return
  end
  local t = getTeamEntry(p.team)
  if not t then
    S.pendingRegen = nil
    return
  end

  notePostShotDelay(p.team, t, p.wpn, p.delayAddr)
  local live = rawRead(p.delayAddr, getDelay(p.team, p.wpn))
  if live > (p.peakMax or 0) then p.peakMax = live end

  if p.wait > 0 then
    p.wait = p.wait - 1
    return
  end

  local stop, ok = applyRegen(p.team, t, p.wpn, p)
  if stop or p.retry <= 0 then
    if not ok and p.retry <= 0 then
      regenDebugMsg("regen give up wpn=%d", p.wpn)
      if S.regen == 2 then
        if not t.regen2NeedDelay then t.regen2NeedDelay = {} end
        t.regen2NeedDelay[p.wpn] = true
      end
    end
    S.pendingRegen = nil
    if t.postShotDelay then t.postShotDelay[p.wpn] = nil end
    if t.ammoAtFire then t.ammoAtFire[p.wpn] = nil end
    return
  end

  p.retry = p.retry - 1
end

local function armRegenForTeam(team, wpn)
  if S.regen == 0 or not isValidTask(team) then return end
  registerTeam(team)
  S.fireArmed = true
  S.pendingTeam = team
  S.pendingWpn = wpn
  if wpn == nil or wpn < 0 then
    local ok, id = pcall(function() return team.lastLaunchedWeapon_dword60 end)
    S.pendingWpn = ok and id or nil
  end
  if S.pendingWpn ~= nil and S.pendingWpn >= 0 then
    rememberAmmoBeforeShot(team, S.pendingWpn)
  end
end

local function applyDelayDown(team, t)
  if S.delayDown <= 0 or t.delayDownDone or not S.apiOk then return end
  adjustTeamDelays(team, S.delayDown)
  t.delayDownDone = true
  log("delay down -%d team=%X", S.delayDown, team:getAddr())
end

function getTeamEntry(team)
  local addr = safeAddr(team)
  if not addr then return nil end
  local t = S.teams[addr]
  if not t then
    t = {
      task = team,
      snap = false,
      delayDownDone = false,
      ammo0 = {},
      delay0 = {},
      postShotDelay = {},
      ammoAtFire = {},
      regen2NeedDelay = {},
      knownReload = {},
      nextTurnDelay = {},
      prevWpn = team.lastLaunchedWeapon_dword60,
    }
    S.teams[addr] = t
    log("team registered addr=%X", addr)
  end
  t.task = team
  return t, addr
end

function registerTeam(team)
  if not isValidTask(team) then return end
  getTeamEntry(team)
end

forEachTeam = function(fn)
  for addr, t in pairs(S.teams) do
    if t.task and isValidTask(t.task) then
      fn(t.task, t)
    else
      S.teams[addr] = nil
    end
  end
end

local function tickDelayRate()
  if S.delayRateSec <= 0 or not S.apiOk then return end
  if type(getTickCount) ~= "function" then return end
  local now = getTickCount()
  if S.lastRateTick == 0 then S.lastRateTick = now end
  local intervalMs = S.delayRateSec * 1000
  if now - S.lastRateTick < intervalMs then return end
  S.lastRateTick = now
  if teamCount() == 0 and hasTurnGame() then discoverTeams() end
  forEachTeam(function(team) adjustTeamDelays(team, 1) end)
end

local function teamcallback(This, sender, messagetype, psize, params)
  if not isValidTask(This) then return 0 end
  registerTeam(This)
  -- TurnStarted приходит на CTaskTeam, не на червяка — без этого delay0 не снимается.
  if messagetype == MSG.StartTurn or messagetype == MSG.TurnStarted then
    onTurnStarted(This)
  elseif messagetype == MSG.TurnFinished then
    onTurnFinished(This)
  elseif messagetype == MSG.WeaponFinished then
    onWeaponFinished(This)
  end
  local t = S.teams[safeAddr(This)]
  if not t then return 0 end
  local ok, cur = pcall(function() return This.lastLaunchedWeapon_dword60 end)
  if not ok then return 0 end
  if cur ~= t.prevWpn then
    -- смена оружия после выстрела: regen для того, что стреляли (prevWpn)
    if S.fireArmed and S.regen ~= 0 and t.prevWpn ~= nil and t.prevWpn >= 0 then
      S.fireArmed = false
      queueRegen(This, t.prevWpn)
    end
    t.prevWpn = cur
  end
  return 0
end

local function trackWormTeam(worm)
  local team = teamFromWorm(worm)
  if team then registerTeam(team) end
end

local function trackActiveWorm(worm)
  if not isValidTask(worm) then return end
  S.activeWorm = worm
  S.activeAddr = safeAddr(worm) or 0
  trackWormTeam(worm)
end

local function wormcallback(This, sender, messagetype, psize, params)
  if messagetype >= MSG.MoveLeft and messagetype <= MSG.ReleaseWeapon then
    trackActiveWorm(This)
  end
  if messagetype == MSG.FireWeapon and S.regen ~= 0 then
    local team = teamFromWorm(This)
    if team then
      local ok, wpn = pcall(function() return team.lastLaunchedWeapon_dword60 end)
      armRegenForTeam(team, ok and wpn or nil)
    end
  elseif messagetype == MSG.ReleaseWeapon and S.fireArmed and S.regen ~= 0 then
    local team = teamFromWorm(This)
    if team then
      S.fireArmed = false
      queueRegen(team, S.pendingWpn)
    end
  end
  if messagetype == MSG.RenderScene then
    -- RenderScene is broadcast to every worm task, i.e. this fires N times per real frame
    -- (N = worm count). Without this guard all per-frame work below ran N times/frame.
    local now = type(getTickCount) == "function" and getTickCount() or nil
    if not now or now ~= S.lastRenderTick then
      if now then S.lastRenderTick = now end
      pollShotReloadCapture()
      tickShotProbe()
      tickPendingRegen()
      if now and teamCount() == 0 and now - S.discoverTick > 2000 and hasTurnGame() then
        S.discoverTick = now
        discoverTeams()
      end
      tickDelayRate()
    end
  elseif messagetype == MSG.StartTurn or messagetype == MSG.TurnStarted then
    trackActiveWorm(This)
    local team = teamFromWorm(This)
    if team then onTurnStarted(team) end
  end
  return 0
end

local function onFireWeapon(worm, weaponStruct, launchParams)
  if S.regen == 0 then return 0 end
  local team = teamFromWorm(worm)
  if not team then return 0 end
  local ok, wpn = pcall(function() return team.lastLaunchedWeapon_dword60 end)
  armRegenForTeam(team, ok and wpn or nil)
  return 0
end

local function onWeaponRelease(worm, posX, posY, angleX, angleY)
  if not S.fireArmed or S.regen == 0 then return 0 end
  if S.pendingTeam and isValidTask(S.pendingTeam) and S.pendingWpn ~= nil and S.pendingWpn >= 0 then
    S.fireArmed = false
    queueRegen(S.pendingTeam, S.pendingWpn)
  end
  return 0
end

function onTurnStarted(team)
  local t = getTeamEntry(team)
  if not t then return end
  -- StartTurn/TurnStarted is broadcast to the CTaskTeam AND every worm in it, so this can
  -- fire 5-8x for the SAME real turn. Debounce per-team so the heavy work below (full
  -- ammo/delay capture + 128-slot snapshot) runs once per real turn, not once per worm.
  local now = type(getTickCount) == "function" and getTickCount() or nil
  if now and t.turnStartTick and (now - t.turnStartTick) < 500 then return end
  if now then t.turnStartTick = now end
  t.snap = false
  t.ammo0 = {}
  t.delay0 = {}
  t.postShotDelay = {}
  t.ammoAtFire = {}
  -- Match-scheme capture BEFORE delay-down (needs raw scheme delays on panel).
  if S.schemePendingMemory then
    captureMatchScheme(team)
  end
  -- Per-team TurnStarted snap = probe only (ready weapons often delay0=0).
  snapshotScheme(team, t)
  applyDelayDown(team, t)
  applyNextTurnDelays(team, t)
end

function onWeaponFinished(team)
  if not isValidTask(team) then return end
  registerTeam(team)
  local t = getTeamEntry(team)
  if not t then return end
  local ok, wpn = pcall(function() return team.lastLaunchedWeapon_dword60 end)
  if not ok or wpn == nil or wpn < 0 then return end
  local _, delayAddr = snapshotAddrs(team, wpn)
  notePostShotDelay(team, t, wpn, delayAddr)
  local live = getDelay(team, wpn)
  if live > 0 then
    captureKnownReload(t, wpn, live)
    regenDebugMsg("weaponFinished wpn=%d live=%d lastShot=%d", wpn, live, lastShotDelay(team, wpn))
  end
end

function onTurnFinished(team)
  if S.regen == 0 or not isValidTask(team) then return end
  local t = getTeamEntry(team)
  if not t then return end
  for w = 0, WEAPON_SLOTS - 1 do
    notePostShotDelay(team, t, w)
  end
  if S.regen == 2 and t.regen2NeedDelay then
    for wpn, need in pairs(t.regen2NeedDelay) do
      if need then
        local target = select(1, regenDelayTarget(team, t, wpn, nil))
        if target > 0 and getAmmo(team, wpn) > 0 then
          setDelay(team, wpn, target)
          if getDelay(team, wpn) == target then
            t.regen2NeedDelay[wpn] = nil
            regenDebugMsg("regen2 turn-end wpn=%d delay=%d", wpn, target)
          end
        end
      end
    end
  end
end

local function onGameGlobalInit()
  S.teams = {}
  S.lastRateTick = 0
  S.fireArmed = false
  S.pendingWpn = nil
  S.pendingTeam = nil
  S.pendingRegen = nil
  S.discoverTick = 0
  S.activeWorm = nil
  S.activeAddr = 0
  -- Do NOT rebuild scheme index here (io.popen hitch). Index is warm from initialize /
  -- /delay schemes. Auto-detect reads from cache only.
  if schemeBaselinePersists() then
    S.schemePendingMemory = false
    S.schemeAutoDetectDone = true
    log("new match: keep scheme baseline [%s] %s", S.schemeSource or "?", S.schemePath or "?")
  else
    clearSchemeTables()
    S.schemeReload = {}
    S.schemePendingMemory = true
    S.schemeMemorySnapshot = nil
    S.schemeAutoDetectDone = false
    S.schemeCaptureFirstTick = nil
    S.schemeCaptureAttempts = 0
    log("new match: waiting match-memory + auto scheme detect")
  end
end

function getStatusMessage()
  local m = S._statusMsg
  S._statusMsg = nil
  return m or ""
end

function getSettingsSummary()
  local mode = ({[0] = "off", [1] = "delay", [2] = "delay rt"})[S.regen] or tostring(S.regen)
  local rate = S.delayRateSec > 0
      and string.format("rate=1/%ds", S.delayRateSec) or "rate=off"
  local down = S.delayDown > 0 and ("down=" .. S.delayDown) or "down=off"
  local api = S.apiOk and "api=ok" or "api=missing"
  local scheme = S.schemeLoaded and schemeStatusLine() or "scheme=pending"
  return string.format("regen=%s %s %s %s | %s", mode, rate, down, api, scheme)
end

local function firstTeam()
  for _, t in pairs(S.teams) do
    if t.task and isValidTask(t.task) then return t.task end
  end
  return nil
end

local function ensureTeamsDiscovered()
  if teamCount() > 0 then return teamCount() end
  if not hasTurnGame() then return 0 end
  return discoverTeams()
end

local function activeTeam()
  if S.activeWorm and isValidTask(S.activeWorm) then
    local team = teamFromWorm(S.activeWorm)
    if team then
      registerTeam(team)
      return team
    end
  end
  ensureTeamsDiscovered()
  return firstTeam()
end

local function cmdFeedback()
  ensureTeamsDiscovered()
  return debugStatus(activeTeam())
end

local function validWeaponId(wpn)
  return wpn ~= nil and wpn >= 0 and wpn < WEAPON_SLOTS
end

local function probeDelayLine(label, team, wpn)
  local teamDelayAddr, panelDelayAddr = nil, nil
  if hasRawMem() then
    _, teamDelayAddr = snapshotAddrs(team, wpn)
    _, panelDelayAddr = snapshotPanelAddrs(team, wpn)
  end
  local api = getDelay(team, wpn)
  local teamMem = teamDelayAddr and rawRead(teamDelayAddr, nil) or "n/a"
  local panelMem = panelDelayAddr and rawRead(panelDelayAddr, nil) or "n/a"
  return string.format(
    "%s: API=%d teamMem[%s]=%s panelMem[%s]=%s teamSlot=%s panelSlot=%s alliance=%s",
    label, api,
    teamDelayAddr and string.format("%X", teamDelayAddr) or "?",
    teamMem ~= nil and tostring(teamMem) or "err",
    panelDelayAddr and string.format("%X", panelDelayAddr) or "?",
    panelMem ~= nil and tostring(panelMem) or "err",
    teamSlotOf(team), panelSlotOf(team), allianceOf(team))
end

local function probeSetDelay(team, wpn, n)
  if not S.apiOk then
    return false, "weapon API missing - rebuild wkJellyWorm.dll"
  end
  if not team or not isValidTask(team) then
    return false, "no team found - start a match and move your worm"
  end
  if not validWeaponId(wpn) then
    return false, string.format("invalid weapon id %s (use 0-%d)", tostring(wpn), WEAPON_SLOTS - 1)
  end
  if n == nil then
    return false, "missing delay value"
  end
  n = math.floor(n)
  chat(probeDelayLine("before", team, wpn))

  setDelay(team, wpn, n)
  local afterApi = getDelay(team, wpn)

  local _, teamDelayAddr = snapshotAddrs(team, wpn)
  local _, panelDelayAddr = snapshotPanelAddrs(team, wpn)
  local wrote = {}
  if teamDelayAddr and teamDelayAddr ~= 0 then
    if rawWrite(team, teamDelayAddr, n, nil) then wrote[#wrote + 1] = "teamAddr" end
  end
  if panelDelayAddr and panelDelayAddr ~= 0 and panelDelayAddr ~= teamDelayAddr then
    if rawWrite(team, panelDelayAddr, n, nil) then wrote[#wrote + 1] = "panelAddr" end
  end

  chat(probeDelayLine("after", team, wpn))
  local panelVal = panelDelayAddr and rawRead(panelDelayAddr, nil) or nil
  local teamVal = teamDelayAddr and rawRead(teamDelayAddr, nil) or nil
  local paths = (#wrote > 0) and table.concat(wrote, "+") or "apiOnly"

  if panelVal == n then
    return true, string.format("weapon %d delay=%d (panel shows %d via %s)", wpn, afterApi, panelVal, paths)
  end
  if afterApi == n then
    return false, string.format(
      "API=%d but panelMem=%s (teamMem=%s) — panel slot %s vs team slot %s",
      afterApi, tostring(panelVal), tostring(teamVal), panelSlotOf(team), teamSlotOf(team))
  end
  return false, string.format("write failed: wanted %d API=%d panelMem=%s teamMem=%s", n, afterApi, tostring(panelVal), tostring(teamVal))
end

function onChatInput(msg)
  if not msg or msg == "" then return 0 end
  if msg:sub(1, 1) ~= "/" then msg = "/" .. msg end

  local tail = msg:match("^/delay%s*(.*)$")
  if tail == nil then return 0 end

  -- Preserve path case for /delay scheme <path>
  local schemePathArg = tail:match("^[Ss][Cc][Hh][Ee][Mm][Ee]%s+(.+)$")
  if schemePathArg then
    schemePathArg = schemePathArg:match("^%s*(.-)%s*$")
  end

  local args = {}
  for a in tail:gmatch("%S+") do args[#args + 1] = a:lower() end
  local cmd = args[1] or ""

  if cmd == "" then
    S.regen = 1
    discoverTeams()
    ensureSchemeLoaded(true)
    chat("delay: regen=1 (auto .wsc detect / memory baseline) | " .. cmdFeedback())
  elseif cmd == "rt" then
    S.regen = 2
    discoverTeams()
    ensureSchemeLoaded(true)
    chat("delay: regen=2 (ammo+delay; never ammo=0) | " .. cmdFeedback())
    chat("auto: .wsc detected from User/Schemes by ammo on match start; optional /delay scheme override")
    chat("tip: /delay probe 1")
  elseif cmd == "scheme" then
    local raw = schemePathArg or args[2]
    if not raw or raw == "" then
      if S.schemeLoaded then
        chat(schemeStatusLine())
      else
        chat("usage: /delay scheme <path.wsc>  (optional override; default = auto-detect by ammo)")
      end
    else
      local path = resolveSchemePath(raw)
      local ok, msg = loadSchemeFile(path)
      if ok then
        chat(string.format("%s | %s", schemeStatusLine(), msg))
      else
        chat(string.format("scheme load failed: %s (%s)", tostring(msg), tostring(path)))
      end
    end
  elseif cmd == "schemes" then
    chatSchemeFileList()
  elseif cmd == "off" or cmd == "0" then
    S.regen = 0
    chat("delay: regen off | " .. cmdFeedback())
  elseif cmd == "rate" then
    if args[2] == "off" then
      S.delayRateSec = 0
      S.lastRateTick = 0
      chat("delay rate: off | " .. cmdFeedback())
    else
      local n = tonumber(args[2]) or 1
      if n <= 0 then
        chat("usage: /delay rate [sec]  |  /delay rate off  (default 1 sec)")
      else
        S.delayRateSec = math.floor(n)
        S.lastRateTick = 0
        discoverTeams()
        forEachTeam(function(team) adjustTeamDelays(team, 1) end)
        chat(string.format("delay rate: -1 every %d sec | %s", S.delayRateSec, cmdFeedback()))
      end
    end
  elseif cmd == "down" then
    if args[2] == "off" then
      S.delayDown = 0
      chat("delay down: off | " .. cmdFeedback())
    else
      local n = tonumber(args[2]) or 10
      if n <= 0 then
        chat("usage: /delay down [N]  |  /delay down off  (default 10)")
      else
        S.delayDown = math.floor(n)
        discoverTeams()
        forEachTeam(function(team, t)
          t.delayDownDone = false
          applyDelayDown(team, t)
        end)
        chat(string.format("delay down: -%d applied | %s", S.delayDown, cmdFeedback()))
      end
    end
  elseif cmd == "reload" then
    local wpn = tonumber(args[2])
    local n = tonumber(args[3])
    if wpn == nil or n == nil or not validWeaponId(wpn) or n <= 0 then
      chat("usage: /delay reload <wpn_id> <N>  (fallback reload when capture fails)")
    else
      wpn = math.floor(wpn)
      n = math.floor(n)
      S.schemeReload[wpn] = n
      forEachTeam(function(team, t)
        if not t.knownReload then t.knownReload = {} end
        t.knownReload[wpn] = n
      end)
      chat(string.format("delay reload: wpn=%d fallback=%d | %s", wpn, n, cmdFeedback()))
    end
  elseif cmd == "probe" then
    local wpn = tonumber(args[2])
    if wpn == nil then
      wpn = lastShotWeaponFromCounters()
      if wpn then
        chat(string.format("probe: using last shot weapon wpn=%d%s", wpn, weaponHint(wpn)))
      end
    end
    local isSet = args[3] == "set"
    if wpn == nil or (isSet and args[4] == nil) then
      chat("usage: /delay probe <wpn_id>  |  /delay probe <wpn> set <N>  (Bazooka=1, not 0)")
    elseif wpn == 0 then
      chat("probe: weapon id 0 is unused on WA panel — use 1 for Bazooka, 6 for Grenade")
    elseif not validWeaponId(wpn) then
      chat(string.format("probe: invalid weapon id %s (use 0-%d)", tostring(wpn), WEAPON_SLOTS - 1))
    else
      wpn = math.floor(wpn)
      ensureTeamsDiscovered()
      local team = activeTeam()
      if isSet then
        local n = tonumber(args[4])
        if n == nil then
          chat("usage: /delay probe <wpn> set <N>")
        else
          local ok, msg = probeSetDelay(team, wpn, n)
          chat(ok and ("probe set: " .. msg) or ("probe set failed: " .. msg))
        end
      elseif not team then
        chat("probe failed: no team found - start a match and move your worm")
      else
        local ammoAddrStr, delayAddrStr, panelDelayAddrStr = "?", "?", "?"
        if hasRawMem() then
          local ok1, a = pcall(getTeamWeaponAddr, team, wpn, 0)
          local ok2, d = pcall(getTeamWeaponAddr, team, wpn, 1)
          local ok3, pd = pcall(getPanelWeaponAddr, team, wpn, 1)
          ammoAddrStr = (ok1 and a) and string.format("%X", a) or "err"
          delayAddrStr = (ok2 and d) and string.format("%X", d) or "err"
          panelDelayAddrStr = (ok3 and pd) and string.format("%X", pd) or "err"
        end
        local panel = "?"
        if type(getWeaponPanelFromTeam) == "function" then
          local ok, p = pcall(getWeaponPanelFromTeam, team)
          panel = ok and string.format("%X", p or 0) or "err"
        end
        local counters = "?"
        if type(getCustomWeaponsDebugCounters) == "function" then
          local ok, c = pcall(getCustomWeaponsDebugCounters)
          counters = ok and c or "err"
        end
        chat(string.format(
          "probe wpn=%d%s team=%X panel=%s teamSlot=%s panelSlot=%s alliance=%s ammo=%d delay=%d lastShot=%d teamDelayAddr=%s panelDelayAddr=%s ammoAddr=%s",
          wpn, weaponHint(wpn), team:getAddr(), panel, teamSlotOf(team), panelSlotOf(team), allianceOf(team),
          getAmmo(team, wpn), getDelay(team, wpn), lastShotDelay(team, wpn), delayAddrStr, panelDelayAddrStr, ammoAddrStr))
        if S.schemeLoaded then
          chat(string.format("%s | wpn schemeAmmo=%s schemeDelay=%s",
            schemeStatusLine(), tostring(S.schemeAmmo[wpn]), tostring(S.schemeDelay[wpn])))
        elseif S.schemePendingMemory then
          chat("scheme: pending match-memory capture (start/wait your turn)")
        else
          chat("scheme not loaded — start a match or /delay scheme <path.wsc>")
        end
        local teamEntry = getTeamEntry(team)
        if teamEntry and teamEntry.snap then
          chat(string.format("panel snapshot (TurnStarted, probe only): ammo0=%d delay0=%d",
            teamEntry.ammo0[wpn] or 0, teamEntry.delay0[wpn] or 0))
        end
        local liveDelay = getDelay(team, wpn)
        if liveDelay < 0 then
          chat(string.format("delay=%d: negative delay = weapon unavailable", liveDelay))
        end
        local p = S.pendingRegen
        if p and p.wpn == wpn then
          chat(string.format("pending: wait=%d retry=%d ammoDone=%s peakMax=%d ammoAddr=%s delayAddr=%s",
            p.wait, p.retry, tostring(p.ammoDone), p.peakMax or 0,
            p.ammoAddr and string.format("%X", p.ammoAddr) or "nil",
            p.delayAddr and string.format("%X", p.delayAddr) or "nil"))
        end
      end
    end
  elseif cmd == "debug" then
    local arg = args[2]
    if arg == "on" then
      S.regenDebug = true
    elseif arg == "off" then
      S.regenDebug = false
    else
      S.regenDebug = not S.regenDebug
    end
    chat(string.format("regen debug log: %s", S.regenDebug and "on" or "off"))
  else
    chat("usage: /delay | rt | off | scheme <path.wsc> | schemes | rate [sec] | down [N] | reload <wpn> <N> | probe <wpn> [set N] | debug [on|off]")
  end
  return 1
end

function writeConfig()
  local path = ((S.schemeSource == "file" or S.schemeSource == "auto") and S.schemePath) or nil
  if path == "match-memory" or path == "host-sync" then path = nil end
  local cfg = {
    regen = S.regen,
    delayRateSec = S.delayRateSec,
    delayDown = S.delayDown,
    schemePath = path,
    schemeVersion = S.schemeVersion,
    schemeSource = S.schemeSource,
  }
  if S.schemeLoaded then
    local ammoOut, delayOut = schemeTablesForJson()
    if next(ammoOut) or next(delayOut) then
      cfg.schemeAmmo = ammoOut
      cfg.schemeDelay = delayOut
    end
  end
  return json.encode(cfg)
end

function readConfig(config)
  local ok, cfg = pcall(json.decode, config)
  if ok and type(cfg) == "table" then
    if cfg.regen ~= nil then S.regen = cfg.regen end
    if cfg.delayRateSec ~= nil then S.delayRateSec = cfg.delayRateSec
    elseif cfg.delayRate ~= nil then S.delayRateSec = cfg.delayRate end
    if cfg.delayDown ~= nil then S.delayDown = cfg.delayDown end
    if applySchemeFromJsonTables(cfg) then
      S.schemeAutoDetectDone = true
      chat(string.format("scheme[sync]: from host — %s", schemeStatusLine()))
    elseif type(cfg.schemePath) == "string" and cfg.schemePath ~= ""
        and cfg.schemePath ~= "match-memory" and cfg.schemePath ~= "host-sync" then
      S.schemePath = cfg.schemePath
    end
  end
  return 0
end

function resetConfig()
  S.regen = 0
  S.delayRateSec = 0
  S.delayDown = 0
  clearSchemeTables()
  S.schemeReload = {}
  S.schemePendingMemory = true
  S.schemePath = nil
  S.schemeMemorySnapshot = nil
  S.schemeAutoDetectDone = false
  S.schemeCaptureFirstTick = nil
  S.schemeCaptureAttempts = 0
  return 0
end

function lobbyHostCli(lobbyObj, command, args) return 0 end
function lobbyClientCli(lobbyObj, command, args) return 0 end

function initialize()
  S.apiOk = hasWeaponApi()
  if not S.apiOk then
    chat("weapon API missing - rebuild wkJellyWorm.dll")
  end
  local ok, err = pcall(function()
    CTaskTeam_RegisterCallback_vtable8(teamcallback)
    CTaskWorm_RegisterCallback_vtable8(wormcallback)
    registerCallback_fireWeapon(onFireWeapon)
    registerCallback_weaponRelease(onWeaponRelease)
    registerCallback_gameGlobalInit(onGameGlobalInit)
  end)
  if not ok then
    chat("init error: " .. tostring(err))
    return 1
  end
  S.schemePendingMemory = true
  S.schemeAutoDetectDone = false
  -- Build scheme index once at enable (lobby) — not during match frames.
  pcall(buildSchemeIndex, true)
  log("jellydelay ready api=%s regen=%d rate=%d down=%d (scheme=auto-detect on match start)",
    tostring(S.apiOk), S.regen, S.delayRateSec, S.delayDown)
  return 0
end
