#pragma once
// The on-screen controller (touch.cpp): Android's, drawn over the game while no controller drives the first player
// and the menu is shut. Elsewhere it is never shown and these do nothing.
namespace input {
struct Pad;
}

namespace touch {

void Init();  // window thread, after input::Init
// window thread, each SDL event: the fingers. True when the menu button was tapped, for the caller to open the menu
bool Handle(const void* sdl_event);
// render thread, inside the overlay's ImGui frame
void Draw();
// any thread: the controller's report into the first player's, when it is shown
void Merge(int port, input::Pad& pad);

}  // namespace touch
