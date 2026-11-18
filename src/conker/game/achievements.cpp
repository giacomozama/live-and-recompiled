// The Xbox Store release's achievements, earned locally. The Store release reports game events from a hook layer in
// its precompiled DLL (achievements/conker_achievements.md) to a host whose unlock rules are encrypted; the rules here
// are this port's, read from the game's own state:
//
// Checkpoints. [0x84B290] points to a bit per checkpoint ("context", the table at 0x59E9A0, see warp.cpp): set once it
// is completed (the chapter start 0x1FB760 sets the bits of every checkpoint before the chosen one, 0x35DE10). A bit
// that goes from 0 to 1 between two frames of a story level is a checkpoint completed in play; the frontend's chapter
// start and a loaded save set theirs in the frontend, and a frame that sets more than kBurst at once is a bulk set
// (a Debug warp), so neither earns anything. The chapter menu's table (0x74CD44: per chapter its scenes, a word of cash
// shown and the scene's checkpoint) gives the chapters below; a chapter is complete when all its scenes' checkpoints are.
//
// Task records. [0x801904] is a list (count the word [0x801900], next at +0x34) of the running checkpoints' records:
// +0 the checkpoint in the low 10 bits, +4 its level, +0xC the size of its variables and +0x14 where they start.
// The DLL's task events read the five whose checkpoints back the "without dying" achievements (0x49 the bullfish chase
// in DogFish, 0x71 Dining Dino, 0x7B Poovarotti Sings, 0x8A the beach assault, 0xB1 The Vertex); here a record's
// appearance (or its level loading) starts an attempt, a death ends it, and the checkpoint's completion earns it.
//
// Stats. 0x32CF50 changes a player's stat (cdecl: the stat op on the stack, si the player, eax the amount) in the table
// 0x8761EC (0x24 bytes a player; 0x32D0D0 reads it): op 4 adds lives (the squirrel tail pickup, 0xF9BC9), 5 takes one
// (a death, 0x2CCE7F), 0xB adds cash (the task scripts, 0x361045). Health is the word [player+0x3AE] of the player's
// record (the table at [0x84A5F8], 0x6D0 bytes each, the first the story's player); the chocolate pickup (object
// handler 0x707F0) raises it.
//
// B pads. The player's controller [player+0x568] holds at +0xA8 the zone it stands in, an index into the zones at
// [0x84B4A0] (0x78 bytes each, count [0x802324]); a zone whose kind byte +0x15 masked with 0xFC is 0x2C is a B pad, its
// id at +0x34. It counts as used while the player's actions show B (([0x7632EC] & 0x200) or [0x7632E4], the DLL's
// test).
//
// Dumbots ([0x863D04] == 1). The match's end test 0x241760 sets [0x776541] once it is over and the winning team in the
// signed byte [0x776BD8] (3 none); a player's team is the int at [[record+0x238]]. The campaign ("Chapter X",
// [0x875670], made by 0x1C74F0) has its missions at +4..+8 (0x18 bytes each), the one playing at +0x20 (-1 none) and
// the human side at +0x24; 0x227EA0 counts a match won when the victory condition [0x777EC8] is set and the winner is
// that side. A player's class is the name of its full model ([[record+0x50C]], the model table 0x6A58F0):
// Multi\Combat<Squirrel|Tediz>Multi[WWII]<class>. A vehicle the player drives: record+0x135 its record's index + 1
// (records 16..127, kind byte 0x35 in [0x84A5FC]), the driver's seat [[vehicle+0x570]+0x1F0]; one made at a terminal
// waits on its pad, [object+0xCA0] 0 and its countdown [object+0xC9C] from 8.5 s (bots.cpp).
//
// The unlocks and the counts toward them are kept in DataDir/achievements.txt. XBE_ACH_TRACE=1 logs what the rules see
// (checkpoints completed, stat changes, attempts started and ended, B pads, the Dumbots results).
#include "common/achievements.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include "common/warp.h"

namespace achievements {

namespace {

using kernel::G;
using kernel::R16;
using kernel::R32;

enum Id {
    kHairOfTheDog, kStoneColdGatekeeper, kGoneWithTheWind, kHayNow, kBatOutOfHell, kFlushed, kUgaBuggin, kGraveyardShift,
    kSavingPrivateConker, kNuttinButNetWorth, kFarmingImplement, kCaviar, kKnuckleSandwich, kStakeYourClaim,
    kNightOfTheLivingNut, kCornOffTheCob, kElephant, kGetAwayFromHer, kHeinrichManeuver, kSweetTooth, kContext, kHoney,
    kCashGrabber, kSayCheese, kFlammable, kLadyOfTheCogs, kTailHunter, kMillionaire, kRareBreed, kBadFeeling, kSpotless,
    kBullRush, kBiggerBoat, kJurassicBark, kDodgeThis, kFilthyRich, kFewGoodSquirrels, kJackOfAllTirades, kTanks,
    kBridgeTooFur, kBadFurDayEnds,
};

constexpr int kCashGoal = 1000, kTailGoal = 5, kPadGoal = 10, kClassGoal = 6;

struct Entry {
    const char* key;  // in achievements.txt; never changes
    Info info;
};
// clang-format off
constexpr Entry kEntries[kCount] = {
    {"hair_of_the_dog", {"Hair of the Dog", "Complete the Hungover chapter.", 0, false, true}},
    {"stone_cold_gatekeeper", {"Stone Cold Gatekeeper", "Get past the Gargoyle and reopen the way forward in the Hungover chapter.", 0, false, true}},
    {"gone_with_the_wind", {"Gone with the Wind", "Complete the Windy chapter for the first time.", 0, false, true}},
    {"hay_now", {"Hay Now, You're a Rock Star", "Complete the Barn Boys chapter.", 0, false, true}},
    {"bat_out_of_hell", {"Bat Out of Hell", "Complete the Bats Tower chapter.", 0, false, true}},
    {"flushed_with_success", {"Flushed with Success", "Complete the Sloprano chapter.", 0, false, true}},
    {"uga_buggin_out", {"Uga Buggin' Out", "Complete the Uga Buga chapter.", 0, false, true}},
    {"graveyard_shift", {"Graveyard Shift", "Complete the Spooky chapter.", 0, false, true}},
    {"saving_private_conker", {"Saving Private Conker", "Complete the It's War chapter.", 0, false, true}},
    {"nuttin_but_net_worth", {"Nuttin' But Net Worth", "Complete the Heist chapter.", 0, false, true}},
    {"farming_implement_of_doom", {"Farming Implement of Doom", "Defeat the Haybot in the Barn Boys chapter.", 0, false, true}},
    {"have_some_more_caviar", {"Have Some More Caviar", "Defeat the Great Mighty Poo in the Sloprano chapter.", 0, false, true}},
    {"knuckle_sandwich", {"Knuckle Sandwich", "Defeat Buga the Knut in the Uga Buga chapter.", 0, false, true}},
    {"stake_your_claim", {"Stake Your Claim", "Defeat Count Batula in the Spooky chapter.", 0, false, true}},
    {"night_of_the_living_nut", {"Night of the Living Nut", "Fight through the undead squirrels in the Spooky chapter.", 0, false, true}},
    {"corn_off_the_cob", {"Corn Off the Cob", "Toss 3 sweetcorn in the pool to summon the Great Mighty Poo.", 0, false, true}},
    {"im_an_elephant_actually", {"I'm an Elephant, Actually", "Bluff your way past the Weasel Guards by pretending to be an elephant.", 0, false, true}},
    {"get_away_from_her", {"Get Away From Her", "Defeat The Experiment in the It's War chapter.", 0, false, true}},
    {"heinrich_maneuver", {"Heinrich Maneuver", "Defeat Heinrich in the final boss fight.", 0, false, true}},
    {"sweet_tooth", {"Sweet Tooth", "Use chocolate to restore health for the first time.", 0, false, false}},
    {"context_is_everything", {"Context Is Everything", "Use 10 different context-sensitive B Button pads", kPadGoal, false, false}},
    {"honey_im_home", {"Honey, I'm Home", "Return Mrs. Bee's stolen hive to her for the first time.", 0, false, true}},
    {"cash_grabber", {"Cash Grabber", "Collect $1,000 in cash across the single-player story.", kCashGoal, false, false}},
    {"say_cheese", {"Say Cheese", "In Barn Boys, give Marvin 3 pieces of cheese and rid Jack of him.", 0, false, true}},
    {"looks_kinda_flammable", {"Looks Kinda Flammable", "Get drunk and put out the Fire Imps in the Bats Tower chapter.", 0, false, true}},
    {"lady_of_the_cogs", {"Lady of the Cogs", "Complete Lady Cogs' machinery puzzle in the Bats Tower chapter.", 0, false, true}},
    {"tail_hunter", {"Tail Hunter", "Collect 5 squirrel tails across single player", kTailGoal, false, false}},
    {"millionaire_squirrel", {"Millionaire Squirrel", "Empty the Feral Reserve vault and become a millionaire in the Heist chapter.", 0, false, true}},
    {"a_rare_breed", {"A Rare Breed", "See the Programmer give Conker a weapons rack for the Heinrich finale.", 0, false, true}},
    {"ive_got_a_bad_feeling", {"I've Got a Bad Feeling", "Survive the beach assault landing in the It's War chapter without dying.", 0, false, true}},
    {"spotless", {"Spotless", "Defeat the Great Mighty Poo without taking damage.", 0, false, true}},
    {"bull_rush", {"Bull Rush", "In the Windy chapter, ride the bull and charge all 3 cows at the grate.", 0, false, true}},
    {"gonna_need_a_bigger_boat", {"Gonna Need a Bigger Boat", "Escape Brute the Bullfish without dying.", 0, false, true}},
    {"jurassic_bark", {"Jurassic Bark", "In Raptor Food, defeat all infantry without being dismounted from Fangy.", 0, false, true}},
    {"dodge_this", {"Dodge This", "Complete Enter the Vertex in the Heist chapter without dying.", 0, false, true}},
    {"filthy_rich", {"Filthy Rich", "Collect $2,110, pay Mr. Barrel, then access Spooky.", 0, false, true}},
    {"a_few_good_squirrels", {"A Few Good Squirrels", "Win a multiplayer match against Dumbots.", 0, true, false}},
    {"jack_of_all_tirades", {"Jack of All Tirades", "Play a Dumbots match as each of the six character classes.", kClassGoal, true, false}},
    {"tanks_very_much", {"Tanks Very Much", "Spawn and use a Mobile Units vehicle in Dumbots mode.", 0, true, false}},
    {"a_bridge_too_fur", {"A Bridge Too Fur", "Complete Chapter X against Dumbots.", 0, true, false}},
    {"the_bad_fur_day_ends", {"The Bad Fur Day Ends", "Complete the single-player story.", 0, false, false}},
};
// clang-format on

// the chapter menu's scenes (0x74CD44), by their checkpoints
struct Chapter {
    Id id;
    std::vector<int> scenes;
};
const Chapter kChapters[] = {
    {kHairOfTheDog, {0, 5, 236}},
    {kGoneWithTheWind, {10, 117, 132, 133, 120, 28, 11, 39}},
    {kBatOutOfHell, {69, 85, 81, 70, 75, 77, 134, 135, 73}},
    {kHayNow, {45, 60, 54, 61, 65, 67, 68, 59}},
    {kFlushed, {122, 123, 124, 126}},
    {kUgaBuggin, {87, 102, 94, 96, 107, 98, 130, 113, 114}},
    {kGraveyardShift, {166, 169, 171, 173}},
    {kSavingPrivateConker, {197, 158, 159, 138, 139, 164, 147, 154, 155, 161, 162, 163, 141}},
    {kNuttinButNetWorth, {42, 177, 182, 199}},
};

// a checkpoint whose completion earns one
struct Moment {
    int checkpoint;
    Id id;
};
constexpr Moment kMoments[] = {
    {236, kStoneColdGatekeeper},  // Training - The Baddie 2: the Gargoyle's gate
    {67, kFarmingImplement},      // Terminator Haystack
    {124, kCaviar},               // Poovarotti Flush
    {114, kKnuckleSandwich},      // Bugga the Knut
    {169, kStakeYourClaim},       // Count Batula
    {171, kNightOfTheLivingNut},  // Zombie Fest
    {122, kCornOffTheCob},        // Poo Sweetcorn Teeth
    {126, kElephant},             // WEASEL POO SCAM (U-Bend)
    {162, kGetAwayFromHer},       // War Boss
    {182, kHeinrichManeuver},     // Fight The Alien
    {10, kHoney},                 // The Queen's lost her Home!
    {45, kSayCheese},             // Feed Marvin
    {134, kFlammable},            // Fire Imp Frenzy
    {81, kLadyOfTheCogs},         // Cog Needs His Women
    {178, kMillionaire},          // Bank Vault
    {133, kBullRush},             // Bull Shitters
    {199, kBadFurDayEnds},        // End of Game Cutscene and Credits
};

// an attempt without dying: the checkpoint's record in its level
struct Attempt {
    int checkpoint, level;
    Id id;
    bool armed = false, present = false;
};
Attempt g_attempts[] = {
    {73, 7, kBiggerBoat},     // DogFish: the bullfish chase
    {138, 4, kBadFeeling},    // BeachRunup: Beach Assault Bloodbath
    {177, 54, kDodgeThis},    // Bank: The Vertex (TM)
    {113, 20, kJurassicBark}, // DinoArena: Dining Dino, also not dismounted from Fangy (below)
};

constexpr uint32_t kLevel = 0x7FA1F8, kLoadState = 0x7819D6, kFixedStep = 0x7819D5;
constexpr uint32_t kRecords = 0x84A5F8, kRecordSize = 0x6D0, kKinds = 0x84A5FC;
constexpr uint32_t kHealth = 0x3AE;
constexpr uint32_t kCheckpointBits = 0x84B290;
constexpr int kCheckpoints = 238, kBurst = 16;
constexpr uint32_t kTaskCount = 0x801900, kTaskList = 0x801904;
constexpr uint32_t kZones = 0x84B4A0, kZoneCount = 0x802324, kZoneSize = 0x78;
constexpr uint32_t kCutscenePlaying = 0x76F0E0;
constexpr uint32_t kMode = 0x863D04, kMatchOver = 0x776541, kWinTeam = 0x776BD8, kVictory = 0x777EC8;
constexpr uint32_t kCampaign = 0x875670;
constexpr uint32_t kModelNames = 0x6A58F0;

const char* const kClasses[kClassGoal] = {"grunt", "sneaker", "sharpshooter", "demolisher", "poisoner", "skyjockey"};

bool Trace() {
    static const bool on = std::getenv("XBE_ACH_TRACE") != nullptr;
    return on;
}

// ---- the record kept on disk; g_lock guards it (the menu and the toast read it from the render thread)

std::mutex g_lock;
bool g_loaded = false;
std::array<int64_t, kCount> g_when{};  // unix seconds, 0 locked
int g_cash = 0, g_tails = 0;
std::set<uint32_t> g_pads;
uint32_t g_classes = 0;
std::deque<int> g_toasts;

std::filesystem::path File() { return app::DataDir() / "achievements.txt"; }

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(File());
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string word;
        s >> word;
        if (word == "unlocked") {
            std::string key;
            int64_t when = 0;
            s >> key >> when;
            for (int i = 0; i < kCount; i++)
                if (key == kEntries[i].key) g_when[i] = when > 0 ? when : 1;
        } else if (word == "cash") {
            s >> g_cash;
        } else if (word == "tails") {
            s >> g_tails;
        } else if (word == "pad") {
            uint32_t id = 0;
            if (s >> id) g_pads.insert(id);
        } else if (word == "classes") {
            s >> g_classes;
        }
    }
}

void SaveLocked() {
    std::ostringstream out;
    out << "# Conker achievements, earned in this copy only\n";
    for (int i = 0; i < kCount; i++)
        if (g_when[i]) out << "unlocked " << kEntries[i].key << ' ' << g_when[i] << '\n';
    out << "cash " << g_cash << '\n' << "tails " << g_tails << '\n' << "classes " << g_classes << '\n';
    for (uint32_t id : g_pads) out << "pad " << id << '\n';
    const std::filesystem::path path = File(), tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return LOGW("achievements: can't write %s", tmp.string().c_str());
        f << out.str();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) LOGW("achievements: can't write %s: %s", path.string().c_str(), ec.message().c_str());
}

void UnlockLocked(Id id, const char* why) {
    LoadLocked();
    if (g_when[id]) return;
    g_when[id] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    g_toasts.push_back(id);
    SaveLocked();
    LOGI("achievements: %s (%s)", kEntries[id].info.name, why);
    if (Trace()) LOGR("achievements: unlocked %s (%s)", kEntries[id].info.name, why);
}

void Unlock(Id id, const char* why) {
    std::lock_guard<std::mutex> l(g_lock);
    UnlockLocked(id, why);
}

bool Unlocked(Id id) {
    std::lock_guard<std::mutex> l(g_lock);
    LoadLocked();
    return g_when[id] != 0;
}

// ---- the game's state

bool Valid(uint32_t va) { return va >= 0x10000 && va < 0xF0000000u; }

uint32_t Player() { return R32(kRecords); }  // the first record: the story's player, the primary player in a match

int Health() {
    const uint32_t p = Player();
    return Valid(p) ? int(G<int16_t>(p + kHealth)) : 0;
}

// the story is playing: a story level, not loading, not a match
bool StoryLevel() {
    const uint32_t level = R32(kLevel);
    return level < uint32_t(warp::kLevels) && !warp::Frontend() && !warp::Multiplayer();
}

bool Story() { return StoryLevel() && !G<uint8_t>(kLoadState) && !G<uint8_t>(kFixedStep); }

bool Dumbots() { return warp::Multiplayer() && R32(kMode) == 1; }

std::string CheckpointName(int cp) {
    const std::vector<warp::Checkpoint>& list = warp::Checkpoints();
    return cp >= 0 && cp < int(list.size()) ? list[cp].name : std::string();
}

bool Completed(const std::array<uint8_t, (kCheckpoints + 7) / 8>& bits, int cp) { return bits[cp >> 3] >> (cp & 7) & 1; }

bool ReadBits(std::array<uint8_t, (kCheckpoints + 7) / 8>& bits) {
    const uint32_t at = R32(kCheckpointBits);
    if (!Valid(at)) return false;
    for (size_t i = 0; i < bits.size(); i++) bits[i] = G<uint8_t>(at + uint32_t(i));
    return true;
}

// the running checkpoint's task record, 0 none
uint32_t TaskRecord(int cp, int level) {
    const int n = R16(kTaskCount);
    uint32_t rec = R32(kTaskList);
    for (int i = 0; i < n && i < 512 && Valid(rec); i++, rec = R32(rec + 0x34))
        if ((R32(rec) & 0x3FF) == uint32_t(cp) && R32(rec + 4) == uint32_t(level)) return rec;
    return 0;
}

// the story's ending: checkpoint 199's record with its first variable 2 (the DLL's storyCompletion)
bool EndingDone() {
    const uint32_t rec = TaskRecord(199, 64);
    return rec && R32(rec + 0xC) >= 4 && R32(rec + R32(rec + 0x14)) == 2;
}

// ---- per frame state, game thread

std::array<uint8_t, (kCheckpoints + 7) / 8> g_bits{};
bool g_bits_valid = false;
uint32_t g_last_level = 0xFFFFFFFF;
bool g_was_loading = true;
int g_last_health = 0;
uint32_t g_last_player = 0;
bool g_spotless = false;         // in PooBoss with no damage taken since the fight began
bool g_mounted = false, g_dismounted = false;  // Fangy, during the Dining Dino attempt
bool g_match_running = false;    // a Dumbots match seen still playing, its end not yet looked at
int g_campaign_mission = -1;

void Death(const char* how) {
    for (Attempt& a : g_attempts)
        if (a.armed) {
            a.armed = false;
            if (Trace()) LOGR("achievements: %s attempt ended (%s)", kEntries[a.id].info.name, how);
        }
    if (g_spotless && Trace()) LOGR("achievements: Spotless attempt ended (%s)", how);
    g_spotless = false;
}

void Arm(Attempt& a, const char* why) {
    if (a.armed) return;
    a.armed = true;
    if (a.id == kJurassicBark) g_mounted = g_dismounted = false;
    if (Trace()) LOGR("achievements: %s attempt started (%s)", kEntries[a.id].info.name, why);
}

void CheckpointCompleted(int cp, const std::array<uint8_t, (kCheckpoints + 7) / 8>& bits) {
    if (Trace()) LOGR("achievements: checkpoint %d \"%s\" completed in level %u", cp, CheckpointName(cp).c_str(), R32(kLevel));
    for (const Moment& m : kMoments)
        if (m.checkpoint == cp) Unlock(m.id, "checkpoint");
    for (const Chapter& c : kChapters) {
        if (std::find(c.scenes.begin(), c.scenes.end(), cp) == c.scenes.end()) continue;
        bool all = true;
        for (int s : c.scenes) all = all && Completed(bits, s);
        if (all) Unlock(c.id, "chapter's scenes completed");
    }
    for (Attempt& a : g_attempts) {
        if (a.checkpoint != cp) continue;
        if (a.armed && !(a.id == kJurassicBark && g_dismounted)) Unlock(a.id, "completed without dying");
        else if (Trace())
            LOGR("achievements: %s not earned (%s)", kEntries[a.id].info.name, a.armed ? "dismounted" : "no attempt without dying");
        a.armed = false;
    }
    if (cp == 122 && R32(kLevel) == 53) {
        g_spotless = true;  // the sweetcorn's in: the fight begins
        if (Trace()) LOGR("achievements: Spotless attempt started (the fight)");
    }
    if (cp == 124) {
        if (g_spotless) Unlock(kSpotless, "no damage in the fight");
        else if (Trace()) LOGR("achievements: Spotless not earned");
        g_spotless = false;
    }
}

void PollStory() {
    const uint32_t level = R32(kLevel);
    const bool loading = G<uint8_t>(kLoadState) != 0;
    const bool story = Story();

    // checkpoints completed since the last frame of play; a level load in between keeps the frame before it, so one
    // completed as its level changes still counts. Out of the story (the frontend, a match) there is nothing to compare
    std::array<uint8_t, (kCheckpoints + 7) / 8> bits{};
    const bool have = ReadBits(bits);
    if (!have || !StoryLevel()) {
        g_bits_valid = false;
    } else if (!g_bits_valid) {
        g_bits = bits, g_bits_valid = true;
    } else if (story) {
        std::vector<int> fresh;
        for (int cp = 0; cp < kCheckpoints; cp++)
            if (Completed(bits, cp) && !Completed(g_bits, cp)) fresh.push_back(cp);
        g_bits = bits;
        if (int(fresh.size()) > kBurst) {
            if (Trace()) LOGR("achievements: %zu checkpoints completed at once: a bulk set, not play", fresh.size());
        } else {
            for (int cp : fresh) CheckpointCompleted(cp, bits);
        }
    }

    // a story level just loaded: attempts in it start again, as does Spotless
    const bool arrived = story && (g_was_loading || level != g_last_level);
    g_was_loading = loading || !story;
    g_last_level = level;
    if (arrived) {
        for (Attempt& a : g_attempts)
            if (int(level) == a.level && have && !Completed(bits, a.checkpoint)) Arm(a, "level loaded");
        g_spotless = level == 53 && have && Completed(bits, 122) && !Completed(bits, 124);
        if (g_spotless && Trace()) LOGR("achievements: Spotless attempt started (level loaded)");
        if (level == 65 && have && Completed(bits, 39)) Unlock(kFilthyRich, "Spooky reached with Mr. Barrel paid");
    }
    if (!story) {
        g_last_player = 0;
        return;
    }

    // health: a death ends the attempts, damage in PooBoss ends Spotless
    const uint32_t player = Player();
    const int health = Health();
    if (player == g_last_player) {
        if (health <= 0 && g_last_health > 0) Death("health 0");
        if (health < g_last_health && level == 53 && g_spotless) {
            g_spotless = false;
            if (Trace()) LOGR("achievements: Spotless attempt ended (damage %d -> %d)", g_last_health, health);
        }
    }
    g_last_player = player, g_last_health = health;

    // the attempts' records appearing
    for (Attempt& a : g_attempts) {
        if (int(level) != a.level) {
            a.present = false;
            continue;
        }
        const bool present = TaskRecord(a.checkpoint, a.level) != 0;
        if (present && !a.present && !(have && Completed(bits, a.checkpoint))) Arm(a, "task began");
        a.present = present;
    }

    // Fangy: mounted is the player in a vehicle (record+0x135); off him outside a cutscene fails Jurassic Bark
    if (level == 20 && Valid(player)) {
        const bool mounted = G<uint8_t>(player + 0x135) != 0;
        if (mounted && !g_mounted && Trace()) LOGR("achievements: on Fangy");
        if (!mounted && g_mounted && !G<uint8_t>(kCutscenePlaying)) {
            g_dismounted = true;
            if (Trace()) LOGR("achievements: off Fangy");
        }
        g_mounted = mounted;
    }

    // B pads
    if (Valid(player)) {
        const uint32_t ctl = R32(player + 0x568), zones = R32(kZones), count = R32(kZoneCount);
        if (Valid(ctl) && Valid(zones)) {
            const uint32_t b = G<uint8_t>(ctl + 0xA8);
            if (b && b < count && (G<uint8_t>(zones + b * kZoneSize + 0x15) & 0xFC) == 0x2C &&
                ((R32(0x7632EC) & 0x200) || R32(0x7632E4))) {
                const uint32_t id = R32(zones + b * kZoneSize + 0x34);
                std::lock_guard<std::mutex> l(g_lock);
                LoadLocked();
                if (g_pads.insert(id).second) {
                    if (Trace()) LOGR("achievements: B pad %u used (zone %u), %zu different", id, b, g_pads.size());
                    if (int(g_pads.size()) >= kPadGoal) UnlockLocked(kContext, "B pads");
                    else SaveLocked();
                }
            }
        }
    }

    // the Programmer's weapons rack: EndArena's cutscene 19 played to its end (the DLL's programmerRackReceived)
    if (level == 0x40 && G<uint8_t>(0x76F0DC) == 0x13 && G<uint8_t>(kCutscenePlaying) == 1 && R32(0x76F0E8) &&
        R32(0x76F0F8) >= R32(0x76F0E8))
        Unlock(kRareBreed, "the Programmer's cutscene");

    if (level == 64 && EndingDone()) Unlock(kBadFurDayEnds, "the ending's task");
}

// the class of a player's full model, -1 none
int ClassOf(uint32_t player) {
    const uint32_t list = R32(player + 0x50C);
    if (!Valid(list)) return -1;
    const uint32_t model = R32(list);
    if (model >= 0x205) return -1;
    const uint32_t at = R32(kModelNames + 4 * model);
    if (!Valid(at)) return -1;
    std::string name;
    for (uint32_t i = 0; i < 96; i++) {
        const char c = G<char>(at + i);
        if (!c) break;
        name += char(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char* side : {"combatsquirrelmulti", "combattedizmulti"}) {
        const size_t p = name.find(side);
        if (p == std::string::npos) continue;
        std::string cls = name.substr(p + std::strlen(side));
        if (cls.rfind("wwii", 0) == 0) cls = cls.substr(4);
        for (int i = 0; i < kClassGoal; i++)
            if (cls == kClasses[i]) return i;
    }
    return -1;
}

void PollDumbots() {
    if (!Dumbots()) {
        g_match_running = false;
        return;
    }
    const uint32_t player = Player();
    if (!Valid(player)) return;
    const uint32_t campaign = R32(kCampaign);
    const bool over = G<uint8_t>(kMatchOver) != 0;
    if (!over) {
        if (!g_match_running && Trace()) LOGR("achievements: Dumbots match playing");
        g_match_running = true;
        g_campaign_mission = Valid(campaign) ? int(R32(campaign + 0x20)) : -1;

        // the class played
        if (const int cls = ClassOf(player); cls >= 0) {
            std::lock_guard<std::mutex> l(g_lock);
            LoadLocked();
            if (!(g_classes >> cls & 1)) {
                g_classes |= 1u << cls;
                const int n = std::popcount(g_classes);
                if (Trace()) LOGR("achievements: played as %s, %d classes", kClasses[cls], n);
                if (n >= kClassGoal) UnlockLocked(kJackOfAllTirades, "every class");
                else SaveLocked();
            }
        }

        // a vehicle made at a terminal, driven by the player while it still waits on its pad
        const uint32_t sub = G<uint8_t>(player + 0x135), table = R32(kRecords), kinds = R32(kKinds);
        if (sub >= 0x11 && sub <= 0x80 && Valid(table) && Valid(kinds) && G<uint8_t>(kinds + sub - 1) == 0x35) {
            const uint32_t object = R32(table + (sub - 1) * kRecordSize + 0x570);
            if (Valid(object) && R32(object + 0x1F0) == player && !G<uint8_t>(object + 0xCA0) &&
                G<float>(object + 0xC9C) > 0.0f)
                Unlock(kTanks, "drove a terminal's vehicle off its pad");
        }
        return;
    }
    if (!g_match_running) return;  // its end already looked at, or the match was over when this saw it first
    g_match_running = false;
    const int winner = G<int8_t>(kWinTeam);
    const uint32_t team_at = R32(player + 0x238);
    const int team = Valid(team_at) ? int(R32(team_at)) : -1;
    int side = team, missions = 0;
    if (Valid(campaign) && g_campaign_mission >= 0) {
        side = int(R32(campaign + 0x24));
        const uint32_t begin = R32(campaign + 4), end = R32(campaign + 8);
        missions = begin && end > begin ? int((end - begin) / 0x18) : 0;
    }
    if (Trace())
        LOGR("achievements: Dumbots match over: winner %d, the player's team %d, campaign mission %d of %d (side %d), "
             "victory condition %u",
             winner, team, g_campaign_mission, missions, side, R32(kVictory));
    if ((winner == 0 || winner == 1) && (winner == team || winner == side)) {
        Unlock(kFewGoodSquirrels, "won a Dumbots match");
        if (missions > 0 && g_campaign_mission == missions - 1 && winner == side && R32(kVictory))
            Unlock(kBridgeTooFur, "won the campaign's last mission");
    }
}

}  // namespace

const Info& Get(int index) { return kEntries[std::clamp(index, 0, kCount - 1)].info; }

State StateOf(int index) {
    std::lock_guard<std::mutex> l(g_lock);
    LoadLocked();
    index = std::clamp(index, 0, kCount - 1);
    State s{g_when[index] != 0, g_when[index], 0};
    switch (index) {
        case kCashGrabber: s.progress = std::min(g_cash, kCashGoal); break;
        case kTailHunter: s.progress = std::min(g_tails, kTailGoal); break;
        case kContext: s.progress = std::min(int(g_pads.size()), kPadGoal); break;
        case kJackOfAllTirades: s.progress = std::popcount(g_classes); break;
        default: break;
    }
    if (s.unlocked && kEntries[index].info.goal) s.progress = kEntries[index].info.goal;
    return s;
}

int UnlockedCount() {
    std::lock_guard<std::mutex> l(g_lock);
    LoadLocked();
    return int(std::count_if(g_when.begin(), g_when.end(), [](int64_t w) { return w != 0; }));
}

void Reset() {
    std::lock_guard<std::mutex> l(g_lock);
    LoadLocked();
    g_when.fill(0);
    g_cash = g_tails = 0;
    g_pads.clear();
    g_classes = 0;
    g_toasts.clear();
    SaveLocked();
    LOGI("achievements: reset");
}

int NextToast() {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_toasts.empty()) return -1;
    const int id = g_toasts.front();
    g_toasts.pop_front();
    return id;
}

void Poll(X86Context& ctx) {
    (void)ctx;
    PollStory();
    PollDumbots();
}

}  // namespace achievements

using namespace achievements;

// 0x32CF50: a player's stat changes (the op on the stack, si the player, eax the amount). The story's player gaining
// lives (a squirrel tail) or cash counts toward Tail Hunter and Cash Grabber; losing a life is a death
X86_EXTERN_FUNC(__imp__sub_0032CF50);
X86_FUNC(sub_0032CF50) {
    const uint32_t op = kernel::R32(ctx.esp + 4);
    const int player = int16_t(ctx.esi & 0xFFFF);
    const int amount = int(ctx.eax);
    __imp__sub_0032CF50(ctx, base);
    if (player != 0 || !Story()) return;
    if (op == 4 || op == 0xB) {
        if (amount <= 0) return;
        std::lock_guard<std::mutex> l(g_lock);
        LoadLocked();
        int& total = op == 4 ? g_tails : g_cash;
        total += amount;
        if (Trace()) LOGR("achievements: %s +%d, %d in all", op == 4 ? "lives" : "cash", amount, total);
        if (op == 4 && total >= kTailGoal) UnlockLocked(kTailHunter, "tails collected");
        else if (op == 0xB && total >= kCashGoal) UnlockLocked(kCashGrabber, "cash collected");
        else SaveLocked();
    } else if (op == 5) {
        Death("a life lost");
    }
}

// 0x707F0: the chocolate pickup's handler (edi the object). The player's health rising over it is chocolate eaten
X86_EXTERN_FUNC(__imp__sub_000707F0);
X86_FUNC(sub_000707F0) {
    const bool story = Story();
    const int before = story ? Health() : 0;
    __imp__sub_000707F0(ctx, base);
    if (!story || before <= 0) return;
    const int after = Health();
    if (after > before) {
        if (Trace()) LOGR("achievements: chocolate, health %d -> %d", before, after);
        if (!Unlocked(kSweetTooth)) Unlock(kSweetTooth, "chocolate");
    }
}
