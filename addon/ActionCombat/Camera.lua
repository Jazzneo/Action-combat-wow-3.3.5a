-- ActionCombat camera: better control while the right mouse button is held (mouselook).
--   * aiming reticle in the middle of the screen (red when your target is an enemy),
--     with the soft-lock target's name and health under it
--   * faster, sharper camera turning (configurable sensitivity)
--   * camera auto-follow is switched off so it doesn't fight your mouse
-- Everything goes back to your own settings the moment you let go.

local DEFAULTS = { camEnabled = true, reticle = true, sens = 1.4, freeCursor = true }

local function DB()
    ActionCombatDB = ActionCombatDB or {}
    for k, v in pairs(DEFAULTS) do
        if ActionCombatDB[k] == nil then ActionCombatDB[k] = v end
    end
    return ActionCombatDB
end

-- CVars differ a little between clients, so only touch the ones that exist.
local function GetCV(name)
    local ok, v = pcall(GetCVar, name)
    if ok then return v end
end
local function SetCV(name, value)
    if GetCV(name) ~= nil then pcall(SetCVar, name, value) end
end

------------------------------------------------------------------------ reticle

local ret = CreateFrame("Frame", "ActionCombatReticle", UIParent)
ret:SetSize(40, 40)
ret:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
ret:SetFrameStrata("HIGH")
ret:Hide()

local bars = {}
local function Bar(w, h, x, y)
    local t = ret:CreateTexture(nil, "OVERLAY")
    t:SetTexture(1, 1, 1, 0.9)
    t:SetSize(w, h)
    t:SetPoint("CENTER", ret, "CENTER", x, y)
    bars[#bars + 1] = t
    return t
end
-- four ticks with a gap, plus a center dot
Bar(10, 2, -12, 0); Bar(10, 2, 12, 0); Bar(2, 10, 0, 12); Bar(2, 10, 0, -12)
Bar(3, 3, 0, 0)

local retName = ret:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
retName:SetPoint("TOP", ret, "BOTTOM", 0, -6)

local retHp = CreateFrame("StatusBar", nil, ret)
retHp:SetSize(70, 4)
retHp:SetPoint("TOP", retName, "BOTTOM", 0, -2)
retHp:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
retHp:SetStatusBarColor(0.85, 0.15, 0.15)
retHp:SetMinMaxValues(0, 1)
local retHpBg = retHp:CreateTexture(nil, "BACKGROUND")
retHpBg:SetAllPoints()
retHpBg:SetTexture(0, 0, 0, 0.6)

-- interact prompt under the reticle: "[F] Loot  Kobold Miner"
local prompt = ret:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
prompt:SetPoint("TOP", ret, "BOTTOM", 0, -26)
prompt:Hide()

local VERBS = { LOOT = "Loot", TALK = "Talk to", TRADE = "Trade with", USE = "Use" }

function ActionCombat_SetPrompt(verb, name)
    if not verb or verb == "NONE" or not VERBS[verb] then
        prompt:Hide()
        return
    end
    local key = GetBindingKey("ACM_INTERACT")
    local keyText = key and ("|cffffd100[" .. GetBindingText(key, "KEY_") .. "]|r ") or ""
    prompt:SetText(keyText .. VERBS[verb] .. "  |cffffffff" .. (name or "") .. "|r")
    prompt:Show()
end

local function UpdateReticle()
    local enemy = UnitExists("target") and UnitCanAttack("player", "target") and not UnitIsDead("target")
    local r, g, b = 1, 1, 1
    if enemy then r, g, b = 1, 0.25, 0.2 end
    for _, t in ipairs(bars) do t:SetVertexColor(r, g, b) end

    if enemy then
        retName:SetText(UnitName("target"))
        local max = UnitHealthMax("target")
        retHp:SetValue(max > 0 and UnitHealth("target") / max or 0)
        retName:Show(); retHp:Show()
    else
        retName:Hide(); retHp:Hide()
    end
end

------------------------------------------------------------------------ camera mode

local active = false
local saved = nil
local TOUCHED = { "cameraYawMoveSpeed", "cameraPitchMoveSpeed", "cameraSmoothStyle", "cameraSmoothTrackingStyle" }

local function Enter()
    local db = DB()
    saved = {}
    for _, cv in ipairs(TOUCHED) do saved[cv] = GetCV(cv) end
    db.savedCVars = saved                      -- survives a crash / logout mid-hold

    local yaw, pitch = tonumber(saved.cameraYawMoveSpeed), tonumber(saved.cameraPitchMoveSpeed)
    if yaw then SetCV("cameraYawMoveSpeed", yaw * db.sens) end
    if pitch then SetCV("cameraPitchMoveSpeed", pitch * db.sens) end
    SetCV("cameraSmoothStyle", "0")           -- never auto-swing behind you while aiming
    SetCV("cameraSmoothTrackingStyle", "0")

    if db.reticle then UpdateReticle(); ret:Show() end
    active = true
end

local function Leave()
    if saved then
        for cv, v in pairs(saved) do SetCV(cv, v) end
    end
    saved = nil
    DB().savedCVars = nil
    ret:Hide()
    prompt:Hide()
    active = false
end

local watcher = CreateFrame("Frame")
watcher:RegisterEvent("PLAYER_LOGIN")
watcher:RegisterEvent("PLAYER_LOGOUT")
watcher:RegisterEvent("PLAYER_TARGET_CHANGED")
watcher:RegisterEvent("UNIT_HEALTH")
watcher:RegisterEvent("LOOT_OPENED")
-- windows that need the mouse: give the cursor back when they open while you hold the camera
local CURSOR_EVENTS = {
    "GOSSIP_SHOW", "MERCHANT_SHOW", "QUEST_GREETING", "QUEST_DETAIL", "QUEST_PROGRESS",
    "QUEST_COMPLETE", "TRAINER_SHOW", "TAXIMAP_OPENED", "BANKFRAME_OPENED",
    "AUCTION_HOUSE_SHOW", "PET_STABLE_SHOW", "MAIL_SHOW", "LOOT_BIND_CONFIRM",
}
for _, e in ipairs(CURSOR_EVENTS) do watcher:RegisterEvent(e) end
watcher:SetScript("OnEvent", function(_, event, unit)
    if event == "PLAYER_LOGIN" then
        -- restore settings if the game closed while the button was held
        local db = DB()
        if db.savedCVars then
            for cv, v in pairs(db.savedCVars) do SetCV(cv, v) end
            db.savedCVars = nil
        end
    elseif event == "LOOT_OPENED" then
        -- a normal mouse loot: give the cursor back so the loot window can be clicked
        -- (the Interact key loots on the server and never needs the window)
        if DB().freeCursor and IsMouselooking() then MouselookStop() end
    elseif event ~= "PLAYER_TARGET_CHANGED" and event ~= "UNIT_HEALTH" and event ~= "PLAYER_LOGOUT" then
        -- a gossip / vendor / quest / ... window opened: free the cursor so you can click in it
        if DB().freeCursor and IsMouselooking() then MouselookStop() end
    end
    if event == "PLAYER_LOGOUT" then
        if active then Leave() end
    elseif active and (event == "PLAYER_TARGET_CHANGED" or (event == "UNIT_HEALTH" and unit == "target")) then
        UpdateReticle()
    end
end)

local peekTimer = 0
watcher:SetScript("OnUpdate", function(_, elapsed)
    local db = DB()
    -- while aiming, ask the server what's in front of the crosshair a few times a second
    if active and ActionCombat and ActionCombat.Send and GetBindingKey("ACM_INTERACT") then
        peekTimer = peekTimer - elapsed
        if peekTimer <= 0 then
            peekTimer = 0.3
            local f = GetPlayerFacing and GetPlayerFacing()
            ActionCombat.Send("PEEK " .. (f and string.format("%.3f", f) or ""))
        end
    end
    local holding = db.camEnabled and IsMouseButtonDown("RightButton") and IsMouselooking()
    if holding and not active then
        Enter()
    elseif not holding and active then
        Leave()
    end
end)

------------------------------------------------------------------------ slash options (called from /acm)

function ActionCombat_CameraCommand(args)
    local db = DB()
    local cmd, val = strsplit(" ", args or "", 2)
    if cmd == "on" or cmd == "off" then
        db.camEnabled = cmd == "on"
        if not db.camEnabled and active then Leave() end
    elseif cmd == "sens" and tonumber(val) then
        db.sens = math.max(0.5, math.min(3, tonumber(val)))
    elseif cmd == "reticle" and (val == "on" or val == "off") then
        db.reticle = val == "on"
    elseif cmd == "freecursor" and (val == "on" or val == "off") then
        db.freeCursor = val == "on"
    else
        DEFAULT_CHAT_FRAME:AddMessage("/acm cam on | off          - right-mouse action camera")
        DEFAULT_CHAT_FRAME:AddMessage("/acm cam sens <0.5-3>      - turn speed while holding right mouse")
        DEFAULT_CHAT_FRAME:AddMessage("/acm cam reticle on | off  - aiming crosshair")
        DEFAULT_CHAT_FRAME:AddMessage("/acm cam freecursor on | off - show the cursor when a vendor/quest/loot window opens")
    end
    DEFAULT_CHAT_FRAME:AddMessage(string.format("|cff4fc3f7Action Combat|r camera: %s, sensitivity %.1fx, reticle %s, free cursor %s",
        db.camEnabled and "on" or "off", db.sens, db.reticle and "on" or "off", db.freeCursor and "on" or "off"))
end
