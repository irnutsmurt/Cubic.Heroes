// The host side of the generated SPU code (spu_native.h): a data-moving task's DMA done at once.
#include "spu_native.h"

#include "spu_helpers.h"   // the channel numbers (spu_channels.h through it), as the generated code uses them

namespace spu_native {

const char* result_name(Result r)
{
    switch (r) {
    case Result::handled: return "returned";
    case Result::declined: return "no such entry";
    case Result::halted: return "halted";
    case Result::stopped: return "stopped";
    case Result::ended: return "ended by the host";
    case Result::bad_return: return "returned elsewhere";
    case Result::unresolved: return "jumped outside its table";
    }
    return "?";
}

namespace {

void note(Dma& d, const char* what, uint32_t value)
{
    if (!d.trouble) { d.trouble = what; d.trouble_value = value; }
}

void dma_wrch(void* user, uint32_t channel, uint32_t v)
{
    Dma& d = *static_cast<Dma*>(user);
    switch (channel) {
    case MFC_LSA: d.lsa = v; break;
    case MFC_EAH: d.eah = v; break;
    case MFC_EAL: d.eal = v; break;
    case MFC_Size: d.size = v; break;
    case MFC_TagID: d.tag = v; break;
    case MFC_WrTagMask: d.mask = v; break;
    case MFC_WrTagUpdate: break;   // every transfer is complete when issued
    case MFC_Cmd: {
        const uint32_t cmd = v & 0xFF, lsa = d.lsa & 0x3FFFFu, n = d.size;
        if (d.eah) { note(d, "a transfer above 4 GB", d.eah); break; }
        if (lsa + n > 0x40000u) { note(d, "a transfer past the local store's end", lsa); break; }
        if (d.trace) d.trace(d.user, cmd, lsa, d.eal, n, d.tag);
        if ((cmd & ~3u) == 0x40) d.get(d.user, d.ls + lsa, d.eal, n);          // GET, GETB, GETF
        else if ((cmd & ~3u) == 0x20) d.put(d.user, d.eal, d.ls + lsa, n);     // PUT, PUTB, PUTF
        else note(d, "a DMA command other than GET and PUT", cmd);
        break;
    }
    default: note(d, "a channel write a data-moving task does not make", channel); break;
    }
}

uint32_t dma_rdch(void* user, uint32_t channel)
{
    Dma& d = *static_cast<Dma*>(user);
    if (channel == MFC_RdTagStat) return d.mask;   // all complete
    note(d, "a channel read a data-moving task does not make", channel);
    return 0;
}

uint32_t dma_rchcnt(void* user, uint32_t channel)
{
    (void)user;
    return channel == SPU_RdInMbox ? 0u : 1u;
}

int dma_stop(void* user, uint32_t code, uint32_t, uint32_t)
{
    note(*static_cast<Dma*>(user), "a stop", code);
    return 0;
}

int dma_branch(void* user, Branch, uint32_t target, uint32_t, uint32_t*)
{
    note(*static_cast<Dma*>(user), "a branch out of the generated code", target);
    return 0;
}

void dma_unsupported(void* user, uint32_t pc)
{
    note(*static_cast<Dma*>(user), "an instruction the lift could not translate", pc);
}

}  // namespace

Host Dma::host()
{
    return Host{ this, dma_wrch, dma_rdch, dma_rchcnt, dma_stop, dma_branch, dma_unsupported };
}

}  // namespace spu_native
