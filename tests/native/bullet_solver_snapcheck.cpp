// The native solver commands (port/native/bullet_solver.cpp) against captured
// game data, offline: the command's inputs from a DOTHEROES_SOLVER_SNAP
// snapshot taken as its first task was created, its result from the one taken
// as the next round or command was (port/native/bullet_solver_task.cpp).
//
//   bullet_solver_snapcheck writeback <c6r1.bin> <c6r2.bin> <descriptor ea>
//   bullet_solver_snapcheck copyback <c5r1.bin> <c6r1.bin> <tasks.txt>
//   bullet_solver_snapcheck iterate <c4rN.bin> <c4rN+k.bin or c5r1.bin> <tasks.txt> <k rounds>
//
// Runs the write-back as one task claiming every cell (what the round does
// when the other two tasks find nothing left: a schedule the game's allows),
// then compares, for every contact, the 16 bytes at its pointer with the
// second snapshot (pass 1 of the game's write-back), and checks what the task
// leaves in the hash: every cell processed, its mask row clear, the spin lock
// released with one ticket per claim plus the last, empty-handed one.
// The descriptor: a write-back task's in <prefix><step>_tasks.txt.
// Built with the bundled Zig:
//   zig c++ -target x86_64-windows-gnu -O2 -I port/native -I <ps3recomp>/include
//     -I <ps3recomp>/runtime/spu tests/native/bullet_solver_snapcheck.cpp
//     port/native/bullet_solver.cpp
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

// iterate <before.bin> <after.bin> <tasks.txt> <rounds>: an iterate task's
// descriptor from the capture's log put in place, the native iterate run as one
// task for <rounds> rounds -- before each round after the first, the cell masks
// put back as the first snapshot has them (what the PPU sets for every round;
// checked here against the second snapshot when it is one taken at a round's
// start) -- then every solver body up to the highest one a cell lists (0x40
// bytes each) and every contact row (0x90 bytes, three a contact) compared
// with the second snapshot. The game's run must have taken the cells in index
// order (DOTHEROES_SOLVER_SERIAL=1) for cells sharing a body to match.
static uint16_t rd16(const uint8_t* b, uint32_t ea) { return (uint16_t)(b[ea] << 8 | b[ea + 1]); }

static int iterate(const char* pre_path, const char* post_path, const char* tasks, int rounds)
{
    uint8_t* pre = load_space(pre_path);
    uint8_t* post = load_space(post_path);
    FILE* f = fopen(tasks, "r");
    if (!f) { fprintf(stderr, "cannot read %s\n", tasks); return 2; }
    char line[4096], buf[512];
    uint32_t desc = 0, words[36];
    while (!desc && fgets(line, sizeof line, f)) {
        unsigned slot, cmd, round, d;
        int at = 0;
        if (sscanf(line, "task slot %u command %u round %u desc %x:%n", &slot, &cmd, &round, &d, &at) != 4 || cmd != 4) continue;
        const char* p = line + at;
        int k = 0;
        for (; k < 36; k++) { unsigned v; int used; if (sscanf(p, " %x%n", &v, &used) != 1) break; words[k] = v; p += used; }
        if (k == 36) desc = d;
    }
    fclose(f);
    if (!desc) { fprintf(stderr, "no iterate task in %s\n", tasks); return 2; }
    for (int k = 0; k < 36; k++) wr32(pre, desc + 4 * k, words[k]);
    const uint32_t hash = rd32(pre, desc + 0x10), sbl = rd32(pre, desc + 0x14), rows = rd32(pre, desc + 0x18);
    const uint32_t offs = rd32(pre, desc + 0x20);

    // what the step holds: cells, their bodies and contacts
    struct Cell { uint32_t index, nb, list, nc, first; };
    std::vector<Cell> cells;
    std::vector<unsigned> cells_of;          // per solver body: in how many cells' lists
    unsigned contacts = 0, both_dynamic = 0, multi_packet = 0, out_of_cell = 0;
    uint32_t max_body = 0;
    for (uint32_t i = 0; i < 128; i++) {
        const uint32_t c = hash + 0x870 + 16 * i;
        const uint32_t nb = rd16(pre, c), nc = rd16(pre, c + 8);
        if (!nc && !rd16(pre, c + 12)) continue;
        cells.push_back({ i, nb, offs + 4 * rd16(pre, c + 2), nc, rd16(pre, c + 10) });
        for (uint32_t b = 0; b < nb; b++) {
            const uint32_t idx = rd32(pre, cells.back().list + 4 * b);
            if (idx >= cells_of.size()) cells_of.resize(idx + 1, 0);
            cells_of[idx]++;
            if (idx > max_body) max_body = idx;
        }
        const uint32_t used = ((4 * nb + 15) & ~15u) + 0x40 * nb;
        multi_packet += nc > (0x25800 - used) / 0x1B0;
        for (uint32_t j = 0; j < nc; j++) {
            const uint32_t r = rows + (cells.back().first + 3 * j) * 0x90;
            const uint32_t a = rd32(pre, r + 0x58), b = rd32(pre, r + 0x5C);
            if (a >= nb || b >= nb) { out_of_cell++; continue; }
            const uint32_t ba = rd32(pre, cells.back().list + 4 * a), bb = rd32(pre, cells.back().list + 4 * b);
            both_dynamic += (rd32(pre, sbl + 0x40 * ba + 0x24) & 0x7F800000u) && (rd32(pre, sbl + 0x40 * bb + 0x24) & 0x7F800000u);
        }
        contacts += nc;
    }
    unsigned shared = 0, listed = 0;
    for (unsigned n : cells_of) { shared += n > 1; listed += n > 0; }
    printf("step: %zu cells with work, %u contacts (%u between two moving bodies), %u solver bodies listed, "
           "%u in more than one cell; cells needing more than one packet %u; rows naming a body outside their cell %u\n",
           cells.size(), contacts, both_dynamic, listed, shared, multi_packet, out_of_cell);

    uint8_t masks[0x70];
    memcpy(masks, pre + hash + 0x800, sizeof masks);
    const bool post_at_round = memcmp(post + hash + 0x800, masks, sizeof masks) == 0;
    std::vector<uint8_t> body0((max_body + 1) * 0x40);
    memcpy(body0.data(), pre + sbl, body0.size());
    std::vector<uint8_t> rows0;
    for (const Cell& c : cells) rows0.insert(rows0.end(), pre + rows + c.first * 0x90, pre + rows + (c.first + 3 * c.nc) * 0x90);
    bullet_solver::Stats st;
    const bullet_solver::Sync sync = { acquire, release };
    LARGE_INTEGER q0, q1, qf;
    double best = 1e30;
    QueryPerformanceFrequency(&qf);
    for (int r = 0; r < rounds; r++) {
        if (r) memcpy(pre + hash + 0x800, masks, sizeof masks);
        QueryPerformanceCounter(&q0);
        bullet_solver::iterate_task(pre, desc, sync, st);
        QueryPerformanceCounter(&q1);
        const double us = (double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)qf.QuadPart;
        if (us < best) best = us;
    }
    printf("native iterate, compute only: fastest round %.1f us (%d rounds, one task)\n", best, rounds);

    unsigned bodies_same = 0, bodies_changed = 0, rows_same = 0, rows_changed = 0, shown = 0;
    for (uint32_t b = 0; b <= max_body; b++) {
        const bool same = memcmp(pre + sbl + 0x40 * b, post + sbl + 0x40 * b, 0x40) == 0;
        bodies_same += same;
        bodies_changed += memcmp(post + sbl + 0x40 * b, &body0[0x40 * b], 0x40) != 0;
        if (!same && shown < 10) {
            shown++;
            unsigned k = 0;
            while (rd32(pre, sbl + 0x40 * b + 4 * k) == rd32(post, sbl + 0x40 * b + 4 * k)) k++;
            printf("  body %u (in %u cells): first differing word +0x%02X native %08X game %08X\n", b,
                   b < cells_of.size() ? cells_of[b] : 0, 4 * k, rd32(pre, sbl + 0x40 * b + 4 * k), rd32(post, sbl + 0x40 * b + 4 * k));
        }
    }
    unsigned n_rows = 0;
    size_t at0 = 0;
    for (const Cell& c : cells)
        for (uint32_t j = 0; j < 3 * c.nc; j++) {
            const uint32_t r = rows + (c.first + j) * 0x90;
            const bool same = memcmp(pre + r, post + r, 0x90) == 0;
            rows_same += same;
            rows_changed += memcmp(post + r, &rows0[at0], 0x90) != 0;
            at0 += 0x90;
            n_rows++;
            if (!same && shown < 20) {
                shown++;
                unsigned k = 0;
                while (rd32(pre, r + 4 * k) == rd32(post, r + 4 * k)) k++;
                printf("  cell %u contact %u row %u: first differing word +0x%02X native %08X game %08X\n", c.index, j / 3, j % 3,
                       4 * k, rd32(pre, r + 4 * k), rd32(post, r + 4 * k));
            }
        }
    snprintf(buf, sizeof buf, "%u of %u solver bodies (%u changed by the game), %d round(s), %u cells claimed, %u contacts solved",
             bodies_same, max_body + 1, bodies_changed, rounds, st.cells, st.contacts);
    expect(bodies_same == max_body + 1 && st.bad == 0, "native iterate = the game's: solver bodies, 0x40 bytes each", buf);
    snprintf(buf, sizeof buf, "%u of %u rows (%u changed by the game)", rows_same, n_rows, rows_changed);
    expect(rows_same == n_rows, "native iterate = the game's: contact rows, 0x90 bytes each", buf);
    snprintf(buf, sizeof buf, "%s", post_at_round ? "the second snapshot's masks are the first's (as each round starts)"
                                                  : "the second snapshot is not at a round's start (masks differ): not compared");
    expect(true, "cell masks", buf);
    printf("[bullet_solver_snapcheck] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc == 5 && !strcmp(argv[1], "copyback")) return copyback(argv[2], argv[3], argv[4]);
    if (argc == 6 && !strcmp(argv[1], "iterate")) return iterate(argv[2], argv[3], argv[4], atoi(argv[5]));
    if (argc != 5 || strcmp(argv[1], "writeback")) {
        fprintf(stderr, "usage: bullet_solver_snapcheck writeback <c6r1.bin> <c6r2.bin> <descriptor ea>\n"
                        "       bullet_solver_snapcheck copyback <c5r1.bin> <c6r1.bin> <tasks.txt>\n"
                        "       bullet_solver_snapcheck iterate <c4rN.bin> <c4rN+k.bin or c5r1.bin> <tasks.txt> <k>\n");
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
