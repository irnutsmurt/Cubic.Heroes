/* Debris above 60 fps: render-only interpolation between physics steps (issue #2, a prototype behind a switch;
 * nothing in the game's state is written).
 *
 * Bullet steps the world at a fixed 1/60 s; above 60 fps the game draws each body at its last stepped pose, so debris
 * moves on some frames and not others. This keeps, for every body of the world, its pose after the last two physics
 * steps -- an entry hook on btDiscreteDynamicsWorld::internalSingleStepSimulation (config/r8_hooks.json) runs the
 * lifted step, then reads the world's collision objects -- and the native recorder (ppfx_recorder.cpp) draws a debris
 * piece between them: its matrix (vertex constants c256-c259) is the view-projection times the body's pose, so
 *     M' = M . W_cur^-1 . W(t),  W(t) = the pose between the last two steps at t = m_localTime / the fixed step
 * (Bullet's own remainder, in game time). A draw's body: two consecutive piece draws of one pass give
 * M_a^-1 . M_b = W_a^-1 . W_b (the camera cancels), which finds the pass's view-projection; each draw after it is
 * VP^-1 . M, a body's pose to within float rounding. A draw no body matches is drawn as the game drew it.
 *
 *   DOTHEROES_DEBRIS_INTERP=1           interpolate while the frame rate is clearly above the physics rate (under
 *                                       0.85 steps a frame over 32 frames, ~70 fps up); at 60 fps every draw is the game's
 *   DOTHEROES_DEBRIS_INTERP_LOG=<file>  each step's moving bodies, "S us world lr step" then "B obj r0 r1 r2 o" (the
 *                                       basis rows and origin), written at a test's stop and at exit (analysis)
 *
 * A body new since the last step, or one that moved more than 2 units in a step (a reused piece, a teleport), is drawn
 * at its current pose (no blend across the jump); one that turned more than 90 degrees keeps its blended position with
 * its current rotation; a body gone is never matched.
 *
 * Layouts (Bullet 2.74 for the PPU, 32-bit; checked against the lifted updateAabbs and stepSimulation):
 * btCollisionWorld -- the collision objects' array size +0x08, data +0x10; btDiscreteDynamicsWorld -- m_localTime
 * +0xD0; btCollisionObject -- m_worldTransform +0x10 (basis rows +0x10, +0x20, +0x30, origin +0x40),
 * m_activationState1 +0xC8 (2 sleeping, 5 disabled). */
#include "ppu_recomp.h"   /* ppu_context, the lifted functions */
#include "ps3emu/ps3_ftrace.h"   /* the frame trace's clock */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" { int g_debris_interp = 0; int g_debris_interp_render = 0; }
extern "C" void (*g_dotheroes_interp_write)(void);   // dev_state.cpp: called at a test's stop

namespace interp {

enum : uint32_t { kObjSize = 0x08, kObjData = 0x10, kLocalTime = 0xD0, kTransform = 0x10, kActivation = 0xC8 };

inline uint32_t r32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline float rf(uint32_t ea) { const uint32_t u = r32(ea); float f; memcpy(&f, &u, 4); return f; }

struct Pose { float m[12]; };   // basis rows (3 x 3) then the origin
inline Pose pose_of(uint32_t obj)
{
    Pose p;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) p.m[3 * r + c] = rf(obj + kTransform + 16 * r + 4 * c);
    for (int c = 0; c < 3; c++) p.m[9 + c] = rf(obj + kTransform + 0x30 + 4 * c);
    return p;
}

// ---- 4x4 (row-major, double) ---------------------------------------------------------------------------------
struct M4 { double a[4][4]; };
M4 mul(const M4& x, const M4& y)
{
    M4 r{};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += x.a[i][k] * y.a[k][j]; r.a[i][j] = s; }
    return r;
}
M4 of_pose(const Pose& p)
{
    M4 w{};
    for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) w.a[r][c] = p.m[3 * r + c]; w.a[r][3] = p.m[9 + r]; }
    w.a[3][3] = 1;
    return w;
}
M4 rigid_inverse(const M4& w)   // [R t] -> [R^T, -R^T t]
{
    M4 r{};
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) r.a[i][j] = w.a[j][i]; }
    for (int i = 0; i < 3; i++) r.a[i][3] = -(r.a[i][0] * w.a[0][3] + r.a[i][1] * w.a[1][3] + r.a[i][2] * w.a[2][3]);
    r.a[3][3] = 1;
    return r;
}
bool inverse(const M4& m, M4& out)   // Gauss-Jordan with partial pivoting
{
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m.a[i][j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int i = c + 1; i < 4; i++) if (std::fabs(a[i][c]) > std::fabs(a[p][c])) p = i;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; j++) std::swap(a[p][j], a[c][j]);
        const double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int i = 0; i < 4; i++) if (i != c) { const double f = a[i][c]; for (int j = 0; j < 8; j++) a[i][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out.a[i][j] = a[i][j + 4];
    return true;
}
M4 of_rows(const float* m)   // c256..c259, as the recorder holds them
{
    M4 r{};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.a[i][j] = m[4 * i + j];
    return r;
}

// ---- rotations: quaternion from a basis (rows: local -> world), slerp, back -------------------------------------
struct Q { double w, x, y, z; };
Q quat_of(const Pose& p)
{
    const float* m = p.m;   // m[3r+c] = R[r][c]
    const double tr = m[0] + m[4] + m[8];
    Q q;
    if (tr > 0) { const double s = std::sqrt(tr + 1.0) * 2; q = { 0.25 * s, (m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s }; }
    else if (m[0] > m[4] && m[0] > m[8]) { const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2; q = { (m[7] - m[5]) / s, 0.25 * s, (m[1] + m[3]) / s, (m[2] + m[6]) / s }; }
    else if (m[4] > m[8]) { const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2; q = { (m[2] - m[6]) / s, (m[1] + m[3]) / s, 0.25 * s, (m[5] + m[7]) / s }; }
    else { const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2; q = { (m[3] - m[1]) / s, (m[2] + m[6]) / s, (m[5] + m[7]) / s, 0.25 * s }; }
    return q;
}
Q slerp(Q a, Q b, double t)
{
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) { b = { -b.w, -b.x, -b.y, -b.z }; d = -d; }   // the shorter arc
    double ka, kb;
    if (d > 0.9995) { ka = 1 - t; kb = t; }
    else { const double th = std::acos(d), s = std::sin(th); ka = std::sin((1 - t) * th) / s; kb = std::sin(t * th) / s; }
    Q r = { ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z };
    const double n = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    return { r.w / n, r.x / n, r.y / n, r.z / n };
}
M4 of_quat(const Q& q, const double o[3])
{
    M4 w{};
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    w.a[0][0] = 1 - 2 * (yy + zz); w.a[0][1] = 2 * (xy - wz); w.a[0][2] = 2 * (xz + wy);
    w.a[1][0] = 2 * (xy + wz); w.a[1][1] = 1 - 2 * (xx + zz); w.a[1][2] = 2 * (yz - wx);
    w.a[2][0] = 2 * (xz - wy); w.a[2][1] = 2 * (yz + wx); w.a[2][2] = 1 - 2 * (xx + yy);
    w.a[0][3] = o[0]; w.a[1][3] = o[1]; w.a[2][3] = o[2]; w.a[3][3] = 1;
    return w;
}

// ---- the bodies after the last step ----------------------------------------------------------------------------
struct Body { uint32_t obj; Pose prev, cur; bool blend, snapped, turned; M4 wcur; };
std::vector<Body> g_bodies;
std::vector<std::pair<double, uint32_t>> g_by_x;   // the bodies' x, sorted, for finding a pose by its origin
uint32_t g_world;
float g_fixed;   // the fixed step (internalSingleStepSimulation's argument)
uint32_t g_steps;
uint32_t g_frame_steps_at;   // steps at the last frame
uint8_t g_ring[32];          // the last 32 frames' physics steps
uint32_t g_ring_i, g_ring_sum;

// counts
std::atomic<unsigned long long> g_draws{0}, g_matched{0}, g_blended{0}, g_unmatched{0}, g_anchors{0}, g_anchor_fail{0},
    g_snaps{0}, g_turned{0}, g_frames{0}, g_frames_on{0};

// a body whose current pose is `w` to within `tol` (rotation 5e-3, origin 5e-3 units): its index, or -1
int find_body(const M4& w)
{
    constexpr double kTol = 5e-3;
    auto it = std::lower_bound(g_by_x.begin(), g_by_x.end(), std::make_pair(w.a[0][3] - kTol, 0u));
    for (; it != g_by_x.end() && it->first <= w.a[0][3] + kTol; ++it) {
        const M4& b = g_bodies[it->second].wcur;
        bool ok = std::fabs(b.a[1][3] - w.a[1][3]) <= kTol && std::fabs(b.a[2][3] - w.a[2][3]) <= kTol;
        for (int i = 0; ok && i < 3; i++) for (int j = 0; ok && j < 3; j++) ok = std::fabs(b.a[i][j] - w.a[i][j]) <= kTol;
        if (ok) return (int)it->second;
    }
    return -1;
}

// the log (analysis): steps and the bodies that moved
struct LogRec { uint64_t t; uint32_t a, b; uint32_t lr; Pose p; };   // a step: a = world, b = step; a body: a = 0, b = obj
std::vector<LogRec>* g_log;
std::mutex g_log_mu;
const char* g_log_path;
std::unordered_map<uint32_t, Pose>* g_last;

void report()
{
    fprintf(stderr, "[debris-interp] %llu piece draws: %llu matched to a body (%llu drawn between steps), %llu not matched; "
            "%llu passes anchored, %llu anchors failed; %llu draws of a piece that moved over 2 units in a step (drawn at its pose), "
            "%llu of one that turned over 90 degrees (its rotation the current one); frames %llu, %llu with interpolation on\n",
            g_draws.load(), g_matched.load(), g_blended.load(), g_unmatched.load(), g_anchors.load(), g_anchor_fail.load(),
            g_snaps.load(), g_turned.load(), g_frames.load(), g_frames_on.load());
}
void log_write()
{
    static std::atomic<int> once{0};
    if (once.exchange(1)) return;
    if (g_debris_interp_render) report();
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_log_mu);
    FILE* f = fopen(g_log_path, "w");
    if (!f) return;
    fprintf(f, "# DEBRIS_INTERP_LOG: S us world lr step (after the step) / B obj basis rows (9) origin (3)\n");
    for (const LogRec& r : *g_log) {
        if (r.a) { fprintf(f, "S %.1f %08X %08X %u\n", (double)(int64_t)(r.t - g_ps3_ft.base) / 1e3, r.a, r.lr, r.b); continue; }
        fprintf(f, "B %08X", r.b);
        for (int i = 0; i < 12; i++) fprintf(f, " %.5f", r.p.m[i]);
        fprintf(f, "\n");
    }
    fclose(f);
}

void after_step(uint32_t world, uint32_t lr, float fixed)
{
    g_steps++;
    g_world = world;
    g_fixed = fixed;
    const uint32_t n = r32(world + kObjSize), data = r32(world + kObjData);
    // the poses only near or above the switch's rate (at 60 fps the steps cost nothing here): a body's first snapshot
    // after that has no previous pose and is drawn as the game draws it
    if (g_debris_interp_render && g_ring_sum < 0.95 * 32) {
        std::unordered_map<uint32_t, size_t> old;   // built only when the array's order changed
        std::vector<Body> now;
        now.reserve(n);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t obj = r32(data + 4 * i);
            Body b{ obj, {}, pose_of(obj), false, false, false, {} };
            size_t k = i;   // the same place in the array as at the last step, mostly
            if (k >= g_bodies.size() || g_bodies[k].obj != obj) {
                if (old.empty()) { old.reserve(g_bodies.size() * 2); for (size_t j = 0; j < g_bodies.size(); j++) old[g_bodies[j].obj] = j; }
                auto it = old.find(obj);
                k = it == old.end() ? SIZE_MAX : it->second;
            }
            if (k != SIZE_MAX) {
                b.prev = g_bodies[k].cur;
                const float* p = b.prev.m; const float* c = b.cur.m;
                const double dx = c[9] - p[9], dy = c[10] - p[10], dz = c[11] - p[11];
                // the rotation between the two poses: trace(Rp^T Rc) = 1 + 2 cos(angle); below 0: more than 90 degrees
                double tr = 0;
                for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) tr += p[3 * r + k] * c[3 * r + k];
                const bool moved = memcmp(&b.prev, &b.cur, sizeof(Pose)) != 0;
                // moved more than 2 units in a step: a teleport (a reused piece) -- drawn at its pose; turned more than 90
                // degrees: the rotation can't be told from the two poses (the short arc may be the wrong way) -- the
                // position still blends, the rotation is the current one (a twitch of the whole piece is what shows)
                if (moved && dx * dx + dy * dy + dz * dz > 4.0) b.snapped = true;
                else { b.blend = moved; b.turned = moved && tr < 1.0; }
            }
            b.wcur = of_pose(b.cur);
            now.push_back(b);
        }
        g_bodies.swap(now);
        g_by_x.clear();
        for (size_t i = 0; i < g_bodies.size(); i++) g_by_x.push_back({ g_bodies[i].wcur.a[0][3], (uint32_t)i });
        std::sort(g_by_x.begin(), g_by_x.end());
    } else if (!g_bodies.empty()) {   // no stale poses to match later: the switch waits for fresh ones
        g_bodies.clear();
        g_by_x.clear();
    }
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_log_mu);
    if (g_log->size() > (1u << 22)) return;
    g_log->push_back({ ps3_ft_now(), world, g_steps, lr, {} });
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t obj = r32(data + 4 * i), st = r32(obj + kActivation);
        if (st == 2 || st == 5) continue;
        const Pose p = pose_of(obj);
        auto it = g_last->find(obj);
        if (it != g_last->end() && !memcmp(&it->second, &p, sizeof p)) continue;
        (*g_last)[obj] = p;
        g_log->push_back({ 0, 0, obj, 0, p });
    }
}

// the recorder's side: the frame, the pass's view-projection
bool g_on, g_anchored;
M4 g_vp_inv;

thread_local int t_inner;

}  // namespace interp

using namespace interp;

/* The entry hook (config/r8_hooks.json): btDiscreteDynamicsWorld::internalSingleStepSimulation (0x0093DFCC, r3 the
 * world, f1 the step): the lifted step run here, then the bodies' poses read; always handled. */
extern "C" int dotheroes_interp_step(ppu_context* ctx)
{
    if (t_inner) return 0;
    const uint32_t world = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    const float fixed = (float)ctx->fpr[1];
    t_inner++;
    func_0093DFCC(ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf((void*)ctx); }
    t_inner--;
    after_step(world, lr, fixed);
    return 1;
}

/* The recorder (ppfx_recorder.cpp), on the game's thread in record order. A new frame: interpolation applies while one
 * frame rate is clearly above the physics rate (fewer than 0.85 steps a frame over the last 32 frames). */
extern "C" void dotheroes_interp_frame(uint32_t frame)
{
    (void)frame;
    g_frames++;
    const uint32_t steps = std::min<uint32_t>(g_steps - g_frame_steps_at, 8);
    g_frame_steps_at = g_steps;
    g_ring_sum += steps - g_ring[g_ring_i];
    g_ring[g_ring_i] = (uint8_t)steps;
    g_ring_i = (g_ring_i + 1) % 32;
    // on while the frame rate is clearly above the physics rate: below 0.85 steps a frame over the last 32 frames (about
    // 70 fps and up); off again from 0.95 (a 60 fps cap's occasional frame without a step, and the next with two, leave
    // it off: at 60 fps every draw stays the game's)
    const double ratio = g_ring_sum / 32.0;
    const bool rate = g_on ? ratio < 0.95 : ratio < 0.85;
    g_anchored = false;
    g_on = rate && g_world && g_fixed > 0 && !g_bodies.empty();
    if (g_on) g_frames_on++;
}
extern "C" int dotheroes_interp_on(void) { return g_on; }
extern "C" int dotheroes_interp_anchored(void) { return g_anchored; }

/* Two consecutive piece draws of a pass (their c256..c259): 1 when a body pair explains them -- the pass's
 * view-projection is then known */
extern "C" int dotheroes_interp_anchor(const float* a, const float* b)
{
    M4 ia;
    if (!inverse(of_rows(a), ia)) { g_anchor_fail++; return 0; }
    const M4 x = mul(ia, of_rows(b));   // W_a^-1 . W_b
    for (const Body& c : g_bodies) {
        const int j = find_body(mul(c.wcur, x));
        if (j < 0 || &g_bodies[(size_t)j] == &c) continue;
        // the view-projection from a's body: VP = M_a . W_a^-1
        if (!inverse(mul(of_rows(a), rigid_inverse(c.wcur)), g_vp_inv)) break;
        g_anchored = true;
        g_anchors++;
        return 1;
    }
    g_anchor_fail++;
    return 0;
}

/* A piece draw (c256..c259) with the pass's view-projection: 1 when its body is found and `out` is the matrix to draw
 * (between the body's last two poses, or its own when the body does not blend); 0: not this pass's (drawn as is; the
 * view-projection forgotten, for the next pass to anchor) */
extern "C" int dotheroes_interp_apply(const float* m, float* out)
{
    g_draws++;
    const M4 mm = of_rows(m);
    const int i = g_anchored ? find_body(mul(g_vp_inv, mm)) : -1;
    if (i < 0) { g_unmatched++; g_anchored = false; return 0; }   // not this pass's: the next anchors again
    g_matched++;
    const Body& b = g_bodies[(size_t)i];
    if (b.snapped) g_snaps++;   // a drawn piece snapped this step
    if (!b.blend) { memcpy(out, m, 16 * sizeof(float)); return 1; }
    // the fraction now, with the poses it belongs to: the game steps the world before it draws the frame's debris (a
    // frame's first records come before its physics), so read where the draw is, not at the frame's first record
    const double t = std::min(1.0, std::max(0.0, (double)rf(g_world + kLocalTime) / g_fixed)), o[3] = { b.prev.m[9] + t * (b.cur.m[9] - b.prev.m[9]), b.prev.m[10] + t * (b.cur.m[10] - b.prev.m[10]),
                                       b.prev.m[11] + t * (b.cur.m[11] - b.prev.m[11]) };
    if (b.turned) g_turned++;
    const M4 w = of_quat(b.turned ? quat_of(b.cur) : slerp(quat_of(b.prev), quat_of(b.cur), t), o);
    const M4 r = mul(mul(mm, rigid_inverse(b.wcur)), w);
    for (int k = 0; k < 4; k++) for (int j = 0; j < 4; j++) out[4 * k + j] = (float)r.a[k][j];
    g_blended++;
    return 1;
}

/* DOTHEROES_DEBRIS_INTERP and _LOG, read once dotheroes.cfg is applied (port/main.cpp), so a line in it switches
 * the interpolation on */
extern "C" void dotheroes_interp_init(void)
{
    {
        const char* r = getenv("DOTHEROES_DEBRIS_INTERP");
        g_debris_interp_render = r && *r == '1';
        const char* e = getenv("DOTHEROES_DEBRIS_INTERP_LOG");
        if (e && *e) {
            g_log_path = e;
            g_log = new std::vector<LogRec>;
            g_log->reserve(1 << 20);
            g_last = new std::unordered_map<uint32_t, Pose>;
            (void)PS3_FT_ON();
        }
        g_debris_interp = g_debris_interp_render || g_log;
        if (g_debris_interp) { atexit(log_write); g_dotheroes_interp_write = log_write; }
        if (g_debris_interp_render) fprintf(stderr, "[debris-interp] debris drawn between physics steps above 60 fps (DOTHEROES_DEBRIS_INTERP)\n");
    }
}
