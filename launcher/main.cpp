// Cubic.Heroes' launcher (M9.4; docs/M9_LAUNCHER_PLAN.md sections 3, 8 and 14): the single entry point for installing,
// configuring, updating, maintaining and playing. A native window (SDL2, Dear ImGui) driving the pipeline worker
// (tools/worker.py, docs/M9_WORKER_PROTOCOL.md); usable with a controller alone -- every page, the file browser and
// an on-screen keyboard -- or with a keyboard and mouse, the button prompts following the last device used.
#include <SDL.h>

#include "imgui.h"
#include "imgui_internal.h"   // the focus and navigation log (CUBICHEROES_DEBUG_NAV)
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "worker_client.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;
using launcher::Job;

// ============================================================================================================
// paths and the launcher's own small settings (window placement, the install folder, the last choices)
// ============================================================================================================

static std::string g_exe_dir;

static std::string path_str(const fs::path& p)
{
#ifdef _WIN32
    return launcher::narrow(p.wstring());
#else
    return p.string();
#endif
}

static fs::path u8path(const std::string& s)
{
#ifdef _WIN32
    return fs::path(launcher::widen(s));
#else
    return fs::path(s);
#endif
}

static std::string known_folder(int which)
{
#ifdef _WIN32
    static const GUID ids[] = { FOLDERID_RoamingAppData, FOLDERID_LocalAppData, FOLDERID_Documents, FOLDERID_Downloads,
                                FOLDERID_Desktop, FOLDERID_Profile };
    PWSTR p = nullptr;
    std::string out;
    if (SHGetKnownFolderPath(ids[which], 0, nullptr, &p) == S_OK) out = launcher::narrow(p);
    CoTaskMemFree(p);
    return out;
#else
    const char* h = getenv("HOME");
    std::string home = h ? h : "/";
    static const char* sub[] = { "/.config", "/.local/share", "/Documents", "/Downloads", "/Desktop", "" };
    return home + sub[which];
#endif
}
enum { kAppData, kLocalAppData, kDocuments, kDownloads, kDesktop, kHome };

static std::string data_dir()
{
    fs::path p = u8path(known_folder(kAppData)) / (
#ifdef _WIN32
        "CubicHeroes"
#else
        "cubic-heroes"
#endif
    );
    std::error_code ec;
    fs::create_directories(p, ec);
    return path_str(p);
}

static std::string default_install()
{
#ifdef _WIN32
    return path_str(u8path(known_folder(kLocalAppData)) / "Cubic.Heroes");
#else
    return path_str(u8path(known_folder(kHome)) / "Games" / "Cubic.Heroes");
#endif
}

static std::string read_text(const std::string& path, size_t limit = 4u << 20)
{
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > limit) s = s.substr(s.size() - limit);
    return s;
}

static bool write_text(const std::string& path, const std::string& text)
{
    const fs::path p = u8path(path), tmp = u8path(path + ".part");
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << text;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

struct Prefs {
    std::string install, disc, keys;
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, w = 1280, h = 800;
    bool maximized = false;
    bool prepared_once = false;   // shaders prepared at least once: a new build's empty cache is an update's
    bool steam_cleanup = false;   // an uninstall left the launcher's Steam entry (Steam ran): removed once it's closed

    std::string file() const { return path_str(u8path(data_dir()) / "launcher.json"); }
    void load()
    {
        json::Value v;
        if (!json::parse(read_text(file()), v)) return;
        install = v["install"].str();
        disc = v["disc"].str();
        keys = v["keys"].str();
        if (v["window"]["w"].num() >= 640 && v["window"]["h"].num() >= 400) {
            x = (int)v["window"]["x"].num(x); y = (int)v["window"]["y"].num(y);
            w = (int)v["window"]["w"].num(); h = (int)v["window"]["h"].num();
        }
        maximized = v["window"]["maximized"].truthy();
        prepared_once = v["prepared_once"].truthy();
        steam_cleanup = v["steam_cleanup"].truthy();
    }
    void save() const
    {
        char win[200];
        snprintf(win, sizeof win, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"maximized\":%s}", x, y, w, h,
                 maximized ? "true" : "false");
        write_text(file(), "{\n \"install\": " + json::quote(install) + ",\n \"disc\": " + json::quote(disc) +
                               ",\n \"keys\": " + json::quote(keys) + ",\n \"window\": " + win +
                               ",\n \"prepared_once\": " + (prepared_once ? "true" : "false") +
                               ",\n \"steam_cleanup\": " + (steam_cleanup ? "true" : "false") + "\n}\n");
    }
} g_prefs;

// The worker: CUBIC_WORKER (a full command, for development) or the packaged app beside the launcher
// (<launcher>/worker/DotHeroes-Setup.exe --worker), else a source checkout found above the launcher (its venv's Python
// and tools/worker.py)
static std::vector<std::string> worker_argv(const std::string& cmd, const std::vector<std::string>& args)
{
    std::vector<std::string> av;
    fs::path exe = u8path(g_exe_dir);
#ifdef _WIN32
    const fs::path packaged = exe / "worker" / "DotHeroes-Setup.exe";
#else
    const fs::path packaged = exe / "worker" / "DotHeroes-Setup";
#endif
    std::error_code ec;
    if (fs::exists(packaged, ec)) {
        av = { path_str(packaged), "--worker" };
    } else {
        for (fs::path p = exe; !p.empty(); p = p.parent_path()) {
            if (fs::exists(p / "tools" / "worker.py", ec)) {
#ifdef _WIN32
                fs::path py = p / "build" / "venv" / "Scripts" / "python.exe";
                av = { fs::exists(py, ec) ? path_str(py) : std::string("python"), path_str(p / "tools" / "worker.py") };
#else
                fs::path py = p / "build" / "venv" / "bin" / "python3";
                av = { fs::exists(py, ec) ? path_str(py) : std::string("python3"), path_str(p / "tools" / "worker.py") };
#endif
                break;
            }
            if (p == p.parent_path()) break;
        }
    }
    av.push_back(cmd);
    for (const auto& a : args) av.push_back(a);
    av.push_back("--control");
    av.push_back("--log");
    av.push_back(path_str(u8path(data_dir()) / "logs" / ("worker-" + cmd + ".jsonl")));
    return av;
}

// ============================================================================================================
// look (docs/M9_LAUNCHER_PLAN.md section 16): the game's own world as the reference -- a toy-block fantasy in
// chunky pixels: a night-blue sky, voxel grass and earth, retro windows with light borders and stepped corners,
// gold for what is chosen, a bobbing pixel cursor. Every element is drawn here, in code -- the pixel font, the
// icons, the blocks: no image files, nothing taken from the game. Body text stays a clean system font.
// ============================================================================================================

namespace col {
const ImU32 bg = IM_COL32(14, 18, 44, 255), panel = IM_COL32(24, 34, 84, 250), panel2 = IM_COL32(34, 48, 108, 255);
const ImU32 accent = IM_COL32(255, 206, 72, 255), accent_dim = IM_COL32(255, 206, 72, 70);
const ImU32 warm = IM_COL32(255, 168, 76, 255), text = IM_COL32(250, 246, 232, 255), dim = IM_COL32(198, 206, 234, 255);
const ImU32 danger = IM_COL32(255, 108, 96, 255), ok = IM_COL32(128, 226, 108, 255);
const ImU32 ink = IM_COL32(8, 8, 22, 255), frame = IM_COL32(238, 234, 216, 255), inset = IM_COL32(14, 20, 54, 255);
const ImU32 steel = IM_COL32(58, 78, 152, 255), gold = IM_COL32(240, 182, 44, 255), brick = IM_COL32(176, 60, 52, 255);
const ImU32 grass = IM_COL32(92, 184, 72, 255), grass_hi = IM_COL32(156, 226, 104, 255);
const ImU32 dirt = IM_COL32(116, 76, 46, 255), dirt_dk = IM_COL32(84, 54, 34, 255);
}  // namespace col

static ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static ImFont* g_font = nullptr;   // the body font, drawn at several sizes (Dear ImGui 1.92+'s dynamic fonts)
static float g_scale = 1.0f;
static float S(float px) { return px * g_scale; }
static float snap(float v) { return floorf(v + 0.5f); }
static const float kBody = 23.0f, kSmall = 19.0f, kTitle = 34.0f, kHuge = 54.0f;   // couch- and handheld-readable

// a colour under the current alpha (a page fading in, a disabled button)
static ImU32 A(ImU32 c)
{
    const float a = ImGui::GetStyle().Alpha;
    if (a >= 1.0f) return c;
    const ImU32 al = (ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}
static ImU32 shade(ImU32 c, float f)
{
    auto ch = [&](int s) { return (ImU32)std::min(255.0f, (float)((c >> s) & 0xFF) * f) << s; };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

// --- the pixel font: the project's own 5x7 capitals, digits and signs (port/ui_pixel_font.h, shared with the game) ---
#include "../port/ui_pixel_font.h"
static float pix_w(const char* t, float p)
{
    int cols = 0, n = 0;
    for (const char* s = t; *s; s++, n++) cols += (int)strlen(pix_glyph(*s)->rows[0]);
    return n ? (float)(cols + n - 1) * p : 0.0f;
}
// a line of pixel text at pos (its top left), each row's runs drawn as one block; a shadow below and right
static void pix_text(ImDrawList* dl, ImVec2 pos, const char* t, float p, ImU32 c, ImU32 shadow = col::ink)
{
    p = std::max(1.0f, floorf(p));
    const float so = std::max(1.0f, floorf(p * 0.5f));
    for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
        float x = snap(pos.x) + (pass ? 0 : so);
        const float y = snap(pos.y) + (pass ? 0 : so);
        const ImU32 cc = A(pass ? c : shadow);
        for (const char* s = t; *s; s++) {
            const PixGlyph* g = pix_glyph(*s);
            const int w = (int)strlen(g->rows[0]);
            for (int r = 0; r < 7; r++)
                for (int i = 0; i < w; i++) {
                    if (g->rows[r][i] != '#') continue;
                    int j = i;
                    while (j + 1 < w && g->rows[r][j + 1] == '#') j++;
                    dl->AddRectFilled(ImVec2(x + i * p, y + r * p), ImVec2(x + (j + 1) * p, y + (r + 1) * p), cc);
                    i = j;
                }
            x += (w + 1) * p;
        }
    }
}
// the pixel size for a title of `size` body-pixels
static float pix_size(float size) { return std::max(2.0f, floorf(S(size) / 8.5f + 0.5f)); }

// --- the pixel icons: 10x10, up to four colours ('#', '+', '*', 'g'), an ink outline drawn around them ---
struct PixIcon { const char* rows[10]; ImU32 pal[4]; };
static const PixIcon kIcons[] = {
    { { "....++....", "....+#....", "....+#....", "....+#....", "....+#....", "....+#....", "..******..", "....gg....",
        "....gg....", "...****..." },   // a sword: Play
      { IM_COL32(150, 170, 206, 255), IM_COL32(244, 246, 255, 255), IM_COL32(255, 200, 64, 255), IM_COL32(150, 90, 48, 255) } },
    { { "..........", ".++++++++.", "gggggggggg", "gggggggggg", "**********", "gggg**gggg", "gggg##gggg", "gggggggggg",
        "gggggggggg", "##########" },   // a chest: Install
      { IM_COL32(96, 54, 28, 255), IM_COL32(214, 148, 84, 255), IM_COL32(255, 200, 64, 255), IM_COL32(172, 104, 52, 255) } },
    { { "...++##...", "...+###...", "...+###...", "...+###...", ".+#######.", "..+#####..", "...+###...", "....+#....",
        "..........", "**********" },   // an arrow down to the ground: Updates
      { IM_COL32(96, 200, 96, 255), IM_COL32(176, 240, 150, 255), IM_COL32(200, 190, 170, 255), 0 } },
    { { ".++++++++.", ".########.", ".########.", "....gg....", "....gg....", "....gg....", "....gg....", "....gg....",
        "....gg....", "....gg...." },   // a hammer: Builds
      { IM_COL32(150, 160, 184, 255), IM_COL32(224, 230, 244, 255), 0, IM_COL32(166, 102, 52, 255) } },
    { { "*#######..", "*########.", "*##****##.", "*##****##.", "*########.", "*########.", "*########.", "*########.",
        "*++++++++.", ".++++++++." },   // a book: Saves
      { IM_COL32(196, 64, 64, 255), IM_COL32(244, 234, 210, 255), IM_COL32(255, 200, 64, 255), 0 } },
    { { "....##....", "...+###...", "..++####..", ".++######.", "++########", ".********.", "..******..", "...****...",
        "....**....", ".........." },   // a gem: Shaders
      { IM_COL32(84, 200, 236, 255), IM_COL32(210, 250, 255, 255), IM_COL32(40, 120, 190, 255), 0 } },
    { { "....##....", ".#.####.#.", "..######..", ".###..###.", "####..####", "####..####", ".###..###.", "..######..",
        ".#.####.#.", "....##...." },   // a gear: Settings
      { IM_COL32(176, 182, 204, 255), 0, 0, 0 } },
    { { "..####....", ".#++++#...", "#++++++#..", "#++++++#..", "#++++++#..", ".#++++#...", "..####g...", "......gg..",
        ".......gg.", "........gg" },   // a magnifying glass: Diagnostics
      { IM_COL32(214, 178, 90, 255), IM_COL32(170, 222, 250, 255), 0, IM_COL32(150, 90, 48, 255) } },
    { { "..........", ".####.....", "#++++####.", "#++++++++#", "##########", "##########", "##########", "##########",
        "##########", ".........." },   // a folder (the file browser)
      { IM_COL32(232, 176, 64, 255), IM_COL32(255, 224, 130, 255), 0, 0 } },
    { { "...####...", ".##++####.", ".#++#####.", "##+#######", "####..####", "####..####", "#########*", ".#######*.",
        "..##****..", "...####..." },   // a disc (a file in the browser)
      { IM_COL32(196, 204, 224, 255), IM_COL32(250, 250, 255, 255), IM_COL32(130, 140, 170, 255), 0 } },
};
enum { kIconFolder = 8, kIconDisc = 9 };
static void pix_icon(ImDrawList* dl, ImVec2 pos, const PixIcon& ic, float p)
{
    p = std::max(1.0f, floorf(p));
    pos = ImVec2(snap(pos.x), snap(pos.y));
    static const char keys[4] = { '#', '+', '*', 'g' };
    for (int pass = 0; pass < 2; pass++)
        for (int r = 0; r < 10; r++)
            for (int i = 0; i < 10; i++) {
                const char ch = ic.rows[r][i];
                if (ch == '.') continue;
                const ImVec2 a(pos.x + i * p, pos.y + r * p);
                if (pass == 0) { dl->AddRectFilled(ImVec2(a.x - p, a.y - p), ImVec2(a.x + 2 * p, a.y + 2 * p), A(col::ink)); continue; }
                for (int k = 0; k < 4; k++)
                    if (ch == keys[k] && ic.pal[k]) dl->AddRectFilled(a, ImVec2(a.x + p, a.y + p), A(ic.pal[k]));
            }
}
// the cursor: a pixel arrow pointing right (7 rows), white with an ink outline
static void pix_arrow(ImDrawList* dl, ImVec2 pos, float p, ImU32 c = IM_COL32_WHITE)
{
    static const char* rows[7] = { "#....", "##...", "###..", "####.", "###..", "##...", "#...." };
    pos = ImVec2(snap(pos.x), snap(pos.y));
    for (int pass = 0; pass < 2; pass++)
        for (int r = 0; r < 7; r++)
            for (int i = 0; i < 5; i++) {
                if (rows[r][i] != '#') continue;
                const ImVec2 a(pos.x + i * p, pos.y + r * p);
                if (pass == 0) dl->AddRectFilled(ImVec2(a.x - p, a.y - p), ImVec2(a.x + 2 * p, a.y + 2 * p), A(col::ink));
                else dl->AddRectFilled(a, ImVec2(a.x + p, a.y + p), A(c));
            }
}

// --- blocks and windows ---
// a raised block's edges over its fill: lit above and left, shaded below and right, an ink outline
static void bevel_edges(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, float t, bool sunken = false)
{
    t = std::max(1.0f, floorf(t));
    ImU32 hi = A(shade(fill, 1.38f)), lo = A(shade(fill, 0.58f));
    if (sunken) std::swap(hi, lo);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + t), hi);
    dl->AddRectFilled(a, ImVec2(a.x + t, b.y), hi);
    dl->AddRectFilled(ImVec2(a.x, b.y - t), b, lo);
    dl->AddRectFilled(ImVec2(b.x - t, a.y + t), b, lo);
    dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), A(col::ink), 0.0f, std::max(2.0f, floorf(t * 0.7f)));
}
static void bevel_block(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, float t)
{
    dl->AddRectFilled(a, b, A(fill));
    bevel_edges(dl, a, b, fill, t);
}
// a retro window's frame: a light border with stepped corners, ink outside it, a dark line inside
static void retro_frame(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 border = col::frame, float t = 0)
{
    if (t <= 0) t = std::max(2.0f, floorf(S(4)));
    const float o = std::max(2.0f, floorf(S(2)));
    const ImU32 ink = A(col::ink), br = A(border);
    // ink outside, the corners cut
    dl->AddRectFilled(ImVec2(a.x + t, a.y - o), ImVec2(b.x - t, a.y), ink);
    dl->AddRectFilled(ImVec2(a.x + t, b.y), ImVec2(b.x - t, b.y + o), ink);
    dl->AddRectFilled(ImVec2(a.x - o, a.y + t), ImVec2(a.x, b.y - t), ink);
    dl->AddRectFilled(ImVec2(b.x, a.y + t), ImVec2(b.x + o, b.y - t), ink);
    for (int cx = 0; cx < 2; cx++)
        for (int cy = 0; cy < 2; cy++) {
            const float x = cx ? b.x - t : a.x, y = cy ? b.y - t : a.y;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + t, y + t), ink);   // the cut corner
            const float ix = cx ? b.x - 2 * t : a.x + t, iy = cy ? b.y - 2 * t : a.y + t;
            dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + t, iy + t), br);   // the step inside it
        }
    // the border
    dl->AddRectFilled(ImVec2(a.x + t, a.y), ImVec2(b.x - t, a.y + t), br);
    dl->AddRectFilled(ImVec2(a.x + t, b.y - t), ImVec2(b.x - t, b.y), br);
    dl->AddRectFilled(ImVec2(a.x, a.y + t), ImVec2(a.x + t, b.y - t), br);
    dl->AddRectFilled(ImVec2(b.x - t, a.y + t), ImVec2(b.x, b.y - t), br);
}
// the current window (a popup, a card) framed, drawn inside its rectangle and past its clipping
static void frame_window(ImU32 border = col::frame, float t = 0)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    const float o = std::max(2.0f, floorf(S(2)));
    const ImVec2 b(a.x + sz.x, a.y + sz.y);
    const ImRect vis = ImGui::GetCurrentWindow()->OuterRectClipped;   // a card scrolled partly away: only what shows
    dl->PushClipRect(vis.Min, vis.Max, false);
    retro_frame(dl, ImVec2(a.x + o, a.y + o), ImVec2(b.x - o, b.y - o), border, t);
    dl->PopClipRect();
}
// an isometric voxel: its top, left and right faces, outlined (the logo's blocks)
static void voxel(ImDrawList* dl, ImVec2 c, float s, ImU32 top, ImU32 left, ImU32 right)
{
    const ImVec2 t(c.x, c.y - s), r(c.x + s * 0.87f, c.y - s * 0.5f), b(c.x, c.y), l(c.x - s * 0.87f, c.y - s * 0.5f);
    const ImVec2 bl(l.x, l.y + s), bb(b.x, b.y + s), br(r.x, r.y + s);
    dl->AddQuadFilled(t, r, b, l, A(top));
    dl->AddQuadFilled(l, b, bb, bl, A(left));
    dl->AddQuadFilled(b, r, br, bb, A(right));
    const ImVec2 hull[6] = { t, r, br, bb, bl, l };
    const float w = std::max(2.0f, floorf(S(2.5f)));
    dl->AddPolyline(hull, 6, A(col::ink), w, ImDrawFlags_Closed);
    dl->AddLine(l, b, A(shade(top, 0.7f)), w * 0.6f);
    dl->AddLine(b, r, A(shade(top, 0.7f)), w * 0.6f);
    dl->AddLine(b, bb, A(col::ink), w * 0.6f);
}

static void apply_theme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.ChildRounding = 0; s.FrameRounding = 0; s.PopupRounding = 0; s.GrabRounding = 0;
    s.WindowPadding = ImVec2(30, 26); s.FramePadding = ImVec2(20, 12);
    s.ItemSpacing = ImVec2(46, 14);   // wide between buttons side by side: the cursor sits in the gap
    s.ItemInnerSpacing = ImVec2(10, 8); s.ScrollbarSize = 16; s.ScrollbarRounding = 0; s.WindowBorderSize = 0;
    s.ChildBorderSize = 0; s.PopupBorderSize = 0; s.FrameBorderSize = 0; s.DisabledAlpha = 0.6f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = v4(col::bg); c[ImGuiCol_ChildBg] = v4(col::panel2); c[ImGuiCol_PopupBg] = v4(IM_COL32(24, 34, 84, 255));
    c[ImGuiCol_Text] = v4(col::text); c[ImGuiCol_TextDisabled] = v4(col::dim);
    c[ImGuiCol_Button] = v4(col::steel); c[ImGuiCol_ButtonHovered] = v4(shade(col::steel, 1.15f));
    c[ImGuiCol_ButtonActive] = v4(shade(col::steel, 0.85f));
    c[ImGuiCol_FrameBg] = v4(col::inset); c[ImGuiCol_FrameBgHovered] = v4(IM_COL32(24, 32, 76, 255));
    c[ImGuiCol_FrameBgActive] = v4(IM_COL32(30, 40, 92, 255));
    c[ImGuiCol_Header] = v4(IM_COL32(44, 62, 136, 255)); c[ImGuiCol_HeaderHovered] = v4(IM_COL32(58, 80, 168, 255));
    c[ImGuiCol_HeaderActive] = v4(IM_COL32(120, 96, 40, 255));
    c[ImGuiCol_NavCursor] = v4(col::accent); c[ImGuiCol_PlotHistogram] = v4(col::grass);
    c[ImGuiCol_ScrollbarBg] = v4(col::inset); c[ImGuiCol_ScrollbarGrab] = v4(col::steel);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(shade(col::steel, 1.2f)); c[ImGuiCol_ScrollbarGrabActive] = v4(col::gold);
    c[ImGuiCol_ModalWindowDimBg] = v4(IM_COL32(4, 6, 20, 190)); c[ImGuiCol_CheckMark] = v4(col::accent);
    c[ImGuiCol_SliderGrab] = v4(col::accent); c[ImGuiCol_Separator] = v4(col::steel);
    c[ImGuiCol_TextSelectedBg] = v4(IM_COL32(255, 206, 72, 90));
}

// ============================================================================================================
// input: the device last used decides the prompts; page actions from the controller or their keys
// ============================================================================================================

enum class Device { Pad, Keys };
static Device g_device = Device::Keys;
static bool g_saw_input = false;   // the first input decides the prompts; before it, a connected controller does
static bool g_pad_connected = false;

enum class Act { A, B, X, Y, LB, RB, Start, Back };

// false while the page is drawn under a dialog: the page's own buttons and prompts belong to the dialog then
static bool g_page_input = true;
static std::string g_focus_label;   // the focused item's name, for the scripted controller test's log
static bool g_focus_owns_lr = false;  // the focused item uses left and right itself (a settings value)
static bool g_quit = false;   // the launcher closes after this frame
static bool g_nav_test = false;   // a scripted controller test (--nav-test) drives the launcher: the game never starts
static std::string g_nav_event;   // what the test's log notes beside the next step
static float g_nav_rstick = 0;    // the test's right stick (beside a real controller's own, which the backend reports)

static bool pressed(Act a)
{
    using namespace ImGui;
    if (!g_page_input) return false;
    switch (a) {
    case Act::A: return IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    case Act::B: return IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || IsKeyPressed(ImGuiKey_Escape, false);
    case Act::X: return IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || IsKeyPressed(ImGuiKey_X, false);
    case Act::Y: return IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || IsKeyPressed(ImGuiKey_Y, false);
    case Act::LB: return IsKeyPressed(ImGuiKey_GamepadL1, false) || IsKeyPressed(ImGuiKey_Q, false) ||
                         IsKeyPressed(ImGuiKey_PageUp, false);
    case Act::RB: return IsKeyPressed(ImGuiKey_GamepadR1, false) || IsKeyPressed(ImGuiKey_E, false) ||
                         IsKeyPressed(ImGuiKey_PageDown, false);
    case Act::Start: return IsKeyPressed(ImGuiKey_GamepadStart, false) || IsKeyPressed(ImGuiKey_F10, false);
    case Act::Back: return IsKeyPressed(ImGuiKey_GamepadBack, false) || IsKeyPressed(ImGuiKey_F1, false);
    }
    return false;
}

// which controller's buttons the prompts show: the one used last (SDL's type for it, then its name; Steam Input's
// virtual pads report the controller behind them), or the player's choice in Settings (LAUNCHER_PROMPTS)
enum class PadStyle { Xbox, PlayStation, Nintendo, Steam, Generic };
static PadStyle g_pad_detected = PadStyle::Xbox;
static std::string g_prompt_choice = "auto";
static PadStyle style_from(SDL_GameControllerType type, const char* name)
{
    std::string n = name ? name : "";
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)tolower(c); });
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    if (has("steam")) return PadStyle::Steam;
    switch (type) {
    case SDL_CONTROLLER_TYPE_XBOX360: case SDL_CONTROLLER_TYPE_XBOXONE: return PadStyle::Xbox;
    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5: return PadStyle::PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return PadStyle::Nintendo;
    default: break;
    }
    if (has("xbox") || has("xinput")) return PadStyle::Xbox;
    if (has("playstation") || has("dualshock") || has("dualsense") || has("ps4") || has("ps5") || has("wireless controller"))
        return PadStyle::PlayStation;
    if (has("nintendo") || has("switch") || has("joy-con") || has("pro controller")) return PadStyle::Nintendo;
    return PadStyle::Generic;
}
static std::vector<SDL_GameController*> g_pads;   // the controllers connected (their left sticks steer the focus)
static void stick_nav(ImGuiIO& io)
{
    static int held = -1;
    static double next = 0;
    float x = 0, y = 0;
    for (SDL_GameController* gc : g_pads) {
        const float ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        const float ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        if (fabsf(ax) + fabsf(ay) > fabsf(x) + fabsf(y)) { x = ax; y = ay; }
    }
    int dir = -1;   // left, right, up, down
    if (fabsf(x) > 0.55f && fabsf(x) >= fabsf(y)) dir = x < 0 ? 0 : 1;
    else if (fabsf(y) > 0.55f) dir = y < 0 ? 2 : 3;
    const double now = SDL_GetTicks() / 1000.0;
    if (dir < 0) { held = -1; return; }
    if (dir != held) { held = dir; next = now + 0.35; }
    else if (now < next) return;
    else next = now + 0.12;
    static const ImGuiKey keys[4] = { ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                                      ImGuiKey_GamepadDpadDown };
    io.AddKeyEvent(keys[dir], true);   // released by the next frame's real D-pad state
}

static PadStyle pad_style()
{
    const std::string& c = g_prompt_choice;
    if (c == "xbox") return PadStyle::Xbox;
    if (c == "playstation") return PadStyle::PlayStation;
    if (c == "nintendo") return PadStyle::Nintendo;
    if (c == "steam") return PadStyle::Steam;
    if (c == "generic") return PadStyle::Generic;
    return g_pad_detected;
}

// a 7x7 pixel symbol: PlayStation's cross, circle, square, triangle; a generic pad's four positions (one filled)
static void pad_symbol(ImDrawList* dl, ImVec2 c, float px, int sym, ImU32 col)
{
    static const char* shapes[4][7] = {
        { "#.....#", ".#...#.", "..#.#..", "...#...", "..#.#..", ".#...#.", "#.....#" },
        { "..###..", ".#...#.", "#.....#", "#.....#", "#.....#", ".#...#.", "..###.." },
        { "#######", "#.....#", "#.....#", "#.....#", "#.....#", "#.....#", "#######" },
        { "...#...", "..#.#..", "..#.#..", ".#...#.", ".#...#.", "#.....#", "#######" } };
    const ImVec2 o(snap(c.x - 3.5f * px), snap(c.y - 3.5f * px));
    if (sym < 4) {
        for (int r = 0; r < 7; r++)
            for (int i = 0; i < 7; i++)
                if (shapes[sym][r][i] == '#')
                    dl->AddRectFilled(ImVec2(o.x + i * px, o.y + r * px), ImVec2(o.x + (i + 1) * px, o.y + (r + 1) * px), A(col));
        return;
    }
    // a generic pad: south, east, west, north as four blocks, the one meant filled
    const int on = sym - 4;
    const ImVec2 at[4] = { ImVec2(3, 5), ImVec2(5, 3), ImVec2(1, 3), ImVec2(3, 1) };
    for (int k = 0; k < 4; k++) {
        const ImVec2 a0(o.x + (at[k].x - 1) * px, o.y + (at[k].y - 1) * px), a1(a0.x + 2 * px, a0.y + 2 * px);
        if (k == on) dl->AddRectFilled(a0, a1, A(col));
        else dl->AddRect(a0, a1, A(shade(col, 0.6f)), 0.0f, std::max(1.0f, floorf(px * 0.5f)));
    }
}

// a button's glyph, in pixels: the controller's face buttons as round-cornered blocks (in the style of the controller in
// use: Xbox's coloured letters, PlayStation's symbols, Nintendo's and Steam's letters, a generic pad's positions),
// bumpers and menu buttons as steel blocks, or a parchment keycap
static float glyph(ImDrawList* dl, ImVec2 p, Act a, float h)
{
    const bool pad = g_device == Device::Pad;
    const char* t = "";
    ImU32 fill = col::steel;
    bool round = false;
    int sym = -1;
    ImU32 symcol = IM_COL32_WHITE;
    if (pad) {
        const PadStyle st = pad_style();
        const int face = a == Act::A ? 0 : a == Act::B ? 1 : a == Act::X ? 2 : a == Act::Y ? 3 : -1;   // S, E, W, N
        if (face >= 0) {
            round = true;
            static const char* letters[4] = { "A", "B", "X", "Y" };
            static const ImU32 xbox[4] = { IM_COL32(64, 176, 74, 255), IM_COL32(214, 60, 60, 255),
                                           IM_COL32(44, 112, 220, 255), IM_COL32(222, 168, 30, 255) };
            static const ImU32 ps[4] = { IM_COL32(120, 160, 255, 255), IM_COL32(255, 100, 100, 255),
                                         IM_COL32(240, 130, 210, 255), IM_COL32(90, 220, 170, 255) };
            static const int ps_sym[4] = { 0, 1, 2, 3 };   // cross, circle, square, triangle
            switch (st) {
            case PadStyle::Xbox: t = letters[face]; fill = xbox[face]; break;
            case PadStyle::PlayStation: fill = IM_COL32(36, 38, 52, 255); sym = ps_sym[face]; symcol = ps[face]; break;
            case PadStyle::Nintendo: t = letters[face]; fill = IM_COL32(72, 74, 84, 255); break;
            case PadStyle::Steam: t = letters[face]; fill = IM_COL32(32, 36, 48, 255); break;
            case PadStyle::Generic: fill = IM_COL32(72, 78, 96, 255); sym = 4 + face; break;
            }
        } else {
            const bool sony = st == PadStyle::PlayStation, nin = st == PadStyle::Nintendo, gen = st == PadStyle::Generic;
            const bool steam = st == PadStyle::Steam;
            switch (a) {
            case Act::LB: t = sony || steam ? "L1" : nin ? "L" : "LB"; break;
            case Act::RB: t = sony || steam ? "R1" : nin ? "R" : "RB"; break;
            case Act::Start: t = sony ? "OPTIONS" : nin ? "+" : gen ? "START" : "MENU"; break;
            case Act::Back: t = sony ? "CREATE" : nin ? "-" : gen ? "SELECT" : "VIEW"; break;
            default: break;
            }
        }
    } else {
        switch (a) {
        case Act::A: t = "Enter"; break;
        case Act::B: t = "Esc"; break;
        case Act::X: t = "X"; break;
        case Act::Y: t = "Y"; break;
        case Act::LB: t = "Q"; break;
        case Act::RB: t = "E"; break;
        case Act::Start: t = "F10"; break;
        case Act::Back: t = "F1"; break;
        }
    }
    const float px = std::max(2.0f, floorf(h / 11.0f));   // the label's pixel
    const float tw = pix_w(t, px);
    const float w = round ? h : std::max(h, tw + px * 6);   // (a symbol: a round face)
    if (!dl) return w;   // measured, not drawn
    p = ImVec2(snap(p.x), snap(p.y));
    const ImVec2 b(p.x + w, p.y + h);
    const float n = floorf(h * 0.22f);   // the stepped corners
    const ImU32 face = pad ? fill : IM_COL32(232, 224, 200, 255);
    const ImU32 ink = A(col::ink);
    if (round) {   // an octagon of blocks: ink, the face, a highlight
        dl->AddRectFilled(ImVec2(p.x + n, p.y - 2), ImVec2(b.x - n, b.y + 2), ink);
        dl->AddRectFilled(ImVec2(p.x - 2, p.y + n), ImVec2(b.x + 2, b.y - n), ink);
        dl->AddRectFilled(ImVec2(p.x + n * 0.5f - 1, p.y + n * 0.5f - 1), ImVec2(b.x - n * 0.5f + 1, b.y - n * 0.5f + 1), ink);
        dl->AddRectFilled(ImVec2(p.x + n, p.y), ImVec2(b.x - n, b.y), A(face));
        dl->AddRectFilled(ImVec2(p.x, p.y + n), ImVec2(b.x, b.y - n), A(face));
        dl->AddRectFilled(ImVec2(p.x + n * 0.5f, p.y + n * 0.5f), ImVec2(b.x - n * 0.5f, b.y - n * 0.5f), A(face));
        dl->AddRectFilled(ImVec2(p.x + n, p.y + px), ImVec2(b.x - n, p.y + 2 * px), A(shade(face, 1.4f)));
        dl->AddRectFilled(ImVec2(p.x + n, b.y - 2 * px), ImVec2(b.x - n, b.y - px), A(shade(face, 0.65f)));
    } else {
        bevel_block(dl, p, b, face, px);
    }
    if (sym >= 0) {
        pad_symbol(dl, ImVec2(p.x + w / 2, p.y + h / 2), std::max(2.0f, floorf(h / 12.0f)), sym, symcol);
        return w;
    }
    pix_text(dl, ImVec2(p.x + (w - tw) / 2, p.y + (h - 7 * px) / 2 - px * 0.5f), t, px,
             pad ? IM_COL32_WHITE : IM_COL32(40, 30, 24, 255), pad ? col::ink : 0);
    return w;
}

struct Prompt { Act act; std::string label; };
static std::vector<Prompt> g_prompts;   // this frame's, drawn in the bottom bar
static void prompt(Act a, const std::string& label)
{
    if (g_page_input) g_prompts.push_back({ a, label });
}

// ============================================================================================================
// widgets: big buttons with a clear focus, cards, a selector, status lines
// ============================================================================================================

// the focus: a chunky gold ring with stepped corners, pulsing gently, and the pixel cursor bobbing at its left --
// drawn past the window's clipping (the cursor sits in the margin), only while the item can be seen
static void focus_ring(float = 0.0f, ImU32 = 0)
{
    if (!ImGui::IsItemFocused() || !ImGui::GetIO().NavVisible || !ImGui::IsItemVisible()) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    dl->PushClipRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), false);
    const float t = (float)ImGui::GetTime();
    const float pulse = 0.5f + 0.5f * sinf(t * 4.5f);
    const ImU32 ring = IM_COL32(255, (int)(206 + 40 * pulse), (int)(72 + 120 * pulse), 255);
    const float g = floorf(S(7)), w = std::max(3.0f, floorf(S(4)));
    const float gap = std::max(2.0f, floorf(S(3)));   // ink between the ring and the item: clear on any colour
    dl->AddRect(ImVec2(a.x - g + w, a.y - g + w), ImVec2(b.x + g - w, b.y + g - w), col::ink, 0.0f, gap);
    retro_frame(dl, ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), ring, w);
    const float p = std::max(3.0f, floorf(S(4)));
    const float bob = floorf((0.5f + 0.5f * sinf(t * 7.0f)) * 2.5f) * p;
    pix_arrow(dl, ImVec2(a.x - g - p * 7 - bob, (a.y + b.y) / 2 - p * 3.5f), p);
    dl->PopClipRect();
}

enum class Kind { Normal, Primary, Danger };

// a block button: steel, gold for the main choice, brick for what removes something; a pixel label when `pixel`
static bool big_button(const char* label, ImVec2 size = ImVec2(0, 0), Kind k = Kind::Normal, bool enabled = true,
                       float pixel = 0.0f)
{
    if (!enabled) k = Kind::Normal;   // a choice not open yet is plain steel, its label still readable
    const ImU32 base = k == Kind::Primary ? col::gold : k == Kind::Danger ? col::brick : col::steel;
    const ImU32 ink_text = k == Kind::Primary ? IM_COL32(52, 30, 8, 255) : col::text;
    ImGui::PushStyleColor(ImGuiCol_Button, v4(base));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(shade(base, 1.14f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(shade(base, 0.86f)));
    ImGui::PushStyleColor(ImGuiCol_Text, v4(ink_text));
    if (size.y == 0) size.y = S(56);
    ImGui::BeginDisabled(!enabled);
    const std::string id = pixel > 0 ? std::string("##") + label : std::string(label);
    const bool r = ImGui::Button(id.c_str(), size);
    if (ImGui::IsItemFocused()) {
        const char* e = strstr(label, "##");
        g_focus_label = e ? std::string(label, e) : std::string(label);
    }
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool down = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = down ? shade(base, 0.86f) : hov ? shade(base, 1.14f) : base;
    bevel_edges(dl, a, b, fill, S(4), down);
    if (pixel > 0) {
        const char* end = strstr(label, "##");
        const std::string text = end ? std::string(label, end) : std::string(label);
        const float px = floorf(pixel), tw = pix_w(text.c_str(), px);
        pix_text(dl, ImVec2((a.x + b.x - tw) / 2, (a.y + b.y - 7 * px) / 2 + (down ? px * 0.5f : 0)), text.c_str(), px,
                 ink_text, k == Kind::Primary ? shade(base, 1.45f) : col::ink);
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    focus_ring();
    return r;
}

static void text_col(ImU32 c, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, v4(c));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}

// a title in the pixel font, in the current text colour, its words wrapped to the room there is
static void title(const char* t, float size = kTitle)
{
    const float p = pix_size(size);
    const float room = std::max(p * 12, ImGui::GetContentRegionAvail().x);
    const ImU32 c = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Text));   // A() adds the alpha
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string line, word;
    std::vector<std::string> lines;
    std::istringstream in(t);
    while (in >> word) {
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && pix_w(next.c_str(), p) > room) { lines.push_back(line); line = word; }
        else line = next;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    for (size_t i = 0; i < lines.size(); i++) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        pix_text(dl, ImVec2(at.x, at.y + p), lines[i].c_str(), p, c);
        ImGui::Dummy(ImVec2(pix_w(lines[i].c_str(), p), p * 9));
    }
}

// a card: a sunken retro window inside the page
static bool card_begin(const char* id, ImVec2 size = ImVec2(0, 0))
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(24)));
    // flattened: a card's buttons are reached directly, as the page's own (not the card first, then its buttons)
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened |
                                                     (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0), 0);
    ImGui::PopStyleVar();
    frame_window(IM_COL32(150, 166, 220, 255), std::max(2.0f, floorf(S(3))));
    return open;
}
static void card_end() { ImGui::EndChild(); }

// a progress bar of blocks: green segments filling a sunken trough; `frac` < 0: working, its share unknown
static void pix_bar(float frac, float h)
{
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, A(col::inset));
    bevel_edges(dl, a, b, col::inset, std::max(2.0f, floorf(S(2))), true);
    const float pad = std::max(2.0f, floorf(S(4))), seg = std::max(6.0f, floorf(S(14))), gap = std::max(2.0f, floorf(S(3)));
    const float x0 = a.x + pad, x1 = b.x - pad, y0 = a.y + pad, y1 = b.y - pad;
    float from = x0, to = x0 + (x1 - x0) * std::min(1.0f, std::max(0.0f, frac));
    if (frac < 0) {   // a group of blocks travelling along the trough
        const float span = seg * 5, t = (float)ImGui::GetTime();
        from = x0 + fmodf(t * S(260), (x1 - x0) + span) - span;
        to = from + span;
    }
    for (float x = x0; x < x1; x += seg + gap) {
        const float l = std::max(x, from), r = std::min(std::min(x + seg, x1), to);
        if (r <= l) continue;
        dl->AddRectFilled(ImVec2(l, y0), ImVec2(r, y1), A(col::grass));
        dl->AddRectFilled(ImVec2(l, y0), ImVec2(r, y0 + std::max(1.0f, floorf((y1 - y0) * 0.3f))), A(col::grass_hi));
    }
}

static std::string human_bytes(double b)
{
    char s[64];
    if (b >= 1024.0 * 1024 * 1024) snprintf(s, sizeof s, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024.0 * 1024) snprintf(s, sizeof s, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024) snprintf(s, sizeof s, "%.0f KB", b / 1024);
    else snprintf(s, sizeof s, "%.0f bytes", b);
    return s;
}

static std::string human_time(double sec)
{
    char s[64];
    if (sec < 60) snprintf(s, sizeof s, "%.0f s", sec);
    else if (sec < 3600) snprintf(s, sizeof s, "%d min %02d s", (int)sec / 60, (int)sec % 60);
    else snprintf(s, sizeof s, "%d h %02d min", (int)sec / 3600, (int)sec / 60 % 60);
    return s;
}

// ============================================================================================================
// the worker's operations: one long one at a time (shown over everything, cancellable); quick queries behind
// ============================================================================================================

struct Op {
    std::unique_ptr<Job> job;
    std::string title, command;
    bool long_op = false, cancelled = false, finished = false;
    std::vector<std::pair<std::string, std::string>> plan;   // id, name
    std::map<std::string, std::string> state;               // id -> start/done/skipped
    std::string stage, detail, eta_basis;
    double fraction = -1, overall = 0, elapsed = 0, eta = -1;
    std::deque<std::string> log;
    json::Value result;
    std::function<void(const json::Value&)> then;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

static std::unique_ptr<Op> g_op;                   // the long operation, if any
static std::vector<std::unique_ptr<Op>> g_queries; // quick ones
static std::unique_ptr<Op> g_bg;                   // the shaders' preparation, in the background (one at a time)
struct BgDone { bool shown = false, ok = false; std::string title, text; float t0 = 0; };
static BgDone g_bg_done;                           // how it ended, shown in the bar for a while
static bool g_play_after_bg = false;    // the preparation is being paused for the game
static bool g_prepare_after_game = false;   // a preparation asked for while the game ran: once it has closed
static SDL_Window* g_window = nullptr;
static void show_msg(const std::string& t, const std::string& text);

static Op* run(const std::string& cmd, const std::vector<std::string>& args, const std::string& title, bool long_op,
               std::function<void(const json::Value&)> then)
{
    // what changes the active build or its shaders waits for the background preparation (one job on the cache)
    static const std::set<std::string> exclusive = { "install", "update", "migrate", "rollback", "prepare" };
    if (long_op && g_bg && exclusive.count(cmd)) {
        show_msg("Shaders are being prepared", "Wait for the preparation at the bottom to finish, or stop it first "
                 "(the VIEW button, or F1): what it made is kept.");
        return nullptr;
    }
    auto op = std::make_unique<Op>();
    op->job = std::make_unique<Job>();
    op->title = title;
    op->command = cmd;
    op->long_op = long_op;
    op->then = std::move(then);
    if (!op->job->start(worker_argv(cmd, args), cmd)) {
        json::Value r;
        json::parse("{\"ev\":\"result\",\"ok\":false,\"code\":\"no_worker\",\"message\":\"The launcher's worker could not be "
                    "started.\"}", r);
        op->result = r;
        op->finished = true;
    }
    Op* p = op.get();
    if (long_op) g_op = std::move(op);
    else g_queries.push_back(std::move(op));
    return p;
}

static void pump(Op& op)
{
    for (json::Value& e : op.job->take_events()) {
        const std::string ev = e["ev"].str();
        if (ev == "plan") {
            op.plan.clear();
            for (size_t i = 0; i < e["stages"].size(); i++)
                op.plan.push_back({ e["stages"][i]["id"].str(), e["stages"][i]["name"].str() });
        } else if (ev == "stage") {
            op.state[e["id"].str()] = e["state"].str();
            if (e["state"].str() == "start") { op.stage = e["id"].str(); op.fraction = -1; }
        } else if (ev == "progress") {
            op.stage = e["stage"].str();
            op.fraction = e["fraction"].is_null() ? -1 : e["fraction"].num();
            op.overall = e["overall"].num(op.overall);
            op.elapsed = e["elapsed"].num(op.elapsed);
            op.eta = e["eta"].is_null() ? -1 : e["eta"].num();
            op.eta_basis = e["eta_basis"].str();
            op.detail = e["detail"].str();
        } else if (ev == "log") {
            op.log.push_back(e["text"].str());
            if (op.log.size() > 200) op.log.pop_front();
        } else if (ev == "result") {
            op.result = e;
        }
    }
    if (!op.finished && op.job->done()) {
        for (json::Value& e : op.job->take_events())
            if (e["ev"].str() == "result") op.result = e;
        op.finished = true;
        if (op.result.is_null())
            json::parse("{\"ev\":\"result\",\"ok\":false,\"code\":\"internal\",\"message\":\"The worker ended without a "
                        "result.\"}", op.result);
    }
}

// ============================================================================================================
// overlays: a message or confirmation, the file browser, the on-screen keyboard
// ============================================================================================================

struct Confirm {
    bool open = false;
    std::string title, text, yes;
    bool danger = false;
    std::function<void()> on_yes;
    std::string no = "Cancel";   // the other choice's label
    std::function<void()> on_no;
    bool focus_yes = false;      // an offer: the recommended choice has the focus (else the safe one, "no")
} g_confirm;

static void ask(const std::string& t, const std::string& text, const std::string& yes, bool danger, std::function<void()> f)
{
    g_confirm = Confirm();
    g_confirm.open = true;
    g_confirm.title = t; g_confirm.text = text; g_confirm.yes = yes; g_confirm.danger = danger;
    g_confirm.on_yes = std::move(f);
}

struct Message {
    bool open = false;
    std::string title, text, details;
    bool error = false, show_details = false;
} g_msg;

static void show_msg(const std::string& t, const std::string& text) { g_msg = { true, t, text, "", true, false }; }

static void show_result_error(const json::Value& r, const std::string& what)
{
    g_msg = { true, what, r["message"].str("Something went wrong."), r["details"].str(), true, false };
}

// --- the file browser: drives and known folders, folders first, then the files the filter takes ---
static std::string& browse_last()   // the folder last browsed this session
{
    static std::string last;
    return last;
}

struct Browser {
    bool open = false, folders = false;   // folders: a folder is what is chosen
    std::string title, dir, filter_desc;
    std::vector<std::string> exts;        // lower-case, with the dot; empty: any file
    std::function<void(const std::string&)> on_pick;
    struct Entry { std::string name, path; bool dir; uintmax_t size; };
    std::vector<Entry> entries;
    std::string error;
    int refocus = 4;   // frames left to put the focus on the first entry (see OpenFocus)

    void list()
    {
        entries.clear();
        error.clear();
        std::error_code ec;
        if (dir.empty()) {   // the top: drives and known places
#ifdef _WIN32
            DWORD m = GetLogicalDrives();
            for (int i = 0; i < 26; i++)
                if (m >> i & 1) {
                    std::string d = std::string(1, (char)('A' + i)) + ":\\";
                    entries.push_back({ d, d, true, 0 });
                }
#else
            entries.push_back({ "/", "/", true, 0 });
#endif
            return;
        }
        std::vector<Entry> ds, fl;
        for (fs::directory_iterator it(u8path(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            const fs::directory_entry& de = *it;
            std::string name = path_str(de.path().filename());
            if (name.empty() || name[0] == '.' || name[0] == '$') continue;
            std::error_code e2;
            if (de.is_directory(e2)) {
                ds.push_back({ name, path_str(de.path()), true, 0 });
            } else if (!folders) {
                std::string ext = path_str(de.path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (exts.empty() || std::find(exts.begin(), exts.end(), ext) != exts.end())
                    fl.push_back({ name, path_str(de.path()), false, de.file_size(e2) });
            }
        }
        if (ec) error = "This folder can't be read.";
        auto by = [](const Entry& a, const Entry& b) { return SDL_strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
        std::sort(ds.begin(), ds.end(), by);
        std::sort(fl.begin(), fl.end(), by);
        entries = ds;
        entries.insert(entries.end(), fl.begin(), fl.end());
    }
    std::string from;   // after "up a folder": the folder left, which takes the focus in its parent's list
    void go(const std::string& d)
    {
        dir = d;
        from.clear();
        if (!d.empty()) browse_last() = d;
        list();
        refocus = 4;
    }
    void up()
    {
        if (dir.empty()) { open = false; return; }
        fs::path p = u8path(dir);
        fs::path parent = p.parent_path();
        if (parent == p || path_str(p).size() <= 3) go("");
        else go(path_str(parent));
        for (const Entry& e : entries)
            if (u8path(e.path) == p) from = e.path;
    }
} g_browser;

static void browse(const std::string& t, bool folders, std::vector<std::string> exts, const std::string& desc,
                   const std::string& start, std::function<void(const std::string&)> f)
{
    g_browser.open = true;
    g_browser.title = t;
    g_browser.folders = folders;
    g_browser.exts = std::move(exts);
    g_browser.filter_desc = desc;
    g_browser.on_pick = std::move(f);
    // where it opens: the folder last browsed this session; else the install's folder; else the launcher's own
    (void)start;
    std::error_code ec;
    extern std::string install_dir_or_empty();
    std::string d = browse_last();
    if (d.empty() || !fs::is_directory(u8path(d), ec)) d = install_dir_or_empty();
    if (d.empty() || !fs::is_directory(u8path(d), ec)) d = g_exe_dir;
    g_browser.go(d);
}

// --- the on-screen keyboard: letters, digits and a few signs; A types, X deletes, Y a space, MENU done ---
struct Osk {
    bool open = false, shift = false;
    std::string title, text;
    std::function<void(const std::string&)> on_done;
} g_osk;

static void type_text(const std::string& t, const std::string& init, std::function<void(const std::string&)> f)
{
    g_osk = { true, false, t, init, std::move(f) };
}

// ============================================================================================================
// what the launcher knows of the install, refreshed from the worker
// ============================================================================================================

struct Status {
    json::Value info, builds, saves, settings, check, update;
    bool info_ok = false, checking_update = false, checked_once = false;
    std::string check_error;
    std::string playing;   // a running game's note
} g_st;

// the settings as the worker reported them; the log's detail handed on to the jobs the launcher starts (their
// environment: tools/worker.py) -- the game reads the same setting from the file itself
static void set_settings(const json::Value& v)
{
    g_st.settings = v;
    const std::string lv = g_st.settings["settings"]["DOTHEROES_LOG_LEVEL"]["value"].str();
    if (!lv.empty()) {
#ifdef _WIN32
        _putenv_s("DOTHEROES_LOG_LEVEL", lv.c_str());
#else
        setenv("DOTHEROES_LOG_LEVEL", lv.c_str(), 1);
#endif
    }
}

static std::string install_dir() { return g_prefs.install.empty() ? default_install() : g_prefs.install; }
std::string install_dir_or_empty() { return install_dir(); }

void auto_prepare_check_later();

static void refresh_info()
{
    run("info", { "--install", install_dir() }, "", false, [](const json::Value& r) {
        g_st.info = r["data"];
        g_st.info_ok = r["ok"].truthy();
        const std::string kind = g_st.info["kind"].str();
        if (kind == "slots") {
            run("builds", { "--install", install_dir() }, "", false, [](const json::Value& b) { g_st.builds = b["data"]; });
            run("saves", { "--install", install_dir() }, "", false, [](const json::Value& s) { g_st.saves = s["data"]; });
            run("prepare", { "--install", install_dir(), "--check" }, "", false, [](const json::Value& c) {
                g_st.check = c["data"];
                g_st.check_error = c["ok"].truthy() ? "" : c["message"].str();
                auto_prepare_check_later();
            });
        }
        if (kind == "slots" || kind == "setup-app")
            run("settings", { "--install", install_dir() }, "", false, [](const json::Value& s) {
                set_settings(s["data"]);
                auto_prepare_check_later();
            });
    });
}

static bool g_auto_check_due = false;   // a cache check arrived: the automatic preparation looks at it next frame
void auto_prepare_check_later() { g_auto_check_due = true; }

static std::string setting(const char* key, const char* dflt)
{
    const json::Value& v = g_st.settings["settings"][key]["value"];
    return v.is_null() ? dflt : v.str();
}

static void check_updates()
{
    g_st.checking_update = true;
    run("check-update", { "--channel", setting("LAUNCHER_UPDATE_CHANNEL", "stable") }, "", false, [](const json::Value& r) {
        g_st.update = r;
        g_st.checking_update = false;
        g_st.checked_once = true;
    });
}

// the preparation in the background: the launcher stays usable, the bar at the bottom shows it; one at a time (the
// worker also refuses a second for the same install, whoever starts it)
static bool game_running();
static bool bg_prepare(bool rebuild, const std::string& why)
{
    if (g_bg) return false;
    if (game_running()) {   // the game uses the cache and the GPU: after it closes
        g_prepare_after_game = true;
        g_bg_done = BgDone();
        g_bg_done.shown = true;
        g_bg_done.t0 = (float)ImGui::GetTime();
        g_bg_done.title = "Waiting for the game";
        g_bg_done.text = "Shaders are prepared when the game closes.";
        return false;
    }
    std::vector<std::string> a = { "--install", install_dir() };
    if (rebuild) a.push_back("--rebuild");
    auto op = std::make_unique<Op>();
    op->job = std::make_unique<Job>();
    op->title = rebuild ? "Rebuilding the shader cache" : "Preparing shaders";
    op->command = "prepare";
    op->detail = why;
    const auto t0 = std::chrono::steady_clock::now();
    op->then = [rebuild, t0](const json::Value& r) {
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        g_bg_done = BgDone();
        g_bg_done.shown = true;
        g_bg_done.t0 = (float)ImGui::GetTime();
        g_bg_done.ok = r["ok"].truthy();
        const json::Value& d = r["data"];
        char text[256];
        if (g_bg_done.ok && !d["pipelines"].is_null()) {   // M9.2: the comprehensive preparation
            snprintf(text, sizeof text, "%d shaders and %d pipelines ready for this GPU and driver (%d failed), in %.1f s.",
                     (int)d["translations"].num(), (int)d["pipelines"].num(), (int)d["failed_all"].num(), took);
            g_bg_done.title = rebuild ? "Shader cache rebuilt" : "Shaders ready";
            g_bg_done.text = text;
            g_prefs.prepared_once = true;
            g_prefs.save();
        } else if (g_bg_done.ok) {
            snprintf(text, sizeof text, "%d pipelines ready for this GPU and driver (%d failed), in %.1f s.",
                     (int)d["recipes"].num(), (int)d["failed"].num(), took);
            g_bg_done.title = rebuild ? "Shader cache rebuilt" : "Shaders ready";
            g_bg_done.text = text;
            g_prefs.prepared_once = true;
            g_prefs.save();
        } else if (r["code"].str() == "cancelled") {
            g_bg_done.title = "Preparation stopped";
            g_bg_done.text = "What was made is kept: preparing again continues from there.";
        } else {
            g_bg_done.title = "Preparation didn't finish";
            g_bg_done.text = r["message"].str("Something went wrong.");
            if (r["code"].str() != "already_preparing") show_result_error(r, "Preparing shaders didn't finish");
        }
        if (g_window && !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS))
            SDL_FlashWindow(g_window, SDL_FLASH_UNTIL_FOCUSED);   // the end, noticed from another window too
        refresh_info();
    };
    if (!op->job->start(worker_argv("prepare", a), "prepare")) {
        show_msg("The launcher's worker could not be started", "Shaders can't be prepared now.");
        return false;
    }
    g_bg = std::move(op);
    g_bg_done.shown = false;
    return true;
}

// a stop asked for: confirmed, then the worker told (it ends the game's --prepare; the parts done are kept)
static void bg_stop()
{
    if (!g_bg) return;
    ask("Stop preparing shaders?", "What is prepared so far is kept: preparing again continues from there. Until "
        "then, effects not prepared may stutter the first time they appear.", "Stop", true, [] {
        if (g_bg) g_bg->job->cancel();
    });
}

// exit, asked first (a preparation running is stopped: what it made is kept)
static void ask_exit()
{
    ask("Exit the launcher?", g_bg ? "Shaders are being prepared: exiting stops the preparation. What is prepared is kept."
                                   : "The game, if it is running, keeps running.", "Exit", false, [] {
        if (g_bg) g_bg->job->cancel();
        g_quit = true;
    });
    g_confirm.no = "Stay";
}

// after an install (or a move into the launcher's layout): the setting decides -- ask (the first time), on or off
static void after_install_shaders()
{
    if (setting("LAUNCHER_SHADERS_AFTER_UPDATE", "0") == "1") { bg_prepare(false, "after the install"); return; }
    ask("The game is installed", "Would you like to precompile shaders now? This is recommended to reduce stuttering "
        "during gameplay.", "Precompile Now (recommended)", false, [] {
        run("settings", { "--install", install_dir(), "--set", "LAUNCHER_SHADERS_AFTER_UPDATE=1",
                          "--set", "LAUNCHER_SHADERS_ON_DRIVER_CHANGE=1" },
            "", false, [](const json::Value& r) { if (r["ok"].truthy()) set_settings(r["data"]); });
        bg_prepare(false, "after the install");
    });
    g_confirm.no = "Skip for Now";
    g_confirm.focus_yes = true;
}

// M9.2: the shaders are ready when the cache is valid for this GPU and driver, the first-session list is in it and,
// where the comprehensive preparation can run (the profiles and the install's disc copy), it has run into this cache
static bool shaders_ready(const json::Value& c)
{
    return c["prepared"].truthy() && c["pending"].num() <= 0 &&
           (!c["comprehensive"].truthy() || c["comprehensive_done"].truthy());
}

static bool shaders_preparable(const json::Value& c)
{
    return c["recipes"].num() + c["pending"].num() > 0 || c["comprehensive"].truthy();
}

// at each cache check: a cache made invalid (another GPU or driver, damaged) or a new build's empty one (an update),
// prepared on its own when the settings say so -- once a session, never over another job
static void auto_prepare_check()
{
    static bool done = false;
    const json::Value& c = g_st.check;
    if (done || g_bg || g_op || c.is_null() || shaders_ready(c) || g_st.settings.is_null() || game_running()) return;
    if (!shaders_preparable(c)) return;
    const std::string state = c["cache"].str();
    const bool invalid = state.rfind("made ", 0) == 0 || state.rfind("damaged", 0) == 0;
    // a new build's empty cache, or a valid one the comprehensive preparation has not run into yet (an update)
    const bool new_build = (state == "none yet" || (c["prepared"].truthy() && c["comprehensive"].truthy())) &&
                           g_prefs.prepared_once;
    if (invalid && setting("LAUNCHER_SHADERS_ON_DRIVER_CHANGE", "0") == "1") {
        done = true;
        bg_prepare(false, "the cache was " + state);
    } else if (new_build && setting("LAUNCHER_SHADERS_AFTER_UPDATE", "0") == "1") {
        done = true;
        bg_prepare(false, "a new game build");
    }
}

// ============================================================================================================
// pages
// ============================================================================================================

enum Page { kPlay, kInstall, kUpdates, kSaves, kShaders, kSettings, kDiagnostics, kPages };
static const char* kPageNames[kPages] = { "Play", "Install", "Updates", "Saves", "Shaders", "Settings", "Diagnostics" };
static const int kPageIcon[kPages] = { 0, 1, 2, 4, 5, 6, 7 };   // kIcons: the sword, chest, arrow, book, gem, gear, glass
static int g_page = kPlay;
static bool g_focus_page = true;   // give the new page's first item the focus
static bool g_focus_menu = false;  // B on a page: the menu's row for it takes the focus

static void focus_first()
{
    if (g_focus_page && g_page_input) {   // not from under a dialog: the page's content can arrive after it opened
        ImGui::SetKeyboardFocusHere(-1);   // the item just drawn
        g_focus_page = false;
    }
}

#ifdef _WIN32
static HANDLE g_game_proc = nullptr;   // the game the launcher started, while it runs
#else
static pid_t g_game_pid = 0;
#endif
static bool game_running()
{
#ifdef _WIN32
    if (!g_game_proc) return false;
    if (WaitForSingleObject(g_game_proc, 0) == WAIT_TIMEOUT) return true;
    CloseHandle(g_game_proc);
    g_game_proc = nullptr;
    return false;
#else
    if (g_game_pid <= 0) return false;
    if (waitpid(g_game_pid, nullptr, WNOHANG) == 0) return true;
    g_game_pid = 0;
    return false;
#endif
}

static void start_game_now()
{
    if (g_nav_test) { g_nav_event = "  [the game would start: not during a scripted controller test]"; return; }
    const std::string exe = path_str(u8path(install_dir()) / "game" /
#ifdef _WIN32
                                     "dotheroes.exe"
#else
                                     "dotheroes"
#endif
    );
#ifdef _WIN32
    // the game's in-game menu: Return to Launcher reopens this launcher if it has closed itself, and says how the
    // session ended in <install>/logs/session-end (read when the game's process ends)
    {
        std::error_code ec;
        fs::remove(u8path(install_dir()) / "logs" / "session-end", ec);
        _putenv_s("CUBIC_LAUNCHER", path_str(u8path(g_exe_dir) / "CubicHeroes.exe").c_str());
        _putenv_s("CUBIC_LAUNCHER_PID", std::to_string((unsigned long)GetCurrentProcessId()).c_str());
    }
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = launcher::quote_arg(launcher::widen(exe));
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, launcher::widen(install_dir()).c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        if (g_game_proc) CloseHandle(g_game_proc);
        g_game_proc = pi.hProcess;
        g_st.playing = "The game is running.";
        if (setting("LAUNCHER_CLOSE_ON_START", "0") == "1") g_quit = true;   // the game reopens it (Return to Launcher)
    } else {
        g_msg = { true, "The game didn't start", "Its program couldn't be started. Try Repair on the Install page.",
                  exe, true, false };
    }
#else
    const pid_t pid = fork();
    if (pid == 0) { setsid(); execl(exe.c_str(), exe.c_str(), (char*)nullptr); _exit(127); }
    g_game_pid = pid;
    g_st.playing = "The game is running.";
#endif
}


static void start_game()
{
    if (!g_bg) { start_game_now(); return; }
    ask("Shaders are being prepared", "Playing now pauses the preparation, so the game and the preparation never share "
        "the shader cache or the GPU. What is prepared is kept; preparing again after playing continues from there.",
        "Pause and play", false, [] {
        if (g_bg) { g_play_after_bg = true; g_bg->job->cancel(); }
        else start_game_now();
    });
    g_confirm.no = "Keep preparing";
    g_confirm.focus_yes = true;
}

static void prepare_now(bool rebuild)
{
    bg_prepare(rebuild, rebuild ? "asked for" : "asked for");
}

// below the Play page's cards, when there is room: a small voxel landscape of the project's own -- a grass hill, a
// stream, a tree, a gold block on a stone -- drawn with the logo's voxels; still, but for the water's glint
static void play_landscape()
{
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
    if (h < S(110) || w < S(400)) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int Wmax = 10, D = 3;
    const float s = floorf(std::min(S(28), h / (5.6f + (Wmax + D) * 0.5f)));   // a block's edge
    const ImU32 grass_top = col::grass, side_l = col::dirt, side_r = col::dirt_dk;
    const ImU32 stone_t = IM_COL32(150, 156, 176, 255), stone_l = IM_COL32(112, 118, 140, 255), stone_r = IM_COL32(84, 88, 108, 255);
    const float t = (float)ImGui::GetTime();
    struct B { int gx, gy, gz; int kind; };   // kind: 0 grass, 1 water, 2 trunk, 3 leaves, 4 stone, 5 gold
    std::vector<B> bs;
    const int W = std::max(6, std::min(Wmax, (int)(w / (s * 0.87f)) - D - 2));
    // isometric: x along (gx - gz), y along (gx + gz), up by gy; centred, the tree's top within the room
    const float x0 = a.x + w * 0.5f - (W - D) * s * 0.87f * 0.5f, y0 = a.y + s * 5.0f;
    for (int x = 0; x < W; x++)
        for (int z = 0; z < D; z++) {
            const bool stream = x == W / 3 || (x == W / 3 + 1 && z >= 2);
            bs.push_back({ x, 0, z, stream ? 1 : 0 });
            if (!stream && x >= W - 4 && z <= 1) bs.push_back({ x, 1, z, 0 });   // the hill
        }
    bs.push_back({ W - 2, 2, 0, 2 });
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = 0; dz <= 1; dz++) bs.push_back({ W - 2 + dx, 3, dz, 3 });
    bs.push_back({ W - 2, 4, 0, 3 });
    bs.push_back({ 1, 1, 1, 4 }); bs.push_back({ 1, 2, 1, 5 });
    // back to front: the far diagonal first, low before high
    std::sort(bs.begin(), bs.end(), [](const B& p, const B& q) {
        if (p.gx + p.gz != q.gx + q.gz) return p.gx + p.gz < q.gx + q.gz;
        return p.gy < q.gy;
    });
    for (const B& b : bs) {
        const ImVec2 c(floorf(x0 + (b.gx - b.gz) * s * 0.87f), floorf(y0 + (b.gx + b.gz) * s * 0.5f - b.gy * s));
        ImU32 top = grass_top, l = side_l, r = side_r;
        if (b.kind == 1) { const int gl = (int)(30 * (0.5f + 0.5f * sinf(t * 1.5f + b.gx + b.gz)));
                           top = IM_COL32(60 + gl / 2, 130 + gl, 210, 255); l = IM_COL32(40, 90, 170, 255); r = IM_COL32(30, 70, 140, 255); }
        if (b.kind == 2) { top = IM_COL32(150, 100, 60, 255); l = IM_COL32(120, 76, 44, 255); r = IM_COL32(92, 58, 34, 255); }
        if (b.kind == 3) { top = IM_COL32(70, 170, 70, 255); l = IM_COL32(46, 130, 52, 255); r = IM_COL32(34, 100, 40, 255); }
        if (b.kind == 4) { top = stone_t; l = stone_l; r = stone_r; }
        if (b.kind == 5) { top = IM_COL32(255, 222, 110, 255); l = col::gold; r = IM_COL32(186, 126, 26, 255); }
        voxel(dl, c, s, top, l, r);
    }
    ImGui::Dummy(ImVec2(w, h - S(4)));
}

static void page_play()
{
    const std::string kind = g_st.info["kind"].str();
    ImGui::PushStyleColor(ImGuiCol_Text, v4(col::accent));
    title("3D Dot Game Heroes", kHuge);
    ImGui::PopStyleColor();
    text_col(col::dim, "Cubic.Heroes -- the native PC port, built on this PC from your own disc.");
    ImGui::Dummy(ImVec2(0, S(10)));
    if (!g_st.info_ok && g_st.info.is_null()) {
        text_col(col::dim, "Looking for the game...");
        return;
    }
    if (kind == "slots") {
        const json::Value& b = g_st.builds;
        const bool play = big_button("PLAY", ImVec2(S(360), S(92)), Kind::Primary, true, std::max(4.0f, floorf(S(7))));
        if (play) start_game();
        focus_first();
        if (ImGui::IsItemFocused()) prompt(Act::A, "Play");
        ImGui::SameLine(0, S(24));
        ImGui::BeginGroup();
        std::string version = "-";
        for (size_t i = 0; i < b["builds"].size(); i++)
            if (b["builds"][i]["state"].str() == "active") version = b["builds"][i]["version"].str("-");
        text_col(col::text, "Version %s", version.c_str());
        if (!g_st.playing.empty()) text_col(col::ok, "%s", g_st.playing.c_str());
        else text_col(col::dim, "Installed and ready.");
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, S(8)));
        // shaders
        card_begin("##shaders_card", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - S(8), S(214)));
        title("Shaders and pipelines", 24);
        const json::Value& c = g_st.check;
        if (g_bg) {
            text_col(col::accent, "Being prepared now: the bar at the bottom shows how far. You can play meanwhile; "
                                  "effects not prepared yet may stutter the first time.");
        } else if (!g_st.check_error.empty()) {
            text_col(col::danger, "No usable Vulkan device was found: the game can't start. %s", g_st.check_error.c_str());
        } else if (c.is_null()) {
            text_col(col::dim, "Checking...");
        } else if (shaders_ready(c)) {
            text_col(col::ok, "Shaders are ready for your graphics card.");
            text_col(col::dim, "%s", c["gpu"]["name"].str().c_str());
        } else if (shaders_preparable(c)) {
            text_col(col::warm, "Shaders need preparing: it takes under a minute and prevents stutter.");
            if (big_button("Prepare now")) prepare_now(false);
        } else {
            text_col(col::dim, "Prepared as you play: each new effect is kept for next time.");
        }
        card_end();
        ImGui::SameLine();
        card_begin("##update_card", ImVec2(0, S(214)));
        title("Updates", 24);
        const json::Value& u = g_st.update;
        if (g_st.checking_update) text_col(col::dim, "Checking...");
        else if (!g_st.checked_once) text_col(col::dim, "Not checked yet.");
        else if (!u["ok"].truthy()) text_col(col::dim, "%s", u["message"].str("Couldn't check.").c_str());
        else if (u["data"]["offline"].truthy()) text_col(col::dim, "Offline: the game plays as it is.");
        else if (u["data"]["available"].truthy() && u["data"]["newer"].truthy()) {
            text_col(col::warm, "Version %s is available.", u["data"]["version"].str().c_str());
            if (big_button("See what's new")) { g_page = kUpdates; g_focus_page = true; }
        } else text_col(col::ok, "Up to date.");
        card_end();
        prompt(Act::Y, "Check for updates");
        play_landscape();
        if (pressed(Act::Y)) check_updates();
    } else if (kind == "setup-app") {
        title("An install made by the setup app was found", 26);
        text_col(col::dim, "Move it into the launcher's layout: your disc isn't needed again, your saves and settings stay.");
        if (big_button("Move it now", ImVec2(S(320), 0), Kind::Primary))
            run("migrate", { "--install", install_dir(), "--no-prepare" }, "Moving your install", true, [](const json::Value& r) {
                if (!r["ok"].truthy()) show_result_error(r, "Moving the install didn't finish");
                else after_install_shaders();
                refresh_info();
            });
        focus_first();
    } else {
        title(kind == "partial" ? "An install was interrupted" : "The game isn't installed yet", 26);
        text_col(col::dim, kind == "partial" ? "Continue it on the Install page: what was done is kept."
                                             : "You'll need your own disc (an ISO or its folder) and its keys file.");
        if (big_button(kind == "partial" ? "Continue the install" : "Install the game", ImVec2(S(360), 0), Kind::Primary)) {
            g_page = kInstall;
            g_focus_page = true;
        }
        focus_first();
    }
}

// --- the install wizard ---
struct Wizard {
    std::string disc, keys, folder;
    std::string disc_key;          // an encrypted image's key: a .dkey or IRD file, or the private file of a typed key
    bool needs_key = false;        // the disc check found the image encrypted
    json::Value disc_info, keys_info, space;
    std::string disc_err, keys_err, space_err;
    bool probing_disc = false, probing_keys = false;
} g_wiz;

// the worker's arguments for the disc (its key, when the image is encrypted; the keys file too: an IRD's key needs it)
static std::vector<std::string> disc_args()
{
    std::vector<std::string> a = { "--disc", g_wiz.disc };
    if (!g_wiz.disc_key.empty()) {
        a.push_back("--disc-key");
        a.push_back(g_wiz.disc_key);
        if (!g_wiz.keys.empty()) { a.push_back("--keys"); a.push_back(g_wiz.keys); }
    }
    return a;
}

// a typed disc key, kept in a file only this user can read (never on a command line, never in a log)
static std::string typed_key_file(const std::string& text)
{
    const std::string path = path_str(u8path(data_dir()) / "disc-key.txt");
    write_text(path, text + "\n");
    return path;
}

static void probe_disc()
{
    g_wiz.disc_info = json::Value();
    g_wiz.disc_err.clear();
    g_wiz.probing_disc = true;
    run("probe-disc", disc_args(), "", false, [](const json::Value& r) {
        g_wiz.probing_disc = false;
        if (r["ok"].truthy()) g_wiz.disc_info = r["data"];
        else g_wiz.disc_err = r["message"].str();
        const std::string code = r["code"].str();
        if (code == "needs_disc_key" || code == "wrong_disc_key" || r["data"]["encrypted"].truthy()) g_wiz.needs_key = true;
        if (code == "needs_disc_key") g_wiz.disc_err.clear();   // said by the key's own step instead
        if (!g_wiz.keys.empty()) { g_wiz.keys_info = json::Value(); }
    });
}

static void probe_keys()
{
    g_wiz.keys_info = json::Value();
    g_wiz.keys_err.clear();
    if (g_wiz.disc.empty()) return;
    g_wiz.probing_keys = true;
    std::vector<std::string> a = disc_args();
    if (std::find(a.begin(), a.end(), "--keys") == a.end()) { a.push_back("--keys"); a.push_back(g_wiz.keys); }
    run("probe-keys", a, "", false, [](const json::Value& r) {
        g_wiz.probing_keys = false;
        if (r["ok"].truthy()) g_wiz.keys_info = r["data"];
        else g_wiz.keys_err = r["message"].str();
    });
}

static void check_space()
{
    g_wiz.space = json::Value();
    g_wiz.space_err.clear();
    run("space", { "--install", g_wiz.folder }, "", false, [](const json::Value& r) {
        if (r["ok"].truthy()) g_wiz.space = r["data"];
        else g_wiz.space_err = r["message"].str();
    });
}

static void step_row(int n, const char* name, bool ok, const std::string& value, const std::string& err, bool busy,
                     const char* button, std::function<void()> on_press)
{
    card_begin((std::string("##step") + std::to_string(n)).c_str());
    {   // the step's number on a block: green once it is checked
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float bs = floorf(S(52)), p = std::max(3.0f, floorf(S(4)));
        ImGui::Dummy(ImVec2(bs, bs));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        bevel_block(dl, a, ImVec2(a.x + bs, a.y + bs), ok ? col::grass : col::steel, S(4));
        const std::string d = std::to_string(n);
        pix_text(dl, ImVec2(a.x + (bs - pix_w(d.c_str(), p)) / 2, a.y + (bs - 7 * p) / 2), d.c_str(), p, col::text);
    }
    ImGui::SameLine(0, S(20));
    const float wrap_at = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - S(270);   // clear of the button
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(wrap_at);
    title(name, 24);
    if (busy) text_col(col::dim, "Checking...");
    else if (!err.empty()) text_col(col::danger, "%s", err.c_str());
    else if (!value.empty()) text_col(ok ? col::text : col::dim, "%s", value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(230));
    if (big_button(button, ImVec2(S(220), 0))) on_press();
    if (n == 1) focus_first();   // the page opens on the first step's button (the cards are flattened into the page)
    card_end();
}

static void uninstall_open();
static void remove_launcher_open();
static void same_line_fits(float next_w);

static void page_install()
{
    title("Install");
    const std::string kind = g_st.info["kind"].str();
    if (g_wiz.folder.empty()) { g_wiz.folder = install_dir(); check_space(); }
    if (g_wiz.disc.empty() && !g_prefs.disc.empty()) { g_wiz.disc = g_prefs.disc; probe_disc(); }
    if (g_wiz.keys.empty() && !g_prefs.keys.empty()) g_wiz.keys = g_prefs.keys;
    text_col(col::dim, kind == "slots" ? "Repair or reinstall from your disc: your saves and settings stay."
                                       : "Your own disc and keys, checked before anything is written. You can also drop the "
                                         "files onto this window.");
    ImGui::Dummy(ImVec2(0, S(4)));
    const bool disc_ok = !g_wiz.disc_info.is_null();
    if (kind != "slots") {
        card_begin("##need");
        title("What you need", 24);
        text_col(col::text, "Your disc: an ISO of the US release (BLUS-30490), or the disc's files in a folder. An "
                            "encrypted ISO (a raw dump) also needs its disc key -- a .dkey or IRD file, or the key typed "
                            "in -- and the check below asks for it only then.");
        text_col(col::text, "Its keys: a text file with the game's program keys -- lines \"APP <revision> <erk> <riv>\" "
                            "in hexadecimal -- or an EBOOT.elf you decrypted yourself. The installer decrypts the game's "
                            "program (EBOOT.BIN) with them on its own and checks the result; the keys are never copied "
                            "or written to any log.");
        card_end();
    }
    std::string dv = g_wiz.disc.empty() ? "Choose your disc image (an ISO) or the disc's folder."
                                        : g_wiz.disc + (disc_ok ? "\n" + g_wiz.disc_info["title"].str() + " -- " +
                                                                      g_wiz.disc_info["title_id"].str() + " v" +
                                                                      g_wiz.disc_info["app_ver"].str() + ": supported"
                                                                : "");
    step_row(1, "Your disc", disc_ok, dv, g_wiz.disc_err, g_wiz.probing_disc, "Choose...", [] {
        browse("Choose your disc image", false, { ".iso" }, "disc images (.iso)", g_wiz.disc, [](const std::string& p) {
            g_wiz.disc = p;
            g_prefs.disc = p;
            g_prefs.save();
            probe_disc();
        });
    });
    if (g_wiz.needs_key) {   // an encrypted image: its key, by file or typed
        const bool key_ok = disc_ok;
        std::string kv = key_ok ? "The key opens this disc."
                                : g_wiz.disc_key.empty() ? "This disc image is encrypted: choose its disc key (a .dkey or IRD "
                                                           "file), or type the 32-character key."
                                                         : "";
        step_row(2, "Its disc key", key_ok, kv, g_wiz.disc_key.empty() ? "" : g_wiz.disc_err, g_wiz.probing_disc,
                 "Choose...", [] {
            browse("Choose the disc key", false, { ".dkey", ".key", ".ird", ".txt" }, "disc keys (.dkey, .ird)",
                   g_wiz.disc_key, [](const std::string& p) {
                g_wiz.disc_key = p;
                probe_disc();
            });
        });
        if (!key_ok && big_button("Type the key instead", ImVec2(S(360), 0)))
            type_text("The disc key (32 characters, 0-9 and A-F)", "", [](const std::string& t) {
                g_wiz.disc_key = typed_key_file(t);
                probe_disc();
            });
    }
    const bool keys_ok = !g_wiz.keys_info.is_null();
    if (disc_ok && !g_wiz.keys.empty() && g_wiz.keys_info.is_null() && g_wiz.keys_err.empty() && !g_wiz.probing_keys) probe_keys();
    std::string kv = g_wiz.keys.empty() ? "Choose the keys file for your disc (or an EBOOT.elf you decrypted)."
                                        : g_wiz.keys + (keys_ok ? "\nThe keys match this disc." : "");
    step_row(g_wiz.needs_key ? 3 : 2, "Its keys", keys_ok, kv, g_wiz.keys_err, g_wiz.probing_keys, "Choose...", [] {
        browse("Choose the keys file", false, { ".txt", ".keys", ".elf", ".bin" }, "keys files", g_wiz.keys, [](const std::string& p) {
            g_wiz.keys = p;
            g_prefs.keys = p;
            g_prefs.save();
            probe_keys();
        });
    });
    std::string fv = g_wiz.folder;
    const bool space_ok = g_wiz.space["enough"].truthy();
    if (!g_wiz.space.is_null())
        fv += "\n" + human_bytes(g_wiz.space["free"].num()) + " free, " + human_bytes(g_wiz.space["needed"].num()) + " needed" +
              (space_ok ? "" : ": not enough");
    step_row(g_wiz.needs_key ? 4 : 3, "Where to install it", space_ok, fv, g_wiz.space_err, false, "Choose...", [] {
        browse("Choose where to install", true, {}, "folders", g_wiz.folder, [](const std::string& p) {
            std::string f = path_str(u8path(p) / "Cubic.Heroes");
            if (fs::exists(u8path(p) / "active.json") || fs::exists(u8path(p) / "data" / "install.json")) f = p;
            g_wiz.folder = f;
            check_space();
        });
    });
    ImGui::Dummy(ImVec2(0, S(6)));
    const bool ready = disc_ok && keys_ok && space_ok;
    const bool resume = g_st.info["kind"].str() == "partial" && g_wiz.folder == install_dir();
    if (big_button(resume ? "Continue the install" : (kind == "slots" ? "Repair from the disc" : "Install"),
                   ImVec2(S(360), S(64)), Kind::Primary, ready)) {
        g_prefs.install = g_wiz.folder;
        g_prefs.save();
        std::vector<std::string> a = disc_args();
        if (std::find(a.begin(), a.end(), "--keys") == a.end()) { a.push_back("--keys"); a.push_back(g_wiz.keys); }
        a.push_back("--install");
        a.push_back(g_wiz.folder);
        if (resume) a.push_back("--resume");
        a.push_back("--no-prepare");   // the launcher prepares in the background, as the settings or the player say
        run("install", a, "Installing 3D Dot Game Heroes", true, [](const json::Value& r) {
            {   // a typed disc key's file: gone once the install is over (keys are never kept)
                std::error_code ec;
                const fs::path typed = u8path(data_dir()) / "disc-key.txt";
                if (u8path(g_wiz.disc_key) == typed) { fs::remove(typed, ec); g_wiz.disc_key.clear(); }
            }
            if (!r["ok"].truthy() && r["code"].str() != "cancelled") show_result_error(r, "The install didn't finish");
            else if (r["ok"].truthy()) {
                g_page = kPlay;
                g_focus_page = true;
                after_install_shaders();
            }
            refresh_info();
        });
    }
    if (!ready) text_col(col::dim, "Each step above must be checked first.");
    if (kind == "slots") {
        ImGui::Dummy(ImVec2(0, S(10)));
        title("Maintenance", 24);
        bool previous = false;   // the one earlier version kept after an update
        for (size_t i = 0; i < g_st.builds["builds"].size(); i++)
            if (g_st.builds["builds"][i]["state"].str() != "active") previous = true;
        if (big_button("Rollback Last Update", ImVec2(S(360), 0), Kind::Normal, previous))
            ask("Roll back the last update?", "The game goes back to the version you had before the last update. Your saves "
                "and settings stay as they are.", "Roll back", false, [] {
                run("rollback", { "--install", install_dir() }, "Rolling back", true, [](const json::Value& r) {
                    if (!r["ok"].truthy()) show_result_error(r, "The rollback didn't work");
                    else g_msg = { true, "Rolled back", "The previous version is the one you play now.", "", false, false };
                    refresh_info();
                });
            });
        same_line_fits(S(360));
        if (big_button("Uninstall the game...", ImVec2(S(360), 0), Kind::Danger)) uninstall_open();
        if (!previous) text_col(col::dim, "Rollback becomes available after an update.");
    } else {
        ImGui::Dummy(ImVec2(0, S(10)));
        title("Maintenance", 24);
    }
    if (big_button("Remove the launcher...", ImVec2(S(360), 0), Kind::Danger)) remove_launcher_open();
}

// --- updates ---
static void page_updates()
{
    title("Updates");
    const std::string ch = setting("LAUNCHER_UPDATE_CHANNEL", "stable");
    text_col(col::dim, "Channel: %s. Updates are offered, never forced; the game plays offline.",
             ch == "stable" ? "stable releases" : "test releases too");
    if (big_button(g_st.checking_update ? "Checking..." : "Check for updates", ImVec2(S(320), 0), Kind::Normal,
                   !g_st.checking_update))
        check_updates();
    focus_first();
    const json::Value& u = g_st.update;
    if (!g_st.checked_once) return;
    card_begin("##upd");
    if (!u["ok"].truthy()) {
        title("Couldn't check", 24);
        text_col(col::dim, "%s", u["message"].str().c_str());
        if (u["code"].str() == "no_trusted_keys")
            text_col(col::dim, "This launcher carries no release key yet, so it refuses every update: nothing unsigned is "
                               "ever installed.");
    } else if (u["data"]["offline"].truthy()) {
        title("Offline", 24);
        text_col(col::dim, "No connection to the update server. Nothing changes; try again later.");
    } else if (!u["data"]["available"].truthy()) {
        title("No release found", 24);
    } else {
        const json::Value& d = u["data"];
        title(("Version " + d["version"].str()).c_str(), 26);
        text_col(d["newer"].truthy() ? col::warm : col::ok, d["newer"].truthy() ? "Newer than yours (%s)." : "You have it (%s).",
                 d["installed"].str().c_str());
        ImGui::BeginChild("##notes", ImVec2(0, S(220)), ImGuiChildFlags_FrameStyle | ImGuiChildFlags_NavFlattened);
        ImGui::TextWrapped("%s", d["notes"].str("(no notes)").c_str());
        ImGui::EndChild();
        if (d["newer"].truthy() && big_button("Download it", ImVec2(S(300), 0), Kind::Primary))
            run("download-update", { "--install", install_dir(), "--channel", ch, "--version", d["version"].str() },
                "Downloading version " + d["version"].str(), true, [](const json::Value& r) {
                    if (!r["ok"].truthy() && r["code"].str() != "cancelled") show_result_error(r, "The download didn't finish");
                    else if (r["ok"].truthy())
                        g_msg = { true, "Downloaded", "Checked against the release's signature. It is applied when the launcher "
                                                      "restarts.", r["data"]["path"].str(), false, false };
                });
    }
    card_end();
}

// --- saves ---
static void page_saves()
{
    title("Saves");
    text_col(col::dim, "Backups are copies of all your saves. Updates make one first; restoring keeps your current saves "
                       "as a backup too.");
    if (big_button("Back up now", ImVec2(S(300), 0), Kind::Primary))
        type_text("A note for this backup (optional)", "", [](const std::string& note) {
            std::vector<std::string> a = { "--install", install_dir(), "--action", "backup" };
            if (!note.empty()) { a.push_back("--note"); a.push_back(note); }
            run("saves", a, "Backing up your saves", true, [](const json::Value& r) {
                if (!r["ok"].truthy()) show_result_error(r, "The backup didn't finish");
                refresh_info();
            });
        });
    focus_first();
    const json::Value& bs = g_st.saves["backups"];
    text_col(col::dim, "Your saves now: %s", human_bytes(g_st.saves["saves_bytes"].num()).c_str());
    ImGui::BeginChild("##backups", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    for (size_t i = 0; i < bs.size(); i++) {
        const json::Value& x = bs[i];
        const std::string id = x["id"].str();
        card_begin(("##s" + id).c_str());
        std::string made = x["made"].str();
        std::replace(made.begin(), made.end(), 'T', ' ');
        title(made.c_str(), 22);
        text_col(col::dim, "%s  --  %s, %s files%s%s", x["automatic"].truthy() ? "Made automatically" : "Made by you",
                 human_bytes(x["bytes"].num()).c_str(), x["files"].str("0").c_str(), x["note"].is_null() ? "" : "  --  ",
                 x["note"].str().c_str());
        if (big_button(("Restore##" + id).c_str(), ImVec2(S(200), 0)))
            ask("Restore this backup?", "Your saves are replaced by the ones from " + made + ". Your current saves are backed "
                "up first, so nothing is lost.", "Restore", true, [id] {
                run("saves", { "--install", install_dir(), "--action", "restore", "--backup", id, "--confirm" }, "Restoring saves",
                    true, [](const json::Value& r) {
                        if (!r["ok"].truthy()) show_result_error(r, "Restoring didn't finish");
                        refresh_info();
                    });
            });
        ImGui::SameLine();
        if (big_button(("Delete##" + id).c_str(), ImVec2(S(200), 0), Kind::Danger))
            ask("Delete this backup?", "The backup from " + made + " is removed for good. Your current saves are not touched.",
                "Delete", true, [id] {
                run("saves", { "--install", install_dir(), "--action", "delete", "--backup", id, "--confirm" }, "Deleting the backup",
                    true, [](const json::Value& r) {
                        if (!r["ok"].truthy()) show_result_error(r, "Deleting didn't finish");
                        refresh_info();
                    });
            });
        card_end();
    }
    if (bs.size() == 0) text_col(col::dim, "No backups yet.");
    ImGui::EndChild();
}

// --- shaders ---
// the shaders' actions: precompile now, rebuild the cache, or stop the preparation running (one at a time)
static void shader_actions(bool focus = true)
{
    if (g_bg) {
        const bool stopping = g_bg->job->cancel_sent;
        if (big_button(stopping ? "Stopping..." : "Stop preparing", ImVec2(S(300), 0), Kind::Danger, !stopping)) bg_stop();
        if (focus) focus_first();
        ImGui::SameLine();
        text_col(col::dim, "Preparing now: the bar at the bottom shows how far.");
        return;
    }
    const json::Value& c = g_st.check;
    const bool ready = !c.is_null() && shaders_ready(c);
    if (!ready) {   // something to prepare: offered first
        if (big_button("Precompile now", ImVec2(S(300), 0), Kind::Primary)) prepare_now(false);
        if (focus) focus_first();
        same_line_fits(S(360));
    }
    if (big_button("Rebuild shader cache", ImVec2(S(360), 0)))
        ask("Rebuild the shader cache?", "Every shader prepared on this PC is deleted and made again from scratch. Use it "
            "if graphics look wrong, for example after a graphics driver problem. It runs in the background and takes a "
            "few seconds; you can keep using the launcher.", "Rebuild", false, [] { prepare_now(true); });
    if (ready && focus) focus_first();
}

static void page_shaders()
{
    title("Shaders and pipelines");
    text_col(col::dim, "Prepared before you play so effects don't stutter the first time: after an install, an update, or a "
                       "driver or GPU change.");
    const json::Value& c = g_st.check;
    card_begin("##sh");
    if (!g_st.check_error.empty()) text_col(col::danger, "%s", g_st.check_error.c_str());
    else if (c.is_null()) text_col(col::dim, "Checking...");
    else {
        text_col(col::text, "Graphics card: %s (driver %s)", c["gpu"]["name"].str().c_str(), c["gpu"]["driver"].str().c_str());
        const bool ready = shaders_ready(c);
        const json::Value& cov = c["coverage"];
        if (g_bg)   // the check from before it started is out of date until it ends
            text_col(col::warm, "Being prepared for this graphics card and driver now.");
        else if (ready && !cov.is_null())
            text_col(col::ok, "Preparation complete: %d shaders and %d pipelines from the game's files are ready.",
                     (int)cov["translations"].num(), (int)cov["pipelines"].num());
        else if (ready)
            text_col(col::ok, "Preparation complete: every shader this PC has needed so far is ready (%d).",
                     (int)c["recipes"].num());
        else
            text_col(col::warm, "Preparation needed: some shaders aren't ready for this graphics card and driver yet.");
        if (ready && !cov.is_null())   // the details (M9.2): where the prepared pipelines come from
            text_col(col::dim, "Pipelines: %d from the game's shader package (%d records), %d recorded in play.",
                     (int)cov["from_package"].num(), (int)cov["package_records"].num(), (int)cov["recorded"].num());
        text_col(col::dim, "The game can combine more shaders than anyone can list in advance. One it uses for the first "
                           "time is prepared right then -- a brief stutter at most -- and is ready from then on.");
        if (!c["disk_mb"].is_null())
            text_col(col::dim, "On disk: %s.", human_bytes(c["disk_mb"].num() * 1048576).c_str());
    }
    card_end();
    shader_actions();
}

// --- settings ---
// the next item beside the last one only when it fits there (else it starts a line): no sideways overflow
static void same_line_fits(float next_w)
{
    const float limit = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_w <= limit) ImGui::SameLine();
}

static void page_settings()
{
    title("Settings");
    text_col(col::dim, "Shared with the game's own menus. A setting marked * takes effect when the game next starts.");
    const json::Value& s = g_st.settings["settings"];
    if (s.size() == 0) { text_col(col::dim, "Install the game first."); return; }
    for (size_t i = 0; i < g_st.settings["problems"].size(); i++)
        text_col(col::warm, "%s", g_st.settings["problems"][i].str().c_str());
    static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> choices = {
        { "DOTHEROES_FPS", { { "30", "30 fps (the original)" }, { "60", "60 fps" }, { "75", "75 fps" }, { "100", "100 fps" },
                             { "120", "120 fps" }, { "144", "144 fps" }, { "165", "165 fps" }, { "240", "240 fps" },
                             { "display", "The display's rate" }, { "0", "Unlimited" }, { "custom", "Custom" } } },
        { "DOTHEROES_DISPLAY_MODE", { { "windowed", "Windowed" }, { "borderless", "Borderless fullscreen" },
                                      { "exclusive", "Exclusive fullscreen" } } },
        { "DOTHEROES_PERF_OVERLAY", { { "off", "Off" }, { "fps", "Frame rate" }, { "graph", "Frame rate and times" } } },
        { "DOTHEROES_OVERLAY_COMBO", { { "l3r3", "L3 + R3" }, { "selectstart", "Back/Select + Start" },
                                       { "l1r1select", "L1 + R1 + Back/Select" } } },
        { "LAUNCHER_UPDATE_CHANNEL", { { "stable", "Stable releases" }, { "prerelease", "Test releases too" } } },
        { "DOTHEROES_LOG_LEVEL", { { "error", "Errors only" }, { "warning", "Errors and warnings" }, { "info", "Normal" },
                                   { "debug", "Detailed" }, { "trace", "Everything" } } },
        { "LAUNCHER_PROMPTS", { { "auto", "Automatic" }, { "xbox", "Xbox" }, { "playstation", "PlayStation" },
                                { "nintendo", "Nintendo Switch" }, { "steam", "Steam" }, { "generic", "Generic" } } },
        { "LAUNCHER_DISPLAY_MODE", { { "windowed", "Windowed" }, { "borderless", "Borderless fullscreen" },
                                     { "exclusive", "Exclusive fullscreen" } } },
    };
    std::string last_cat;
    bool first = true;
    ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    // the categories in a fixed order, each setting in the schema's order within it
    std::vector<std::pair<std::string, const json::Value*>> ordered;
    static const char* cats[] = { "Game", "Display", "Audio", "Controls", "Launcher", "Shaders", "Updates", "Saves", "Diagnostics" };
    for (const char* c : cats)
        for (const auto& kv : s.o)
            if (kv.second["category"].str() == c) ordered.push_back({ kv.first, &kv.second });
    for (const auto& kv0 : ordered) {
        const std::pair<std::string, const json::Value&> kv(kv0.first, *kv0.second);
        const json::Value& x = kv.second;
        const std::string cat = x["category"].str();
        if (x["advanced"].truthy()) continue;   // developers' switches: the worker's command line, not this page
        if (cat != last_cat) {
            ImGui::Dummy(ImVec2(0, S(6)));
            title(cat.c_str(), 24);
            last_cat = cat;
        }
        const std::string key = kv.first, val = x["value"].str();
        std::string shown = val;
        auto ch = choices.find(key);
        std::vector<std::string> cycle;
        if (ch != choices.end()) {
            for (auto& c : ch->second) { cycle.push_back(c.first); if (c.first == val) shown = c.second; }
        } else if (key == "DOTHEROES_FPS_CUSTOM") {
            if (setting("DOTHEROES_FPS", "60") != "custom") continue;   // only with a custom limit chosen
            cycle = { "20", "24", "25", "40", "45", "48", "50", "72", "80", "85", "90", "110", "180", "200", "300", "360", "500" };
            if (std::find(cycle.begin(), cycle.end(), val) == cycle.end()) cycle.insert(cycle.begin(), val);
            std::sort(cycle.begin(), cycle.end(), [](const std::string& a, const std::string& b) { return atoi(a.c_str()) < atoi(b.c_str()); });
            shown = val + " fps";
        } else if (key == "DOTHEROES_VOLUME_MASTER" || key == "DOTHEROES_VOLUME_MUSIC" || key == "DOTHEROES_VOLUME_SFX") {
            cycle = { "0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100" };
            shown = val + "%";
        } else if (key == "DOTHEROES_SKIP_LOGOS" || key == "DOTHEROES_VSYNC" || key == "DOTHEROES_MUTE" || key == "DOTHEROES_OVERLAY_HINT" ||
                   key == "LAUNCHER_CLOSE_ON_START" || key == "PS3RECOMP_VK_FULLSCREEN" || key == "LAUNCHER_CHECK_UPDATES" ||
                   key == "LAUNCHER_SHADERS_AFTER_UPDATE" || key == "LAUNCHER_SHADERS_ON_DRIVER_CHANGE" ||
                   key == "LAUNCHER_VSYNC") {
            cycle = { "0", "1" };
            shown = val == "1" ? "On" : "Off";
        } else if (key == "LAUNCHER_BUILDS_KEPT") {
            cycle = { "0", "1", "2", "3", "4", "5" };
        } else if (key == "LAUNCHER_SAVE_BACKUPS_KEPT") {
            cycle = { "1", "3", "5", "10", "20", "50", "100" };
        }
        std::string label = x["label"].str(key) + (x["applies"].str() == "restart" ? " *" : "");
        char btn[300];
        snprintf(btn, sizeof btn, "<     %s     >###%s", shown.c_str(), key.c_str());   // its id: the key alone
        // the value's button in a column that fits the page (no sideways scrolling at any scale)
        const float room = ImGui::GetContentRegionAvail().x;
        const float bw = std::min(S(340), room * 0.4f), bx = std::max(room * 0.45f, room - bw - S(24));
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bx - S(30));
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(bx);
        int dir = 0;
        if (big_button(btn, ImVec2(bw, S(50)))) dir = 1;
        if (first) { focus_first(); first = false; }
        if (ImGui::IsItemFocused()) {
            const ImGuiID id = ImGui::GetItemID();   // left and right are this row's: they change its value
            g_focus_owns_lr = true;
            for (ImGuiKey k : { ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow })
                ImGui::SetKeyOwner(k, id);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dir = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dir = 1;
            prompt(Act::A, "Change");
        }
        if (dir && !cycle.empty()) {
            auto it = std::find(cycle.begin(), cycle.end(), val);
            int at = it == cycle.end() ? 0 : (int)(it - cycle.begin());
            at = (at + dir + (int)cycle.size()) % (int)cycle.size();
            run("settings", { "--install", install_dir(), "--set", key + "=" + cycle[at] }, "", false, [](const json::Value& r) {
                if (r["ok"].truthy()) set_settings(r["data"]);
                else show_result_error(r, "That setting wasn't saved");
            });
        }
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    if (big_button("Restore the defaults", ImVec2(S(340), 0)))
        ask("Restore every setting's default?", "Every setting goes back to its default. Your saves are not touched.",
            "Restore", false, [] {
            run("settings", { "--install", install_dir(), "--reset", "all" }, "", false, [](const json::Value& r) {
                if (r["ok"].truthy()) set_settings(r["data"]);
            });
        });
    same_line_fits(S(260));
    if (big_button("Add to Steam", ImVec2(S(260), 0)))
        ask("Add the launcher to Steam?", "It appears in your Steam library (as a non-Steam game) and opens this launcher. "
            "Close Steam first: it rewrites its library when it exits.", "Add it", false, [] {
            run("shortcuts", { "--exe", path_str(u8path(g_exe_dir) / "CubicHeroes.exe"), "--steam" }, "", false,
                [](const json::Value& r) {
                    if (r["ok"].truthy()) g_msg = { true, "Added to Steam", "Start Steam to see it in your library.", "", false, false };
                    else show_result_error(r, "It wasn't added to Steam");
                });
        });
    same_line_fits(S(300));
    if (big_button("Start-menu shortcut", ImVec2(S(300), 0)))
        run("shortcuts", { "--exe", path_str(u8path(g_exe_dir) / "CubicHeroes.exe") }, "", false, [](const json::Value& r) {
            if (r["ok"].truthy()) g_msg = { true, "Shortcut made", "The launcher is in your Start menu.", "", false, false };
            else show_result_error(r, "The shortcut wasn't made");
        });
    if (big_button("Choose another install folder", ImVec2(S(420), 0)))
        browse("Choose the install folder", true, {}, "folders", install_dir(), [](const std::string& p) {
            g_prefs.install = p;
            g_prefs.save();
            g_wiz.folder.clear();
            refresh_info();
        });
    ImGui::EndChild();
}

// --- diagnostics ---
struct Diag {
    json::Value files;
    std::string folder, viewing, view_text, zip;
    int refocus = -1, last = -1;   // after X: the next file (-2: the last one) takes the focus
} g_diag;
static ImGuiWindow* g_rs_window = nullptr;   // this frame's text the right stick scrolls first (a file being viewed)

static void page_diagnostics()
{
    title("Diagnostics");
    text_col(col::dim, "Export Logs gathers what is needed to look into a problem -- even after the game crashed -- and "
                       "shows you every file before anything is saved. Never in it: your saves, keys or the game's files; "
                       "your user name, folders and addresses are replaced. Nothing is ever uploaded.");
    if (big_button("Export Logs", ImVec2(S(300), 0), Kind::Primary)) {
        g_diag.folder = path_str(u8path(data_dir()) / "report");
        g_diag.viewing.clear();
        run("diagnostics", { "--install", install_dir(), "--out", g_diag.folder }, "Making the report", true,
            [](const json::Value& r) {
                if (r["ok"].truthy()) g_diag.files = r["data"]["files"];
                else show_result_error(r, "The report wasn't made");
            });
    }
    focus_first();
    if (g_diag.files.size() == 0) return;
    same_line_fits(S(360));
    if (big_button("Save the zip to...", ImVec2(S(360), 0))) {   // where: the player's choice (Documents to start)
        browse("Where to save the logs", true, {}, "folders", known_folder(kDocuments), [](const std::string& dir) {
            char name[80];
            std::time_t now = std::time(nullptr);
            std::strftime(name, sizeof name, "CubicHeroes-logs-%Y%m%d-%H%M%S.zip", std::localtime(&now));
            const std::string zip = path_str(u8path(dir) / name);
            run("diagnostics", { "--install", install_dir(), "--out", g_diag.folder, "--pack", zip }, "Saving the logs", true,
                [zip](const json::Value& r) {
                    if (r["ok"].truthy()) g_msg = { true, "Logs saved", "The zip is ready to attach to a report.", zip, false, false };
                    else show_result_error(r, "The logs weren't saved");
                });
        });
    }
    text_col(col::dim, "Review: choose a file to see it; X leaves it out. Then save the zip where you like.");
    ImGui::BeginChild("##files", ImVec2(S(380), 0), ImGuiChildFlags_NavFlattened);
    int last = -1;
    for (size_t i = 0; i < g_diag.files.size(); i++) {
        const std::string f = g_diag.files[i]["file"].str();
        if (!fs::exists(u8path(g_diag.folder) / f)) continue;
        if (ImGui::Selectable(("  " + f + "\n  " + human_bytes(g_diag.files[i]["bytes"].num())).c_str(), g_diag.viewing == f,
                              0, ImVec2(0, S(56)))) {
            g_diag.viewing = f;
            g_diag.view_text = read_text(path_str(u8path(g_diag.folder) / f), 200000);
        }
        if ((g_diag.refocus >= 0 && (int)i >= g_diag.refocus) || (g_diag.refocus == -2 && (int)i == g_diag.last)) {
            ImGui::SetKeyboardFocusHere(-1);   // the file after the one left out (or the last)
            g_diag.refocus = -1;
        }
        last = (int)i;
        focus_ring();
        if (ImGui::IsItemFocused()) {
            g_focus_label = "file: " + f;
            prompt(Act::A, "View");
            prompt(Act::X, "Leave out of the report");
        }
        if (ImGui::IsItemFocused() && pressed(Act::X)) {
            std::error_code ec;
            fs::remove(u8path(g_diag.folder) / f, ec);
            if (g_diag.viewing == f) g_diag.viewing.clear();
            g_diag.refocus = (int)i + 1;
        }
    }
    if (g_diag.refocus >= 0) g_diag.refocus = last >= 0 ? -2 : -1;   // nothing after it: the last one, next frame
    g_diag.last = last;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##view", ImVec2(0, 0), ImGuiChildFlags_FrameStyle);
    if (g_diag.viewing.empty()) {
        text_col(col::dim, "Choose a file to see what it holds.");
    } else {
        ImGui::TextUnformatted(g_diag.view_text.c_str());
        g_rs_window = ImGui::GetCurrentWindow();   // the right stick scrolls the file being read
    }
    ImGui::EndChild();
}

// ============================================================================================================
// overlays drawn over the pages
// ============================================================================================================

// a dialog's first frames: its chosen button takes the focus. A popup's first frame is laid out unseen and a focus
// request made then can be lost, so it is made on its first few frames.
struct OpenFocus { bool was = false; int left = 0; };
static void open_focus(OpenFocus& f, bool open)
{
    if (!open) f.was = false;
    else if (!f.was) { f.was = true; f.left = 4; }
}
static void want_focus(OpenFocus& f) { if (f.left > 0) ImGui::SetKeyboardFocusHere(-1); }
static void focus_tick(OpenFocus& f) { if (f.left > 0) f.left--; }

static void overlay_op()
{
    static OpenFocus of;
    open_focus(of, g_op != nullptr);
    if (!g_op) return;
    pump(*g_op);
    Op& op = *g_op;
    if (op.finished) {
        auto then = op.then;
        json::Value r = op.result;
        g_op.reset();
        if (then) then(r);
        return;
    }
    ImGui::OpenPopup("##op");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(80), S(900)), 0));
    if (ImGui::BeginPopupModal("##op", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        frame_window();
        title(op.title.c_str(), 30);
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - op.t0).count();
        std::string line = "Elapsed " + human_time(now);
        if (op.eta >= 0) line += "  --  " + std::string(op.eta_basis == "reference" ? "about " : "") + human_time(op.eta) + " left";
        text_col(col::dim, "%s", line.c_str());
        pix_bar(op.plan.empty() ? (op.fraction >= 0 ? (float)op.fraction : -1.0f) : (float)op.overall, floorf(S(30)));
        ImGui::Dummy(ImVec2(0, S(4)));
        for (auto& st : op.plan) {
            const std::string s = op.state.count(st.first) ? op.state[st.first] : "";
            const ImU32 c = s == "done" ? col::ok : s == "skipped" ? col::dim : st.first == op.stage ? col::accent : col::dim;
            const char* mark = s == "done" ? "[done]" : s == "skipped" ? "[not needed]" : st.first == op.stage ? "[now]" : "";
            text_col(c, "%s  %s", st.second.c_str(), mark);
            if (st.first == op.stage && op.fraction >= 0) {
                pix_bar((float)op.fraction, floorf(S(16)));
            }
        }
        if (!op.detail.empty()) text_col(col::dim, "%s", op.detail.c_str());
        ImGui::Dummy(ImVec2(0, S(6)));
        if (big_button(op.job->cancel_sent ? "Cancelling..." : "Cancel", ImVec2(S(240), 0), Kind::Danger, !op.job->cancel_sent) ||
            (pressed(Act::B) && !op.job->cancel_sent))
            op.job->cancel();
        want_focus(of);
        focus_tick(of);
        prompt(Act::B, "Cancel");
        ImGui::EndPopup();
    }
}

static void overlay_confirm()
{
    static OpenFocus cf;
    open_focus(cf, g_confirm.open);
    if (!g_confirm.open) return;
    ImGui::OpenPopup("##confirm");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(80), S(760)), 0));
    if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        frame_window();
        title(g_confirm.title.c_str(), 28);
        ImGui::TextWrapped("%s", g_confirm.text.c_str());
        ImGui::Dummy(ImVec2(0, S(8)));
        const float bw = std::max(S(260), ImGui::CalcTextSize(g_confirm.yes.c_str()).x + S(60));
        bool yes = big_button(g_confirm.yes.c_str(), ImVec2(bw, 0), g_confirm.danger ? Kind::Danger : Kind::Primary);
        if (g_confirm.focus_yes) want_focus(cf);   // an offer: the recommended choice first
        ImGui::SameLine();
        bool no = big_button(g_confirm.no.c_str(), ImVec2(S(260), 0));
        if (!g_confirm.focus_yes) want_focus(cf);   // else the safe choice first
        focus_tick(cf);
        prompt(Act::A, "Choose");
        prompt(Act::B, g_confirm.no);
        if (pressed(Act::B)) no = true;
        if (yes || no) {
            auto f = yes ? g_confirm.on_yes : g_confirm.on_no;
            g_confirm.open = false;
            ImGui::CloseCurrentPopup();
            if (f) f();
        }
        ImGui::EndPopup();
    }
}

static void overlay_message()
{
    static OpenFocus mf;
    open_focus(mf, g_msg.open);
    if (!g_msg.open) return;
    ImGui::OpenPopup("##msg");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(80), S(820)), 0));
    if (ImGui::BeginPopupModal("##msg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        frame_window();
        ImGui::PushStyleColor(ImGuiCol_Text, v4(g_msg.error ? col::danger : col::ok));
        title(g_msg.title.c_str(), 28);
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", g_msg.text.c_str());
        if (!g_msg.details.empty() && g_msg.show_details) {
            ImGui::BeginChild("##details", ImVec2(0, S(220)), ImGuiChildFlags_FrameStyle);
            ImGui::TextUnformatted(g_msg.details.c_str());
            ImGui::EndChild();
        }
        ImGui::Dummy(ImVec2(0, S(8)));
        bool close = big_button("OK", ImVec2(S(200), 0), Kind::Primary);
        want_focus(mf);
        focus_tick(mf);
        if (!g_msg.details.empty()) {
            ImGui::SameLine();
            if (big_button(g_msg.show_details ? "Hide the details" : "Technical details", ImVec2(S(300), 0)))
                g_msg.show_details = !g_msg.show_details;
        }
        prompt(Act::A, "Choose");
        prompt(Act::B, "Close");
        if (close || pressed(Act::B)) { g_msg.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static void overlay_browser()
{
    if (!g_browser.open) return;
    ImGui::OpenPopup("##browser");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x - S(120), vp->Size.y - S(140)));
    if (ImGui::BeginPopupModal("##browser", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        frame_window();
        Browser& b = g_browser;
        title(b.title.c_str(), 28);
        text_col(col::dim, "%s   (%s)", b.dir.empty() ? "This PC" : b.dir.c_str(), b.filter_desc.c_str());
        // the quick places
        struct Place { const char* name; std::string path; };
        const Place places[] = { { "Downloads", known_folder(kDownloads) }, { "Documents", known_folder(kDocuments) },
                                 { "Desktop", known_folder(kDesktop) }, { "Home", known_folder(kHome) }, { "This PC", "" } };
        for (const Place& p : places) {
            if (big_button(p.name, ImVec2(S(170), S(46)))) b.go(p.path);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        if (b.folders && !b.dir.empty()) {
            if (big_button("Use this folder", ImVec2(S(300), S(50)), Kind::Primary)) {
                auto f = b.on_pick;
                std::string d = b.dir;
                b.open = false;
                ImGui::CloseCurrentPopup();
                if (f) f(d);
            }
        }
        ImGui::BeginChild("##entries", ImVec2(0, -S(10)), ImGuiChildFlags_NavFlattened);
        if (!b.error.empty()) text_col(col::danger, "%s", b.error.c_str());
        const std::string room(8, ' ');   // the icon's place before the name
        auto entry_icon = [](int icon) {
            const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
            const float p = std::max(2.0f, floorf(S(2.6f)));
            pix_icon(ImGui::GetWindowDrawList(), ImVec2(a.x + S(12), (a.y + z.y) / 2 - p * 5), kIcons[icon], p);
        };
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        if (!b.dir.empty()) {
            if (ImGui::Selectable((room + "..   (up a folder)").c_str(), false, 0, ImVec2(0, S(48)))) b.up();
            entry_icon(kIconFolder);
            focus_ring();
            if (ImGui::IsItemFocused()) g_focus_label = "browse: ..";
            if (b.refocus && b.from.empty()) { ImGui::SetKeyboardFocusHere(-1); }
        }
        bool picked = false;
        for (size_t i = 0; i < b.entries.size() && !picked; i++) {
            const Browser::Entry& e = b.entries[i];
            std::string label = room + e.name + (e.dir ? "" : "    " + human_bytes((double)e.size));
            const bool chosen = ImGui::Selectable((label + "##" + std::to_string(i)).c_str(), false, 0, ImVec2(0, S(48)));
            entry_icon(e.dir ? kIconFolder : kIconDisc);
            if (chosen) {
                if (e.dir) { b.go(e.path); break; }
                auto f = b.on_pick;
                std::string p = e.path;
                b.open = false;
                ImGui::CloseCurrentPopup();
                picked = true;
                if (f) f(p);
            }
            focus_ring();
            if (ImGui::IsItemFocused()) g_focus_label = "browse: " + e.name;
            if (b.refocus && (b.from.empty() ? b.dir.empty() && i == 0 : e.path == b.from)) ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::PopStyleVar();
        if (b.refocus > 0) b.refocus--;
        if (b.entries.empty() && b.error.empty()) text_col(col::dim, b.folders ? "No folders here." : "Nothing here that fits.");
        ImGui::EndChild();
        prompt(Act::A, b.folders ? "Open" : "Choose");
        prompt(Act::B, b.dir.empty() ? "Close" : "Up a folder");
        prompt(Act::Y, "Close");
        if (pressed(Act::B)) b.up();
        if (pressed(Act::Y)) b.open = false;
        if (!b.open) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// --- removing the launcher itself (docs/M9_LAUNCHER_PLAN.md section 20): only a downloaded copy; the game, its saves
// and settings stay where they are. The worker measures first (nothing removed; it refuses a source folder or an
// install inside the launcher's folder), then, confirmed, starts a helper from a temporary folder that waits for
// the launcher to exit and removes it -- so the launcher closes right away.
static void remove_launcher_open()
{
    if (game_running() || g_bg || g_op) {
        show_msg("Not now", "The game, a shader preparation or another job is running: wait until it ends.");
        return;
    }
    std::vector<std::string> base = { "--launcher", g_exe_dir, "--data-dir", data_dir() };
    if (g_st.info["kind"].str() != "none" && !install_dir().empty()) { base.push_back("--install"); base.push_back(install_dir()); }
    std::vector<std::string> dry = base;
    dry.push_back("--dry-run");
    run("remove-launcher", dry, "", false, [base](const json::Value& r) {
        if (!r["ok"].truthy()) { show_result_error(r, "The launcher can't be removed"); return; }
        const json::Value& d = r["data"];
        std::string text = "The launcher, its preferences and its shortcuts are removed from this PC (" +
                           human_bytes(d["bytes"].num()) + "). The game stays installed, with your saves and settings: "
                           "download the launcher again to play. To remove the game too, uninstall it first.";
        if (d["steam_open"].truthy()) text += "\n\nSteam is open: close it first, or the launcher's Steam entry stays.";
        if (d["kept_items"].size()) text += "\n\nOther files you keep in the launcher's folder stay.";
        ask("Remove the launcher?", text, "Remove", true, [base] {
            if (g_nav_test) { g_nav_event = "  [the launcher would be removed: not during a scripted controller test]"; return; }
            std::vector<std::string> go = base;
            go.push_back("--confirm");
            go.push_back("--wait-pid");
#ifdef _WIN32
            go.push_back(std::to_string((unsigned long)GetCurrentProcessId()));
#else
            go.push_back(std::to_string((long)getpid()));
#endif
            run("remove-launcher", go, "Removing the launcher", true, [](const json::Value& r) {
                if (!r["ok"].truthy()) { show_result_error(r, "The launcher couldn't be removed"); return; }
                g_quit = true;   // the helper removes the launcher once it has exited
            });
        });
    });
}

// --- uninstalling: the parts chosen (saves, backups and settings kept unless chosen), what each frees ---
struct Uninst {
    bool open = false;
    json::Value parts;   // the worker's plan: {part: {bytes, files}}
    std::map<std::string, bool> on;
} g_uninst;
static const std::pair<const char*, const char*> kUninstParts[] = {
    { "builds", "" }, { "data", "" }, { "shaders", "" }, { "objects", "" }, { "settings", "" }, { "saves", "" },
    { "shortcuts", "" } };

static bool game_running();
static void uninstall_open()
{
    if (game_running() || g_bg || g_op) {
        show_msg("Not now", "The game, a shader preparation or another job is running: uninstalling waits until it ends.");
        return;
    }
    run("uninstall", { "--install", install_dir(), "--dry-run" }, "", false, [](const json::Value& r) {
        if (!r["ok"].truthy()) { show_result_error(r, "The install couldn't be measured"); return; }
        g_uninst = Uninst();
        g_uninst.open = true;
        g_uninst.parts = r["data"]["parts"];
        for (const auto& pr : kUninstParts) g_uninst.on[pr.first] = true;   // everything the game installed goes...
        g_uninst.on["saves"] = g_uninst.on["settings"] = false;            // ...but these stay unless the player says
    });
}

static void overlay_uninstall()
{
    static OpenFocus uf;
    open_focus(uf, g_uninst.open);
    if (!g_uninst.open) return;
    ImGui::OpenPopup("##uninstall");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(80), S(900)), 0));
    if (ImGui::BeginPopupModal("##uninstall", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        frame_window();
        title("Uninstall the game?", 28);
        text_col(col::text, "The game and everything it installed on this PC are removed. Your disc image and keys are "
                            "never touched.");
        double total = 0;
        for (const auto& pr : kUninstParts)
            if (g_uninst.on[pr.first]) total += g_uninst.parts[pr.first]["bytes"].num();
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        const bool keep_saves = !g_uninst.on["saves"], keep_settings = !g_uninst.on["settings"];
        if (big_button(keep_saves ? "[x]  Keep my saves and backups###u_saves" : "[  ]  Keep my saves and backups###u_saves",
                       ImVec2(-1, S(52)), keep_saves ? Kind::Normal : Kind::Danger))
            g_uninst.on["saves"] = keep_saves;
        if (big_button(keep_settings ? "[x]  Keep my settings###u_set" : "[  ]  Keep my settings###u_set", ImVec2(-1, S(52)),
                       keep_settings ? Kind::Normal : Kind::Danger))
            g_uninst.on["settings"] = keep_settings;
        ImGui::PopStyleVar();
        text_col(col::warm, "About %s freed.", human_bytes(total).c_str());
        ImGui::Dummy(ImVec2(0, S(6)));
        bool go = big_button("Uninstall", ImVec2(S(260), 0), Kind::Danger);
        same_line_fits(S(260));
        bool no = big_button("Cancel", ImVec2(S(260), 0));
        want_focus(uf);   // the safe choice first
        focus_tick(uf);
        prompt(Act::A, "Choose");
        prompt(Act::B, "Cancel");
        if (pressed(Act::B)) no = true;
        if (go) {
            std::string parts;
            for (const auto& pr : kUninstParts)
                if (g_uninst.on[pr.first]) parts += std::string(parts.empty() ? "" : ",") + pr.first;
            g_uninst.open = false;
            ImGui::CloseCurrentPopup();
            if (!parts.empty())
                run("uninstall", { "--install", install_dir(), "--confirm", "--parts", parts, "--exe",
                                   path_str(u8path(g_exe_dir) / "CubicHeroes.exe") },
                    "Uninstalling", true, [](const json::Value& r) {
                        if (r["data"]["removed"]["shortcuts"]["steam_later"].truthy()) {   // Steam ran: once it's closed
                            g_prefs.steam_cleanup = true;
                            g_prefs.save();
                        }
                        if (!r["ok"].truthy()) show_result_error(r, "Uninstalling didn't finish");
                        else g_msg = { true, "Uninstalled", "About " + human_bytes(r["data"]["bytes"].num()) +
                                       " freed. Install again whenever you like: what you kept is used again.", "", false, false };
                        g_page = kInstall;
                        g_focus_page = true;
                        refresh_info();
                    });
        } else if (no) {
            g_uninst.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void overlay_osk()
{
    static OpenFocus kf;
    open_focus(kf, g_osk.open);
    if (!g_osk.open) return;
    ImGui::OpenPopup("##osk");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(60), S(1100)), 0));
    if (ImGui::BeginPopupModal("##osk", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        frame_window();
        title(g_osk.title.c_str(), 26);
        // the text as typed (a keyboard types into it directly too)
        char buf[256];
        snprintf(buf, sizeof buf, "%s", g_osk.text.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##osktext", buf, sizeof buf)) g_osk.text = buf;
        static const char* rows[2][4] = { { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" },
                                          { "!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:_" } };
        const float key = S(76);
        bool first = true;
        for (int r = 0; r < 4; r++) {
            const char* row = rows[g_osk.shift ? 1 : 0][r];
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + r * key * 0.25f);
            for (int i = 0; row[i]; i++) {
                char k[8] = { row[i], 0 };
                char id[24];
                snprintf(id, sizeof id, "%s##k%d_%d", k, r, i);
                if (big_button(id, ImVec2(key, key))) g_osk.text += row[i];
                if (first) { want_focus(kf); focus_tick(kf); }
                first = false;
                if (row[i + 1]) ImGui::SameLine(0, S(8));
            }
        }
        if (big_button("Shift", ImVec2(key * 2, key))) g_osk.shift = !g_osk.shift;
        ImGui::SameLine(0, S(8));
        if (big_button("Space", ImVec2(key * 4, key))) g_osk.text += ' ';
        ImGui::SameLine(0, S(8));
        if (big_button("Delete", ImVec2(key * 2, key)) && !g_osk.text.empty()) g_osk.text.pop_back();
        ImGui::SameLine(0, S(8));
        bool done = big_button("Done", ImVec2(key * 2, key), Kind::Primary);
        prompt(Act::A, "Type");
        prompt(Act::X, "Delete");
        prompt(Act::Y, "Space");
        prompt(Act::Start, "Done");
        prompt(Act::B, "Cancel");
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft) && !g_osk.text.empty()) g_osk.text.pop_back();
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) g_osk.text += ' ';
        if (pressed(Act::Start) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) done = true;
        bool cancel = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (done || cancel) {
            auto f = g_osk.on_done;
            std::string t = g_osk.text;
            g_osk.open = false;
            ImGui::CloseCurrentPopup();
            if (done && f) f(t);
        }
        ImGui::EndPopup();
    }
}

// ============================================================================================================
// the frame: the header (the pages, LB/RB), the page, the prompts
// ============================================================================================================

static bool overlay_open() { return g_op || g_confirm.open || g_msg.open || g_browser.open || g_osk.open || g_uninst.open; }

static void voxel_bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, float t)
{
    dl->AddRectFilled(a, b, A(col::inset));
    bevel_edges(dl, a, b, col::inset, std::max(2.0f, floorf(S(2))), true);
    const float pad = std::max(3.0f, floorf(S(5)));
    const float h = floorf(b.y - a.y - 2 * pad), gap = std::max(2.0f, floorf(S(3)));
    const float bw = std::max(8.0f, floorf(h * 0.9f));
    const int n = std::max(1, (int)((b.x - a.x - 2 * pad + gap) / (bw + gap)));
    const float x0 = a.x + pad + floorf(((b.x - a.x - 2 * pad) - (n * (bw + gap) - gap)) / 2), y0 = a.y + pad;
    const int filled = frac < 0 ? 0 : (int)floorf(frac * n + 0.0001f);
    const int run_at = frac < 0 ? (int)fmodf(t * 14.0f, (float)(n + 5)) - 5 : -100;
    for (int i = 0; i < n; i++) {
        const ImVec2 p(x0 + i * (bw + gap), y0), q(p.x + bw, y0 + h);
        const bool on = i < filled || (i >= run_at && i < run_at + 5);
        if (!on) {   // an empty slot: a faint block outline
            dl->AddRect(p, q, A(IM_COL32(40, 54, 110, 255)), 0.0f, std::max(1.0f, floorf(S(1.5f))));
            continue;
        }
        const bool edge = !(frac < 0) && i == filled - 1 && filled < n;   // the newest block glints
        const ImU32 face = edge ? IM_COL32(150, 236, 120, 255) : col::grass;
        const float top = floorf(h * 0.3f), side = std::max(2.0f, floorf(bw * 0.18f));
        dl->AddRectFilled(p, q, A(face));
        dl->AddRectFilled(p, ImVec2(q.x, p.y + top), A(shade(face, 1.3f)));               // the lit top
        dl->AddRectFilled(ImVec2(q.x - side, p.y + top), q, A(shade(face, 0.62f)));       // the shaded side
        dl->AddRectFilled(ImVec2(p.x, p.y + top), ImVec2(q.x - side, p.y + top + std::max(1.0f, floorf(S(1.5f)))),
                          A(shade(face, 0.8f)));                                         // the top's edge
        dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(q.x + 1, q.y + 1), A(col::ink), 0.0f, std::max(1.0f, floorf(S(1.5f))));
    }
}

// the persistent bar along the bottom while shaders are prepared (and for a while after, saying how it ended)
static void prepare_strip(ImDrawList* dl, ImVec2 a, ImVec2 b, float t)
{
    dl->AddRectFilled(a, b, A(col::panel));
    retro_frame(dl, a, b, col::frame, std::max(2.0f, floorf(S(3))));
    const float ip = std::max(2.0f, floorf(S(2.6f))), tp = std::max(2.0f, floorf(S(2.4f)));
    const float cy = (a.y + b.y) / 2;
    pix_icon(dl, ImVec2(a.x + S(20), cy - ip * 5), kIcons[5], ip);
    std::string head, right;
    float frac = -1;
    ImU32 head_col = col::accent;
    char buf[200];
    if (g_bg) {
        const Op& op = *g_bg;
        head = op.job->cancel_sent ? "Stopping..." : op.title;
        frac = op.fraction >= 0 ? (float)op.fraction : -1.0f;
        if (frac >= 0) {
            std::string eta = op.eta >= 0 ? std::string(op.eta_basis == "reference" ? "about " : "") + human_time(op.eta) + " left" : "";
            snprintf(buf, sizeof buf, "%d%%   %s%s%s", (int)(frac * 100), op.detail.c_str(), eta.empty() ? "" : "   ", eta.c_str());
            right = buf;
        } else {
            right = "Starting" + (op.detail.empty() ? std::string() : " (" + op.detail + ")") + "...";
        }
    } else {
        head = g_bg_done.title;
        head_col = g_bg_done.ok ? col::ok : col::warm;
        frac = g_bg_done.ok ? 1.0f : 0.0f;
        right = g_bg_done.text;
    }
    std::string up = head;
    for (char& ch : up) ch = (char)toupper((unsigned char)ch);
    const float hx = a.x + S(20) + ip * 10 + S(18);
    pix_text(dl, ImVec2(hx, cy - tp * 3.5f), up.c_str(), tp, head_col);
    const float fs = S(20);
    const ImVec2 rs = g_font->CalcTextSizeA(fs, FLT_MAX, 0, right.c_str());
    const float rx = b.x - S(24) - rs.x;
    dl->AddText(g_font, fs, ImVec2(rx + 2, cy - rs.y / 2 + 2), A(col::ink), right.c_str());
    dl->AddText(g_font, fs, ImVec2(rx, cy - rs.y / 2), A(col::text), right.c_str());
    const float bx0 = hx + pix_w(up.c_str(), tp) + S(24), bx1 = rx - S(24);
    if (bx1 - bx0 > S(120) && (g_bg || g_bg_done.ok))
        voxel_bar(dl, ImVec2(bx0, a.y + S(12)), ImVec2(bx1, b.y - S(12)), frac, t);
}

// behind everything: a night sky (twinkling stars, blocky clouds drifting by) over a ground of voxel blocks -- grass on
// earth -- that the button prompts stand on
static void backdrop(ImDrawList* dl, ImVec2 o, ImVec2 sz, float foot, float t)
{
    const float gy = o.y + sz.y - foot;
    dl->AddRectFilledMultiColor(o, ImVec2(o.x + sz.x, gy), IM_COL32(10, 12, 34, 255), IM_COL32(10, 12, 34, 255),
                                IM_COL32(40, 56, 120, 255), IM_COL32(40, 56, 120, 255));
    const float px = std::max(2.0f, floorf(S(3)));
    for (uint32_t i = 0; i < 70; i++) {   // stars
        const uint32_t h = hash32(i * 2654435761U + 17);
        const float x = o.x + (float)(h % 10007) / 10007.0f * sz.x, y = o.y + (float)((h >> 14) % 997) / 997.0f * (gy - o.y) * 0.7f;
        const float tw = 0.5f + 0.5f * sinf(t * (0.8f + (h & 7) * 0.15f) + (float)(h % 628) / 100.0f);
        const int a = (int)(60 + 150 * tw);
        dl->AddRectFilled(ImVec2(snap(x), snap(y)), ImVec2(snap(x) + px * ((h >> 3 & 3) == 0 ? 2 : 1), snap(y) + px * ((h >> 3 & 3) == 0 ? 2 : 1)),
                          IM_COL32(230, 236, 255, a));
    }
    for (int i = 0; i < 4; i++) {   // clouds: three blocks each, drifting slowly to the right
        const float w = S(150 + 40 * (i % 3)), span = sz.x + w * 2;
        const float x = o.x + fmodf(i * span * 0.29f + t * S(5.0f + 3.0f * (i % 2)), span) - w;
        const float y = snap(o.y + sz.y * (0.18f + 0.17f * i));
        const ImU32 c = IM_COL32(52, 68, 140, 150);
        const float u = floorf(S(12));
        dl->AddRectFilled(ImVec2(snap(x), y), ImVec2(snap(x + w), y + u * 2), c);
        dl->AddRectFilled(ImVec2(snap(x + w * 0.2f), y - u), ImVec2(snap(x + w * 0.62f), y), c);
        dl->AddRectFilled(ImVec2(snap(x + w * 0.38f), y - u * 2), ImVec2(snap(x + w * 0.52f), y - u), c);
    }
    // the ground: a row of blocks, grass on top, earth with darker grains, seams between them
    const float bw = floorf(S(40)), grass = floorf(S(12)), hi = floorf(S(4));
    dl->AddRectFilled(ImVec2(o.x, gy), ImVec2(o.x + sz.x, o.y + sz.y), col::dirt);
    dl->AddRectFilled(ImVec2(o.x, gy - 2), ImVec2(o.x + sz.x, gy), col::ink);
    dl->AddRectFilled(ImVec2(o.x, gy), ImVec2(o.x + sz.x, gy + grass), col::grass);
    dl->AddRectFilled(ImVec2(o.x, gy), ImVec2(o.x + sz.x, gy + hi), col::grass_hi);
    int k = 0;
    for (float x = o.x; x < o.x + sz.x; x += bw, k++) {
        const uint32_t h = hash32((uint32_t)k * 7919U + 3);
        for (int d = 0; d < 2; d++) {   // grass hanging over the edge
            const float gx = snap(x + (float)((h >> (d * 8)) % 9) / 9.0f * (bw - px * 2));
            dl->AddRectFilled(ImVec2(gx, gy + grass), ImVec2(gx + px * 2, gy + grass + px * (1 + (h >> (d + 20) & 1))), col::grass);
        }
        for (int d = 0; d < 3; d++) {   // grains in the earth
            const uint32_t hh = hash32(h + d * 101);
            const float ex = snap(x + (float)(hh % 97) / 97.0f * (bw - px * 2));
            const float ey = snap(gy + grass + px * 3 + (float)((hh >> 8) % 89) / 89.0f * (foot - grass - px * 6));
            dl->AddRectFilled(ImVec2(ex, ey), ImVec2(ex + px * 2, ey + px), col::dirt_dk);
        }
        dl->AddRectFilled(ImVec2(snap(x), gy + grass), ImVec2(snap(x) + std::max(1.0f, floorf(S(2))), o.y + sz.y), col::dirt_dk);
    }
}

static void frame()
{
    g_prompts.clear();
    const bool owned_lr = g_focus_owns_lr;   // last frame's (the item sets it as it is drawn)
    g_focus_owns_lr = false;
    const bool dialog_at_start = overlay_open();   // a press that closes a dialog this frame is the dialog's alone
    // X is the launcher's own (leave a file out, delete a letter): ImGui would take a tap of it to move the focus to a
    // window's menu layer -- to the root window when there is none, the focus lost -- and a hold to switch windows
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceLeft, ImHashStr("cubicheroes: X"));
    if (g_bg) {   // the background preparation
        pump(*g_bg);
        if (g_bg->finished) {
            auto then = g_bg->then;
            json::Value r = g_bg->result;
            g_bg.reset();
            if (then) then(r);
            if (g_play_after_bg) {   // paused for the game: what was made is kept
                g_play_after_bg = false;
                g_bg_done.title = "Paused for the game";
                g_bg_done.text = "What was prepared is kept. Prepare again after playing to finish.";
                start_game_now();
            }
        }
    }
    {   // the launcher's game closing: the note cleared, the cache checked, a preparation it held back started
        static bool was_running = false;
        const bool running = game_running();
        if (was_running && !running) {
            g_st.playing.clear();
            // how the session ended (the game's menu): Exit Game closes the launcher too; Return to Launcher (or the
            // game closed any other way) brings it back to the front with the controller's focus
            const std::string how = read_text(path_str(u8path(install_dir()) / "logs" / "session-end"), 64);
            if (how.rfind("exit", 0) == 0) g_quit = true;
            else if (g_window) {
                SDL_RestoreWindow(g_window);
                SDL_RaiseWindow(g_window);
                g_focus_page = true;
            }
            if (g_prepare_after_game) { g_prepare_after_game = false; bg_prepare(false, "after the game closed"); }
            refresh_info();
        }
        was_running = running;
    }
    {   // the right stick scrolls the list or text under the focus (the D-pad and left stick move the focus)
        ImGuiContext& g = *ImGui::GetCurrentContext();
        const float ry = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue -
                         ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue + g_nav_rstick;
        if (fabsf(ry) > 0.15f) {
            ImGuiWindow* w = g_rs_window && g_rs_window->ScrollMax.y > 0.0f ? g_rs_window
                           : g.NavWindow ? g.NavWindow : g.HoveredWindow;
            while (w && w->ScrollMax.y <= 0.0f && w->ParentWindow) w = w->ParentWindow;
            if (w && w->ScrollMax.y > 0.0f)
                ImGui::SetScrollY(w, std::min(w->ScrollMax.y, std::max(0.0f, w->Scroll.y + ry * S(1500) * ImGui::GetIO().DeltaTime)));
        }
        g_rs_window = nullptr;   // the page sets it again as it is drawn
    }
    if (g_auto_check_due) { g_auto_check_due = false; auto_prepare_check(); }
    if (g_prefs.steam_cleanup && !g_nav_test) {   // the launcher's Steam entry an uninstall left: tried at start, then
        static double next = 0;                   // every 20 s, quietly, until Steam is closed and it is gone
        static bool asking = false;
        const double now = SDL_GetTicks() / 1000.0;
        if (!asking && now >= next) {
            asking = true;
            next = now + 20;
            run("shortcuts", { "--exe", path_str(u8path(g_exe_dir) / "CubicHeroes.exe"), "--remove-steam" }, "", false,
                [](const json::Value& r) {
                    asking = false;
                    if (!r["data"]["later"].truthy()) {   // removed, or nothing left to remove (an error isn't retried)
                        g_prefs.steam_cleanup = false;
                        g_prefs.save();
                    }
                });
        }
    }
    for (size_t i = 0; i < g_queries.size();) {   // the quick queries: their callbacks run here, on this thread
        pump(*g_queries[i]);
        if (g_queries[i]->finished) {
            auto then = g_queries[i]->then;
            json::Value r = g_queries[i]->result;
            g_queries.erase(g_queries.begin() + i);
            if (then) then(r);
        } else {
            i++;
        }
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = vp->Pos, sz = vp->Size;
    const float t = (float)ImGui::GetTime();
    const float foot = S(68), m = floorf(S(18));
    dl->PushClipRectFullScreen();   // the window's own clipping leaves out its padding: the scene is the whole window
    backdrop(dl, o, sz, foot, t);
    // the command menu: the logo, the pages with their icons (LB/RB, Q/E, or a click), the active one marked
    const float side_w = floorf(S(318));
    const bool strip = g_bg || (g_bg_done.shown && t - g_bg_done.t0 < 8.0f);
    const float strip_h = floorf(S(64));
    const ImVec2 sa(o.x + m, o.y + m), sb(o.x + m + side_w, o.y + sz.y - foot - m - (strip ? strip_h + m : 0));
    dl->AddRectFilled(sa, sb, A(col::panel));
    retro_frame(dl, sa, sb);
    const float bobc = floorf((0.5f + 0.5f * sinf(t * 2.2f)) * S(4));
    voxel(dl, ImVec2(sa.x + S(56), sa.y + S(86)), S(26), col::grass, col::dirt, col::dirt_dk);
    voxel(dl, ImVec2(sa.x + S(56), sa.y + S(46) - bobc), S(12), IM_COL32(255, 222, 110, 255), col::gold, IM_COL32(186, 126, 26, 255));
    const float lp = std::max(3.0f, floorf(S(4.5f)));
    pix_text(dl, ImVec2(sa.x + S(104), sa.y + S(30)), "CUBIC", lp, col::accent);
    pix_text(dl, ImVec2(sa.x + S(104), sa.y + S(30) + lp * 9), ".HEROES", lp, col::text);
    for (float x = sa.x + S(24); x < sb.x - S(24); x += S(12))   // a dotted rule under the logo
        dl->AddRectFilled(ImVec2(snap(x), snap(sa.y + S(136))), ImVec2(snap(x + S(6)), snap(sa.y + S(139))), A(col::steel));
    const float row = floorf(std::min(S(52), (sb.y - S(78) - (sa.y + S(156))) / (kPages + 1.2f)));
    const float ip = std::max(2.0f, floorf(std::min(S(3), row / 14.0f))), np = std::max(2.0f, floorf(std::min(S(3), row / 13.0f)));
    float y = sa.y + S(156);
    // the rows are items of their own (a child flattened into the window's navigation): left from the page reaches them,
    // up and down move through them, right goes back to the page; A (or a click) opens a page, or EXIT asks to exit
    ImGui::SetCursorScreenPos(sa);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##side", ImVec2(sb.x - sa.x, sb.y - sa.y), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    // from a page only left reaches the menu (below, when nothing on the page is further left): up and down never cross
    // into it, so the rows are no navigation targets while the page has the focus
    bool menu_has_focus = false;   // (by ImGui's focus itself: a dialog closing gives the focus back to a row)
    for (int i = 0; i <= kPages; i++) {
        ImGui::PushID(i);
        if (ImGui::GetID("##row") == ImGui::GetFocusID()) menu_has_focus = true;
        ImGui::PopID();
    }
    for (int i = 0; i <= kPages; i++) {   // the pages, then EXIT
        const bool exit_row = i == kPages;
        const float ry = exit_row ? y + row * 0.2f : y;
        const ImVec2 ra(sa.x + S(14), ry), rb(sb.x - S(14), ry + row - S(6));
        ImGui::SetCursorScreenPos(ra);
        ImGui::PushID(i);
        if (!menu_has_focus) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        const bool click = ImGui::InvisibleButton("##row", ImVec2(rb.x - ra.x, rb.y - ra.y), ImGuiButtonFlags_EnableNav);
        if (!menu_has_focus) ImGui::PopItemFlag();
        ImGui::PopID();
        if (g_focus_menu && i == g_page) {   // at once (a focus request would resolve a frame late, and the page's
            // focused item, drawn after the menu, would take the window back from it)
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisibleAfterMove();
            g_focus_menu = false;
        }
        const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        const bool hov = ImGui::IsItemHovered() || focused;
        const bool on = !exit_row && i == g_page;
        if (on) {
            sdl->AddRectFilled(ra, rb, A(col::inset));
            sdl->AddRectFilled(ra, ImVec2(ra.x + S(6), rb.y), A(col::accent));
        } else if (hov) {
            sdl->AddRectFilled(ra, rb, A(IM_COL32(34, 48, 108, 255)));
        }
        const float cy = (ra.y + rb.y) / 2;
        if (exit_row) {   // a door, drawn in blocks: the frame, the door, its knob
            const float d = ip;
            const ImVec2 da(ra.x + S(22) + d, cy - d * 5);
            sdl->AddRectFilled(ImVec2(da.x - d, da.y - d), ImVec2(da.x + d * 9, da.y + d * 11), A(col::ink));
            sdl->AddRectFilled(da, ImVec2(da.x + d * 8, da.y + d * 10), A(IM_COL32(150, 96, 52, 255)));
            sdl->AddRectFilled(ImVec2(da.x + d, da.y + d), ImVec2(da.x + d * 7, da.y + d * 10), A(IM_COL32(110, 66, 36, 255)));
            sdl->AddRectFilled(ImVec2(da.x + d * 5, da.y + d * 5), ImVec2(da.x + d * 6, da.y + d * 6), A(col::accent));
        } else {
            pix_icon(sdl, ImVec2(ra.x + S(22), cy - ip * 5), kIcons[kPageIcon[i]], ip);
        }
        pix_text(sdl, ImVec2(ra.x + S(22) + ip * 10 + S(22), cy - np * 3.5f), exit_row ? "EXIT" : kPageNames[i], np,
                 on ? col::accent : hov ? col::text : col::dim);
        if (ImGui::IsItemFocused()) {
            g_focus_label = std::string("menu: ") + (exit_row ? "Exit" : kPageNames[i]);
            // the menu's own keys: left and right (right goes to the page's first item, below), up on its first row and
            // down on its last (the navigation would otherwise find the page's items beside them)
            const ImGuiID id = ImGui::GetItemID();
            for (ImGuiKey k : { ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow })
                ImGui::SetKeyOwner(k, id);
            if (i == 0) for (ImGuiKey k : { ImGuiKey_GamepadDpadUp, ImGuiKey_UpArrow }) ImGui::SetKeyOwner(k, id);
            if (exit_row) for (ImGuiKey k : { ImGuiKey_GamepadDpadDown, ImGuiKey_DownArrow }) ImGui::SetKeyOwner(k, id);
        }
        if (focused) prompt(Act::A, exit_row ? "Exit" : "Open");
        focus_ring();
        if (click) {
            if (exit_row) ask_exit();
            else { g_page = i; g_focus_page = false; }   // the focus stays in the menu; right goes to the page
        }
        if (!exit_row) y += row;
    }
    ImGui::EndChild();
    {   // LB / RB beside "PAGES" at the menu's foot
        const float gh = floorf(S(30));
        float gx = sa.x + S(28);
        const float gy = sb.y - S(26) - gh;
        gx += glyph(dl, ImVec2(gx, gy), Act::LB, gh) + S(8);
        gx += glyph(dl, ImVec2(gx, gy), Act::RB, gh) + S(14);
        pix_text(dl, ImVec2(gx, gy + (gh - 7 * np) / 2), "PAGES", np, col::dim);
    }
    if (!overlay_open()) {
        if (pressed(Act::LB)) { g_page = (g_page + kPages - 1) % kPages; g_focus_page = true; }
        if (pressed(Act::RB)) { g_page = (g_page + 1) % kPages; g_focus_page = true; }
    }
    // the page's window; a new page slides in and fades up
    static int shown = -1;
    static float since = 0;
    if (shown != g_page) { shown = g_page; since = t; }
    const float e = std::min(1.0f, (t - since) / 0.22f), ease = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e);
    const ImVec2 ca(sb.x + m, sa.y), cb(o.x + sz.x - m, sb.y);
    dl->AddRectFilled(ca, cb, A(col::panel));
    retro_frame(dl, ca, cb);
    if (strip) prepare_strip(dl, ImVec2(sa.x, sb.y + m), ImVec2(cb.x, sb.y + m + strip_h), t);
    dl->PopClipRect();
    const float in_x = floorf(S(58)), in_y = floorf(S(34));
    ImGui::SetCursorScreenPos(ImVec2(ca.x + in_x + floorf((1.0f - ease) * S(28)), ca.y + in_y));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.2f + 0.8f * ease);
    g_page_input = !overlay_open();
    ImGui::BeginChild("##page", ImVec2(cb.x - ca.x - in_x - S(34), cb.y - ca.y - in_y - S(26)), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(nullptr, S(kBody));
    switch (g_page) {
    case kPlay: page_play(); break;
    case kInstall: page_install(); break;
    case kUpdates: page_updates(); break;
    case kSaves: page_saves(); break;
    case kShaders: page_shaders(); break;
    case kSettings: page_settings(); break;
    case kDiagnostics: page_diagnostics(); break;
    }
    ImGui::PopFont();
    ImGui::EndChild();
    g_page_input = true;
    ImGui::PopStyleVar();
    // the device in use, at the ground's left
    const char* dev = g_device == Device::Pad ? "Controller" : g_pad_connected ? "Keyboard and mouse (a controller is connected)"
                                                                              : "Keyboard and mouse";
    // overlays
    ImGui::PushFont(nullptr, S(kBody));
    overlay_op();
    overlay_browser();
    overlay_osk();
    overlay_confirm();
    overlay_uninstall();
    overlay_message();
    ImGui::PopFont();
    // the prompts
    if (!overlay_open() && !dialog_at_start && g_bg) {
        prompt(Act::Back, "Stop preparing");
        if (pressed(Act::Back)) bg_stop();
    }
    if (!overlay_open() && !dialog_at_start) {   // MENU (F10): exit, asked first
        prompt(Act::Start, "Exit");
        if (pressed(Act::Start)) ask_exit();
        // between the menu and the page: right from the menu goes to the page's first item; left on a page goes to
        // the menu when nothing further left takes it (a settings row keeps left and right: they change its value)
        static std::string left_from;
        static int left_wait = 0;
        const bool in_menu = g_focus_label.rfind("menu: ", 0) == 0;
        auto free_press = [owned_lr](ImGuiKey k) { return !owned_lr && !g_focus_owns_lr && ImGui::IsKeyPressed(k, false); };
        if (in_menu && (free_press(ImGuiKey_GamepadDpadRight) || free_press(ImGuiKey_RightArrow))) g_focus_page = true;
        if (!in_menu && (free_press(ImGuiKey_GamepadDpadLeft) || free_press(ImGuiKey_LeftArrow))) {
            left_from = g_focus_label;
            left_wait = 2;
        } else if (left_wait > 0 && --left_wait == 0 && g_focus_label == left_from) {
            g_focus_menu = true;   // nothing to its left: the menu
        }
        if (g_focus_label.rfind("menu: ", 0) != 0) {   // on a page: B goes to the menu
            prompt(Act::B, "Menu");
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                g_focus_menu = true;
        }
    }
    if (!overlay_open()) {
        prompt(Act::A, "Select");
        prompt(Act::LB, "");
        prompt(Act::RB, "Pages");
    }
    {   // a controller always has a focus: when the focused item went away (a button replaced as a job started or ended,
        // a list emptied, a dialog closed whose opener is gone), the page's first item takes it
        static int lost = 0;
        if (g_device == Device::Pad && !overlay_open() && !ImGui::GetCurrentContext()->NavIdIsAlive) {
            if (++lost >= 2) { g_focus_page = true; lost = 0; }
        } else {
            lost = 0;
        }
    }
    // the prompts, on the ground: what each button does here and now (drawn over the overlays' dimming)
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float fy = o.y + sz.y - foot, gy = fy + S(14), gh2 = foot - S(14);   // below the grass
    auto shadow_text = [&](ImVec2 at, ImU32 c, const char* s) {
        fg->AddText(g_font, S(21), ImVec2(at.x + 2, at.y + 2), col::ink, s);
        fg->AddText(g_font, S(21), at, c, s);
    };
    {
        const ImVec2 ds = g_font->CalcTextSizeA(S(21), FLT_MAX, 0, dev);
        shadow_text(ImVec2(o.x + S(28), gy + (gh2 - ds.y) / 2), col::text, dev);
    }
    float px = o.x + sz.x - S(28);
    std::set<int> seen;   // the first prompt given for a button wins
    std::vector<Prompt> listed;
    for (const Prompt& p : g_prompts)
        if (seen.insert((int)p.act).second) listed.push_back(p);
    const float gsz = floorf(S(34));
    for (auto it = listed.rbegin(); it != listed.rend(); ++it) {   // right to left: each glyph measured, then drawn
        const bool pair_next = std::next(it) != listed.rend() && std::next(it)->label.empty();   // LB, then RB "Pages"
        const ImVec2 ts = g_font->CalcTextSizeA(S(21), FLT_MAX, 0, it->label.c_str());
        px -= ts.x;
        shadow_text(ImVec2(px, gy + (gh2 - ts.y) / 2), col::text, it->label.c_str());
        if (!it->label.empty()) px -= S(10);
        const float gw = glyph(nullptr, ImVec2(0, 0), it->act, gsz);
        px -= gw;
        glyph(fg, ImVec2(px, gy + (gh2 - gsz) / 2), it->act, gsz);
        px -= pair_next ? S(6) : S(30);   // LB beside RB: one "Pages"
    }
    ImGui::End();
}

// ============================================================================================================
// start-up, the loop, the window's placement
// ============================================================================================================

static std::string exe_dir_of(const char* argv0)
{
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    (void)argv0;
    return path_str(fs::path(std::wstring(buf, n)).parent_path());
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return path_str(ec ? fs::absolute(argv0).parent_path() : p.parent_path());
#endif
}

static std::string font_path()
{
#ifdef _WIN32
    const std::string win = known_folder(kLocalAppData);   // fonts installed for the user, then the system's
    const char* sys[] = { "C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf" };
    (void)win;
    for (const char* f : sys)
        if (fs::exists(f)) return f;
#else
    const char* sys[] = { "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" };
    for (const char* f : sys)
        if (fs::exists(f)) return f;
#endif
    return {};
}

// --nav-test FILE: a controller driven by a script, inside the launcher only (no input reaches any other window): one
// step a line -- up, down, left, right (the D-pad), a, b, x, y, lb, rb, menu, view [times]; rs FRAMES (the right
// stick down); wait FRAMES; shot NAME (the launcher saves its own frame, NAME.bmp beside the script); quit. Each step
// is logged with the page, any dialog and the focused item after it settles (FILE.log)
struct NavScript {
    struct Step { std::string op; int n = 1; std::string arg; };
    std::vector<Step> steps;
    size_t at = 0;
    int wait = 30, phase = 0, done_n = 0, rs = 0, idle = 0;
    std::string dir, shot;
    FILE* log = nullptr;
    bool active() const { return !steps.empty(); }
    void load(const char* path)
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            Step st;
            if (!(ls >> st.op) || st.op[0] == '#') continue;
            std::string rest;   // a count, or a name (a shot's name is a name, even "01-play")
            if (st.op == "expect") {   // "expect TEXT": the rest of the line
                std::getline(ls, rest);
                st.arg = rest.substr(std::min(rest.size(), rest.find_first_not_of(' ')));
            } else if (ls >> rest) {
                if (st.op != "shot" && isdigit((unsigned char)rest[0])) st.n = atoi(rest.c_str());
                else st.arg = rest;
            }
            steps.push_back(st);
        }
        dir = path_str(u8path(path).parent_path());
        log = fopen((std::string(path) + ".log").c_str(), "w");
    }
    static ImGuiKey key(const std::string& op)
    {
        if (op == "up") return ImGuiKey_GamepadDpadUp;
        if (op == "down") return ImGuiKey_GamepadDpadDown;
        if (op == "left") return ImGuiKey_GamepadDpadLeft;
        if (op == "right") return ImGuiKey_GamepadDpadRight;
        if (op == "a") return ImGuiKey_GamepadFaceDown;
        if (op == "b") return ImGuiKey_GamepadFaceRight;
        if (op == "x") return ImGuiKey_GamepadFaceLeft;
        if (op == "y") return ImGuiKey_GamepadFaceUp;
        if (op == "lb") return ImGuiKey_GamepadL1;
        if (op == "rb") return ImGuiKey_GamepadR1;
        if (op == "menu") return ImGuiKey_GamepadStart;
        if (op == "view") return ImGuiKey_GamepadBack;
        return ImGuiKey_None;
    }
    void note(const Step& st)
    {
        if (!log) return;
        const char* dlg = g_confirm.open ? g_confirm.title.c_str() : g_msg.open ? g_msg.title.c_str()
                        : g_browser.open ? "file browser" : g_osk.open ? "keyboard" : g_uninst.open ? "uninstall"
                        : g_op ? "operation" : "";
        fprintf(log, "%-6s %-14s page=%-12s dialog=%-28s focus=%s%s%s\n", st.op.c_str(), st.arg.c_str(), kPageNames[g_page],
                dlg, g_focus_label.c_str(), g_bg ? "  [preparing]" : "", g_nav_event.c_str());
        g_nav_event.clear();
        fflush(log);
    }
    void inject(ImGuiIO& io)
    {
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        g_device = Device::Pad;
        g_saw_input = true;
        // the right stick as a value of the launcher's own, not an input event: a connected controller's backend reports
        // its stick every frame, and two differing values a frame would queue up (ImGui trickles them) and delay the rest
        g_nav_rstick = rs > 1 ? 1.0f : 0.0f;
        if (rs > 0) { rs--; return; }
        if (wait > 0) { wait--; return; }
        if (idle > 0) {   // "idle N": until the background preparation ends (N frames at most)
            if (g_bg && --idle > 0) return;
            idle = 0;
        }
        if (at >= steps.size()) return;
        Step& st = steps[at];
        const ImGuiKey k = key(st.op);
        if (k != ImGuiKey_None) {
            io.AddKeyAnalogEvent(k, phase == 0, phase == 0 ? 1.0f : 0.0f);
            if (phase == 0) { phase = 1; wait = 2; return; }
            phase = 0;
            if (++done_n < st.n) { wait = 6; return; }
            done_n = 0;
            wait = 12;
        } else if (st.op == "wait") {
            wait = st.n;
        } else if (st.op == "rs") {
            rs = st.n + 1;
        } else if (st.op == "idle") {
            idle = st.n;
        } else if (st.op == "expect") {   // the focus must be on this (part of its label), or the test stops at once:
            // a script out of step must never press A on something it didn't mean to
            if (g_focus_label.find(st.arg) == std::string::npos) {
                if (log) fprintf(log, "FAILED: expected the focus on \"%s\", it is on \"%s\": the test stops here\n",
                                 st.arg.c_str(), g_focus_label.c_str());
                at = steps.size();
                g_quit = true;
                return;
            }
        } else if (st.op == "shot") {
            shot = st.arg;
        } else if (st.op == "quit") {
            g_quit = true;
        }
        at++;
        pending_note = true;
        noted = st;
    }
    bool pending_note = false;
    Step noted;
    void after_render(SDL_Renderer* ren)
    {
        if (pending_note && wait <= 1 && phase == 0) { note(noted); pending_note = false; }
        if (shot.empty()) return;
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(ren, &w, &h);
        if (SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888)) {
            const std::string out = dir + "/" + shot + ".bmp";
            if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) != 0 ||
                SDL_SaveBMP(sf, out.c_str()) != 0)
                if (log) fprintf(log, "shot %s failed: %s\n", shot.c_str(), SDL_GetError());
            SDL_FreeSurface(sf);
        } else if (log) {
            fprintf(log, "shot %s: no surface (%s)\n", shot.c_str(), SDL_GetError());
        }
        shot.clear();
    }
} nav;

int main(int argc, char** argv)
{
    g_exe_dir = exe_dir_of(argv[0]);
    bool after_update = false, update_failed = false;
    std::string show;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--after-update")) after_update = true;
        if (!strcmp(argv[i], "--update-failed")) update_failed = true;
        if (!strcmp(argv[i], "--show") && i + 1 < argc) show = argv[i + 1];   // a dialog opened at start (the review's)
        if (!strcmp(argv[i], "--nav-test") && i + 1 < argc) {   // the scripted controller test
            nav.load(argv[i + 1]);
            g_nav_test = true;
        }
        if (!strcmp(argv[i], "--page") && i + 1 < argc)   // a page to open on (a shortcut's choice; the review's)
            for (int p = 0; p < kPages; p++)
                if (!SDL_strcasecmp(argv[i + 1], kPageNames[p])) g_page = p;
    }
    g_prefs.load();
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");   // sharp text at any display scale
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // --nav-test: the scripted test's window is never shown (it reads its pictures back itself) unless
    // CUBIC_NAV_HIDDEN=0 asks for it -- which the test harness's desktop guard (tests/desktop_guard.py) sets only
    // while the owner has approved an on-screen run
    const char* hid = getenv("CUBIC_NAV_HIDDEN");
    const Uint32 hidden = g_nav_test && !(hid && *hid == '0') ? SDL_WINDOW_HIDDEN : 0;
    SDL_Window* win = SDL_CreateWindow("Cubic.Heroes", g_prefs.x, g_prefs.y, g_prefs.w, g_prefs.h,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | hidden |
                                       (g_prefs.maximized && !hidden ? SDL_WINDOW_MAXIMIZED : 0));
    if (!win) {   // the remembered place may be on a display that is gone
        win = SDL_CreateWindow("Cubic.Heroes", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | hidden);
    }
    if (!win) return 1;
    g_window = win;
    SDL_SetWindowMinimumSize(win, 960, 600);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);   // no GPU acceleration: still a launcher
    if (!ren) return 1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    if (getenv("CUBICHEROES_DEBUG_NAV"))   // a developer's look at where the focus goes, on stdout
        ImGui::GetCurrentContext()->DebugLogFlags |= ImGuiDebugLogFlags_EventFocus | ImGuiDebugLogFlags_EventPopup |
                                                    ImGuiDebugLogFlags_EventNav | ImGuiDebugLogFlags_OutputToTTY;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavEscapeClearFocusItem = false;   // B (Escape) never drops the focus: the next press continues from it
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll);   // any controller, connected or plugged in later
    ImGui_ImplSDLRenderer2_Init(ren);
    {   // the scale follows the window (as each frame does after)
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        g_scale = std::max(0.75f, std::min(4.0f, floorf(std::min(w / 1280.0f, h / 800.0f) * 20.0f + 0.5f) / 20.0f));
    }
    apply_theme();
    ImGui::GetStyle().ScaleAllSizes(g_scale);
    const std::string fp = font_path();
    g_font = fp.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(fp.c_str(), S(kBody));
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    ImGui::GetStyle().FontSizeBase = S(kBody);
    refresh_info();
    // --show: each dialog as a player meets it, opened once the page has settled; nothing done by it (closing it is
    // all it does)
    auto open_show = [&show] {
        if (show == "browser") browse("Choose your disc image", false, { ".iso" }, "disc images (.iso)", "", [](const std::string&) {});
        if (show == "keyboard") type_text("A note for this backup", "before the boss", [](const std::string&) {});
        if (show == "confirm")
            ask("Go back to this build?", "The game starts from it next time. The build you have now is kept, so you can "
                "come back to it.", "Go back to it", false, [] {});
        if (show == "first-run") after_install_shaders();   // the offer after an install (nothing installed)
        if (show == "uninstall") uninstall_open();          // the choice, measured (a dry run: nothing removed)
        if (show == "prepare") bg_prepare(false, "asked for");   // a real preparation, in the background
        if (show == "rebuild") bg_prepare(true, "asked for");    // the same, the pipeline cache made again
        if (show == "message")
            g_msg = { true, "The update didn't finish", "The download didn't match the release's signature, so nothing was "
                      "changed. Try again later.", "release.json: checked against key 1 -- the signature differs", true, false };
        show.clear();
    };
    int frames = 0;
    if (update_failed)
        g_msg = { true, "The update didn't start", "The new launcher didn't come up, so this one was put back. Nothing else "
                                                   "changed.", "", true, false };
    bool wrote_health = false, quit = false;
    bool checked_updates = false;
    while (!quit && !g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYDOWN: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEWHEEL: case SDL_TEXTINPUT:
                g_device = Device::Keys;
                g_saw_input = true;
                break;
            case SDL_MOUSEMOTION:   // (not during a scripted controller test: someone else's mouse is no player)
                if (!nav.active() && std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 3) { g_device = Device::Keys; g_saw_input = true; }
                break;
            case SDL_CONTROLLERBUTTONDOWN: {
                g_device = Device::Pad;
                g_saw_input = true;
                if (SDL_GameController* gc = SDL_GameControllerFromInstanceID(e.cbutton.which))
                    g_pad_detected = style_from(SDL_GameControllerGetType(gc), SDL_GameControllerName(gc));
                break;
            }
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs(e.caxis.value) > 12000) g_device = Device::Pad;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_GameController* gc = SDL_GameControllerOpen(e.cdevice.which)) g_pads.push_back(gc);
                [[fallthrough]];
            case SDL_CONTROLLERDEVICEREMOVED:
                if (e.type == SDL_CONTROLLERDEVICEREMOVED)
                    for (size_t j = 0; j < g_pads.size(); j++)
                        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[j])) == e.cdevice.which) {
                            SDL_GameControllerClose(g_pads[j]);
                            g_pads.erase(g_pads.begin() + j);
                            break;
                        }
                if (!g_saw_input || g_device != Device::Pad)   // the first controller at hand names the prompts
                    for (int j = 0; j < SDL_NumJoysticks(); j++)
                        if (SDL_IsGameController(j)) {
                            g_pad_detected = style_from(SDL_GameControllerTypeForIndex(j), SDL_GameControllerNameForIndex(j));
                            break;
                        }
                break;
            case SDL_DROPFILE: {   // a disc image or a keys file dropped onto the window
                std::string p = e.drop.file;
                SDL_free(e.drop.file);
                std::string ext = path_str(u8path(p).extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                g_page = kInstall;
                std::error_code dec;
                if (ext == ".iso" || fs::is_directory(u8path(p), dec)) { g_wiz.disc = p; g_prefs.disc = p; probe_disc(); }
                else { g_wiz.keys = p; g_prefs.keys = p; probe_keys(); }
                g_prefs.save();
                break;
            }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
                break;
            }
        }
        {   // the scale follows the window: 1280x800 is 1; the smaller ratio rules (ultrawide: the height)
            int w, h;
            SDL_GetWindowSize(win, &w, &h);
            float want = std::min(w / 1280.0f, h / 800.0f);
            want = std::max(0.75f, std::min(4.0f, floorf(want * 20.0f + 0.5f) / 20.0f));
            if (fabsf(want - g_scale) > 0.001f) {
                g_scale = want;
                ImGui::GetStyle() = ImGuiStyle();
                apply_theme();
                ImGui::GetStyle().ScaleAllSizes(g_scale);
                ImGui::GetStyle().FontSizeBase = S(kBody);
            }
        }
        {   // the launcher's own display mode and VSync (Settings, Launcher): applied as they change
            static std::string mode_now = "windowed";
            static int vsync_now = 1;
            const std::string mode = setting("LAUNCHER_DISPLAY_MODE", "windowed");
            if (mode != mode_now) {
                if (mode == "exclusive") {   // the display's own mode: its resolution and refresh rate unchanged
                    SDL_DisplayMode dm;
                    if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) != 0 ||
                        SDL_SetWindowDisplayMode(win, &dm) != 0 || SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN) != 0)
                        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);   // not supported: borderless
                } else {
                    SDL_SetWindowFullscreen(win, mode == "borderless" ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
                mode_now = mode;
            }
            const int vsync = setting("LAUNCHER_VSYNC", "1") == "1";
            if (vsync != vsync_now) { SDL_RenderSetVSync(ren, vsync); vsync_now = vsync; }
            static Uint64 last = SDL_GetPerformanceCounter();
            if (!vsync) {   // no VSync: at most 240 frames a second, not a busy loop
                const double since = (double)(SDL_GetPerformanceCounter() - last) / SDL_GetPerformanceFrequency();
                if (since < 1.0 / 240) SDL_Delay((Uint32)((1.0 / 240 - since) * 1000));
            }
            last = SDL_GetPerformanceCounter();
        }
        g_pad_connected = SDL_NumJoysticks() > 0;
        if (g_pad_connected && !g_saw_input) g_device = Device::Pad;   // a controller at hand: its prompts from the start
        io.ConfigNavCursorVisibleAlways = g_device == Device::Pad || nav.active();
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        g_prompt_choice = setting("LAUNCHER_PROMPTS", "auto");
        stick_nav(io);
        if (nav.active()) nav.inject(io);   // the scripted controller (--nav-test)
        ImGui::NewFrame();
        if (!checked_updates && !g_st.settings.is_null()) {
            checked_updates = true;
            if (setting("LAUNCHER_CHECK_UPDATES", "1") == "1") check_updates();
        }
        frame();
        if (++frames == 20 && !show.empty()) open_show();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 14, 18, 44, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        if (nav.active()) nav.after_render(ren);
        SDL_RenderPresent(ren);
        if (after_update && !wrote_health) {   // the new launcher is up: the update is kept (tools/updates.py)
            write_text(path_str(u8path(g_exe_dir) / ".started-ok"), "ok\n");
            wrote_health = true;
        }
    }
    // the window's placement for next time
    const Uint32 fl = SDL_GetWindowFlags(win);
    g_prefs.maximized = (fl & SDL_WINDOW_MAXIMIZED) != 0;
    if (!g_prefs.maximized && !(fl & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))) {
        SDL_GetWindowPosition(win, &g_prefs.x, &g_prefs.y);
        SDL_GetWindowSize(win, &g_prefs.w, &g_prefs.h);
    }
    g_prefs.save();
    g_queries.clear();
    g_op.reset();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
