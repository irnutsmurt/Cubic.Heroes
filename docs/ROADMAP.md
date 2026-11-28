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
- **0.2.0-beta.6**: the character library and the **Character Store**. The library imports the editor's heroes,
  character files and PS3 characters (with the player's own key), exports them, and installs them in the game. The
  store offers community characters from the official characters repository, among them the recovered 2010 Hall of
  Heroes collection. Also the album fix ([#10](https://github.com/irnutsmurt/Cubic.Heroes/issues/10)), the debris
  slowdown work ([#2](https://github.com/irnutsmurt/Cubic.Heroes/issues/2)), and a **simpler installation from the
  ISO** (v1.00): the game program's keys found by themselves -- beside the ISO, in any common form (a keys file, or a
  folder or ZIP of key files), or where they were found before -- or the program decrypted by the player's own RPCS3,
  if they have it. Cubic.Heroes still includes no keys.
- **0.2.0-beta.7**: the **official v1.01 update** (Atlus, 2010), optional: downloaded from Sony or taken from the
  player's own copy, checked byte for byte; the updated program supported alongside v1.00; games already updated on a
  PS3 or in an emulator recognised. v1.00 stays supported.
