// The game's Bullet solver SPU task (runtime image 5, CreateTask entry
// 0xF8EC80): a step's solver is ~71 tasks in five commands the PPU issues in
// turn, three slots at a time, waiting for every task of one before the next
// (docs/BULLET_NATIVE.md, "The solver"). Each slot's descriptor (Bullet 2.74's
// SpuSolverTaskDesc as far as checked: +0 command, +4 task id) is fixed for
// the run and named by the slot's input-queue entries [slot, 1, desc, slot].
//
// DOTHEROES_PHYS_STATS=1 marks each task with its command as it starts
// (frame-trace kind 20; tests/solver_breakdown.py).
//
// DOTHEROES_SOLVER_SNAP=<step>[,<step>...]:<prefix> captures chosen steps for
// checking the solver's structures and a native command offline: guest memory
// (cellSpurs's SPURS_SNAP writer) when the first task of each command is
// created -- the PPU has then seen every task of the command before complete,
// and none of this one has run -- and for iterate and write-back also at their
// second round; then once more at the next collision task, after the step.
// <prefix><step>_<n>_c<command>r<round>.bin, and <prefix><step>_tasks.txt with
// what follows. DOTHEROES_SOLVER_SNAP_ROUNDS=1 snapshots every iterate round
// (c4r1..c4r10), not only the first two. DOTHEROES_SOLVER_SNAP_SHARED=<n> also
// captures, from iterate on, the first n steps whose iterate has a body in two
// cells' lists (the cells sharing it then wait for each other, and their order
// decides the result). The prefix is a path the game opens: a Windows one on
// Windows (C:/...).
//
// DOTHEROES_SOLVER_SERIAL=1 (for captures and checks, not timing): the lifted
// iterate tasks of a round run one at a time -- each waits in front of the
// image until the one before has returned -- so the first to start claims
// every cell, in index order (the next free cell is the first one not yet
// processed), and the other two find none left: the cell order one task alone
// takes, a schedule the game's protocol allows, and the one a native task run
// alone reproduces. Without it, which task takes which cell, and so the order
// of cells that share bodies, varies run to run.
//
// DOTHEROES_NATIVE_SOLVER=<command>[,<command>...] (experimental) does those
// commands here instead of the lifted image -- so far 6, the warm-start
// write-back, 5, the copy-back of the solved velocities to the bodies, and 4,
// a round of iterate (bullet_solver.cpp, on the game's structures as checked
// against captured steps). A native task keeps the task's protocol: it waits for its
// slot's command, pops it, takes cells under the same spin lock and masks the
// lifted tasks of the round use (so a lifted task beside it sees every claim),
// and pushes its own completion; the PPU still creates three tasks a round and
// waits for all three. A task whose command is not there within 20 ms, or names
// another descriptor, runs lifted, counted. Every 5000 native tasks a
// [native-solver] line counts them.
//
// DOTHEROES_NATIVE_SOLVER_CHECK=1 checks the copy-back too: what each copy-back
// task must leave in its bodies (velocities, companion id) worked out as it is
// created, compared as the step's first write-back task is (the copy-back is
// done then, and nothing else writes the bodies in between).
// And as each step's first write-back task is
// created, what the write-back must leave -- min(3, applied impulse) in the
// first float of each contact's 16 bytes, the other 12 untouched -- is worked
// out from the inputs; as the step's last write-back task finishes, memory is
// compared with it. With the lifted write-back, that compares the native model
// with what the game's task did; with the native one, it checks that every
// contact was written. Also counted: contacts changed again after the solver,
// before the next collision task. A [native-solver] check line every 200 steps.
// Iterate is checked (and benched) round by round: iterate_on_create below.
//
// DOTHEROES_SOLVER_BENCH=<file> (a measurement, for runs with the lifted
// commands): as each step's first write-back task is created -- the PPU in
// CreateTask, no solver task running -- the native write-back is run on the
// step's own inputs as one task claiming every cell, timed (best of 3), and
// everything it changed put back (the cell masks and task rows, the spin
// lock, each contact's 16 bytes), so the lifted tasks then do the step as
// always; likewise each copy-back task's range of bodies as the task is
// created (velocities and companion ids put back). A line each: the command,
// the time on the frame trace's clock (us from its base), contacts or bodies,
// cells, the native run's microseconds -- compute only, to set beside the
// lifted command's dispatched phase for the same step (tests/solver_bench.py).
//
// The capture writes too every solver task created in the step: its slot, command, round, descriptor
// address and bytes; and, for iterate and write-back, as each task starts and
// as it returns, the spin lock's 8 bytes and the hash's cell masks (the lock
// itself is freed by the step's end). A snapshot is ~170 MB of game memory:
// keep it out of git.
#include "bullet_solver_task.h"

#ifdef DOTHEROES_BULLET_CD
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bullet_solver.h"
#include "ps3emu/ps3_ftrace.h"
#include "spu_workload.h"

extern "C" uint8_t* vm_base;
extern "C" void (*g_spurs_create_hook)(uint32_t entry, const uint32_t arg[4]);
extern "C" void spurs_snap_write_file(const char* path);
extern "C" void spu_lockline_lock(void);
extern "C" void spu_lockline_unlock(void);
extern "C" void spu_coh_notify_write(uint32_t ea);

namespace {

constexpr int kImageBulletSolver = 5;
constexpr uint32_t kEntryCollision = 0xF63600, kEntrySolver = 0xF8EC80;
constexpr uint32_t kDescBytes = 0x90;   // the slots' descriptors are this far apart

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// A slot's descriptor, from its input queue's ring: every entry for the slot
// names the same one, and one is there whether or not the task has popped it.
uint32_t desc_from_queue(uint32_t inq, uint32_t slot)
{
    const uint32_t buf = be32(vm_base + inq + 0x14), depth = be32(vm_base + inq + 0x0C);
    for (uint32_t i = 0; i < depth && i < 16; i++) {
        const uint8_t* e = vm_base + buf + i * 16u;
        if (be32(e) == slot && be32(e + 4) == 1 && be32(e + 8)) return be32(e + 8);
    }
    return 0;
}

// The items a command covers, where its descriptor says: bodies (1, 5),
// manifolds (2); 0 for the others.
uint32_t command_count(const uint8_t* d)
{
    const uint32_t cmd = be32(d);
    return cmd == 1 || cmd == 5 ? be32(d + 0x34) : cmd == 2 ? be32(d + 0x3C) : 0;
}

bool g_stats;
void capture_task_state(const char* when, uint32_t slot, uint32_t desc);
void check_task_done(uint32_t cmd);
thread_local uint32_t t_task_cmd;   // the command the task on this thread started with
bool g_serial;                       // DOTHEROES_SOLVER_SERIAL
std::mutex g_serial_mu;
thread_local bool t_serial;          // this thread holds g_serial_mu for its lifted iterate task
int native_command(uint32_t desc, uint32_t cmd, const uint32_t arg[4]);
std::set<uint32_t> g_native;   // DOTHEROES_NATIVE_SOLVER

int solver_run(uint32_t, uint32_t, const uint32_t arg[4])
{
    const uint32_t desc = desc_from_queue(arg[2], arg[0]);
    t_task_cmd = desc ? be32(vm_base + desc) : 0;
    const bool native = desc && !g_native.empty() && g_native.count(t_task_cmd);
    if (g_serial && !native && t_task_cmd == 4) {
        g_serial_mu.lock();
        t_serial = true;
    }
    if (desc) capture_task_state("start", arg[0], desc);
    if (desc && g_stats && PS3_FT_ON()) {
        const uint8_t* d = vm_base + desc;
        const uint64_t t = ps3_ft_now();
        ps3_ft_span(PS3_FT_PHYS_CMD, (uint32_t)kImageBulletSolver, be32(d),
                    (arg[0] & 0xFFu) | (command_count(d) & 0xFFFFFFu) << 8, t, t);
    }
    if (native) return native_command(desc, t_task_cmd, arg);
    return 0;   // the lifted solver task
}

void solver_after(uint32_t, uint32_t, const uint32_t arg[4])
{
    const uint32_t desc = desc_from_queue(arg[2], arg[0]);
    if (desc) capture_task_state("end", arg[0], desc);
    check_task_done(t_task_cmd);   // a lifted task (native ones return before this); by the command it
                                   // started with: its slot's descriptor may already hold the next one
    if (t_serial) {
        t_serial = false;
        g_serial_mu.unlock();
    }
}

// DOTHEROES_SOLVER_SNAP: on the creating PPU thread, before the task can run.
struct Capture {
    std::set<unsigned> steps;
    std::string prefix;
    unsigned step = 0, in_command = 0, n = 0;
    uint32_t cmd = 0;
    bool all_rounds = false;   // DOTHEROES_SOLVER_SNAP_ROUNDS
    unsigned shared_left = 0;  // DOTHEROES_SOLVER_SNAP_SHARED
    FILE* log = nullptr;    // open while a chosen step is being captured
    std::mutex mu;          // the log: the PPU thread and the tasks' threads
} g_cap;

// Iterate and write-back tasks of a captured step, as each starts and returns:
// the spin lock (descriptor +0x30) and the cell masks (hash +0x800, 7 x 4 words).
void capture_task_state(const char* when, uint32_t slot, uint32_t desc)
{
    if (!g_cap.log) return;
    const uint8_t* d = vm_base + desc;
    const uint32_t cmd = be32(d);
    if (cmd != 4 && cmd != 6) return;
    const uint32_t lock = be32(d + 0x30), hash = be32(d + 0x10);
    std::lock_guard<std::mutex> lk(g_cap.mu);
    if (!g_cap.log) return;
    fprintf(g_cap.log, "%s slot %u command %u lock %08X: %08X %08X masks", when, slot, cmd, lock,
            be32(vm_base + lock), be32(vm_base + lock + 4));
    for (uint32_t o = 0; o < 7 * 16; o += 4) fprintf(g_cap.log, " %08X", be32(vm_base + hash + 0x800 + o));
    fputc('\n', g_cap.log);
}

void capture_snap(const char* label)
{
    Capture& c = g_cap;
    std::lock_guard<std::mutex> lk(c.mu);
    char path[700];
    snprintf(path, sizeof path, "%s%u_%02u_%s.bin", c.prefix.c_str(), c.step, c.n++, label);
    spurs_snap_write_file(path);
    fprintf(c.log, "snapshot %s\n", path);
    fflush(c.log);
}

void capture_on_create(uint32_t entry, const uint32_t arg[4])
{
    Capture& c = g_cap;
    if (entry == kEntryCollision) {
        if (c.log) {   // the step's solver is done
            capture_snap("end");
            std::lock_guard<std::mutex> lk(c.mu);
            fclose(c.log);
            c.log = nullptr;
            fprintf(stderr, "[solver-snap] step %u captured\n", c.step);
        }
        c.cmd = 0;
        return;
    }
    if (entry != kEntrySolver) return;
    const uint32_t desc = desc_from_queue(arg[2], arg[0]);
    if (!desc) return;
    const uint8_t* d = vm_base + desc;
    const uint32_t cmd = be32(d);
    const auto open_log = [&c](const char* from) {
        char path[700];
        snprintf(path, sizeof path, "%s%u_tasks.txt", c.prefix.c_str(), c.step);
        std::lock_guard<std::mutex> lk(c.mu);
        c.log = fopen(path, "w");
        c.n = 0;
        if (c.log) fprintf(c.log, "# step %u: solver tasks as created%s (slot, command, round, descriptor, its 0x%X bytes, "
                           "task argument)\n", c.step, from, kDescBytes);
        else fprintf(stderr, "[solver-snap] cannot write %s\n", path);
    };
    if (cmd != c.cmd) {
        if (cmd == 1 && c.cmd != 1 && ++c.step && c.steps.count(c.step)) open_log("");
        c.in_command = 0;
        c.cmd = cmd;
    }
    if (cmd == 4 && c.in_command == 0 && !c.log && c.shared_left) {   // DOTHEROES_SOLVER_SNAP_SHARED
        std::vector<bullet_solver::Span> bodies, rows;
        bullet_solver::IterateShape shape;
        bullet_solver::iterate_spans(vm_base, desc, bodies, rows, shape);
        if (shape.shared) {
            c.shared_left--;
            open_log(", from iterate on (a body in two cells' lists)");
        }
    }
    const unsigned round = cmd == 4 || cmd == 6 ? c.in_command / 3 + 1 : 1;
    if (c.log) {
        if (c.in_command == 0 || ((cmd == 4 || cmd == 6) && c.in_command == 3) ||
            (cmd == 4 && c.all_rounds && c.in_command % 3 == 0)) {
            char label[32];
            snprintf(label, sizeof label, "c%ur%u", cmd, round);
            capture_snap(label);
        }
        std::lock_guard<std::mutex> lk(c.mu);
        fprintf(c.log, "task slot %u command %u round %u desc %08X:", arg[0], cmd, round, desc);
        for (uint32_t o = 0; o < kDescBytes; o += 4) fprintf(c.log, " %08X", be32(d + o));
        fprintf(c.log, " arg %08X %08X %08X %08X\n", arg[0], arg[1], arg[2], arg[3]);
    }
    c.in_command++;
}

// ---- native commands (DOTHEROES_NATIVE_SOLVER)
std::atomic<unsigned long long> g_ran_native{ 0 }, g_cells{ 0 }, g_contacts{ 0 }, g_waits{ 0 };
std::atomic<unsigned long long> g_no_command{ 0 }, g_other_desc{ 0 }, g_bodies{ 0 }, g_solved{ 0 }, g_bad_rows{ 0 };

// The command the PPU pushes right after CreateTask: wait for it a while
// (the task may start before the push); past that the lifted task waits.
bool peek_command(uint32_t inq, uint8_t entry[16])
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    for (;;) {
        if (spurs_queue_spu_peek(inq, entry)) return true;
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::yield();
    }
}

// The hash's spin lock as the lifted tasks of the round take it: libsync's
// ticket lock, +0 now serving and +2 next ticket, 16 bits each (checked on
// captured steps). Each change under the runtime's lock-line lock with the
// line's SPU reservations broken, as an SPU's PUTLLC would be.
inline uint16_t rd16v(const uint8_t* p)
{
    const volatile uint8_t* v = p;
    return (uint16_t)(v[0] << 8 | v[1]);
}
inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

void lock_acquire(uint8_t* base, uint32_t ea)
{
    spu_lockline_lock();
    const uint16_t ticket = rd16v(base + ea + 2);
    wr16(base + ea + 2, (uint16_t)(ticket + 1));
    spu_coh_notify_write(ea);
    spu_lockline_unlock();
    for (unsigned n = 0; rd16v(base + ea) != ticket; n++)
        if (n > 64) std::this_thread::yield();
}

void lock_release(uint8_t* base, uint32_t ea)
{
    spu_lockline_lock();
    wr16(base + ea, (uint16_t)(rd16v(base + ea) + 1));
    spu_coh_notify_write(ea);
    spu_lockline_unlock();
}

void native_work(uint32_t desc, uint32_t cmd);

int native_command(uint32_t desc, uint32_t cmd, const uint32_t arg[4])
{
    const uint32_t inq = arg[2], outq = arg[3];
    uint8_t entry[16];
    if (!peek_command(inq, entry)) { g_no_command++; return 0; }
    if (be32(entry + 4) != 1 || be32(entry + 8) != desc) { g_other_desc++; return 0; }
    if (!spurs_queue_spu_pop(inq, entry)) { g_no_command++; return 0; }
    native_work(desc, cmd);
    while (!spurs_queue_spu_push(outq, entry)) std::this_thread::yield();
    return 1;
}

// A native command's work for one task, and its counts: from a SPURS task
// (native_command) or issued directly (bullet_solver_direct_run, R8).
void native_work(uint32_t desc, uint32_t cmd)
{
    bullet_solver::Stats st;
    const bullet_solver::Sync sync = { lock_acquire, lock_release };
    if (cmd == 6) bullet_solver::writeback_task(vm_base, desc, sync, st);
    else if (cmd == 5) g_bodies += bullet_solver::copyback_task(vm_base, desc);
    else if (cmd == 4) bullet_solver::iterate_task(vm_base, desc, sync, st);
    check_task_done(cmd);   // before the PPU can see this task done
    g_cells += st.cells; g_waits += st.waits; g_bad_rows += st.bad;
    (cmd == 4 ? g_solved : g_contacts) += st.contacts;
    const unsigned long long n = ++g_ran_native;
    if (n % 5000 == 0)
        fprintf(stderr, "[native-solver] %llu native tasks: %llu cells, %llu contacts solved (iterate), %llu written back, "
                "%llu waits for a held cell, %llu rows naming a body outside their cell; %llu bodies copied back; lifted "
                "instead: no command within 20 ms %llu, another descriptor %llu\n",
                n, g_cells.load(), g_solved.load(), g_contacts.load(), g_waits.load(), g_bad_rows.load(), g_bodies.load(),
                g_no_command.load(), g_other_desc.load());
}

// ---- DOTHEROES_NATIVE_SOLVER_CHECK
// Armed on the PPU thread as the step's first write-back task is created;
// compared as the step's last write-back task finishes (the 30th: 10 rounds of
// 3), before the PPU goes on -- a native task before it pushes its completion,
// a lifted one just after (its completion already pushed; the PPU's next use
// of the manifolds comes much later). Then once more as the next collision
// task is created, counting what changed after the solver: a manifold released
// and made again for a new pair starts its points at zero impulse.
struct Check {
    bool on = false;
    std::atomic<bool> armed{ false };
    std::atomic<int> created{ 0 }, done{ 0 };
    std::vector<bullet_solver::Write> expect;
    std::vector<std::array<uint8_t, 16>> before;
    std::mutex mu;            // one comparison a step
    bool compared = false;
    unsigned long long steps = 0, contacts = 0, first_bad = 0, rest_bad = 0, late = 0, changed_after = 0, steps_changed = 0;
} g_chk;

void check_compare()   // g_chk.mu held
{
    Check& k = g_chk;
    for (size_t i = 0; i < k.expect.size(); i++) {
        const uint8_t* p = vm_base + k.expect[i].ea;
        const bool bad = be32(p) != k.expect[i].bits;
        k.first_bad += bad;
        k.rest_bad += memcmp(p + 4, k.before[i].data() + 4, 12) != 0;
        static int shown = 0;
        if (bad && shown < 12) {
            shown++;
            fprintf(stderr, "[native-solver] check: contact %zu of %zu at %08X: expected %08X, found %08X, before %08X\n",
                    i, k.expect.size(), k.expect[i].ea, k.expect[i].bits, be32(p), be32(k.before[i].data()));
        }
    }
    k.steps++;
    k.contacts += k.expect.size();
    k.compared = true;
    if (k.steps % 200 == 0 || (k.first_bad + k.rest_bad && k.steps < 20))
        fprintf(stderr, "[native-solver] check (%s write-back): %llu steps, %llu contacts: first float not min(3, impulse) "
                "%llu, other 12 bytes changed %llu; compared only at the next collision task %llu steps; changed after "
                "the solver, before the next collision task: %llu contacts in %llu steps\n", g_native.count(6) ? "native" : "lifted",
                k.steps, k.contacts, k.first_bad, k.rest_bad, k.late, k.changed_after, k.steps_changed);
}

void check_task_done(uint32_t cmd)   // a task's work is done (its thread), cmd as it started
{
    Check& k = g_chk;
    if (!k.on || !k.armed || cmd != 6) return;
    if (++k.done == 30 && k.created == 30) {
        std::lock_guard<std::mutex> lk(k.mu);
        if (!k.compared) check_compare();
    }
}

// The copy-back's check, on the PPU thread: expected words per body as each
// copy-back task is created, compared as the first write-back task is.
struct CopybackCheck {
    std::vector<bullet_solver::BodyWords> expect;
    unsigned long long steps = 0, bodies = 0, words_bad = 0, bodies_bad = 0;
} g_cbk;

void copyback_check(uint32_t cmd, uint32_t desc)
{
    CopybackCheck& c = g_cbk;
    if (cmd == 5) {
        const unsigned n = bullet_solver::copyback_expected(vm_base, desc, nullptr, 0);
        const size_t at = c.expect.size();
        c.expect.resize(at + n);
        bullet_solver::copyback_expected(vm_base, desc, c.expect.data() + at, n);
        return;
    }
    if (cmd != 6 || c.expect.empty()) return;
    for (const auto& bw : c.expect) {
        unsigned bad = 0;
        for (uint32_t k = 0; k < 4; k++) {
            bad += be32(vm_base + bw.rb + 0x130 + 4 * k) != bw.w[k];
            bad += be32(vm_base + bw.rb + 0x140 + 4 * k) != bw.w[4 + k];
        }
        bad += be32(vm_base + bw.rb + 0xC4) != bw.w[8];
        c.words_bad += bad;
        c.bodies_bad += bad != 0;
        static int shown = 0;
        if (bad && shown < 8) {
            shown++;
            fprintf(stderr, "[native-solver] copy-back check: body %08X: %u of 9 words differ (linear x %08X, expected %08X)\n",
                    bw.rb, bad, be32(vm_base + bw.rb + 0x130), bw.w[0]);
        }
    }
    c.steps++;
    c.bodies += c.expect.size();
    c.expect.clear();
    if (c.steps % 200 == 0 || (c.bodies_bad && c.steps < 20))
        fprintf(stderr, "[native-solver] copy-back check (%s): %llu steps, %llu bodies: %llu bodies with a word not as expected (%llu words)\n",
                g_native.count(5) ? "native" : "lifted", c.steps, c.bodies, c.bodies_bad, c.words_bad);
}

void check_on_create(uint32_t entry, const uint32_t arg[4])   // the PPU thread
{
    Check& k = g_chk;
    if (entry == kEntrySolver) {
        const uint32_t d = desc_from_queue(arg[2], arg[0]);
        if (d) copyback_check(be32(vm_base + d), d);
    }
    if (entry == kEntryCollision && k.armed) {
        std::lock_guard<std::mutex> lk(k.mu);
        if (!k.compared) { k.late++; check_compare(); }
        else {
            unsigned changed = 0;
            for (const auto& w : k.expect) changed += be32(vm_base + w.ea) != w.bits;
            k.changed_after += changed;
            k.steps_changed += changed != 0;
        }
        k.armed = false;
        return;
    }
    if (entry != kEntrySolver) return;
    const uint32_t desc = desc_from_queue(arg[2], arg[0]);
    if (!desc || be32(vm_base + desc) != 6) return;
    if (!k.armed) {
        const unsigned n = bullet_solver::writeback_expected(vm_base, desc, nullptr, 0);
        k.expect.resize(n);
        bullet_solver::writeback_expected(vm_base, desc, k.expect.data(), n);
        k.before.resize(n);
        for (unsigned i = 0; i < n; i++) memcpy(k.before[i].data(), vm_base + k.expect[i].ea, 16);
        k.created = 0;
        k.done = 0;
        k.compared = false;
        k.armed = true;
    }
    k.created++;
}

// ---- DOTHEROES_SOLVER_BENCH: on the creating PPU thread
struct Bench {
    FILE* f = nullptr;
    bool done_this_step = false;
    std::vector<bullet_solver::Write> w;
    std::vector<std::array<uint8_t, 16>> saved;
} g_bench;

// One task alone, nothing else running: the ticket lock as a plain counter pair.
void bench_acquire(uint8_t* b, uint32_t ea)
{
    const uint16_t t = rd16v(b + ea + 2);
    wr16(b + ea + 2, (uint16_t)(t + 1));
}
void bench_release(uint8_t* b, uint32_t ea) { wr16(b + ea, (uint16_t)(rd16v(b + ea) + 1)); }

void bench_on_create(uint32_t entry, const uint32_t arg[4])
{
    Bench& B = g_bench;
    if (entry == kEntryCollision) { B.done_this_step = false; return; }
    if (entry != kEntrySolver) return;
    const uint32_t desc = desc_from_queue(arg[2], arg[0]);
    if (desc && be32(vm_base + desc) == 5) {   // each copy-back task: its own range of bodies
        const unsigned nb = bullet_solver::copyback_expected(vm_base, desc, nullptr, 0);
        std::vector<bullet_solver::BodyWords> bw(nb);
        bullet_solver::copyback_expected(vm_base, desc, bw.data(), nb);
        std::vector<std::array<uint8_t, 36>> keep(nb);
        for (unsigned i = 0; i < nb; i++) {
            memcpy(keep[i].data(), vm_base + bw[i].rb + 0x130, 32);
            memcpy(keep[i].data() + 32, vm_base + bw[i].rb + 0xC4, 4);
        }
        double best = 1e30;
        const uint64_t t_at = ps3_ft_now();
        for (int rep = 0; rep < 3; rep++) {
            const uint64_t t0 = ps3_ft_now();
            bullet_solver::copyback_task(vm_base, desc);
            const uint64_t t1 = ps3_ft_now();
            for (unsigned i = 0; i < nb; i++) {
                memcpy(vm_base + bw[i].rb + 0x130, keep[i].data(), 32);
                memcpy(vm_base + bw[i].rb + 0xC4, keep[i].data() + 32, 4);
            }
            best = std::min(best, (double)(t1 - t0) / 1000.0);
        }
        fprintf(B.f, "5 %.1f %u 0 %.2f\n", (double)(int64_t)(t_at - g_ps3_ft.base) / 1000.0, nb, best);
        fflush(B.f);
        return;
    }
    if (B.done_this_step || !desc || be32(vm_base + desc) != 6) return;
    B.done_this_step = true;
    const uint32_t hash = be32(vm_base + desc + 0x10), lock = be32(vm_base + desc + 0x30);
    const unsigned n = bullet_solver::writeback_expected(vm_base, desc, nullptr, 0);
    B.w.resize(n);
    bullet_solver::writeback_expected(vm_base, desc, B.w.data(), n);
    B.saved.resize(n);
    for (unsigned i = 0; i < n; i++) memcpy(B.saved[i].data(), vm_base + B.w[i].ea, 16);
    uint8_t masks[0x70], lk[4];
    memcpy(masks, vm_base + hash + 0x800, sizeof masks);
    memcpy(lk, vm_base + lock, sizeof lk);
    const auto restore = [&] {
        for (unsigned i = 0; i < n; i++) memcpy(vm_base + B.w[i].ea, B.saved[i].data(), 16);
        memcpy(vm_base + hash + 0x800, masks, sizeof masks);
        memcpy(vm_base + lock, lk, sizeof lk);
    };
    const bullet_solver::Sync sync = { bench_acquire, bench_release };
    double best = 1e30;
    unsigned cells = 0;
    const uint64_t t_at = ps3_ft_now();
    for (int rep = 0; rep < 3; rep++) {
        bullet_solver::Stats st;
        const uint64_t t0 = ps3_ft_now();
        bullet_solver::writeback_task(vm_base, desc, sync, st);
        const uint64_t t1 = ps3_ft_now();
        restore();
        best = std::min(best, (double)(t1 - t0) / 1000.0);
        cells = st.cells;
    }
    fprintf(B.f, "6 %.1f %u %u %.2f\n", (double)(int64_t)(t_at - g_ps3_ft.base) / 1000.0, n, cells, best);
    fflush(B.f);   // the run ends with the process stopped: nothing buffered survives
}

// ---- iterate's check and bench (DOTHEROES_NATIVE_SOLVER_CHECK, DOTHEROES_SOLVER_BENCH):
// on the PPU thread as each round's first task is created -- the round before
// it done (the PPU waits for its three tasks), nothing of this one started (a
// task does nothing before the command the PPU pushes after CreateTask). The
// native iterate is run there as one task on the round's own inputs; what it
// leaves is kept (check) or its time noted (bench: best of 3); then everything
// it changed is put back -- the bodies its cells list, their contact rows, the
// cell masks and the spin lock -- and the round's own tasks do the round. The
// check compares what was kept with memory as the next round's first task, or
// the copy-back's, is created. With the lifted iterate run one task at a time
// (DOTHEROES_SOLVER_SERIAL=1) both take the cells in index order; without, a
// body in two cells' lists (counted) may end differently, from the order alone.
struct IterateRounds {
    uint32_t cmd = 0;   // the command of the solver tasks being created, and how many so far
    unsigned in_cmd = 0, round = 0;
    bool pending = false;
    std::vector<bullet_solver::Span> bodies, rows;
    std::vector<uint8_t> saved, result;
    unsigned long long steps = 0, rounds = 0, n_bodies = 0, n_rows = 0, bodies_bad = 0, rows_bad = 0, rounds_bad = 0;
    unsigned long long contacts = 0, shared = 0, steps_shared = 0, multi_packet = 0;
} g_it;

void spans_out(const std::vector<bullet_solver::Span>& v, uint8_t*& to)
{
    for (const auto& s : v) { memcpy(to, vm_base + s.ea, s.bytes); to += s.bytes; }
}
void spans_in(const std::vector<bullet_solver::Span>& v, const uint8_t*& from)
{
    for (const auto& s : v) { memcpy(vm_base + s.ea, from, s.bytes); from += s.bytes; }
}

void iterate_compare()
{
    IterateRounds& R = g_it;
    const uint8_t* want = R.result.data();
    unsigned bad_b = 0, bad_r = 0, n_r = 0;
    for (const auto& s : R.bodies) { bad_b += memcmp(vm_base + s.ea, want, s.bytes) != 0; want += s.bytes; }
    for (const auto& s : R.rows) {
        for (uint32_t o = 0; o < s.bytes; o += 0x90, n_r++) bad_r += memcmp(vm_base + s.ea + o, want + o, 0x90) != 0;
        want += s.bytes;
    }
    R.rounds++;
    R.n_bodies += R.bodies.size();
    R.n_rows += n_r;
    R.bodies_bad += bad_b;
    R.rows_bad += bad_r;
    R.rounds_bad += bad_b || bad_r;
    static int shown = 0;
    if ((bad_b || bad_r) && shown < 12) {
        shown++;
        fprintf(stderr, "[native-solver] iterate check: step %llu round %u: %u of %zu bodies, %u of %u rows not as the "
                "native model left them\n", R.steps, R.round, bad_b, R.bodies.size(), bad_r, n_r);
    }
    R.pending = false;
}

void iterate_on_create(uint32_t cmd, uint32_t desc)
{
    IterateRounds& R = g_it;
    if (cmd != R.cmd) { R.cmd = cmd; R.in_cmd = 0; }
    const bool round_start = R.in_cmd++ % 3 == 0;
    if (R.pending && (cmd != 4 || round_start)) iterate_compare();
    if (cmd != 4 || !round_start) return;
    if (R.in_cmd == 1) {   // a step's first round
        if (R.steps && R.steps % 200 == 0 && g_chk.on)
            fprintf(stderr, "[native-solver] iterate check (%s): %llu steps, %llu rounds, %llu contacts a round summed: "
                    "rounds not as the native model %llu (bodies %llu of %llu, rows %llu of %llu); bodies in two cells' "
                    "lists %llu (in %llu steps); cells needing more than one packet %llu\n",
                    g_native.count(4) ? "native" : g_serial ? "lifted, one task at a time" : "lifted", R.steps, R.rounds,
                    R.contacts, R.rounds_bad, R.bodies_bad, R.n_bodies, R.rows_bad, R.n_rows, R.shared, R.steps_shared,
                    R.multi_packet);
        R.steps++;
        R.round = 0;
    }
    R.round++;
    bullet_solver::IterateShape shape;
    bullet_solver::iterate_spans(vm_base, desc, R.bodies, R.rows, shape);
    R.contacts += shape.contacts;
    if (R.round == 1) {
        R.shared += shape.shared;
        R.steps_shared += shape.shared != 0;
        R.multi_packet += shape.multi_packet;
    }
    size_t n = 0;
    for (const auto& s : R.bodies) n += s.bytes;
    for (const auto& s : R.rows) n += s.bytes;
    R.saved.resize(n);
    uint8_t* to = R.saved.data();
    spans_out(R.bodies, to);
    spans_out(R.rows, to);
    const uint32_t hash = be32(vm_base + desc + 0x10), lock = be32(vm_base + desc + 0x30);
    uint8_t masks[0x70], lk[4];
    memcpy(masks, vm_base + hash + 0x800, sizeof masks);
    memcpy(lk, vm_base + lock, sizeof lk);
    const bullet_solver::Sync sync = { bench_acquire, bench_release };
    double best = 1e30;
    unsigned cells = 0;
    const uint64_t t_at = ps3_ft_now();
    for (int rep = 0; rep < (g_bench.f ? 3 : 1); rep++) {
        bullet_solver::Stats st;
        const uint64_t t0 = ps3_ft_now();
        bullet_solver::iterate_task(vm_base, desc, sync, st);
        const uint64_t t1 = ps3_ft_now();
        if (rep == 0 && g_chk.on) {
            R.result.resize(n);
            uint8_t* r = R.result.data();
            spans_out(R.bodies, r);
            spans_out(R.rows, r);
        }
        const uint8_t* from = R.saved.data();
        spans_in(R.bodies, from);
        spans_in(R.rows, from);
        memcpy(vm_base + hash + 0x800, masks, sizeof masks);
        memcpy(vm_base + lock, lk, sizeof lk);
        best = std::min(best, (double)(t1 - t0) / 1000.0);
        cells = st.cells;
    }
    if (g_bench.f) {
        fprintf(g_bench.f, "4 %.1f %u %u %.2f\n", (double)(int64_t)(t_at - g_ps3_ft.base) / 1000.0, shape.contacts, cells, best);
        fflush(g_bench.f);
    }
    R.pending = g_chk.on;
}

void on_create(uint32_t entry, const uint32_t arg[4])
{
    if (!g_cap.steps.empty()) capture_on_create(entry, arg);
    if ((g_chk.on || g_bench.f) && entry == kEntrySolver) {
        const uint32_t desc = desc_from_queue(arg[2], arg[0]);
        if (desc) iterate_on_create(be32(vm_base + desc), desc);
    }
    if (g_chk.on) check_on_create(entry, arg);
    if (g_bench.f) bench_on_create(entry, arg);
}

}  // namespace

bool bullet_solver_direct_accept(const uint32_t entry[4], uint32_t* cmd)
{
    if (g_native.empty() || entry[1] != 1 || !entry[2]) return false;
    const uint32_t c = be32(vm_base + entry[2]);
    if (!g_native.count(c)) return false;
    *cmd = c;
    return true;
}

void bullet_solver_direct_run(uint32_t slot, uint32_t desc, uint32_t cmd)
{
    t_task_cmd = cmd;
    capture_task_state("start", slot, desc);
    if (g_stats && PS3_FT_ON()) {
        const uint8_t* d = vm_base + desc;
        const uint64_t t = ps3_ft_now();
        ps3_ft_span(PS3_FT_PHYS_CMD, (uint32_t)kImageBulletSolver, be32(d),
                    (slot & 0xFFu) | (command_count(d) & 0xFFFFFFu) << 8, t, t);
    }
    native_work(desc, cmd);
}

void bullet_solver_init(bool phys_stats)
{
    g_stats = phys_stats;
    if (const char* e = getenv("DOTHEROES_SOLVER_SNAP")) {
        const char* colon = strchr(e, ':');
        g_cap.prefix = colon ? colon + 1 : "solver_";
        for (const char* p = e; p && *p && p != colon; ) {
            g_cap.steps.insert((unsigned)strtoul(p, nullptr, 10));
            p = strchr(p, ',');
            if (p) p++;
            if (colon && p > colon) break;
        }
        const char* r = getenv("DOTHEROES_SOLVER_SNAP_ROUNDS");
        g_cap.all_rounds = r && *r == '1';
        if (const char* sh = getenv("DOTHEROES_SOLVER_SNAP_SHARED")) g_cap.shared_left = (unsigned)strtoul(sh, nullptr, 10);
        fprintf(stderr, "[solver-snap] capturing %zu step(s) to %s*%s\n", g_cap.steps.size(), g_cap.prefix.c_str(),
                g_cap.all_rounds ? ", every iterate round" : "");
    }
    const char* se = getenv("DOTHEROES_SOLVER_SERIAL");
    g_serial = se && *se == '1';
    if (g_serial) fprintf(stderr, "[native-solver] DOTHEROES_SOLVER_SERIAL: lifted iterate tasks one at a time\n");
    if (const char* e = getenv("DOTHEROES_NATIVE_SOLVER")) {
        for (const char* p = e; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr) {
            const unsigned c = (unsigned)strtoul(p, nullptr, 10);
            if (c >= 4 && c <= 6) g_native.insert(c);
            else fprintf(stderr, "[native-solver] command %u has no native version; it runs lifted\n", c);
        }
        if (!g_native.empty()) fprintf(stderr, "[native-solver] native (experimental):%s%s%s\n", g_native.count(4) ? " iterate" : "",
                                       g_native.count(5) ? " the copy-back" : "", g_native.count(6) ? " the warm-start write-back" : "");
    }
    const char* ck = getenv("DOTHEROES_NATIVE_SOLVER_CHECK");
    g_chk.on = ck && *ck == '1';
    if (const char* bf = getenv("DOTHEROES_SOLVER_BENCH")) {
        g_bench.f = fopen(bf, "w");
        if (g_bench.f) {
            fprintf(g_bench.f, "# command t_us items cells native_us (best of 3, the task's own inputs): 6 write-back, "
                    "one task for every contact; 5 copy-back, each task's range of bodies; 4 iterate, each round as "
                    "one task (items: contacts)\n");
            fflush(g_bench.f);
            if (g_native.count(6)) fprintf(stderr, "[native-solver] DOTHEROES_SOLVER_BENCH is for runs with the lifted write-back\n");
        }
    }
    if (!g_cap.steps.empty() || g_chk.on || g_bench.f) g_spurs_create_hook = on_create;
    if (g_stats || g_spurs_create_hook || !g_native.empty() || g_serial)
        spu_workload_native_register(kImageBulletSolver, solver_run, solver_after);
}
#else
void bullet_solver_init(bool) {}
#endif
