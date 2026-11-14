// Cubic.Heroes' launcher (M9.4; docs/M9_LAUNCHER_PLAN.md sections 3, 8 and 14): the single entry point for installing,
// configuring, updating, maintaining and playing. A native window (SDL2, Dear ImGui) driving the pipeline worker
// (tools/worker.py, docs/M9_WORKER_PROTOCOL.md); usable with a controller alone -- every page, the file browser and
// an on-screen keyboard -- or with a keyboard and mouse, the button prompts following the last device used.
#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <chrono>
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
    }
    void save() const
    {
        char win[200];
        snprintf(win, sizeof win, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"maximized\":%s}", x, y, w, h,
                 maximized ? "true" : "false");
        write_text(file(), "{\n \"install\": " + json::quote(install) + ",\n \"disc\": " + json::quote(disc) +
                               ",\n \"keys\": " + json::quote(keys) + ",\n \"window\": " + win + "\n}\n");
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
// look: a dark, high-contrast theme with large type, a teal accent and a clearly drawn focus
// ============================================================================================================

namespace col {
const ImU32 bg = IM_COL32(9, 12, 22, 255), panel = IM_COL32(19, 25, 41, 255), panel2 = IM_COL32(28, 36, 58, 255);
const ImU32 accent = IM_COL32(57, 211, 192, 255), accent_dim = IM_COL32(57, 211, 192, 70);
const ImU32 warm = IM_COL32(255, 181, 71, 255), text = IM_COL32(234, 238, 247, 255), dim = IM_COL32(140, 150, 172, 255);
const ImU32 danger = IM_COL32(255, 92, 108, 255), ok = IM_COL32(92, 219, 139, 255);
}  // namespace col

static ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static ImFont* g_font = nullptr;   // one font, drawn at several sizes (Dear ImGui 1.92+'s dynamic fonts)
static float g_scale = 1.0f;
static float S(float px) { return px * g_scale; }
static const float kBody = 21.0f, kSmall = 17.0f, kTitle = 34.0f, kHuge = 54.0f;

static void apply_theme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.ChildRounding = 14; s.FrameRounding = 10; s.PopupRounding = 14; s.GrabRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(20, 12); s.ItemSpacing = ImVec2(14, 14);
    s.ItemInnerSpacing = ImVec2(10, 8); s.ScrollbarSize = 14; s.ScrollbarRounding = 8; s.WindowBorderSize = 0;
    s.ChildBorderSize = 0; s.PopupBorderSize = 0; s.FrameBorderSize = 0;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = v4(col::bg); c[ImGuiCol_ChildBg] = v4(col::panel); c[ImGuiCol_PopupBg] = v4(col::panel);
    c[ImGuiCol_Text] = v4(col::text); c[ImGuiCol_TextDisabled] = v4(col::dim);
    c[ImGuiCol_Button] = v4(col::panel2); c[ImGuiCol_ButtonHovered] = v4(IM_COL32(40, 52, 82, 255));
    c[ImGuiCol_ButtonActive] = v4(col::accent_dim);
    c[ImGuiCol_FrameBg] = v4(col::panel2); c[ImGuiCol_FrameBgHovered] = v4(IM_COL32(40, 52, 82, 255));
    c[ImGuiCol_FrameBgActive] = v4(IM_COL32(48, 62, 96, 255));
    c[ImGuiCol_Header] = v4(col::panel2); c[ImGuiCol_HeaderHovered] = v4(IM_COL32(40, 52, 82, 255));
    c[ImGuiCol_HeaderActive] = v4(col::accent_dim);
    c[ImGuiCol_NavCursor] = v4(col::accent); c[ImGuiCol_PlotHistogram] = v4(col::accent);
    c[ImGuiCol_ScrollbarBg] = v4(IM_COL32(0, 0, 0, 0)); c[ImGuiCol_ScrollbarGrab] = v4(col::panel2);
    c[ImGuiCol_ModalWindowDimBg] = v4(IM_COL32(0, 0, 0, 170)); c[ImGuiCol_CheckMark] = v4(col::accent);
    c[ImGuiCol_SliderGrab] = v4(col::accent); c[ImGuiCol_Separator] = v4(col::panel2);
}

// ============================================================================================================
// input: the device last used decides the prompts; page actions from the controller or their keys
// ============================================================================================================

enum class Device { Pad, Keys };
static Device g_device = Device::Keys;
static bool g_saw_input = false;   // the first input decides the prompts; before it, a connected controller does
static bool g_pad_connected = false;

enum class Act { A, B, X, Y, LB, RB, Start, Back };

static bool pressed(Act a)
{
    using namespace ImGui;
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

// a button's glyph: the controller's face buttons in their colours, bumpers and menu buttons as pills, or a keycap
static float glyph(ImDrawList* dl, ImVec2 p, Act a, float h)
{
    const bool pad = g_device == Device::Pad;
    const char* t = "";
    ImU32 fill = col::panel2;
    bool round = false;
    if (pad) {
        switch (a) {
        case Act::A: t = "A"; fill = IM_COL32(64, 176, 74, 255); round = true; break;
        case Act::B: t = "B"; fill = IM_COL32(214, 60, 60, 255); round = true; break;
        case Act::X: t = "X"; fill = IM_COL32(44, 112, 220, 255); round = true; break;
        case Act::Y: t = "Y"; fill = IM_COL32(222, 178, 40, 255); round = true; break;
        case Act::LB: t = "LB"; break;
        case Act::RB: t = "RB"; break;
        case Act::Start: t = "MENU"; break;
        case Act::Back: t = "VIEW"; break;
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
    const float fs = h * 0.6f;
    const ImVec2 ts = g_font->CalcTextSizeA(fs, FLT_MAX, 0, t);
    if (!dl) return round ? h : std::max(h, ts.x + h * 0.6f);   // measured, not drawn
    if (round) {
        dl->AddCircleFilled(ImVec2(p.x + h / 2, p.y + h / 2), h / 2, fill, 24);
        dl->AddText(g_font, fs, ImVec2(p.x + (h - ts.x) / 2, p.y + (h - ts.y) / 2), IM_COL32_WHITE, t);
        return h;
    }
    const float w = std::max(h, ts.x + h * 0.6f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), pad ? IM_COL32(60, 68, 92, 255) : col::panel2, h * (pad ? 0.5f : 0.22f));
    if (!pad) dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(90, 100, 128, 255), h * 0.22f, 1.5f);
    dl->AddText(g_font, fs, ImVec2(p.x + (w - ts.x) / 2, p.y + (h - ts.y) / 2), col::text, t);
    return w;
}

struct Prompt { Act act; std::string label; };
static std::vector<Prompt> g_prompts;   // this frame's, drawn in the bottom bar
static void prompt(Act a, const std::string& label) { g_prompts.push_back({ a, label }); }

// ============================================================================================================
// widgets: big buttons with a clear focus, cards, a selector, status lines
// ============================================================================================================

static void focus_ring(float rounding = 10.0f, ImU32 ring = col::accent)
{
    if (!ImGui::IsItemFocused() || !ImGui::GetIO().NavVisible) return;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(ImVec2(a.x - S(4), a.y - S(4)), ImVec2(b.x + S(4), b.y + S(4)), ring, rounding + S(4), S(3));
    dl->AddRect(ImVec2(a.x - S(8), a.y - S(8)), ImVec2(b.x + S(8), b.y + S(8)), col::accent_dim, rounding + S(8), S(2));
}

enum class Kind { Normal, Primary, Danger };

static bool big_button(const char* label, ImVec2 size = ImVec2(0, 0), Kind k = Kind::Normal, bool enabled = true)
{
    if (k == Kind::Primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, v4(col::accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(IM_COL32(90, 230, 212, 255)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(IM_COL32(40, 170, 156, 255)));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(IM_COL32(8, 20, 24, 255)));
    } else if (k == Kind::Danger) {
        ImGui::PushStyleColor(ImGuiCol_Button, v4(IM_COL32(92, 30, 40, 255)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, v4(IM_COL32(130, 40, 54, 255)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, v4(col::danger));
        ImGui::PushStyleColor(ImGuiCol_Text, v4(col::text));
    }
    if (size.y == 0) size.y = S(56);
    ImGui::BeginDisabled(!enabled);
    const bool r = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (k != Kind::Normal) ImGui::PopStyleColor(4);
    focus_ring(10.0f, k == Kind::Primary ? IM_COL32_WHITE : col::accent);   // a teal button: a white ring
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

static void title(const char* t, float size = kTitle)
{
    ImGui::PushFont(nullptr, S(size));
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

static bool card_begin(const char* id, ImVec2 size = ImVec2(0, 0))
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(26), S(22)));
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding |
                                                     (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0), 0);
    ImGui::PopStyleVar();
    return open;
}
static void card_end() { ImGui::EndChild(); }

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

static Op* run(const std::string& cmd, const std::vector<std::string>& args, const std::string& title, bool long_op,
               std::function<void(const json::Value&)> then)
{
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
} g_confirm;

static void ask(const std::string& t, const std::string& text, const std::string& yes, bool danger, std::function<void()> f)
{
    g_confirm = { true, t, text, yes, danger, std::move(f) };
}

struct Message {
    bool open = false;
    std::string title, text, details;
    bool error = false, show_details = false;
} g_msg;

static void show_result_error(const json::Value& r, const std::string& what)
{
    g_msg = { true, what, r["message"].str("Something went wrong."), r["details"].str(), true, false };
}

// --- the file browser: drives and known folders, folders first, then the files the filter takes ---
struct Browser {
    bool open = false, folders = false;   // folders: a folder is what is chosen
    std::string title, dir, filter_desc;
    std::vector<std::string> exts;        // lower-case, with the dot; empty: any file
    std::function<void(const std::string&)> on_pick;
    struct Entry { std::string name, path; bool dir; uintmax_t size; };
    std::vector<Entry> entries;
    std::string error;
    bool refocus = true;

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
    void go(const std::string& d)
    {
        dir = d;
        list();
        refocus = true;
    }
    void up()
    {
        if (dir.empty()) { open = false; return; }
        fs::path p = u8path(dir);
        fs::path parent = p.parent_path();
        if (parent == p || path_str(p).size() <= 3) go("");
        else go(path_str(parent));
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
    std::error_code ec;
    std::string d = start;
    while (!d.empty() && !fs::is_directory(u8path(d), ec)) {
        std::string up = path_str(u8path(d).parent_path());
        if (up == d) { d.clear(); break; }
        d = up;
    }
    g_browser.go(d.empty() ? known_folder(kDownloads) : d);
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

static std::string install_dir() { return g_prefs.install.empty() ? default_install() : g_prefs.install; }

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
            });
        }
        if (kind == "slots" || kind == "setup-app")
            run("settings", { "--install", install_dir() }, "", false, [](const json::Value& s) { g_st.settings = s["data"]; });
    });
}

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

// ============================================================================================================
// pages
// ============================================================================================================

enum Page { kPlay, kInstall, kUpdates, kBuilds, kSaves, kShaders, kSettings, kDiagnostics, kPages };
static const char* kPageNames[kPages] = { "Play", "Install", "Updates", "Builds", "Saves", "Shaders", "Settings", "Diagnostics" };
static int g_page = kPlay;
static bool g_focus_page = true;   // give the new page's first item the focus

static void focus_first()
{
    if (g_focus_page) {
        ImGui::SetKeyboardFocusHere(-1);   // the item just drawn
        g_focus_page = false;
    }
}

static void start_game()
{
    const std::string exe = path_str(u8path(install_dir()) / "game" /
#ifdef _WIN32
                                     "dotheroes.exe"
#else
                                     "dotheroes"
#endif
    );
#ifdef _WIN32
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = launcher::quote_arg(launcher::widen(exe));
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, launcher::widen(install_dir()).c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        g_st.playing = "The game is running.";
    } else {
        g_msg = { true, "The game didn't start", "Its program couldn't be started. Try Repair on the Install page.",
                  exe, true, false };
    }
#else
    if (fork() == 0) { setsid(); execl(exe.c_str(), exe.c_str(), (char*)nullptr); _exit(127); }
    g_st.playing = "The game is running.";
#endif
}

static void prepare_now(bool rebuild)
{
    std::vector<std::string> a = { "--install", install_dir() };
    if (rebuild) a.push_back("--rebuild");
    run("prepare", a, rebuild ? "Rebuilding the shader cache" : "Preparing shaders and pipelines", true, [](const json::Value& r) {
        if (!r["ok"].truthy()) show_result_error(r, "Preparing shaders didn't finish");
        refresh_info();
    });
}

static void page_play()
{
    const std::string kind = g_st.info["kind"].str();
    ImGui::PushFont(nullptr, S(kHuge));
    ImGui::TextUnformatted("3D Dot Game Heroes");
    ImGui::PopFont();
    text_col(col::dim, "Cubic.Heroes -- the native PC port, built on this PC from your own disc.");
    ImGui::Dummy(ImVec2(0, S(10)));
    if (!g_st.info_ok && g_st.info.is_null()) {
        text_col(col::dim, "Looking for the game...");
        return;
    }
    if (kind == "slots") {
        const json::Value& b = g_st.builds;
        ImGui::PushFont(nullptr, S(kTitle));
        const bool play = big_button("PLAY", ImVec2(S(360), S(92)), Kind::Primary);
        ImGui::PopFont();
        if (play) start_game();
        focus_first();
        if (ImGui::IsItemFocused()) prompt(Act::A, "Play");
        ImGui::SameLine(0, S(24));
        ImGui::BeginGroup();
        text_col(col::text, "Build %s", b["active"].str("-").c_str());
        if (!g_st.playing.empty()) text_col(col::ok, "%s", g_st.playing.c_str());
        else text_col(col::dim, "%s", install_dir().c_str());
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, S(8)));
        // shaders
        card_begin("##shaders_card", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - S(8), S(190)));
        title("Shaders and pipelines", 24);
        const json::Value& c = g_st.check;
        if (!g_st.check_error.empty()) {
            text_col(col::danger, "No usable Vulkan device was found: the game can't start. %s", g_st.check_error.c_str());
        } else if (c.is_null()) {
            text_col(col::dim, "Checking...");
        } else if (c["prepared"].truthy()) {
            text_col(col::ok, "Ready for this GPU and driver (%s pipelines).", c["recipes"].str("0").c_str());
            text_col(col::dim, "%s, driver %s", c["gpu"]["name"].str().c_str(), c["gpu"]["driver"].str().c_str());
        } else if (c["recipes"].num() > 0) {
            text_col(col::warm, "A preparation is due (%s). It takes a few seconds and prevents stutter.",
                     c["cache"].str().c_str());
            if (big_button("Prepare now")) prepare_now(false);
        } else {
            text_col(col::dim, "Prepared as you play: each new effect is kept for next time.");
        }
        card_end();
        ImGui::SameLine();
        card_begin("##update_card", ImVec2(0, S(190)));
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
        if (pressed(Act::Y)) check_updates();
    } else if (kind == "setup-app") {
        title("An install made by the setup app was found", 26);
        text_col(col::dim, "Move it into the launcher's layout: your disc isn't needed again, your saves and settings stay.");
        if (big_button("Move it now", ImVec2(S(320), 0), Kind::Primary))
            run("migrate", { "--install", install_dir() }, "Moving your install", true, [](const json::Value& r) {
                if (!r["ok"].truthy()) show_result_error(r, "Moving the install didn't finish");
                refresh_info();
            });
        focus_first();
    } else {
        title(kind == "partial" ? "An install was interrupted" : "The game isn't installed yet", 26);
        text_col(col::dim, kind == "partial" ? "Continue it on the Install page: what was done is kept."
                                             : "You'll need your own disc image (a decrypted ISO) and its keys file.");
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
    json::Value disc_info, keys_info, space;
    std::string disc_err, keys_err, space_err;
    bool probing_disc = false, probing_keys = false;
} g_wiz;

static void probe_disc()
{
    g_wiz.disc_info = json::Value();
    g_wiz.disc_err.clear();
    g_wiz.probing_disc = true;
    run("probe-disc", { "--disc", g_wiz.disc }, "", false, [](const json::Value& r) {
        g_wiz.probing_disc = false;
        if (r["ok"].truthy()) g_wiz.disc_info = r["data"];
        else g_wiz.disc_err = r["message"].str();
        if (!g_wiz.keys.empty()) { g_wiz.keys_info = json::Value(); }
    });
}

static void probe_keys()
{
    g_wiz.keys_info = json::Value();
    g_wiz.keys_err.clear();
    if (g_wiz.disc.empty()) return;
    g_wiz.probing_keys = true;
    run("probe-keys", { "--disc", g_wiz.disc, "--keys", g_wiz.keys }, "", false, [](const json::Value& r) {
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
    ImGui::PushFont(nullptr, S(26));
    ImGui::TextColored(v4(ok ? col::ok : col::dim), ok ? "  %d  " : "  %d  ", n);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    title(name, 24);
    if (busy) text_col(col::dim, "Checking...");
    else if (!err.empty()) text_col(col::danger, "%s", err.c_str());
    else if (!value.empty()) text_col(ok ? col::text : col::dim, "%s", value.c_str());
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(230));
    if (big_button(button, ImVec2(S(220), 0))) on_press();
    card_end();
}

static void page_install()
{
    title("Install");
    const std::string kind = g_st.info["kind"].str();
    if (g_wiz.folder.empty()) { g_wiz.folder = install_dir(); check_space(); }
    if (g_wiz.disc.empty() && !g_prefs.disc.empty()) { g_wiz.disc = g_prefs.disc; probe_disc(); }
    if (g_wiz.keys.empty() && !g_prefs.keys.empty()) g_wiz.keys = g_prefs.keys;
    text_col(col::dim, kind == "slots" ? "Repair or reinstall from your disc: your saves, settings and kept builds stay."
                                       : "Your own disc and keys, checked before anything is written. You can also drop the "
                                         "files onto this window.");
    ImGui::Dummy(ImVec2(0, S(4)));
    const bool disc_ok = !g_wiz.disc_info.is_null();
    std::string dv = g_wiz.disc.empty() ? "Choose your disc image (a decrypted ISO) or the disc's folder."
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
    focus_first();
    const bool keys_ok = !g_wiz.keys_info.is_null();
    if (disc_ok && !g_wiz.keys.empty() && g_wiz.keys_info.is_null() && g_wiz.keys_err.empty() && !g_wiz.probing_keys) probe_keys();
    std::string kv = g_wiz.keys.empty() ? "Choose the keys file for your disc (or an EBOOT.elf you decrypted)."
                                        : g_wiz.keys + (keys_ok ? "\nThe keys match this disc." : "");
    step_row(2, "Its keys", keys_ok, kv, g_wiz.keys_err, g_wiz.probing_keys, "Choose...", [] {
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
    step_row(3, "Where to install it", space_ok, fv, g_wiz.space_err, false, "Choose...", [] {
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
        std::vector<std::string> a = { "--disc", g_wiz.disc, "--keys", g_wiz.keys, "--install", g_wiz.folder };
        if (resume) a.push_back("--resume");
        run("install", a, "Installing 3D Dot Game Heroes", true, [](const json::Value& r) {
            if (!r["ok"].truthy() && r["code"].str() != "cancelled") show_result_error(r, "The install didn't finish");
            else if (r["ok"].truthy()) {
                g_msg = { true, "Installed", "The game is ready to play.", "", false, false };
                g_page = kPlay;
                g_focus_page = true;
            }
            refresh_info();
        });
    }
    if (!ready) text_col(col::dim, "Each step above must be checked first.");
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

// --- builds and rollback ---
static void page_builds()
{
    title("Game builds");
    text_col(col::dim, "Each update builds the game anew beside the last one. Going back is instant and works offline.");
    const json::Value& b = g_st.builds["builds"];
    bool first = true;
    for (size_t i = b.size(); i-- > 0;) {
        const json::Value& x = b[i];
        const std::string id = x["id"].str(), state = x["state"].str();
        card_begin(("##b" + id).c_str());
        title(id.c_str(), 22);
        text_col(state == "active" ? col::ok : col::dim, "%s  --  version %s, made %s%s", state == "active" ? "Active" : "Kept for rollback",
                 x["version"].str("?").c_str(), x["made"].str("?").c_str(),
                 x["damaged"].is_null() ? "" : ("  --  damaged: " + x["damaged"].str()).c_str());
        if (state != "active") {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(250));
            if (big_button(("Go back to it##" + id).c_str(), ImVec2(S(240), 0))) {
                ask("Go back to this build?", "Build " + id + " becomes the one the game starts. Your saves and settings "
                    "stay as they are. You can come forward again the same way.", "Go back", false, [id] {
                    run("rollback", { "--install", install_dir(), "--to", id }, "Going back", true, [](const json::Value& r) {
                        if (!r["ok"].truthy()) show_result_error(r, "Going back didn't work");
                        refresh_info();
                    });
                });
            }
            if (first) { focus_first(); first = false; }
        }
        card_end();
    }
    if (b.size() == 0) text_col(col::dim, "No builds yet.");
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
        text_col(col::text, "GPU: %s (driver %s, Vulkan %s)", c["gpu"]["name"].str().c_str(), c["gpu"]["driver"].str().c_str(),
                 c["gpu"]["vulkan"].str().c_str());
        text_col(c["prepared"].truthy() ? col::ok : col::warm, "The pipeline cache: %s, %s.",
                 c["prepared"].truthy() ? "ready for this GPU and driver" : c["cache"].str().c_str(),
                 human_bytes(c["cache_mb"].num() * 1048576).c_str());
        text_col(col::dim, "Pipelines known on this PC: %s (recorded as you play).", c["recipes"].str("0").c_str());
    }
    card_end();
    if (big_button("Prepare now", ImVec2(S(300), 0), Kind::Primary)) prepare_now(false);
    focus_first();
    ImGui::SameLine();
    if (big_button("Rebuild the cache", ImVec2(S(300), 0)))
        ask("Rebuild the shader cache?", "The pipeline cache is made again from scratch for this GPU and driver. It takes "
            "a few seconds; nothing else changes.", "Rebuild", false, [] { prepare_now(true); });
}

// --- settings ---
static void page_settings()
{
    title("Settings");
    text_col(col::dim, "Shared with the game's own menus. A setting marked * takes effect when the game next starts.");
    const json::Value& s = g_st.settings["settings"];
    if (s.size() == 0) { text_col(col::dim, "Install the game first."); return; }
    for (size_t i = 0; i < g_st.settings["problems"].size(); i++)
        text_col(col::warm, "%s", g_st.settings["problems"][i].str().c_str());
    static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> choices = {
        { "DOTHEROES_FPS", { { "60", "60 fps" }, { "30", "30 fps (the original)" }, { "0", "Unlimited" } } },
        { "LAUNCHER_UPDATE_CHANNEL", { { "stable", "Stable releases" }, { "prerelease", "Test releases too" } } },
        { "LAUNCHER_LOG_LEVEL", { { "info", "Normal" }, { "debug", "Detailed" } } },
    };
    std::string last_cat;
    bool first = true;
    ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    // the categories in a fixed order, each setting in the schema's order within it
    std::vector<std::pair<std::string, const json::Value*>> ordered;
    static const char* cats[] = { "Display", "Updates", "Saves", "Diagnostics" };
    for (const char* c : cats)
        for (const auto& kv : s.o)
            if (kv.second["category"].str() == c) ordered.push_back({ kv.first, &kv.second });
    for (const auto& kv0 : ordered) {
        const std::pair<std::string, const json::Value&> kv(kv0.first, *kv0.second);
        const json::Value& x = kv.second;
        const std::string cat = x["category"].str();
        if (cat != last_cat) { ImGui::Dummy(ImVec2(0, S(6))); title(cat.c_str(), 24); last_cat = cat; }
        const std::string key = kv.first, val = x["value"].str();
        std::string shown = val;
        auto ch = choices.find(key);
        std::vector<std::string> cycle;
        if (ch != choices.end()) {
            for (auto& c : ch->second) { cycle.push_back(c.first); if (c.first == val) shown = c.second; }
        } else if (key == "PS3RECOMP_VK_FULLSCREEN" || key == "LAUNCHER_CHECK_UPDATES") {
            cycle = { "0", "1" };
            shown = val == "1" ? "On" : "Off";
        } else if (key == "LAUNCHER_BUILDS_KEPT") {
            cycle = { "0", "1", "2", "3", "4", "5" };
        } else if (key == "LAUNCHER_SAVE_BACKUPS_KEPT") {
            cycle = { "1", "3", "5", "10", "20", "50", "100" };
        }
        std::string label = x["label"].str(key) + (x["applies"].str() == "restart" ? " *" : "");
        char btn[300];
        snprintf(btn, sizeof btn, "<     %s     >##%s", shown.c_str(), key.c_str());   // left and right change it
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine(S(520));
        int dir = 0;
        if (big_button(btn, ImVec2(S(340), S(50)))) dir = 1;
        if (first) { focus_first(); first = false; }
        if (ImGui::IsItemFocused()) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dir = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dir = 1;
            prompt(Act::A, "Change");
        }
        if (dir && !cycle.empty()) {
            auto it = std::find(cycle.begin(), cycle.end(), val);
            int at = it == cycle.end() ? 0 : (int)(it - cycle.begin());
            at = (at + dir + (int)cycle.size()) % (int)cycle.size();
            run("settings", { "--install", install_dir(), "--set", key + "=" + cycle[at] }, "", false, [](const json::Value& r) {
                if (r["ok"].truthy()) g_st.settings = r["data"];
                else show_result_error(r, "That setting wasn't saved");
            });
        }
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    if (big_button("Restore the defaults", ImVec2(S(340), 0)))
        ask("Restore every setting's default?", "Every setting goes back to its default. Your saves are not touched.",
            "Restore", false, [] {
            run("settings", { "--install", install_dir(), "--reset", "all" }, "", false, [](const json::Value& r) {
                if (r["ok"].truthy()) g_st.settings = r["data"];
            });
        });
    ImGui::SameLine();
    if (big_button("Add to Steam", ImVec2(S(260), 0)))
        ask("Add the launcher to Steam?", "It appears in your Steam library (as a non-Steam game) and opens this launcher. "
            "Close Steam first: it rewrites its library when it exits.", "Add it", false, [] {
            run("shortcuts", { "--exe", path_str(u8path(g_exe_dir) / "CubicHeroes.exe"), "--steam" }, "", false,
                [](const json::Value& r) {
                    if (r["ok"].truthy()) g_msg = { true, "Added to Steam", "Start Steam to see it in your library.", "", false, false };
                    else show_result_error(r, "It wasn't added to Steam");
                });
        });
    ImGui::SameLine();
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
} g_diag;

static void page_diagnostics()
{
    title("Diagnostics");
    text_col(col::dim, "A report for a bug or a question: you see every file before it is packed. Never in it: your saves, "
                       "keys, the game's files. Your user name, folders and addresses are replaced.");
    if (big_button("Make a report", ImVec2(S(300), 0), Kind::Primary)) {
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
    ImGui::SameLine();
    if (big_button("Pack it as a zip", ImVec2(S(300), 0))) {
        char name[80];
        std::time_t now = std::time(nullptr);
        std::strftime(name, sizeof name, "CubicHeroes-report-%Y%m%d-%H%M%S.zip", std::localtime(&now));
        const std::string zip = path_str(u8path(known_folder(kDocuments)) / name);
        run("diagnostics", { "--install", install_dir(), "--out", g_diag.folder, "--pack", zip }, "Packing the report", true,
            [zip](const json::Value& r) {
                if (r["ok"].truthy()) g_msg = { true, "Report packed", "Saved in your Documents folder.", zip, false, false };
                else show_result_error(r, "The report wasn't packed");
            });
    }
    ImGui::BeginChild("##files", ImVec2(S(380), 0), ImGuiChildFlags_NavFlattened);
    for (size_t i = 0; i < g_diag.files.size(); i++) {
        const std::string f = g_diag.files[i]["file"].str();
        if (!fs::exists(u8path(g_diag.folder) / f)) continue;
        if (ImGui::Selectable((f + "\n" + human_bytes(g_diag.files[i]["bytes"].num())).c_str(), g_diag.viewing == f, 0,
                              ImVec2(0, S(56)))) {
            g_diag.viewing = f;
            g_diag.view_text = read_text(path_str(u8path(g_diag.folder) / f), 200000);
        }
        focus_ring();
        if (ImGui::IsItemFocused()) { prompt(Act::A, "View"); prompt(Act::X, "Leave out of the report"); }
        if (ImGui::IsItemFocused() && pressed(Act::X)) {
            std::error_code ec;
            fs::remove(u8path(g_diag.folder) / f, ec);
            if (g_diag.viewing == f) g_diag.viewing.clear();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##view", ImVec2(0, 0), ImGuiChildFlags_FrameStyle);
    if (g_diag.viewing.empty()) text_col(col::dim, "Choose a file to see what it holds.");
    else ImGui::TextUnformatted(g_diag.view_text.c_str());
    ImGui::EndChild();
}

// ============================================================================================================
// overlays drawn over the pages
// ============================================================================================================

static void overlay_op()
{
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
        title(op.title.c_str(), 30);
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - op.t0).count();
        std::string line = "Elapsed " + human_time(now);
        if (op.eta >= 0) line += "  --  " + std::string(op.eta_basis == "reference" ? "about " : "") + human_time(op.eta) + " left";
        text_col(col::dim, "%s", line.c_str());
        ImGui::ProgressBar(op.plan.empty() ? (op.fraction >= 0 ? (float)op.fraction : -1.0f * (float)ImGui::GetTime())
                                           : (float)op.overall, ImVec2(-1, S(22)), "");
        ImGui::Dummy(ImVec2(0, S(4)));
        for (auto& st : op.plan) {
            const std::string s = op.state.count(st.first) ? op.state[st.first] : "";
            const ImU32 c = s == "done" ? col::ok : s == "skipped" ? col::dim : st.first == op.stage ? col::accent : col::dim;
            const char* mark = s == "done" ? "[done]" : s == "skipped" ? "[not needed]" : st.first == op.stage ? "[now]" : "";
            text_col(c, "%s  %s", st.second.c_str(), mark);
            if (st.first == op.stage && op.fraction >= 0) {
                ImGui::ProgressBar((float)op.fraction, ImVec2(-1, S(8)), "");
            }
        }
        if (!op.detail.empty()) text_col(col::dim, "%s", op.detail.c_str());
        ImGui::Dummy(ImVec2(0, S(6)));
        if (big_button(op.job->cancel_sent ? "Cancelling..." : "Cancel", ImVec2(S(240), 0), Kind::Danger, !op.job->cancel_sent) ||
            (pressed(Act::B) && !op.job->cancel_sent))
            op.job->cancel();
        ImGui::SetItemDefaultFocus();
        prompt(Act::B, "Cancel");
        ImGui::EndPopup();
    }
}

static void overlay_confirm()
{
    if (!g_confirm.open) return;
    ImGui::OpenPopup("##confirm");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(80), S(760)), 0));
    if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        title(g_confirm.title.c_str(), 28);
        ImGui::TextWrapped("%s", g_confirm.text.c_str());
        ImGui::Dummy(ImVec2(0, S(8)));
        bool yes = big_button(g_confirm.yes.c_str(), ImVec2(S(260), 0), g_confirm.danger ? Kind::Danger : Kind::Primary);
        ImGui::SameLine();
        bool no = big_button("Cancel", ImVec2(S(260), 0));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);   // the safe choice first
        prompt(Act::A, "Choose");
        prompt(Act::B, "Cancel");
        if (pressed(Act::B)) no = true;
        if (yes || no) {
            auto f = g_confirm.on_yes;
            g_confirm.open = false;
            ImGui::CloseCurrentPopup();
            if (yes && f) f();
        }
        ImGui::EndPopup();
    }
}

static void overlay_message()
{
    if (!g_msg.open) return;
    ImGui::OpenPopup("##msg");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(80), S(820)), 0));
    if (ImGui::BeginPopupModal("##msg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
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
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
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
        if (!b.dir.empty()) {
            if (ImGui::Selectable("..   (up a folder)", false, 0, ImVec2(0, S(48)))) b.up();
            focus_ring();
            if (b.refocus) { ImGui::SetKeyboardFocusHere(-1); }
        }
        bool picked = false;
        for (size_t i = 0; i < b.entries.size() && !picked; i++) {
            const Browser::Entry& e = b.entries[i];
            std::string label = (e.dir ? "[folder]  " : "") + e.name + (e.dir ? "" : "    " + human_bytes((double)e.size));
            if (ImGui::Selectable((label + "##" + std::to_string(i)).c_str(), false, 0, ImVec2(0, S(48)))) {
                if (e.dir) { b.go(e.path); break; }
                auto f = b.on_pick;
                std::string p = e.path;
                b.open = false;
                ImGui::CloseCurrentPopup();
                picked = true;
                if (f) f(p);
            }
            focus_ring();
            if (b.refocus && b.dir.empty() && i == 0) ImGui::SetKeyboardFocusHere(-1);
        }
        b.refocus = false;
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

static void overlay_osk()
{
    if (!g_osk.open) return;
    ImGui::OpenPopup("##osk");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(60), S(1100)), 0));
    if (ImGui::BeginPopupModal("##osk", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
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
                if (first && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
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

static bool overlay_open() { return g_op || g_confirm.open || g_msg.open || g_browser.open || g_osk.open; }

static void frame()
{
    g_prompts.clear();
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
    // the header
    const float head = S(84);
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + head), col::panel);
    dl->AddRectFilled(ImVec2(vp->Pos.x, vp->Pos.y + head - S(3)), ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + head), col::accent_dim);
    dl->AddText(g_font, S(30), ImVec2(vp->Pos.x + S(28), vp->Pos.y + S(24)), col::accent, "CUBIC");
    dl->AddText(g_font, S(30), ImVec2(vp->Pos.x + S(28) + g_font->CalcTextSizeA(S(30), FLT_MAX, 0, "CUBIC").x + S(2),
                                      vp->Pos.y + S(24)), col::text, ".HEROES");
    // the page tabs: LB/RB (or Q/E, or a click)
    float x = vp->Pos.x + S(300);
    const float gh = S(30);
    x += glyph(dl, ImVec2(x, vp->Pos.y + (head - gh) / 2), Act::LB, gh) + S(14);
    for (int i = 0; i < kPages; i++) {
        const ImVec2 ts = g_font->CalcTextSizeA(S(22), FLT_MAX, 0, kPageNames[i]);
        const ImVec2 a(x, vp->Pos.y + S(18)), b(x + ts.x + S(28), vp->Pos.y + head - S(18));
        if (i == g_page) {
            dl->AddRectFilled(a, b, col::accent, S(10));
            dl->AddText(g_font, S(22), ImVec2(x + S(14), a.y + (b.y - a.y - ts.y) / 2), IM_COL32(8, 20, 24, 255), kPageNames[i]);
        } else {
            dl->AddText(g_font, S(22), ImVec2(x + S(14), a.y + (b.y - a.y - ts.y) / 2), col::dim, kPageNames[i]);
        }
        if (!overlay_open() && ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(0)) { g_page = i; g_focus_page = true; }
        x = b.x + S(6);
    }
    glyph(dl, ImVec2(x + S(8), vp->Pos.y + (head - gh) / 2), Act::RB, gh);
    if (!overlay_open()) {
        if (pressed(Act::LB)) { g_page = (g_page + kPages - 1) % kPages; g_focus_page = true; }
        if (pressed(Act::RB)) { g_page = (g_page + 1) % kPages; g_focus_page = true; }
    }
    // the device in use
    const char* dev = g_device == Device::Pad ? "Controller" : g_pad_connected ? "Keyboard and mouse (a controller is connected)"
                                                                              : "Keyboard and mouse";
    const ImVec2 dsz = g_font->CalcTextSizeA(S(16), FLT_MAX, 0, dev);
    dl->AddText(g_font, S(16), ImVec2(vp->Pos.x + vp->Size.x - dsz.x - S(24), vp->Pos.y + head + S(8)), col::dim, dev);
    // the page
    const float foot = S(64);
    ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + S(48), vp->Pos.y + head + S(28)));
    ImGui::BeginChild("##page", ImVec2(vp->Size.x - S(96), vp->Size.y - head - foot - S(40)), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(nullptr, S(kBody));
    switch (g_page) {
    case kPlay: page_play(); break;
    case kInstall: page_install(); break;
    case kUpdates: page_updates(); break;
    case kBuilds: page_builds(); break;
    case kSaves: page_saves(); break;
    case kShaders: page_shaders(); break;
    case kSettings: page_settings(); break;
    case kDiagnostics: page_diagnostics(); break;
    }
    ImGui::PopFont();
    ImGui::EndChild();
    // overlays
    ImGui::PushFont(nullptr, S(kBody));
    overlay_op();
    overlay_browser();
    overlay_osk();
    overlay_confirm();
    overlay_message();
    ImGui::PopFont();
    // the prompts
    if (!overlay_open()) {
        prompt(Act::A, "Select");
        prompt(Act::LB, "");
        prompt(Act::RB, "Pages");
    }
    const float fy = vp->Pos.y + vp->Size.y - foot;
    dl->AddRectFilled(ImVec2(vp->Pos.x, fy), ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), col::panel);
    float px = vp->Pos.x + vp->Size.x - S(28);
    std::set<int> seen;   // the first prompt given for a button wins
    std::vector<Prompt> shown;
    for (const Prompt& p : g_prompts)
        if (seen.insert((int)p.act).second) shown.push_back(p);
    for (auto it = shown.rbegin(); it != shown.rend(); ++it) {   // right to left: each glyph measured, then drawn
        const bool pair_next = std::next(it) != shown.rend() && std::next(it)->label.empty();   // LB, then RB "Pages"
        const ImVec2 ts = g_font->CalcTextSizeA(S(19), FLT_MAX, 0, it->label.c_str());
        px -= ts.x;
        dl->AddText(g_font, S(19), ImVec2(px, fy + (foot - ts.y) / 2), col::text, it->label.c_str());
        if (!it->label.empty()) px -= S(10);
        const float gw = glyph(nullptr, ImVec2(0, 0), it->act, S(32));
        px -= gw;
        glyph(dl, ImVec2(px, fy + (foot - S(32)) / 2), it->act, S(32));
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
    const char* sys[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf" };
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

int main(int argc, char** argv)
{
    g_exe_dir = exe_dir_of(argv[0]);
    bool after_update = false, update_failed = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--after-update")) after_update = true;
        if (!strcmp(argv[i], "--update-failed")) update_failed = true;
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
    SDL_Window* win = SDL_CreateWindow("Cubic.Heroes", g_prefs.x, g_prefs.y, g_prefs.w, g_prefs.h,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (g_prefs.maximized ? SDL_WINDOW_MAXIMIZED : 0));
    if (!win) {   // the remembered place may be on a display that is gone
        win = SDL_CreateWindow("Cubic.Heroes", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    }
    if (!win) return 1;
    SDL_SetWindowMinimumSize(win, 960, 600);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);   // no GPU acceleration: still a launcher
    if (!ren) return 1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll);   // any controller, connected or plugged in later
    ImGui_ImplSDLRenderer2_Init(ren);
    g_scale = std::max(1.0f, ImGui_ImplSDL2_GetContentScaleForDisplay(SDL_GetWindowDisplayIndex(win)));
    {   // a window smaller than the content needs: the scale down to fit
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        g_scale = std::min(g_scale, std::max(0.75f, std::min(w / 1280.0f, h / 800.0f) * g_scale));
    }
    apply_theme();
    ImGui::GetStyle().ScaleAllSizes(g_scale);
    const std::string fp = font_path();
    g_font = fp.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(fp.c_str(), S(kBody));
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    ImGui::GetStyle().FontSizeBase = S(kBody);
    refresh_info();
    if (update_failed)
        g_msg = { true, "The update didn't start", "The new launcher didn't come up, so this one was put back. Nothing else "
                                                   "changed.", "", true, false };
    bool wrote_health = false, quit = false;
    bool checked_updates = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            switch (e.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYDOWN: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEWHEEL: case SDL_TEXTINPUT:
                g_device = Device::Keys;
                g_saw_input = true;
                break;
            case SDL_MOUSEMOTION:
                if (std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 3) { g_device = Device::Keys; g_saw_input = true; }
                break;
            case SDL_CONTROLLERBUTTONDOWN: g_device = Device::Pad; g_saw_input = true; break;
            case SDL_CONTROLLERAXISMOTION:
                if (std::abs(e.caxis.value) > 12000) g_device = Device::Pad;
                break;
            case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
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
        g_pad_connected = SDL_NumJoysticks() > 0;
        if (g_pad_connected && !g_saw_input) g_device = Device::Pad;   // a controller at hand: its prompts from the start
        io.ConfigNavCursorVisibleAlways = g_device == Device::Pad;
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        if (!checked_updates && !g_st.settings.is_null()) {
            checked_updates = true;
            if (setting("LAUNCHER_CHECK_UPDATES", "1") == "1") check_updates();
        }
        frame();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 9, 12, 22, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
        if (after_update && !wrote_health) {   // the new launcher is up: the update is kept (tools/updates.py)
            write_text(path_str(u8path(g_exe_dir) / ".started-ok"), "ok\n");
            wrote_health = true;
        }
    }
    // the window's placement for next time
    const Uint32 fl = SDL_GetWindowFlags(win);
    g_prefs.maximized = (fl & SDL_WINDOW_MAXIMIZED) != 0;
    if (!g_prefs.maximized && !(fl & SDL_WINDOW_MINIMIZED)) {
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
