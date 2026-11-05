// Development aids for testing the game (docs/HANDOFF.md, "Debugging tools").
// All off by default; a test's run.json lists the ones it ran with.
//
// DOTHEROES_DEV_KEYS=1: hotkeys while the game's window has the focus --
//   F8, or L3+R3 on an XInput controller: a hitch mark. It goes into the
//     frame trace (PS3_FT_MARK, with PS3_FTRACE), the log ("[mark] <n> at <s>
//     s", on the trace's clock) and on screen for a moment ("MARK <n>"), so a
//     hitch felt while playing can be found in the trace (tests/slow_frames.py
//     --marks).
//   F7: the frame-rate readout off / text / text and graph (PS3_OSD_FPS sets
//     it at start; include/ps3emu/osd.h).
//   A drive script can mark too: `touch mark.trigger` in the run directory
//     (a segment's start or end, say).
//   A drive script can set the hero's life: life.trigger holding a number of
//     half apples (empty: 1), so the next hit ends the game -- the way to the
//     game-over screen and its save (tests/scripts/save_game.drive). Needs the
//     player found (DOTHEROES_DEV_STATE=1 or another switch that looks).
//
// DOTHEROES_DEV_MEMSNAP=1: a thread watches the game's working directory (a
// test run's directory) for memsnap.trigger; when a drive script touches it,
// guest memory is written to memsnap_<n>.bin (cellSpurs's snapshot format:
// game memory, keep it local) and the trigger removed. The game keeps running
// while memory is copied (a few tens of ms; the log gives the copy's start and
// end on the trace's clock), so a value changing then may be torn. Used to
// find where the game keeps state a test checks (tests/native/find_state.cpp)
// and how the game's clocks run at different frame rates (tests/fps_check.py).
//
// DOTHEROES_DEV_PLAYER_LOG=1: the player's object every 4 ms, into player.bin
// (player_log_loop below; tests/fps_check.py).
//
// DOTHEROES_DEV_WATCH=<hex offset>: the guest functions storing to the player's
// object from <offset>, once a drive script touches watch.trigger (watch_loop).
//
// DOTHEROES_DEV_ACTOR_LOG=1: the positions of the characters near the hero
// every 4 ms, into actors.bin (actor_log_loop below; tests/actor_steps.py).
//
// DOTHEROES_DEV_STATE=1: ten times a second, the game's state a test checks --
// the hero's life (half apples), the camera target's x and z (it follows the
// hero in the area's own coordinates, clamped at its edges), physics collision
// tasks so far (a step has one per 64 pairs; dotheroes_phys_steps), can't-die
// top-ups so far, the field area's number -- into state.txt in
// the working directory ("t_s life x z steps topups area", replaced whole each
// time; tests/drive.py's `state` waits on it) and, with PS3_FTRACE, the frame
// trace (PS3_FT_GAME_STATE).
//
// DOTHEROES_DEV_CANT_DIE=1: keeps the hero alive by topping life up below
// full. Every 2 ms, a life of 8 half apples or less (4 apples) is raised to 9
// (4.5 of the save's 5) -- the player's own life and the HUD's copies: hits
// still land and knock back, enemies are untouched, and life is never full --
// a full life keeps the sword beam, which would change the fight. This heals:
// it does not only stop the lethal hit (that would need the game's damage
// path), so the hero spends a fight near 4-4.5 apples. Each top-up is logged
// and counted. Safe while no hit takes 9 half apples or more, or two land
// within 2 ms: the river's enemies hit for 2 to 7. =<floor>,<refill> sets
// other values. Every arm of a comparison uses the same setting; a test
// reports runs with it apart and checks the result again with normal damage.
// DOTHEROES_DEV_LIFE_LOG=1 logs every change of the life values.
//
// Where the game keeps them (BLUS-30490): the player's life at +0x5B8 of its
// object (class vtable 0x00FE3D20, in the heap, found by scanning); the HUD's
// copies at 0x02B38E0C and +4, rewritten from it every few frames
// (func_005C6280, found with the runtime's PPU_WWATCH); the camera target's x
// and z at 0x029B0E18, 0x029B0E20; the field area at 0x029A6488 (17 -> 18
// crossing from the castle's area to the river's). Found with
// tests/native/find_state.cpp on snapshots; u32 and float, big-endian.
#include "dev_state.h"

#ifdef DOTHEROES_BULLET_CD
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

#include "ps3emu/osd.h"
#include "ps3emu/ps3_ftrace.h"
extern "C" void rsx_vulkan_cache_save(void);   // libs/video/rsx_vulkan_backend.h

extern "C" void spurs_snap_write_file(const char* path);
extern "C" uint8_t* vm_base;
// Work a development tool needs done before a test's stop ends the process
// (no exit handlers run then): the native PPFX recorder's last report, after
// its renderer has finished what it was given (ppfx_recorder.cpp).
extern "C" { void (*g_dotheroes_stop_hook)(void) = nullptr; }

namespace {

constexpr uint32_t kLife = 0x02B38E0C, kLifeShown = 0x02B38E10, kCameraX = 0x029B0E18, kCameraZ = 0x029B0E20;
constexpr uint32_t kArea = 0x029A6488;   // the field area's number: 17 the castle's, 18 the river's east of it
uint32_t g_low_life = 8, g_top_up = 9;   // DOTHEROES_DEV_CANT_DIE=1, or =<floor>,<refill> (half apples)

uint32_t rd32(uint32_t ea)
{
    const volatile uint8_t* p = vm_base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void wr32(uint32_t ea, uint32_t v)
{
    volatile uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
float rdf(uint32_t ea)
{
    const uint32_t u = rd32(ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

std::atomic<unsigned> g_topups{ 0 };
bool g_cant_die, g_life_log;   // DOTHEROES_DEV_CANT_DIE, DOTHEROES_DEV_LIFE_LOG

// The player's object: its life at +0x5B8 is the real one; the HUD's two words
// above are copies the game rewrites from it every few frames (func_005C6280,
// found with PPU_WWATCH), so a value written only there does not last. Found by
// its class (vtable 0x00FE3D20) with +0x5B8 equal to the HUD's life, in the heap
// (0x10000000-0x20000000); looked for again if it stops being of that class.
constexpr uint32_t kPlayerVtable = 0x00FE3D20, kPlayerLife = 0x5B8;
std::atomic<uint32_t> g_player{ 0 };

uint32_t find_player(uint32_t hud_life)
{
#ifdef _WIN32
    const uint8_t want[4] = { 0x00, 0xFE, 0x3D, 0x20 };
    for (uint64_t ea = 0x10000000u; ea < 0x20000000u; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + ea, &mbi, sizeof mbi)) break;
        const uint64_t len = (uint64_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)(vm_base + ea));
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && len > kPlayerLife + 4)
            for (uint64_t o = 0; o + kPlayerLife + 4 <= len; o += 4)
                if (!memcmp(vm_base + ea + o, want, 4) && rd32((uint32_t)(ea + o + kPlayerLife)) == hud_life)
                    return (uint32_t)(ea + o);
        ea += len;
    }
#endif
    (void)hud_life;
    return 0;
}

// The player's life: the real one when the player is known, else the HUD's.
uint32_t player_life()
{
    const uint32_t p = g_player.load();
    return p && rd32(p) == kPlayerVtable ? rd32(p + kPlayerLife) : rd32(kLife);
}

// Every 2 ms: keep the player found; with DOTHEROES_DEV_LIFE_LOG=1, each change
// of the real life or the HUD's copies logged ("[life] <s> <real> <hud> <hud2>");
// with can't-die, a low life topped up -- the real one and the HUD's.
void life_loop()
{
    uint32_t seen[3] = { ~0u, ~0u, ~0u };
    for (unsigned tick = 0;; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const uint32_t hud = rd32(kLife);
        uint32_t p = g_player.load();
        if ((!p || rd32(p) != kPlayerVtable) && hud && tick % 250 == 0) {   // in play, no player known: every 0.5 s
            p = find_player(hud);
            if (p) fprintf(stderr, "[dev-state] the player's object at %08X\n", p);
            g_player = p;
        }
        const uint32_t life = player_life();
        if (g_life_log) {
            const uint32_t now[3] = { life, hud, rd32(kLifeShown) };
            if (memcmp(now, seen, sizeof now))
                fprintf(stderr, "[life] %.3f %u %u %u\n", (double)(int64_t)(ps3_ft_now() - g_ps3_ft.base) / 1e9, now[0], now[1], now[2]);
            memcpy(seen, now, sizeof now);
        }
        if (!g_cant_die || !p || life == 0 || life > g_low_life) continue;   // 0: not in play (a title screen, a load)
        wr32(p + kPlayerLife, g_top_up);
        wr32(kLife, g_top_up);
        wr32(kLifeShown, g_top_up);
        const unsigned n = ++g_topups;
        fprintf(stderr, "[cant-die] life %u half apples raised to %u (%u so far) at %.3f s\n", life, g_top_up, n,
                (double)(int64_t)(ps3_ft_now() - g_ps3_ft.base) / 1e9);
    }
}

// The characters near the hero: objects in the heap (0x10000000-0x20000000)
// with a class vtable at +0 in the game's .data (0x00F00000-0x01100000) and a
// position at +0x428/+0x42C/+0x430 within `radius` of the hero's (and 20
// units in height), up to `max`, the player's own object left out. A pass over
// the committed heap (tens of ms, on the calling thread).
struct Near { uint32_t ea, vt; float x, y, z; };

int scan_near(uint32_t p, float radius, Near* out, int max)
{
    int n = 0;
#ifdef _WIN32
    const float px = rdf(p + 0x428), py = rdf(p + 0x42C), pz = rdf(p + 0x430);
    for (uint64_t ea = 0x10000000u; ea < 0x20000000u && n < max; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + ea, &mbi, sizeof mbi)) break;
        const uint64_t len = (uint64_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)(vm_base + ea));
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && len > 0x440)
            for (uint64_t o = 0; o + 0x440 <= len && n < max; o += 16) {
                const uint32_t a = (uint32_t)(ea + o), vt = rd32(a);
                if (vt < 0x00F00000u || vt >= 0x01100000u || a == p) continue;
                const float x = rdf(a + 0x428), y = rdf(a + 0x42C), z = rdf(a + 0x430);
                if (!(fabsf(x - px) < radius && fabsf(z - pz) < radius && fabsf(y - py) < 20.0f)) continue;
                if (x == 0.0f && z == 0.0f) continue;
                out[n++] = { a, vt, x, y, z };
            }
        ea += len;
    }
#else
    (void)p; (void)radius; (void)out; (void)max;
#endif
    return n;
}

// actors.trigger (a drive's `actors` step): the characters within 40 units of
// the hero counted now, into actors.txt -- "n=<all> c<vtable>=<count>
// hp<vtable>=<summed +0x5B8> ...": the player's life field, which characters
// share the layout of. Read by tests/drive.py at a segment's ends, outside
// the measured frames (the scan reads the whole heap).
void actors_now(uint32_t p)
{
    static Near found[256];
    const int n = p && rd32(p) == kPlayerVtable ? scan_near(p, 40.0f, found, 256) : 0;
    struct Cls { uint32_t vt, count; uint64_t hp; };
    std::vector<Cls> cls;
    for (int i = 0; i < n; i++) {
        size_t k = 0;
        while (k < cls.size() && cls[k].vt != found[i].vt) k++;
        if (k == cls.size()) cls.push_back({ found[i].vt, 0, 0 });
        cls[k].count++;
        const uint32_t hp = rd32(found[i].ea + kPlayerLife);
        cls[k].hp += hp < 0x10000 ? hp : 0;
    }
    if (FILE* f = fopen("actors.tmp", "w")) {
        fprintf(f, "n=%d", n);
        for (const Cls& c : cls) fprintf(f, " c%X=%u hp%X=%llu", c.vt, c.count, c.vt, (unsigned long long)c.hp);
        fprintf(f, "\n");
        // Then each one with life left: "a <vtable> <x> <z> <life>" (a drive's
        // `fight` walks to the nearest of a class and swings).
        for (int i = 0; i < n; i++) {
            const uint32_t hp = rd32(found[i].ea + kPlayerLife);
            if (hp && hp < 0x10000) fprintf(f, "a %X %.2f %.2f %u\n", found[i].vt, found[i].x, found[i].z, hp);
        }
        // And every one, life or not: "o <vtable> <x> <y> <z>" (chests,
        // switches, doors: what a route has to walk up to; `actors list`).
        for (int i = 0; i < n; i++)
            fprintf(f, "o %X %.2f %.2f %.2f\n", found[i].vt, found[i].x, found[i].y, found[i].z);
        fclose(f);
        remove("actors.txt");
        rename("actors.tmp", "actors.txt");
    }
}

// The guest's frame counter (one of the words that count a frame each,
// tests/fps_check.py): how far the game itself got, whatever the host did.
constexpr uint32_t kFrames = 0x02CF3570;
// Two words that change when the system menu opens (find_state differ on
// tests/scripts/find_menu.drive's snapshots: the same in four snapshots in
// play, the same in two with the menu open): 3 in play, 1 in the menu; 11 in
// play, 3 in the menu. Reported raw ("menu", "scene") until the other screens'
// values are known (docs/HANDOFF.md, "Debugging tools").
constexpr uint32_t kMenuWord = 0x02B399C0, kSceneWord = 0x02B399A4;

void state_loop()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint32_t life = player_life(), topups = g_topups.load(), area = rd32(kArea);
        const float x = rdf(kCameraX), z = rdf(kCameraZ);
        const unsigned long long steps = dotheroes_phys_steps();
        const uint64_t t = ps3_ft_now();
        const uint32_t p = g_player.load();
        const bool known = p && rd32(p) == kPlayerVtable;
        const float hx = known ? rdf(p + 0x428) : 0.0f, hy = known ? rdf(p + 0x42C) : 0.0f,
                    hz = known ? rdf(p + 0x430) : 0.0f;
        if (PS3_FT_ON()) {
            uint32_t xb, zb;
            memcpy(&xb, &x, 4); memcpy(&zb, &z, 4);
            ps3_ft_span(PS3_FT_GAME_STATE, (life & 0xFF) | (area & 0xFF) << 8 | (topups < 0xFFFF ? topups : 0xFFFF) << 16,
                        xb, zb, t, t);
        }
        if (FILE* f = fopen("state.tmp", "w")) {
            fprintf(f, "%.3f %u %.2f %.2f %llu %u %u %.2f %.2f %.2f %u %u %u\n", (double)(int64_t)(t - g_ps3_ft.base) / 1e9,
                    life, x, z, steps, topups, area, hx, hy, hz, rd32(kMenuWord), rd32(kSceneWord), rd32(kFrames));
            fclose(f);
            remove("state.txt");
            rename("state.tmp", "state.txt");
        }
        if (FILE* a = fopen("actors.trigger", "rb")) {
            fclose(a);
            actors_now(p);
            remove("actors.trigger");
        }
    }
}

// DOTHEROES_DEV_PLAYER_LOG=1 (or =<bytes>): every 4 ms while the player's object
// is known, its first 0x1000 bytes (or <bytes>) appended to player.bin -- records
// of (u64 ns on the trace's clock, u32 ea, u32 size, little-endian) then the
// object's bytes as the guest has them -- for timing what the hero does at
// different frame rates (tests/fps_check.py). About 1 MB a second; flushed
// every half second, since a test's end kills the game.
uint32_t g_player_log_bytes;

void player_log_loop()
{
    FILE* f = fopen("player.bin", "wb");
    if (!f) return;
    std::vector<uint8_t> buf(g_player_log_bytes);
    for (unsigned tick = 1;; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const uint32_t p = g_player.load();
        if (p && rd32(p) == kPlayerVtable) {
            const uint64_t t = ps3_ft_now() - g_ps3_ft.base;
            const uint32_t n = g_player_log_bytes;
            memcpy(buf.data(), vm_base + p, n);
            fwrite(&t, 8, 1, f); fwrite(&p, 4, 1, f); fwrite(&n, 4, 1, f); fwrite(buf.data(), 1, n, f);
        }
        if (tick % 125 == 0) fflush(f);
    }
}

// DOTHEROES_DEV_ACTOR_LOG=1: the other characters near the hero -- enemies,
// say -- for timing their movement at different frame rates
// (tests/actor_steps.py). Characters share the player's layout: a class
// vtable at +0 (the game's .data, 0x00F00000-0x01100000) and the position at
// +0x428/+0x42C/+0x430, which the movers write (func_000A7C78). About once a
// second the heap is scanned for such objects within 40 units of the hero
// (up to 64; the player's own object left out); every 4 ms each one's
// position, when it changed, is appended to actors.bin: records of (u64 ns
// on the trace's clock, u32 ea, u32 vtable, f32 x, y, z), little-endian.
// Flushed every half second.
constexpr int kMaxActors = 64;

void actor_log_loop()
{
    FILE* f = fopen("actors.bin", "wb");
    if (!f) return;
    Near actors[kMaxActors];
    int n = 0;
    for (unsigned tick = 0;; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const uint32_t p = g_player.load();
        if (!p || rd32(p) != kPlayerVtable) continue;
        if (tick % 250 == 0) n = scan_near(p, 40.0f, actors, kMaxActors);   // rescan
        const uint64_t t = ps3_ft_now() - g_ps3_ft.base;
        for (int i = 0; i < n; i++) {
            Near& a = actors[i];
            if (rd32(a.ea) != a.vt) continue;
            const float x = rdf(a.ea + 0x428), y = rdf(a.ea + 0x42C), z = rdf(a.ea + 0x430);
            if (x == a.x && y == a.y && z == a.z) continue;
            a.x = x; a.y = y; a.z = z;
            fwrite(&t, 8, 1, f); fwrite(&a.ea, 4, 1, f); fwrite(&a.vt, 4, 1, f);
            fwrite(&x, 4, 1, f); fwrite(&y, 4, 1, f); fwrite(&z, 4, 1, f);
        }
        if (tick % 125 == 0) fflush(f);
    }
}

// DOTHEROES_DEV_WATCH=<hex offset>: who writes a field of the player's object.
// When a drive script touches watch.trigger (the player known), the runtime's
// second store watch -- g_barrier_sync_watch, which logs stores to its base +
// 0x40..0xC0 as "[sync-write] +<off> <- <value> (w<width>) guest-fn=<fn>", the
// first 48 -- is set to cover 0x80 bytes of the object from <offset>: the
// printed +<off> is 0x40 above <offset>'s. The lifted code's inline stores do
// not look at that watch, so with DOTHEROES_DEV_WATCH set they are turned off
// before the guest starts (vm_inline_st_ok, as the runtime's own store
// diagnostics do): every store then reaches the runtime's hooks, which read the
// watch on each one, and it can be armed from this thread at any time.
extern "C" uint32_t g_barrier_sync_watch;
extern "C" int vm_inline_st_ok;
uint32_t g_watch_offset;

void watch_loop()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const uint32_t p = g_player.load();
        FILE* t = p ? fopen("watch.trigger", "rb") : nullptr;
        if (!t) continue;
        fclose(t);
        remove("watch.trigger");
        g_barrier_sync_watch = p + g_watch_offset - 0x40;
        fprintf(stderr, "[dev-state] watching stores to the player's +0x%X..+0x%X (%08X) at %.3f s\n", g_watch_offset,
                g_watch_offset + 0x80, p + g_watch_offset, (double)(int64_t)(ps3_ft_now() - g_ps3_ft.base) / 1e9);
    }
}

#ifdef _WIN32
// The whole committed guest space (cellSpurs's writer stops at 0x20000000; the
// game also keeps state in the system VM and HLE heap areas above it), in the
// same format: records of (u32 ea, u32 size, little-endian) then the bytes.
// Copied in memory first, so the snapshot spans the copy (a few tens of ms;
// its start and end on the trace's clock in t0, t1), not the file's writing.
void memsnap_write(const char* path, uint64_t& t0, uint64_t& t1)
{
    struct Rec { uint32_t ea, len; size_t at; };
    std::vector<Rec> recs;
    std::vector<uint8_t> bytes;
    t0 = ps3_ft_now();
    for (uint64_t ea = 0x10000u; ea < 0xF0000000u; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + ea, &mbi, sizeof mbi)) break;
        uint64_t len = (uint64_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)(vm_base + ea));
        if (ea + len > 0xF0000000u) len = 0xF0000000u - ea;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && len < 0xFFFFFFFFu) {
            recs.push_back({ (uint32_t)ea, (uint32_t)len, bytes.size() });
            bytes.insert(bytes.end(), vm_base + ea, vm_base + ea + len);
        }
        ea += len;
    }
    t1 = ps3_ft_now();
    FILE* f = fopen(path, "wb");
    if (!f) return;
    for (const Rec& r : recs) {
        fwrite(&r.ea, 4, 1, f); fwrite(&r.len, 4, 1, f); fwrite(bytes.data() + r.at, 1, r.len, f);
    }
    fclose(f);
}
#endif

void memsnap_loop()
{
    for (unsigned n = 0;; ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        FILE* t = fopen("memsnap.trigger", "rb");
        if (!t) continue;
        fclose(t);
        char path[64];
        snprintf(path, sizeof path, "memsnap_%02u.bin", n++);
        uint64_t t0 = ps3_ft_now(), t1 = t0;
#ifdef _WIN32
        memsnap_write(path, t0, t1);
#else
        spurs_snap_write_file(path);
        t1 = ps3_ft_now();
#endif
        remove("memsnap.trigger");
        fprintf(stderr, "[memsnap] %s at %.3f-%.3f s\n", path, (double)(int64_t)(t0 - g_ps3_ft.base) / 1e9,
                (double)(int64_t)(t1 - g_ps3_ft.base) / 1e9);
    }
}

// DOTHEROES_DEV_STOP=1 (tests/boot_smoke.py sets it): stop.trigger in the
// working directory ends the game the way a test wants -- the frame log
// (DIAG_FRAMES) and the frame trace (PS3_FTRACE) written first, which a
// harness's TerminateProcess would lose (they are otherwise written at their
// set time or at exit) -- then the process ends with status 0 without running
// exit handlers (other threads are mid-frame).
void stop_loop()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        FILE* t = fopen("stop.trigger", "rb");
        if (!t) continue;
        fclose(t);
        remove("stop.trigger");
        fprintf(stderr, "[dev-state] stop.trigger at %.3f s: writing the logs, then exiting\n",
                (double)(int64_t)(ps3_ft_now() - g_ps3_ft.base) / 1e9);
        if (__atomic_load_n(&g_ps3_osd_log.on, __ATOMIC_ACQUIRE) == 1) ps3_osd_log_write();
        if (__atomic_load_n(&g_ps3_ft.on, __ATOMIC_ACQUIRE) == 2) ps3_ft_dump();
        rsx_vulkan_cache_save();   // the shader cache's pipelines (a no-op without one)
        if (g_dotheroes_stop_hook) g_dotheroes_stop_hook();
        fflush(stdout);
        fflush(stderr);
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0);
#else
        _Exit(0);
#endif
    }
}

// DOTHEROES_DEV_RECORD=1: a play session's record for deriving a test route,
// record.log, one event a line on the trace's clock: "<s> in <PS3 button mask>
// <stick x> <stick y>" at every change of the controller (XInput, mapped as
// cellPad maps SDL's: A CROSS, B CIRCLE, X SQUARE, Y TRIANGLE, the stick 0-255
// with 128 centred, changes under 16 ignored) or of the keyboard's game keys
// while the window has the focus; "<s> area <n> <x> <y> <z>" on entering a field
// area, "<s> life <n>" when the life changes, "<s> pos <area> <x> <y> <z>" every
// second while the hero is known (the player object's position), "<s> mark <n>"
// for each hitch mark. Replaying the inputs would not reproduce a session
// (enemies, loads and timing vary); the positions and areas are the route.
std::mutex g_record_mu;
FILE* g_record;

void record_line(const char* fmt, ...)
{
    if (!g_record) return;
    std::lock_guard<std::mutex> lock(g_record_mu);
    fprintf(g_record, "%.3f ", (double)(int64_t)(ps3_ft_now() - g_ps3_ft.base) / 1e9);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_record, fmt, ap);
    va_end(ap);
    fputc('\n', g_record);
}

#ifdef _WIN32
bool our_window_focused();

uint32_t keyboard_mask()
{
    static const struct { int vk; uint32_t btn; } map[] = {   // cellPad's keyboard map
        { VK_UP, 0x0010 }, { VK_DOWN, 0x0040 }, { VK_LEFT, 0x0080 }, { VK_RIGHT, 0x0020 }, { 'Z', 0x4000 },
        { 'X', 0x2000 }, { 'A', 0x8000 }, { 'S', 0x1000 }, { 'Q', 0x0400 }, { 'W', 0x0800 }, { '1', 0x0100 },
        { '2', 0x0200 }, { VK_RETURN, 0x0008 }, { VK_TAB, 0x0001 },
    };
    uint32_t m = 0;
    if (our_window_focused())
        for (const auto& k : map)
            if (GetAsyncKeyState(k.vk) & 0x8000) m |= k.btn;
    return m;
}
#endif

void record_loop()
{
#ifdef _WIN32
    typedef DWORD(WINAPI * GetStateFn)(DWORD, XINPUT_STATE*);
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    const GetStateFn get_state = xi ? (GetStateFn)(void*)GetProcAddress(xi, "XInputGetState") : nullptr;
    static const struct { WORD x; uint32_t btn; } pad_map[] = {
        { XINPUT_GAMEPAD_DPAD_UP, 0x0010 }, { XINPUT_GAMEPAD_DPAD_DOWN, 0x0040 }, { XINPUT_GAMEPAD_DPAD_LEFT, 0x0080 },
        { XINPUT_GAMEPAD_DPAD_RIGHT, 0x0020 }, { XINPUT_GAMEPAD_START, 0x0008 }, { XINPUT_GAMEPAD_BACK, 0x0001 },
        { XINPUT_GAMEPAD_LEFT_THUMB, 0x0002 }, { XINPUT_GAMEPAD_RIGHT_THUMB, 0x0004 },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, 0x0400 }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, 0x0800 },
        { XINPUT_GAMEPAD_A, 0x4000 }, { XINPUT_GAMEPAD_B, 0x2000 }, { XINPUT_GAMEPAD_X, 0x8000 },
        { XINPUT_GAMEPAD_Y, 0x1000 },
    };
#endif
    uint32_t last_mask = ~0u, last_area = ~0u, last_life = ~0u;
    int last_x = -100, last_y = -100;
    for (unsigned tick = 0;; tick++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint32_t mask = 0;
        int x = 128, y = 128;
#ifdef _WIN32
        for (DWORD u = 0; get_state && u < 4; u++) {
            XINPUT_STATE s;
            if (get_state(u, &s) != ERROR_SUCCESS) continue;
            for (const auto& m : pad_map)
                if (s.Gamepad.wButtons & m.x) mask |= m.btn;
            if (s.Gamepad.bLeftTrigger > 23) mask |= 0x0100;
            if (s.Gamepad.bRightTrigger > 23) mask |= 0x0200;
            x = (s.Gamepad.sThumbLX + 32768) >> 8;
            y = (32767 - s.Gamepad.sThumbLY) >> 8;
            break;
        }
        mask |= keyboard_mask();
#endif
        if (mask != last_mask || abs(x - last_x) >= 16 || abs(y - last_y) >= 16) {
            record_line("in %04X %d %d", mask, x, y);
            last_mask = mask; last_x = x; last_y = y;
        }
        if (tick % 10) continue;
        const uint32_t p = g_player.load();
        const bool known = p && rd32(p) == kPlayerVtable;
        const uint32_t area = rd32(kArea), life = player_life();
        const float hx = known ? rdf(p + 0x428) : 0.0f, hy = known ? rdf(p + 0x42C) : 0.0f,
                    hz = known ? rdf(p + 0x430) : 0.0f;
        if (area != last_area) record_line("area %u %.2f %.2f %.2f", area, hx, hy, hz);
        if (life != last_life) record_line("life %u", life);
        last_area = area; last_life = life;
        if (tick % 100 == 0) {
            if (known) record_line("pos %u %.2f %.2f %.2f", area, hx, hy, hz);
            std::lock_guard<std::mutex> lock(g_record_mu);
            fflush(g_record);
        }
    }
}

#ifdef _WIN32
void mark(unsigned& n, uint32_t who)
{
    n++;
    const uint64_t t = ps3_ft_now();
    if (PS3_FT_ON()) ps3_ft_span(PS3_FT_MARK, who, n, 0, t, t);
    record_line("mark %u", n);
    fprintf(stderr, "[mark] %u at %.3f s (%s)\n", n, (double)(int64_t)(t - g_ps3_ft.base) / 1e9,
            who == 1 ? "F8" : who == 2 ? "L3+R3" : "mark.trigger");
    char text[16];
    snprintf(text, sizeof text, "MARK %u", n);
    ps3_osd_post(text, 1500);
}

bool our_window_focused()
{
    DWORD pid = 0;
    const HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

void keys_loop()
{
    typedef DWORD(WINAPI * GetStateFn)(DWORD, XINPUT_STATE*);
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    const GetStateFn get_state = xi ? (GetStateFn)(void*)GetProcAddress(xi, "XInputGetState") : nullptr;
    const WORD sticks = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
    bool f8 = false, f7 = false, pad = false;
    unsigned n = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (FILE* t = fopen("mark.trigger", "rb")) {
            fclose(t);
            remove("mark.trigger");
            mark(n, 3);
        }
        const uint32_t p = g_player.load();
        FILE* lt = p && rd32(p) == kPlayerVtable ? fopen("life.trigger", "rb") : nullptr;
        if (lt) {
            unsigned life = 0;
            if (fscanf(lt, "%u", &life) != 1 || !life) life = 1;
            fclose(lt);
            remove("life.trigger");
            wr32(p + kPlayerLife, life);
            wr32(kLife, life);
            wr32(kLifeShown, life);
            fprintf(stderr, "[dev-state] life set to %u half apples at %.3f s\n", life,
                    (double)(int64_t)(ps3_ft_now() - g_ps3_ft.base) / 1e9);
        }
        if (!our_window_focused()) { f8 = f7 = pad = false; continue; }
        const bool f8_now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0, f7_now = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        bool pad_now = false;
        for (DWORD u = 0; get_state && u < 4 && !pad_now; u++) {
            XINPUT_STATE s;
            pad_now = get_state(u, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & sticks) == sticks;
        }
        if (f8_now && !f8) mark(n, 1);
        if (pad_now && !pad) mark(n, 2);
        if (f7_now && !f7) ps3_osd_fps_set_mode((ps3_osd_fps_mode() + 1) % 3);
        f8 = f8_now; f7 = f7_now; pad = pad_now;
    }
}
#endif

// A loop that reads guest memory, started once there is some: main() reserves
// it (alloc_guest_vm) after dev_state_init, and a read of vm_base + ea before
// that is a read of ea itself -- life_loop's first, 2 ms in, faulted on
// 0x02B38E0C when the reservation came later than that.
void start_reader(void (*loop)())
{
    std::thread([loop] {
        while (!__atomic_load_n(&vm_base, __ATOMIC_ACQUIRE))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        loop();
    }).detach();
}

}  // namespace

void dev_state_init()
{
    (void)PS3_FT_ON();   // the trace's clock set before the first record (it is set lazily, at its first use)
    const char* st = getenv("DOTHEROES_DEV_STATE");
    if (st && *st == '1') {
        start_reader(state_loop);
        fprintf(stderr, "[dev-state] state.txt: t_s life x z steps topups area hx hy hz menu scene frames, every 100 ms; "
                        "actors.trigger -> actors.txt\n");
    }
    const char* cd = getenv("DOTHEROES_DEV_CANT_DIE");
    unsigned lo = 0, up = 0;
    if (cd && sscanf(cd, "%u,%u", &lo, &up) == 2 && lo > 0 && up > lo) {
        g_low_life = lo;
        g_top_up = up;
    }
    g_cant_die = cd && (*cd == '1' || up > lo);
    const char* ll = getenv("DOTHEROES_DEV_LIFE_LOG");
    g_life_log = ll && *ll == '1';
    const char* pl = getenv("DOTHEROES_DEV_PLAYER_LOG");
    if (pl && *pl && *pl != '0') {
        const unsigned long n = strtoul(pl, nullptr, 0);
        g_player_log_bytes = n > 1 ? (uint32_t)(n + 3) / 4 * 4 : 0x1000;
        start_reader(player_log_loop);
        fprintf(stderr, "[dev-state] player.bin: the player's first 0x%X bytes every 4 ms\n", g_player_log_bytes);
    }
    const char* wa = getenv("DOTHEROES_DEV_WATCH");
    if (wa && *wa) {
        g_watch_offset = (uint32_t)strtoul(wa, nullptr, 16) & ~3u;
        vm_inline_st_ok = 0;   // every store out of line, where the watch is read (watch_loop)
        start_reader(watch_loop);
        fprintf(stderr, "[dev-state] watch.trigger: watch stores to the player's +0x%X..+0x%X\n", g_watch_offset,
                g_watch_offset + 0x80);
    }
    const char* al = getenv("DOTHEROES_DEV_ACTOR_LOG");
    const bool actor_log = al && *al == '1';
    if (actor_log) {
        start_reader(actor_log_loop);
        fprintf(stderr, "[dev-state] actors.bin: characters near the hero, every 4 ms\n");
    }
    const char* rc = getenv("DOTHEROES_DEV_RECORD");
    if (rc && *rc == '1' && (g_record = fopen("record.log", "w")) != nullptr) {
        start_reader(record_loop);
        fprintf(stderr, "[dev-state] record.log: inputs, areas, life, the hero's position every second, marks\n");
    }
    const char* sp = getenv("DOTHEROES_DEV_STOP");
    if (sp && *sp == '1') std::thread(stop_loop).detach();
    if (g_cant_die || g_life_log || (st && *st == '1') || g_player_log_bytes || (wa && *wa) || actor_log || g_record)
        start_reader(life_loop);   // also finds the player, for state.txt, player.bin and the watch
    if (g_cant_die) {
        fprintf(stderr, "[cant-die] on (development only): life of %u half apples or less raised to %u\n", g_low_life, g_top_up);
    }
    const char* e = getenv("DOTHEROES_DEV_MEMSNAP");
    if (e && *e == '1') {
        start_reader(memsnap_loop);
        fprintf(stderr, "[memsnap] watching for memsnap.trigger\n");
    }
#ifdef _WIN32
    const char* k = getenv("DOTHEROES_DEV_KEYS");
    if (k && *k == '1') {
        std::thread(keys_loop).detach();
        fprintf(stderr, "[dev-keys] F8 or L3+R3: mark a hitch; F7: frame-rate readout off / text / graph\n");
    }
#endif
}
#else
void dev_state_init() {}
#endif
