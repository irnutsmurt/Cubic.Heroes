/* A small JSON reader for R1's pass descriptions (r1_replay.c): objects,
 * arrays, numbers, strings, true, false, null. Enough for desc.json; not a
 * general parser (no \u escapes beyond ASCII). */
#ifndef DOTHEROES_R1_JSON_H
#define DOTHEROES_R1_JSON_H

typedef enum { RJ_NULL, RJ_BOOL, RJ_NUM, RJ_STR, RJ_ARR, RJ_OBJ } rj_type;

typedef struct rj {
    rj_type type;
    double num;                 /* RJ_NUM, RJ_BOOL (0 or 1) */
    char* str;                  /* RJ_STR */
    struct rj** items;          /* RJ_ARR, RJ_OBJ (values) */
    char** keys;                /* RJ_OBJ */
    int n;
} rj;

/* Parse text; NULL on a syntax error (err says where). */
rj* rj_parse(const char* text, char* err, int err_size);
void rj_free(rj* v);

/* Lookups: NULL when absent or of another type. */
const rj* rj_get(const rj* obj, const char* key);
const rj* rj_at(const rj* arr, int i);
double rj_num(const rj* obj, const char* key, double dflt);
const char* rj_str(const rj* obj, const char* key);

#endif
