// The native solver commands (port/native/bullet_solver.cpp) against captured
// game data, offline: the command's inputs from a DOTHEROES_SOLVER_SNAP
// snapshot taken as its first task was created, its result from the one taken
// as the next round or command was (port/native/bullet_solver_task.cpp).
//
//   bullet_solver_snapcheck writeback <c6r1.bin> <c6r2.bin> <descriptor ea>
//   bullet_solver_snapcheck copyback <c5r1.bin> <c6r1.bin> <tasks.txt>
//
// Runs the write-back as one task claiming every cell (what the round does
// when the other two tasks find nothing left: a schedule the game's allows),
// then compares, for every contact, the 16 bytes at its pointer with the
// second snapshot (pass 1 of the game's write-back), and checks what the task
// leaves in the hash: every cell processed, its mask row clear, the spin lock
// released with one ticket per claim plus the last, empty-handed one.
// The descriptor: a write-back task's in <prefix><step>_tasks.txt.
// Built with the bundled Zig:
//   zig c++ -target x86_64-windows-gnu -O2 -I port/native
//     tests/native/bullet_solver_snapcheck.cpp port/native/bullet_solver.cpp
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bullet_solver.h"

static uint8_t* load_space(const char* path)   // a snapshot laid out at its guest addresses
{
    uint8_t* base = (uint8_t*)VirtualAlloc(nullptr, 0x20000000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    FILE* f = fopen(path, "rb");
    if (!base || !f) { fprintf(stderr, "cannot load %s\n", path); exit(2); }
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2) {
        if ((uint64_t)hdr[0] + hdr[1] > 0x20000000u) { fseek(f, hdr[1], SEEK_CUR); continue; }
        if (fread(base + hdr[0], 1, hdr[1], f) != hdr[1]) break;
    }
    fclose(f);
    return base;
}

static uint32_t rd32(const uint8_t* b, uint32_t ea)
{
    const uint8_t* p = b + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// One task alone: the ticket lock is a counter pair nobody else touches.
static void acquire(uint8_t* b, uint32_t ea)
{
    const uint32_t next = (uint32_t)(b[ea + 2] << 8 | b[ea + 3]);
    b[ea + 2] = (uint8_t)((next + 1) >> 8); b[ea + 3] = (uint8_t)(next + 1);
    if ((uint32_t)(b[ea] << 8 | b[ea + 1]) != next) { fprintf(stderr, "lock: not this task's turn\n"); exit(3); }
}
static void release(uint8_t* b, uint32_t ea)
{
    const uint32_t now = (uint32_t)(b[ea] << 8 | b[ea + 1]) + 1;
    b[ea] = (uint8_t)(now >> 8); b[ea + 1] = (uint8_t)now;
}

static int fails;
static void expect(bool ok, const char* what, const char* detail)
{
    printf("[%s] %s: %s\n", ok ? "PASS" : "FAIL", what, detail);
    fails += !ok;
}

static void wr32(uint8_t* b, uint32_t ea, uint32_t v)
{
    b[ea] = (uint8_t)(v >> 24); b[ea + 1] = (uint8_t)(v >> 16); b[ea + 2] = (uint8_t)(v >> 8); b[ea + 3] = (uint8_t)v;
}

// copyback <c5r1.bin> <c6r1.bin> <tasks.txt>: each copy-back task's descriptor as
// it was created (the capture's log; a later task's may not be in the first
// snapshot yet) put back in place, the native copy-back run for every task, and
// each body's first 0x200 bytes compared with the second snapshot.
static int copyback(const char* pre_path, const char* post_path, const char* tasks)
{
    uint8_t* pre = load_space(pre_path);
    uint8_t* post = load_space(post_path);
    FILE* f = fopen(tasks, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", tasks); return 2; }
    char line[4096], buf[256];
    std::vector<uint32_t> descs;
    std::vector<std::vector<uint32_t>> words;
    while (fgets(line, sizeof line, f)) {
        unsigned slot, cmd, round, desc;
        int at = 0;
        if (sscanf(line, "task slot %u command %u round %u desc %x:%n", &slot, &cmd, &round, &desc, &at) != 4 || cmd != 5) continue;
        std::vector<uint32_t> w;
        const char* p = line + at;
        for (int k = 0; k < 36; k++) { unsigned v; int used; if (sscanf(p, " %x%n", &v, &used) != 1) break; w.push_back(v); p += used; }
        if (w.size() == 36) { descs.push_back(desc); words.push_back(w); }
    }
    fclose(f);
    unsigned bodies = 0, same = 0, moved = 0;
    std::vector<uint32_t> rbs;
    for (size_t t = 0; t < descs.size(); t++) {
        for (int k = 0; k < 36; k++) wr32(pre, descs[t] + 4 * k, words[t][k]);
        std::vector<bullet_solver::BodyWords> want(bullet_solver::copyback_expected(pre, descs[t], nullptr, 0));
        bullet_solver::copyback_expected(pre, descs[t], want.data(), (unsigned)want.size());
        for (const auto& bw : want) {
            rbs.push_back(bw.rb);
            for (int k = 0; k < 8; k++) moved += bw.w[k] != rd32(pre, bw.rb + (k < 4 ? 0x130 : 0x140) + 4 * (k & 3));
        }
        bodies += bullet_solver::copyback_task(pre, descs[t]);
    }
    for (uint32_t rb : rbs) same += memcmp(pre + rb, post + rb, 0x200) == 0;
    snprintf(buf, sizeof buf, "%u of %u bodies, first 0x200 bytes each (%u velocity floats changed); %zu tasks",
             same, bodies, moved, descs.size());
    expect(same == bodies && bodies > 0, "native copy-back = the game's, byte for byte", buf);
    printf("[bullet_solver_snapcheck] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc == 5 && !strcmp(argv[1], "copyback")) return copyback(argv[2], argv[3], argv[4]);
    if (argc != 5 || strcmp(argv[1], "writeback")) {
        fprintf(stderr, "usage: bullet_solver_snapcheck writeback <c6r1.bin> <c6r2.bin> <descriptor ea>\n"
                        "       bullet_solver_snapcheck copyback <c5r1.bin> <c6r1.bin> <tasks.txt>\n");
        return 2;
    }
    uint8_t* pre = load_space(argv[2]);
    uint8_t* post = load_space(argv[3]);
    const uint32_t desc = (uint32_t)strtoul(argv[4], nullptr, 16);
    char buf[256];
    if (rd32(pre, desc) != 6) { fprintf(stderr, "descriptor %08X holds command %u, not 6\n", desc, rd32(pre, desc)); return 2; }
    const uint32_t task = rd32(pre, desc + 4), hash = rd32(pre, desc + 0x10), lock = rd32(pre, desc + 0x30);

    std::vector<bullet_solver::Write> want(bullet_solver::writeback_expected(pre, desc, nullptr, 0));
    bullet_solver::writeback_expected(pre, desc, want.data(), (unsigned)want.size());
    std::vector<uint8_t> before(want.size() * 16);
    for (size_t i = 0; i < want.size(); i++) memcpy(&before[i * 16], pre + want[i].ea, 16);

    bullet_solver::Stats st;
    const bullet_solver::Sync sync = { acquire, release };
    bullet_solver::writeback_task(pre, desc, sync, st);

    unsigned same16 = 0, first_ok = 0, rest_kept = 0, changed = 0;
    for (size_t i = 0; i < want.size(); i++) {
        const uint8_t* n = pre + want[i].ea;
        same16 += memcmp(n, post + want[i].ea, 16) == 0;
        first_ok += rd32(pre, want[i].ea) == want[i].bits;
        rest_kept += memcmp(n + 4, &before[i * 16 + 4], 12) == 0;
        changed += memcmp(n, &before[i * 16], 4) != 0;
    }
    snprintf(buf, sizeof buf, "%u of %zu contacts (%u changed value), %u cells claimed", same16, want.size(), changed, st.cells);
    expect(same16 == want.size(), "native write-back = the game's pass 1, 16 bytes per contact", buf);
    snprintf(buf, sizeof buf, "%u of %zu", first_ok, want.size());
    expect(first_ok == want.size() && st.contacts == want.size(), "every contact written once, min(3, impulse)", buf);
    snprintf(buf, sizeof buf, "%u of %zu", rest_kept, want.size());
    expect(rest_kept == want.size(), "the other 12 bytes of each untouched", buf);
    bool all = true, row_clear = true;
    for (uint32_t k = 0; k < 4; k++) {
        all &= rd32(pre, hash + 0x800 + 4 * k) == 0xFFFFFFFFu;
        row_clear &= rd32(pre, hash + 0x800 + 16 * (1 + task) + 4 * k) == 0;
    }
    snprintf(buf, sizeof buf, "processed %s, task %u's row %s", all ? "all" : "NOT all", task, row_clear ? "clear" : "NOT clear");
    expect(all && row_clear, "the hash as a finished round leaves it", buf);
    const uint32_t lk = rd32(pre, lock);
    snprintf(buf, sizeof buf, "now serving %u, next ticket %u; claims %u + 1", lk >> 16, lk & 0xFFFF, st.cells);
    expect((lk >> 16) == (lk & 0xFFFF) && (lk >> 16) == st.cells + 1, "spin lock released, one ticket a claim and the last", buf);
    printf("[bullet_solver_snapcheck] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
