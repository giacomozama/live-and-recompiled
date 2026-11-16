#pragma once

// System link and a match's rules (src/conker/game/link.cpp, friendly_fire.cpp)
namespace syslink {

// settings.toml no_friendly_fire: in a match a player's teammates don't hurt them (friendly_fire.cpp). The rule this
// copy plays split screen and hosts with; a copy that joins takes its host's
extern bool g_no_friendly_fire;

// whether the match played now has friendly fire off: this copy's rule outside a system link session and when it
// hosts one, else as its host advertised it at the join; any thread
bool SessionNoFriendlyFire();

// in a system link session (net state 1..5): the rules stay as they are; any thread
bool InSession();

}  // namespace syslink
