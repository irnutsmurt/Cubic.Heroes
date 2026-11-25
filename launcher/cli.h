// M11.2c: CubicHeroes's public command line (docs/CLI.md) -- the one program a player, Steam or a third-party launcher
// needs: play directly, install, update, prepare the shaders, read the status, change settings. The worker and the
// game's program stay internal: this file drives the worker exactly as the launcher's window does (worker_argv) and
// starts the game exactly as Play does (game_exe). Included by main.cpp after its paths and helpers.
//
// Exit codes (stable, documented in docs/CLI.md):
//   0 success (--play: the game's own exit status when it ran)
//   1 the operation failed (the message says why; --json: `code`)
//   2 the command line is wrong (unknown option, missing value, conflicting commands)
//   3 not installed (play, update or prepare asked of a folder with no installation)
//   4 a prerequisite is missing or wrong (the disc image, the keys, the game's program)
//   5 busy (another operation holds this installation: a preparation, an update, the game running)
//   6 cancelled (Ctrl+C, the console closed, the calling launcher ended us)
//   7 the installation folder can't be used (read-only, protected, or not enough free space)
#pragma once
#include "game_exit.h"   // the game's exit statuses explained (M11.4, the review's W-2)
#include <cerrno>
#include <chrono>
#ifdef _WIN32
#include <shellapi.h>
#else
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cli {

enum Exit : int { kOk = 0, kFailed = 1, kUsage = 2, kNotInstalled = 3, kPrerequisite = 4, kBusy = 5, kCancelled = 6,
                  kRoot = 7, kManaged = 8,
                  kOpenLauncher = -1 };   // internal: --play --or-launcher hands over to the window (never an exit status)

#ifndef CUBIC_VERSION
#define CUBIC_VERSION "0.0.0-dev"
#endif

struct Args {
    std::string command;                     // the one command (see usage()); empty: the launcher's window
    std::string install_dir, disc, keys, from;
    std::string disc_key;                    // 0.2.0-beta.5: an encrypted ISO's disc key, a FILE (.dkey, .key, IRD)
    std::string rpcs3;                       // beta.6: the player's own RPCS3, to decrypt the game's program with
    bool json = false, no_gui = false, persist = false, remember = false, repair = false;
    bool or_launcher = false;                // --play --or-launcher: the window instead, when the game isn't ready
    std::vector<std::string> sets;           // --set KEY=VALUE, in order
    std::string display, fps, vsync;         // --play's display for this session (DOTHEROES_* settings)
    std::string resolution;                  // M11.2f: --resolution, the rendering resolution (native or WxH)
    int width = 0, height = 0;
    bool window_flags = false;               // --after-update, --show, --nav-test, --nav-size, --page: the window's
    std::string error;                       // a usage error
};

static const char* kUsageText =
    "CubicHeroes " CUBIC_VERSION " -- 3D Dot Game Heroes, recompiled for this PC from your own disc.\n"
    "\n"
    "Usage: CubicHeroes [COMMAND] [OPTIONS]\n"
    "\n"
    "With no command the launcher's window opens.\n"
    "\n"
    "Commands (one at a time):\n"
    "  --launcher             open the launcher's window (the default)\n"
    "  --play                 start the game directly, without the launcher's window; waits for it and exits with\n"
    "                         the game's exit status\n"
    "  --install              install from --disc (headless); does nothing if already installed\n"
    "                         (add --repair to rebuild from the disc anyway)\n"
    "  --update               bring the game up to date with this version of Cubic.Heroes: rebuilt only if the\n"
    "                         tools changed (your saves are backed up first)\n"
    "  --check-update         look for a newer release of Cubic.Heroes on the installation's update channel\n"
    "                         (nothing downloaded)\n"
    "  --apply-update         download the newer release (if it isn't already), check it against the release's\n"
    "                         signature and install it: CubicHeroes exits, and the update finishes a few seconds\n"
    "                         later; --status says how it ended. Then --update brings the game up to date\n"
    "  --rollback-launcher    put back the launcher the last update replaced (kept by it); the newer one is kept\n"
    "                         in turn\n"
    "  --prepare-shaders      prepare the shaders and pipelines the game will need (headless)\n"
    "  --rebuild-shaders      prepare them again from scratch\n"
    "  --status               the installation, its build, its settings and its shader cache\n"
    "  --set KEY=VALUE        change a setting (repeatable; with --play: for that session only, unless --persist)\n"
    "  --migrate-from PATH    copy an existing installation (from an earlier version's folder) into this one;\n"
    "                         the original is left as it is\n"
    "  --version              the versions of Cubic.Heroes and its parts\n"
    "  --help                 this text\n"
    "\n"
    "Options:\n"
    "  --install-dir PATH     the installation folder (default: the folder CubicHeroes is in); --remember keeps it\n"
    "                         for the next start (cubicheroes-root.txt beside CubicHeroes)\n"
    "  --disc PATH            the disc image (.iso) or disc folder, for --install\n"
    "  --keys PATH            the game program's keys, for --install: a keys file, a folder or ZIP of key files,\n"
    "                         or an EBOOT.elf you decrypted (read, never copied, never shown). Without it, the keys\n"
    "                         are looked for beside the disc and where they were found before\n"
    "  --rpcs3 PATH           your own RPCS3, to decrypt the game's program with (optional; never required)\n"
    "  --disc-key PATH        an encrypted ISO's disc key, for --install: a .dkey, .key or IRD file (never the key\n"
    "                         itself on a command line; read, never copied, never shown)\n"
    "  --json                 machine-readable output: one JSON object (--status, --version, --set ...), or the\n"
    "                         operation's events as JSON lines (--install, --update, --prepare-shaders ...)\n"
    "  --no-gui               never open a window (errors are reported here instead)\n"
    "  --persist              keep --play's display options and --set values as settings\n"
    "  --or-launcher          with --play: open the launcher's window instead when the game isn't installed yet (its\n"
    "                         setup) or isn't yet brought up to date with this version -- for other launchers\n"
    "\n"
    "Display options for --play (this session only unless --persist):\n"
    "  --fullscreen | --windowed | --borderless\n"
    "  --width N --height N   the window's size (windowed)\n"
    "  --resolution R         the rendering resolution: native (the display's), 1280x720, 1920x1080, 2560x1440,\n"
    "                         3840x2160 or any WxH -- the size the game's picture is drawn at, apart from the\n"
    "                         window's size\n"
    "  --fps N                30 (the original pacing), 60, 75, 100, 120, 144, 165, 240, any number, display, or 0\n"
    "                         (uncapped)\n"
    "  --vsync on|off\n"
    "\n"
    "Exit codes: 0 success (--play: the game's exit status), 1 failed, 2 wrong command line, 3 not installed,\n"
    "4 a prerequisite missing or wrong, 5 busy, 6 cancelled, 7 the installation folder can't be used.\n"
    "Full reference: docs/CLI.md.\n";

// ---- the command line ----

static std::vector<std::string> utf8_args(int argc, char** argv)
{
    std::vector<std::string> v;
#ifdef _WIN32
    (void)argc; (void)argv;
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; i++) v.push_back(launcher::narrow(w[i]));
        LocalFree(w);
    }
#else
    for (int i = 1; i < argc; i++) v.push_back(argv[i]);
#endif
    return v;
}

static Args parse(const std::vector<std::string>& av)
{
    Args a;
    auto set_command = [&](const std::string& c) {
        if (!a.command.empty() && a.command != c && a.error.empty())
            a.error = "--" + a.command + " and --" + c + " can't be used together";
        a.command = c;
    };
    for (size_t i = 0; i < av.size(); i++) {
        std::string o = av[i], val;
        bool has_val = false;
        const size_t eq = o.find('=');
        if (o.rfind("--", 0) == 0 && eq != std::string::npos && o.rfind("--set", 0) != 0) {
            val = o.substr(eq + 1);
            o = o.substr(0, eq);
            has_val = true;
        }
        auto value = [&](const char* what) -> std::string {
            if (has_val) return val;
            if (i + 1 < av.size()) return av[++i];
            if (a.error.empty()) a.error = o + " needs " + what;
            return std::string();
        };
        if (o == "--help" || o == "-h" || o == "/?" || o == "-?") set_command("help");
        else if (o == "--version") set_command("version");
        else if (o == "--launcher") set_command("launcher");
        else if (o == "--play") set_command("play");
        else if (o == "--install") set_command("install");
        else if (o == "--update") set_command("update");
        else if (o == "--check-update") set_command("check-update");
        else if (o == "--apply-update") set_command("apply-update");
        else if (o == "--rollback-launcher") set_command("rollback-launcher");
        else if (o == "--prepare-shaders") set_command("prepare-shaders");
        else if (o == "--rebuild-shaders") set_command("rebuild-shaders");
        else if (o == "--status") set_command("status");
        else if (o == "--migrate-from") { set_command("migrate-from"); a.from = value("a folder"); }
        else if (o == "--set") a.sets.push_back(value("KEY=VALUE"));
        else if (o.rfind("--set=", 0) == 0) a.sets.push_back(o.substr(6));
        else if (o == "--install-dir") a.install_dir = value("a folder");
        else if (o == "--disc") a.disc = value("a disc image or folder");
        else if (o == "--keys") a.keys = value("a keys file, a folder or ZIP of key files, or an EBOOT.elf");
        else if (o == "--rpcs3") a.rpcs3 = value("an RPCS3 program");
        else if (o == "--disc-key") a.disc_key = value("a disc key file");
        else if (o == "--json") a.json = true;
        else if (o == "--no-gui") a.no_gui = true;
        else if (o == "--persist") a.persist = true;
        else if (o == "--or-launcher") a.or_launcher = true;
        else if (o == "--remember") a.remember = true;
        else if (o == "--repair") a.repair = true;
        else if (o == "--fullscreen") a.display = "exclusive";
        else if (o == "--windowed") a.display = "windowed";
        else if (o == "--borderless") a.display = "borderless";
        else if (o == "--width" || o == "--height") {
            const std::string v = value("a number of pixels");
            char* end = nullptr;
            const long n = strtol(v.c_str(), &end, 10);
            if (v.empty() || (end && *end) || n < 320 || n > 16384) {
                if (a.error.empty()) a.error = o + " takes a number of pixels from 320 to 16384";
            } else {
                (o == "--width" ? a.width : a.height) = (int)n;
            }
        }
        else if (o == "--fps") a.fps = value("a frame rate");
        else if (o == "--resolution") a.resolution = value("a resolution (native or WxH)");
        else if (o == "--vsync") {
            const std::string v = value("on or off");
            if (v == "on" || v == "1" || v == "true") a.vsync = "1";
            else if (v == "off" || v == "0" || v == "false") a.vsync = "0";
            else if (a.error.empty()) a.error = "--vsync takes on or off";
        }
        // the launcher window's own options (reviews, the controller test, the update's restart)
        else if (o == "--after-update" || o == "--update-failed" || o == "--after-game") a.window_flags = true;
        else if (o == "--show" || o == "--nav-test" || o == "--nav-size" || o == "--page") {
            value("a value");
            a.window_flags = true;
        }
        else if (a.error.empty()) a.error = "unknown option " + av[i] + " (--help lists them)";
    }
    const bool display0 = !a.display.empty() || !a.fps.empty() || !a.vsync.empty() || a.width || a.height || !a.resolution.empty();
    if (a.error.empty() && !a.sets.empty() && a.command.empty()) a.command = "set";
    if (a.error.empty() && display0 && a.persist && a.command.empty()) a.command = "persist";   // display options kept
    if (a.error.empty() && a.command == "set" && a.sets.empty()) a.error = "--set needs KEY=VALUE";
    const bool display = !a.display.empty() || !a.fps.empty() || !a.vsync.empty() || a.width || a.height || !a.resolution.empty();
    if (a.error.empty() && display && a.command != "play" && !a.persist)
        a.error = "the display options apply to --play (or keep them with --persist)";
    if (a.error.empty() && (a.width != 0) != (a.height != 0)) a.error = "--width and --height go together";
    if (a.error.empty() && a.or_launcher && a.command != "play") a.error = "--or-launcher goes with --play";
    if (a.error.empty() && !a.resolution.empty() && a.resolution != "native") {   // M11.2f: WxH, as the settings take it
        int w = 0, h = 0;
        char tail = 0;
        if (sscanf(a.resolution.c_str(), "%dx%d%c", &w, &h, &tail) != 2 || w < 320 || h < 180 || w > 7680 || h > 4320)
            a.error = "--resolution takes native or a size from 320x180 to 7680x4320 (1920x1080, say)";
        else a.resolution = std::to_string(w) + "x" + std::to_string(h);
    }
    if (a.error.empty() && !a.fps.empty()) {
        static const char* named[] = { "display", "0", "uncapped" };
        bool ok = false;
        for (const char* n : named) ok = ok || a.fps == n;
        char* end = nullptr;
        const long n = strtol(a.fps.c_str(), &end, 10);
        if (!ok && end && !*end && n >= 30 && n <= 1000) ok = true;
        if (!ok) a.error = "--fps takes 30 to 1000, display, or 0 (uncapped)";
        if (a.fps == "uncapped") a.fps = "0";
    }
    if (a.error.empty() && a.command == "install" && !a.repair && a.disc.empty() && (!a.keys.empty() || !a.rpcs3.empty()))
        a.error = "--install needs --disc (the keys are found beside it, or given with --keys)";
    if (a.error.empty() && a.command == "install" && a.disc.empty() && a.repair)
        a.error = "--install --repair needs --disc";
    if (a.error.empty() && a.no_gui && (a.command.empty() || a.command == "launcher"))
        a.error = "--no-gui with nothing to do: give a command (--play, --status ...)";
    return a;
}

// whether this command line is the launcher's window (no command, --launcher, or the window's own options)
static bool wants_window(const Args& a) { return a.error.empty() && (a.command.empty() || a.command == "launcher"); }

// ---- output: the launcher is a windowed program (no console of its own); its output reaches the caller's pipe, or
// the console it was started from ----

static void attach_console()
{
#ifdef _WIN32
    auto usable = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    const bool out_ok = usable(STD_OUTPUT_HANDLE), err_ok = usable(STD_ERROR_HANDLE);
    if (out_ok && err_ok) return;   // a pipe or a file from the caller: the C runtime already writes there
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!out_ok) freopen("CONOUT$", "w", stdout);
        if (!err_ok) freopen("CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
}

static std::atomic<bool> g_cancel{ false };
#ifdef _WIN32
static BOOL WINAPI on_console_event(DWORD) { g_cancel = true; return TRUE; }
#else
static void on_signal(int) { g_cancel = true; }
#endif

static void say(const Args& a, const std::string& line) { if (!a.json) { fputs((line + "\n").c_str(), stdout); fflush(stdout); } }

static int fail(const Args& a, int code, const std::string& kind, const std::string& message, const std::string& details = "")
{
    if (a.json) {
        printf("{\"ok\":false,\"exit\":%d,\"code\":%s,\"message\":%s%s}\n", code, json::quote(kind).c_str(),
               json::quote(message).c_str(), details.empty() ? "" : (",\"details\":" + json::quote(details)).c_str());
        fflush(stdout);
    } else {
        fprintf(stderr, "CubicHeroes: %s\n", message.c_str());
        if (!details.empty()) fprintf(stderr, "%s\n", details.c_str());
    }
    return code;
}

// the worker's failure codes as this command line's exit codes
static int exit_for(const std::string& code)
{
    static const char* prerequisite[] = { "not_found", "not_a_disc", "unsupported_version", "damaged_disc", "keys_missing",
                                          "keys_unreadable", "keys_empty", "wrong_key", "no_disc_copy", "no_crypto",
                                          "no_trusted_keys" };
    static const char* busy[] = { "already_preparing", "busy", "game_running" };
    if (code == "managed_elsewhere") return kManaged;
    static const char* root[] = { "disk_space", "root_not_writable", "insufficient-space", "unsafe_path" };
    if (code == "cancelled") return kCancelled;
    if (code == "usage") return kUsage;
    if (code == "no_install" || code == "needs_migration" || code == "not_setup_app") return kNotInstalled;
    for (const char* c : prerequisite) if (code == c) return kPrerequisite;
    for (const char* c : busy) if (code == c) return kBusy;
    for (const char* c : root) if (code == c) return kRoot;
    return kFailed;
}

// ---- the worker, run to its end: its events shown as progress (or passed on as JSON lines), its result returned ----

struct Run { bool started = false; json::Value result; int exit = -1; };

static Run worker(const Args& a, const std::string& cmd, const std::vector<std::string>& args, bool show)
{
    Run r;
    launcher::Job job;
    if (!job.start(worker_argv(cmd, args), cmd)) return r;
    r.started = true;
    int shown_pct = -10;
    bool cancelled = false;
    for (;;) {
        const bool done = job.done();
        for (const json::Value& e : job.take_events()) {
            if (!show) continue;
            if (a.json) { printf("%s\n", json::dump(e).c_str()); fflush(stdout); continue; }
            const std::string ev = e["ev"].str();
            if (ev == "stage" && e["state"].str() == "start" && !e["name"].str().empty()) {
                say(a, e["name"].str() + "...");
                shown_pct = -10;
            } else if (ev == "progress" && e["overall"].type == json::Value::Number) {
                const int pct = (int)(e["overall"].num() * 100.0 + 0.5);
                if (pct >= shown_pct + 10 || (pct == 100 && shown_pct != 100)) {
                    say(a, "  " + std::to_string(pct) + "%");
                    shown_pct = pct;
                }
            } else if (ev == "log" && (e["level"].str() == "error" || e["level"].str() == "warning")) {
                fprintf(stderr, "  %s\n", e["text"].str().c_str());
            }
        }
        if (done) break;
        if (g_cancel && !cancelled) { job.cancel(); cancelled = true; say(a, "Cancelling..."); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    r.result = job.result();
    r.exit = job.exit_code();
    return r;
}

// a worker's failure reported (and its exit code); ok results return 0
static int finish(const Args& a, const Run& r, const std::string& what)
{
    if (!r.started) return fail(a, kPrerequisite, "no_worker", "Cubic.Heroes's worker is missing beside CubicHeroes (" +
                                                                   std::string("worker/") + "): reinstall Cubic.Heroes.");
    if (r.result.is_null()) return fail(a, kFailed, "internal", what + " ended without a result (exit " +
                                                                   std::to_string(r.exit) + ").");
    if (r.result["ok"].truthy()) return kOk;
    const std::string code = r.result["code"].str("internal");
    return fail(a, exit_for(code), code, r.result["message"].str(what + " failed."), r.result["details"].str());
}

static std::vector<std::string> install_arg() { return { "--install", g_root }; }
static std::string update_channel(const Args& a);

// ---- the commands ----

static int cmd_version(const Args& a)
{
    const Run r = worker(a, "version", {}, false);
    const json::Value& d = r.result["data"];
    if (a.json) {
        printf("{\"ok\":true,\"cubicheroes\":%s,\"worker\":%s,\"protocol\":%s,\"game\":%s,\"platform\":%s}\n",
               json::quote(CUBIC_VERSION).c_str(), json::quote(d["worker"].str()).c_str(),
               json::quote(d["protocol"].str()).c_str(), json::dump(d["game"]).c_str(), json::quote(d["platform"].str()).c_str());
        return kOk;
    }
    say(a, std::string("CubicHeroes ") + CUBIC_VERSION);
    if (r.started && r.result["ok"].truthy())
        say(a, "worker " + d["worker"].str("?") + " (protocol " + d["protocol"].str("?") + ")" +
                   (d["platform"].str().empty() ? "" : ", " + d["platform"].str()));
    else say(a, "worker: not found beside CubicHeroes");
    return kOk;
}

static bool installed(const json::Value& info) { return info["data"]["kind"].str() == "slots"; }

static int cmd_status(const Args& a, bool with_update)
{
    std::string why;
    const bool writable = root_writable(why, false);   // a look: nothing made
    const Run info = worker(a, "info", install_arg(), false);
    if (!info.started) return finish(a, info, "status");
    const bool inst = installed(info.result);
    Run builds, prep, settings, update;
    if (inst) {
        builds = worker(a, "builds", install_arg(), false);
        prep = worker(a, "prepare", { "--install", g_root, "--check" }, false);
        settings = worker(a, "settings", install_arg(), false);
    }
    if (with_update) update = worker(a, "check-update", { "--channel", update_channel(a) }, false);
    const Run lstate = worker(a, "launcher-state", launcher_update_args(), false);   // beta.4
    const json::Value& L = lstate.result["data"];
    const std::string exe = inst ? game_exe(g_root) : std::string();
    if (a.json) {
        std::string o = "{\"ok\":true,\"cubicheroes\":" + json::quote(CUBIC_VERSION) + ",\"install_dir\":" + json::quote(g_root) +
                        ",\"install_dir_from\":" + json::quote(g_root_from) + ",\"writable\":" + (writable ? "true" : "false") +
                        ",\"installed\":" + (inst ? "true" : "false") + ",\"game\":" + (exe.empty() ? "null" : json::quote(exe)) +
                        ",\"install\":" + json::dump(info.result["data"]);
        if (inst) {
            o += ",\"builds\":" + json::dump(builds.result["data"]) + ",\"shaders\":" + json::dump(prep.result["ok"].truthy()
                     ? prep.result["data"] : prep.result) + ",\"settings\":" + json::dump(settings.result["data"]);
        }
        if (with_update) o += ",\"update\":" + json::dump(update.result);
        o += ",\"launcher_update\":" + json::dump(L);
        printf("%s}\n", o.c_str());
        return kOk;
    }
    say(a, std::string("Cubic.Heroes ") + CUBIC_VERSION);
    say(a, "Installation folder: " + g_root + "  (" + g_root_from + ")" + (writable ? "" : "  -- can't be written: " + why));
    const std::string kind = info.result["data"]["kind"].str("?");
    say(a, std::string("Installed: ") + (inst ? "yes" : kind == "none" ? "no" : kind == "partial" ? "no (an install was "
            "interrupted: --install resumes it)" : kind == "setup-app" ? "an older setup app's install (open the launcher "
            "to move it)" : kind));
    if (inst) {
        const json::Value& b = builds.result["data"];
        std::string active = b["active"].str("-"), version = "-";
        for (size_t i = 0; i < b["builds"].size(); i++)
            if (b["builds"][i]["state"].str() == "active") version = b["builds"][i]["version"].str("-");
        const size_t kept = b["previous"].size();
        say(a, "Active build: " + active + (version == "-" ? "" : " (version " + version + ")") +
                   (kept ? ", " + std::to_string(kept) + " earlier kept for rollback" : ""));
        say(a, "Game program: " + exe);
        if (prep.result["ok"].truthy()) {
            const json::Value& p = prep.result["data"];
            const std::string cache = p["cache"].str();   // "reused": valid for this GPU and driver; else why not
            const bool all = p["comprehensive_done"].truthy();
            say(a, cache == "reused" && all ? "Shaders: prepared (" + p["coverage"]["pipelines"].str("?") + " pipelines)"
                   : cache == "reused" ? "Shaders: partly prepared (--prepare-shaders completes them)"
                   : "Shaders: not prepared for this GPU and driver" + (cache.empty() ? "" : " (" + cache + ")") +
                     " -- --prepare-shaders");
        } else {
            say(a, "Shaders: " + prep.result["message"].str("unknown"));
        }
    }
    if (!L["ready"].is_null())
        say(a, "Launcher update: version " + L["ready"]["version"].str() + " is downloaded and ready (--apply-update)");
    if (!L["rollback"].is_null())
        say(a, "Kept for the launcher's rollback: version " + L["rollback"]["version"].str() + " (--rollback-launcher)");
    if (!L["last"].is_null())
        say(a, "Last launcher update: " + L["last"]["result"].str() + (L["last"]["version"].str().empty() ? "" :
            " (version " + L["last"]["version"].str() + ")") + (L["last"]["why"].str().empty() ? "" : " -- " +
            L["last"]["why"].str()));
    if (L["interrupted"].truthy()) say(a, "Launcher update: interrupted; the next start puts it right");
    if (with_update) {
        if (update.result["ok"].truthy()) {
            const json::Value& d = update.result["data"];
            say(a, d["version"].str().empty() ? "Update: none available" : "Update: version " + d["version"].str() +
                                                                            " is available");
        } else {
            say(a, "Update: can't check -- " + update.result["message"].str("no answer"));
        }
    }
    return kOk;
}

static int cmd_set(const Args& a)
{
    std::vector<std::string> args = install_arg();
    for (const std::string& kv : a.sets) {
        if (kv.find('=') == std::string::npos || kv.front() == '=')
            return fail(a, kUsage, "usage", "--set takes KEY=VALUE, not " + kv);
        args.push_back("--set");
        args.push_back(kv);
    }
    const Run r = worker(a, "settings", args, false);
    const int code = finish(a, r, "Changing the settings");
    if (code != kOk) return code;
    if (a.json) printf("{\"ok\":true,\"settings\":%s}\n", json::dump(r.result["data"]).c_str());
    else for (const std::string& kv : a.sets) say(a, "Set " + kv);
    return kOk;
}

static int cmd_install(const Args& a)
{
    const Run info = worker(a, "info", install_arg(), false);
    if (!info.started) return finish(a, info, "install");
    const std::string kind = info.result["data"]["kind"].str();
    if (kind == "slots" && !a.repair) {
        if (a.json) printf("{\"ok\":true,\"already_installed\":true,\"install_dir\":%s}\n", json::quote(g_root).c_str());
        else say(a, "Already installed in " + g_root + " (nothing done; --repair rebuilds it from the disc).");
        return kOk;
    }
    if (kind == "setup-app")
        return fail(a, kNotInstalled, "needs_migration", "This folder holds an older setup app's install: open the "
                                                          "launcher to move it into the current layout.");
    if (a.disc.empty())
        return fail(a, kUsage, "usage", "--install needs --disc PATH (and --keys PATH, unless the keys are beside the "
                                        "disc or were found before).");
    std::vector<std::string> args = { "--disc", a.disc, "--install", g_root };
    g_prefs.load();   // the window's remembered key source and RPCS3 (only their paths), read here
    if (!a.keys.empty()) { args.push_back("--keys"); args.push_back(a.keys); }   // beta.6: else found beside the disc
    if (!g_prefs.keys.empty() && g_prefs.keys != a.keys) { args.push_back("--key-source"); args.push_back(g_prefs.keys); }
    const std::string rp = !a.rpcs3.empty() ? a.rpcs3 : g_prefs.rpcs3;
    if (!rp.empty()) { args.push_back("--rpcs3"); args.push_back(rp); }
    if (!a.disc_key.empty()) { args.push_back("--disc-key"); args.push_back(a.disc_key); }   // an encrypted ISO
    if (kind == "partial") args.push_back("--resume");
    say(a, "Installing into " + g_root + " (this takes a while)...");
    const Run r = worker(a, "install", args, true);
    const int code = finish(a, r, "The install");
    if (code == kOk && !a.json) say(a, "Installed. Play with: CubicHeroes --play");
    return code;
}

static int cmd_update(const Args& a)
{
    const Run info = worker(a, "info", install_arg(), false);
    if (!info.started) return finish(a, info, "update");
    if (!installed(info.result)) return fail(a, kNotInstalled, "no_install", "Nothing is installed in " + g_root + ".");
    say(a, "Bringing the game up to date with Cubic.Heroes " CUBIC_VERSION "...");
    const Run r = worker(a, "update", install_arg(), true);
    const int code = finish(a, r, "The update");
    if (code == kOk) write_text(game_update_marker(), CUBIC_VERSION "\n");   // the window won't do it again
    if (code == kOk && !a.json)
        say(a, r.result["data"]["changed"].truthy() ? "Updated: build " + r.result["data"]["build"].str() + " is active."
                                                    : "Already up to date: nothing rebuilt.");
    return code;
}

// the installation's update channel (its setting; a beta's default: test releases too), as the window uses it
static std::string update_channel(const Args& a)
{
    const Run s = worker(a, "settings", install_arg(), false);
    const std::string c = s.result["data"]["settings"]["LAUNCHER_UPDATE_CHANNEL"]["value"].str();
    if (c.empty()) return default_update_channel();   // no settings to read: the build's own default, as the window
    return c == "prerelease" ? c : std::string("stable");
}


// 0.2.0-beta.5: an installation RecompLauncher manages (updates_managed_elsewhere(), main.cpp) gets its updates from
// RecompLauncher; this launcher's own updater refuses, so the two never change the same folder (exit 8)
static int refuse_managed(const Args& a)
{
    return fail(a, kManaged, "managed_elsewhere", "RecompLauncher installs this installation's updates, so this "
                "launcher's own updater is off (the two must never change the same folder). Update it from "
                "RecompLauncher; or, to manage it here instead, delete " + std::string(kRecompMark) +
                " from the installation folder (outside RecompLauncher).");
}

static int cmd_check_update(const Args& a)
{
    if (updates_managed_elsewhere()) return refuse_managed(a);
    const Run r = worker(a, "check-update", { "--channel", update_channel(a) }, false);
    const int code = finish(a, r, "The update check");
    if (code != kOk) return code;
    const json::Value& d = r.result["data"];
    if (a.json) printf("{\"ok\":true,\"update\":%s}\n", json::dump(d).c_str());
    else {
        say(a, d["version"].str().empty() ? "No newer release." : "Version " + d["version"].str() + " is available.");
        for (size_t i = 0; i < d["refused"].size(); i++)
            say(a, "Refused: " + d["refused"][i]["tag"].str("?") + " -- " + d["refused"][i]["why"].str());
    }
    return kOk;
}

// beta.4: the launcher's own update, from the command line -- the same worker commands as the window's. This
// process exits at once; the helper then swaps the launcher's files, checks the new one (its --version) and keeps
// it or puts this one back. Nothing is started afterwards (no window).
static int cmd_apply_update(const Args& a)
{
    if (updates_managed_elsewhere()) return refuse_managed(a);
    const std::string channel = update_channel(a);
    const Run c = worker(a, "check-update", { "--channel", channel }, false);
    int code = finish(a, c, "The update check");
    if (code != kOk) return code;
    const json::Value& d = c.result["data"];
    if (d["offline"].truthy()) return fail(a, kFailed, "offline", d["why"].str("No connection to the update server."));
    if (!d["available"].truthy() && d["refused"].size() > 0) {   // a release that doesn't verify: never installed
        const json::Value& r0 = d["refused"][(size_t)0];
        return fail(a, kFailed, r0["code"].str("refused"), "The newest release (" + r0["tag"].str("?") + ") was refused: " +
                    r0["why"].str("its signature isn't valid") + ". Nothing was installed.");
    }
    if (!d["available"].truthy() || !d["newer"].truthy()) {
        if (a.json) printf("{\"ok\":true,\"installing\":false,\"version\":%s}\n", json::quote(CUBIC_VERSION).c_str());
        else say(a, std::string("Up to date: version ") + CUBIC_VERSION + ".");
        return kOk;
    }
    const std::string v = d["version"].str();
    say(a, "Downloading version " + v + "...");
    code = finish(a, worker(a, "download-update", { "--install", g_root, "--channel", channel, "--version", v }, true),
                  "The download");
    if (code != kOk) return code;
    std::vector<std::string> args = launcher_update_args();
    for (const char* x : { "--version", v.c_str() }) args.push_back(x);
    args.push_back("--wait-pid");
    args.push_back(std::to_string(this_pid()));
    args.push_back("--no-restart");
    const Run r = worker(a, "apply-update", args, true);
    code = finish(a, r, "Installing the update");
    if (code != kOk) return code;
    if (a.json)
        printf("{\"ok\":true,\"installing\":true,\"version\":%s,\"from\":%s}\n", json::quote(v).c_str(),
               json::quote(CUBIC_VERSION).c_str());
    else
        say(a, "Installing version " + v + ": CubicHeroes exits now and the update finishes in a few seconds. Then "
               "--version shows it, --status says how it ended, and --update brings the game up to date.");
    return kOk;
}

static int cmd_rollback_launcher(const Args& a)
{
    if (updates_managed_elsewhere()) return refuse_managed(a);
    std::vector<std::string> args = launcher_update_args();
    args.push_back("--wait-pid");
    args.push_back(std::to_string(this_pid()));
    args.push_back("--no-restart");
    const Run r = worker(a, "rollback-launcher", args, true);
    const int code = finish(a, r, "Going back to the previous launcher");
    if (code != kOk) return code;
    const std::string v = r.result["data"]["version"].str();
    if (a.json) printf("{\"ok\":true,\"installing\":true,\"version\":%s}\n", json::quote(v).c_str());
    else say(a, "Putting back version " + v + ": CubicHeroes exits now and it is back in a few seconds.");
    return kOk;
}

static int cmd_prepare(const Args& a, bool rebuild)
{
    const Run info = worker(a, "info", install_arg(), false);
    if (!info.started) return finish(a, info, "prepare");
    if (!installed(info.result)) return fail(a, kNotInstalled, "no_install", "Nothing is installed in " + g_root + ".");
    std::vector<std::string> args = install_arg();
    if (rebuild) args.push_back("--rebuild");
    say(a, rebuild ? "Preparing the shaders again from scratch..." : "Preparing the shaders...");
    const Run r = worker(a, "prepare", args, true);
    const int code = finish(a, r, "The shader preparation");
    if (code == kOk && !a.json) say(a, "Shaders prepared.");
    return code;
}

static int cmd_migrate(const Args& a)
{
    say(a, "Copying the installation in " + a.from + " into " + g_root + " (the original stays as it is)...");
    const Run r = worker(a, "migrate-root", { "--from", a.from, "--install", g_root }, true);
    const int code = finish(a, r, "The copy");
    if (code == kOk && !a.json) say(a, "Copied. This folder is now a complete installation.");
    return code;
}

// the settings a session's options change: the game reads its settings file, and a variable already set wins
static std::vector<std::pair<std::string, std::string>> session_settings(const Args& a)
{
    std::vector<std::pair<std::string, std::string>> v;
    if (!a.display.empty()) v.push_back({ "DOTHEROES_DISPLAY_MODE", a.display });
    if (!a.vsync.empty()) v.push_back({ "DOTHEROES_VSYNC", a.vsync });
    if (!a.resolution.empty()) {   // M11.2f: a preset by its name, any other size as the custom one
        static const char* presets[] = { "native", "1280x720", "1920x1080", "2560x1440", "3840x2160" };
        bool is_preset = false;
        for (const char* p : presets) is_preset = is_preset || a.resolution == p;
        if (is_preset) v.push_back({ "DOTHEROES_RENDER_RES", a.resolution });
        else { v.push_back({ "DOTHEROES_RENDER_RES", "custom" }); v.push_back({ "DOTHEROES_RENDER_RES_CUSTOM", a.resolution }); }
    }
    if (!a.fps.empty()) {
        static const char* listed[] = { "30", "60", "75", "100", "120", "144", "165", "240", "display", "0" };
        bool is_listed = false;
        for (const char* l : listed) is_listed = is_listed || a.fps == l;
        if (is_listed) v.push_back({ "DOTHEROES_FPS", a.fps });
        else { v.push_back({ "DOTHEROES_FPS", "custom" }); v.push_back({ "DOTHEROES_FPS_CUSTOM", a.fps }); }
    }
    for (const std::string& kv : a.sets) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0) v.push_back({ kv.substr(0, eq), kv.substr(eq + 1) });
    }
    return v;
}

// --play's end: the game's status (passed on as CubicHeroes's own); a status that means something to a player
// (game_exit.h: 70, the graphics' start never finished) explained on stderr, and in the JSON event's "why"
static void play_ended(const Args& a, int status)
{
    const launcher::GameExitNote* n = launcher::game_exit_note(status);
    if (n) {
        fprintf(stderr, "CubicHeroes: %s (status %d). %s\n", n->title, status, n->text);
        fflush(stderr);
    }
    if (a.json)
        printf("{\"ev\":\"ended\",\"exit\":%d%s}\n", status,
               n ? (std::string(",\"why\":") + json::quote(std::string(n->title) + ": " + n->text)).c_str() : "");
}

static int cmd_play(const Args& a)
{
    std::error_code ec;
    const bool installed_here = fs::exists(u8path(g_root) / "active.json", ec) || fs::exists(u8path(g_root) / "game", ec);
    // --or-launcher (other launchers; RecompLauncher's start): the window instead, when the game isn't installed yet
    // (our setup: the disc and the keys, which no other launcher can pass) or isn't yet brought up to date with this
    // version (the window does it at its start, with its progress, then the player plays from it)
    if (a.or_launcher && (!installed_here || trimmed(read_text(game_update_marker(), 256)) != CUBIC_VERSION))
        return kOpenLauncher;
    if (!installed_here)
        return fail(a, kNotInstalled, "no_install", "Nothing is installed in " + g_root +
                                                    ". Install with: CubicHeroes --install --disc PATH [--keys PATH]");
    const std::string exe = game_exe(g_root);
    if (!fs::is_regular_file(u8path(exe), ec))
        return fail(a, kPrerequisite, "no_game", "The game's program is missing (" + exe + "). Repair the installation: "
                                                 "CubicHeroes --update, or the launcher's Install page.");
    const auto session = session_settings(a);
    if (a.persist && (!session.empty())) {   // kept as settings first (checked against the schema by the worker)
        std::vector<std::string> args = install_arg();
        for (const auto& kv : session) { args.push_back("--set"); args.push_back(kv.first + "=" + kv.second); }
        const int code = finish(a, worker(a, "settings", args, false), "Keeping the settings");
        if (code != kOk) return code;
    }
    for (const auto& kv : session) {   // this session's values: a variable already set wins over the settings file
#ifdef _WIN32
        _wputenv_s(launcher::widen(kv.first).c_str(), launcher::widen(kv.second).c_str());
#else
        setenv(kv.first.c_str(), kv.second.c_str(), 1);
#endif
    }
    if (a.width && a.height) {   // the window's size for this session (the game keeps its remembered place untouched)
        const std::string sz = std::to_string(a.width) + "x" + std::to_string(a.height);
#ifdef _WIN32
        _putenv_s("DOTHEROES_WINDOW_SIZE", sz.c_str());
#else
        setenv("DOTHEROES_WINDOW_SIZE", sz.c_str(), 1);
#endif
    }
    // direct play: no launcher to return to (the in-game menu offers Exit Game; QUIT GAME just quits)
    {
        std::error_code e2;
        fs::remove(u8path(g_root) / "logs" / "session-end", e2);
    }
#ifdef _WIN32
    _putenv_s("CUBIC_DIRECT_PLAY", "1");
    _putenv_s("CUBIC_LAUNCHER", "");
    _putenv_s("CUBIC_LAUNCHER_PID", "");
    // the game in a job of ours: a calling launcher (Steam, a frontend) that ends CubicHeroes ends the game with it
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = launcher::quote_arg(launcher::widen(exe));
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        launcher::widen(g_root).c_str(), &si, &pi)) {
        if (job) CloseHandle(job);
        return fail(a, kPrerequisite, "no_game", "The game couldn't be started (error " + std::to_string(GetLastError()) + ").");
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (a.json) { printf("{\"ev\":\"started\",\"pid\":%lu,\"game\":%s}\n", (unsigned long)pi.dwProcessId, json::quote(exe).c_str()); fflush(stdout); }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(pi.hProcess, &status);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    play_ended(a, (int)status);
    return (int)status;
#else
    setenv("CUBIC_DIRECT_PLAY", "1", 1);
    unsetenv("CUBIC_LAUNCHER");
    unsetenv("CUBIC_LAUNCHER_PID");
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) {
        // the game ends with CubicHeroes, as the job object makes it on Windows: a calling launcher (Steam, a
        // frontend) that kills CubicHeroes outright ends the game too (and if it is already gone, at once)
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(kCancelled);
        if (chdir(g_root.c_str()) != 0) _exit(127);
        execl(exe.c_str(), exe.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (pid < 0) return fail(a, kPrerequisite, "no_game", "The game couldn't be started.");
    if (a.json) { printf("{\"ev\":\"started\",\"pid\":%d,\"game\":%s}\n", (int)pid, json::quote(exe).c_str()); fflush(stdout); }
    // SIGTERM or SIGINT to CubicHeroes (a launcher's stop, Ctrl+C) passed on to the game once; then the game's own
    // orderly end is waited for. Polled: the handlers are signal()'s (SA_RESTART), so a blocking waitpid would
    // never see them (M11.4)
    int st = 0;
    bool forwarded = false;
    for (;;) {
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) break;
        if (g_cancel && !forwarded) { kill(pid, SIGTERM); forwarded = true; }
        usleep(50 * 1000);
    }
    const int status = forwarded ? kCancelled
                                 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    play_ended(a, status);
    return status;
#endif
}

// the command line's own run (not the window); its exit code
static int run(const Args& a)
{
    attach_console();
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif
    if (!a.error.empty()) {
        if (a.json) return fail(a, kUsage, "usage", a.error);
        fprintf(stderr, "CubicHeroes: %s\n", a.error.c_str());
        return kUsage;
    }
    if (a.command == "help") { fputs(kUsageText, stdout); return kOk; }
    if (a.command == "version") return cmd_version(a);
    if (a.remember && !a.install_dir.empty() && !remember_root(g_root))
        return fail(a, kRoot, "root_not_writable", "The folder CubicHeroes is in can't be written, so --remember can't "
                                                   "keep the installation folder (pass --install-dir each time).");
    std::string why;
    const bool reads_only = a.command == "status" || a.command == "check-update" || a.command == "play";
    if (!reads_only && !root_writable(why))
        return fail(a, kRoot, "root_not_writable", "The installation folder " + g_root + " can't be used: " + why +
                                                   ". Choose another with --install-dir PATH.");
    if (a.command == "status") return cmd_status(a, false);
    if (a.command == "check-update") return cmd_check_update(a);
    if (a.command == "apply-update") return cmd_apply_update(a);
    if (a.command == "rollback-launcher") return cmd_rollback_launcher(a);
    if (a.command == "set") return cmd_set(a);
    if (a.command == "install") return cmd_install(a);
    if (a.command == "update") return cmd_update(a);
    if (a.command == "prepare-shaders") return cmd_prepare(a, false);
    if (a.command == "rebuild-shaders") return cmd_prepare(a, true);
    if (a.command == "migrate-from") return cmd_migrate(a);
    if (a.command == "play") return cmd_play(a);
    if (a.persist) {   // --persist alone with display options: kept as settings
        std::vector<std::string> args = install_arg();
        for (const auto& kv : session_settings(a)) { args.push_back("--set"); args.push_back(kv.first + "=" + kv.second); }
        return finish(a, worker(a, "settings", args, false), "Keeping the settings");
    }
    return fail(a, kUsage, "usage", "nothing to do (--help lists the commands)");
}

}  // namespace cli
