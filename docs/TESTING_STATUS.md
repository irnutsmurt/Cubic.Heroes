# Testing status

What has been tested, and on what. The [README](../README.md#known-issues) lists the known issues.

**Windows**: the beta has been validated area by area on Windows 11 with an NVIDIA GPU: every reachable map visited
(166 of 167), every boss fought to its defeat (and won by the game's own mechanics where they need one), the ending
and credits, saving and loading, frame-rate independence of movement at 30, 60, 144 and 240 fps for every enemy type
and the bosses, the movies, every event the game places, the installation, updating by hand from the previous beta,
and rollback -- with automated scenarios, the developer tools and comparisons against RPCS3.

**Linux**: played and qualified on one machine, a Ryzen 5 5500U laptop with Radeon graphics (Manjaro Linux, kernel
6.18, Mesa 26.2 RADV), under KDE Plasma on Wayland and on X11 -- the installation from the disc, the shaders, play,
saves, both packages (the AppImage and the tarball), and the game started through Steam (as a non-Steam game, with the
Steam overlay and a controller) and through Heroic's Flatpak; Lutris on an earlier package of the same day. On Debian
11, Ubuntu 20.04, Ubuntu 24.04 and Fedora 41 the packages have only been started in clean containers (the launcher,
its command line, the worker and the game's own start-up checks): the game hasn't been played there.

The full matrix, with what is still `partial` or `todo`, is [M11_VALIDATION.md](M11_VALIDATION.md); the release
checks are [RELEASE_QUALIFICATION.md](RELEASE_QUALIFICATION.md). **Not every part of the game has been tested**, and
Windows 10, AMD and Intel GPUs on Windows, NVIDIA and Intel GPUs on Linux, other distributions and handhelds (the
Steam Deck among them) have not been tested at all -- reports are very welcome.
