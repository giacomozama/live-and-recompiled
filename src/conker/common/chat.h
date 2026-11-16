#pragma once

#include <cstdint>

// System link text chat (src/conker/game/chat.cpp): the Chat key (T) opens a line to the team, Shift + it a line to
// everyone, while this copy is in a session; Enter sends it, Escape drops it. What is said shows in a corner of the
// screen.
namespace chat {

// game thread, once a frame (keyboard::Pad, port 0): the session, this copy's player name and team, as chat sends them
void Tick();

// from ui::Message, window thread: the keys and characters typed; true takes the message
bool Message(uint32_t msg, uintptr_t wp, intptr_t lp);

// from ui::Draw, render thread with app::UiLock held: the lines said and the line being typed; the labels of a team
// line and an everyone line, in the menu's language
void Draw(const char* team, const char* all);

// the line is being typed: the keyboard drives no pad (keyboard::Pad). Any thread
bool Typing();

}  // namespace chat
