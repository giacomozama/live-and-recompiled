#pragma once

#include <cstdint>

// The medgun's beam (src/conker/game/medbeam.cpp): the console's medgun heals with no visual at all; a match draws a
// beam from each healing player to the teammate it heals
namespace medbeam {

// the medgun's heal of target by healer (both player records of [0x84A5F8]), from the heal tick's hit (0x323BA0), which
// every machine runs for every healer (medbeam.cpp); game thread
void Heal(uint32_t healer, uint32_t target);

// the same heal seen as a hit (damage type 0x64) by the hit function 0x2EDDD0 (friendly_fire.cpp): taken only for a
// healer whose heal tick this machine has not seen lately; game thread
void HealHit(uint32_t healer, uint32_t target);

}  // namespace medbeam
