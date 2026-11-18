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
#include "devtools_internal.h"

#include "ppu_context.h"
#include "ps3emu/guest_pause.h"
#include "ps3emu/osd.h"
#include "ps3emu/ps3_ftrace.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif   /* the compile log's clock: [warp] lines line up with [vk-compile] (map_sweep.py) */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// port/diag/game_log.h, in the game's executable (weak here, as ppfx_render.cpp declares it)
extern "C" void dh_log(int level, const char* tag, const char* fmt, ...) __attribute__((weak, format(printf, 3, 4)));
enum { DH_LOG_ERROR = 0, DH_LOG_WARNING = 1, DH_LOG_INFO = 2 };
#define DLOG(...) do { if (dh_log) dh_log(__VA_ARGS__); } while (0)

extern "C" uint8_t* vm_base;
extern "C" void (*g_ppfx_unforeseen_hook)(uint64_t key, const char* kind, double ms);   /* ppfx_render.cpp (M11.0f) */
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
/* M11.2d: with developer mode off, QUIT GAME's label is the only call into the game's scripts: one 64 KB page of its
 * own (a stack and the chunk's text), never the tools' region -- the tools stay off (PLAN 14) */
constexpr uint32_t kLabelRegionBytes = 0x10000;
constexpr uint32_t kLabelBufBytes = 0xE00;       /* the label chunk's text (its name just above), the stack above both */
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
uint32_t g_region_bytes = 0;            /* its size: the tools' (kRegionBytes) or the label's alone (kLabelRegionBytes) */
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
    const uint32_t top = (g_region + g_region_bytes - 0x200) & ~15u;
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
bool ensure_region(ppu_context* c, bool label_only = false)
{
    if (g_region) return true;
    if (g_region_failed) return false;
    const uint32_t bytes = label_only ? kLabelRegionBytes : kRegionBytes;
    ppu_context tmp{};
    const uint32_t out = (uint32_t)c->gpr[1] - 0x200;
    tmp.gpr[3] = bytes;
    tmp.gpr[4] = 0x200;   /* SYS_MEMORY_PAGE_SIZE_64K */
    tmp.gpr[5] = out;
    w32(out, 0);
    if (sys_memory_allocate(&tmp) != 0 || !g32(out)) {
        g_region_failed = true;
        if (label_only)
            DLOG(DH_LOG_ERROR, "title", "no guest memory for QUIT GAME's label (%u KB): the item keeps its old name",
                 bytes >> 10);
        else
            DLOG(DH_LOG_ERROR, "dev", "no guest memory for the developer tools (%u KB): they stay off", bytes >> 10);
        return false;
    }
    g_region = g32(out);
    g_region_bytes = bytes;
    if (label_only)
        DLOG(DH_LOG_INFO, "title", "QUIT GAME's label: %u KB of guest memory at 0x%08X for its call into the title's "
             "script", bytes >> 10, g_region);
    else
        DLOG(DH_LOG_INFO, "dev", "the tools' guest memory at 0x%08X (%u KB)", g_region, bytes >> 10);
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
    const bool small = g_region_bytes == kLabelRegionBytes;
    if (chunk.size() >= (small ? kLabelBufBytes : kBufBytes)) { out = "the chunk is too long"; return -1; }
    memcpy(vm_base + g_region, chunk.data(), chunk.size());
    vm_base[g_region + chunk.size()] = 0;
    static const char kName[] = "=devtool";
    const uint32_t name_off = small ? kLabelBufBytes : kNameOff;
    memcpy(vm_base + g_region + name_off, kName, sizeof kName);
    const int top = (int)(int32_t)call(c, fn_gettop, kLuaToc, { L });
    int st = (int)(int32_t)call(c, fn_loadbuffer, kLuaToc, { L, g_region, chunk.size(), g_region + name_off });
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
    std::string map_state = "null";
    if (run_lua(c, "local Event=EventClass.Event; return Event:GetMapState()", out) == 1) map_state = out;
    char b[600];
    snprintf(b, sizeof b,
             "{\"map_state\":%s,\"area\":%u,\"frames\":%u,\"hud_life\":%u,\"money\":%s,\"lua_state\":\"0x%08X\",\"lua_level\":%d,"
             "\"event_state\":%u,\"event_object\":\"0x%08X\",\"manager_event\":\"0x%08X\",\"event_same\":%d,"
             "\"held\":%d,\"hold_runs\":%d}",
             map_state.c_str(), g32(kArea), g32(kFrames), g32(kHudLife), money.c_str(), g32(kLuaStatePtr), (int32_t)g32(kBiLuaLevel),
             ev ? g32(ev + 0x20) : 0u, event_object(), ev, event_object() && event_object() == ev ? 1 : 0,
             ps3_guest_paused(), g_hold_runs.load());
    return b;
}

/* ---- the catalog (docs/M11_PLAN.md section 3.7): made by the game itself, from the player's own files ---- */
std::string g_usrdir;        /* the game's USRDIR on the player's disc (the EBOOT's folder) */
std::string g_install_root;

double ft_clock() { return (double)(int64_t)(ps3_ft_now() - g_ps3_ft.base) / 1e9; }   /* the compile log's clock */

using devtools::MapInfo;
std::vector<MapInfo> g_maps;
bool g_catalog = false;

/* the engine's name for a map's files: the first nine fields' are Field_01..09, the engine says Field_1..9 */
std::string engine_name(const std::string& stem)
{
    if (stem.size() == 8 && stem.compare(0, 7, "Field_0") == 0 && stem[7] >= '1' && stem[7] <= '9')
        return "Field_" + stem.substr(7);
    return stem;
}
/* the engine's name for a map-list constant: MS_FIELD_17 -> Field_17, MS_DUNGEON_1_1_1 -> D_1_1_1,
 * MS_CAPTURE_0001 -> Capture0001; others (the editor, the sample field) have no map of their own */
std::string ms_to_name(const std::string& ms)
{
    if (ms.rfind("MS_FIELD_", 0) == 0) return "Field_" + ms.substr(9);
    if (ms.rfind("MS_DUNGEON_", 0) == 0) return "D_" + ms.substr(11);
    if (ms.rfind("MS_CAPTURE_", 0) == 0) return "Capture" + ms.substr(11);
    return std::string();
}

/* a map's arrival points: its alignment file's arrival records (u32 type 0x1005, u32 0, u32 name length, u32 kind
 * length, the name -- what the map change takes: "-gDebug", "-gfrom_d_1", a door's "-g212;-o212;" -- then the kind:
 * Exit, WarpDoor, PitFall, destructionWall, BlackHole, TombStone, WoodDoor; little-endian) [C: the records read in
 * every map's file]. The default first: the developers' "-gDebug" where the map has one, else the first. */
uint32_t le32(const std::string& d, size_t at)
{
    return (uint32_t)(uint8_t)d[at] | (uint32_t)(uint8_t)d[at + 1] << 8 | (uint32_t)(uint8_t)d[at + 2] << 16 |
           (uint32_t)(uint8_t)d[at + 3] << 24;
}
std::vector<std::string> map_entries(const std::string& file)
{
    std::vector<std::string> v;
    FILE* f = fopen((g_usrdir + "/data/map/map_alignment/" + file + ".dat").c_str(), "rb");
    if (!f) return v;
    std::string d;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.append(buf, n);
    fclose(f);
    for (size_t at = 0; at + 16 < d.size(); at++) {
        if (le32(d, at) != 0x1005 || le32(d, at + 4) != 0) continue;
        const uint32_t la = le32(d, at + 8), lb = le32(d, at + 12);
        if (la < 3 || la > 64 || lb == 0 || lb > 32 || at + 16 + la + lb > d.size()) continue;
        const std::string name = d.substr(at + 16, la);
        if (name.compare(0, 2, "-g") != 0) continue;
        bool printable = true;
        for (char ch : name) printable = printable && ch > 0x20 && ch < 0x7F;
        if (printable && std::find(v.begin(), v.end(), name) == v.end()) v.push_back(name);
    }
    auto dbg = std::find(v.begin(), v.end(), std::string("-gDebug"));
    if (dbg != v.end()) { v.erase(dbg); v.insert(v.begin(), "-gDebug"); }
    return v;
}

/* the group a map is listed under: the fields together, each D_<n>'s maps together */
std::string map_group(const std::string& name)
{
    if (name.rfind("Field_", 0) == 0) return "Fields";
    if (name.rfind("D_", 0) == 0) {
        const size_t u = name.find('_', 2);
        return "D_" + name.substr(2, u == std::string::npos ? std::string::npos : u - 2);
    }
    return "Other";
}

const char kCatalogLua[] = R"(local out = {}
for k, v in pairs(map_manager) do
  if type(k) == "string" and string.sub(k, 1, 3) == "MS_" and type(v) == "number" then out[#out+1] = "M\t" .. k .. "\t" .. v end
end
if type(MapParameter) == "table" then
  for i = 1, (MapParameter.Size or #MapParameter) do
    local p = MapParameter[i]
    if type(p) == "table" and p.Identifier and p.Location then out[#out+1] = "L\t" .. p.Identifier .. "\t" .. p.Location end
  end
end
return table.concat(out, "\n"))";

/* the catalog made (once): 1, or 0 with why in `err` */
int make_catalog(const ppu_context* c, std::string& err)
{
    if (g_catalog) return 1;
    std::string out;
    if (run_lua(c, kCatalogLua, out) != 1) { err = "the game's map list: " + out; return 0; }
    std::unordered_map<std::string, int> number;     /* engine name -> map-list number */
    std::unordered_map<int, std::string> location;
    size_t a = 0;
    while (a < out.size()) {
        size_t e = out.find('\n', a);
        if (e == std::string::npos) e = out.size();
        const std::string ln = out.substr(a, e - a);
        a = e + 1;
        const size_t t1 = ln.find('\t'), t2 = t1 == std::string::npos ? t1 : ln.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        const std::string k = ln.substr(t1 + 1, t2 - t1 - 1), v = ln.substr(t2 + 1);
        if (ln[0] == 'M') { const std::string nm = ms_to_name(k); if (!nm.empty()) number[nm] = atoi(v.c_str()); }
        else if (ln[0] == 'L') location[atoi(k.c_str())] = v;
    }
    if (number.empty()) { err = "the game's map list is empty (its scripts not loaded yet?)"; return 0; }
    std::vector<std::string> stems;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(g_usrdir + "/data/map/pac_data", ec)) {
        const std::string fn = de.path().filename().string();
        const std::string suffix = ".ps3.pac.gz";
        if (fn.size() > suffix.size() && fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0)
            stems.push_back(fn.substr(0, fn.size() - suffix.size()));
    }
    if (stems.empty()) { err = "no map files under " + g_usrdir + "/data/map/pac_data"; return 0; }
    std::sort(stems.begin(), stems.end());
    g_maps.clear();
    for (const std::string& s : stems) {
        MapInfo m;
        m.file = s;
        m.name = engine_name(s);
        auto f = number.find(m.name);
        if (f == number.end()) continue;   /* a file the game's map list lacks: the map change cannot reach it */
        m.number = f->second;
        m.group = map_group(m.name);
        auto l = location.find(m.number);
        if (l != location.end()) m.location = l->second;
        m.entries = map_entries(s);
        g_maps.push_back(std::move(m));
    }
    g_catalog = true;
    DLOG(DH_LOG_INFO, "dev", "the catalog: %zu maps the game's map change can reach (of %zu map files)", g_maps.size(),
         stems.size());
    return 1;
}

const MapInfo* find_map(const std::string& name)
{
    for (const MapInfo& m : g_maps)
        if (m.name == name || m.file == name) return &m;
    return nullptr;
}

std::string maps_json()
{
    std::string o = "[";
    for (const MapInfo& m : g_maps) {
        if (o.size() > 1) o += ",";
        o += "{\"name\":" + json_str(m.name) + ",\"number\":" + std::to_string(m.number) + ",\"group\":" +
             json_str(m.group) + ",\"location\":" + json_str(m.location) + ",\"entries\":[";
        for (size_t i = 0; i < m.entries.size(); i++) o += (i ? "," : "") + json_str(m.entries[i]);
        o += "]}";
    }
    return o + "]";
}

/* ---- warp: the game's own map change (Event:SetMap), answered when the game arrives ---- */
struct Pending {
    bool on = false;
    Cmd cmd;
    std::string map, entry;
    int expected = -1;
    uint32_t area0 = 0, frames0 = 0, last_area = 0, last_frames = 0;
    bool loaded = false;         /* a load seen: the guest's frame counter stood still (it does while a map loads) */
    std::chrono::steady_clock::time_point t0, changed, frames_moved;
};
Pending g_warp;

bool safe_text(const std::string& s)
{
    if (s.empty() || s.size() > 64) return false;
    for (char ch : s)
        if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-' && ch != ';') return false;
    return true;
}

/* 1 started (answered later), 0 refused (answered) */
int start_warp(const ppu_context* c, const Cmd& cmd, std::string rest)
{
    if (g_warp.on) { answer(cmd, false, "a warp is in progress (to " + g_warp.map + ")"); return 0; }
    std::string err;
    if (!make_catalog(c, err)) { answer(cmd, false, err); return 0; }
    const std::string name = word(rest), given = word(rest);
    const MapInfo* m = find_map(name);
    if (!m) { answer(cmd, false, "no map named " + name + " that the game's map change can reach (`maps` lists them)"); return 0; }
    const std::string entry = !given.empty() ? given : !m->entries.empty() ? m->entries[0] : "-gDebug";
    if (!safe_text(entry)) { answer(cmd, false, "not an arrival point: " + entry); return 0; }
    std::string out;
    const std::string chunk = "local Event=EventClass.Event; Event:SetMap(\"" + m->name + "\",\"" + entry + "\")";
    if (run_lua(c, chunk, out) != 1) { answer(cmd, false, "the map change failed: " + out); return 0; }
    g_warp.on = true;
    g_warp.cmd = cmd;
    g_warp.map = m->name;
    g_warp.entry = entry;
    g_warp.expected = m->number;
    g_warp.area0 = g_warp.last_area = g32(kArea);
    g_warp.frames0 = g_warp.last_frames = g32(kFrames);
    g_warp.loaded = false;
    g_warp.t0 = g_warp.changed = g_warp.frames_moved = std::chrono::steady_clock::now();
    fprintf(stderr, "[warp] t=%.3f map %s %s (map %d, from area %u)\n", ft_clock(), m->name.c_str(), entry.c_str(),
            m->number, g_warp.area0);
    return 1;
}

/* each frame: arrived (the map's own number, the game running there), or in another area that has settled (a map
 * the game shows as another's: Field_80), or not at all within 90 s */
void check_warp()
{
    if (!g_warp.on) return;
    using clk = std::chrono::steady_clock;
    const clk::time_point now = clk::now();
    const uint32_t area = g32(kArea), frames = g32(kFrames);
    if (area != g_warp.last_area) { g_warp.last_area = area; g_warp.changed = now; }
    if (frames != g_warp.last_frames) {
        if (std::chrono::duration<double>(now - g_warp.frames_moved).count() >= 0.25) g_warp.loaded = true;
        g_warp.last_frames = frames;
        g_warp.frames_moved = now;
    }
    const double since = std::chrono::duration<double>(now - g_warp.t0).count();
    const double settled = std::chrono::duration<double>(now - g_warp.changed).count();
    /* the map's own number reached and settled -- and, for the map it was already in (a respawn), a load seen first */
    const bool there = (int)area == g_warp.expected && settled >= 1.0 &&
                       ((int)g_warp.area0 != g_warp.expected || (g_warp.loaded && std::chrono::duration<double>(now - g_warp.frames_moved).count() < 0.2 &&
                                                               std::chrono::duration<double>(now - g_warp.changed).count() >= 1.0));
    const bool elsewhere = area != g_warp.area0 && (int)area != g_warp.expected && settled >= 3.0 && since >= 5.0;
    if (!there && !elsewhere && since < 90.0) return;
    g_warp.on = false;
    char b[512];
    snprintf(b, sizeof b,
             "{\"map\":%s,\"entry\":%s,\"area\":%u,\"expected\":%d,\"frames\":%u,\"seconds\":%.1f%s}",
             json_str(g_warp.map).c_str(), json_str(g_warp.entry).c_str(), area, g_warp.expected, frames - g_warp.frames0,
             since, elsewhere ? ",\"note\":\"arrived in another area than its own number\"" : "");
    fprintf(stderr, "[warp] t=%.3f %s %s area %u frames %u\n", ft_clock(), there || elsewhere ? "arrived" : "not-arrived",
            g_warp.map.c_str(), area, frames);
    if (there || elsewhere) answer(g_warp.cmd, true, b, true);
    else answer(g_warp.cmd, false, std::string("not arrived in 90 s: ") + b);
}

/* ---- the player (M11.0c): the game's own CPlayerActor functions on the player its actor manager returns [C] ---- */
constexpr uint32_t kActorToc = 0x01028658;        /* actor_manager's module TOC */
constexpr uint32_t kActorManagerSlot = kActorToc + 0x196C;   /* the TOC word holding gActorManager (its getter) */
constexpr uint32_t fn_get_player = 0x000A2248;    /* CActorManager::getPlayer */
constexpr uint32_t kPlayerToc = 0x01038050;       /* CPlayerActor's module TOC */
constexpr uint32_t fn_set_hp = 0x00101928, fn_set_mp = 0x00101940, fn_set_max_hp = 0x001020B0,
                   fn_set_max_mp = 0x00101FFC, fn_set_invincible = 0x00101F78, fn_damage_hp = 0x0011A94C;
constexpr uint32_t kPlayerVtable = 0x00FE3D20;    /* dev_state.cpp: the player's class */
constexpr uint32_t kHp = 0x5B8, kMp = 0x5BC, kMaxHp = 0x5C0, kMaxMp = 0x5C4, kInvincible = 0x54D;
constexpr uint32_t kSwordType = 0x5D0;   /* M11.1: the equipped sword (CPlayerActor::getSwordType, 0x000FA024, reads it) */

std::atomic<bool> g_nodeath{ false }, g_invulnerable{ false };
std::atomic<uint32_t> g_nodeath_saves{ 0 }, g_invuln_reasserts{ 0 }, g_invuln_blocked{ 0 };

/* the player object, or 0 (not in play) */
uint32_t player(const ppu_context* c)
{
    const uint32_t mgr = g32(kActorManagerSlot);
    if (!mgr) return 0;
    const uint32_t p = (uint32_t)call(c, fn_get_player, kActorToc, { mgr });
    return p && g32(p) == kPlayerVtable ? p : 0;
}

std::string player_json(const ppu_context* c)
{
    const uint32_t p = player(c);
    std::string money = "null", out;
    if (run_lua(c, "local Event=EventClass.Event; return Event:GetMoney()", out) == 1) money = out;
    char b[512];
    if (!p) {
        snprintf(b, sizeof b, "{\"in_play\":false,\"money\":%s,\"invulnerable\":%s,\"nodeath\":%s}", money.c_str(),
                 g_invulnerable ? "true" : "false", g_nodeath ? "true" : "false");
        return b;
    }
    snprintf(b, sizeof b,
             "{\"in_play\":true,\"life\":%d,\"max_life\":%d,\"magic\":%d,\"max_magic\":%d,\"money\":%s,"
             "\"invincible_flag\":%u,\"invulnerable\":%s,\"nodeath\":%s,\"nodeath_saves\":%u,\"invulnerable_reasserts\":%u,"
             "\"invulnerable_blocked\":%u,\"sword\":%u}",
             (int32_t)g32(p + kHp), (int32_t)g32(p + kMaxHp), (int32_t)g32(p + kMp), (int32_t)g32(p + kMaxMp),
             money.c_str(), vm_base[p + kInvincible], g_invulnerable ? "true" : "false", g_nodeath ? "true" : "false",
             g_nodeath_saves.load(), g_invuln_reasserts.load(), g_invuln_blocked.load(), g32(p + kSwordType));
    return b;
}

bool number_arg(std::string& rest, long& v, long lo, long hi)
{
    const std::string s = word(rest);
    char* e = nullptr;
    v = s.empty() ? 0 : strtol(s.c_str(), &e, 10);
    return !s.empty() && e && !*e && v >= lo && v <= hi;
}

bool on_off(std::string& rest, bool& v)
{
    const std::string s = word(rest);
    if (s == "on" || s == "1") { v = true; return true; }
    if (s == "off" || s == "0") { v = false; return true; }
    return false;
}

/* 1 answered (a change), 0 answered (none), -1 not a player command */
int player_command(const ppu_context* c, const Cmd& cmd, const std::string& w, std::string rest)
{
    long n = 0;
    bool on = false;
    if (w == "player") { answer(cmd, true, player_json(c), true); return 0; }
    if (w == "money") {
        if (!number_arg(rest, n, 0, 9999)) { answer(cmd, false, "money: a number 0-9999"); return 0; }
        std::string out;
        if (run_lua(c, "local Event=EventClass.Event; Event:SetMoney(" + std::to_string(n) + ")", out) != 1) {
            answer(cmd, false, "Event:SetMoney: " + out);
            return 0;
        }
        answer(cmd, true, player_json(c), true);
        return 1;
    }
    if (w == "nodeath") {
        if (!on_off(rest, on)) { answer(cmd, false, "nodeath on|off"); return 0; }
        if (on && !lookup(fn_damage_hp)) { answer(cmd, false, "this build has no damage function"); return 0; }
        g_nodeath = on;
        answer(cmd, true, player_json(c), true);
        return 0;
    }
    const bool needs_player = w == "life" || w == "maxlife" || w == "magic" || w == "maxmagic" || w == "heal" ||
                              w == "invulnerable" || w == "damage";
    if (!needs_player) return -1;
    const uint32_t p = player(c);
    if (!p) { answer(cmd, false, "not in play (no player)"); return 0; }
    if (w == "heal") {
        call(c, fn_set_hp, kPlayerToc, { p, g32(p + kMaxHp) });
        call(c, fn_set_mp, kPlayerToc, { p, g32(p + kMaxMp) });
    } else if (w == "invulnerable") {
        if (!on_off(rest, on)) { answer(cmd, false, "invulnerable on|off"); return 0; }
        g_invulnerable = on;
        call(c, fn_set_invincible, kPlayerToc, { p, on ? 1u : 0u });
    } else {
        if (!number_arg(rest, n, w == "damage" ? 1 : 0, 1000)) { answer(cmd, false, w + ": a number"); return 0; }
        const uint32_t f = w == "life" ? fn_set_hp : w == "magic" ? fn_set_mp : w == "maxlife" ? fn_set_max_hp
                         : w == "maxmagic" ? fn_set_max_mp : fn_damage_hp;
        call(c, f, kPlayerToc, { p, (uint64_t)n });
    }
    answer(cmd, true, player_json(c), true);
    return 1;
}

/* each frame (the game's main thread): invulnerability held -- the game clears its own flag (a hit's blink ending,
 * a map change), so it is set again whenever it is found off */
void keep_invulnerable(const ppu_context* c)
{
    if (!g_invulnerable) return;
    const uint32_t mgr = g32(kActorManagerSlot);
    if (!mgr) return;
    const uint32_t p = player(c);
    if (!p || vm_base[p + kInvincible]) return;
    call(c, fn_set_invincible, kPlayerToc, { p, 1 });
    g_invuln_reasserts++;
}

/* ---- progression, quests, items (M11.0d) ---- */
constexpr uint32_t kEventToc = 0x01057910;        /* EventClass's module TOC */
constexpr uint32_t fn_player_data = 0x00985C08;   /* GetItem's first call: the player's data, by the name in its TOC */
constexpr uint32_t fn_item_object = 0x00985D88;   /* GetItem's second: (player data, id) -> the item's object */
constexpr uint32_t kItemCount = 0x54;             /* the item object's count (CItem::setNum/getNum) */
constexpr uint32_t kBundle = 0x00A7C908, kBundleSize = 0x39B7D5;   /* the scripts the game runs, in its EBOOT */

/* the main story in steps (docs/M11_PLAN.md section 9): our own labels; the flags the game's scripts set at each
 * (flag, value), the world map's progress flags, the items the step gives, the magic containers its event adds
 * (Event:AddMaxMP: the yellow orb's 3, mainevent_007_yelloworb) -- read in the scripts the game runs */
struct Step { const char* label; std::vector<std::pair<int, int>> flags; std::vector<int> progress, items; int max_magic = 0; };
const std::vector<Step>& story()
{
    static const std::vector<Step> s = {
        { "The hero's dream", { { 92, 1 } }, {}, {} },
        { "The dream ends", { { 93, 1 }, { 95, 1 } }, {}, {} },
        { "Waking at the inn", {}, {}, {} },
        { "The castle: the story begins", { { 0, 1 } }, { 1 }, { 105, 89 } },
        { "The king's request", { { 2, 1 } }, { 2 }, {} },
        { "Setting out", { { 9, 1 } }, {}, {} },
        { "The ancient sword", { { 1, 1 }, { 5, 1 } }, {}, { 95 } },
        { "Temple 1 entered", { { 4, 1 }, { 98, 1 } }, {}, {} },
        { "Temple 1: its guardian defeated", { { 6, 1 }, { 8, 1 } }, {}, {} },
        { "Orb 1 and spell 1", { { 7, 1 }, { 82, 1 } }, { 3 }, { 70, 15 }, 3 },
        { "Temple 2: its guardian defeated, orb 2", { { 11, 1 }, { 14, 1 }, { 15, 1 }, { 12, 1 }, { 82, 2 } }, {}, { 6, 71 } },
        { "Spell 2", { { 13, 1 } }, { 4 }, { 19, 55 } },
        { "Temple 3: its guardian defeated, orb 3", { { 21, 1 }, { 22, 1 }, { 25, 1 }, { 23, 1 }, { 82, 3 } }, {}, { 72 } },
        { "Spell 3", { { 24, 1 } }, { 5 }, { 17, 55 } },
        { "Temple 4: its guardian defeated, orb 4",
          { { 31, 1 }, { 38, 1 }, { 32, 1 }, { 33, 1 }, { 34, 1 }, { 37, 1 }, { 35, 1 }, { 82, 4 } }, {}, { 54, 73 } },
        { "Spell 4", { { 36, 1 }, { 309, 1 } }, { 6 }, { 18, 55 } },
        { "Temple 5: its guardian defeated, orb 5",
          { { 97, 1 }, { 40, 1 }, { 41, 1 }, { 229, 1 }, { 42, 1 }, { 91, 1 }, { 43, 1 }, { 82, 5 } }, {}, { 62, 74 } },
        { "Spell 5", { { 44, 1 }, { 45, 1 }, { 46, 1 }, { 47, 1 }, { 48, 1 }, { 49, 1 } }, { 7 }, { 20, 55 } },
        { "Temple 6: its guardian defeated, orb 6", { { 50, 1 }, { 52, 1 }, { 51, 1 }, { 82, 6 } }, { 8 }, { 75 } },
        { "The princess", { { 60, 1 }, { 61, 1 }, { 236, 1 }, { 62, 1 } }, {}, {} },
        { "The battle at the castle", { { 63, 1 }, { 64, 1 } }, {}, {} },
        { "Spell 6 and the orb of light", { { 66, 1 } }, { 9 }, { 16, 76, 55 } },
        { "The tower entered", { { 70, 1 } }, {}, {} },
        { "The tower's rematches won",
          { { 310, 1 }, { 311, 1 }, { 312, 1 }, { 313, 1 }, { 314, 1 }, { 315, 1 }, { 316, 1 }, { 317, 1 }, { 318, 1 },
            { 319, 1 }, { 320, 1 }, { 321, 1 } }, {}, {} },
        { "The last enemy defeated", { { 74, 1 } }, {}, {} },
        { "The ending", { { 81, 1 }, { 99, 1 } }, {}, {} },
    };
    return s;
}

/* the flags' names (the scripts' EventFlag table) and the items' (item.Item's IL_ names), from the game's state */
std::map<int, std::string> g_flag_names, g_item_names;
/* the quests: the script functions that set flags of 100 and above, each with its flags (the scripts in memory) */
std::vector<std::pair<std::string, std::vector<int>>> g_quests;
bool g_names_made = false;

const char kNamesLua[] = R"(local out = {}
if type(EventFlag) == "table" then for k, v in pairs(EventFlag) do if type(v) == "number" then out[#out+1] = "F\t" .. v .. "\t" .. k end end end
if type(item) == "table" and type(item.Item) == "table" then
  for k, v in pairs(item.Item) do if type(k) == "string" and string.sub(k, 1, 3) == "IL_" and type(v) == "number" then out[#out+1] = "I\t" .. v .. "\t" .. string.sub(k, 4) end end
end
return table.concat(out, "\n"))";

int make_names(const ppu_context* c, std::string& err)
{
    if (g_names_made) return 1;
    std::string out;
    if (run_lua(c, kNamesLua, out) != 1) { err = "the game's flag and item names: " + out; return 0; }
    size_t a = 0;
    while (a < out.size()) {
        size_t e = out.find('\n', a);
        if (e == std::string::npos) e = out.size();
        const std::string ln = out.substr(a, e - a);
        a = e + 1;
        const size_t t1 = ln.find('\t'), t2 = t1 == std::string::npos ? t1 : ln.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        (ln[0] == 'F' ? g_flag_names : g_item_names)[atoi(ln.c_str() + t1 + 1)] = ln.substr(t2 + 1);
    }
    /* the quests: in the scripts the game runs (its own memory), which function sets which flag of 100 and above */
    std::map<std::string, std::vector<int>> by_fn;
    const char* text = (const char*)vm_base + kBundle;
    std::string fn;
    for (uint32_t i = 0; i + 16 < kBundleSize; i++) {
        if ((i == 0 || text[i - 1] == '\n') && !strncmp(text + i, "function ", 9)) {
            uint32_t j = i + 9;
            while (j < kBundleSize && (isalnum((unsigned char)text[j]) || text[j] == '_' || text[j] == '.' || text[j] == ':')) j++;
            fn.assign(text + i + 9, j - i - 9);
        } else if (!strncmp(text + i, "SetEventFlag(", 13) && !fn.empty()) {
            uint32_t j = i + 13;
            while (j < kBundleSize && text[j] == ' ') j++;
            if (j < kBundleSize && isdigit((unsigned char)text[j])) {
                const int f = atoi(text + j);
                if (f >= 100 && f < 1000) {
                    auto& v = by_fn[fn];
                    if (std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
                }
            }
        }
    }
    g_quests.assign(by_fn.begin(), by_fn.end());
    for (auto& q : g_quests) std::sort(q.second.begin(), q.second.end());
    g_names_made = true;
    DLOG(DH_LOG_INFO, "dev", "names: %zu flags, %zu items; %zu quest functions", g_flag_names.size(), g_item_names.size(),
         g_quests.size());
    return 1;
}

/* the values of these flags (the game's GetEventFlag) */
std::map<int, int> flag_values(const ppu_context* c, const std::vector<int>& ids)
{
    std::map<int, int> v;
    std::string chunk = "local E=EventClass.Event; local t={}; for _, i in ipairs({", out;
    for (size_t k = 0; k < ids.size(); k++) chunk += (k ? "," : "") + std::to_string(ids[k]);
    chunk += "}) do t[#t+1]=i..'='..E:GetEventFlag(i) end; return table.concat(t,' ')";
    if (run_lua(c, chunk, out) != 1) throw CallError{ "the game's flags could not be read" };
    size_t a = 0;
    while (a < out.size()) {
        const size_t sp = out.find(' ', a), eq = out.find('=', a);
        if (eq == std::string::npos) break;
        v[atoi(out.c_str() + a)] = atoi(out.c_str() + eq + 1);
        a = sp == std::string::npos ? out.size() : sp + 1;
    }
    return v;
}

std::string progress_json(const ppu_context* c)
{
    std::vector<int> ids;
    for (const Step& s : story()) for (auto& f : s.flags) ids.push_back(f.first);
    const std::map<int, int> v = flag_values(c, ids);
    std::string o = "[";
    int k = 0;
    for (const Step& s : story()) {
        bool done = true;
        for (auto& f : s.flags) { auto it = v.find(f.first); done = done && it != v.end() && it->second >= f.second; }
        o += std::string(k ? "," : "") + "{\"step\":" + std::to_string(k + 1) + ",\"label\":" + json_str(s.label) +
             ",\"done\":" + (done ? "true" : "false") + "}";
        k++;
    }
    return o + "]";
}

/* M11.1b: the game's items are read and given through its own Event:GetItem and AddItem2 -- with no hero in play (its
 * title, after a game over declined) GetItem finds no player data and the game ends itself (exit(0) in its
 * implementation, 0x4CCE44): those commands are refused then */
const char kNoItemsOutOfPlay[] = "not in play (no player): the game's own item read ends the game without one";

/* the item's object in the player's data (GetItem's way), or 0 */
uint32_t item_object(const ppu_context* c, int id)
{
    const uint32_t pd = (uint32_t)call(c, fn_player_data, kEventToc, { g32(kEventToc + 0x3A84) });
    return pd ? (uint32_t)call(c, fn_item_object, kEventToc, { pd, (uint64_t)id }) : 0;
}

std::string items_json(const ppu_context* c, bool all)
{
    std::string out;
    if (run_lua(c, "local E=EventClass.Event; local t={}; for i=1,159 do t[#t+1]=E:GetItem(i) end; "
                   "return table.concat(t,' ')", out) != 1)
        throw CallError{ "the game's items could not be read" };
    std::string o = "[";
    size_t a = 0;
    for (int id = 1; id <= 159 && a < out.size(); id++) {
        const int n = atoi(out.c_str() + a);
        const size_t sp = out.find(' ', a);
        a = sp == std::string::npos ? out.size() : sp + 1;
        auto nm = g_item_names.find(id);
        if (n == 0 && (!all || nm == g_item_names.end())) continue;   /* `all`: every item the game names, and any held */
        o += std::string(o.size() > 1 ? "," : "") + "{\"id\":" + std::to_string(id) + ",\"name\":" +
             json_str(nm != g_item_names.end() ? nm->second : "") + ",\"count\":" + std::to_string(n) + "}";
    }
    return o + "]";
}

/* 1 answered (a change), 0 answered, -1 not one of these */
int progression_command(const ppu_context* c, const Cmd& cmd, const std::string& w, std::string rest)
{
    long a = 0, b = 0;
    std::string err, out;
    const bool ours = w == "flag" || w == "flags" || w == "progress" || w == "quests" || w == "items" || w == "give" ||
                      w == "count" || w == "remove";
    if (!ours) return -1;
    if (!make_names(c, err)) { answer(cmd, false, err); return 0; }
    if (w == "flag") {
        const std::string op = word(rest);
        if (op == "get" && number_arg(rest, a, 0, 999)) {
            const auto v = flag_values(c, { (int)a });
            auto nm = g_flag_names.find((int)a);
            answer(cmd, true, "{\"flag\":" + std::to_string(a) + ",\"name\":" + json_str(nm != g_flag_names.end() ? nm->second : "") +
                              ",\"value\":" + std::to_string(v.count((int)a) ? v.at((int)a) : 0) + "}", true);
            return 0;
        }
        if (op == "set" && number_arg(rest, a, 0, 999) && number_arg(rest, b, 0, 255)) {
            if (run_lua(c, "local E=EventClass.Event; E:SetEventFlag(" + std::to_string(a) + "," + std::to_string(b) + ")", out) != 1) {
                answer(cmd, false, "Event:SetEventFlag: " + out);
                return 0;
            }
            answer(cmd, true, "{\"flag\":" + std::to_string(a) + ",\"value\":" + std::to_string(flag_values(c, { (int)a })[(int)a]) + "}", true);
            return 1;
        }
        answer(cmd, false, "flag get N | flag set N VALUE");
        return 0;
    }
    if (w == "flags") {   /* every flag of a range that is not 0 (`all`: and every named one), with its name */
        std::string r2 = rest;
        if (!number_arg(rest, a, 0, 999)) a = 0;
        if (!number_arg(rest, b, 0, 999)) b = 330;
        const bool named_too = r2.find("all") != std::string::npos;
        std::vector<int> ids;
        for (long i = a; i <= b; i++) ids.push_back((int)i);
        const auto v = flag_values(c, ids);
        std::string o = "[";
        for (auto& kv : v) {
            auto nm = g_flag_names.find(kv.first);
            if (!kv.second && !(named_too && nm != g_flag_names.end())) continue;
            o += std::string(o.size() > 1 ? "," : "") + "{\"flag\":" + std::to_string(kv.first) + ",\"name\":" +
                 json_str(nm != g_flag_names.end() ? nm->second : "") + ",\"value\":" + std::to_string(kv.second) + "}";
        }
        answer(cmd, true, o + "]", true);
        return 0;
    }
    if (w == "progress") {
        const std::string op = word(rest);
        if (op.empty()) { answer(cmd, true, progress_json(c), true); return 0; }
        if (op != "set" || !number_arg(rest, a, 1, (long)story().size())) { answer(cmd, false, "progress | progress set STEP"); return 0; }
        if (!player(c)) { answer(cmd, false, kNoItemsOutOfPlay); return 0; }   /* its items: the game's GetItem */
        /* every step up to this one, as the scripts leave it: its flags, the world map's progress, its items (given
         * through the game's own item-get only when not already held), the magic containers its event adds (only when
         * its first flag was not set yet: a step done once adds them once) */
        std::string chunk = "local E=EventClass.Event; ";
        for (long k = 0; k < a; k++) {
            const Step& s = story()[k];
            if (s.max_magic && !s.flags.empty())
                chunk += "if E:GetEventFlag(" + std::to_string(s.flags[0].first) + ")<" + std::to_string(s.flags[0].second) +
                         " then E:AddMaxMP(" + std::to_string(s.max_magic) + ") end; ";
            for (auto& f : s.flags) chunk += "E:SetEventFlag(" + std::to_string(f.first) + "," + std::to_string(f.second) + "); ";
            for (int p : s.progress) chunk += "E:SetProgressFlag(" + std::to_string(p) + ",1); ";
            for (int it : s.items) chunk += "if E:GetItem(" + std::to_string(it) + ")==0 then E:AddItem2(" + std::to_string(it) + ") end; ";
        }
        if (run_lua(c, chunk, out) != 1) { answer(cmd, false, "the story's flags: " + out); return 0; }
        answer(cmd, true, progress_json(c), true);
        return 1;
    }
    if (w == "quests") {
        std::vector<int> ids;
        for (auto& q : g_quests) for (int f : q.second) ids.push_back(f);
        const auto v = flag_values(c, ids);
        std::string o = "[";
        for (auto& q : g_quests) {
            o += std::string(o.size() > 1 ? "," : "") + "{\"function\":" + json_str(q.first) + ",\"flags\":[";
            for (size_t k = 0; k < q.second.size(); k++)
                o += std::string(k ? "," : "") + "{\"flag\":" + std::to_string(q.second[k]) + ",\"value\":" +
                     std::to_string(v.count(q.second[k]) ? v.at(q.second[k]) : 0) + "}";
            o += "]}";
        }
        answer(cmd, true, o + "]", true);
        return 0;
    }
    if ((w == "items" || w == "give" || w == "count" || w == "remove") && !player(c)) {
        answer(cmd, false, kNoItemsOutOfPlay);
        return 0;
    }
    if (w == "items") { answer(cmd, true, items_json(c, word(rest) == "all"), true); return 0; }
    if (!number_arg(rest, a, 1, 159)) { answer(cmd, false, w + ": an item number 1-159"); return 0; }
    if (w == "give") {
        if (!number_arg(rest, b, 1, 99)) b = 1;
        if (run_lua(c, "local E=EventClass.Event; for i=1," + std::to_string(b) + " do E:AddItem2(" + std::to_string(a) + ") end", out) != 1) {
            answer(cmd, false, "Event:AddItem2: " + out);
            return 0;
        }
    } else {   /* count / remove: the item object's own count (CItem::setNum) */
        if (w == "count" && !number_arg(rest, b, 0, 9999)) { answer(cmd, false, "count ITEM N"); return 0; }
        uint32_t obj = item_object(c, (int)a);
        if (!obj && w == "count" && b > 0) {   /* never held: the game makes its object when it is first given */
            run_lua(c, "local E=EventClass.Event; E:AddItem2(" + std::to_string(a) + ")", out);
            obj = item_object(c, (int)a);
        }
        if (!obj && w == "remove") { answer(cmd, true, "{\"id\":" + std::to_string(a) + ",\"count\":0}", true); return 0; }
        if (!obj) { answer(cmd, false, "the game has no object for that item (not in play?)"); return 0; }
        w32(obj + kItemCount, w == "remove" ? 0u : (uint32_t)b);
    }
    const std::string all = items_json(c, true);
    const std::string key = "{\"id\":" + std::to_string(a) + ",";
    const size_t at = all.find(key);
    answer(cmd, true, at == std::string::npos ? "{}" : all.substr(at, all.find('}', at) - at + 1), true);
    return 1;
}

/* ---- bosses, enemies, events, cinematics (M11.0e) ---- */
constexpr uint32_t fn_start_event = 0x004D5BC0;   /* StartEvent(event, name): `Coroutine=coroutine.create(name)` */
constexpr uint32_t kEventNameOff = 0xFC00;        /* an event's name, in the tools' region */

/* the bosses, the game's way: each one's arena, and the flags its defeat sets -- cleared, the arena's own entry event
 * starts the fight again (docs/M11_PLAN.md section 10; our own labels) */
struct Boss { const char* id; const char* label; const char* map; std::vector<int> flags; };
const std::vector<Boss>& bosses()
{
    static const std::vector<Boss> b = {
        { "temple1", "Temple 1's guardian", "D_1_2", { 6, 8 } }, { "temple2", "Temple 2's guardian", "D_2_2", { 11, 14 } },
        { "temple3", "Temple 3's guardian", "D_3_2", { 22, 25 } }, { "temple4", "Temple 4's guardian", "D_4_2", { 33, 37 } },
        { "temple5", "Temple 5's guardian", "D_5_2", { 42, 91 } }, { "temple6", "Temple 6's guardian", "D_6_2", { 50, 52 } },
        { "castle", "The battle at the castle", "D_8_2", { 64 } },
        { "tower1", "The tower: rematch 1", "D_7_2_1", { 311 } }, { "tower2", "The tower: rematch 2", "D_7_2_2", { 313 } },
        { "tower3", "The tower: rematch 3", "D_7_2_3", { 315 } }, { "tower4", "The tower: rematch 4", "D_7_2_4", { 317 } },
        { "tower5", "The tower: rematch 5", "D_7_2_5", { 319 } }, { "tower6", "The tower: rematch 6", "D_7_2_6", { 321 } },
        { "tower7", "The tower: before the last", "D_7_2_7", {} }, { "last", "The last enemy", "D_7_2_8", { 74 } },
    };
    return b;
}

/* the event functions: the script functions the game runs as events (each yields: a coroutine), read in the scripts
 * the game runs (its own memory); a "cinematic" one moves the camera or plays a scene */
struct EventFn { std::string name; bool cinematic; };
std::vector<EventFn> g_events;
bool g_events_made = false;

void make_events()
{
    if (g_events_made) return;
    const char* text = (const char*)vm_base + kBundle;
    auto at_line = [&](uint32_t i) { return i == 0 || text[i - 1] == '\n'; };
    uint32_t i = 0;
    while (i + 9 < kBundleSize) {
        if (!(at_line(i) && !strncmp(text + i, "function ", 9))) { i++; continue; }
        uint32_t j = i + 9;
        while (j < kBundleSize && (isalnum((unsigned char)text[j]) || text[j] == '_')) j++;
        const std::string name(text + i + 9, j - i - 9);
        uint32_t e = j;   /* the body: up to the next function at a line's start */
        while (e + 9 < kBundleSize && !(at_line(e) && !strncmp(text + e, "function ", 9))) e++;
        const std::string body(text + j, e - j);
        if (!name.empty() && body.find("coroutine.yield") != std::string::npos)
            g_events.push_back({ name, body.find("PlayScene") != std::string::npos || body.find("SetCamera") != std::string::npos ||
                                       body.find("PlayCamera") != std::string::npos });
        i = e;
    }
    std::sort(g_events.begin(), g_events.end(), [](const EventFn& a, const EventFn& b) { return a.name < b.name; });
    g_events.erase(std::unique(g_events.begin(), g_events.end(), [](const EventFn& a, const EventFn& b) { return a.name == b.name; }),
                   g_events.end());
    g_events_made = true;
    DLOG(DH_LOG_INFO, "dev", "the event functions: %zu", g_events.size());
}

/* M11.1 (docs/M11_PLAN.md section 19): the enemy types, read in the scripts the game runs as make_events reads its
 * events -- each `function <Name>Initialize(Object)` whose body makes its object an enemy
 * (SetType(actor_manager.AT_ENEMY)); `rare`: its body can hide the object instead (AT_UNKNOWN: a chance to appear);
 * `minigame`: a minigame's own version (its scripts' MG prefix), which expects its minigame's state -- not made
 * elsewhere (a script error inside the engine's own calls silences every later script call) */
struct Spawnable { std::string name; bool rare, minigame; };
std::vector<Spawnable> g_spawnables;
bool g_spawnables_made = false;
/* the scripts' math.random given back a few frames after a rare enemy's set-up (`spawn`): its Initialize may run in
 * the frame after its object is made (on_flip counts down) */
int g_random_restore = 0;

void make_spawnables()
{
    if (g_spawnables_made) return;
    const char* text = (const char*)vm_base + kBundle;
    auto at_line = [&](uint32_t i) { return i == 0 || text[i - 1] == '\n'; };
    auto enemy_type = [](const std::string& body) {
        for (size_t p = body.find("actor_manager.AT_ENEMY"); p != std::string::npos; p = body.find("actor_manager.AT_ENEMY", p + 1)) {
            size_t q = p + 22;
            while (q < body.size() && body[q] == ' ') q++;
            if (q < body.size() && body[q] == ')') return true;   /* AT_ENEMY itself, not AT_ENEMYWEAPON */
        }
        return false;
    };
    uint32_t i = 0;
    while (i + 9 < kBundleSize) {
        if (!(at_line(i) && !strncmp(text + i, "function ", 9))) { i++; continue; }
        uint32_t j = i + 9;
        while (j < kBundleSize && (isalnum((unsigned char)text[j]) || text[j] == '_')) j++;
        const std::string name(text + i + 9, j - i - 9);
        uint32_t e = j;
        while (e + 9 < kBundleSize && !(at_line(e) && !strncmp(text + e, "function ", 9))) e++;
        const size_t n = name.size();
        if (n > 10 && name.compare(n - 10, 10, "Initialize") == 0) {
            const std::string body(text + j, e - j);
            if (enemy_type(body))
                g_spawnables.push_back({ name.substr(0, n - 10), body.find("AT_UNKNOWN") != std::string::npos,
                                         name.compare(0, 2, "MG") == 0 });
        }
        i = e;
    }
    std::sort(g_spawnables.begin(), g_spawnables.end(), [](const Spawnable& a, const Spawnable& b) { return a.name < b.name; });
    g_spawnables.erase(std::unique(g_spawnables.begin(), g_spawnables.end(),
                                   [](const Spawnable& a, const Spawnable& b) { return a.name == b.name; }), g_spawnables.end());
    g_spawnables_made = true;
    DLOG(DH_LOG_INFO, "dev", "the enemy types: %zu", g_spawnables.size());
}

/* the game's own StartEvent on `name` (a global script function), only while no event runs */
bool start_event(const ppu_context* c, const std::string& name, std::string& why)
{
    const uint32_t ev = manager_event();
    if (!ev) { why = "no event object (the scripts not loaded?)"; return false; }
    if (g32(ev + 0x20) != 0) { why = "an event is running (its state " + std::to_string(g32(ev + 0x20)) + ")"; return false; }
    if (name.size() > 120) { why = "the name is too long"; return false; }
    memcpy(vm_base + g_region + kEventNameOff, name.c_str(), name.size() + 1);
    call(c, fn_start_event, kEventToc, { ev, g_region + kEventNameOff });
    return true;
}

const char kActorsLua[] = R"(local m = actor_manager.gActorManager
local cat = {}
for k, v in pairs(actor_manager) do if type(k) == "string" and string.sub(k, 1, 3) == "AT_" and type(v) == "number" then cat[v] = string.sub(k, 4) end end
local t = {}
for i = 0, 1023 do
  local a = m:getActor(i)
  if a then
    local p = a:getPosition()
    t[#t+1] = i .. "\t" .. (cat[a:getType()] or a:getType()) .. "\t" .. string.format("%.1f\t%.1f", p.x, p.z) .. "\t" .. a:getState() .. "\t" .. tostring(a:isVisible())
  end
end
return table.concat(t, "\n"))";

/* 1 answered (a change), 0 answered, -1 not one of these; 2 started an answer that comes later (a warp) */
int encounter_command(const ppu_context* c, const Cmd& cmd, const std::string& w, std::string rest)
{
    std::string out, why;
    if (w == "bosses") {
        std::vector<int> ids;
        for (const Boss& b : bosses()) for (int f : b.flags) ids.push_back(f);
        const auto v = flag_values(c, ids);
        std::string o = "[";
        for (const Boss& b : bosses()) {
            bool beaten = !b.flags.empty();
            for (int f : b.flags) beaten = beaten && v.count(f) && v.at(f) != 0;
            o += std::string(o.size() > 1 ? "," : "") + "{\"id\":" + json_str(b.id) + ",\"label\":" + json_str(b.label) +
                 ",\"map\":" + json_str(b.map) + ",\"beaten\":" + (beaten ? "true" : "false") + "}";
        }
        answer(cmd, true, o + "]", true);
        return 0;
    }
    if (w == "boss") {
        const std::string id = word(rest);
        const Boss* b = nullptr;
        for (const Boss& x : bosses()) if (id == x.id) b = &x;
        if (!b) { answer(cmd, false, "no boss " + id + " (`bosses` lists them)"); return 0; }
        if (!b->flags.empty()) {
            std::string chunk = "local E=EventClass.Event; ";
            for (int f : b->flags) chunk += "E:SetEventFlag(" + std::to_string(f) + ",0); ";
            if (run_lua(c, chunk, out) != 1) { answer(cmd, false, "the boss's flags: " + out); return 0; }
        }
        return start_warp(c, cmd, b->map) == 1 ? 2 : 0;
    }
    if (w == "respawn") {   /* the map entered again: its own spawns (answered on arrival: the warp's rule) */
        std::string err;
        if (!make_catalog(c, err)) { answer(cmd, false, err); return 0; }
        const uint32_t area = g32(kArea);
        for (const MapInfo& m : g_maps)
            if (m.number == (int)area) return start_warp(c, cmd, m.name) == 1 ? 2 : 0;
        answer(cmd, false, "the map now (" + std::to_string(area) + ") is not in the catalog");
        return 0;
    }
    if (w == "enemies") {
        if (run_lua(c, kActorsLua, out) != 1) { answer(cmd, false, "the actors: " + out); return 0; }
        std::string o = "[";
        size_t a = 0;
        while (a < out.size()) {
            size_t e = out.find('\n', a);
            if (e == std::string::npos) e = out.size();
            std::string ln = out.substr(a, e - a), f[6];
            a = e + 1;
            for (int k = 0; k < 6; k++) { const size_t t = ln.find('\t'); f[k] = ln.substr(0, t); ln = t == std::string::npos ? "" : ln.substr(t + 1); }
            o += std::string(o.size() > 1 ? "," : "") + "{\"slot\":" + f[0] + ",\"kind\":" + json_str(f[1]) + ",\"x\":" + f[2] +
                 ",\"z\":" + f[3] + ",\"state\":" + f[4] + ",\"visible\":" + (f[5] == "true" ? "true" : "false") + "}";
        }
        answer(cmd, true, o + "]", true);
        return 0;
    }
    if (w == "events") {
        make_events();
        const std::string filter = word(rest);
        std::string o = "[";
        for (const EventFn& e : g_events)
            if (filter.empty() || e.name.find(filter) != std::string::npos)
                o += std::string(o.size() > 1 ? "," : "") + "{\"name\":" + json_str(e.name) + ",\"cinematic\":" +
                     (e.cinematic ? "true" : "false") + "}";
        answer(cmd, true, o + "]", true);
        return 0;
    }
    if (w == "event") {
        make_events();
        const std::string name = word(rest);
        bool known = false;
        for (const EventFn& e : g_events) known = known || e.name == name;
        if (!known) { answer(cmd, false, "no event function " + name + " (`events` lists them)"); return 0; }
        if (!start_event(c, name, why)) { answer(cmd, false, why); return 0; }
        answer(cmd, true, "{\"event\":" + json_str(name) + ",\"started\":true}", true);
        return 1;
    }
    if (w == "scene") {   /* a scene of the map, as the game's events play one: inside an event of its own */
        long n = 0;
        if (!number_arg(rest, n, 0, 99)) { answer(cmd, false, "scene N"); return 0; }
        const std::string def = "function __devtools_scene() local Event=EventClass.Event; Event:PlayScene(" +
                                std::to_string(n) + "); Event:WaitScene(); coroutine.yield(); end";
        if (run_lua(c, def, out) != 1) { answer(cmd, false, "the scene's event: " + out); return 0; }
        if (!start_event(c, "__devtools_scene", why)) { answer(cmd, false, why); return 0; }
        answer(cmd, true, "{\"scene\":" + std::to_string(n) + ",\"started\":true}", true);
        return 1;
    }
    if (w == "spawnables") {   /* M11.1: the enemy types the scripts define (make_spawnables) */
        make_spawnables();
        std::string o = "[";
        for (const Spawnable& e : g_spawnables)
            o += std::string(o.size() > 1 ? "," : "") + "{\"name\":" + json_str(e.name) + ",\"rare\":" + (e.rare ? "true" : "false") +
                 ",\"minigame\":" + (e.minigame ? "true" : "false") + "}";
        answer(cmd, true, o + "]", true);
        return 0;
    }
    if (w == "spawn") {   /* M11.1: an enemy made by the game's own Event:CreateNpc running its script, near the hero */
        make_spawnables();
        const std::string name = word(rest);
        const Spawnable* sp = nullptr;
        for (const Spawnable& e : g_spawnables) if (e.name == name) sp = &e;
        if (!sp) { answer(cmd, false, "no enemy type " + name + " (`spawnables` lists them)"); return 0; }
        if (sp->minigame) { answer(cmd, false, name + " is a minigame's own enemy: met in its minigame"); return 0; }
        double dx = 4.8, dz = 0.0;   /* three cells beside the hero, by default */
        const std::string a = word(rest), b = word(rest);
        if (!a.empty()) dx = atof(a.c_str());
        if (!b.empty()) dz = atof(b.c_str());
        if (dx < -40 || dx > 40 || dz < -40 || dz > 40) { answer(cmd, false, "spawn NAME [DX DZ]: within 40 units"); return 0; }
        char xyz[96];
        snprintf(xyz, sizeof xyz, "%.2f,0,%.2f", dx, dz);
        /* a rare one hides itself at random in its set-up: the scripts' math.random answers 1 until it has run */
        std::string chunk = "local E=EventClass.Event; ";
        if (sp->rare) chunk += "if not __devtools_random then __devtools_random=math.random end; math.random=function(a,b) return 1 end; ";
        chunk += "E:CreateNpc(" + std::string(xyz) + ",\"" + name + "\")";   /* a name from the list: letters, digits */
        if (run_lua(c, chunk, out) != 1) {
            if (sp->rare) run_lua(c, "if __devtools_random then math.random=__devtools_random; __devtools_random=nil end", why);
            answer(cmd, false, "Event:CreateNpc: " + out);
            return 0;
        }
        if (sp->rare) g_random_restore = 3;
        answer(cmd, true, "{\"spawn\":" + json_str(name) + ",\"dx\":" + std::to_string(dx) + ",\"dz\":" + std::to_string(dz) +
                          ",\"rare\":" + (sp->rare ? "true" : "false") + "}", true);
        return 1;
    }
    if (w == "ending") {   /* the game's own Ending: its ending and the credits movie (as M8's credits switch) */
        if (run_lua(c, "local E=EventClass.Event; E:Ending()", out) != 1) { answer(cmd, false, "Event:Ending: " + out); return 0; }
        answer(cmd, true, "{\"ending\":true}", true);
        return 1;
    }
    return -1;
}

/* ---- coverage, performance, the native runtime, test states (M11.0f) ---- */
struct Unforeseen { uint64_t key; std::string kind; double ms; uint32_t area; double t; };
std::mutex g_unf_mu;
std::vector<Unforeseen> g_unforeseen;   /* this session's pipelines built in play that the preparation had not made */

void on_unforeseen(uint64_t key, const char* kind, double ms)
{
    std::lock_guard<std::mutex> g(g_unf_mu);
    if (g_unforeseen.size() < 100000) g_unforeseen.push_back({ key, kind ? kind : "", ms, g32(kArea), ft_clock() });
}

/* the frames, measured at the flip (the game's own frame boundary), the time the menu held the game left out */
struct Frames {
    std::mutex mu;
    std::deque<double> recent;   /* the last 600 frame times (ms) */
    uint64_t n = 0, over50 = 0, over100 = 0;
    double longest = 0;
    std::chrono::steady_clock::time_point last;
    bool have_last = false;
    std::atomic<bool> held{ false };   /* the menu held the game since the last flip: that interval is not a frame */
} g_frames;

void note_frame()
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(g_frames.mu);
    const bool held = g_frames.held.exchange(false);
    if (g_frames.have_last && !held && !ps3_guest_paused()) {
        const double ms = std::chrono::duration<double, std::milli>(now - g_frames.last).count();
        if (ms < 60000) {
            g_frames.recent.push_back(ms);
            if (g_frames.recent.size() > 600) g_frames.recent.pop_front();
            g_frames.n++;
            g_frames.over50 += ms > 50;
            g_frames.over100 += ms > 100;
            if (ms > g_frames.longest) g_frames.longest = ms;
        }
    }
    g_frames.last = now;
    g_frames.have_last = true;
}

std::string perf_json()
{
    std::vector<double> v;
    uint64_t n, o50, o100;
    double longest;
    {
        std::lock_guard<std::mutex> g(g_frames.mu);
        v.assign(g_frames.recent.begin(), g_frames.recent.end());
        n = g_frames.n; o50 = g_frames.over50; o100 = g_frames.over100; longest = g_frames.longest;
    }
    double sum = 0, mx = 0;
    for (double x : v) { sum += x; mx = std::max(mx, x); }
    std::vector<double> s = v;
    std::sort(s.begin(), s.end());
    const double p99 = s.empty() ? 0 : s[std::min(s.size() - 1, (size_t)(s.size() * 0.99))];
    double ws = 0, pb = 0;   /* the process's memory (MB) */
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
        ws = pm.WorkingSetSize / 1048576.0; pb = pm.PrivateUsage / 1048576.0;
    }
#endif
    const char* fps = getenv("DOTHEROES_FPS");
    const char* vs = getenv("DOTHEROES_VSYNC");
    char b[600];
    snprintf(b, sizeof b,
             "{\"frames\":%llu,\"window\":%zu,\"fps\":%.1f,\"mean_ms\":%.2f,\"p99_ms\":%.2f,\"max_ms\":%.2f,"
             "\"over_50ms\":%llu,\"over_100ms\":%llu,\"longest_ms\":%.1f,\"frame_limit\":%s,\"vsync\":%s,"
             "\"working_set_mb\":%.0f,\"private_mb\":%.0f}",
             (unsigned long long)n, v.size(), v.empty() ? 0.0 : 1000.0 * v.size() / sum, v.empty() ? 0.0 : sum / v.size(),
             p99, mx, (unsigned long long)o50, (unsigned long long)o100, longest, json_str(fps ? fps : "").c_str(),
             json_str(vs ? vs : "").c_str(), ws, pb);
    return b;
}

/* the developer tools' own folder: <install>/data/devmode, or the working directory (a test's run) */
std::string devmode_dir()
{
    std::error_code ec;
    return g_install_root.empty() ? std::filesystem::current_path(ec).generic_string() : g_install_root + "/data/devmode";
}

bool safe_name(const std::string& s)
{
    if (s.empty() || s.size() > 40 || !isalnum((unsigned char)s[0])) return false;
    for (char ch : s) if (!isalnum((unsigned char)ch) && ch != '_' && ch != '-') return false;
    return true;
}

/* the test saves (the save root in use): only ever one in developer mode's own folder or a test's run directory */
std::string test_saves_root(std::string& why)
{
    const char* r = getenv("PS3_GAMEDATA_ROOT");
    if (!r || !*r) { why = "no save root"; return std::string(); }
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(r, ec);
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    const std::string rs = root.generic_string(), cs = cwd.generic_string();
    const bool devmode = rs.find("/devmode/") != std::string::npos || (rs.size() >= 8 && rs.compare(rs.size() - 8, 8, "/devmode") == 0);
    const bool test_run = g_install_root.empty() && rs.compare(0, cs.size(), cs) == 0;
    if (!devmode && !test_run) { why = "the save root is not developer mode's own: " + rs; return std::string(); }
    return rs;
}

/* dest replaced by a copy of src (src only read), through a folder beside it and a rename */
bool replace_tree(const std::string& src, const std::string& dest, std::string& why)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string part = dest + ".incoming", old = dest + ".replaced";
    fs::remove_all(part, ec);
    fs::remove_all(old, ec);
    fs::create_directories(fs::path(dest).parent_path(), ec);
    if (fs::exists(src, ec)) fs::copy(src, part, fs::copy_options::recursive, ec);
    else fs::create_directories(part, ec);
    if (ec) { why = "the copy failed: " + ec.message(); fs::remove_all(part, ec); return false; }
    if (fs::exists(dest)) fs::rename(dest, old, ec);
    if (ec) { why = "the old tree could not be moved aside: " + ec.message(); return false; }
    fs::rename(part, dest, ec);
    if (ec) { why = "the new tree could not be put in place: " + ec.message(); fs::rename(old, dest, ec); return false; }
    fs::remove_all(old, ec);
    return true;
}

extern "C" void phys_direct_report(void) __attribute__((weak));
extern "C" void inflate_direct_report(void) __attribute__((weak));
extern "C" void water_task_report(void) __attribute__((weak));
extern "C" void waver_game_report(void) __attribute__((weak));
extern "C" void cellGcm_flip_wake_report(void) __attribute__((weak));
extern "C" void ppfx_render_coverage(double out[8]) __attribute__((weak));
struct SpirvStats { uint32_t memory_hits, disk_hits, translations; };   /* the runtime's RsxSpirvCacheStats */
extern "C" void rsx_hlsl_to_spirv_cache_stats(SpirvStats* out) __attribute__((weak));

/* 1 answered (a change), 0 answered, -1 not one of these */
int diagnostics_command(const ppu_context* c, const Cmd& cmd, const std::string& w, std::string rest)
{
    (void)c;
    std::string why;
    if (w == "coverage") {
        const std::string op = word(rest);
        if (op == "export") {   /* the session's new pipelines with the maps they were met in, and their recipes */
            std::string name = word(rest);
            if (name.empty()) name = "coverage-" + std::to_string((long long)ft_clock());
            if (!safe_name(name)) { answer(cmd, false, "coverage export NAME: letters, digits, - and _"); return 0; }
            namespace fs = std::filesystem;
            std::error_code ec;
            const std::string dir = devmode_dir() + "/exports/" + name;
            fs::create_directories(dir, ec);
            size_t n = 0;
            if (FILE* f = fopen((dir + "/coverage.json").c_str(), "w")) {
                std::lock_guard<std::mutex> g(g_unf_mu);
                fprintf(f, "[");
                for (const Unforeseen& u : g_unforeseen) {
                    std::string map;
                    for (const MapInfo& m : g_maps) if (m.number == (int)u.area) map = m.name;
                    fprintf(f, "%s\n{\"key\":\"%016llX\",\"kind\":%s,\"ms\":%.2f,\"area\":%u,\"map\":%s,\"t\":%.3f}", n++ ? "," : "",
                            (unsigned long long)u.key, json_str(u.kind).c_str(), u.ms, u.area, json_str(map).c_str(), u.t);
                }
                fprintf(f, "\n]\n");
                fclose(f);
            }
            const char* rec = getenv("DOTHEROES_RECIPES");
            bool copied = false;
            if (rec && *rec && fs::exists(rec, ec)) {
                fs::copy_file(rec, dir + "/recipes.bin", fs::copy_options::overwrite_existing, ec);
                copied = !ec;
            }
            answer(cmd, true, "{\"dir\":" + json_str(fs::absolute(dir, ec).generic_string()) + ",\"pipelines\":" +
                              std::to_string(n) + ",\"recipes\":" + (copied ? "true" : "false") + "}", true);
            return 0;
        }
        double cv[8] = { 0 };
        if (ppfx_render_coverage) ppfx_render_coverage(cv);
        SpirvStats sp = { 0, 0, 0 };
        if (rsx_hlsl_to_spirv_cache_stats) rsx_hlsl_to_spirv_cache_stats(&sp);
        size_t nu;
        { std::lock_guard<std::mutex> g(g_unf_mu); nu = g_unforeseen.size(); }
        char b[600];
        snprintf(b, sizeof b,
                 "{\"built_prepared\":%.0f,\"built_unforeseen\":%.0f,\"prepared_ms\":%.1f,\"unforeseen_ms\":%.1f,"
                 "\"frames_held\":%.0f,\"longest_held_ms\":%.1f,\"prepared_for_gpu\":%.0f,\"warmed\":%.0f,"
                 "\"translations\":%u,\"translations_from_disk\":%u,\"translations_from_memory\":%u,\"unforeseen_listed\":%zu}",
                 cv[0], cv[1], cv[2], cv[3], cv[4], cv[5], cv[6], cv[7], sp.translations, sp.disk_hits, sp.memory_hits, nu);
        answer(cmd, true, b, true);
        return 0;
    }
    if (w == "perf") { answer(cmd, true, perf_json(), true); return 0; }
    if (w == "runtime") {   /* the native systems' reports, into the game's log now rather than at the exit */
        std::string o = "[";
        auto run = [&](void (*f)(void), const char* n) { if (f) { f(); o += std::string(o.size() > 1 ? "," : "") + json_str(n); } };
        run(phys_direct_report, "physics tasks");
        run(inflate_direct_report, "inflation");
        run(water_task_report, "water");
        run(waver_game_report, "waver");
        run(cellGcm_flip_wake_report, "flip waits");
        fflush(stderr);
        answer(cmd, true, "{\"written_to_the_log\":" + o + "]}", true);
        return 0;
    }
    if (w == "mark") {   /* a hitch mark, as F8 makes one (dev_state.cpp): the trace, the log, the screen */
        static unsigned n = 0;
        n++;
        const uint64_t t = ps3_ft_now();
        if (PS3_FT_ON()) ps3_ft_span(PS3_FT_MARK, 8, n, 0, t, t);
        fprintf(stderr, "[mark] %u at %.3f s (developer tools)\n", n, (double)(int64_t)(t - g_ps3_ft.base) / 1e9);
        char text[24];
        snprintf(text, sizeof text, "MARK %u", n);
        ps3_osd_post(text, 1500);
        answer(cmd, true, "{\"mark\":" + std::to_string(n) + "}", true);
        return 0;
    }
    if (w == "states") {
        std::string o = "[";
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(devmode_dir() + "/states", ec))
            if (de.is_directory()) o += std::string(o.size() > 1 ? "," : "") + json_str(de.path().filename().string());
        answer(cmd, true, o + "]", true);
        return 0;
    }
    if (w == "state") {   /* the test saves kept as a named state, or one put back (the game reads saves at its load) */
        const std::string op = word(rest), name = word(rest);
        if ((op != "keep" && op != "restore") || !safe_name(name)) { answer(cmd, false, "state keep|restore NAME"); return 0; }
        const std::string root = test_saves_root(why);
        if (root.empty()) { answer(cmd, false, why); return 0; }
        const std::string st = devmode_dir() + "/states/" + name + "/gamedata";
        if (op == "restore" && !std::filesystem::exists(st)) { answer(cmd, false, "no test state " + name); return 0; }
        if (!(op == "keep" ? replace_tree(root, st, why) : replace_tree(st, root, why))) { answer(cmd, false, why); return 0; }
        answer(cmd, true, "{\"state\":" + json_str(name) + ",\"" + (op == "keep" ? "kept" : "restored") +
                          "\":true,\"note\":" + json_str(op == "keep" ? "" : "load it from the title (CONTINUE)") + "}", true);
        return 0;
    }
    if (w == "saves") {   /* the player's saves copied into the test saves (only read; never the other way) */
        if (word(rest) != "copy-player") { answer(cmd, false, "saves copy-player"); return 0; }
        if (g_install_root.empty()) { answer(cmd, false, "not an installed game (a test has its own saves)"); return 0; }
        const std::string root = test_saves_root(why);
        if (root.empty()) { answer(cmd, false, why); return 0; }
        if (!replace_tree(g_install_root + "/data/gamedata", root, why)) { answer(cmd, false, why); return 0; }
        answer(cmd, true, "{\"copied\":\"the player's saves into the test saves\",\"note\":\"load them from the title\"}", true);
        return 0;
    }
    return -1;
}

/* true when the command changed the game (the next one waits for the next frame) */
bool execute(const ppu_context* c, const Cmd& cmd)
{
    std::string rest = cmd.line;
    const std::string w = word(rest);
    try {
        if (w == "ping") { answer(cmd, true, "pong"); return false; }
        {
            const int r = player_command(c, cmd, w, rest);
            if (r >= 0) return r == 1;
        }
        {
            const int r = progression_command(c, cmd, w, rest);
            if (r >= 0) return r == 1;
        }
        {
            const int r = encounter_command(c, cmd, w, rest);
            if (r >= 0) return r >= 1;
        }
        {
            const int r = diagnostics_command(c, cmd, w, rest);
            if (r >= 0) return r == 1;
        }
        if (w == "warp") return start_warp(c, cmd, rest) == 1;
        if (w == "maps" || w == "catalog") {
            std::string err;
            if (!make_catalog(c, err)) { answer(cmd, false, err); return false; }
            answer(cmd, true, maps_json(), true);
            return false;
        }
        if (w == "status") { answer(cmd, true, status_json(c), true); return false; }
        if (w == "peek") {   /* M11.1b: guest words (a boss's own fields, behind a script object's userdata) */
            long ea = 0, n = 1;
            const std::string at = word(rest);
            char* end = nullptr;
            ea = at.empty() ? 0 : (long)strtoul(at.c_str(), &end, 0);
            if (at.empty() || (end && *end) || ea < 0x10000 || ea >= 0x30000000 || (ea & 3) ||
                (rest.find_first_not_of(" \t") != std::string::npos && !number_arg(rest, n, 1, 64))) {
                answer(cmd, false, "peek ADDR [N]: N (1-64) words from a word-aligned guest address in 0x10000-0x2FFFFFFF");
                return false;
            }
            std::string o = "[";
            for (long k = 0; k < n; k++) {
                char b[16];
                snprintf(b, sizeof b, "%s%u", k ? "," : "", g32((uint32_t)(ea + 4 * k)));
                o += b;
            }
            answer(cmd, true, o + "]", true);
            return false;
        }
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

/* M11.2d: QUIT GAME's label -- the Title script's two menus' last entries (the PS3's INSTALL) renamed in the game's
 * own Lua state, in the full-width letters the title's other items use; checked every 30 flips (the title loads its
 * script again each time it opens; a chunk is refused while a game script runs: the next check). Our own words. */
extern "C" int g_dotheroes_quit_item;
const char kQuitLabelLua[] =
    "local Q='\xEF\xBC\xB1\xEF\xBC\xB5\xEF\xBC\xA9\xEF\xBC\xB4\xE3\x80\x80\xEF\xBC\xA7\xEF\xBC\xA1\xEF\xBC\xAD\xEF\xBC\xA5' "
    "local D='\xEF\xBC\xB1\xEF\xBD\x95\xEF\xBD\x89\xEF\xBD\x94\xE3\x80\x80\xEF\xBD\x94\xEF\xBD\x88\xEF\xBD\x85\xE3\x80\x80"
    "\xEF\xBD\x87\xEF\xBD\x81\xEF\xBD\x8D\xEF\xBD\x85\xEF\xBC\x8E' "
    "local t=Title local n=0 if type(t)=='table' then for _,m in ipairs({t.Menu,t.ClearMenu}) do "
    "if type(m)=='table' and type(m.Size)=='number' then local e=m[m.Size] "
    "if type(e)=='table' and e.MenuString~=Q then e.MenuString=Q e.DescriptionString=D n=n+1 end end end end return n";
void title_tick(ppu_context* c)
{
    static unsigned flips = 0;
    if (!g_dotheroes_quit_item || (++flips % 30) != 0) return;
    if (!(g_region || ensure_region(c, !g_on))) return;   /* developer mode off: the label's own page only */
    std::string out;
    try {
        if (run_lua(c, kQuitLabelLua, out) == 1 && out != "0")
            fprintf(stderr, "[title] QUIT GAME's label set (%s menu entr%s)\n", out.c_str(), out == "1" ? "y" : "ies");
    } catch (const CallError&) {}
}
void title_flip(ppu_context* c) { title_tick(c); }

void on_flip(ppu_context* c)
{
    title_tick(c);
    g_flip_thread.store(this_thread_key(), std::memory_order_relaxed);
    g_flip_ctx.store(c, std::memory_order_release);
    note_frame();
    check_warp();
    if (g_random_restore && --g_random_restore == 0 && (g_region || ensure_region(c))) {
        std::string out;
        try { run_lua(c, "if __devtools_random then math.random=__devtools_random; __devtools_random=nil end", out); }
        catch (const CallError&) {}
    }
    if (g_invulnerable.load(std::memory_order_relaxed) && (g_region || ensure_region(c))) {
        try { keep_invulnerable(c); } catch (const CallError&) {}
    }
    bool any;
    { std::lock_guard<std::mutex> g(g_mu); any = !g_queue.empty(); }
    if (any) run_queue(c);
}

/* the runtime's hold (the in-game menu open): where the held thread waits; only the main thread runs commands */
void on_hold()
{
    g_frames.held = true;
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

extern "C" int g_dev_changes_ok = 1;

extern "C" void devtools_gate(int installed)
{
    const char* e = getenv("DOTHEROES_DEVELOPER_MODE");
    if (!installed || (e && !strcmp(e, "1"))) return;
    g_dev_changes_ok = 0;
    std::string ignored;
    for (const char* k : { "DOTHEROES_DEV_CANT_DIE", "DOTHEROES_DEV_CREDITS", "DOTHEROES_DEV_WAVER", "DOTHEROES_DEV_SOUND" }) {
        const char* v = getenv(k);
        if (!v || !*v) continue;
        ignored += ' ';
        ignored += k;
#ifdef _WIN32
        _putenv_s(k, "");
#else
        unsetenv(k);
#endif
    }
    if (!ignored.empty())
        fprintf(stderr, "[dev-switches] developer mode is off: ignored in an installed game:%s\n", ignored.c_str());
}

/* the lift's entry observers (r1_observe.cpp), developer mode only: the player's damage (CPlayerActor::damageHP,
 * 0x0011A94C: life at +0x5B8 less r4; at 0 or below, the player dies). No death: a hit that would end the hero's
 * life takes all but one unit of it -- every other hit, the knockback and the enemies stay as the game makes them
 * (the owner's 2026-11-01 requirement). */
extern "C" void devtools_entry(uint32_t addr, ppu_context* ctx)
{
    if (addr != fn_damage_hp) return;
    const bool invuln = g_invulnerable.load(std::memory_order_relaxed);
    if (!invuln && !g_nodeath.load(std::memory_order_relaxed)) return;
    const uint32_t p = (uint32_t)ctx->gpr[3];
    if (!p || g32(p) != kPlayerVtable) return;
    const int32_t life = (int32_t)g32(p + kHp), hit = (int32_t)ctx->gpr[4];
    if (invuln && hit > 0) {   /* a hit past the game's own invincibility (its flag cleared as a blink ends): no damage */
        ctx->gpr[4] = 0;
        g_invuln_blocked++;
        return;
    }
    if (hit <= 0 || life - hit >= 1) return;
    const int32_t held = life > 1 ? life - 1 : 0;
    ctx->gpr[4] = (uint64_t)(uint32_t)held;
    g_nodeath_saves++;
    DLOG(DH_LOG_INFO, "dev", "no death: a hit of %d with %d life left held to %d", hit, life, held);
}

namespace devtools {

const std::vector<MapInfo>& maps() { return g_maps; }
bool catalog_made() { return g_catalog; }

bool run_sync(const std::string& line, std::string& out, int ms)
{
    struct Wait {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false, ok = false;
        std::string out;
    };
    auto* w = new Wait;   /* kept if the answer comes after the wait gives up (then it is the answer's to free) */
    devtools_submit(line.c_str(), [](void* u, int ok, const char* o) {
        Wait* w = (Wait*)u;
        std::unique_lock<std::mutex> g(w->mu);
        if (w->done) { g.unlock(); delete w; return; }   /* the waiter gave up: nobody reads it */
        w->ok = ok != 0;
        w->out = o ? o : "";
        w->done = true;
        w->cv.notify_all();
    }, w);
    std::unique_lock<std::mutex> g(w->mu);
    if (!w->cv.wait_for(g, std::chrono::milliseconds(ms), [&] { return w->done; })) {
        w->done = true;   /* the answer, when it comes, frees it */
        out = "no answer from the game";
        return false;
    }
    out = w->out;
    const bool ok = w->ok;
    g.unlock();
    delete w;
    return ok;
}

}  // namespace devtools

extern "C" uint64_t devtools_submit(const char* line, devtools_reply done, void* user)
{
    if (!g_on) { if (done) done(user, 0, "developer mode is off"); return 0; }
    Cmd c{ g_next++, "", line ? line : "", done, user, false };
    c.id = std::to_string(c.n);
    const uint64_t n = c.n;
    push(std::move(c));
    return n;
}

extern "C" void devtools_start(const char* install_root, const char* elf)
{
    const char* e = getenv("DOTHEROES_DEVELOPER_MODE");
    g_on = e && !strcmp(e, "1");
    if (!g_on) {   /* M11.2d: the flip still names QUIT GAME */
        if (g_dotheroes_quit_item) g_devtools_flip = title_flip;
        return;
    }
    g_install_root = install_root ? install_root : "";
    g_usrdir = elf ? std::filesystem::path(elf).parent_path().generic_string() : std::string();
    g_devtools_flip = on_flip;
    g_ps3_guest_hold_work = on_hold;
    g_ppfx_unforeseen_hook = on_unforeseen;
    ps3_osd_set_tag("DEVELOPER MODE - TEST SAVES");
    std::thread(file_loop).detach();
    DLOG(DH_LOG_INFO, "dev", "developer mode: the tools are on; saves in %s; commands from dev.cmd, answers in dev.out",
           getenv("PS3_GAMEDATA_ROOT") ? getenv("PS3_GAMEDATA_ROOT") : "(the runtime's default)");
}
