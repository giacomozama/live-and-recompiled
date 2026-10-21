// Setup's window (FLTK, linked statically): the game, the working folder and the game folder, each starting at its
// default, and the build's progress below them. The build is main.cpp's, run on its own thread.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>

#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "setup.h"

namespace fs = std::filesystem;

namespace setup {

bool WindowAvailable() {
#ifdef _WIN32
    return true;
#else
    const char* x11 = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return (x11 && *x11) || (wayland && *wayland);
#endif
}

namespace {

// FLTK speaks UTF-8
fs::path ToPath(const char* s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s))); }
std::string ToUtf8(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

struct Row {
    Fl_Input* input = nullptr;
    Fl_Button* browse = nullptr;
    Fl_Button* browse_file = nullptr;  // the game: a file (package, image) or a folder
};

struct Window {
    Options opt;
    KitInfo kit;
    Fl_Double_Window* win = nullptr;
    Row source, work, game;
    Fl_Box* status = nullptr;
    Fl_Check_Button* keep = nullptr;
    Fl_Button* install = nullptr;
    Fl_Button* close = nullptr;
    Fl_Progress* bar = nullptr;
    Fl_Text_Display* log = nullptr;
    Fl_Text_Buffer* text = nullptr;
    std::thread worker;
    bool running = false, close_when_done = false, overwrite = false;
    std::mutex lock;
    std::string pending;  // what the build said and the window has not shown yet
    double progress = 0;  // the build's, for the bar
};

Window* g_ui = nullptr;

// the build's text into the log, on the window's thread: \r goes back to the start of the line (progress counters)
void Flush(void*) {
    std::string text;
    {
        std::lock_guard<std::mutex> g(g_ui->lock);
        text.swap(g_ui->pending);
    }
    Window& u = *g_ui;
    for (char c : text) {
        if (c == '\r') {
            u.overwrite = true;
            continue;
        }
        if (u.overwrite) {
            int end = u.text->length();
            u.text->remove(u.text->line_start(end), end);
            u.overwrite = false;
        }
        u.text->append(std::string(1, c).c_str());
    }
    u.log->insert_position(u.text->length());
    u.log->show_insert_position();
}

void ShowProgress(void*) {
    double fraction;
    {
        std::lock_guard<std::mutex> g(g_ui->lock);
        fraction = g_ui->progress;
    }
    static char label[16];
    std::snprintf(label, sizeof(label), "%d%%", int(fraction * 100));
    g_ui->bar->value(float(fraction));
    g_ui->bar->label(label);
    g_ui->bar->redraw();
}

void Progressed(double fraction) {
    {
        std::lock_guard<std::mutex> g(g_ui->lock);
        g_ui->progress = fraction;
    }
    Fl::awake(ShowProgress, nullptr);
}

void Sink(const char* text) {
    {
        std::lock_guard<std::mutex> g(g_ui->lock);
        g_ui->pending += text;
    }
    Fl::awake(Flush, nullptr);
}

Options Read(Window& u) {
    Options o = u.opt;
    o.source = ToPath(u.source.input->value());
    o.work = ToPath(u.work.input->value());
    o.game_dir = ToPath(u.game.input->value());
    o.keep = u.keep->value();
    return o;
}

void Refresh(Window& u) {
    Probe p = CheckSource(Read(u));
    u.status->copy_label(p.text.c_str());
    u.status->labelcolor(p.ok ? fl_rgb_color(0, 120, 40) : fl_rgb_color(180, 40, 30));
    u.status->redraw();
    u.install->activate();
    if (!p.ok || u.running) u.install->deactivate();
}

void Lock(Window& u, bool busy) {
    for (Fl_Widget* w : {static_cast<Fl_Widget*>(u.source.input), static_cast<Fl_Widget*>(u.source.browse),
                         static_cast<Fl_Widget*>(u.source.browse_file), static_cast<Fl_Widget*>(u.work.input),
                         static_cast<Fl_Widget*>(u.work.browse), static_cast<Fl_Widget*>(u.game.input),
                         static_cast<Fl_Widget*>(u.game.browse),
                         static_cast<Fl_Widget*>(u.keep)}) {
        if (busy) w->deactivate();
        else w->activate();
    }
}

void Finished(void*) {
    Window& u = *g_ui;
    if (u.worker.joinable()) u.worker.join();
    u.running = false;
    Lock(u, false);
    u.install->label("Install");
    Refresh(u);
    if (u.close_when_done) u.win->hide();
}

void Start(Fl_Widget*, void*) {
    Window& u = *g_ui;
    if (u.running) return;
    u.running = true;
    Lock(u, true);
    u.install->deactivate();
    u.text->text("");
    Progressed(0);
    Options o = Read(u);
    u.worker = std::thread([o]() {
        Build(o);
        Fl::awake(Finished, nullptr);
    });
}

// a folder or file into the input beside the button
void Browse(Row& row, bool file) {
    Fl_Native_File_Chooser chooser;
    chooser.type(file ? Fl_Native_File_Chooser::BROWSE_FILE : Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    chooser.options(Fl_Native_File_Chooser::NEW_FOLDER);
    std::string current = row.input->value();
    if (!current.empty()) chooser.preset_file(current.c_str());
    if (chooser.show() != 0 || !chooser.filename()) return;
    row.input->value(chooser.filename());
    row.input->do_callback();
}

void Changed(Fl_Widget*, void*) {
    if (!g_ui->running) Refresh(*g_ui);
}

void OnClose(Fl_Widget*, void*) {
    Window& u = *g_ui;
    if (!u.running) {
        u.win->hide();
        return;
    }
    u.close_when_done = true;  // the build stops at its next step
    Cancel();
    u.install->label("Cancelling ...");
}

}  // namespace

int RunWindow(const Options& defaults) {
    Window u;
    g_ui = &u;
    u.opt = defaults;
    u.kit = ReadKit();
    Fl::lock();
    Fl::scheme("gtk+");

    const int W = 780, H = 620, M = 14, label_h = 20, row_h = 30, browse_w = 90;
    u.win = new Fl_Double_Window(W, H, "Setup");
    u.win->callback(OnClose);
    int y = M;
    auto* title = new Fl_Box(M, y, W - 2 * M, 28, nullptr);
    static std::string heading = "Conker: Live & Recompiled Setup";
    title->label(heading.c_str());
    title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelsize(16);
    y += 36;

    auto row = [&](Row& r, const char* name, const fs::path& value, bool two) {
        auto* l = new Fl_Box(M, y, W - 2 * M, label_h, name);
        l->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        y += label_h;
        int buttons = two ? 2 * browse_w + 8 : browse_w;
        r.input = new Fl_Input(M, y, W - 2 * M - buttons - 8, row_h);
        r.input->value(ToUtf8(value).c_str());
        r.input->when(FL_WHEN_CHANGED);
        r.input->callback(Changed);
        int x = W - M - buttons;
        if (two) {
            r.browse_file = new Fl_Button(x, y, browse_w, row_h, "File ...");
            r.browse_file->callback([](Fl_Widget*, void*) { Browse(g_ui->source, true); });
            x += browse_w + 8;
        }
        r.browse = new Fl_Button(x, y, browse_w, row_h, two ? "Folder ..." : "Browse ...");
        y += row_h;
    };

    row(u.source, "Your copy of the game: the DefaultPackage file, a disc image (.iso) or the folder with the extracted files", defaults.source, true);
    u.source.browse->callback([](Fl_Widget*, void*) { Browse(g_ui->source, false); });
    u.status = new Fl_Box(M, y + 2, W - 2 * M, label_h, "");
    u.status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    y += label_h + 10;

    row(u.work, "Working folder: where Setup keeps the tools it downloads (about 100 MB, reused next time) and its temporary files", defaults.work, false);
    u.work.browse->callback([](Fl_Widget*, void*) { Browse(g_ui->work, false); });
    y += 10;
    row(u.game, "Game folder: where Setup puts the finished game and its data files", defaults.game_dir, false);
    u.game.browse->callback([](Fl_Widget*, void*) { Browse(g_ui->game, false); });
    y += 12;

    u.keep = new Fl_Check_Button(M, y, 260, row_h, "Keep temporary files (for troubleshooting)");
    u.keep->value(defaults.keep);
    u.install = new Fl_Button(W - M - 2 * browse_w - 8, y, browse_w, row_h, "Install");
    u.install->callback(Start);
    u.close = new Fl_Button(W - M - browse_w, y, browse_w, row_h, "Close");
    u.close->callback(OnClose);
    y += row_h + 12;

    u.bar = new Fl_Progress(M, y, W - 2 * M, 22);
    u.bar->minimum(0);
    u.bar->maximum(1);
    u.bar->value(0);
    y += 22 + 10;

    u.text = new Fl_Text_Buffer();
    u.log = new Fl_Text_Display(M, y, W - 2 * M, H - y - M);
    u.log->buffer(u.text);
    u.log->textfont(FL_COURIER);
    u.log->textsize(12);
    u.log->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    u.win->resizable(u.log);
    u.win->size_range(W, 480);
    u.win->end();

    if (!u.kit.error.empty()) {
        u.text->text((u.kit.error + "\n").c_str());
        u.install->deactivate();
    }
    SetSink(Sink);
    SetProgress(Progressed);
    Refresh(u);
    u.win->show();
    int code = Fl::run();
    if (u.worker.joinable()) u.worker.join();
    SetSink(nullptr);
    SetProgress(nullptr);
    return code;
}

}  // namespace setup
