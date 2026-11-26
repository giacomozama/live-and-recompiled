#pragma once
// SMAA (http://www.iryoku.com/smaa/), Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia,
// Fernando Navarro and Diego Gutierrez, MIT licensed: the full notice heads kSmaaHlsl in gpu/smaa_hlsl.h.
// Its precomputed lookups, generated from the release's AreaTex and SearchTex: the area texture's red and green, 160 x 560,
// and the search texture's red, 64 x 16, a row after another from the top.
#include <cstdint>

namespace gpu {

constexpr uint32_t kSmaaAreaW = 160, kSmaaAreaH = 560;
constexpr uint32_t kSmaaSearchW = 64, kSmaaSearchH = 16;

inline const uint8_t kSmaaArea[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "smaa_area.bin"
#pragma clang diagnostic pop
};
static_assert(sizeof kSmaaArea == kSmaaAreaW * kSmaaAreaH * 2);

inline const uint8_t kSmaaSearch[] = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "smaa_search.bin"
#pragma clang diagnostic pop
};
static_assert(sizeof kSmaaSearch == kSmaaSearchW * kSmaaSearchH);

}  // namespace gpu
