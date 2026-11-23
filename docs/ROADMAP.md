# Roadmap

Done:

- **The first native Linux release** -- an **AppImage** and a **tarball** for x86-64, in 0.2.0-beta.3
  ([LINUX_PACKAGING.md](LINUX_PACKAGING.md)).
- **Updates installed by the launcher itself** on Windows and Linux, with rollback of the launcher and recovery of an
  interrupted update, and Linux's update check fixed -- in 0.2.0-beta.4.

Planned, not yet available:

- **0.2.0-beta.5**: more disc formats (an ISO rebuilt from the disc's folder) with every game file checked against the
  verified game's manifest, and a hardened extraction; the slowdown when cutting grass and other debris-heavy scenes
  ([#2](https://github.com/irnutsmurt/Cubic.Heroes/issues/2)), measured on Windows and Linux before any change; a
  stronger release-signing design (an offline root key with delegated release keys); optional RecompLauncher support
  (launching and settings), alongside the standalone launcher.
- **Completing the gameplay validation** -- the rows still open in the matrix, and what testers report.
- **Steam Deck** testing and compatibility.
- **Further Linux compatibility** -- NVIDIA and Intel GPUs, more distributions and desktops, and the AMD rendering
  differences in the known issues.
- **Optional hardware-accelerated ray tracing** (RTX and other GPUs with ray tracing).
- **HDR** output (being considered).
- **Character import, export and community sharing** -- the editor's heroes as files, and a shared library.
