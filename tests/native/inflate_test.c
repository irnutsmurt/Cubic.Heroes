/* port/native/inflate.c against zlib's output and for speed.
 *
 *   zig cc -target x86_64-windows-gnu -O2 -Iport/native tests/native/inflate_test.c
 *     port/native/inflate.c -o build/native-test/inflate_test.exe
 *   build/native-test/inflate_test.exe <file.gz> [<out>]
 *
 * Inflates a gzip file's deflate stream (the header skipped) five times and
 * prints the output size, the best time, the input the stream used and an
 * FNV-1a hash of the output; <out> gets the output itself.
 * tests/native/inflate_check.py runs it over the game's map packs and compares
 * each output with Python's zlib. */
#include "inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: inflate_test <file.gz> [<out>]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* src = malloc((size_t)n);
    if (!src || fread(src, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 2; }
    fclose(f);
    if (n < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) { fprintf(stderr, "not gzip\n"); return 2; }
    const unsigned flg = src[3];
    size_t p = 10;
    if (flg & 4) p += 2 + (src[p] | src[p + 1] << 8);       /* FEXTRA */
    if (flg & 8) while (p < (size_t)n && src[p++]) {}        /* FNAME */
    if (flg & 16) while (p < (size_t)n && src[p++]) {}       /* FCOMMENT */
    if (flg & 2) p += 2;                                     /* FHCRC */
    const size_t isize = src[n - 4] | src[n - 3] << 8 | src[n - 2] << 16 | (size_t)src[n - 1] << 24;
    unsigned char* dst = malloc(isize + 64);
    if (argc > 2 && !strcmp(argv[2], "--fuzz")) {
        /* Damaged streams: cut short, bytes changed, too little room. Each must
         * end in an error or a short output, never a crash or a write past dst. */
        const int rounds = argc > 3 ? atoi(argv[3]) : 2000;
        const size_t len = (size_t)n - p;
        unsigned char* m = malloc(len);
        unsigned seed = 12345, counts[4] = { 0 };
        for (int r = 0; r < rounds; r++) {
            memcpy(m, src + p, len);
            seed = seed * 1103515245u + 12345u;
            size_t mlen = len, cap = isize;
            switch (r % 3) {
            case 0: mlen = (seed >> 4) % len; break;                          /* cut short */
            case 1: for (int k = 0; k < 1 + r % 8; k++) {                     /* bytes changed */
                        seed = seed * 1103515245u + 12345u;
                        m[(seed >> 4) % len] ^= (unsigned char)(1 + (seed >> 24) % 255);
                    } break;
            case 2: cap = (seed >> 4) % (isize + 1); break;                   /* too little room */
            }
            memset(dst + cap, 0xA5, 64);
            const long g = dh_inflate_raw(m, mlen, dst, cap, NULL);
            for (int k = 0; k < 64; k++)
                if (dst[cap + k] != 0xA5) { printf("round %d: wrote past the output\n", r); return 1; }
            counts[g >= 0 ? 0 : -g]++;
        }
        printf("%d rounds: %u decoded, %u bad data, %u no room, %u short\n", rounds, counts[0], counts[1],
               counts[2], counts[3]);
        return 0;
    }
    LARGE_INTEGER fq, t0, t1;
    QueryPerformanceFrequency(&fq);
    double best = 1e9;
    long got = 0;
    size_t used = 0;
    for (int r = 0; r < 5; r++) {
        QueryPerformanceCounter(&t0);
        got = dh_inflate_raw(src + p, (size_t)n - p, dst, isize, &used);
        QueryPerformanceCounter(&t1);
        const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
        if (ms < best) best = ms;
    }
    if (got < 0) { printf("%s: error %ld\n", argv[1], got); return 1; }
    unsigned long long h = 1469598103934665603ull;
    for (long i = 0; i < got; i++) h = (h ^ dst[i]) * 1099511628211ull;
    printf("%s: %ld bytes in %.2f ms (%.0f MB/s), used %zu of %zu, trailer at +%zu, fnv %016llx\n", argv[1], got, best,
           got / best / 1000.0, used, (size_t)n - p, (size_t)n - p - used, h);
    if (argc > 2) {
        FILE* o = fopen(argv[2], "wb");
        if (o) { fwrite(dst, 1, (size_t)got, o); fclose(o); }
    }
    return 0;
}
