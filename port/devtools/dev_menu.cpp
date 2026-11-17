/* M11.0b (docs/M11_PLAN.md section 3.6): the in-game menu's Developer pages -- what they list and what choosing a row
 * does. The menu (port/overlay.cpp) draws them in its own look and runs the input; every action is a command of the
 * developer tools (devtools.cpp), run on the game's main thread while the menu holds the game. */
#include "devtools.h"
#include "devtools_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

enum Kind { V_ROOT, V_WARP, V_GROUP, V_ENTRIES, V_STATUS };
struct Level { Kind kind; std::string arg; int focus; };
std::vector<Level> g_stack;
std::string g_note;

/* the Warp page's groups, in order: the fields, then each D_<n> by its number, then the rest */
std::vector<std::string> groups()
{
    std::vector<std::string> fields, other;
    std::map<int, std::string> ds;
    for (const devtools::MapInfo& m : devtools::maps()) {
        if (m.group == "Fields") { if (fields.empty()) fields.push_back(m.group); }
        else if (m.group.rfind("D_", 0) == 0) ds[atoi(m.group.c_str() + 2)] = m.group;
        else if (std::find(other.begin(), other.end(), m.group) == other.end()) other.push_back(m.group);
    }
    std::vector<std::string> g = fields;
    for (auto& kv : ds) g.push_back(kv.second);
    g.insert(g.end(), other.begin(), other.end());
    return g;
}

std::vector<const devtools::MapInfo*> in_group(const std::string& group)
{
    std::vector<const devtools::MapInfo*> v;
    for (const devtools::MapInfo& m : devtools::maps())
        if (m.group == group) v.push_back(&m);
    return v;
}

const devtools::MapInfo* map_named(const std::string& name)
{
    for (const devtools::MapInfo& m : devtools::maps())
        if (m.name == name) return &m;
    return nullptr;
}

/* the group's label: its name and the place most of its maps are in (the game's own map parameters) */
std::string group_label(const std::string& group)
{
    std::map<std::string, int> count;
    for (const devtools::MapInfo* m : in_group(group))
        if (!m->location.empty()) count[m->location]++;
    std::string best;
    int n = 0;
    for (auto& kv : count) if (kv.second > n) { best = kv.first; n = kv.second; }
    if (group == "Fields") return "Fields";
    return best.empty() ? group : group + "   " + best;
}

bool ensure_catalog()
{
    if (devtools::catalog_made()) return true;
    std::string out;
    if (!devtools::run_sync("catalog", out, 8000)) { g_note = "The map list could not be made: " + out; return false; }
    return devtools::catalog_made();
}

/* `status`'s flat JSON object as rows */
std::vector<DevRow> status_rows()
{
    std::string out;
    if (!devtools::run_sync("status", out)) return { { "Status", "not available: " + out } };
    static const std::map<std::string, std::string> kLabel = {
        { "area", "Map number" }, { "frames", "Game frames" }, { "hud_life", "Life (half hearts)" },
        { "money", "Money" }, { "event_state", "Event state (0: none)" }, { "held", "Paused by this menu" },
        { "lua_state", "Script state" }, { "lua_level", "Script call level" }, { "event_object", "Event object" },
        { "manager_event", "Game manager's event" }, { "event_same", "Event objects agree" }, { "hold_runs", "Commands run paused" } };
    std::vector<DevRow> rows;
    size_t i = 0;
    while ((i = out.find('"', i)) != std::string::npos) {
        const size_t k = out.find('"', i + 1);
        if (k == std::string::npos || k + 1 >= out.size() || out[k + 1] != ':') break;
        const std::string key = out.substr(i + 1, k - i - 1);
        size_t v = k + 2, e;
        std::string val;
        if (v < out.size() && out[v] == '"') { e = out.find('"', v + 1); val = out.substr(v + 1, e - v - 1); e++; }
        else { e = out.find_first_of(",}", v); val = out.substr(v, e - v); }
        if (key == "area") {   /* the map itself, by its number (the catalog) */
            for (const devtools::MapInfo& m : devtools::maps())
                if (m.number == atoi(val.c_str())) {
                    rows.push_back({ "Map", m.name + (m.location.empty() ? "" : "   " + m.location) });
                    break;
                }
        }
        auto l = kLabel.find(key);
        rows.push_back({ l != kLabel.end() ? l->second : key, val });
        i = e;
    }
    return rows;
}

}  // namespace

void dev_menu_reset()
{
    g_stack.assign(1, Level{ V_ROOT, "", 0 });
    g_note.clear();
}

int dev_menu_focus() { return g_stack.empty() ? 0 : g_stack.back().focus; }

DevView dev_menu_view()
{
    if (g_stack.empty()) dev_menu_reset();
    DevView v;
    const Level& L = g_stack.back();
    switch (L.kind) {
    case V_ROOT:
        v.title = "DEVELOPER";
        v.rows = { { "Warp", "Any map, floor or entry" }, { "Status", "The game's state now" } };
        v.note = "Test saves only. Actions apply to the paused game.";
        break;
    case V_WARP:
        v.title = "WARP";
        if (ensure_catalog())
            for (const std::string& g : groups())
            {
                const size_t k = in_group(g).size();
                v.rows.push_back({ group_label(g), std::to_string(k) + (k == 1 ? " map" : " maps") });
            }
        break;
    case V_GROUP:
        v.title = "WARP: " + L.arg;
        for (const devtools::MapInfo* m : in_group(L.arg))
            v.rows.push_back({ m->name + (m->location.empty() ? "" : "   " + m->location),
                               m->entries.size() > 1 ? std::to_string(m->entries.size()) + " entries" : "" });
        break;
    case V_ENTRIES: {
        v.title = "WARP: " + L.arg;
        const devtools::MapInfo* m = map_named(L.arg);
        if (m)
            for (size_t i = 0; i < m->entries.size(); i++) v.rows.push_back({ m->entries[i], i ? "" : "Default" });
        break;
    }
    case V_STATUS:
        v.title = "STATUS";
        v.rows = status_rows();
        v.note = "Choose any row to read again.";
        break;
    }
    if (!g_note.empty()) v.note = g_note;
    return v;
}

int dev_menu_pick(int row)
{
    if (g_stack.empty()) dev_menu_reset();
    Level& L = g_stack.back();
    L.focus = row;
    g_note.clear();
    switch (L.kind) {
    case V_ROOT:
        if (row == 0) g_stack.push_back({ V_WARP, "", 0 });
        if (row == 1) g_stack.push_back({ V_STATUS, "", 0 });
        return 0;
    case V_WARP: {
        const std::vector<std::string> g = groups();
        if (row >= 0 && row < (int)g.size()) g_stack.push_back({ V_GROUP, g[row], 0 });
        return 0;
    }
    case V_GROUP: {
        const auto ms = in_group(L.arg);
        if (row < 0 || row >= (int)ms.size()) return 0;
        if (ms[row]->entries.size() > 1) { g_stack.push_back({ V_ENTRIES, ms[row]->name, 0 }); return 0; }
        devtools_submit(("warp " + ms[row]->name).c_str(), nullptr, nullptr);
        return 1;
    }
    case V_ENTRIES: {
        const devtools::MapInfo* m = map_named(L.arg);
        if (!m || row < 0 || row >= (int)m->entries.size()) return 0;
        devtools_submit(("warp " + m->name + " " + m->entries[row]).c_str(), nullptr, nullptr);
        return 1;
    }
    case V_STATUS:
        return 0;
    }
    return 0;
}

bool dev_menu_back()
{
    g_note.clear();
    if (g_stack.size() <= 1) return false;
    g_stack.pop_back();
    return true;
}
