// Find where the game keeps a piece of state, from guest-memory snapshots taken
// at chosen moments of a drive script (DOTHEROES_DEV_MEMSNAP=1,
// port/native/dev_state.cpp).
//
//   find_state position <s0> ... <s6>
//
// position: tests/scripts/find_position.drive's seven snapshots -- standing
// still (0, 1), after walking right 1 s (2) and 1 s more (3), back left 2 s
// (4), down 1 s (5), back up 1 s (6). Lists the floats that move like the
// hero along each axis: the same in 0 and 1; for the right-left axis, two
// steps of about the same size the same way, back near the start after
// walking left, and still while walking down and up; for the other axis the
// reverse. Then the pairs of one of each within 16 bytes (a position vector).
//
//   find_state life <h0,h1,...> <s0> <s1> ...
//
// life: snapshots taken with screenshots (tests/scripts/find_life.drive) and
// the life each screenshot shows, h0,h1,... (in half apples, say): the fields
// -- 8, 16 or 32-bit integers, or floats -- whose values in the snapshots are
// all nonzero and proportional to those, with the factor.
//
//   find_state differ <limit> <na> <nb> <a snapshots> <b snapshots>
//   find_state clocks <limit> <before> <after>
//
// (below, at each function.)
// Built with the bundled Zig:
//   zig c++ -target x86_64-windows-gnu -O2 tests/native/find_state.cpp
//     -o build/native-test/find_state.exe
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Range { uint32_t ea; std::vector<uint8_t> bytes; };

static std::vector<Range> load(const char* path)
{
    std::vector<Range> out;
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2) {
        Range r;
        r.ea = hdr[0];
        r.bytes.resize(hdr[1]);
        if (fread(r.bytes.data(), 1, hdr[1], f) != hdr[1]) break;
        out.push_back(std::move(r));
    }
    fclose(f);
    return out;
}

static float fbe(const uint8_t* p)
{
    const uint32_t u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    float v;
    memcpy(&v, &u, 4);
    return v;
}

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static uint32_t ube(const uint8_t* p, int bytes)
{
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) v = v << 8 | p[i];
    return v;
}

static int life(const char* list, int nsnap, char** paths)
{
    std::vector<double> h;
    for (const char* p = list; *p; ) { h.push_back(strtod(p, (char**)&p)); while (*p == ',') p++; }
    if ((int)h.size() != nsnap || nsnap > 64) { fprintf(stderr, "%zu values for %d snapshots\n", h.size(), nsnap); return 2; }
    size_t ref = 0;   // the snapshot with the most life, to scale from
    for (size_t i = 1; i < h.size(); i++) if (h[i] > h[ref]) ref = i;
    std::vector<std::vector<Range>> s;
    for (int i = 0; i < nsnap; i++) s.push_back(load(paths[i]));
    unsigned found = 0;
    for (size_t r = 0; r < s[0].size(); r++) {
        const size_t n = s[0][r].bytes.size();
        // the same range in every snapshot, by address (the ranges' count may differ: memory mapped since)
        std::vector<const uint8_t*> b(nsnap, nullptr);
        bool same = true;
        for (int i = 0; i < nsnap && same; i++) {
            for (const Range& x : s[i])
                if (x.ea == s[0][r].ea && x.bytes.size() == n) { b[i] = x.bytes.data(); break; }
            same = b[i] != nullptr;
        }
        if (!same) continue;
        for (int bytes : { 1, 2, 4 })
            for (size_t o = 0; o + bytes <= n; o += bytes) {
                double v[64];
                bool ok = true;
                for (int i = 0; i < nsnap && ok; i++) {
                    v[i] = (double)ube(b[i] + o, bytes);
                    ok = v[i] != 0;
                }
                for (int i = 0; i < nsnap && ok; i++) ok = v[i] * h[ref] == v[ref] * h[i];
                if (ok && found++ < 40)
                    printf("  %08X u%d  factor %g  (%g in the first)\n", s[0][r].ea + (uint32_t)o, 8 * bytes, v[ref] / h[ref], v[0]);
            }
        for (size_t o = 0; o + 4 <= n; o += 4) {
            float v[64];
            bool ok = true;
            for (int i = 0; i < nsnap && ok; i++) {
                v[i] = fbe(b[i] + o);
                ok = std::isfinite(v[i]) && std::fabs(v[i]) > 1e-6f && std::fabs(v[i]) < 1e7f;
            }
            for (int i = 0; i < nsnap && ok; i++) ok = std::fabs(v[i] * h[ref] - v[ref] * h[i]) <= 1e-4 * std::fabs(v[ref] * h[i]);
            if (ok && found++ < 40)
                printf("  %08X f32 factor %g  (%g in the first)\n", s[0][r].ea + (uint32_t)o, v[ref] / h[ref], v[0]);
        }
    }
    printf("%u fields\n", found);
    return 0;
}

// differ: two groups of snapshots taken in two situations (an area each, say):
// the 32-bit words below `limit` the same within each group and different
// between them, small (below 0x10000) in both -- an identifier of the situation.
static int differ(uint32_t limit, int na, int nb, char** paths)
{
    std::vector<std::vector<Range>> s;
    for (int i = 0; i < na + nb; i++) s.push_back(load(paths[i]));
    unsigned found = 0;
    for (const Range& r0 : s[0]) {
        if (r0.ea >= limit) continue;
        std::vector<const uint8_t*> b;
        for (auto& snap : s)
            for (const Range& x : snap)
                if (x.ea == r0.ea && x.bytes.size() == r0.bytes.size()) { b.push_back(x.bytes.data()); break; }
        if ((int)b.size() != na + nb) continue;
        for (size_t o = 0; o + 4 <= r0.bytes.size() && r0.ea + o < limit; o += 4) {
            const uint32_t va = ube(b[0] + o, 4), vb = ube(b[na] + o, 4);
            if (va == vb || va >= 0x10000 || vb >= 0x10000) continue;
            bool ok = true;
            for (int i = 1; i < na && ok; i++) ok = ube(b[i] + o, 4) == va;
            for (int i = na + 1; i < na + nb && ok; i++) ok = ube(b[i] + o, 4) == vb;
            if (ok && found++ < 60) printf("  %08X  %u -> %u\n", r0.ea + (uint32_t)o, va, vb);
        }
    }
    printf("%u words\n", found);
    return 0;
}

// clocks: two snapshots taken some seconds apart (tests/scripts/fps_check.drive):
// every 32-bit word below `limit` that changed, as an integer that moved by less
// than 2^24 ("<ea> u32 <before> <after>") or a finite float below 1e7 ("<ea> f32
// <before> <after>") -- both, when it is both. tests/fps_check.py compares two
// runs' lists at different frame rates: a clock counting frames moves in
// proportion to the frame rate, one counting time does not.
static int clocks(uint32_t limit, const char* pa, const char* pb)
{
    const std::vector<Range> a = load(pa), b = load(pb);
    unsigned n = 0;
    for (const Range& ra : a) {
        if (ra.ea >= limit) continue;
        const Range* rb = nullptr;
        for (const Range& x : b)
            if (x.ea == ra.ea && x.bytes.size() == ra.bytes.size()) { rb = &x; break; }
        if (!rb) continue;
        for (size_t o = 0; o + 4 <= ra.bytes.size() && ra.ea + o < limit; o += 4) {
            const uint8_t *p = ra.bytes.data() + o, *q = rb->bytes.data() + o;
            if (!memcmp(p, q, 4)) continue;
            const uint32_t ua = ube(p, 4), ub = ube(q, 4), ea = ra.ea + (uint32_t)o;
            if ((ub > ua ? ub - ua : ua - ub) < 0x1000000u) { printf("%08X u32 %u %u\n", ea, ua, ub); n++; }
            const float fa = fbe(p), fb = fbe(q);
            if (std::isfinite(fa) && std::isfinite(fb) && std::fabs(fa) < 1e7f && std::fabs(fb) < 1e7f &&
                (std::fabs(fa) > 1e-6f || fa == 0) && (std::fabs(fb) > 1e-6f || fb == 0)) {
                printf("%08X f32 %.9g %.9g\n", ea, fa, fb);
                n++;
            }
        }
    }
    fprintf(stderr, "%u changed\n", n);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && !strcmp(argv[1], "life")) return life(argv[2], argc - 3, argv + 3);
    if (argc == 5 && !strcmp(argv[1], "clocks")) return clocks((uint32_t)strtoul(argv[2], nullptr, 16), argv[3], argv[4]);
    if (argc >= 7 && !strcmp(argv[1], "differ"))   // differ <limit> <na> <nb> <a snaps> <b snaps>
        return differ((uint32_t)strtoul(argv[2], nullptr, 16), atoi(argv[3]), atoi(argv[4]), argv + 5);
    if (argc != 9 || strcmp(argv[1], "position")) {
        fprintf(stderr, "usage: find_state position <s0> ... <s6>\n       find_state life <h0,h1,...> <s0> <s1> ...\n");
        return 2;
    }
    std::vector<std::vector<Range>> s;
    for (int i = 2; i < 9; i++) s.push_back(load(argv[i]));
    for (size_t i = 1; i < s.size(); i++)
        if (s[i].size() != s[0].size()) { fprintf(stderr, "snapshots cover different memory\n"); return 2; }
    struct Hit { uint32_t ea; float v[7]; };
    std::vector<Hit> xs, zs;
    for (size_t r = 0; r < s[0].size(); r++) {
        const size_t n = s[0][r].bytes.size();
        for (size_t i = 1; i < s.size(); i++)
            if (s[i][r].ea != s[0][r].ea || s[i][r].bytes.size() != n) { fprintf(stderr, "range %zu differs\n", r); return 2; }
        for (size_t o = 0; o + 4 <= n; o += 4) {
            Hit h;
            h.ea = s[0][r].ea + (uint32_t)o;
            bool ok = true;
            for (int k = 0; k < 7 && ok; k++) {
                h.v[k] = fbe(&s[k][r].bytes[o]);
                ok = std::isfinite(h.v[k]) && std::fabs(h.v[k]) < 1e6f;
            }
            if (!ok || memcmp(&s[0][r].bytes[o], &s[1][r].bytes[o], 4)) continue;
            const float* v = h.v;
            const float d1 = v[2] - v[1], d2 = v[3] - v[2], walk = v[3] - v[1];
            if (std::fabs(d1) > 0.05f && std::fabs(d2) > 0.05f && (d1 > 0) == (d2 > 0) &&
                std::fabs(d1) < 2 * std::fabs(d2) && std::fabs(d2) < 2 * std::fabs(d1) &&
                std::fabs(v[4] - v[1]) < 0.25f * std::fabs(walk) &&
                near(v[5], v[4], 0.1f * std::fabs(d1)) && near(v[6], v[5], 0.1f * std::fabs(d1)))
                xs.push_back(h);
            const float dz = v[5] - v[4];
            if (std::fabs(dz) > 0.05f && std::fabs(v[6] - v[4]) < 0.25f * std::fabs(dz) &&
                near(v[2], v[1], 0.1f * std::fabs(dz)) && near(v[3], v[2], 0.1f * std::fabs(dz)) &&
                near(v[4], v[3], 0.1f * std::fabs(dz)))
                zs.push_back(h);
        }
    }
    auto show = [](const char* what, const std::vector<Hit>& v) {
        printf("%s: %zu candidates\n", what, v.size());
        for (size_t i = 0; i < v.size() && i < 40; i++)
            printf("  %08X  %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f\n", v[i].ea, v[i].v[0], v[i].v[1], v[i].v[2],
                   v[i].v[3], v[i].v[4], v[i].v[5], v[i].v[6]);
    };
    show("right-left axis (moves with RIGHT and LEFT only)", xs);
    show("other axis (moves with DOWN and UP only)", zs);
    printf("pairs within 16 bytes (right-left, other):\n");
    for (const Hit& x : xs)
        for (const Hit& z : zs)
            if (z.ea > x.ea ? z.ea - x.ea <= 16 : x.ea - z.ea <= 16)
                printf("  %08X %08X  (%+.3f, %+.3f) after 2 s right / 1 s down\n", x.ea, z.ea, x.v[3] - x.v[1], z.v[5] - z.v[4]);
    return 0;
}
