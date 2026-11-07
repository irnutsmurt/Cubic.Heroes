// R8: the game's native physics tasks issued and completed directly, without
// SPURS (docs/NATIVE_R8.md section 6.1).
//
// The game hands each physics task to SPURS in one function and waits for
// any task of a manager in another (both found by reading the lift; nothing
// else creates these tasks, pushes their input queues or pops their output
// queues):
//   func_00955CF0(M, slot, w0..w3): cellSpursCreateTask on the manager's
//     taskset (M+0x80) with the argument [slot, M+0x2300, input queue
//     M+0x1A00+slot*0x80, output queue M+0x1D00+slot*0x80], then
//     cellSpursQueuePushBody of the entry [w0..w3] = [slot, command,
//     descriptor, slot] on the input queue; r3 = 0, or -1 if the push failed.
//   func_00955BD4(M, &cursor, &w0..&w3): round the slots 0..*(M+0x1980) from
//     *cursor, a non-blocking cellSpursQueuePopBody of each slot's output
//     queue, until one gives an entry; the cursor left at that slot, the
//     entry's four words stored through the four pointers; r3 = 0.
// The two managers are Bullet's collision (task image at 0xF63600) and its
// solver (0xF8EC80); a step is ~73 tasks, the PPU waiting for every task of
// a command before it issues the next.
//
// With a native version on (DOTHEROES_NATIVE_CD=1, DOTHEROES_NATIVE_SOLVER),
// SPURS was only an envelope round native code: CreateTask's bookkeeping and
// its hash of the 174 KB task image, the pool's wake-up, the task waiting for
// its entry, the queue operations under the lock-line lock, the PPU's polls.
// Here, through two entry hooks (config/r8_hooks.json):
//  - the issue of a task whose work is native becomes a job in a small native
//    pool: no SPURS task, no queue push, no taskset slot; the hook returns 0
//    as the game's function does. Any other task (solver commands 1 and 2,
//    collision work bullet_cd does not cover) is declined: the lifted body
//    creates its SPURS task as before.
//  - the wait goes round the slots as the game's does, taking for each slot a
//    native job's completion or, while the manager has lifted tasks out, an
//    entry from the slot's SPURS output queue (the runtime's host-side pop:
//    the same pop under the lock-line lock, its coherence notice and its
//    wake-up of an SPU waiting to push, into host memory). It writes the
//    cursor and the entry as the game's poll does. A native job's entry is
//    the one the task would have pushed back: the one it was issued with.
//    While nothing is complete the waiting thread runs a job not yet started
//    itself, then spins.
// What is kept: the order (a job is issued when the game issues it, taken in
// issue order, and the game still waits for all of a command before the
// next); visibility (a job is published to its runner and its completion to
// the poller with release and acquire, so the runner sees everything the
// PPU wrote before the issue, and the PPU everything the job wrote once it
// sees it done); lifetimes (a job holds no guest resource past its
// completion, as before: the descriptor and its data are the game's, and the
// game does not touch them until it has seen the task done). The work itself
// is the native tasks' (bullet_cd_task.cpp, bullet_solver_task.cpp), with
// their checks, captures and counts.
//
// DOTHEROES_PHYS_DIRECT=0 leaves every task to SPURS, as before R8.
// DOTHEROES_PHYS_DIRECT_WORKERS=<n> (default 2): pool threads besides the
// waiting PPU thread.
#include "phys_direct.h"

#ifdef DOTHEROES_BULLET_CD
#include "bullet_cd_task.h"
#include "bullet_solver_task.h"
#include "ps3emu/ps3_ftrace.h"
#include "ps3emu/thread_name.h"
#include "spu_workload.h"   /* spurs_queue_spu_pop */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void vm_write32(uint64_t a, uint32_t v);
extern "C" void (*g_spurs_create_hook)(uint32_t entry, const uint32_t arg[4]);   // cellSpurs.c

extern "C" int g_phys_direct = 0;

namespace {

constexpr uint32_t kElfCollision = 0xF63600, kElfSolver = 0xF8EC80;
constexpr int kImageCollision = 4, kImageSolver = 5;   // the runtime's image ids, for the frame trace
constexpr int kMaxJobs = 64, kMaxManagers = 4;

enum : int { kFree = 0, kQueued = 1, kRunning = 2, kDone = 3 };

struct Job {
    std::atomic<int> state{ kFree };
    uint32_t m = 0, slot = 0, entry[4] = {}, cmd = 0;
    int image = 0;
    uint64_t seq = 0, t_issue = 0;
};
Job g_jobs[kMaxJobs];

struct Manager {
    uint32_t m = 0;
    int image = 0;
    std::atomic<int> native_out{ 0 };   // native jobs issued, not yet taken by a wait
    int lifted_out = 0;                 // lifted tasks issued, their entries not yet popped (the PPU thread's)
    bool described = false;
};
Manager g_mgr[kMaxManagers];
int g_nmgr;                             // the PPU thread's

std::mutex g_mu;                        // the job table's queued -> running step, and the sleepers
std::condition_variable g_cv;
std::atomic<int> g_queued{ 0 };
int g_sleepers;                         // under g_mu
uint64_t g_seq;                         // under g_mu

int g_workers;                          // the pool's threads, started by the first native task
std::atomic<unsigned long long> g_n_native{ 0 }, g_n_lifted{ 0 }, g_n_by_waiter{ 0 }, g_n_hle_pops{ 0 },
    g_n_waits{ 0 }, g_wait_ns{ 0 }, g_ran_ns{ 0 };

inline void cpu_relax()
{
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

inline uint32_t host_tid()
{
#ifdef _WIN32
    return (uint32_t)GetCurrentThreadId();
#else
    return 0;
#endif
}

inline uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The managers' table and their lifted-task counts belong to the one thread
// that issues and waits (the game's main thread, its physics step): a second
// thread at either would race them, so it stops the program instead.
uint32_t g_owner;   // the host thread id of the first issue or wait

Manager* manager(uint32_t m, bool add)
{
    const uint32_t me = host_tid();
    if (!g_owner) g_owner = me;
    else if (g_owner != me) {
        fprintf(stderr, "[phys-direct] FATAL: a physics task issued or awaited on host thread %u, the first on %u: "
                "the direct issue assumes one thread (DOTHEROES_PHYS_DIRECT=0 runs it through SPURS)\n", me, g_owner);
        fflush(stderr);
        abort();
    }
    for (int i = 0; i < g_nmgr; i++)
        if (g_mgr[i].m == m) return &g_mgr[i];
    if (!add || g_nmgr == kMaxManagers) return nullptr;
    const uint32_t elf = vm_read32(m + 0x1984);
    if (elf != kElfCollision && elf != kElfSolver) return nullptr;
    Manager& g = g_mgr[g_nmgr];
    g.m = m;
    g.image = elf == kElfCollision ? kImageCollision : kImageSolver;
    g_nmgr++;   // only the PPU thread adds and reads this
    return &g;
}

// The oldest queued job, now running (nullptr if none). g_mu held.
Job* take_locked()
{
    Job* best = nullptr;
    for (Job& j : g_jobs)
        if (j.state.load(std::memory_order_relaxed) == kQueued && (!best || j.seq < best->seq)) best = &j;
    if (best) {
        best->state.store(kRunning, std::memory_order_relaxed);
        g_queued.fetch_sub(1, std::memory_order_relaxed);
    }
    return best;
}

Job* take()
{
    if (!g_queued.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    return take_locked();
}

void run(Job* j, bool by_waiter)
{
    const uint64_t t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    if (j->image == kImageCollision) bullet_cd_direct_run(j->entry);
    else bullet_solver_direct_run(j->slot, j->entry[2], j->cmd);
    if (t0) {
        // as the runtime's async tasks are traced (PS3_FT_SPU_TASK): b = us from the issue to the start
        const uint64_t lat = t0 > j->t_issue ? (t0 - j->t_issue) / 1000 : 0;
        ps3_ft_span(PS3_FT_SPU_TASK, host_tid(), (uint32_t)j->image, (uint32_t)lat, t0, ps3_ft_now());
    }
    if (by_waiter) g_n_by_waiter.fetch_add(1, std::memory_order_relaxed);
    j->state.store(kDone, std::memory_order_release);   // the work, then done: the waiter acquires
}

void worker()
{
    ps3_thread_name("phys native");
    for (;;) {
        if (Job* j = take()) { run(j, false); continue; }
        // A command's next round follows within microseconds: spin a little
        // before sleeping, so the next job does not wait for a wake-up.
        const uint64_t until = now_ns() + 100000;
        bool got = false;
        for (unsigned n = 0; !got; n++) {
            if (g_queued.load(std::memory_order_acquire)) got = true;
            else if ((n & 63) == 0 && now_ns() > until) break;
            else cpu_relax();
        }
        if (got) continue;
        std::unique_lock<std::mutex> lk(g_mu);
        g_sleepers++;
        g_cv.wait(lk, [] { return g_queued.load(std::memory_order_relaxed) > 0; });
        g_sleepers--;
    }
}

// What the game's pop (cellSpursQueuePopBody) checked before taking an
// entry, which the runtime's host-side pop does not: a queue 128-byte
// aligned, SPU-to-PPU (+0x1C = 1), its entries 16 bytes (+0x08) -- the
// host-side pop copies an entry's size into a 16-byte buffer. A slot the
// game's round visits with no such queue there is skipped, as the game's
// pop answered it with an error.
bool is_output_queue(uint32_t q)
{
    return !(q & 0x7F) && vm_read32(q + 0x1C) == 1u && vm_read32(q + 0x08) == 16u && vm_read32(q + 0x0C) != 0u;
}

// A completed native job for the manager's slot: its entry, the job freed.
bool take_done(Manager* g, uint32_t slot, uint32_t e[4])
{
    if (!g->native_out.load(std::memory_order_relaxed)) return false;
    Job* best = nullptr;
    for (Job& j : g_jobs)
        if (j.state.load(std::memory_order_acquire) == kDone && j.m == g->m && j.slot == slot && (!best || j.seq < best->seq))
            best = &j;
    if (!best) return false;
    memcpy(e, best->entry, 16);
    best->state.store(kFree, std::memory_order_release);
    g->native_out.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

void report()
{
    if (!g_phys_direct) return;
    fprintf(stderr, "[phys-direct] %llu native tasks issued directly (%llu run by the waiting PPU thread), %llu lifted "
            "through SPURS (%llu queue pops); %llu waits, %.1f ms waiting, %.1f ms of it running jobs\n",
            g_n_native.load(), g_n_by_waiter.load(), g_n_lifted.load(), g_n_hle_pops.load(), g_n_waits.load(),
            g_wait_ns.load() / 1e6, g_ran_ns.load() / 1e6);
}

}  // namespace

extern "C" void phys_direct_report(void) { report(); }

extern "C" int phys_direct_issue(uint32_t m, uint32_t slot, const uint32_t entry[4])
{
    Manager* g = manager(m, true);
    if (!g) return 0;   // not a physics manager seen here: the lifted body
    uint32_t cmd = 0;
    const bool native = g->image == kImageCollision ? bullet_cd_direct_accept(entry)
                                                    : bullet_solver_direct_accept(entry, &cmd);
    if (!native) {
        g->lifted_out++;   // the lifted body creates the task and pushes its entry
        g_n_lifted.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    // What the SPURS task's creation showed a port before it could run
    // (captures, checks, benches: bullet_solver_task.cpp's on_create).
    {
        if (g_spurs_create_hook) {
            const uint32_t arg[4] = { slot, m + 0x2300u, m + 0x1A00u + slot * 0x80u, m + 0x1D00u + slot * 0x80u };
            g_spurs_create_hook(g->image == kImageCollision ? kElfCollision : kElfSolver, arg);
        }
    }
    Job* j = nullptr;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (Job& c : g_jobs)
            if (c.state.load(std::memory_order_relaxed) == kFree) { j = &c; break; }
        if (j) {
            j->m = m; j->slot = slot; memcpy(j->entry, entry, 16); j->cmd = cmd; j->image = g->image;
            j->seq = ++g_seq;
            j->t_issue = PS3_FT_ON() ? ps3_ft_now() : 0;
            g->native_out.fetch_add(1, std::memory_order_relaxed);
            j->state.store(kQueued, std::memory_order_release);
            g_queued.fetch_add(1, std::memory_order_release);
            wake = g_sleepers > 0;
        }
    }
    if (!j) {   // more jobs out than the game's protocol makes: leave it to SPURS
        static std::atomic<int> once{ 0 };
        if (!once.exchange(1)) fprintf(stderr, "[phys-direct] %d jobs out at once: this task and any more go through SPURS\n", kMaxJobs);
        g->lifted_out++;
        g_n_lifted.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (wake) g_cv.notify_one();
    // the pool, started by the first native task: a lift without the hooks
    // (the production lift, a player's) never starts it
    static std::once_flag started;
    std::call_once(started, [] { for (int i = 0; i < g_workers; i++) std::thread(worker).detach(); });
    const unsigned long long n = g_n_native.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 20000 == 0) report();
    return 1;
}

extern "C" int phys_direct_poll(uint32_t m, uint32_t cursor_ea, const uint32_t out_ea[4], uint32_t tid)
{
    Manager* g = manager(m, false);
    const uint32_t last = vm_read32(m + 0x1980);   // slots 0..last, as the game's poll goes round them
    uint32_t slot = vm_read32(cursor_ea);
    if (g && !g->described) {   // once a manager: the slots the game's round visits, and which are queues
        g->described = true;
        char q[64] = "";
        for (uint32_t k = 0; k <= last && k < 8; k++) strcat(q, is_output_queue(m + 0x1D00u + k * 0x80u) ? " q" : " -");
        fprintf(stderr, "[phys-direct] manager %08X (image %d): its wait goes round slots 0..%u:%s\n", m, g->image, last, q);
    }
    uint32_t e[4];
    uint64_t t0 = 0, w0 = 0, ran_ns = 0;   // ran_ns: of the wait, jobs this thread ran itself
    unsigned tries = 0, idle = 0;
    for (;;) {
        for (uint32_t k = 0; k <= last + 1; k++) {
            if (g && take_done(g, slot, e)) goto found;
            // A lifted task's entry, as the game's poll takes it. Without a
            // manager seen issuing (none here yet), every queue is popped.
            if ((!g || g->lifted_out > 0) && is_output_queue(m + 0x1D00u + slot * 0x80u)) {
                g_n_hle_pops.fetch_add(1, std::memory_order_relaxed);
                uint8_t b[16];
                if (spurs_queue_spu_pop(m + 0x1D00u + slot * 0x80u, b)) {
                    for (int i = 0; i < 4; i++)
                        e[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
                    if (g) g->lifted_out--;
                    goto found;
                }
            }
            slot = slot + 1 > last ? 0 : slot + 1;
            tries++;
        }
        if (!w0) { w0 = now_ns(); t0 = PS3_FT_ON() ? ps3_ft_now() : 0; }
        if (Job* j = take()) { const uint64_t r0 = now_ns(); run(j, true); ran_ns += now_ns() - r0; idle = 0; continue; }
        if (++idle < 4096) cpu_relax();
        else std::this_thread::yield();   // a lifted command's long run: let the pool's threads have the core
    }
found:
    vm_write32(cursor_ea, slot);
    for (int i = 0; i < 4; i++) vm_write32(out_ea[i], e[i]);
    if (w0) {
        g_n_waits.fetch_add(1, std::memory_order_relaxed);
        g_wait_ns.fetch_add(now_ns() - w0, std::memory_order_relaxed);
        g_ran_ns.fetch_add(ran_ns, std::memory_order_relaxed);
        // the frame trace's queue wait: a = the microseconds of it this
        // thread spent running jobs (useful work), so the polling is the rest
        if (t0) ps3_ft_span(PS3_FT_QUEUE_WAIT, tid, (uint32_t)(ran_ns / 1000), tries, t0, ps3_ft_now());
    }
    return 0;
}

extern "C" void phys_direct_init(void)
{
    const char* e = getenv("DOTHEROES_PHYS_DIRECT");
    if (e && *e == '0') return;
    const char* cd = getenv("DOTHEROES_NATIVE_CD");
    const char* sv = getenv("DOTHEROES_NATIVE_SOLVER");
    const bool any = (cd && atoi(cd) == 1) || (sv && *sv);
    if (!any) return;   // no native task: SPURS as before
    int n = 2;
    if (const char* w = getenv("DOTHEROES_PHYS_DIRECT_WORKERS")) n = atoi(w);
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    g_workers = n;   // started with the first native task (phys_direct_issue)
    g_phys_direct = 1;
    atexit(report);
    fprintf(stderr, "[phys-direct] native physics tasks issued directly where the lift has the hooks: %d pool "
            "thread(s), started with the first, and the waiting PPU thread (DOTHEROES_PHYS_DIRECT=0: through SPURS)\n", n);
}
#else
/* Without Bullet 2.74 at build time there is no native physics task: the
 * hooks' guard stays 0 and the game's functions run lifted. */
#include <stdlib.h>
extern "C" int g_phys_direct = 0;
extern "C" void phys_direct_init(void) {}
extern "C" void phys_direct_report(void) {}
extern "C" int phys_direct_issue(uint32_t, uint32_t, const uint32_t*) { return 0; }
extern "C" int phys_direct_poll(uint32_t, uint32_t, const uint32_t*, uint32_t) { abort(); }
#endif
