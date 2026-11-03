/* A native raw-deflate decoder (RFC 1951) for the game's Edge zlib work: the
 * map packs it inflates on an SPU at every area load.  One-shot, all input and
 * output in memory -- what Edge's fetch-and-inflate does between main-memory
 * buffers -- written for speed (64-bit bit buffer, two-level decode tables,
 * word copies) and checked byte for byte against zlib (tests/native/
 * inflate_test.c). */
#ifndef DOTHEROES_INFLATE_H
#define DOTHEROES_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DH_INFLATE_BAD_DATA = -1,   /* not a valid deflate stream */
    DH_INFLATE_NO_ROOM  = -2,   /* the output does not fit in dst_cap */
    DH_INFLATE_SHORT    = -3,   /* the input ends before the final block does */
};

/* Inflates the raw deflate stream at src (no zlib or gzip wrapper) into dst.
 * Returns the bytes written, or a DH_INFLATE_* error. *src_used (if not NULL)
 * gets the input bytes the stream took, up to the end of its final block. */
long dh_inflate_raw(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, size_t* src_used);

#ifdef __cplusplus
}
#endif

#endif
