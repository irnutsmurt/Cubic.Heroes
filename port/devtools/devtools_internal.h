/* M11.0: what the developer tools' parts share inside the game's program (not for the rest of the port). */
#pragma once

#include <string>
#include <vector>

namespace devtools {

struct MapInfo {
    std::string name;        /* as the game's map change takes it (Field_1, D_1_1_1) */
    std::string file;        /* its files' stem (Field_01, D_1_1_1) */
    int number = -1;         /* its place in the game's map list (map_manager.MS_*): the area the map arrives as */
    std::string group, location;
    std::vector<std::string> entries;   /* its arrival points, the default first */
};

/* the catalog's maps, once made (the `catalog` command makes it on the game's thread; never changed afterwards) */
const std::vector<MapInfo>& maps();
bool catalog_made();

/* a command run on the game's thread and its answer waited for (the in-game menu: the game is held, so the hold runs
 * it within milliseconds); false with why in `out` when refused or not answered in `ms` */
bool run_sync(const std::string& line, std::string& out, int ms = 3000);

}  // namespace devtools

/* the in-game menu's Developer pages (dev_menu.cpp), on the menu's thread */
struct DevRow { std::string label, value; };
struct DevView { std::string title; std::vector<DevRow> rows; std::string note; };
void dev_menu_reset();              /* the Developer page */
DevView dev_menu_view();            /* what the menu shows now */
int dev_menu_pick(int row);         /* 0: stay (the page may have changed); 1: close the menu (the action needs frames) */
bool dev_menu_back();               /* false: back out of the Developer pages */
int dev_menu_focus();               /* the row focused on the page shown (kept when a page is left and come back to) */
