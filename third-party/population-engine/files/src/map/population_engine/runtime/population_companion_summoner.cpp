// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder
//
// Companion Summoner: hire a party companion by duty, job and gear quality
// instead of whispering to an ambient shell, and command the party with the
// game window's buttons instead of party-chat words.
//
// Included at the END of population_engine.cpp (not from the factory), because
// it builds on that file's static helpers: population_engine_spawn_shell,
// pop_is_companion, pop_companion_owner, the index pool and the shell list.
//
// Channel: the client sends `@companion <verb> ...`; the answer is one JSON
// object, sent as `[CMP]<seq>/<i>/<n>:<chunk>` lines that the patched client
// reassembles and keeps out of the chat box. A player without the patched
// client still gets a plain sentence for every refusal.

#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>

// ---------------------------------------------------------------------------
// Build database: db/population_companion_builds.yml
// ---------------------------------------------------------------------------

struct PopCompanionBuildItem {
	std::string slot;               ///< Head_Top, Armor, Weapon, Acc_L ... (informative, and picks the accessory side)
	t_itemid nameid = 0;
	uint8 refine = 0;
	std::vector<t_itemid> cards;
};

struct PopCompanionBuild {
	std::string family;             ///< Summoner job family key (knight, priest ...)
	PopulationCompanionDuty duty = PopulationCompanionDuty::None;
	uint8 quality = 0;              ///< 0 Standard, 1 Good, 2 Excellent
	uint16 min_level = 1;           ///< Applies from this base level up, until a higher band takes over
	std::vector<std::pair<int32, int32>> stats; ///< (SP_STR.., target) in priority order
	int32 rest_stat = SP_VIT;       ///< Where points left over after the plan go
	std::vector<PopCompanionBuildItem> equip;
};

class PopCompanionBuildDatabase : public YamlDatabase {
public:
	std::vector<PopCompanionBuild> builds;

	PopCompanionBuildDatabase() : YamlDatabase("POPULATION_COMPANION_BUILD_DB", 1) {}
	void clear() override { builds.clear(); }
	const std::string getDefaultLocation() override {
		return std::string(db_path) + "/population_companion_builds.yml";
	}
	uint64 parseBodyNode(const ryml::NodeRef& node) override;

	const PopCompanionBuild *find(const std::string &family, PopulationCompanionDuty duty,
		uint8 quality, uint16 level) const
	{
		const PopCompanionBuild *best = nullptr;
		for (const PopCompanionBuild &b : builds) {
			if (b.family != family || b.duty != duty || b.quality != quality || b.min_level > level)
				continue;
			if (!best || b.min_level > best->min_level)
				best = &b;
		}
		return best;
	}
};

static PopCompanionBuildDatabase g_pop_companion_builds;

static PopulationCompanionDuty pop_companion_duty_from_name(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (s == "attacker" || s == "atk" || s == "dps") return PopulationCompanionDuty::Attacker;
	if (s == "defender" || s == "def" || s == "tank") return PopulationCompanionDuty::Defender;
	if (s == "support1" || s == "s1" || s == "utility") return PopulationCompanionDuty::Support1;
	if (s == "support2" || s == "s2" || s == "healer") return PopulationCompanionDuty::Support2;
	return PopulationCompanionDuty::None;
}

static const char *pop_companion_duty_key(PopulationCompanionDuty d)
{
	switch (d) {
	case PopulationCompanionDuty::Attacker: return "attacker";
	case PopulationCompanionDuty::Defender: return "defender";
	case PopulationCompanionDuty::Support1: return "support1";
	case PopulationCompanionDuty::Support2: return "support2";
	default: return "none";
	}
}

static int pop_companion_quality_from_name(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (s == "excellent" || s == "2") return 2;
	if (s == "good" || s == "1") return 1;
	if (s == "standard" || s == "basic" || s == "bad" || s == "0") return 0;
	return -1;
}

static const char *pop_companion_quality_key(uint8 q)
{
	return q >= 2 ? "excellent" : q == 1 ? "good" : "standard";
}

static int32 pop_companion_stat_from_name(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (s == "str") return SP_STR;
	if (s == "agi") return SP_AGI;
	if (s == "vit") return SP_VIT;
	if (s == "int") return SP_INT;
	if (s == "dex") return SP_DEX;
	if (s == "luk") return SP_LUK;
	return -1;
}

static t_itemid pop_companion_item_from_node(const ryml::NodeRef &n)
{
	if (!n.has_val())
		return 0;
	std::string v(n.val().str, n.val().len);
	if (v.empty())
		return 0;
	if (std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
		const t_itemid id = static_cast<t_itemid>(std::strtoul(v.c_str(), nullptr, 10));
		return itemdb_exists(id) ? id : 0;
	}
	std::shared_ptr<item_data> data = item_db.search_aegisname(v.c_str());
	return data ? data->nameid : 0;
}

uint64 PopCompanionBuildDatabase::parseBodyNode(const ryml::NodeRef& node)
{
	PopCompanionBuild b;
	std::string text;
	if (!this->asString(node, "Job", b.family) || b.family.empty()) {
		this->invalidWarning(node, "Companion build without Job, skipping.\n");
		return 0;
	}
	std::transform(b.family.begin(), b.family.end(), b.family.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (!this->asString(node, "Duty", text) ||
		(b.duty = pop_companion_duty_from_name(text)) == PopulationCompanionDuty::None) {
		this->invalidWarning(node, "Companion build %s has no valid Duty, skipping.\n", b.family.c_str());
		return 0;
	}
	int q = 0;
	if (!this->asString(node, "Quality", text) || (q = pop_companion_quality_from_name(text)) < 0) {
		this->invalidWarning(node, "Companion build %s has no valid Quality, skipping.\n", b.family.c_str());
		return 0;
	}
	b.quality = static_cast<uint8>(q);
	if (this->nodeExists(node, "MinLevel"))
		this->asUInt16(node, "MinLevel", b.min_level);

	if (this->nodeExists(node, "Stats")) {
		// A map, read in the order it is written: that order is the priority.
		for (const ryml::NodeRef &s : node["Stats"].children()) {
			if (!s.has_key() || !s.has_val())
				continue;
			const int32 stat = pop_companion_stat_from_name(std::string(s.key().str, s.key().len));
			if (stat < 0) {
				this->invalidWarning(s, "Unknown stat in companion build %s.\n", b.family.c_str());
				continue;
			}
			b.stats.emplace_back(stat, std::atoi(std::string(s.val().str, s.val().len).c_str()));
		}
	}
	if (this->nodeExists(node, "Rest") && this->asString(node, "Rest", text)) {
		const int32 stat = pop_companion_stat_from_name(text);
		if (stat >= 0)
			b.rest_stat = stat;
	}

	if (this->nodeExists(node, "Equip")) {
		for (const ryml::NodeRef &e : node["Equip"].children()) {
			PopCompanionBuildItem it;
			if (e.has_child("Slot"))
				e["Slot"] >> it.slot;
			if (e.has_child("Item"))
				it.nameid = pop_companion_item_from_node(e["Item"]);
			if (it.nameid == 0) {
				this->invalidWarning(e, "Unknown item in companion build %s, skipping the slot.\n", b.family.c_str());
				continue;
			}
			if (e.has_child("Refine")) {
				int r = 0;
				e["Refine"] >> r;
				it.refine = static_cast<uint8>(cap_value(r, 0, MAX_REFINE));
			}
			if (e.has_child("Cards")) {
				for (const ryml::NodeRef &c : e["Cards"].children()) {
					const t_itemid card = pop_companion_item_from_node(c);
					if (card != 0 && it.cards.size() < MAX_SLOTS)
						it.cards.push_back(card);
				}
			}
			b.equip.push_back(std::move(it));
		}
	}
	this->builds.push_back(std::move(b));
	return 1;
}

// ---------------------------------------------------------------------------
// Job families the window offers
// ---------------------------------------------------------------------------

struct PopCompanionFamily {
	const char *key;
	uint16 job;       ///< 2nd job
	uint16 trans_job; ///< Transcendent job (same as job when there is none)
	PopulationCompanionDuty duties[2]; ///< Duties this family can be hired for
	/// Fallback stat plan when the build DB has no entry: (stat, target), rest to VIT.
	std::array<std::pair<int32, int32>, 5> plan;
};

static const PopCompanionFamily kPopCompanionFamilies[] = {
	{ "knight",        JOB_KNIGHT,         JOB_LORD_KNIGHT,     { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::Defender },
		{{ {SP_STR, 90}, {SP_AGI, 70}, {SP_DEX, 40}, {SP_VIT, 40}, {SP_LUK, 10} }} },
	{ "assassin",      JOB_ASSASSIN,       JOB_ASSASSIN_CROSS,  { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_AGI, 90}, {SP_STR, 80}, {SP_LUK, 50}, {SP_DEX, 30}, {SP_VIT, 20} }} },
	{ "hunter",        JOB_HUNTER,         JOB_SNIPER,          { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_DEX, 99}, {SP_AGI, 70}, {SP_LUK, 30}, {SP_INT, 20}, {SP_VIT, 20} }} },
	{ "wizard",        JOB_WIZARD,         JOB_HIGH_WIZARD,     { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_INT, 99}, {SP_DEX, 80}, {SP_VIT, 40}, {SP_AGI, 1}, {SP_LUK, 1} }} },
	{ "monk",          JOB_MONK,           JOB_CHAMPION,        { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_STR, 90}, {SP_INT, 50}, {SP_DEX, 50}, {SP_VIT, 50}, {SP_AGI, 30} }} },
	{ "blacksmith",    JOB_BLACKSMITH,     JOB_WHITESMITH,      { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_STR, 90}, {SP_AGI, 60}, {SP_DEX, 50}, {SP_LUK, 30}, {SP_VIT, 20} }} },
	{ "rogue",         JOB_ROGUE,          JOB_STALKER,         { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_STR, 80}, {SP_AGI, 70}, {SP_DEX, 60}, {SP_VIT, 30}, {SP_LUK, 10} }} },
	{ "gunslinger",    JOB_GUNSLINGER,     JOB_GUNSLINGER,      { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_DEX, 99}, {SP_AGI, 70}, {SP_LUK, 40}, {SP_VIT, 20}, {SP_STR, 10} }} },
	{ "ninja",         JOB_NINJA,          JOB_NINJA,           { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_INT, 70}, {SP_DEX, 70}, {SP_STR, 50}, {SP_AGI, 40}, {SP_VIT, 20} }} },
	{ "stargladiator", JOB_STAR_GLADIATOR, JOB_STAR_GLADIATOR,  { PopulationCompanionDuty::Attacker, PopulationCompanionDuty::None },
		{{ {SP_STR, 90}, {SP_AGI, 70}, {SP_DEX, 40}, {SP_LUK, 30}, {SP_VIT, 20} }} },
	{ "crusader",      JOB_CRUSADER,       JOB_PALADIN,         { PopulationCompanionDuty::Defender, PopulationCompanionDuty::None },
		{{ {SP_VIT, 99}, {SP_STR, 60}, {SP_INT, 40}, {SP_DEX, 40}, {SP_AGI, 20} }} },
	{ "bard",          JOB_BARD,           JOB_CLOWN,           { PopulationCompanionDuty::Support1, PopulationCompanionDuty::None },
		{{ {SP_DEX, 90}, {SP_AGI, 50}, {SP_INT, 50}, {SP_VIT, 50}, {SP_LUK, 1} }} },
	{ "dancer",        JOB_DANCER,         JOB_GYPSY,           { PopulationCompanionDuty::Support1, PopulationCompanionDuty::None },
		{{ {SP_DEX, 90}, {SP_AGI, 50}, {SP_INT, 50}, {SP_VIT, 50}, {SP_LUK, 1} }} },
	{ "soullinker",    JOB_SOUL_LINKER,    JOB_SOUL_LINKER,     { PopulationCompanionDuty::Support1, PopulationCompanionDuty::None },
		{{ {SP_INT, 90}, {SP_DEX, 60}, {SP_VIT, 50}, {SP_AGI, 1}, {SP_LUK, 1} }} },
	{ "sage",          JOB_SAGE,           JOB_PROFESSOR,       { PopulationCompanionDuty::Support1, PopulationCompanionDuty::None },
		{{ {SP_INT, 90}, {SP_DEX, 70}, {SP_VIT, 50}, {SP_AGI, 1}, {SP_LUK, 1} }} },
	{ "priest",        JOB_PRIEST,         JOB_HIGH_PRIEST,     { PopulationCompanionDuty::Support2, PopulationCompanionDuty::None },
		{{ {SP_INT, 90}, {SP_DEX, 60}, {SP_VIT, 60}, {SP_AGI, 1}, {SP_LUK, 1} }} },
};
static constexpr size_t kPopCompanionFamilyCount = sizeof(kPopCompanionFamilies) / sizeof(kPopCompanionFamilies[0]);

static const PopCompanionFamily *pop_companion_family(const std::string &key, size_t *index = nullptr)
{
	for (size_t i = 0; i < kPopCompanionFamilyCount; ++i) {
		if (key == kPopCompanionFamilies[i].key) {
			if (index)
				*index = i;
			return &kPopCompanionFamilies[i];
		}
	}
	return nullptr;
}

static bool pop_companion_family_has_duty(const PopCompanionFamily &f, PopulationCompanionDuty d)
{
	return d != PopulationCompanionDuty::None && (f.duties[0] == d || f.duties[1] == d);
}

/// The duty a real player's job would have, for the heal order and formation.
static PopulationCompanionDuty pop_companion_duty_of_job(uint16 class_)
{
	const uint64 mapid = pc_jobid2mapid(class_);
	if (mapid == static_cast<uint64>(-1))
		return PopulationCompanionDuty::Attacker;
	switch (mapid & MAPID_SECONDMASK) {
	case MAPID_CRUSADER:     return PopulationCompanionDuty::Defender;
	case MAPID_PRIEST:       return PopulationCompanionDuty::Support2;
	case MAPID_BARDDANCER:
	case MAPID_SOUL_LINKER:
	case MAPID_SAGE:         return PopulationCompanionDuty::Support1;
	default:                 return PopulationCompanionDuty::Attacker;
	}
}

// ---------------------------------------------------------------------------
// Owner tactics (runtime, restored from the COMPANION_TACTICS$ char variable)
// ---------------------------------------------------------------------------

/// Heal-order slots. You = the owner; the rest are duties.
enum : uint8 { POP_SLOT_YOU = 0 };

struct PopCompanionTactics {
	PopulationCompanionMode mode = PopulationCompanionMode::Defensive;
	bool heal_by_duty = true;
	/// Heal/buff order: POP_SLOT_YOU or a PopulationCompanionDuty value.
	std::array<uint8, 5> order = {{ POP_SLOT_YOU,
		static_cast<uint8>(PopulationCompanionDuty::Defender),
		static_cast<uint8>(PopulationCompanionDuty::Support2),
		static_cast<uint8>(PopulationCompanionDuty::Attacker),
		static_cast<uint8>(PopulationCompanionDuty::Support1) }};
	uint8 heal_line = 75;
	uint8 emergency_line = 35;
	PopulationCompanionDuty my_duty = PopulationCompanionDuty::None; ///< None = from the job
	bool skills_by_level = false; ///< Summoned companions know only what a real character of their level could
	bool loaded = false;
};

static std::unordered_map<uint32, PopCompanionTactics> g_pop_companion_tactics;

static int64 pop_companion_reg(const char *name)
{
	return add_str(name);
}

static void pop_companion_tactics_save(map_session_data *owner, const PopCompanionTactics &t)
{
	char buf[64];
	safesnprintf(buf, sizeof(buf), "%d,%d,%d%d%d%d%d,%d,%d,%d,%d",
		static_cast<int>(t.mode), t.heal_by_duty ? 1 : 0,
		t.order[0], t.order[1], t.order[2], t.order[3], t.order[4],
		t.heal_line, t.emergency_line, static_cast<int>(t.my_duty), t.skills_by_level ? 1 : 0);
	pc_setregistry_str(owner, pop_companion_reg("COMPANION_TACTICS$"), buf);
}

static PopCompanionTactics &pop_companion_tactics(map_session_data *owner)
{
	PopCompanionTactics &t = g_pop_companion_tactics[owner->status.account_id];
	if (t.loaded)
		return t;
	t.loaded = true;
	const char *saved = pc_readregistry_str(owner, pop_companion_reg("COMPANION_TACTICS$"));
	int mode = 0, hbd = 0, heal = 0, emerg = 0, duty = 0, by_level = 0;
	char order[8] = {};
	// The skills field came later: a saved string without it keeps Full kit.
	if (saved && sscanf(saved, "%d,%d,%5[0-4],%d,%d,%d,%d", &mode, &hbd, order, &heal, &emerg, &duty, &by_level) >= 6
		&& strlen(order) == 5) {
		t.mode = static_cast<PopulationCompanionMode>(cap_value(mode, 0, 3));
		t.heal_by_duty = hbd != 0;
		for (int i = 0; i < 5; ++i)
			t.order[i] = static_cast<uint8>(order[i] - '0');
		t.heal_line = static_cast<uint8>(cap_value(heal, 10, 99));
		t.emergency_line = static_cast<uint8>(cap_value(emerg, 5, t.heal_line));
		t.my_duty = static_cast<PopulationCompanionDuty>(cap_value(duty, 0, 4));
		t.skills_by_level = by_level != 0;
	}
	return t;
}

static PopulationCompanionDuty pop_companion_owner_duty(map_session_data *owner)
{
	const PopCompanionTactics &t = pop_companion_tactics(owner);
	return t.my_duty != PopulationCompanionDuty::None ? t.my_duty : pop_companion_duty_of_job(owner->status.class_);
}

/// The duty an ally fills, whatever it is: a summoned shell, a recruited one, or a player.
static PopulationCompanionDuty pop_companion_ally_duty(const map_session_data *ally)
{
	if (population_engine_is_population_pc(ally->id)) {
		if (ally->pop.companion_duty != PopulationCompanionDuty::None)
			return ally->pop.companion_duty;
		switch (static_cast<PopulationRoleType>(ally->pop.role)) {
		case PopulationRoleType::Tank:    return PopulationCompanionDuty::Defender;
		case PopulationRoleType::Support: return pop_companion_duty_of_job(ally->status.class_) == PopulationCompanionDuty::Support1
			? PopulationCompanionDuty::Support1 : PopulationCompanionDuty::Support2;
		default: break;
		}
	}
	return pop_companion_duty_of_job(ally->status.class_);
}

int population_companion_ally_rank(const map_session_data *shell, const map_session_data *ally, int hp_pct)
{
	if (!shell || !ally || !population_engine_is_population_pc(shell->id) || !pop_is_companion(shell))
		return -1;
	map_session_data *owner = pop_companion_owner(const_cast<map_session_data *>(shell));
	if (!owner)
		return -1;
	const PopCompanionTactics &t = pop_companion_tactics(owner);
	// Nobody is left to die for the sake of the order.
	if (hp_pct < t.emergency_line)
		return 0;
	if (!t.heal_by_duty)
		return 1;
	uint8 slot = POP_SLOT_YOU;
	if (ally != owner) {
		slot = static_cast<uint8>(pop_companion_ally_duty(ally));
	} else if (pop_companion_owner_duty(owner) == PopulationCompanionDuty::Defender &&
		std::find(t.order.begin(), t.order.end(), POP_SLOT_YOU) == t.order.end()) {
		slot = static_cast<uint8>(PopulationCompanionDuty::Defender);
	}
	for (size_t i = 0; i < t.order.size(); ++i) {
		if (t.order[i] == slot)
			return 1 + static_cast<int>(i);
	}
	return 1 + static_cast<int>(t.order.size());
}

uint8 population_companion_heal_line(const map_session_data *shell)
{
	if (!shell || !population_engine_is_population_pc(shell->id) || !pop_is_companion(shell))
		return 0;
	map_session_data *owner = pop_companion_owner(const_cast<map_session_data *>(shell));
	return owner ? pop_companion_tactics(owner).heal_line : 0;
}

// ---------------------------------------------------------------------------
// Companion skills: who they help, which song and endow, "Match level"
// ---------------------------------------------------------------------------

/// How a party member fights, for picking the song or endow that helps it.
enum class PopCompanionStyle : uint8 { Melee, Crit, Ranged, Caster, Healer, Tank };

/// `duty` is the owner's "You are" choice (None = from the job).
static PopCompanionStyle pop_companion_style_of(const map_session_data *who, PopulationCompanionDuty duty)
{
	if (duty == PopulationCompanionDuty::Defender) return PopCompanionStyle::Tank;
	if (duty == PopulationCompanionDuty::Support2) return PopCompanionStyle::Healer;
	// The 2nd-job line covers transcendent, 3rd and 4th jobs; the 1st-job line the rest.
	switch (who->class_ & MAPID_SECONDMASK) {
	case MAPID_CRUSADER:       return PopCompanionStyle::Tank;
	case MAPID_PRIEST:         return PopCompanionStyle::Healer;
	case MAPID_WIZARD:
	case MAPID_SAGE:
	case MAPID_SOUL_LINKER:
	case MAPID_SPIRIT_HANDLER: return PopCompanionStyle::Caster;
	case MAPID_ASSASSIN:       return PopCompanionStyle::Crit;
	case MAPID_MONK:           return PopCompanionStyle::Melee;
	default: break;
	}
	switch (who->class_ & MAPID_FIRSTMASK) {
	case MAPID_MAGE:
	case MAPID_ACOLYTE:
	case MAPID_NINJA:
	case MAPID_SUMMONER:   return PopCompanionStyle::Caster;
	case MAPID_ARCHER:
	case MAPID_GUNSLINGER: return PopCompanionStyle::Ranged;
	default:               return PopCompanionStyle::Melee;
	}
}

static PopCompanionStyle pop_companion_owner_style(map_session_data *owner)
{
	return pop_companion_style_of(owner, pop_companion_tactics(owner).my_duty);
}

/// The one song a Bard or Dancer companion keeps up: every song ends the one
/// before it, so playing them in turn left none running.
static uint16 pop_companion_song_for(const map_session_data *shell, PopCompanionStyle owner)
{
	const bool caster = owner == PopCompanionStyle::Caster || owner == PopCompanionStyle::Healer;
	if (shell->status.sex == SEX_FEMALE) { // Dancer, Gypsy
		if (caster || owner == PopCompanionStyle::Tank) return DC_SERVICEFORYOU; // SP cost, Max SP
		if (owner == PopCompanionStyle::Crit) return DC_FORTUNEKISS;            // Critical
		return DC_HUMMING;                                                      // HIT
	}
	if (caster) return BA_POEMBRAGI;                             // cast time and delay
	if (owner == PopCompanionStyle::Tank) return BA_APPLEIDUN;   // Max HP, HP recovery
	return BA_ASSASSINCROSS;                                     // ASPD
}

/// The endow that beats the monster being fought (the owner's target first), 0 = none.
static uint16 pop_companion_endow_for(map_session_data *shell)
{
	map_session_data *owner = pop_companion_owner(shell);
	const int32 tid = owner ? owner->ud.target : 0;
	block_list *t = tid ? map_id2bl(tid) : nullptr;
	if (!t || t->type != BL_MOB || t->m != shell->m)
		t = shell->pop.target_id ? map_id2bl(shell->pop.target_id) : nullptr;
	if (!t || t->type != BL_MOB || t->m != shell->m)
		return 0;
	const status_data *st = status_get_base_status(t);
	if (!st)
		return 0;
	switch (st->def_ele) {
	case ELE_EARTH:
	case ELE_UNDEAD: return SA_FLAMELAUNCHER;
	case ELE_FIRE:   return SA_FROSTWEAPON;
	case ELE_WATER:  return SA_LIGHTNINGLOADER;
	case ELE_WIND:   return SA_SEISMICWEAPON;
	default:         return 0;
	}
}

bool population_companion_ally_ok(const map_session_data *shell, const map_session_data *ally, uint16 skill_id)
{
	if (!shell || !ally || !population_engine_is_population_pc(shell->id) || !pop_is_companion(shell))
		return true;
	// A companion looks after its own party, not every fake player fighting nearby.
	if (ally->status.party_id != shell->status.party_id)
		return false;
	switch (skill_id) {
	case SA_FLAMELAUNCHER:
	case SA_FROSTWEAPON:
	case SA_LIGHTNINGLOADER:
	case SA_SEISMICWEAPON: {
		map_session_data *owner = pop_companion_owner(const_cast<map_session_data *>(shell));
		const PopulationCompanionDuty duty = owner && owner == ally
			? pop_companion_tactics(owner).my_duty : PopulationCompanionDuty::None;
		const PopCompanionStyle st = pop_companion_style_of(ally, duty);
		return st != PopCompanionStyle::Caster && st != PopCompanionStyle::Healer;
	}
	default:
		return true;
	}
}

bool population_companion_skill_allowed(map_session_data *shell, uint16 skill_id)
{
	if (!shell || !population_engine_is_population_pc(shell->id) || !pop_is_companion(shell))
		return true;
	switch (skill_id) {
	case BA_FROSTJOKER:  // freezes and stuns the party as well
	case DC_SCREAM:
	case BA_DISSONANCE:  // performances: they would end the party song
	case DC_UGLYDANCE:
		return false;
	case BA_WHISTLE:
	case BA_ASSASSINCROSS:
	case BA_POEMBRAGI:
	case BA_APPLEIDUN:
	case DC_HUMMING:
	case DC_DONTFORGETME:
	case DC_FORTUNEKISS:
	case DC_SERVICEFORYOU: {
		map_session_data *owner = pop_companion_owner(shell);
		return owner && skill_id == pop_companion_song_for(shell, pop_companion_owner_style(owner));
	}
	case SA_FLAMELAUNCHER:
	case SA_FROSTWEAPON:
	case SA_LIGHTNINGLOADER:
	case SA_SEISMICWEAPON:
		return skill_id == pop_companion_endow_for(shell);
	default:
		return true;
	}
}

bool population_companion_skill_level(const map_session_data *shell, uint16 skill_id, uint16 yaml_lv, uint16 &use_lv)
{
	if (!shell || !shell->pop.companion_skills_by_level || !population_engine_is_population_pc(shell->id))
		return false;
	const uint16 learned = pc_checkskill(const_cast<map_session_data *>(shell), skill_id);
	use_lv = learned > 0 ? std::min(yaml_lv, learned) : 0;
	return true;
}

/// Forget every skill but the Novice basics.
static void pop_companion_forget_skills(map_session_data *sd)
{
	for (uint16 i = 1; i < MAX_SKILL; i++) {
		sd->status.skill[i].id = 0;
		sd->status.skill[i].lv = 0;
		sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
	}
	pc_skill(sd, NV_BASIC, 9, ADDSKILL_PERMANENT_GRANTED);
	pc_skill(sd, NV_FIRSTAID, 1, ADDSKILL_PERMANENT_GRANTED);
}

/// Full kit: the job's whole tree at maximum, as every fake player spawns.
/// Match level: the skill points a real character of this base level has,
/// spent the way its rotation lists them (main skills first), prerequisites
/// bought on the way, as far as the points go.
static void pop_companion_apply_skills(map_session_data *sd, bool by_level)
{
	sd->pop.companion_skills_by_level = by_level;
	std::shared_ptr<s_skill_tree> tree = skill_tree_db.find(sd->status.class_);
	if (!tree || tree->skills.empty())
		return;
	pop_companion_forget_skills(sd);
	if (!by_level) {
		for (const auto &[sid, entry] : tree->skills) {
			if (entry && entry->max_lv > 0)
				pc_skill(sd, sid, entry->max_lv, ADDSKILL_PERMANENT_GRANTED);
		}
		return;
	}

	// 49 points from the 1st job, then the job levels of this one.
	const uint32 max_job = job_db.get_maxJobLv(sd->status.class_);
	const int32 own = static_cast<int32>(max_job > 1 ? max_job - 1 : 49);
	const int32 max_points = (sd->class_ & JOBL_2) != 0 ? 49 + own : own;
	const int32 lv = static_cast<int32>(std::min<uint32>(sd->status.base_level, 99));
	const int32 budget_total = (lv - 1) * max_points / 98;
	int32 budget = budget_total;

	std::function<void(uint16, uint16)> learn = [&](uint16 id, uint16 want) {
		auto it = tree->skills.find(id);
		if (it == tree->skills.end() || !it->second || budget <= 0)
			return;
		const s_skill_tree_entry &e = *it->second;
		if (e.baselv > sd->status.base_level || e.joblv > sd->status.job_level)
			return;
		want = std::min(want, e.max_lv);
		const uint16 have = pc_checkskill(sd, id);
		if (have >= want)
			return;
		for (const auto &[need_id, need_lv] : e.need)
			learn(need_id, need_lv);
		for (const auto &[need_id, need_lv] : e.need) {
			if (pc_checkskill(sd, need_id) < need_lv)
				return;
		}
		const int32 add = std::min<int32>(want - have, budget);
		if (add <= 0)
			return;
		pc_skill(sd, id, static_cast<uint16>(have + add), ADDSKILL_PERMANENT_GRANTED);
		budget -= add;
	};
	if (const std::vector<s_pop_skill_entry> *rotation = population_skill_db().find(sd->status.class_)) {
		for (const s_pop_skill_entry &e : *rotation) {
			if (e.skill_id != 0)
				learn(e.skill_id, e.skill_lv);
		}
	}
	// A Priest companion always carries Resurrection: that is how fallen companions come back.
	if ((sd->class_ & MAPID_SECONDMASK) == MAPID_PRIEST && pc_checkskill(sd, ALL_RESURRECTION) < 4)
		pc_skill(sd, ALL_RESURRECTION, 4, ADDSKILL_PERMANENT_GRANTED);

	int known = 0;
	for (uint16 i = 1; i < MAX_SKILL; i++) {
		if (sd->status.skill[i].id != 0 && sd->status.skill[i].lv > 0)
			++known;
	}
	ShowInfo("Population engine: companion %s (%s Lv%d) knows %d skills for its level (%d of %d skill points).\n",
		sd->status.name, job_name(sd->status.class_), lv, known, budget_total - budget, budget_total);
}

static void pop_companion_town_revive(map_session_data *sd, map_session_data *owner, t_tick now);
static bool pop_companion_can_be_revived(map_session_data *sd);

// ---------------------------------------------------------------------------
// Replies: [CMP] JSON for the window, a sentence for everyone else
// ---------------------------------------------------------------------------

static std::string pop_companion_json_str(const char *s)
{
	std::string out = "\"";
	for (const char *p = s; p && *p; ++p) {
		const unsigned char c = static_cast<unsigned char>(*p);
		if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
		else if (c < 0x20) out += ' ';
		else out += static_cast<char>(c);
	}
	out += '"';
	return out;
}

static void pop_companion_send_json(map_session_data *sd, const std::string &json)
{
	static uint32 seq = 0;
	seq = (seq + 1) % 100000;
	// clif_displaymessage caps a line at 255 characters; the chunk leaves room
	// for the header. Line breaks never occur in the JSON we build.
	constexpr size_t chunk = 200;
	const size_t n = std::max<size_t>(1, (json.size() + chunk - 1) / chunk);
	for (size_t i = 0; i < n; ++i) {
		char line[CHAT_SIZE_MAX];
		safesnprintf(line, sizeof(line), "[CMP]%u/%zu/%zu:%s", seq, i + 1, n,
			json.substr(i * chunk, chunk).c_str());
		clif_displaymessage(sd->fd, line);
	}
}

/// A refusal or a confirmation: the window shows `text` in its status line,
/// and a player typing the command gets the same words.
static void pop_companion_notice(map_session_data *sd, const char *code, const char *text, bool plain = true)
{
	std::string json = "{\"t\":\"notice\",\"code\":";
	json += pop_companion_json_str(code);
	json += ",\"text\":";
	json += pop_companion_json_str(text);
	json += "}";
	pop_companion_send_json(sd, json);
	if (plain) {
		char buf[CHAT_SIZE_MAX];
		safesnprintf(buf, sizeof(buf), "Companions: %s", text);
		clif_displaymessage(sd->fd, buf);
	}
}

static const char *pop_companion_mode_key(PopulationCompanionMode m)
{
	switch (m) {
	case PopulationCompanionMode::Dangerous: return "dangerous";
	case PopulationCompanionMode::Attack:    return "free";
	case PopulationCompanionMode::Passive:   return "hold";
	default:                                 return "standard";
	}
}

static std::vector<map_session_data *> pop_companion_list(map_session_data *owner)
{
	std::vector<map_session_data *> out;
	for (map_session_data *sd : g_population_engine_pcs) {
		if (sd && pop_is_companion(sd) && sd->pop.companion_owner_account == owner->status.account_id
			&& sd->status.party_id == owner->status.party_id)
			out.push_back(sd);
	}
	std::sort(out.begin(), out.end(), [](const map_session_data *a, const map_session_data *b) { return a->id < b->id; });
	return out;
}

static void pop_companion_send_state(map_session_data *owner)
{
	const PopCompanionTactics &t = pop_companion_tactics(owner);
	struct party_data *p = owner->status.party_id > 0 ? party_search(owner->status.party_id) : nullptr;
	int members = 0;
	if (p) {
		for (int i = 0; i < MAX_PARTY; ++i)
			if (p->party.member[i].account_id != 0)
				++members;
	}
	const bool upper = (pc_jobid2mapid(owner->status.class_) & (JOBL_UPPER | JOBL_THIRD | JOBL_FOURTH)) != 0;
	const char *last = pc_readregistry_str(owner, pop_companion_reg("COMPANION_LAST$"));

	std::string j = "{\"t\":\"state\",\"engine\":";
	j += battle_config.population_engine_enable && g_population_engine_running ? "true" : "false";
	j += ",\"party\":" + std::to_string(p ? owner->status.party_id : 0);
	j += ",\"leader\":";
	j += p && party_isleader(owner) ? "true" : "false";
	j += ",\"members\":" + std::to_string(members);
	j += ",\"max\":" + std::to_string(MAX_PARTY);
	j += ",\"cap\":" + std::to_string(pop_companion_limit());
	j += ",\"level\":" + std::to_string(owner->status.base_level);
	j += ",\"upper\":";
	j += upper ? "true" : "false";
	j += ",\"myduty\":\"";
	j += pop_companion_duty_key(pop_companion_owner_duty(owner));
	j += "\",\"mydutyauto\":";
	j += t.my_duty == PopulationCompanionDuty::None ? "true" : "false";
	j += ",\"skills\":\"";
	j += t.skills_by_level ? "level" : "full";
	j += "\"";
	j += ",\"tactic\":\"";
	j += pop_companion_mode_key(t.mode);
	j += "\",\"heal\":{\"mode\":\"";
	j += t.heal_by_duty ? "duty" : "lowest";
	j += "\",\"order\":\"";
	for (uint8 o : t.order)
		j += static_cast<char>('0' + o);
	j += "\",\"line\":" + std::to_string(t.heal_line);
	j += ",\"emergency\":" + std::to_string(t.emergency_line);
	j += "},\"last\":";
	j += pop_companion_json_str(last ? last : "");
	j += ",\"list\":[";
	bool first = true;
	for (map_session_data *sd : pop_companion_list(owner)) {
		if (!first) j += ",";
		first = false;
		const PopCompanionFamily *fam = sd->pop.companion_family > 0 && sd->pop.companion_family <= kPopCompanionFamilyCount
			? &kPopCompanionFamilies[sd->pop.companion_family - 1] : nullptr;
		j += "{\"gid\":" + std::to_string(sd->id);
		j += ",\"name\":" + pop_companion_json_str(sd->status.name);
		j += ",\"job\":" + std::to_string(sd->status.class_);
		j += ",\"family\":\"";
		j += fam ? fam->key : "";
		j += "\",\"lv\":" + std::to_string(sd->status.base_level);
		j += ",\"duty\":\"";
		j += pop_companion_duty_key(sd->pop.companion_duty != PopulationCompanionDuty::None
			? sd->pop.companion_duty : pop_companion_ally_duty(sd));
		j += "\",\"quality\":\"";
		j += sd->pop.companion_summoned ? pop_companion_quality_key(sd->pop.companion_quality) : "recruited";
		j += "\",\"role\":" + std::to_string(static_cast<int>(sd->pop.role));
		j += ",\"hp\":" + std::to_string(sd->battle_status.hp);
		j += ",\"maxhp\":" + std::to_string(sd->battle_status.max_hp);
		j += ",\"sp\":" + std::to_string(sd->battle_status.sp);
		j += ",\"maxsp\":" + std::to_string(sd->battle_status.max_sp);
		j += ",\"dead\":";
		j += pc_isdead(sd) ? "true" : "false";
		j += ",\"pull\":" + std::to_string(static_cast<int>(sd->pop.companion_pull));
		const t_tick now_tick = gettick();
		j += ",\"down\":" + std::to_string(pc_isdead(sd) && sd->pop.companion_down_since != 0
			? DIFF_TICK(now_tick, sd->pop.companion_down_since) / 1000 : 0);
		j += ",\"reviver\":";
		j += pc_isdead(sd) && sd->m == owner->m && pop_companion_can_be_revived(sd) ? "true" : "false";
		j += ",\"town\":" + std::to_string(sd->pop.companion_town_until != 0 && DIFF_TICK(sd->pop.companion_town_until, now_tick) > 0
			? (DIFF_TICK(sd->pop.companion_town_until, now_tick) + 999) / 1000 : 0);
		j += "}";
	}
	j += "]}";
	pop_companion_send_json(owner, j);
}

// ---------------------------------------------------------------------------
// Summoning
// ---------------------------------------------------------------------------

static void pop_companion_clear_gear(map_session_data *sd)
{
	for (int i = 0; i < MAX_INVENTORY; ++i) {
		item &it = sd->inventory.u.items_inventory[i];
		if (it.nameid == 0)
			continue;
		if (it.equip != 0)
			pc_unequipitem(sd, i, 1 | 2);
		pc_delitem(sd, i, it.amount, 0, 0, LOG_TYPE_NONE);
	}
}

static uint32 pop_companion_slot_pos(const std::string &slot)
{
	if (slot == "Acc_L") return EQP_ACC_L;
	if (slot == "Acc_R") return EQP_ACC_R;
	return 0;
}

static bool pop_companion_equip(map_session_data *sd, const PopCompanionBuildItem &want)
{
	std::shared_ptr<item_data> data = itemdb_exists(want.nameid);
	if (!data || !data->equip)
		return false;
	sd->max_weight = 2000000;
	item tmp = {};
	tmp.nameid = want.nameid;
	tmp.amount = 1;
	tmp.identify = 1;
	if (!data->flag.no_refine)
		tmp.refine = std::min<uint8>(want.refine, MAX_REFINE);
	for (size_t c = 0; c < want.cards.size() && c < data->slots; ++c)
		tmp.card[c] = want.cards[c];
	if (pc_additem(sd, &tmp, 1, LOG_TYPE_NONE, false) != ADDITEM_SUCCESS)
		return false;
	for (int16 i = 0; i < MAX_INVENTORY; ++i) {
		const item &slot = sd->inventory.u.items_inventory[i];
		if (slot.nameid != want.nameid || slot.equip != 0 || slot.refine != tmp.refine)
			continue;
		// The same validation a real player gets: a build row that names an item
		// this job or level cannot wear is dropped, not forced on.
		if (pc_isequip(sd, i) != ITEM_EQUIP_ACK_OK) {
			pc_delitem(sd, i, 1, 0, 0, LOG_TYPE_NONE);
			return false;
		}
		const uint32 pos = pop_companion_slot_pos(want.slot);
		return pc_equipitem(sd, i, pos != 0 ? pos : data->equip, false);
	}
	return false;
}

/// Spend a real character's stat points for this level along the plan.
static void pop_companion_allocate_stats(map_session_data *sd, const std::vector<std::pair<int32, int32>> &plan, int32 rest_stat)
{
	sd->status.str = sd->status.agi = sd->status.vit = 1;
	sd->status.int_ = sd->status.dex = sd->status.luk = 1;
	int32 budget = static_cast<int32>(statpoint_db.get_table_point(sd->status.base_level));
	if ((sd->class_ & JOBL_UPPER) != 0)
		budget += 52; // rebirth bonus, as pc_resetstate grants it
	auto stat_ref = [sd](int32 type) -> uint16 & {
		switch (type) {
		case SP_STR: return sd->status.str;
		case SP_AGI: return sd->status.agi;
		case SP_VIT: return sd->status.vit;
		case SP_INT: return sd->status.int_;
		case SP_DEX: return sd->status.dex;
		default:     return sd->status.luk;
		}
	};
	auto raise = [&](int32 type, int32 target) {
		uint16 &v = stat_ref(type);
		while (v < target) {
			const int32 cost = pc_need_status_point(sd, type, 1);
			if (cost <= 0 || cost > budget)
				break;
			budget -= cost;
			++v;
		}
	};
	for (const auto &step : plan)
		raise(step.first, step.second);
	raise(rest_stat, 999);
	// Whatever is still left cannot buy a point in the rest stat; spread it.
	for (int32 type = SP_STR; type <= SP_LUK; ++type)
		raise(type, 999);
	sd->status.status_point = 0;
}

struct PopCompanionSummonRequest {
	size_t family = 0;
	PopulationCompanionDuty duty = PopulationCompanionDuty::None;
	uint8 quality = 0;
	bool trans = false;
	std::string name;
};

/// Returns the spawned shell, or nullptr after telling the owner why not.
static map_session_data *pop_companion_summon(map_session_data *owner, const PopCompanionSummonRequest &req, bool quiet_limits = false)
{
	if (!battle_config.population_engine_enable || !g_population_engine_running) {
		pop_companion_notice(owner, "engine_off", "turn on Settings > Population > Fake players first; companions are fake players.");
		return nullptr;
	}
	if (pc_isdead(owner)) {
		pop_companion_notice(owner, "dead", "you cannot hire companions while dead.");
		return nullptr;
	}
	struct party_data *p = owner->status.party_id > 0 ? party_search(owner->status.party_id) : nullptr;
	if (!p) {
		pop_companion_notice(owner, "no_party", "form a party first.");
		return nullptr;
	}
	if (!party_isleader(owner)) {
		pop_companion_notice(owner, "not_leader", "only the party leader can hire companions.");
		return nullptr;
	}
	// Joining is a char-server round trip: a companion summoned a moment ago
	// is not a member yet, but it has its seat.
	size_t pending = 0;
	for (const map_session_data *c : g_population_engine_pcs) {
		if (c && c->pop.companion_summoned && !pop_is_companion(c) &&
			c->pop.companion_owner_account == owner->status.account_id)
			++pending;
	}
	int members = static_cast<int>(pending);
	for (int i = 0; i < MAX_PARTY; ++i)
		if (p->party.member[i].account_id != 0)
			++members;
	if (members >= MAX_PARTY) {
		if (!quiet_limits)
			pop_companion_notice(owner, "party_full", "the party is full.");
		return nullptr;
	}
	if (!population_engine_can_recruit_companion(owner) ||
		pop_companion_list(owner).size() + pending >= pop_companion_limit()) {
		if (!quiet_limits) {
			char text[128];
			safesnprintf(text, sizeof(text), "you already have %zu companions (Settings > Population > Party invitations).",
				pop_companion_limit());
			pop_companion_notice(owner, "cap", text);
		}
		return nullptr;
	}
	if (req.family >= kPopCompanionFamilyCount) {
		pop_companion_notice(owner, "bad_request", "unknown job.");
		return nullptr;
	}
	const PopCompanionFamily &fam = kPopCompanionFamilies[req.family];
	if (!pop_companion_family_has_duty(fam, req.duty)) {
		pop_companion_notice(owner, "bad_request", "that job cannot be hired for that duty.");
		return nullptr;
	}

	const uint16 job_id = req.trans ? fam.trans_job : fam.job;
	PopulationDbSource pop_src = PopulationDbSource::Main;
	std::shared_ptr<PopulationEngine> profile = population_engine_find_any(job_id, &pop_src);
	if (!profile && job_id != fam.job)
		profile = population_engine_find_any(fam.job, &pop_src);
	if (!profile) {
		const uint16 base_job = get_base_job(job_id);
		if (base_job != job_id)
			profile = population_engine_find_any(base_job, &pop_src);
	}
	const PopulationEngine *pop_cfg = profile ? profile.get() : nullptr;

	int16 x = owner->x, y = owner->y;
	if (!map_search_freecell(owner, owner->m, &x, &y, 2, 2, 0)) {
		x = owner->x;
		y = owner->y;
	}

	char sex;
	const char req_sex = get_job_required_sex(job_id);
	if (req_sex != '\0') sex = req_sex;
	else if (pop_cfg && pop_cfg->sex_override >= 0) sex = pop_cfg->sex_override ? 'M' : 'F';
	else sex = (rnd() % 2) ? 'M' : 'F';

	auto pick = [](const std::vector<uint16_t> &pool) -> uint16_t {
		return pool.empty() ? 0 : pool[rnd() % pool.size()];
	};
	const uint16 head_top    = pop_cfg ? pick(pop_cfg->head_top_pool) : 0;
	const uint16 head_mid    = pop_cfg ? pick(pop_cfg->head_mid_pool) : 0;
	const uint16 head_bottom = pop_cfg ? pick(pop_cfg->head_bottom_pool) : 0;
	const uint16 garment     = pop_cfg ? pick(pop_cfg->garment_pool) : 0;
	const uint16 weapon      = pop_cfg ? pick(pop_cfg->weapon_pool) : get_job_weapon(job_id);
	const uint16 shield      = pop_cfg ? pick(pop_cfg->shield_pool) : 0;

	const uint32_t index = population_engine_allocate_index();
	if (index == 0) {
		pop_companion_notice(owner, "spawn_failed", "the server has no room for another fake player.");
		return nullptr;
	}

	g_pop_companion_name_override = req.name;
	map_session_data *sd = population_engine_spawn_shell(owner->m, x, y, index, job_id, sex,
		static_cast<uint8_t>(population_roll_closed_range(MIN_HAIR_STYLE, MAX_HAIR_STYLE)),
		static_cast<uint16_t>(population_roll_closed_range(MIN_HAIR_COLOR, MAX_HAIR_COLOR)),
		weapon, shield, head_top, head_mid, head_bottom, 0,
		static_cast<uint16_t>(population_roll_closed_range(MIN_CLOTH_COLOR, MAX_CLOTH_COLOR)),
		garment, pop_cfg ? pop_cfg->script : nullptr, false, pop_cfg, 2 /*field*/, pop_src);
	g_pop_companion_name_override.clear();
	if (!sd) {
		pop_companion_notice(owner, "spawn_failed", "the companion could not be placed here.");
		return nullptr;
	}

	// Level: yours, as far as the job goes (2nd and transcendent jobs stop at their cap).
	const uint32 max_base = job_db.get_maxBaseLv(sd->status.class_);
	sd->status.base_level = static_cast<uint32>(std::min<uint32>(owner->status.base_level, max_base > 0 ? max_base : 99));
	const uint32 max_job = job_db.get_maxJobLv(sd->status.class_);
	sd->status.job_level = max_job > 0 ? max_job : 50;

	const std::string family_key = fam.key;
	const PopCompanionBuild *build = g_pop_companion_builds.find(family_key, req.duty, req.quality,
		static_cast<uint16>(sd->status.base_level));
	std::vector<std::pair<int32, int32>> plan;
	int32 rest = SP_VIT;
	if (build && !build->stats.empty()) {
		plan = build->stats;
		rest = build->rest_stat;
	} else {
		for (const auto &step : fam.plan)
			plan.push_back(step);
		if (req.duty == PopulationCompanionDuty::Defender && family_key == "knight")
			plan = { {SP_VIT, 90}, {SP_STR, 60}, {SP_DEX, 40}, {SP_AGI, 30} };
	}
	pop_companion_allocate_stats(sd, plan, rest);
	if (pop_companion_tactics(owner).skills_by_level)
		pop_companion_apply_skills(sd, true);

	if (build && !build->equip.empty()) {
		pop_companion_clear_gear(sd);
		int dropped = 0;
		for (const PopCompanionBuildItem &want : build->equip) {
			if (!pop_companion_equip(sd, want))
				++dropped;
		}
		if (dropped > 0)
			ShowWarning("Population engine: companion build %s/%s/%s Lv%d: %d item(s) could not be equipped by %s.\n",
				fam.key, pop_companion_duty_key(req.duty), pop_companion_quality_key(req.quality),
				build->min_level, dropped, sd->status.name);
		population_shell_prepare_ammo(sd);
	}

	status_calc_pc(sd, SCO_FORCE);
	sd->status.hp = sd->battle_status.hp = sd->battle_status.max_hp;
	sd->status.sp = sd->battle_status.sp = sd->battle_status.max_sp;
	population_engine_sync_vd_weapon_shield(sd);
	clif_changelook(sd, LOOK_WEAPON, sd->vd.look[LOOK_WEAPON]);
	clif_changelook(sd, LOOK_SHIELD, sd->vd.look[LOOK_SHIELD]);
	clif_changelook(sd, LOOK_HEAD_TOP, sd->status.head_top);
	clif_changelook(sd, LOOK_HEAD_MID, sd->status.head_mid);
	clif_changelook(sd, LOOK_HEAD_BOTTOM, sd->status.head_bottom);
	clif_changelook(sd, LOOK_ROBE, sd->status.robe);

	// Mortal like a party member should be; companions stay put as corpses on death.
	sd->pop.flags |= PSF::Mortal | PSF::CombatActive;
	sd->pop.companion_summoned = true;
	sd->pop.companion_duty = req.duty;
	sd->pop.companion_quality = req.quality;
	sd->pop.companion_family = static_cast<uint8_t>(req.family + 1);
	sd->pop.companion_trans = req.trans && fam.trans_job != fam.job;
	switch (req.duty) {
	case PopulationCompanionDuty::Defender: sd->pop.role = static_cast<int8_t>(PopulationRoleType::Tank); break;
	case PopulationCompanionDuty::Support1:
	case PopulationCompanionDuty::Support2: sd->pop.role = static_cast<int8_t>(PopulationRoleType::Support); break;
	default: sd->pop.role = static_cast<int8_t>(PopulationRoleType::Attacker); break;
	}
	sd->pop.companion_mode = pop_companion_tactics(owner).mode;

	g_population_engine_pcs.push_back(sd);
	g_population_engine_count++;
	g_population_engine_stats.total_created++;
	g_population_engine_stats.active_units++;

	// Join through the same consent path a whispered recruit takes: party.cpp's
	// shell branch of party_invite accepts at once and clears the synthetic party.
	sd->pop.accept_party_request = true;
	sd->pop.party_request_account = owner->status.account_id;
	sd->pop.party_request_until = gettick() + 60000;
	sd->pop.companion_owner_account = owner->status.account_id;
	if (!party_invite(*owner, sd)) {
		pop_companion_notice(owner, "spawn_failed", "the companion could not join your party.");
		population_engine_shell_release(sd);
		return nullptr;
	}
	ShowInfo("Population engine: %s summoned %s (%s, %s, %s, Lv %d).\n", owner->status.name, sd->status.name,
		job_name(sd->status.class_), pop_companion_duty_key(req.duty), pop_companion_quality_key(req.quality),
		sd->status.base_level);
	return sd;
}

/// Remember who is in the party, for "Summon last party" after a restart.
static void pop_companion_save_lineup(map_session_data *owner)
{
	std::string lineup;
	for (map_session_data *sd : pop_companion_list(owner)) {
		if (!sd->pop.companion_summoned || sd->pop.companion_family == 0)
			continue;
		if (!lineup.empty())
			lineup += ";";
		lineup += kPopCompanionFamilies[sd->pop.companion_family - 1].key;
		lineup += ":" + std::to_string(static_cast<int>(sd->pop.companion_duty));
		lineup += ":" + std::to_string(sd->pop.companion_quality);
		lineup += sd->pop.companion_trans ? ":1:" : ":0:";
		lineup += sd->status.name;
	}
	// A lineup lives until the owner builds a different one; an empty party
	// after a restart must not erase what "Summon last party" would bring back.
	if (lineup.empty())
		return;
	const char *saved = pc_readregistry_str(owner, pop_companion_reg("COMPANION_LAST$"));
	if (saved && lineup == saved)
		return;
	pc_setregistry_str(owner, pop_companion_reg("COMPANION_LAST$"), lineup.c_str());
}

// ---------------------------------------------------------------------------
// Tactics at runtime: formation, target choice, Taunt/Pull
// ---------------------------------------------------------------------------

/// Tight formation for Dangerous: which of the cells around the owner this
/// companion prefers. Higher is better.
static int pop_companion_dangerous_cell_score(map_session_data *sd, map_session_data *owner, int dx, int dy)
{
	int score = -10 * std::max(std::abs(dx), std::abs(dy));
	if (pop_companion_owner_duty(owner) == PopulationCompanionDuty::Defender) {
		// You hold the front: companions stand behind you, the side away from
		// where you face, and companion defenders flank you.
		const int8 dir = static_cast<int8>(unit_getdir(owner));
		const int bx = -dirx[dir], by = -diry[dir];
		const int dot = dx * bx + dy * by;
		if (sd->pop.companion_duty == PopulationCompanionDuty::Defender)
			score -= 4 * std::abs(dot);
		else
			score += 4 * dot;
	}
	return score;
}

static bool pop_companion_dangerous_cell(map_session_data *sd, map_session_data *owner, int16 &out_x, int16 &out_y)
{
	std::vector<map_session_data *> companions;
	for (map_session_data *c : g_population_engine_pcs) {
		if (pop_is_companion(c) && c->state.active && !pc_isdead(c) &&
			c->pop.companion_owner_account == owner->status.account_id)
			companions.push_back(c);
	}
	std::sort(companions.begin(), companions.end(), [](const map_session_data *a, const map_session_data *b) { return a->id < b->id; });
	std::vector<std::pair<int16, int16>> reserved;
	for (map_session_data *c : companions) {
		int best = INT_MIN;
		int16 bx = 0, by = 0;
		for (int dy = -2; dy <= 2; ++dy) {
			for (int dx = -2; dx <= 2; ++dx) {
				if (dx == 0 && dy == 0)
					continue;
				const int16 cx = static_cast<int16>(owner->x + dx), cy = static_cast<int16>(owner->y + dy);
				if (map_getcell(owner->m, cx, cy, CELL_CHKNOPASS))
					continue;
				if (std::find(reserved.begin(), reserved.end(), std::make_pair(cx, cy)) != reserved.end())
					continue;
				const int score = pop_companion_dangerous_cell_score(c, owner, dx, dy);
				if (score > best) {
					best = score;
					bx = cx;
					by = cy;
				}
			}
		}
		if (best == INT_MIN)
			continue;
		reserved.emplace_back(bx, by);
		if (c == sd) {
			out_x = bx;
			out_y = by;
			return true;
		}
	}
	return false;
}

struct PopCompanionMobCount { uint32 tank_id; int count; };

static int32 pop_companion_count_pulled_cb(block_list *bl, va_list ap)
{
	auto *ctx = va_arg(ap, PopCompanionMobCount *);
	mob_data *md = BL_CAST(BL_MOB, bl);
	if (md && !status_isdead(*md) && md->target_id == static_cast<int32>(ctx->tank_id))
		ctx->count++;
	return 0;
}

static void pop_companion_pull_finish(map_session_data *sd, map_session_data *owner, bool ok, int pulled)
{
	sd->pop.companion_pull = PopulationCompanionPull::None;
	sd->pop.companion_pull_target = 0;
	sd->pop.companion_pull_until = 0;
	if (!owner)
		return;
	char text[128];
	if (ok)
		safesnprintf(text, sizeof(text), "%s pulled %d monster%s to you.", sd->status.name, pulled, pulled == 1 ? "" : "s");
	else
		safesnprintf(text, sizeof(text), "%s could not pull the target.", sd->status.name);
	pop_companion_notice(owner, ok ? "pull_ok" : "pull_failed", text);
	pop_companion_send_state(owner);
}

/// Drive one Defender through Approach -> Provoke -> Return -> Hold.
/// Returns true while the pull owns this shell's tick.
static bool pop_companion_pull_tick(map_session_data *sd, map_session_data *owner, t_tick now)
{
	if (sd->pop.companion_pull == PopulationCompanionPull::None)
		return false;
	mob_data *md = map_id2md(static_cast<int32>(sd->pop.companion_pull_target));
	if (DIFF_TICK(now, sd->pop.companion_pull_until) > 0 || sd->m != owner->m) {
		pop_companion_pull_finish(sd, owner, false, 0);
		return false;
	}
	switch (sd->pop.companion_pull) {
	case PopulationCompanionPull::Approach: {
		if (!md || status_isdead(*md) || md->m != sd->m) {
			pop_companion_pull_finish(sd, owner, false, 0);
			return false;
		}
		if (md->target_id == sd->id) {
			sd->pop.companion_pull = PopulationCompanionPull::Return;
			unit_stop_attack(sd);
			return true;
		}
		const int dist = distance_bl(sd, md);
		const uint16 provoke = pc_checkskill(sd, SM_PROVOKE);
		if (sd->ud.skilltimer != INVALID_TIMER)
			return true;
		if (provoke > 0 && dist <= skill_get_range2(sd, SM_PROVOKE, provoke, true)) {
			if (unit_is_walking(sd))
				unit_stop_walking(sd, USW_FIXPOS);
			unit_skilluse_id(sd, md->id, SM_PROVOKE, provoke);
		} else if (provoke == 0 && dist <= 1) {
			unit_attack(sd, md->id, 0);
		} else if (!unit_is_walking(sd)) {
			unit_walktobl(sd, md, provoke > 0 ? 7 : 1, 1);
		}
		return true;
	}
	case PopulationCompanionPull::Return:
		if (distance_bl(sd, owner) <= 1) {
			sd->pop.companion_pull = PopulationCompanionPull::Hold;
			return true;
		}
		if (!unit_is_walking(sd))
			unit_walktobl(sd, owner, 1, 1);
		return true;
	case PopulationCompanionPull::Hold: {
		PopCompanionMobCount ctx{ static_cast<uint32>(sd->id), 0 };
		map_foreachinallrange(pop_companion_count_pulled_cb, owner, 3, BL_MOB, &ctx);
		if (ctx.count > 0) {
			pop_companion_pull_finish(sd, owner, true, ctx.count);
			return false;
		}
		return true;
	}
	default:
		return false;
	}
}

/// A monster to pull when you have no target: the closest one within 12 cells
/// that is not already on the party.
static int32 pop_companion_pick_pull_target(map_session_data *owner, const std::vector<uint32> &taken)
{
	struct Ctx { map_session_data *owner; const std::vector<uint32> *taken; int32 best; int best_dist; } ctx{ owner, &taken, 0, 99 };
	map_foreachinallrange([](block_list *bl, va_list ap) -> int32 {
		auto *c = va_arg(ap, Ctx *);
		mob_data *md = BL_CAST(BL_MOB, bl);
		if (!md || status_isdead(*md) || md->special_state.ai != AI_NONE)
			return 0;
		if (std::find(c->taken->begin(), c->taken->end(), static_cast<uint32>(md->id)) != c->taken->end())
			return 0;
		if (md->target_id != 0) {
			map_session_data *victim = map_id2sd(md->target_id);
			if (victim && victim->status.party_id == c->owner->status.party_id)
				return 0; // already on us
		}
		if (battle_check_target(c->owner, md, BCT_ENEMY) <= 0)
			return 0;
		const int d = distance_bl(c->owner, md);
		if (d < c->best_dist) {
			c->best_dist = d;
			c->best = md->id;
		}
		return 0;
	}, owner, 12, BL_MOB, &ctx);
	return ctx.best;
}

/// Free mode: go after something the others are not already hitting, the
/// closest to dying first, within 14 cells of the owner.
static uint32 pop_companion_free_target(map_session_data *sd, map_session_data *owner)
{
	if (sd->pop.target_id != 0) {
		block_list *cur = map_id2bl(sd->pop.target_id);
		if (cur && cur->m == owner->m && distance_bl(owner, cur) <= 14 &&
			population_shell_check_target(sd, static_cast<uint32>(sd->pop.target_id)))
			return static_cast<uint32>(sd->pop.target_id);
	}
	uint32 best_id = 0;
	int best_score = INT_MAX;
	for (const auto &entry : sd->pop.mob_tracker.tracked_mobs) {
		const s_pe_tracked_mob &mob = entry.second;
		mob_data *md = map_id2md(static_cast<int32>(mob.mob_id));
		if (!md || md->m != owner->m || status_isdead(*md))
			continue;
		if (distance_bl(owner, md) > 14)
			continue;
		if (!population_shell_check_target(sd, mob.mob_id) &&
			!population_shell_check_target_for_movement(sd, mob.mob_id))
			continue;
		int claimed = 0;
		for (map_session_data *c : g_population_engine_pcs) {
			if (c != sd && pop_is_companion(c) && c->pop.companion_owner_account == owner->status.account_id &&
				c->pop.target_id == static_cast<int>(mob.mob_id))
				++claimed;
		}
		const int hp_pct = md->status.max_hp > 0 ? static_cast<int>(static_cast<int64>(md->status.hp) * 100 / md->status.max_hp) : 100;
		const int score = claimed * 1000 + hp_pct * 3 + distance_bl(sd, md) * 10;
		if (score < best_score) {
			best_score = score;
			best_id = mob.mob_id;
		}
	}
	return best_id;
}

/// Summoned shells are conjured for their owner: once the owner has been gone
/// for 15 seconds (logout, character select), they go too. "Summon last party"
/// brings them back.
static bool pop_companion_orphaned(map_session_data *sd, t_tick now)
{
	if (!sd->pop.companion_summoned)
		return false;
	if (pop_companion_owner(sd) != nullptr) {
		sd->pop.companion_orphan_since = 0;
		return false;
	}
	if (sd->pop.companion_orphan_since == 0) {
		sd->pop.companion_orphan_since = now;
		return false;
	}
	return DIFF_TICK(now, sd->pop.companion_orphan_since) > 15000;
}

// ---------------------------------------------------------------------------
// @companion
// ---------------------------------------------------------------------------

static void pop_companion_set_mode(map_session_data *owner, PopulationCompanionMode mode)
{
	PopCompanionTactics &t = pop_companion_tactics(owner);
	t.mode = mode;
	pop_companion_tactics_save(owner, t);
	for (map_session_data *sd : pop_companion_list(owner)) {
		sd->pop.companion_mode = mode;
		population_companion_clear_target(sd);
	}
}

static void pop_companion_cmd_resummon(map_session_data *owner)
{
	const char *saved = pc_readregistry_str(owner, pop_companion_reg("COMPANION_LAST$"));
	if (!saved || !*saved) {
		pop_companion_notice(owner, "no_lineup", "there is no saved party yet.");
		return;
	}
	std::vector<std::string> present;
	for (map_session_data *sd : pop_companion_list(owner))
		present.emplace_back(sd->status.name);
	int summoned = 0;
	std::string all(saved);
	size_t start = 0;
	while (start <= all.size()) {
		const size_t end = all.find(';', start);
		const std::string entry = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
		start = end == std::string::npos ? all.size() + 1 : end + 1;
		char fam[24] = {}, name[NAME_LENGTH] = {};
		int duty = 0, quality = 0, trans = 0;
		if (sscanf(entry.c_str(), "%23[a-z]:%d:%d:%d:%23[^;]", fam, &duty, &quality, &trans, name) < 4)
			continue;
		if (name[0] && std::find(present.begin(), present.end(), std::string(name)) != present.end())
			continue;
		PopCompanionSummonRequest req;
		if (!pop_companion_family(fam, &req.family))
			continue;
		req.duty = static_cast<PopulationCompanionDuty>(cap_value(duty, 0, 4));
		req.quality = static_cast<uint8>(cap_value(quality, 0, 2));
		req.trans = trans != 0;
		req.name = name;
		if (!pop_companion_summon(owner, req, summoned > 0))
			break;
		++summoned;
	}
	if (summoned > 0) {
		char text[96];
		safesnprintf(text, sizeof(text), "%d companion%s rejoined you.", summoned, summoned == 1 ? "" : "s");
		pop_companion_notice(owner, "resummoned", text, false);
	}
}

/// @companion <verb> [args]. With no verb the patched client opens its window.
int population_companion_command(map_session_data *owner, const char *message)
{
	std::vector<std::string> args;
	{
		std::string word;
		for (const char *p = message ? message : ""; ; ++p) {
			if (*p == '\0' || std::isspace(static_cast<unsigned char>(*p))) {
				if (!word.empty())
					args.push_back(word);
				word.clear();
				if (*p == '\0')
					break;
			} else {
				word += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
			}
		}
	}
	const std::string verb = args.empty() ? "open" : args[0];

	if (verb == "open") {
		pop_companion_send_json(owner, "{\"t\":\"open\"}");
		pop_companion_send_state(owner);
		clif_displaymessage(owner->fd, "Companions: use the Companions window (party window button). "
			"Verbs: state, summon <job> <duty> <quality> [trans], dismiss <name>, tactic <dangerous|standard|free|hold>, taunt, recall, resummon.");
		return 0;
	}
	if (verb == "state") {
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "formparty") {
		if (owner->status.party_id > 0) {
			pop_companion_notice(owner, "has_party", "you are already in a party.", false);
		} else {
			char name[NAME_LENGTH];
			safesnprintf(name, sizeof(name), "%.16s's Party", owner->status.name);
			party_create(*owner, name, 0, 0);
		}
		return 0;
	}
	if (verb == "summon") {
		// summon <family> <duty> <quality> [trans]
		PopCompanionSummonRequest req;
		if (args.size() < 4 || !pop_companion_family(args[1], &req.family)) {
			pop_companion_notice(owner, "bad_request", "usage: @companion summon <job> <attacker|defender|support1|support2> <excellent|good|standard> [trans].");
			return -1;
		}
		req.duty = pop_companion_duty_from_name(args[2]);
		const int q = pop_companion_quality_from_name(args[3]);
		if (q < 0) {
			pop_companion_notice(owner, "bad_request", "quality is excellent, good or standard.");
			return -1;
		}
		req.quality = static_cast<uint8>(q);
		req.trans = args.size() > 4 && args[4] == "trans";
		if (map_session_data *sd = pop_companion_summon(owner, req)) {
			char text[96];
			safesnprintf(text, sizeof(text), "%s joins as your %s.", sd->status.name, pop_companion_duty_key(req.duty));
			pop_companion_notice(owner, "summoned", text, false);
		}
		return 0;
	}
	if (verb == "resummon") {
		pop_companion_cmd_resummon(owner);
		return 0;
	}

	const std::vector<map_session_data *> mine = pop_companion_list(owner);
	auto by_gid_or_name = [&](const std::string &key) -> map_session_data * {
		for (map_session_data *sd : mine) {
			std::string lower = sd->status.name;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (std::to_string(sd->id) == key || lower == key)
				return sd;
		}
		return nullptr;
	};
	if (owner->status.party_id <= 0 || !party_isleader(owner)) {
		pop_companion_notice(owner, "not_leader", "only the party leader commands companions.");
		return -1;
	}

	if (verb == "dismiss" || verb == "dismissall") {
		std::vector<map_session_data *> gone;
		if (verb == "dismissall")
			gone = mine;
		else if (args.size() > 1) {
			if (map_session_data *sd = by_gid_or_name(args[1]))
				gone.push_back(sd);
		}
		if (gone.empty()) {
			pop_companion_notice(owner, "bad_request", "no such companion.");
			return -1;
		}
		for (map_session_data *sd : gone)
			population_engine_shell_release(sd);
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "tactic" || verb == "mode") {
		const std::string w = args.size() > 1 ? args[1] : "";
		PopulationCompanionMode mode;
		if (w == "dangerous") mode = PopulationCompanionMode::Dangerous;
		else if (w == "standard" || w == "defensive") mode = PopulationCompanionMode::Defensive;
		else if (w == "free" || w == "attack") mode = PopulationCompanionMode::Attack;
		else if (w == "hold" || w == "passive") mode = PopulationCompanionMode::Passive;
		else {
			pop_companion_notice(owner, "bad_request", "tactic is dangerous, standard, free or hold.");
			return -1;
		}
		pop_companion_set_mode(owner, mode);
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "role") {
		map_session_data *sd = args.size() > 2 ? by_gid_or_name(args[1]) : nullptr;
		const std::string w = args.size() > 2 ? args[2] : "";
		PopulationRoleType role = PopulationRoleType::None;
		if (w == "tank") role = PopulationRoleType::Tank;
		else if (w == "support") role = PopulationRoleType::Support;
		else if (w == "attacker") role = PopulationRoleType::Attacker;
		if (!sd || role == PopulationRoleType::None) {
			pop_companion_notice(owner, "bad_request", "usage: @companion role <name> <tank|support|attacker>.");
			return -1;
		}
		sd->pop.role = static_cast<int8_t>(role);
		population_companion_clear_target(sd);
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "taunt" || verb == "pull") {
		std::vector<map_session_data *> tanks;
		for (map_session_data *sd : mine) {
			if (!pc_isdead(sd) && sd->m == owner->m &&
				(sd->pop.companion_duty == PopulationCompanionDuty::Defender ||
				 static_cast<PopulationRoleType>(sd->pop.role) == PopulationRoleType::Tank))
				tanks.push_back(sd);
		}
		if (tanks.empty()) {
			pop_companion_notice(owner, "no_defender",
				pop_companion_owner_duty(owner) == PopulationCompanionDuty::Defender
					? "you are the defender: provoke the target yourself."
					: "no living defender companion is here to taunt.");
			return -1;
		}
		std::vector<uint32> taken;
		// What to pull: the monster named in the command, else whatever you are
		// hitting or casting at, else the nearest one not yet on the party.
		int32 given = 0;
		if (args.size() > 1)
			given = std::atoi(args[1].c_str());
		if (given <= 0) {
			if (unit_data *oud = unit_bl2ud(owner)) {
				if (oud->target > 0 && map_id2md(oud->target))
					given = oud->target;
				else if (oud->skilltarget > 0 && map_id2md(oud->skilltarget))
					given = oud->skilltarget;
			}
		}
		int ordered = 0;
		for (size_t i = 0; i < tanks.size(); ++i) {
			int32 target = 0;
			if (i == 0 && given > 0) {
				mob_data *md = map_id2md(given);
				if (md && md->m == owner->m && !status_isdead(*md) && distance_bl(owner, md) <= 14)
					target = md->id;
			}
			if (target == 0)
				target = pop_companion_pick_pull_target(owner, taken);
			if (target == 0)
				break;
			taken.push_back(static_cast<uint32>(target));
			map_session_data *sd = tanks[i];
			population_shell_target_change(sd, 0);
			unit_stop_attack(sd);
			sd->pop.companion_formation_active = false;
			sd->pop.companion_pull = PopulationCompanionPull::Approach;
			sd->pop.companion_pull_target = static_cast<uint32>(target);
			sd->pop.companion_pull_until = gettick() + 12000;
			++ordered;
		}
		if (ordered == 0) {
			pop_companion_notice(owner, "no_target", "there is nothing within 12 cells to pull.");
			return -1;
		}
		char text[96];
		safesnprintf(text, sizeof(text), "%d defender%s going to pull.", ordered, ordered == 1 ? " is" : "s are");
		pop_companion_notice(owner, "pulling", text, false);
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "revive") {
		map_session_data *sd = args.size() > 1 ? by_gid_or_name(args[1]) : nullptr;
		if (!sd || !pc_isdead(sd)) {
			pop_companion_notice(owner, "bad_request", "that companion is not down.");
			return -1;
		}
		pop_companion_town_revive(sd, owner, gettick());
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "recall") {
		for (map_session_data *sd : mine) {
			sd->pop.companion_pull = PopulationCompanionPull::None;
			sd->pop.companion_pull_target = 0;
			sd->pop.companion_recall_until = gettick() + 4000;
			population_companion_clear_target(sd);
			if (unit_is_walking(sd))
				unit_stop_walking(sd, USW_FIXPOS);
		}
		pop_companion_notice(owner, "recalled", "companions fall back to you.", false);
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "healprio") {
		PopCompanionTactics &t = pop_companion_tactics(owner);
		if (args.size() > 1 && args[1] == "lowest") {
			t.heal_by_duty = false;
		} else if (args.size() > 2 && args[1] == "order" && args[2].size() == 5 &&
			std::is_permutation(args[2].begin(), args[2].end(), std::string("01234").begin())) {
			t.heal_by_duty = true;
			for (int i = 0; i < 5; ++i)
				t.order[i] = static_cast<uint8>(args[2][i] - '0');
		} else {
			pop_companion_notice(owner, "bad_request", "usage: @companion healprio lowest | order <5 digits: 0 you 1 atk 2 def 3 s1 4 s2>.");
			return -1;
		}
		pop_companion_tactics_save(owner, t);
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "healline") {
		PopCompanionTactics &t = pop_companion_tactics(owner);
		if (args.size() > 2) {
			t.heal_line = static_cast<uint8>(cap_value(std::atoi(args[1].c_str()), 10, 99));
			t.emergency_line = static_cast<uint8>(cap_value(std::atoi(args[2].c_str()), 5, static_cast<int>(t.heal_line)));
			pop_companion_tactics_save(owner, t);
		}
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "skills") {
		PopCompanionTactics &t = pop_companion_tactics(owner);
		if (args.size() < 2 || (args[1] != "full" && args[1] != "level")) {
			pop_companion_notice(owner, "bad_request", "usage: @companion skills full | level.");
			return -1;
		}
		t.skills_by_level = args[1] == "level";
		pop_companion_tactics_save(owner, t);
		// Hired companions switch now; recruited ones keep the skills they came with.
		for (map_session_data *sd : mine) {
			if (!sd->pop.companion_summoned)
				continue;
			pop_companion_apply_skills(sd, t.skills_by_level);
			status_calc_pc(sd, SCO_FORCE);
		}
		pop_companion_notice(owner, "skills", t.skills_by_level
			? "companions now know only what a character of their level could."
			: "companions now know every skill of their job.", false);
		pop_companion_send_state(owner);
		return 0;
	}
	if (verb == "myduty") {
		PopCompanionTactics &t = pop_companion_tactics(owner);
		t.my_duty = args.size() > 1 && args[1] != "auto" ? pop_companion_duty_from_name(args[1]) : PopulationCompanionDuty::None;
		pop_companion_tactics_save(owner, t);
		pop_companion_send_state(owner);
		return 0;
	}
	pop_companion_notice(owner, "bad_request", "unknown verb; try @companion with no arguments.");
	return -1;
}

/// Called from the global combat timer for every companion with an owner.
/// Returns true when the summoner took this shell's tick (a pull in progress).
static bool pop_companion_tactics_tick(map_session_data *sd, map_session_data *owner, t_tick now)
{
	return pop_companion_pull_tick(sd, owner, now);
}

/// Keep the saved lineup in step with who is actually in the party. Joining
/// is a char-server round trip, so this runs from the timer rather than from
/// the summon itself; it only writes when the lineup changed.
static void pop_companion_lineup_tick(map_session_data *owner, t_tick now)
{
	static std::unordered_map<uint32, t_tick> next_check;
	t_tick &next = next_check[owner->status.account_id];
	if (DIFF_TICK(now, next) < 0)
		return;
	next = now + 3000;
	pop_companion_save_lineup(owner);
}

/// Typed party-chat modes (`atk`, `def`, `pass`) become the owner's tactic too,
/// so the window and later summons agree with what was typed.
static void pop_companion_remember_mode(map_session_data *leader, PopulationCompanionMode mode)
{
	if (!leader)
		return;
	PopCompanionTactics &t = pop_companion_tactics(leader);
	t.mode = mode;
	pop_companion_tactics_save(leader, t);
}

static void pop_companion_load_builds()
{
	if (!g_pop_companion_builds.load())
		ShowWarning("Population engine: population_companion_builds.yml missing or invalid; summoned companions keep their profile gear.\n");
	else
		ShowStatus("Population engine: %zu companion builds loaded.\n", g_pop_companion_builds.builds.size());
}

// ---------------------------------------------------------------------------
// Death: a Priest resurrects the companion, otherwise it recovers in town
// ---------------------------------------------------------------------------

/// A companion says something to its party (as the shell, like role replies).
static void pop_companion_party_say(map_session_data *sd, const char *text)
{
	char buf[CHAT_SIZE_MAX];
	safesnprintf(buf, sizeof(buf), "%s : %s", sd->status.name, text);
	party_send_message(sd, buf, strlen(buf) + 1);
}

struct PopCompanionReviverCtx {
	map_session_data *corpse;
	int32 sp_needed;
	bool found;
};

/// A living party member on the corpse's map who could resurrect it right now:
/// a Priest-line companion (they always know Resurrection) or a real player who
/// has learned it, either way with the SP for the cast.
static int32 pop_companion_reviver_cb(block_list *bl, va_list ap)
{
	auto *c = va_arg(ap, PopCompanionReviverCtx *);
	map_session_data *p = BL_CAST(BL_PC, bl);
	if (!p || c->found || p == c->corpse || pc_isdead(p) || !p->state.active)
		return 0;
	if (p->status.party_id != c->corpse->status.party_id)
		return 0;
	if (population_engine_is_population_pc(p->id)) {
		if (!pop_is_companion(p) || !pop_is_resurrection_job(p->status.class_))
			return 0;
	} else if (pc_checkskill(p, ALL_RESURRECTION) <= 0) {
		return 0;
	}
	if (static_cast<int32>(p->battle_status.sp) < c->sp_needed)
		return 0;
	c->found = true;
	return 1;
}

static bool pop_companion_can_be_revived(map_session_data *sd)
{
	PopCompanionReviverCtx c{ sd, skill_get_sp(ALL_RESURRECTION, 4), false };
	map_foreachinmap(pop_companion_reviver_cb, sd->m, BL_PC, &c);
	return c.found;
}

/// Take a dead companion to its owner's save point and revive it there. Same
/// steps as population_engine_respawn_shell_timer, which is the known-safe way
/// to move a shell's corpse without leaving a duplicate actor behind.
static void pop_companion_town_revive(map_session_data *sd, map_session_data *owner, t_tick now)
{
	uint16 idx = mapindex_name2id(owner->status.save_point.map);
	int16 m = idx != 0 ? map_mapindex2mapid(idx) : -1;
	int16 x = static_cast<int16>(owner->status.save_point.x);
	int16 y = static_cast<int16>(owner->status.save_point.y);
	if (m < 0) {
		// No usable save point: recover beside the owner instead.
		m = owner->m;
		idx = owner->mapindex;
		x = owner->x;
		y = owner->y;
	}
	struct map_data *mapdata = map_getmapdata(m);
	if (!mapdata)
		return;
	if (x > 0 && y > 0)
		map_search_freecell(nullptr, m, &x, &y, 3, 3, 1);
	for (int attempt = 0; attempt < 30 && (x <= 0 || y <= 0 || map_getcell(m, x, y, CELL_CHKNOPASS)); ++attempt) {
		x = static_cast<int16>(1 + rnd() % std::max(1, static_cast<int>(mapdata->xs - 2)));
		y = static_cast<int16>(1 + rnd() % std::max(1, static_cast<int>(mapdata->ys - 2)));
	}

	population_shell_target_change(sd, 0);
	sd->pop.sticky_target_id = 0;
	sd->pop.sticky_until = 0;
	sd->pop.companion_pull = PopulationCompanionPull::None;
	sd->pop.companion_formation_active = false;
	unit_remove_map(sd, CLR_OUTSIGHT);
	if (pc_setpos(sd, idx, x, y, CLR_OUTSIGHT) != SETPOS_OK) {
		ShowError("Population engine: could not take companion %s to town (%s).\n", sd->status.name, mapindex_id2name(idx));
		return;
	}
	status_revive(sd, 100, 100);
	status_calc_pc(sd, SCO_FORCE);
	sd->ud.canmove_tick = 0;
	sd->ud.canact_tick = 0;
	sd->pop.target_id = 0;
	sd->pop.last_attacked_tick = 0;
	sd->pop.last_attacker_id = 0;
	sd->pop.skill_next_use_tick.clear();
	sd->pop.companion_down_since = 0;
	sd->pop.companion_town_until = now + 10000;
	sd->pop.flags |= PSF::CombatActive;
	if (!pop_shell_finish_map_placement(sd)) {
		ShowError("Population engine: failed to place companion %s in town.\n", sd->status.name);
		return;
	}
	pop_shell_broadcast_map_placement(sd);
	population_shell_prepare_ammo(sd);
	pop_companion_party_say(sd, "I'll recover in town and be right back.");
	ShowInfo("Population engine: companion %s revived in town (%s) for %s.\n",
		sd->status.name, mapindex_id2name(idx), owner->status.name);
}

/// Every combat tick for a dead companion with an owner online: wait while
/// someone on its map can resurrect it (30 s at most), otherwise go to town
/// after 5 s. Leaving the map counts as nobody being able to.
static void pop_companion_death_tick(map_session_data *sd, map_session_data *owner, t_tick now)
{
	if (sd->pop.companion_down_since == 0)
		sd->pop.companion_down_since = now;
	const t_tick down = DIFF_TICK(now, sd->pop.companion_down_since);
	if (down < 5000)
		return;
	if (down < 30000 && sd->m == owner->m && pop_companion_can_be_revived(sd))
		return;
	pop_companion_town_revive(sd, owner, now);
}

/// A companion that recovered in town sits there for its rest, then rejoins
/// (the normal follow warps it back beside the owner).
static bool pop_companion_resting_in_town(map_session_data *sd, map_session_data *owner, t_tick now)
{
	(void)owner;
	if (sd->pop.companion_town_until == 0)
		return false;
	if (DIFF_TICK(now, sd->pop.companion_town_until) < 0) {
		if (!pc_issit(sd)) {
			pc_setsit(sd);
			clif_sitting(*sd);
		}
		return true;
	}
	sd->pop.companion_town_until = 0;
	if (pc_issit(sd) && pc_setstand(sd, false))
		clif_standing(*sd);
	pop_companion_party_say(sd, "I'm back.");
	return false;
}
