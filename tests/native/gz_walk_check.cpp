// port/native/gz_walk.h -- the game's gzip header walk as inflate_direct.cpp takes a map pack's header -- checked
// branch by branch on images made here, then each image the walk accepts inflated with port/native/inflate.c, as
// the native inflate does in the game: the disc's form (FNAME alone), no flags, the ignored bits (FTEXT, 0x40,
// 0x80), FEXTRA with the game's arithmetic (the data at 10 + XLEN), FEXTRA with a name, a comment, compressed
// data bigger than its output (Edge only warns), and what the walk refuses (FHCRC, 0x20, another magic or method,
// a short image, a name, a comment or an extra field running into the trailer) or the inflate finds damaged.
// No game data: the deflate streams are made here or written below (zlib's raw deflate of "abcdefgh" x 125).
//
// Built with the bundled Zig from the repository root (Z = build/toolchain/zig-x86_64-windows-0.17.0/zig.exe):
//   $Z cc -target x86_64-windows-gnu -O2 -c port/native/inflate.c -o <scratch>/inflate.o
//   $Z c++ -target x86_64-windows-gnu -O2 -std=c++17 -I port/native tests/native/gz_walk_check.cpp
//       <scratch>/inflate.o -o <scratch>/gz_walk_check.exe
#include "gz_walk.h"
#include "inflate.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

typedef std::vector<uint8_t> Bytes;
int g_fail, g_pass;

void check(bool ok, const char* what)
{
    (ok ? g_pass : g_fail)++;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

const uint8_t kAbc[] = { 0x4B, 0x4C, 0x4A, 0x4E, 0x49, 0x4D, 0x4B, 0xCF, 0x48,
                         0x1C, 0xA5, 0x47, 0xE9, 0x51, 0x7A, 0xD8, 0xD2, 0x00 };   // 1,000 bytes

Bytes abc_output()
{
    Bytes o;
    for (int i = 0; i < 125; i++)
        for (char c : std::string("abcdefgh")) o.push_back((uint8_t)c);
    return o;
}

Bytes stored(const Bytes& data)   // one final stored block: compressed data 5 bytes bigger than its output
{
    const uint16_t n = (uint16_t)data.size();
    Bytes b = { 0x01, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8) };
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

void put32(Bytes& b, uint32_t v)
{
    for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

// header bytes (from the flags on), then the deflate data, then the trailer of the given output
Bytes image(uint8_t flags, const Bytes& after_header, const Bytes& deflate, const Bytes& output)
{
    Bytes b = { 0x1F, 0x8B, 8, flags, 0, 0, 0, 0, 0, 0x0B };
    b.insert(b.end(), after_header.begin(), after_header.end());
    b.insert(b.end(), deflate.begin(), deflate.end());
    put32(b, dh_crc32(0, output.data(), output.size()));
    put32(b, (uint32_t)output.size());
    return b;
}

Bytes str0(const char* s)
{
    Bytes b(s, s + strlen(s));
    b.push_back(0);
    return b;
}

Bytes cat(Bytes a, const Bytes& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// the walk, then (accepted) the inflate as inflate_direct.cpp runs it: damaged unless it fills the output exactly
void accepted(const char* what, const Bytes& img, uint32_t start_want, const Bytes& output)
{
    printf("%s\n", what);
    const GzWalk w = gz_walk(img.data(), (uint32_t)img.size());
    check(!w.refused, "the walk takes it");
    if (w.refused) {
        printf("        refused: %s\n", w.refused);
        return;
    }
    char t[160];
    snprintf(t, sizeof t, "the data at %u (want %u), %u bytes, ISIZE %u", w.start, start_want, w.csize, w.usize);
    check(w.start == start_want && w.csize == img.size() - 8 - start_want && w.usize == output.size(), t);
    Bytes out(w.usize ? w.usize : 1);
    const long got = dh_inflate_raw(img.data() + w.start, w.csize, out.data(), w.usize, nullptr);
    snprintf(t, sizeof t, "inflated: %ld of %u bytes", got, w.usize);
    check(got == (long)w.usize && Bytes(out.begin(), out.begin() + w.usize) == output, t);
    check(dh_crc32(0, out.data(), w.usize) == w.crc, "the output's CRC-32 is the trailer's");
}

void refused(const char* what, const Bytes& img)
{
    printf("%s\n", what);
    const GzWalk w = gz_walk(img.data(), (uint32_t)img.size());
    char t[160];
    snprintf(t, sizeof t, "refused (%s)", w.refused ? w.refused : "taken");
    check(w.refused != nullptr, t);
}

void damaged(const char* what, const Bytes& img)
{
    printf("%s\n", what);
    const GzWalk w = gz_walk(img.data(), (uint32_t)img.size());
    check(!w.refused, "the walk takes it");
    if (w.refused) return;
    Bytes out(w.usize ? w.usize : 1);
    const long got = dh_inflate_raw(img.data() + w.start, w.csize, out.data(), w.usize, nullptr);
    char t[160];
    snprintf(t, sizeof t, "damaged: the stream gave %ld of %u bytes", got, w.usize);
    check(got != (long)w.usize, t);
}

}  // namespace

int main()
{
    const Bytes abc(kAbc, kAbc + sizeof kAbc), out = abc_output(), none;
    const Bytes name = str0("stage01.pack"), comment = str0("a comment");

    accepted("FNAME alone (every pack on the disc)", image(0x08, name, abc, out), 10 + (uint32_t)name.size(), out);
    accepted("no flags", image(0x00, none, abc, out), 10, out);
    accepted("FTEXT, 0x40 and 0x80 with FNAME: ignored", image(0x01 | 0x40 | 0x80 | 0x08, name, abc, out),
             10 + (uint32_t)name.size(), out);
    // FEXTRA: XLEN 6, the field's six bytes; the game starts the data at 10 + XLEN, two bytes before the field's
    // end -- so an image the game reads whole has the data there (the field's last two bytes are the data's first)
    {
        const Bytes field = { 6, 0, 'A', 'B', 2, 0 };   // XLEN 6, then the field's first four bytes
        accepted("FEXTRA: the data at 10 + XLEN (the game's arithmetic, not RFC 1952's 12 + XLEN)",
                 image(0x04, field, abc, out), 16, out);
        const Bytes field_n = { 6, 0, 'A', 'B', 2, 0, 'x', 'y' };   // the name's search starts at 16: "xy" ends it
        accepted("FEXTRA and FNAME: the name looked for from 10 + XLEN",
                 image(0x04 | 0x08, cat(field_n, name), abc, out), 10 + 8 + (uint32_t)name.size(), out);
    }
    accepted("FNAME and FCOMMENT", image(0x08 | 0x10, cat(name, comment), abc, out),
             10 + (uint32_t)(name.size() + comment.size()), out);
    accepted("compressed data bigger than its output (Edge's warning only)", image(0x08, name, stored(out), out),
             10 + (uint32_t)name.size(), out);

    refused("FHCRC (the game refuses it)", image(0x08 | 0x02, name, abc, out));
    refused("flag 0x20 (the game refuses it)", image(0x08 | 0x20, name, abc, out));
    {
        Bytes m = image(0x08, name, abc, out);
        m[1] = 0x8C;
        refused("another magic", m);
        Bytes d = image(0x08, name, abc, out);
        d[2] = 7;
        refused("another method", d);
    }
    {
        Bytes s = image(0, none, none, none);   // 18 bytes: a header and a trailer
        s.pop_back();
        refused("an image shorter than a header and a trailer", s);
    }
    refused("a name without its zero before the trailer", image(0x08, Bytes(name.begin(), name.end() - 1), none, out));
    refused("a comment without its zero before the trailer",
            image(0x08 | 0x10, cat(name, Bytes(comment.begin(), comment.end() - 1)), none, out));
    refused("FEXTRA longer than the image", image(0x04, Bytes{ 0xFF, 0x00 }, abc, out));

    {
        Bytes bad = abc;
        bad[0] ^= 0x04;   // the first block's type 3 (reserved): not a valid deflate stream
        damaged("a stream that is not deflate", image(0x08, name, bad, out));
        damaged("a stream that ends early", image(0x08, name, Bytes(abc.begin(), abc.end() - 6), out));
        damaged("a stream shorter than ISIZE", image(0x08, name, abc, cat(out, str0("more"))));
    }

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
