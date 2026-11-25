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

#include <cerrno>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "worker_client.h"

#ifndef CUBIC_VERSION   // tools/build_launcher.py passes the release's (tools/version.py); cli.h says the same
#define CUBIC_VERSION "0.0.0-dev"
#endif
#include "game_exit.h"   // the game's exit statuses explained (M11.4, the review's W-2)

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

// Before M11.2 the launcher kept its own files in the user's profile and installed the game there by default: read
// only now, to find an earlier installation to use or copy (nothing is written there any more)
static std::string legacy_data_dir()
{
    return path_str(u8path(known_folder(kAppData)) / (
#ifdef _WIN32
        "CubicHeroes"
#else
        "cubic-heroes"
#endif
    ));
}

static std::string legacy_default_install()
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

// The file's folder is made first: an installation need not have it yet (a beta.2 or beta.3 installation upgraded by
// hand has no downloads/, where the game's up-to-date marker goes -- without it the marker was never written, and
// every start brought the game up to date again, undoing a rollback of the game; found in beta.4's qualification).
static bool write_text(const std::string& path, const std::string& text)
{
    const fs::path p = u8path(path), tmp = u8path(path + ".part");
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << text;
        f.close();
        if (!f) { fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

// ============================================================================================================
// M11.2b: the installation root -- every file the launcher, its worker and the game manage lives under it (docs/CLI.md):
// the launcher's preferences (config/launcher.json), the game's settings (config/dotheroes.cfg), the builds, the
// game data and the saves, the caches, the logs and crash reports, the downloads and the temporary files. By default
// the launcher's own folder: the whole folder can be moved or copied. --install-dir PATH, CUBIC_INSTALL_DIR, or the
// folder named in cubicheroes-root.txt beside the launcher (one line; a relative path is taken from the launcher's
// folder) choose another, in that order.
// ============================================================================================================
static std::string g_root, g_root_from;

static std::string env_u8(const char* name)
{
#ifdef _WIN32
    const wchar_t* w = _wgetenv(launcher::widen(name).c_str());
    return w ? launcher::narrow(w) : std::string();
#else
    const char* v = getenv(name);
    return v ? v : "";
#endif
}

// the folder the program was started from, as a player sees it: an AppImage's own file's folder (Linux: the program
// itself runs from the image's read-only mount, $APPIMAGE names the file), else the program's folder
static std::string home_dir_of_program()
{
#ifndef _WIN32
    const std::string img = env_u8("APPIMAGE");
    if (!img.empty()) return path_str(u8path(img).parent_path());
#endif
    return g_exe_dir;
}
static bool in_appimage()
{
#ifndef _WIN32
    return !env_u8("APPIMAGE").empty();
#else
    return false;
#endif
}

static std::string root_pointer_file() { return path_str(u8path(home_dir_of_program()) / "cubicheroes-root.txt"); }

static std::string trimmed(std::string t)
{
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    size_t a = 0;
    while (a < t.size() && (t[a] == ' ' || t[a] == '\t' || (unsigned char)t[a] == 0xEF || (unsigned char)t[a] == 0xBB ||
                            (unsigned char)t[a] == 0xBF)) a++;   // spaces, a UTF-8 byte-order mark
    return t.substr(a);
}

static std::string resolve_root(const std::string& cli)
{
    std::string r = cli, from = "--install-dir";
    if (r.empty()) { r = env_u8("CUBIC_INSTALL_DIR"); from = "CUBIC_INSTALL_DIR"; }
    if (r.empty()) {
        const std::string t = trimmed(read_text(root_pointer_file(), 8192));
        const size_t nl = t.find_first_of("\r\n");
        r = nl == std::string::npos ? t : trimmed(t.substr(0, nl));
        from = "cubicheroes-root.txt";
    }
    if (r.empty()) {   // the default: the program's folder (an AppImage's: a Cubic.Heroes folder beside its file)
        r = in_appimage() ? path_str(u8path(home_dir_of_program()) / "Cubic.Heroes") : g_exe_dir;
        from = in_appimage() ? "beside the AppImage" : "the launcher's folder";
    }
    fs::path p = u8path(r);
    // a relative pointer is taken from the program's folder (it moves with it); a relative --install-dir or
    // CUBIC_INSTALL_DIR from the current folder, as any command line's paths
    if (p.is_relative() && from == "cubicheroes-root.txt") p = u8path(home_dir_of_program()) / p;
    std::error_code ec;
    const fs::path a = fs::absolute(p, ec);
    g_root_from = from;
    std::string out = path_str((ec ? p : a).lexically_normal());
    while (out.size() > 3 && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    return out;
}

static bool same_folder(const std::string& a, const std::string& b)
{
    std::error_code ec;
    const bool eq = fs::equivalent(u8path(a), u8path(b), ec);
    return ec ? u8path(a).lexically_normal() == u8path(b).lexically_normal() : eq;
}

// the root kept for the next start: no pointer for the launcher's own folder (the default), else its path
static bool remember_root(const std::string& dir)
{
    std::error_code ec;
    const fs::path ptr = u8path(root_pointer_file());
    const std::string dflt = in_appimage() ? path_str(u8path(home_dir_of_program()) / "Cubic.Heroes") : g_exe_dir;
    if (same_folder(dir, dflt)) { fs::remove(ptr, ec); return !fs::exists(ptr, ec); }
    return write_text(root_pointer_file(), dir + "\n");
}

// a folder under the root, made when first needed
static std::string root_sub(const char* sub)
{
    const fs::path p = u8path(g_root) / sub;
    std::error_code ec;
    fs::create_directories(p, ec);
    return path_str(p);
}
static std::string data_dir() { return root_sub("config"); }   // the launcher's own preferences
static std::string log_dir() { return root_sub("logs"); }

// whether files can be made in the root (a read-only folder, a protected one, a full drive refuse); create: the root
// made when it isn't there yet -- else (a look only: --status) the nearest folder above it that is there is tried
static bool root_writable(std::string& why, bool create = true)
{
    std::error_code ec;
    fs::path dir = u8path(g_root);
    if (create) {
        fs::create_directories(dir, ec);
        if (ec) { why = ec.message(); return false; }
    } else {
        while (!fs::exists(dir, ec)) {
            if (!dir.has_parent_path() || dir.parent_path() == dir) { why = "its drive isn't there"; return false; }
            dir = dir.parent_path();
        }
        if (!fs::is_directory(dir, ec)) { why = "a file is in the way (" + path_str(dir) + ")"; return false; }
    }
    const fs::path probe = dir / (".write-test-" + std::to_string((unsigned long)
#ifdef _WIN32
        GetCurrentProcessId()
#else
        getpid()
#endif
    ));
    bool ok;
    {
        std::ofstream f(probe, std::ios::binary);
        ok = f && (f << "ok") && f.flush();
    }
    fs::remove(probe, ec);
    if (!ok) why = "files can't be made in it (read-only, protected, or the drive is full)";
    return ok;
}

struct Prefs {
    std::string legacy_install, disc, keys;   // legacy_install: an earlier launcher's install folder (read, never written)
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, w = 1280, h = 800;
    bool maximized = false;
    bool prepared_once = false;   // shaders prepared at least once: a new build's empty cache is an update's
    bool steam_cleanup = false;   // an uninstall left the launcher's Steam entry (Steam ran): removed once it's closed
    std::string legacy_answered;  // M11.2b: the earlier installation the player was offered (not offered again)

    std::string file() const { return path_str(u8path(data_dir()) / "launcher.json"); }
    void load()
    {
        json::Value v;
        const std::string legacy = path_str(u8path(legacy_data_dir()) / "launcher.json");
        if (!json::parse(read_text(file()), v)) {   // none in the root yet: an earlier launcher's choices, if any
            if (!json::parse(read_text(legacy), v)) return;
            legacy_install = v["install"].str();
        }
        disc = v["disc"].str();
        keys = v["keys"].str();
        if (v["window"]["w"].num() >= 640 && v["window"]["h"].num() >= 400) {
            x = (int)v["window"]["x"].num(x); y = (int)v["window"]["y"].num(y);
            w = (int)v["window"]["w"].num(); h = (int)v["window"]["h"].num();
        }
        maximized = v["window"]["maximized"].truthy();
        prepared_once = v["prepared_once"].truthy();
        steam_cleanup = v["steam_cleanup"].truthy();
        legacy_answered = v["legacy_answered"].str();
    }
    void save() const
    {
        char win[200];
        snprintf(win, sizeof win, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"maximized\":%s}", x, y, w, h,
                 maximized ? "true" : "false");
        write_text(file(), "{\n \"disc\": " + json::quote(disc) +
                               ",\n \"keys\": " + json::quote(keys) + ",\n \"window\": " + win +
                               ",\n \"prepared_once\": " + (prepared_once ? "true" : "false") +
                               ",\n \"steam_cleanup\": " + (steam_cleanup ? "true" : "false") +
                               ",\n \"legacy_answered\": " + json::quote(legacy_answered) + "\n}\n");
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
    // the job's log in the root's logs/ -- not for a root that doesn't exist yet, which only an install or a copy
    // into it creates (a --status of a folder that isn't there leaves it not there)
    std::error_code dec;
    // (--version asks the worker its own version: nothing to keep)
    if (cmd != "version" && (fs::is_directory(u8path(g_root), dec) || cmd == "install" || cmd == "migrate-root")) {
        av.push_back("--log");
        av.push_back(path_str(u8path(log_dir()) / ("worker-" + cmd + ".jsonl")));
    }
    return av;
}

// ============================================================================================================
// beta.4: the launcher's own update, installed on restart (tools/updates.py). The worker verifies and stages it
// (apply-update) and starts its helper, which swaps the launcher's files once this launcher has exited; an AppImage
// installation's AppImage file is replaced instead (its image is read-only). Everything stays inside the folder.
// ============================================================================================================

// where the launcher's update keeps its files: the launcher's folder (portable: the installation's root too); an
// AppImage's, its installation's root (the launcher itself runs from the image's read-only mount)
static std::string launcher_update_dir() { return in_appimage() ? g_root : g_exe_dir; }

static std::string appimage_file()
{
#ifndef _WIN32
    return env_u8("APPIMAGE");
#else
    return std::string();
#endif
}

// the arguments every launcher-update command of the worker takes
static std::vector<std::string> launcher_update_args()
{
    std::vector<std::string> a = { "--install", g_root, "--launcher", launcher_update_dir() };
    const std::string img = appimage_file();
    if (!img.empty()) { a.push_back("--appimage"); a.push_back(img); }
    return a;
}

static unsigned long this_pid()
{
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

// a program that outlives the launcher (the update's helper): detached, outside any job of the launcher's
static bool start_detached(const std::vector<std::string>& argv, const std::string& cwd)
{
#ifdef _WIN32
    std::wstring cmd;
    for (const std::string& a : argv) cmd += (cmd.empty() ? L"" : L" ") + launcher::quote_arg(launcher::widen(a));
    // Its standard handles given explicitly (the null device): the worker is a windowed program, and without them its
    // first output raised an error that PyInstaller shows as a dialog -- a recovery that never ran, and a window
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                             0, nullptr);
    const BOOL inherit = nul != INVALID_HANDLE_VALUE;
    if (inherit) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = si.hStdOutput = si.hStdError = nul;
    }
    const DWORD base = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, inherit, base | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                             launcher::widen(cwd).c_str(), &si, &pi);
    if (!ok) ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, inherit, base, nullptr, launcher::widen(cwd).c_str(),
                                 &si, &pi);   // a job that refuses the breakaway
    if (inherit) CloseHandle(nul);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {   // a grandchild runs it, so nothing is left for this process to wait for
        setsid();
        if (fork() != 0) _exit(0);
        if (chdir(cwd.c_str()) != 0) _exit(127);
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        // and nothing else this process holds (beta.4, the Linux side's D5): an AppImage's runtime keeps its mount
        // alive through a descriptor every program it starts inherits (fd 1023, not close-on-exec). A recovery that
        // kept it would keep the old image mounted -- the runtime could never end, and the recovery, waiting for it,
        // never finish. Plain close() calls (async-signal-safe: the launcher has threads), up to the open-file limit
        struct rlimit rl;
        const long top = getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < (1u << 20)
                             ? (long)rl.rlim_cur : (1l << 20);
        for (long fd = 3; fd < top; fd++) close((int)fd);
        std::vector<char*> av;
        for (const std::string& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execv(av[0], av.data());
        _exit(127);
    }
    int st = 0;
    waitpid(child, &st, 0);
    return true;
#endif
}

// the worker that recovers an interrupted update: the helper's copy when there is one (worker/ itself may be the
// part the interruption left half-replaced), else worker/ (or this checkout's)
static std::vector<std::string> recovery_argv()
{
#ifdef _WIN32
    const char* name = "DotHeroes-Setup.exe";
#else
    const char* name = "DotHeroes-Setup";
#endif
    std::error_code ec;
    const fs::path helper = u8path(launcher_update_dir()) / "downloads" / "launcher-helper" / name;
    std::vector<std::string> av;
    if (fs::exists(helper, ec)) av = { path_str(helper), "--worker", "recover-launcher" };
    else {
        av = worker_argv("recover-launcher", {});
        while (!av.empty() && av.back() != "recover-launcher") av.pop_back();   // without --control and --log
    }
    return av;
}

// the file that says how the last launcher update ended (tools/updates.py: downloads/launcher-update-last.json)
static json::Value last_launcher_update()
{
    json::Value v;
    json::parse(read_text(path_str(u8path(launcher_update_dir()) / "downloads" / "launcher-update-last.json"), 1u << 16), v);
    return v;
}

// the game brought up to date with this launcher's version once, after an update of the launcher (by itself, or
// by hand: a newer package unpacked over the folder): the version it was last brought up to date with
static std::string game_update_marker() { return path_str(u8path(g_root) / "downloads" / "game-up-to-date-with.txt"); }

// RecompLauncher (0.2.0-beta.5, docs/RECOMPLAUNCHER.md): a launcher that installs a game's updates itself. Started by it
// (RECOMPLAUNCHER=1 on every start it makes), or in a folder it has started us in before (its mark at the root, written
// at that first start), this launcher's own updater stays off: the two must never both change the same folder (the
// SDK's rule). The game's own update (--update; the window's at its start) stays: it changes the installation's
// builds, never the files RecompLauncher manages. A standalone installation is untouched by any of this.
static const char* const kRecompMark = "recomplauncher-managed.txt";
static bool started_by_recomplauncher() { return env_u8("RECOMPLAUNCHER") == "1"; }
static std::string recomp_mark() { return path_str(u8path(g_root) / kRecompMark); }
static bool updates_managed_elsewhere()
{
    std::error_code ec;
    return started_by_recomplauncher() || fs::exists(u8path(recomp_mark()), ec);
}
static void note_recomplauncher()
{
    std::error_code ec;
    if (started_by_recomplauncher() && !fs::exists(u8path(recomp_mark()), ec))
        write_text(recomp_mark(), "RecompLauncher " + env_u8("RECOMPLAUNCHER_VERSION") + "\n");
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
    { { "..######..", ".########.", ".#++++++#.", ".#+*++*+#.", "..++++++..", "...gggg...", "..gggggg..", ".+gggggg+.",
        "..gggggg..", "..**..**.." },   // a hero: Characters
      { IM_COL32(60, 150, 80, 255), IM_COL32(248, 200, 170, 255), IM_COL32(70, 44, 30, 255), IM_COL32(96, 178, 100, 255) } },
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

enum class Act { A, B, X, Y, LB, RB, LT, RT, Start, Back };

// false while the page is drawn under a dialog: the page's own buttons and prompts belong to the dialog then
static bool g_page_input = true;
static std::string g_focus_label;   // the focused item's name, for the scripted controller test's log
static bool g_focus_owns_lr = false;  // the focused item uses left and right itself (a settings value)
static bool g_quit = false;   // the launcher closes after this frame
static bool g_nav_test = false;   // a scripted controller test (--nav-test) drives the launcher: the game never starts
static std::string g_nav_event;   // what the test's log notes beside the next step
static std::vector<std::string> g_nav_titles;   // the test's "see": the titles drawn wholly in view in the last frame
static float g_nav_rstick = 0;    // the test's right stick (beside a real controller's own, which the backend reports)

static bool pressed(Act a)
{
    using namespace ImGui;
    if (!g_page_input) return false;
    // a text field being typed in (the Characters page's search) keeps the keyboard: its letters, Escape and the
    // page keys are text there, not shortcuts (a controller's buttons still act)
    const bool kb = !GetIO().WantTextInput;
    switch (a) {
    case Act::A: return IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    case Act::B: return IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || (kb && IsKeyPressed(ImGuiKey_Escape, false));
    case Act::X: return IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || (kb && IsKeyPressed(ImGuiKey_X, false));
    case Act::Y: return IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || (kb && IsKeyPressed(ImGuiKey_Y, false));
    case Act::LB: return IsKeyPressed(ImGuiKey_GamepadL1, false) || (kb && (IsKeyPressed(ImGuiKey_Q, false) ||
                                                                            IsKeyPressed(ImGuiKey_PageUp, false)));
    case Act::RB: return IsKeyPressed(ImGuiKey_GamepadR1, false) || (kb && (IsKeyPressed(ImGuiKey_E, false) ||
                                                                            IsKeyPressed(ImGuiKey_PageDown, false)));
    case Act::LT: return IsKeyPressed(ImGuiKey_GamepadL2, false) || (kb && IsKeyPressed(ImGuiKey_LeftBracket, false));
    case Act::RT: return IsKeyPressed(ImGuiKey_GamepadR2, false) || (kb && IsKeyPressed(ImGuiKey_RightBracket, false));
    case Act::Start: return IsKeyPressed(ImGuiKey_GamepadStart, false) || (kb && IsKeyPressed(ImGuiKey_F10, false));
    case Act::Back: return IsKeyPressed(ImGuiKey_GamepadBack, false) || (kb && IsKeyPressed(ImGuiKey_F1, false));
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
    // M11.2g: a manual choice holds whatever was used last (as in the game); automatic follows the device
    const bool pad = g_prompt_choice == "auto" ? g_device == Device::Pad : g_prompt_choice != "keyboard";
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
            case Act::LT: t = sony || steam || gen ? "L2" : nin ? "ZL" : "LT"; break;
            case Act::RT: t = sony || steam || gen ? "R2" : nin ? "ZR" : "RT"; break;
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
        case Act::LT: t = "["; break;
        case Act::RT: t = "]"; break;
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
    ImRect drawn;
    for (size_t i = 0; i < lines.size(); i++) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        pix_text(dl, ImVec2(at.x, at.y + p), lines[i].c_str(), p, c);
        ImGui::Dummy(ImVec2(pix_w(lines[i].c_str(), p), p * 9));
        if (i == 0) drawn = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        else drawn.Add(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
    }
    // a scripted controller test's "see": this title wholly in the visible part of its page (a card's clip is the page's)
    if (g_nav_test && ImGui::GetCurrentWindow()->ClipRect.Contains(drawn)) g_nav_titles.push_back(t);
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
    std::string pending_key;   // M12: sent once when the worker asks for it ("needs": key), then wiped
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

static double g_nav_spawn_max = 0, g_nav_page_max = 0, g_nav_view_max = 0;   // the test log's split of a step's worst frame (ms)
static Op* run(const std::string& cmd, const std::vector<std::string>& args, const std::string& title, bool long_op,
               std::function<void(const json::Value&)> then)
{
    const Uint64 spawn_t0 = SDL_GetPerformanceCounter();
    struct SpawnTime { Uint64 t0; ~SpawnTime() {
        g_nav_spawn_max = std::max(g_nav_spawn_max, (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency()); } }
        spawn_time{ spawn_t0 };
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
            op.result = std::move(e);   // a large answer (1,000 characters) is moved, never copied on this thread
        } else if (ev == "needs" && e["what"].str() == "key") {
            op.job->send_line(op.pending_key.empty() ? "nokey" : "key " + op.pending_key);
            std::fill(op.pending_key.begin(), op.pending_key.end(), '\0');
            op.pending_key.clear();
        }
    }
    if (!op.finished && op.job->done()) {
        for (json::Value& e : op.job->take_events())
            if (e["ev"].str() == "result") op.result = std::move(e);
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
    bool secret = false;   // M12: shown as dots (the PS3 save key)
    std::string title, text;
    std::function<void(const std::string&)> on_done;
} g_osk;

static void type_text(const std::string& t, const std::string& init, std::function<void(const std::string&)> f,
                      bool secret = false)
{
    g_osk = Osk();
    g_osk.open = true;
    g_osk.secret = secret;
    g_osk.title = t;
    g_osk.text = init;
    g_osk.on_done = std::move(f);
}

// ============================================================================================================
// what the launcher knows of the install, refreshed from the worker
// ============================================================================================================

struct Status {
    json::Value info, builds, saves, settings, check, update;
    json::Value launcher;  // beta.4: the launcher's own update (the worker's launcher-state): ready, rollback, last
    bool info_ok = false, checking_update = false, checked_once = false;
    std::string check_error;
    std::string playing;   // a running game's note
} g_st;
// refresh_info's answers so far (whether the installation's settings are still to come: settings_pending)
static bool g_info_answered = false, g_settings_answered = false, g_check_when_settings = false;

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

static std::string install_dir() { return g_root; }
std::string install_dir_or_empty() { return install_dir(); }

void auto_prepare_check_later();

static void refresh_info();
static void maybe_update_game();

// M11.2b: an installation made before the portable root (the earlier launcher's folder in its preferences, or an
// earlier default folder: the worker's find-legacy) offered once when this folder holds none -- copied in
// (migrate-root: every file checked before anything is put in place; the earlier folder left exactly as it was).
// Declined: not offered again (`CubicHeroes --migrate-from PATH` copies one later).
static void offer_legacy()
{
    static bool asked = false;
    // a scripted controller test (--nav-test) meets it only when it asks to (CUBIC_NAV_LEGACY_OFFER=1): its own pages
    // stay as they were otherwise
    if (asked || (g_nav_test && env_u8("CUBIC_NAV_LEGACY_OFFER") != "1")) return;
    asked = true;
    run("find-legacy", { "--install", install_dir() }, "", false, [](const json::Value& r) {
        const json::Value& m = r["data"]["migratable"];
        if (!r["ok"].truthy() || m.size() == 0) return;
        const std::string from = m[(size_t)0].str();
        if (from.empty() || same_folder(from, g_prefs.legacy_answered) || g_confirm.open || g_op) return;
        g_confirm = Confirm();
        g_confirm.open = true;
        g_confirm.title = "EARLIER INSTALLATION FOUND";
        g_confirm.text = "The game was installed before, in\n" + from + "\n\nCopy that installation into this folder? "
                         "Its saves, settings and prepared game come along; the earlier folder is left as it is.";
        g_confirm.yes = "Copy It Here";
        g_confirm.no = "Not Now";
        g_confirm.focus_yes = true;
        g_confirm.on_yes = [from]() {
            g_prefs.legacy_answered = from;
            g_prefs.save();
            run("migrate-root", { "--from", from, "--install", install_dir() }, "Copying the earlier installation", true,
                [](const json::Value& c) {
                    if (c["ok"].truthy())
                        show_msg("Installation copied", "The earlier installation is now in this folder, checked file by "
                                 "file. The earlier folder is unchanged: remove it yourself once you no longer need it.");
                    else
                        show_result_error(c, "The copy did not finish");
                    refresh_info();
                });
        };
        g_confirm.on_no = [from]() {
            g_prefs.legacy_answered = from;
            g_prefs.save();
        };
    });
}

static void refresh_info()
{
    run("info", { "--install", install_dir() }, "", false, [](const json::Value& r) {
        g_st.info = r["data"];
        g_st.info_ok = r["ok"].truthy();
        g_info_answered = true;
        const std::string kind = g_st.info["kind"].str();
        if (kind == "none") offer_legacy();
        if (kind == "slots") {
            maybe_update_game();
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
                g_settings_answered = true;   // (an error too: the build's default channel then)
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

// The update channel. Before the installation's settings are known, the build's own default -- as the worker's schema
// has it (tools/settings.py: a beta's is test releases too) -- never "stable" for want of them: a beta's check made
// then asked the stable channel and found no beta (the Linux side's intermittent start-up miss, 2026-11-20).
static const char* default_update_channel() { return strchr(CUBIC_VERSION, '-') ? "prerelease" : "stable"; }
static std::string update_channel() { return setting("LAUNCHER_UPDATE_CHANNEL", default_update_channel()); }

// whether the installation's settings are still on their way (refresh_info: info, then for an installation its
// settings): a check asked for meanwhile waits for them, so it asks the channel the player chose
static bool settings_pending()
{
    if (g_settings_answered) return false;
    if (!g_info_answered) return true;
    const std::string kind = g_st.info["kind"].str();
    return kind == "slots" || kind == "setup-app";
}

static void refresh_launcher_state()
{
    run("launcher-state", launcher_update_args(), "", false, [](const json::Value& r) {
        if (r["ok"].truthy()) g_st.launcher = r["data"];
    });
}

// beta.4: the game brought up to date with this launcher's version, once -- after the launcher updated itself, or
// after a newer package was unpacked over the folder by hand. The worker's update rebuilds only what changed (a
// second when nothing did), backs up the saves first and keeps the previous build for rollback.
static void maybe_update_game()
{
    static bool done = false;
    if (done || g_nav_test) return;
    done = true;
    if (trimmed(read_text(game_update_marker(), 256)) == CUBIC_VERSION) return;
    run("update", { "--install", install_dir(), "--no-prepare" }, "Bringing the game up to date with version " CUBIC_VERSION,
        true, [](const json::Value& r) {
            if (r["ok"].truthy()) write_text(game_update_marker(), CUBIC_VERSION "\n");
            else if (r["code"].str() != "cancelled") show_result_error(r, "The game couldn't be brought up to date");
            refresh_info();
        });
}

static void check_updates()
{
    if (updates_managed_elsewhere()) { g_st.checking_update = false; return; }   // RecompLauncher's to do
    g_st.checking_update = true;
    if (settings_pending()) { g_check_when_settings = true; return; }   // after the settings (the main loop)
    g_check_when_settings = false;
    run("check-update", { "--channel", update_channel() }, "", false, [](const json::Value& r) {
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

// 0.2.0-beta.5: five pages (from seven) -- the updates are on Play, the install, the shaders, Steam and the shortcuts
// on Install / Manage; Settings holds settings only
enum Page { kPlay, kManage, kSaves, kCharacters, kSettings, kDiagnostics, kPages };
static const char* kPageNames[kPages] = { "Play", "Install / Manage", "Saves", "Characters", "Settings", "Diagnostics" };
static const int kPageIcon[kPages] = { 0, 1, 4, 10, 6, 7 };   // kIcons: the sword, chest, book, hero, gear, glass
static int g_page = kPlay;
static bool g_focus_updates = false;   // Play's update controls take the focus (and scroll into view) when next drawn

// --page NAME: a page by its name, or an older name (Install, Updates, Shaders) by the page that holds it now
static int page_from_name(const char* n)
{
    for (int p = 0; p < kPages; p++)
        if (!SDL_strcasecmp(n, kPageNames[p])) return p;
    if (!SDL_strcasecmp(n, "install") || !SDL_strcasecmp(n, "manage") || !SDL_strcasecmp(n, "shaders")) return kManage;
    if (!SDL_strcasecmp(n, "updates")) return kPlay;
    return -1;
}
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
// the game the launcher started has ended: a status that means something to the player is explained (M11.4, the
// review's W-2: 70, the graphics' start never finished)
static void game_ended(int status)
{
    if (const launcher::GameExitNote* n = launcher::game_exit_note(status))
        g_msg = { true, n->title, n->text, path_str(u8path(install_dir()) / "logs" / "game.log"), true, false };
}

static bool game_running()
{
#ifdef _WIN32
    if (!g_game_proc) return false;
    if (WaitForSingleObject(g_game_proc, 0) == WAIT_TIMEOUT) return true;
    DWORD status = 0;
    const bool known = GetExitCodeProcess(g_game_proc, &status) != 0;
    CloseHandle(g_game_proc);
    g_game_proc = nullptr;
    if (known) game_ended((int)status);
    return false;
#else
    if (g_game_pid <= 0) return false;
    int st = 0;
    const pid_t r = waitpid(g_game_pid, &st, WNOHANG);
    if (r == 0) return true;
    g_game_pid = 0;
    if (r > 0 && WIFEXITED(st)) game_ended(WEXITSTATUS(st));
    return false;
#endif
}

// the game's program in a root: the active build named in active.json (builds/<id>/dotheroes) -- M11.2b: not through
// game/, a junction on Windows whose absolute target a moved or copied install no longer has; game/ only when
// active.json names nothing usable (an install made before build slots)
static std::string game_exe(const std::string& root)
{
#ifdef _WIN32
    const char* name = "dotheroes.exe";
#else
    const char* name = "dotheroes";
#endif
    std::error_code ec;
    json::Value a;
    if (json::parse(read_text(path_str(u8path(root) / "active.json"), 1 << 16), a)) {
        const std::string id = a["active"].str();
        if (!id.empty() && id.find_first_of("/\\:") == std::string::npos && id != "." && id != "..") {
            const fs::path p = u8path(root) / "builds" / u8path(id) / name;
            if (fs::is_regular_file(p, ec)) return path_str(p);
        }
    }
    return path_str(u8path(root) / "game" / name);
}

// the program shortcuts, Steam and Return to Launcher name (M11.4): CubicHeroes.exe on Windows; on Linux the .AppImage
// file when it runs from one ($APPIMAGE -- the mounted program is gone once it ends), else CubicHeroes
static std::string self_program()
{
#ifdef _WIN32
    return path_str(u8path(g_exe_dir) / "CubicHeroes.exe");
#else
    const char* appimage = getenv("APPIMAGE");
    return appimage && *appimage ? std::string(appimage) : path_str(u8path(g_exe_dir) / "CubicHeroes");
#endif
}

static void start_game_now()
{
    if (g_nav_test) { g_nav_event = "  [the game would start: not during a scripted controller test]"; return; }
    const std::string exe = game_exe(install_dir());
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
        g_msg = { true, "The game didn't start", "Its program couldn't be started. Try Repair on the Install / Manage page.",
                  exe, true, false };
    }
#else
    // as on Windows: Return to Launcher reopens this launcher if it has closed itself (in an AppImage the .AppImage
    // file, $APPIMAGE -- the mounted program is gone once this process ends); the session's end in logs/session-end
    {
        std::error_code ec;
        fs::remove(u8path(install_dir()) / "logs" / "session-end", ec);
        setenv("CUBIC_LAUNCHER", self_program().c_str(), 1);
        setenv("CUBIC_LAUNCHER_PID", std::to_string((long)getpid()).c_str(), 1);
    }
    const pid_t pid = fork();
    if (pid == 0) { setsid(); execl(exe.c_str(), exe.c_str(), (char*)nullptr); _exit(127); }
    if (pid < 0) {
        g_msg = { true, "The game didn't start", "Its program couldn't be started. Try Repair on the Install / Manage page.",
                  exe, true, false };
        return;
    }
    g_game_pid = pid;
    g_st.playing = "The game is running.";
    if (setting("LAUNCHER_CLOSE_ON_START", "0") == "1") g_quit = true;   // the game reopens it (Return to Launcher)
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

static void updates_section();

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
        if (updates_managed_elsewhere()) text_col(col::dim, "RecompLauncher installs this game's updates.");
        else if (g_st.checking_update) text_col(col::dim, "Checking...");
        else if (!g_st.checked_once) text_col(col::dim, "Not checked yet.");
        else if (!u["ok"].truthy()) text_col(col::dim, "%s", u["message"].str("Couldn't check.").c_str());
        else if (!g_st.launcher["ready"].is_null())
            text_col(col::warm, "Version %s is downloaded and ready to install (below).",
                     g_st.launcher["ready"]["version"].str().c_str());
        else if (u["data"]["offline"].truthy()) text_col(col::dim, "Offline: the game plays as it is.");
        else if (u["data"]["available"].truthy() && u["data"]["newer"].truthy())
            text_col(col::warm, "Version %s is available (below).", u["data"]["version"].str().c_str());
        else if (u["data"]["no_package"].truthy())   // a release exists, but none of its downloads is for this PC
            text_col(col::dim, "Version %s has no download for this computer yet.", u["data"]["version"].str().c_str());
        else text_col(col::ok, "Up to date.");
        card_end();
        if (!updates_managed_elsewhere()) prompt(Act::Y, "Check for updates");
        ImGui::Dummy(ImVec2(0, S(8)));
        updates_section();   // 0.2.0-beta.5: the updates' own controls, here on Play (no page of their own)
        play_landscape();
        if (pressed(Act::Y) && !updates_managed_elsewhere()) check_updates();
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
        ImGui::Dummy(ImVec2(0, S(14)));
        updates_section();   // the launcher's own updates, whatever the game's state (their own page before)
    } else {
        title(kind == "partial" ? "An install was interrupted" : "The game isn't installed yet", 26);
        text_col(col::dim, kind == "partial" ? "Continue it on the Install / Manage page: what was done is kept."
                                             : "You'll need your own disc (an ISO or its folder) and its keys file.");
        if (big_button(kind == "partial" ? "Continue the install" : "Install the game", ImVec2(S(360), 0), Kind::Primary)) {
            g_page = kManage;
            g_focus_page = true;
        }
        focus_first();
        ImGui::Dummy(ImVec2(0, S(14)));
        updates_section();   // the launcher's own updates, whatever the game's state (their own page before)
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

static void shaders_section();
static void steam_section();
static void location_section();

// Install / Manage (0.2.0-beta.5: the Install and Shaders pages, and Settings' Steam, shortcut and folder actions, in one
// page): install or repair from the disc; then, installed, its folder, the shaders, Steam and shortcuts, and the
// maintenance -- the game's previous build, uninstalling, removing the launcher
static void page_manage()
{
    title("Install / Manage");
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
        if (!same_folder(g_wiz.folder, g_root)) {   // installed elsewhere: that folder is the root from now on
            g_root = g_wiz.folder;
            remember_root(g_root);
        }
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
                write_text(game_update_marker(), CUBIC_VERSION "\n");
                g_page = kPlay;
                g_focus_page = true;
                after_install_shaders();
            }
            refresh_info();
        });
    }
    if (!ready) text_col(col::dim, "Each step above must be checked first.");
    if (kind == "slots") {
        location_section();
        shaders_section();
        steam_section();
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    title("Maintenance", 24);
    if (kind == "slots") {
        bool previous = false;   // the one earlier game build kept after an update
        for (size_t i = 0; i < g_st.builds["builds"].size(); i++)
            if (g_st.builds["builds"][i]["state"].str() != "active") previous = true;
        // the GAME's previous build (the launcher's own previous version is on Play, with the updates)
        if (big_button("Go back to the previous game build", ImVec2(S(460), 0), Kind::Normal, previous))
            ask("Go back to the previous game build?", "The game goes back to the build you had before the last update. "
                "The launcher itself stays as it is. Your saves and settings stay as they are.", "Go back", false, [] {
                run("rollback", { "--install", install_dir() }, "Going back to the previous game build", true,
                    [](const json::Value& r) {
                    if (!r["ok"].truthy()) show_result_error(r, "The game build wasn't changed");
                    else g_msg = { true, "Previous game build", "The game build you had before is the one you play now.",
                                   "", false, false };
                    refresh_info();
                });
            });
        same_line_fits(S(360));
        if (big_button("Uninstall the game...", ImVec2(S(360), 0), Kind::Danger)) uninstall_open();
        if (!previous) text_col(col::dim, "Going back to the previous game build becomes possible after an update.");
    }
    if (big_button("Remove the launcher...", ImVec2(S(360), 0), Kind::Danger)) remove_launcher_open();
}

// --- updates ---
// beta.4: installing a downloaded update, and going back to the launcher the last one replaced. The worker checks
// the download again (signature, version, this system's package, size, SHA-256), stages it and starts its helper;
// this launcher then closes, and the helper puts the new one in place and starts it.
static void restart_into(const std::string& cmd, const std::string& version, const std::string& title)
{
    std::vector<std::string> a = launcher_update_args();
    if (cmd == "apply-update") { a.push_back("--version"); a.push_back(version); }
    a.push_back("--wait-pid");
    a.push_back(std::to_string(this_pid()));
    run(cmd, a, title, true, [](const json::Value& r) {
        if (r["ok"].truthy()) g_quit = true;   // the helper waits for this launcher to close
        else {
            show_result_error(r, "The update can't be installed");
            refresh_launcher_state();
        }
    });
}

static void launcher_update_card()
{
    const json::Value& L = g_st.launcher;
    const bool ready = !L["ready"].is_null(), back = !L["rollback"].is_null();
    const std::string last = L["last"]["result"].str();
    if (!ready && !back && last != "restored" && last != "busy") return;
    card_begin("##launcher_update");
    const bool idle = !game_running() && !g_bg && !g_op;
    if (ready) {
        const std::string v = L["ready"]["version"].str();
        title(("Version " + v + " is ready to install").c_str(), 24);
        text_col(col::dim, "Downloaded and checked against the release's signature. The launcher closes, installs it and opens "
                           "again. Your saves, settings, shader caches and builds stay as they are, and this version is kept "
                           "to go back to.");
        if (big_button("Restart and update", ImVec2(S(320), 0), Kind::Primary, idle))
            ask("Install version " + v + " now?", "The launcher closes for a few seconds while it is installed, then opens "
                "again. The game is brought up to date after that if the new version needs it.", "Restart and update", false,
                [v] { restart_into("apply-update", v, "Installing version " + v); });
        if (game_running()) text_col(col::warm, "Close the game first.");
        else if (g_bg || g_op) text_col(col::dim, "When the current task has finished.");
    }
    if (back) {
        const std::string v = L["rollback"]["version"].str();
        ImGui::Dummy(ImVec2(0, S(6)));
        text_col(col::dim, "The launcher before the last update, version %s, is kept. (This is the launcher itself; the "
                           "game's own previous build is on Install / Manage.)", v.c_str());
        if (big_button(("Go back to the previous launcher version (" + v + ")").c_str(), ImVec2(S(560), 0), Kind::Normal,
                       idle))
            ask("Go back to launcher version " + v + "?", "The launcher closes, puts version " + v + " back and opens again. "
                "This version is kept in turn, so you can come back to it. Your saves, settings and builds stay as they are.",
                "Go back", false, [v] { restart_into("rollback-launcher", v, "Going back to version " + v); });
    }
    if (last == "restored" || last == "busy") {
        ImGui::Dummy(ImVec2(0, S(6)));
        text_col(col::warm, "The last update wasn't installed: %s. Nothing was changed.",
                 L["last"]["why"].str("it didn't finish").c_str());
    }
    card_end();
}

// a button the controller or keyboard has just reached is brought near the top of the page: the cards below it are
// child windows, whose buttons the navigation can't reach while the card lies wholly outside the page's view
// (0.2.0-beta.5: the updates' cards sit low on Play). It is brought there again, keeping the focus, whenever the page
// grows below it: a check's result, or the downloaded update's card, arrives under the button just pressed -- when the
// focus came, the page couldn't scroll that far yet (at 1280x800 and below, Download it and Restart and update were
// then out of the D-pad's reach)
static void scroll_to_top_on_focus()
{
    static ImGuiID last = 0;
    static ImGuiWindow* last_w = nullptr;
    static float last_max = 0.0f;
    const ImGuiID id = ImGui::GetItemID();
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) {
        ImGuiWindow* w = ImGui::GetCurrentWindow();   // the page that scrolls, not a card around the button
        while (w && w->ScrollMax.y <= 0.0f && w->ParentWindow) w = w->ParentWindow;
        if (w && (last != id || w != last_w || w->ScrollMax.y > last_max + 0.5f))
            ImGui::SetScrollY(w, std::max(0.0f, w->Scroll.y + ImGui::GetItemRectMin().y - w->Pos.y - S(40)));
        last = id;
        last_w = w;
        last_max = w ? w->ScrollMax.y : 0.0f;
    } else if (last == id) {
        last = 0;
    }
}

// a D-pad move up or down that found nothing, on a page that can still scroll that way, scrolls the page a step (the
// focused button kept wholly in view: Dear ImGui drops the focus of a gamepad move that starts outside it); the next
// press then reaches what came into view. Dear ImGui's navigation can't reach a card's buttons while the card lies
// wholly outside the page's view (0.2.0-beta.5: at 1280x800 and below, the shortcuts under Add to Steam on Install /
// Manage). Called after NewFrame, which has applied the last frame's move (or found nothing)
static ImGuiWindow* g_page_window = nullptr;   // the page's own window (##page), as drawn in the last frame
static bool in_page(ImGuiWindow* w)
{
    for (; w; w = w->ParentWindow)
        if (w == g_page_window) return true;
    return false;
}
// a button the D-pad or keys have just reached is kept wholly inside the page's view, with a margin, for the few frames
// the move's own scroll and the layout take to settle: Dear ImGui keeps it only just inside, where the page's frame can
// cut it off and its gamepad moves start from outside the view (0.2.0-beta.5, Linux at 1280x720: the artwork row on
// the page's bottom edge, after which neither right nor left worked from it). The player's own scrolling is left alone
static void keep_focus_in_view()
{
    ImGuiContext& g = *GImGui;
    static ImGuiID last = 0;
    static int frames = 0;
    static float last_y = 0.0f;
    // the page's content above the focused button changed height (a line of text arriving, a card's result): Dear
    // ImGui's remembered height for left and right moves no longer lies on its row -- from a row's top 20 % a button
    // beside it scores as "below" and left or right finds nothing (0.2.0-beta.5, Linux at 1280x720: the artwork row,
    // where left then fell through to the side menu). Forgotten, the next move starts from the button's own middle
    if (g.NavId && g.NavWindow && g.NavId == last && fabsf(g.NavWindow->NavRectRel[ImGuiNavLayer_Main].Min.y - last_y) >= 1.0f)
        ImGui::NavClearPreferredPosForAxis(ImGuiAxis_Y);
    if (g.NavWindow) last_y = g.NavWindow->NavRectRel[ImGuiNavLayer_Main].Min.y;
    if (g.NavId != last) { last = g.NavId; frames = 3; }
    if (frames <= 0) return;
    frames--;
    if (!g.NavId || !ImGui::GetIO().NavVisible || !in_page(g.NavWindow)) return;
    const ImRect item = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[ImGuiNavLayer_Main]);
    for (ImGuiWindow* w = g.NavWindow; w; w = w == g_page_window ? nullptr : w->ParentWindow) {
        if (w->ScrollMax.y < 1.0f) continue;   // a card: the page around it scrolls
        const float m = S(16);
        float d = 0.0f;
        if (item.Max.y > w->InnerRect.Max.y - m) d = item.Max.y - (w->InnerRect.Max.y - m);
        if (item.Min.y - d < w->InnerRect.Min.y + m) d = item.Min.y - (w->InnerRect.Min.y + m);   // its top first
        if (fabsf(d) >= 1.0f) ImGui::SetScrollY(w, ImClamp(floorf(w->Scroll.y + d), 0.0f, floorf(w->ScrollMax.y)));
        break;
    }
}

static void scroll_when_stuck()
{
    ImGuiContext& g = *GImGui;
    static ImGuiID from = 0;
    static ImGuiDir dir = ImGuiDir_None;
    if (dir != ImGuiDir_None && g.NavId == from && in_page(g.NavWindow)) {
        const ImRect item = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[ImGuiNavLayer_Main]);
        // the nearest window around the focus that can still scroll that way (in whole pixels: scrolling is rounded),
        // by a third of its view at most and never so far that the focused button leaves it
        for (ImGuiWindow* w = g.NavWindow; w; w = w == g_page_window ? nullptr : w->ParentWindow) {
            const float top = floorf(w->ScrollMax.y);
            if (dir == ImGuiDir_Down ? w->Scroll.y + 1.0f > top : w->Scroll.y < 1.0f) continue;
            const float room = dir == ImGuiDir_Down ? item.Min.y - w->InnerRect.Min.y : w->InnerRect.Max.y - item.Max.y;
            const float step = std::min(floorf(w->InnerRect.GetHeight() / 3), floorf(room - S(8)));
            if (step >= 1.0f)
                ImGui::SetScrollY(w, dir == ImGuiDir_Down ? std::min(w->Scroll.y + step, top) : std::max(w->Scroll.y - step, 0.0f));
            break;
        }
    }
    dir = ImGuiDir_None;
    if (g.NavMoveSubmitted && !g.NavMoveForwardToNextFrame && (g.NavMoveDir == ImGuiDir_Up || g.NavMoveDir == ImGuiDir_Down) &&
        !(g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_IsPageMove)) && g.NavId != 0 &&
        in_page(g.NavWindow)) {
        from = g.NavId;
        dir = g.NavMoveDir;
    }
}

// the update controls take the focus (and scroll into view) when asked to (--page updates)
static void focus_updates_here()
{
    if (g_focus_updates && g_page_input) {
        ImGui::SetKeyboardFocusHere(-1);   // the item just drawn
        ImGui::SetScrollHereY(0.3f);
        g_focus_updates = false;
    }
}

// the updates, on Play (0.2.0-beta.5; their own page before): checked in the background when the launcher starts (never
// holding up Play), shown here with the download, the install on restart and the way back to the previous launcher
static void updates_section()
{
    title("Updates", 26);
    if (updates_managed_elsewhere()) {   // RecompLauncher's (0.2.0-beta.5): our own updater off, said why
        text_col(col::dim, "RecompLauncher installs this installation's updates, so this launcher's own updater is off: "
                           "the two never change the same folder. Update the game from RecompLauncher.");
        if (!started_by_recomplauncher()) {   // started outside it: the player may take the updates back
            if (big_button("Update it with this launcher instead", ImVec2(S(420), 0), Kind::Normal, !g_op && !g_bg))
                ask("Update this installation with this launcher?", "Then this launcher checks for and installs its "
                    "updates. Starting the game from RecompLauncher again hands them back to RecompLauncher, so use one "
                    "or the other. Your saves, settings and builds stay as they are.", "Update it here", false, [] {
                        std::error_code ec;
                        fs::remove(u8path(recomp_mark()), ec);
                        check_updates();
                    });
            focus_updates_here();
        }
        return;
    }
    const std::string ch = update_channel();
    text_col(col::dim, "Channel: %s. Updates are offered, never forced; the game plays offline.",
             ch == "stable" ? "stable releases" : "test releases too");
    // one id whatever its label: the controller's focus stays on it while it checks (a new id lost the focus, which
    // went to the page's first item -- on Play, PLAY: a second A started the game)
    if (big_button(g_st.checking_update ? "Checking...###check_updates" : "Check for updates###check_updates",
                   ImVec2(S(320), 0), Kind::Normal, !g_st.checking_update))
        check_updates();
    scroll_to_top_on_focus();
    focus_updates_here();
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
    } else if (u["data"]["no_package"].truthy()) {
        // the review's W-1: a newer release whose downloads are all for another system or processor is not offered
        title(("Version " + u["data"]["version"].str()).c_str(), 26);
        text_col(col::dim, "This version has no download for this computer yet. The game keeps playing as it is.");
    } else if (!u["data"]["available"].truthy()) {
        title("No release found", 24);
        if (u["data"]["refused"].size() > 0)   // a release whose signature doesn't verify: never offered, said why
            text_col(col::warm, "A release was refused, so it isn't offered: %s", u["data"]["refused"][(size_t)0]["why"].str().c_str());
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
                        g_msg = { true, "Downloaded", "Checked against the release's signature. Install it with Restart and "
                                                      "update, on this page.", r["data"]["path"].str(), false, false };
                    refresh_launcher_state();
                });
        if (d["newer"].truthy()) scroll_to_top_on_focus();
    }
    card_end();
    launcher_update_card();
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

// --- characters (M12): the local character library (tools/worker.py characters, docs/CHARACTERS.md) ---------------
// The page's parts -- the preview textures, the card, the filters, the details -- are kept apart from the library's
// own calls so that a later section (a community catalogue) can reuse them over another list.

// the previews the worker leaves beside each PNG: preview/*.rle ("RLE1", u32 width, u32 height, then runs of u16
// count + r, g, b): no PNG decoder in the launcher. Decoded only when a card is on screen, a few a frame, kept in a
// small cache (least recently used out), all freed when the page is left.
struct PreviewTex { SDL_Texture* tex = nullptr; int w = 0, h = 0; long long stamp = -1; int used = 0; bool bad = false; };
static std::map<std::string, PreviewTex> g_previews;
static SDL_Renderer* g_ren = nullptr;
static int g_preview_frame = 0, g_preview_budget = 0;
static Uint64 g_preview_t0 = 0, g_preview_spent = 0;   // this frame's decoding: when it began; 0 none yet, else the limit
static const size_t kPreviewCacheMax = 160;

static bool rle_decode(const std::string& path, int& w, int& h, std::vector<unsigned char>& px)
{
    std::ifstream f(u8path(path), std::ios::binary | std::ios::ate);   // read whole (not a byte at a time)
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 12 || size > (64 << 20)) return false;
    std::vector<unsigned char> b((size_t)size);
    f.seekg(0);
    if (!f.read((char*)b.data(), size)) return false;
    if (b.size() < 12 || memcmp(b.data(), "RLE1", 4) != 0) return false;
    uint32_t uw, uh;
    memcpy(&uw, &b[4], 4);
    memcpy(&uh, &b[8], 4);
    if (uw == 0 || uh == 0 || uw > 4096 || uh > 4096) return false;
    const size_t total = (size_t)uw * uh;
    px.resize(total * 4);   // RGBA: the renderer's own layout, uploaded without a conversion
    size_t at = 0;
    for (size_t o = 12; o + 5 <= b.size(); o += 5) {
        const size_t n = (size_t)b[o] | (size_t)b[o + 1] << 8;
        if (n == 0 || at + n > total) return false;
        const uint32_t c = (uint32_t)b[o + 2] | (uint32_t)b[o + 3] << 8 | (uint32_t)b[o + 4] << 16 | 0xff000000u;
        uint32_t* d = (uint32_t*)(px.data() + at * 4);
        for (size_t k = 0; k < n; k++) d[k] = c;
        at += n;
    }
    if (at != total) return false;
    w = (int)uw; h = (int)uh;
    return true;
}

// a preview's texture, or null (not loaded yet this frame, or unreadable: then `bad`)
static const PreviewTex* preview_tex(const std::string& path, bool* bad = nullptr)
{
    if (path.empty() || !g_ren) return nullptr;
    PreviewTex& e = g_previews[path];
    e.used = g_preview_frame;
    if (e.tex || e.bad) { if (bad) *bad = e.bad; return e.tex ? &e : nullptr; }
    // the rest next frame: at most a few decodes and ~3 ms a frame (one at least), so a page full of new pictures
    // never holds a frame up
    if (g_preview_budget <= 0 || (g_preview_spent > 0 && SDL_GetPerformanceCounter() - g_preview_t0 > g_preview_spent))
        return nullptr;
    g_preview_budget--;
    int w = 0, h = 0;
    std::vector<unsigned char> px;
    if (!rle_decode(path, w, h, px)) { e.bad = true; if (bad) *bad = true; return nullptr; }
    e.tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);   // bytes R,G,B,A (x86-64)
    if (!e.tex) { e.bad = true; return nullptr; }
    SDL_UpdateTexture(e.tex, nullptr, px.data(), w * 4);
    if (!g_preview_spent) g_preview_spent = SDL_GetPerformanceFrequency() * 3 / 1000;   // one done: ~3 ms a frame from now on
    e.w = w; e.h = h;
    return &e;
}

static void previews_trim()
{
    if (g_previews.size() <= kPreviewCacheMax) return;
    std::vector<std::pair<int, std::string>> age;
    for (auto& kv : g_previews) age.push_back({ kv.second.used, kv.first });
    std::sort(age.begin(), age.end());
    for (size_t i = 0; i + kPreviewCacheMax < age.size(); i++) {
        PreviewTex& e = g_previews[age[i].second];
        if (e.tex) SDL_DestroyTexture(e.tex);
        g_previews.erase(age[i].second);
    }
}

static void previews_release()   // the page left: nothing of it stays in memory
{
    for (auto& kv : g_previews) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    g_previews.clear();
}

// a preview drawn into a box, its aspect kept, centred; a sharp upscale, a smooth downscale
static void preview_draw(ImDrawList* dl, const std::string& path, ImVec2 a, ImVec2 b, bool* bad = nullptr)
{
    const PreviewTex* t = preview_tex(path, bad);
    dl->AddRectFilled(a, b, IM_COL32(30, 42, 98, 255));
    if (!t) return;
    const float bw = b.x - a.x, bh = b.y - a.y, s = std::min(bw / t->w, bh / t->h);
    SDL_SetTextureScaleMode(t->tex, s >= 1.0f ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
    const float w = floorf(t->w * s), h = floorf(t->h * s);
    const ImVec2 p(floorf(a.x + (bw - w) / 2), floorf(a.y + (bh - h) / 2));
    dl->AddImage((ImTextureID)(intptr_t)t->tex, p, ImVec2(p.x + w, p.y + h));
}

// text cut to a width with an ellipsis (long names never break a card)
static std::string fit_text(const std::string& s, float w)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    std::string t = s;
    while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > w) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();   // whole UTF-8 characters
    }
    return t + "...";
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static const char* source_name(const std::string& k)
{
    if (k == "ps3") return "A PS3 save";
    if (k == "chchar") return "A character file";
    if (k == "hall-of-heroes") return "The Hall of Heroes";
    if (k == "community") return "The community";
    if (k == "game") return "Made in the game";
    if (k == "store") return "The Character Store";
    if (k == "historical") return "Recovered (historical)";
    return "Your files";
}

// the page's tabs: what each shows, with its own search, filters, order, scroll and selected card
enum { kTabLibrary, kTabInstalled, kTabStore, kCharTabs };
struct CharTab {
    char search[128] = "";
    std::string f_class, f_source;    // "" any
    int f_installed = 0;              // 0 any, 1 in the game, 2 not
    int sort = 0;                     // 0 name A-Z, 1 name Z-A, 2 newest added, 3 class
    std::vector<size_t> view;         // the tab's items after the filters, in order
    std::string view_key;             // what the view was made from
    std::string focus_id;             // the card to focus (back from the details, back to the tab)
    float scroll = 0;                 // the grid's scroll
    bool restore = false;
};
struct Chars {
    json::Value list;                 // the worker's characters --char-action list
    bool asked = false, loading = false, loaded = false, previews_asked = false;
    json::Value store;                // the worker's characters --char-action store: the catalog's items and its state
    bool store_loading = false, store_loaded = false, thumbs_running = false;
    std::set<std::string> downloading;   // catalog ids being downloaded (in the background: the page stays usable)
    std::map<std::string, int> downloaded;   // done, until a list asked for after it shows them in the library
    int store_gen = 0;                   // the store lists asked for so far
    bool store_again = false;            // one more list when the one running ends (it was asked for too early)
    std::string notice;                  // the last download's outcome, under the store's toolbar for a while
    double notice_t = -1e9;
    int store_seen = -10;             // the page frame the store tab was last drawn (shown again: checked again)
    CharTab tabs[kCharTabs];
    int tab = kTabLibrary;
    std::string open_id;              // the character shown in detail
    bool info_open = false;
} g_chars;
static CharTab& ct() { return g_chars.tabs[g_chars.tab]; }
// the current tab's items: the library, or the characters the game can use now
static const json::Value& chars_items()
{
    if (g_chars.tab == kTabStore) return g_chars.store["items"];
    return g_chars.list[g_chars.tab == kTabInstalled ? "in_game" : "characters"];
}

static bool g_page_owns_back = false;           // a page's own Back (the details, a sub-view): not the menu's
static std::string g_page_back_label;
static std::vector<std::string> g_char_drops;   // files dropped on the window while this page is shown

// a worker answer's "data", moved out of it: the answer is the callback's own copy, dropped after the callback
static json::Value take_data(const json::Value& r)
{
    auto& m = const_cast<json::Value&>(r).o;
    auto it = m.find("data");
    return it == m.end() ? json::Value() : std::move(it->second);
}

static void chars_refresh()
{
    if (g_root.empty() || g_chars.loading) return;
    g_chars.loading = true;
    g_chars.asked = true;
    run("characters", { "--install", install_dir(), "--char-action", "list" }, "", false, [](const json::Value& r) {
        g_chars.loading = false;
        if (r["ok"].truthy()) {
            g_chars.list = take_data(r);
            g_chars.loaded = true;
            for (CharTab& t : g_chars.tabs) t.view_key.clear();
        }
        else show_result_error(r, "The character library couldn't be read");
        // previews from an earlier renderer (or missing): made again in the background, once
        if (!g_chars.previews_asked && g_chars.list["stale_previews"].num() > 0) {
            g_chars.previews_asked = true;
            run("characters", { "--install", install_dir(), "--char-action", "previews" }, "Updating character previews",
                true, [](const json::Value&) { previews_release(); chars_refresh(); });
        }
    });
}

// the store: its catalog checked with the repository when the tab is shown (the worker asks GitHub at most every
// 10 minutes, conditionally) or on Refresh; its thumbnails fetched in the background; a download on request
static void store_refresh(bool force)
{
    if (g_chars.store_loading) { g_chars.store_again = true; return; }
    g_chars.store_loading = true;
    const int gen = ++g_chars.store_gen;
    run("characters", { "--install", install_dir(), "--char-action", force ? "store-refresh" : "store" },
        "", false, [gen](const json::Value& r) {
            g_chars.store_loading = false;
            // a download that ended before this list was asked for is in it: its card stops saying DOWNLOADING
            for (auto it = g_chars.downloaded.begin(); it != g_chars.downloaded.end();) {
                if (it->second < gen) { g_chars.downloading.erase(it->first); it = g_chars.downloaded.erase(it); }
                else ++it;
            }
            if (g_chars.store_again) { g_chars.store_again = false; store_refresh(false); }
            if (!r["ok"].truthy()) { show_result_error(r, "The Character Store couldn't be read"); return; }
            g_chars.store = take_data(r);
            g_chars.store_loaded = true;
            g_chars.tabs[kTabStore].view_key.clear();
            if (g_chars.store["missing_thumbs"].num() > 0 && !g_chars.thumbs_running &&
                g_chars.store["status"]["status"].str() != "limited" && g_chars.store["status"]["status"].str() != "offline") {
                g_chars.thumbs_running = true;
                run("characters", { "--install", install_dir(), "--char-action", "store-thumbs" }, "", false,
                    [](const json::Value&) {
                        g_chars.thumbs_running = false;
                        previews_release();
                        store_refresh(false);
                    });
            }
        });
}

static void store_download(const json::Value& c, bool install)
{
    std::vector<std::string> args = { "--install", install_dir(), "--char-action", "store-download", "--id",
                                      c["catalog_id"].str() };
    if (install) args.push_back("--and-install");
    const std::string label = c["label"].str(), cid = c["catalog_id"].str();
    if (g_chars.downloading.count(cid)) return;
    g_chars.downloading.insert(cid);
    run("characters", args, "", false, [label, install, cid](const json::Value& r) {
        if (!r["ok"].truthy()) g_chars.downloading.erase(cid);
        else g_chars.downloaded[cid] = g_chars.store_gen;   // DOWNLOADING until a list asked for from now on shows it
        if (!r["ok"].truthy()) show_result_error(r, "\"" + label + "\" wasn't downloaded");
        else {
            const json::Value& d = r["data"];
            const bool dup = d["status"].str() == "duplicate";
            g_chars.notice = dup ? "\"" + label + "\" was already in My Library: nothing was downloaded."
                                 : "\"" + label + "\" is in My Library now.";
            if (install && !d["folder"].is_null()) g_chars.notice += " It's in the game too.";
            g_chars.notice_t = ImGui::GetTime();
        }
        chars_refresh();
        store_refresh(false);
    });
}

static const json::Value* chars_find(const std::string& id)
{
    if (id.rfind("store:", 0) == 0) {
        const json::Value& cs = g_chars.store["items"];
        for (size_t i = 0; i < cs.size(); i++)
            if (cs[i]["id"].str() == id) return &cs[i];
        return nullptr;
    }
    for (const char* k : { g_chars.tab == kTabInstalled ? "in_game" : "characters", "characters", "in_game" }) {
        const json::Value& cs = g_chars.list[k];
        for (size_t i = 0; i < cs.size(); i++)
            if (cs[i]["id"].str() == id) return &cs[i];
    }
    return nullptr;
}

static std::string chars_preview_path(const std::string& id, const char* which)
{
    return g_chars.list["root"].str() + "/entries/" + id + "/preview/" + which;
}

static bool chars_filtered() { return ct().search[0] || !ct().f_class.empty() || !ct().f_source.empty() || ct().f_installed; }

static void chars_clear_filters()
{
    ct().search[0] = 0;
    ct().f_class.clear();
    ct().f_source.clear();
    ct().f_installed = 0;
}

// the filtered, sorted view: made again only when the list, the search, a filter or the order changed
static void chars_view()
{
    const json::Value& cs = chars_items();
    const std::string key = std::to_string(g_chars.tab) + "|" + std::to_string(cs.size()) + "|" + ct().search + "|" + ct().f_class + "|" + ct().f_source +
                            "|" + std::to_string(ct().f_installed) + "|" + std::to_string(ct().sort);
    if (key == ct().view_key) return;
    ct().view_key = key;
    ct().view.clear();
    const std::string q = lower(ct().search);
    for (size_t i = 0; i < cs.size(); i++) {
        const json::Value& c = cs[i];
        if (!ct().f_class.empty() && c["class"].str() != ct().f_class) continue;
        if (!ct().f_source.empty() && c["source"].str() != ct().f_source) continue;
        if (ct().f_installed == 1 && c["installed"].is_null()) continue;
        if (ct().f_installed == 2 && !c["installed"].is_null()) continue;
        if (!q.empty()) {
            std::string hay = c["label"].str() + " " + c["name"].str() + " " + c["class"].str() + " " + c["creator"].str();
            for (size_t t = 0; t < c["tags"].size(); t++) hay += " " + c["tags"][t].str();
            if (lower(hay).find(q) == std::string::npos) continue;
        }
        ct().view.push_back(i);
    }
    std::vector<std::string> names(cs.size()), classes;
    for (size_t i : ct().view) names[i] = lower(cs[i]["label"].str());
    if (ct().sort == 3) {
        classes.resize(cs.size());
        for (size_t i : ct().view) classes[i] = lower(cs[i]["class"].str());
    }
    auto name = [&](size_t i) -> const std::string& { return names[i]; };
    std::stable_sort(ct().view.begin(), ct().view.end(), [&](size_t a, size_t b) {
        switch (ct().sort) {
        case 1: return name(a) > name(b);
        case 2: return cs[a]["imported"].str() > cs[b]["imported"].str();
        case 3: return classes[a] != classes[b] ? classes[a] < classes[b] : name(a) < name(b);
        default: return name(a) < name(b);
        }
    });
}

// ---- importing: the key, the results ------------------------------------------------------------------------------
struct KeyDialog {
    bool open = false, show = false, keep = true;
    std::vector<std::string> paths, folders;   // what is being imported: files and folders, or the game's saves
    int count = 0;                             // the encrypted characters waiting
    std::string error;                         // a wrong key's message
    char buf[80] = "";
} g_key;

struct ImportResults {
    bool open = false, details = false;
    std::string title, summary, lines;
    bool error = false;
} g_results;

struct Pick {   // the page's small choosers: the import menu, the filters, the order, the encrypted characters
    bool import_menu = false, filters = false, sort = false, review = false;
} g_pick;

static void chars_import(const std::vector<std::string>& paths, const std::vector<std::string>& folders,
                         const std::string& key = "", bool keep = false);

static void key_dialog_open(const std::vector<std::string>& paths, const std::vector<std::string>& folders, int n,
                            const std::string& error)
{
    g_key.open = true;
    g_key.show = false;
    g_key.paths = paths;
    g_key.folders = folders;
    g_key.count = n;
    g_key.error = error;
    memset(g_key.buf, 0, sizeof g_key.buf);
}

static void chars_import(const std::vector<std::string>& paths, const std::vector<std::string>& folders,
                         const std::string& key, bool keep)
{
    std::vector<std::string> a = { "--install", install_dir(), "--char-action", folders.empty() ? "import" : "import-saves" };
    for (const std::string& p : paths) { a.push_back("--paths"); a.push_back(p); }
    for (const std::string& f : folders) { a.push_back("--id"); a.push_back(f); }
    if (!key.empty()) { a.push_back("--with-key"); if (keep) a.push_back("--keep-key"); }
    const bool with_key = !key.empty();
    Op* op = run("characters", a, "Importing characters", true, [paths, folders, with_key](const json::Value& r) {
        chars_refresh();
        if (!r["ok"].truthy()) {
            if (r["code"].str() == "cancelled") { show_msg("Import stopped", "Nothing was left half-imported: each character "
                                                           "is added whole or not at all."); return; }
            show_result_error(r, "Importing didn't finish");
            return;
        }
        const json::Value& d = r["data"];
        const json::Value& n = d["counts"];
        const int imported = (int)n["imported"].num() + (int)n["similar"].num(), dup = (int)n["duplicate"].num(),
                  needs = (int)n["needs-key"].num(), wrong = (int)n["wrong-key"].num(), cancelled = (int)n["cancelled"].num();
        const int bad = (int)n["damaged"].num() + (int)n["unsupported"].num() + (int)n["failed"].num() + (int)n["errors"].num();
        // the key: asked for when it is needed (and not kept), asked again when it was wrong
        if (needs > 0 && !with_key && imported + dup + bad == 0) {
            key_dialog_open(paths, folders, needs, ""); return;
        }
        if (wrong > 0 && imported + dup + bad == 0) {
            key_dialog_open(paths, folders, wrong, with_key ? "That key didn't decrypt the character. Check the 32 digits "
                            "and try again." : "The PS3 key kept on this computer didn't decrypt this character.");
            return;
        }
        std::string s;
        auto line = [&s](int k, const char* what) { if (k) s += std::to_string(k) + " " + what + "\n"; };
        line(imported, imported == 1 ? "character imported" : "characters imported");
        line((int)n["similar"].num(), "of them the same model as one you have (another name or settings)");
        line(dup, dup == 1 ? "already in your library (not imported twice)" : "already in your library (not imported twice)");
        line(needs, "still encrypted: they need your PS3 key");
        line(wrong, "not decrypted: a wrong PS3 key (nothing was changed or kept)");
        line(bad, bad == 1 ? "couldn't be imported (see the details)" : "couldn't be imported (see the details)");
        line(cancelled, "not imported: the import was stopped");
        if (!d["key_kept"].is_null() && d["key_kept"].str().rfind("this session", 0) != 0)
            s += "Your PS3 key is kept by " + d["key_kept"].str() + ".\n";
        std::string lines;
        const json::Value& rs = d["results"];
        for (size_t i = 0; i < rs.size(); i++) {
            const std::string st = rs[i]["status"].str();
            if (st != "imported" && st != "similar")
                lines += rs[i]["label"].str() + ": " + rs[i]["message"].str() + "\n";
        }
        const json::Value& errs = d["errors"];
        for (size_t i = 0; i < errs.size(); i++) lines += errs[i].str() + "\n";
        g_results = ImportResults();
        g_results.open = true;
        g_results.title = d["stopped"].truthy() ? "Import stopped" : imported ? "Import finished" : "Nothing new imported";
        g_results.summary = s.empty() ? "No character was found in what you chose." : s;
        g_results.lines = lines;
        g_results.error = imported == 0 && bad > 0;
    });
    if (op && !key.empty()) op->pending_key = key;
}

static void chars_install(const std::string& id)
{
    run("characters", { "--install", install_dir(), "--char-action", "install", "--id", id }, "Installing the character",
        false, [](const json::Value& r) {
            if (!r["ok"].truthy()) show_result_error(r, "Installing didn't finish");
            else show_msg("Installed in the game", "Choose it in the game: CONTINUE, \"Change characters?\" YES, then EDIT "
                          "DATA -- or EDITOR, LOAD. Nothing else in your saves changed.");
            chars_refresh();
        });
}

// ---- the details ----------------------------------------------------------------------------------------------------
static void chars_detail(const json::Value& c)
{
    const std::string id = c["id"].str();
    g_page_owns_back = true;
    g_page_back_label = "Back";
    const bool store = g_chars.tab == kTabStore;
    const bool back = big_button(g_chars.tab == kTabInstalled ? "< Back to Installed" : store ? "< Back to the Store"
                                 : "< Back to My Library", ImVec2(S(320), S(44)));
    focus_first();
    if (back || (g_page_input && pressed(Act::B))) { g_chars.open_id.clear(); ct().restore = true; return; }
    const float avail = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(nullptr, S(kBody));
    title(fit_text(c["label"].str("Character"), avail).c_str(), 30);
    ImGui::PopFont();
    // class, where it is, where it came from
    const bool inst = !c["installed"].is_null();
    text_col(col::dim, "%s", c["class"].str().c_str());
    ImGui::SameLine(0, S(24));
    if (store && !inst)
        text_col(c["library_id"].truthy() ? col::ok : col::dim, "%s", c["library_id"].truthy() ? "In My Library"
                 : "Not downloaded");
    else
        text_col(inst ? col::ok : col::dim, "%s", inst ? "Installed in the game" : "Not installed in the game");
    ImGui::SameLine(0, S(24));
    text_col(col::dim, "From: %s", source_name(c["source"].str()));
    if (!c["creator"].str().empty()) { ImGui::SameLine(0, S(24)); text_col(col::dim, "Made by %s", c["creator"].str().c_str()); }
    // the views, the page's main picture: as large as the page allows, labelled
    // (room kept under them for two rows of actions, and a notice line when there is one: every action in view
    // without scrolling, on 16:9 screens too)
    const bool note = (int)c["voxels"].num() == 0 || !c["preview_ok"].truthy();
    // a store character not downloaded yet has its catalog thumbnail only: shown large instead of the three views
    const bool thumb_only = store && c["views"].str().empty();
    const float reserve = S(176) + (note ? S(kBody) + S(8) : 0.0f);
    const float label_h = S(26), max_h = std::max(S(220), ImGui::GetContentRegionAvail().y - reserve);
    float vw = avail, vh = vw * 400.0f / 972.0f;
    if (vh > max_h) { vh = max_h; vw = vh * 972.0f / 400.0f; }
    if (thumb_only) vw = vh;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float x0 = at.x + floorf((avail - vw) / 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* names[3] = { "FRONT", "SIDE", "BACK" };
    const float px = std::max(2.0f, floorf(S(3)));
    for (int i = 0; i < (thumb_only ? 0 : 3); i++) {
        const float cx = x0 + vw * (i * 2 + 1) / 6.0f;
        pix_text(dl, ImVec2(floorf(cx - pix_w(names[i], px) / 2), at.y), names[i], px, col::accent);
    }
    bool bad = false;
    preview_draw(dl, thumb_only ? c["thumb"].str() : c["views"].str(), ImVec2(x0, at.y + label_h),
                 ImVec2(x0 + vw, at.y + label_h + vh), &bad);
    retro_frame(dl, ImVec2(x0 - S(3), at.y + label_h - S(3)), ImVec2(x0 + vw + S(3), at.y + label_h + vh + S(3)), col::frame,
                std::max(2.0f, floorf(S(3))));
    ImGui::Dummy(ImVec2(avail, label_h + vh + S(10)));
    if (thumb_only && c["thumb"].str().empty()) text_col(col::dim, "Its picture hasn't been downloaded yet.");
    else if ((int)c["voxels"].num() == 0) text_col(col::dim, "This character has no blocks yet (made in the editor and saved empty).");
    else if (bad || !c["preview_ok"].truthy()) text_col(col::warm, "No preview could be drawn for this character.");
    // the actions that apply now, on as many rows as the page needs
    const float bh = S(48);
    if (store) {   // the store: download it (and install it), or it's already here
        const std::string lib_id = c["library_id"].str();
        text_col(col::dim, "%s", fit_text(c["attribution"].str(), avail).c_str());
        const bool hist = c["category"].str() == "historical";
        if (hist) text_col(col::dim, "%s", fit_text(c["rights"].str(), avail).c_str());
        if (g_chars.downloading.count(c["catalog_id"].str())) {
            text_col(col::accent, "Downloading... (you can keep browsing)");
        } else if (lib_id.empty()) {
            const bool upd = c["older_in_library"].truthy();
            if (big_button(upd ? "Download the new version" : "Download", ImVec2(S(upd ? 360 : 240), bh), Kind::Primary))
                store_download(c, false);
            ImGui::SameLine(0, S(16));
            if (big_button("Download and install", ImVec2(S(320), bh))) store_download(c, true);
            if (upd) text_col(col::dim, "An earlier version is in My Library; the new one is added beside it.");
        } else if (!inst) {
            if (big_button("Install in the game", ImVec2(S(280), bh), Kind::Primary)) chars_install(lib_id);
            ImGui::SameLine(0, S(16));
            text_col(col::dim, "Already in My Library.");
        } else {
            text_col(col::ok, "In My Library and in the game.");
        }
        if (big_button(g_chars.info_open ? "Less information" : "More information", ImVec2(S(300), bh)))
            g_chars.info_open = !g_chars.info_open;
        if (g_chars.info_open) {
            ImGui::PushFont(nullptr, S(kSmall));
            if (!c["creator"].str().empty()) text_col(col::dim, "Made by %s%s%s", c["creator"].str().c_str(),
                                                      c["creator_url"].str().empty() ? "" : " -- ",
                                                      c["creator_url"].str().c_str());
            if (hist) {   // a recovered character: where it was first shared, where it was found, how to ask for removal
                text_col(col::dim, "First shared on %s%s%s", c["collection"].str().c_str(),
                         c["original_date"].str().empty() ? "" : ", ", c["original_date"].str().c_str());
                if (!c["archived_url"].str().empty())
                    ImGui::TextWrapped("Archived source: %s", c["archived_url"].str().c_str());
                text_col(col::dim, "Licence: none claimed -- all rights remain with its creator.");
                if (!g_chars.store["removal_url"].str().empty())
                    ImGui::TextWrapped("Is this your character, or yours to decide? Ask for its removal: %s",
                                       g_chars.store["removal_url"].str().c_str());
            } else {
                text_col(col::dim, "Licence: %s (https://creativecommons.org/licenses/by/4.0/)",
                         c["license"].str() == "CC-BY-4.0" ? "CC BY 4.0" : c["license"].str("not stated").c_str());
            }
            text_col(col::dim, "Version %d; added %s, updated %s; file %d bytes; catalog id %s", (int)c["version"].num(),
                     c["added"].str().substr(0, 10).c_str(), c["imported"].str().substr(0, 10).c_str(),
                     (int)c["size"].num(), c["catalog_id"].str().c_str());
            if (!c["description"].str().empty()) ImGui::TextWrapped("%s", c["description"].str().c_str());
            text_col(col::dim, "From the community characters repository%s: its signed catalog (version %d), and this "
                     "file checked against the SHA-256 the catalog gives.", g_chars.store["official"].truthy() ? ""
                     : " (a development copy)", (int)g_chars.store["catalog_version"].num());
            ImGui::PopFont();
        }
        return;
    }
    if (!c["in_library"].truthy()) {   // made in the game: it can be added to the library (then exported, renamed)
        if (big_button("Add to my library", ImVec2(S(300), bh), Kind::Primary)) {
            chars_import({}, { c["folder"].str() });
            g_chars.open_id.clear();
            ct().restore = true;
        }
        text_col(col::dim, "Made in the game. Add it to your library to keep a copy, export or rename it.");
        return;
    }
    const float right_edge = ImGui::GetCurrentWindow()->WorkRect.Max.x;
    auto flow = [right_edge](float w) {   // the next button beside the last when it fits, else below it
        if (ImGui::GetItemRectMax().x + S(16) + w <= right_edge) ImGui::SameLine(0, S(16));
    };
    if (!inst) {
        if (big_button("Install in the game", ImVec2(S(280), bh), Kind::Primary)) chars_install(id);
    } else if (c["installed_by_library"].truthy()) {
        if (big_button("Uninstall from the game", ImVec2(S(300), bh)))
            ask("Uninstall from the game?", "\"" + c["label"].str() + "\" is taken out of the game's characters (its folder "
                "in the game's saves, which the library made). It stays in your library: you can install it again. "
                "Nothing else in your saves changes.", "Uninstall", true, [id] {
                    run("characters", { "--install", install_dir(), "--char-action", "uninstall", "--id", id, "--confirm" },
                        "Uninstalling", false, [](const json::Value& r) {
                            if (!r["ok"].truthy()) show_result_error(r, "Not uninstalled");
                            chars_refresh();
                        });
                });
    } else {
        text_col(col::dim, "In the game's saves (made there, so the game manages it).");
    }
    flow(S(200));
    if (big_button("Export...", ImVec2(S(200), bh)))
        browse("Export to which folder?", true, {}, "", "", [id](const std::string& dir) {
            run("characters", { "--install", install_dir(), "--char-action", "export", "--id", id, "--out", dir },
                "Exporting", false, [](const json::Value& r) {
                    if (!r["ok"].truthy()) { show_result_error(r, "Exporting didn't finish"); return; }
                    const json::Value& d = r["data"];
                    if (d["written"].size() > 0)
                        g_msg = { true, "Exported", "Saved as " + d["written"][(size_t)0].str() + "\n\nIt holds the character "
                                  "and its previews -- nothing of the PS3 save.", "", false, false };
                    else
                        g_msg = { true, "Not exported", d["skipped"][(size_t)0]["path"].str() + " already exists: nothing was "
                                  "replaced. Choose another folder, or rename or move that file.", "", true, false };
                });
        });
    flow(S(200));
    if (big_button("Rename...", ImVec2(S(200), bh)))
        type_text("A name for it in your library", c["label"].str(), [id](const std::string& name) {
            if (name.empty()) return;
            run("characters", { "--install", install_dir(), "--char-action", "rename", "--id", id, "--label", name },
                "", false, [](const json::Value& r) {
                    if (!r["ok"].truthy()) show_result_error(r, "Not renamed");
                    chars_refresh();
                });
        });
    flow(S(280));
    if (big_button("Remove from library", ImVec2(S(280), bh), Kind::Danger))
        ask("Remove it from your library?", "\"" + c["label"].str() + "\" is removed from your library on this computer, with "
            "the copy of the files it was imported from." + std::string(inst ? " The copy installed in the game stays: "
            "uninstall it first to take it out of the game too." : "") + " You can import it again from its file.",
            "Remove", true, [id] {
                run("characters", { "--install", install_dir(), "--char-action", "delete", "--id", id, "--confirm" },
                    "", false, [](const json::Value& r) {
                        if (!r["ok"].truthy()) show_result_error(r, "Not removed");
                        g_chars.open_id.clear();
                        chars_refresh();
                    });
            });
    // the rest, on request
    flow(S(300));
    if (big_button(g_chars.info_open ? "Less information" : "More information", ImVec2(S(300), bh)))
        g_chars.info_open = !g_chars.info_open;
    if (g_chars.info_open) {
        ImGui::PushFont(nullptr, S(kSmall));
        text_col(col::dim, "%d blocks in %d poses (Stand, Walk 1, Walk 2, Hurray, Attack 1, Attack 2)", (int)c["voxels"].num(),
                 (int)c["poses"].num());
        text_col(col::dim, "Game: 3D Dot Game Heroes (US, BLUS-30490) -- the character editor's data (EDTFILE1.9)");
        text_col(col::dim, "%d bytes; added %s; id %s", (int)c["size"].num(), c["imported"].str().substr(0, 10).c_str(),
                 id.c_str());
        if (!c["description"].str().empty()) ImGui::TextWrapped("%s", c["description"].str().c_str());
        if (inst) text_col(col::dim, "Its folder in the game's saves: %s", c["installed"].str().c_str());
        ImGui::PopFont();
    }
}

// ---- the library: the toolbar, the notice, the grid ---------------------------------------------------------------
static void chars_card(const json::Value& c, ImVec2 size, bool focus_me)
{
    const std::string id = c["id"].str();
    ImGui::PushID(id.c_str());
    if (focus_me) ImGui::SetKeyboardFocusHere();
    const bool hit = ImGui::InvisibleButton("##card", size, ImGuiButtonFlags_EnableNav);   // reached by the D-pad
    const bool focused = ImGui::IsItemFocused(), hovered = ImGui::IsItemHovered();
    if (focused) { g_focus_label = "card: " + c["label"].str(); ct().focus_id = id; }
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 face = hovered || focused ? shade(col::panel2, 1.25f) : col::panel2;
    dl->AddRectFilled(a, b, face);
    bevel_edges(dl, a, b, face, std::max(2.0f, floorf(S(3))));
    const float pad = floorf(S(10)), thumb = size.x - 2 * pad;
    bool bad = false;
    preview_draw(dl, c["thumb"].str(), ImVec2(a.x + pad, a.y + pad), ImVec2(a.x + pad + thumb, a.y + pad + thumb), &bad);
    const bool store = g_chars.tab == kTabStore;
    const bool game_only = !store && !c["in_library"].truthy();
    const bool busy = store && g_chars.downloading.count(c["catalog_id"].str());
    const bool update = store && !c["library_id"].truthy() && c["older_in_library"].truthy();
    const bool store_have = store && (!c["installed"].is_null() || c["library_id"].truthy() || busy || update);
    if ((g_chars.tab == kTabLibrary && !c["installed"].is_null()) || game_only || store_have) {   // a badge on the picture
        const char* t = game_only ? "GAME ONLY" : busy ? "DOWNLOADING" : update ? "UPDATE"
                        : store && c["installed"].is_null() ? "IN LIBRARY" : "IN GAME";
        const float p = std::max(2.0f, floorf(S(2)));
        const float tw = pix_w(t, p);
        const ImVec2 ba(a.x + pad + thumb - tw - p * 6, a.y + pad + p * 2), bb(a.x + pad + thumb - p * 2, a.y + pad + p * 13);
        dl->AddRectFilled(ba, bb, game_only || (store && c["installed"].is_null()) ? IM_COL32(40, 70, 150, 235)
                                                                                    : IM_COL32(40, 110, 40, 235));
        pix_text(dl, ImVec2(ba.x + p * 2, ba.y + p * 2), t, p, IM_COL32_WHITE);
    }
    ImGui::PushFont(nullptr, S(kBody));
    const std::string name = fit_text(c["label"].str("Character"), size.x - 2 * pad);
    dl->AddText(ImVec2(a.x + pad, a.y + pad + thumb + S(6)), col::text, name.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, S(kSmall));
    const std::string cls = fit_text(c["class"].str(), size.x - 2 * pad);
    dl->AddText(ImVec2(a.x + pad, a.y + pad + thumb + S(6) + S(kBody) + S(2)), col::dim, cls.c_str());
    ImGui::PopFont();
    if (hovered && !focused) retro_frame(dl, a, b, shade(col::frame, 0.9f), std::max(2.0f, floorf(S(2))));
    focus_ring();
    if (hit) { g_chars.open_id = id; g_chars.info_open = false; ct().focus_id = id; }
    ImGui::PopID();
}

// an empty tab or view: its picture, what it means, and the one thing to do next (which takes the focus)
enum class EmptyAction { None, Import, ClearFilters, ShowLibrary, Retry };
static void store_refresh(bool force);
static void chars_empty(const char* head, const char* text, EmptyAction act)
{
    ImGui::Dummy(ImVec2(0, S(30)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float p = std::max(4.0f, floorf(S(9)));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    pix_icon(dl, ImVec2(floorf(at.x + (w - p * 10) / 2), at.y), kIcons[10], p);
    ImGui::Dummy(ImVec2(w, p * 11));
    title(head, 26);
    ImGui::TextWrapped("%s", text);
    ImGui::Spacing();
    if (act == EmptyAction::ClearFilters) {
        if (big_button("Clear the search and filters", ImVec2(S(360), S(48)))) chars_clear_filters();
    } else if (act == EmptyAction::Import) {
        if (big_button("Import characters...", ImVec2(S(320), S(48)), Kind::Primary)) g_pick.import_menu = true;
    } else if (act == EmptyAction::Retry) {
        if (big_button("Refresh", ImVec2(S(240), S(48)), Kind::Primary) && !g_chars.store_loading) store_refresh(true);
    } else if (act == EmptyAction::ShowLibrary) {
        if (big_button("Show My Library", ImVec2(S(320), S(48)), Kind::Primary)) {
            g_chars.tab = kTabLibrary;
            ct().restore = true;
            g_focus_page = true;
        }
    }
    if (act != EmptyAction::None && g_chars.loaded) focus_first();
}

// the tabs under the heading: compact, the count beside each; LT/RT (or [ ]) switch them, LB/RB stay the pages'
static void chars_tabs()
{
    const int nlib = (int)g_chars.list["characters"].size(), ngame = (int)g_chars.list["in_game"].size();
    std::vector<std::pair<int, std::string>> tabs = { { kTabLibrary, "My Library (" + std::to_string(nlib) + ")" },
                                                      { kTabInstalled, "Installed (" + std::to_string(ngame) + ")" } };
    tabs.push_back({ kTabStore, "Character Store" });   // the read-only store: the characters repository's catalog
    const float h = S(40), gap = S(10);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < tabs.size(); i++) {
        if (i) ImGui::SameLine(0, gap);
        const bool on = g_chars.tab == tabs[i].first;
        const float w = ImGui::CalcTextSize(tabs[i].second.c_str()).x + S(44);
        if (big_button((tabs[i].second + "##tab" + std::to_string(i)).c_str(), ImVec2(w, h), on ? Kind::Primary : Kind::Normal)
            && !on) { g_chars.tab = tabs[i].first; ct().restore = true; g_focus_page = true; }
        if (on) {   // the chosen tab: a gold bar under it, joined to the page
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddRectFilled(ImVec2(a.x, b.y + S(3)), ImVec2(b.x, b.y + S(6)), col::accent);
        }
    }
    if (g_page_input && !tabs.empty()) {
        int at = 0;
        for (size_t i = 0; i < tabs.size(); i++) if (tabs[i].first == g_chars.tab) at = (int)i;
        if (pressed(Act::LT)) { g_chars.tab = tabs[(at + tabs.size() - 1) % tabs.size()].first; ct().restore = true; g_focus_page = true; }
        if (pressed(Act::RT)) { g_chars.tab = tabs[(at + 1) % tabs.size()].first; ct().restore = true; g_focus_page = true; }
        if (tabs.size() > 1) { prompt(Act::LT, ""); prompt(Act::RT, "Tabs"); }
    }
    // how many the tab shows, on the tabs' row (the header stays short)
    const size_t total = chars_items().size(), shown = ct().view.size();
    char count[96];
    if (g_chars.tab == kTabStore) count[0] = 0;
    else if (chars_filtered()) snprintf(count, sizeof count, "%zu of %zu shown", shown, total);
    else snprintf(count, sizeof count, total == 1 ? "%zu character" : "%zu characters", total);
    const float cw = ImGui::CalcTextSize(count).x;
    ImGui::SameLine();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(std::max(cur.x + S(16), ImGui::GetCurrentWindow()->WorkRect.Max.x - cw),
                                     cur.y + floorf((h - ImGui::GetFontSize()) / 2)));
    text_col(col::dim, "%s", count);
    ImGui::Dummy(ImVec2(0, S(4)));
}

// the store's state under its toolbar: when the catalog was checked, or why the saved one is shown
static void store_status_line(float avail)
{
    const json::Value& st = g_chars.store["status"];
    const std::string s = st["status"].str(), msg = st["message"].str();
    const bool warn = s == "offline" || s == "limited" || s == "error";
    std::string text = msg;
    if (!g_chars.notice.empty() && ImGui::GetTime() - g_chars.notice_t < 8.0) text = g_chars.notice;
    else if (!g_chars.downloading.empty() && text.empty())
        text = "Downloading " + std::to_string(g_chars.downloading.size()) + " character" +
               (g_chars.downloading.size() == 1 ? "" : "s") + "...";
    if (text.empty()) {
        const int nh = (int)g_chars.store["historical"].num(), nall = (int)g_chars.store["items"].size();
        text = nh == 0 ? "Community characters, checked and approved"
             : std::to_string(nall - nh) + " community and " + std::to_string(nh) + " recovered historical characters";
        if (!g_chars.store["updated"].str().empty()) text += " -- catalog of " + g_chars.store["updated"].str();
        if (g_chars.thumbs_running) text += " (fetching pictures)";
    }
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float h = S(40);
    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + avail, a.y + h), warn ? IM_COL32(120, 70, 20, 200)
                                                                                    : IM_COL32(30, 44, 100, 200));
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(14), a.y + floorf((h - ImGui::GetFontSize()) / 2)));
    text_col(col::text, "%s", fit_text(text, avail - S(28)).c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + S(8)));
    ImGui::Dummy(ImVec2(1, 1));
}

static void page_characters()
{
    g_preview_frame++;
    g_preview_budget = 8;
    g_preview_t0 = SDL_GetPerformanceCounter();
    g_preview_spent = 0;
    if (!g_chars.asked) chars_refresh();
    if (!g_char_drops.empty()) { chars_import(g_char_drops, {}); g_char_drops.clear(); g_chars.tab = kTabLibrary; }
    if (!g_chars.open_id.empty()) {
        if (const json::Value* c = chars_find(g_chars.open_id)) { chars_detail(*c); previews_trim(); return; }
        g_chars.open_id.clear();
    }
    title("Characters");
    {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        chars_view();
        g_nav_view_max = std::max(g_nav_view_max, (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency());
    }
    chars_tabs();
    const bool store_tab = g_chars.tab == kTabStore;
    if (store_tab) {   // shown (again): the catalog checked -- the worker asks GitHub only when it's due
        if (g_chars.store_seen + 1 != g_preview_frame) store_refresh(false);
        g_chars.store_seen = g_preview_frame;
    }
    const json::Value& cs = chars_items();
    const bool lib_tab = g_chars.tab == kTabLibrary;
    // the toolbar: the search, the filters, the order -- and the tab's own action
    int addable = 0;
    if (g_chars.tab == kTabInstalled) for (size_t i = 0; i < cs.size(); i++) addable += cs[i]["in_library"].truthy() ? 0 : 1;
    const bool action = lib_tab || store_tab || addable > 0;
    const float avail = ImGui::GetContentRegionAvail().x, gap = S(12), bh = S(48);
    const std::string add_label = "Add " + std::to_string(addable) + " to library";
    const float bw = floorf(avail * 0.17f);
    const float aw = !action ? 0.0f : lib_tab || store_tab ? bw : std::max(bw, ImGui::CalcTextSize(add_label.c_str()).x + S(48));
    ImGui::SetNextItemWidth(avail - 2 * (bw + gap) - (action ? aw + gap : 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), floorf((bh - ImGui::GetFontSize()) / 2)));
    if (ImGui::InputTextWithHint("##search", "Search: name, class, creator", ct().search, sizeof ct().search))
        ct().view_key.clear();
    ImGui::PopStyleVar();
    if (ImGui::IsItemFocused()) g_focus_label = "search";
    if (ImGui::IsItemFocused() && g_device == Device::Pad && pressed(Act::A))   // a controller: the on-screen keyboard
        type_text("Search: a name, class, creator or tag", ct().search, [](const std::string& s) {
            snprintf(ct().search, sizeof ct().search, "%s", s.c_str());
            ct().view_key.clear();
        });
    ImGui::SameLine(0, gap);
    const int nfilt = (!ct().f_class.empty()) + (!ct().f_source.empty()) + (ct().f_installed != 0);
    const std::string fl = nfilt ? "Filters (" + std::to_string(nfilt) + ")" : std::string("Filters");
    if (big_button(fl.c_str(), ImVec2(bw, bh), nfilt ? Kind::Primary : Kind::Normal)) g_pick.filters = true;
    ImGui::SameLine(0, gap);
    const char* sorts[] = { "Order: A-Z", "Order: Z-A", "Order: Newest", "Order: Class" };
    if (big_button(sorts[ct().sort], ImVec2(bw, bh))) g_pick.sort = true;
    if (lib_tab) {
        ImGui::SameLine(0, gap);
        if (big_button("Import...", ImVec2(aw, bh), Kind::Primary)) g_pick.import_menu = true;
    } else if (store_tab) {
        ImGui::SameLine(0, gap);
        if (big_button(g_chars.store_loading ? "Checking..." : "Refresh", ImVec2(aw, bh)) && !g_chars.store_loading)
            store_refresh(true);
    } else if (addable > 0) {
        ImGui::SameLine(0, gap);
        if (big_button(add_label.c_str(), ImVec2(aw, bh), Kind::Primary)) {
            std::vector<std::string> fs;
            for (size_t i = 0; i < cs.size(); i++) if (!cs[i]["in_library"].truthy()) fs.push_back(cs[i]["folder"].str());
            chars_import({}, fs);
        }
    }
    if (chars_filtered()) {
        if (ImGui::SmallButton("Clear the search and filters")) chars_clear_filters();
    }
    // a compact notice: encrypted characters in the game's saves (the store's tab: the catalog's state)
    if (store_tab && g_chars.store_loaded) store_status_line(avail);
    int enc = 0;
    const json::Value& sv = g_chars.list["in_saves"];
    for (size_t i = 0; i < sv.size(); i++) enc += sv[i]["kind"].str() == "encrypted" ? 1 : 0;
    if (enc > 0 && !store_tab) {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float h = S(50);
        ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + avail, a.y + h), IM_COL32(120, 70, 20, 200));
        ImGui::SetCursorScreenPos(ImVec2(a.x + S(14), a.y + floorf((h - ImGui::GetFontSize()) / 2)));
        text_col(col::text, enc == 1 ? "1 encrypted PS3 character detected in the game's saves."
                                     : "%d encrypted PS3 characters detected in the game's saves.", enc);
        ImGui::SetCursorScreenPos(ImVec2(a.x + avail - S(300), a.y + S(4)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(4)));
        const bool review = big_button("Review and import", ImVec2(S(292), h - S(8)));
        ImGui::PopStyleVar();
        if (review) g_pick.review = true;
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + S(8)));
        ImGui::Dummy(ImVec2(1, 1));
    }
    // the grid
    const size_t total = cs.size(), shown = ct().view.size();
    if (total == 0 && store_tab) {
        if (!g_chars.store_loaded) chars_empty("Reading the catalog...", "", EmptyAction::None);
        else if (!g_chars.store["have_catalog"].truthy())
            chars_empty("The Character Store couldn't be reached", "No catalog has been saved on this computer yet. "
                        "Check the connection, then refresh.", EmptyAction::Retry);
        else
            chars_empty("No characters in the store yet", "Approved community characters will appear here.",
                        EmptyAction::Retry);
        return;
    }
    if (total == 0) {
        if (!g_chars.loaded) chars_empty("Reading your library...", "", EmptyAction::None);
        else if (lib_tab)
            chars_empty("Your character library is empty", "Import the characters you made in the game, character files, "
                        "or characters saved on a PS3. You can also drop files on this window.", EmptyAction::Import);
        else
            chars_empty("No characters in the game yet", "Install characters from My Library, or make one in the game's "
                        "editor.", EmptyAction::ShowLibrary);
        return;
    }
    if (shown == 0) { chars_empty("No characters match", "Nothing here matches the search and filters.", EmptyAction::ClearFilters); return; }
    const float card_min = S(180), cgap = S(16);
    const float gw = ImGui::GetContentRegionAvail().x - S(14);   // room for the scroll bar
    const int cols = std::max(2, std::min(6, (int)((gw + cgap) / (card_min + cgap))));
    const float cw = floorf((gw - cgap * (cols - 1)) / cols), ch = cw + S(kBody) + S(kSmall) + S(14);
    const int rows = (int)((shown + cols - 1) / cols);
    ImGui::BeginChild(g_chars.tab == kTabLibrary ? "##grid_library" : store_tab ? "##grid_store" : "##grid_installed", ImVec2(0, 0),
                      ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    int focus_row = -1;
    if (ct().restore) {
        for (size_t k = 0; k < shown; k++) if (cs[ct().view[k]]["id"].str() == ct().focus_id) focus_row = (int)(k / cols);
        ImGui::SetScrollY(ct().scroll);
    }
    ImGuiListClipper clip;
    clip.Begin(rows, ch + cgap);
    if (focus_row >= 0) clip.IncludeItemByIndex(focus_row);
    const ImVec2 o = ImGui::GetCursorPos();
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            for (int k = 0; k < cols; k++) {
                const size_t vi = (size_t)r * cols + k;
                if (vi >= shown) break;
                ImGui::SetCursorPos(ImVec2(o.x + k * (cw + cgap), o.y + r * (ch + cgap)));
                const json::Value& c = cs[ct().view[vi]];
                chars_card(c, ImVec2(cw, ch), ct().restore && c["id"].str() == ct().focus_id);
                if (vi == 0 && g_chars.loaded) focus_first();
            }
        }
    }
    clip.End();
    ImGui::SetCursorPos(ImVec2(o.x, o.y + rows * (ch + cgap)));
    ImGui::Dummy(ImVec2(1, 1));   // the grid's extent, as an item
    if (!ct().restore) ct().scroll = ImGui::GetScrollY();
    ct().restore = false;
    ImGui::EndChild();
    previews_trim();
    if (g_page_input) {
        prompt(Act::X, "Filters");
        prompt(Act::Y, "Search");
        if (pressed(Act::X)) g_pick.filters = true;
        if (pressed(Act::Y))
            type_text("Search: a name, class, creator or tag", ct().search, [](const std::string& s) {
                snprintf(ct().search, sizeof ct().search, "%s", s.c_str());
                ct().view_key.clear();
            });
    }
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

// Install / Manage's shaders (0.2.0-beta.5; their own page before)
static void shaders_section()
{
    ImGui::Dummy(ImVec2(0, S(10)));
    title("Shaders and pipelines", 24);
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
    shader_actions(false);   // the page opens on its install step, not here
}

// Install / Manage's installation folder (0.2.0-beta.5: from Settings)
static void location_section()
{
    ImGui::Dummy(ImVec2(0, S(10)));
    title("Installation", 24);
    text_col(col::text, "Installed in %s", install_dir().c_str());
    text_col(col::dim, "Another folder: the launcher uses the installation there, or installs into it. Nothing is moved.");
    if (big_button("Change the installation folder", ImVec2(S(460), 0)))
        browse("Choose the install folder", true, {}, "folders", install_dir(), [](const std::string& p) {
            g_root = p;   // that folder is the root from now on (cubicheroes-root.txt beside the launcher)
            if (!remember_root(p))
                g_msg = { true, "Not remembered", "This folder is used now, but the launcher's own folder can't be written, so "
                          "it isn't remembered for next time. Start the launcher with --install-dir to use it.", p, false, false };
            g_prefs.save();
            g_wiz.folder.clear();
            refresh_info();
        });
}

// --- Steam and shortcuts (Install / Manage; 0.2.0-beta.5: from Settings) ---
// The library artwork is the player's own choice: they download the images themselves from the artists' pages on
// SteamGridDB (nothing is downloaded by Cubic.Heroes: the site's terms rule out automated use), and the launcher only
// places the files they choose (the worker's steam-art: PNG or JPEG, never over artwork of their own unless they say)
static const char* kArtPoster = "https://www.steamgriddb.com/grid/54932";
static const char* kArtHero = "https://www.steamgriddb.com/hero/14114";

static void open_link(const char* url)
{
    if (g_nav_test) { g_nav_event = std::string("  [would open ") + url + ": not during a scripted controller test]"; return; }
    if (SDL_OpenURL(url) != 0)
        g_msg = { true, "The browser didn't open", "Open this page yourself:", url, false, false };
}

static void steam_art_place(const std::string& kind, const std::string& path, bool replace)
{
    std::vector<std::string> a = { "--exe", self_program(), "--kind", kind, "--image", path };
    if (replace) a.push_back("--replace");
    run("steam-art", a, "", false, [kind, path, replace](const json::Value& r) {
        if (!r["ok"].truthy()) { show_result_error(r, "The artwork wasn't placed"); return; }
        if (r["data"]["kept"].size() > 0 && !replace)
            ask("Replace your own artwork?", "Steam already has a " + std::string(kind == "hero" ? "hero" : "poster") +
                " image for Cubic.Heroes that the launcher didn't put there. Replace it with the one you chose?",
                "Replace", true, [kind, path] { steam_art_place(kind, path, true); });
        else
            g_msg = { true, "Artwork placed", "Steam shows it the next time it starts.", "", false, false };
    });
}

static void steam_art_choose(const char* kind)
{
    const bool hero = !strcmp(kind, "hero");
    browse(hero ? "Choose the hero image" : "Choose the poster image", false, { ".png", ".jpg", ".jpeg" },
           "images (.png, .jpg)", "", [kind](const std::string& p) { steam_art_place(kind, p, false); });
}

static void steam_section()
{
    ImGui::Dummy(ImVec2(0, S(10)));
    title("Steam and shortcuts", 24);
    if (big_button("Add to Steam", ImVec2(S(260), 0)))
        ask("Add the launcher to Steam?", "It appears in your Steam library (as a non-Steam game) and opens this launcher, "
            "with its library artwork -- the poster by Fami-Kon and the hero image by Jib, downloaded from SteamGridDB onto "
            "this computer, and the Cubic.Heroes logo. Close Steam first: it rewrites its library when it exits.", "Add it",
            false, [] {
            run("shortcuts", { "--exe", self_program(), "--steam" }, "Adding it to Steam", true,
                [](const json::Value& r) {
                    if (!r["ok"].truthy()) { show_result_error(r, "It wasn't added to Steam"); return; }
                    const json::Value& art = r["data"]["artwork"];
                    std::string text = "Start Steam to see it in your library.";
                    if (art["failed"].size() > 0)   // the game is added all the same: the artwork is the player's to add
                        text += std::string(art["placed"].size() > 0 ? " Some of its artwork" : " The artwork") +
                                " wasn't added: " + art["failed"].o.begin()->second.str() + ". You can choose the "
                                "images yourself under Steam artwork.";
                    else if (art["kept"].size() > 0)
                        text += " Artwork you had put there yourself was kept.";
                    else if (art["placed"].size() > 0)
                        text += " Its library artwork is the poster by Fami-Kon and the hero image by Jib (SteamGridDB), "
                                "with the Cubic.Heroes logo.";
                    g_msg = { true, "Added to Steam", text, "", false, false };
                });
        });
    same_line_fits(S(300));
    if (big_button("Remove from Steam", ImVec2(S(300), 0)))
        ask("Remove the launcher from Steam?", "Its entry leaves your Steam library; nothing else changes. Close Steam first: "
            "it rewrites its library when it exits.", "Remove", false, [] {
            run("shortcuts", { "--exe", self_program(), "--remove-steam" }, "", false, [](const json::Value& r) {
                if (!r["ok"].truthy()) show_result_error(r, "It wasn't removed from Steam");
                else if (r["data"]["later"].truthy()) g_msg = { true, "Close Steam first", "Steam is running: close it, then "
                                                               "remove the entry again.", "", false, false };
                else g_msg = { true, "Removed from Steam", "The launcher's entry is gone from your Steam library.", "", false, false };
            });
        });
#ifdef _WIN32
    const char* menu = "Start-menu shortcut", *menu_done = "The launcher is in your Start menu.";
#else
    const char* menu = "App-menu shortcut", *menu_done = "The launcher is in your applications menu.";
#endif
    if (big_button(menu, ImVec2(S(300), 0)))
        run("shortcuts", { "--exe", self_program() }, "", false, [menu_done](const json::Value& r) {
            if (r["ok"].truthy()) g_msg = { true, "Shortcut made", menu_done, "", false, false };
            else show_result_error(r, "The shortcut wasn't made");
        });
    same_line_fits(S(300));
    if (big_button("Desktop shortcut", ImVec2(S(300), 0)))
        run("shortcuts", { "--exe", self_program(), "--desktop" }, "", false, [](const json::Value& r) {
            if (r["ok"].truthy()) g_msg = { true, "Shortcuts made", "The launcher is on your desktop and in your menu.", "", false, false };
            else show_result_error(r, "The shortcut wasn't made");
        });
    // (no card around it: a card's buttons can't be reached by the controller while it lies below the page's view)
    ImGui::Dummy(ImVec2(0, S(6)));
    title("Steam artwork (optional)", 22);
    text_col(col::dim, "Add to Steam downloads the library artwork onto this computer: the poster by Fami-Kon and the hero "
                       "image by Jib, made by their artists and hosted on SteamGridDB (never part of Cubic.Heroes itself), "
                       "and the Cubic.Heroes logo, which comes with the launcher. "
                       "If that didn't work, or for other images, download them yourself and choose the files here. Artwork "
                       "you put there yourself stays unless you choose to replace it.");
    if (big_button("Open the artwork pages", ImVec2(S(340), 0))) { open_link(kArtPoster); open_link(kArtHero); }
    same_line_fits(S(340));
    if (big_button("Choose the poster image...", ImVec2(S(340), 0))) steam_art_choose("poster");
    same_line_fits(S(340));
    if (big_button("Choose the hero image...", ImVec2(S(340), 0))) steam_art_choose("hero");
}

// --- settings ---
// the next item beside the last one only when it fits there (else it starts a line): no sideways overflow
static void same_line_fits(float next_w)
{
    const float limit = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + next_w <= limit) ImGui::SameLine();
}

// 0.2.0-beta.5: the Settings page in tabs -- only the chosen tab's settings drawn (each setting saved the moment it
// changes, as before: nothing is held back when the tab changes)
static int g_settings_tab = 0;
static const char* kSettingsTabs[] = { "Graphics", "Performance", "Audio", "Controller", "Launcher" };

static int settings_tab_of(const std::string& key, const std::string& cat)
{
    if (key == "DOTHEROES_FPS" || key == "DOTHEROES_FPS_CUSTOM" || key == "DOTHEROES_PERF_OVERLAY" || cat == "Shaders" ||
        cat == "Performance") return 1;
    if (cat == "Display" || cat == "Game") return 0;
    if (cat == "Audio") return 2;
    if (cat == "Controls") return 3;
    return 4;   // Launcher, Updates, Saves, Diagnostics -- and any category added later: never hidden
}

static std::string settings_section_of(const std::string& key, const std::string& cat)
{
    if (key == "DOTHEROES_FPS" || key == "DOTHEROES_FPS_CUSTOM" || key == "DOTHEROES_PERF_OVERLAY") return "Frame rate";
    if (cat == "Game") return "Game start";
    return cat;
}

static void page_settings()
{
    title("Settings");
    text_col(col::dim, "Shared with the game's own menus. A setting marked * takes effect when the game next starts.");
    const json::Value& s = g_st.settings["settings"];
    if (s.size() == 0) { text_col(col::dim, "Install the game first."); return; }
    static int shown_tab = -1;
    ImGui::Dummy(ImVec2(0, S(2)));
    const int ntabs = (int)(sizeof kSettingsTabs / sizeof *kSettingsTabs);
    const float gap = S(12);   // one row whatever the window's width (tabs closer together than the page's buttons): the controller's left
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, ImGui::GetStyle().ItemSpacing.y));
    float need = 0;   // the widest label with the buttons' padding, never clipped while the row has room
    for (const char* t : kSettingsTabs) need = std::max(need, ImGui::CalcTextSize(t).x + S(40));
    const float share = (ImGui::GetContentRegionAvail().x - gap * (ntabs - 1) - S(24)) / ntabs;
    const float tw = floorf(std::min(share, std::max(S(210), need)));
    for (int i = 0; i < ntabs; i++) {                     // and right go along it, down goes into the settings
        if (i) ImGui::SameLine();
        char lab[64];
        snprintf(lab, sizeof lab, "%s###settings_tab%d", kSettingsTabs[i], i);
        if (big_button(lab, ImVec2(tw, 0), g_settings_tab == i ? Kind::Primary : Kind::Normal)) g_settings_tab = i;
        if (g_settings_tab == i) focus_first();   // the page's first focus: the chosen tab
        if (ImGui::IsItemFocused()) prompt(Act::A, "Show");
    }
    ImGui::PopStyleVar();
    for (size_t i = 0; i < g_st.settings["problems"].size(); i++)
        text_col(col::warm, "%s", g_st.settings["problems"][i].str().c_str());
    static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> choices = {
        { "DOTHEROES_FPS", { { "30", "30 fps (the original)" }, { "60", "60 fps" }, { "75", "75 fps" }, { "100", "100 fps" },
                             { "120", "120 fps" }, { "144", "144 fps" }, { "165", "165 fps" }, { "240", "240 fps" },
                             { "display", "The display's rate" }, { "0", "Unlimited" }, { "custom", "Custom" } } },
        { "DOTHEROES_DISPLAY_MODE", { { "windowed", "Windowed" }, { "borderless", "Borderless fullscreen" },
                                      { "exclusive", "Exclusive fullscreen" } } },
        { "DOTHEROES_RENDER_RES", { { "native", "Native (the display's)" }, { "1280x720", "1280 x 720 (the original)" },
                                    { "1920x1080", "1920 x 1080" }, { "2560x1440", "2560 x 1440" }, { "3840x2160", "3840 x 2160" },
                                    { "custom", "Custom" } } },
        { "DOTHEROES_PERF_OVERLAY", { { "off", "Off" }, { "fps", "Frame rate" }, { "graph", "Frame rate and times" } } },
        { "DOTHEROES_OVERLAY_COMBO", { { "l3r3", "L3 + R3" }, { "selectstart", "Back/Select + Start" },
                                       { "l1r1select", "L1 + R1 + Back/Select" } } },
        { "LAUNCHER_UPDATE_CHANNEL", { { "stable", "Stable releases" }, { "prerelease", "Test releases too" } } },
        { "DOTHEROES_LOG_LEVEL", { { "error", "Errors only" }, { "warning", "Errors and warnings" }, { "info", "Normal" },
                                   { "debug", "Detailed" }, { "trace", "Everything" } } },
        { "LAUNCHER_PROMPTS", { { "auto", "Automatic" }, { "xbox", "Xbox" }, { "playstation", "PlayStation" },
                                { "nintendo", "Nintendo Switch" }, { "steam", "Steam" }, { "generic", "Generic" },
                                { "keyboard", "Keyboard and mouse" } } },
        { "LAUNCHER_DISPLAY_MODE", { { "windowed", "Windowed" }, { "borderless", "Borderless fullscreen" },
                                     { "exclusive", "Exclusive fullscreen" } } },
    };
    std::string last_cat;
    ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    if (shown_tab != g_settings_tab) { ImGui::SetScrollY(0); shown_tab = g_settings_tab; }   // another tab: from its top
    // the categories in a fixed order, each setting in the schema's order within it
    std::vector<std::pair<std::string, const json::Value*>> ordered;
    static const char* cats[] = { "Display", "Game", "Audio", "Controls", "Launcher", "Shaders", "Updates", "Saves", "Diagnostics" };
    for (const char* c : cats)
        for (const auto& kv : s.o)
            if (kv.second["category"].str() == c) ordered.push_back({ kv.first, &kv.second });
    for (const auto& kv0 : ordered) {
        const std::pair<std::string, const json::Value&> kv(kv0.first, *kv0.second);
        const json::Value& x = kv.second;
        const std::string cat = x["category"].str();
        if (x["advanced"].truthy()) continue;   // developers' switches: the worker's command line, not this page
        if (settings_tab_of(kv.first, cat) != g_settings_tab) continue;   // another tab's
        const std::string section = settings_section_of(kv.first, cat);
        if (section != last_cat) {
            ImGui::Dummy(ImVec2(0, S(6)));
            title(section.c_str(), 24);
            last_cat = section;
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
        } else if (key == "DOTHEROES_RENDER_RES_CUSTOM") {   // M11.2f: only with a custom resolution chosen
            if (setting("DOTHEROES_RENDER_RES", "native") != "custom") continue;
            cycle = { "960x540", "1600x900", "1920x1200", "2048x1152", "2560x1080", "2560x1600", "2880x1620", "3200x1800",
                      "3440x1440", "5120x2160", "5120x2880", "7680x4320" };
            if (std::find(cycle.begin(), cycle.end(), val) == cycle.end()) cycle.push_back(val);
            auto px = [](const std::string& s) { int w = 0, h = 0; sscanf(s.c_str(), "%dx%d", &w, &h); return (long)w * h; };
            std::sort(cycle.begin(), cycle.end(), [&](const std::string& a, const std::string& b) { return px(a) < px(b); });
            shown = val;
            for (char& c : shown) if (c == 'x') { shown.replace(&c - &shown[0], 1, " x "); break; }
        } else if (key == "DOTHEROES_VOLUME_MASTER" || key == "DOTHEROES_VOLUME_MUSIC" || key == "DOTHEROES_VOLUME_SFX") {
            cycle = { "0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100" };
            shown = val + "%";
        } else if (key == "DOTHEROES_SKIP_LOGOS" || key == "DOTHEROES_VSYNC" || key == "DOTHEROES_MUTE" || key == "DOTHEROES_OVERLAY_HINT" ||
                   key == "LAUNCHER_CLOSE_ON_START" || key == "PS3RECOMP_VK_FULLSCREEN" || key == "LAUNCHER_CHECK_UPDATES" ||
                   key == "LAUNCHER_SHADERS_AFTER_UPDATE" || key == "LAUNCHER_SHADERS_ON_DRIVER_CHANGE" ||
                   key == "LAUNCHER_VSYNC" || key == "DOTHEROES_DEVELOPER_MODE") {
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
    // (0.2.0-beta.5: Add to Steam, the shortcuts and the install folder are on Install / Manage)
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
        g_diag.folder = path_str(u8path(log_dir()) / "report");
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
        auto then = std::move(op.then);
        json::Value r = std::move(op.result);
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
                                   self_program() },
                    "Uninstalling", true, [](const json::Value& r) {
                        if (r["data"]["removed"]["shortcuts"]["steam_later"].truthy()) {   // Steam ran: once it's closed
                            g_prefs.steam_cleanup = true;
                            g_prefs.save();
                        }
                        if (!r["ok"].truthy()) show_result_error(r, "Uninstalling didn't finish");
                        else g_msg = { true, "Uninstalled", "About " + human_bytes(r["data"]["bytes"].num()) +
                                       " freed. Install again whenever you like: what you kept is used again.", "", false, false };
                        g_page = kManage;
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

static bool IsKeyPressedNoNav(ImGuiKey k) { return ImGui::IsKeyPressed(k, true); }

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
        // the text as typed (a keyboard types into it directly too): opened from a keyboard or mouse, the field
        // has the focus; from a controller, the keys do -- and a physical keyboard's first keys still reach the text
        // (the next ones go into the field directly)
        static bool field_next = false, caret_end = false;
        ImGuiIO& io = ImGui::GetIO();
        if ((kf.left > 0 && g_device == Device::Keys) || field_next) {
            ImGui::SetKeyboardFocusHere();
            caret_end = field_next;   // caught-up typing goes on after what was typed (not over it: no select-all)
            field_next = false;
        }
        char buf[256];
        snprintf(buf, sizeof buf, "%s", g_osk.text.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##osktext", buf, sizeof buf,
                             (g_osk.secret ? ImGuiInputTextFlags_Password : 0) | ImGuiInputTextFlags_CallbackAlways,
                             [](ImGuiInputTextCallbackData* d) {
                                 if (caret_end) {
                                     d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
                                     caret_end = false;
                                 }
                                 return 0;
                             }))
            g_osk.text = buf;
        if (g_osk.secret) memset(buf, 0, sizeof buf);
        bool typed = false;   // (this frame: the on-screen keys don't also act on the same key -- Space, say)
        if (!ImGui::IsItemActive() && kf.left == 0) {
            for (ImWchar c : io.InputQueueCharacters)
                if (c >= 32 && c < 127 && g_osk.text.size() < 200) { g_osk.text += (char)c; typed = true; }
            if (IsKeyPressedNoNav(ImGuiKey_Backspace) && !g_osk.text.empty()) { g_osk.text.pop_back(); typed = true; }
            if (typed) { io.InputQueueCharacters.resize(0); field_next = true; }
        }
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
                if (big_button(id, ImVec2(key, key)) && !typed) g_osk.text += row[i];
                if (first) { if (g_device != Device::Keys) want_focus(kf); focus_tick(kf); }
                first = false;
                if (row[i + 1]) ImGui::SameLine(0, S(8));
            }
        }
        if (big_button("Shift", ImVec2(key * 2, key))) g_osk.shift = !g_osk.shift;
        ImGui::SameLine(0, S(8));
        if (big_button("Space", ImVec2(key * 4, key)) && !typed) g_osk.text += ' ';
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
            std::fill(g_osk.text.begin(), g_osk.text.end(), '\0');
            g_osk.text.clear();
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

// ---- the overlays: the key, the results, the choosers ------------------------------------------------------------
static bool chars_overlay_open()
{
    return g_key.open || g_results.open || g_pick.import_menu || g_pick.filters || g_pick.sort || g_pick.review;
}

static void small_label(const char* t, ImU32 c = col::dim)
{
    ImGui::PushStyleColor(ImGuiCol_Text, v4(c));
    ImGui::PushFont(nullptr, S(kSmall));
    ImGui::TextWrapped("%s", t);
    ImGui::PopFont();
    ImGui::PopStyleColor();
}

static bool modal_begin(const char* id, float width)
{
    ImGui::OpenPopup(id);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x - S(80), S(width)), 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, vp->Size.y - S(60)));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
        return false;
    frame_window();
    return true;
}

static void overlay_key()
{
    static OpenFocus kf;
    open_focus(kf, g_key.open);
    if (!g_key.open) return;
    if (!modal_begin("##ps3key", 900)) return;
    title(g_key.count > 1 ? (std::to_string(g_key.count) + " encrypted PS3 characters").c_str() : "Encrypted PS3 character", 28);
    ImGui::TextWrapped("%s", g_key.count > 1
        ? "These characters are encrypted and need your PS3 key before they can be imported. Your key is used on this "
          "computer only: never uploaded or shared."
        : "This character is encrypted and needs your PS3 key before it can be imported. Your key is used on this "
          "computer only: never uploaded or shared.");
    ImGui::Spacing();
    small_label("The key: the PS3's \"syscon manager key\" -- the console's key for protected save data. 32 hexadecimal "
                "digits (0-9, A-F), 16 bytes. Cubic.Heroes doesn't include it.");
    ImGui::Spacing();
    // the field (the whole width), then paste, show or hide, the on-screen keyboard
    ImGui::SetNextItemWidth(-1);
    const ImGuiInputTextFlags fl = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase |
                                   (g_key.show ? 0 : ImGuiInputTextFlags_Password);
    ImGui::InputTextWithHint("##key", "32 hexadecimal digits", g_key.buf, 33, fl);
    want_focus(kf);
    if (ImGui::IsItemFocused()) g_focus_label = "the key";
    const bool type_pad = ImGui::IsItemFocused() && g_device == Device::Pad && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    if (big_button("Paste", ImVec2(S(150), S(44)))) {
        if (char* clip = SDL_GetClipboardText()) {
            std::string hex;
            for (const char* c = clip; *c && hex.size() < 32; c++)
                if (isxdigit((unsigned char)*c)) hex += (char)toupper((unsigned char)*c);
            SDL_free(clip);
            memset(g_key.buf, 0, sizeof g_key.buf);
            memcpy(g_key.buf, hex.data(), hex.size());
        }
    }
    ImGui::SameLine(0, S(12));
    if (big_button(g_key.show ? "Hide" : "Show", ImVec2(S(150), S(44)))) g_key.show = !g_key.show;
    ImGui::SameLine(0, S(12));
    if (big_button("Type on the on-screen keyboard", ImVec2(S(380), S(44))) || type_pad)
        type_text("Your PS3 key (32 hexadecimal digits)", g_key.buf, [](const std::string& t) {
            std::string hex;
            for (char c : t) if (isxdigit((unsigned char)c) && hex.size() < 32) hex += (char)toupper((unsigned char)c);
            memset(g_key.buf, 0, sizeof g_key.buf);
            memcpy(g_key.buf, hex.data(), hex.size());
        }, true);
    // the format, checked as it is typed
    const size_t n = strlen(g_key.buf);
    const bool valid = n == 32;
    char msg[96];
    snprintf(msg, sizeof msg, valid ? "32 of 32 digits: the format is right" : "%zu of 32 digits", n);
    small_label(msg, valid ? col::ok : col::dim);
    if (!g_key.error.empty()) small_label(g_key.error.c_str(), col::danger);
    // keeping it: only in the system's protected store, else for this import only
    const json::Value& ks = g_chars.list["key"];
    if (!ks["store"].is_null()) {
        std::string lbl = "Keep it on this computer (" + ks["store"].str() + ")";
        ImGui::Checkbox(lbl.c_str(), &g_key.keep);
    } else {
        g_key.keep = false;
        small_label("No protected key store on this system: the key is used for this import only, never written to a file.");
    }
    if (ks["stored"].truthy()) {
        if (big_button("Forget the kept key", ImVec2(S(320), S(44)), Kind::Danger)) {
            run("characters", { "--install", install_dir(), "--char-action", "key-forget" }, "", false,
                [](const json::Value&) { chars_refresh(); });
            g_key.error = "The kept key was forgotten. Enter the key to import.";
        }
    }
    ImGui::Spacing();
    const bool go = big_button("Import", ImVec2(S(260), S(52)), Kind::Primary, valid);
    ImGui::SameLine();
    const bool cancel = big_button("Cancel", ImVec2(S(260), S(52)));
    focus_tick(kf);
    prompt(Act::A, "Choose");
    prompt(Act::B, "Cancel");
    const bool b = pressed(Act::B) && !ImGui::IsItemActive();
    if ((go && valid) || cancel || b) {
        std::string key(g_key.buf, strlen(g_key.buf));
        memset(g_key.buf, 0, sizeof g_key.buf);
        const auto paths = g_key.paths;
        const auto folders = g_key.folders;
        const bool keep = g_key.keep;
        g_key.open = false;
        ImGui::CloseCurrentPopup();
        if (go && valid) chars_import(paths, folders, key, keep);
        std::fill(key.begin(), key.end(), '\0');
    }
    ImGui::EndPopup();
}

static void overlay_results()
{
    static OpenFocus rf;
    open_focus(rf, g_results.open);
    if (!g_results.open) return;
    if (!modal_begin("##importresults", 820)) return;
    ImGui::PushStyleColor(ImGuiCol_Text, v4(g_results.error ? col::danger : col::ok));
    title(g_results.title.c_str(), 28);
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", g_results.summary.c_str());
    if (!g_results.lines.empty()) {
        if (big_button(g_results.details ? "Hide the details" : "Show the details", ImVec2(S(300), S(44))))
            g_results.details = !g_results.details;
        if (g_results.details) {
            ImGui::BeginChild("##resultlines", ImVec2(0, S(220)), ImGuiChildFlags_Borders);
            ImGui::PushFont(nullptr, S(kSmall));
            ImGui::TextWrapped("%s", g_results.lines.c_str());
            ImGui::PopFont();
            ImGui::EndChild();
        }
    }
    const bool ok = big_button("OK", ImVec2(S(220), S(52)), Kind::Primary);
    want_focus(rf);
    focus_tick(rf);
    prompt(Act::A, "Choose");
    prompt(Act::B, "Close");
    if (ok || pressed(Act::B)) { g_results.open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

// a chip: a small toggle in a chooser, gold when chosen (the focus ring shows where the cursor is)
static bool chip(const std::string& label, bool on, float w = 0)
{
    const float tw = ImGui::CalcTextSize(label.c_str()).x + S(40);
    return big_button(label.c_str(), ImVec2(std::max(w, tw), S(44)), on ? Kind::Primary : Kind::Normal);
}

static void chips_wrap(float w)
{
    // the next chip on this row when it fits, else on the next
    const float right = ImGui::GetCurrentWindow()->WorkRect.Max.x;
    if (ImGui::GetItemRectMax().x + S(12) + w < right) ImGui::SameLine(0, S(12));
}

static void overlay_pickers()
{
    static OpenFocus im, fi, so, rv;
    open_focus(im, g_pick.import_menu);
    open_focus(fi, g_pick.filters);
    open_focus(so, g_pick.sort);
    open_focus(rv, g_pick.review);
    if (g_pick.import_menu && modal_begin("##importmenu", 720)) {
        title("Import characters", 28);
        small_label("Character files (.chchar), ZIP archives, DATA.EDT files, and folders of characters saved on a PS3. "
                    "You can also drop them on this window.");
        ImGui::Spacing();
        const float w = ImGui::GetContentRegionAvail().x;
        bool close = false;
        if (big_button("Character files...", ImVec2(w, S(52)), Kind::Primary)) {
            close = true;
            browse("Import characters", false, { ".chchar", ".zip", ".edt" }, "Characters (.chchar, .zip, DATA.EDT)", "",
                   [](const std::string& f) { chars_import({ f }, {}); });
        }
        want_focus(im);
        if (big_button("A folder of characters...", ImVec2(w, S(52)))) {
            close = true;
            browse("Import the characters in a folder (a PS3's SAVEDATA, for example)", true, {}, "", "",
                   [](const std::string& d) { chars_import({ d }, {}); });
        }
        int plain = 0;
        const json::Value& sv = g_chars.list["in_saves"];
        for (size_t i = 0; i < sv.size(); i++) plain += sv[i]["kind"].str() == "plain" ? 1 : 0;
        if (plain > 0 && big_button(("Characters made in the game (" + std::to_string(plain) + " not in the library)").c_str(),
                                    ImVec2(w, S(52)))) {
            close = true;
            std::vector<std::string> fs;
            for (size_t i = 0; i < sv.size(); i++) if (sv[i]["kind"].str() == "plain") fs.push_back(sv[i]["folder"].str());
            chars_import({}, fs);
        }
        if (g_chars.list["key"]["stored"].truthy() && big_button("Forget my kept PS3 key", ImVec2(w, S(52)), Kind::Danger)) {
            close = true;
            ask("Forget your PS3 key?", "It is removed from " + g_chars.list["key"]["store"].str() + ". Characters already "
                "imported stay as they are; importing another encrypted character asks for the key again.", "Forget it",
                true, [] { run("characters", { "--install", install_dir(), "--char-action", "key-forget" }, "", false,
                               [](const json::Value&) { chars_refresh(); }); });
        }
        if (big_button("Cancel", ImVec2(w, S(52))) || pressed(Act::B)) close = true;
        focus_tick(im);
        prompt(Act::A, "Choose");
        prompt(Act::B, "Cancel");
        if (close) { g_pick.import_menu = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (g_pick.filters && modal_begin("##filters", 860)) {
        title("Filters", 28);
        std::map<std::string, int> cl_n, so_n;
        const json::Value& items = chars_items();
        for (size_t i = 0; i < items.size(); i++) { cl_n[items[i]["class"].str()]++; so_n[items[i]["source"].str()]++; }
        small_label("Class");
        bool first = true;
        if (chip("Any class", ct().f_class.empty())) ct().f_class.clear();
        if (first) { want_focus(fi); first = false; }
        for (const auto& kv : cl_n) {
            const std::string& k = kv.first;
            const std::string lbl = k + " (" + std::to_string(kv.second) + ")";
            chips_wrap(ImGui::CalcTextSize(lbl.c_str()).x + S(40));
            if (chip(lbl, ct().f_class == k)) ct().f_class = k;
        }
        if (g_chars.tab == kTabLibrary) {
            small_label("In the game");
            const char* inst[] = { "Any", "Installed", "Not installed" };
            for (int i = 0; i < 3; i++) {
                if (i) ImGui::SameLine(0, S(12));
                if (chip(inst[i], ct().f_installed == i)) ct().f_installed = i;
            }
        }
        small_label("Source");

        if (chip("Any source", ct().f_source.empty())) ct().f_source.clear();
        for (const auto& kv : so_n) {
            const std::string& k = kv.first;
            const std::string lbl = std::string(source_name(k)) + " (" + std::to_string(kv.second) + ")";
            chips_wrap(ImGui::CalcTextSize(lbl.c_str()).x + S(40));
            if (chip(lbl, ct().f_source == k)) ct().f_source = k;
        }
        ImGui::Spacing();
        bool close = false;
        if (big_button("Clear the filters", ImVec2(S(300), S(52)))) { ct().f_class.clear(); ct().f_source.clear(); ct().f_installed = 0; }
        ImGui::SameLine();
        if (big_button("Done", ImVec2(S(220), S(52)), Kind::Primary) || pressed(Act::B)) close = true;
        focus_tick(fi);
        prompt(Act::A, "Choose");
        prompt(Act::B, "Done");
        if (close) { g_pick.filters = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (g_pick.sort && modal_begin("##sort", 560)) {
        title("Order", 28);
        const char* names[] = { "Name, A to Z", "Name, Z to A", "Newest added first", "Class" };
        bool close = false;
        const float w = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < 4; i++) {
            if (chip(names[i], ct().sort == i, w)) { ct().sort = i; close = true; }
            if (i == ct().sort) want_focus(so);
        }
        small_label("(Recently used isn't offered: the game doesn't record which character was played.)");
        if (pressed(Act::B)) close = true;
        focus_tick(so);
        prompt(Act::A, "Choose");
        prompt(Act::B, "Cancel");
        if (close) { g_pick.sort = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (g_pick.review && modal_begin("##review", 820)) {
        const json::Value& sv = g_chars.list["in_saves"];
        std::vector<std::string> fs;
        for (size_t i = 0; i < sv.size(); i++) if (sv[i]["kind"].str() == "encrypted") fs.push_back(sv[i]["folder"].str());
        title(fs.size() == 1 ? "An encrypted PS3 character" : (std::to_string(fs.size()) + " encrypted PS3 characters").c_str(), 28);
        small_label("In the game's saves, copied from a PS3 and still encrypted: the game shows them blank. Importing them "
                    "decrypts a copy into your library (the originals are not changed); then install them from here.");
        ImGui::BeginChild("##reviewlist", ImVec2(0, std::min(S(260), S(34) * (float)fs.size() + S(16))), ImGuiChildFlags_Borders);
        for (size_t i = 0; i < sv.size(); i++)
            if (sv[i]["kind"].str() == "encrypted")
                ImGui::TextUnformatted((sv[i]["name"].str().empty() ? std::string("(no name)") : sv[i]["name"].str()).c_str());
        ImGui::EndChild();
        bool close = false;
        if (big_button(fs.size() == 1 ? "Import it" : "Import all", ImVec2(S(260), S(52)), Kind::Primary)) {
            close = true;
            chars_import({}, fs);
        }
        want_focus(rv);
        ImGui::SameLine();
        if (big_button("Not now", ImVec2(S(220), S(52))) || pressed(Act::B)) close = true;
        focus_tick(rv);
        prompt(Act::A, "Choose");
        prompt(Act::B, "Not now");
        if (close) { g_pick.review = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static void overlay_chars()
{
    overlay_pickers();
    overlay_key();
    overlay_results();
}


static bool overlay_open() { return g_op || g_confirm.open || g_msg.open || g_browser.open || g_osk.open || g_uninst.open || chars_overlay_open(); }

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
            auto then = std::move(g_bg->then);
            json::Value r = std::move(g_bg->result);
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
            run("shortcuts", { "--exe", self_program(), "--remove-steam" }, "", false,
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
            auto then = std::move(g_queries[i]->then);
            json::Value r = std::move(g_queries[i]->result);
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
        {   // the row's name; a long one (Install / Manage) a size smaller when the menu is too narrow for it
            const char* label = exit_row ? "EXIT" : kPageNames[i];
            const float tx = ra.x + S(22) + ip * 10 + S(22), room = rb.x - tx - S(8);
            float lp = np;
            while (lp > 2.0f && pix_w(label, lp) > room) lp -= 1.0f;
            pix_text(sdl, ImVec2(tx, cy - lp * 3.5f), label, lp, on ? col::accent : hov ? col::text : col::dim);
        }
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
    g_page_owns_back = false;
    ImGui::BeginChild("##page", ImVec2(cb.x - ca.x - in_x - S(34), cb.y - ca.y - in_y - S(26)), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    g_page_window = ImGui::GetCurrentWindow();
    ImGui::PushFont(nullptr, S(kBody));
    switch (g_page) {
    case kPlay: page_play(); break;
    case kManage: page_manage(); break;
    case kSaves: page_saves(); break;
    case kCharacters: {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        page_characters();
        g_nav_page_max = std::max(g_nav_page_max, (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency());
        break;
    }
    case kSettings: page_settings(); break;
    case kDiagnostics: page_diagnostics(); break;
    }
    if (g_page != kCharacters && !g_previews.empty()) previews_release();   // nothing of it kept in memory
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
    overlay_chars();
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
        // (a text field being typed in keeps its arrows: they move the caret)
        const bool typing = ImGui::GetIO().WantTextInput;
        auto free_press = [owned_lr, typing](ImGuiKey k) {
            return !owned_lr && !g_focus_owns_lr && !(typing && k == ImGuiKey_LeftArrow) && ImGui::IsKeyPressed(k, false);
        };
        if (in_menu && (free_press(ImGuiKey_GamepadDpadRight) || free_press(ImGuiKey_RightArrow))) g_focus_page = true;
        if (!in_menu && (free_press(ImGuiKey_GamepadDpadLeft) || free_press(ImGuiKey_LeftArrow))) {
            left_from = g_focus_label;
            left_wait = 2;
        } else if (left_wait > 0 && --left_wait == 0 && g_focus_label == left_from) {
            g_focus_menu = true;   // nothing to its left: the menu
        }
        if (g_page_owns_back) {   // a page's own Back (the character details): handled by the page
            prompt(Act::B, g_page_back_label);
        } else if (g_focus_label.rfind("menu: ", 0) != 0) {   // on a page: B goes to the menu
            prompt(Act::B, "Menu");
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
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
// stick down); wait FRAMES; type TEXT (as a keyboard types it); shot NAME (the launcher saves its own frame, NAME.bmp
// beside the script); quit. Each step
// is logged with the page, any dialog and the focused item after it settles (FILE.log). expect TEXT[|TEXT2...] stops
// the test unless the focus is on one of them (until and seek take alternatives the same way); see TEXT[|TEXT2...]
// unless a title with one of them is wholly in view. --nav-size WxH:
// the test window's size (0.2.0-beta.5: the Steam Deck's 1280x800 and 1280x720 tested); a test's window placement is
// never remembered
// the focused item's label holds one of "A|B|...": a page whose first item depends on its content (the Characters
// page: a card, or the empty library's Import characters...)
static bool focus_has(const std::string& alts)
{
    for (size_t from = 0; from <= alts.size();) {
        size_t bar = alts.find('|', from);
        if (bar == std::string::npos) bar = alts.size();
        const std::string want = alts.substr(from, bar - from);
        if (!want.empty() && g_focus_label.find(want) != std::string::npos) return true;
        from = bar + 1;
    }
    return alts.empty();
}
// the frames' work since the last logged step (building and submitting a frame: not the wait for VSync), for the
// test's log -- a page's cost measured where it runs (the Characters page with 1,000 characters)
static double g_nav_work_sum = 0, g_nav_work_max = 0;
static int g_nav_work_n = 0;
struct NavScript {
    struct Step { std::string op; int n = 1; std::string arg; };
    std::vector<Step> steps;
    size_t at = 0;
    int wait = 30, phase = 0, done_n = 0, rs = 0, idle = 0, until = 0, seek = 0, seek_t = 0;
    bool seek_held = false;   // the seek's key is down (pressed, its release not yet sent)
    std::string until_text, seek_text;
    ImGuiKey seek_key = ImGuiKey_None;
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
            if (st.op == "expect" || st.op == "see" || st.op == "type") {   // "expect TEXT", "see TEXT", "type TEXT": the rest
                std::getline(ls, rest);
                st.arg = rest.substr(std::min(rest.size(), rest.find_first_not_of(' ')));
            } else if (st.op == "until" || st.op == "seek") {   // "until N TEXT", "seek N DIR TEXT": at most N
                ls >> st.n;                                         // frames, then the rest of the line
                std::getline(ls, rest);
                st.arg = rest.substr(std::min(rest.size(), rest.find_first_not_of(' ')));
            } else if (ls >> rest) {
                if (st.op != "shot" && isdigit((unsigned char)rest[0])) st.n = atoi(rest.c_str());
                else st.arg = rest;
            }
            steps.push_back(st);
        }
        {   // a relative script's pictures beside it (its parent alone is empty: the drive's root)
            std::error_code aec;
            const fs::path ap = fs::absolute(u8path(path), aec);
            dir = path_str((aec ? u8path(path) : ap).parent_path());
        }
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
        if (op == "lt") return ImGuiKey_GamepadL2;
        if (op == "rt") return ImGuiKey_GamepadR2;
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
        char work[128] = "";
        if (g_nav_work_n)
            snprintf(work, sizeof work, "  work=%.2f/%.2fms page<=%.2f spawn<=%.2f view<=%.2f", g_nav_work_sum / g_nav_work_n,
                     g_nav_work_max, g_nav_page_max, g_nav_spawn_max, g_nav_view_max);
        fprintf(log, "%-6s %-14s page=%-12s dialog=%-28s focus=%s%s%s%s\n", st.op.c_str(), st.arg.c_str(), kPageNames[g_page],
                dlg, g_focus_label.c_str(), g_bg ? "  [preparing]" : "", g_nav_event.c_str(), work);
        g_nav_event.clear();
        g_nav_work_sum = g_nav_work_max = g_nav_page_max = g_nav_spawn_max = g_nav_view_max = 0;
        g_nav_work_n = 0;
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
        if (seek > 0) {   // "seek N DIR TEXT": DIR pressed now and then until the focus is on TEXT, N frames at most
            if (focus_has(seek_text) && !g_op) seek = 0;
            else {
                --seek;
                const int at_t = seek_t++ % 20;   // down for 2 frames, up, then the frames a move takes
                if (at_t == 0) { io.AddKeyAnalogEvent(seek_key, true, 1.0f); seek_held = true; }
                else if (at_t == 2) { io.AddKeyAnalogEvent(seek_key, false, 0.0f); seek_held = false; }
                if (seek > 0) return;
            }
            // the key let go however the seek ended: one left held (its target reached before its release) went on
            // repeating with no controller connected to report it up, and moved the focus by itself (0.2.0-beta.5,
            // Linux: onto a Settings value, which the script's next left or right then changed)
            if (seek_held) { io.AddKeyAnalogEvent(seek_key, false, 0.0f); seek_held = false; }
        }
        if (until > 0) {   // "until N TEXT": until the focus is on TEXT (part of its label) and no operation is on
            // screen, N frames at most -- then the script goes on (an expect after it says whether it got there)
            if ((!focus_has(until_text) || g_op) && --until > 0) return;
            until = 0;
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
        } else if (st.op == "type") {   // "type TEXT": typed as a keyboard would (into the field or keyboard on screen)
            io.AddInputCharactersUTF8(st.arg.c_str());
            wait = 6;
        } else if (st.op == "idle") {
            idle = st.n;
        } else if (st.op == "until") {
            until = st.n;
            until_text = st.arg;
        } else if (st.op == "seek") {
            const size_t sp = st.arg.find(' ');
            seek_key = key(st.arg.substr(0, sp));
            seek_text = sp == std::string::npos ? std::string() : st.arg.substr(sp + 1);
            seek = seek_key == ImGuiKey_None ? 0 : st.n;
            seek_t = 0;
        } else if (st.op == "expect") {   // the focus must be on this (part of its label), or the test stops at once:
            // a script out of step must never press A on something it didn't mean to
            if (!focus_has(st.arg)) {
                if (log) fprintf(log, "FAILED: expected the focus on \"%s\", it is on \"%s\": the test stops here\n",
                                 st.arg.c_str(), g_focus_label.c_str());
                at = steps.size();
                g_quit = true;
                return;
            }
        } else if (st.op == "see") {   // a title with this text wholly in view (the last frame), or the test stops: a
            // result the controller's player can't see, or a card the D-pad can't reach, is caught by its title
            bool seen = false;   // "see A|B": either of them
            for (size_t from = 0; from <= st.arg.size() && !seen;) {
                size_t bar = st.arg.find('|', from);
                if (bar == std::string::npos) bar = st.arg.size();
                const std::string want = st.arg.substr(from, bar - from);
                for (const std::string& t : g_nav_titles) seen = seen || (!want.empty() && t.find(want) != std::string::npos);
                from = bar + 1;
            }
            if (!seen) {
                if (log) fprintf(log, "FAILED: expected \"%s\" in view, the focus on \"%s\": the test stops here\n",
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

#include "cli.h"   // M11.2c: the public command line

// beta.4: a launcher update a crash or a power cut left unfinished (its journal, downloads/launcher-update.json, with
// its helper gone) is put right before anything uses the installation: the recovery's helper starts, waits for this
// process to exit, puts the previous launcher back (or keeps a swap that had finished and answers as expected) and,
// for the window, starts the launcher again. An update still being installed is left to finish. Returns the exit
// code when this process must exit now, else -1.
static int unfinished_launcher_update(const cli::Args& a, int argc, char** argv)
{
    for (int i = 1; i < argc; i++)   // the update's own starts never wait for it
        if (!strcmp(argv[i], "--after-update") || !strcmp(argv[i], "--update-failed")) return -1;
    if (a.command == "version" || a.command == "help") return -1;   // the helper's own check of the new launcher
    const std::string jpath = path_str(u8path(launcher_update_dir()) / "downloads" / "launcher-update.json");
    std::error_code ec;
    if (!fs::exists(u8path(jpath), ec)) return -1;
    // The recovery decides (tools/updates.py recover_launcher): an update still being installed by its helper is
    // left to finish -- that helper starts the launcher when it is done -- and anything else is put right, then (for
    // the window) the launcher is started again with how it went. Whether the recorded helper still runs isn't
    // judged here: a process id alone can't tell (one reused after a reboot, one ended but not yet reaped).
    std::vector<std::string> av = recovery_argv();
    for (const std::string& x : launcher_update_args()) av.push_back(x);
    av.push_back("--wait-pid");
    av.push_back(std::to_string(this_pid()));
    const bool window = cli::wants_window(a);
    if (!window) av.push_back("--no-restart");
    start_detached(av, launcher_update_dir());
    if (window) return 0;   // the launcher opens again once the update is finished or put right
    const std::string msg = "An update of the launcher is being installed, or an interrupted one put right; run the "
                            "command again in a moment.";
    if (a.json) printf("{\"ok\":false,\"code\":\"busy\",\"message\":%s}\n", json::quote(msg).c_str());
    else fprintf(stderr, "CubicHeroes: %s\n", msg.c_str());
    return cli::kBusy;
}

int main(int argc, char** argv)
{
    g_exe_dir = exe_dir_of(argv[0]);
    // M11.2: the command line first -- a command (--play, --status, --install ...) runs here and never opens a window;
    // no command (or --launcher) is the launcher's window. Either way the installation root is resolved first.
    const cli::Args cli_args = cli::parse(cli::utf8_args(argc, argv));
    g_root = resolve_root(cli_args.install_dir);
    note_recomplauncher();   // 0.2.0-beta.5: started by RecompLauncher -- its mark, so later starts keep our updater off
    {
        const int u = unfinished_launcher_update(cli_args, argc, argv);
        if (u >= 0) return u;
    }
    if (!cli::wants_window(cli_args)) {
        const int rc = cli::run(cli_args);
        if (rc != cli::kOpenLauncher) return rc;   // --play --or-launcher: the game not ready -- the window instead
    }
    if (cli_args.remember && !cli_args.install_dir.empty()) remember_root(g_root);
    bool after_update = false, update_failed = false;
    std::string show;
    int nav_w = 0, nav_h = 0;   // --nav-size WxH: a scripted test's window size
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--nav-size") && i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &nav_w, &nav_h) != 2)
            nav_w = nav_h = 0;
        if (!strcmp(argv[i], "--after-update")) after_update = true;
        if (!strcmp(argv[i], "--update-failed")) update_failed = true;
        if (!strcmp(argv[i], "--show") && i + 1 < argc) show = argv[i + 1];   // a dialog opened at start (the review's)
        if (!strcmp(argv[i], "--nav-test") && i + 1 < argc) {   // the scripted controller test
            nav.load(argv[i + 1]);
            g_nav_test = true;
        }
        if (!strcmp(argv[i], "--page") && i + 1 < argc) {   // a page to open on (a shortcut's choice; the review's)
            const int p = page_from_name(argv[i + 1]);
            if (p >= 0) g_page = p;
            if (!SDL_strcasecmp(argv[i + 1], "updates")) g_focus_updates = true;   // the updates, now on Play
        }
    }
    g_prefs.load();
    if (g_nav_test && nav_w >= 960 && nav_h >= 600) {   // the test's size (the window's smallest is 960x600)
        g_prefs.w = nav_w;
        g_prefs.h = nav_h;
        g_prefs.maximized = false;
    }
    {   // M11.2b: a root that can't be written (a read-only or protected folder, a full drive) said at once
        std::string why;
        if (!root_writable(why))
            g_msg = { true, "This folder can't be used", "Cubic.Heroes keeps the game, its saves and settings in its own "
                      "folder, and this one can't be written to: " + why + ". Move Cubic.Heroes to a folder you own (for "
                      "example in Documents or on another drive), or choose another folder on the Install / Manage page.",
                      g_root, true, false };
    }
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");   // sharp text at any display scale
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // --nav-test: the scripted test's window is never shown (it reads its pictures back itself) unless
    // CUBIC_NAV_HIDDEN=0 asks for it -- which the test harness's desktop guard (tests/desktop_guard.py) sets only
    // while the owner has approved an on-screen run
    // (beta.4: CUBIC_NAV_HIDDEN=1 hides any start's window, such as the one an update's helper starts during the
    // update tests -- the desktop guard sets it the same way)
    const char* hid = getenv("CUBIC_NAV_HIDDEN");
    const Uint32 hidden = (g_nav_test && !(hid && *hid == '0')) || (hid && *hid == '1') ? SDL_WINDOW_HIDDEN : 0;
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
    g_ren = ren;   // M12: the character previews' textures
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
    refresh_launcher_state();
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
    if (update_failed) {   // the helper put this launcher back (or never replaced it): why, from its record
        const json::Value last = last_launcher_update();
        const std::string why = last["why"].str("the new launcher didn't come up");
        g_msg = { true, "The update wasn't installed", last["result"].str() == "busy"
                  ? "Something was still running from the installation (" + why + "), so nothing was changed. Close the "
                    "game, then install the update again from the Play page."
                  : "Version " + last["version"].str("?") + " couldn't be put in place (" + why + "), so this launcher was "
                    "kept. Nothing else changed.", "", true, false };
    }
    if (after_update)
        g_msg = { true, "Updated to version " CUBIC_VERSION, "The launcher was updated, and the one before it is kept on the "
                  "Play page (the updates). Your saves, settings and builds are as they were; the game is brought up to date next if "
                  "this version needs it.", "", false, false };
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
            case SDL_DROPFILE: {   // a disc image or a keys file dropped onto the window (characters: on their page)
                std::string p = e.drop.file;
                SDL_free(e.drop.file);
                if (g_page == kCharacters) { g_char_drops.push_back(p); break; }
                std::string ext = path_str(u8path(p).extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                g_page = kManage;
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
        const Uint64 work_t0 = SDL_GetPerformanceCounter();
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        g_prompt_choice = setting("LAUNCHER_PROMPTS", "auto");
        stick_nav(io);
        if (nav.active()) nav.inject(io);   // the scripted controller (--nav-test)
        g_nav_titles.clear();   // its "see" has read the last frame's; this frame's are gathered as they're drawn
        ImGui::NewFrame();
        keep_focus_in_view();
        scroll_when_stuck();
        if (!checked_updates && !g_st.settings.is_null()) {
            checked_updates = true;
            if (setting("LAUNCHER_CHECK_UPDATES", "1") == "1") check_updates();
        }
        if (g_check_when_settings && !settings_pending()) check_updates();   // one asked for before they came
        frame();
        if (++frames == 20 && !show.empty()) open_show();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 14, 18, 44, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        if (nav.active()) {
            const double ms = (double)(SDL_GetPerformanceCounter() - work_t0) * 1000.0 / SDL_GetPerformanceFrequency();
            g_nav_work_sum += ms;
            g_nav_work_max = std::max(g_nav_work_max, ms);
            g_nav_work_n++;
            nav.after_render(ren);
        }
        SDL_RenderPresent(ren);
        if (after_update && !wrote_health) {   // the new launcher is up: the update is kept (tools/updates.py)
            const std::string health = env_u8("CUBIC_UPDATE_HEALTH");   // the helper's file, waiting for this version
            if (!health.empty()) write_text(health, CUBIC_VERSION "\n");
            wrote_health = true;
        }
    }
    // the window's placement for next time (not a scripted test's hidden window, whose size the test may have chosen)
    if (!g_nav_test) {
        const Uint32 fl = SDL_GetWindowFlags(win);
        g_prefs.maximized = (fl & SDL_WINDOW_MAXIMIZED) != 0;
        if (!g_prefs.maximized && !(fl & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))) {
            SDL_GetWindowPosition(win, &g_prefs.x, &g_prefs.y);
            SDL_GetWindowSize(win, &g_prefs.w, &g_prefs.h);
        }
        g_prefs.save();
    }
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
