// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
//
// Population Engine — merged core header.
// Consolidates:
//   population_engine_globals.hpp   — shared extern state (g_population_engine_pcs)
//   population_engine_internal.hpp  — internal function declarations
//   population_engine_types.hpp     — standalone runtime types/enums/structs
//   population_shell_constants.hpp  — timing constants and PAI flag namespace
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <common/cbasetypes.hpp>
#include <common/timer.hpp>

class map_session_data;

// ---------------------------------------------------------------------------
// Shared extern state (was: population_engine_globals.hpp)
// ---------------------------------------------------------------------------

extern std::vector<map_session_data *> g_population_engine_pcs;

// ---------------------------------------------------------------------------
// Internal function declarations (was: population_engine_internal.hpp)
// ---------------------------------------------------------------------------

/// Remove one shell from the map (map_quit). Clears chat/wander keys.
void population_engine_shell_release(map_session_data *sd);

/// Removes stale shells from g_population_engine_pcs and returns them.
/// Caller must call population_engine_shell_release on each returned pointer.
std::vector<map_session_data*> population_engine_collect_stale_shells();

/// Increment walk_failures counter in g_population_engine_stats.
void population_engine_stats_record_walk_failure();

// ---------------------------------------------------------------------------
// Runtime types (was: population_engine_types.hpp)
// ---------------------------------------------------------------------------

/// Which subsystem currently holds the movement token for this shell.
/// Exactly one owner can emit movement per tick.
enum class MovementOwner : uint8 {
	None   = 0,
	Combat = 1,
	Loot   = 2,
	Roam   = 3,
};

enum class MovementOwnerReason : uint8 {
	None           = 0,
	TargetActive   = 1,
	LootActive     = 2,
	NoValidTarget  = 3,
	TargetSearch   = 4,
	PathRecovery   = 5,
};

/// Player-controlled engagement policy for a recruited party companion.
/// This is deliberately separate from the YAML Role: combat specialization.
enum class PopulationCompanionMode : uint8 {
	Passive   = 0, ///< Never acquire a monster target; support/follow remain active. ("Hold")
	Defensive = 1, ///< Assist the owner and defend party members (safe default). ("Standard")
	Attack    = 2, ///< Roam near the owner and hunt independently for EXP. ("Free")
	Dangerous = 3, ///< Tight formation; nobody engages until the owner does. Tanks pull on order.
};

/// What a companion hired from the Companion Summoner window was hired to do.
/// Recruited (whispered) companions keep None.
enum class PopulationCompanionDuty : uint8 {
	None     = 0,
	Attacker = 1, ///< Full damage build.
	Defender = 2, ///< VIT/DEF build; taunts and holds aggro.
	Support1 = 3, ///< Party utility: songs, links, endows.
	Support2 = 4, ///< Buffer and healer.
};

/// Taunt/Pull order progress for a Defender companion.
enum class PopulationCompanionPull : uint8 {
	None     = 0,
	Approach = 1, ///< Walking into Provoke range of the pull target.
	Return   = 2, ///< Target provoked; walking back beside the owner.
	Hold     = 3, ///< Beside the owner, waiting for the pulled monsters to arrive.
};

/// Heal/buff rank of `ally` for a companion healer `shell`, lower is better, or
/// -1 when the shell has no owner policy and the caller should keep its own
/// lowest-HP choice. Defined with the summoner, used by the combat support AI.
int population_companion_ally_rank(const map_session_data *shell, const map_session_data *ally, int hp_pct);
/// The owner's "heal below" line for this companion, or 0 for no override.
uint8 population_companion_heal_line(const map_session_data *shell);
/// Whether a companion `shell` may give `skill_id` (0 = any heal/buff) to `ally`:
/// party members only, weapon endows on weapon users. Always true for other shells.
bool population_companion_ally_ok(const map_session_data *shell, const map_session_data *ally, uint16 skill_id);
/// Whether a companion may cast `skill_id` now: the one song picked for its
/// owner's job, no Frost Joker / Scream, the endow that beats the current
/// target. Always true for other shells.
bool population_companion_skill_allowed(map_session_data *shell, uint16 skill_id);
/// For a companion set to "Match level", the level to cast `skill_id` at
/// (`yaml_lv` capped by what it learned, 0 = not learned) and true; false for
/// every other shell, which keeps the rotation's own rule.
bool population_companion_skill_level(const map_session_data *shell, uint16 skill_id, uint16 yaml_lv, uint16 &use_lv);
/// For a companion's self-buff whose level it picks itself (Mild Wind: the
/// element that beats the party's target), true with `use_lv` set (0 = don't
/// cast; it arrives capped by what the companion knows) and `stale` when the
/// buff it has on is the wrong level. False for every other skill and shell.
bool population_companion_self_buff_level(map_session_data *shell, uint16 skill_id, uint16 &use_lv, bool &stale);
/// Whether a companion fights by casting only (caster and healer styles): it
/// never basic-attacks, so it never walks into melee. False for other shells.
bool population_companion_holds_back(const map_session_data *shell);
/// How badly a Defender companion `shell` should cover `ally`, lower first:
/// 0 = under the emergency line, then healer, utility and casters, the owner,
/// attackers. -1 = a tank (another Defender, a Crusader, an owner playing
/// Defender) that holds its own monsters, or someone outside the party.
int population_companion_protect_rank(const map_session_data *shell, const map_session_data *ally);
/// Whether a shell's weapon, shield and mount let it use `skill_id`.
bool population_companion_gear_ok(map_session_data *shell, uint16 skill_id);
/// Whether `shell` is a companion hired as Defender.
bool population_companion_is_defender(const map_session_data *shell);
/// Whether `shell` is a companion hired as Attacker.
bool population_companion_is_attacker(const map_session_data *shell);
/// Whether `ally` is a tank in companion `shell`'s party (a Defender, a
/// Crusader, an owner playing Defender), so its monsters stay where they are.
bool population_companion_is_tank(const map_session_data *shell, const map_session_data *ally);
/// Tells the owner why a companion chain picked `skill_id` (0 = swing or
/// wait), while `@companion debug` is on. Repeats of the same line are dropped.
void population_companion_log_pick(map_session_data *shell, uint16 skill_id, const char *why);

// ---------------------------------------------------------------------------
// Local navigation FSM
// ---------------------------------------------------------------------------

enum class LocalNavState : uint8 {
	DirectAdvance = 0,
	WallFollow    = 1,
	Recover       = 2,
};

// ---------------------------------------------------------------------------
// Navigation blocked cells
// ---------------------------------------------------------------------------

constexpr int PE_NAV_BLOCKED_CELLS_MAX = 16;
constexpr int PE_TYPE3_HOTSPOTS_MAX    = 8;

struct CANavBlockedCell {
	int    x          = 0;
	int    y          = 0;
	t_tick until_tick = 0;
};

// ---------------------------------------------------------------------------
// Forward-roam hotspot seeding
// ---------------------------------------------------------------------------

struct Type3Hotspot {
	int    x              = 0;
	int    y              = 0;
	int    score          = 0;
	t_tick last_seen_tick = 0;
};

// ---------------------------------------------------------------------------
// Active buff tracking
// ---------------------------------------------------------------------------

struct s_pe_active_buff {
	uint16 skill_id   = 0;
	uint16 skill_lv   = 0;
	t_tick expires_at = 0;  ///< When this buff expires (gettick()-based)
	uint32 target_id  = 0;  ///< Who the buff is on (0 = self)

	s_pe_active_buff() = default;
	s_pe_active_buff(uint16 id, uint16 lv, t_tick expires, uint32 target = 0)
		: skill_id(id), skill_lv(lv), expires_at(expires), target_id(target) {}
};

// ---------------------------------------------------------------------------
// Mob tracking
// ---------------------------------------------------------------------------

struct s_pe_tracked_mob {
	uint32  mob_id      = 0;
	int16   mob_type_id = 0; ///< mob_data->mob_id (the type, not the instance ID)
	int     x           = 0;
	int     y           = 0;
	t_tick  last_seen   = 0;
	int     hp          = 0;
	int     max_hp      = 0;
	uint32  target_id   = 0;
};

struct s_pe_mob_tracker {
	t_tick last_scan     = 0;
	t_tick scan_interval = 1000; ///< Scan interval in ms (default 1 s)
	std::unordered_map<uint32, s_pe_tracked_mob> tracked_mobs; ///< Key: mob instance ID
	int16  map_id        = 0;
};

// ---------------------------------------------------------------------------
// Position record
// ---------------------------------------------------------------------------

struct s_pe_position {
	int   map = 0;
	short x   = 0;
	short y   = 0;
	short dx  = 0;
	short dy  = 0;
};

// ---------------------------------------------------------------------------
// Mob-map record (tracks which map the last mob scan was performed on)
// ---------------------------------------------------------------------------

struct s_pe_mobs {
	uint16_t map = 0; ///< mapindex of the most recent mob scan.
};

// ---------------------------------------------------------------------------
// Detection cache
// ---------------------------------------------------------------------------

struct s_pe_detection_cache {
	std::vector<unsigned int> cached_monsters{}; ///< IDs from last mob scan.
	std::vector<unsigned int> cached_items{};    ///< IDs from last item scan.
	t_tick last_update  = 0; ///< Tick of last scan (0 = never).
	int    last_x       = 0; ///< Shell x at time of last scan.
	int    last_y       = 0; ///< Shell y at time of last scan.
	int    cache_radius = 0; ///< Radius used for the cached scan.
};

// ---------------------------------------------------------------------------
// Timing constants (was: population_shell_constants.hpp)
// ---------------------------------------------------------------------------

/// Defer SC start while warping / inactive
constexpr int PE_SHELL_DEFER_START_MS = 200;
/// Target exclusion after forced clear (matches legacy AC_TARGET_EXCLUSION_DURATION_MS)
constexpr int PE_SHELL_TARGET_EXCLUSION_MS = 500;
/// Stuck target / item pick (matches legacy autocombat timeouts)
constexpr int PE_SHELL_TARGET_ATTACK_TIMEOUT_MS = 4000;
constexpr int PE_SHELL_ITEM_PICK_TIMEOUT_MS = 5000;
constexpr int PE_SHELL_ATTACK_FAIL_CLEAR_COUNT = 5;
constexpr int PE_SHELL_SKILL_FAIL_CLEAR_TARGET_MS = 2000;
constexpr int PE_SHELL_ATTACK_APPROACH_CELLS = 2;
/// Max defer timer retries (~30s at 200ms) before giving up
constexpr intptr_t PE_SHELL_DEFER_MAX_ATTEMPTS = 150;

/// Population AI behavior flags (bitmask, read from battle_config.population_engine_ai):
namespace PAI {
	constexpr int32 ChaseRefresh    = 0x001; ///< Re-evaluate chase path every step
	constexpr int32 TargetSwitch    = 0x002; ///< Switch to closer enemy mid-combat
	constexpr int32 SkillWhileChase = 0x004; ///< Evaluate skills while approaching
	constexpr int32 ReactiveCheck   = 0x008; ///< Re-check conditions every tick
	constexpr int32 FleeOnLowHP    = 0x010; ///< Squishy roles retreat at low HP
	constexpr int32 SupportPriority = 0x020; ///< Support roles heal before self-buff
	constexpr int32 PackBehavior   = 0x040; ///< Share target info with nearby shells
	constexpr int32 KiteRanged     = 0x080; ///< Ranged jobs maintain max skill range
	constexpr int32 ComboAwareness = 0x100; ///< Chain skills fire in sequence
	constexpr int32 BossAvoidance  = 0x200; ///< Non-tank roles avoid boss monsters
}
