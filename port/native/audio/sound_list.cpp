// The game's sound list (sound_list.h).
#include "sound_list.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wav_pcm.h"

namespace sound_list {
namespace {

// The text with every `--` comment removed (to the end of its line); strings are kept as they are.
std::string strip_comments(const std::string& t)
{
    std::string out;
    out.reserve(t.size());
    bool in_string = false;
    for (size_t i = 0; i < t.size(); i++) {
        const char c = t[i];
        if (in_string) {
            out += c;
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out += c;
        } else if (c == '-' && i + 1 < t.size() && t[i + 1] == '-') {
            while (i < t.size() && t[i] != '\n') i++;
            out += '\n';
        } else {
            out += c;
        }
    }
    return out;
}

void skip_space(const std::string& t, size_t& i)
{
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
}

bool number(const std::string& t, size_t& i, float* out)
{
    skip_space(t, i);
    const char* start = t.c_str() + i;
    char* end = nullptr;
    const double v = strtod(start, &end);
    if (end == start) return false;
    i += (size_t)(end - start);
    *out = (float)v;
    return true;
}

bool comma(const std::string& t, size_t& i)
{
    skip_space(t, i);
    if (i >= t.size() || t[i] != ',') return false;
    i++;
    return true;
}

// One {"file", volume, 3d, min, max} at t[i] == '{'; false (i unchanged) when it is not one.
bool entry(const std::string& t, size_t& i, Entry* e)
{
    size_t k = i + 1;
    skip_space(t, k);
    if (k >= t.size() || t[k] != '"') return false;
    const size_t close = t.find('"', k + 1);
    if (close == std::string::npos) return false;
    e->file = t.substr(k + 1, close - k - 1);
    k = close + 1;
    float is3d = 0;
    if (!comma(t, k) || !number(t, k, &e->volume) || !comma(t, k) || !number(t, k, &is3d) || !comma(t, k) ||
        !number(t, k, &e->min_distance) || !comma(t, k) || !number(t, k, &e->max_distance))
        return false;
    skip_space(t, k);
    if (k < t.size() && t[k] == ',') k++;   // a trailing comma inside the braces
    skip_space(t, k);
    if (k >= t.size() || t[k] != '}') return false;
    e->is3d = is3d != 0;
    i = k + 1;
    return true;
}

}  // namespace

bool parse(const std::string& text, List* out, std::string* error)
{
    *out = List{};
    const std::string t = strip_comments(text);
    const size_t fade = t.find("BGM_FadeTime");
    if (fade != std::string::npos) {
        size_t i = t.find('=', fade);
        if (i != std::string::npos) {
            i++;
            out->has_fade_time = number(t, i, &out->bgm_fade_time);
        }
    }
    const size_t list = t.find("List");
    if (list == std::string::npos) {
        if (error) *error = "no List in the sound list";
        return false;
    }
    for (size_t i = t.find('{', list); i != std::string::npos && i < t.size(); i = t.find('{', i + 1)) {
        Entry e;
        size_t at = i;
        if (entry(t, at, &e)) {
            out->entries.push_back(e);
            i = at - 1;
        }
    }
    if (out->entries.empty()) {
        if (error) *error = "no entries in the sound list";
        return false;
    }
    return true;
}

bool load(const std::string& path, List* out, std::string* error)
{
    FILE* f = wav_pcm::open_utf8(path);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return parse(text, out, error);
}

}  // namespace sound_list
