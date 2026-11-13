// The game's gzip header walk (func_0052EE34, with func_0052EFF8 and func_0052EDF8 for the sizes), as
// inflate_direct.cpp takes a map pack's header: a pure function of the image's bytes, so that every branch can be
// checked without the game (tests/native/gz_walk_check.cpp). Read from the player's lift; no game code.
//
// The game checks the magic (1F 8B) and the method (8), then takes the flags in the order below:
//   FHCRC (0x02)     refused: the walk returns a null data pointer, which its caller (func_005CF228) passes on
//                    unchecked -- on a PS3 the SPU task would then read from address 0 and fault; here the pack is
//                    damaged (inflate_direct.cpp writes nothing and names it)
//   FEXTRA (0x04)    the start moves on by XLEN (the two bytes at 10) -- without the length field's own two bytes,
//                    the game's arithmetic, kept: the data then starts two bytes early, as the game reads it
//   FNAME (0x08)     the start moves past the name's terminating zero
//   FCOMMENT (0x10)  the same past the comment (the game prints a message; nothing else changes)
//   0x20             refused
//   FTEXT (0x01), 0x40 and 0x80 are ignored (0x40 and 0x80 print a message only).
// The deflate data is the rest less the 8-byte trailer (CRC-32, then ISIZE, little-endian). What the game's walk
// would run past the image with (a name or comment without its zero before the trailer, an extra field longer
// than the image) is refused here: a damaged pack, never a disc's (every pack on it has FNAME alone).
#ifndef DOTHEROES_GZ_WALK_H
#define DOTHEROES_GZ_WALK_H

#include <cstdint>
#include <cstring>

struct GzWalk {
    const char* refused;   // why the game would not take the pack (a damaged one), or nullptr
    uint32_t start;        // the deflate data's offset in the image
    uint32_t csize;        // its size: the image less the start and the 8-byte trailer
    uint32_t usize;        // ISIZE, the output's size
    uint32_t crc;          // the trailer's CRC-32 of the output
};

inline GzWalk gz_walk(const uint8_t* g, uint32_t size)
{
    constexpr uint8_t kFHcrc = 0x02, kFExtra = 0x04, kFName = 0x08, kFComment = 0x10, kFRefused = 0x20;
    GzWalk w{ nullptr, 10, 0, 0, 0 };
    auto le = [&](uint32_t at) {
        return (uint32_t)g[at] | (uint32_t)g[at + 1] << 8 | (uint32_t)g[at + 2] << 16 | (uint32_t)g[at + 3] << 24;
    };
    auto past_zero = [&](uint32_t at) -> bool {   // a name or a comment: its zero, before the trailer
        const void* nul = at < size - 8 ? memchr(g + at, 0, size - 8 - at) : nullptr;
        if (!nul) return false;
        w.start = (uint32_t)((const uint8_t*)nul - g) + 1;
        return true;
    };
    if (size < 18) return w.refused = "shorter than a header and a trailer", w;
    const uint8_t flg = g[3];
    if (g[0] != 0x1F || g[1] != 0x8B) w.refused = "not gzip (magic)";
    else if (g[2] != 8) w.refused = "not deflate (method)";
    else if (flg & kFHcrc) w.refused = "a header CRC (FHCRC), which the game refuses";
    else if ((flg & kFExtra) && (w.start += (uint32_t)g[10] | (uint32_t)g[11] << 8) > size - 8)
        w.refused = "FEXTRA running into the trailer";
    else if ((flg & kFName) && !past_zero(w.start)) w.refused = "a name running into the trailer";
    else if ((flg & kFComment) && !past_zero(w.start)) w.refused = "a comment running into the trailer";
    else if (flg & kFRefused) w.refused = "header flag 0x20, which the game refuses";
    if (w.refused) return w;
    w.csize = size - 8 - w.start;
    w.crc = le(size - 8);
    w.usize = le(size - 4);
    return w;
}

#endif
