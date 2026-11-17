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
#include "ps3emu/ps3_ftrace.h"   /* the compile log's clock: [warp] lines line up with [vk-compile] (map_sweep.py) */

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
    uint32_t area0 = 0, frames0 = 0, last_area = 0;
    std::chrono::steady_clock::time_point t0, changed;
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
    g_warp.frames0 = g32(kFrames);
    g_warp.t0 = g_warp.changed = std::chrono::steady_clock::now();
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
    const double since = std::chrono::duration<double>(now - g_warp.t0).count();
    const double settled = std::chrono::duration<double>(now - g_warp.changed).count();
    const bool there = (int)area == g_warp.expected && settled >= 1.0;
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

/* true when the command changed the game (the next one waits for the next frame) */
bool execute(const ppu_context* c, const Cmd& cmd)
{
    std::string rest = cmd.line;
    const std::string w = word(rest);
    try {
        if (w == "ping") { answer(cmd, true, "pong"); return false; }
        if (w == "warp") return start_warp(c, cmd, rest) == 1;
        if (w == "maps" || w == "catalog") {
            std::string err;
            if (!make_catalog(c, err)) { answer(cmd, false, err); return false; }
            answer(cmd, true, maps_json(), true);
            return false;
        }
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
    check_warp();
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
    if (!g_on) return;
    g_install_root = install_root ? install_root : "";
    g_usrdir = elf ? std::filesystem::path(elf).parent_path().generic_string() : std::string();
    g_devtools_flip = on_flip;
    g_ps3_guest_hold_work = on_hold;
    ps3_osd_set_tag("DEVELOPER MODE - TEST SAVES");
    std::thread(file_loop).detach();
    DLOG(DH_LOG_INFO, "dev", "developer mode: the tools are on; saves in %s; commands from dev.cmd, answers in dev.out",
           getenv("PS3_GAMEDATA_ROOT") ? getenv("PS3_GAMEDATA_ROOT") : "(the runtime's default)");
}
