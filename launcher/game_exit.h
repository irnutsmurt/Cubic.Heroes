// The game's own exit statuses that mean something to a player, said in plain words: the launcher's window (when the
// game it started ends) and the command line (`CubicHeroes --play`) show the same text. docs/CLI.md lists them.
#pragma once

#define CUBIC_GAME_EXIT_STARTUP 70   // the graphics' start never finished (port/main.cpp, startup_watch)

namespace launcher {

// a title and an explanation for a status, or nullptr for the statuses that need none (0, a player's own end)
struct GameExitNote {
    const char* title;
    const char* text;
};

inline const GameExitNote* game_exit_note(int status)
{
    static const GameExitNote startup = {
        "The graphics didn't start",
        "The game stopped because its graphics didn't finish starting in time: the graphics driver or the window "
        "system didn't respond. The game's log (logs/game.log) names the step that stalled. Updating the graphics "
        "driver, closing overlay or recording tools, or restarting the computer usually helps; then play again."};
    return status == CUBIC_GAME_EXIT_STARTUP ? &startup : nullptr;
}

}   // namespace launcher
