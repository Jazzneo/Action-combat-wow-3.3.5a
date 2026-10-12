#ifndef MOD_ACTION_COMBAT_H
#define MOD_ACTION_COMBAT_H

#include "DataMap.h"
#include "Define.h"
#include "ObjectGuid.h"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

class Player;
class Creature;

namespace ActionCombat
{
    constexpr char const* ADDON_PREFIX = "ACM";

    enum Style : uint8
    {
        STYLE_MELEE  = 0,
        STYLE_MAGIC  = 1,
        STYLE_RANGED = 2,
    };

    // A magic element casters can switch to. The bolt is a real spell, so its school (resistances,
    // immunities), projectile, impact and any built-in extra (Fireball's burn, Frostbolt's slow)
    // come with it; the numbers below are the element's own playstyle on top.
    struct Element
    {
        std::string name;
        uint32 bolt = 0;
        float  damageMult = 1.0f;
        float  costMult = 1.0f;
        float  cooldownMult = 1.0f;
        float  poiseMult = 1.0f;
        float  leechPct = 0.0f;             // % of damage dealt healed back to the caster
        uint32 chainTargets = 0;            // extra enemies the bolt jumps to
        float  chainRange = 8.0f;
        float  chainFalloff = 0.5f;         // each jump does this much of the previous one
        float  bonusVsDemonUndead = 1.0f;   // damage multiplier against demons and undead
    };

    struct Config
    {
        bool   enabled = true;
        bool   announce = true;

        float  staminaMax = 100.0f;
        float  staminaRegen = 35.0f;
        uint32 staminaRegenDelay = 900;
        float  guardRegenPct = 40.0f;

        float  dodgeCost = 25.0f;
        uint32 dodgeCooldown = 450;
        uint32 dodgeIFrames = 400;
        float  dodgeSpeedXY = 16.0f;
        float  dodgeSpeedZ = 3.0f;
        float  backstepSpeedXY = 10.0f;
        uint32 backstepIFrames = 250;
        uint8  dodgeStyle = 1;              // 0 = hop (knockback), 1 = grounded dash
        float  dashDistance = 7.0f;
        float  dashSpeed = 26.0f;           // yards per second
        uint32 dodgeCustomEmote = 0;        // emote id of a custom roll animation (client patch)

        float  lightCost = 12.0f;
        uint32 lightCooldown = 550;
        float  lightRange = 5.0f;
        float  lightArcDeg = 100.0f;
        float  lightFinisherArcDeg = 200.0f;
        float  lightPoise = 12.0f;
        uint32 comboWindow = 1100;
        float  comboMult[3] = { 1.0f, 1.15f, 1.5f };

        float  heavyCost = 30.0f;
        uint32 heavyMinCharge = 350;
        uint32 heavyMaxCharge = 1500;
        uint32 heavyCooldown = 900;
        float  heavyRange = 6.0f;
        float  heavyArcDeg = 140.0f;
        float  heavyMinMult = 1.6f;
        float  heavyMaxMult = 2.8f;
        float  heavyPoise = 40.0f;

        uint32 maxTargets = 5;

        // ---- Dual wield (main hand + off-hand weapon)
        bool   dualWieldEnabled = true;
        float  offhandMult = 1.5f;          // boost for off-hand swings (WoW's off-hand already does 50%)
        uint32 offhandFollowUpMs = 200;     // delay before the off-hand half of a double strike
        float  dualWieldCooldownMult = 0.85f; // light attacks come a little faster
        float  autoAttackPct = 30.0f;

        float  guardReductionPct = 70.0f;
        float  guardStaminaPerDmgPct = 2.0f;
        float  guardMinStaminaHit = 8.0f;
        bool   guardSpellsToo = false;
        uint32 parryWindow = 220;
        float  parryPoise = 100.0f;

        uint32 staggerSpell = 56;
        float  poiseNormal = 40.0f;
        float  poiseElite = 120.0f;
        float  poiseBoss = 0.0f;
        uint32 poiseRegenDelay = 4000;
        uint32 riposteWindow = 3000;
        float  riposteMult = 2.5f;

        // ---- Magic style (mana classes). Costs are % of BASE mana, like Blizzard's own spell costs.
        bool   magicEnabled = true;
        uint32 magicClassMask = 0;          // bit (1 << (class-1)), built from config
        uint32 boltSpell[12] = { };         // per class id
        float  magicRegenPct = 2.0f;        // % base mana per second (out of the 5-sec rule)
        uint32 magicRegenDelay = 1500;

        float  boltCostPct = 2.5f;
        uint32 boltCooldown = 450;
        float  boltRange = 30.0f;
        float  boltAimArcDeg = 40.0f;       // free aim cone
        float  boltSoftLockArcDeg = 120.0f; // keep your selected target if it's roughly in front
        float  boltBaseDmg = 25.0f;
        float  boltDmgPerLevel = 8.0f;
        float  boltSpCoef = 0.55f;
        float  boltPoise = 10.0f;
        float  boltComboMult[3] = { 1.0f, 1.1f, 1.4f };

        float  volleyCostPct = 9.0f;
        uint32 volleyCooldown = 1000;
        float  volleyRange = 25.0f;
        float  volleyArcDeg = 90.0f;
        float  volleyMinMult = 1.3f;
        float  volleyMaxMult = 2.4f;
        float  volleyPoise = 35.0f;

        float  blinkCostPct = 4.0f;
        float  blinkDistance = 8.0f;
        float  backBlinkDistance = 5.0f;
        uint32 blinkVisualSpell = 0;        // extra optional visual on the caster
        uint32 blinkDepart[12] = { };       // per class: flash left behind at the start point
        uint32 blinkArrive[12] = { };       // per class: flash where you land
        uint32 blinkSound[12] = { };        // per class: optional extra SoundEntries id
        uint32 blinkMarkerEntry = 12999;    // invisible creature that "holds" the effects

        float  wardReductionPct = 75.0f;
        float  wardManaPerDamage = 1.5f;
        bool   wardBlocksSpells = true;

        // ---- Ranged style (bows, guns, crossbows). Uses stamina, rolls and guard like melee.
        bool   rangedEnabled = true;
        uint32 rangedClassMask = 0;         // classes that default to ranged when holding a bow/gun
        bool   rangedAllowThrown = false;
        uint32 shootSpell = 6660;           // single-fire "Shoot" (NPC archers/riflemen): projectile, ammo, weapon damage
        float  shotCost = 10.0f;
        uint32 shotCooldown = 600;
        float  shotRange = 35.0f;
        float  shotAimArcDeg = 30.0f;
        float  shotSoftLockArcDeg = 120.0f;
        float  shotPoise = 8.0f;
        float  shotComboMult[3] = { 1.0f, 1.1f, 1.3f };

        float  powerShotCost = 28.0f;
        uint32 powerShotCooldown = 1000;
        float  powerShotRange = 40.0f;
        float  powerShotMinMult = 1.8f;
        float  powerShotMaxMult = 3.2f;
        float  powerShotPoise = 35.0f;
        float  pierceWidth = 2.0f;          // half-width of the piercing line at full draw
        uint32 pierceMaxTargets = 4;

        // What a ranged fighter does when an enemy is in their face:
        // 0 = nothing (must roll back), 1 = point-blank shots, 2 = swap to the melee weapon
        // (falls back to point-blank shots if no melee weapon is equipped)
        uint8  closeRangeMode = 2;
        float  closeRange = 6.0f;           // "in your face" distance for the swap
        float  pointBlankMult = 0.85f;      // damage of a point-blank shot vs a normal shot

        // ---- Sprint (hold to run faster)
        bool   sprintEnabled = true;
        float  sprintMult = 1.45f;          // run speed multiplier on top of normal speed (buffs included)
        float  sprintDrain = 12.0f;         // stamina per second while actually moving
        float  sprintMinStart = 10.0f;      // stamina needed to start sprinting
        float  sprintManaPctPerSec = 1.0f;  // magic style pays % base mana per second instead
        bool   sprintInCombat = true;

        // ---- Animations / visuals (built from the client's existing animations)
        bool   animEnabled = true;
        bool   animGuardStance = true;
        uint32 dodgeVisualSpell = 0;        // optional cosmetic spell on roll/backstep
        uint32 parryVisualSpell = 0;        // optional cosmetic spell on perfect parry/deflect
        bool   magicChargeCast = true;      // magic Heavy shows the element spell's real wind-up

        // ---- Interact key (loot / talk / use while the camera is held)
        bool   interactEnabled = true;
        float  interactRange = 6.0f;
        float  interactArcDeg = 120.0f;

        // Magic ward bubble: a purely cosmetic shield aura per class (0 = none)
        uint32 wardVisual[12] = { };

        // ---- Elements
        std::vector<Element> elements;
        std::vector<uint8> classElements[12];   // indexes into elements, per class
        bool   elementRequireKnown = false;     // must know any rank of the element's spell
        uint32 elementSwitchCooldown = 600;
    };

    extern Config Cfg;

    struct PlayerState : public DataMap::Base
    {
        float  stamina = 100.0f;
        int32  lastSentStamina = -1;
        uint32 lastSpend = 0;
        uint32 syncTimer = 0;
        bool   addonPresent = false;

        uint32 iframesUntil = 0;
        uint32 nextDodgeAt = 0;

        uint8  comboIndex = 0;
        uint32 lastLight = 0;
        uint32 nextLightAt = 0;

        uint32 heavyChargeStart = 0;
        uint32 nextHeavyAt = 0;

        bool   guarding = false;
        uint32 guardStart = 0;
        bool   parryUsed = false;

        // 0 = auto (by class / form / weapon), 1 = force melee, 2 = force magic, 3 = force ranged
        uint8  styleOverride = 0;
        int8   lastSentMode = -1;
        uint32 lastManaSpend = 0;
        float  manaRegenAcc = 0.0f;

        // ranged close-range behaviour: 0 = server default, 1 = off, 2 = point-blank, 3 = swap
        uint8  closeOverride = 0;
        bool   swappedToMelee = false;

        bool   sprinting = false;
        float  sprintTargetRate = 0.0f;
        uint32 stanceEmote = 0;
        uint32 wardVisual = 0;              // cosmetic shield aura currently shown

        // shooting has its own timers so it never blocks melee/magic (and vice versa)
        uint32 nextShotAt = 0;
        uint32 lastShot = 0;
        uint8  shotCombo = 0;
        uint32 drawStart = 0;
        uint32 nextPowerShotAt = 0;
        bool   aiming = false;

        // queued off-hand half of a double strike
        uint32 pendingOffAt = 0;
        float  pendingOffFacing = 0.0f;
        float  pendingOffRange = 0.0f;
        float  pendingOffArc = 0.0f;
        float  pendingOffMult = 0.0f;
        float  pendingOffPoise = 0.0f;

        // current element (index into Cfg.elements; 255 = not chosen yet -> class default)
        uint8  element = 255;
        uint32 nextElementSwitchAt = 0;
        int32  lastSentElement = -2;        // position | (list size << 8)

        // bolts in flight whose hit should heal the caster (Shadow / Nature leech)
        struct PendingLeech { ObjectGuid target; uint32 spellId; uint32 expires; float pct; };
        std::vector<PendingLeech> pendingLeech;
        uint32 pendingHeal = 0;

        // magic Heavy wind-up currently shown (spell id), 0 = none
        uint32 chargeVisualSpell = 0;

        std::mutex queueLock;
        std::deque<std::string> queue;
    };

    struct CreatureState : public DataMap::Base
    {
        float  poise = 0.0f;
        uint32 lastPoiseHit = 0;
        uint32 riposteUntil = 0;
    };

    void LoadConfig();
}

#endif
