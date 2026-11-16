// M9.2a (docs/M92_SHADER_PLAN.md): the shader programs on the player's own disc, found without playing.
//
// The containers, read from <disc>/PS3_GAME/USRDIR (the player's copy; nothing of it is kept or written here):
//   data/shaders/shaders.ps3.pkg, shaders_0.ps3.pkg  "spkg": a vertex table, a fragment table (the same (group, id)
//                                                     at the same index: a record's two programs are drawn together)
//                                                     and a (group, id) index; each record a "CGB" container
//   data/shaders/SasShader*.ps3.spk                   size-prefixed CGB records
//   EBOOT.elf                                         CGB records, and standard Cg binary programs (CgBinaryProgram)
// Each program is kept as the renderer is handed it (a vertex program's words byte-reversed, little-endian; a
// fragment program as stored) and identified as the renderer identifies it: a vertex program by its bytes, a
// fragment program by its structure (rsx_fp_structural_hash: the inline constants the game patches as it draws do
// not change its translation).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace sd {

enum Source : uint8_t { kPackage = 0, kPackage0, kSas, kSas2, kEbootCgb, kEbootCg, kSources };
const char* source_name(int s);
const char* source_file(int s);   // relative to USRDIR

struct Program {
    uint8_t stage = 0;            // 0 vertex, 1 fragment
    uint8_t source = 0;
    uint32_t file_off = 0;        // the microcode's offset in its file
    uint32_t index = 0;           // its record's index in its container (packages: the table's)
    uint32_t group = 0, id = 0;   // a package record's
    std::string code;             // as the renderer is handed it
    std::string name;             // a Cg program's name (its parameters' prefix), else empty
    uint32_t header_inputs = 0;   // vertex: the inputs its header declares (CGB, Cg attributeInputMask)
    uint32_t inputs = 0xFFFFu;    // vertex: the inputs it reads (the renderer's analysis); 0xFFFF unknown
    int inputs_exact = 0;
    uint32_t fp_ctrl = 0;         // fragment: the control word its header gives (register count, H0, depth, kill)
    uint32_t units = 0;           // fragment: the texture units it samples
    uint32_t consts = 0;          // fragment: its inline constant blocks
    uint32_t sampler_cube = 0, sampler_3d = 0;   // fragment, Cg: units its parameters declare cube or 3D
    bool ok = false;              // the walker found its end (and, fragment, its constants)
    uint64_t ident = 0;           // vertex: FNV-1a of code; fragment: structural hash (seed 0xF5, as fp_info)
};

struct Pair { uint32_t vp, fp; uint32_t group, id; uint8_t source; };   // indices into programs

struct Inventory {
    std::string usrdir;
    std::vector<Program> programs;
    std::vector<Pair> pairs;               // package records (both programs drawn together)
    std::vector<std::string> notes;        // what could not be read, and why
    uint32_t records[kSources][2] = {};    // per source and stage: records read
    uint32_t bad[kSources][2] = {};        // per source and stage: records that did not parse
};

// Reads every container it finds under usrdir; false when none could be read (notes say why).
bool discover(const std::string& usrdir, Inventory& out);

// The inventory's summary as JSON (counts, unique programs, constant-only variants, by source); no program bytes.
std::string summary_json(const Inventory& inv);

uint64_t fnv64(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull);

// A recorded pipeline's recipe as the inventory checks it (ppfx::recipe_views reads a recipes file)
struct RecipeView {
    uint32_t kind;                 // 1 PPFX's, 2 asp's
    std::string vp, fp;            // the programs as the renderer was handed them
    uint32_t fp_ctrl, layout_mask, alpha_test, shadow_mask, rt, df;
};

// Every recorded program looked up in the inventory (vertex by bytes, fragment by structure), and whether the
// translation's parameters the headers give agree with the recorded ones: JSON.
std::string check_json(const Inventory& inv, const std::vector<RecipeView>& rs);

}  // namespace sd
