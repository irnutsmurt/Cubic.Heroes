// M11.2g: where the game's English build shows PlayStation buttons in its UI textures (docs/M11_PLAN.md section 35).
// Our own annotations, from an audit of the textures' pixels (build/m11 prompts, not part of the repository): for each
// texture, its package (the .ps3.tpk under the game's data folder, without the extension) and its name in it, then
// each button's cell -- x0, y0, x1, y1 (the end exclusive) in the texture's pixels, and the button it shows. No game
// data: the player's own files are read at start to recognise the textures (port/prompts.cpp).
#include "prompts_table.h"

namespace prompts {

const std::vector<Tex>& table()
{
    static const std::vector<Tex> t = {
        // the HUD: the key guide (START, L1 MAP, R1 GUARD, Triangle MENU, Square DASH), the item cycle, the windows
        { "UserInterface/Status/Status_eng", "status_eng",
          { { 18, 72, 53, 98, B_START }, { 61, 75, 98, 95, B_L1 }, { 106, 75, 143, 95, B_R1 }, { 151, 72, 175, 96, B_TRIANGLE },
            { 183, 72, 207, 96, B_SQUARE }, { 157, 151, 186, 167, B_L2 }, { 189, 151, 218, 167, B_R2 },
            { 1, 202, 25, 226, B_CROSS }, { 79, 202, 103, 226, B_CIRCLE } } },
        { "UserInterface/Pause/Pause_eng", "pause_eng", { { 17, 46, 41, 70, B_CROSS }, { 47, 46, 71, 70, B_CIRCLE } } },
        { "UserInterface/Option/Option_eng", "option",
          { { 18, 46, 42, 70, B_SQUARE }, { 48, 46, 72, 70, B_CROSS }, { 78, 46, 102, 70, B_CIRCLE } } },
        // the item and equipment menus, the orb menu
        { "UserInterface/Item/selectmenu_eng", "selectmenu_eng",
          { { 16, 47, 53, 67, B_L1 }, { 61, 47, 98, 67, B_R1 }, { 102, 45, 126, 69, B_SQUARE }, { 130, 45, 154, 69, B_CROSS },
            { 158, 45, 182, 69, B_CIRCLE } } },
        { "UserInterface/Item/aubemenu_eng", "aubemenu_eng",
          { { 16, 9, 53, 29, B_L1 }, { 61, 9, 98, 29, B_R1 }, { 388, 6, 412, 30, B_CIRCLE } } },
        { "UserInterface/ScreenShotCamera/SScamera_eng", "sscamera",
          { { 18, 35, 43, 57, B_LSTICK }, { 49, 35, 74, 57, B_RSTICK }, { 80, 36, 117, 56, B_L1 }, { 125, 36, 162, 56, B_R1 },
            { 168, 34, 192, 58, B_CROSS }, { 198, 34, 222, 58, B_CIRCLE } } },
        { "UserInterface/Skip/Skip", "skip", { { 17, 5, 52, 31, B_START } } },
        // the action balloons over the hero (CHECK, TALK, OPEN)
        { "Balloon/balloon_eng", "c_etc_03a_eng", { { 128, 9, 152, 33, B_CROSS } } },
        { "Balloon/balloon_eng", "c_etc_03b_eng", { { 117, 9, 141, 33, B_CROSS } } },
        { "Balloon/balloon_eng", "c_etc_03c_eng", { { 117, 9, 141, 33, B_CROSS } } },
        // the title's character choice, the difficulty, the new character
        { "CharSelect/SelectCharacter", "selectcharacter",
          { { 19, 93, 43, 117, B_CROSS }, { 174, 93, 198, 117, B_CIRCLE }, { 544, 187, 575, 204, B_L1 },
            { 596, 187, 627, 204, B_R1 }, { 455, 96, 492, 116, B_L1 }, { 500, 96, 537, 116, B_R1 },
            { 566, 93, 590, 117, B_TRIANGLE } } },
        { "Difficulty/Difficulty", "difficulty", { { 242, 95, 266, 119, B_CROSS }, { 399, 95, 423, 119, B_CIRCLE } } },
        { "Editor/NewCharacter/NewCharacter", "newcharacter", { { 17, 84, 41, 108, B_CROSS }, { 174, 84, 198, 108, B_CIRCLE } } },
        // the character editor
        { "Editor/Editor_eng", "editor_eng",
          { { 640, 150, 664, 174, B_CIRCLE }, { 640, 183, 664, 207, B_CROSS }, { 630, 217, 665, 243, B_START } } },
        { "Editor/Category/Category", "category", { { 1119, 745, 1143, 769, B_CIRCLE }, { 1119, 778, 1143, 802, B_CROSS } } },
        { "Editor/CharEditor/CharEditor_eng", "chareditor_eng",
          { { 26, 29, 63, 49, B_L1 }, { 71, 29, 108, 49, B_R1 }, { 83, 58, 108, 83, B_L3 }, { 83, 89, 108, 114, B_R3 },
            { 65, 122, 108, 148, B_SELECT }, { 83, 156, 108, 178, B_LSTICK }, { 83, 188, 108, 210, B_RSTICK },
            { 17, 222, 54, 242, B_R2 }, { 78, 216, 108, 247, B_DPAD }, { 78, 249, 108, 280, B_DPAD },
            { 78, 282, 108, 313, B_DPAD }, { 30, 252, 54, 276, B_CROSS }, { 84, 384, 108, 408, B_CROSS },
            { 84, 318, 108, 342, B_SQUARE }, { 84, 351, 108, 375, B_CIRCLE }, { 84, 417, 108, 441, B_TRIANGLE },
            { 74, 451, 109, 477, B_START }, { 423, 20, 453, 51, B_DPAD }, { 429, 56, 453, 80, B_CIRCLE },
            { 429, 89, 453, 113, B_TRIANGLE }, { 429, 122, 453, 146, B_CROSS }, { 419, 156, 454, 182, B_START },
            { 811, 18, 848, 38, B_L1 }, { 1001, 18, 1038, 38, B_R1 } } },
        { "NameEntry/NameEntry", "nameentry",
          { { 154, 9, 191, 29, B_L1 }, { 199, 9, 236, 29, B_R1 }, { 413, 6, 437, 30, B_CROSS }, { 570, 6, 594, 30, B_CIRCLE } } },
        // the albums (loading pictures, enemies)
        { "Load/LoadingAlbum/LoadingAlbum", "loadingalbum",
          { { 190, 12, 227, 32, B_L1 }, { 235, 12, 272, 32, B_R1 }, { 446, 6, 478, 38, B_DPAD }, { 652, 9, 676, 33, B_CROSS },
            { 807, 9, 831, 33, B_CIRCLE }, { 196, 89, 227, 106, B_L1 }, { 248, 89, 279, 106, B_R1 } } },
        { "UserInterface/EnemyAlbum/EnemyAlbum", "enemyalbum",
          { { 190, 12, 227, 32, B_L1 }, { 235, 12, 272, 32, B_R1 }, { 446, 6, 478, 38, B_DPAD }, { 652, 9, 676, 33, B_CROSS },
            { 807, 9, 831, 33, B_CIRCLE }, { 196, 89, 227, 106, B_L1 }, { 248, 89, 279, 106, B_R1 } } },
        { "UserInterface/EnemyView/EnemyView_eng", "enemyalbumdetails_eng",
          { { 17, 10, 42, 32, B_LSTICK }, { 285, 10, 310, 32, B_RSTICK }, { 487, 8, 511, 32, B_SQUARE },
            { 688, 8, 712, 32, B_CIRCLE }, { 428, 142, 459, 159, B_L1 }, { 477, 142, 508, 159, B_R1 } } },
        // the shops and the blacksmith
        { "Store/Store_eng", "store_eng", { { 18, 45, 42, 69, B_CROSS }, { 46, 45, 70, 69, B_CIRCLE } } },
        { "Store/GeneralStore/GeneralStore", "generalstore", { { 565, 96, 589, 120, B_CROSS }, { 720, 96, 744, 120, B_CIRCLE } } },
        { "Store/MedalKing/MedalKing", "medalking", { { 564, 96, 588, 120, B_CROSS }, { 721, 96, 745, 120, B_CIRCLE } } },
        { "Store/Blacksmith/Blacksmith", "blacksmith", { { 300, 96, 324, 120, B_CROSS }, { 457, 96, 481, 120, B_CIRCLE } } },
        // the minigames
        { "MiniGame/MiniGame_eng", "mini_game_info_eng",
          { { 19, 49, 56, 69, B_R1 }, { 160, 47, 184, 71, B_SQUARE }, { 298, 47, 322, 71, B_TRIANGLE },
            { 418, 47, 461, 73, B_SELECT }, { 557, 47, 581, 71, B_CROSS }, { 19, 87, 56, 107, B_R1 },
            { 182, 85, 225, 111, B_SELECT }, { 19, 123, 43, 147, B_SQUARE }, { 147, 123, 190, 149, B_SELECT } } },
        { "MiniGame/MiniGame_eng", "block_info_eng", { { 18, 273, 42, 297, B_SQUARE } } },
        { "MiniGame/MiniGame_eng", "block_tower_menu_eng", { { 17, 335, 41, 359, B_CROSS }, { 197, 335, 221, 359, B_CIRCLE } } },
        { "MiniGame/MiniGame_eng", "mini_window_eng",
          { { 2, 152, 31, 168, B_L2 }, { 34, 152, 63, 168, B_R2 }, { 75, 206, 99, 230, B_CIRCLE } } },
        { "MiniGame/MiniGame_eng", "tutorial_eng", { { 640, 7, 664, 31, B_CROSS }, { 790, 7, 814, 31, B_CIRCLE } } },
        // the maps (the dungeons' window is the same picture in each)
        { "UserInterface/radar/dungeonMap/01/dungeonMap01_eng", "dm_01_window_eng", { { 267, 328, 291, 352, B_CIRCLE } } },
        { "UserInterface/radar/dungeonMap/02/dungeonMap02_eng", "dm_01_window_eng", { { 267, 328, 291, 352, B_CIRCLE } } },
        { "UserInterface/radar/dungeonMap/03/dungeonMap03_01_eng", "dm_01_window_eng", { { 267, 328, 291, 352, B_CIRCLE } } },
        { "UserInterface/radar/dungeonMap/04/dungeonMap04_01_eng", "dm_01_window_eng", { { 267, 328, 291, 352, B_CIRCLE } } },
        { "UserInterface/radar/dungeonMap/05/dungeonMap05_01_eng", "dm_01_window_eng", { { 267, 328, 291, 352, B_CIRCLE } } },
        { "UserInterface/radar/dungeonMap/06/dungeonMap06_01_eng", "dm_01_window_eng", { { 267, 328, 291, 352, B_CIRCLE } } },
        { "UserInterface/radar/dungeonMap/07/dungeonMap07_01_eng", "dm_01_window_eng", { { 267, 328, 291, 352, B_CIRCLE } } },
        { "UserInterface/radar/fieldMap/fieldMap", "mapmenu", { { 18, 49, 42, 73, B_CROSS }, { 183, 49, 207, 73, B_CIRCLE } } },
    };
    return t;
}

// The fonts' button glyphs: their cells (32x32, 16 a row, by the index of the character in Font.bin; a wide glyph two
// cells side by side). Font_eng is the menus', Font_b and Font_c the messages' (FONT A and B).
const std::vector<FontGlyph>& font_glyphs()
{
    static const std::vector<FontGlyph> g = {
        { 248, 1, B_CIRCLE }, { 249, 1, B_CROSS }, { 250, 1, B_TRIANGLE }, { 251, 1, B_SQUARE },
        { 252, 1, B_R1 }, { 253, 1, B_L1 }, { 254, 1, B_SELECT }, { 255, 1, B_START },
        { 647, 2, B_R1 }, { 649, 2, B_L1 }, { 665, 2, B_SELECT }, { 667, 2, B_START }, { 678, 2, B_R2 }, { 680, 2, B_L2 },
    };
    return g;
}
const char* const kFontPackages[] = { "UserInterface/Font/Font_eng", "UserInterface/Font/Font_b", "UserInterface/Font/Font_c" };
const int kFontPackageCount = 3;

}  // namespace prompts
