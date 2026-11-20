// the window icon where an executable carries none (Linux: the game's SDL window, Setup's FLTK one): 256x256 straight
// RGBA, rows top down, made by tools/icon.py from tools/icon.svg. Windows takes the exe's own (app/icon/icon.rc)
#pragma once

#include <cstdint>

namespace app {

inline constexpr int kIconSize = 256;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
inline constexpr uint8_t kIcon[] = {
#embed "icon/icon.rgba"
};
#pragma clang diagnostic pop
static_assert(sizeof kIcon == kIconSize * kIconSize * 4);

}  // namespace app
