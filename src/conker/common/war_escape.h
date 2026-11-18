#pragma once
#include <cstdint>

namespace war_escape {
// It's War's exit (war_escape.cpp): the hit function 0x2EDDD0 leaves actor untouched while the departure is committed
bool SparesHit(uint8_t* base, uint32_t actor);
}
