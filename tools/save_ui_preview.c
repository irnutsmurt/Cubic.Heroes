/* save_ui_preview: the save-data screen (ps3recomp libs/video/osd_saves.c) composed off-line into a PNG,
 * from save directories on disk, without the game -- for the design and its comparison with the reference.
 *   save_ui_preview <out.png> <width> <height> <state> <prompts> <font.ttf> <prompt dir> <save dir>...
 * state: load, save (a new-save row first), confirm (the overwrite question on the first save), empty,
 *        edit (the character editor's load), many (the saves repeated to fill a long list, the 6th selected)
 * prompts: keyboard, playstation, xbox, switch. Each <save dir> is a copy of a save (PARAM.SFO, ICON0.PNG). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "ps3emu/osd.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../codec/stb_image_write.h"

static unsigned char* slurp(const char* p, size_t* n)
{
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc((size_t)sz);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

/* the PARAM.SFO text fields, as cellSaveData.c's pick_describe reads them */
static void sfo_field(const unsigned char* b, size_t n, const char* key, char* out, size_t cap)
{
    out[0] = 0;
    if (n < 20 || memcmp(b, "\0PSF", 4)) return;
    unsigned kt = b[8] | b[9] << 8 | b[10] << 16 | (unsigned)b[11] << 24;
    unsigned dt = b[12] | b[13] << 8 | b[14] << 16 | (unsigned)b[15] << 24;
    unsigned cnt = b[16] | b[17] << 8 | b[18] << 16 | (unsigned)b[19] << 24;
    for (unsigned i = 0; i < cnt && 20 + (i + 1) * 16 <= n; i++) {
        const unsigned char* r = b + 20 + i * 16;
        unsigned ko = r[0] | r[1] << 8, len = r[4] | r[5] << 8 | r[6] << 16 | (unsigned)r[7] << 24;
        unsigned dof = r[12] | r[13] << 8 | r[14] << 16 | (unsigned)r[15] << 24;
        if (kt + ko >= n || dt + dof + len > n || strcmp((const char*)b + kt + ko, key)) continue;
        size_t l = len < cap ? len : cap - 1;
        memcpy(out, b + dt + dof, l);
        out[l] = 0;
    }
}

/* cellSaveData.c's ui_fold, the same rules (kept in step by hand: this is a preview) */
static void fold(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    int label = 1, spaced = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 5 < cap; ) {
        unsigned cp = *p++;
        if (cp >= 0xF0 && p[0] && p[1] && p[2]) { cp = ((cp & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); p += 3; }
        else if (cp >= 0xE0 && p[0] && p[1])   { cp = ((cp & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u); p += 2; }
        else if (cp >= 0xC0 && p[0])           { cp = ((cp & 31) << 6) | (p[0] & 63u); p += 1; }
        if (cp == 0x3000) cp = ' ';
        else if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        if (cp == '\r' || cp == '\n' || cp < 32) continue;
        if (cp == ':' && label && !spaced && o) {
            while (o && out[o - 1] == ' ') o--;
            memcpy(out + o, " : ", 3); o += 3;
            spaced = 1;
            while (*p == ' ') p++;
            continue;
        }
        if (!((cp >= 'A' && cp <= 'Z') || cp == ' ' || cp == '_')) label = 0;
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 63)); }
        else { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
    }
    out[o] = 0;
}

static void describe(const char* dir, ps3_osd_save_entry* e)
{
    char p[1024], v[1024];
    size_t n = 0;
    memset(e, 0, sizeof *e);
    snprintf(p, sizeof p, "%s/PARAM.SFO", dir);
    unsigned char* b = slurp(p, &n);
    struct stat st;
    if (stat(p, &st) == 0) {
        time_t t = st.st_mtime;
        struct tm* lt = localtime(&t);
        if (lt) strftime(e->date, sizeof e->date, "%a %b %d %H:%M:%S %Y", lt);
    }
    if (b) {
        sfo_field(b, n, "SUB_TITLE", v, sizeof v);
        if (!v[0]) sfo_field(b, n, "TITLE", v, sizeof v);
        fold(v, e->title, sizeof e->title);
        sfo_field(b, n, "DETAIL", v, sizeof v);
        int li = 0;
        for (char* s = strtok(v, "\n"); s && li < PS3_OSD_SAVES_LINES; s = strtok(NULL, "\n")) {
            fold(s, e->lines[li], sizeof e->lines[li]);
            if (e->lines[li][0]) li++;
        }
        free(b);
    }
    unsigned long long total = 0;
#ifdef _WIN32
    char search[1100];
    snprintf(search, sizeof search, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    if (total) snprintf(e->size, sizeof e->size, "%llu KB", (total + 999) / 1000);
    snprintf(p, sizeof p, "%s/ICON0.PNG", dir);
    if (stat(p, &st) == 0) snprintf(e->icon, sizeof e->icon, "%s", p);
}

int main(int argc, char** argv)
{
    if (argc < 8) { fprintf(stderr, "usage: see the source\n"); return 2; }
    const char* out = argv[1];
    const unsigned W = (unsigned)atoi(argv[2]), H = (unsigned)atoi(argv[3]);
    const char* state = argv[4];
    const char* pr = argv[5];
    size_t fn = 0;
    unsigned char* font = slurp(argv[6], &fn);
    if (!font) { fprintf(stderr, "no font %s\n", argv[6]); return 1; }
    ps3_ui_set_font(font, fn);
    static const char* names[4][2] = {
        { "prompt_keyboard_confirm.png", "prompt_keyboard_back.png" }, { NULL, NULL },
        { "prompt_xbox_confirm.png", "prompt_xbox_back.png" }, { "prompt_switch_confirm.png", "prompt_switch_back.png" } };
    for (int s = 0; s < 4; s++)
        for (int k = 0; k < 2; k++) {
            if (!names[s][k]) continue;
            char p[1024];
            size_t n = 0;
            snprintf(p, sizeof p, "%s/%s", argv[7], names[s][k]);
            unsigned char* b = slurp(p, &n);
            if (b) ps3_ui_set_prompt_png(s, k, b, n);
        }
    static ps3_osd_saves_state S;
    memset(&S, 0, sizeof S);
    S.prompts = !strcmp(pr, "playstation") ? PS3_UI_PROMPTS_PLAYSTATION : !strcmp(pr, "xbox") ? PS3_UI_PROMPTS_XBOX
              : !strcmp(pr, "switch") ? PS3_UI_PROMPTS_SWITCH : PS3_UI_PROMPTS_KEYBOARD;
    const int is_save = !strcmp(state, "save") || !strcmp(state, "confirm");
    const int edit = !strcmp(state, "edit");
    snprintf(S.heading, sizeof S.heading, "%s", is_save ? "Save Game" : edit ? "Load Character" : "Load Save");
    snprintf(S.empty, sizeof S.empty, "There is no saved data.");
    snprintf(S.question, sizeof S.question, "Overwrite this save data?");
    S.confirm = !strcmp(state, "confirm");
    snprintf(S.ok_label, sizeof S.ok_label, "%s", S.confirm ? "Yes" : "Enter");
    snprintf(S.back_label, sizeof S.back_label, "%s", S.confirm ? "No" : "Back");
    unsigned n = 0;
    if (is_save && !S.confirm) {
        S.e[n].is_new = 1;
        snprintf(S.e[n].title, sizeof S.e[n].title, "New Save Data");
        n++;
    }
    if (strcmp(state, "empty")) {
        const int reps = !strcmp(state, "many") ? 6 : 1;
        for (int r = 0; r < reps; r++)
            for (int i = 8; i < argc && n < PS3_OSD_SAVES_MAX; i++) describe(argv[i], &S.e[n++]);
    }
    if (!strcmp(state, "long") && n) {   /* text too long for its row: it must end in an ellipsis */
        snprintf(S.e[0].title, sizeof S.e[0].title, "SAVE DATA WITH A SUBTITLE FAR LONGER THAN ANY ROW CAN HOLD");
        snprintf(S.e[0].lines[0], sizeof S.e[0].lines[0],
                 "NAME : Sir Reginald Bartholomew Fitzwilliam the Third of the Northern Reaches");
        snprintf(S.e[0].lines[2], sizeof S.e[0].lines[2],
                 "LOCATION : The Endless Corridor of the Very Long Location Names, Lower Level");
    }
    S.n = n;
    S.cur = !strcmp(state, "many") ? 5 : 0;
    S.open = 1;
    unsigned char* img = (unsigned char*)malloc((size_t)W * H * 4);
    if (!ps3_osd_saves_compose(&S, img, W, H, 0)) { fprintf(stderr, "compose failed\n"); return 1; }
    if (!stbi_write_png(out, (int)W, (int)H, 4, img, (int)W * 4)) { fprintf(stderr, "write failed\n"); return 1; }
    printf("%s: %ux%u, %u row(s)\n", out, W, H, n);
    return 0;
}
