/*
 * mod-action-combat — Zelda / Elden Ring style action combat for AzerothCore (3.3.5a)
 *
 * Features
 *   - Stamina bar (server-authoritative, synced to the ActionCombat client addon)
 *   - Dodge roll in any of 8 directions with invulnerability frames (backstep when idle)
 *   - Light attack combo (3 hits, cone cleave, rising damage)
 *   - Charged heavy attack (hold to charge, bigger cone, big poise damage)
 *   - Guard (reduces damage, drains stamina, guard break when empty)
 *   - Perfect parry (press guard just before a hit: negate it and stagger the attacker)
 *   - Enemy poise: enough hits stagger an enemy; the next hit is a critical "riposte"
 *
 *
 * Magic style (mana classes: Priest, Shaman, Mage, Warlock, Druid by default)
 *   - Light  -> aimed class bolt (Fireball / Shadow Bolt / Smite / Lightning Bolt / Wrath), 3-hit combo
 *   - Heavy  -> charged volley: a bolt at every enemy in a cone
 *   - Dodge  -> short Blink in the movement direction with i-frames
 *   - Guard  -> mana ward: prevented damage is paid with mana; tap timing = spell deflect
 *   - Everything costs % of base mana instead of stamina; bonus mana regen when not spending
 *
 * Shooting (bows, guns, crossbows) has its own two keys, Quick Shot and Aimed Shot, usable in
 * every style; Light/Heavy stay melee (or magic). The optional ranged *style* (/acm ranged)
 * also puts shooting on Light/Heavy:
 *   - Light  -> quick shot with the real "Shoot" spell (ammo, projectile), soft lock / free aim
 *   - Heavy  -> draw and release a power shot; a full draw pierces every enemy on the line
 *   - Dodge / Guard work like melee; costs are stamina. Too close (inside min range) = roll back!
 *
 * Transport: the client addon sends SendAddonMessage("ACM", "<cmd>", "WHISPER", <self>).
 * The whisper is swallowed by OnPlayerCanUseChat and the command is queued; it is executed
 * in OnPlayerUpdate (map thread) so all combat logic runs where unit state is safe to touch.
 */

#include "ActionCombat.h"

#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

namespace ActionCombat
{
    Config Cfg;

    // Context of the action swing currently being resolved on this thread.
    // AttackerStateUpdate() calls back into ModifyMeleeDamage synchronously, so a thread_local
    // is enough to know "this melee hit came from a light/heavy attack" (map updates can be
    // multi-threaded, hence thread_local rather than a plain global).
    struct SwingContext
    {
        ObjectGuid attacker;
        float damageMult = 1.0f;
        float poiseDamage = 0.0f;
    };
    thread_local SwingContext const* tl_swing = nullptr;

    // ------------------------------------------------------------------ config

    void LoadConfig()
    {
        Cfg.enabled             = sConfigMgr->GetOption<bool>("ActionCombat.Enable", true);
        Cfg.announce            = sConfigMgr->GetOption<bool>("ActionCombat.Announce", true);

        Cfg.staminaMax          = sConfigMgr->GetOption<float>("ActionCombat.Stamina.Max", 100.0f);
        Cfg.staminaRegen        = sConfigMgr->GetOption<float>("ActionCombat.Stamina.RegenPerSec", 35.0f);
        Cfg.staminaRegenDelay   = sConfigMgr->GetOption<uint32>("ActionCombat.Stamina.RegenDelayMs", 900);
        Cfg.guardRegenPct       = sConfigMgr->GetOption<float>("ActionCombat.Stamina.GuardRegenPct", 40.0f);

        Cfg.dodgeCost           = sConfigMgr->GetOption<float>("ActionCombat.Dodge.Cost", 25.0f);
        Cfg.dodgeCooldown       = sConfigMgr->GetOption<uint32>("ActionCombat.Dodge.CooldownMs", 450);
        Cfg.dodgeIFrames        = sConfigMgr->GetOption<uint32>("ActionCombat.Dodge.IFramesMs", 400);
        Cfg.dodgeSpeedXY        = sConfigMgr->GetOption<float>("ActionCombat.Dodge.SpeedXY", 16.0f);
        Cfg.dodgeSpeedZ         = sConfigMgr->GetOption<float>("ActionCombat.Dodge.SpeedZ", 3.0f);
        Cfg.backstepSpeedXY     = sConfigMgr->GetOption<float>("ActionCombat.Dodge.BackstepSpeedXY", 10.0f);
        Cfg.backstepIFrames     = sConfigMgr->GetOption<uint32>("ActionCombat.Dodge.BackstepIFramesMs", 250);
        Cfg.dodgeStyle          = uint8(std::min<uint32>(1, sConfigMgr->GetOption<uint32>("ActionCombat.Dodge.Style", 1)));
        Cfg.dashDistance        = sConfigMgr->GetOption<float>("ActionCombat.Dodge.DashDistance", 7.0f);
        Cfg.dashSpeed           = sConfigMgr->GetOption<float>("ActionCombat.Dodge.DashSpeed", 26.0f);
        Cfg.dodgeCustomEmote    = sConfigMgr->GetOption<uint32>("ActionCombat.Dodge.CustomAnimEmote", 0);

        Cfg.lightCost           = sConfigMgr->GetOption<float>("ActionCombat.Light.Cost", 12.0f);
        Cfg.lightCooldown       = sConfigMgr->GetOption<uint32>("ActionCombat.Light.CooldownMs", 550);
        Cfg.lightRange          = sConfigMgr->GetOption<float>("ActionCombat.Light.Range", 5.0f);
        Cfg.lightArcDeg         = sConfigMgr->GetOption<float>("ActionCombat.Light.ArcDegrees", 100.0f);
        Cfg.lightFinisherArcDeg = sConfigMgr->GetOption<float>("ActionCombat.Light.FinisherArcDegrees", 200.0f);
        Cfg.lightPoise          = sConfigMgr->GetOption<float>("ActionCombat.Light.Poise", 12.0f);
        Cfg.comboWindow         = sConfigMgr->GetOption<uint32>("ActionCombat.Light.ComboWindowMs", 1100);
        Cfg.comboMult[0]        = sConfigMgr->GetOption<float>("ActionCombat.Light.Combo1Mult", 1.0f);
        Cfg.comboMult[1]        = sConfigMgr->GetOption<float>("ActionCombat.Light.Combo2Mult", 1.15f);
        Cfg.comboMult[2]        = sConfigMgr->GetOption<float>("ActionCombat.Light.Combo3Mult", 1.5f);

        Cfg.heavyCost           = sConfigMgr->GetOption<float>("ActionCombat.Heavy.Cost", 30.0f);
        Cfg.heavyMinCharge      = sConfigMgr->GetOption<uint32>("ActionCombat.Heavy.MinChargeMs", 350);
        Cfg.heavyMaxCharge      = sConfigMgr->GetOption<uint32>("ActionCombat.Heavy.MaxChargeMs", 1500);
        Cfg.heavyCooldown       = sConfigMgr->GetOption<uint32>("ActionCombat.Heavy.CooldownMs", 900);
        Cfg.heavyRange          = sConfigMgr->GetOption<float>("ActionCombat.Heavy.Range", 6.0f);
        Cfg.heavyArcDeg         = sConfigMgr->GetOption<float>("ActionCombat.Heavy.ArcDegrees", 140.0f);
        Cfg.heavyMinMult        = sConfigMgr->GetOption<float>("ActionCombat.Heavy.MinMult", 1.6f);
        Cfg.heavyMaxMult        = sConfigMgr->GetOption<float>("ActionCombat.Heavy.MaxMult", 2.8f);
        Cfg.heavyPoise          = sConfigMgr->GetOption<float>("ActionCombat.Heavy.Poise", 40.0f);

        Cfg.maxTargets          = sConfigMgr->GetOption<uint32>("ActionCombat.Attack.MaxTargets", 5);
        Cfg.dualWieldEnabled    = sConfigMgr->GetOption<bool>("ActionCombat.DualWield.Enable", true);
        Cfg.offhandMult         = sConfigMgr->GetOption<float>("ActionCombat.DualWield.OffhandMult", 1.5f);
        Cfg.offhandFollowUpMs   = sConfigMgr->GetOption<uint32>("ActionCombat.DualWield.FollowUpMs", 200);
        Cfg.dualWieldCooldownMult = sConfigMgr->GetOption<float>("ActionCombat.DualWield.CooldownMult", 0.85f);
        Cfg.autoAttackPct       = sConfigMgr->GetOption<float>("ActionCombat.AutoAttack.DamagePct", 30.0f);

        Cfg.guardReductionPct   = sConfigMgr->GetOption<float>("ActionCombat.Guard.DamageReductionPct", 70.0f);
        Cfg.guardStaminaPerDmgPct = sConfigMgr->GetOption<float>("ActionCombat.Guard.StaminaPerHpPct", 2.0f);
        Cfg.guardMinStaminaHit  = sConfigMgr->GetOption<float>("ActionCombat.Guard.MinStaminaPerHit", 8.0f);
        Cfg.guardSpellsToo      = sConfigMgr->GetOption<bool>("ActionCombat.Guard.BlocksSpells", false);
        Cfg.parryWindow         = sConfigMgr->GetOption<uint32>("ActionCombat.Parry.WindowMs", 220);
        Cfg.parryPoise          = sConfigMgr->GetOption<float>("ActionCombat.Parry.Poise", 100.0f);

        Cfg.staggerSpell        = sConfigMgr->GetOption<uint32>("ActionCombat.Stagger.SpellId", 56);
        Cfg.poiseNormal         = sConfigMgr->GetOption<float>("ActionCombat.Poise.Normal", 40.0f);
        Cfg.poiseElite          = sConfigMgr->GetOption<float>("ActionCombat.Poise.Elite", 120.0f);
        Cfg.poiseBoss           = sConfigMgr->GetOption<float>("ActionCombat.Poise.Boss", 0.0f);
        Cfg.poiseRegenDelay     = sConfigMgr->GetOption<uint32>("ActionCombat.Poise.ResetAfterMs", 4000);
        Cfg.riposteWindow       = sConfigMgr->GetOption<uint32>("ActionCombat.Riposte.WindowMs", 3000);
        Cfg.riposteMult         = sConfigMgr->GetOption<float>("ActionCombat.Riposte.Mult", 2.5f);

        // ---- Magic style
        Cfg.magicEnabled        = sConfigMgr->GetOption<bool>("ActionCombat.Magic.Enable", true);
        Cfg.magicClassMask      = 0;
        {
            std::istringstream list(sConfigMgr->GetOption<std::string>("ActionCombat.Magic.Classes", "5,7,8,9,11"));
            std::string tok;
            while (std::getline(list, tok, ','))
            {
                uint32 cls = uint32(std::strtoul(tok.c_str(), nullptr, 10));
                if (cls > 0 && cls < 12)
                    Cfg.magicClassMask |= 1u << (cls - 1);
            }
        }
        Cfg.boltSpell[CLASS_PALADIN] = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.BoltSpell.Paladin", 879);   // Exorcism
        Cfg.boltSpell[CLASS_PRIEST]  = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.BoltSpell.Priest", 585);    // Smite
        Cfg.boltSpell[CLASS_SHAMAN]  = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.BoltSpell.Shaman", 403);    // Lightning Bolt
        Cfg.boltSpell[CLASS_MAGE]    = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.BoltSpell.Mage", 133);      // Fireball
        Cfg.boltSpell[CLASS_WARLOCK] = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.BoltSpell.Warlock", 686);   // Shadow Bolt
        Cfg.boltSpell[CLASS_DRUID]   = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.BoltSpell.Druid", 5176);    // Wrath

        // ---- Elements
        {
            struct Def { char const* name; uint32 bolt; float dmg, cost, cd, poise, leech; uint32 chain; float bonusDU; };
            // name       bolt   dmg   cost  cd    poise leech chain vsDemonUndead
            Def const defs[] = {
                { "Fire",      133,  1.15f, 1.20f, 1.00f, 1.0f,  0.0f, 0, 1.0f },   // Fireball: burns
                { "Frost",     116,  0.90f, 1.00f, 1.00f, 1.6f,  0.0f, 0, 1.0f },   // Frostbolt: slows
                { "Arcane",  44425,  0.80f, 0.90f, 0.70f, 0.8f,  0.0f, 0, 1.0f },   // Arcane Barrage: rapid fire
                { "Shadow",    686,  1.00f, 1.00f, 1.00f, 1.0f, 20.0f, 0, 1.0f },   // Shadow Bolt: life drain
                { "Holy",      585,  0.95f, 1.00f, 1.00f, 1.0f,  0.0f, 0, 1.5f },   // Smite: smites demons/undead
                { "Lightning", 403,  0.90f, 1.05f, 1.00f, 1.0f,  0.0f, 2, 1.0f },   // Lightning Bolt: chains
                { "Nature",   5176,  0.90f, 0.80f, 1.00f, 1.0f, 10.0f, 0, 1.0f },   // Wrath: cheap, mends you
                { "Lunar",    2912,  1.30f, 1.25f, 1.40f, 1.3f,  0.0f, 0, 1.0f },   // Starfire: slow and heavy
            };
            Cfg.elements.clear();
            for (Def const& d : defs)
            {
                std::string base = std::string("ActionCombat.Magic.Element.") + d.name;
                Element e;
                e.name               = d.name;
                e.bolt               = sConfigMgr->GetOption<uint32>(base + ".Spell", d.bolt);
                e.damageMult         = sConfigMgr->GetOption<float>(base + ".DamageMult", d.dmg);
                e.costMult           = sConfigMgr->GetOption<float>(base + ".CostMult", d.cost);
                e.cooldownMult       = sConfigMgr->GetOption<float>(base + ".CooldownMult", d.cd);
                e.poiseMult          = sConfigMgr->GetOption<float>(base + ".PoiseMult", d.poise);
                e.leechPct           = sConfigMgr->GetOption<float>(base + ".LeechPct", d.leech);
                e.chainTargets       = sConfigMgr->GetOption<uint32>(base + ".ChainTargets", d.chain);
                e.chainRange         = sConfigMgr->GetOption<float>(base + ".ChainRange", 8.0f);
                e.chainFalloff       = sConfigMgr->GetOption<float>(base + ".ChainFalloff", 0.5f);
                e.bonusVsDemonUndead = sConfigMgr->GetOption<float>(base + ".VsDemonUndeadMult", d.bonusDU);
                // Spell data isn't loaded yet when the config is read at startup, so the spell
                // itself is checked when a bolt is fired (see FireBolt).
                if (e.bolt)
                    Cfg.elements.push_back(e);
            }

            struct ClassDef { uint8 cls; char const* name; char const* list; };
            ClassDef const classDefs[] = {
                { CLASS_PALADIN, "Paladin", "Holy" },
                { CLASS_PRIEST,  "Priest",  "Holy,Shadow" },
                { CLASS_SHAMAN,  "Shaman",  "Lightning,Fire,Frost" },
                { CLASS_MAGE,    "Mage",    "Fire,Frost,Arcane" },
                { CLASS_WARLOCK, "Warlock", "Shadow,Fire" },
                { CLASS_DRUID,   "Druid",   "Nature,Lunar" },
            };
            for (auto& v : Cfg.classElements)
                v.clear();
            for (ClassDef const& c : classDefs)
            {
                std::istringstream list(sConfigMgr->GetOption<std::string>(std::string("ActionCombat.Magic.Elements.") + c.name, c.list));
                std::string tok;
                while (std::getline(list, tok, ','))
                {
                    tok.erase(0, tok.find_first_not_of(" \t"));
                    tok.erase(tok.find_last_not_of(" \t") + 1);
                    for (size_t i = 0; i < Cfg.elements.size(); ++i)
                        if (strcasecmp(Cfg.elements[i].name.c_str(), tok.c_str()) == 0)
                            Cfg.classElements[c.cls].push_back(uint8(i));
                }
            }
            Cfg.elementRequireKnown   = sConfigMgr->GetOption<bool>("ActionCombat.Magic.Elements.RequireSpellKnown", false);
            Cfg.elementSwitchCooldown = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.Elements.SwitchCooldownMs", 600);
        }

        Cfg.magicRegenPct       = sConfigMgr->GetOption<float>("ActionCombat.Magic.RegenPctPerSec", 2.0f);
        Cfg.magicRegenDelay     = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.RegenDelayMs", 1500);

        Cfg.boltCostPct         = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.CostPct", 2.5f);
        Cfg.boltCooldown        = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.Bolt.CooldownMs", 450);
        Cfg.boltRange           = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.Range", 30.0f);
        Cfg.boltAimArcDeg       = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.AimArcDegrees", 40.0f);
        Cfg.boltSoftLockArcDeg  = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.SoftLockArcDegrees", 120.0f);
        Cfg.boltBaseDmg         = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.BaseDamage", 25.0f);
        Cfg.boltDmgPerLevel     = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.DamagePerLevel", 8.0f);
        Cfg.boltSpCoef          = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.SpellPowerCoef", 0.55f);
        Cfg.boltPoise           = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.Poise", 10.0f);
        Cfg.boltComboMult[0]    = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.Combo1Mult", 1.0f);
        Cfg.boltComboMult[1]    = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.Combo2Mult", 1.1f);
        Cfg.boltComboMult[2]    = sConfigMgr->GetOption<float>("ActionCombat.Magic.Bolt.Combo3Mult", 1.4f);

        Cfg.volleyCostPct       = sConfigMgr->GetOption<float>("ActionCombat.Magic.Volley.CostPct", 9.0f);
        Cfg.volleyCooldown      = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.Volley.CooldownMs", 1000);
        Cfg.volleyRange         = sConfigMgr->GetOption<float>("ActionCombat.Magic.Volley.Range", 25.0f);
        Cfg.volleyArcDeg        = sConfigMgr->GetOption<float>("ActionCombat.Magic.Volley.ArcDegrees", 90.0f);
        Cfg.volleyMinMult       = sConfigMgr->GetOption<float>("ActionCombat.Magic.Volley.MinMult", 1.3f);
        Cfg.volleyMaxMult       = sConfigMgr->GetOption<float>("ActionCombat.Magic.Volley.MaxMult", 2.4f);
        Cfg.volleyPoise         = sConfigMgr->GetOption<float>("ActionCombat.Magic.Volley.Poise", 35.0f);

        Cfg.blinkCostPct        = sConfigMgr->GetOption<float>("ActionCombat.Magic.Blink.CostPct", 4.0f);
        Cfg.blinkDistance       = sConfigMgr->GetOption<float>("ActionCombat.Magic.Blink.Distance", 8.0f);
        Cfg.backBlinkDistance   = sConfigMgr->GetOption<float>("ActionCombat.Magic.Blink.BackstepDistance", 5.0f);
        Cfg.blinkVisualSpell    = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.Blink.VisualSpellId", 0);
        Cfg.blinkMarkerEntry    = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.Blink.MarkerCreature", 12999);
        {
            // Visual-only spells NPCs cast in the 3.3.5 world database.
            //   51347 Teleport Visual Only      61456 Evil Teleport Visual Only
            //   35426 Arcane Explosion Visual   70785 Holy Bomb Visual
            //   64785 Random Lightning Visual   34770 Plant Spawn Effect
            struct { uint8 cls; char const* name; uint32 depart, arrive; } const defs[] = {
                { CLASS_PALADIN, "Paladin", 51347, 70785 },
                { CLASS_PRIEST,  "Priest",  51347, 70785 },
                { CLASS_SHAMAN,  "Shaman",  51347, 64785 },
                { CLASS_MAGE,    "Mage",    51347, 35426 },
                { CLASS_WARLOCK, "Warlock", 61456, 61456 },
                { CLASS_DRUID,   "Druid",   51347, 34770 },
            };
            for (auto const& d : defs)
            {
                std::string base = std::string("ActionCombat.Magic.Blink.") + d.name;
                Cfg.blinkDepart[d.cls] = sConfigMgr->GetOption<uint32>(base + ".DepartSpell", d.depart);
                Cfg.blinkArrive[d.cls] = sConfigMgr->GetOption<uint32>(base + ".ArriveSpell", d.arrive);
                Cfg.blinkSound[d.cls]  = sConfigMgr->GetOption<uint32>(base + ".SoundId", 0);
            }
        }

        Cfg.wardReductionPct    = sConfigMgr->GetOption<float>("ActionCombat.Magic.Ward.DamageReductionPct", 75.0f);
        Cfg.wardManaPerDamage   = sConfigMgr->GetOption<float>("ActionCombat.Magic.Ward.ManaPerDamage", 1.5f);
        Cfg.wardBlocksSpells    = sConfigMgr->GetOption<bool>("ActionCombat.Magic.Ward.BlocksSpells", true);

        // ---- Ranged style
        Cfg.rangedEnabled       = sConfigMgr->GetOption<bool>("ActionCombat.Ranged.Enable", true);
        Cfg.rangedClassMask     = 0;
        {
            std::istringstream list(sConfigMgr->GetOption<std::string>("ActionCombat.Ranged.Classes", ""));
            std::string tok;
            while (std::getline(list, tok, ','))
            {
                uint32 cls = uint32(std::strtoul(tok.c_str(), nullptr, 10));
                if (cls > 0 && cls < 12)
                    Cfg.rangedClassMask |= 1u << (cls - 1);
            }
        }
        Cfg.rangedAllowThrown   = sConfigMgr->GetOption<bool>("ActionCombat.Ranged.AllowThrown", false);
        Cfg.shootSpell          = sConfigMgr->GetOption<uint32>("ActionCombat.Ranged.ShootSpellId", 6660);
        Cfg.shotCost            = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.Cost", 10.0f);
        Cfg.shotCooldown        = sConfigMgr->GetOption<uint32>("ActionCombat.Ranged.Shot.CooldownMs", 600);
        Cfg.shotRange           = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.Range", 35.0f);
        Cfg.shotAimArcDeg       = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.AimArcDegrees", 30.0f);
        Cfg.shotSoftLockArcDeg  = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.SoftLockArcDegrees", 120.0f);
        Cfg.shotPoise           = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.Poise", 8.0f);
        Cfg.shotComboMult[0]    = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.Combo1Mult", 1.0f);
        Cfg.shotComboMult[1]    = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.Combo2Mult", 1.1f);
        Cfg.shotComboMult[2]    = sConfigMgr->GetOption<float>("ActionCombat.Ranged.Shot.Combo3Mult", 1.3f);
        Cfg.powerShotCost       = sConfigMgr->GetOption<float>("ActionCombat.Ranged.PowerShot.Cost", 28.0f);
        Cfg.powerShotCooldown   = sConfigMgr->GetOption<uint32>("ActionCombat.Ranged.PowerShot.CooldownMs", 1000);
        Cfg.powerShotRange      = sConfigMgr->GetOption<float>("ActionCombat.Ranged.PowerShot.Range", 40.0f);
        Cfg.powerShotMinMult    = sConfigMgr->GetOption<float>("ActionCombat.Ranged.PowerShot.MinMult", 1.8f);
        Cfg.powerShotMaxMult    = sConfigMgr->GetOption<float>("ActionCombat.Ranged.PowerShot.MaxMult", 3.2f);
        Cfg.powerShotPoise      = sConfigMgr->GetOption<float>("ActionCombat.Ranged.PowerShot.Poise", 35.0f);
        Cfg.pierceWidth         = sConfigMgr->GetOption<float>("ActionCombat.Ranged.PowerShot.PierceWidth", 2.0f);
        Cfg.pierceMaxTargets    = sConfigMgr->GetOption<uint32>("ActionCombat.Ranged.PowerShot.PierceMaxTargets", 4);
        Cfg.closeRangeMode      = uint8(std::min<uint32>(2, sConfigMgr->GetOption<uint32>("ActionCombat.Ranged.CloseRangeMode", 2)));
        Cfg.closeRange          = sConfigMgr->GetOption<float>("ActionCombat.Ranged.CloseRange", 6.0f);
        Cfg.pointBlankMult      = sConfigMgr->GetOption<float>("ActionCombat.Ranged.PointBlankMult", 0.85f);

        Cfg.sprintEnabled       = sConfigMgr->GetOption<bool>("ActionCombat.Sprint.Enable", true);
        Cfg.sprintMult          = sConfigMgr->GetOption<float>("ActionCombat.Sprint.SpeedMult", 1.45f);
        Cfg.sprintDrain         = sConfigMgr->GetOption<float>("ActionCombat.Sprint.StaminaPerSec", 12.0f);
        Cfg.sprintMinStart      = sConfigMgr->GetOption<float>("ActionCombat.Sprint.MinStaminaToStart", 10.0f);
        Cfg.sprintManaPctPerSec = sConfigMgr->GetOption<float>("ActionCombat.Sprint.MagicManaPctPerSec", 1.0f);
        Cfg.sprintInCombat      = sConfigMgr->GetOption<bool>("ActionCombat.Sprint.AllowInCombat", true);

        Cfg.interactEnabled     = sConfigMgr->GetOption<bool>("ActionCombat.Interact.Enable", true);
        Cfg.interactRange       = sConfigMgr->GetOption<float>("ActionCombat.Interact.Range", 6.0f);
        Cfg.interactArcDeg      = sConfigMgr->GetOption<float>("ActionCombat.Interact.ArcDegrees", 120.0f);

        Cfg.animEnabled         = sConfigMgr->GetOption<bool>("ActionCombat.Anim.Enable", true);
        Cfg.animGuardStance     = sConfigMgr->GetOption<bool>("ActionCombat.Anim.GuardStance", true);
        Cfg.dodgeVisualSpell    = sConfigMgr->GetOption<uint32>("ActionCombat.Anim.DodgeVisualSpellId", 0);
        Cfg.parryVisualSpell    = sConfigMgr->GetOption<uint32>("ActionCombat.Anim.ParryVisualSpellId", 0);
        Cfg.magicChargeCast     = sConfigMgr->GetOption<bool>("ActionCombat.Anim.MagicChargeCast", true);

        // Cosmetic shield auras used by NPCs in the 3.3.5 world database
        Cfg.wardVisual[CLASS_PALADIN] = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.WardVisual.Paladin", 45775);  // Cosmetic - Divine Shield Blue
        Cfg.wardVisual[CLASS_PRIEST]  = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.WardVisual.Priest", 45775);   // Cosmetic - Divine Shield Blue
        Cfg.wardVisual[CLASS_SHAMAN]  = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.WardVisual.Shaman", 46934);   // Cosmetic - Arcane Force Shield
        Cfg.wardVisual[CLASS_MAGE]    = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.WardVisual.Mage", 46934);     // Cosmetic - Arcane Force Shield
        Cfg.wardVisual[CLASS_WARLOCK] = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.WardVisual.Warlock", 53143);  // Cosmetic - Arcane Force Shield Dark State
        Cfg.wardVisual[CLASS_DRUID]   = sConfigMgr->GetOption<uint32>("ActionCombat.Magic.WardVisual.Druid", 46934);    // Cosmetic - Arcane Force Shield
    }

    // ------------------------------------------------------------------ helpers

    PlayerState* GetState(Player* player)
    {
        return player->CustomData.GetDefault<PlayerState>("ActionCombat");
    }

    CreatureState* GetState(Creature* creature)
    {
        return creature->CustomData.GetDefault<CreatureState>("ActionCombat");
    }

    void SendToClient(Player* player, std::string const& msg)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, std::string(ADDON_PREFIX) + "\t" + msg);
        player->SendDirectMessage(&data);
    }

    void SendEvent(Player* player, char const* evt)
    {
        SendToClient(player, std::string("E ") + evt);
    }

    void SendStamina(Player* player, PlayerState* st)
    {
        std::ostringstream ss;
        ss << "S " << int32(st->stamina) << ' ' << int32(Cfg.staminaMax);
        SendToClient(player, ss.str());
        st->lastSentStamina = int32(st->stamina);
    }

    bool SpendStamina(Player* player, PlayerState* st, float cost, uint32 now)
    {
        // Elden Ring rule: you can start an action as long as you have *any* stamina left,
        // but the bar can't go below zero.
        if (st->stamina <= 0.0f)
        {
            SendEvent(player, "NOSTAM");
            return false;
        }
        st->stamina = std::max(0.0f, st->stamina - cost);
        st->lastSpend = now;
        SendStamina(player, st);
        return true;
    }

    bool CanAct(Player* player)
    {
        return player->IsAlive()
            && !player->IsMounted()
            && !player->GetVehicle()
            && !player->IsFlying()
            && !player->HasUnitState(UNIT_STATE_LOST_CONTROL)
            && !player->HasUnitFlag(UNIT_FLAG_PACIFIED)
            && !player->IsNonMeleeSpellCast(false);
    }

    float ClientFacing(Player* player, std::string const& arg)
    {
        // The client tells us where the camera/character is facing at the moment of the button
        // press (movement packets may lag a few ms behind). Accept it only if it is close to
        // what the server already believes, so a cheater can't hit things behind them freely.
        if (arg.empty())
            return player->GetOrientation();

        float f = std::strtof(arg.c_str(), nullptr);
        if (!std::isfinite(f))
            return player->GetOrientation();

        f = Position::NormalizeOrientation(f);
        float diff = std::fabs(Position::NormalizeOrientation(f - player->GetOrientation()));
        if (diff > float(M_PI))
            diff = 2.0f * float(M_PI) - diff;

        return diff <= float(M_PI) / 2.0f ? f : player->GetOrientation();
    }

    float PoiseFor(Creature* c)
    {
        if (c->isWorldBoss() || c->IsDungeonBoss())
            return Cfg.poiseBoss;
        if (c->isElite())
            return Cfg.poiseElite;
        return Cfg.poiseNormal;
    }

    void Stagger(Unit* target)
    {
        if (Cfg.staggerSpell)
            target->AddAura(Cfg.staggerSpell, target);
    }

    // Applies poise damage to a creature; returns true if it got staggered by this hit.
    bool ApplyPoise(Player* attacker, Creature* target, float poiseDamage, uint32 now)
    {
        float maxPoise = PoiseFor(target);
        if (maxPoise <= 0.0f || poiseDamage <= 0.0f)
            return false;

        CreatureState* cs = GetState(target);
        if (cs->lastPoiseHit && getMSTimeDiff(cs->lastPoiseHit, now) > Cfg.poiseRegenDelay)
            cs->poise = 0.0f;

        cs->lastPoiseHit = now;
        cs->poise += poiseDamage;

        if (cs->poise < maxPoise)
            return false;

        cs->poise = 0.0f;
        cs->riposteUntil = now + Cfg.riposteWindow;
        Stagger(target);
        SendEvent(attacker, "STAGGER");
        return true;
    }

    // ------------------------------------------------------------------ animations
    // 3.3.5 can only play animations the character models already have, so these pick the
    // best existing ones: weapon "ready" stances, shield block, 1H/2H/unarmed parry, dodge.

    void PlayAnim(Unit* unit, uint32 emote)
    {
        if (Cfg.animEnabled && emote)
            unit->HandleEmoteCommand(emote);
    }

    void PlayVisual(Unit* unit, uint32 spellId)
    {
        if (Cfg.animEnabled && spellId)
            unit->CastSpell(unit, spellId, true);
    }

    bool IsTwoHander(Item* item)
    {
        return item && item->GetTemplate()->InventoryType == INVTYPE_2HWEAPON;
    }

    bool IsPolearmOrStaff(Item* item)
    {
        return item && (item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM
                     || item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_STAFF);
    }

    // Looping "ready" pose held while guarding.
    uint32 GuardStanceEmote(Player* player, bool magic)
    {
        if (magic)
            return EMOTE_STATE_READY_SPELL_OMNI;
        Item* main = player->GetWeaponForAttack(BASE_ATTACK, true);
        if (!main)
            return EMOTE_STATE_READY_UNARMED;
        if (IsPolearmOrStaff(main))
            return EMOTE_STATE_READY2HL;
        if (IsTwoHander(main))
            return EMOTE_STATE_READY2H;
        return EMOTE_STATE_READY1H;
    }

    // One-shot played when a guarded hit lands (or a parry succeeds).
    uint32 BlockEmote(Player* player, bool magic)
    {
        if (!magic && player->GetShield(true))
            return EMOTE_ONESHOT_PARRY_SHIELD;
        Item* main = magic ? nullptr : player->GetWeaponForAttack(BASE_ATTACK, true);
        if (!main)
            return EMOTE_ONESHOT_PARRY_UNARMED;
        if (IsTwoHander(main))
            return IsPolearmOrStaff(main) ? EMOTE_ONESHOT_PARRY2HL : EMOTE_ONESHOT_PARRY2H;
        return EMOTE_ONESHOT_PARRY1H;
    }

    void SetStance(Player* player, PlayerState* st, uint32 emote)
    {
        if (!Cfg.animEnabled || !Cfg.animGuardStance)
            return;
        st->stanceEmote = emote;
        player->SetUInt32Value(UNIT_NPC_EMOTESTATE, emote);
    }

    // Only allow auras that do nothing but look pretty (dummy effects), so a wrong spell id in
    // the config can never give players a real buff.
    bool IsCosmeticAura(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return false;
        bool hasAura = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = info->Effects[i];
            if (!eff.Effect)
                continue;
            if (eff.Effect == SPELL_EFFECT_DUMMY)
                continue;
            if (eff.Effect != SPELL_EFFECT_APPLY_AURA)
                return false;
            if (eff.ApplyAuraName != SPELL_AURA_DUMMY && eff.ApplyAuraName != SPELL_AURA_PERIODIC_DUMMY
                && eff.ApplyAuraName != SPELL_AURA_NONE)
                return false;
            hasAura = true;
        }
        return hasAura;
    }

    // Stricter check for one-shot effects: only "nothing" effects (dummy / dummy auras) and no
    // spell script or DB spell_scripts entry attached, so casting it can't do anything but look
    // and sound like something.
    bool IsVisualOnlySpell(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return false;

        bool any = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = info->Effects[i];
            if (!eff.Effect)
                continue;
            any = true;
            if (eff.Effect == SPELL_EFFECT_DUMMY)
            {
                if (sSpellScripts.find(spellId | (uint32(i) << 24)) != sSpellScripts.end())
                    return false;
                continue;
            }
            if (eff.Effect != SPELL_EFFECT_APPLY_AURA)
                return false;
            if (eff.ApplyAuraName != SPELL_AURA_DUMMY && eff.ApplyAuraName != SPELL_AURA_PERIODIC_DUMMY
                && eff.ApplyAuraName != SPELL_AURA_NONE)
                return false;
        }

        auto bounds = sObjectMgr->GetSpellScriptsBounds(spellId);
        if (bounds.first != bounds.second)
            return false;
        return any;
    }

    // Plays a visual-only spell at a spot in the world (via a short-lived invisible marker), so
    // the effect stays where it happened instead of following the player.
    void PlayEffectAt(Player* player, Position const& pos, uint32 spellId)
    {
        if (!Cfg.animEnabled || !spellId)
            return;

        if (!IsVisualOnlySpell(spellId))
        {
            static std::mutex warnLock;
            static std::set<uint32> warned;
            std::lock_guard<std::mutex> lock(warnLock);
            if (warned.insert(spellId).second)
                LOG_ERROR("module", "mod-action-combat: effect spell {} is not visual-only (or has a script); not played.", spellId);
            return;
        }

        if (Cfg.blinkMarkerEntry)
        {
            if (TempSummon* marker = player->SummonCreature(Cfg.blinkMarkerEntry, pos, TEMPSUMMON_TIMED_DESPAWN, 3000))
            {
                marker->SetFaction(player->GetFaction());
                marker->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE);
                marker->CastSpell(marker, spellId, true);
                return;
            }
        }
        player->CastSpell(player, spellId, true);   // fallback: play it on the caster
    }

    void ShowWardBubble(Player* player, PlayerState* st)
    {
        if (!Cfg.animEnabled)
            return;
        uint8 cls = player->getClass();
        uint32 spellId = cls < 12 ? Cfg.wardVisual[cls] : 0;
        if (!spellId)
            return;

        static std::array<bool, 12> warned = { };
        if (!IsCosmeticAura(spellId))
        {
            if (!warned[cls])
            {
                warned[cls] = true;
                LOG_ERROR("module", "mod-action-combat: ward visual spell {} for class {} is not a purely cosmetic aura; not applied.", spellId, cls);
            }
            return;
        }
        st->wardVisual = spellId;
        if (!player->HasAura(spellId))
            player->AddAura(spellId, player);
    }

    void HideWardBubble(Player* player, PlayerState* st)
    {
        if (!st->wardVisual)
            return;
        player->RemoveAurasDueToSpell(st->wardVisual);
        st->wardVisual = 0;
    }

    void ClearStance(Player* player, PlayerState* st)
    {
        HideWardBubble(player, st);
        if (!st->stanceEmote)
            return;
        if (player->GetUInt32Value(UNIT_NPC_EMOTESTATE) == st->stanceEmote)
            player->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
        st->stanceEmote = 0;
    }

    // Bow, gun or crossbow (thrown optional) equipped and usable.
    bool HasRangedWeapon(Player* player)
    {
        Item* item = player->GetWeaponForAttack(RANGED_ATTACK, true);
        if (!item || item->IsBroken())
            return false;
        uint32 sub = item->GetTemplate()->SubClass;
        return sub == ITEM_SUBCLASS_WEAPON_BOW || sub == ITEM_SUBCLASS_WEAPON_GUN || sub == ITEM_SUBCLASS_WEAPON_CROSSBOW
            || (Cfg.rangedAllowThrown && sub == ITEM_SUBCLASS_WEAPON_THROWN);
    }

    bool CanUseMagic(Player* player)
    {
        uint8 cls = player->getClass();
        return Cfg.magicEnabled && player->GetMaxPower(POWER_MANA) > 0 && cls < 12
            && (Cfg.boltSpell[cls] || !Cfg.classElements[cls].empty())
            && !player->IsInFeralForm();
    }

    bool CanUseRanged(Player* player)
    {
        return Cfg.rangedEnabled && !player->IsInFeralForm() && HasRangedWeapon(player);
    }

    // Picks the fighting style:
    //   forced by /acm melee|magic|ranged (falls back to melee if not possible right now),
    //   otherwise by class: ranged classes holding a bow/gun -> ranged, mana classes -> magic,
    //   everyone else (and druids in Cat/Bear) -> melee.
    Style GetStyle(Player* player, PlayerState const* st)
    {
        switch (st->styleOverride)
        {
            case 1: return STYLE_MELEE;
            case 2: return CanUseMagic(player) ? STYLE_MAGIC : STYLE_MELEE;
            case 3: return CanUseRanged(player) ? STYLE_RANGED : STYLE_MELEE;
            default: break;
        }
        if ((Cfg.rangedClassMask & player->getClassMask()) && CanUseRanged(player))
            return STYLE_RANGED;
        if ((Cfg.magicClassMask & player->getClassMask()) && CanUseMagic(player))
            return STYLE_MAGIC;
        return STYLE_MELEE;
    }

    bool IsMagic(Player* player, PlayerState const* st)
    {
        return GetStyle(player, st) == STYLE_MAGIC;
    }

    uint32 ManaCost(Player* player, float pctOfBase)
    {
        return uint32(float(player->GetCreateMana()) * pctOfBase / 100.0f);
    }

    // Magic must be paid in full (unlike stamina, which lets you go to zero).
    bool SpendMana(Player* player, PlayerState* st, uint32 cost, uint32 now)
    {
        if (player->GetPower(POWER_MANA) < cost)
        {
            SendEvent(player, "NOMANA");
            return false;
        }
        player->ModifyPower(POWER_MANA, -int32(cost));
        st->lastManaSpend = now;
        return true;
    }

    // ------------------------------------------------------------------ actions

    bool IsDualWielding(Player* player)
    {
        return Cfg.dualWieldEnabled && player->GetWeaponForAttack(OFF_ATTACK, true) != nullptr;
    }

    bool IsDagger(Item* item)
    {
        return item && item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
    }

    // Swing animation for when nothing is hit (on a hit the client picks the animation itself
    // from the weapon and hand: off-hand slash, dagger stab, 2H swing...).
    uint32 WhiffEmote(Player* player, WeaponAttackType hand)
    {
        if (hand == OFF_ATTACK)
            return IsDagger(player->GetWeaponForAttack(OFF_ATTACK, true)) ? EMOTE_ONESHOT_ATTACK_OFF_PIERCE : EMOTE_ONESHOT_ATTACK_OFF;
        Item* main = player->GetWeaponForAttack(BASE_ATTACK, true);
        if (!main)
            return EMOTE_ONESHOT_ATTACK_UNARMED;
        if (main->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
            return EMOTE_ONESHOT_ATTACK2HTIGHT;
        return EMOTE_ONESHOT_ATTACK1H;
    }

    void DoSwing(Player* player, float facing, float range, float arcDeg, float mult, float poise, WeaponAttackType hand = BASE_ATTACK)
    {
        if (hand == OFF_ATTACK)
            mult *= Cfg.offhandMult;

        player->SetOrientation(facing);

        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, range + player->GetCombatReach());
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, targets, check);
        Cell::VisitObjects(player, searcher, range + player->GetCombatReach() + 5.0f);

        float arc = arcDeg * float(M_PI) / 180.0f;
        targets.remove_if([&](Unit* u)
        {
            return u == player
                || !player->isInFront(u, arc)
                || !player->IsValidAttackTarget(u)
                || !player->IsWithinLOSInMap(u)
                || player->GetDistance(u) > range + u->GetCombatReach();
        });

        // Closest targets first, capped
        targets.sort([&](Unit* a, Unit* b) { return player->GetDistance(a) < player->GetDistance(b); });
        if (targets.size() > Cfg.maxTargets)
            targets.resize(Cfg.maxTargets);

        if (targets.empty())
        {
            // whiff — still play the swing so it feels responsive
            player->HandleEmoteCommand(WhiffEmote(player, hand));
            return;
        }

        SwingContext ctx;
        ctx.attacker = player->GetGUID();
        ctx.damageMult = mult;
        ctx.poiseDamage = poise;

        for (Unit* target : targets)
        {
            tl_swing = &ctx;
            player->AttackerStateUpdate(target, hand, true);
            tl_swing = nullptr;
        }
    }

    // Second half of a double strike: the off-hand weapon swings a moment after the main hand,
    // so the client plays both animations instead of one overriding the other.
    void QueueOffhand(PlayerState* st, uint32 now, float facing, float range, float arc, float mult, float poise)
    {
        st->pendingOffAt = now + Cfg.offhandFollowUpMs;
        if (!st->pendingOffAt)
            st->pendingOffAt = 1;
        st->pendingOffFacing = facing;
        st->pendingOffRange = range;
        st->pendingOffArc = arc;
        st->pendingOffMult = mult;
        st->pendingOffPoise = poise;
    }

    void RunPendingOffhand(Player* player, PlayerState* st, uint32 now)
    {
        if (!st->pendingOffAt || int32(now - st->pendingOffAt) < 0)
            return;
        st->pendingOffAt = 0;
        if (!CanAct(player) || !IsDualWielding(player))
            return;
        DoSwing(player, st->pendingOffFacing, st->pendingOffRange, st->pendingOffArc,
                st->pendingOffMult, st->pendingOffPoise, OFF_ATTACK);
    }

    void DrawMelee(Player* player)
    {
        if (player->GetSheath() != SHEATH_STATE_MELEE)
            player->SetSheath(SHEATH_STATE_MELEE);
    }

    void DoLight(Player* player, PlayerState* st, std::string const& arg, uint32 now)
    {
        if (st->nextLightAt && int32(now - st->nextLightAt) < 0)
            return;
        DrawMelee(player);
        if (!SpendStamina(player, st, Cfg.lightCost, now))
            return;

        if (st->comboIndex > 0 && getMSTimeDiff(st->lastLight, now) > Cfg.comboWindow)
            st->comboIndex = 0;

        uint8 idx = st->comboIndex;
        bool finisher = idx == 2;
        float facing = ClientFacing(player, arg);
        float arc = finisher ? Cfg.lightFinisherArcDeg : Cfg.lightArcDeg;
        float poise = Cfg.lightPoise * (finisher ? 1.5f : 1.0f);
        bool dual = IsDualWielding(player);

        if (!dual)
            DoSwing(player, facing, Cfg.lightRange, arc, Cfg.comboMult[idx], poise);
        else if (idx == 1)
            // right hand, LEFT hand, both
            DoSwing(player, facing, Cfg.lightRange, arc, Cfg.comboMult[idx], poise, OFF_ATTACK);
        else
        {
            DoSwing(player, facing, Cfg.lightRange, arc, Cfg.comboMult[idx], poise, BASE_ATTACK);
            if (finisher)
                QueueOffhand(st, now, facing, Cfg.lightRange, arc, Cfg.comboMult[idx], poise);
        }

        st->lastLight = now;
        st->comboIndex = uint8((idx + 1) % 3);
        st->nextLightAt = now + uint32(float(Cfg.lightCooldown) * (dual ? Cfg.dualWieldCooldownMult : 1.0f));

        std::ostringstream ss;
        ss << "COMBO " << int32(idx + 1);
        SendEvent(player, ss.str().c_str());
    }

    void DoHeavyStart(PlayerState* st, uint32 now)
    {
        st->heavyChargeStart = now;
    }

    void DoHeavyRelease(Player* player, PlayerState* st, std::string const& arg, uint32 now)
    {
        if (!st->heavyChargeStart)
            return;

        uint32 charge = getMSTimeDiff(st->heavyChargeStart, now);
        st->heavyChargeStart = 0;

        if (charge < Cfg.heavyMinCharge)
        {
            SendEvent(player, "HEAVY_CANCEL");
            return;
        }
        if (st->nextHeavyAt && int32(now - st->nextHeavyAt) < 0)
            return;
        if (!SpendStamina(player, st, Cfg.heavyCost, now))
            return;

        float t = std::min(1.0f, float(charge - Cfg.heavyMinCharge) / float(std::max<uint32>(1, Cfg.heavyMaxCharge - Cfg.heavyMinCharge)));
        float mult = Cfg.heavyMinMult + (Cfg.heavyMaxMult - Cfg.heavyMinMult) * t;

        float facing = ClientFacing(player, arg);
        DrawMelee(player);
        DoSwing(player, facing, Cfg.heavyRange, Cfg.heavyArcDeg, mult, Cfg.heavyPoise * (1.0f + t));
        if (IsDualWielding(player))      // cross slash: the left blade follows the right
            QueueOffhand(st, now, facing, Cfg.heavyRange, Cfg.heavyArcDeg, mult, Cfg.heavyPoise * (1.0f + t) * 0.5f);

        st->comboIndex = 0;
        st->nextHeavyAt = now + Cfg.heavyCooldown;
        st->nextLightAt = now + Cfg.lightCooldown;
        SendEvent(player, t >= 1.0f ? "HEAVY_FULL" : "HEAVY");
    }

    // dir: F, B, L, R, FL, FR, BL, BR, or N (no movement -> backstep)
    void DoDodge(Player* player, PlayerState* st, std::string const& dir, std::string const& facingArg, uint32 now)
    {
        if (st->nextDodgeAt && int32(now - st->nextDodgeAt) < 0)
            return;
        if (player->HasUnitState(UNIT_STATE_ROOT) || player->IsFalling() || player->IsInWater())
            return;
        if (!SpendStamina(player, st, Cfg.dodgeCost, now))
            return;

        float offset = 0.0f;
        bool backstep = false;
        if      (dir == "F")  offset = 0.0f;
        else if (dir == "B")  offset = float(M_PI);
        else if (dir == "L")  offset = float(M_PI) / 2.0f;
        else if (dir == "R")  offset = -float(M_PI) / 2.0f;
        else if (dir == "FL") offset = float(M_PI) / 4.0f;
        else if (dir == "FR") offset = -float(M_PI) / 4.0f;
        else if (dir == "BL") offset = 3.0f * float(M_PI) / 4.0f;
        else if (dir == "BR") offset = -3.0f * float(M_PI) / 4.0f;
        else { offset = float(M_PI); backstep = true; }

        float facing = ClientFacing(player, facingArg);
        float angle = Position::NormalizeOrientation(facing + offset);

        st->guarding = false;
        ClearStance(player, st);
        PlayVisual(player, Cfg.dodgeVisualSpell);

        // Custom roll animation from a client patch (e.g. saved in the Custom Spell 01 slot).
        if (Cfg.animEnabled && Cfg.dodgeCustomEmote)
            player->HandleEmoteCommand(Cfg.dodgeCustomEmote);

        // Directional dodge = fast dash along the ground (follows terrain, stops at walls).
        // Backstep keeps the little backward hop, which reads well as an Elden Ring backstep.
        bool dashed = false;
        if (!backstep && Cfg.dodgeStyle == 1 && Cfg.dashSpeed > 0.0f
            && player->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == NULL_MOTION_TYPE)
        {
            player->SetOrientation(facing);
            Position dest = player->GetFirstCollisionPosition(Cfg.dashDistance, offset);
            if (player->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY()) > 1.0f)
            {
                constexpr uint32 DASH_MOVE_ID = 0x41434D; // not EVENT_CHARGE: no charge state / auto-attack
                player->GetMotionMaster()->MoveCharge(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
                                                      Cfg.dashSpeed, DASH_MOVE_ID, nullptr, true);
                dashed = true;
            }
        }

        if (!dashed)
        {
            // KnockbackFrom pushes *away* from the given point, so put the point opposite to where we roll.
            float ox = player->GetPositionX() - std::cos(angle);
            float oy = player->GetPositionY() - std::sin(angle);
            player->KnockbackFrom(ox, oy, backstep ? Cfg.backstepSpeedXY : Cfg.dodgeSpeedXY, Cfg.dodgeSpeedZ);
        }

        st->iframesUntil = now + (backstep ? Cfg.backstepIFrames : Cfg.dodgeIFrames);
        st->nextDodgeAt = now + Cfg.dodgeCooldown;
        st->pendingOffAt = 0;
        st->drawStart = 0;
        st->aiming = false;
        st->comboIndex = 0;
        st->heavyChargeStart = 0;
        st->guarding = false;
        SendEvent(player, backstep ? "BACKSTEP" : "ROLL");
    }

    void StopSprint(Player* player, PlayerState* st);

    void DoGuard(Player* player, PlayerState* st, bool on, uint32 now)
    {
        if (on)
        {
            if (st->guarding)
                return;
            st->guarding = true;
            st->guardStart = now;
            st->parryUsed = false;
            StopSprint(player, st);

            bool magic = IsMagic(player, st);
            if (!magic)
                player->SetSheath(SHEATH_STATE_MELEE);        // raise weapon / shield
            PlayAnim(player, BlockEmote(player, magic));      // snap into guard
            SetStance(player, st, GuardStanceEmote(player, magic));
            if (magic)
                ShowWardBubble(player, st);                   // magic shield around the caster
        }
        else
        {
            st->guarding = false;
            ClearStance(player, st);
        }
        SendEvent(player, on ? "GUARD_ON" : "GUARD_OFF");
    }

    // ------------------------------------------------------------------ magic actions

    // Does the player know any rank of this spell?
    bool KnowsSpellChain(Player* player, uint32 spellId)
    {
        for (uint32 id = sSpellMgr->GetFirstSpellInChain(spellId); id; id = sSpellMgr->GetNextSpellInChain(id))
            if (player->HasSpell(id))
                return true;
        return player->HasSpell(spellId);
    }

    // Element indexes this player may use right now, in the order of the class list.
    std::vector<uint8> AvailableElements(Player* player)
    {
        std::vector<uint8> out;
        uint8 cls = player->getClass();
        if (cls >= 12)
            return out;
        for (uint8 idx : Cfg.classElements[cls])
            if (!Cfg.elementRequireKnown || KnowsSpellChain(player, Cfg.elements[idx].bolt))
                out.push_back(idx);
        return out;
    }

    // The element the player fights with. Classes without an element list fall back to the
    // old single BoltSpell.<Class> setting.
    Element CurrentElement(Player* player, PlayerState* st)
    {
        std::vector<uint8> avail = AvailableElements(player);
        if (!avail.empty())
        {
            if (std::find(avail.begin(), avail.end(), st->element) == avail.end())
                st->element = avail.front();
            return Cfg.elements[st->element];
        }
        Element legacy;
        legacy.name = "Magic";
        uint8 cls = player->getClass();
        legacy.bolt = cls < 12 ? Cfg.boltSpell[cls] : 0;
        return legacy;
    }

    void SendElement(Player* player, PlayerState* st, bool force = false)
    {
        Element e = CurrentElement(player, st);
        std::vector<uint8> avail = AvailableElements(player);
        int32 idx = avail.empty() ? -1 : int32(std::find(avail.begin(), avail.end(), st->element) - avail.begin());
        int32 key = (idx & 0xFF) | (int32(avail.size()) << 8);
        if (!force && key == st->lastSentElement)
            return;
        st->lastSentElement = key;

        std::ostringstream ss;
        ss << "ELEM " << e.name << ' ' << e.bolt << ' ' << int32(idx + 1) << ' ' << avail.size() << ' ';
        for (size_t i = 0; i < avail.size(); ++i)
            ss << (i ? "," : "") << Cfg.elements[avail[i]].name;
        SendToClient(player, ss.str());
    }

    void StartChargeVisual(Player* player, PlayerState* st);
    void StopChargeVisual(Player* player, PlayerState* st);

    // name = element name, "NEXT" or "PREV"
    void SwitchElement(Player* player, PlayerState* st, std::string const& name, uint32 now)
    {
        std::vector<uint8> avail = AvailableElements(player);
        if (avail.empty())
        {
            SendEvent(player, "NOELEMENT");
            return;
        }
        if (st->nextElementSwitchAt && int32(now - st->nextElementSwitchAt) < 0)
        {
            SendEvent(player, "ELEM_WAIT");
            return;
        }

        CurrentElement(player, st);     // makes sure st->element is valid
        size_t pos = std::find(avail.begin(), avail.end(), st->element) - avail.begin();
        uint8 wanted = st->element;

        if (name == "NEXT")
            wanted = avail[(pos + 1) % avail.size()];
        else if (name == "PREV")
            wanted = avail[(pos + avail.size() - 1) % avail.size()];
        else
        {
            bool found = false;
            for (uint8 idx : avail)
                if (strcasecmp(Cfg.elements[idx].name.c_str(), name.c_str()) == 0)
                {
                    wanted = idx;
                    found = true;
                }
            if (!found)
            {
                SendEvent(player, "NOELEMENT");
                SendElement(player, st, true);
                return;
            }
        }

        bool changed = wanted != st->element;
        st->element = wanted;
        if (changed && st->chargeVisualSpell)
        {
            StopChargeVisual(player, st);
            StartChargeVisual(player, st);
        }
        st->comboIndex = 0;
        if (changed)
            st->nextElementSwitchAt = now + Cfg.elementSwitchCooldown;
        SendElement(player, st, true);
    }

    uint32 BoltSpellFor(Player* player)
    {
        return CurrentElement(player, GetState(player)).bolt;
    }

    bool IsBoltCandidate(Player* player, Unit* u, float range)
    {
        return u && u != player && u->IsAlive()
            && player->IsValidAttackTarget(u)
            && player->GetDistance(u) <= range
            && player->IsWithinLOSInMap(u);
    }

    // Enemies inside a cone in front of the player, sorted by how close they are to the aim line.
    std::vector<Unit*> FindAimedTargets(Player* player, float range, float arcDeg)
    {
        std::list<Unit*> found;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, found, check);
        Cell::VisitObjects(player, searcher, range + 5.0f);

        float arc = arcDeg * float(M_PI) / 180.0f;
        std::vector<Unit*> out;
        for (Unit* u : found)
            if (player->isInFront(u, arc) && IsBoltCandidate(player, u, range))
                out.push_back(u);

        auto offAxis = [&](Unit* u)
        {
            float a = std::fabs(player->GetRelativeAngle(u));
            return a > float(M_PI) ? 2.0f * float(M_PI) - a : a;
        };
        std::sort(out.begin(), out.end(), [&](Unit* a, Unit* b)
        {
            // weight angle heavily, distance lightly
            return offAxis(a) * 20.0f + player->GetDistance(a) * 0.1f < offAxis(b) * 20.0f + player->GetDistance(b) * 0.1f;
        });
        return out;
    }

    // Fires the class bolt at a target as a triggered spell with our own damage value.
    // Using the real spell gives the projectile, travel time, impact visual, school, resists,
    // crits and procs for free.
    void FireBolt(Player* player, Unit* target, float mult, float poise, uint32 now, bool canChain = true)
    {
        PlayerState* st = GetState(player);
        Element e = CurrentElement(player, st);
        uint32 spellId = e.bolt;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
        {
            static std::mutex warnLock;
            static std::set<uint32> warned;
            std::lock_guard<std::mutex> lock(warnLock);
            if (warned.insert(spellId).second)
                LOG_ERROR("module", "mod-action-combat: element {} uses unknown spell {}.", e.name, spellId);
            return;
        }

        if (e.damageMult <= 0.0f) e.damageMult = 1.0f;
        if (e.poiseMult <= 0.0f)  e.poiseMult = 1.0f;
        mult *= e.damageMult;
        poise *= e.poiseMult;
        if (e.bonusVsDemonUndead != 1.0f)
        {
            uint32 type = target->GetCreatureType();
            if (type == CREATURE_TYPE_DEMON || type == CREATURE_TYPE_UNDEAD)
                mult *= e.bonusVsDemonUndead;
        }

        // Find which effect does the direct damage
        int8 dmgEff = -1;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
            {
                dmgEff = int8(i);
                break;
            }
        if (dmgEff < 0)
            return;

        float sp = float(std::max(player->SpellBaseDamageBonusDone(info->GetSchoolMask()),
                                  player->SpellBaseHealingBonusDone(info->GetSchoolMask())));
        float dmg = Cfg.boltBaseDmg + Cfg.boltDmgPerLevel * float(player->GetLevel()) + sp * Cfg.boltSpCoef;

        if (Creature* c = target->ToCreature())
        {
            CreatureState* cs = GetState(c);
            if (cs->riposteUntil && int32(now - cs->riposteUntil) < 0)
            {
                mult *= Cfg.riposteMult;
                cs->riposteUntil = 0;
                SendEvent(player, "RIPOSTE");
            }
            else
                ApplyPoise(player, c, poise, now);
        }

        // remember bolts whose hit should heal the caster (resolved when the damage lands)
        if (e.leechPct > 0.0f)
        {
            if (st->pendingLeech.size() > 32)
                st->pendingLeech.erase(st->pendingLeech.begin());
            st->pendingLeech.push_back({ target->GetGUID(), spellId, now + 5000, e.leechPct });
        }

        int32 bp[MAX_SPELL_EFFECTS] = { };
        bp[dmgEff] = std::max<int32>(1, int32(dmg * mult));
        player->CastCustomSpell(target,
                                spellId,
                                dmgEff == 0 ? &bp[0] : nullptr,
                                dmgEff == 1 ? &bp[1] : nullptr,
                                dmgEff == 2 ? &bp[2] : nullptr,
                                true);

        // Lightning: jump to the nearest other enemies around the target, weaker each jump
        if (canChain && e.chainTargets)
        {
            std::list<Unit*> near;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(target, player, e.chainRange);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, near, check);
            Cell::VisitObjects(target, searcher, e.chainRange + 5.0f);
            near.remove_if([&](Unit* u)
            {
                return u == target || !IsBoltCandidate(player, u, Cfg.boltRange + e.chainRange);
            });
            near.sort([&](Unit* a, Unit* b) { return target->GetDistance(a) < target->GetDistance(b); });

            float jumpMult = 1.0f;
            uint32 jumps = 0;
            for (Unit* u : near)
            {
                if (jumps++ >= e.chainTargets)
                    break;
                jumpMult *= e.chainFalloff;
                // undo the element multiplier, FireBolt applies it again
                FireBolt(player, u, mult / e.damageMult * jumpMult, poise / e.poiseMult * jumpMult, now, false);
            }
        }
    }

    void DoBolt(Player* player, PlayerState* st, std::string const& arg, uint32 now)
    {
        if (st->nextLightAt && int32(now - st->nextLightAt) < 0)
            return;

        float facing = ClientFacing(player, arg);
        player->SetOrientation(facing);

        // Soft lock: keep the selected enemy if it's roughly in front, otherwise free-aim.
        Unit* target = player->GetSelectedUnit();
        if (!target || !player->isInFront(target, Cfg.boltSoftLockArcDeg * float(M_PI) / 180.0f)
            || !IsBoltCandidate(player, target, Cfg.boltRange))
        {
            std::vector<Unit*> aimed = FindAimedTargets(player, Cfg.boltRange, Cfg.boltAimArcDeg);
            target = aimed.empty() ? nullptr : aimed.front();
        }

        if (!target)
        {
            SendEvent(player, "NOTARGET");
            return;                     // don't charge mana for shooting at nothing
        }
        Element elem = CurrentElement(player, st);
        if (!SpendMana(player, st, ManaCost(player, Cfg.boltCostPct * elem.costMult), now))
            return;

        if (st->comboIndex > 0 && getMSTimeDiff(st->lastLight, now) > Cfg.comboWindow)
            st->comboIndex = 0;
        uint8 idx = st->comboIndex;

        FireBolt(player, target, Cfg.boltComboMult[idx], Cfg.boltPoise * (idx == 2 ? 1.5f : 1.0f), now);

        st->lastLight = now;
        st->comboIndex = uint8((idx + 1) % 3);
        st->nextLightAt = now + uint32(float(Cfg.boltCooldown) * elem.cooldownMult);

        std::ostringstream ss;
        ss << "COMBO " << int32(idx + 1);
        SendEvent(player, ss.str().c_str());
    }

    // ------------------------------------------------------------------ magic charge wind-up
    // While a caster holds Heavy, we tell nearby clients (and their own) that they started casting
    // their element's spell, with the full charge time as cast time. Nothing is actually cast:
    // the client just plays that spell's own wind-up: element-colored glowing hands, the cast
    // animation, the cast sound and a cast bar that fills as the charge meter. The real bolts fired
    // on release send the normal "cast finished" packet, which ends the wind-up; any other ending
    // (dodge, too-short release, no mana / target, element switch) sends "interrupted".

    void StartChargeVisual(Player* player, PlayerState* st)
    {
        if (!Cfg.animEnabled || !Cfg.magicChargeCast)
            return;
        uint32 spellId = CurrentElement(player, st).bolt;
        if (!spellId || !sSpellMgr->GetSpellInfo(spellId))
            return;

        WorldPacket data(SMSG_SPELL_START, 8 + 8 + 1 + 4 + 4 + 4 + 4);
        data << player->GetPackGUID();                  // cast item / caster
        data << player->GetPackGUID();                  // caster
        data << uint8(0);                               // cast count
        data << uint32(spellId);
        data << uint32(CAST_FLAG_HAS_TRAJECTORY);
        data << int32(Cfg.heavyMaxCharge);              // cast time = full charge time
        data << uint32(TARGET_FLAG_NONE);               // no target: wind-up plays on the caster
        player->SendMessageToSet(&data, true);
        st->chargeVisualSpell = spellId;
    }

    void StopChargeVisual(Player* player, PlayerState* st)
    {
        if (!st->chargeVisualSpell)
            return;
        uint32 spellId = st->chargeVisualSpell;
        st->chargeVisualSpell = 0;

        WorldPacket data(SMSG_SPELL_FAILURE, 8 + 1 + 4 + 1);
        data << player->GetPackGUID();
        data << uint8(0);
        data << uint32(spellId);
        data << uint8(SPELL_FAILED_INTERRUPTED);
        player->SendMessageToSet(&data, true);

        data.Initialize(SMSG_SPELL_FAILED_OTHER, 8 + 1 + 4 + 1);
        data << player->GetPackGUID();
        data << uint8(0);
        data << uint32(spellId);
        data << uint8(SPELL_FAILED_INTERRUPTED);
        player->SendMessageToSet(&data, true);
    }

    void DoVolley(Player* player, PlayerState* st, std::string const& arg, uint32 now)
    {
        if (!st->heavyChargeStart)
            return;

        uint32 charge = getMSTimeDiff(st->heavyChargeStart, now);
        st->heavyChargeStart = 0;

        if (charge < Cfg.heavyMinCharge)
        {
            SendEvent(player, "HEAVY_CANCEL");
            return;
        }
        if (st->nextHeavyAt && int32(now - st->nextHeavyAt) < 0)
            return;

        player->SetOrientation(ClientFacing(player, arg));
        std::vector<Unit*> targets = FindAimedTargets(player, Cfg.volleyRange, Cfg.volleyArcDeg);
        if (targets.empty())
        {
            SendEvent(player, "NOTARGET");
            return;
        }
        Element elem = CurrentElement(player, st);
        if (!SpendMana(player, st, ManaCost(player, Cfg.volleyCostPct * elem.costMult), now))
            return;

        float t = std::min(1.0f, float(charge - Cfg.heavyMinCharge) / float(std::max<uint32>(1, Cfg.heavyMaxCharge - Cfg.heavyMinCharge)));
        float mult = Cfg.volleyMinMult + (Cfg.volleyMaxMult - Cfg.volleyMinMult) * t;

        if (targets.size() > Cfg.maxTargets)
            targets.resize(Cfg.maxTargets);

        // The bolts' own "cast finished" ends the wind-up, as long as it's the same spell.
        if (st->chargeVisualSpell == elem.bolt)
            st->chargeVisualSpell = 0;
        else
            StopChargeVisual(player, st);

        for (Unit* u : targets)
            FireBolt(player, u, mult, Cfg.volleyPoise * (1.0f + t), now);

        st->comboIndex = 0;
        st->nextHeavyAt = now + uint32(float(Cfg.volleyCooldown) * elem.cooldownMult);
        st->nextLightAt = now + uint32(float(Cfg.boltCooldown) * elem.cooldownMult);
        SendEvent(player, t >= 1.0f ? "HEAVY_FULL" : "HEAVY");
    }

    // Short-range teleport with i-frames instead of a roll.
    void DoBlink(Player* player, PlayerState* st, std::string const& dir, std::string const& facingArg, uint32 now)
    {
        if (st->nextDodgeAt && int32(now - st->nextDodgeAt) < 0)
            return;
        if (player->HasUnitState(UNIT_STATE_ROOT) || player->IsFalling())
            return;
        if (!SpendMana(player, st, ManaCost(player, Cfg.blinkCostPct), now))
            return;

        float offset = float(M_PI);
        bool backstep = false;
        if      (dir == "F")  offset = 0.0f;
        else if (dir == "B")  offset = float(M_PI);
        else if (dir == "L")  offset = float(M_PI) / 2.0f;
        else if (dir == "R")  offset = -float(M_PI) / 2.0f;
        else if (dir == "FL") offset = float(M_PI) / 4.0f;
        else if (dir == "FR") offset = -float(M_PI) / 4.0f;
        else if (dir == "BL") offset = 3.0f * float(M_PI) / 4.0f;
        else if (dir == "BR") offset = -3.0f * float(M_PI) / 4.0f;
        else backstep = true;

        float facing = ClientFacing(player, facingArg);
        player->SetOrientation(facing);

        // Stops at walls/terrain, same as the real Blink.
        Position dest = player->GetFirstCollisionPosition(backstep ? Cfg.backBlinkDistance : Cfg.blinkDistance, offset);
        st->guarding = false;
        ClearStance(player, st);

        uint8 cls = player->getClass() < 12 ? player->getClass() : 0;
        Position origin = player->GetPosition();
        PlayEffectAt(player, origin, Cfg.blinkDepart[cls]);         // flash where you vanish
        PlayVisual(player, Cfg.blinkVisualSpell);
        player->NearTeleportTo(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), facing);
        PlayEffectAt(player, dest, Cfg.blinkArrive[cls]);           // flash where you appear
        if (Cfg.animEnabled && Cfg.blinkSound[cls])
            player->PlayDistanceSound(Cfg.blinkSound[cls]);         // heard by players nearby

        st->iframesUntil = now + (backstep ? Cfg.backstepIFrames : Cfg.dodgeIFrames);
        st->nextDodgeAt = now + Cfg.dodgeCooldown;
        st->pendingOffAt = 0;
        st->drawStart = 0;
        st->aiming = false;
        st->comboIndex = 0;
        st->heavyChargeStart = 0;
        st->guarding = false;
        SendEvent(player, backstep ? "BACKSTEP" : "BLINK");
    }

    // ------------------------------------------------------------------ ranged actions

    bool HasAmmo(Player* player)
    {
        Item* item = player->GetWeaponForAttack(RANGED_ATTACK, true);
        if (!item)
            return false;
        if (item->GetTemplate()->InventoryType == INVTYPE_THROWN || sWorld->getBoolConfig(CONFIG_ENABLE_INFINITEAMMO))
            return true;
        uint32 ammo = player->GetUInt32Value(PLAYER_AMMO_ID);
        return ammo && player->HasItemCount(ammo, 1);
    }

    float ShotMinRange()
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(Cfg.shootSpell);
        return info ? info->GetMinRange(false) : 0.0f;
    }

    // 0 = must roll back, 1 = point-blank shots, 2 = swap to melee weapon (else point-blank)
    uint8 CloseMode(PlayerState const* st)
    {
        return st->closeOverride ? uint8(st->closeOverride - 1) : Cfg.closeRangeMode;
    }

    bool HasMeleeWeapon(Player* player)
    {
        Item* item = player->GetWeaponForAttack(BASE_ATTACK, true);
        return item && !item->IsBroken();
    }

    // Can a target inside the shot's minimum range still be shot?
    bool PointBlankAllowed(Player* player, PlayerState const* st)
    {
        uint8 mode = CloseMode(st);
        return mode == 1 || (mode == 2 && !HasMeleeWeapon(player));
    }

    bool IsShotCandidate(Player* player, bool pointBlank, Unit* u, float range)
    {
        return IsBoltCandidate(player, u, range) && (pointBlank || player->GetDistance(u) >= ShotMinRange());
    }

    // A shot at a target inside the "Shoot" spell's minimum range. The spell can't be cast there,
    // so roll the ranged weapon damage ourselves and deliver it like a spell hit (armor, crit,
    // absorb, resilience and combat text all apply).
    void PointBlankShot(Player* player, Unit* target, SpellInfo const* info, float mult)
    {
        uint32 school = SPELL_SCHOOL_MASK_NORMAL;
        int32 dmg = int32(float(player->CalculateDamage(RANGED_ATTACK, false, true)) * mult * Cfg.pointBlankMult);

        float critChance = target->SpellTakenCritChance(player, info, SpellSchoolMask(school),
            player->SpellDoneCritChance(target, info, SpellSchoolMask(school), RANGED_ATTACK, true), RANGED_ATTACK, true);
        bool crit = roll_chance_f(critChance);

        SpellNonMeleeDamage dmgInfo(player, target, info, school);
        player->CalculateSpellDamageTaken(&dmgInfo, std::max(1, dmg), info, RANGED_ATTACK, crit);
        Unit::DealDamageMods(target, dmgInfo.damage, &dmgInfo.absorb);
        player->SendSpellNonMeleeDamageLog(&dmgInfo);
        player->DealSpellDamage(&dmgInfo, true);
    }

    // Fires the real "Shoot" spell (projectile, ammo, weapon damage, crits, procs) with extra
    // damage so the total is roughly weapon damage * mult.
    void FireShot(Player* player, Unit* target, float mult, float poise, uint32 now)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(Cfg.shootSpell);
        if (!info || info->IsAutoRepeatRangedSpell())
        {
            // Auto-repeat spells (Auto Shot, the player "Shoot") would start the auto-shot loop
            // instead of firing once, so they can't be used here.
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                LOG_ERROR("module", "mod-action-combat: ActionCombat.Ranged.ShootSpellId {} is missing or auto-repeat; use a single-fire shot such as 6660.", Cfg.shootSpell);
            }
            return;
        }

        int8 eff = -1;
        bool isSchoolDamage = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            switch (info->Effects[i].Effect)
            {
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                    eff = int8(i);
                    break;
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                    eff = int8(i);
                    isSchoolDamage = true;
                    break;
                default:
                    continue;
            }
            break;
        }
        if (eff < 0)
            return;

        if (Creature* c = target->ToCreature())
        {
            CreatureState* cs = GetState(c);
            if (cs->riposteUntil && int32(now - cs->riposteUntil) < 0)
            {
                mult *= Cfg.riposteMult;
                cs->riposteUntil = 0;
                SendEvent(player, "RIPOSTE");
            }
            else
                ApplyPoise(player, c, poise, now);
        }

        if (player->GetDistance(target) < info->GetMinRange(false))
        {
            PointBlankShot(player, target, info, mult);
            return;
        }

        float avg = (player->GetWeaponDamageRange(RANGED_ATTACK, MINDAMAGE) + player->GetWeaponDamageRange(RANGED_ATTACK, MAXDAMAGE)) / 2.0f;
        // Weapon-damage effects add their base points on top of the weapon roll;
        // a plain school-damage effect uses base points as the whole hit.
        int32 value = isSchoolDamage ? int32(avg * mult) : int32(avg * (mult - 1.0f));

        int32 bp[MAX_SPELL_EFFECTS] = { };
        bp[eff] = std::max<int32>(isSchoolDamage ? 1 : 0, value);
        player->CastCustomSpell(target, Cfg.shootSpell,
                                eff == 0 ? &bp[0] : nullptr,
                                eff == 1 ? &bp[1] : nullptr,
                                eff == 2 ? &bp[2] : nullptr,
                                true);
    }

    // Common checks before any shot; returns false (and tells the client why) if we can't shoot.
    bool ReadyToShoot(Player* player)
    {
        if (!HasRangedWeapon(player))
        {
            SendEvent(player, "NOWEAPON");
            return false;
        }
        if (!HasAmmo(player))
        {
            SendEvent(player, "NOAMMO");
            return false;
        }
        return true;
    }

    bool IsGunLike(Player* player)
    {
        Item* item = player->GetWeaponForAttack(RANGED_ATTACK, true);
        return item && (item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_GUN
                     || item->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW);
    }

    void DoShot(Player* player, PlayerState* st, std::string const& arg, uint32 now, bool dedicatedKey)
    {
        if (st->nextShotAt && int32(now - st->nextShotAt) < 0)
            return;
        if (!ReadyToShoot(player))
            return;

        bool pb = dedicatedKey ? CloseMode(st) != 0 : PointBlankAllowed(player, st);
        player->SetOrientation(ClientFacing(player, arg));

        // Soft lock on the selected enemy, otherwise free-aim along the camera.
        Unit* target = player->GetSelectedUnit();
        if (!target || !player->isInFront(target, Cfg.shotSoftLockArcDeg * float(M_PI) / 180.0f)
            || !IsShotCandidate(player, pb, target, Cfg.shotRange))
        {
            target = nullptr;
            for (Unit* u : FindAimedTargets(player, Cfg.shotRange, Cfg.shotAimArcDeg))
                if (IsShotCandidate(player, pb, u, Cfg.shotRange))
                {
                    target = u;
                    break;
                }
        }
        if (!target)
        {
            // Distinguish "too close" so archers know to roll back
            std::vector<Unit*> near = FindAimedTargets(player, ShotMinRange(), Cfg.shotSoftLockArcDeg);
            SendEvent(player, near.empty() ? "NOTARGET" : "TOOCLOSE");
            return;
        }
        if (!SpendStamina(player, st, Cfg.shotCost, now))
            return;

        if (st->shotCombo > 0 && getMSTimeDiff(st->lastShot, now) > Cfg.comboWindow)
            st->shotCombo = 0;
        uint8 idx = st->shotCombo;
        player->SetSheath(SHEATH_STATE_RANGED);

        FireShot(player, target, Cfg.shotComboMult[idx], Cfg.shotPoise, now);

        st->lastShot = now;
        st->shotCombo = uint8((idx + 1) % 3);
        st->nextShotAt = now + Cfg.shotCooldown;

        std::ostringstream ss;
        ss << "COMBO " << int32(idx + 1);
        SendEvent(player, ss.str().c_str());
    }

    void DoDrawStart(Player* player, PlayerState* st, uint32 now)
    {
        if (!ReadyToShoot(player))
            return;
        st->drawStart = now;
        st->aiming = true;
        StopSprint(player, st);
        player->SetSheath(SHEATH_STATE_RANGED);
        PlayAnim(player, IsGunLike(player) ? EMOTE_ONESHOT_READY_RIFLE : EMOTE_ONESHOT_READY_BOW);
    }

    // Hold to draw, release to fire. A full draw becomes a piercing shot that hits
    // every enemy along the aim line.
    void DoPowerShot(Player* player, PlayerState* st, std::string const& arg, uint32 now, bool dedicatedKey)
    {
        st->aiming = false;
        if (!st->drawStart)
            return;

        uint32 charge = getMSTimeDiff(st->drawStart, now);
        st->drawStart = 0;

        if (charge < Cfg.heavyMinCharge)
        {
            SendEvent(player, "HEAVY_CANCEL");
            return;
        }
        if (st->nextPowerShotAt && int32(now - st->nextPowerShotAt) < 0)
            return;
        if (!ReadyToShoot(player))
            return;

        bool pb = dedicatedKey ? CloseMode(st) != 0 : PointBlankAllowed(player, st);
        float t = std::min(1.0f, float(charge - Cfg.heavyMinCharge) / float(std::max<uint32>(1, Cfg.heavyMaxCharge - Cfg.heavyMinCharge)));
        float mult = Cfg.powerShotMinMult + (Cfg.powerShotMaxMult - Cfg.powerShotMinMult) * t;
        bool pierce = t >= 1.0f;

        player->SetOrientation(ClientFacing(player, arg));

        std::vector<Unit*> targets;
        if (pierce)
        {
            // everything whose body touches the line in front of us, nearest first
            std::list<Unit*> found;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, Cfg.powerShotRange);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, found, check);
            Cell::VisitObjects(player, searcher, Cfg.powerShotRange + 5.0f);
            for (Unit* u : found)
            {
                if (!IsShotCandidate(player, pb, u, Cfg.powerShotRange))
                    continue;
                float a = player->GetRelativeAngle(u);
                float d = player->GetDistance(u);
                if (std::cos(a) <= 0.0f)
                    continue;
                if (std::fabs(d * std::sin(a)) <= Cfg.pierceWidth + u->GetCombatReach())
                    targets.push_back(u);
            }
            std::sort(targets.begin(), targets.end(), [&](Unit* a, Unit* b) { return player->GetDistance(a) < player->GetDistance(b); });
            if (targets.size() > Cfg.pierceMaxTargets)
                targets.resize(Cfg.pierceMaxTargets);
        }
        if (targets.empty())
        {
            Unit* sel = player->GetSelectedUnit();
            if (sel && player->isInFront(sel, Cfg.shotSoftLockArcDeg * float(M_PI) / 180.0f) && IsShotCandidate(player, pb, sel, Cfg.powerShotRange))
                targets.push_back(sel);
            else
                for (Unit* u : FindAimedTargets(player, Cfg.powerShotRange, Cfg.shotAimArcDeg))
                    if (IsShotCandidate(player, pb, u, Cfg.powerShotRange))
                    {
                        targets.push_back(u);
                        break;
                    }
        }
        if (targets.empty())
        {
            SendEvent(player, "NOTARGET");
            return;
        }
        if (!SpendStamina(player, st, Cfg.powerShotCost, now))
            return;

        player->SetSheath(SHEATH_STATE_RANGED);
        for (Unit* u : targets)
            FireShot(player, u, mult, Cfg.powerShotPoise * (1.0f + t), now);

        st->shotCombo = 0;
        st->nextPowerShotAt = now + Cfg.powerShotCooldown;
        st->nextShotAt = now + Cfg.shotCooldown;
        SendEvent(player, pierce ? "PIERCE" : "HEAVY");
    }

    // ------------------------------------------------------------------ sprint

    void StopSprint(Player* player, PlayerState* st)
    {
        if (!st->sprinting)
            return;
        st->sprinting = false;
        st->sprintTargetRate = 0.0f;
        player->UpdateSpeed(MOVE_RUN, true);     // back to normal (keeps buffs/debuffs)
        SendEvent(player, "SPRINT_OFF");
    }

    bool CanSprint(Player* player)
    {
        return Cfg.sprintEnabled && player->IsAlive() && !player->IsMounted() && !player->GetVehicle()
            && !player->IsFlying() && !player->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_ROOT)
            && (Cfg.sprintInCombat || !player->IsInCombat());
    }

    void StartSprint(Player* player, PlayerState* st)
    {
        if (st->sprinting || !CanSprint(player))
            return;

        if (IsMagic(player, st))
        {
            if (player->GetPower(POWER_MANA) < ManaCost(player, Cfg.sprintManaPctPerSec))
            {
                SendEvent(player, "NOMANA");
                return;
            }
        }
        else if (st->stamina < Cfg.sprintMinStart)
        {
            SendEvent(player, "NOSTAM");
            return;
        }

        st->sprinting = true;
        st->guarding = false;
        ClearStance(player, st);
        player->UpdateSpeed(MOVE_RUN, true);
        st->sprintTargetRate = player->GetSpeedRate(MOVE_RUN) * Cfg.sprintMult;
        player->SetSpeed(MOVE_RUN, st->sprintTargetRate, true);
        SendEvent(player, "SPRINT_ON");
    }

    void UpdateSprint(Player* player, PlayerState* st, uint32 diff, uint32 now)
    {
        if (!st->sprinting)
            return;
        if (!CanSprint(player))
        {
            StopSprint(player, st);
            return;
        }

        // Something (a slow, a buff, a form) recalculated our speed: re-apply on top of it.
        if (player->GetSpeedRate(MOVE_RUN) != st->sprintTargetRate)
        {
            player->UpdateSpeed(MOVE_RUN, true);
            st->sprintTargetRate = player->GetSpeedRate(MOVE_RUN) * Cfg.sprintMult;
            player->SetSpeed(MOVE_RUN, st->sprintTargetRate, true);
        }

        if (!player->isMoving())
            return;     // standing still while holding sprint is free

        if (IsMagic(player, st))
        {
            st->manaRegenAcc -= float(player->GetCreateMana()) * Cfg.sprintManaPctPerSec / 100.0f * float(diff) / 1000.0f;
            st->lastManaSpend = now;
            if (st->manaRegenAcc <= -1.0f)
            {
                int32 cost = int32(-st->manaRegenAcc);
                st->manaRegenAcc += float(cost);
                if (player->GetPower(POWER_MANA) < uint32(cost))
                {
                    SendEvent(player, "NOMANA");
                    StopSprint(player, st);
                    return;
                }
                player->ModifyPower(POWER_MANA, -cost);
            }
            return;
        }

        st->stamina -= Cfg.sprintDrain * float(diff) / 1000.0f;
        st->lastSpend = now;
        if (st->stamina <= 0.0f)
        {
            st->stamina = 0.0f;
            SendEvent(player, "NOSTAM");
            StopSprint(player, st);
        }
    }

    // Enemy right in front of a ranged fighter who has a melee weapon (and swap mode is on)?
    bool ShouldSwapToMelee(Player* player, PlayerState const* st, std::string const& facingArg)
    {
        if (CloseMode(st) != 2 || !HasMeleeWeapon(player))
            return false;

        player->SetOrientation(ClientFacing(player, facingArg));
        float arc = Cfg.shotSoftLockArcDeg * float(M_PI) / 180.0f;

        if (Unit* sel = player->GetSelectedUnit())
            if (IsBoltCandidate(player, sel, Cfg.closeRange) && player->isInFront(sel, arc))
                return true;

        return !FindAimedTargets(player, Cfg.closeRange, Cfg.shotSoftLockArcDeg).empty();
    }

    // Draws the right weapon on the character model and tells the addon, only when it changes.
    void SetWeaponSwap(Player* player, PlayerState* st, bool melee)
    {
        if (st->swappedToMelee == melee)
            return;
        st->swappedToMelee = melee;
        st->comboIndex = 0;
        player->SetSheath(melee ? SHEATH_STATE_MELEE : SHEATH_STATE_RANGED);
        SendEvent(player, melee ? "SWAP_MELEE" : "SWAP_RANGED");
    }

    void SyncMode(Player* player, PlayerState* st)
    {
        int8 mode = int8(GetStyle(player, st));
        if (mode == st->lastSentMode)
            return;
        st->lastSentMode = mode;
        st->comboIndex = 0;
        st->heavyChargeStart = 0;
        st->swappedToMelee = false;
        SendToClient(player, mode == STYLE_MAGIC ? "MODE MAGIC" : mode == STYLE_RANGED ? "MODE RANGED" : "MODE MELEE");
        if (mode == STYLE_MAGIC)
            SendElement(player, st, true);
    }

    // ------------------------------------------------------------------ interact key
    // Loot / talk / use whatever is in front of the crosshair, so players never have to let go of
    // the camera. Every action goes through the same WorldSession handler a real mouse click
    // uses, so range, faction, quest and loot-rights checks are exactly the normal ones.

    enum InteractKind : uint8 { IK_NONE, IK_LOOT, IK_NPC, IK_OBJECT };

    struct InteractChoice
    {
        InteractKind kind = IK_NONE;
        ObjectGuid guid;
        std::string name;
        uint32 npcFlags = 0;
    };

    struct NearCreatureCheck
    {
        WorldObject const* src; float range;
        bool operator()(Creature* c) const { return src->IsWithinDistInMap(c, range); }
    };

    struct NearGameObjectCheck
    {
        WorldObject const* src; float range;
        bool operator()(GameObject* g) const { return src->IsWithinDistInMap(g, range); }
    };

    bool IsUsableObject(Player* player, GameObject* go)
    {
        if (!go->isSpawned() || go->HasGameObjectFlag(GameObjectFlags(GO_FLAG_IN_USE | GO_FLAG_LOCKED | GO_FLAG_NOT_SELECTABLE)))
            return false;
        // quest-only objects: only when the player can actually use them
        if (go->HasGameObjectFlag(GO_FLAG_INTERACT_COND) && !go->ActivateToQuest(player))
            return false;
        // anything with a lock (herbs, ore, locked chests) needs its proper spell: use the mouse
        if (go->GetGOInfo()->GetLockId())
            return false;
        switch (go->GetGoType())
        {
            case GAMEOBJECT_TYPE_CHEST:
            case GAMEOBJECT_TYPE_GOOBER:
            case GAMEOBJECT_TYPE_QUESTGIVER:
            case GAMEOBJECT_TYPE_MAILBOX:
            case GAMEOBJECT_TYPE_BUTTON:
            case GAMEOBJECT_TYPE_DOOR:
            case GAMEOBJECT_TYPE_CHAIR:
            case GAMEOBJECT_TYPE_SPELLCASTER:
            case GAMEOBJECT_TYPE_MEETINGSTONE:
            case GAMEOBJECT_TYPE_BARBER_CHAIR:
                return go->IsWithinDistInMap(player, go->GetInteractionDistance());
            default:
                return false;
        }
    }

    InteractChoice FindInteractable(Player* player, std::string const& facingArg)
    {
        player->SetOrientation(ClientFacing(player, facingArg));
        float arc = Cfg.interactArcDeg * float(M_PI) / 180.0f;
        InteractChoice best;
        float bestScore = 1e9f;

        auto consider = [&](WorldObject* obj, InteractKind kind, uint32 npcFlags)
        {
            float d = player->GetDistance(obj);
            bool inFront = player->isInFront(obj, arc);
            if (!inFront && d > 2.0f)           // right at your feet counts in any direction
                return;
            float a = std::fabs(player->GetRelativeAngle(obj));
            if (a > float(M_PI)) a = 2.0f * float(M_PI) - a;
            float score = a * 8.0f + d;
            if (kind == IK_LOOT)
                score -= 1.0f;                  // corpses win ties: looting is the common case
            if (score < bestScore)
            {
                bestScore = score;
                best.kind = kind;
                best.guid = obj->GetGUID();
                best.name = obj->GetName();
                best.npcFlags = npcFlags;
            }
        };

        std::list<Creature*> creatures;
        NearCreatureCheck ccheck{ player, Cfg.interactRange };
        Acore::CreatureListSearcher<NearCreatureCheck> csearch(player, creatures, ccheck);
        Cell::VisitObjects(player, csearch, Cfg.interactRange);
        for (Creature* c : creatures)
        {
            if (!c->IsAlive())
            {
                // same range the loot handler enforces, so we never pick a corpse that then fails
                if (c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE) && player->isAllowedToLoot(c)
                    && c->IsWithinDistInMap(player, INTERACTION_DISTANCE))
                    consider(c, IK_LOOT, 0);
                continue;
            }
            uint32 flags = c->GetNpcFlags();
            if (flags && !c->IsHostileTo(player) && !player->IsHostileTo(c) && !c->IsInCombat())
                consider(c, IK_NPC, flags);
        }

        std::list<GameObject*> objects;
        NearGameObjectCheck gcheck{ player, Cfg.interactRange };
        Acore::GameObjectListSearcher<NearGameObjectCheck> gsearch(player, objects, gcheck);
        Cell::VisitObjects(player, gsearch, Cfg.interactRange);
        for (GameObject* go : objects)
            if (IsUsableObject(player, go))
                consider(go, IK_OBJECT, 0);

        return best;
    }

    // What the prompt under the crosshair should say
    char const* InteractVerb(InteractChoice const& c)
    {
        switch (c.kind)
        {
            case IK_LOOT:   return "LOOT";
            case IK_OBJECT: return "USE";
            case IK_NPC:
                if (c.npcFlags & (UNIT_NPC_FLAG_VENDOR_MASK | UNIT_NPC_FLAG_REPAIR)) return "TRADE";
                if (c.npcFlags & UNIT_NPC_FLAG_QUESTGIVER) return "TALK";
                return "TALK";
            default:        return "NONE";
        }
    }

    void SendPeek(Player* player, std::string const& facingArg)
    {
        InteractChoice c = FindInteractable(player, facingArg);
        if (c.kind == IK_NONE)
            SendToClient(player, "P NONE");
        else
            SendToClient(player, std::string("P ") + InteractVerb(c) + " " + c.name);
    }

    // The 3.3.5 client ignores a loot window it didn't ask for, so after the server opens the
    // loot we pick it up right here through the same handlers the loot window's clicks use:
    // money first, then every item this player may take, then close the loot. Items that can't
    // be taken now (bags full, group roll pending, someone else's) simply stay on the corpse.
    void PickUpOpenedLoot(Player* player, ObjectGuid guid)
    {
        if (player->GetLootGUID() != guid)
        {
            SendEvent(player, "NOLOOT");                // out of range / not allowed after all
            return;
        }

        Loot* loot = nullptr;
        if (guid.IsGameObject())
        {
            if (GameObject* go = player->GetMap()->GetGameObject(guid))
                loot = &go->loot;
        }
        else if (Creature* c = player->GetMap()->GetCreature(guid))
            loot = &c->loot;
        if (!loot)
            return;

        WorldSession* session = player->GetSession();

        if (loot->gold)
        {
            WorldPacket money(CMSG_LOOT_MONEY, 0);
            session->HandleLootMoneyOpcode(money);
        }

        Group* group = player->GetGroup();
        bool notMasterLooter = group && group->GetLootMethod() == MASTER_LOOT && group->GetMasterLooterGuid() != player->GetGUID();
        size_t slots = std::min<size_t>(255, loot->items.size() + loot->quest_items.size());

        for (size_t i = 0; i < slots && player->GetLootGUID() == guid; ++i)
        {
            // the same checks Player::StoreLootItem does, so we never trigger its error/release paths
            QuestItem* qitem = nullptr;
            QuestItem* ffaitem = nullptr;
            QuestItem* conditem = nullptr;
            LootItem* item = loot->LootItemInSlot(uint32(i), player, &qitem, &ffaitem, &conditem);
            if (!item || item->is_looted)
                continue;
            if (!qitem && item->is_blocked)
                continue;                               // waiting on a group roll
            if (item->rollWinnerGUID && item->rollWinnerGUID != player->GetGUID())
                continue;
            if (!item->AllowedForPlayer(player, loot->sourceWorldObjectGUID))
                continue;
            if (notMasterLooter && !item->is_underthreshold && loot->roundRobinPlayer && !qitem && !ffaitem && !conditem)
                continue;

            WorldPacket take(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            take << uint8(i);
            session->HandleAutostoreLootItemOpcode(take);
        }

        if (player->GetLootGUID() == guid)
        {
            WorldPacket release(CMSG_LOOT_RELEASE, 8);
            release << guid;
            session->HandleLootReleaseOpcode(release);
        }
        SendEvent(player, "LOOTED");
    }

    void DoInteract(Player* player, std::string const& facingArg)
    {
        if (!player->IsAlive() || player->HasUnitState(UNIT_STATE_LOST_CONTROL))
            return;

        InteractChoice c = FindInteractable(player, facingArg);
        WorldSession* session = player->GetSession();

        switch (c.kind)
        {
            case IK_LOOT:
            {
                WorldPacket pkt(CMSG_LOOT, 8);
                pkt << c.guid;
                session->HandleLootOpcode(pkt);
                PickUpOpenedLoot(player, c.guid);
                return;
            }
            case IK_OBJECT:
            {
                WorldPacket pkt(CMSG_GAMEOBJ_USE, 8);
                pkt << c.guid;
                session->HandleGameObjectUseOpcode(pkt);
                if (player->GetLootGUID() == c.guid)    // a chest opened its loot: pick it up too
                    PickUpOpenedLoot(player, c.guid);
                else
                    SendEvent(player, "INTERACT");
                return;
            }
            case IK_NPC:
            {
                // Same choice the game client makes when you right-click an NPC.
                if (c.npcFlags & UNIT_NPC_FLAG_GOSSIP)
                {
                    WorldPacket pkt(CMSG_GOSSIP_HELLO, 8);
                    pkt << c.guid;
                    session->HandleGossipHelloOpcode(pkt);
                }
                else if (c.npcFlags & UNIT_NPC_FLAG_QUESTGIVER)
                {
                    WorldPacket pkt(CMSG_QUESTGIVER_HELLO, 8);
                    pkt << c.guid;
                    session->HandleQuestgiverHelloOpcode(pkt);
                }
                else if (Creature* npc = player->GetNPCIfCanInteractWith(c.guid, UNIT_NPC_FLAG_NONE))
                {
                    if (c.npcFlags & UNIT_NPC_FLAG_VENDOR_MASK)
                        session->SendListInventory(c.guid);
                    else if (c.npcFlags & UNIT_NPC_FLAG_TRAINER)
                        session->SendTrainerList(npc);
                    else if (c.npcFlags & UNIT_NPC_FLAG_FLIGHTMASTER)
                    {
                        if (!session->SendLearnNewTaxiNode(npc))
                            session->SendTaxiMenu(npc);
                    }
                    else if (c.npcFlags & UNIT_NPC_FLAG_BANKER)
                        session->SendShowBank(c.guid);
                    else if (c.npcFlags & UNIT_NPC_FLAG_AUCTIONEER)
                        session->SendAuctionHello(c.guid, npc);
                    else if (c.npcFlags & UNIT_NPC_FLAG_STABLEMASTER)
                        session->SendStablePet(c.guid);
                }
                SendEvent(player, "INTERACT");
                return;
            }
            default:
                SendEvent(player, "NOTHING");
                return;
        }
    }

    void Execute(Player* player, PlayerState* st, std::string const& line, uint32 now)
    {
        std::istringstream in(line);
        std::string cmd, a1, a2;
        in >> cmd >> a1 >> a2;

        if (cmd == "HELLO")
        {
            st->addonPresent = true;
            st->lastSentMode = -1;
            SendStamina(player, st);
            SendToClient(player, "V 3");
            SyncMode(player, st);
            if (IsMagic(player, st))
                SendElement(player, st, true);
            return;
        }

        if (cmd == "CLOSE")             // /acm close default | off | shoot | swap
        {
            st->closeOverride = a1 == "OFF" ? 1 : a1 == "SHOOT" ? 2 : a1 == "SWAP" ? 3 : 0;
            static char const* names[] = { "OFF", "SHOOT", "SWAP" };
            SendToClient(player, std::string("CLOSE ") + names[CloseMode(st)]);
            return;
        }

        if (cmd == "STYLE")             // /acm auto | melee | magic
        {
            st->styleOverride = a1 == "MELEE" ? 1 : a1 == "MAGIC" ? 2 : a1 == "RANGED" ? 3 : 0;
            if (st->styleOverride == 3 && !CanUseRanged(player))
                SendEvent(player, "NOWEAPON");
            SyncMode(player, st);
            return;
        }

        if (cmd == "EL")                // EL <name> | EL NEXT | EL PREV | EL (just report)
        {
            // Always answer, so the player is never left wondering why nothing happened.
            if (!IsMagic(player, st))
            {
                SendEvent(player, CanUseMagic(player) ? "ELEM_NOTMAGIC" : "ELEM_NOCLASS");
                return;
            }
            if (AvailableElements(player).size() < 2 && !a1.empty())
            {
                SendEvent(player, "ELEM_ONLYONE");
                SendElement(player, st, true);
                return;
            }
            if (a1.empty())
                SendElement(player, st, true);
            else
            {
                std::string want = a1;
                std::transform(want.begin(), want.end(), want.begin(), ::toupper);
                SwitchElement(player, st, (want == "NEXT" || want == "PREV") ? want : a1, now);
            }
            return;
        }

        if (cmd == "SS")
        {
            StartSprint(player, st);
            return;
        }
        if (cmd == "SE")
        {
            StopSprint(player, st);
            return;
        }

        // Dedicated shot keys: work in every style as long as a bow / gun / crossbow is equipped.
        if (cmd == "R" || cmd == "RS" || cmd == "RR")
        {
            if (!CanUseRanged(player))
            {
                st->aiming = false;
                st->drawStart = 0;
                SendEvent(player, "NOWEAPON");
                return;
            }
            if (!CanAct(player))
                return;
            if      (cmd == "R")  DoShot(player, st, a1, now, true);
            else if (cmd == "RS") DoDrawStart(player, st, now);
            else                  DoPowerShot(player, st, a1, now, true);
            return;
        }

        Style style = GetStyle(player, st);
        bool magic = style == STYLE_MAGIC;
        bool needsAct = cmd == "L" || cmd == "HR" || cmd == "D" || cmd == "GS";
        if (needsAct && (!CanAct(player) || (magic && player->HasUnitFlag(UNIT_FLAG_SILENCED))))
            return;

        if (magic)
        {
            if      (cmd == "L")  DoBolt(player, st, a1, now);
            else if (cmd == "HS") { DoHeavyStart(st, now); StopChargeVisual(player, st); StartChargeVisual(player, st); }
            else if (cmd == "HR") DoVolley(player, st, a1, now);
            else if (cmd == "D")  DoBlink(player, st, a1, a2, now);
            else if (cmd == "GS") DoGuard(player, st, true, now);
            else if (cmd == "GE") DoGuard(player, st, false, now);
            return;
        }

        if (style == STYLE_RANGED)
        {
            // Enemy in your face + melee weapon equipped: the same buttons swing the melee weapon.
            if (cmd == "L" || cmd == "HR")
            {
                bool melee = ShouldSwapToMelee(player, st, a1);
                SetWeaponSwap(player, st, melee);
                if (melee)
                {
                    if (cmd == "L")
                        DoLight(player, st, a1, now);
                    else
                        DoHeavyRelease(player, st, a1, now);
                    return;
                }
            }

            if      (cmd == "L")  DoShot(player, st, a1, now, false);
            else if (cmd == "HS") { DoHeavyStart(st, now); st->drawStart = now; }
            else if (cmd == "HR") DoPowerShot(player, st, a1, now, false);
            else if (cmd == "D")  DoDodge(player, st, a1, a2, now);     // archers roll like melee
            else if (cmd == "GS") DoGuard(player, st, true, now);
            else if (cmd == "GE") DoGuard(player, st, false, now);
            return;
        }

        if      (cmd == "L")  DoLight(player, st, a1, now);
        else if (cmd == "HS") DoHeavyStart(st, now);
        else if (cmd == "HR") DoHeavyRelease(player, st, a1, now);
        else if (cmd == "D")  DoDodge(player, st, a1, a2, now);
        else if (cmd == "GS") DoGuard(player, st, true, now);
        else if (cmd == "GE") DoGuard(player, st, false, now);
    }

    // Shared logic for melee and direct spell damage taken by a player.
    // Returns true if the damage was fully negated.
    bool DefendPlayer(Player* victim, Unit* attacker, uint32& damage, bool isMelee)
    {
        PlayerState* st = GetState(victim);
        uint32 now = getMSTime();

        if (st->iframesUntil && int32(now - st->iframesUntil) < 0)
        {
            damage = 0;
            PlayAnim(victim, EMOTE_ONESHOT_DODGE);
            SendEvent(victim, "EVADE");
            return true;
        }

        if (!st->guarding || !attacker || attacker == victim)
            return false;

        bool magic = IsMagic(victim, st);
        bool covers = magic ? (isMelee || Cfg.wardBlocksSpells) : (isMelee || Cfg.guardSpellsToo);
        if (!covers)
            return false;
        if (!victim->HasInArc(float(M_PI), attacker))   // guard / ward only covers the front
            return false;

        // Perfect parry (melee guard) / spell deflect (magic ward also deflects spells)
        if ((isMelee || magic) && !st->parryUsed && getMSTimeDiff(st->guardStart, now) <= Cfg.parryWindow)
        {
            st->parryUsed = true;
            damage = 0;
            PlayAnim(victim, BlockEmote(victim, magic));
            PlayVisual(victim, Cfg.parryVisualSpell);
            if (!magic)
                PlayAnim(attacker, EMOTE_ONESHOT_WOUND);     // attacker recoils
            SendEvent(victim, magic ? "DEFLECT" : "PARRY");
            if (Creature* c = attacker->ToCreature())
                ApplyPoise(victim, c, Cfg.parryPoise, now);
            return true;
        }

        // Magic ward: the prevented damage is paid for with mana
        if (magic)
        {
            uint32 prevented = uint32(float(damage) * Cfg.wardReductionPct / 100.0f);
            uint32 cost = uint32(float(prevented) * Cfg.wardManaPerDamage);
            if (victim->GetPower(POWER_MANA) < cost)
            {
                victim->SetPower(POWER_MANA, 0);
                st->guarding = false;
                ClearStance(victim, st);
                st->lastManaSpend = now;
                Stagger(victim);
                SendEvent(victim, "WARDBREAK");
                return false;   // full damage on a ward break
            }
            victim->ModifyPower(POWER_MANA, -int32(cost));
            st->lastManaSpend = now;
            damage -= prevented;
            PlayAnim(victim, BlockEmote(victim, true));
            SendEvent(victim, "WARD");
            return false;
        }

        // Regular guard: reduce damage, pay stamina
        float hpPct = victim->GetMaxHealth() ? 100.0f * float(damage) / float(victim->GetMaxHealth()) : 0.0f;
        float cost = std::max(Cfg.guardMinStaminaHit, hpPct * Cfg.guardStaminaPerDmgPct);
        st->stamina -= cost;
        st->lastSpend = now;

        if (st->stamina <= 0.0f)
        {
            st->stamina = 0.0f;
            st->guarding = false;
            ClearStance(victim, st);
            Stagger(victim);
            SendEvent(victim, "GUARDBREAK");
            SendStamina(victim, st);
            return false; // full damage goes through on a guard break
        }

        damage = uint32(float(damage) * (100.0f - Cfg.guardReductionPct) / 100.0f);
        PlayAnim(victim, BlockEmote(victim, false));
        SendEvent(victim, "BLOCK");
        SendStamina(victim, st);
        return false;
    }
}

using namespace ActionCombat;

// ====================================================================== scripts

class ActionCombat_WorldScript : public WorldScript
{
public:
    ActionCombat_WorldScript() : WorldScript("ActionCombat_WorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class ActionCombat_PlayerScript : public PlayerScript
{
public:
    ActionCombat_PlayerScript() : PlayerScript("ActionCombat_PlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!Cfg.enabled)
            return;
        PlayerState* st = GetState(player);
        st->stamina = Cfg.staminaMax;

        // A ward bubble can be saved with the character if they logged out mid-ward.
        for (uint32 spellId : Cfg.wardVisual)
            if (spellId)
                player->RemoveAurasDueToSpell(spellId);
        if (Cfg.announce)
            ChatHandler(player->GetSession()).SendSysMessage("This realm uses |cff4fc3f7Action Combat|r. Install the ActionCombat addon and set your keybinds (Esc > Key Bindings > ActionCombat).");
    }

    // Addon messages arrive here as a whisper to yourself: "ACM\t<command>"
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (lang != LANG_ADDON || receiver != player)
            return true;

        std::string const prefix = std::string(ADDON_PREFIX) + "\t";
        if (msg.compare(0, prefix.size(), prefix) != 0)
            return true;

        if (!Cfg.enabled)
            return false;

        std::string body = msg.substr(prefix.size());

        // Interact / peek run right here: this is the same world-thread context the game uses for
        // real loot / gossip / object clicks.
        if (Cfg.interactEnabled && (body.compare(0, 4, "INT ") == 0 || body == "INT"))
        {
            DoInteract(player, body.size() > 4 ? body.substr(4) : std::string());
            return false;
        }
        if (Cfg.interactEnabled && (body.compare(0, 5, "PEEK ") == 0 || body == "PEEK"))
        {
            SendPeek(player, body.size() > 5 ? body.substr(5) : std::string());
            return false;
        }

        PlayerState* st = GetState(player);
        std::lock_guard<std::mutex> lock(st->queueLock);
        if (st->queue.size() < 16)                      // flood guard
            st->queue.push_back(body);
        return false;                                   // swallow it, don't echo the whisper
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!Cfg.enabled)
            return;

        PlayerState* st = GetState(player);
        uint32 now = getMSTime();

        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(st->queueLock);
            pending.swap(st->queue);
        }
        for (std::string const& line : pending)
            Execute(player, st, line, now);

        RunPendingOffhand(player, st, now);

        if (!st->addonPresent)
            return;

        UpdateSprint(player, st, diff, now);

        if (st->chargeVisualSpell && (!st->heavyChargeStart || !player->IsAlive() || !IsMagic(player, st)))
            StopChargeVisual(player, st);

        // element leech heals (collected in the damage hook, applied here outside combat math)
        if (st->pendingHeal && player->IsAlive())
        {
            Element e = CurrentElement(player, st);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(e.bolt);
            HealInfo heal(player, player, st->pendingHeal, info, info ? info->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL);
            player->HealBySpell(heal);
            st->pendingHeal = 0;
        }
        if (!st->pendingLeech.empty())
            st->pendingLeech.erase(std::remove_if(st->pendingLeech.begin(), st->pendingLeech.end(),
                [now](PlayerState::PendingLeech const& l) { return int32(now - l.expires) >= 0; }), st->pendingLeech.end());

        if (st->guarding && st->wardVisual && !player->HasAura(st->wardVisual))
            player->AddAura(st->wardVisual, player);
        else if (!st->guarding && st->wardVisual)
            HideWardBubble(player, st);

        // Walking clears emote states, so put the guard pose back when the player stops.
        if (st->guarding && st->stanceEmote && !player->isMoving()
            && player->GetUInt32Value(UNIT_NPC_EMOTESTATE) != st->stanceEmote)
            player->SetUInt32Value(UNIT_NPC_EMOTESTATE, st->stanceEmote);

        // Stamina regen
        if (!st->sprinting && st->stamina < Cfg.staminaMax && (!st->lastSpend || getMSTimeDiff(st->lastSpend, now) >= Cfg.staminaRegenDelay))
        {
            float rate = Cfg.staminaRegen * (st->guarding ? Cfg.guardRegenPct / 100.0f : 1.0f);
            st->stamina = std::min(Cfg.staminaMax, st->stamina + rate * float(diff) / 1000.0f);
        }

        // Arcane focus: extra mana regen for magic users while they aren't spending,
        // so casters can keep fighting with the action buttons (WotLK's in-combat regen is tiny).
        bool magic = IsMagic(player, st);
        if (magic && !st->sprinting && Cfg.magicRegenPct > 0.0f
            && (!st->lastManaSpend || getMSTimeDiff(st->lastManaSpend, now) >= Cfg.magicRegenDelay)
            && player->GetPower(POWER_MANA) < player->GetMaxPower(POWER_MANA))
        {
            float rate = float(player->GetCreateMana()) * Cfg.magicRegenPct / 100.0f;
            if (st->guarding)
                rate *= Cfg.guardRegenPct / 100.0f;
            st->manaRegenAcc += rate * float(diff) / 1000.0f;
        }

        // Throttled sync (every ~200ms)
        st->syncTimer += diff;
        if (st->syncTimer >= 200)
        {
            st->syncTimer = 0;
            SyncMode(player, st);                       // catches shapeshifts, respecs, etc.
            if (magic)
                SendElement(player, st);                // only sends when it changed
            if (st->manaRegenAcc >= 1.0f)
            {
                player->ModifyPower(POWER_MANA, int32(st->manaRegenAcc));
                st->manaRegenAcc -= float(int32(st->manaRegenAcc));
            }
            if (!magic && int32(st->stamina) != st->lastSentStamina)
                SendStamina(player, st);
        }
    }
};

class ActionCombat_UnitScript : public UnitScript
{
public:
    ActionCombat_UnitScript() : UnitScript("ActionCombat_UnitScript", true, {
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN
    }) { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!Cfg.enabled || !target || !attacker)
            return;

        uint32 now = getMSTime();

        // --- Outgoing: player attacking
        if (Player* p = attacker->ToPlayer())
        {
            if (tl_swing && tl_swing->attacker == p->GetGUID())
            {
                float mult = tl_swing->damageMult;
                if (Creature* c = target->ToCreature())
                {
                    CreatureState* cs = GetState(c);
                    if (cs->riposteUntil && int32(now - cs->riposteUntil) < 0)
                    {
                        mult *= Cfg.riposteMult;
                        cs->riposteUntil = 0;
                        SendEvent(p, "RIPOSTE");
                    }
                    else
                        ApplyPoise(p, c, tl_swing->poiseDamage, now);
                }
                damage = uint32(float(damage) * mult);
            }
            else if (GetState(p)->addonPresent)
            {
                // Regular white swing: de-emphasize it so the action buttons matter.
                damage = uint32(float(damage) * Cfg.autoAttackPct / 100.0f);
            }
        }

        // --- Incoming: player being hit
        if (Player* victim = target->ToPlayer())
            DefendPlayer(victim, attacker, damage, true);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!Cfg.enabled || damage <= 0)
            return;

        // Outgoing element bolt that should heal its caster
        if (Player* caster = attacker ? attacker->ToPlayer() : nullptr)
        {
            PlayerState* st = GetState(caster);
            if (spellInfo && target && !st->pendingLeech.empty())
            {
                for (auto it = st->pendingLeech.begin(); it != st->pendingLeech.end(); ++it)
                    if (it->target == target->GetGUID() && it->spellId == spellInfo->Id)
                    {
                        st->pendingHeal += uint32(float(damage) * it->pct / 100.0f);
                        st->pendingLeech.erase(it);
                        break;
                    }
            }
        }

        Player* victim = target ? target->ToPlayer() : nullptr;
        if (!victim)
            return;

        // Physical "on next swing"/instant melee abilities count as melee for guarding/parrying.
        bool melee = spellInfo && spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MELEE;
        uint32 dmg = uint32(damage);
        DefendPlayer(victim, attacker, dmg, melee);
        damage = int32(dmg);
    }
};

void AddSC_mod_action_combat()
{
    new ActionCombat_WorldScript();
    new ActionCombat_PlayerScript();
    new ActionCombat_UnitScript();
}
