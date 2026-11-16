// M9.2a: the shader programs on the player's own disc (shader_discovery.h). Read-only; no program bytes leave
// the process except as the renderer's own preparation uses them.
#include "shader_discovery.h"

#include <map>
#include <memory>
#include <set>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>

#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"

namespace sd {

const char* source_name(int s)
{
    static const char* n[] = { "package", "package_0", "sas", "sas_2", "eboot_cgb", "eboot_cg" };
    return s >= 0 && s < kSources ? n[s] : "?";
}

const char* source_file(int s)
{
    static const char* f[] = { "data/shaders/shaders.ps3.pkg", "data/shaders/shaders_0.ps3.pkg",
                               "data/shaders/SasShader.ps3.spk", "data/shaders/SasShader2.ps3.spk", "EBOOT.elf",
                               "EBOOT.elf" };
    return s >= 0 && s < kSources ? f[s] : "";
}

uint64_t fnv64(const void* p, size_t n, uint64_t h)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)((uint32_t)p[0] << 8 | p[1]); }

bool read_file(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n >= 0 && (n == 0 || fread(&out[0], 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return ok;
}

// a vertex program as the renderer is handed it: each 32-bit word's bytes reversed (little-endian words)
std::string words_reversed(const uint8_t* p, size_t n)
{
    n -= n % 4;
    std::string out(n, '\0');
    for (size_t i = 0; i < n; i += 4) {
        out[i] = (char)p[i + 3]; out[i + 1] = (char)p[i + 2]; out[i + 2] = (char)p[i + 1]; out[i + 3] = (char)p[i];
    }
    return out;
}

// the renderer's fp_control (ppfx_render.cpp): the register count (at least 2) above the header's control bits
uint32_t fp_ctrl_of(uint32_t regs, uint32_t bits) { return (regs < 2 ? 2u : regs) << 24 | (bits & 0xFFFFu); }

// the derived fields, as the renderer computes them (asp's pipeline: the inputs analysis, fp_info)
void finish(Program& p)
{
    const uint8_t* c = (const uint8_t*)p.code.data();
    const uint32_t n = (uint32_t)p.code.size();
    if (p.stage == 0) {
        rsx_vp_input_analysis an = { 0xFFFFu, 0 };
        const int instrs = rsx_vp_analyze_inputs(c, n, &an);
        p.ok = instrs > 0 && (uint32_t)instrs * 16u == n;
        if (p.ok && an.exact && an.input_mask) { p.inputs = an.input_mask; p.inputs_exact = 1; }
        p.ident = fnv64(c, n);
    } else {
        static std::unique_ptr<rsx_fp_constant_block> cb(new rsx_fp_constant_block);
        cb->count = 0;
        const int k = rsx_fp_collect_constants(c, n, cb.get());
        p.ok = k >= 0;
        p.consts = k >= 0 ? (uint32_t)k : 0;
        p.units = rsx_fp_texture_units(c, n);
        p.ident = rsx_fp_structural_hash(c, n, 0xF5ull);
    }
}

// a "CGB" record (the game's own container): "CGB\0", 0, u16 microcode bytes, u16 profile (3 vertex, 0x103
// fragment), the vertex program's input mask (u16 at 12), the fragment program's control bits (u16 at 26) and
// register count (byte 28), the microcode at 32. A fragment program's length is the renderer's walk to its end.
bool parse_cgb(const uint8_t* r, size_t size, Program& p)
{
    if (size < 48 || memcmp(r, "CGB\0", 4) || be32(r + 4) != 0) return false;
    const uint32_t ub = be16(r + 8), prof = be16(r + 10);
    if (prof == 3) {
        if (!ub || ub % 16 || 32u + ub > size) return false;
        p.stage = 0;
        p.code = words_reversed(r + 32, ub);
        p.header_inputs = be16(r + 12);
    } else if (prof == 0x103) {
        const uint32_t n = rsx_fp_program_size(r + 32, (uint32_t)(size - 32 > 0x20000 ? 0x20000 : size - 32));
        if (!n) return false;
        p.stage = 1;
        p.code.assign((const char*)r + 32, n);
        p.fp_ctrl = fp_ctrl_of(r[28], be16(r + 26));
    } else {
        return false;
    }
    finish(p);
    return true;
}

void read_spkg(const std::string& d, int src, Inventory& inv)
{
    const uint8_t* p = (const uint8_t*)d.data();
    if (d.size() < 0x40 || memcmp(p, "spkg", 4)) {
        inv.notes.push_back(std::string(source_file(src)) + ": not a shader package");
        return;
    }
    const uint32_t n = be32(p + 0x14);
    if ((uint64_t)0x40 + (uint64_t)n * 40 > d.size()) {
        inv.notes.push_back(std::string(source_file(src)) + ": its tables run past its end");
        return;
    }
    std::vector<int64_t> at[2];
    for (int t = 0; t < 2; t++) {
        at[t].assign(n, -1);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* e = p + 0x40 + 16u * (t * n + i);
            const uint32_t off = be32(e), size = be32(e + 4);
            Program pr;
            pr.source = (uint8_t)src; pr.index = i; pr.group = be32(e + 8); pr.id = be32(e + 12);
            pr.file_off = off + 32;
            if ((uint64_t)off + size > d.size() || !parse_cgb(p + off, size, pr) || pr.stage != t) {
                inv.bad[src][t]++;
                continue;
            }
            inv.records[src][t]++;
            at[t][i] = (int64_t)inv.programs.size();
            inv.programs.push_back(std::move(pr));
        }
    }
    for (uint32_t i = 0; i < n; i++)
        if (at[0][i] >= 0 && at[1][i] >= 0)
            inv.pairs.push_back({ (uint32_t)at[0][i], (uint32_t)at[1][i], inv.programs[at[0][i]].group,
                                  inv.programs[at[0][i]].id, (uint8_t)src });
}

// a SAS container: a count, then size-prefixed CGB records (more follow the counted ones: read to the end)
void read_spk(const std::string& d, int src, Inventory& inv)
{
    const uint8_t* p = (const uint8_t*)d.data();
    size_t pos = 4;
    uint32_t i = 0;
    while (pos + 8 <= d.size()) {
        const uint32_t sz = be32(p + pos);
        if (pos + 4 + (uint64_t)sz > d.size() || memcmp(p + pos + 4, "CGB\0", 4)) break;
        Program pr;
        pr.source = (uint8_t)src; pr.index = i++; pr.file_off = (uint32_t)(pos + 4 + 32);
        const int stage_guess = be16(p + pos + 4 + 10) == 0x103 ? 1 : 0;
        if (parse_cgb(p + pos + 4, sz, pr)) {
            inv.records[src][pr.stage]++;
            inv.programs.push_back(std::move(pr));
        } else {
            inv.bad[src][stage_guess]++;
        }
        pos += 4 + sz;
    }
    if (pos != d.size()) {
        char line[200];
        snprintf(line, sizeof line, "%s: %zu bytes after the last record not read", source_file(src), d.size() - pos);
        inv.notes.push_back(line);
    }
}

// the EBOOT: the game's CGB records, and standard Cg binary programs (CgBinaryProgram, big-endian: profile
// 0x1B5B vertex / 0x1B5C fragment, revision 6, total size, parameter count and array, the program's own struct,
// the microcode's size and offset), packed at any alignment
void read_eboot(const std::string& d, Inventory& inv)
{
    const uint8_t* p = (const uint8_t*)d.data();
    const size_t n = d.size();
    uint32_t ncgb = 0, ncg = 0;
    for (size_t at = d.find(std::string("CGB\0\0\0\0\0", 8)); at != std::string::npos;
         at = d.find(std::string("CGB\0\0\0\0\0", 8), at + 1)) {
        Program pr;
        pr.source = kEbootCgb; pr.index = ncgb++; pr.file_off = (uint32_t)(at + 32);
        const size_t lim = n - at < 0x10000 ? n - at : 0x10000;
        const int stage_guess = at + 12 <= n && be16(p + at + 10) == 0x103 ? 1 : 0;
        if (parse_cgb(p + at, lim, pr)) {
            inv.records[kEbootCgb][pr.stage]++;
            inv.programs.push_back(std::move(pr));
        } else {
            inv.bad[kEbootCgb][stage_guess]++;
        }
    }
    for (size_t at = 0; at + 32 <= n;) {
        const uint32_t prof = be32(p + at);
        if ((prof != 0x1B5Bu && prof != 0x1B5Cu) || be32(p + at + 4) != 6) { at++; continue; }
        const uint32_t total = be32(p + at + 8), npar = be32(p + at + 12), parr = be32(p + at + 16),
                       prog = be32(p + at + 20), usize = be32(p + at + 24), uoff = be32(p + at + 28);
        if (total < 32 || total > 0x10000 || at + total > n || !usize || usize % 16 || (uint64_t)uoff + usize > total ||
            (uint64_t)prog + 24 > total || (uint64_t)parr + (uint64_t)npar * 48 > total) {
            at++;
            continue;
        }
        const uint8_t* b = p + at;
        Program pr;
        pr.source = kEbootCg; pr.index = ncg++; pr.file_off = (uint32_t)(at + uoff);
        bool ok = true;
        if (prof == 0x1B5Bu) {   // CgBinaryVertexProgram: count, slot, registers, attributeInputMask, ...
            pr.stage = 0;
            pr.code = words_reversed(b + uoff, usize);
            pr.header_inputs = be32(b + prog + 12);
        } else {                 // CgBinaryFragmentProgram: ..., registerCount, outputFromH0, depthReplace, pixelKill
            pr.stage = 1;
            const uint32_t len = rsx_fp_program_size(b + uoff, usize);
            ok = len != 0;
            pr.code.assign((const char*)b + uoff, len);
            // the control word as the game sets it (recorded: 102 of 102 PPFX programs): the header's register
            // count, 4 more when the output is from H0 (the translator reads only the H0 and depth bits)
            const uint8_t* f = b + prog;
            pr.fp_ctrl = fp_ctrl_of(f[18] + (f[19] ? 4u : 0u), 0x8000u | 0x400u | (f[19] ? 0u : 0x40u) |
                                                                (f[20] ? 0xEu : 0u) | (f[21] ? 0x80u : 0u));
        }
        for (uint32_t k = 0; k < npar && ok; k++) {   // CgBinaryParameter: type, resource, ..., name, ...
            const uint8_t* e = b + parr + 48u * k;
            const uint32_t type = be32(e), res = be32(e + 4), name = be32(e + 16);
            if (res >= 0x800 && res < 0x810) {
                if (type == 0x42Du) pr.sampler_cube |= 1u << (res - 0x800);   // CG_SAMPLERCUBE
                if (type == 0x42Bu) pr.sampler_3d |= 1u << (res - 0x800);     // CG_SAMPLER3D
            }
            if (pr.name.empty() && name && name < total) {
                const char* s = (const char*)b + name;
                const size_t room = total - name;
                const size_t len = strnlen(s, room);
                if (len < room && len > 3 && (!strncmp(s, "VS_", 3) || !strncmp(s, "PS_", 3))) {
                    const char* dot = (const char*)memchr(s, '.', len);
                    pr.name.assign(s, dot ? (size_t)(dot - s) : len);
                }
            }
        }
        if (ok) {
            finish(pr);
            inv.records[kEbootCg][pr.stage]++;
            inv.programs.push_back(std::move(pr));
        } else {
            inv.bad[kEbootCg][pr.stage]++;
        }
        at += total;   // records do not overlap
    }
}

}  // namespace

bool discover(const std::string& usrdir, Inventory& out)
{
    out = Inventory();
    out.usrdir = usrdir;
    bool any = false;
    std::string d;
    for (int s : { (int)kPackage, (int)kPackage0, (int)kSas, (int)kSas2 }) {
        if (!read_file(usrdir + "/" + source_file(s), d)) {
            out.notes.push_back(std::string(source_file(s)) + ": not found");
            continue;
        }
        any = true;
        if (s == kPackage || s == kPackage0) read_spkg(d, s, out);
        else read_spk(d, s, out);
    }
    if (read_file(usrdir + "/EBOOT.elf", d)) {
        any = true;
        read_eboot(d, out);
    } else {
        out.notes.push_back("EBOOT.elf: not found (the decrypted executable)");
    }
    return any;
}

std::string summary_json(const Inventory& inv)
{
    std::string j = "{\"sources\":[";
    char b[600];
    // per source and stage: records, unparsed, distinct (vertex: bytes; fragment: structure)
    for (int s = 0; s < kSources; s++) {
        std::unordered_set<uint64_t> u[2];
        std::set<std::string> names;
        for (const Program& p : inv.programs)
            if (p.source == s) { u[p.stage].insert(p.ident); if (!p.name.empty()) names.insert(p.name); }
        snprintf(b, sizeof b, "%s{\"source\":\"%s\",\"file\":\"%s\",\"vertex\":{\"records\":%u,\"unparsed\":%u,"
                 "\"distinct\":%zu},\"fragment\":{\"records\":%u,\"unparsed\":%u,\"distinct\":%zu},\"named\":%zu}",
                 s ? "," : "", source_name(s), source_file(s), inv.records[s][0], inv.bad[s][0], u[0].size(),
                 inv.records[s][1], inv.bad[s][1], u[1].size(), names.size());
        j += b;
    }
    // over every source
    std::unordered_set<uint64_t> vu, fu;
    std::unordered_map<uint64_t, std::set<std::string>> fvariants;   // structure -> distinct byte strings
    uint32_t nv = 0, nf = 0, v_exact = 0, v_hdr_eq = 0, f_bad = 0, v_bad = 0, f_cube = 0, f_3d = 0;
    for (const Program& p : inv.programs) {
        if (p.stage == 0) {
            nv++; vu.insert(p.ident);
            if (!p.ok) v_bad++;
            if (p.inputs_exact) { v_exact++; if (p.header_inputs == p.inputs) v_hdr_eq++; }
        } else {
            nf++; fu.insert(p.ident);
            if (!p.ok) f_bad++;
            if (p.sampler_cube) f_cube++;
            if (p.sampler_3d) f_3d++;
            std::set<std::string>& v = fvariants[p.ident];
            if (v.size() < 64) v.insert(p.code);
        }
    }
    uint32_t cgroups = 0, cvariants = 0, fbytes = 0;
    for (auto& kv : fvariants) {
        fbytes += (uint32_t)kv.second.size();
        if (kv.second.size() > 1) { cgroups++; cvariants += (uint32_t)kv.second.size(); }
    }
    std::set<std::pair<uint64_t, uint64_t>> pu;
    for (const Pair& pr : inv.pairs) pu.insert({ inv.programs[pr.vp].ident, inv.programs[pr.fp].ident });
    snprintf(b, sizeof b,
             "],\"vertex\":{\"programs\":%u,\"distinct\":%zu,\"inputs_known\":%u,\"header_agrees\":%u,"
             "\"walk_failed\":%u},\"fragment\":{\"programs\":%u,\"distinct_bytes\":%u,\"distinct_structure\":%zu,"
             "\"constant_only_groups\":%u,\"constant_only_variants\":%u,\"constants_unreadable\":%u,"
             "\"cube_sampler\":%u,\"volume_sampler\":%u},\"pairs\":{\"records\":%zu,\"distinct\":%zu},\"notes\":[",
             nv, vu.size(), v_exact, v_hdr_eq, v_bad, nf, fbytes, fu.size(), cgroups, cvariants, f_bad, f_cube, f_3d,
             inv.pairs.size(), pu.size());
    j += b;
    for (size_t i = 0; i < inv.notes.size(); i++) {
        std::string s;
        for (char c : inv.notes[i]) if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) s += c;
        j += (i ? ",\"" : "\"") + s + "\"";
    }
    return j + "]}";
}

std::string check_json(const Inventory& inv, const std::vector<RecipeView>& rs)
{
    std::unordered_map<std::string, std::vector<uint32_t>> vby;     // vertex code -> programs
    std::unordered_map<uint64_t, std::vector<uint32_t>> fby;        // fragment structure -> programs
    std::set<std::pair<uint64_t, uint64_t>> pairs;
    for (uint32_t i = 0; i < inv.programs.size(); i++) {
        const Program& p = inv.programs[i];
        if (p.stage == 0) vby[p.code].push_back(i);
        else fby[p.ident].push_back(i);
    }
    for (const Pair& pr : inv.pairs) pairs.insert({ inv.programs[pr.vp].ident, inv.programs[pr.fp].ident });
    std::set<std::string> vseen, vfound;
    std::set<uint64_t> fseen, ffound;
    uint32_t n = 0, both = 0, as_pair = 0, layout_eq = 0, layout_sub = 0, layout_n = 0, ctrl_eq = 0, ctrl_n = 0,
             ctrl_tr = 0;
    uint32_t by_src[kSources][2] = {};
    uint32_t missing_kind[3][2] = {};
    std::string missing;
    for (const RecipeView& r : rs) {
        n++;
        const uint64_t fid = rsx_fp_structural_hash((const uint8_t*)r.fp.data(), (uint32_t)r.fp.size(), 0xF5ull);
        auto vt = vby.find(r.vp);
        auto ft = fby.find(fid);
        vseen.insert(r.vp);
        fseen.insert(fid);
        if (vt != vby.end()) { vfound.insert(r.vp); by_src[inv.programs[vt->second[0]].source][0]++; }
        else if (r.kind <= 2) missing_kind[r.kind][0]++;
        if (ft != fby.end()) { ffound.insert(fid); by_src[inv.programs[ft->second[0]].source][1]++; }
        else if (r.kind <= 2) missing_kind[r.kind][1]++;
        if (vt != vby.end() && ft != fby.end()) {
            both++;
            if (pairs.count({ fnv64(r.vp.data(), r.vp.size()), fid })) as_pair++;
            const Program& vp = inv.programs[vt->second[0]];
            layout_n++;
            if (r.layout_mask == vp.inputs) layout_eq++;
            else if ((r.layout_mask & vp.inputs) == r.layout_mask) layout_sub++;
            ctrl_n++;
            bool eq = false, tr = false;   // the whole word; the bits the translation reads (H0, depth)
            for (uint32_t i : ft->second) {
                eq = eq || inv.programs[i].fp_ctrl == r.fp_ctrl;
                tr = tr || (inv.programs[i].fp_ctrl & 0x4Eu) == (r.fp_ctrl & 0x4Eu);
            }
            if (eq) ctrl_eq++;
            if (tr) ctrl_tr++;
        } else if (missing.size() < 600) {
            char b[160];
            snprintf(b, sizeof b, "%s{\"kind\":%u,\"vp_bytes\":%zu,\"vp_found\":%s,\"fp_bytes\":%zu,\"fp_found\":%s}",
                     missing.empty() ? "" : ",", r.kind, r.vp.size(), vt != vby.end() ? "true" : "false", r.fp.size(),
                     ft != fby.end() ? "true" : "false");
            missing += b;
        }
    }
    std::string j;
    char b[900];
    snprintf(b, sizeof b,
             "{\"pipelines\":%u,\"both_programs_found\":%u,\"as_one_package_record\":%u,"
             "\"vertex\":{\"distinct\":%zu,\"found\":%zu},\"fragment\":{\"distinct\":%zu,\"found\":%zu},"
             "\"layout\":{\"checked\":%u,\"equal_to_analysis\":%u,\"a_subset\":%u},"
             "\"fp_ctrl\":{\"checked\":%u,\"equal_to_header\":%u,\"translation_bits_equal\":%u},\"found_in\":{",
             n, both, as_pair, vseen.size(), vfound.size(), fseen.size(), ffound.size(), layout_n, layout_eq, layout_sub,
             ctrl_n, ctrl_eq, ctrl_tr);
    j += b;
    for (int s = 0; s < kSources; s++) {
        snprintf(b, sizeof b, "%s\"%s\":[%u,%u]", s ? "," : "", source_name(s), by_src[s][0], by_src[s][1]);
        j += b;
    }
    snprintf(b, sizeof b, "},\"not_found\":{\"ppfx\":[%u,%u],\"asp\":[%u,%u]},\"missing\":[", missing_kind[1][0],
             missing_kind[1][1], missing_kind[2][0], missing_kind[2][1]);
    j += b;
    return j + missing + "]}";
}

}  // namespace sd

namespace ppfx { std::vector<sd::RecipeView> recipe_views(const std::string& path); }

// `dotheroes --shader-inventory [--disc USRDIR] [--recipes FILE ...]` (main.cpp): the inventory's summary, and with
// recipes files the check of every recorded program against it; JSON on stdout. 0, or 2 when nothing was read.
extern "C" int dotheroes_shader_inventory(int argc, char** argv, const char* default_usrdir)
{
    std::string usrdir = default_usrdir ? default_usrdir : "";
    std::vector<std::string> recipes;
    for (int i = 2; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--disc")) usrdir = argv[++i];
        else if (!strcmp(argv[i], "--recipes")) recipes.push_back(argv[++i]);
    }
    sd::Inventory inv;
    const bool any = sd::discover(usrdir, inv);
    std::string out = "{\"inventory\":" + sd::summary_json(inv);
    if (!recipes.empty()) {
        std::vector<sd::RecipeView> rs;
        for (const std::string& f : recipes) {
            std::vector<sd::RecipeView> one = ppfx::recipe_views(f);
            rs.insert(rs.end(), one.begin(), one.end());
        }
        out += ",\"recipes\":" + sd::check_json(inv, rs);
    }
    printf("%s}\n", out.c_str());
    fflush(stdout);
    return any ? 0 : 2;
}
