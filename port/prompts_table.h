// M11.2g: the game's button prompts in its textures -- where they are (port/prompts_table.cpp) and the buttons.
#pragma once
#include <cstdint>
#include <vector>

namespace prompts {

// the game's buttons: 0-15 its pad bits (cellPad.h CELL_PAD_CTRL_*), then the art for a direction pad and the sticks
enum Button : uint8_t {
    B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE,
    B_CROSS, B_SQUARE, B_DPAD, B_LSTICK, B_RSTICK, B_COUNT
};

struct Cell { uint16_t x0, y0, x1, y1; uint8_t button; };
struct Tex { const char* package; const char* name; std::vector<Cell> cells; };
struct FontGlyph { uint16_t index; uint8_t cells; uint8_t button; };   // cells: 1, or 2 side by side

const std::vector<Tex>& table();
const std::vector<FontGlyph>& font_glyphs();
extern const char* const kFontPackages[];
extern const int kFontPackageCount;

}  // namespace prompts
