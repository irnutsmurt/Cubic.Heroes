// The UI's font and prompt images (port/assets/ui, embedded by port/cmake/embed_files.cmake) handed to the
// runtime's save-data screen (ps3recomp libs/video/osd_saves.c). Redistributable files of the port's own:
// DejaVu Sans (LICENSE-DejaVuSans.txt) and Kenney's input prompts (CC0, LICENSE-Kenney.txt). The PlayStation
// and keyboard styles have no images: the screen draws Cross and Circle, and key caps with the key's name,
// from shapes (sharp at any size; Kenney's keyboard keys are unreadable small).
#include <cstddef>
#include <cstring>

extern "C" {
struct embedded_file { const char* name; const unsigned char* data; size_t size; };
extern const struct embedded_file embedded_files[];
extern const unsigned embedded_files_count;
void ps3_ui_set_font(const void* ttf, size_t size);
void ps3_ui_set_prompt_png(int style, int button, const void* png, size_t size);
}

namespace {
// ps3emu/osd.h PS3_UI_PROMPTS_*: 0 keyboard, 1 PlayStation, 2 Xbox, 3 Switch; button 0 confirm, 1 back
struct Prompt { const char* file; int style, button; };
const Prompt kPrompts[] = {
    { "prompt_xbox_confirm.png", 2, 0 },     { "prompt_xbox_back.png", 2, 1 },
    { "prompt_switch_confirm.png", 3, 0 },   { "prompt_switch_back.png", 3, 1 },
};
}  // namespace

extern "C" void dotheroes_ui_assets_init(void)
{
    for (unsigned i = 0; i < embedded_files_count; i++) {
        const embedded_file& f = embedded_files[i];
        if (!std::strcmp(f.name, "DejaVuSans.ttf")) ps3_ui_set_font(f.data, f.size);
        for (const Prompt& p : kPrompts)
            if (!std::strcmp(f.name, p.file)) ps3_ui_set_prompt_png(p.style, p.button, f.data, f.size);
    }
}
