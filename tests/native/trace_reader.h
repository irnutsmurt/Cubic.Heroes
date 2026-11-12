// A reader for the audio traces of port/audio_hooks.cpp (DOTHEROES_AUDIO_TRACE; tools/audio_trace.py documents
// the format): one call a line, `<us> <frame> <thread> <name> [this=<hex>] <arguments> -> <r3 hex> [out=...]`.
// A call's line is written when it returns, so a sound-manager call follows the FMOD calls it made; `us` is the
// call's start. The replay (sound_replay.cpp) orders the lines by start.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace trace_reader {

struct Call {
    uint64_t us = 0;
    uint32_t frame = 0;
    int thread = 0;
    std::string name;
    uint32_t self = 0;                  // this=, 0 when absent
    std::vector<std::string> args;      // as written: ints, hex words, floats, "(x,y,z)", "null", "\"str\""
    uint32_t r3 = 0;
    std::vector<std::string> outs;      // out=... values
    size_t line = 0;
};

inline bool vec3(const std::string& s, float out[3])
{
    return sscanf(s.c_str(), "(%f,%f,%f)", &out[0], &out[1], &out[2]) == 3;
}

inline bool parse_line(const std::string& raw, Call* c)
{
    if (raw.empty() || raw[0] == '#') return false;
    const size_t arrow = raw.rfind(" -> ");
    if (arrow == std::string::npos) return false;
    char name[96];
    unsigned long long us = 0;
    unsigned frame = 0;
    int thread = 0, n = 0;
    if (sscanf(raw.c_str(), "%llu %u %d %95s%n", &us, &frame, &thread, name, &n) < 4) return false;
    c->us = us;
    c->frame = frame;
    c->thread = thread;
    c->name = name;
    c->self = 0;
    c->args.clear();
    c->outs.clear();
    // arguments: tokens up to the arrow, a parenthesised vector or a quoted string as one
    const std::string mid = raw.substr((size_t)n, arrow - (size_t)n);
    size_t i = 0;
    while (i < mid.size()) {
        while (i < mid.size() && mid[i] == ' ') i++;
        if (i >= mid.size()) break;
        size_t j = i;
        if (mid[i] == '(') j = mid.find(')', i) + 1;
        else if (mid[i] == '"') j = mid.find('"', i + 1) + 1;
        else while (j < mid.size() && mid[j] != ' ') j++;
        if (j == 0 || j > mid.size()) j = mid.size();
        std::string tok = mid.substr(i, j - i);
        if (tok.rfind("this=", 0) == 0) c->self = (uint32_t)strtoul(tok.c_str() + 5, nullptr, 16);
        else c->args.push_back(tok);
        i = j;
    }
    const std::string tail = raw.substr(arrow + 4);
    c->r3 = (uint32_t)strtoul(tail.c_str(), nullptr, 16);
    for (size_t k = tail.find(" out="); k != std::string::npos; k = tail.find(" out=", k + 1)) {
        size_t e = tail.find(" out=", k + 1);
        c->outs.push_back(tail.substr(k + 5, e == std::string::npos ? std::string::npos : e - k - 5));
    }
    return true;
}

// Every call of the file, ordered by start time (a stable sort: ties keep the file's order).
inline std::vector<Call> read(const std::string& path, bool sound_manager_only = false)
{
    std::vector<Call> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string line;
    char buf[4096];
    size_t no = 0;
    while (fgets(buf, sizeof buf, f)) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        no++;
        Call c;
        if (!parse_line(line, &c)) continue;
        if (sound_manager_only && c.name.rfind("SM.", 0) != 0) continue;
        c.line = no;
        out.push_back(std::move(c));
    }
    fclose(f);
    std::stable_sort(out.begin(), out.end(), [](const Call& a, const Call& b) { return a.us < b.us; });
    return out;
}

}  // namespace trace_reader
