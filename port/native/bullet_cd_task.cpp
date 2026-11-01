// The game's BulletCD SPU task (runtime image 4, CreateTask entry 0xF63600)
// done by native code (bullet_cd.cpp), registered with the runtime's native
// task hook (spu_workload_native_register). Experimental: not bit-exact with
// the SPU task (docs/BULLET_NATIVE.md, "How close it is"), so the lifted task
// stays the default. DOTHEROES_NATIVE_CD:
//   0 (default)  the lifted SPU task, as before
//   1            native (experimental): the task's whole work done here
//   2            check: native works out the result without writing it, the
//                lifted task does the work, and the two are compared
//   3            self-check: as 2, but native rounding to nearest against
//                native rounding toward zero (how much rounding alone moves it)
// DOTHEROES_NATIVE_CD_FP=rz runs native toward zero (the SPU's rounding);
// DOTHEROES_PHYS_STATS=1 (any mode, 0 included) counts each collision task's
// workload (record_census below).
// DOTHEROES_NATIVE_CD_DUMP=n prints the first n manifolds of each kind of
// difference in full.
// A task whose pairs bullet_cd does not handle runs lifted in every mode,
// counted by reason (each logged when first seen). Every 250 tasks a
// [native-cd] line says how many ran which way (and, checking, how the
// results compared, by kind of pair). Without Bullet 2.74 at build time
// (DOTHEROES_BULLET_CD undefined) this is an empty init.
#include "bullet_cd_task.h"

#ifdef DOTHEROES_BULLET_CD
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <xmmintrin.h>

#include "bullet_cd.h"
#include "bullet_solver_task.h"
#include "ps3emu/ps3_ftrace.h"
#include "spu_workload.h"

extern "C" uint8_t* vm_base;

namespace {

constexpr int kImageBulletCD = 4;
constexpr uint32_t kCmdGatherAndProcess = 1;

int g_mode;
std::atomic<unsigned long long> g_tasks{ 0 }, g_native{ 0 }, g_lifted{ 0 };
std::mutex g_mu;   // guards what follows
std::unordered_map<std::string, unsigned long long> g_unsupported;
bool g_rz;         // DOTHEROES_NATIVE_CD_FP=rz: native truncates like the SPU
int g_dump_left;  // DOTHEROES_NATIVE_CD_DUMP=n: the first n of each kind of difference in full
unsigned long long g_checked;   // tasks compared
// Mesh/box contact points whose normal pushes the box down into the ground
// (world y beyond 0.7 that way; resting debris has it the other way): for
// the lifted task's results [0] and native's [1], against all their points.
unsigned long long g_down[2], g_mesh_points[2];
uint32_t be32(const uint8_t* p);
float bef(const uint8_t* p);
void count_down(const uint8_t* mf, uint32_t types, int who)
{
    const uint32_t t0 = types >> 8, t1 = types & 0xFF;
    if (!((t0 == 19 && t1 == 0) || (t0 == 0 && t1 == 19))) return;
    // the normal is on body 1, pointing at body 0: on the box when the mesh is body 0
    const float down = t0 == 19 ? 1.f : -1.f;
    for (uint32_t i = 0, n = be32(mf + 0x2C8); i < n && i < 4; i++) {
        g_mesh_points[who]++;
        g_down[who] += bef(mf + i * 0xB0 + 0x44) * down > 0.7f;
    }
}
// How a manifold compares, from close to far. Points match within kEqual
// (float noise) or kSmall (well inside the contact breaking threshold,
// 0.0121 for debris) in position and distance, and kSmallDeg in normal;
// a normal more than kMaterialDeg from every point of the other side is a
// different contact direction.
constexpr double kEqual = 1e-4, kSmall = 2e-3, kEqualDeg = 0.01, kSmallDeg = 1.0, kMaterialDeg = 5.0;
enum Class {
    kIdentical,        // every point in place, within float noise
    kSmallNumeric,     // every point in place, within kSmall / kSmallDeg
    kReordered,        // the same points (within kSmall) in other slots
    kOtherPoints,      // other contact points, every normal within kMaterialDeg of one on the other side
    kOtherNormal,      // a normal more than kMaterialDeg from all of the other side's
    kCountNormalsAgree,  // another point count, normals agreeing
    kCountOtherNormal,   // another point count and a normal more than kMaterialDeg off
    kOnlyLifted,       // written by the lifted task (self-check: toward zero) alone
    kOnlyNative,       // written by native alone
    kClasses
};
const char* const kClassNames[kClasses] = {
    "identical", "small numerical", "reordered", "other points, normals agree", "normal >5 deg off",
    "point count, normals agree", "point count and normal >5 deg off", "only lifted wrote", "only native wrote" };
struct Check {   // per kind of pair
    unsigned long long n[kClasses] = {};
    unsigned long long manifolds = 0;   // written by either
    double small_pos = 0, small_deg = 0;    // largest differences among the small-numerical ones
    double max_deg = 0;                     // largest normal difference among other-normal ones
} g_check[3];
const char* const kKinds[3] = { "box/box", "mesh/box", "with a cylinder" };
int kind(uint32_t types)
{
    const uint32_t t0 = types >> 8, t1 = types & 0xFF;
    return t0 == 13 || t1 == 13 ? 2 : t0 == 19 || t1 == 19 ? 1 : 0;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
float bef(const uint8_t* p) { const uint32_t u = be32(p); float f; memcpy(&f, &u, 4); return f; }

void count_task()
{
    const unsigned long long n = ++g_tasks;
    if (n % 250) return;
    std::lock_guard<std::mutex> lk(g_mu);
    std::string uns;
    for (const auto& u : g_unsupported) uns += " [" + u.first + "] " + std::to_string(u.second);
    fprintf(stderr, "[native-cd] %llu tasks: %llu native, %llu lifted; unsupported:%s\n", n,
            g_native.load(), g_lifted.load(), uns.empty() ? " none" : uns.c_str());
    fprintf(stderr, "[native-cd] mesh/box points pushing the box down: lifted %llu of %llu, native %llu of %llu\n",
            g_down[0], g_mesh_points[0], g_down[1], g_mesh_points[1]);
    if (g_mode < 2) return;
    fprintf(stderr, "[native-cd] %s%s, %llu tasks:\n", g_mode == 3 ? "self-check, nearest against toward zero" : "check",
            g_mode == 2 && g_rz ? " (native rounding toward zero)" : "", g_checked);
    for (int k = 0; k < 3; k++) {
        const Check& c = g_check[k];
        std::string line;
        for (int i = 0; i < kClasses; i++) {
            char b[96];
            snprintf(b, sizeof b, "%s%s %llu (%.2f%%)", i ? ", " : "", kClassNames[i], c.n[i],
                     c.manifolds ? 100.0 * c.n[i] / c.manifolds : 0.0);
            line += b;
        }
        fprintf(stderr, "[native-cd]   %-15s %llu manifolds written: %s; small numerical up to %.2g position, "
                "%.2g deg; other normals up to %.1f deg\n", kKinds[k], c.manifolds, line.c_str(),
                c.small_pos, c.small_deg, c.max_deg);
    }
}

void lifted(const char* why)
{
    if (why) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_unsupported[why]++) fprintf(stderr, "[native-cd] runs lifted: %s\n", why);
    }
    g_lifted++;
}

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

// The native run, under the SPU's rounding if asked: truncation, denormals as zero
void native_run(const bullet_cd::Mem& m, uint32_t desc, bullet_cd::Result& r, bool write, bool rz)
{
    const unsigned csr = _mm_getcsr();
    if (rz) _mm_setcsr(csr | 0x6000u | 0x8040u);   // RC toward zero, FTZ, DAZ
    bullet_cd::run(m, desc, r, write);
    if (rz) _mm_setcsr(csr);
}

// check modes, from cd_run to cd_after on the same worker: each manifold
// before, as native would leave it, and (mode 3) as native rounding toward
// zero would -- compared with that instead of the lifted task's
struct Pending {
    std::vector<bullet_cd::ManifoldOut> before, native, other;
};
thread_local Pending* t_pending;

// DOTHEROES_PHYS_STATS=1, any mode: each collision task's workload
// (bullet_cd::census, after the task's work) as a frame-trace instant
// (PS3_FT_PHYS_LOAD: who = bodies, a = pairs processed, b = contact points)
// and a [phys-stats] line every 250 tasks. Lifted tasks are counted after
// they return, so they run exactly as without it.
bool g_stats;
struct StatSum { unsigned long long tasks = 0, pairs = 0, active = 0, mesh = 0, boxbox = 0, cylinder = 0, bodies = 0, points = 0; } g_stat;

void record_census(uint32_t desc)
{
    bullet_cd::Census c;
    bullet_cd::census(bullet_cd::Mem{ vm_base }, desc, c);
    if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now(); ps3_ft_span(PS3_FT_PHYS_LOAD, (uint32_t)c.bodies, (uint32_t)c.active, (uint32_t)c.points, t, t); }
    std::lock_guard<std::mutex> lk(g_mu);
    StatSum& s = g_stat;
    s.tasks++; s.pairs += c.pairs; s.active += c.active; s.mesh += c.mesh; s.boxbox += c.boxbox;
    s.cylinder += c.cylinder; s.bodies += c.bodies; s.points += c.points;
    if (s.tasks % 250 == 0)
        fprintf(stderr, "[phys-stats] %llu collision tasks: %llu SPU pairs, %llu processed (%llu mesh/box, %llu box/box, "
                "%llu with a cylinder), %llu awake bodies, %llu contact points\n", s.tasks, s.pairs, s.active, s.mesh,
                s.boxbox, s.cylinder, s.bodies, s.points);
}

// The descriptor of a slot's task, from its input queue's ring: every entry
// for a slot names the same one (fixed for the run), and one is there
// whether or not the task has popped it yet.
uint32_t desc_from_queue(uint32_t inq, uint32_t slot)
{
    const uint32_t buf = be32(vm_base + inq + 0x14), depth = be32(vm_base + inq + 0x0C);
    for (uint32_t i = 0; i < depth && i < 16; i++) {
        const uint8_t* e = vm_base + buf + i * 16u;
        if (be32(e) == slot && be32(e + 4) == kCmdGatherAndProcess && be32(e + 8)) return be32(e + 8);
    }
    return 0;
}

int cd_run(uint32_t, uint32_t, const uint32_t arg[4])
{
    if (g_mode == 0) return 0;   // registered for DOTHEROES_PHYS_STATS alone: the lifted task, counted after
    const uint32_t inq = arg[2], outq = arg[3];
    uint8_t entry[16];
    if (!peek_command(inq, entry)) { lifted("no command within 20 ms"); count_task(); return 0; }
    if (be32(entry + 4) != kCmdGatherAndProcess) { lifted("command not 1"); count_task(); return 0; }
    const uint32_t desc = be32(entry + 8);
    const bullet_cd::Mem m = { vm_base };
    char why[160];
    if (!bullet_cd::supported(m, desc, why, sizeof why)) { lifted(why); count_task(); return 0; }
    if (g_mode >= 2) {
        Pending* p = new Pending;
        bullet_cd::manifolds(m, desc, p->before);
        bullet_cd::Result r;
        native_run(m, desc, r, false, g_rz);
        p->native = std::move(r.manifolds);
        if (g_mode == 3) {
            bullet_cd::Result z;
            native_run(m, desc, z, false, true);
            p->other = std::move(z.manifolds);
        }
        t_pending = p;
        lifted(nullptr);
        return 0;   // the lifted task does the work; cd_after compares
    }
    if (!spurs_queue_spu_pop(inq, entry)) { lifted("command gone"); count_task(); return 0; }
    bullet_cd::Result r;
    native_run(m, desc, r, true, g_rz);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& o : r.manifolds) count_down(o.bytes, o.types, 1);
    }
    if (g_stats) record_census(desc);
    while (!spurs_queue_spu_push(outq, entry)) std::this_thread::yield();
    g_native++;
    count_task();
    return 1;
}

// check mode: the lifted task's manifolds against native's. The PPU may
// already have the solver writing impulses into them, so only what the
// collision task owns is compared: point count, local and world points,
// normal, distance, friction, restitution, lifetime.
struct PointDiff {
    double pos = 0, dist = 0, deg = 0;   // positions (local A/B, world A/B), distance, normal angle
    bool exact = true;                   // friction, restitution, lifetime equal
    bool within(double tol, double tol_deg) const { return pos <= tol && dist <= tol && deg <= tol_deg && exact; }
};
double angle_deg(const uint8_t* a, const uint8_t* b)
{
    double dot = 0, la = 0, lb = 0;
    for (int k = 0; k < 3; k++) {
        const double x = bef(a + 0x40 + 4 * k), y = bef(b + 0x40 + 4 * k);
        dot += x * y; la += x * x; lb += y * y;
    }
    const double c = la > 0 && lb > 0 ? dot / sqrt(la * lb) : 1.0;
    return acos(fmax(-1.0, fmin(1.0, c))) * 57.29577951308232;
}
PointDiff point_diff(const uint8_t* a, const uint8_t* b)
{
    PointDiff d;
    for (int k = 0; k < 3; k++)
        for (uint32_t off : { 0x00u, 0x10u, 0x20u, 0x30u })
            d.pos = fmax(d.pos, fabs((double)bef(a + off + 4 * k) - bef(b + off + 4 * k)));
    d.dist = fabs((double)bef(a + 0x50) - bef(b + 0x50));
    d.deg = angle_deg(a, b);
    for (uint32_t off : { 0x54u, 0x58u, 0x80u }) d.exact &= be32(a + off) == be32(b + off);
    return d;
}

void dump_manifold(const char* what, const uint8_t* mf)
{
    const uint32_t n = be32(mf + 0x2C8);
    fprintf(stderr, "[native-cd]   %-6s %u point(s)\n", what, n);
    for (uint32_t i = 0; i < n && i < 4; i++) {
        const uint8_t* q = mf + i * 0xB0;
        fprintf(stderr, "[native-cd]     %u: localA %9.5f %9.5f %9.5f  normal %7.4f %7.4f %7.4f  dist %9.6f  life %u\n",
                i, bef(q), bef(q + 4), bef(q + 8), bef(q + 0x40), bef(q + 0x44), bef(q + 0x48), bef(q + 0x50),
                be32(q + 0x80));
    }
}

// A normal of one side more than kMaterialDeg from every normal of the other.
double normal_gap(const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb)
{
    double worst = 0;
    for (int side = 0; side < 2; side++) {
        const uint8_t* x = side ? b : a; const uint8_t* y = side ? a : b;
        const uint32_t nx = side ? nb : na, ny = side ? na : nb;
        for (uint32_t i = 0; i < nx; i++) {
            double best = 180;
            for (uint32_t j = 0; j < ny; j++) best = fmin(best, angle_deg(x + i * 0xB0, y + j * 0xB0));
            if (ny) worst = fmax(worst, best);
        }
    }
    return worst;
}

Class classify(const uint8_t* n, const uint8_t* l, Check& c)
{
    const uint32_t nc = be32(n + 0x2C8), lc = be32(l + 0x2C8);
    if (nc > 4 || lc > 4) return kCountOtherNormal;
    const double gap = normal_gap(n, nc, l, lc);
    if (nc != lc) {
        if (gap > kMaterialDeg) { c.max_deg = fmax(c.max_deg, gap); return kCountOtherNormal; }
        return kCountNormalsAgree;
    }
    bool equal = true, small = true, as_set = true;
    double pos = 0, deg = 0;
    for (uint32_t i = 0; i < nc; i++) {
        const PointDiff d = point_diff(n + i * 0xB0, l + i * 0xB0);
        equal &= d.within(kEqual, kEqualDeg);
        small &= d.within(kSmall, kSmallDeg);
        pos = fmax(pos, fmax(d.pos, d.dist));
        deg = fmax(deg, d.deg);
        bool found = false;
        for (uint32_t j = 0; j < nc && !found; j++) found = point_diff(n + j * 0xB0, l + i * 0xB0).within(kSmall, kSmallDeg);
        as_set &= found;
    }
    if (equal) return kIdentical;
    if (small) { c.small_pos = fmax(c.small_pos, pos); c.small_deg = fmax(c.small_deg, deg); return kSmallNumeric; }
    if (as_set) return kReordered;
    if (gap > kMaterialDeg) { c.max_deg = fmax(c.max_deg, gap); return kOtherNormal; }
    return kOtherPoints;
}

void compare(const Pending& p)
{
    std::unordered_map<uint32_t, const uint8_t*> nat;
    std::unordered_map<uint32_t, const uint8_t*> other;
    for (const auto& o : p.native) nat[o.ea] = o.bytes;
    for (const auto& o : p.other) other[o.ea] = o.bytes;
    std::lock_guard<std::mutex> lk(g_mu);
    g_checked++;
    for (const auto& b : p.before) {
        Check& c = g_check[kind(b.types)];
        const uint8_t* now = vm_base + b.ea;   // the lifted task's, or (mode 3) native toward zero
        if (g_mode == 3) { const auto o = other.find(b.ea); now = o == other.end() ? b.bytes : o->second; }
        const bool lifted_wrote = memcmp(b.bytes, now, bullet_cd::kManifoldBytes) != 0;
        const auto it = nat.find(b.ea);
        const bool native_wrote = it != nat.end() && memcmp(b.bytes, it->second, bullet_cd::kManifoldBytes) != 0;
        if (!lifted_wrote && !native_wrote) continue;
        c.manifolds++;
        const uint8_t* n = native_wrote ? it->second : nullptr;
        const Class k = !native_wrote ? kOnlyLifted : !lifted_wrote ? kOnlyNative : classify(n, now, c);
        c.n[k]++;
        if (g_mode == 2) {
            if (lifted_wrote) count_down(now, b.types, 0);
            if (n) count_down(n, b.types, 1);
        }
        // the first few of each kind of difference, in full (DOTHEROES_NATIVE_CD_DUMP)
        if (k >= kOtherPoints && g_dump_left > 0) {
            static int shown[kClasses];
            if (shown[k]++ < g_dump_left) {
                fprintf(stderr, "[native-cd] manifold %08X (types %u/%u, breaking threshold %g): %s\n", b.ea,
                        b.types >> 8, b.types & 0xFF, bef(b.bytes + 0x2CC), kClassNames[k]);
                dump_manifold("before", b.bytes);
                if (n) dump_manifold("native", n);
                if (lifted_wrote) dump_manifold(g_mode == 3 ? "rz" : "lifted", now);
            }
        }
    }
}

void cd_after(uint32_t, uint32_t, const uint32_t arg[4])
{
    if (g_stats) {   // the lifted task's work, as it left it
        const uint32_t desc = desc_from_queue(arg[2], arg[0]);
        if (desc) record_census(desc);
    }
    Pending* p = t_pending;
    t_pending = nullptr;
    if (!p) return;   // stats only, or ran lifted as unsupported: counted in cd_run
    compare(*p);
    delete p;
    count_task();
}

}  // namespace

extern "C" void dotheroes_native_init(void)
{
    const char* e = getenv("DOTHEROES_NATIVE_CD");
    g_mode = e ? atoi(e) : 0;
    if (g_mode < 0 || g_mode > 3) g_mode = 0;
    const char* st = getenv("DOTHEROES_PHYS_STATS");
    g_stats = st && *st == '1';
    bullet_solver_init(g_stats);   // the solver task's own switches (bullet_solver_task.cpp)
    if (g_mode == 0 && !g_stats) return;
    const char* fp = getenv("DOTHEROES_NATIVE_CD_FP");
    g_rz = fp && !strcmp(fp, "rz");
    const char* dump = getenv("DOTHEROES_NATIVE_CD_DUMP");
    g_dump_left = dump ? atoi(dump) : 0;
    spu_workload_native_register(kImageBulletCD, cd_run, cd_after);
    fprintf(stderr, "[native-cd] Bullet collision task: %s%s\n",
            g_mode == 0 ? "lifted" : g_mode == 1 ? "native (experimental)"
                        : g_mode == 2 ? "check (native against lifted)"
                        : "self-check (native rounding to nearest against toward zero)",
            g_stats ? "; workload counted (DOTHEROES_PHYS_STATS)" : "");
}
#else
extern "C" void dotheroes_native_init(void) {}
#endif
