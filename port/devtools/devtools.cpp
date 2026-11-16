/* M11.0 (docs/M11_PLAN.md sections 3.3-3.5): the developer tools' core.
 *
 * The command channel: commands are text lines, from the in-game menu (devtools_submit) or from a test (dev.cmd in the
 * game's working directory: read and removed every 50 ms; a line may start with "@ID " to name its answer), queued and
 * executed on the game's main thread -- the thread that runs every Lua script -- at the entry of its flip
 * (0x0069DD60: after the frame's update, no script running) or, while the in-game menu holds the game, from the
 * runtime's hold (g_ps3_guest_hold_work, called where that same thread waits). Each answer is one JSON line,
 * {"id":..,"ok":..,"out":..}, appended to dev.out and written to the log ("[dev]").
 *
 * The Lua runner (section 3.4): the game's one lua_State (*0x0159C08C), stock Lua 5.1.3; a chunk is loaded with
 * luaL_loadbuffer and run with lua_pcall through the lift's function table, on a guest stack region of the tools' own
 * (the main thread's is 64 KB and Lua's parser recurses), only while the engine's biLua is idle (its call level -1);
 * the stack is restored afterwards. Never lua_call (an error there reaches Lua's panic and ends the game) and never
 * the engine's biLua helpers (a failure there sets a sticky error that silences every later script call).
 *
 * Off (DOTHEROES_DEVELOPER_MODE not 1): nothing is set up, no file is read, no hook is installed.
 */
#include "devtools.h"

#include "ppu_context.h"
#include "ps3emu/guest_pause.h"
#include "ps3emu/osd.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// port/diag/game_log.h, in the game's executable (weak here, as ppfx_render.cpp declares it)
extern "C" void dh_log(int level, const char* tag, const char* fmt, ...) __attribute__((weak, format(printf, 3, 4)));
enum { DH_LOG_ERROR = 0, DH_LOG_WARNING = 1, DH_LOG_INFO = 2 };
#define DLOG(...) do { if (dh_log) dh_log(__VA_ARGS__); } while (0)

extern "C" uint8_t* vm_base;
extern "C" int64_t sys_memory_allocate(ppu_context* ctx);
struct DevFnEntry { uint64_t addr; void (*func)(ppu_context*); const char* name; };   /* the lift's func_entry */
extern "C" const DevFnEntry function_table[];
extern "C" const uint64_t function_table_count;
extern "C" __thread void (*g_trampoline_fn)(void*);   /* the lift's (PPU_THREAD_LOCAL) */

void (*g_devtools_flip)(struct ppu_context* ctx) = nullptr;

namespace {

/* ---- the game (BLUS-30490), docs/M11_PLAN.md section 3.4 [C] ---- */
constexpr uint32_t kLuaToc = 0x010764C4;        /* the Lua library's module TOC */
constexpr uint32_t kLuaStatePtr = 0x0159C08C;   /* biLua (0x0159BE98) + 0x1F4: the lua_State */
constexpr uint32_t kBiLuaLevel = 0x0159BFE8;    /* biLua + 0x150: its call level, -1 when idle */
constexpr uint32_t kEventVar = 0x02AE1568;      /* EventClass.Event, the scripts' `Event` */
constexpr uint32_t kGameManager = 0x02AE8704;   /* the game manager; its EventClass at +0x8F0 */
constexpr uint32_t kArea = 0x029A6488;          /* the map's number (dev_state.cpp; the map's MS_ order) */
constexpr uint32_t kFrames = 0x02CF3570;        /* the guest's frame counter (dev_state.cpp) */
constexpr uint32_t kHudLife = 0x02B38E0C;       /* the HUD's copy of the hero's life (dev_state.cpp) */
constexpr uint32_t fn_gettop = 0x007F02EC, fn_settop = 0x007F033C, fn_type = 0x007F09DC, fn_toboolean = 0x007F1250,
                   fn_tolstring = 0x007F130C, fn_pcall = 0x007F3450, fn_loadbuffer = 0x0081B060;

/* the tools' guest memory: a stack for their calls and a buffer for a chunk */
constexpr uint32_t kRegionBytes = 0x60000;       /* six 64 KB pages */
constexpr uint32_t kStackBytes = 0x40000;        /* at the region's top */
constexpr uint32_t kBufBytes = 0xF000;           /* a chunk's text, at the region's base */
constexpr uint32_t kNameOff = 0xF800;            /* the chunk's name */

uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void w32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
std::string gstr(uint32_t ea, size_t max = 4096)
{
    if (!ea) return std::string();
    const char* s = (const char*)(vm_base + ea);
    return std::string(s, strnlen(s, max));
}

bool g_on = false;
uint32_t g_region = 0;                  /* the guest region (0: not yet) */
bool g_region_failed = false;
std::atomic<uint64_t> g_flip_thread{ 0 };   /* the thread seen at the flip (std::hash of its id) */
std::atomic<ppu_context*> g_flip_ctx{ nullptr };
std::atomic<int> g_hold_runs{ 0 }, g_hold_other_thread{ 0 };
bool g_in_run = false;                  /* re-entry guard (a command never runs inside another) */

uint64_t this_thread_key() { return (uint64_t)std::hash<std::thread::id>()(std::this_thread::get_id()); }

/* ---- calling the game's functions ---- */
using Fn = void (*)(ppu_context*);
Fn lookup(uint32_t addr)
{
    static std::unordered_map<uint32_t, Fn>* m = nullptr;
    if (!m) {
        m = new std::unordered_map<uint32_t, Fn>;
        for (uint64_t i = 0; i < function_table_count; i++) (*m)[(uint32_t)function_table[i].addr] = function_table[i].func;
    }
    auto f = m->find(addr);
    return f == m->end() ? nullptr : f->second;
}

struct CallError { const char* what; };

/* the function at `addr` called as the game would (r2 its module's TOC, r3.. the arguments) on the tools' own stack;
 * its r3 returned */
uint64_t call(const ppu_context* base, uint32_t addr, uint32_t toc, std::initializer_list<uint64_t> args)
{
    const Fn f = lookup(addr);
    if (!f) throw CallError{ "a game function is missing from this build" };
    ppu_context g = *base;
    const uint32_t top = (g_region + kRegionBytes - 0x200) & ~15u;
    memset(vm_base + top, 0, 0x40);   /* a null back chain */
    g.gpr[1] = top;
    g.gpr[2] = toc;
    int r = 3;
    for (uint64_t a : args) g.gpr[r++] = a;
    g.lr = 0;
    void (*const pending)(void*) = g_trampoline_fn;   /* the interrupted guest code's own (none expected): kept */
    g_trampoline_fn = nullptr;
    f(&g);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&g); }
    g_trampoline_fn = pending;
    return g.gpr[3];
}
uint64_t sx(int64_t v) { return (uint64_t)v; }

/* the region, from the runtime's user memory (its address through a word below the caller's stack pointer) */
bool ensure_region(ppu_context* c)
{
    if (g_region) return true;
    if (g_region_failed) return false;
    ppu_context tmp{};
    const uint32_t out = (uint32_t)c->gpr[1] - 0x200;
    tmp.gpr[3] = kRegionBytes;
    tmp.gpr[4] = 0x200;   /* SYS_MEMORY_PAGE_SIZE_64K */
    tmp.gpr[5] = out;
    w32(out, 0);
    if (sys_memory_allocate(&tmp) != 0 || !g32(out)) {
        g_region_failed = true;
        DLOG(DH_LOG_ERROR, "dev", "no guest memory for the developer tools (%u KB): they stay off", kRegionBytes >> 10);
        return false;
    }
    g_region = g32(out);
    DLOG(DH_LOG_INFO, "dev", "the tools' guest memory at 0x%08X (%u KB)", g_region, kRegionBytes >> 10);
    return true;
}

/* ---- Lua ---- */
const char* kTypeName[] = { "nil", "boolean", "lightuserdata", "number", "string", "table", "function", "userdata", "thread" };

std::string lua_value(const ppu_context* c, uint32_t L, int idx)
{
    const int t = (int)(int32_t)call(c, fn_type, kLuaToc, { L, sx(idx) });
    if (t == 0) return "nil";
    if (t == 1) return call(c, fn_toboolean, kLuaToc, { L, sx(idx) }) ? "true" : "false";
    if (t == 3 || t == 4) return gstr((uint32_t)call(c, fn_tolstring, kLuaToc, { L, sx(idx), 0 }), 60000);
    return t > 0 && t < 9 ? std::string("<") + kTypeName[t] + ">" : "<none>";
}

/* 1 ok (results tab-separated in `out`), 0 the chunk failed (its message in `out`), -1 not run (why in `out`) */
int run_lua(const ppu_context* c, const std::string& chunk, std::string& out)
{
    const uint32_t L = g32(kLuaStatePtr);
    if (!L) { out = "the game's scripts are not loaded yet"; return -1; }
    if ((int32_t)g32(kBiLuaLevel) != -1) { out = "a game script is running"; return -1; }
    if (chunk.size() >= kBufBytes) { out = "the chunk is too long"; return -1; }
    memcpy(vm_base + g_region, chunk.data(), chunk.size());
    vm_base[g_region + chunk.size()] = 0;
    static const char kName[] = "=devtool";
    memcpy(vm_base + g_region + kNameOff, kName, sizeof kName);
    const int top = (int)(int32_t)call(c, fn_gettop, kLuaToc, { L });
    int st = (int)(int32_t)call(c, fn_loadbuffer, kLuaToc, { L, g_region, chunk.size(), g_region + kNameOff });
    if (st == 0) st = (int)(int32_t)call(c, fn_pcall, kLuaToc, { L, 0, sx(-1), 0 });
    const int now = (int)(int32_t)call(c, fn_gettop, kLuaToc, { L });
    out.clear();
    if (st != 0) out = now > top ? lua_value(c, L, -1) : "an error with no message";
    else
        for (int i = top + 1; i <= now; i++) out += (i > top + 1 ? "\t" : "") + lua_value(c, L, i);
    call(c, fn_settop, kLuaToc, { L, sx(top) });
    return st == 0 ? 1 : 0;
}

/* ---- the commands ---- */
struct Cmd {
    uint64_t n;
    std::string id, line;
    devtools_reply done;
    void* user;
    bool from_file;
};
std::mutex g_mu;
std::deque<Cmd> g_queue;
std::atomic<uint64_t> g_next{ 1 };
std::mutex g_out_mu;

std::string json_str(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
        else if (ch == '\n') o += "\\n";
        else if (ch == '\t') o += "\\t";
        else if (ch == '\r') o += "\\r";
        else if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o += (char)ch;
    }
    return o + "\"";
}

void answer(const Cmd& c, bool ok, const std::string& out, bool json_out = false)
{
    DLOG(ok ? DH_LOG_INFO : DH_LOG_WARNING, "dev", "%s -> %s%s", c.line.c_str(), ok ? "" : "refused: ",
           out.size() > 400 ? (out.substr(0, 400) + "...").c_str() : out.c_str());
    if (c.from_file) {
        std::lock_guard<std::mutex> g(g_out_mu);
        if (FILE* f = fopen("dev.out", "ab")) {
            fprintf(f, "{\"id\":%s,\"ok\":%s,\"out\":%s}\n", json_str(c.id).c_str(), ok ? "true" : "false",
                    json_out ? out.c_str() : json_str(out).c_str());
            fclose(f);
        }
    }
    if (c.done) c.done(c.user, ok ? 1 : 0, out.c_str());
}

std::string word(std::string& rest)
{
    size_t a = rest.find_first_not_of(" \t");
    if (a == std::string::npos) { rest.clear(); return std::string(); }
    size_t b = rest.find_first_of(" \t", a);
    std::string w = rest.substr(a, b == std::string::npos ? std::string::npos : b - a);
    rest = b == std::string::npos ? std::string() : rest.substr(b);
    return w;
}

/* the event object the scripts use, and the game manager's own (section 3.4's open point) */
uint32_t event_object() { return g32(kEventVar); }
uint32_t manager_event() { const uint32_t gm = g32(kGameManager); return gm ? gm + 0x8F0 : 0; }

std::string status_json(const ppu_context* c)
{
    const uint32_t ev = manager_event();
    std::string money = "null";
    std::string out;
    if (run_lua(c, "local Event=EventClass.Event; return Event:GetMoney()", out) == 1) money = out;
    char b[512];
    snprintf(b, sizeof b,
             "{\"area\":%u,\"frames\":%u,\"hud_life\":%u,\"money\":%s,\"lua_state\":\"0x%08X\",\"lua_level\":%d,"
             "\"event_state\":%u,\"event_object\":\"0x%08X\",\"manager_event\":\"0x%08X\",\"event_same\":%d,"
             "\"held\":%d,\"hold_runs\":%d}",
             g32(kArea), g32(kFrames), g32(kHudLife), money.c_str(), g32(kLuaStatePtr), (int32_t)g32(kBiLuaLevel),
             ev ? g32(ev + 0x20) : 0u, event_object(), ev, event_object() && event_object() == ev ? 1 : 0,
             ps3_guest_paused(), g_hold_runs.load());
    return b;
}

/* true when the command changed the game (the next one waits for the next frame) */
bool execute(const ppu_context* c, const Cmd& cmd)
{
    std::string rest = cmd.line;
    const std::string w = word(rest);
    try {
        if (w == "ping") { answer(cmd, true, "pong"); return false; }
        if (w == "status") { answer(cmd, true, status_json(c), true); return false; }
        if (w == "lua") {
            std::string out;
            const size_t a = rest.find_first_not_of(" \t");
            const int r = run_lua(c, a == std::string::npos ? std::string() : rest.substr(a), out);
            answer(cmd, r == 1, r < 0 ? "not run: " + out : out);
            return r == 1;
        }
        if (w == "check") {   /* the design's run-time open points (docs/M11_PLAN.md section 3.4) */
            std::string out, lua;
            const int r = run_lua(c, "return 1+1, type(EventClass), type(EventClass.Event)", lua);
            char b[512];
            snprintf(b, sizeof b,
                     "{\"event_object\":\"0x%08X\",\"manager_event\":\"0x%08X\",\"same\":%s,\"lua\":%s,"
                     "\"hold_runs\":%d,\"hold_other_thread\":%d,\"held_now\":%d}",
                     event_object(), manager_event(), event_object() && event_object() == manager_event() ? "true" : "false",
                     json_str(r == 1 ? lua : "failed: " + lua).c_str(), g_hold_runs.load(), g_hold_other_thread.load(),
                     ps3_guest_paused());
            answer(cmd, true, b, true);
            return false;
        }
        answer(cmd, false, "unknown command: " + w);
    } catch (const CallError& e) {
        answer(cmd, false, e.what);
    }
    return false;
}

/* the queue, on the game's main thread */
void run_queue(ppu_context* c)
{
    if (g_in_run) return;
    if (!ensure_region(c)) {
        std::deque<Cmd> drop;
        { std::lock_guard<std::mutex> g(g_mu); drop.swap(g_queue); }
        for (const Cmd& d : drop) answer(d, false, "the developer tools have no guest memory");
        return;
    }
    g_in_run = true;
    for (int i = 0; i < 64; i++) {
        Cmd cmd;
        {
            std::lock_guard<std::mutex> g(g_mu);
            if (g_queue.empty()) break;
            cmd = std::move(g_queue.front());
            g_queue.pop_front();
        }
        if (execute(c, cmd)) break;   /* a change: the rest wait for the next frame */
    }
    g_in_run = false;
}

void on_flip(ppu_context* c)
{
    g_flip_thread.store(this_thread_key(), std::memory_order_relaxed);
    g_flip_ctx.store(c, std::memory_order_release);
    bool any;
    { std::lock_guard<std::mutex> g(g_mu); any = !g_queue.empty(); }
    if (any) run_queue(c);
}

/* the runtime's hold (the in-game menu open): where the held thread waits; only the main thread runs commands */
void on_hold()
{
    ppu_context* c = g_flip_ctx.load(std::memory_order_acquire);
    if (!c) return;
    if (this_thread_key() != g_flip_thread.load(std::memory_order_relaxed)) { g_hold_other_thread++; return; }
    bool any;
    { std::lock_guard<std::mutex> g(g_mu); any = !g_queue.empty(); }
    if (!any) return;
    g_hold_runs++;
    run_queue(c);
}

void push(Cmd c)
{
    std::lock_guard<std::mutex> g(g_mu);
    g_queue.push_back(std::move(c));
}

/* dev.cmd in the working directory: read and removed every 50 ms (a test writes it whole: tests/devtools.py) */
void file_loop()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        FILE* f = fopen("dev.cmd", "rb");
        if (!f) continue;
        std::string all;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
        fclose(f);
        if (remove("dev.cmd") != 0) continue;   /* still being written: next time */
        size_t a = 0;
        while (a < all.size()) {
            size_t e = all.find('\n', a);
            if (e == std::string::npos) e = all.size();
            std::string line = all.substr(a, e - a);
            a = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            Cmd c{ g_next++, "", line, nullptr, nullptr, true };
            if (line[0] == '@') {
                const size_t sp = line.find(' ');
                c.id = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
                c.line = sp == std::string::npos ? std::string() : line.substr(sp + 1);
            } else {
                c.id = std::to_string(c.n);
            }
            push(std::move(c));
        }
    }
}

}  // namespace

extern "C" int devtools_on(void) { return g_on ? 1 : 0; }

extern "C" uint64_t devtools_submit(const char* line, devtools_reply done, void* user)
{
    if (!g_on) { if (done) done(user, 0, "developer mode is off"); return 0; }
    Cmd c{ g_next++, "", line ? line : "", done, user, false };
    c.id = std::to_string(c.n);
    const uint64_t n = c.n;
    push(std::move(c));
    return n;
}

extern "C" void devtools_start(const char* install_root)
{
    (void)install_root;
    const char* e = getenv("DOTHEROES_DEVELOPER_MODE");
    g_on = e && !strcmp(e, "1");
    if (!g_on) return;
    g_devtools_flip = on_flip;
    g_ps3_guest_hold_work = on_hold;
    ps3_osd_set_tag("DEVELOPER MODE - TEST SAVES");
    std::thread(file_loop).detach();
    DLOG(DH_LOG_INFO, "dev", "developer mode: the tools are on; saves in %s; commands from dev.cmd, answers in dev.out",
           getenv("PS3_GAMEDATA_ROOT") ? getenv("PS3_GAMEDATA_ROOT") : "(the runtime's default)");
}
