-- ActionCombat client addon (WoW 3.3.5a) for mod-action-combat
-- Sends button presses to the server and draws the stamina bar / combat feedback.

local PREFIX = "ACM"
local AC = CreateFrame("Frame", "ActionCombatFrame", UIParent)
ActionCombat = AC

-- Key binding labels (Esc > Key Bindings > ActionCombat)
BINDING_HEADER_ACTIONCOMBAT = "Action Combat"
BINDING_NAME_ACM_LIGHT = "Light attack (tap for combo)"
BINDING_NAME_ACM_HEAVY = "Heavy attack (hold to charge)"
BINDING_NAME_ACM_DODGE = "Dodge roll / backstep"
BINDING_NAME_ACM_GUARD = "Guard (hold) / perfect parry (tap on hit)"
BINDING_NAME_ACM_SPRINT = "Sprint (hold while moving)"
BINDING_NAME_ACM_SHOT = "Quick shot (bow / gun / crossbow)"
BINDING_NAME_ACM_INTERACT = "Interact: loot / talk / use what's in front of you"
BINDING_NAME_ACM_ELEMENT = "Switch magic element (next)"
BINDING_NAME_ACM_AIMSHOT = "Aimed shot (hold to draw, release to fire)"

------------------------------------------------------------------------ comms

local function Send(msg)
    if AC.debug then DEFAULT_CHAT_FRAME:AddMessage("|cff888888[ACM >] " .. msg .. "|r") end
    SendAddonMessage(PREFIX, msg, "WHISPER", UnitName("player"))
end
AC.Send = Send

local function Facing()
    local f = GetPlayerFacing and GetPlayerFacing()
    return f and string.format("%.3f", f) or ""
end

------------------------------------------------------------------------ movement tracking (for 8-way roll)

local move = { F = false, B = false, L = false, R = false }
local function hook(fn, key, state)
    if _G[fn] then hooksecurefunc(fn, function() move[key] = state end) end
end
hook("MoveForwardStart", "F", true);   hook("MoveForwardStop", "F", false)
hook("MoveBackwardStart", "B", true);  hook("MoveBackwardStop", "B", false)
hook("StrafeLeftStart", "L", true);    hook("StrafeLeftStop", "L", false)
hook("StrafeRightStart", "R", true);   hook("StrafeRightStop", "R", false)

local function MoveDir()
    local v = (move.F and not move.B) and "F" or (move.B and not move.F) and "B" or ""
    local h = (move.L and not move.R) and "L" or (move.R and not move.L) and "R" or ""
    local d = v .. h
    return d ~= "" and d or "N"
end

------------------------------------------------------------------------ UI: stamina bar

local holder = CreateFrame("Frame", "ActionCombatStamina", UIParent)
holder:SetSize(260, 10)
holder:SetPoint("BOTTOM", UIParent, "BOTTOM", 0, 190)
holder:Hide()

local bg = holder:CreateTexture(nil, "BACKGROUND")
bg:SetAllPoints()
bg:SetTexture(0, 0, 0, 0.55)

local function MakeBar(level, r, g, b, a)
    local sb = CreateFrame("StatusBar", nil, holder)
    sb:SetAllPoints()
    sb:SetFrameLevel(holder:GetFrameLevel() + level)
    sb:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
    sb:SetStatusBarColor(r, g, b, a)
    sb:SetMinMaxValues(0, 100)
    sb:SetValue(100)
    return sb
end
-- "ghost" bar that lags behind, like Elden Ring's chip damage
local ghost = MakeBar(1, 0.9, 0.8, 0.3, 0.6)
local bar = MakeBar(2, 0.35, 0.85, 0.35, 1)

local border = CreateFrame("Frame", nil, holder)
border:SetFrameLevel(holder:GetFrameLevel() + 3)
border:SetPoint("TOPLEFT", -3, 3)
border:SetPoint("BOTTOMRIGHT", 3, -3)
border:SetBackdrop({ edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", edgeSize = 10 })
border:SetBackdropBorderColor(0.6, 0.6, 0.6, 0.9)

-- heavy-attack charge bar
local charge = CreateFrame("StatusBar", nil, UIParent)
charge:SetSize(120, 5)
charge:SetPoint("TOP", holder, "BOTTOM", 0, -6)
charge:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
charge:SetStatusBarColor(1, 0.55, 0.1)
charge:SetMinMaxValues(0, 1)
charge:Hide()
local chargeBg = charge:CreateTexture(nil, "BACKGROUND")
chargeBg:SetAllPoints()
chargeBg:SetTexture(0, 0, 0, 0.5)

-- guard icon
local guard = border:CreateTexture(nil, "OVERLAY")
guard:SetSize(18, 18)
guard:SetPoint("RIGHT", holder, "LEFT", -6, 0)
guard:SetTexture("Interface\\Icons\\INV_Shield_06")
guard:Hide()

-- current magic element: icon + name above the left end of the bar
local elemIcon = border:CreateTexture(nil, "OVERLAY")
elemIcon:SetSize(22, 22)
elemIcon:SetPoint("BOTTOMLEFT", holder, "TOPLEFT", 0, 4)
elemIcon:Hide()
local elemText = border:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
elemText:SetPoint("LEFT", elemIcon, "RIGHT", 4, 0)
elemText:Hide()

local ELEMENT_COLORS = {
    Fire = { 1, 0.45, 0.1 },   Frost = { 0.45, 0.8, 1 },  Arcane = { 0.85, 0.5, 1 },
    Shadow = { 0.6, 0.3, 0.9 }, Holy = { 1, 0.9, 0.45 },  Lightning = { 0.4, 0.65, 1 },
    Nature = { 0.4, 0.9, 0.35 }, Lunar = { 0.75, 0.8, 1 },
}
local currentElement = nil

-- sprint icon (right side of the bar)
local sprintIcon = border:CreateTexture(nil, "OVERLAY")
sprintIcon:SetSize(18, 18)
sprintIcon:SetPoint("LEFT", holder, "RIGHT", 6, 0)
sprintIcon:SetTexture("Interface\\Icons\\Ability_Rogue_Sprint")
sprintIcon:Hide()

-- big center feedback text
local feedback = UIParent:CreateFontString(nil, "OVERLAY")
feedback:SetFont("Fonts\\MORPHEUS.ttf", 34, "OUTLINE")
feedback:SetPoint("CENTER", UIParent, "CENTER", 0, 140)
feedback:SetAlpha(0)

-- full-screen flash for parries / guard breaks
local flash = UIParent:CreateTexture(nil, "BACKGROUND")
flash:SetAllPoints(UIParent)
flash:SetTexture(1, 1, 1)
flash:SetBlendMode("ADD")
flash:SetAlpha(0)

local feedbackT, flashT = 0, 0
local function Say(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cff4fc3f7Action Combat|r " .. text)
end

local function Feedback(text, r, g, b, doFlash)
    feedback:SetText(text)
    feedback:SetTextColor(r, g, b)
    feedbackT = 0.9
    if doFlash then
        flash:SetVertexColor(r, g, b)
        flashT = 0.25
    end
end

------------------------------------------------------------------------ state

local stamina, staminaMax, ghostVal = 100, 100, 100
local heavyDown, heavyStart = false, 0
local aimDown, aimStart = false, 0
local MAX_CHARGE = 1.5
local magicMode = false
local rangedMode = false

-- Melee and ranged styles use the server's stamina; magic style shows real mana.
local function SetMode(mode)
    local magic = mode == "MAGIC"
    if not magic then elemIcon:Hide(); elemText:Hide() end
    magicMode, rangedMode = magic, mode == "RANGED"
    if magic then
        local c = currentElement and ELEMENT_COLORS[currentElement]
        if c then bar:SetStatusBarColor(c[1] * 0.7, c[2] * 0.7, c[3] * 0.9) else bar:SetStatusBarColor(0.25, 0.5, 1) end
        guard:SetTexture("Interface\\Icons\\Spell_Shadow_DetectLesserInvisibility")
    else
        bar:SetStatusBarColor(0.35, 0.85, 0.35)
        guard:SetTexture("Interface\\Icons\\INV_Shield_06")
        stamina, staminaMax = 100, 100
        ghost:SetMinMaxValues(0, staminaMax); bar:SetMinMaxValues(0, staminaMax)
    end
    ghostVal = stamina
end

local function ReadMana()
    stamina, staminaMax = UnitPower("player", 0), math.max(1, UnitPowerMax("player", 0))
    bar:SetMinMaxValues(0, staminaMax); ghost:SetMinMaxValues(0, staminaMax)
    bar:SetValue(stamina)
    if stamina > ghostVal then ghostVal = stamina end
end

local EVENTS = {
    PARRY      = function() Feedback("PARRY", 1, 0.85, 0.3, true); PlaySoundFile("Sound\\Interface\\iAbilitiesTurnOn.wav") end,
    STAGGER    = function() Feedback("STAGGER", 1, 0.5, 0.2) end,
    RIPOSTE    = function() Feedback("CRITICAL", 1, 0.2, 0.2, true) end,
    EVADE      = function() Feedback("EVADED", 0.6, 0.9, 1) end,
    GUARDBREAK = function() Feedback("GUARD BROKEN", 0.9, 0.2, 0.2, true) end,
    NOSTAM     = function() Feedback("Out of stamina", 0.7, 0.7, 0.7); UIFrameFlash(holder, 0.1, 0.1, 0.4, true, 0, 0) end,
    HEAVY_FULL = function() Feedback("!", 1, 0.55, 0.1) end,
    GUARD_ON   = function() guard:Show() end,
    GUARD_OFF  = function() guard:Hide() end,
    ROLL       = function() guard:Hide(); if aimDown then aimDown = false; charge:Hide() end end,
    BACKSTEP   = function() guard:Hide() end,
    -- magic style
    BLINK      = function() guard:Hide() end,
    DEFLECT    = function() Feedback("DEFLECT", 0.6, 0.4, 1, true); PlaySoundFile("Sound\\Interface\\iAbilitiesTurnOn.wav") end,
    WARDBREAK  = function() Feedback("WARD SHATTERED", 0.5, 0.3, 1, true) end,
    NOMANA     = function() Feedback("Not enough mana", 0.5, 0.6, 1); UIFrameFlash(holder, 0.1, 0.1, 0.4, true, 0, 0) end,
    NOTARGET   = function() Feedback("No target in sight", 0.7, 0.7, 0.7) end,
    -- ranged style
    PIERCE     = function() Feedback("PIERCING SHOT", 1, 0.85, 0.4) end,
    TOOCLOSE   = function() Feedback("Too close - roll back!", 1, 0.6, 0.3) end,
    NOAMMO     = function() Feedback("Out of ammo", 0.9, 0.4, 0.3); UIFrameFlash(holder, 0.1, 0.1, 0.4, true, 0, 0) end,
    NOWEAPON   = function() Feedback("Equip a bow, gun or crossbow", 0.8, 0.8, 0.8) end,
    NOTHING    = function() Feedback("Nothing to interact with", 0.7, 0.7, 0.7) end,
    NOELEMENT  = function() AC.elemPending = nil; Feedback("You can't use that element", 0.7, 0.7, 0.7) end,
    ELEM_NOTMAGIC = function()
        AC.elemPending = nil
        Feedback("Elements need magic style", 0.7, 0.7, 1)
        Say("elements only work in |cff6f9fffmagic style|r. Type |cffffffff/acm magic|r to switch (Druids: leave Cat/Bear form).")
    end,
    ELEM_NOCLASS = function()
        AC.elemPending = nil
        Feedback("Your class has no elements", 0.7, 0.7, 0.7)
    end,
    ELEM_ONLYONE = function()
        AC.elemPending = nil
        Say("your class only has one element on this server (" .. (currentElement or "?") .. ").")
    end,
    ELEM_WAIT = function() AC.elemPending = nil end,
    NOLOOT     = function() Feedback("Can't loot that from here", 0.8, 0.6, 0.4) end,
    LOOTED     = function() if ActionCombat_SetPrompt then ActionCombat_SetPrompt("NONE") end end,
    SPRINT_ON  = function() sprintIcon:Show() end,
    SPRINT_OFF = function() sprintIcon:Hide() end,
    SWAP_MELEE = function() Feedback("Melee!", 0.9, 0.9, 0.9); PlaySoundFile("Sound\\Interface\\PickUp\\PickUpMetalLarge.wav") end,
    SWAP_RANGED= function() Feedback("Ranged", 1, 0.75, 0.4) end,
}

------------------------------------------------------------------------ bindings (called from Bindings.xml)

function ActionCombat_Light()
    Send("L " .. Facing())
end

function ActionCombat_Heavy(keystate)
    if keystate == "down" then
        heavyDown, heavyStart = true, GetTime()
        charge:SetValue(0); charge:Show()
        Send("HS")
    else
        if heavyDown then Send("HR " .. Facing()) end
        heavyDown = false
        if not aimDown then charge:Hide() end
    end
end

function ActionCombat_Dodge()
    Send("D " .. MoveDir() .. " " .. Facing())
end

function ActionCombat_Shot()
    Send("R " .. Facing())
end

function ActionCombat_AimShot(keystate)
    if keystate == "down" then
        aimDown, aimStart = true, GetTime()
        charge:SetValue(0); charge:Show()
        Send("RS")
    else
        if aimDown then Send("RR " .. Facing()) end
        aimDown = false
        if not heavyDown then charge:Hide() end
    end
end

function ActionCombat_Element(name)
    if not AC.serverVersion then
        Say("isn't active on this server yet (no answer to the addon). Is mod-action-combat installed and enabled?")
        Send("HELLO")
        return
    end
    if AC.serverVersion < 3 then
        Say("|cffff6060the server module is out of date|r (version " .. AC.serverVersion .. "). Rebuild the worldserver with the new mod-action-combat to use elements.")
        return
    end
    AC.elemPending = GetTime()
    Send("EL " .. (name or "NEXT"))
end

function ActionCombat_Interact()
    AC.interactAt = GetTime()
    Send("INT " .. Facing())
end

function ActionCombat_Sprint(keystate)
    Send(keystate == "down" and "SS" or "SE")
end

function ActionCombat_Guard(keystate)
    Send(keystate == "down" and "GS" or "GE")
end

------------------------------------------------------------------------ events / update

AC:RegisterEvent("PLAYER_ENTERING_WORLD")
AC:RegisterEvent("CHAT_MSG_ADDON")
AC:RegisterEvent("UNIT_MANA")
AC:RegisterEvent("UNIT_MAXMANA")
AC:SetScript("OnEvent", function(_, event, prefix, msg)
    if event == "PLAYER_ENTERING_WORLD" then
        Send("HELLO")
    elseif (event == "UNIT_MANA" or event == "UNIT_MAXMANA") then
        if prefix == "player" and magicMode then ReadMana() end
    elseif event == "CHAT_MSG_ADDON" and prefix == PREFIX then
        if AC.debug and not msg:find("^S ") and not msg:find("^P ") then
            DEFAULT_CHAT_FRAME:AddMessage("|cff888888[ACM <] " .. msg .. "|r")
        end
        local cmd, a, b = strsplit(" ", msg)
        if cmd == "S" and not magicMode then
            stamina, staminaMax = tonumber(a) or stamina, tonumber(b) or staminaMax
            bar:SetMinMaxValues(0, staminaMax); ghost:SetMinMaxValues(0, staminaMax)
            bar:SetValue(stamina)
            if stamina > ghostVal then ghostVal = stamina end
        elseif cmd == "V" then
            AC.serverVersion = tonumber(a) or 1
            holder:Show()
        elseif cmd == "ELEM" then
            local name, spellId, pos, count, list = strsplit(" ", msg:sub(6))
            local db = ActionCombatDB or {}
            ActionCombatDB = db
            -- first report after login: switch back to the element saved last time
            if not AC.elementRestored then
                AC.elementRestored = true
                if db.element and db.element ~= name and list and ("," .. list .. ","):find("," .. db.element .. ",", 1, true) then
                    Send("EL " .. db.element)
                    return
                end
            end
            local fromKey = AC.elemPending ~= nil
            AC.elemPending = nil
            local changed = (currentElement ~= nil and currentElement ~= name) or fromKey
            currentElement = name
            AC.showBarUntil = GetTime() + 3
            db.element = name
            AC.elementList = list
            local _, _, icon = GetSpellInfo(tonumber(spellId) or 0)
            local c = ELEMENT_COLORS[name] or { 0.7, 0.7, 1 }
            if magicMode then
                elemIcon:SetTexture(icon or "Interface\\Icons\\INV_Misc_QuestionMark"); elemIcon:Show()
                elemText:SetText(name .. ((tonumber(count) or 0) > 1 and ("  |cff999999" .. pos .. "/" .. count .. "|r") or ""))
                elemText:SetTextColor(c[1], c[2], c[3]); elemText:Show()
                bar:SetStatusBarColor(c[1] * 0.7, c[2] * 0.7, c[3] * 0.9)
            end
            if changed then
                Feedback(name, c[1], c[2], c[3], true)
                PlaySoundFile("Sound\\Interface\\iAbilitiesTurnOn.wav")
                if fromKey then
                    Say(string.format("element: |cff%02x%02x%02x%s|r (%s/%s)", math.floor(c[1] * 255), math.floor(c[2] * 255), math.floor(c[3] * 255), name, pos or "?", count or "?"))
                end
            end
        elseif cmd == "MODE" then
            SetMode(a)
            if magicMode then ReadMana() end
            local label = magicMode and "|cff6f9fffMagic (mana)|r"
                or rangedMode and "|cffffc060Ranged (stamina)|r"
                or "|cff7fdf7fMelee (stamina)|r"
            DEFAULT_CHAT_FRAME:AddMessage("|cff4fc3f7Action Combat|r style: " .. label)
        elseif cmd == "CLOSE" then
            local text = { OFF = "roll back to make space", SHOOT = "keep shooting point-blank", SWAP = "swap to melee weapon" }
            DEFAULT_CHAT_FRAME:AddMessage("|cff4fc3f7Action Combat|r enemies up close: " .. (text[a] or a))
        elseif cmd == "P" then
            -- crosshair prompt: "P <VERB> <name with spaces>"
            local verb, name = msg:match("^P (%u+) ?(.*)$")
            if ActionCombat_SetPrompt then ActionCombat_SetPrompt(verb, name) end
        elseif cmd == "E" and EVENTS[a] then
            EVENTS[a]()
        end
    end
end)

AC:SetScript("OnUpdate", function(_, elapsed)
    -- ghost bar drains toward real value
    if ghostVal > stamina then
        ghostVal = math.max(stamina, ghostVal - staminaMax * 0.8 * elapsed)
    end
    ghost:SetValue(ghostVal)

    -- hide bar when full and out of combat, like modern action games
    if holder:IsShown() then
        local full = stamina >= staminaMax and not UnitAffectingCombat("player")
            and GetTime() > (AC.showBarUntil or 0)
        holder:SetAlpha(full and math.max(0, holder:GetAlpha() - elapsed) or 1)
    end

    if AC.elemPending and GetTime() - AC.elemPending > 2 then
        AC.elemPending = nil
        Say("|cffff6060no answer from the server|r for the element switch. Make sure the worldserver was rebuilt with the latest mod-action-combat, then /reload.")
    end

    -- aimed shot draw (bow colors, flashes white at full draw = piercing shot ready)
    if aimDown and not heavyDown then
        local t = math.min(1, (GetTime() - aimStart) / MAX_CHARGE)
        charge:SetValue(t)
        if t >= 1 then charge:SetStatusBarColor(1, 1, 1) else charge:SetStatusBarColor(0.9, 0.7, 0.3) end
    end

    if heavyDown then
        local t = math.min(1, (GetTime() - heavyStart) / MAX_CHARGE)
        charge:SetValue(t)
        if magicMode then
            charge:SetStatusBarColor(0.4 + 0.5 * t, 0.3 + 0.5 * t, 1)
        elseif rangedMode then
            -- bow draw: flashes white when fully drawn (piercing shot ready)
            if t >= 1 then charge:SetStatusBarColor(1, 1, 1) else charge:SetStatusBarColor(0.9, 0.7, 0.3) end
        else
            charge:SetStatusBarColor(1, 0.55 + 0.45 * t, 0.1 + 0.6 * t)
        end
    end

    if feedbackT > 0 then
        feedbackT = feedbackT - elapsed
        feedback:SetAlpha(math.min(1, feedbackT * 3))
    end
    if flashT > 0 then
        flashT = flashT - elapsed
        flash:SetAlpha(math.max(0, flashT))
    end
end)

SLASH_ACTIONCOMBAT1 = "/acm"
SlashCmdList.ACTIONCOMBAT = function(arg)
    arg = strlower(strtrim(arg or ""))
    if arg == "melee" or arg == "magic" or arg == "ranged" or arg == "auto" then
        Send("STYLE " .. strupper(arg))
        return
    end
    if arg == "debug" then
        AC.debug = not AC.debug
        Say("debug " .. (AC.debug and "on: addon messages are printed in chat" or "off"))
        return
    end
    local elemArg = arg:match("^element%s*(.*)$") or arg:match("^elem%s*(.*)$")
    if elemArg then
        if elemArg == "" then
            DEFAULT_CHAT_FRAME:AddMessage("|cff4fc3f7Action Combat|r element: " .. (currentElement or "none")
                .. (AC.elementList and ("  (available: " .. AC.elementList:gsub(",", ", ") .. ")") or ""))
        else
            ActionCombat_Element(elemArg)
        end
        return
    end
    local camArgs = arg:match("^cam%s*(.*)$")
    if camArgs then
        ActionCombat_CameraCommand(camArgs)
        return
    end
    local closeArg = arg:match("^close%s+(%a+)$")
    if closeArg then
        Send("CLOSE " .. strupper(closeArg))
        return
    end
    DEFAULT_CHAT_FRAME:AddMessage("|cff4fc3f7Action Combat|r: set keys in Esc > Key Bindings > Action Combat.")
    DEFAULT_CHAT_FRAME:AddMessage("Suggested: Light = 1, Heavy = 2 (hold), Dodge = Mouse4, Guard = Mouse5 (hold), Sprint = Shift (hold), Quick shot = 3, Aimed shot = 4 (hold), Interact = F, Switch element = Tab or Mouse wheel.")
    DEFAULT_CHAT_FRAME:AddMessage("/acm melee | magic | ranged | auto  - pick your fighting style (ranged needs a bow, gun or crossbow)")
    DEFAULT_CHAT_FRAME:AddMessage("/acm close swap | shoot | off | default  - ranged: what happens when an enemy is in your face")
    DEFAULT_CHAT_FRAME:AddMessage("/acm cam  - right-mouse action camera options (sensitivity, reticle)")
    DEFAULT_CHAT_FRAME:AddMessage("/acm element [name | next]  - magic: show or switch your element (e.g. /acm element frost)")
    Send("HELLO")
end
