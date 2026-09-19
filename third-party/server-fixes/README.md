# Pinned rAthena corrections

These small patches apply to the pinned rAthena sources through
`scripts/apply-server-mods.sh`, in both normal and diagnostic builds. They retain
rAthena's GPL license; changes are marked `RAGNAROKMAC` where applicable.

`0001-language-mask.patch` validates the language index before shifting. The
English index is zero, so the previous operation shifted by -1 before reaching
its English early return. Large indices could also shift out of range or wrap
the 16-bit mask. Valid languages and the enabled-language policy are unchanged.
This was caught during real diagnostic map-server startup, not during logout.

`0002-base-sp-range.patch` clamps calculated base SP before converting the
floating-point result to unsigned. Default coefficients for a Ninja-mapped job
produce -2 at level 10; converting that to `uint32` is undefined. Normal
nonnegative values are unchanged, and overflow saturates at the return type's
maximum. The actual function has a separate UBSan reduction covering both bounds.

`0003-unlearned-skill-level.patch` returns zero for an unlearned skill before the
shared accessor indexes a level table. Native character login asks for SP at
level zero while constructing the skill list; the previous macro read index -1.
Stored and extrapolated learned levels retain their existing behavior.

`0004-weapon-bonus-bounds.patch` sizes both weapon bonus arrays from the existing
weapon enum (moved before the player structure) and checks item-script indices.
The fixed diagnostic image reached a real Pre-Renewal population spawn, where
equipping a Katar read index 16 from the old 16-element array. Later weapon types
also exceeded that array, including the damage bonus used by combat.
`verify-weapon-bounds.py` reproduces the actual declaration/setter/status-read
fault and checks every weapon type plus invalid script indices after patching.

`0005-persist-loot-preferences.patch` keeps `@autoloot`, `@autoloottype` and
`@showexp` across a logout. All three lived only in the session, so every login
began by retyping them, and players were writing login scripts to do it for
them. The values are stored as ordinary per-character variables, which needs no
schema change and no migration, written by the commands that set them so a
crash loses nothing, and read back in `pc_reg_received` because that is the
first point at which character variables have arrived. A stored rate is clamped
to rAthena's own range on the way in, so a hand-edited variable cannot put the
session into a state the command itself could not produce.

`0007-exceed-final-damage.patch` adds the NY-MMO "Exceed Final Damage" family of
item bonuses, used by the custom item database of a private server whose items
this install imports. They are a percentage applied on top of the finished
damage of one attack - after the skill ratio, cards, DEF, element and the
target's own reductions - rather than a modifier inside the damage formula, so
none of rAthena's existing bonuses express them. `battle_calc_exceed_final_damage`
runs at the end of `battle_calc_attack`: every bonus matching the attack
(all / physical / magical / misc / melee / ranged / normal / critical / the
element the skill attacked with) is summed and applied once, then the target's
matching `...Def` bonuses reduce the result the same way, summed and capped at
100%. A character with none of these bonuses is unaffected, which is every
character on a stock install.

`0008-nymmo-combat-stats.patch` adds the rest of the NY-MMO combat stats those item
descriptions rely on. One pass, `battle_calc_nymmo_combat`, runs straight after 0007's
on the finished damage: evasion and blocking (skill, Hyper, Ultimate, critical, normal
attack, melee weapon), perfect block (the hit lands but deals 1), perfect defense and
absorb as percentages off the finished damage, and lifesteal on what actually landed.
Each defensive chance is reduced by the attacker's matching "Anti" bonus before it is
rolled. Two pieces sit outside that function: "All Stats +n%" and "All Trait Stats +n%"
apply in `status_calc_pc_` once the totals are known, because that is what the
descriptions mean - a percentage of everything the character has - and the Hyper and
Ultimate skill tiers are skill-id keyed item bonuses (`bonus2 bHyperSkill,"SKILL",1`)
that feed both `pc_skillatk_bonus` and 0007's Exceed sums. A character with none of
these bonuses is unaffected, which is every character on a stock install.

That private server's own scripts are not public, so these semantics are read off the
item descriptions rather than copied from it. Families whose descriptions do not define
the arithmetic are deliberately absent - "chance to max damage" above all, since nothing
in any description says what the maximum is measured against. The full bonus list is
documented alongside the item translator that emits them, which is also what validates
the generated item scripts.

`0009-nymmo-pipeline-autocast.patch` follows up on the paragraph above: the private
server's GMs have since been read on their own Discord, and they define what the
descriptions left open. The server has a per-hit damage limit (49,999,999; "Break
Damage Limit" items raise the wearer's), "Exceed" is what goes past it, and "chance to
max damage" is a proc that makes a skill hit land on the limit. Their stated order,
"Main DMG => DMG Limit => Exceed DMG => Penalty DMG => Trigger Reflect", is what
`battle_calc_nymmo_pipeline` now runs in place of 0007's single call: the max-damage
roll, the limit, Exceed as `limit * (1 + (Exceed - target's Exceed DEF)%)` on a hit that
reached it, flat damage past the limit, HP-based ("blue") damage against players, and a
PvP/WoE penalty. Three battle config values tune it (`nymmo_damage_limit`,
`nymmo_pvp_exceed_penalty`, `nymmo_pvp_damage_penalty`); a limit of 0 gives exactly
0007's behaviour. The same patch adds their autocast controls, which they describe as
two different stats: "Block Autocast" (a chance an enemy's autospell does not fire) and
"Disable the target's autocast" (a timed stop on the target's own autospells), plus
"Interrupt" on a player's continuous attack. Item autospells gain three option bits
for the map scope their text states - PvE only, PvP only, half chance on PvP/WoE - and
a `nymmo_vsmap()` script function lets ordinary "(PVE)" / "(PVP)" bonuses be gated too;
item scripts are re-run when a warp crosses between such a map and a normal one. Only
characters wearing these bonuses are affected; with none, the only change on a stock
install is the damage limit, which `nymmo_damage_limit: 0` turns off.

`0010-nymmo-refine-30.patch` brings that server's refine rules: its Level 5 weapons refine
to +30 ("max refine, +20 or +30"), so renewal `MAX_REFINE` becomes 30. Which items may go
past +20 stays data - `refine.yml` only lists levels 21-30 for Level 5 weapons, and a level
with no entry cannot be attempted, so every other item still stops at +20 and a stock
refine.yml behaves exactly as before. Their Enchant Grade gives a Level 5 weapon Exceed
Final Damage +2/6/10/20% for grade D/C/B/A on top of rAthena's own refine-ATK percentage
(GM announcement, 2022-12-08); that is added to 0007's `exceed_final_dmg` in
`status_calc_pc_sub` and switched by `nymmo_grade_exceed` (default on).
Two refine database fields also accept 0 now - a chance's `BreakingRate` and a grade's
`Chance` - so an import file can switch off what the base file set: that server's armor
and Level 5 weapons never break (a fail resets them to +0), and its grade is only
offered at +30. rAthena otherwise rejects 0 there, and an import cannot delete a key.
