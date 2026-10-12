# mod-action-combat

Zelda / Elden Ring style action combat for **AzerothCore (WotLK 3.3.5a)**.

| Action | Input | What it does |
|---|---|---|
| Light attack | tap | 3-hit combo, hits everything in a cone in front of you. 3rd hit is a wide sweep. |
| Heavy attack | hold, release | Charge up to 1.5 s for up to 2.8× damage and big poise damage. |
| Dodge roll | tap | Rolls in the direction you're moving (8-way) with i-frames. Standing still = backstep. |
| Guard | hold | Blocks 70% of frontal melee damage, costs stamina per hit. Empty bar = guard break. |
| Perfect parry | tap guard just before a hit | Negates the hit and deals heavy poise damage to the attacker. |

| Sprint | hold while moving | Run 45% faster, draining stamina (mana for casters). |

**Dual wield** (two swords, two daggers, sword + dagger...): the light combo goes right hand → **left hand** → double strike with both blades, a bit faster than one weapon. The heavy becomes a **cross slash** (right blade, then the left follows). Hits use each weapon's own damage and the client's off-hand and dagger-stab animations.

Every action costs **stamina**, which regenerates after a short pause (slower while guarding).
Enemies have **poise**: hit them enough (or parry them) and they are **staggered**, and your next
hit within 3 s is a **critical riposte** (2.5×). Bosses are immune by default.

### Magic style (mana classes)

Priests, Shamans, Mages, Warlocks and Druids fight with spells, and **every action costs mana**
(as % of base mana, like Blizzard's own spell costs). The same four keys do:

| Action | Magic version |
|---|---|
| Light attack | **Aimed bolt**: your class spell (Fireball, Shadow Bolt, Smite, Lightning Bolt, Wrath) fired instantly. Keeps your target if it's in front of you, otherwise auto-aims at the enemy closest to your crosshair line. 3-hit combo. |
| Heavy attack | **Charged volley**: hold, release to launch a bolt at every enemy in a 90° cone (up to 5), 1.3×–2.4× damage. |
| Dodge | **Blink** 8 yards in your movement direction with i-frames (5 yd backward when standing still). Stops at walls. |
| Guard | **Mana ward**: blocks 75% of damage from the front, including spells, paid for with 1.5 mana per point blocked. Out of mana = ward shatters and you're staggered. |
| Perfect guard | **Deflect**: negates a melee hit *or* a spell and staggers the attacker. |

**Elements**: casters switch between elements with the *Switch element* key (cycles) or `/acm element frost`. The current one shows as an icon and name above the mana bar, which takes on its color. Your choice is remembered between logins. Bolts and volleys both use it.

| Element | Spell | Playstyle |
|---|---|---|
| Fire | Fireball | Hits harder, costs more, burns |
| Frost | Frostbolt | Less damage, slows, staggers enemies fast |
| Arcane | Arcane Barrage | Weaker but 30% faster and cheaper |
| Shadow | Shadow Bolt | Heals you for 20% of damage dealt |
| Holy | Smite | 1.5× damage to demons and undead |
| Lightning | Lightning Bolt | Chains to 2 more enemies (50%, then 25%) |
| Nature | Wrath | Cheap, heals you for 10% of damage dealt |
| Lunar | Starfire | Slow and heavy hitter |

Default lists: Mage Fire/Frost/Arcane, Warlock Shadow/Fire, Priest Holy/Shadow, Shaman Lightning/Fire/Frost, Druid Nature/Lunar, Paladin Holy. Because each element is a real spell school, swapping matters: fire elementals shrug off Fire, so switch to Frost. Everything is tunable in the config, and `Elements.RequireSpellKnown = 1` makes players learn the spell first.

If switching seems to do nothing, the addon now says why in chat. Common causes: not in magic style (`/acm magic`; Paladins are melee by default), a one-element class, or a worldserver that wasn't rebuilt. `/acm debug` prints every message between the addon and the server.

- Bolt damage = `25 + 8 × level + 0.55 × spell power`, and it uses the real spell for projectile, school, crits, resists and procs.
- You get bonus mana regen (2% base mana per second) after 1.5 s of not spending, because WotLK's in-combat regen would otherwise run casters dry.
- Druids in Cat/Bear form (and Ghost Wolf) automatically switch to melee/stamina style.
- Hybrids can pick: `/acm magic`, `/acm melee`, `/acm auto`. Paladins are melee by default; add `2` to `ActionCombat.Magic.Classes` to make them magic (Exorcism bolts).
- Silence blocks all magic actions.

### Shooting (bows, guns, crossbows)

Shooting has **its own two keys**, usable in any style while a bow, gun or crossbow is equipped,
so Light/Heavy stay melee (or magic). A Hunter can slash with Light, roll back and shoot,
with no mode switching. Uses **stamina**.

| Key | What it does |
|---|---|
| Quick shot (tap) | Keeps your target if it's in front of you, otherwise auto-aims along your camera. 3-shot rhythm, last shot 1.3×. |
| Aimed shot (hold, release) | Draw (the bar under the stamina fills, the character readies the bow/rifle), release for 1.8×–3.2×. A **full draw pierces** every enemy on the line in front of you (up to 4). Rolling cancels the draw. |

Your character draws the ranged weapon to shoot and the melee weapon to swing.

*Optional ranged style*: `/acm ranged` (or `ActionCombat.Ranged.Classes`) also puts shooting on Light/Heavy, for players who prefer it. In that style an enemy in your face makes Light/Heavy swing your melee weapon instead.

- Shots fire the single-fire NPC "Shoot" spell (6660), so you get the real projectile, ranged weapon damage, ammo use, crits and procs.
- You need ammo (unless the realm has infinite ammo on).
- **Enemies in your face** (`ActionCombat.Ranged.CloseRangeMode`, or per player with `/acm close ...`):
  - `swap` (default): with a melee weapon equipped, Light/Heavy automatically swing it while an enemy is within 6 yd. The melee weapon is drawn on your character, then you go back to shooting when they're further away. Same stamina, so a Hunter can stab, roll back and keep shooting.
  - `shoot`: keep shooting. Close enemies take point-blank shots (85% damage; armor, crits and absorbs apply, but no projectile or on-hit procs).
  - `off`: classic WoW, roll back out of minimum range.
  - Without a melee weapon, `swap` falls back to point-blank shots.
- Magic bolts have no minimum range, so casters can already blast enemies at any distance.
- Style switches automatically when you equip or unequip a ranged weapon, or shift into a druid form.

### Camera (hold right mouse button)

While you hold the right mouse button (mouselook):
- An **aiming reticle** appears in the middle of the screen; it turns red when your target is an enemy, with its name and health under it. Bolts and shots aim where you're looking, so this is your crosshair.
- **Camera turning is faster and sharper** (1.4× by default).
- **Camera auto-follow is turned off** so it doesn't fight your mouse.

**Interact key** (bind it, e.g. `F`): loot the corpse, talk to the NPC or use the object in front of your crosshair, without letting go of the right mouse button. While aiming, a prompt under the crosshair shows what it will do ("[F] Loot Kobold Miner", "[F] Talk to Innkeeper", "[F] Trade with ...", "[F] Use Mailbox").
- **Looting**: the Interact key picks up everything from a corpse (or an unlocked chest) straight into your bags; no loot window needed. Anything that can't be taken right now stays on the corpse: full bags, an item waiting on a group roll, or an item that's someone else's.
- When a vendor, quest, gossip, trainer, bank, mail or loot window opens, the cursor comes back so you can click in it.
- It works with everything a right-click works with: corpses, gossip and quest NPCs, vendors, trainers, flight masters, bankers, auctioneers, stable masters, unlocked chests, quest objects, mailboxes, doors and portals. Herbs, ore and locked chests still need a normal click.
- Option: `/acm cam freecursor on|off`.

Your own camera settings come back the moment you let go (and are restored at login if the game closed mid-hold).
Options: `/acm cam on|off`, `/acm cam sens 1.6`, `/acm cam reticle on|off`.

### Animations

3.3.5 can't play *new* animations without a client patch (custom `.m2` models in an MPQ),
so the module uses the best ones the character models already have:

- **Magic blink**: casters leave a flash where they vanish and another where they land, each with its own sound. Mage: teleport flash + arcane explosion. Warlock: dark "evil teleport". Priest/Paladin: teleport + holy burst. Shaman: teleport + lightning. Druid: teleport + nature bloom. Uses visual-only NPC spells held in place by an invisible marker; the server refuses any configured spell that has a real effect or a script.
- **Magic charge (Heavy)**: while you hold Heavy, your character plays the current element's real spell wind-up: fire, frost, shadow, holy, lightning or nature glowing hands, the casting animation and sound, plus a cast bar that fills as you charge. Other players see it too. Release fires the volley; dodging or switching element interrupts it.
- **Magic ward**: casters get a shield bubble around them while warding: Divine Shield blue for Priests/Paladins, Arcane Force Shield for Mages/Shamans/Druids, the dark version for Warlocks. These are cosmetic NPC auras, and the server refuses any configured spell that isn't purely visual.
- **Guard**: you draw your weapon and snap into a block, then hold a ready pose that matches your weapon (1H, 2H, polearm/staff, unarmed, or spellcaster for magic). Walking clears the pose; it comes back when you stop.
- **Blocked hit**: shield-block animation with a shield, otherwise a 1H / 2H / polearm / unarmed parry.
- **Perfect parry**: parry animation plus the attacker recoils.
- **I-frame dodge**: a dodge flinch when a hit is evaded.
- **Dodge**: WoW 3.3.5 has no roll animation, so a directional dodge is a fast **dash** along the ground (follows terrain, stops at walls), and dodging while standing still is a short backward hop. If you make a roll animation with a client patch and save it in an unused emote slot (Custom Spell 01–10), set `ActionCombat.Dodge.CustomAnimEmote` (e.g. 402) and the module plays it on every dodge. Optional cosmetic spells for roll and parry can be set in the config.

Hits go through the normal melee pipeline (`AttackerStateUpdate`), so armor, crits, procs,
weapon damage, rage generation and combat text all work as usual — the module just multiplies the damage.

## Install

**Server**
1. Clone this repository into your modules folder **as `mod-action-combat`** (the folder name matters: AzerothCore builds the module's loader name from it, and the dots in the repository name would break the build):
   ```
   cd azerothcore-wotlk/modules
   git clone https://github.com/Jazzneo/Action-combat-wow-3.3.5a.git mod-action-combat
   ```
2. Re-run CMake and rebuild the worldserver.
3. Copy `conf/mod_action_combat.conf.dist` to your configs folder as `mod_action_combat.conf` (the build's install step also places the `.dist` there).

**Client** — every player needs this
1. Copy `addon/ActionCombat` into `World of Warcraft/Interface/AddOns/`.
2. In game: Esc → Key Bindings → **Action Combat**, bind the actions (Light, Heavy, Dodge, Guard, Sprint, Quick shot, Aimed shot, Interact, Switch element).
   Suggested: Light = `1`, Heavy = `2` (hold), Dodge = `Mouse4` or `Alt+Space`, Guard = `Mouse5` (hold), Sprint = `Shift` (hold), Quick shot = `3`, Aimed shot = `4` (hold), Interact = `F`, Switch element = `Tab` (or mouse wheel).

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

Commands (client → server): `HELLO`, `L <facing>`, `HS`, `HR <facing>`, `D <F|B|L|R|FL|FR|BL|BR|N> <facing>`, `GS`, `GE`, `STYLE <AUTO|MELEE|MAGIC|RANGED>`, `CLOSE <DEFAULT|OFF|SHOOT|SWAP>`, `SS`/`SE` (sprint start/end), `R <facing>` (quick shot), `RS`/`RR <facing>` (aimed shot draw/release), `INT <facing>` (interact), `EL <name|NEXT|PREV>` (element) → `ELEM <name> <spellId> <pos> <count> <list>`, `PEEK <facing>` (prompt) → `P <LOOT|TALK|TRADE|USE|NONE> <name>`. Server → client: `S`, `E <event>`, `MODE <MELEE|MAGIC|RANGED>`.
The server is authoritative for stamina, cooldowns, charge time and i-frames, so the addon can't be edited to cheat those.

## Known limits of the 3.3.5 client (and ideas for next steps)

- **No roll animation in 3.3.5.** Dodge is a ground dash (or a hop for backstep). A real roll needs a client patch with a custom animation; see `ActionCombat.Dodge.CustomAnimEmote`.
- **Stagger uses spell 56 (3 s stun).** Create a 1–1.5 s stun spell in `spell_dbc` and set `ActionCombat.Stagger.SpellId` for snappier pacing.
- **Enemies don't telegraph.** For true Elden Ring feel, give creatures wind-up attacks: SmartAI/C++ casts with cast times and a ground decal (e.g. a visual spell) so players have something to roll through. That's the natural next module.
- **No hard lock-on.** Bolts and shots soft-lock your target or auto-aim along the camera; there is no camera lock-on in the 3.3.5 client.
- **Auto-attack** is reduced to 30% for addon users. Set `ActionCombat.AutoAttack.DamagePct = 0` for pure action combat (note: warriors get rage from damage, so 0 starves them unless you add rage on action hits).
- Damage negated by i-frames still counts as a "hit" for on-hit procs.
