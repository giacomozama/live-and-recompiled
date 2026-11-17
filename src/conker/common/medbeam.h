#pragma once

#include <cstdint>

// The medgun's beam (src/conker/game/medbeam.cpp): the console's medgun heals with no visual at all; a match draws a
// beam from each healing player to the teammate it heals
namespace medbeam {

// the medgun's hit (damage type 0x64) on target, dealt by healer: both player records of [0x84A5F8]. Called from the
// hit function 0x2EDDD0 (friendly_fire.cpp), which every machine runs for every player's hits, and from the local heal
// tick; game thread
void Heal(uint32_t healer, uint32_t target);

}  // namespace medbeam
