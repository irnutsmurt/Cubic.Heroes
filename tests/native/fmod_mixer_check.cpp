// The native FMOD mixer (fmod_mixer.cpp, generated) against the lifted mixer
// (build/gen/spu/spu_0005_at_00F86A80/spu_recomp.c), offline.
//
// 1. The game's own inputs: a channel trace of the mixer's thread (ps3recomp spu_channels.c, SPU_TRACE_IMAGE=6;
//    docs/AUDIO.md "A0, the captures") replayed into each of them from the thread's entry.
//    The trace holds the thread's 128 registers and 256 KB local store as it started (pc 0xF0), then every
//    channel read with the value it returned, every channel write, every MFC command with the local store it
//    filled (a GET, GETLLAR) or sent (a PUT, PUTLLC), and every stop. A replay starts from that state and feeds
//    the program, in order, every rdch value and every GET's and GETLLAR's bytes from the trace, and checks, in
//    order, every wrch (channel and value), every MFC command (command, LSA, EAL, size), the bytes every PUT and
//    PUTLLC sends, and every stop (its code; the pc the runtime reports is counted apart). The overlay plugins
//    the mixer loads and calls (codec_raw, image 9; sfxreverb, image 25) ran natively in the game build that
//    made the traces, so their channel operations are in the trace among the mixer's: a branch out of the
//    mixer's image is identified by the plugin's code in the local store at the target (the first 4 KB of its
//    text, at target - entry link + 0x80) and run by the plugin's native port (port/native/fmod_codec_raw,
//    fmod_sfxreverb, bit-exact with their lifts) on the same model. rchcnt is not in the trace; the replay
//    answers it as the runtime does (spu_rchcnt): 1 for the MFC channels, the inbound mailbox's count (0: the
//    program reads every word it is given) for RdInMbox.
//    One pass replays the lifted mixer (a stand-in for the runtime's drain loop, indirect-branch dispatch and
//    stop service, as the A1/A2 checkers have it): that proves the trace and the harness. A second replays the
//    port. Both are compared with the trace as they go, and with each other at every block boundary (each stop
//    0x110: all 128 registers, all four words, and the local store in 16 KB chunks, hashed) and where they end
//    (every local-store byte). A replay stops at the first difference from the trace, reporting it.
//
// 2. Synthesized inputs (--fuzz N): blocks and functions the traces do not reach. During the lifted pass the
//    checker keeps the main memory the trace shows (every GET's, GETLLAR's, PUT's and PUTLLC's bytes, at their
//    EAs, the latest kept) and, at sampled block boundaries, the local store and registers and the GETs of the
//    block that follows. A case starts from a sample and either calls the block (0xDA30, with main's arguments
//    as the main loop passes them) with a few words of what its GETs read mutated (random, 0, small, a bit
//    flipped, another word), or calls any function the port has with arguments drawn from the sample. The
//    lifted mixer runs it against that memory (GETs read it, PUTs write it, tags complete at once, GETLLAR then
//    PUTLLC succeed, the decrementer counts down, the inbound mailbox is empty, the plugins run natively on the
//    same model), and its channel transcript is recorded in the trace's format. Then the port runs from the same
//    state and replays that transcript exactly as it replays a game trace; the two outcomes (returned, halted at
//    a pc, stopped with a code, a branch to code the replay does not know, a return elsewhere than the link),
//    every local-store byte and all 128 registers are compared. A case the lifted run cannot finish within its
//    budget (local-store accesses, counted through the runtime's LS watch hook; 3,000 nested calls; a 256 MB
//    transcript) is not counted, nor one whose lifted run (on a thread of its own) makes no progress at all for
//    10 s (abandoned), nor one where the lifted run went on through a case's pointer into a block of the image
//    that is no function of the port (the port hands such a branch to its host: Branch::unresolved_*). The
//    block cases with no mutation re-run a real block on exactly the bytes its GETs read in the trace.
//
//   fmod_mixer_check <trace.bin> [--lift-only | --port-only] [--blocks N] [--preload] [--show N]
//                    [--fuzz N [--seed S] [--case K]] [--image-dir DIR]
//     --blocks N: the first N blocks only (the port ends at the N+1-th receive through its stop callback)
//     --preload: the whole trace read into memory first (1.5 GB): the passes time the programs, not the disk
//     --show N: the last N records consumed before a difference (default 24)
//     --fuzz N: N synthesized cases after the two passes (needs the lifted pass); --case K: only case K, with
//       its details; MIXER_CASE_PROGRESS=1: each case's number before it runs; MIXER_TRACE_BRANCHES=1: the
//       lifted run's indirect branches
//   The trace ends at its size limit inside a block: the blocks before are the complete ones.
//   MIXER_SHOW_UNTAKEN=1: the port's blocks never taken; MIXER_COVERAGE_FILE=<path>: every block's count.
//   --profile N [--blocks B] [--sample PREFIX]: no comparison; N passes each of the lifted mixer and the port,
//     alternating, over the trace preloaded, without the block-boundary snapshots, timing the mixer's code alone
//     (each pass's time less the time in the model -- every channel operation, DMA copy and compare, stop -- and
//     in the plugins, counted in TSC ticks at their outermost entry); --sample: the replay thread's instruction
//     pointer every 250 us in the mixer's code, for tools/fmod_mixer_profile.py. Build it without the block
//     counters, as the game builds the port: COVERAGE=0 tools/fmod_mixer_check.sh (fmod_mixer_prof_*.exe).
//
// Built with the bundled Zig, from the repository root (OUT = a scratch folder, here F:/3d dot game heroes/scratch/mixer/out;
// tools/fmod_mixer_check.sh does both steps and regenerates the port):
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe cc -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -g -c
//       -I build/ref/ps3recomp/runtime/spu -I build/ref/ps3recomp/include
//       build/gen/spu/spu_0005_at_00F86A80/spu_recomp.c -o OUT/mixer_lift_v3.o
//   build/toolchain/zig-x86_64-windows-0.17.0/zig.exe c++ -target x86_64-windows-gnu -mcpu=x86_64_v3 -O2 -std=c++17
//       -DFMOD_MIXER_COVERAGE -I port/native -I build/ref/ps3recomp/runtime/spu
//       -I build/ref/ps3recomp/include tests/native/fmod_mixer_check.cpp port/native/fmod_mixer.cpp
//       port/native/fmod_codec_raw.cpp port/native/fmod_sfxreverb.cpp OUT/mixer_lift_v3.o
//       -Wl,--stack,67108864 -o OUT/fmod_mixer_check_v3.exe
// (x86_64_v2 and _v2 names for the shipped level.) tools/fmod_mixer_batch.sh runs the set below.
//
// 2026-11-11, the generated port (97 functions, 903 blocks), at x86_64_v3 and at x86_64_v2 alike:
//   the game's traces (rel24, each from the thread's entry to its 1,500 MB limit, about 28 s of audio after the
//     mixer starts): the river 5,189 blocks (6,102,400 records), the dungeon 5,375 (6,163,801): every record
//     equal for the lifted mixer and for the port, the two equal at every block boundary (5,189 and 5,375 of
//     5,189 and 5,375); 31,088 and 30,978 codec_raw calls, no sfxreverb call, the DSP plugin call at 0x6118
//     never reached. The port's blocks taken by the traces: 347 of 903 (48 of 97 functions).
//   synthesized: 20,000 cases a trace and level (seeds 7, 11, 3, 5): 19,452 / 19,484 / 19,455 / 19,467
//     compared (v3 river, v3 dungeon, v2 river, v2 dungeon), all equal -- 77,858 cases, ended returned 65,453,
//     halted 7,689, stopped 1,493, branched to code no plugin has 3,007, returned elsewhere 216. Not counted:
//     1,695 over the lifted run's budget, 2 where it looped on registers alone, 435 it could not finish (an
//     MFC command that is no command, from drawn arguments or mutated descriptors; codec_raw's port halting on a
//     mutated state), 10 where the port handed a branch into the image to its host. The 8,836 block cases with
//     no mutation all returned, none reaching unknown code. Blocks taken, all runs together: 753 of 903 (all 97
//     functions entered, 70 of them whole).
//   Two differences the synthesized cases found were in this checker, not in the port: the stand-in dispatch
//   rounded an odd target down to a word (the runtime's registry matches exactly), and the DMA clamp wrapped
//   for a size near 4 GB (`lsa + n > 0x40000`; the trace writer in spu_channels.c has the same expression).
//
// 2026-11-11, the port with the generator's register effects (stores and reloads around calls by what the
// callee can reach and change, a dataflow of what may be dirty within each function; tools/fmod_mixer_batch.sh):
//   the four traces, at x86_64_v3 and x86_64_v2: the river 5,189 blocks, the dungeon 5,375, the long dungeon run
//     24,070 (15,308 sfxreverb calls through 0x6118), the native mixer's own run in the game 24,445: every record
//     equal for both, every block boundary equal.
//   synthesized: 20,000 cases a trace and level (seed 7 at v3, 3 at v2): 19,452 / 19,479 / 19,455 / 19,469
//     compared (v3 river, v3 dungeon, v2 river, v2 dungeon), all equal -- 77,855, ended returned 65,535, halted
//     7,645, stopped 1,440, branched to code no plugin has 3,014, returned elsewhere 221; not counted 1,697 over
//     the budget, 2 looping on registers alone, 440 the lifted run could not finish, 6 handed to the host. The
//     8,836 block cases with no mutation all returned. Blocks taken, all runs: 750 of 903 (97 functions).
//     One more difference found here and fixed in the checker: a branch to 0x84 (.SpuGUID, which the lift took
//     for code) was counted a difference -- the lifted run went on there, the port handed it to its host -- and
//     is now counted apart with the others like it (the image's span for Branch starts at 0x80 now).
//   --profile (the mixer's code a block, best of the passes): river v3 the lifted mixer 50.2 us, the port 42.3
//     (-15.9%; the port without the effects 69.2 against 49.9); river v2 70.6 and 59.9 (-15.2%); the long dungeon
//     v3 53.0 and 44.3 (-16.6%).
#define __USE_MINGW_SETJMP_NON_SEH   // setjmp(b) = _setjmp(b, NULL): longjmp restores registers, no SEH unwind
#include <setjmp.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>   // the types only (SymFromAddr is looked up at run time)
#include <intrin.h>    // __rdtsc (--profile)

#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fmod_codec_raw.h"
#include "fmod_mixer.h"
#include "fmod_sfxreverb.h"
#include "spu_context.h"

namespace mx = fmod_mixer;

constexpr uint32_t kLs = 0x40000, kMask = kLs - 1;

static uint64_t fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    return h;
}

// ---- a trace: a file, or records in memory (a preloaded file, a synthesized transcript) ---------------------
struct Rec {
    uint32_t kind, a, b, c, d, n;
    const uint8_t* data;
    uint64_t index;
};
enum : uint32_t { kRdch = 1, kWrch = 2, kDma = 3, kStop = 4, kEnd = 5, kUnknownBranch = 6 };   // 6: synthesized only

struct Trace {
    FILE* f = nullptr;
    uint32_t image = 0, entry = 0;
    u128 gpr[128];
    std::vector<uint8_t> ls;
    std::vector<uint8_t> payload;
    std::vector<char> buf;
    uint64_t index = 0;
    bool ended = false;     // an end record or the end of the data was met
    uint32_t end_kind = 0;  // the end record's a: 1 the size limit, 2 the thread ended; 0 none (cut off)
    const uint8_t* mbuf = nullptr;   // records in memory
    size_t msize = 0, at = 0;

    static std::vector<uint8_t>& preloaded() { static std::vector<uint8_t> v; return v; }

    bool open(const char* p, bool preload)
    {
        f = fopen(p, "rb");
        if (!f) return false;
        buf.resize(64u << 20);
        setvbuf(f, buf.data(), _IOFBF, buf.size());
        char magic[8];
        uint32_t h[4];
        if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SPUTRC01", 8) || fread(h, 4, 4, f) != 4) return false;
        image = h[0];
        entry = h[1];
        ls.resize(kLs);
        if (fread(gpr, 16, 128, f) != 128 || fread(ls.data(), 1, kLs, f) != kLs) return false;
        if (preload) {
            std::vector<uint8_t>& mem = preloaded();
            if (mem.empty()) {
                _fseeki64(f, 0, SEEK_END);
                const long long sz = _ftelli64(f);
                _fseeki64(f, 0, SEEK_SET);
                mem.resize((size_t)sz);
                if (fread(mem.data(), 1, mem.size(), f) != mem.size()) return false;
            }
            mbuf = mem.data();
            msize = mem.size();
            at = 24 + 128 * 16 + kLs;
        }
        return true;
    }
    void open_memory(const std::vector<uint8_t>& v)
    {
        mbuf = v.data();
        msize = v.size();
        at = 0;
        index = 0;
        ended = false;
    }
    void close() { if (f) fclose(f); f = nullptr; }
    // The next record; false at the end (the end record is not returned).
    bool next(Rec& r)
    {
        if (ended) return false;
        uint32_t h[6];
        if (mbuf) {
            if (at + 24 > msize) { ended = true; return false; }
            memcpy(h, mbuf + at, 24);
            at += 24;
            r = { h[0], h[1], h[2], h[3], h[4], h[5], nullptr, index++ };
            if (r.kind == kEnd) { ended = true; end_kind = r.a; return false; }
            if (at + r.n > msize) { ended = true; return false; }
            r.data = mbuf + at;
            at += r.n;
            return true;
        }
        if (fread(h, 4, 6, f) != 6) { ended = true; return false; }
        r = { h[0], h[1], h[2], h[3], h[4], h[5], nullptr, index++ };
        if (r.kind == kEnd) { ended = true; end_kind = r.a; return false; }
        if (r.n) {
            payload.resize(r.n);
            if (fread(payload.data(), 1, r.n, f) != r.n) { ended = true; return false; }
            r.data = payload.data();
        }
        return true;
    }
};

// ---- the plugins: their native ports, found by their code in the local store ---------------------------------
struct Plugin {
    const char* name;
    int image;
    std::vector<uint8_t> text;          // the first 4 KB of the text, at link 0x80
    std::vector<uint32_t> links;        // the entries the game's hook runs natively
    unsigned long long calls = 0;
};
static Plugin g_plugins[2] = {
    { "codec_raw", 9, {}, { fmod_codec_raw::kEntryRead, fmod_codec_raw::kEntrySetPosition, fmod_codec_raw::kEntryDescriptor } },
    { "sfxreverb", 25, {}, { fmod_sfxreverb::kEntryRead, fmod_sfxreverb::kEntryDescriptor } },
};

static bool load_plugin(Plugin& p, const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    if (d.size() < 0x1100 || memcmp(d.data(), "\x7F" "ELF", 4)) return false;
    p.text.assign(d.begin() + 0x100, d.begin() + 0x1100);   // [0x80, 0x1080): the first PT_LOAD is at file offset 0x100
    return true;
}

// ---- main memory as the trace shows it (synthesized cases) --------------------------------------------------------
using Line = std::array<uint8_t, 128>;
struct Mem {
    std::unordered_map<uint32_t, Line> base;       // the trace's DMA, the latest bytes of each line
    std::unordered_map<uint32_t, Line>* over = nullptr;   // a case's writes and mutations
    size_t known_bytes = 0;

    static void write(std::unordered_map<uint32_t, Line>& m, uint32_t ea, const uint8_t* p, uint32_t n, const Mem* from)
    {
        for (uint32_t i = 0; i < n;) {
            const uint32_t a = ea + i, line = a & ~127u, off = a & 127u, k = std::min(n - i, 128u - off);
            auto it = m.find(line);
            if (it == m.end()) {
                Line l{};
                if (from) from->read_line(line, l);
                it = m.emplace(line, l).first;
            }
            memcpy(it->second.data() + off, p + i, k);
            i += k;
        }
    }
    void read_line(uint32_t line, Line& l) const
    {
        if (over) {
            auto it = over->find(line);
            if (it != over->end()) { l = it->second; return; }
        }
        auto it = base.find(line);
        if (it != base.end()) l = it->second;
        else l.fill(0);   // memory the trace never showed: zeros
    }
    void read(uint32_t ea, uint8_t* p, uint32_t n) const
    {
        Line l;
        for (uint32_t i = 0; i < n;) {
            const uint32_t a = ea + i, off = a & 127u, k = std::min(n - i, 128u - off);
            read_line(a & ~127u, l);
            memcpy(p + i, l.data() + off, k);
            i += k;
        }
    }
};

struct Sample {
    uint64_t block = 0;
    std::vector<uint8_t> ls;
    u128 gpr[128];
    std::vector<std::pair<uint32_t, uint32_t>> gets;   // the GETs of the block that follows (EA, size)
    std::unordered_map<uint32_t, Line> mem;            // and their bytes as that block read them (and GETLLAR's)
};

// ---- the model: the trace fed and checked (replay), or the inputs synthesized and recorded (generate) ------------
struct Snap {
    u128 gpr[128];
    uint64_t chunk[16];
};

struct Model {
    enum Mode { replay, generate } mode = replay;
    Trace* t = nullptr;
    uint8_t* ls = nullptr;          // the live local store
    u128* gpr = nullptr;            // the live register file
    jmp_buf* jb = nullptr;
    bool is_port = false;
    bool fuzz = false;              // a synthesized case: any stop ends it
    bool profile = false;           // a timing pass: no block-boundary snapshots
    // MFC staging and the generated answers
    uint32_t lsa = 0, eah = 0, eal = 0, size = 0, tag = 0, tag_mask = 0, atomic_stat = 0, dec = 0x7FFFFFFF;
    std::mt19937* rng = nullptr;
    Mem* mem = nullptr;                 // generate: the memory; replay of the game's trace: the memory built
    std::vector<uint8_t>* out = nullptr;   // generate: the transcript
    // progress
    uint64_t blocks = 0;            // receives (stop 0x110) met
    uint64_t block_limit = ~0ull;   // end the run at this receive
    bool failed = false, at_end = false, ended_normally = false;
    std::string why;
    uint64_t fail_record = 0;
    uint32_t unknown_target = 0;    // a branch to code no plugin has (synthesized cases)
    uint32_t unknown_site = 0;      // its site (the port's)
    bool unknown_branch = false;
    uint32_t stop_code = 0;
    bool stopped = false;
    bool unresolved = false;        // the port handed an internal branch to its host (synthesized cases)
    uint32_t unresolved_site = 0;
    // counts
    uint64_t rdch = 0, wrch = 0, gets = 0, puts = 0, getllar = 0, putllc = 0, dma_bytes = 0, stops = 0, rchcnt = 0;
    uint64_t pc_mismatch = 0, overlay_calls = 0;
    uint32_t first_pc_mismatch_trace = 0, first_pc_mismatch_ours = 0;
    // the last records consumed, for a report
    struct Seen { Rec r; const char* fmt; uint32_t x, y; };   // formatted only for a report
    std::vector<Seen> ring = std::vector<Seen>(64);
    unsigned ring_n = 0;
    // block-boundary snapshots: the lift's recorded, the port's compared
    std::vector<Snap>* snaps = nullptr;
    uint64_t snap_equal = 0, snap_diff = 0;
    std::string first_snap_diff;
    // the lifted pass of a game trace, for synthesized cases: samples
    std::vector<Sample>* samples = nullptr;
    Sample* cur_sample = nullptr;

    void seen(const Rec& r, const char* fmt, uint32_t x, uint32_t y)
    {
        Seen& s = ring[ring_n++ % ring.size()];
        s.r = r;
        s.fmt = fmt; s.x = x; s.y = y;
    }
    [[noreturn]] void fail(const std::string& w, const Rec* r)
    {
        failed = true;
        why = w;
        fail_record = r ? r->index : (t ? t->index : 0);
        longjmp(*jb, 1);
    }
    [[noreturn]] void trace_end()
    {
        at_end = true;
        longjmp(*jb, 1);
    }
    void need(Rec& r)
    {
        if (!t->next(r)) trace_end();
    }
    bool over_cap = false;   // a synthesized case's transcript passed its cap (part of the lifted run's budget)
    void put(uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const uint8_t* p, uint32_t n)
    {
        if (kind != kEnd && out->size() > (256u << 20)) { over_cap = true; longjmp(*jb, 1); }
        const uint32_t h[6] = { kind, a, b, c, d, n };
        out->insert(out->end(), (const uint8_t*)h, (const uint8_t*)h + 24);
        if (n) out->insert(out->end(), p, p + n);
    }
};

static std::atomic<unsigned long long> g_progress{ 0 };   // the watchdog's

// --profile: the time spent outside the mixer's own code -- in the model (every channel operation, the DMA
// copies and compares, the stops) and in the plugins it calls -- counted in TSC ticks at the outermost entry,
// so that a pass's time less this is the mixer's code alone. g_cb_depth also tells the sampler (--sample)
// whether a sample fell in the mixer's code.
static uint64_t g_cb_ticks = 0, g_cb_t0 = 0;
static volatile int g_cb_depth = 0;
static inline void cb_in() { if (g_cb_depth++ == 0) g_cb_t0 = __rdtsc(); }
static inline void cb_out() { if (--g_cb_depth == 0) g_cb_ticks += __rdtsc() - g_cb_t0; }

static const char* chan_name(uint32_t c)
{
    switch (c) {
    case 7: return "WrDec"; case 8: return "RdDec"; case 16: return "MFC_LSA"; case 17: return "MFC_EAH"; case 18: return "MFC_EAL";
    case 19: return "MFC_Size"; case 20: return "MFC_TagID"; case 21: return "MFC_Cmd"; case 22: return "MFC_WrTagMask";
    case 23: return "MFC_WrTagUpdate"; case 24: return "MFC_RdTagStat"; case 27: return "MFC_RdAtomicStat";
    case 28: return "WrOutMbox"; case 29: return "RdInMbox"; case 30: return "WrOutIntrMbox";
    }
    return "?";
}

static uint32_t model_rdch_(Model& m, uint32_t ch)
{
    g_progress++;
    if (m.mode == Model::generate) {
        m.rdch++;
        uint32_t v = 0;
        switch (ch) {
        case MFC_RdTagStat: v = m.tag_mask; break;             // every transfer done when issued
        case MFC_RdAtomicStat: v = m.atomic_stat; break;       // GETLLAR 4 (as the traces show), PUTLLC 0: it held
        case SPU_RdDec: m.dec -= 0x40 + (*m.rng)() % 0x400; v = m.dec; break;
        case SPU_RdInMbox: v = 0; break;
        default: v = 0; break;
        }
        m.put(kRdch, ch, v, 0, 0, nullptr, 0);
        return v;
    }
    Rec r;
    m.need(r);
    if (r.kind != kRdch || r.a != ch) {
        char w[160];
        snprintf(w, sizeof w, "rdch %s (%u): the trace has kind %u a %X b %X", chan_name(ch), ch, r.kind, r.a, r.b);
        m.fail(w, &r);
    }
    m.rdch++;
    m.seen(r, "rdch %X -> %X", ch, r.b);
    return r.b;
}

static uint32_t model_rdch(Model& m, uint32_t ch)
{
    cb_in();
    const uint32_t v = model_rdch_(m, ch);
    cb_out();
    return v;
}

static uint32_t model_rchcnt(Model& m, uint32_t ch)
{
    cb_in();
    m.rchcnt++;
    cb_out();
    return ch == SPU_RdInMbox ? 0u : 1u;   // spu_rchcnt: the MFC channels 1 (synchronous), RdInMbox its count
}

static void model_wrch_(Model& m, uint32_t ch, uint32_t v)
{
    g_progress++;
    const bool gen = m.mode == Model::generate;
    Rec r{};
    if (gen) {
        m.put(kWrch, ch, v, 0, 0, nullptr, 0);
    } else {
        m.need(r);
        if (r.kind != kWrch || r.a != ch || r.b != v) {
            char w[200];
            snprintf(w, sizeof w, "wrch %s (%u) = %X: the trace has kind %u a %X b %X", chan_name(ch), ch, v, r.kind, r.a, r.b);
            m.fail(w, &r);
        }
        m.seen(r, "wrch %X = %X", ch, v);
    }
    m.wrch++;
    switch (ch) {
    case MFC_LSA: m.lsa = v; return;
    case MFC_EAH: m.eah = v; return;
    case MFC_EAL: m.eal = v; return;
    case MFC_Size: m.size = v; return;
    case MFC_TagID: m.tag = v; return;
    case MFC_WrTagMask: m.tag_mask = v; return;
    case MFC_Cmd: break;
    default: return;
    }
    const uint32_t lsa = m.lsa & kMask;
    const uint32_t op = v & 0xFF;
    const bool atomic = op == 0xD0 || op == 0xB4 || op == 0xB0 || op == 0xB8;
    const bool get = op == 0x40 || op == 0x41 || op == 0x42 || op == 0xD0;
    const bool putc = op == 0x20 || op == 0x21 || op == 0x22 || atomic;
    uint32_t n = atomic ? 128 : m.size;
    if (n > kLs - lsa) n = kLs - lsa;   // (the trace writer's `lsa + n > 0x40000` wraps for a size near 4 GB)
    if (gen) {
        if (!get && !putc) {
            char w[96];
            snprintf(w, sizeof w, "an MFC command the generator does not model: %X", v);
            m.fail(w, nullptr);
        }
        const uint32_t ea = atomic ? (m.eal & ~127u) : m.eal;
        if (get) m.mem->read(ea, m.ls + lsa, n);
        else Mem::write(*m.mem->over, ea, m.ls + lsa, n, m.mem);
        if (op == 0xD0) m.atomic_stat = 4;
        if (op == 0xB4) m.atomic_stat = 0;
        m.put(kDma, v, lsa, m.eal, m.size, m.ls + lsa, n);
        m.dma_bytes += n;
        (op == 0xD0 ? m.getllar : op == 0xB4 ? m.putllc : get ? m.gets : m.puts)++;
        return;
    }
    Rec d;
    m.need(d);
    if (d.kind != kDma || d.a != v || d.b != lsa || d.c != m.eal || d.d != m.size) {
        char w[240];
        snprintf(w, sizeof w, "MFC command %X lsa %05X ea %08X size %X: the trace has kind %u cmd %X lsa %X ea %X size %X", v, lsa, m.eal,
                 m.size, d.kind, d.a, d.b, d.c, d.d);
        m.fail(w, &d);
    }
    if (d.n != n) {
        char w[160];
        snprintf(w, sizeof w, "MFC command %X: %u bytes, the trace has %u", v, n, d.n);
        m.fail(w, &d);
    }
    m.dma_bytes += n;
    if (get) {
        memcpy(m.ls + lsa, d.data, n);   // what the GET brought
        (op == 0xD0 ? m.getllar : m.gets)++;
        m.seen(d, op == 0xD0 ? "GETLLAR lsa %05X ea %08X" : "GET lsa %05X ea %08X", lsa, m.eal);
        if (m.cur_sample && op != 0xD0) m.cur_sample->gets.push_back({ m.eal, n });
        if (m.cur_sample && m.mem) Mem::write(m.cur_sample->mem, op == 0xD0 ? (m.eal & ~127u) : m.eal, d.data, n, m.mem);
    } else if (putc) {
        if (memcmp(m.ls + lsa, d.data, n)) {
            uint32_t i = 0;
            while (m.ls[lsa + i] == d.data[i]) i++;
            char w[200];
            snprintf(w, sizeof w, "%s lsa %05X ea %08X size %X: byte %X differs (ours %02X, the trace's %02X)", op == 0xB4 ? "PUTLLC" : "PUT",
                     lsa, m.eal, m.size, i, m.ls[lsa + i], d.data[i]);
            m.fail(w, &d);
        }
        (op == 0xB4 ? m.putllc : m.puts)++;
        m.seen(d, op == 0xB4 ? "PUTLLC lsa %05X ea %08X" : "PUT lsa %05X ea %08X", lsa, m.eal);
    } else {
        char w[96];
        snprintf(w, sizeof w, "an MFC command the mixer is not known to use: %X", v);
        m.fail(w, &d);
    }
    if (m.mem) Mem::write(m.mem->base, atomic ? (m.eal & ~127u) : m.eal, d.data, n, nullptr);   // memory as the trace shows it
}
static void model_wrch(Model& m, uint32_t ch, uint32_t v)
{
    cb_in();
    model_wrch_(m, ch, v);
    cb_out();
}

// A stop: 1 = go on (a receive the trace serviced), 0 = end the run.
static int model_stop_(Model& m, uint32_t code, uint32_t runtime_pc)
{
    g_progress++;
    if (m.mode == Model::generate) {
        m.put(kStop, code, runtime_pc, 0, 0, nullptr, 0);
        m.stops++;
        m.stopped = true;
        m.stop_code = code;
        return 0;   // a synthesized case ends at any stop
    }
    if (m.blocks >= m.block_limit && code == 0x110) {   // --blocks: the run ends here, before the receive
        m.ended_normally = true;
        return 0;
    }
    Rec r;
    m.need(r);
    if (r.kind != kStop || r.a != code) {
        char w[160];
        snprintf(w, sizeof w, "stop %X: the trace has kind %u a %X b %X", code, r.kind, r.a, r.b);
        m.fail(w, &r);
    }
    m.stops++;
    m.seen(r, "stop %X pc %05X", code, r.b);
    if (r.b != runtime_pc) {
        if (!m.pc_mismatch++) { m.first_pc_mismatch_trace = r.b; m.first_pc_mismatch_ours = runtime_pc; }
    }
    if (m.fuzz) {
        m.stopped = true;
        m.stop_code = code;
        return 0;
    }
    if (code != 0x110) return 0;
    if (m.profile) {   // timing passes: no snapshots
        m.blocks++;
        return 1;
    }
    // a block boundary: the state against the other run's
    Snap s;
    memcpy(s.gpr, m.gpr, sizeof s.gpr);
    for (int k = 0; k < 16; k++) s.chunk[k] = fnv(m.ls + k * 0x4000, 0x4000);
    if (!m.is_port) {
        m.snaps->push_back(s);
    } else if (m.blocks < m.snaps->size()) {
        const Snap& o = (*m.snaps)[m.blocks];
        if (memcmp(o.gpr, s.gpr, sizeof s.gpr) || memcmp(o.chunk, s.chunk, sizeof s.chunk)) {
            if (!m.snap_diff++) {
                char w[200];
                int reg = -1, chunk = -1;
                for (int i = 0; i < 128 && reg < 0; i++) if (memcmp(&o.gpr[i], &s.gpr[i], 16)) reg = i;
                for (int k = 0; k < 16 && chunk < 0; k++) if (o.chunk[k] != s.chunk[k]) chunk = k;
                snprintf(w, sizeof w, "block %llu: first register differing r%d, first local-store chunk differing %05X",
                         (unsigned long long)m.blocks, reg, chunk < 0 ? 0 : chunk * 0x4000);
                m.first_snap_diff = w;
            }
        } else {
            m.snap_equal++;
        }
    }
    m.blocks++;
    // samples for synthesized cases: blocks 1, 2, 3, 5, 10, 20, 50, then every 100
    if (m.samples) {
        const uint64_t k = m.blocks;
        m.cur_sample = nullptr;
        if (k == 1 || k == 2 || k == 3 || k == 5 || k == 10 || k == 20 || k == 50 || k % 100 == 0) {
            m.samples->emplace_back();
            Sample& sm = m.samples->back();
            sm.block = k;
            sm.ls.assign(m.ls, m.ls + kLs);
            memcpy(sm.gpr, m.gpr, sizeof sm.gpr);
            m.cur_sample = &sm;
        }
    }
    return 1;
}
static int model_stop(Model& m, uint32_t code, uint32_t runtime_pc)
{
    cb_in();
    const int r = model_stop_(m, code, runtime_pc);
    cb_out();
    return r;
}

static void model_unsupported(Model& m, uint32_t pc)
{
    char w[96];
    snprintf(w, sizeof w, "an instruction the lift could not translate ran at %05X", pc);
    m.fail(w, nullptr);
}

// The plugins' channels
static void plug_wrch(void* u, uint32_t ch, uint32_t v) { model_wrch(*(Model*)u, ch, v); }
static uint32_t plug_rdch(void* u, uint32_t ch) { return model_rdch(*(Model*)u, ch); }

// A branch out of the mixer's image: a plugin's entry, run by its native port. 1 with *ret = the pc it returned
// to; 0 (synthesized cases) at code no plugin has: the run ends there.
static int run_overlay_(Model& m, uint8_t* ls, u128* gpr, uint32_t target, uint32_t* ret)
{
    for (Plugin& p : g_plugins) {
        for (uint32_t link : p.links) {
            const uint32_t delta = (target - link) & kMask;
            const uint32_t base = (0x80 + delta) & kMask;
            if (base + p.text.size() > kLs || memcmp(ls + base, p.text.data(), p.text.size())) continue;
            const fmod_codec_raw::Channels cch{ &m, plug_wrch, plug_rdch };
            const fmod_sfxreverb::Channels rch{ &m, plug_wrch, plug_rdch };
            uint32_t hpc = 0;
            bool handled = false, halted = false;
            if (p.image == 9) {
                const auto r = fmod_codec_raw::call(ls, gpr, target, delta, cch, &hpc);
                handled = r == fmod_codec_raw::Result::handled;
                halted = r == fmod_codec_raw::Result::halted;
            } else {
                const auto r = fmod_sfxreverb::call(ls, gpr, target, delta, rch, &hpc);
                handled = r == fmod_sfxreverb::Result::handled;
                halted = r == fmod_sfxreverb::Result::halted;
            }
            if (!handled) {
                char w[160];
                snprintf(w, sizeof w, "%s at %05X (link %X): the native port %s", p.name, target, link, halted ? "halted" : "declined");
                m.fail(w, nullptr);
            }
            p.calls++;
            m.overlay_calls++;
            *ret = gpr[0]._u32[0] & kMask;
            return 1;
        }
    }
    if (m.fuzz) {
        if (m.mode == Model::generate) {
            m.put(kUnknownBranch, target, 0, 0, 0, nullptr, 0);
        } else {
            // The same end as the lifted run's when it could not go there either; when it went on there (code the
            // lift has at that address -- in the image, the runtime's registry -- but no plugin and no function of
            // the port), the port has handed the branch to its host, as its contract says: counted apart.
            Rec r;
            if (!m.t->next(r) || r.kind != kUnknownBranch || r.a != target) {
                m.unresolved = true;
                m.unknown_target = target;
                m.unresolved_site = m.unknown_site;
                longjmp(*m.jb, 1);
            }
        }
        m.unknown_branch = true;
        m.unknown_target = target;
        return 0;
    }
    char w[96];
    snprintf(w, sizeof w, "a branch to %05X: no plugin the replay knows is there", target);
    m.fail(w, nullptr);
}
static int run_overlay(Model& m, uint8_t* ls, u128* gpr, uint32_t target, uint32_t* ret)
{
    cb_in();
    const int r = run_overlay_(m, ls, gpr, target, ret);
    cb_out();
    return r;
}

// ---- the lifted side: the stand-in runtime -------------------------------------------------------------------------
static Model* g_m;
static void (*g_lifted[kLs / 4])(spu_context*);
static volatile long long g_budget = 0;   // local-store accesses left (synthesized cases); 0 = no budget
static bool g_over_budget = false;

// The lifted code prints its halts to stderr ("[spu] HALT-ASSERT heqi pc=0x..."): stderr goes to a file for the
// whole run, read after each halt (as the A2 checker does).
static FILE* g_err_reader;
static void capture_stderr()
{
    const std::string path = "F:/3d dot game heroes/scratch/mixer/out/lift_stderr_" + std::to_string(_getpid()) + ".txt";
    fflush(stderr);
    const int fd = _open(path.c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return;
    _dup2(fd, 2);
    _close(fd);
    g_err_reader = fopen(path.c_str(), "rb");
}
static uint32_t lifted_halt_pc()
{
    if (!g_err_reader) return 0;
    fflush(stderr);
    char buf[4096];
    std::string s;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, g_err_reader)) > 0) s.append(buf, n);
    clearerr(g_err_reader);
    const size_t at = s.rfind("pc=0x");
    return at == std::string::npos ? 0 : (uint32_t)strtoul(s.c_str() + at + 5, nullptr, 16);
}

static bool g_lift_halted = false;
static uint32_t g_lift_halt_pc = 0;

extern "C" {
void spu_0005_at_00F86A80_spu_recomp_register(void);

__thread void (*g_spu_trampoline_fn)(spu_context*) = nullptr;
volatile int g_spu_direct = 1;     // SPU_DIRECT's tail calls, as in the game
uint8_t g_spu_stack_reset_hint[1024];
int g_spu_ls_watch_n = 0;

void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { g_lifted[(addr & kMask) >> 2] = fn; }
// The runtime's LS watch: armed (g_spu_ls_watch_n) only for a synthesized case's lifted run, as its budget:
// local-store accesses, and the depth of nested calls (a case's pointer can make a function call itself
// without end; each call nests a host frame, and the runtime guards that only on the way back up).
spu_context* g_fuzz_ctx;
void spu_ls_watch_slow(uint32_t, int, const uint8_t*, uint32_t, uint32_t)
{
    if (g_budget && (--g_budget == 0 || (g_fuzz_ctx && g_fuzz_ctx->host_depth > 3000))) {
        g_over_budget = true;
        longjmp(*g_m->jb, 1);
    }
}
int ps3_strict(void) { return 0; }
void ps3_strict_fail(const char* kind, const char*, ...) { printf("%s (not expected)\n", kind); exit(4); }
void spu_img_restore(spu_context*, int32_t) {}
void spu_halt(spu_context* ctx)
{
    if (g_m->fuzz) {
        g_lift_halted = true;
        g_lift_halt_pc = lifted_halt_pc();
        longjmp(*g_m->jb, 1);
    }
    char w[96];
    snprintf(w, sizeof w, "the lifted mixer halted at %05X", lifted_halt_pc());
    (void)ctx;
    g_m->fail(w, nullptr);
}
void spu_stop(spu_context* ctx)
{
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    if (model_stop(*g_m, ctx->stop_code, ctx->pc & kMask)) { ctx->status = SPU_STATUS_RUNNING; return; }
    if (g_m->ended_normally || g_m->fuzz) longjmp(*g_m->jb, 1);
    char w[64];
    snprintf(w, sizeof w, "the lifted mixer stopped (stop %X)", ctx->stop_code);
    g_m->why = w;
    g_m->at_end = true;
    longjmp(*g_m->jb, 1);
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
u128 spu_rdch(spu_context*, uint32_t ch) { return spu_make_preferred_u32(model_rdch(*g_m, ch)); }
#pragma clang diagnostic pop
uint32_t spu_rchcnt(spu_context*, uint32_t ch) { return model_rchcnt(*g_m, ch); }
void spu_wrch(spu_context*, uint32_t ch, u128 v) { model_wrch(*g_m, ch, v._u32[0]); }

// The runtime's dispatch (spu_channels.c spu_indirect_branch): a lifted function of the image, else (here) a
// plugin by its code.
void spu_indirect_branch(spu_context* ctx)
{
    ctx->pc &= kMask;
    static const bool trace = getenv("MIXER_TRACE_BRANCHES") != nullptr;
    if (trace) printf("  [lift] indirect branch to %05X (r0 %05X)\n", ctx->pc, ctx->gpr[0]._u32[0]);
    if (void (*fn)(spu_context*) = (ctx->pc & 3) ? nullptr : g_lifted[ctx->pc >> 2]) { fn(ctx); return; }   // the registry: exact addresses
    uint32_t ret = 0;
    if (!run_overlay(*g_m, ctx->ls, ctx->gpr, ctx->pc, &ret)) longjmp(*g_m->jb, 1);   // a synthesized case ends
    ctx->pc = ret;
}
void spu_indirect_branch_mt(spu_context* ctx) { spu_indirect_branch(ctx); }
// The runtime's drain (spu_drain.c spu_drain_call), without its diagnostics and interrupts: the pending
// transfers run until a dispatch to the return pc; a callee that comes back elsewhere ends the run here (the
// runtime would go on at a lifted entry there).
static bool g_bad_return = false;
static uint32_t g_bad_return_pc = 0;
void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    while (g_spu_trampoline_fn) {
        if (g_spu_trampoline_fn == spu_indirect_branch && (ctx->pc & kMask) == (return_pc & kMask)) {
            g_spu_trampoline_fn = nullptr;
            return;
        }
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = nullptr;
        fn(ctx);
    }
    if ((ctx->pc & kMask) == (return_pc & kMask)) return;
    if (g_m->fuzz) {
        g_bad_return = true;
        g_bad_return_pc = ctx->pc & kMask;
        longjmp(*g_m->jb, 1);
    }
    char w[96];
    snprintf(w, sizeof w, "the lifted mixer returned to %05X, not to %05X", ctx->pc & kMask, return_pc & kMask);
    g_m->fail(w, nullptr);
}
}  // extern "C"

// ---- the port's host ------------------------------------------------------------------------------------------
static void port_wrch(void* u, uint32_t ch, uint32_t v) { model_wrch(*(Model*)u, ch, v); }
static uint32_t port_rdch(void* u, uint32_t ch) { return model_rdch(*(Model*)u, ch); }
static uint32_t port_rchcnt(void* u, uint32_t ch) { return model_rchcnt(*(Model*)u, ch); }
static int port_stop(void* u, uint32_t code, uint32_t pc, uint32_t block)
{
    (void)pc;
    Model& m = *(Model*)u;
    const int r = model_stop(m, code, block);   // the runtime's ctx->pc there: the lifted block the stop is in
    if (!r && !m.ended_normally && !m.fuzz) { m.at_end = true; m.why = "the port stopped"; }
    return r;
}
static int port_branch(void* u, mx::Branch kind, uint32_t target, uint32_t site, uint32_t* ret)
{
    Model& m = *(Model*)u;
    m.unknown_site = site;
    if (kind != mx::Branch::overlay_call && kind != mx::Branch::overlay_jump) {
        if (m.fuzz) {
            // A synthesized case. The same end as the lifted run's when it could not go there either (no code
            // there); when the lifted run went on there (a block of the image the runtime dispatches to, but no
            // function of the port), the port has handed the branch to its host, as its contract says, and the
            // case is counted apart.
            Rec r;
            if (m.t->next(r) && r.kind == kUnknownBranch && r.a == target) {
                m.unknown_branch = true;
                m.unknown_target = target;
                return 0;
            }
            m.unresolved = true;
            m.unknown_target = target;
            m.unresolved_site = site;
            longjmp(*m.jb, 1);
        }
        char w[128];
        snprintf(w, sizeof w, "the port met an unresolved %s to %05X at %05X", kind == mx::Branch::unresolved_call ? "call" : "jump", target, site);
        m.fail(w, nullptr);
    }
    return run_overlay(m, m.ls, m.gpr, target, ret);
}
static void port_unsupported(void* u, uint32_t pc) { model_unsupported(*(Model*)u, pc); }

// ---- the watchdog: a run with no channel operation for 120 s is reported, and the checker stops ------------------
static std::atomic<int> g_busy{ 0 };
static char g_busy_what[200];
static void watchdog()
{
    unsigned long long last = 0;
    int idle = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const unsigned long long p = g_progress.load();
        if (!g_busy.load() || p != last) { idle = 0; last = p; continue; }
        if (++idle >= 120) {
            printf("[HANG] %s: no channel operation for 120 s\nFAILED\n", g_busy_what);
            fflush(stdout);
            _exit(3);
        }
    }
}

// ---- the game trace's passes -----------------------------------------------------------------------------------------
struct Pass {
    const char* name;
    bool ran = false;
    double seconds = 0;
    Model m;
    std::vector<uint8_t> final_ls;
    u128 final_gpr[128];
    std::string outcome;
};

static void report_pass(Pass& p, unsigned show)
{
    Model& m = p.m;
    printf("[%s] %s: %llu blocks (receives) replayed%s; %llu records (%llu rdch, %llu wrch, %llu GETs, %llu PUTs, %llu GETLLAR, %llu "
           "PUTLLC, %.1f MB DMA, %llu stops); %llu rchcnt answered; %llu plugin calls (codec_raw %llu, sfxreverb %llu); %.2f s\n",
           m.failed ? "FAIL" : "PASS", p.name, (unsigned long long)m.blocks, m.failed ? " before the difference" : "",
           (unsigned long long)m.t->index, (unsigned long long)m.rdch, (unsigned long long)m.wrch, (unsigned long long)m.gets,
           (unsigned long long)m.puts, (unsigned long long)m.getllar, (unsigned long long)m.putllc, m.dma_bytes / 1048576.0,
           (unsigned long long)m.stops, (unsigned long long)m.rchcnt, (unsigned long long)m.overlay_calls, g_plugins[0].calls,
           g_plugins[1].calls, p.seconds);
    printf("  outcome: %s\n", p.outcome.c_str());
    if (m.pc_mismatch)
        printf("  stops whose pc differs from the trace's (the runtime's ctx->pc): %llu (first: trace %05X, ours %05X)\n",
               (unsigned long long)m.pc_mismatch, m.first_pc_mismatch_trace, m.first_pc_mismatch_ours);
    if (m.failed) {
        printf("  the difference at record %llu (block %llu): %s\n", (unsigned long long)m.fail_record, (unsigned long long)m.blocks, m.why.c_str());
        const unsigned n = m.ring_n < show ? m.ring_n : show;
        printf("  the last %u records consumed:\n", n);
        for (unsigned i = m.ring_n - n; i < m.ring_n; i++) {
            const Model::Seen& s = m.ring[i % m.ring.size()];
            char what[96];
            snprintf(what, sizeof what, s.fmt, s.x, s.y);
            printf("    #%llu %s\n", (unsigned long long)s.r.index, what);
        }
    }
}

// ---- synthesized cases -------------------------------------------------------------------------------------------------
struct End {
    int kind = 0;     // 1 returned (a: the pc), 2 halted (a: pc), 3 stopped (a: code), 4 a branch to unknown code (a: target),
                      // 5 returned elsewhere than the link (a: pc), 6 over the budget, 7 a failure (why), 8 the transcript ran out
    uint32_t a = 0;
    std::string why;
};
static std::string end_text(const End& e)
{
    char w[200];
    switch (e.kind) {
    case 1: snprintf(w, sizeof w, "returned to %05X", e.a); break;
    case 2: snprintf(w, sizeof w, "halted at %05X", e.a); break;
    case 3: snprintf(w, sizeof w, "stopped (stop %X)", e.a); break;
    case 4: snprintf(w, sizeof w, "a branch to %05X, code no plugin has", e.a); break;
    case 5: snprintf(w, sizeof w, "returned to %05X, not to the link", e.a); break;
    case 6: snprintf(w, sizeof w, "over the budget"); break;
    case 7: snprintf(w, sizeof w, "failed: %s", e.why.c_str()); break;
    case 8: snprintf(w, sizeof w, "the transcript ran out"); break;
    default: snprintf(w, sizeof w, "?"); break;
    }
    return w;
}

struct FuzzTally {
    unsigned cases = 0, compared = 0, equal = 0, differing = 0, over_budget = 0, lift_failed = 0, unresolved = 0, stuck = 0;
    std::vector<std::pair<std::string, unsigned>> lift_failures;   // why, how often (the reason's first words)
    unsigned by_end[9] = {};
    unsigned by_end_unmutated[9] = {};   // the block cases with no mutation: real blocks on their own inputs
    unsigned unmutated = 0;
    // branches to unknown code (compared, equal ends) and unresolved ones (handed to the host), by site and by the
    // kind of case: 0 the block unmutated, 1 the block mutated, 2 a single function
    std::vector<std::array<unsigned, 5>> sites;   // site, kind of end (4 unknown, 9 unresolved), counts by case kind
    void site(uint32_t at, unsigned end, unsigned kind)
    {
        for (auto& x : sites)
            if (x[0] == at && x[1] == end) { x[2 + kind]++; return; }
        std::array<unsigned, 5> x{ at, end, 0, 0, 0 };
        x[2 + kind] = 1;
        sites.push_back(x);
    }
    unsigned block_cases = 0, function_cases = 0;
    unsigned long long records = 0;
};

constexpr uint32_t kFakeLink = 0x3FF00;   // a random case's return address (no code there)

// A synthesized case's lifted run, on a thread of its own (its stack 64 MB), so that a run caught in a loop on
// registers alone -- no channel operation, no local-store access, so no budget ends it -- can be abandoned.
struct LiftRun {
    spu_context* ctx;
    uint32_t entry, link;
    int kind = 0;      // 1 returned (a: the pc), else 0: see the model's flags
    uint32_t a = 0;
};
// Where an abandoned run was: the lifted function its host instruction pointer is in (dbghelp, the checker's
// .pdb), for the report.
static std::string where_is(HANDLE thread)
{
    CONTEXT cx{};
    cx.ContextFlags = CONTEXT_CONTROL;
    SuspendThread(thread);
    if (!GetThreadContext(thread, &cx)) return "?";
    static bool sym = false;
    using SymInit = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymFrom = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, void*);
    static SymFrom from = nullptr;
    if (!sym) {
        sym = true;
        if (HMODULE d = LoadLibraryA("dbghelp.dll")) {
            SymInit init = (SymInit)(void*)GetProcAddress(d, "SymInitialize");
            from = (SymFrom)(void*)GetProcAddress(d, "SymFromAddr");
            if (!init || !init(GetCurrentProcess(), nullptr, TRUE)) from = nullptr;
        }
    }
    char buf[sizeof(SYMBOL_INFO_PACKAGE)] = {};
    SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    DWORD64 off = 0;
    char w[200];
    if (from && from(GetCurrentProcess(), cx.Rip, &off, si)) snprintf(w, sizeof w, "%s+0x%llX", si->Name, (unsigned long long)off);
    else   // the repository's tools/symbolize.py names it from the .pdb
        snprintf(w, sizeof w, "the checker's rva 0x%llX", (unsigned long long)(cx.Rip - (DWORD64)(uintptr_t)GetModuleHandleA(nullptr)));
    return w;
}
static DWORD WINAPI lift_worker(void* p)
{
    LiftRun& r = *(LiftRun*)p;
    jmp_buf jb;
    g_m->jb = &jb;
    g_spu_trampoline_fn = nullptr;   // this thread's
    if (setjmp(jb) == 0) {
        g_lifted[r.entry >> 2](r.ctx);
        spu_drain_call(r.ctx, r.link);
        r.kind = 1;
        r.a = r.ctx->pc & kMask;
    }
    return 0;
}

static bool fuzz_case(unsigned k, const std::vector<Sample>& samples, Mem& mem, std::mt19937& rng, FuzzTally& tally, bool verbose)
{
    const Sample& S = samples[rng() % samples.size()];
    std::vector<uint8_t> ls0(S.ls);
    u128 g0[128];
    memcpy(g0, S.gpr, sizeof g0);
    std::unordered_map<uint32_t, Line> over(S.mem);   // what the sample's next block read, over the memory
    mem.over = &over;
    uint32_t entry = 0, link = 0;
    unsigned nmut = 0;
    std::string what;
    char w[256];
    const bool block_case = !S.gets.empty() && (rng() % 3) != 0;
    if (block_case) {
        // the block, as the main loop calls it (0xE4A4: brsl 0xDA30 with main's r83, r90, r89, r82, r84, r91)
        entry = mx::kEntryBlock;
        link = 0xE4A8;
        const int args[6] = { 83, 90, 89, 82, 84, 91 };
        for (int i = 0; i < 6; i++) g0[3 + i] = S.gpr[args[i]];
        g0[0] = spu_make_preferred_u32(link);
        // mutations in what the block's GETs read
        nmut = rng() % 6;
        std::string muts;
        for (unsigned i = 0; i < nmut; i++) {
            const auto& gt = S.gets[rng() % S.gets.size()];
            if (gt.second < 4) continue;
            const uint32_t ea = (gt.first + 4 * (rng() % (gt.second / 4))) & ~3u;
            uint8_t b[4];
            mem.read(ea, b, 4);
            uint32_t v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
            switch (rng() % 6) {
            case 0: v = rng(); break;
            case 1: v = 0; break;
            case 2: v = rng() % 16; break;
            case 3: v ^= 1u << (rng() % 32); break;
            case 4: v = v + 1; break;
            default: { uint8_t c[4]; mem.read((gt.first + 4 * (rng() % (gt.second / 4))) & ~3u, c, 4);
                       v = (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3]; } break;
            }
            const uint8_t nb[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
            Mem::write(over, ea, nb, 4, &mem);
            char m[40];
            snprintf(m, sizeof m, " [%08X]=%08X", ea, v);
            muts += m;
        }
        snprintf(w, sizeof w, "case %u: the block (0xDA30) from block %llu's state, %u mutation(s)%s", k, (unsigned long long)S.block, nmut,
                 muts.c_str());
        tally.block_cases++;
    } else {
        // any function, arguments from the sample
        entry = mx::entry_address((int)(rng() % (unsigned)mx::entry_count()));
        link = kFakeLink;
        g0[0] = spu_make_preferred_u32(link);
        for (int r = 3; r <= 10; r++) {
            uint32_t v;
            switch (rng() % 5) {
            case 0: case 1: v = S.gpr[rng() % 128]._u32[0]; break;
            case 2: v = 0x10800 + 16 * (rng() % ((0x3FE00 - 0x10800) / 16)); break;
            case 3: v = rng() % 64; break;
            default: v = S.gets.empty() ? rng() : S.gets[rng() % S.gets.size()].first; break;
            }
            g0[r] = spu_make_preferred_u32(v);
            if (rng() % 4 == 0) for (int q = 1; q < 4; q++) g0[r]._u32[q] = rng();
        }
        g0[1]._u32[0] = (S.gpr[1]._u32[0] - 16 * (rng() % 64)) & ~15u;
        snprintf(w, sizeof w, "case %u: function %05X from block %llu's state, r3-r6 %X %X %X %X", k, entry, (unsigned long long)S.block,
                 g0[3]._u32[0], g0[4]._u32[0], g0[5]._u32[0], g0[6]._u32[0]);
        tally.function_cases++;
    }
    what = w;
    snprintf(g_busy_what, sizeof g_busy_what, "%s", what.c_str());
    if (verbose) printf("%s\n", what.c_str());
    tally.cases++;
    const uint32_t seed = rng();

    // ---- the lifted run, its transcript recorded ----
    std::vector<uint8_t> transcript;
    transcript.reserve(1 << 20);
    static spu_context* ctx = (spu_context*)calloc(1, sizeof(spu_context));
    g_fuzz_ctx = ctx;
    ctx->ls = ctx->ls_store;
    memcpy(ctx->ls, ls0.data(), kLs);
    memcpy(ctx->gpr, g0, sizeof g0);
    ctx->pc = entry;
    ctx->image_id = 6;
    ctx->host_depth = 1;
    ctx->steps = 1;
    ctx->status = SPU_STATUS_RUNNING;
    Model lm;
    lm.mode = Model::generate;
    lm.fuzz = true;
    lm.ls = ctx->ls;
    lm.gpr = ctx->gpr;
    std::mt19937 lrng(seed);
    lm.rng = &lrng;
    lm.mem = &mem;
    lm.out = &transcript;
    g_m = &lm;
    g_lift_halted = g_bad_return = g_over_budget = false;
    g_budget = entry == mx::kEntryBlock ? 40000000 : 8000000;
    g_spu_ls_watch_n = 1;
    g_spu_ls_rdiag = g_spu_ls_wdiag = 1;
    End le;
    LiftRun* run = new LiftRun{ ctx, entry, link };   // left behind if its thread is abandoned
    bool stuck = false;
    std::string stuck_at;
    {
        HANDLE h = CreateThread(nullptr, 64u << 20, lift_worker, run, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        unsigned long long p0 = g_progress.load();
        long long b0 = g_budget;
        int still = 0;
        while (WaitForSingleObject(h, 1000) == WAIT_TIMEOUT) {
            const unsigned long long p = g_progress.load();
            const long long b = g_budget;
            if (p != p0 || b != b0) { p0 = p; b0 = b; still = 0; continue; }
            if (++still >= 10) {   // 10 s with no channel operation and no local-store access
                stuck_at = where_is(h);
                TerminateThread(h, 1);
                WaitForSingleObject(h, INFINITE);
                stuck = true;
                break;
            }
        }
        CloseHandle(h);
    }
    if (!stuck) {
        le.kind = run->kind;
        le.a = run->a;
        delete run;
    }
    g_budget = 0;
    g_spu_ls_watch_n = 0;
    g_spu_ls_rdiag = g_spu_ls_wdiag = 0;
    g_spu_trampoline_fn = nullptr;
    if (!le.kind) {
        if (g_over_budget || lm.over_cap) le.kind = 6;
        else if (g_lift_halted) { le.kind = 2; le.a = g_lift_halt_pc; }
        else if (lm.stopped) { le.kind = 3; le.a = lm.stop_code; }
        else if (lm.unknown_branch) { le.kind = 4; le.a = lm.unknown_target; }
        else if (g_bad_return) { le.kind = 5; le.a = g_bad_return_pc; }
        else { le.kind = 7; le.why = lm.why; }
    }
    if (stuck) {
        tally.stuck++;
        mem.over = nullptr;
        printf("[info] %s: the lifted run is caught in a loop with no channel operation and no local-store access, in %s "
               "(abandoned after 10 s; not counted)\n", what.c_str(), stuck_at.c_str());
        return true;
    }
    lm.put(kEnd, 3, 0, 0, 0, nullptr, 0);
    if (le.kind == 6) { tally.over_budget++; mem.over = nullptr; if (verbose) printf("%s: the lifted run is over its budget\n", what.c_str()); return true; }
    if (le.kind == 7) {
        std::string key = le.why.substr(0, le.why.find(" at "));
        key = key.substr(0, key.find(": the"));
        bool found = false;
        for (auto& f : tally.lift_failures) if (f.first == key) { f.second++; found = true; }
        if (!found) tally.lift_failures.push_back({ key, 1 });
    }
    if (le.kind == 7) { tally.lift_failed++; mem.over = nullptr; if (verbose) printf("%s: the lifted run: %s\n", what.c_str(), le.why.c_str()); return true; }

    // ---- the port, replaying the transcript from the same state ----
    if (verbose) printf("  the lifted run: %s, %zu transcript bytes\n", end_text(le).c_str(), transcript.size());
    std::vector<uint8_t> ls(ls0);
    static u128 g[128];
    memcpy(g, g0, sizeof g);
    Trace t;
    t.open_memory(transcript);
    Model pm;
    pm.mode = Model::replay;
    pm.fuzz = true;
    pm.is_port = true;
    pm.t = &t;
    pm.ls = ls.data();
    pm.gpr = g;
    jmp_buf pjb;
    pm.jb = &pjb;
    const mx::Host host{ &pm, port_wrch, port_rdch, port_rchcnt, port_stop, port_branch, port_unsupported };
    End pe;
    g_busy = 1;
    if (setjmp(pjb) == 0) {
        mx::Outcome out;
        const mx::Result r = mx::call(ls.data(), g, entry, host, &out);
        switch (r) {
        case mx::Result::handled:   // the port returns where its function went; the lift's drain checks it against the link
            pe.kind = (out.ret_pc & kMask) == (link & kMask) ? 1 : 5;
            pe.a = out.ret_pc & kMask;
            break;
        case mx::Result::halted: pe.kind = 2; pe.a = out.pc; break;
        case mx::Result::stopped: pe.kind = 3; pe.a = out.code; break;
        case mx::Result::ended:
            if (pm.unknown_branch) { pe.kind = 4; pe.a = pm.unknown_target; }
            else if (pm.stopped) { pe.kind = 3; pe.a = pm.stop_code; }
            else { pe.kind = 7; pe.why = "ended"; }
            break;
        case mx::Result::bad_return: pe.kind = 5; pe.a = out.pc & kMask; break;
        case mx::Result::unresolved: pe.kind = 7; pe.why = "a jump table's target outside its table"; break;
        default: pe.kind = 7; pe.why = "declined"; break;
        }
        Rec rest;
        if (t.next(rest)) { pe.kind = 7; pe.why = "the transcript has records the port did not consume"; }
    } else {
        pe.kind = pm.failed ? 7 : 8;
        pe.why = pm.why;
    }
    g_busy = 0;
    mem.over = nullptr;
    const unsigned case_kind = block_case ? (nmut ? 1u : 0u) : 2u;
    if (pm.unresolved) {
        // the lifted run went on into a block of the image through a pointer the case made; the port gave the
        // branch to its host (Branch::unresolved_*): not a comparison
        tally.unresolved++;
        tally.site(pm.unresolved_site, 9, case_kind);
        if (verbose || tally.unresolved <= 5)
            printf("[info] %s: the port handed a branch to %05X (at %05X; no function of the port there) to its host; the lifted run "
                   "went on there (%s)\n", what.c_str(), pm.unknown_target, pm.unresolved_site, end_text(le).c_str());
        return true;
    }
    tally.records += t.index;
    tally.compared++;
    tally.by_end[le.kind]++;
    if (case_kind == 0) { tally.unmutated++; tally.by_end_unmutated[le.kind]++; }
    if (le.kind == 4 && pe.kind == 4) tally.site(pm.unknown_site, 4, case_kind);
    // the outcomes, every local-store byte, all 128 registers
    std::vector<std::string> why;
    if (pe.kind != le.kind || pe.a != le.a) why.push_back("outcome: lifted " + end_text(le) + ", port " + end_text(pe));
    unsigned lsd = 0;
    uint32_t first = 0;
    for (uint32_t i = 0; i < kLs; i++)
        if (ls[i] != ctx->ls[i]) { if (!lsd++) first = i; }
    if (lsd) {
        snprintf(w, sizeof w, "local store: %u bytes differ, first at %05X (port %02X, lifted %02X)", lsd, first, ls[first], ctx->ls[first]);
        why.push_back(w);
    }
    for (int r = 0; r < 128; r++)
        if (memcmp(&g[r], &ctx->gpr[r], 16)) {
            snprintf(w, sizeof w, "r%d: port %08X %08X %08X %08X, lifted %08X %08X %08X %08X", r, g[r]._u32[0], g[r]._u32[1], g[r]._u32[2],
                     g[r]._u32[3], ctx->gpr[r]._u32[0], ctx->gpr[r]._u32[1], ctx->gpr[r]._u32[2], ctx->gpr[r]._u32[3]);
            why.push_back(w);
            break;
        }
    const bool ok = why.empty();
    (ok ? tally.equal : tally.differing)++;
    if (!ok || verbose) {
        printf("[%s] %s: %s; %llu records (%llu GETs, %llu PUTs)\n", ok ? "PASS" : "FAIL", what.c_str(), end_text(le).c_str(),
               (unsigned long long)t.index, (unsigned long long)lm.gets, (unsigned long long)lm.puts);
        for (const std::string& s : why) printf("  %s\n", s.c_str());
        if (!ok && pm.failed) printf("  the port's replay: record %llu: %s\n", (unsigned long long)pm.fail_record, pm.why.c_str());
    }
    return ok;
}

// ---- --profile: the mixer's own time, lifted against the port ----------------------------------------------------
// Passes over a preloaded trace, alternately the lifted mixer and the port, without the block-boundary
// snapshots; each pass's time less the time in the model and the plugins (cb_in / cb_out) is the mixer's code.
// --sample PREFIX: a sampler thread reads the replay thread's instruction pointer every 250 us and counts it by
// RVA in this executable when it is in the mixer's own code; PREFIX_lift.txt and PREFIX_port.txt for
// tools/fmod_mixer_profile.py (it names the functions from the .pdb).
static std::atomic<int> g_sample_side{ -1 };   // -1 not sampling, 0 the lifted mixer's pass, 1 the port's
static std::atomic<bool> g_sampler_quit{ false };
static std::unordered_map<uint64_t, unsigned> g_samples[2];
static unsigned long long g_samples_cb[2], g_samples_code[2];

static void sampler(HANDLE target)
{
    const uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(nullptr);
    LARGE_INTEGER f, now, next;
    QueryPerformanceFrequency(&f);
    const long long period = f.QuadPart / 4000;
    QueryPerformanceCounter(&next);
    while (!g_sampler_quit.load()) {
        next.QuadPart += period;
        do QueryPerformanceCounter(&now); while (now.QuadPart < next.QuadPart);
        const int side = g_sample_side.load();
        if (side < 0) continue;
        if (SuspendThread(target) == (DWORD)-1) continue;
        alignas(16) CONTEXT cx{};
        cx.ContextFlags = CONTEXT_CONTROL;
        const bool ok = GetThreadContext(target, &cx) != 0;
        const int cb = g_cb_depth;
        ResumeThread(target);
        if (!ok) continue;
        if (cb) g_samples_cb[side]++;
        else {
            g_samples_code[side]++;
            g_samples[side][cx.Rip - base]++;
        }
    }
}

struct Timing {
    double wall = 0, cb = 0;
    uint64_t blocks = 0;
    bool ok = false;
    std::string why;
};

static Timing profile_pass(bool port_side, const char* path, uint64_t block_limit, double tsc_hz)
{
    Timing T;
    Trace t;
    if (!t.open(path, true)) { T.why = "cannot read the trace"; return T; }
    std::vector<Snap> snaps;
    Model m;
    m.t = &t;
    m.is_port = port_side;
    m.block_limit = block_limit;
    m.profile = true;
    m.snaps = &snaps;
    jmp_buf jb;
    m.jb = &jb;
    g_cb_ticks = 0;
    g_cb_depth = 0;
    std::chrono::steady_clock::time_point t0, t1;
    if (!port_side) {
        static spu_context* ctx = (spu_context*)calloc(1, sizeof(spu_context));
        ctx->ls = ctx->ls_store;
        memcpy(ctx->ls, t.ls.data(), kLs);
        memcpy(ctx->gpr, t.gpr, sizeof ctx->gpr);
        ctx->pc = t.entry;
        ctx->image_id = 6;
        ctx->host_depth = 1;
        ctx->steps = 1;
        ctx->status = SPU_STATUS_RUNNING;
        m.ls = ctx->ls;
        m.gpr = ctx->gpr;
        g_m = &m;
        g_spu_trampoline_fn = nullptr;
        g_sample_side = 0;
        t0 = std::chrono::steady_clock::now();
        if (setjmp(jb) == 0) {
            g_lifted[t.entry >> 2](ctx);
            spu_drain_call(ctx, 0xFFFFFFFFu);
        }
        t1 = std::chrono::steady_clock::now();
        g_spu_trampoline_fn = nullptr;
    } else {
        static std::vector<uint8_t> ls(kLs);
        static u128 gpr[128];
        memcpy(ls.data(), t.ls.data(), kLs);
        memcpy(gpr, t.gpr, sizeof gpr);
        m.ls = ls.data();
        m.gpr = gpr;
        const mx::Host host{ &m, port_wrch, port_rdch, port_rchcnt, port_stop, port_branch, port_unsupported };
        g_sample_side = 1;
        t0 = std::chrono::steady_clock::now();
        if (setjmp(jb) == 0) {
            mx::Outcome out;
            mx::call(ls.data(), gpr, t.entry, host, &out);
        }
        t1 = std::chrono::steady_clock::now();
    }
    g_sample_side = -1;
    g_cb_depth = 0;
    T.wall = std::chrono::duration<double>(t1 - t0).count();
    T.cb = (double)g_cb_ticks / tsc_hz;
    T.blocks = m.blocks;
    T.ok = !m.failed;
    T.why = m.why;
    t.close();
    return T;
}

static int profile_main(const char* path, unsigned passes, uint64_t block_limit, const char* sample_prefix)
{
    // the TSC's rate, against the steady clock
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t k0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double tsc_hz = (double)(__rdtsc() - k0) / std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    printf("[info] profile: %s preloaded; TSC %.3f GHz; %u pass(es) each, alternating%s\n", path, tsc_hz / 1e9, passes,
           sample_prefix ? ", sampled at 4 kHz" : "");
    HANDLE self = nullptr;
    if (sample_prefix) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
        std::thread(sampler, self).detach();
    }
    double best[2] = { 1e30, 1e30 }, sum[2] = { 0, 0 }, cbsum[2] = { 0, 0 };
    uint64_t blocks[2] = { 0, 0 };
    bool ok = true;
    for (unsigned p = 0; p < passes; p++)
        for (int side = 0; side < 2; side++) {
            const Timing T = profile_pass(side == 1, path, block_limit, tsc_hz);
            const double code = T.wall - T.cb;
            printf("  pass %u %-16s %llu blocks: %.3f s in all, %.3f s in the model and plugins, %.3f s the mixer's code = %.1f us a "
                   "block%s%s\n", p + 1, side ? "the port" : "the lifted mixer", (unsigned long long)T.blocks, T.wall, T.cb, code,
                   T.blocks ? code * 1e6 / (double)T.blocks : 0.0, T.ok ? "" : "; FAILED: ", T.ok ? "" : T.why.c_str());
            ok = ok && T.ok;
            blocks[side] = T.blocks;
            if (T.blocks) {
                const double us = code * 1e6 / (double)T.blocks;
                if (us < best[side]) best[side] = us;
                sum[side] += us;
                cbsum[side] += T.cb * 1e6 / (double)T.blocks;
            }
        }
    printf("[%s] the mixer's code a block (%llu blocks; best of %u, mean): the lifted mixer %.1f us (%.1f), the port %.1f us (%.1f): "
           "the port %+.1f%%; the model and plugins %.1f / %.1f us a block\n",
           ok ? "info" : "FAIL", (unsigned long long)blocks[0], passes, best[0], sum[0] / passes, best[1], sum[1] / passes,
           (best[1] / best[0] - 1) * 100, cbsum[0] / passes, cbsum[1] / passes);
    if (sample_prefix) {
        g_sampler_quit = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int side = 0; side < 2; side++) {
            const std::string fn = std::string(sample_prefix) + (side ? "_port.txt" : "_lift.txt");
            FILE* f = fopen(fn.c_str(), "w");
            if (!f) { printf("cannot write %s\n", fn.c_str()); continue; }
            fprintf(f, "# samples in the mixer's code %llu, in the model and plugins %llu, us a block %.2f\n", g_samples_code[side],
                    g_samples_cb[side], best[side]);
            for (const auto& kv : g_samples[side]) fprintf(f, "%llX %u\n", (unsigned long long)kv.first, kv.second);
            fclose(f);
            printf("[info] %s: %llu samples in the mixer's code, %llu in the model and plugins\n", fn.c_str(), g_samples_code[side],
                   g_samples_cb[side]);
        }
    }
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        printf("usage: fmod_mixer_check <trace.bin> [--lift-only | --port-only] [--blocks N] [--preload] [--show N] [--fuzz N [--seed S] "
               "[--case K]] [--profile N [--sample PREFIX]] [--image-dir DIR]\n");
        return 2;
    }
    const char* trace_path = argv[1];
    bool do_lift = true, do_port = true, preload = false;
    uint64_t block_limit = ~0ull;
    unsigned show = 24, fuzz = 0, seed = 1, only_case = 0, profile = 0;
    const char* sample_prefix = nullptr;
    std::string image_dir = "build/gen/spu/images";
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--lift-only")) do_port = false;
        else if (!strcmp(argv[i], "--port-only")) do_lift = false;
        else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) block_limit = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--image-dir") && i + 1 < argc) image_dir = argv[++i];
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preload")) preload = true;
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) fuzz = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--case") && i + 1 < argc) only_case = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample") && i + 1 < argc) sample_prefix = argv[++i];
    }
    if (!load_plugin(g_plugins[0], image_dir + "/spu_0008_at_00FA9B80.elf") ||
        !load_plugin(g_plugins[1], image_dir + "/spu_0024_at_00FC7180.elf")) {
        printf("cannot read the plugin images under %s\n", image_dir.c_str());
        return 2;
    }
    capture_stderr();
    std::thread(watchdog).detach();
    spu_0005_at_00F86A80_spu_recomp_register();
    if (profile) return profile_main(trace_path, profile, block_limit, sample_prefix);

    std::vector<Snap> snaps;
    snaps.reserve(8192);
    std::vector<Sample> samples;
    Mem mem;
    Pass lift{ "the lifted mixer" }, port{ "the port" };

    auto setup = [&](Pass& p, Trace& t, bool is_port) {
        p.m.t = &t;
        p.m.is_port = is_port;
        p.m.block_limit = block_limit;
        p.m.snaps = &snaps;
    };

    if (do_lift) {
        Trace t;
        if (!t.open(trace_path, preload)) { printf("cannot read the trace %s\n", trace_path); return 2; }
        printf("[info] %s: image %u, entry pc %05X\n", trace_path, t.image, t.entry);
        if (t.image != 6 || t.entry != mx::kEntry) { printf("not a trace of the mixer from its entry\n"); return 2; }
        setup(lift, t, false);
        if (fuzz) {
            lift.m.samples = &samples;
            lift.m.mem = &mem;
        }
        spu_context* ctx = (spu_context*)calloc(1, sizeof(spu_context));
        ctx->ls = ctx->ls_store;
        memcpy(ctx->ls, t.ls.data(), kLs);
        memcpy(ctx->gpr, t.gpr, sizeof ctx->gpr);
        ctx->pc = t.entry;
        ctx->image_id = 6;
        ctx->host_depth = 1;
        ctx->steps = 1;
        ctx->status = SPU_STATUS_RUNNING;
        lift.m.ls = ctx->ls;
        lift.m.gpr = ctx->gpr;
        jmp_buf jb;
        lift.m.jb = &jb;
        g_m = &lift.m;
        g_spu_trampoline_fn = nullptr;
        snprintf(g_busy_what, sizeof g_busy_what, "the lifted mixer's replay");
        g_busy = 1;
        const auto t0 = std::chrono::steady_clock::now();
        if (setjmp(jb) == 0) {
            g_lifted[t.entry >> 2](ctx);
            spu_drain_call(ctx, 0xFFFFFFFFu);
            lift.outcome = "the thread returned";
        }
        lift.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        g_busy = 0;
        g_spu_trampoline_fn = nullptr;
        if (lift.m.failed) lift.outcome = "a difference from the trace";
        else if (lift.m.ended_normally) lift.outcome = "ended at the block limit";
        else if (lift.m.at_end)
            lift.outcome = lift.m.why.empty() ? std::string("the trace's end (") + (t.end_kind == 1 ? "its size limit" : t.end_kind == 2 ? "the thread ended" : "cut off") +
                                                    "), inside the next block"
                                              : lift.m.why;
        lift.final_ls.assign(ctx->ls, ctx->ls + kLs);
        memcpy(lift.final_gpr, ctx->gpr, sizeof lift.final_gpr);
        lift.ran = true;
        report_pass(lift, show);
        free(ctx);
        t.close();
    }

    if (do_port) {
        Trace t;
        if (!t.open(trace_path, preload)) { printf("cannot read the trace %s\n", trace_path); return 2; }
        for (Plugin& p : g_plugins) p.calls = 0;
        setup(port, t, true);
        std::vector<uint8_t> ls(t.ls);
        static u128 gpr[128];
        memcpy(gpr, t.gpr, sizeof gpr);
        port.m.ls = ls.data();
        port.m.gpr = gpr;
        jmp_buf jb;
        port.m.jb = &jb;
        const mx::Host host{ &port.m, port_wrch, port_rdch, port_rchcnt, port_stop, port_branch, port_unsupported };
        snprintf(g_busy_what, sizeof g_busy_what, "the port's replay");
        g_busy = 1;
        const auto t0 = std::chrono::steady_clock::now();
        if (setjmp(jb) == 0) {
            mx::Outcome out;
            const mx::Result r = mx::call(ls.data(), gpr, t.entry, host, &out);
            char w[160];
            snprintf(w, sizeof w, "the port's call returned %d (pc %05X, code %X, ret %05X)", (int)r, out.pc, out.code, out.ret_pc);
            port.outcome = w;
            if (r == mx::Result::ended && port.m.ended_normally) port.outcome = std::string("ended at the block limit (Result::ended)");
        }
        port.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        g_busy = 0;
        if (port.m.failed) port.outcome = "a difference from the trace";
        else if (port.m.at_end && port.outcome.empty())
            port.outcome = std::string("the trace's end (") + (t.end_kind == 1 ? "its size limit" : t.end_kind == 2 ? "the thread ended" : "cut off") +
                           "), inside the next block";
        port.final_ls = ls;
        memcpy(port.final_gpr, gpr, sizeof port.final_gpr);
        port.ran = true;
        report_pass(port, show);
        t.close();
    }

    bool ok = (!lift.ran || !lift.m.failed) && (!port.ran || !port.m.failed);
    if (lift.ran && port.ran) {
        const uint64_t compared = port.m.snap_equal + port.m.snap_diff;
        printf("[%s] block boundaries compared, the lifted mixer and the port (all 128 registers, the local store in 16 KB chunks): "
               "%llu, %llu equal, %llu differing%s%s\n",
               port.m.snap_diff ? "FAIL" : "PASS", (unsigned long long)compared, (unsigned long long)port.m.snap_equal,
               (unsigned long long)port.m.snap_diff, port.m.snap_diff ? "; first: " : "", port.m.first_snap_diff.c_str());
        if (port.m.blocks == lift.m.blocks && port.m.failed == lift.m.failed) {
            unsigned lsd = 0, rd = 0;
            uint32_t first = 0;
            for (uint32_t i = 0; i < kLs; i++)
                if (lift.final_ls[i] != port.final_ls[i]) { if (!lsd++) first = i; }
            // The port's register file is current at its stop and branch callbacks and at its exits, not at a
            // channel operation (it holds the registers in locals there): the registers are compared when both
            // ended at a stop (--blocks); they are compared at every block boundary in any case.
            const bool at_stop = port.m.ended_normally && lift.m.ended_normally;
            if (at_stop)
                for (int r = 0; r < 128; r++) rd += memcmp(&lift.final_gpr[r], &port.final_gpr[r], 16) != 0;
            printf("[%s] the state where both ended (block %llu): %u local-store bytes differ%s, %s\n", lsd || rd ? "FAIL" : "PASS",
                   (unsigned long long)port.m.blocks, lsd, lsd ? (" (first at " + std::to_string(first) + ")").c_str() : "",
                   at_stop ? (std::to_string(rd) + " registers differ").c_str()
                           : "the registers not compared there (the trace ends at a channel operation inside a block)");
            ok = ok && !lsd && !rd;
        } else {
            printf("[FAIL] the two passes ended at different places (blocks %llu and %llu)\n", (unsigned long long)lift.m.blocks,
                   (unsigned long long)port.m.blocks);
            ok = false;
        }
        ok = ok && !port.m.snap_diff && compared == lift.m.blocks;
        printf("[info] replay time (the replay model's DMA copies and compares included%s): lifted %.2f s, port %.2f s\n",
               preload ? "" : ", and the trace's reading", lift.seconds, port.seconds);
    }

    if (fuzz && lift.ran && !samples.empty()) {
        size_t lines = mem.base.size();
        printf("[info] synthesized cases: %zu samples (block states), main memory from the trace: %zu lines of 128 bytes\n", samples.size(), lines);
        std::mt19937 rng(seed);
        const bool progress = getenv("MIXER_CASE_PROGRESS") != nullptr;   // each case announced before it runs
        FuzzTally tally;
        const auto t0 = std::chrono::steady_clock::now();
        for (unsigned k = 1; k <= fuzz; k++) {
            std::mt19937 crng(rng());
            if (only_case && k != only_case) continue;
            if (progress) printf("case %u\n", k);
            fuzz_case(k, samples, mem, crng, tally, only_case != 0);
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("[%s] synthesized cases: %u (%u of the block, %u of single functions): %u compared, %u equal, %u differing; not counted: "
               "%u over the lifted run's budget, %u where the lifted run loops on registers alone, %u the lifted run could not "
               "finish, %u where the port handed a branch into the image (no function of the port there) to its host; %llu "
               "records; %.1f s\n",
               tally.differing ? "FAIL" : "PASS", tally.cases, tally.block_cases, tally.function_cases, tally.compared, tally.equal,
               tally.differing, tally.over_budget, tally.stuck, tally.lift_failed, tally.unresolved, tally.records, secs);
        if (!tally.lift_failures.empty()) {
            printf("[info] why lifted runs could not finish:");
            for (auto& f : tally.lift_failures) printf(" %s (%u);", f.first.c_str(), f.second);
            printf("\n");
        }
        printf("[info] the block cases with no mutation (real blocks on the bytes they read, the generator's clock): %u; ended: "
               "returned %u, halted %u, stopped %u, branched to unknown code %u, returned elsewhere %u\n", tally.unmutated,
               tally.by_end_unmutated[1], tally.by_end_unmutated[2], tally.by_end_unmutated[3], tally.by_end_unmutated[4],
               tally.by_end_unmutated[5]);
        printf("[info] branches to code no plugin has (compared: same end) and unresolved ones (handed to the host), by site: "
               "the block unmutated / mutated / single functions:");
        for (const auto& x : tally.sites)
            printf(" %05X %s %u/%u/%u;", x[0], x[1] == 4 ? "unknown" : "unresolved", x[2], x[3], x[4]);
        printf("\n");
        printf("[info] how the compared cases ended: returned %u, halted %u, stopped %u, branched to unknown code %u, returned "
               "elsewhere %u\n", tally.by_end[1], tally.by_end[2], tally.by_end[3], tally.by_end[4], tally.by_end[5]);
        ok = ok && !tally.differing;
    }

    if (port.ran || fuzz) {
        const int nb = mx::block_count();
        int taken = 0;
        for (int k = 0; k < nb; k++) taken += mx::g_block_hits[k] != 0;
        printf("[info] the port's blocks taken: %d of %d\n", taken, nb);
        if (getenv("MIXER_SHOW_UNTAKEN")) {
            printf("[info] never taken:");
            for (int k = 0; k < nb; k++) if (!mx::g_block_hits[k]) printf(" %05X(%05X)", mx::block_address(k), mx::block_function(k));
            printf("\n");
        }
        if (getenv("MIXER_COVERAGE_FILE")) {
            FILE* f = fopen(getenv("MIXER_COVERAGE_FILE"), "w");
            if (f) {
                for (int k = 0; k < nb; k++) fprintf(f, "%05X %05X %llu\n", mx::block_address(k), mx::block_function(k), mx::g_block_hits[k]);
                fclose(f);
            }
        }
    }
    printf("%s\n", ok ? "all passed" : "FAILED");
    return ok ? 0 : 1;
}
