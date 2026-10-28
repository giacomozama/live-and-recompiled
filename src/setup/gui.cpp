// Setup's window (FLTK, linked statically): the game, the working folder and the game folder, each starting at its
// default, and the build's progress below them. The build is main.cpp's, run on its own thread.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

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
    Fl_Check_Button* keep_compiler = nullptr;
    Fl_Button* install = nullptr;
    Fl_Button* close = nullptr;
    Fl_Progress* bar = nullptr;
    Fl_Text_Display* log = nullptr;
    Fl_Text_Buffer* text = nullptr;
    std::thread worker;
    bool running = false, close_when_done = false, overwrite = false;
    bool built = false;      // the last run finished: the button reads Play until something above it changes
    fs::path exe;            // the game this window built
    bool build_ok = false;   // set by the worker before it hands over to Finished
    std::mutex lock;
    std::string pending;  // what the build said and the window has not shown yet
    double progress = 0;  // the build's, for the bar
};

Window* g_ui = nullptr;

// the window's colours: dark grays and one blue
const Fl_Color kBackground = fl_rgb_color(0x22, 0x22, 0x22), kField = fl_rgb_color(0x2e, 0x2e, 0x2e),
               kButton = fl_rgb_color(0x3a, 0x3a, 0x3a), kText = fl_rgb_color(0xe8, 0xe8, 0xe8),
               kMuted = fl_rgb_color(0xa0, 0xa0, 0xa0), kLog = fl_rgb_color(0x1a, 0x1a, 0x1a),
               kAccent = fl_rgb_color(0x4c, 0x8d, 0xff), kOk = fl_rgb_color(0x6f, 0xcf, 0x8a),
               kBad = fl_rgb_color(0xff, 0x7b, 0x72);

// square boxes: the radius is kept in the signatures but no longer rounds anything
void FillRounded(int x, int y, int w, int h, int, Fl_Color c) {
    fl_color(c);
    fl_rectf(x, y, w, h);
}

void OutlineRounded(int x, int y, int w, int h, int, Fl_Color c) {
    fl_color(c);
    fl_rect(x, y, w, h);
}

const int kRadius = 6;

// a flat button with rounded corners that lightens under the pointer and darkens when pressed; grayed out when it
// can't be used
class FlatButton : public Fl_Button {
public:
    using Fl_Button::Fl_Button;

    int handle(int event) override {
        if (event == FL_ENTER || event == FL_LEAVE) {
            hover_ = event == FL_ENTER;
            redraw();
        }
        return Fl_Button::handle(event);
    }

    void draw() override {
        Fl_Color fill = color();
        if (!active()) fill = fl_color_average(kBackground, FL_WHITE, 0.93f);
        else if (value()) fill = fl_color_average(fill, FL_BLACK, 0.8f);
        else if (hover_) fill = fl_color_average(fill, FL_WHITE, 0.85f);
        FillRounded(x(), y(), w(), h(), kRadius, fill);
        Fl_Color text = labelcolor();
        if (!active()) text = fl_color_average(kMuted, fill, 0.5f);
        fl_color(text);
        fl_font(labelfont(), labelsize());
        fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_CENTER);
    }

private:
    bool hover_ = false;
};

// a check box with a check mark in it when it is on, and an empty one when it is off
class CheckBox : public Fl_Check_Button {
public:
    using Fl_Check_Button::Fl_Check_Button;

    void draw() override {
        const int s = 20, bx = x(), by = y() + (h() - s) / 2;
        if (value()) {
            FillRounded(bx, by, s, s, 5, kAccent);
            fl_color(FL_WHITE);
            fl_line_style(FL_SOLID, 2, nullptr);
            fl_begin_line();
            fl_vertex(bx + 5, by + 10.5);
            fl_vertex(bx + 8.5, by + 14);
            fl_vertex(bx + 15, by + 6);
            fl_end_line();
            fl_line_style(FL_SOLID);
        } else {
            FillRounded(bx, by, s, s, 5, kField);
            OutlineRounded(bx, by, s, s, 5, kMuted);
        }
        fl_color(labelcolor());
        fl_font(labelfont(), labelsize());
        fl_draw(label(), x() + s + 10, y(), w() - s - 10, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
};

// the progress bar: a rounded track with a rounded fill and the percentage across it
class ProgressBar : public Fl_Progress {
public:
    using Fl_Progress::Fl_Progress;

    void draw() override {
        FillRounded(x(), y(), w(), h(), kRadius, kField);
        double fraction = maximum() > minimum() ? (value() - minimum()) / (maximum() - minimum()) : 0;
        int filled = int(std::max(0.0, std::min(1.0, fraction)) * w());
        if (filled > 0) FillRounded(x(), y(), std::max(filled, 2 * kRadius), h(), kRadius, kAccent);
        fl_color(FL_WHITE);
        fl_font(labelfont(), labelsize());
        fl_draw(label(), x(), y(), w(), h(), FL_ALIGN_CENTER);
    }
};

// a scroll bar that is only a rounded thumb on the log's own background, brighter under the pointer and while dragged;
// it moves as FLTK's does (the same thumb size and position)
class FlatScrollbar : public Fl_Scrollbar {
public:
    FlatScrollbar() : Fl_Scrollbar(0, 0, 1, 1) {}

    int handle(int event) override {
        switch (event) {
            case FL_ENTER:
            case FL_LEAVE:
                hover_ = event == FL_ENTER;
                redraw();
                return 1;
            case FL_PUSH:
            case FL_DRAG:
            case FL_RELEASE: {
                int result = Fl_Slider::handle(event, x(), y(), w(), h());
                redraw();
                return result;
            }
            default: return Fl_Scrollbar::handle(event);
        }
    }

    void draw() override {
        fl_rectf(x(), y(), w(), h(), kLog);
        const int length = horizontal() ? w() : h(), thick = horizontal() ? h() : w();
        int thumb = length, offset = 0;
        if (minimum() != maximum()) {
            thumb = std::min(length, std::max(int(slider_size() * length + 0.5), thick / 2 + 1));
            double at = std::max(0.0, std::min(1.0, (value() - minimum()) / (maximum() - minimum())));
            offset = int(at * (length - thumb) + 0.5);
        }
        const int inset = 2, size = thick - 2 * inset;
        Fl_Color c = hover_ || Fl::pushed() == this ? fl_rgb_color(0x68, 0x68, 0x68) : fl_rgb_color(0x4c, 0x4c, 0x4c);
        if (horizontal()) FillRounded(x() + offset, y() + inset, thumb, size, size / 2, c);
        else FillRounded(x() + inset, y() + offset, size, thumb, size / 2, c);
    }

private:
    bool hover_ = false;
};

// the log, with that scroll bar in place of FLTK's
class LogView : public Fl_Text_Display {
public:
    LogView(int x, int y, int w, int h) : Fl_Text_Display(x, y, w, h) {
        begin();
        remove(mVScrollBar);
        delete mVScrollBar;
        mVScrollBar = new FlatScrollbar();
        mVScrollBar->callback(reinterpret_cast<Fl_Callback*>(v_scrollbar_cb), this);
        end();
        scrollbar_width(12);
    }
};

// a rounded box with room before the text, for the inputs and the log
const Fl_Boxtype kInsetBox = FL_FREE_BOXTYPE;
void DrawInset(int x, int y, int w, int h, Fl_Color c) {
    FillRounded(x, y, w, h, kRadius, c);
    OutlineRounded(x, y, w, h, kRadius, fl_color_average(c, FL_WHITE, 0.88f));
}

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
    o.keep_compiler = u.keep_compiler->value();
    return o;
}

void Refresh(Window& u) {
    Probe p = CheckSource(Read(u));
    u.status->copy_label(p.text.c_str());
    u.status->labelcolor(p.ok ? kOk : kBad);
    u.status->redraw();
    u.install->activate();
    if ((!p.ok && !u.built) || u.running) u.install->deactivate();
}

void Lock(Window& u, bool busy) {
    for (Fl_Widget* w : {static_cast<Fl_Widget*>(u.source.input), static_cast<Fl_Widget*>(u.source.browse),
                         static_cast<Fl_Widget*>(u.source.browse_file), static_cast<Fl_Widget*>(u.work.input),
                         static_cast<Fl_Widget*>(u.work.browse), static_cast<Fl_Widget*>(u.game.input),
                         static_cast<Fl_Widget*>(u.game.browse),
                         static_cast<Fl_Widget*>(u.keep),
                         static_cast<Fl_Widget*>(u.keep_compiler)}) {
        if (busy) w->deactivate();
        else w->activate();
    }
}

void Finished(void*) {
    Window& u = *g_ui;
    if (u.worker.joinable()) u.worker.join();
    u.running = false;
    u.built = u.build_ok;
    if (u.built) u.exe = Read(u).game_dir / u.kit.game;
    Lock(u, false);
    u.install->label(u.built ? "Play" : "Start");
    u.close->label("Close");
    u.close->activate();
    Refresh(u);
    if (u.close_when_done) u.win->hide();
}

// the finished game, on its own: it keeps running after this window closes
void Launch(const fs::path& exe) {
#ifdef _WIN32
    std::wstring cmd = L"\"" + exe.wstring() + L"\"", dir = exe.parent_path().wstring();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (chdir(exe.parent_path().c_str()) == 0) execl(exe.c_str(), exe.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (pid > 0) {
        std::thread([pid]() { waitpid(pid, nullptr, 0); }).detach();  // no zombie when it quits
    }
#endif
}

void Start(Fl_Widget*, void*) {
    Window& u = *g_ui;
    if (u.running) return;
    if (u.built) {
        Launch(u.exe);
        u.win->hide();
        return;
    }
    u.running = true;
    Lock(u, true);
    u.install->label("Working ...");
    u.install->deactivate();
    u.text->text("");
    u.close->label("Cancel");
    Progressed(0);
    Options o = Read(u);
    u.worker = std::thread([o]() {
        g_ui->build_ok = Build(o);
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
    if (g_ui->running) return;
    g_ui->built = false;
    g_ui->install->label("Start");
    Refresh(*g_ui);
}

void OnClose(Fl_Widget*, void*) {
    Window& u = *g_ui;
    if (!u.running) {
        u.win->hide();
        return;
    }
    u.close_when_done = true;  // the build stops at its next step
    Cancel();
    u.close->label("Cancelling ...");
    u.close->deactivate();
}

}  // namespace

int RunWindow(const Options& defaults) {
    Window u;
    g_ui = &u;
    u.opt = defaults;
    u.kit = ReadKit();
    Fl::lock();
    Fl::scheme("gleam");
    Fl::background(0x22, 0x22, 0x22);
    Fl::background2(0x2e, 0x2e, 0x2e);
    Fl::foreground(0xe8, 0xe8, 0xe8);
    Fl::set_color(FL_SELECTION_COLOR, 0x4c, 0x8d, 0xff);
    Fl::set_boxtype(kInsetBox, DrawInset, 8, 4, 16, 8);

    const int W = 780, H = 710, M = 22, label_h = 22, row_h = 32, browse_w = 100;
    static const char* const kTitle = "Live & Recompiled Setup";
    u.win = new Fl_Double_Window(W, H, kTitle);
    u.win->color(kBackground);
    u.win->callback(OnClose);
    int y = M;
    auto* title = new Fl_Box(M - 3, y, W - 2 * M, 34, nullptr);
    title->label(kTitle);
    title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelsize(22);
    title->labelcolor(kText);
    y += 48;

    auto row = [&](Row& r, const char* name, const fs::path& value, bool two) {
        auto* l = new Fl_Box(M - 3, y, W - 2 * M, label_h, name);
        l->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        l->labelcolor(kMuted);
        l->labelsize(13);
        y += label_h;
        int buttons = two ? 2 * browse_w + 8 : browse_w;
        r.input = new Fl_Input(M, y, W - 2 * M - buttons - 8, row_h);
        r.input->value(ToUtf8(value).c_str());
        r.input->color(kField);
        r.input->textcolor(kText);
        r.input->cursor_color(kText);
        r.input->box(kInsetBox);
        r.input->when(FL_WHEN_CHANGED);
        r.input->callback(Changed);
        int x = W - M - buttons;
        if (two) {
            r.browse_file = new FlatButton(x, y, browse_w, row_h, "File ...");
            r.browse_file->color(kButton);
            r.browse_file->labelcolor(kText);
            r.browse_file->callback([](Fl_Widget*, void*) { Browse(g_ui->source, true); });
            x += browse_w + 8;
        }
        r.browse = new FlatButton(x, y, browse_w, row_h, two ? "Folder ..." : "Browse ...");
        r.browse->color(kButton);
        r.browse->labelcolor(kText);
        y += row_h;
    };

    row(u.source, "Your copy of the game: the DefaultPackage file, a disc image (.iso) or the folder with the extracted files", defaults.source, true);
    u.source.browse->callback([](Fl_Widget*, void*) { Browse(g_ui->source, false); });
    u.status = new Fl_Box(M - 3, y + 2, W - 2 * M, label_h, "");
    u.status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    u.status->labelsize(13);
    y += label_h + 18;

    row(u.work, "Working folder: where Setup puts the compiler it downloads (about 100 MB) and its temporary files", defaults.work, false);
    u.work.browse->callback([](Fl_Widget*, void*) { Browse(g_ui->work, false); });
    y += 18;
    row(u.game, "Game folder: where Setup puts the finished game and its data files", defaults.game_dir, false);
    u.game.browse->callback([](Fl_Widget*, void*) { Browse(g_ui->game, false); });
    y += 20;

    u.keep_compiler = new CheckBox(M, y, W - 2 * M, row_h, "Keep the downloaded compiler (about 100 MB), so the next run is quicker");
    u.keep_compiler->value(defaults.keep_compiler);
    u.keep_compiler->labelcolor(kMuted);
    u.keep_compiler->labelsize(13);
    u.keep = new CheckBox(M, y + row_h, W - 2 * M, row_h, "Keep temporary files (for troubleshooting)");
    u.keep->value(defaults.keep);
    u.keep->labelcolor(kMuted);
    u.keep->labelsize(13);
    y += 2 * row_h + 12;

    u.install = new FlatButton(W - M - browse_w - 8 - 150, y, 150, row_h, "Start");
    u.install->callback(Start);
    u.install->color(kAccent);
    u.install->labelcolor(FL_WHITE);
    u.install->labelfont(FL_HELVETICA_BOLD);
    u.close = new FlatButton(W - M - browse_w, y, browse_w, row_h, "Cancel");
    u.close->color(kButton);
    u.close->labelcolor(kText);
    u.close->callback(OnClose);
    y += row_h + 20;

    u.bar = new ProgressBar(M, y, W - 2 * M, 24);
    u.bar->minimum(0);
    u.bar->maximum(1);
    u.bar->value(0);
    y += 24 + 16;

    u.text = new Fl_Text_Buffer();
    u.log = new LogView(M, y, W - 2 * M, H - y - M);
    u.log->buffer(u.text);
    u.log->box(kInsetBox);
    u.log->color(kLog);
    u.log->textcolor(kText);
    u.log->textfont(FL_COURIER);
    u.log->textsize(12);
    u.log->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    // a fixed size and its own app id: tiling window managers and compositors float such a window, and a rule can match
    // the id (WM_CLASS on X11, app_id on Wayland)
    u.win->size_range(W, H, W, H);
    u.win->xclass("conker-setup");
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
