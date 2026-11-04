/*
 * Where the game's threads run: DOTHEROES_CPU_PLACE (experimental, for A/Bs).
 *
 *   normal   (or unset) Windows places every thread
 *   percore  the whole process on one logical processor of each physical
 *            core, so no two of its threads share a core
 *   isolate  the PPU main thread -- this one: main() runs the guest's entry on
 *            it -- alone on one logical processor, and every other thread of
 *            the process kept off both logical processors of that core
 *   reserve  as isolate, but the main thread may run on either logical
 *            processor of its core: when an interrupt or another process takes
 *            one, it moves to the other instead of waiting
 *
 * The guest's main thread is the game's critical path, and with SMT a busy
 * thread on the other logical processor of its core takes a share of the
 * core for as long as they share: unpinned, a river fight ran 7.8 or ~10 ms
 * an ordinary frame by where the scheduler happened to put things (build/ab/
 * 2026-11-03-store-check). The trace's kind 26 records, per frame, how busy
 * the main thread's core's other logical processor was (PS3_FTRACE).
 *
 * The topology is the system's (GetSystemCpuSetInformation): which logical
 * processors share a core, never assumed from their numbering. The core for
 * isolate and reserve: DOTHEROES_CPU_CORE=<one of its logical processors>, else among the
 * cores of the highest efficiency class (a hybrid CPU's performance cores) and
 * then the highest scheduling class (the system's preferred cores, where it
 * ranks them), the last one -- not the core of logical processor 0 where
 * others remain, where much of the system's interrupt work lands. Process
 * group 0 only (up to 64 logical processors).
 *
 * percore is a hard affinity mask, as the A/B's affinity set from outside.
 * isolate is CPU sets: the process's default set without the chosen core
 * (threads made later, the driver's and the audio's included, take it too),
 * and the main thread's own set, with a hard affinity as well, that one
 * logical processor. reserve gives the main thread its core's CPU sets and
 * no affinity.
 */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

namespace {

struct Lp { ULONG id; int lp, core; int eff, sched; };

std::string list(uint64_t mask)
{
    std::string s;
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) {
            int j = i;
            while (j < 63 && (mask >> (j + 1) & 1)) j++;
            if (!s.empty()) s += ',';
            s += std::to_string(i);
            if (j > i) s += '-' + std::to_string(j);
            i = j;
        }
    return s;
}

}  // namespace

extern "C" void dotheroes_cpu_place(void)
{
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    std::vector<char> buf(len);
    if (!len || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len,
                                            GetCurrentProcess(), 0)) {
        fprintf(stderr, "[port] cpu: no CPU set information (error %lu); threads placed by Windows\n",
                GetLastError());
        return;
    }
    std::vector<Lp> lps;
    for (ULONG o = 0; o < len;) {
        auto* e = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + o);
        if (e->Type == CpuSetInformation && e->CpuSet.Group == 0)
            lps.push_back({ e->CpuSet.Id, e->CpuSet.LogicalProcessorIndex, e->CpuSet.CoreIndex,
                            e->CpuSet.EfficiencyClass, e->CpuSet.SchedulingClass });
        o += e->Size;
    }
    std::sort(lps.begin(), lps.end(), [](const Lp& a, const Lp& b) { return a.lp < b.lp; });
    // Cores in order of their first logical processor, each as a mask.
    std::vector<int> cores;
    std::vector<uint64_t> core_mask;
    for (const Lp& p : lps) {
        auto it = std::find(cores.begin(), cores.end(), p.core);
        if (it == cores.end()) { cores.push_back(p.core); core_mask.push_back(0); it = cores.end() - 1; }
        core_mask[it - cores.begin()] |= 1ull << p.lp;
    }
    std::string pairs;
    int most = 0, effs = 0, scheds = 0;
    for (size_t c = 0; c < cores.size(); c++) {
        most = std::max(most, __builtin_popcountll(core_mask[c]));
        if (c < 4 || c + 1 == cores.size()) pairs += (pairs.empty() ? "" : " ") + list(core_mask[c]);
        else if (c == 4) pairs += " ...";
    }
    for (const Lp& p : lps) { effs |= 1 << (p.eff & 7); scheds |= 1 << (p.sched & 31); }
    fprintf(stderr, "[port] cpu: %zu cores, %zu logical processors, at most %d a core (%s)%s%s\n",
            cores.size(), lps.size(), most, pairs.c_str(),
            __builtin_popcount(effs) > 1 ? ", efficiency classes differ" : "",
            __builtin_popcount(scheds) > 1 ? ", scheduling classes differ" : "");

    const char* how = getenv("DOTHEROES_CPU_PLACE");
    if (!how || !*how || !strcmp(how, "normal")) {
        fprintf(stderr, "[port] cpu: placement normal (Windows)\n");
        return;
    }
    if (!strcmp(how, "percore")) {
        uint64_t mask = 0;
        for (uint64_t m : core_mask) mask |= m & (~m + 1);   // each core's lowest
        const BOOL ok = SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask);
        fprintf(stderr, "[port] cpu: placement percore, the process on %s (0x%llx)%s\n", list(mask).c_str(),
                (unsigned long long)mask, ok ? "" : " -- FAILED");
        return;
    }
    const bool reserve = !strcmp(how, "reserve");
    if (strcmp(how, "isolate") && !reserve) {
        fprintf(stderr, "[port] cpu: DOTHEROES_CPU_PLACE=%s not known (normal, percore, isolate, reserve); placement normal\n", how);
        return;
    }
    // The core for the main thread.
    int pick = -1;
    if (const char* e = getenv("DOTHEROES_CPU_CORE"); e && *e) {
        const int want = atoi(e);
        for (size_t c = 0; c < cores.size(); c++)
            if (want >= 0 && want < 64 && core_mask[c] >> want & 1) pick = (int)c;
        if (pick < 0) fprintf(stderr, "[port] cpu: DOTHEROES_CPU_CORE=%s is no logical processor here\n", e);
    }
    if (pick < 0) {
        auto rank = [&](size_t c) {
            int eff = 0, sched = 0;
            for (const Lp& p : lps)
                if (core_mask[c] >> p.lp & 1) { eff = std::max(eff, p.eff); sched = std::max(sched, p.sched); }
            return std::make_pair(eff, sched);
        };
        std::pair<int, int> best = { -1, -1 };
        for (size_t c = 0; c < cores.size(); c++) best = std::max(best, rank(c));
        for (size_t c = 0; c < cores.size(); c++)
            if (rank(c) == best && !(core_mask[c] & 1)) pick = (int)c;
        if (pick < 0)
            for (size_t c = 0; c < cores.size(); c++)
                if (rank(c) == best) pick = (int)c;
    }
    if (pick < 0 || cores.size() < 2) {
        fprintf(stderr, "[port] cpu: %s needs two cores or more; placement normal\n", how);
        return;
    }
    const uint64_t mine = core_mask[pick];
    const int main_lp = __builtin_ctzll(mine);
    std::vector<ULONG> main_ids, others;
    uint64_t others_mask = 0;
    for (const Lp& p : lps) {
        if (reserve ? (mine >> p.lp & 1) : p.lp == main_lp) main_ids.push_back(p.id);
        if (!(mine >> p.lp & 1)) { others.push_back(p.id); others_mask |= 1ull << p.lp; }
    }
    const BOOL a = SetProcessDefaultCpuSets(GetCurrentProcess(), others.data(), (ULONG)others.size());
    const BOOL b = SetThreadSelectedCpuSets(GetCurrentThread(), main_ids.data(), (ULONG)main_ids.size());
    const BOOL c = reserve || SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << main_lp) != 0;
    fprintf(stderr, "[port] cpu: placement %s, the PPU main thread on %s (its core: %s), the other threads on %s%s\n",
            how, reserve ? list(mine).c_str() : std::to_string(main_lp).c_str(), list(mine).c_str(),
            list(others_mask).c_str(), a && b && c ? "" : " -- FAILED");
}
#else
extern "C" void dotheroes_cpu_place(void) {}
#endif
