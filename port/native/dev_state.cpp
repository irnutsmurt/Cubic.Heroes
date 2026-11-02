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
//
// DOTHEROES_DEV_MEMSNAP=1: a thread watches the game's working directory (a
// test run's directory) for memsnap.trigger; when a drive script touches it,
// guest memory is written to memsnap_<n>.bin (cellSpurs's snapshot writer:
// game memory, keep it local) and the trigger removed. The game keeps running
// while the snapshot is written, so a value changing then may be torn. Used to
// find where the game keeps state a test checks (tests/native/find_state.cpp).
//
// DOTHEROES_DEV_STATE=1: ten times a second, the game's state a test checks --
// the hero's life (half apples), the camera target's x and z (it follows the
// hero in the area's own coordinates, clamped at its edges), physics steps so
// far, can't-die top-ups so far, the field area's number -- into state.txt in
// the working directory ("t_s life x z steps topups area", replaced whole each
// time; tests/drive.py's `state` waits on it) and, with PS3_FTRACE, the frame
// trace (PS3_FT_GAME_STATE).
//
// DOTHEROES_DEV_CANT_DIE=1: no lethal damage. Every 2 ms, a life of 8 half
// apples or less (4 apples) is raised to 9 (4.5 of the save's 5) -- the
// player's own life and the HUD's copies: hits still land and knock back,
// enemies are untouched, and life is never full -- a full life keeps the sword
// beam, which would change the fight. Each top-up is logged and counted. Safe
// while no hit takes 9 half apples or more, or two land within 2 ms: the
// river's enemies hit for 2 to 7. =<floor>,<refill> sets other values. A test
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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

#include "ps3emu/osd.h"
#include "ps3emu/ps3_ftrace.h"

extern "C" void spurs_snap_write_file(const char* path);
extern "C" uint8_t* vm_base;

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

void state_loop()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint32_t life = player_life(), topups = g_topups.load(), area = rd32(kArea);
        const float x = rdf(kCameraX), z = rdf(kCameraZ);
        const unsigned long long steps = dotheroes_phys_steps();
        const uint64_t t = ps3_ft_now();
        if (PS3_FT_ON()) {
            uint32_t xb, zb;
            memcpy(&xb, &x, 4); memcpy(&zb, &z, 4);
            ps3_ft_span(PS3_FT_GAME_STATE, (life & 0xFF) | (area & 0xFF) << 8 | (topups < 0xFFFF ? topups : 0xFFFF) << 16,
                        xb, zb, t, t);
        }
        if (FILE* f = fopen("state.tmp", "w")) {
            fprintf(f, "%.3f %u %.2f %.2f %llu %u %u\n", (double)(int64_t)(t - g_ps3_ft.base) / 1e9, life, x, z, steps,
                    topups, area);
            fclose(f);
            remove("state.txt");
            rename("state.tmp", "state.txt");
        }
    }
}

#ifdef _WIN32
// The whole committed guest space (cellSpurs's writer stops at 0x20000000; the
// game also keeps state in the system VM and HLE heap areas above it), in the
// same format: records of (u32 ea, u32 size, little-endian) then the bytes.
void memsnap_write(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    for (uint64_t ea = 0x10000u; ea < 0xF0000000u; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + ea, &mbi, sizeof mbi)) break;
        uint64_t len = (uint64_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)(vm_base + ea));
        if (ea + len > 0xF0000000u) len = 0xF0000000u - ea;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && len < 0xFFFFFFFFu) {
            const uint32_t e32 = (uint32_t)ea, l32 = (uint32_t)len;
            fwrite(&e32, 4, 1, f); fwrite(&l32, 4, 1, f); fwrite(vm_base + ea, 1, l32, f);
        }
        ea += len;
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
#ifdef _WIN32
        memsnap_write(path);
#else
        spurs_snap_write_file(path);
#endif
        remove("memsnap.trigger");
        fprintf(stderr, "[memsnap] %s\n", path);
    }
}

#ifdef _WIN32
void mark(unsigned& n, uint32_t who)
{
    n++;
    const uint64_t t = ps3_ft_now();
    if (PS3_FT_ON()) ps3_ft_span(PS3_FT_MARK, who, n, 0, t, t);
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

}  // namespace

void dev_state_init()
{
    const char* st = getenv("DOTHEROES_DEV_STATE");
    if (st && *st == '1') {
        std::thread(state_loop).detach();
        fprintf(stderr, "[dev-state] state.txt: t_s life x z steps topups area, every 100 ms\n");
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
    if (g_cant_die || g_life_log || (st && *st == '1')) std::thread(life_loop).detach();   // also finds the player for state.txt
    if (g_cant_die) {
        fprintf(stderr, "[cant-die] on (development only): life of %u half apples or less raised to %u\n", g_low_life, g_top_up);
    }
    const char* e = getenv("DOTHEROES_DEV_MEMSNAP");
    if (e && *e == '1') {
        std::thread(memsnap_loop).detach();
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
