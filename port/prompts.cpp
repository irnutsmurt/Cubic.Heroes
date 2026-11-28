// M11.2g: the game's button prompts in the player's style (docs/M11_PLAN.md section 35).
//
// The game shows its buttons in four ways: glyphs of its fonts inside text, button art in its UI atlases (DXT
// textures), three prompts written as words, and the attract demo's video. The first two are replaced as the
// renderer uploads the textures: a texture is recognised by a fingerprint of its first level, made at start from the
// player's own files (port/prompts_table.cpp names them and the cells -- our annotations, no game data), and is
// uploaded with each button's cell drawn again: the button that performs that game button now (the player's
// bindings), in the style chosen or of the device used last. PlayStation keeps the game's own art (with the default
// bindings the textures are not touched at all; remapped, the game's own art of the physical button is drawn in).
// The other styles draw Kenney's input prompts (CC0, port/assets/ui/prompts) as they are drawn, on a transparent
// background (the owner's direction: the icon alone, nothing behind it). The words are the game's
// own Lua (prompt_lua_chunk: the title's line and the guard's lesson), the video is left as it is (docs).
#include "prompts_table.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
void stbi_image_free(void* p);
struct embedded_file { const char* name; const unsigned char* data; size_t size; };
extern const struct embedded_file embedded_files[];
extern const unsigned embedded_files_count;
int cellPad_ui_device(void);
int cellPad_connected_family(void);
uint32_t cellPad_pad_binding(int game_bit);
const char* cellPad_key_binding_name(int game_bit);
void dh_log(int level, const char* tag, const char* fmt, ...) __attribute__((weak, format(printf, 3, 4)));
typedef struct { uint8_t* p; int w, h, bgra; } ps3_ui_canvas;
void ps3_ui_rrect(const ps3_ui_canvas* c, float x0, float y0, float x1, float y1, float r, uint32_t rgb, int a);
float ps3_ui_text(const ps3_ui_canvas* c, float x, float y, const char* s, int px, uint32_t rgb, int a, float maxw);
float ps3_ui_text_width(const char* s, int px);
int ps3_ui_have_font(void);
}

namespace prompts {
namespace {

// the styles (ps3emu/osd.h PS3_UI_PROMPTS_*)
enum Style { S_KEYBOARD = 0, S_PLAYSTATION = 1, S_XBOX = 2, S_NINTENDO = 3, S_STEAM = 4, S_GENERIC = 5, S_COUNT };
const char* const kStyleNames[S_COUNT] = { "keyboard", "playstation", "xbox", "nintendo", "steam", "generic" };

std::atomic<int> g_choice{ -1 };   // the player's choice (LAUNCHER_PROMPTS); -1 automatic

struct Rgba { int w = 0, h = 0; std::vector<uint8_t> px; };   // straight alpha, RGBA

// what a fingerprint is: a table texture, or a font (its package index)
struct Known { int tex = -1, font = -1; };
std::unordered_map<uint64_t, Known> g_known;
std::vector<uint64_t> g_dims;      // bc << 40 | w << 20 | h of every known texture (a cheap filter first)
bool g_ready = false;
Rgba g_ps_art[B_COUNT];            // the game's own art of each button (from the player's files), for remapping
std::mutex g_mu;                   // the glyph cache
std::unordered_map<std::string, Rgba> g_glyphs;   // a style's glyph (Kenney's 64x64 icon as drawn), by file

uint32_t bc_of_tpk(uint32_t f) { return f == 9 ? 1 : f == 10 ? 2 : f == 11 ? 3 : 0; }
size_t level0_bytes(uint32_t bc, uint32_t w, uint32_t h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * (bc == 1 ? 8 : 16); }
uint64_t dims_key(uint32_t bc, uint32_t w, uint32_t h) { return (uint64_t)bc << 40 | (uint64_t)w << 20 | h; }

// a texture's fingerprint: its kind and size, its first level's first 16 KB and last 4 KB
uint64_t fingerprint(uint32_t bc, uint32_t w, uint32_t h, const uint8_t* p, size_t n)
{
    uint64_t x = 1469598103934665603ull ^ dims_key(bc, w, h);
    auto mix = [&](const uint8_t* q, size_t k) { for (size_t i = 0; i < k; i++) { x ^= q[i]; x *= 1099511628211ull; } };
    const size_t head = std::min<size_t>(n, 16384);
    mix(p, head);
    if (n > head) { const size_t tail = std::min<size_t>(n - head, 4096); mix(p + n - tail, tail); }
    return x ? x : 1;
}

// ---- the game's files: its texture packages ----
struct TpkEntry { std::string name; uint32_t off = 0, fmt = 0, w = 0, h = 0; };
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}
bool parse_tpk(const std::vector<uint8_t>& b, std::vector<TpkEntry>& out)
{
    if (b.size() < 0x20 || memcmp(b.data(), "TPKGps3", 8)) return false;
    const uint32_t n = be16(&b[0x0A]), data = be32(&b[0x0C]);
    for (uint32_t i = 0; i < n && 0x40 + i * 0x20 <= b.size(); i++) {
        const uint8_t* e = &b[0x20 + i * 0x20];
        TpkEntry t;
        const uint32_t noff = be32(e);
        for (uint32_t k = noff; k < b.size() && b[k]; k++) t.name.push_back((char)tolower(b[k]));
        t.off = data + be32(e + 8); t.fmt = be32(e + 12); t.w = be16(e + 16); t.h = be16(e + 18);
        out.push_back(t);
    }
    return true;
}

// ---- DXT ----
void rgb565(uint16_t c, uint8_t* o) { o[0] = (uint8_t)((c >> 11) * 255 / 31); o[1] = (uint8_t)((c >> 5 & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); }
void decode_bc(uint32_t bc, uint32_t w, uint32_t h, const uint8_t* src, std::vector<uint8_t>& out)
{
    out.assign((size_t)w * h * 4, 0);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = bc == 1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = src + ((size_t)by * bw + bx) * bs;
            const uint8_t* col = bc == 1 ? b : b + 8;
            const uint16_t c0 = (uint16_t)(col[0] | col[1] << 8), c1 = (uint16_t)(col[2] | col[3] << 8);
            uint8_t pal[4][4];
            rgb565(c0, pal[0]); rgb565(c1, pal[1]);
            pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
            if (bc != 1 || c0 > c1) {
                for (int k = 0; k < 3; k++) { pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3); }
            } else {
                for (int k = 0; k < 3; k++) { pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
                pal[3][3] = 0;
            }
            const uint32_t idx = (uint32_t)col[4] | (uint32_t)col[5] << 8 | (uint32_t)col[6] << 16 | (uint32_t)col[7] << 24;
            uint8_t alpha[16];
            if (bc == 2) {
                for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            } else if (bc == 3) {
                uint8_t a[8] = { b[0], b[1] };
                if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
                else { for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = a[bits >> (3 * i) & 7];
            }
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t* o = &out[((size_t)y * w + x) * 4];
                memcpy(o, pal[idx >> (2 * i) & 3], 4);
                if (bc != 1) o[3] = alpha[i];
            }
        }
}

// ---- drawing ----
struct Box { int x0, y0, x1, y1; bool empty() const { return x1 <= x0 || y1 <= y0; } };

Rgba crop(const std::vector<uint8_t>& px, int W, const Box& b)
{
    Rgba r;
    r.w = b.x1 - b.x0; r.h = b.y1 - b.y0;
    r.px.resize((size_t)r.w * r.h * 4);
    for (int y = 0; y < r.h; y++) memcpy(&r.px[(size_t)y * r.w * 4], &px[((size_t)(b.y0 + y) * W + b.x0) * 4], (size_t)r.w * 4);
    return r;
}

// the background around a cell: its ring 2 px outside (inside the texture), averaged; transparent when mostly so
void background(const std::vector<uint8_t>& px, int W, int H, const Box& c, uint8_t bg[4])
{
    double s[4] = { 0, 0, 0, 0 }; int n = 0, clear = 0;
    auto at = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        const uint8_t* p = &px[((size_t)y * W + x) * 4];
        if (p[3] < 40) { clear++; return; }
        for (int k = 0; k < 4; k++) s[k] += p[k];
        n++;
    };
    for (int x = c.x0 - 2; x < c.x1 + 2; x++) { at(x, c.y0 - 2); at(x, c.y1 + 1); }
    for (int y = c.y0 - 1; y < c.y1 + 1; y++) { at(c.x0 - 2, y); at(c.x1 + 1, y); }
    if (n <= clear) { bg[0] = bg[1] = bg[2] = bg[3] = 0; return; }
    for (int k = 0; k < 4; k++) bg[k] = (uint8_t)(s[k] / n + 0.5);
}
bool differs(const uint8_t* p, const uint8_t bg[4])
{
    if (bg[3] < 40) return p[3] >= 40;
    return abs(p[3] - bg[3]) > 48 || abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]) > 90;
}
// the old art's box inside a cell (what differs from the background), else the cell
Box art_box(const std::vector<uint8_t>& px, int W, const Box& c, const uint8_t bg[4])
{
    Box b = { c.x1, c.y1, c.x0, c.y0 };
    for (int y = c.y0; y < c.y1; y++)
        for (int x = c.x0; x < c.x1; x++)
            if (differs(&px[((size_t)y * W + x) * 4], bg)) {
                b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y); b.x1 = std::max(b.x1, x + 1); b.y1 = std::max(b.y1, y + 1);
            }
    return b.empty() ? c : b;
}

// src (straight alpha) scaled into dst box with its aspect kept, centred, drawn over (area-averaged, premultiplied)
void draw_fit(std::vector<uint8_t>& px, int W, int H, const Box& box, const Rgba& g)
{
    // the glyph's own content
    Box gb = { g.w, g.h, 0, 0 };
    for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++)
            if (g.px[((size_t)y * g.w + x) * 4 + 3] > 8) { gb.x0 = std::min(gb.x0, x); gb.y0 = std::min(gb.y0, y); gb.x1 = std::max(gb.x1, x + 1); gb.y1 = std::max(gb.y1, y + 1); }
    if (gb.empty()) return;
    const double sw = gb.x1 - gb.x0, sh = gb.y1 - gb.y0, bw = box.x1 - box.x0, bh = box.y1 - box.y0;
    const double k = std::min(bw / sw, bh / sh);
    const double dw = sw * k, dh = sh * k, ox = box.x0 + (bw - dw) / 2, oy = box.y0 + (bh - dh) / 2;
    for (int y = (int)floor(oy); y < (int)ceil(oy + dh); y++)
        for (int x = (int)floor(ox); x < (int)ceil(ox + dw); x++) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            // the source area under this pixel
            const double sx0 = gb.x0 + (x - ox) / k, sx1 = gb.x0 + (x + 1 - ox) / k;
            const double sy0 = gb.y0 + (y - oy) / k, sy1 = gb.y0 + (y + 1 - oy) / k;
            double acc[4] = { 0, 0, 0, 0 }, wsum = 0;
            for (int v = (int)floor(sy0); v < (int)ceil(sy1); v++)
                for (int u = (int)floor(sx0); u < (int)ceil(sx1); u++) {
                    const double wx = std::min<double>(u + 1, sx1) - std::max<double>(u, sx0);
                    const double wy = std::min<double>(v + 1, sy1) - std::max<double>(v, sy0);
                    if (wx <= 0 || wy <= 0) continue;
                    const double wt = wx * wy;
                    wsum += wt;
                    if (u < 0 || v < 0 || u >= g.w || v >= g.h) continue;
                    const uint8_t* s = &g.px[((size_t)v * g.w + u) * 4];
                    const double a = s[3] / 255.0;
                    acc[0] += s[0] * a * wt; acc[1] += s[1] * a * wt; acc[2] += s[2] * a * wt; acc[3] += a * wt;
                }
            if (wsum <= 0) continue;
            const double a = acc[3] / wsum;
            if (a <= 0.0) continue;
            uint8_t* d = &px[((size_t)y * W + x) * 4];
            const double da = d[3] / 255.0, oa = a + da * (1 - a);
            for (int c = 0; c < 3; c++) {
                const double sc = acc[c] / wsum;   // premultiplied
                d[c] = (uint8_t)std::min(255.0, (sc + d[c] * da * (1 - a)) / (oa > 0 ? oa : 1) + 0.5);
            }
            d[3] = (uint8_t)std::min(255.0, oa * 255.0 + 0.5);
        }
}

const embedded_file* asset(const std::string& name)
{
    for (unsigned i = 0; i < embedded_files_count; i++)
        if (name == embedded_files[i].name) return &embedded_files[i];
    return nullptr;
}

// a style's glyph by its file name (cached; g_mu held); nullptr when the file is missing
const Rgba* glyph_file(const std::string& file, uint32_t ink)
{
    const std::string key = file + "#" + std::to_string(ink);
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second.w ? &it->second : nullptr;
    Rgba& r = g_glyphs[key];
    if (const embedded_file* f = asset("prompts/" + file + ".png")) {
        int w = 0, h = 0, n = 0;
        if (unsigned char* p = stbi_load_from_memory(f->data, (int)f->size, &w, &h, &n, 4)) {
            r.w = w; r.h = h; r.px.assign(p, p + (size_t)w * h * 4);   // Kenney's icon as drawn: transparent around it
            stbi_image_free(p);
        }
    }
    return r.w ? &r : nullptr;
}

// a key cap with its name (a key with no Kenney picture): white, its label clear, as Kenney draws keys
const Rgba* glyph_keycap(const std::string& label)
{
    const std::string key = "cap#" + label;
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second.w ? &it->second : nullptr;
    Rgba& r = g_glyphs[key];
    if (!ps3_ui_have_font()) return nullptr;
    const int px = 30;
    const int w = std::max(64, (int)ps3_ui_text_width(label.c_str(), px) + 24), h = 64;
    std::vector<uint8_t> cap((size_t)w * h * 4, 0);
    ps3_ui_canvas c = { cap.data(), w, h, 0 };
    ps3_ui_rrect(&c, 3, 3, (float)w - 3, (float)h - 3, 10, 0xFFFFFF, 255);
    // the label as a hole: drawn in black, then black made transparent
    ps3_ui_text(&c, 12, h / 2 + px * 0.36f, label.c_str(), px, 0x000000, 255, 0);
    for (size_t i = 0; i < cap.size(); i += 4) {
        const int lum = cap[i];   // white cap 255, label 0
        cap[i + 3] = (uint8_t)(cap[i + 3] * lum / 255);
        cap[i] = cap[i + 1] = cap[i + 2] = 255;
    }
    r.w = w; r.h = h; r.px.swap(cap);   // white, its label clear, as Kenney's keys are
    return &r;
}

// ---- which glyph: the style, the physical button ----
// a pad bit (cellPad.h order) in a style: its Kenney file and its ink
struct Pick { const char* file; uint32_t ink; };
Pick pad_pick(int style, int bit)
{
    const uint32_t W = 0xF0F0F0;
    switch (style) {
    case S_XBOX: {
        static const Pick p[16] = { { "xbox_button_view", W }, { "xbox_ls", W }, { "xbox_rs", W }, { "xbox_button_menu", W },
            { "xbox_dpad_up", W }, { "xbox_dpad_right", W }, { "xbox_dpad_down", W }, { "xbox_dpad_left", W },
            { "xbox_lt", W }, { "xbox_rt", W }, { "xbox_lb", W }, { "xbox_rb", W },
            { "xbox_button_color_y", W }, { "xbox_button_color_b", W }, { "xbox_button_color_a", W },
            { "xbox_button_color_x", W } };
        return p[bit];
    }
    case S_NINTENDO: {   // by label (SDL's label mode): Cross = A, Circle = B, Square = X, Triangle = Y
        static const Pick p[16] = { { "switch_button_minus", W }, { "switch_stick_l", W }, { "switch_stick_r", W },
            { "switch_button_plus", W }, { "switch_dpad_up", W }, { "switch_dpad_right", W }, { "switch_dpad_down", W },
            { "switch_dpad_left", W }, { "switch_button_zl", W }, { "switch_button_zr", W }, { "switch_button_l", W },
            { "switch_button_r", W }, { "switch_button_y", W }, { "switch_button_b", W }, { "switch_button_a", W },
            { "switch_button_x", W } };
        return p[bit];
    }
    case S_STEAM: {
        static const Pick p[16] = { { "steam_button_back_icon", W }, { "xbox_ls", W }, { "xbox_rs", W },
            { "steam_button_start_icon", W }, { "steam_dpad_up", W }, { "steam_dpad_right", W }, { "steam_dpad_down", W },
            { "steam_dpad_left", W }, { "steam_lt", W }, { "steam_rt", W }, { "steam_lb", W }, { "steam_rb", W },
            { "steam_button_y", W }, { "steam_button_b", W }, { "steam_button_a", W }, { "steam_button_x", W } };
        return p[bit];
    }
    default: {   // generic: the face buttons by their place, the rest neutral
        static const Pick p[16] = { { "steam_button_back_icon", W }, { "xbox_ls", W }, { "xbox_rs", W },
            { "steam_button_start_icon", W }, { "xbox_dpad_up", W }, { "xbox_dpad_right", W }, { "xbox_dpad_down", W },
            { "xbox_dpad_left", W }, { "controller_button_l2", W }, { "controller_button_r2", W }, { "controller_button_l1", W },
            { "controller_button_r1", W }, { "switch_buttons_up", 0xE84C5C }, { "switch_buttons_right", 0xE84C5C },
            { "switch_buttons_down", 0xE84C5C }, { "switch_buttons_left", 0xE84C5C } };
        return p[bit];
    }
    }
}
const char* art_pick(int style, int b)   // the direction pad's and the sticks' pictures
{
    if (b == B_DPAD) return style == S_NINTENDO ? "switch_dpad" : style == S_STEAM ? "steam_dpad" : "xbox_dpad";
    if (b == B_LSTICK) return style == S_XBOX ? "xbox_stick_l" : "switch_stick_l";
    return style == S_XBOX ? "xbox_stick_r" : "switch_stick_r";
}
// a key's Kenney file by its SDL name ("" when there is none)
std::string key_file(const std::string& n0)
{
    std::string n;
    for (char ch : n0) n.push_back((char)tolower((unsigned char)ch));
    if (n.size() == 1 && (isalnum((unsigned char)n[0]))) return "keyboard_" + n;
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'f' && isdigit((unsigned char)n[1])) return "keyboard_" + n;
    static const std::pair<const char*, const char*> m[] = {
        { "return", "enter" }, { "enter", "enter" }, { "tab", "tab" }, { "space", "space" }, { "escape", "escape" },
        { "backspace", "backspace" }, { "left shift", "shift" }, { "right shift", "shift" }, { "left ctrl", "ctrl" },
        { "right ctrl", "ctrl" }, { "left alt", "alt" }, { "right alt", "alt" }, { "up", "arrow_up" }, { "down", "arrow_down" },
        { "left", "arrow_left" }, { "right", "arrow_right" }, { "capslock", "capslock" }, { "insert", "insert" },
        { "delete", "delete" }, { "home", "home" }, { "end", "end" }, { "pageup", "page_up" }, { "pagedown", "page_down" },
        { "-", "minus" }, { "=", "equals" }, { "[", "bracket_open" }, { "]", "bracket_close" }, { ";", "semicolon" },
        { "'", "apostrophe" }, { ",", "comma" }, { ".", "period" }, { "/", "slash_forward" }, { "\\", "slash_back" },
        { "`", "tilde" }, { "keypad enter", "numpad_enter" }, { "keypad +", "numpad_plus" }, { "numlock", "numlock" },
        { "printscreen", "printscreen" }, { "scrolllock", "scroll_lock" }, { "pause", "pause" } };
    for (const auto& e : m) if (n == e.first) return std::string("keyboard_") + e.second;
    return "";
}

int effective_style()
{
    const int c = g_choice.load(std::memory_order_relaxed);
    if (c >= 0) return c;
    int d = cellPad_ui_device();
    if (d < 0) d = cellPad_connected_family();
    return d < 0 || d >= S_COUNT ? S_KEYBOARD : d;
}
bool keys_are_arrows()
{
    const char* want[4] = { "Up", "Right", "Down", "Left" };
    for (int i = 0; i < 4; i++) { const char* k = cellPad_key_binding_name(4 + i); if (!k || strcmp(k, want[i])) return false; }
    return true;
}
int pad_bit_for(int game_bit)
{
    const uint32_t m = cellPad_pad_binding(game_bit);
    for (int i = 0; i < 16; i++) if (m >> i & 1) return i;
    return game_bit;
}
bool pad_default()
{
    for (int i = 0; i < 16; i++) if (cellPad_pad_binding(i) != 1u << i) return false;
    return true;
}

// the glyph for one of the game's buttons now, in `style` (g_mu held); nullptr: leave the cell as it is
const Rgba* glyph_for(int style, int b)
{
    if (style == S_PLAYSTATION) {
        if (b >= 16) return nullptr;   // a pad's or stick's art: not remappable
        const int p = pad_bit_for(b);
        return p == b || !g_ps_art[p].w ? nullptr : &g_ps_art[p];
    }
    if (style == S_KEYBOARD) {
        if (b == B_DPAD || b == B_LSTICK) {   // the keyboard's movement: the direction keys
            if (keys_are_arrows()) return glyph_file("keyboard_arrows", 0xF0F0F0);
            const char* up = cellPad_key_binding_name(B_UP);
            const std::string f = key_file(up ? up : "");
            return f.empty() ? glyph_keycap(up ? up : "?") : glyph_file(f, 0xF0F0F0);
        }
        if (b == B_RSTICK) return glyph_file(art_pick(S_GENERIC, b), 0xF0F0F0);   // no key moves it: the stick shown
        const char* k = cellPad_key_binding_name(b);
        const std::string name = k ? k : "";
        const std::string f = key_file(name);
        const Rgba* g = f.empty() ? nullptr : glyph_file(f, 0xF0F0F0);
        return g ? g : glyph_keycap(name.empty() ? "?" : name);
    }
    if (b >= 16) return glyph_file(art_pick(style, b), 0xF0F0F0);
    const Pick p = pad_pick(style, pad_bit_for(b));
    return glyph_file(p.file, p.ink);
}

uint64_t mixs(uint64_t x, const char* s) { for (; s && *s; s++) { x ^= (uint8_t)*s; x *= 1099511628211ull; } return x * 31 + 7; }
uint64_t mixu(uint64_t x, uint64_t v) { x ^= v; return x * 1099511628211ull; }

}  // namespace

bool ppfx_prompt_patch_for_test(uint64_t id, uint32_t bc, uint32_t w, uint32_t h, const uint8_t* level0,
                                std::vector<std::vector<uint8_t>>& levels);   // below (the self-test)

// ---- the module's own calls ----

void set_choice(const char* v)
{
    int c = -1;
    if (v) for (int i = 0; i < S_COUNT; i++) if (!strcmp(v, kStyleNames[i])) c = i;
    if (v && !strcmp(v, "switch")) c = S_NINTENDO;
    g_choice.store(c, std::memory_order_relaxed);
}
int style_now() { return effective_style(); }
const char* style_name(int s) { return s >= 0 && s < S_COUNT ? kStyleNames[s] : "keyboard"; }

// the player's own files: every table texture's and every font's fingerprint, and the game's art of each button
void start(const char* elf)
{
    // beta.6: the folders the game reads its data from, in its own order. The official update's comes first: with
    // v1.01 the game's /dev_hdd0/game/<title>/ is the update's folder (PS3_GAME_UPDATE_DIR, port/main.cpp), holding
    // only the files the update replaces (two fonts and the character select's picture; their button cells are the
    // disc's, pixel for pixel). The disc's comes next (PS3_VFS_ROOT, as port/devtools does), else the program's own
    // folder. The program's folder alone was v1.01's update folder: 2 of the 40 textures were recognised, and the
    // naming screen and every other UI picture kept the PlayStation buttons whatever the style chosen.
    std::vector<std::string> roots;
    auto add_root = [&](std::string r) {
        for (char& ch : r) if (ch == '\\') ch = '/';
        if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
    };
    if (const char* u = getenv("PS3_GAME_UPDATE_DIR"); u && *u) add_root(std::string(u) + "/USRDIR/data/");
    {
        std::string d = elf ? elf : "";
        const size_t sl = d.find_last_of("/\\");
        add_root((sl == std::string::npos ? std::string(".") : d.substr(0, sl)) + "/data/");
    }
    if (const char* v = getenv("PS3_VFS_ROOT"); v && *v) add_root(std::string(v) + "/PS3_GAME/USRDIR/data/");
    int found = 0, missing = 0;
    std::unordered_map<std::string, std::pair<std::vector<uint8_t>, std::vector<TpkEntry>>> files;
    auto open = [&](const std::string& root, const std::string& pkg) -> std::pair<std::vector<uint8_t>, std::vector<TpkEntry>>* {
        const std::string path = root + pkg + ".ps3.tpk";
        auto it = files.find(path);
        if (it != files.end()) return it->second.second.empty() ? nullptr : &it->second;
        auto& f = files[path];
        if (!read_file(path, f.first) || !parse_tpk(f.first, f.second)) f.second.clear();
        return f.second.empty() ? nullptr : &f;
    };
    auto level0 = [&](const std::vector<uint8_t>& b, const TpkEntry& e, uint32_t bc) -> const uint8_t* {
        return bc && (size_t)e.off + level0_bytes(bc, e.w, e.h) <= b.size() ? &b[e.off] : nullptr;
    };
    const auto& tab = table();
    for (size_t i = 0; i < tab.size(); i++) {
        bool any = false;
        for (const std::string& root : roots) {   // each copy the game may upload: the update's and the disc's
            auto* f = open(root, tab[i].package);
            const TpkEntry* e = nullptr;
            if (f) for (const TpkEntry& t : f->second) if (t.name == std::string(tab[i].name) + ".dds" || t.name == tab[i].name) e = &t;
            const uint32_t bc = e ? bc_of_tpk(e->fmt) : 0;
            const uint8_t* p = e ? level0(f->first, *e, bc) : nullptr;
            if (!p) continue;
            const uint64_t fp = fingerprint(bc, e->w, e->h, p, level0_bytes(bc, e->w, e->h));
            if (!g_known.count(fp)) g_known[fp].tex = (int)i;
            g_dims.push_back(dims_key(bc, e->w, e->h));
            any = true;
            // the game's art of each button, from the first texture showing it on a clear background
            std::vector<uint8_t> px;
            for (const Cell& c : tab[i].cells) {
                if (c.button >= B_COUNT || g_ps_art[c.button].w) continue;
                if (px.empty()) decode_bc(bc, e->w, e->h, p, px);
                const Box cb = { c.x0, c.y0, c.x1, c.y1 };
                uint8_t bg[4];
                background(px, (int)e->w, (int)e->h, cb, bg);
                Rgba art = crop(px, (int)e->w, art_box(px, (int)e->w, cb, bg));
                if (bg[3] >= 40)   // on a panel: the panel's own colour made clear
                    for (size_t k = 0; k < art.px.size(); k += 4) if (!differs(&art.px[k], bg)) art.px[k + 3] = 0;
                g_ps_art[c.button] = std::move(art);
            }
        }
        any ? found++ : missing++;
    }
    for (int i = 0; i < kFontPackageCount; i++) {
        bool any = false;
        for (const std::string& root : roots) {
            auto* f = open(root, kFontPackages[i]);
            const TpkEntry* e = f && !f->second.empty() ? &f->second[0] : nullptr;
            const uint32_t bc = e ? bc_of_tpk(e->fmt) : 0;
            const uint8_t* p = e ? level0(f->first, *e, bc) : nullptr;
            if (!p) continue;
            const uint64_t fp = fingerprint(bc, e->w, e->h, p, level0_bytes(bc, e->w, e->h));
            g_known[fp].font = i;
            g_dims.push_back(dims_key(bc, e->w, e->h));
            any = true;
        }
        any ? found++ : missing++;
    }
    std::sort(g_dims.begin(), g_dims.end());
    g_dims.erase(std::unique(g_dims.begin(), g_dims.end()), g_dims.end());
    g_ready = true;
    // DOTHEROES_PROMPT_SELFTEST=<dir> (development): every table texture and font redrawn from the files in each style
    // but PlayStation's, written as <dir>/<style>__<package>__<name>.rgba (width, height as two little-endian words,
    // then RGBA) -- every cell checked without visiting its screen (build/m11 pr_selftest.py makes the pictures)
    if (const char* st = getenv("DOTHEROES_PROMPT_SELFTEST")) {
        const int keep = g_choice.load();
        int n = 0;
        for (int s = 0; s < S_COUNT; s++) {
            if (s == S_PLAYSTATION) continue;
            g_choice.store(s);
            auto one = [&](const std::string& pkg, const std::string& name, const TpkEntry& e, const std::vector<uint8_t>& b) {
                const uint32_t bc = bc_of_tpk(e.fmt);
                const uint8_t* p = level0(b, e, bc);
                if (!p) return;
                const uint64_t fp = fingerprint(bc, e.w, e.h, p, level0_bytes(bc, e.w, e.h));
                std::vector<std::vector<uint8_t>> lv;
                if (!ppfx_prompt_patch_for_test(fp, bc, e.w, e.h, p, lv)) return;
                std::string fn = std::string(kStyleNames[s]) + "__" + pkg + "__" + name + ".rgba";
                for (char& ch : fn) if (ch == '/') ch = '_';
                if (FILE* f = fopen((std::string(st) + "/" + fn).c_str(), "wb")) {
                    const uint32_t wh[2] = { e.w, e.h };
                    fwrite(wh, 4, 2, f);
                    fwrite(lv[0].data(), 1, lv[0].size(), f);
                    fclose(f);
                    n++;
                }
            };
            auto first = [&](const std::string& pkg) {   // the copy the game uploads: the first folder holding it
                for (const std::string& root : roots) if (auto* f = open(root, pkg)) return f;
                return (std::pair<std::vector<uint8_t>, std::vector<TpkEntry>>*)nullptr;
            };
            for (size_t i = 0; i < tab.size(); i++) {
                auto* f = first(tab[i].package);
                if (f) for (const TpkEntry& t : f->second) if (t.name == std::string(tab[i].name) + ".dds") one(tab[i].package, tab[i].name, t, f->first);
            }
            for (int i = 0; i < kFontPackageCount; i++) {
                auto* f = first(kFontPackages[i]);
                if (f && !f->second.empty()) one(kFontPackages[i], "font", f->second[0], f->first);
            }
        }
        g_choice.store(keep);
        if (dh_log) dh_log(2, "prompts", "self-test: %d textures written to %s", n, st);
    }
    if (dh_log) dh_log(2, "prompts", "the game's prompt textures: %d recognised from its files, %d not found (%zu kinds; %zu "
                       "folder%s: %s)", found, missing, g_dims.size(), roots.size(), roots.size() == 1 ? "" : "s",
                       getenv("PS3_GAME_UPDATE_DIR") ? "the update's, then the disc's" : "the disc's");
}

// the style and the bindings now, as one number (a change re-uploads the prompt textures)
uint64_t signature()
{
    const int s = effective_style();
    uint64_t x = mixu(0x9E3779B97F4A7C15ull, (uint64_t)s + 1);
    if (s == S_KEYBOARD) for (int i = 0; i < 16; i++) x = mixs(x, cellPad_key_binding_name(i));
    else for (int i = 0; i < 16; i++) x = mixu(x, cellPad_pad_binding(i));
    return x ? x : 1;
}

// ---- the words: the game's own Lua (developer tools' title tick, every 30 flips; port/devtools/devtools.cpp) ----
namespace {
std::string fw(const char* ascii)   // full-width, as the game writes its text
{
    std::string o;
    for (const char* p = ascii; *p; p++) {
        unsigned c = (unsigned char)*p;
        unsigned u = c == ' ' ? 0x3000u : c >= 0x21 && c <= 0x7E ? c - 0x21 + 0xFF01u : c;
        if (u < 0x80) o.push_back((char)u);
        else { o.push_back((char)(0xE0 | u >> 12)); o.push_back((char)(0x80 | (u >> 6 & 63))); o.push_back((char)(0x80 | (u & 63))); }
    }
    return o;
}
}  // namespace

// The chunk for the game's Lua now: the title's "PRESS START BUTTON" (PlayStation: as it is; else "PRESS" and the
// START glyph, which the font draws in the style), and the guard's lesson's words for the keyboard ("the left stick",
// "the directional buttons": the direction keys). Installed once (a hook on the Title table's assignment -- the title
// loads its script each time it opens -- and on Event:Print), then only the mode changes. Our own words.
std::string lua_chunk()
{
    const int s = effective_style();
    const char* mode = s == S_PLAYSTATION ? "ps" : s == S_KEYBOARD ? "kb" : "pad";
    const std::string keys = fw(keys_are_arrows() ? "arrow keys" : "direction keys");
    std::string c;
    c += "__dh_pm='" + std::string(mode) + "' ";
    c += "__dh_pk='" + keys + "' ";
    c += "__dh_ls='" + fw("left stick") + "' __dh_db='" + fw("directional buttons") + "' ";
    c += "__dh_pt='" + fw("PRESS ") + "\xE2\x8A\x86\xE2\x8A\x87' ";   // the START glyph: U+2286 U+2287 (two cells)
    c += "function __dh_fixt(v) if type(v)=='table' and type(v.Start)=='table' and type(v.Start.String)=='string' then "
         "if not __dh_t0 then __dh_t0=v.Start.String end local w=(__dh_pm=='ps') and __dh_t0 or __dh_pt "
         "if v.Start.String~=w then v.Start.String=w end end end ";
    c += "if not __dh_g then __dh_g=1 local mt=getmetatable(_G) or {} local pi,pn=mt.__index,mt.__newindex "
         "__dh_tt=rawget(_G,'Title') rawset(_G,'Title',nil) "
         "mt.__index=function(t,k) if k=='Title' then return __dh_tt end if type(pi)=='function' then return pi(t,k) "
         "elseif pi then return pi[k] end end "
         "mt.__newindex=function(t,k,v) if k=='Title' then __dh_fixt(v) __dh_tt=v return end if pn then return pn(t,k,v) end "
         "rawset(t,k,v) end setmetatable(_G,mt) end ";
    c += "__dh_fixt(__dh_tt) ";
    // Event is the engine's object: its metatable's __index (a function of the engine's) finds its methods -- asked for
    // Print once, then answering Print with the wrapper and everything else as before
    c += "if not __dh_pp and EventClass and EventClass.Event then local E=EventClass.Event local m=getmetatable(E) "
         "if type(m)=='table' then local oi=m.__index local f=nil "
         "if type(oi)=='function' then f=oi(E,'Print') elseif type(oi)=='table' then f=oi.Print end "
         "if f then __dh_pp=f local w=function(self,s,...) "
         "if __dh_pm=='kb' and type(s)=='string' then s=string.gsub(s,__dh_ls,__dh_pk) s=string.gsub(s,__dh_db,__dh_pk) end "
         "return __dh_pp(self,s,...) end "
         "if type(oi)=='function' then m.__index=function(t,k) if k=='Print' then return w end return oi(t,k) end "
         "else oi.Print=w end end end end ";
    c += "return (__dh_pp and 1 or 0)+(__dh_g and 2 or 0)";
    return c;
}

}  // namespace prompts

extern "C" void prompts_set_choice(const char* v) { prompts::set_choice(v); }

// ---- the attract demo's video (PlayDemoMovieUS: a recording of play, its HUD's PlayStation buttons in the picture) ----
// Where its HUD shows them (1280x720; our measurement of the recording, build/m11 pr_demo_locate.py): the sword and
// item slots, the item cycle's L2 and R2, and the key guide in its two layouts (without and with DASH). Each frame,
// each place is checked (2 px around) against the game's own art of that button from the player's files (normalised
// correlation of the luma: the video's own scale and offset), and where it is found the button is drawn again in the
// style, in the frame's own planes (BT.601). Nothing where the HUD is not shown (a cut, a fade). M11.2g.
namespace {
struct DemoSpot { int x, y, button; float need; };
const DemoSpot kDemoSpots[] = {
    { 716, 106, prompts::B_CROSS, 0.80f }, { 826, 106, prompts::B_CIRCLE, 0.80f },
    { 806, 54, prompts::B_L2, 0.62f },     { 903, 54, prompts::B_R2, 0.62f },
    { 760, 640, prompts::B_L1, 0.80f },    { 883, 639, prompts::B_R1, 0.80f },   { 1052, 637, prompts::B_TRIANGLE, 0.80f },
    { 625, 640, prompts::B_L1, 0.80f },    { 748, 639, prompts::B_R1, 0.80f },   { 916, 637, prompts::B_TRIANGLE, 0.80f },
    { 1049, 637, prompts::B_SQUARE, 0.80f },
};
struct Sample { int x, y; float v; };
std::vector<Sample> demo_samples(int b)   // the art's opaque pixels' luma, at most ~60 of them
{
    static std::vector<Sample> s[prompts::B_COUNT];
    static bool made[prompts::B_COUNT] = {};
    if (made[b]) return s[b];
    made[b] = true;
    const auto& a = prompts::g_ps_art[b];
    std::vector<Sample> all;
    for (int y = 0; y < a.h; y++)
        for (int x = 0; x < a.w; x++) {
            const uint8_t* p = &a.px[((size_t)y * a.w + x) * 4];
            if (p[3] >= 250) all.push_back({ x, y, 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] });
        }
    const size_t step = std::max<size_t>(1, all.size() / 60);
    for (size_t i = 0; i < all.size(); i += step) s[b].push_back(all[i]);
    return s[b];
}
}  // namespace

extern "C" void prompts_movie_frame(const char* name, uint8_t* yp, uint32_t ypitch, uint8_t* cr, uint8_t* cb, uint32_t cpitch,
                                    int w, int h)
{
    using namespace prompts;
    if (!g_ready || !name || !strstr(name, "PlayDemo") || w != 1280 || h != 720) return;
    const int style = effective_style();
    if (style == S_PLAYSTATION && pad_default()) return;
    for (const DemoSpot& d : kDemoSpots) {
        const auto& a = g_ps_art[d.button];
        const std::vector<Sample> t = demo_samples(d.button);
        if (!a.w || t.size() < 12) continue;
        float tm = 0;
        for (const Sample& s : t) tm += s.v;
        tm /= t.size();
        float best = -1; int bx = 0, by = 0;
        for (int oy = -3; oy <= 3; oy++)
            for (int ox = -3; ox <= 3; ox++) {
                const int X = d.x + ox, Y = d.y + oy;
                if (X < 0 || Y < 0 || X + a.w > w || Y + a.h > h) continue;
                float fm = 0;
                for (const Sample& s : t) fm += yp[(size_t)(Y + s.y) * ypitch + X + s.x];
                fm /= t.size();
                float num = 0, df = 0, dt = 0;
                for (const Sample& s : t) {
                    const float f = yp[(size_t)(Y + s.y) * ypitch + X + s.x] - fm, g = s.v - tm;
                    num += f * g; df += f * f; dt += g * g;
                }
                const float c = df > 0 && dt > 0 ? num / sqrtf(df * dt) : -1;
                if (c > best) { best = c; bx = X; by = Y; }
            }
        if (best < d.need) continue;
        std::lock_guard<std::mutex> g(g_mu);
        const Rgba* gl = glyph_for(style, d.button);
        if (!gl) continue;
        // the art's box cleared to its surroundings (the ring's average), then the glyph drawn over in RGBA, then
        // the box written back as Y, Cr, Cb (BT.601, video range)
        const int x0 = std::max(0, bx - 1), y0 = std::max(0, by - 1), x1 = std::min(w, bx + a.w + 1), y1 = std::min(h, by + a.h + 1);
        const int bw = x1 - x0, bh = y1 - y0;
        double ry = 0, rcr = 0, rcb = 0; int rn = 0;
        for (int x = x0 - 1; x <= x1; x++)
            for (int y : { y0 - 2, y1 + 1 })
                if (x >= 0 && y >= 0 && x < w && y < h) { ry += yp[(size_t)y * ypitch + x]; rcr += cr[(size_t)(y / 2) * cpitch + x / 2]; rcb += cb[(size_t)(y / 2) * cpitch + x / 2]; rn++; }
        if (!rn) continue;
        ry /= rn; rcr /= rn; rcb /= rn;
        // the background as RGB (video range BT.601 back), the box an RGBA canvas
        const double Yv = (ry - 16) * 1.164, Cr = rcr - 128, Cb = rcb - 128;
        const uint8_t bg[4] = { (uint8_t)std::clamp(Yv + 1.596 * Cr, 0.0, 255.0), (uint8_t)std::clamp(Yv - 0.813 * Cr - 0.391 * Cb, 0.0, 255.0),
                                (uint8_t)std::clamp(Yv + 2.018 * Cb, 0.0, 255.0), 255 };
        // the box as the frame has it (RGB from its planes), the old button's own pixels (its art's coverage, a pixel
        // wider for the video's blur) set to the surroundings, the icon drawn over: no box around it
        std::vector<uint8_t> box((size_t)bw * bh * 4);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                const double Y2 = (yp[(size_t)(y0 + y) * ypitch + x0 + x] - 16) * 1.164;
                const double R2 = cr[(size_t)((y0 + y) / 2) * cpitch + (x0 + x) / 2] - 128.0;
                const double B2 = cb[(size_t)((y0 + y) / 2) * cpitch + (x0 + x) / 2] - 128.0;
                uint8_t* o = &box[((size_t)y * bw + x) * 4];
                o[0] = (uint8_t)std::clamp(Y2 + 1.596 * R2, 0.0, 255.0);
                o[1] = (uint8_t)std::clamp(Y2 - 0.813 * R2 - 0.391 * B2, 0.0, 255.0);
                o[2] = (uint8_t)std::clamp(Y2 + 2.018 * B2, 0.0, 255.0);
                o[3] = 255;
            }
        // ...filled from what surrounds them: each pass, every pixel still to fill that touches known ones takes their
        // average, until none is left (the picture behind the HUD is blurred: this reads as it)
        std::vector<uint8_t> known((size_t)bw * bh, 1);
        int todo = 0;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                bool covered = false;
                for (int dy = -1; dy <= 1 && !covered; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int u = x + x0 - bx + dx, v = y + y0 - by + dy;
                        if (u >= 0 && v >= 0 && u < a.w && v < a.h && a.px[((size_t)v * a.w + u) * 4 + 3] > 24) { covered = true; break; }
                    }
                if (covered && x > 0 && y > 0 && x < bw - 1 && y < bh - 1) { known[(size_t)y * bw + x] = 0; todo++; }
            }
        while (todo > 0) {
            std::vector<uint8_t> now = known;
            int filled = 0;
            for (int y = 1; y < bh - 1; y++)
                for (int x = 1; x < bw - 1; x++) {
                    if (known[(size_t)y * bw + x]) continue;
                    int s[3] = { 0, 0, 0 }, n = 0;
                    for (int k = 0; k < 4; k++) {
                        const int u = x + (k == 0) - (k == 1), v = y + (k == 2) - (k == 3);
                        if (!known[(size_t)v * bw + u]) continue;
                        const uint8_t* q = &box[((size_t)v * bw + u) * 4];
                        s[0] += q[0]; s[1] += q[1]; s[2] += q[2]; n++;
                    }
                    if (!n) continue;
                    uint8_t* o = &box[((size_t)y * bw + x) * 4];
                    o[0] = (uint8_t)(s[0] / n); o[1] = (uint8_t)(s[1] / n); o[2] = (uint8_t)(s[2] / n); o[3] = 255;
                    now[(size_t)y * bw + x] = 1;
                    filled++;
                }
            known.swap(now);
            todo -= filled;
            if (!filled) break;
        }
        draw_fit(box, bw, bh, Box{ bx - x0, by - y0, bx - x0 + a.w, by - y0 + a.h }, *gl);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                const uint8_t* p = &box[((size_t)y * bw + x) * 4];
                yp[(size_t)(y0 + y) * ypitch + x0 + x] = (uint8_t)std::clamp(16 + 0.257 * p[0] + 0.504 * p[1] + 0.098 * p[2], 16.0, 235.0);
            }
        for (int y = y0 / 2; y < (y1 + 1) / 2; y++)
            for (int x = x0 / 2; x < (x1 + 1) / 2; x++) {
                double r = 0, gg = 0, b = 0; int n = 0;
                for (int k = 0; k < 4; k++) {
                    const int px = x * 2 + (k & 1) - x0, py = y * 2 + (k >> 1) - y0;
                    if (px < 0 || py < 0 || px >= bw || py >= bh) continue;
                    const uint8_t* p = &box[((size_t)py * bw + px) * 4];
                    r += p[0]; gg += p[1]; b += p[2]; n++;
                }
                if (!n) continue;
                r /= n; gg /= n; b /= n;
                cr[(size_t)y * cpitch + x] = (uint8_t)std::clamp(128 + 0.439 * r - 0.368 * gg - 0.071 * b, 16.0, 240.0);
                cb[(size_t)y * cpitch + x] = (uint8_t)std::clamp(128 - 0.148 * r - 0.291 * gg + 0.439 * b, 16.0, 240.0);
            }
    }
}

// ---- the renderer's calls (port/native/render/ppfx_native.h) ----
namespace ppfx {

uint64_t prompt_sig() { return prompts::g_ready ? prompts::signature() : 0; }

uint64_t prompt_texture_id(uint32_t bc, uint32_t w, uint32_t h, const uint8_t* level0, size_t bytes)
{
    using namespace prompts;
    if (!g_ready || !bc || !std::binary_search(g_dims.begin(), g_dims.end(), dims_key(bc, w, h))) return 0;
    if (bytes < level0_bytes(bc, w, h)) return 0;
    const uint64_t fp = fingerprint(bc, w, h, level0, level0_bytes(bc, w, h));
    return g_known.count(fp) ? fp : 0;
}

// the texture's first level as the player sees it now (RGBA), every level made from it; false: leave it as it is
bool prompt_texture_patch(uint64_t id, uint32_t bc, uint32_t w, uint32_t h, uint32_t nlv, const uint8_t* level0,
                          std::vector<std::vector<uint8_t>>& levels)
{
    using namespace prompts;
    const auto t0 = std::chrono::steady_clock::now();
    auto it = g_known.find(id);
    if (it == g_known.end()) return false;
    const int style = effective_style();
    if (style == S_PLAYSTATION && pad_default()) return false;   // the game's own art, untouched
    std::lock_guard<std::mutex> g(g_mu);
    std::vector<uint8_t> px;
    decode_bc(bc, w, h, level0, px);
    const int W = (int)w, H = (int)h;
    bool changed = false;
    auto redraw = [&](const Box& cell, const Box* fixed, int button) {
        const Rgba* gl = glyph_for(style, button);
        if (!gl) return;
        uint8_t bg[4];
        background(px, W, H, cell, bg);
        const Box art = fixed ? *fixed : art_box(px, W, cell, bg);
        // the old art cleared to the background: the cell, and what of it spills a pixel over the cell's edge
        for (int y = std::max(0, cell.y0 - 1); y < std::min(H, cell.y1 + 1); y++)
            for (int x = std::max(0, cell.x0 - 1); x < std::min(W, cell.x1 + 1); x++) {
                uint8_t* p = &px[((size_t)y * W + x) * 4];
                const bool inside = x >= cell.x0 && x < cell.x1 && y >= cell.y0 && y < cell.y1;
                if (inside || (!fixed && differs(p, bg))) memcpy(p, bg, 4);
            }
        draw_fit(px, W, H, art, *gl);
        changed = true;
    };
    if (it->second.tex >= 0) {
        for (const Cell& c : table()[it->second.tex].cells) {
            const Box cb = { c.x0, c.y0, c.x1, c.y1 };
            redraw(cb, nullptr, c.button);
        }
    } else {
        // the fonts: each glyph's box as the font's own Circle (one cell) and wide R1 (two) sit in theirs, so every
        // replaced glyph has the size the text was laid out for (a font with "?" in some cells too)
        auto cell_of = [&](int index, int n) { const int x = (index % 16) * 32, y = (index / 16) * 32; return Box{ x, y, x + 32 * n, y + 32 }; };
        auto rel = [&](int index, int n) {
            const Box c = cell_of(index, n);
            uint8_t clear[4] = { 0, 0, 0, 0 };
            Box b = art_box(px, W, c, clear);
            return Box{ b.x0 - c.x0, b.y0 - c.y0, b.x1 - c.x0, b.y1 - c.y0 };
        };
        // a wide glyph is two characters: the text puts the second one an advance after the first -- 20 pixels (the
        // fonts' FontWidth; measured: the game's own R1 is 39 wide on screen, its halves 21 and 19) -- so the room it
        // keeps runs from the left cell's art to the right cell's art's end, 20 further on (the box below)
        const int adv = 20;
        const Box one = rel(248, 1);
        Box two;
        {
            const Box l = rel(647, 1);
            Box r = rel(648, 1);
            two = { l.x0, std::min(l.y0, r.y0), adv + r.x1, std::max(l.y1, r.y1) };
        }
        for (const FontGlyph& f : font_glyphs()) {
            const Box c = cell_of(f.index, f.cells);
            if (c.y1 > H || c.x1 > W) continue;
            const Rgba* gl = glyph_for(style, f.button);
            if (!gl) continue;
            for (int y = c.y0; y < c.y1; y++) memset(&px[((size_t)y * W + c.x0) * 4], 0, (size_t)(c.x1 - c.x0) * 4);
            if (f.cells == 1) {
                draw_fit(px, W, H, Box{ c.x0 + one.x0, c.y0 + one.y0, c.x0 + one.x1, c.y0 + one.y1 }, *gl);
            } else {
                // the whole icon in the first character's cell, centred in the room the text keeps (both advances, as
                // far as that cell reaches), the second cell left empty: the text draws each character's dark outline
                // and then it, one character after the other, so the second half's outline would cross a light icon
                // where the halves meet (the game's dark R1 hides it; a seam at a larger rendering resolution)
                draw_fit(px, W, H, Box{ c.x0 + two.x0, c.y0 + two.y0, c.x0 + std::min(two.x1, 32), c.y0 + two.y1 }, *gl);
            }
            changed = true;
        }
    }
    if (!changed) return false;
    {   // said (the first 60): which texture, in which style -- the tests count them (none in PlayStation's own art)
        static std::atomic<int> said{ 0 };
        if (said.fetch_add(1) < 60)
            fprintf(stderr, "[prompts] redrawn: %s %ux%u in the %s style (%.2f ms)\n",
                    it->second.tex >= 0 ? table()[it->second.tex].name : kFontPackages[it->second.font], w, h, kStyleNames[style],
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    levels.clear();
    levels.push_back(std::move(px));
    uint32_t lw = w, lh = h;
    for (uint32_t m = 1; m < nlv; m++) {   // each level the 2x2 average of the one above (premultiplied)
        const std::vector<uint8_t>& s = levels.back();
        const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        std::vector<uint8_t> d((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++) {
                double acc[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < 4; k++) {
                    const uint32_t sx = std::min(lw - 1, x * 2 + (k & 1)), sy = std::min(lh - 1, y * 2 + (k >> 1));
                    const uint8_t* p = &s[((size_t)sy * lw + sx) * 4];
                    const double a = p[3] / 255.0;
                    acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
                }
                uint8_t* o = &d[((size_t)y * nw + x) * 4];
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)(acc[3] > 0 ? acc[c] / acc[3] + 0.5 : 0);
                o[3] = (uint8_t)(acc[3] / 4 * 255 + 0.5);
            }
        levels.push_back(std::move(d));
        lw = nw; lh = nh;
    }
    return true;
}

}  // namespace ppfx

bool prompts::ppfx_prompt_patch_for_test(uint64_t id, uint32_t bc, uint32_t w, uint32_t h, const uint8_t* level0,
                                         std::vector<std::vector<uint8_t>>& levels)
{
    return ppfx::prompt_texture_patch(id, bc, w, h, 1, level0, levels);
}
