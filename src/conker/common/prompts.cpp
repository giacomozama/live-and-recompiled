// Key sprites for the game's button prompts: PNGs embedded from src/conker/common/prompts/, one for every key a binding
// can name, by tools/prompt_sprites.py, which writes sprites.inc there: the PNGs embedded and the table of them by
// keyboard::KeyName. Each is made a guest texture the first time a prompt asks for it: A8R8G8B8, swizzled, on a square
// of a power of two with the sprite centred in it, as the icons it stands in for are square.
#include "common/prompts.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include "common/keyboard.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"

#if __has_include(<SDL3/SDL.h>) && !defined(XBE_HEADLESS)
#include <SDL3/SDL.h>
#define PROMPTS_SPRITES 1
#endif

namespace prompts {

#ifdef PROMPTS_SPRITES

namespace {

struct Sprite {
    std::string_view key;  // its keyboard::KeyName
    const unsigned char* png;
    size_t size;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#include "prompts/sprites.inc"
#pragma clang diagnostic pop

// the bits of v on the even bits: a coordinate's part of a square's Morton order
uint32_t Spread(uint32_t v) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 16; i++) r |= (v >> i & 1) << (2 * i);
    return r;
}

// the sprite as a guest texture, 0 if its PNG does not decode
uint32_t Texture(const Sprite& s) {
    SDL_Surface* loaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(s.png, s.size), true);
    if (!loaded) {
        LOGR("prompts: the sprite of %.*s does not decode: %s", int(s.key.size()), s.key.data(), SDL_GetError());
        return 0;
    }
    SDL_Surface* argb = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(loaded);
    if (!argb) return 0;
    const uint32_t w = uint32_t(argb->w), h = uint32_t(argb->h);
    uint32_t log2 = 0;
    while ((1u << log2) < std::max(w, h) && log2 < 11) log2++;
    const uint32_t side = 1u << log2, bytes = side * side * 4;
    const uint32_t data = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 0x1000);
    if (!data) {
        SDL_DestroySurface(argb);
        return 0;
    }
    uint8_t* texels = mem::Base() + data;
    std::memset(texels, 0, bytes);
    const uint32_t ox = (side - std::min(w, side)) / 2, oy = (side - std::min(h, side)) / 2;
    for (uint32_t y = 0; y < h && y < side; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(argb->pixels) + size_t(y) * size_t(argb->pitch);
        for (uint32_t x = 0; x < w && x < side; x++)
            std::memcpy(texels + size_t(Spread(x + ox) | Spread(y + oy) << 1) * 4, row + x * 4, 4);
    }
    SDL_DestroySurface(argb);
    // D3DPixelContainer: Common (a texture, one reference), Data (physical), Lock, Format, Size (0: swizzled)
    constexpr uint32_t kTextureCommon = 0x00040001, kA8R8G8B8 = 0x06;
    const uint32_t header = mem::AllocSystem(20);
    uint32_t* hdr = reinterpret_cast<uint32_t*>(mem::Base() + header);
    hdr[0] = kTextureCommon;
    hdr[1] = mem::PhysicalFromVirtual(data);
    hdr[2] = 0;
    // DMA channel A, two dimensions, the colour format, one level, log2 sizes
    hdr[3] = 0x01 | 2u << 4 | kA8R8G8B8 << 8 | 1u << 16 | log2 << 20 | log2 << 24;
    hdr[4] = 0;
    LOGI("prompts: %.*s, %ux%u on %u", int(s.key.size()), s.key.data(), w, h, side);
    return header;
}

// the sprite of the key as a guest texture, 0 if there is none
uint32_t KeyTexture(int vk) {
    static std::unordered_map<int, uint32_t> made;  // by key, game thread
    if (const auto it = made.find(vk); it != made.end()) return it->second;
    uint32_t& tex = made[vk];
    const char* key = keyboard::KeyName(vk);
    if (!vk || !key) return tex = 0;
    for (const Sprite& s : kSprites)
        if (s.key == key) return tex = Texture(s);
    return tex = 0;
}

}  // namespace

uint32_t KeySprite(int port, int vk) {
    if (port < 0 || port >= input::kPorts || input::LastUsed(port) != input::Source::Keyboard) return 0;
    return KeyTexture(vk);
}

uint32_t ActionIcon(int port, int action) {
    if (action < 0 || action >= keyboard::kActions || port < 0 || port >= input::kPorts) return 0;
    if (input::LastUsed(port) != input::Source::Keyboard) return 0;
    for (int vk : keyboard::g_keys[keyboard::ActiveMode()][port][action])
        if (const uint32_t tex = KeyTexture(vk)) return tex;
    return 0;
}

uint32_t KeyIcon(int port, int face) { return face < 0 || face > 3 ? 0 : ActionIcon(port, keyboard::kA + face); }

#else

uint32_t ActionIcon(int, int) { return 0; }
uint32_t KeyIcon(int, int) { return 0; }
uint32_t KeySprite(int, int) { return 0; }

#endif

}  // namespace prompts
