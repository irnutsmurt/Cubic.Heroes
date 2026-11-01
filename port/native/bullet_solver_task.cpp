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
// DOTHEROES_NATIVE_SOLVER=<command>[,<command>...] (experimental) does those
// commands here instead of the lifted image -- so far only 6, the warm-start
// write-back (bullet_solver.cpp, on the game's structures as checked against
// captured steps). A native task keeps the task's protocol: it waits for its
// slot's command, pops it, takes cells under the same spin lock and masks the
// lifted tasks of the round use (so a lifted task beside it sees every claim),
// and pushes its own completion; the PPU still creates three tasks a round and
// waits for all three. A task whose command is not there within 20 ms, or names
// another descriptor, runs lifted, counted. Every 5000 native tasks a
// [native-solver] line counts them.
//
// DOTHEROES_NATIVE_SOLVER_CHECK=1: as each step's first write-back task is
// created, what the write-back must leave -- min(3, applied impulse) in the
// first float of each contact's 16 bytes, the other 12 untouched -- is worked
// out from the inputs; as the step's last write-back task finishes, memory is
// compared with it. With the lifted write-back, that compares the native model
// with what the game's task did; with the native one, it checks that every
// contact was written. Also counted: contacts changed again after the solver,
// before the next collision task. A [native-solver] check line every 200 steps.
//
// The capture writes too every solver task created in the step: its slot, command, round, descriptor
// address and bytes; and, for iterate and write-back, as each task starts and
// as it returns, the spin lock's 8 bytes and the hash's cell masks (the lock
// itself is freed by the step's end). A snapshot is ~170 MB of game memory:
// keep it out of git.
#include "bullet_solver_task.h"

#ifdef DOTHEROES_BULLET_CD
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
int native_command(uint32_t desc, uint32_t cmd, const uint32_t arg[4]);
std::set<uint32_t> g_native;   // DOTHEROES_NATIVE_SOLVER

int solver_run(uint32_t, uint32_t, const uint32_t arg[4])
{
    const uint32_t desc = desc_from_queue(arg[2], arg[0]);
    t_task_cmd = desc ? be32(vm_base + desc) : 0;
    if (desc) capture_task_state("start", arg[0], desc);
    if (desc && g_stats && PS3_FT_ON()) {
        const uint8_t* d = vm_base + desc;
        const uint64_t t = ps3_ft_now();
        ps3_ft_span(PS3_FT_PHYS_CMD, (uint32_t)kImageBulletSolver, be32(d),
                    (arg[0] & 0xFFu) | (command_count(d) & 0xFFFFFFu) << 8, t, t);
    }
    if (desc && !g_native.empty() && g_native.count(be32(vm_base + desc)))
        return native_command(desc, be32(vm_base + desc), arg);
    return 0;   // the lifted solver task
}

void solver_after(uint32_t, uint32_t, const uint32_t arg[4])
{
    const uint32_t desc = desc_from_queue(arg[2], arg[0]);
    if (desc) capture_task_state("end", arg[0], desc);
    check_task_done(t_task_cmd);   // a lifted task (native ones return before this); by the command it
                                   // started with: its slot's descriptor may already hold the next one
}

// DOTHEROES_SOLVER_SNAP: on the creating PPU thread, before the task can run.
struct Capture {
    std::set<unsigned> steps;
    std::string prefix;
    unsigned step = 0, in_command = 0, n = 0;
    uint32_t cmd = 0;
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
    if (cmd != c.cmd) {
        if (cmd == 1 && c.cmd != 1 && ++c.step && c.steps.count(c.step)) {
            char path[700];
            snprintf(path, sizeof path, "%s%u_tasks.txt", c.prefix.c_str(), c.step);
            std::lock_guard<std::mutex> lk(c.mu);
            c.log = fopen(path, "w");
            c.n = 0;
            if (c.log) fprintf(c.log, "# step %u: solver tasks as created (slot, command, round, descriptor, its 0x%X bytes, task argument)\n",
                               c.step, kDescBytes);
        }
        c.in_command = 0;
        c.cmd = cmd;
    }
    const unsigned round = cmd == 4 || cmd == 6 ? c.in_command / 3 + 1 : 1;
    if (c.log) {
        if (c.in_command == 0 || ((cmd == 4 || cmd == 6) && c.in_command == 3)) {
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
std::atomic<unsigned long long> g_no_command{ 0 }, g_other_desc{ 0 };

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

int native_command(uint32_t desc, uint32_t cmd, const uint32_t arg[4])
{
    const uint32_t inq = arg[2], outq = arg[3];
    uint8_t entry[16];
    if (!peek_command(inq, entry)) { g_no_command++; return 0; }
    if (be32(entry + 4) != 1 || be32(entry + 8) != desc) { g_other_desc++; return 0; }
    if (!spurs_queue_spu_pop(inq, entry)) { g_no_command++; return 0; }
    bullet_solver::Stats st;
    const bullet_solver::Sync sync = { lock_acquire, lock_release };
    if (cmd == 6) bullet_solver::writeback_task(vm_base, desc, sync, st);
    check_task_done(cmd);   // before the PPU can see this task done
    while (!spurs_queue_spu_push(outq, entry)) std::this_thread::yield();
    g_cells += st.cells; g_contacts += st.contacts; g_waits += st.waits;
    const unsigned long long n = ++g_ran_native;
    if (n % 5000 == 0)
        fprintf(stderr, "[native-solver] %llu native tasks: %llu cells, %llu contacts written back, %llu waits for a "
                "held cell; lifted instead: no command within 20 ms %llu, another descriptor %llu\n",
                n, g_cells.load(), g_contacts.load(), g_waits.load(), g_no_command.load(), g_other_desc.load());
    return 1;
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

void check_on_create(uint32_t entry, const uint32_t arg[4])   // the PPU thread
{
    Check& k = g_chk;
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

void on_create(uint32_t entry, const uint32_t arg[4])
{
    if (!g_cap.steps.empty()) capture_on_create(entry, arg);
    if (g_chk.on) check_on_create(entry, arg);
}

}  // namespace

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
        fprintf(stderr, "[solver-snap] capturing %zu step(s) to %s*\n", g_cap.steps.size(), g_cap.prefix.c_str());
    }
    if (const char* e = getenv("DOTHEROES_NATIVE_SOLVER")) {
        for (const char* p = e; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr) {
            const unsigned c = (unsigned)strtoul(p, nullptr, 10);
            if (c == 6) g_native.insert(c);
            else fprintf(stderr, "[native-solver] command %u has no native version; it runs lifted\n", c);
        }
        if (!g_native.empty()) fprintf(stderr, "[native-solver] native (experimental): the warm-start write-back\n");
    }
    const char* ck = getenv("DOTHEROES_NATIVE_SOLVER_CHECK");
    g_chk.on = ck && *ck == '1';
    if (!g_cap.steps.empty() || g_chk.on) g_spurs_create_hook = on_create;
    if (g_stats || g_spurs_create_hook || !g_native.empty())
        spu_workload_native_register(kImageBulletSolver, solver_run, solver_after);
}
#else
void bullet_solver_init(bool) {}
#endif
