// The window module's side of Setup (Linux, setup.h GuiApi): gui.cpp's calls into the build go back to the Setup that
// loaded it, and setup_gui_run, its one export (gui.map), opens the window.
#include "setup.h"

namespace setup {

namespace {
const GuiApi* g_api = nullptr;
}

KitInfo ReadKit() { return g_api->read_kit(); }
Probe CheckSource(const Options& opt) { return g_api->check_source(opt); }
bool Build(const Options& opt) { return g_api->build(opt); }
void SetSink(void (*sink)(const char*)) { g_api->set_sink(sink); }
void SetProgress(void (*progress)(double)) { g_api->set_progress(progress); }
void Cancel() { g_api->cancel(); }

}  // namespace setup

extern "C" __attribute__((visibility("default"))) int setup_gui_run(const setup::GuiApi* api, const setup::Options* defaults) {
    setup::g_api = api;
    return setup::RunWindow(*defaults);
}
