# mod-action-combat

Zelda / Elden Ring style action combat for **AzerothCore (WotLK 3.3.5a)**.

| Action | Input | What it does |
|---|---|---|
| Light attack | tap | 3-hit combo, hits everything in a cone in front of you. 3rd hit is a wide sweep. |
| Heavy attack | hold, release | Charge up to 1.5 s for up to 2.8× damage and big poise damage. |
| Dodge roll | tap | Rolls in the direction you're moving (8-way) with i-frames. Standing still = backstep. |
| Guard | hold | Blocks 70% of frontal melee damage, costs stamina per hit. Empty bar = guard break. |
| Perfect parry | tap guard just before a hit | Negates the hit and deals heavy poise damage to the attacker. |

Every action costs **stamina**, which regenerates after a short pause (slower while guarding).
Enemies have **poise**: hit them enough (or parry them) and they are **staggered**, and your next
hit within 3 s is a **critical riposte** (2.5×). Bosses are immune by default.

Hits go through the normal melee pipeline (`AttackerStateUpdate`), so armor, crits, procs,
weapon damage, rage generation and combat text all work as usual — the module just multiplies the damage.

## Install

**Server**
1. Copy this folder to `azerothcore-wotlk/modules/mod-action-combat`.
2. Re-run CMake and rebuild the worldserver.
3. Copy `conf/mod_action_combat.conf.dist` to your configs folder as `mod_action_combat.conf` (the build's install step also places the `.dist` there).

**Client** — every player needs this
1. Copy `addon/ActionCombat` into `World of Warcraft/Interface/AddOns/`.
2. In game: Esc → Key Bindings → **Action Combat**, bind the four actions.
   Suggested: Light = `1` or Shift+LMB via a mouse tool, Heavy = `2` (hold), Dodge = `Mouse4` or `Alt+Space`, Guard = `Mouse5` (hold).

Players without the addon keep normal WoW combat: the server only changes damage for players whose addon has said `HELLO`.

## How it works

```
client keypress ──SendAddonMessage("ACM", "...", "WHISPER", self)──▶ worldserver
   OnPlayerCanUseChat  → swallow whisper, queue command
   OnPlayerUpdate      → run it on the map thread (stamina, cooldowns, hit cone, knockback roll)
   ModifyMeleeDamage   → action multipliers, poise, riposte; i-frames / guard / parry on incoming hits
   ModifySpellDamageTaken → i-frames vs. spells, guard vs. melee abilities
server ──addon whisper "S cur max" / "E PARRY" …──▶ addon draws stamina bar + feedback
```

Commands (client → server): `HELLO`, `L <facing>`, `HS`, `HR <facing>`, `D <F|B|L|R|FL|FR|BL|BR|N> <facing>`, `GS`, `GE`.
The server is authoritative for stamina, cooldowns, charge time and i-frames, so the addon can't be edited to cheat those.

## Known limits of the 3.3.5 client (and ideas for next steps)

- **The roll is a knockback.** You briefly lose control mid-roll (that's actually close to Souls behaviour). There's no real roll animation; you can make one by adding a custom spell in `spell_dbc` with a visual/anim kit and setting it as a cosmetic aura in `DoDodge`.
- **Stagger uses spell 56 (3 s stun).** Create a 1–1.5 s stun spell in `spell_dbc` and set `ActionCombat.Stagger.SpellId` for snappier pacing.
- **Enemies don't telegraph.** For true Elden Ring feel, give creatures wind-up attacks: SmartAI/C++ casts with cast times and a ground decal (e.g. a visual spell) so players have something to roll through. That's the natural next module.
- **No soft lock-on.** A good addition: on light/heavy press with no target, `TargetNearestEnemy()` in the addon and snap facing toward it server-side.
- **Casters / hunters.** Light/heavy use the main-hand weapon. You could map casters' light attack to a fast wand/staff bolt instead.
- **Auto-attack** is reduced to 30% for addon users. Set `ActionCombat.AutoAttack.DamagePct = 0` for pure action combat (note: warriors get rage from damage, so 0 starves them unless you add rage on action hits).
- Damage negated by i-frames still counts as a "hit" for on-hit procs.
