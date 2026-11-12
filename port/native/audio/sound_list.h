// The game's sound list, read from the player's own `sound.lua.eu` (docs/OPENAL_MIGRATION_PLAN.md section 1):
//   Sound = { BGM_FadeTime = <seconds>, List = { {"<file>", <volume>, <3D 0|1>, <min>, <max>}, ... } }
// one entry per sound id, in list order; `--` starts a comment (a commented-out entry is not in the list); an
// entry with an empty file name is a "no sound" id. On the player's disc: 324 entries -- music ids 0-41 (id 32
// empty), effects 42-322, id 323 empty. Parsed here independently of the game's own reader.
#pragma once
#include <string>
#include <vector>

namespace sound_list {

struct Entry {
    std::string file;      // as written in the list (the file's name in the list's folder), "" for no sound
    float volume = 0.0f;
    bool is3d = false;
    float min_distance = 0.0f, max_distance = 0.0f;
};

struct List {
    std::vector<Entry> entries;
    float bgm_fade_time = 0.0f;   // BGM_FadeTime (the list's own default music fade), 0 if absent
    bool has_fade_time = false;
};

// False with *error set when the text holds no list.
bool parse(const std::string& text, List* out, std::string* error);
bool load(const std::string& path, List* out, std::string* error);

}  // namespace sound_list
