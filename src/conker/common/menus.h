#pragma once

#include <array>

#include "core/input.h"

// The game's menus (src/conker/game/menus.cpp).
namespace menus {

// settings.toml bright_menu_hints: a multiplayer menu's page hints ("R Configure" over its list, the page under it)
// drawn as solid as the menu's title, rather than the console's faint flickering strips. Read each frame
extern bool g_bright_hints;

// from keyboard::Pad, before gamepad::Pad: while one of the game's menu lists shows, the cursor over an item moves the
// list's cursor onto it (up or down on the pad), a left click picks it (A) and the wheel steps it; in the pub the wheel
// turns from picture to picture (left and right) and a click picks the one in front
void Pad(int port, input::Pad& pad);

// from mouse::Message, window thread: the wheel turned (120 a notch, away from the player positive), which steps a
// shown list's cursor up or down a notch at a time
void Wheel(int turn);

// from single player's pause screen's input, each frame it runs: a click on one of its prompts presses the prompt's
// button
void Paused();

// The screens whose prompts show fixed keys whatever the bindings (PauseKeys), so the prompts have sprites: single
// player's pause screen on its first page (A Quit?, B Resume, Y Options), the quit prompt over it, which keeps those
// prompts on show, and the menu lists outside a match (the frontend's multiplayer menus, A Select and B Back). Game
// thread
constexpr int kMenuList = 3;
int KeyPage();  // 0 the pause screen, 2 the quit prompt, kMenuList a menu list, -1 none of them
bool PauseScreen();  // the pause screen or its quit prompt
// the key a face button's prompt shows on both (0 A, 1 B, 2 X, 3 Y): Q quits, Escape resumes, E opens Options; 0 for
// X, which has none
int PauseKey(int face);
// the keys that press a face button on the page: PauseKey's on the pause screen; on the quit prompt Space and Enter
// answer with the word lit (A); on a menu list Space selects (A) and Escape goes back (B). 0 for none
std::array<int, 2> PauseKeys(int page, int face);
// the page's keys take Start's (Enter, A on the quit prompt), so the Start binding is left out there
bool PauseKeysTakeStart(int page);
// the face button's bindings are left out on the page, its keys PauseKeys': all four on the pause screen and its quit
// prompt, A and B on a menu list
bool FaceFixed(int page, int face);
// the quit prompt's keys back to the pause screen, Q and Escape: A answering No (the game's B there resumes the game)
std::array<int, 2> QuitBackKeys();
// the quit prompt's next A answers with this, whichever word is lit (within half a second). Game thread
void AnswerQuit(bool yes);

}  // namespace menus
