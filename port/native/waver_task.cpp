// The game's waver task run natively (waver_task.h): the host around the task's own code, which is generated
// on this machine from the player's lift (waver_gen, port/native/spu_native.h). Checked offline against the
// lifted image (tests/native/waver_task_check.cpp) and in the game (DOTHEROES_NATIVE_WAVER=check).
#include "waver_task.h"

#include <cstring>

#include "spu_helpers.h"   // spu_pref_u32

namespace waver_task {
namespace {

// The task's start (tools/spu_native/waver.py): its entry sets r1 = 0x3FFD0 and calls main with r3 = the
// argument; main returns to the entry at 0x3090.
constexpr uint32_t kMain = 0x36D8, kMainLink = 0x3090, kStack = 0x3FFD0;

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
bool past_4gb(uint32_t ea, uint32_t bytes) { return (uint64_t)ea + bytes > (1ull << 32); }

void guest_get(void* user, void* dst, uint32_t ea, uint32_t bytes) { memcpy(dst, (uint8_t*)user + ea, bytes); }
void guest_put(void* user, uint32_t ea, const void* src, uint32_t bytes) { memcpy((uint8_t*)user + ea, src, bytes); }

// The block's parameters as the run read them: the first transfer from the argument's EA (the block).
struct Reading {
    Io io;
    uint32_t param_ea;
    uint8_t first[kParamBytes];
    uint32_t have;
};
void reading_get(void* user, void* dst, uint32_t ea, uint32_t bytes)
{
    Reading& r = *static_cast<Reading*>(user);
    r.io.get(r.io.user, dst, ea, bytes);
    if (ea >= r.param_ea && ea < r.param_ea + kParamBytes && ea - r.param_ea == r.have) {
        const uint32_t n = bytes < kParamBytes - r.have ? bytes : kParamBytes - r.have;
        memcpy(r.first + r.have, dst, n);
        r.have += n;
    }
}
void reading_put(void* user, uint32_t ea, const void* src, uint32_t bytes)
{
    Reading& r = *static_cast<Reading*>(user);
    r.io.put(r.io.user, ea, src, bytes);
}

}  // namespace

Params parse_params(const uint8_t b[kParamBytes])
{
    Params p;
    p.field = be32(b + 0x00);
    p.texture[0] = be32(b + 0x04);
    p.texture[1] = be32(b + 0x08);
    p.select = be32(b + 0x0C);
    p.phase = be32(b + 0x10);
    p.radius = be32(b + 0x14);
    p.frequency = be32(b + 0x18);
    p.amplitude = be32(b + 0x1C);
    p.slope = be32(b + 0x20);
    return p;
}

Io guest_io(uint8_t* base) { return Io{ base, guest_get, guest_put }; }

const char* unsupported_reason(const Params& p, uint32_t param_ea)
{
    if (param_ea & 15) return "argument EA not 16-byte aligned (the task halts before its GET)";
    if (past_4gb(param_ea, kParamBytes)) return "parameter block past 4 GB";
    if (past_4gb(p.field, kFieldBytes)) return "field past 4 GB";
    if (past_4gb(p.texture_ea(), kTextureBytes)) return "texture past 4 GB";
    if (p.field & 15) return "field not 16-byte aligned (the task halts before its first PUT)";
    if (p.texture_ea() & 15) return "texture not 16-byte aligned (the task halts after writing the field)";
    return nullptr;
}

bool available() { return waver_gen::has_entry(kMain); }

Status run_frame(uint32_t param_ea, const Io& io, Local& ls, Params* used)
{
    ls.why = nullptr;
    if (past_4gb(param_ea, kParamBytes)) { ls.why = "parameter block past 4 GB"; return Status::unsupported; }
    waver_gen::load(ls.c.ls);
    memset(ls.c.gpr, 0, sizeof ls.c.gpr);
    ls.c.gpr[0] = spu_pref_u32(kMainLink);
    ls.c.gpr[1] = spu_pref_u32(kStack);
    ls.c.gpr[3]._u32[1] = param_ea;   // the argument: { u64 EA of the block, the event flag's EA (never read), 0 }
    Reading rd{ io, param_ea, {}, 0 };
    spu_native::Dma d;
    d.user = &rd;
    d.get = reading_get;
    d.put = reading_put;
    d.ls = ls.c.ls;
    spu_native::Outcome o;
    const spu_native::Result r = waver_gen::call(ls.c.ls, ls.c.gpr, kMain, d.host(), &o);
    if (used) *used = parse_params(rd.first);
    if (r == spu_native::Result::handled && !d.trouble && o.ret_pc == kMainLink && ls.c.gpr[3]._u32[0] == 0)
        return Status::done;
    ls.why = d.trouble ? d.trouble : r == spu_native::Result::halted ? "the task halted" : spu_native::result_name(r);
    return Status::unsupported;
}

}  // namespace waver_task
