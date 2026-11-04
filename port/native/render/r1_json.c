/* See r1_json.h. */
#include "r1_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char* p; char* err; int err_size; int bad; } rj_in;

static void rj_fail(rj_in* in, const char* what)
{
    if (!in->bad) snprintf(in->err, (size_t)in->err_size, "%s near '%.20s'", what, in->p);
    in->bad = 1;
}

static void rj_ws(rj_in* in)
{
    while (*in->p == ' ' || *in->p == '\t' || *in->p == '\n' || *in->p == '\r') in->p++;
}

static rj* rj_new(rj_type t)
{
    rj* v = (rj*)calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

static char* rj_string(rj_in* in)
{
    in->p++;   /* the opening quote */
    size_t cap = 32, n = 0;
    char* s = (char*)malloc(cap);
    while (*in->p && *in->p != '"') {
        char c = *in->p++;
        if (c == '\\') {
            c = *in->p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned u = 0;
                sscanf(in->p, "%4x", &u);
                in->p += 4;
                c = (char)(u < 128 ? u : '?');
                break;
            }
            default: break;   /* \" \\ \/ */
            }
        }
        if (n + 1 >= cap) s = (char*)realloc(s, cap *= 2);
        s[n++] = c;
    }
    if (*in->p != '"') { rj_fail(in, "unterminated string"); free(s); return NULL; }
    in->p++;
    s[n] = 0;
    return s;
}

static rj* rj_value(rj_in* in, int depth);

static void rj_push(rj* v, rj* item, char* key)
{
    v->items = (rj**)realloc(v->items, sizeof(rj*) * (size_t)(v->n + 1));
    if (v->type == RJ_OBJ) {
        v->keys = (char**)realloc(v->keys, sizeof(char*) * (size_t)(v->n + 1));
        v->keys[v->n] = key;
    }
    v->items[v->n++] = item;
}

static rj* rj_value(rj_in* in, int depth)
{
    rj_ws(in);
    if (depth > 64) { rj_fail(in, "too deep"); return NULL; }
    const char c = *in->p;
    if (c == '{' || c == '[') {
        rj* v = rj_new(c == '{' ? RJ_OBJ : RJ_ARR);
        const char close = c == '{' ? '}' : ']';
        in->p++;
        rj_ws(in);
        if (*in->p == close) { in->p++; return v; }
        for (;;) {
            char* key = NULL;
            if (v->type == RJ_OBJ) {
                rj_ws(in);
                if (*in->p != '"') { rj_fail(in, "expected a key"); rj_free(v); return NULL; }
                key = rj_string(in);
                rj_ws(in);
                if (!key || *in->p != ':') { rj_fail(in, "expected ':'"); free(key); rj_free(v); return NULL; }
                in->p++;
            }
            rj* item = rj_value(in, depth + 1);
            if (!item) { free(key); rj_free(v); return NULL; }
            rj_push(v, item, key);
            rj_ws(in);
            if (*in->p == ',') { in->p++; continue; }
            if (*in->p == close) { in->p++; return v; }
            rj_fail(in, "expected ',' or a close");
            rj_free(v);
            return NULL;
        }
    }
    if (c == '"') {
        rj* v = rj_new(RJ_STR);
        v->str = rj_string(in);
        if (!v->str) { rj_free(v); return NULL; }
        return v;
    }
    if (!strncmp(in->p, "true", 4)) { in->p += 4; rj* v = rj_new(RJ_BOOL); v->num = 1; return v; }
    if (!strncmp(in->p, "false", 5)) { in->p += 5; return rj_new(RJ_BOOL); }
    if (!strncmp(in->p, "null", 4)) { in->p += 4; return rj_new(RJ_NULL); }
    char* end;
    const double d = strtod(in->p, &end);
    if (end == in->p) { rj_fail(in, "unexpected character"); return NULL; }
    in->p = end;
    rj* v = rj_new(RJ_NUM);
    v->num = d;
    return v;
}

rj* rj_parse(const char* text, char* err, int err_size)
{
    rj_in in = { text, err, err_size, 0 };
    if (err && err_size) err[0] = 0;
    rj* v = rj_value(&in, 0);
    if (v) { rj_ws(&in); if (*in.p) { rj_fail(&in, "trailing text"); rj_free(v); v = NULL; } }
    return v;
}

void rj_free(rj* v)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        rj_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

const rj* rj_get(const rj* obj, const char* key)
{
    if (!obj || obj->type != RJ_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

const rj* rj_at(const rj* arr, int i)
{
    return arr && arr->type == RJ_ARR && i >= 0 && i < arr->n ? arr->items[i] : NULL;
}

double rj_num(const rj* obj, const char* key, double dflt)
{
    const rj* v = rj_get(obj, key);
    return v && (v->type == RJ_NUM || v->type == RJ_BOOL) ? v->num : dflt;
}

const char* rj_str(const rj* obj, const char* key)
{
    const rj* v = rj_get(obj, key);
    return v && v->type == RJ_STR ? v->str : NULL;
}
