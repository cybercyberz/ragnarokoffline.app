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
