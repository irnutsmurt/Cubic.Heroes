/* M10.2 (docs/M10_PLAN.md): the in-game settings overlay.
 *
 * Opened by the controller's shortcut (L3+R3 held 500 ms by default; DOTHEROES_OVERLAY_COMBO), F1, Escape, or the
 * window's close button (straight to Exit Game's question). While it is open the game is held
 * (dotheroes_host_pause: its clocks stand still, its frame loop waits at its flip, the sound pauses) and its pad
 * input withheld (cellPad_ui_intercept); the shortcut's buttons never reach the game as a chord
 * (cellPad_set_chord_guard), and they must be released before it can open again. It closes only once every button
 * is released, so nothing pressed to close it reaches the game. The original START menu is the game's, untouched.
 *
 * The picture: composed here on the CPU at the window's size -- the game's paused frame dimmed behind, a panel in
 * the launcher's look (its colours, its pixel font for titles, the gold focus) with the game's UI font for text and
 * the player's prompt style -- and handed to the renderer (ps3emu/osd.h g_ps3_osd_menu), which copies it over the
 * frame in each present; the overlay presents itself while the game is held (g_ps3_osd_present_again).
 *
 * Settings: read from the one settings file when it opens, written back there (read, merged, written whole under
 * its lock: config/dotheroes.cfg.lock, as the launcher's worker does), applied at once where they can be (display
 * mode, VSync, the frame limit between rates, the performance overlay, the volumes, the prompts); the rest marked
 * for the next start.
 */
#include "diag/game_log.h"
#include "devtools/devtools.h"            /* M11.0b: the Developer pages (developer mode only) */
#include "devtools/devtools_internal.h"
#include "ps3emu/guest_pause.h"
#include "ps3emu/osd.h"
#include "ui_pixel_font.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

extern "C" {
uint32_t cellPad_host_buttons(uint32_t* ly);
uint32_t cellPad_host_keys(int* mx, int* my, int* mbuttons);
void cellPad_set_chord_guard(uint32_t mask, unsigned ms);
void cellPad_ui_intercept(int on);
int cellPad_ui_device(void);
void dotheroes_host_pause(int on);
void ps3_host_quit(const char* why);
void rsx_vulkan_backend_request_display(int mode, int vsync) __attribute__((weak));
void nsm_set_volumes(float master, float music, float effects, int mute) __attribute__((weak));
int dotheroes_set_frame_limit(const char* value);   /* main.cpp: 1 when applied at once */
void cellPad_set_pad_binding(int game_bit, uint32_t pad_bits) __attribute__((weak));
int cellPad_set_key_binding(int game_bit, const char* key_name) __attribute__((weak));
const char* cellPad_key_binding_name(int game_bit) __attribute__((weak));
int cellPad_host_key_held(char* name, int n) __attribute__((weak));
}

namespace {

/* the pad's buttons (cellPad.h CELL_PAD_CTRL_*) */
enum : uint32_t {
    B_SELECT = 1u << 0, B_L3 = 1u << 1, B_R3 = 1u << 2, B_START = 1u << 3, B_UP = 1u << 4, B_RIGHT = 1u << 5,
    B_DOWN = 1u << 6, B_LEFT = 1u << 7, B_L2 = 1u << 8, B_R2 = 1u << 9, B_L1 = 1u << 10, B_R1 = 1u << 11,
    B_TRIANGLE = 1u << 12, B_CIRCLE = 1u << 13, B_CROSS = 1u << 14, B_SQUARE = 1u << 15
};
/* the keys (cellPad_host_keys) */
enum : uint32_t { K_F1 = 1, K_UP = 2, K_DOWN = 4, K_LEFT = 8, K_RIGHT = 16, K_ENTER = 32, K_ESC = 64, K_BACK = 128, K_SPACE = 256 };

/* ---- the settings this menu shows ---- */
struct Choice { const char* v; const char* label; };
struct Item {
    const char* key;      /* nullptr: an action */
    const char* label;
    std::vector<Choice> choices;
    int live;             /* 1 applied at once, 0 at the next start, 2 at once when it can be */
    const char* dflt;
    const char* right = nullptr;   /* M11.0b: a text at the row's right (a Developer page's value) */
};

std::vector<Choice> percent()
{
    static const char* v[] = { "0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100" };
    static const char* l[] = { "0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%" };
    std::vector<Choice> c;
    for (int i = 0; i < 11; i++) c.push_back({ v[i], l[i] });
    return c;
}

enum Page { P_MAIN, P_DISPLAY, P_AUDIO, P_CONTROLS, P_PERF, P_PAD, P_KEYS, P_DEV, P_COUNT };
const char* kPageTitle[P_COUNT] = { "PAUSED", "DISPLAY & GRAPHICS", "AUDIO", "CONTROLS", "PERFORMANCE",
                                    "CONTROLLER BUTTONS", "KEYBOARD KEYS", "DEVELOPER" };
DevView g_dev_view;   /* M11.0b: the Developer page shown (port/devtools/dev_menu.cpp), its rows' text kept here */

/* ---- bindings (M10.3): the game's sixteen buttons, by bit (cellPad.h CELL_PAD_CTRL_*) ---- */
const char* kGameButton[16] = { "Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left", "L2", "R2", "L1", "R1",
                                "Triangle", "Circle", "Cross", "Square" };
/* the rows of the binding pages, in a reading order: the face buttons first */
const int kBindOrder[16] = { 14, 13, 15, 12, 10, 11, 8, 9, 1, 2, 3, 0, 4, 6, 7, 5 };
/* a pad's button by its place, in the player's prompt style (bit order as above) */
const char* pad_button_name(int bit, int style)
{
    static const char* xb[16] = { "Back", "LS", "RS", "Start", "D-pad up", "D-pad right", "D-pad down", "D-pad left",
                                  "LT", "RT", "LB", "RB", "Y", "B", "A", "X" };
    static const char* ps[16] = { "Select", "L3", "R3", "Start", "D-pad up", "D-pad right", "D-pad down", "D-pad left",
                                  "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square" };
    static const char* sw[16] = { "Minus", "LS", "RS", "Plus", "D-pad up", "D-pad right", "D-pad down", "D-pad left",
                                  "ZL", "ZR", "L", "R", "X", "A", "B", "Y" };
    if (bit < 0 || bit > 15) return "?";
    return style == PS3_UI_PROMPTS_PLAYSTATION ? ps[bit] : style == PS3_UI_PROMPTS_SWITCH ? sw[bit] : xb[bit];
}
int g_pad_bind[16];           /* for each game button, the pad's button (bit) that presses it */
std::string g_key_bind[16];   /* for each game button, its key's name ("" the default) */
int g_capture = -1;           /* a binding row waiting for its button or key (its game bit) */
int g_capture_ms = 0;
bool g_capture_armed = false; /* everything released since the capture began */

std::vector<Item> page_items(int p)
{
    static const std::vector<Choice> onoff = { { "1", "On" }, { "0", "Off" } };
    switch (p) {
    case P_MAIN: {
        std::vector<Item> v = { { nullptr, "Resume Game", {}, 1, "" }, { nullptr, "Display & Graphics", {}, 1, "" },
                                { nullptr, "Audio", {}, 1, "" }, { nullptr, "Controls", {}, 1, "" },
                                { nullptr, "Performance", {}, 1, "" } };
        if (devtools_on()) v.push_back({ nullptr, "Developer", {}, 1, "" });   /* M11.0: developer mode only */
        v.push_back({ nullptr, "Return to Launcher", {}, 1, "" });
        v.push_back({ nullptr, "Exit Game", {}, 1, "" });
        return v;
    }
    case P_DEV: {
        std::vector<Item> v;
        for (const DevRow& r : g_dev_view.rows) v.push_back({ nullptr, r.label.c_str(), {}, 1, "", r.value.c_str() });
        return v;
    }
    case P_DISPLAY:
        return { { "DOTHEROES_DISPLAY_MODE", "Display mode",
                   { { "windowed", "Windowed" }, { "borderless", "Borderless fullscreen" }, { "exclusive", "Exclusive fullscreen" } },
                   1, "windowed" },
                 { "DOTHEROES_VSYNC", "VSync", onoff, 1, "1" } };
    case P_AUDIO:
        return { { "DOTHEROES_VOLUME_MASTER", "Master volume", percent(), 1, "100" },
                 { "DOTHEROES_VOLUME_MUSIC", "Music volume", percent(), 1, "100" },
                 { "DOTHEROES_VOLUME_SFX", "Sound effects volume", percent(), 1, "100" },
                 { "DOTHEROES_MUTE", "Mute", { { "0", "Off" }, { "1", "On" } }, 1, "0" } };
    case P_CONTROLS:
        return { { "LAUNCHER_PROMPTS", "Button prompts",
                   { { "auto", "Automatic" }, { "xbox", "Xbox" }, { "playstation", "PlayStation" },
                     { "nintendo", "Nintendo Switch" }, { "steam", "Steam" }, { "generic", "Generic" } }, 1, "auto" },
                 { "DOTHEROES_OVERLAY_COMBO", "Menu shortcut (held)",
                   { { "l3r3", "L3 + R3" }, { "selectstart", "Back/Select + Start" }, { "l1r1select", "L1 + R1 + Back/Select" } },
                   1, "l3r3" },
                 { "DOTHEROES_OVERLAY_HINT", "Show the shortcut at start", onoff, 1, "1" },
                 { nullptr, "Controller buttons...", {}, 1, "" }, { nullptr, "Keyboard keys...", {}, 1, "" } };
    case P_PAD:
    case P_KEYS: {
        std::vector<Item> v;
        for (int i = 0; i < 16; i++) v.push_back({ nullptr, kGameButton[kBindOrder[i]], {}, 1, "" });
        v.push_back({ nullptr, "Restore the defaults", {}, 1, "" });
        return v;
    }
    case P_PERF:
        return { { "DOTHEROES_FPS", "Frame limit",
                   { { "30", "30 fps (the original)" }, { "60", "60 fps" }, { "75", "75 fps" }, { "100", "100 fps" },
                     { "120", "120 fps" }, { "144", "144 fps" }, { "165", "165 fps" }, { "240", "240 fps" },
                     { "display", "The display's rate" }, { "0", "Unlimited" } }, 2, "60" },
                 { "DOTHEROES_PERF_OVERLAY", "Performance overlay",
                   { { "off", "Off" }, { "fps", "Frame rate" }, { "graph", "Frame rate and times" } }, 1, "off" },
                 { "DOTHEROES_LOG_LEVEL", "Log detail",
                   { { "error", "Errors only" }, { "warning", "Errors and warnings" }, { "info", "Normal" },
                     { "debug", "Detailed" }, { "trace", "Everything" } }, 0, "info" } };
    }
    return {};
}

/* ---- the settings file: read, and written merged under its lock (tools/game_settings.py takes the same) ---- */
std::string g_cfg;   /* <install>/config/dotheroes.cfg (empty: not installed -- nothing is written) */
std::map<std::string, std::string> g_vals;

std::map<std::string, std::string> read_cfg()
{
    std::map<std::string, std::string> m;
    FILE* f = g_cfg.empty() ? nullptr : fopen(g_cfg.c_str(), "r");
    if (!f) return m;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || !*s) continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = s, v = eq + 1;
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        size_t a = 0;
        while (a < v.size() && v[a] == ' ') a++;
        m[k] = v.substr(a);
    }
    fclose(f);
    return m;
}

struct CfgLock {
    std::string path;
    bool ok = false;
    explicit CfgLock(const std::string& cfg) : path(cfg + ".lock")
    {
        for (int i = 0; i < 100 && !ok; i++) {
            if (FILE* f = fopen(path.c_str(), "wx")) { fprintf(f, "game\n"); fclose(f); ok = true; break; }
            std::error_code ec;   /* a lock left by a process that died: older than 10 s, taken over */
            const auto t = std::filesystem::last_write_time(path, ec);
            if (!ec && std::filesystem::file_time_type::clock::now() - t > std::chrono::seconds(10))
                std::filesystem::remove(path, ec);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ~CfgLock()
    {
        std::error_code ec;
        if (ok) std::filesystem::remove(path, ec);
    }
};

bool write_setting(const std::string& key, const std::string& value)
{
    if (g_cfg.empty()) return false;
    CfgLock lock(g_cfg);
    if (!lock.ok) { dh_log(DH_LOG_WARNING, "overlay", "the settings file stayed locked: %s not saved", key.c_str()); return false; }
    std::vector<std::string> lines;
    bool done = false;
    if (FILE* f = fopen(g_cfg.c_str(), "r")) {
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) {
            std::string ln = buf;
            while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r')) ln.pop_back();
            size_t a = ln.find_first_not_of(" \t");
            if (a != std::string::npos && ln[a] != '#') {
                size_t eq = ln.find('=', a);
                std::string k = eq == std::string::npos ? "" : ln.substr(a, eq - a);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                if (k == key) { if (!done) lines.push_back(key + "=" + value); done = true; continue; }
            }
            lines.push_back(ln);
        }
        fclose(f);
    }
    if (!done) lines.push_back(key + "=" + value);
    const std::string part = g_cfg + ".part";
    FILE* f = fopen(part.c_str(), "w");
    if (!f) return false;
    for (const auto& l : lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    std::error_code ec;
    std::filesystem::rename(part, g_cfg, ec);
    if (ec) { std::filesystem::remove(part, ec); return false; }
    return true;
}

std::string val(const Item& it)
{
    auto f = g_vals.find(it.key);
    return f != g_vals.end() && !f->second.empty() ? f->second : it.dflt;
}

/* ---- applying a setting while the game runs ---- */
void put_env(const char* k, const std::string& v)
{
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

uint32_t combo_mask(const std::string& c)
{
    if (c == "selectstart") return B_SELECT | B_START;
    if (c == "l1r1select") return B_L1 | B_R1 | B_SELECT;
    return B_L3 | B_R3;
}
const char* combo_name(uint32_t m)
{
    return m == (B_SELECT | B_START) ? "BACK+START" : m == (B_L1 | B_R1 | B_SELECT) ? "L1+R1+BACK" : "L3+R3";
}
std::atomic<uint32_t> g_combo{ B_L3 | B_R3 };

void apply_volumes()
{
    if (!nsm_set_volumes) return;
    auto pc = [](const char* k, const char* d) {
        auto f = g_vals.find(k);
        const int v = atoi(f != g_vals.end() && !f->second.empty() ? f->second.c_str() : d);
        return (float)(v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f;
    };
    auto m = g_vals.find("DOTHEROES_MUTE");
    nsm_set_volumes(pc("DOTHEROES_VOLUME_MASTER", "100"), pc("DOTHEROES_VOLUME_MUSIC", "100"),
                    pc("DOTHEROES_VOLUME_SFX", "100"), m != g_vals.end() && m->second == "1");
}

/* 1 applied now, 0 at the next start */
int apply(const Item& it, const std::string& v)
{
    const std::string k = it.key;
    if (k == "DOTHEROES_DISPLAY_MODE") {
        if (!rsx_vulkan_backend_request_display) return 0;
        rsx_vulkan_backend_request_display(v == "exclusive" ? 2 : v == "borderless" ? 1 : 0, -1);
        return 1;
    }
    if (k == "DOTHEROES_VSYNC") {
        if (!rsx_vulkan_backend_request_display) return 0;
        rsx_vulkan_backend_request_display(-1, v == "0" ? 0 : 1);
        return 1;
    }
    if (k == "DOTHEROES_PERF_OVERLAY") { ps3_osd_fps_set_mode(v == "graph" ? 2 : v == "fps" ? 1 : 0); return 1; }
    if (k == "DOTHEROES_FPS") return dotheroes_set_frame_limit(v.c_str());
    if (k.rfind("DOTHEROES_VOLUME_", 0) == 0 || k == "DOTHEROES_MUTE") { apply_volumes(); return nsm_set_volumes ? 1 : 0; }
    if (k == "LAUNCHER_PROMPTS") {   /* the game's own screens (the save list) follow the same choice */
        put_env("PS3_UI_PROMPTS", v == "xbox" || v == "steam" || v == "generic" ? "xbox" : v == "playstation" ? "playstation"
                                  : v == "nintendo" ? "switch" : "");
        return 1;
    }
    if (k == "DOTHEROES_OVERLAY_COMBO") {
        g_combo = combo_mask(v);
        cellPad_set_chord_guard(g_combo, 120);
        return 1;
    }
    if (k == "DOTHEROES_OVERLAY_HINT") return 1;
    return it.live == 1;
}

/* ---- the overlay's state ---- */
struct Dialog { bool open = false; int kind = 0; int focus = 0; };   /* kind 1 return to the launcher, 2 exit */
struct State {
    bool open = false;
    bool closing = false;        /* hidden, the game still held until every button is released */
    int closing_ms = 0;
    int page = P_MAIN;
    int focus = 0;
    int top = 0;                 /* the first row shown (a page longer than the panel scrolls) */
    int vis = 9;                 /* the rows the panel shows (as last drawn) */
    int main_focus = 0;
    Dialog dlg;
    std::string note;            /* a line under the items: "applies when the game next starts" */
    std::vector<uint8_t> px;     /* the picture */
    std::vector<uint8_t> bg;     /* the game's paused frame, dimmed, at the picture's size */
    uint32_t W = 0, H = 0;
    int bgra = 0;
    uint32_t rev = 0;
} S;

std::atomic<int> g_request{ 0 };   /* from the window: 1 open, 2 the exit question */
std::string g_root;                /* the install's folder */

/* ---- drawing ---- */
const uint32_t C_PANEL = 0x182254, C_PANEL2 = 0x22306C, C_FRAME = 0xEEEAD8, C_TEXT = 0xFAF6E8, C_DIM = 0xC6CEEA,
               C_GOLD = 0xF0B62C, C_ACCENT = 0xFFCE48, C_BRICK = 0xB03C34, C_INK = 0x080816;

void pix_text(const ps3_ui_canvas* c, float x, float y, const char* s, float k, uint32_t rgb, int a = 255)
{
    for (; *s; s++) {
        const PixGlyph* g = pix_glyph(*s);
        const int w = (int)strlen(g->rows[0]);
        for (int r = 0; r < 7; r++)
            for (int q = 0; q < w; q++)
                if (g->rows[r][q] == '#')
                    ps3_ui_fill(c, (int)(x + q * k), (int)(y + r * k), (int)(x + (q + 1) * k), (int)(y + (r + 1) * k), rgb, a);
        x += (w + 1) * k;
    }
}
float pix_width(const char* s, float k)
{
    float w = 0;
    for (; *s; s++) w += ((float)strlen(pix_glyph(*s)->rows[0]) + 1) * k;
    return w;
}

/* a retro window: the launcher's panel, its light frame with stepped corners */
void panel(const ps3_ui_canvas* c, float x0, float y0, float x1, float y1, float s, uint32_t fill = C_PANEL, int a = 244)
{
    const float t = 3 * s, st = 6 * s;
    ps3_ui_fill(c, (int)(x0 + st), (int)y0, (int)(x1 - st), (int)y1, fill, a);
    ps3_ui_fill(c, (int)x0, (int)(y0 + st), (int)(x0 + st), (int)(y1 - st), fill, a);
    ps3_ui_fill(c, (int)(x1 - st), (int)(y0 + st), (int)x1, (int)(y1 - st), fill, a);
    ps3_ui_fill(c, (int)(x0 + st), (int)y0, (int)(x1 - st), (int)(y0 + t), C_FRAME, 255);
    ps3_ui_fill(c, (int)(x0 + st), (int)(y1 - t), (int)(x1 - st), (int)y1, C_FRAME, 255);
    ps3_ui_fill(c, (int)x0, (int)(y0 + st), (int)(x0 + t), (int)(y1 - st), C_FRAME, 255);
    ps3_ui_fill(c, (int)(x1 - t), (int)(y0 + st), (int)x1, (int)(y1 - st), C_FRAME, 255);
    /* the stepped corners */
    for (int i = 0; i < 2; i++) {
        const float yy = i ? y1 - st : y0 + st - t;
        ps3_ui_fill(c, (int)(x0 + t), (int)yy, (int)(x0 + st), (int)(yy + t), C_FRAME, 255);
        ps3_ui_fill(c, (int)(x1 - st), (int)yy, (int)(x1 - t), (int)(yy + t), C_FRAME, 255);
    }
}

void focus_box(const ps3_ui_canvas* c, float x0, float y0, float x1, float y1, float s)
{
    ps3_ui_rrect(c, x0, y0, x1, y1, 4 * s, C_PANEL2, 255);
    ps3_ui_rrect_outline(c, x0, y0, x1, y1, 4 * s, 3 * s, C_GOLD, 255);
    /* the pixel cursor, at the row's left */
    const float k = 3 * s, cx = x0 - 7 * k, cy = (y0 + y1) / 2 - 3.5f * k;
    static const char* arrow[7] = { "#...", "##..", "###.", "####", "###.", "##..", "#..." };
    for (int r = 0; r < 7; r++)
        for (int q = 0; q < 4; q++)
            if (arrow[r][q] == '#')
                ps3_ui_fill(c, (int)(cx + q * k), (int)(cy + r * k), (int)(cx + (q + 1) * k), (int)(cy + (r + 1) * k), C_TEXT, 255);
}

int prompt_style()
{
    const char* e = getenv("PS3_UI_PROMPTS");
    if (e && *e) {
        if (!strcmp(e, "playstation")) return PS3_UI_PROMPTS_PLAYSTATION;
        if (!strcmp(e, "xbox")) return PS3_UI_PROMPTS_XBOX;
        if (!strcmp(e, "switch")) return PS3_UI_PROMPTS_SWITCH;
        if (!strcmp(e, "keyboard")) return PS3_UI_PROMPTS_KEYBOARD;
    }
    const int d = cellPad_ui_device();
    return d >= 0 && d < PS3_UI_PROMPTS_N ? d : PS3_UI_PROMPTS_KEYBOARD;
}

struct Row { float x0, y0, x1, y1; };
std::vector<Row> g_rows;   /* the focusable rows as last drawn (the mouse's targets) */

void compose()
{
    ps3_ui_canvas c = { S.px.data(), (int)S.W, (int)S.H, S.bgra };
    const float s = S.H / 720.0f;
    if (S.bg.size() == S.px.size()) memcpy(S.px.data(), S.bg.data(), S.px.size());
    else ps3_ui_fill(&c, 0, 0, (int)S.W, (int)S.H, 0x0E122C, 255);
    g_rows.clear();
    const float pw = std::min((float)S.W - 40 * s, 760 * s), ph = std::min((float)S.H - 40 * s, 600 * s);
    const float x0 = (S.W - pw) / 2, y0 = (S.H - ph) / 2, x1 = x0 + pw, y1 = y0 + ph;
    panel(&c, x0, y0, x1, y1, s);
    const float tk = 4 * s;   /* the pixel font's dot */
    pix_text(&c, x0 + 36 * s, y0 + 30 * s, S.page == P_DEV ? g_dev_view.title.c_str() : kPageTitle[S.page], tk, C_ACCENT);
    ps3_ui_fill(&c, (int)(x0 + 36 * s), (int)(y0 + 70 * s), (int)(x1 - 36 * s), (int)(y0 + 72 * s), C_FRAME, 90);
    const std::vector<Item> items = page_items(S.page);
    const int n = (int)items.size();
    /* the rows fit the panel: shorter rows for a longer page, down to a floor; past that the page scrolls (M11.0b) */
    const float avail = ph - 92 * s - 92 * s;
    const int rowh = (int)std::max(44 * s, std::min(54 * s, n ? avail / n : 54 * s));
    const int px = (int)std::min(24 * s, rowh * 0.45f);
    S.vis = std::max(1, (int)(avail / rowh));
    if (S.focus < S.top) S.top = S.focus;
    if (S.focus >= S.top + S.vis) S.top = S.focus - S.vis + 1;
    S.top = std::max(0, std::min(S.top, std::max(0, n - S.vis)));
    float y = y0 + 92 * s;
    if (S.top > 0) {
        char m[32];
        snprintf(m, sizeof m, "%d more above", S.top);
        ps3_ui_text(&c, x1 - 36 * s - ps3_ui_text_width(m, (int)(17 * s)), y0 + 84 * s, m, (int)(17 * s), C_DIM, 255, 0);
    }
    for (int i = S.top; i < n && i < S.top + S.vis; i++) {
        const Item& it = items[i];
        const float rx0 = x0 + 60 * s, rx1 = x1 - 36 * s;
        const bool f = (int)i == S.focus && !S.dlg.open;
        if (f) focus_box(&c, rx0, y, rx1, y + rowh - 8 * s, s);
        g_rows.push_back({ rx0, y, rx1, y + rowh - 8 * s });
        const float base = y + (rowh - 8 * s) / 2 + px * 0.36f;
        const bool danger = !it.key && (!strcmp(it.label, "Exit Game"));
        ps3_ui_text(&c, rx0 + 18 * s, base, it.label, px, f ? C_TEXT : danger ? 0xFF8A80 : C_DIM, 255, 0);
        if (it.key) {
            const std::string v = val(it);
            std::string shown = v;
            for (const Choice& ch : it.choices) if (v == ch.v) shown = ch.label;
            const std::string t = std::string("<  ") + shown + "  >";
            const float w = ps3_ui_text_width(t.c_str(), px);
            ps3_ui_text(&c, rx1 - 18 * s - w, base, t.c_str(), px, f ? C_ACCENT : C_DIM, 255, 0);
        } else if ((S.page == P_PAD || S.page == P_KEYS) && i < 16) {   /* the button or key bound to it */
            const int g = kBindOrder[i];
            std::string t = g_capture == g ? (S.page == P_PAD ? "Press a button..." : "Press a key...")
                          : S.page == P_PAD ? pad_button_name(g_pad_bind[g], prompt_style())
                          : (cellPad_key_binding_name ? cellPad_key_binding_name(g) : "");
            const float w = ps3_ui_text_width(t.c_str(), px);
            ps3_ui_text(&c, rx1 - 18 * s - w, base, t.c_str(), px, g_capture == g ? C_GOLD : f ? C_ACCENT : C_DIM, 255, 0);
        } else if (it.right && *it.right) {   /* a Developer page's value */
            const float w = ps3_ui_text_width(it.right, px);
            ps3_ui_text(&c, rx1 - 18 * s - w, base, it.right, px, f ? C_ACCENT : C_DIM, 255, 0);
        }
        y += rowh;
    }
    if (S.top + S.vis < n) {
        char m[32];
        snprintf(m, sizeof m, "%d more below", n - S.top - S.vis);
        ps3_ui_text(&c, x1 - 36 * s - ps3_ui_text_width(m, (int)(17 * s)), y + 4 * s, m, (int)(17 * s), C_DIM, 255, 0);
    }
    if (!S.note.empty())   /* below the "more below" line when the list goes on */
        ps3_ui_text(&c, x0 + 60 * s, y + (S.top + S.vis < n ? 40 : 20) * s, S.note.c_str(), (int)(19 * s), C_GOLD, 255, pw - 96 * s);
    /* the prompts, along the panel's foot */
    const int st = prompt_style();
    const float fy = y1 - 34 * s, bs = 30 * s, fpx = (int)(20 * s);
    float fx = x0 + 36 * s;
    fx += ps3_ui_button(&c, st, 0, fx, fy, bs) + 8 * s;
    fx += ps3_ui_text(&c, fx, fy + fpx * 0.36f, S.page == P_MAIN || S.page == P_DEV ? "Select" : "Change", (int)fpx, C_TEXT,
                      255, 0) + 24 * s;
    fx += ps3_ui_button(&c, st, 1, fx, fy, bs) + 8 * s;
    fx += ps3_ui_text(&c, fx, fy + fpx * 0.36f, S.page == P_MAIN ? "Resume" : "Back", (int)fpx, C_TEXT, 255, 0);
    char hint[64];
    snprintf(hint, sizeof hint, "%s or F1: resume", combo_name(g_combo.load()));
    const float hw = ps3_ui_text_width(hint, (int)(18 * s));
    if (fx + 24 * s < x1 - 36 * s - hw)   /* only where it fits beside the prompts (a narrow window: left out) */
        ps3_ui_text(&c, x1 - 36 * s - hw, fy + 18 * s * 0.36f, hint, (int)(18 * s), C_DIM, 255, 0);
    if (S.dlg.open) {   /* the question, over the panel; Cancel focused */
        ps3_ui_fill(&c, 0, 0, (int)S.W, (int)S.H, C_INK, 120);
        const float dw = std::min((float)S.W - 60 * s, 640 * s), dh = 250 * s;
        const float d0 = (S.W - dw) / 2, e0 = (S.H - dh) / 2;
        panel(&c, d0, e0, d0 + dw, e0 + dh, s);
        const char* t = S.dlg.kind == 1 ? "RETURN TO THE LAUNCHER?" : "EXIT THE GAME?";
        pix_text(&c, d0 + 30 * s, e0 + 28 * s, t, 3.5f * s, C_ACCENT);
        ps3_ui_text(&c, d0 + 30 * s, e0 + 100 * s, "Progress since your last save will be lost.", (int)(22 * s), C_TEXT, 255, dw - 60 * s);
        const char* yes = S.dlg.kind == 1 ? "Return to Launcher" : "Exit Game";
        const float bw = (dw - 90 * s) / 2, by = e0 + dh - 80 * s;
        for (int b = 0; b < 2; b++) {
            const float bx = d0 + 30 * s + b * (bw + 30 * s);
            const bool f = S.dlg.focus == b;
            ps3_ui_rrect(&c, bx, by, bx + bw, by + 52 * s, 4 * s, b == 0 ? C_PANEL2 : C_BRICK, 255);
            if (f) ps3_ui_rrect_outline(&c, bx, by, bx + bw, by + 52 * s, 4 * s, 3 * s, C_GOLD, 255);
            const char* l = b == 0 ? "Cancel" : yes;
            const float lw = ps3_ui_text_width(l, (int)(22 * s));
            ps3_ui_text(&c, bx + (bw - lw) / 2, by + 26 * s + 22 * s * 0.36f, l, (int)(22 * s), C_TEXT, 255, 0);
            g_rows.push_back({ bx, by, bx + bw, by + 52 * s });
        }
    }
}

/* the paused frame, scaled into the window as the game's picture is (its aspect kept), dimmed toward night blue */
void make_background()
{
    S.bg.assign(S.px.size(), 0);
    const ps3_osd_menu_state* M = &g_ps3_osd_menu;
    if (!__atomic_load_n(&M->snap_ready, __ATOMIC_ACQUIRE) || !M->snap) {
        ps3_ui_canvas c = { S.bg.data(), (int)S.W, (int)S.H, S.bgra };
        ps3_ui_fill(&c, 0, 0, (int)S.W, (int)S.H, 0x0E122C, 255);
        return;
    }
    const uint32_t sw = M->snap_w, sh = M->snap_h;
    const double fa = (double)sw / sh, wa = (double)S.W / S.H;
    int dx0 = 0, dy0 = 0, dw = (int)S.W, dh = (int)S.H;
    if (wa > fa) { dw = (int)(S.H * fa); dx0 = ((int)S.W - dw) / 2; } else if (wa < fa) { dh = (int)(S.W / fa); dy0 = ((int)S.H - dh) / 2; }
    for (int y = 0; y < (int)S.H; y++) {
        uint8_t* o = &S.bg[(size_t)y * S.W * 4];
        for (int x = 0; x < (int)S.W; x++, o += 4) {
            int r = 0, g = 0, b = 0;
            if (x >= dx0 && x < dx0 + dw && y >= dy0 && y < dy0 + dh) {
                const uint32_t sx = (uint32_t)((x - dx0) * (double)sw / dw), sy = (uint32_t)((y - dy0) * (double)sh / dh);
                const uint8_t* p = &M->snap[((size_t)sy * sw + sx) * 4];
                r = p[0]; g = p[1]; b = p[2];
            }
            r = r * 30 / 100 + 10; g = g * 30 / 100 + 13; b = b * 30 / 100 + 34;   /* dimmed, toward the launcher's night */
            o[0] = (uint8_t)(S.bgra ? b : r); o[1] = (uint8_t)g; o[2] = (uint8_t)(S.bgra ? r : b); o[3] = 255;
        }
    }
}

void post()
{
    ps3_osd_menu_state* M = &g_ps3_osd_menu;
    ps3_osd_menu_lock();
    M->px = S.px.data(); M->w = S.W; M->h = S.H; M->rev = ++S.rev;
    ps3_osd_menu_unlock();
    if (g_ps3_osd_present_again) g_ps3_osd_present_again();
}

/* the window's size and byte order as the renderer last reported: the picture made again when they change */
bool resize()
{
    const ps3_osd_menu_state* M = &g_ps3_osd_menu;
    uint32_t w = __atomic_load_n(&M->want_w, __ATOMIC_ACQUIRE), h = __atomic_load_n(&M->want_h, __ATOMIC_ACQUIRE);
    const int bgra = __atomic_load_n(&M->bgra, __ATOMIC_ACQUIRE);
    if ((!w || !h) && getenv("DOTHEROES_OVERLAY_DUMP")) { w = 1280; h = 720; }   /* a test offscreen: composed anyway */
    if (!w || !h) return false;
    if (w == S.W && h == S.H && bgra == S.bgra && !S.px.empty()) return true;
    ps3_osd_menu_lock();   /* the renderer never copies a buffer being replaced */
    S.W = w; S.H = h; S.bgra = bgra;
    S.px.assign((size_t)w * h * 4, 0);
    g_ps3_osd_menu.px = nullptr;
    ps3_osd_menu_unlock();
    make_background();
    return true;
}

void redraw()
{
    if (!resize()) return;
    compose();
    post();
    if (const char* d = getenv("DOTHEROES_OVERLAY_DUMP")) {   /* tests: each picture as composed */
        static unsigned n;
        char path[1024];
        snprintf(path, sizeof path, "%s/overlay-%03u.ppm", d, ++n);
        if (FILE* f = fopen(path, "wb")) {
            fprintf(f, "P6\n%u %u\n255\n", S.W, S.H);
            for (size_t i = 0; i < (size_t)S.W * S.H; i++) {
                const uint8_t* p = &S.px[i * 4];
                const uint8_t rgb[3] = { p[S.bgra ? 2 : 0], p[1], p[S.bgra ? 0 : 2] };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
}

void open_overlay(int kind)
{
    cellPad_ui_intercept(1);
    dotheroes_host_pause(1);
    g_vals = read_cfg();
    __atomic_store_n(&g_ps3_osd_menu.snap_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ps3_osd_menu.snap_want, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 40 && !__atomic_load_n(&g_ps3_osd_menu.snap_ready, __ATOMIC_ACQUIRE); i++) {   /* <= 0.4 s */
        if (g_ps3_osd_present_again) g_ps3_osd_present_again();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    S.open = true;
    S.page = P_MAIN;
    S.focus = kind == 2 ? (int)page_items(P_MAIN).size() - 1 : 0;
    S.top = 0;
    S.note.clear();
    S.dlg = Dialog();
    if (kind == 2) S.dlg = { true, 2, 0 };
    S.px.clear();   /* the background read again */
    __atomic_store_n(&g_ps3_osd_menu.open, 1, __ATOMIC_RELEASE);
    redraw();
    dh_log(DH_LOG_INFO, "overlay", "opened%s", kind == 2 ? " (the exit question)" : "");
}

/* the menu hidden at once; the game released only once every button and key is up (loop: S.closing), so nothing
 * pressed to close the menu reaches it */
void close_overlay()
{
    __atomic_store_n(&g_ps3_osd_menu.open, 0, __ATOMIC_RELEASE);
    if (g_ps3_osd_present_again) g_ps3_osd_present_again();
    S.open = false;
    S.closing = true;
    S.closing_ms = 0;
}

void release_game()
{
    S.closing = false;
    dotheroes_host_pause(0);
    cellPad_ui_intercept(0);
    dh_log(DH_LOG_INFO, "overlay", "closed");
}

/* the game ends: Return to Launcher (`launcher`) or Exit Game (`exit`) -- the launcher reads which
 * (<install>/logs/session-end) when the game's process ends; reopened here when it had closed itself */
void end_session(const char* how)
{
    if (!g_root.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_root + "/logs", ec);
        if (FILE* f = fopen((g_root + "/logs/session-end").c_str(), "w")) { fprintf(f, "%s\n", how); fclose(f); }
    }
    dh_log(DH_LOG_INFO, "overlay", "%s", !strcmp(how, "exit") ? "Exit Game: the game and the launcher close"
                                                               : "Return to Launcher: the game closes");
#ifdef _WIN32
    const char* l = getenv("CUBIC_LAUNCHER");
    const char* pid = getenv("CUBIC_LAUNCHER_PID");
    bool running = false;
    if (pid && *pid) {
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(pid, nullptr, 10))) {
            running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
            CloseHandle(h);
        }
    }
    if (!strcmp(how, "launcher") && l && *l && !running) {   /* the launcher closed itself when the game started */
        std::string cmd = std::string("\"") + l + "\" --after-game";
        STARTUPINFOA si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB,
                           nullptr, nullptr, &si, &pi) ||
            CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
#endif
    fflush(stdout);
    fflush(stderr);
    /* the game released (its pad still withheld) so its own shutdown runs: the request to exit goes to its
     * callbacks, then the process exits with its exit handlers (the settings, the caches, the log flushed) */
    __atomic_store_n(&g_ps3_osd_menu.open, 0, __ATOMIC_RELEASE);
    S.open = false;
    dotheroes_host_pause(0);
    ps3_host_quit(!strcmp(how, "exit") ? "Exit Game" : "Return to Launcher");
}

/* ---- the bindings: DOTHEROES_PAD_MAP "Cross=Circle,..." (the game's button = the pad's, by their PlayStation
 * names), DOTHEROES_KEY_MAP "Cross=Space,..." (the game's button = an SDL key name); only what differs ---- */
int game_bit(const std::string& name)
{
    for (int i = 0; i < 16; i++) if (name == kGameButton[i]) return i;
    return -1;
}

void parse_map(const std::string& v, void (*each)(int, const std::string&))
{
    size_t a = 0;
    while (a < v.size()) {
        size_t c = v.find(',', a);
        if (c == std::string::npos) c = v.size();
        const std::string kv = v.substr(a, c - a);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const int g = game_bit(kv.substr(0, eq));
            if (g >= 0) each(g, kv.substr(eq + 1));
        }
        a = c + 1;
    }
}

void load_bindings()
{
    for (int i = 0; i < 16; i++) { g_pad_bind[i] = i; g_key_bind[i].clear(); }
    auto p = g_vals.find("DOTHEROES_PAD_MAP");
    if (p != g_vals.end()) parse_map(p->second, [](int g, const std::string& v) { const int b = game_bit(v); if (b >= 0) g_pad_bind[g] = b; });
    auto k = g_vals.find("DOTHEROES_KEY_MAP");
    if (k != g_vals.end()) parse_map(k->second, [](int g, const std::string& v) { g_key_bind[g] = v; });
    for (int i = 0; i < 16; i++) {
        if (cellPad_set_pad_binding) cellPad_set_pad_binding(i, 1u << g_pad_bind[i]);
        if (cellPad_set_key_binding) cellPad_set_key_binding(i, g_key_bind[i].c_str());
    }
}

void save_bindings(bool pad)
{
    std::string v;
    for (int i = 0; i < 16; i++) {
        if (pad ? g_pad_bind[i] == i : g_key_bind[i].empty()) continue;
        v += std::string(v.empty() ? "" : ",") + kGameButton[i] + "=" + (pad ? kGameButton[g_pad_bind[i]] : g_key_bind[i]);
    }
    const char* key = pad ? "DOTHEROES_PAD_MAP" : "DOTHEROES_KEY_MAP";
    g_vals[key] = v;
    write_setting(key, v);
    dh_log(DH_LOG_INFO, "settings", "%s=%s (from the in-game menu)", key, v.c_str());
}

void restore_bindings(bool pad)
{
    for (int i = 0; i < 16; i++) {
        if (pad) { g_pad_bind[i] = i; if (cellPad_set_pad_binding) cellPad_set_pad_binding(i, 1u << i); }
        else { g_key_bind[i].clear(); if (cellPad_set_key_binding) cellPad_set_key_binding(i, ""); }
    }
    save_bindings(pad);
    S.note = pad ? "The controller's buttons are back to the defaults." : "The keys are back to the defaults.";
}

/* the pad's button `b` now presses the game's `g`; the game's button that had it takes g's old one (no clash) */
void bind_pad(int g, int b)
{
    const int old = g_pad_bind[g];
    S.note.clear();
    for (int i = 0; i < 16; i++)
        if (i != g && g_pad_bind[i] == b) {
            g_pad_bind[i] = old;
            if (cellPad_set_pad_binding) cellPad_set_pad_binding(i, 1u << old);
            S.note = std::string("Swapped: ") + kGameButton[i] + " is now on " + pad_button_name(old, prompt_style()) + ".";
        }
    g_pad_bind[g] = b;
    if (cellPad_set_pad_binding) cellPad_set_pad_binding(g, 1u << b);
    save_bindings(true);
}

void bind_key(int g, const std::string& name)
{
    if (!cellPad_key_binding_name || !cellPad_set_key_binding) return;
    const std::string old = cellPad_key_binding_name(g);
    S.note.clear();
    for (int i = 0; i < 16; i++)
        if (i != g && name == cellPad_key_binding_name(i)) {
            g_key_bind[i] = old;
            cellPad_set_key_binding(i, old.c_str());
            S.note = std::string("Swapped: ") + kGameButton[i] + " is now on " + old + ".";
        }
    g_key_bind[g] = name;
    cellPad_set_key_binding(g, name.c_str());
    save_bindings(false);
}

void change(int dir)
{
    std::vector<Item> items = page_items(S.page);
    if (S.focus < 0 || S.focus >= (int)items.size() || !items[S.focus].key) return;
    const Item& it = items[S.focus];
    const std::string cur = val(it);
    int at = 0;
    for (size_t i = 0; i < it.choices.size(); i++) if (cur == it.choices[i].v) at = (int)i;
    at = (at + dir + (int)it.choices.size()) % (int)it.choices.size();
    const std::string v = it.choices[at].v;
    g_vals[it.key] = v;
    write_setting(it.key, v);
    const int now = apply(it, v);
    S.note = now ? "" : "This takes effect when the game next starts.";
    dh_log(DH_LOG_INFO, "settings", "%s=%s (from the in-game menu; %s)", it.key, v.c_str(), now ? "applied now" : "at the next start");
}

void activate()
{
    if (S.dlg.open) {
        if (S.dlg.focus == 0) { S.dlg.open = false; return; }
        end_session(S.dlg.kind == 1 ? "launcher" : "exit");
        return;
    }
    if (S.page == P_CONTROLS && S.focus >= 3) {   /* the binding pages */
        S.page = S.focus == 3 ? P_PAD : P_KEYS;
        S.focus = 0;
        S.note.clear();
        return;
    }
    if (S.page == P_PAD || S.page == P_KEYS) {
        if (S.focus >= 16) { restore_bindings(S.page == P_PAD); return; }
        g_capture = kBindOrder[S.focus];
        g_capture_ms = 0;
        g_capture_armed = false;
        S.note = S.page == P_PAD ? "Press the controller button for it (5 s)." : "Press the key for it (5 s; Esc keeps it).";
        return;
    }
    if (S.page == P_DEV) {   /* M11.0b: a Developer page's row */
        if (dev_menu_pick(S.focus) == 1) { close_overlay(); return; }
        g_dev_view = dev_menu_view();
        S.focus = dev_menu_focus();
        S.top = 0;
        S.note = g_dev_view.note;
        return;
    }
    if (S.page != P_MAIN) { change(1); return; }
    const std::vector<Item> items = page_items(P_MAIN);   /* by its label: Developer comes and goes with the mode */
    if (S.focus < 0 || S.focus >= (int)items.size()) return;
    const std::string l = items[S.focus].label;
    S.main_focus = S.focus;
    if (l == "Resume Game") { close_overlay(); return; }
    if (l == "Display & Graphics") { S.page = P_DISPLAY; S.focus = 0; return; }
    if (l == "Audio") { S.page = P_AUDIO; S.focus = 0; return; }
    if (l == "Controls") { S.page = P_CONTROLS; S.focus = 0; return; }
    if (l == "Performance") { S.page = P_PERF; S.focus = 0; return; }
    if (l == "Developer") {
        dev_menu_reset();
        g_dev_view = dev_menu_view();
        S.page = P_DEV; S.focus = 0; S.top = 0;
        S.note = g_dev_view.note;
        return;
    }
    if (l == "Return to Launcher") { S.dlg = { true, 1, 0 }; return; }
    if (l == "Exit Game") { S.dlg = { true, 2, 0 }; return; }
}

void back()
{
    if (S.dlg.open) { S.dlg.open = false; return; }
    if (S.page == P_PAD || S.page == P_KEYS) { S.focus = S.page == P_PAD ? 3 : 4; S.page = P_CONTROLS; S.note.clear(); return; }
    if (S.page == P_DEV && dev_menu_back()) {   /* M11.0b: up one Developer page */
        g_dev_view = dev_menu_view();
        S.focus = dev_menu_focus();
        S.top = 0;
        S.note = g_dev_view.note;
        return;
    }
    if (S.page != P_MAIN) { S.page = P_MAIN; S.focus = S.main_focus; S.top = 0; S.note.clear(); return; }
    close_overlay();
}

/* tests (DOTHEROES_OVERLAY_TEST=1): overlay.input in the game's working directory, words one at a time:
 * open, exit, up, down, left, right, a, b, f1, esc, wait */
std::vector<std::string> g_script;
std::string test_word()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DOTHEROES_OVERLAY_TEST"); on = e && *e == '1'; }
    if (!on) return "";
    if (g_script.empty()) {
        if (FILE* f = fopen("overlay.input", "rb")) {
            char w[64];
            while (fscanf(f, "%63s", w) == 1) g_script.push_back(w);
            fclose(f);
            remove("overlay.input");
        }
    }
    if (g_script.empty()) return "";
    std::string w = g_script.front();
    g_script.erase(g_script.begin());
    if (w != "wait") dh_log(DH_LOG_INFO, "overlay", "test input: %s", w.c_str());
    return w;
}

int close_request(int why)
{
    if (S.open) {
        if (why == 0 && S.dlg.open && S.dlg.kind == 2) return 0;   /* closing the window again at the question: close */
        if (why == 0) { g_request = 2; return 1; }
        return 1;   /* Escape: the menu's own Back */
    }
    g_request = why == 0 ? 2 : 1;
    return 1;
}

void loop()
{
    using clk = std::chrono::steady_clock;
    uint32_t last_b = 0xFFFFFFFFu, last_k = 0xFFFFFFFFu;
    int last_mb = 0, held_ms = 0, rep_ms = 0;
    bool armed = true;
    const auto t0 = clk::now();
    bool hinted = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        uint32_t ly = 128;
        uint32_t b = cellPad_host_buttons(&ly);
        int mx, my, mb;
        uint32_t k = cellPad_host_keys(&mx, &my, &mb);
        if (ly < 64) b |= B_UP;
        if (ly > 192) b |= B_DOWN;
        const std::string w = test_word();
        uint32_t tb = 0, tk = 0;
        if (w == "up") tb = B_UP; else if (w == "down") tb = B_DOWN; else if (w == "left") tb = B_LEFT;
        else if (w == "right") tb = B_RIGHT; else if (w == "a") tb = B_CROSS; else if (w == "b") tb = B_CIRCLE;
        else if (w == "f1") tk = K_F1; else if (w == "esc") tk = K_ESC;
        else if (w == "open") g_request = 1; else if (w == "exit") g_request = 2;
        if (last_b == 0xFFFFFFFFu) { last_b = b; last_k = k; }
        const uint32_t eb = (b & ~last_b) | tb, ek = (k & ~last_k) | tk;
        const int emb = mb && !last_mb;
        last_b = b; last_k = k; last_mb = mb;
        if (S.closing) {   /* every button up (or 3 s): the game released */
            S.closing_ms += 16;
            if ((!b && !k && !mb) || S.closing_ms >= 3000) release_game();
            continue;
        }
        if (!S.open && (__atomic_load_n(&g_ps3_osd_saves.open, __ATOMIC_ACQUIRE) ||
                        __atomic_load_n(&g_ps3_osd_list.open, __ATOMIC_ACQUIRE))) {
            g_request = 0;   /* the game's own save screen is up: the menu waits */
            continue;
        }
        if (!S.open) {
            const uint32_t combo = g_combo.load();
            if ((b & combo) == combo && armed) {
                held_ms += 16;
                if (held_ms >= 500) { armed = false; held_ms = 0; open_overlay(1); continue; }
            } else {
                held_ms = 0;
                if (!(b & combo)) armed = true;   /* released: it can open again */
            }
            const int req = g_request.exchange(0);
            if (req || (ek & K_F1)) { open_overlay(req == 2 ? 2 : 1); continue; }
            if (!hinted && clk::now() - t0 > std::chrono::seconds(4)) {
                hinted = true;
                auto f = g_vals.find("DOTHEROES_OVERLAY_HINT");
                if (f == g_vals.end() || f->second != "0") {
                    char t[48];
                    snprintf(t, sizeof t, "HOLD %s OR PRESS F1: SETTINGS", combo_name(combo));
                    ps3_osd_post(t, 6000);
                }
            }
            continue;
        }
        /* open: a binding row waiting for its button or key -- once everything is released, the first pressed */
        if (g_capture >= 0) {
            g_capture_ms += 16;
            const uint32_t raw = cellPad_host_buttons(nullptr) | (tb & ~(B_UP | B_DOWN | B_LEFT | B_RIGHT));
            bool done = false;
            if (!g_capture_armed) {
                if (!raw && !k) g_capture_armed = true;
            } else if (S.page == P_PAD && raw) {
                int bit = 0;
                while (!(raw & (1u << bit))) bit++;
                bind_pad(g_capture, bit);
                done = true;
            } else if (S.page == P_KEYS) {
                char nm[64];
                if (k & K_ESC) { S.note = "Unchanged."; done = true; }
                else if (cellPad_host_key_held && cellPad_host_key_held(nm, sizeof nm)) { bind_key(g_capture, nm); done = true; }
            }
            if (!done && g_capture_ms > 5000) { S.note = "Nothing was pressed: unchanged."; done = true; }
            if (done) { g_capture = -1; redraw(); }
            continue;
        }
        const int req = g_request.exchange(0);
        if (req == 2) { S.dlg = { true, 2, 0 }; redraw(); }
        bool dirty = false;
        const uint32_t dir = b & (B_UP | B_DOWN | B_LEFT | B_RIGHT);
        if (dir && !(eb & dir)) rep_ms += 16; else rep_ms = 0;
        const bool rep = rep_ms >= 400 && (rep_ms - 400) % 112 < 16;
        const bool up = (eb & B_UP) || (ek & K_UP) || (rep && (b & B_UP));
        const bool down = (eb & B_DOWN) || (ek & K_DOWN) || (rep && (b & B_DOWN));
        const bool left = (eb & B_LEFT) || (ek & K_LEFT) || (rep && (b & B_LEFT));
        const bool right = (eb & B_RIGHT) || (ek & K_RIGHT) || (rep && (b & B_RIGHT));
        const int n = S.dlg.open ? 2 : (int)page_items(S.page).size();
        int& f = S.dlg.open ? S.dlg.focus : S.focus;
        if (S.dlg.open) {
            if (left || up) { f = 0; dirty = true; }
            if (right || down) { f = 1; dirty = true; }
        } else {
            if (up) { f = (f + n - 1) % n; dirty = true; }
            if (down) { f = (f + 1) % n; dirty = true; }
            if (S.page == P_DEV && (left || right)) {   /* a long list: a panel's worth at a time */
                f = std::max(0, std::min(n - 1, f + (left ? -S.vis : S.vis)));
                dirty = true;
            } else if (S.page != P_MAIN && (left || right)) { change(left ? -1 : 1); dirty = true; }
        }
        /* the mouse: over a row focuses it, a click chooses it */
        if (mx >= 0 && !g_rows.empty()) {
            static int lmx = -1, lmy = -1;
            for (size_t i = 0; i < g_rows.size(); i++) {
                const Row& r = g_rows[i];
                if (mx >= r.x0 && mx < r.x1 && my >= r.y0 && my < r.y1) {
                    const int want = S.dlg.open ? (int)i - (int)(g_rows.size() - 2) : S.top + (int)i;
                    if (want >= 0 && want < n && (mx != lmx || my != lmy) && f != want) { f = want; dirty = true; }
                    if (emb && want == f) { activate(); dirty = true; }
                }
            }
            lmx = mx; lmy = my;
        }
        const uint32_t combo = g_combo.load();
        if ((b & combo) == combo && armed) {   /* the shortcut again: resume */
            held_ms += 16;
            if (held_ms >= 500) { armed = false; held_ms = 0; S.dlg.open = false; close_overlay(); }
        } else { held_ms = 0; if (!(b & combo)) armed = true; }
        if (S.open) {
            if ((eb & (B_CROSS | B_START)) || (ek & (K_ENTER | K_SPACE))) { activate(); dirty = true; }
            else if ((eb & B_CIRCLE) || (ek & (K_ESC | K_BACK))) { back(); dirty = true; }
            else if (ek & K_F1) { S.dlg.open = false; close_overlay(); }
        }
        if (!S.open) {
            /* closed: held until every button and key is up, so nothing pressed to close it reaches the game */
            continue;
        }
        if (dirty) redraw();
        else {   /* the window kept alive while the game is held; the picture made again after a resize */
            static int tick = 0;
            if (++tick % 6 == 0) {
                const uint32_t w2 = __atomic_load_n(&g_ps3_osd_menu.want_w, __ATOMIC_ACQUIRE);
                if (w2 != S.W) redraw(); else if (g_ps3_osd_present_again) g_ps3_osd_present_again();
            }
        }
    }
}

}  // namespace

/* main.cpp: the overlay's thread, once the game runs (the settings file and the install's folder known) */
extern "C" void dotheroes_overlay_start(const char* settings_path, const char* install_root)
{
    g_cfg = settings_path ? settings_path : "";
    g_root = install_root ? install_root : "";
    g_vals = read_cfg();
    auto f = g_vals.find("DOTHEROES_OVERLAY_COMBO");
    g_combo = combo_mask(f == g_vals.end() ? "l3r3" : f->second);
    cellPad_set_chord_guard(g_combo, 120);
    apply_volumes();
    load_bindings();
    g_ps3_host_close_request = close_request;
    std::thread(loop).detach();
    dh_log(DH_LOG_INFO, "overlay", "ready: %s held, or F1", combo_name(g_combo.load()));
}
