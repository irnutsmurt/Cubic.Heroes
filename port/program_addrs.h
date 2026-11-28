/* The game's addresses port/ writes down, for every program of the game this project builds (beta.6: the disc's
 * v1.00 and the official v1.01 update's).
 *
 * port/ writes each one as v1.00's: PA(0x02CF3570) is the frame counter, PA(0x007F3450) lua_pcall. A build of v1.00
 * takes them as they are. A build of another program (DOTHEROES_PROGRAM_ADDRS: config/programs/<program>/port_addrs.inc,
 * made by tools/program_remap.py from the two programs) takes that program's: each pair {v1.00, this program}; an
 * address the table lacks stops the build -- the tool's report names it, and it is decided by hand there
 * (remap_manual.json), never guessed. The lifted functions port/ calls by name (func_XXXXXXXX) follow the same way
 * (config/programs/<program>/port_aliases.h, forced in by the build).
 *
 * Not written this way: the numbers the lifted code hands port/ to dispatch on -- an observer's id, an audio or
 * movie hook's host -- which stay v1.00's in every program (tools/program_remap.py, ppu_lifter's observer "id"). */
#pragma once
#include <cstdint>
#include <type_traits>

namespace program_addrs {
#ifdef DOTHEROES_PROGRAM_ADDRS
struct Pair { uint32_t v100, here; };
inline constexpr Pair kTable[] = {
#include DOTHEROES_PROGRAM_ADDRS
    { 0u, 0u }   // the end (0 is never an address of the game's here)
};
constexpr uint32_t pa(uint32_t a)
{
    for (const Pair& p : kTable)
        if (p.v100 == a && a) return p.here;
    throw "an address of v1.00 that this program's table (config/programs/<program>/port_addrs.inc) does not map";
}
#else
constexpr uint32_t pa(uint32_t a) { return a; }
#endif
}  // namespace program_addrs

/* v1.00's address -> this program's, at compile time (a constant expression: case labels, constexpr) -- in the
 * sources the player's machine compiles with the program (tools/zig_build.py PORT_SOURCES, main.cpp, ...) */
#define PA(a) (std::integral_constant<uint32_t, program_addrs::pa(a)>::value)

/* The same at run time, for port/'s prebuilt library (dotheroes_native: built once, linked into every program's
 * build): the table is the one main.cpp was compiled with, for the program being built. Read it once (a
 * namespace-scope `static const`, or a function's `static const`), not on a hot path. An address the table lacks
 * stops the game with a message: the library and the program's table do not belong together. */
extern "C" uint32_t dotheroes_program_addr(uint32_t v100);   /* main.cpp */
#define PA_RT(a) (dotheroes_program_addr(a))
