#pragma once

#include <cstdint>

// The game's button prompts as keys: while a player last used the keyboard or the mouse (input::LastUsed), a face
// button's icon is drawn as the key its action is bound to (src/conker/common/prompts.cpp).
namespace prompts {

// the image to draw in place of a face button's icon (0 A, 1 B, 2 X, 3 Y) for the player on the port: a guest texture
// of the first key bound to that button which has a sprite, while the player last used the keyboard or mouse; 0
// keeps the icon (a controller in use, or no sprite for the keys bound). Game thread
uint32_t KeyIcon(int port, int face);

// the same for one key (a virtual-key code), whatever it is bound to: a prompt that takes a fixed key; 0 keeps the
// icon
uint32_t KeySprite(int port, int vk);

}  // namespace prompts
