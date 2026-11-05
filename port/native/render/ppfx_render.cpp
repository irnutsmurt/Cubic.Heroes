// R3's native PPFX renderer (docs/NATIVE_R3.md): a thread of its own, with a
// Vulkan queue and command pool of its own on the existing renderer's device,
// rendering PPFX's records (ppfx_recorder.cpp) into images of its own.
//
// Per record, the same derivation the existing renderer makes for the draw
// (rsx_draw_engine.c eng_pipeline_get, sink_end; the backend's pipeline,
// sampler and view mappings), from the record's own state -- the recorder's
// state block in a scratch register file the toolkit's getters read, the
// record's programs, constants and vertex bytes. The programs go through the
// same translators and the shared SPIR-V cache.
//
// Images. A surface address PPFX renders to gets a native image of its own
// (the target registry); the engine's surfaces are never bound here. What
// PPFX reads that other work produced arrives through the bridge: the walker
// copies the engine's surface, in the engine's own command stream, into a
// slot image, and the record names the slot and the semaphore that orders
// it; the copy into the native image happens here, after the wait. A small
// static texture comes from the bytes the recorder took. A draw that samples
// its own target samples a copy taken just before it, as the engine's does.
//
// Lifetime. Work is recorded into batches (a command buffer, a fence, a
// descriptor pool and an upload buffer each), submitted at frame ends and
// when they fill. A batch's completion (its fence) releases what it used:
// slots go back to the bridge, retired images are destroyed, comparisons run.
// Records are released to the recorder (`done`) once recorded: nothing here
// reads a record after that.
//
// Comparison. For the records the walker copied the engine's output of
// (compare_wanted: every PPFX draw of every DOTHEROES_PPFX_COMPARE_EVERY-th
// frame, default 30, and every frame's final composite), the native output is
// read back after the record and compared byte for byte once both copies are
// complete. DOTHEROES_PPFX_DUMP=<dir>: the first differing outputs, both
// sides, as raw files.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ppfx_native.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

#include "rsx_dispatch.h"
#include "rsx_draw_engine.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_texture_layout.h"
#include "rsx_vertex_compact.h"
#include "rsx_vp_decompiler.h"
#include "rsx_vulkan_bridge.h"

extern "C" uint8_t* vm_base;

namespace ppfx {
namespace {

// ---- Vulkan entry points ------------------------------------------------------

#define PPFX_VK_DEVICE(X) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkCreatePipelineLayout) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdClearColorImage) \
    X(vkCmdClearAttachments) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkCreateQueryPool) \
    X(vkGetQueryPoolResults) X(vkCreateFence) X(vkGetFenceStatus) X(vkResetFences) X(vkWaitForFences) \
    X(vkQueueSubmit) X(vkCreateSemaphore)

struct Fn {
#define X(f) PFN_##f f;
    PPFX_VK_DEVICE(X)
#undef X
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
} F;

rsx_vk_bridge_dev D;
VkPhysicalDeviceMemoryProperties s_mem;
float s_ts_period = 1.0f;
std::atomic<int> s_ready{0};

uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void fail(const char* what, VkResult r)
{
    char line[160];
    snprintf(line, sizeof line, "render: %s failed (%d)", what, (int)r);
    report_add(line);
    fprintf(stderr, "[ppfx] %s\n", line);
}

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < s_mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (s_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

// ---- buffers and images ----------------------------------------------------------

struct Buf { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; uint8_t* p = nullptr; VkDeviceSize n = 0; };

// Host-visible memory; a buffer the CPU reads back (readback) is cached
// memory, which the CPU reads at its own speed -- write-combined memory read
// by the CPU costs hundreds of milliseconds a frame.
bool make_buffer(Buf& o, VkDeviceSize n, VkBufferUsageFlags usage, bool readback = false)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = n; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (F.vkCreateBuffer(D.device, &ci, nullptr, &o.b) != VK_SUCCESS) return false;
    VkMemoryRequirements rq;
    F.vkGetBufferMemoryRequirements(D.device, o.b, &rq);
    uint32_t t = ~0u;
    if (readback)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t == ~0u)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size; ai.memoryTypeIndex = t;
    if (t == ~0u || F.vkAllocateMemory(D.device, &ai, nullptr, &o.m) != VK_SUCCESS) return false;
    F.vkBindBufferMemory(D.device, o.b, o.m, 0);
    void* p = nullptr;
    F.vkMapMemory(D.device, o.m, 0, VK_WHOLE_SIZE, 0, &p);
    o.p = (uint8_t*)p; o.n = n;
    return true;
}

struct Img {
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED; uint32_t w = 0, h = 0, levels = 1, bpp = 0;
    VkFramebuffer fb = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkImageView> views;   // (remap, format byte) -> a view with that crossbar
    uint32_t written = 0;                              // records have written it (or a handoff filled it)
    VkDeviceSize bytes = 0;                            // its memory
};

VkImageView make_view(VkImage img, VkFormat fmt, uint32_t levels, VkComponentMapping map)
{
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.components = map;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    VkImageView v = VK_NULL_HANDLE;
    F.vkCreateImageView(D.device, &vi, nullptr, &v);
    return v;
}

Img* make_image(VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, uint32_t bpp, bool target)
{
    Img* o = new Img;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = { w, h, 1 }; ci.mipLevels = levels;
    ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               (target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (F.vkCreateImage(D.device, &ci, nullptr, &o->img) != VK_SUCCESS) { delete o; return nullptr; }
    VkMemoryRequirements rq;
    F.vkGetImageMemoryRequirements(D.device, o->img, &rq);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, 0);
    if (F.vkAllocateMemory(D.device, &ai, nullptr, &o->mem) != VK_SUCCESS) {
        F.vkDestroyImage(D.device, o->img, nullptr); delete o; return nullptr;
    }
    F.vkBindImageMemory(D.device, o->img, o->mem, 0);
    o->fmt = fmt; o->w = w; o->h = h; o->levels = levels; o->bpp = bpp;
    o->view = make_view(o->img, fmt, levels, { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY });
    o->bytes = rq.size;
    stats().n_images++;
    stats().n_image_bytes += rq.size;
    return o;
}

void destroy_image(Img* o)
{
    if (!o) return;
    for (auto& kv : o->views) F.vkDestroyImageView(D.device, kv.second, nullptr);
    if (o->fb) F.vkDestroyFramebuffer(D.device, o->fb, nullptr);
    if (o->view) F.vkDestroyImageView(D.device, o->view, nullptr);
    if (o->img) F.vkDestroyImage(D.device, o->img, nullptr);
    if (o->mem) F.vkFreeMemory(D.device, o->mem, nullptr);
    stats().n_images--;
    stats().n_image_bytes -= o->bytes;
    delete o;
}

// Every image stays in GENERAL, as the engine's do; ordering is a full
// barrier between operations (the engine's vk_cmd_after_everything).
void barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    F.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
}

void to_general(VkCommandBuffer cb, VkImage img, uint32_t levels)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    F.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);
}

// ---- the bridge's slots ----------------------------------------------------------------
// Staging images (handoffs), buffers (the engine's outputs for comparison)
// and binary semaphores. The walker takes one (slot_*), the record names it,
// and the batch that used it gives it back once complete.

struct SlotImg { Img* im; bool busy; };
struct SlotBuf { Buf b; bool busy; };
struct SlotSem { VkSemaphore s; bool busy; };
std::mutex s_slot_lock;
std::vector<SlotImg> s_simg;
std::vector<SlotBuf> s_sbuf;
std::vector<SlotSem> s_ssem;
const size_t kMaxSlotImages = 128, kMaxSlotBuffers = 512, kMaxSlotSems = 64;
const size_t kFinalReserve = 32;   // buffers kept for final composites: those are compared every frame

// ---- batches --------------------------------------------------------------------------

struct Batch {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkQueryPool qp = VK_NULL_HANDLE;
    Buf up;
    VkDeviceSize used = 0;
    bool open = false, pending = false;
    uint32_t frame = 0, records = 0, sets = 0;
    std::vector<int> sems;                       // semaphore slots to wait on ...
    std::vector<long long> ords;                 // ... and their signals' ordinals
    std::vector<std::function<void()>> done;     // after completion
    uint64_t t_record = 0;
};
const int kBatches = 4;
const VkDeviceSize kUpload = 16u << 20;
const uint32_t kSets = 1024;
Batch s_b[kBatches];
int s_cur = 0;

VkCommandPool s_pool;
VkDescriptorSetLayout s_dsl;
VkPipelineLayout s_pl;
Img* s_dummy;
VkSampler s_dummy_smp;

void retire(Batch& b)
{
    if (!b.pending) return;
    F.vkWaitForFences(D.device, 1, &b.fence, VK_TRUE, UINT64_MAX);
    uint64_t ts[2] = { 0, 0 };
    if (b.qp && F.vkGetQueryPoolResults(D.device, b.qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        stats().gpu_ns += (uint64_t)((double)(ts[1] - ts[0]) * s_ts_period);
    for (auto& f : b.done) f();
    b.done.clear();
    {
        std::lock_guard<std::mutex> g(s_slot_lock);
        for (int s : b.sems) s_ssem[s].busy = false;
    }
    b.sems.clear();
    b.ords.clear();
    b.pending = false;
}

bool retire_ready(Batch& b) { return b.pending && F.vkGetFenceStatus(D.device, b.fence) == VK_SUCCESS; }

Batch& batch(uint32_t frame)
{
    Batch& b = s_b[s_cur];
    if (b.open) return b;
    retire(b);
    F.vkResetFences(D.device, 1, &b.fence);
    F.vkResetDescriptorPool(D.device, b.dp, 0);
    F.vkResetCommandBuffer(b.cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    F.vkBeginCommandBuffer(b.cb, &bi);
    F.vkCmdResetQueryPool(b.cb, b.qp, 0, 2);
    F.vkCmdWriteTimestamp(b.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, b.qp, 0);
    b.used = 0; b.open = true; b.frame = frame; b.records = 0; b.sets = 0;
    return b;
}

void submit()
{
    Batch& b = s_b[s_cur];
    if (!b.open) return;
    // the engine's submissions that signal what we wait on must be made
    // before ours waits (the engine submits at its own points: a frame at most)
    if (!b.ords.empty()) {
        const uint64_t t0 = now_ns();
        for (long long o : b.ords)
            while (!rsx_vk_bridge_submitted(o)) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                if (now_ns() - t0 > 10000000000ull) { fail("waiting for the engine's signal", VK_TIMEOUT); break; }
            }
        stats().wait_signal_us += (now_ns() - t0) / 1000;
    }
    // the engine's copies (other queue, ordered by the semaphores) and ours,
    // visible to the host for the comparisons
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    F.vkCmdPipelineBarrier(b.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    F.vkCmdWriteTimestamp(b.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b.qp, 1);
    F.vkEndCommandBuffer(b.cb);
    std::vector<VkSemaphore> waits;
    std::vector<VkPipelineStageFlags> stages;
    {
        std::lock_guard<std::mutex> g(s_slot_lock);
        for (int s : b.sems) { waits.push_back(s_ssem[s].s); stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT); }
    }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = (uint32_t)waits.size(); si.pWaitSemaphores = waits.data();
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1; si.pCommandBuffers = &b.cb;
    rsx_vk_bridge_queue_lock();
    const VkResult r = F.vkQueueSubmit(D.queue, 1, &si, b.fence);
    rsx_vk_bridge_queue_unlock();
    if (r != VK_SUCCESS) fail("vkQueueSubmit", r);
    b.open = false; b.pending = true;
    stats().n_batches++;
    s_cur = (s_cur + 1) % kBatches;
}

// Upload bytes into the batch's buffer; the offset, or ~0 when full.
VkDeviceSize upload(Batch& b, const void* p, VkDeviceSize n, VkDeviceSize align = 256)
{
    VkDeviceSize at = (b.used + align - 1) & ~(align - 1);
    if (at + n > b.up.n) return ~(VkDeviceSize)0;
    memcpy(b.up.p + at, p, (size_t)n);
    b.used = at + n;
    return at;
}

// ---- formats ----------------------------------------------------------------------------

VkFormat be_vkformat(rsx_be_format f, uint32_t* bpp)
{
    switch (f) {   // the backend's vk_eng_format
    case RSX_BE_FMT_R8: *bpp = 1; return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8: *bpp = 2; return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8: *bpp = 4; return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16: *bpp = 2; return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16: *bpp = 4; return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F: *bpp = 4; return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F: *bpp = 4; return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: *bpp = 0; return VK_FORMAT_UNDEFINED;
    }
}

rsx_be_format texfmt_be(rsx_texfmt f)
{
    switch (f) {   // the engine's eng_texfmt
    case RSX_TEXFMT_R8: return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8: return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8: return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_R16: return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16: return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F: return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F: return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F: return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F: return RSX_BE_FMT_R32G32B32A32F;
    default: return RSX_BE_FMT_NONE;
    }
}

VkComponentSwizzle remap_sel(uint8_t s)
{
    switch (s) {   // the backend's vk_remap_sel
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}

VkComponentMapping remap(uint32_t control1, uint32_t fmt)
{
    uint8_t sel[4];
    rsx_texture_component_remap(control1, fmt & 0x9Fu, sel);
    return { remap_sel(sel[1]), remap_sel(sel[2]), remap_sel(sel[3]), remap_sel(sel[0]) };
}

VkSamplerAddressMode gcm_wrap(uint32_t w)
{
    switch (w & 0xFu) {   // the backend's vk_gcm_wrap
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

std::map<uint64_t, VkSampler> s_samplers;

// The engine's eng_decode_sampler and the backend's vk_eng_sampler.
VkSampler sampler(uint32_t filter, uint32_t wrap, uint32_t control0)
{
    const uint32_t minf = (filter >> 16) & 7u, magf = (filter >> 24) & 7u;
    const int min_linear = minf == 2 || minf == 4 || minf == 6, mag_linear = magf == 2;
    const int mip_present = minf >= 3, mip_linear = minf == 5 || minf == 6;
    float min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    if (!mip_present) max_lod = min_lod;
    uint32_t lo, hi;
    memcpy(&lo, &min_lod, 4); memcpy(&hi, &max_lod, 4);
    const uint64_t key = (uint64_t)min_linear | (uint64_t)mag_linear << 1 | (uint64_t)mip_linear << 2 |
                         (uint64_t)mip_present << 3 | (uint64_t)(wrap & 0xF) << 4 | (uint64_t)(wrap >> 8 & 0xF) << 8 |
                         (uint64_t)(wrap >> 16 & 0xF) << 12 | (uint64_t)(lo >> 16) << 16 | (uint64_t)(hi >> 16) << 32;
    auto it = s_samplers.find(key);
    if (it != s_samplers.end()) return it->second;
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = gcm_wrap(wrap); ci.addressModeV = gcm_wrap(wrap >> 8); ci.addressModeW = gcm_wrap(wrap >> 16);
    ci.minLod = min_lod; ci.maxLod = max_lod;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    F.vkCreateSampler(D.device, &ci, nullptr, &s);
    return s_samplers[key] = s;
}

std::map<VkFormat, VkRenderPass> s_rps;

VkRenderPass render_pass(VkFormat f)
{
    auto it = s_rps.find(f);
    if (it != s_rps.end()) return it->second;
    VkAttachmentDescription a = {};
    a.format = f; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_GENERAL; a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1; ci.pAttachments = &a; ci.subpassCount = 1; ci.pSubpasses = &sp;
    VkRenderPass rp = VK_NULL_HANDLE;
    F.vkCreateRenderPass(D.device, &ci, nullptr, &rp);
    return s_rps[f] = rp;
}

VkFramebuffer framebuffer(Img* o)
{
    if (o->fb) return o->fb;
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = render_pass(o->fmt); ci.attachmentCount = 1; ci.pAttachments = &o->view;
    ci.width = o->w; ci.height = o->h; ci.layers = 1;
    F.vkCreateFramebuffer(D.device, &ci, nullptr, &o->fb);
    return o->fb;
}

// ---- the registries -------------------------------------------------------------------------

std::unordered_map<uint64_t, Img*> s_targets;     // a surface address PPFX renders to, or other work's surface handed over
std::unordered_map<uint64_t, Img*> s_static;      // static textures by content
std::map<std::tuple<VkFormat, uint32_t, uint32_t>, Img*> s_snap;   // own-target snapshots
std::vector<Img*> s_retired;                      // destroyed once the batches in flight are done

uint64_t akey(uint32_t loc, uint32_t off) { return (uint64_t)loc << 32 | off; }

Img* target_image(Batch& b, uint32_t loc, uint32_t off, VkFormat fmt, uint32_t bpp, uint32_t w, uint32_t h)
{
    Img*& t = s_targets[akey(loc, off)];
    if (t && t->fmt == fmt && t->w >= w && t->h >= h) return t;
    // new, or other than this record needs: a new image (resource reuse); the
    // old one goes once nothing in flight can use it
    if (t) { s_retired.push_back(t); stats().n_retired++; }
    t = make_image(fmt, w, h, 1, bpp, true);
    if (t) to_general(b.cb, t->img, 1);
    return t;
}

// ---- programs and pipelines -----------------------------------------------------------------

struct Pipe { VkPipeline p; uint32_t nconst; uint32_t tex_used; };
std::unordered_map<uint64_t, Pipe> s_pipes;

// The engine's eng_programs_text, step for step, under the translators' lock.
struct ProgParams {
    const uint8_t* vp; uint32_t vp_bytes; const uint8_t* fp; uint32_t fp_size;
    uint32_t fp_ctrl, cube_mask, vol_mask, vtex_mask, layout_mask, vp_branch_bits, ucp, srgb_output, alpha_test,
             alpha_func, unnorm_mask, shadow_mask, gamma_units, expand_units;
    uint32_t wpos[2]; int wpos_scaled;
    const rsx_fp_shadow_desc* shadow; const uint8_t* gamma_masks; const uint8_t* expand_masks;
    const uint32_t (*unnorm_dim)[3];
};

bool programs_text(const ProgParams& p, std::string& vs_out, std::string& ps_out, uint32_t* nconst)
{
    static char vs[16384], ps[65536];
    rsx_shader_text_lock();
    rsx_vp_set_branch_bits(p.vp_branch_bits);
    int vi = rsx_vp_decompile_compact_ex(p.vp, p.vp_bytes, p.vtex_mask, p.layout_mask, vs, sizeof vs);
    if (vi > 0 && p.ucp && rsx_vp_apply_user_clip(vs, sizeof vs, p.ucp) < 0) vi = -1;
    int fi = rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(p.fp, p.fp_size, p.fp_ctrl, p.cube_mask, p.vol_mask,
            p.shadow_mask ? p.shadow : nullptr, p.gamma_masks, p.expand_units ? p.expand_masks : nullptr, ps, sizeof ps,
            nconst);
    if (fi > 0 && p.wpos_scaled && rsx_fp_apply_wpos_grid(ps, sizeof ps, p.wpos[0], p.wpos[1]) < 0) fi = -1;
    if (fi > 0 && rsx_fp_apply_unnorm_scale(ps, sizeof ps, p.unnorm_mask, p.unnorm_dim, p.cube_mask, p.vol_mask) < 0)
        fi = -1;
    if (fi > 0 && p.srgb_output && rsx_fp_apply_srgb_output(ps, sizeof ps) < 0) fi = -1;
    if (fi > 0 && p.alpha_test && rsx_fp_apply_alpha_test_buffered(ps, sizeof ps, p.alpha_func) < 0) fi = -1;
    if (vi > 0 && fi > 0) { vs_out = vs; ps_out = ps; }
    rsx_shader_text_unlock();
    return vi > 0 && fi > 0;
}

VkShaderModule spirv_module(const std::string& hlsl, int stage)
{
    static std::vector<uint32_t> words(262144);
    uint32_t n = 0;
    char log[2048];
    if (rsx_hlsl_to_spirv(hlsl.c_str(), stage, words.data(), (uint32_t)words.size(), &n, log, sizeof log) || !n) {
        report_add((std::string("render: SPIR-V translation failed: ") + log).c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = n * 4u; ci.pCode = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    F.vkCreateShaderModule(D.device, &ci, nullptr, &m);
    return m;
}

// The engine's pipeline, for this record: its programs as the engine makes
// them, and the backend's state for them. 0 when it cannot be made.
const Pipe* pipeline(const rsx_dispatch* R, const Rec* r, const rsx_be_render_state& rs,
                     const rsx_vertex_layout_plan& layout, VkFormat rt, rsx_fp_constant_block* consts)
{
    const uint8_t* vp = r->vp->bytes.data();
    const uint32_t vp_bytes = (uint32_t)r->vp->bytes.size();
    const uint8_t* fp = r->fp->bytes.data();
    const uint32_t fp_size = (uint32_t)r->fp->bytes.size();
    // eng_pipeline_get's parameters, from the record's state
    const uint32_t fp_ctrl = rsx_dsp_shader_control(R);
    const uint32_t srgb = rsx_fp_srgb_output_enabled(rsx_dsp_reg(R, 0x1FEC), fp_ctrl, rsx_dsp_reg(R, 0x0208) & 0x1F);
    uint32_t cube = 0, vol = 0, vtex = 0;
    for (uint32_t u = 0; u < 16; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(R, u, &t);
        if (t.enabled && t.cubemap && t.width && t.width == t.height) cube |= 1u << u;
        if (t.enabled && !t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height) vol |= 1u << u;
    }
    for (uint32_t u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt; rsx_dsp_get_vertex_texture(R, u, &vt);
        if (vt.enabled && vt.width && vt.height) vtex |= 1u << u;
    }
    const int bb_used = rsx_vp_uses_branch_bits(vp, vp_bytes);
    const uint32_t bb = bb_used ? rsx_dsp_reg(R, 0x1FF8) : 0;
    uint32_t ucp = 0;
    { const uint32_t ctl = rsx_dsp_reg(R, 0x1478);
      for (uint32_t p = 0; p < 6; p++) { const uint32_t op = ctl >> (p * 4) & 0xF; if (op == 1 || op == 2) ucp |= op << (p * 2); } }
    rsx_fp_shadow_desc shadow[16] = {};
    uint32_t shadow_mask = 0;
    const uint32_t sampled = rsx_fp_texture_units(fp, fp_size);
    for (uint32_t u = 0; u < 16; u++) {
        if (!(sampled >> u & 1)) continue;
        rsx_dsp_texture t; rsx_dsp_get_texture(R, u, &t);
        rsx_fp_shadow_desc_from_texture(&shadow[u], t.format, t.dimension, t.cubemap, t.mipmaps, t.filter, t.wrap, t.control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    uint8_t gamma[16] = {}, expand[16] = {};
    for (uint32_t u = 0; u < 16; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(R, u, &t);
        if (t.enabled) {
            gamma[u] = rsx_texture_gamma_mask(t.format, t.remap, t.filter, t.wrap);
            expand[u] = rsx_texture_expand_mask(t.format, t.remap, t.filter, t.wrap);
        }
    }
    const uint32_t gamma_units = rsx_fp_texture_gamma_units(fp, fp_size, gamma);
    for (uint32_t u = 0; u < 16; u++) if (!(gamma_units >> u & 1)) gamma[u] = 0;
    const uint32_t expand_units = rsx_fp_texture_expand_units(fp, fp_size, expand);
    for (uint32_t u = 0; u < 16; u++) if (!(expand_units >> u & 1)) expand[u] = 0;
    uint32_t unnorm[16][3] = {};
    uint32_t unnorm_mask = 0;
    for (uint32_t u = 0; u < 16; u++) {
        rsx_dsp_texture t; rsx_dsp_get_texture(R, u, &t);
        if (!t.enabled || !(t.format & RSX_TEX_FMT_UNNORM) || !t.width || !t.height) continue;
        unnorm_mask |= 1u << u;
        unnorm[u][0] = t.width; unnorm[u][1] = t.height; unnorm[u][2] = t.depth;
    }
    unnorm_mask &= sampled & ~shadow_mask;
    for (uint32_t u = 0; u < 16; u++) if (!(unnorm_mask >> u & 1)) unnorm[u][0] = unnorm[u][1] = unnorm[u][2] = 0;
    consts->count = 0;
    if (rsx_fp_collect_constants(fp, fp_size, consts) < 0) return nullptr;
    rsx_dsp_surface sf; rsx_dsp_get_surface(R, &sf);
    const uint32_t wpos[2] = { sf.aa_sx ? sf.aa_sx : 1u, sf.aa_sy ? sf.aa_sy : 1u };
    const int wpos_scaled = wpos[0] > 1 || wpos[1] > 1;
    // identity: the engine's key, and the full program bytes (the native
    // cache keys exactly what it translates)
    uint64_t k = fnv(vp, vp_bytes);
    k = fnv(&bb, 4, k);
    k = rsx_fp_structural_hash(fp, fp_size, k);
    const uint32_t fp_ctrl_key = fp_ctrl & 0x4Eu;
    const uint32_t ints[] = { fp_ctrl_key, srgb, cube, vol, shadow_mask, vtex, ucp, unnorm_mask, gamma_units,
                              expand_units, wpos[0], wpos[1], layout.mask, layout.stride, (uint32_t)rt,
                              rs.alpha_test_enable, rs.alpha_test_enable ? rs.alpha_func : 0u };
    k = fnv(ints, sizeof ints, k);
    k = fnv(gamma, sizeof gamma, k); k = fnv(expand, sizeof expand, k);
    k = fnv(unnorm, sizeof unnorm, k); k = fnv(shadow, sizeof shadow, k);
    k = rsx_draw_engine_hash_render_state(&rs, k);
    auto it = s_pipes.find(k);
    if (it != s_pipes.end()) return it->second.p ? &it->second : nullptr;
    Pipe& P = s_pipes[k];
    P.p = VK_NULL_HANDLE;
    ProgParams pp = { vp, vp_bytes, fp, fp_size, fp_ctrl, cube, vol, vtex, layout.mask, bb, ucp, srgb,
                      rs.alpha_test_enable, rs.alpha_func, unnorm_mask, shadow_mask, gamma_units, expand_units,
                      { wpos[0], wpos[1] }, wpos_scaled, shadow, gamma, expand, (const uint32_t (*)[3])unnorm };
    std::string vs, ps;
    uint32_t nconst = 0;
    if (!programs_text(pp, vs, ps, &nconst) || nconst != consts->count) {
        report_add("render: a record's programs did not translate");
        return nullptr;
    }
    VkShaderModule vsm = spirv_module(vs, RSX_SHADER_STAGE_VERTEX), fsm = spirv_module(ps, RSX_SHADER_STAGE_FRAGMENT);
    if (!vsm || !fsm) return nullptr;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main", nullptr } };
    VkVertexInputBindingDescription vb = { 0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[16];
    for (uint32_t s = 0; s < layout.count; s++) va[s] = { s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = layout.count; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo ras = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    ras.polygonMode = VK_POLYGON_MODE_FILL;
    ras.cullMode = !rs.cull_enable ? VK_CULL_MODE_NONE
                 : (rs.cull_face == 0x0404u || rs.cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    ras.frontFace = rs.front_face == 0x0901u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    ras.depthBiasEnable = VK_TRUE; ras.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkColorComponentFlags wm = 0;
    if (rs.color_mask >> 16 & 0xFF) wm |= VK_COLOR_COMPONENT_R_BIT;
    if (rs.color_mask >> 8 & 0xFF) wm |= VK_COLOR_COMPONENT_G_BIT;
    if (rs.color_mask & 0xFF) wm |= VK_COLOR_COMPONENT_B_BIT;
    if (rs.color_mask >> 24 & 0xFF) wm |= VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE; cba.colorWriteMask = wm;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    const VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 5; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gci.stageCount = 2; gci.pStages = st; gci.pVertexInputState = &vi; gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vps; gci.pRasterizationState = &ras; gci.pMultisampleState = &ms; gci.pColorBlendState = &cb;
    gci.pDynamicState = &dy; gci.layout = s_pl; gci.renderPass = render_pass(rt);
    const VkResult res = F.vkCreateGraphicsPipelines(D.device, VK_NULL_HANDLE, 1, &gci, nullptr, &P.p);
    F.vkDestroyShaderModule(D.device, vsm, nullptr);
    F.vkDestroyShaderModule(D.device, fsm, nullptr);
    if (res != VK_SUCCESS) { fail("vkCreateGraphicsPipelines", res); P.p = VK_NULL_HANDLE; return nullptr; }
    P.nconst = nconst;
    stats().n_pipelines++;
    return &P;
}

// ---- a record's inputs ---------------------------------------------------------------------

rsx_dispatch* s_R;   // the scratch register file a record's state is read through

void load_state(const Rec* r)
{
    const uint32_t n = reg_count();
    for (uint32_t i = 0; i < n; i++) s_R->regs[reg_method(i) >> 2] = r->regs[i];
    memcpy(s_R->constants, r->vconst, sizeof r->vconst);
}

// The vertex fetch reads guest memory through this: the record's snapshot.
const uint8_t* snap_ptr(void* user, uint32_t location, uint32_t offset, uint32_t n)
{
    const Rec* r = (const Rec*)user;
    for (uint32_t k = 0; k < r->nvr; k++) {
        const VtxRange& v = r->vr[k];
        if (v.location == location && offset >= v.offset && (uint64_t)offset + n <= (uint64_t)v.offset + v.bytes)
            return &r->vdata[v.at + (offset - v.offset)];
    }
    return nullptr;
}

// A static texture from the bytes the recorder took (the engine's
// eng_texture_upload, one face), by content.
Img* static_texture(Batch& b, const Rec* r, uint32_t unit, const rsx_dsp_texture& t)
{
    const VtxRange* snap = nullptr;
    for (uint32_t k = 0; k < r->ntex; k++) if (r->tex[k].location == unit) snap = &r->tex[k];
    if (!snap || t.cubemap || t.dimension == 3) return nullptr;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const uint32_t nlv = rsx_texture_mip_chain(t.format, t.width, t.height, t.mipmaps, t.pitch, lv);
    if (!nlv || lv[nlv - 1].offset + lv[nlv - 1].tl.face_bytes > snap->bytes) return nullptr;
    uint32_t bpp;
    const VkFormat vf = be_vkformat(texfmt_be(lv[0].tl.fmt), &bpp);
    if (vf == VK_FORMAT_UNDEFINED) return nullptr;
    const uint8_t* src = &r->tdata[snap->at];
    uint64_t h = fnv(src, snap->bytes);
    const uint32_t key[] = { t.format, t.width, t.height, nlv, t.pitch };
    h = fnv(key, sizeof key, h);
    auto it = s_static.find(h);
    if (it != s_static.end()) return it->second;
    Img* o = make_image(vf, t.width, t.height, nlv, bpp, false);
    if (!o) return nullptr;
    to_general(b.cb, o->img, nlv);
    std::vector<uint8_t> dec;
    for (uint32_t m = 0; m < nlv; m++) {
        const rsx_tex_layout* tl = &lv[m].tl;
        dec.assign((size_t)tl->dst_row_bytes * tl->rows, 0);
        rsx_texture_decode(dec.data(), tl->dst_row_bytes, src + lv[m].offset, lv[m].w, lv[m].h, tl,
                           rsx_texture_argb_is_rgba());
        const VkDeviceSize at = upload(b, dec.data(), dec.size(), 16);
        if (at == ~(VkDeviceSize)0) { destroy_image(o); return nullptr; }
        VkBufferImageCopy c = {};
        c.bufferOffset = at; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 };
        c.imageExtent = { lv[m].w, lv[m].h, 1 };
        F.vkCmdCopyBufferToImage(b.cb, b.up.b, o->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    barrier_all(b.cb);
    o->written = 1;
    stats().n_static_textures++;
    if (s_static.size() > 256) { for (auto& kv : s_static) s_retired.push_back(kv.second); s_static.clear(); }
    return s_static[h] = o;
}

VkImageView view_of(Img* o, uint32_t control1, uint32_t fmt)
{
    const uint64_t k = (uint64_t)control1 << 32 | fmt;
    auto it = o->views.find(k);
    if (it != o->views.end()) return it->second;
    return o->views[k] = make_view(o->img, o->fmt, o->levels, remap(control1, fmt));
}

// The handoffs of a record: the engine's copies, from their slots into the
// native images of their addresses.
void apply_handoffs(Batch& b, const Rec* r)
{
    bool any = false;
    for (uint32_t k = 0; k < r->nneed; k++) {
        const int s = r->walk.hand[k];
        if (s < 0) continue;
        Img* src;
        { std::lock_guard<std::mutex> g(s_slot_lock); src = s_simg[s].im; }
        const Need& n = r->need[k];
        Img*& dst = s_targets[akey(n.location, n.offset)];
        if (!dst || dst->fmt != src->fmt || dst->w < src->w || dst->h < src->h) {
            if (dst) { s_retired.push_back(dst); stats().n_retired++; }
            dst = make_image(src->fmt, src->w, src->h, 1, src->bpp, true);
            if (!dst) continue;
            to_general(b.cb, dst->img, 1);
        }
        if (!any) barrier_all(b.cb);
        any = true;
        VkImageCopy c = {};
        c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.extent = { src->w, src->h, 1 };
        F.vkCmdCopyImage(b.cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        dst->written = 1;
        b.done.push_back([s] { std::lock_guard<std::mutex> g(s_slot_lock); s_simg[s].busy = false; });
    }
    if (any) barrier_all(b.cb);
}

// ---- chain variants -----------------------------------------------------------------------
// A frame's chain structure: the sequence of its records' targets (address
// and size) and, as a second level, of their fragment programs' content. Each
// frame's comparisons are counted under its structure and program variant.

struct FrameAcc { uint64_t shape = 1469598103934665603ull, progs = 1469598103934665603ull; uint32_t draws = 0;
                  uint32_t eq = 0, diff = 0, fin = 0, fin_eq = 0; int pending = 0; std::string desc; };
struct VarAcc { std::string desc; uint64_t frames = 0, cmp_frames = 0, eq = 0, diff = 0, fin = 0, fin_eq = 0;
                std::map<uint64_t, uint64_t> progs; };
std::mutex s_var_lock;
std::map<uint32_t, FrameAcc> s_frames;   // open and recent frames
std::map<uint64_t, VarAcc> s_vars;

void frame_note(const Rec* r, uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    FrameAcc& f = s_frames[r->frame];
    // the final composite alternates between the display buffers: one place
    const uint32_t t[3] = { r->need[0].format == 0x08 ? 0xD15Bu : r->need[0].offset, w, h };
    f.shape = fnv(t, sizeof t, f.shape);
    // programs by structure: the constants PPFX patches each frame are not a variant
    const uint64_t ph = r->fp ? rsx_fp_structural_hash(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size(), 0) : 0;
    f.progs = fnv(&ph, 8, f.progs);
    if (f.draws < 3 || f.desc.size() < 200) {
        char b[48];
        snprintf(b, sizeof b, "%s%ux%u", f.draws ? " " : "", w, h);
        if (f.draws < 64) f.desc += b;
    }
    f.draws++;
}

void frame_result(uint32_t frame, bool equal, bool final_)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    FrameAcc& f = s_frames[frame];
    (equal ? f.eq : f.diff)++;
    if (final_) { f.fin++; if (equal) f.fin_eq++; }
}

void frame_pending(uint32_t frame, int d)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    s_frames[frame].pending += d;
}

// Frames well behind the renderer whose comparisons have all run are
// complete: into their variant's totals.
void frames_close(uint32_t before)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    for (auto it = s_frames.begin(); it != s_frames.end() && it->first + 8 < before;) {
        FrameAcc& f = it->second;
        if (f.pending > 0) { ++it; continue; }
        char key[64];
        snprintf(key, sizeof key, "%u draws", f.draws);
        VarAcc& v = s_vars[f.shape];
        if (v.desc.empty()) v.desc = std::string(key) + ": " + f.desc;
        v.frames++;
        v.progs[f.progs]++;
        if (f.eq + f.diff > f.fin) v.cmp_frames++;
        v.eq += f.eq; v.diff += f.diff; v.fin += f.fin; v.fin_eq += f.fin_eq;
        it = s_frames.erase(it);
    }
}

// ---- comparison ----------------------------------------------------------------------------

char s_dump[512];
std::atomic<int> s_dumped{0};

void dump(const char* side, uint32_t frame, uint64_t seq, uint32_t off, const uint8_t* p, size_t n, uint32_t w,
          uint32_t h, VkFormat f)
{
    char path[700];
    snprintf(path, sizeof path, "%s/f%u_r%llu_%08X_%ux%u_vk%d_%s.bin", s_dump, frame, (unsigned long long)seq, off, w,
             h, (int)f, side);
    FILE* o = fopen(path, "wb");
    if (o) { fwrite(p, 1, n, o); fclose(o); }
}

// The comparisons run on a thread of their own, so the native renderer is
// not held up by them: a job per compared record, queued at its batch's
// completion (both copies are then complete), bounded (the renderer waits
// for room: counted). Readback buffers come from a pool.
struct CmpJob { Buf mine; int ref; VkDeviceSize bytes; uint32_t w, h, bpp, ref_w, ref_h, frame, off; uint64_t seq;
                int final_; VkFormat f; };
std::mutex s_cmp_lock;
std::condition_variable s_cmp_cv;
std::vector<CmpJob> s_cmp_q;
const size_t kCmpQ = 512;
std::multimap<VkDeviceSize, Buf> s_rb_pool;   // free readback buffers by size

Buf rb_take(VkDeviceSize bytes)
{
    {
        std::lock_guard<std::mutex> g(s_cmp_lock);
        auto it = s_rb_pool.lower_bound(bytes);
        if (it != s_rb_pool.end() && it->first < bytes * 2) { Buf b = it->second; s_rb_pool.erase(it); return b; }
    }
    Buf b;
    if (!make_buffer(b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return Buf();
    return b;
}

void cmp_run(CmpJob& j)
{
    const uint64_t t0 = now_ns();
    Stats& st = stats();
    st.cmp_records++;
    if (j.final_) st.cmp_finals++;
    Buf eng;
    { std::lock_guard<std::mutex> g(s_slot_lock); eng = s_sbuf[j.ref].b; }
    const bool same = j.ref_w == j.w && j.ref_h == j.h && eng.n >= j.bytes;
    const bool equal = same && !memcmp(j.mine.p, eng.p, (size_t)j.bytes);
    if (!same) st.cmp_skipped++;
    else {
        frame_result(j.frame, equal, j.final_);
        if (equal) { st.cmp_equal++; if (j.final_) st.cmp_finals_equal++; }
        else {
            st.cmp_differ++;
            uint64_t differ = 0;
            for (VkDeviceSize i = 0; i < j.bytes; i += j.bpp) differ += memcmp(j.mine.p + i, eng.p + i, j.bpp) != 0;
            static int said = 0;
            if (said++ < 40) {
                char line[240];
                snprintf(line, sizeof line, "compare: record %llu frame %u target %08X %ux%u: %llu of %u pixels differ",
                         (unsigned long long)j.seq, j.frame, j.off, j.w, j.h, (unsigned long long)differ, j.w * j.h);
                report_add(line);
            }
            if (s_dump[0] && s_dumped++ < 12) {
                dump("native", j.frame, j.seq, j.off, j.mine.p, (size_t)j.bytes, j.w, j.h, j.f);
                dump("engine", j.frame, j.seq, j.off, eng.p, (size_t)j.bytes, j.w, j.h, j.f);
            }
        }
    }
    frame_pending(j.frame, -1);
    { std::lock_guard<std::mutex> g(s_slot_lock); s_sbuf[j.ref].busy = false; }
    { std::lock_guard<std::mutex> g(s_cmp_lock); s_rb_pool.emplace(j.mine.n, j.mine); }
    st.compare_ns += now_ns() - t0;
}

void cmp_thread()
{
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"ppfx compare");
#endif
    for (;;) {
        CmpJob j;
        {
            std::unique_lock<std::mutex> lk(s_cmp_lock);
            s_cmp_cv.wait(lk, [] { return !s_cmp_q.empty(); });
            j = s_cmp_q.front();
            s_cmp_q.erase(s_cmp_q.begin());
        }
        s_cmp_cv.notify_all();
        cmp_run(j);
    }
}

void compare_after(Batch& b, const Rec* r, Img* t, uint32_t w, uint32_t h)
{
    const int ref = r->walk.ref;
    if (ref < 0) return;
    CmpJob j;
    j.bytes = (VkDeviceSize)w * h * t->bpp;
    j.mine = rb_take(j.bytes);
    if (!j.mine.b) { std::lock_guard<std::mutex> g(s_slot_lock); s_sbuf[ref].busy = false; return; }
    barrier_all(b.cb);
    VkBufferImageCopy c = {};
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { w, h, 1 };
    F.vkCmdCopyImageToBuffer(b.cb, t->img, VK_IMAGE_LAYOUT_GENERAL, j.mine.b, 1, &c);
    // what the comparison needs after the record is gone
    j.ref = ref; j.w = w; j.h = h; j.bpp = t->bpp; j.ref_w = r->walk.ref_w; j.ref_h = r->walk.ref_h;
    j.frame = r->frame; j.off = r->need[0].offset; j.seq = r->seq; j.final_ = r->need[0].format == 0x08; j.f = t->fmt;
    frame_pending(j.frame, +1);
    b.done.push_back([j] {
        const uint64_t t0 = now_ns();
        std::unique_lock<std::mutex> lk(s_cmp_lock);
        if (s_cmp_q.size() >= kCmpQ) stats().cmp_queue_waits++;
        s_cmp_cv.wait(lk, [] { return s_cmp_q.size() < kCmpQ; });
        s_cmp_q.push_back(j);
        lk.unlock();
        s_cmp_cv.notify_all();
        stats().cmp_queue_wait_ns += now_ns() - t0;
    });
}

// ---- one record ------------------------------------------------------------------------------

void unsupported(const Rec* r, const char* why)
{
    stats().n_unsupported++;
    static int said = 0;
    if (said++ < 20) {
        char line[200];
        snprintf(line, sizeof line, "render: record %llu (frame %u) not rendered: %s", (unsigned long long)r->seq,
                 r->frame, why);
        report_add(line);
    }
}

// The engine's eng_scissor_rect, on the record's state.
void scissor_rect(const rsx_dsp_surface* sf, uint32_t w, uint32_t h, uint32_t out[4])
{
    uint32_t sx = 0, sy = 0, sw = w, sh = h;
    const uint32_t hr = rsx_dsp_reg(s_R, 0x08C0), vr = rsx_dsp_reg(s_R, 0x08C4);
    const uint32_t gx = (hr & 0xFFFFu) * sf->aa_sx, gw = (hr >> 16) * sf->aa_sx;
    const uint32_t gy = (vr & 0xFFFFu) * sf->aa_sy, gh = (vr >> 16) * sf->aa_sy;
    if (gw > 0 && gh > 0) {
        uint32_t right = sx + sw, bottom = sy + sh;
        if (gx > sx) sx = gx;
        if (gy > sy) sy = gy;
        if (gx + gw < right) right = gx + gw;
        if (gy + gh < bottom) bottom = gy + gh;
        sw = right > sx ? right - sx : 0;
        sh = bottom > sy ? bottom - sy : 0;
    }
    out[0] = sx; out[1] = sy; out[2] = sw; out[3] = sh;
}

long long s_waited = -1;   // the last engine signal a batch waits on

// The engine's signal a record's copies are ordered by: waited on once, in
// the batch of the first record that needs it.
void wait_for(Batch& b, const Rec* r)
{
    if (r->walk.sem < 0 || r->walk.ordinal <= s_waited) return;
    b.sems.push_back(r->walk.sem);
    b.ords.push_back(r->walk.ordinal);
    s_waited = r->walk.ordinal;
}

void draw(const Rec* r)
{
    Batch& b = batch(r->frame);
    wait_for(b, r);
    if (r->why) { unsupported(r, "no programs or vertex data"); return; }
    load_state(r);
    rsx_dsp_surface sf; rsx_dsp_get_surface(s_R, &sf);
    if (sf.color_target != 1 || sf.antialias) { unsupported(r, "MRT or antialiased target"); return; }
    rsx_be_render_state rs; rsx_draw_engine_decode_render_state(s_R, &rs);
    if (rs.blend_enable || rs.blend_mrt || rs.depth_test || rs.stencil_enable || rs.alpha_test_enable ||
        rs.cull_enable || rs.depth_bounds_test) { unsupported(r, "render state beyond PPFX's (blend, depth, stencil, alpha test, cull)"); return; }
    uint32_t bpp = 0;
    const VkFormat rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
    const uint32_t W = sf.clip_w, H = sf.clip_h;
    if (!W || !H) { unsupported(r, "no clip"); return; }
    apply_handoffs(b, r);
    Img* t = target_image(b, sf.color_location[0], sf.color_offset[0], rt, bpp, W, H);
    if (!t) { unsupported(r, "target image"); return; }
    // the vertex layout and the vertices
    rsx_vertex_layout_plan layout;
    {
        uint32_t mask = 0xFFFFu;
        rsx_vp_input_analysis an = { 0xFFFFu, 0 };
        const uint32_t instrs = (uint32_t)r->vp->bytes.size() / 16;
        if (rsx_vp_analyze_inputs(r->vp->bytes.data(), instrs * 16u, &an) == (int)instrs && an.exact && an.input_mask)
            mask = an.input_mask;
        rsx_vertex_layout_plan_init(&layout, mask);
    }
    int indexed = 0;
    if (!rsx_vertex_topology_plan(r->prim, 0, &indexed)) { unsupported(r, "a primitive the engine does not rebuild"); return; }
    const uint32_t n = r->count;
    std::vector<rsx_vertex_ref> refs(n);
    for (uint32_t i = 0; i < n; i++) refs[i] = { r->first + i, 0 };
    rsx_vertex_fetch_plan plan;
    rsx_vertex_fetch_plan_init(&plan, s_R, &layout, snap_ptr, (void*)r);
    rsx_vertex_fetch_plan_prepare(&plan, refs.data(), n);
    std::vector<uint8_t> verts((size_t)n * layout.stride);
    if (!rsx_vertex_fetch_many(&plan, refs.data(), n, verts.data())) { unsupported(r, "vertex fetch"); return; }
    const uint32_t n_draw = indexed ? rsx_draw_engine_topology_index_count(r->prim, n, nullptr, 0) : n - n % 3;
    std::vector<uint32_t> idx(n_draw ? n_draw : 1);
    if (indexed) rsx_draw_engine_write_topology_indices(r->prim, n, nullptr, 0, nullptr, idx.data());
    else for (uint32_t i = 0; i < n_draw; i++) idx[i] = i;
    // the pipeline
    rsx_fp_constant_block fc;
    const Pipe* P = pipeline(s_R, r, rs, layout, rt, &fc);
    if (!P) { unsupported(r, "pipeline"); return; }
    // constants: the engine's block and the viewport epilogue; the fragment
    // program's buffered constants and the alpha/window slot
    rsx_dsp_viewport vp; rsx_dsp_get_viewport(s_R, &vp);
    const float Wf = (float)W, Hf = (float)H;
    const float Wv = Wf / (float)sf.aa_sx, Hv = Hf / (float)sf.aa_sy;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    uint8_t vcb[512 * 16 + 32];
    memcpy(vcb, r->vconst, 512 * 16);
    memcpy(vcb + 512 * 16, xf, sizeof xf);
    const uint32_t nslots = fc.count ? fc.count : 1u;
    std::vector<uint8_t> pcb((nslots + 1) * 16, 0);
    if (fc.count) memcpy(pcb.data(), fc.values, fc.count * 16u);
    {
        float* alpha = (float*)(pcb.data() + nslots * 16);
        alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
        const uint32_t win = rsx_dsp_reg(s_R, 0x1D88);
        alpha[1] = (win >> 12 & 0xF) ? (float)(win & 0xFFF) : 0.0f;
        alpha[2] = (win >> 16 & 0xF) ? -0.5f : 0.0f;
        alpha[3] = 0.0f;
    }
    // textures
    VkDescriptorImageInfo ii[16], si[16];
    const uint32_t sampled = rsx_fp_texture_units(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size());
    for (uint32_t u = 0; u < 16; u++) {
        ii[u] = { VK_NULL_HANDLE, s_dummy->view, VK_IMAGE_LAYOUT_GENERAL };
        si[u] = { s_dummy_smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        if (!(sampled >> u & 1)) continue;
        rsx_dsp_texture tx; rsx_dsp_get_texture(s_R, u, &tx);
        if (!tx.enabled) continue;
        Img* src = nullptr;
        auto it = s_targets.find(akey(tx.location, tx.offset));
        if (it != s_targets.end() && it->second->written) src = it->second;
        if (src && src == t) {
            // its own target: a copy taken just before the draw
            Img*& snap = s_snap[std::make_tuple(t->fmt, t->w, t->h)];
            if (!snap) { snap = make_image(t->fmt, t->w, t->h, 1, t->bpp, false); if (snap) to_general(b.cb, snap->img, 1); }
            if (!snap) { unsupported(r, "snapshot image"); return; }
            barrier_all(b.cb);
            VkImageCopy c = {};
            c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.extent = { t->w, t->h, 1 };
            F.vkCmdCopyImage(b.cb, t->img, VK_IMAGE_LAYOUT_GENERAL, snap->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            barrier_all(b.cb);
            src = snap;
            stats().n_feedback++;
        }
        if (!src) src = static_texture(b, r, u, tx);
        if (!src) {
            char why[200];
            snprintf(why, sizeof why, "unit %u at %u:%08X (format %02X %ux%u pitch %u) has no native source%s", u,
                     tx.location, tx.offset, tx.format, tx.width, tx.height, tx.pitch,
                     it != s_targets.end() ? " (a native image not yet written)" : "");
            unsupported(r, why);
            return;
        }
        ii[u].imageView = view_of(src, tx.remap & 0xFFFFu, tx.format);
        si[u].sampler = sampler(tx.filter, tx.wrap, tx.control0);
    }
    // uploads
    const VkDeviceSize o_vc = upload(b, vcb, sizeof vcb), o_pc = upload(b, pcb.data(), pcb.size());
    const VkDeviceSize o_v = upload(b, verts.data(), verts.size(), 16), o_i = upload(b, idx.data(), idx.size() * 4, 16);
    if (o_vc == ~(VkDeviceSize)0 || o_pc == ~(VkDeviceSize)0 || o_v == ~(VkDeviceSize)0 || o_i == ~(VkDeviceSize)0 ||
        b.sets >= kSets) {
        unsupported(r, "the batch is full");   // not expected: batches are submitted well before
        return;
    }
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = b.dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dsl;
    VkDescriptorSet set;
    if (F.vkAllocateDescriptorSets(D.device, &dai, &set) != VK_SUCCESS) { unsupported(r, "descriptor set"); return; }
    b.sets++;
    VkDescriptorBufferInfo bi[2] = { { b.up.b, o_vc, sizeof vcb }, { b.up.b, o_pc, pcb.size() } };
    VkWriteDescriptorSet wr[4] = {};
    for (int k = 0; k < 4; k++) { wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = set; }
    wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[0].pBufferInfo = &bi[0];
    wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[1].pBufferInfo = &bi[1];
    wr[2].dstBinding = 16; wr[2].descriptorCount = 16; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[2].pImageInfo = ii;
    wr[3].dstBinding = 48; wr[3].descriptorCount = 16; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; wr[3].pImageInfo = si;
    F.vkUpdateDescriptorSets(D.device, 4, wr, 0, nullptr);
    // the draw. DOTHEROES_PPFX_HAZARD_TEST=1 (the synchronisation check's
    // negative control): one draw in 5000 goes without its barrier, so the
    // validation layer must report a hazard
    static int hazard = -1;
    if (hazard < 0) { const char* e = getenv("DOTHEROES_PPFX_HAZARD_TEST"); hazard = e && *e == '1'; }
    if (!(hazard && stats().n_draws % 5000 == 4999)) barrier_all(b.cb);
    else report_add("render: hazard test: a draw recorded without its barrier");
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = render_pass(rt); rbi.framebuffer = framebuffer(t);
    rbi.renderArea = { { 0, 0 }, { t->w, t->h } };
    F.vkCmdBeginRenderPass(b.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    F.vkCmdBindPipeline(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->p);
    VkViewport v = { 0.0f, 0.0f, Wf, Hf, 0.0f, 1.0f };
    F.vkCmdSetViewport(b.cb, 0, 1, &v);
    uint32_t sc[4];
    scissor_rect(&sf, t->w, t->h, sc);
    VkRect2D s = { { (int32_t)sc[0], (int32_t)sc[1] }, { sc[2], sc[3] } };
    F.vkCmdSetScissor(b.cb, 0, 1, &s);
    F.vkCmdSetStencilReference(b.cb, VK_STENCIL_FACE_FRONT_AND_BACK, rsx_dsp_reg(s_R, 0x0334) & 0xFFu);
    F.vkCmdSetDepthBias(b.cb, 0.0f, 0.0f, 0.0f);
    const float bc[4] = { 0, 0, 0, 0 };   // blend is off (checked above): the constant is never read
    F.vkCmdSetBlendConstants(b.cb, bc);
    F.vkCmdBindDescriptorSets(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl, 0, 1, &set, 0, nullptr);
    F.vkCmdBindVertexBuffers(b.cb, 0, 1, &b.up.b, &o_v);
    F.vkCmdBindIndexBuffer(b.cb, b.up.b, o_i, VK_INDEX_TYPE_UINT32);
    F.vkCmdDrawIndexed(b.cb, n_draw, 1, 0, 0, 0);
    F.vkCmdEndRenderPass(b.cb);
    t->written = 1;
    b.records++;
    stats().n_draws++;
    frame_note(r, W, H);
    compare_after(b, r, t, W, H);
}

void clear(const Rec* r)
{
    Batch& b = batch(r->frame);
    wait_for(b, r);
    load_state(r);
    rsx_dsp_surface sf; rsx_dsp_get_surface(s_R, &sf);
    uint32_t bpp = 0;
    const VkFormat rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
    if (!sf.clip_w || !sf.clip_h || sf.color_target != 1) { unsupported(r, "clear target"); return; }
    apply_handoffs(b, r);
    Img* t = target_image(b, sf.color_location[0], sf.color_offset[0], rt, bpp, sf.clip_w, sf.clip_h);
    if (!t) return;
    const uint32_t c = rsx_dsp_clear_color(s_R);
    VkClearColorValue cv;
    cv.float32[0] = (float)(c >> 16 & 0xFF) / 255.0f; cv.float32[1] = (float)(c >> 8 & 0xFF) / 255.0f;
    cv.float32[2] = (float)(c & 0xFF) / 255.0f; cv.float32[3] = (float)(c >> 24 & 0xFF) / 255.0f;
    // the engine's eng_clear_rect
    const uint32_t cw = sf.clip_w < t->w ? sf.clip_w : t->w, ch = sf.clip_h < t->h ? sf.clip_h : t->h;
    uint32_t rc[4];
    scissor_rect(&sf, cw, ch, rc);
    if (!rc[2] || !rc[3]) return;
    barrier_all(b.cb);
    if (rc[0] == 0 && rc[1] == 0 && rc[2] >= cw && rc[3] >= ch) {
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        F.vkCmdClearColorImage(b.cb, t->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg);
    } else {
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rbi.renderPass = render_pass(rt); rbi.framebuffer = framebuffer(t);
        rbi.renderArea = { { 0, 0 }, { t->w, t->h } };
        F.vkCmdBeginRenderPass(b.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, {} };
        ca.clearValue.color = cv;
        VkClearRect cr = { { { (int32_t)rc[0], (int32_t)rc[1] }, { rc[2], rc[3] } }, 0, 1 };
        F.vkCmdClearAttachments(b.cb, 1, &ca, 1, &cr);
        F.vkCmdEndRenderPass(b.cb);
    }
    t->written = 1;
    stats().n_clears++;
}

// ---- the thread ------------------------------------------------------------------------------

bool init()
{
    while (rsx_vk_bridge_device(&D)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
#define X(f) F.f = (PFN_##f)D.get_device_proc(D.device, #f); if (!F.f) { fail(#f, VK_ERROR_INITIALIZATION_FAILED); return false; }
    PPFX_VK_DEVICE(X)
#undef X
    F.GetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)D.get_instance_proc(D.instance, "vkGetPhysicalDeviceMemoryProperties");
    F.GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)D.get_instance_proc(D.instance, "vkGetPhysicalDeviceProperties");
    F.GetPhysicalDeviceMemoryProperties(D.phys, &s_mem);
    VkPhysicalDeviceProperties props;
    F.GetPhysicalDeviceProperties(D.phys, &props);
    s_ts_period = props.limits.timestampPeriod;
    {
        char line[200];
        snprintf(line, sizeof line, "render: on %s queue of family %u (%s)", D.shared_queue ? "the backend's" : "a second",
                 D.queue_family, props.deviceName);
        report_add(line);
    }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = D.queue_family;
    if (F.vkCreateCommandPool(D.device, &pci, nullptr, &s_pool) != VK_SUCCESS) return false;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding lb[4] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr },
        { 16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        { 48, VK_DESCRIPTOR_TYPE_SAMPLER, 16, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr } };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 4; lci.pBindings = lb;
    F.vkCreateDescriptorSetLayout(D.device, &lci, nullptr, &s_dsl);
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_dsl;
    F.vkCreatePipelineLayout(D.device, &plci, nullptr, &s_pl);
    for (Batch& b : s_b) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = s_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        F.vkAllocateCommandBuffers(D.device, &ai, &b.cb);
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        F.vkCreateFence(D.device, &fi, nullptr, &b.fence);
        VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSets },
                                       { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * kSets },
                                       { VK_DESCRIPTOR_TYPE_SAMPLER, 16 * kSets } };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = kSets; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
        F.vkCreateDescriptorPool(D.device, &dpi, nullptr, &b.dp);
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2;
        F.vkCreateQueryPool(D.device, &qi, nullptr, &b.qp);
        if (!make_buffer(b.up, kUpload, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return false;
    }
    // the dummy every unbound unit samples (as the engine's: 1x1 magenta)
    {
        Batch& b = batch(0);
        s_dummy = make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 4, false);
        to_general(b.cb, s_dummy->img, 1);
        const uint8_t magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
        const VkDeviceSize at = upload(b, magenta, 4, 16);
        VkBufferImageCopy c = {};
        c.bufferOffset = at; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; c.imageExtent = { 1, 1, 1 };
        F.vkCmdCopyBufferToImage(b.cb, b.up.b, s_dummy->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        submit();
        s_dummy_smp = sampler(0x02020000u, 0x00030303u, 0);
    }
    s_R = (rsx_dispatch*)malloc(sizeof(rsx_dispatch));
    rsx_dispatch_init(s_R, nullptr);
    const char* d = getenv("DOTHEROES_PPFX_DUMP");
    snprintf(s_dump, sizeof s_dump, "%s", d && *d ? d : "");
#ifdef _WIN32
    if (s_dump[0]) CreateDirectoryA(s_dump, NULL);
#endif
    s_ready = 1;
    return true;
}

void run()
{
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"ppfx native");
#endif
    if (!init()) { report_add("render: the native renderer did not start"); return; }
    Queue& q = queue();
    uint64_t next = 0;
    uint32_t frame = 0;
    for (;;) {
        for (Batch& b : s_b) if (retire_ready(b)) retire(b);
        if (!s_retired.empty()) {
            // retired images: destroyed once every batch in flight has completed
            bool idle = true;
            for (Batch& b : s_b) idle &= !b.pending;
            if (idle) { for (Img* o : s_retired) destroy_image(o); s_retired.clear(); }
        }
        if (next >= q.walked.load(std::memory_order_acquire)) {
            // nothing walked yet: submit what is open, so its work and
            // comparisons go ahead, and wait for the walker
            const uint64_t t0 = now_ns();
            submit();
            std::unique_lock<std::mutex> lk(q.m);
            q.cv.wait_for(lk, std::chrono::milliseconds(2),
                          [&] { return next < q.walked.load(std::memory_order_acquire); });
            stats().wait_walker_us += (now_ns() - t0) / 1000;
            continue;
        }
        Rec* r = q.at(next);
        if (r->frame != frame && s_b[s_cur].open) submit();   // a frame's work in a batch of its own
        if (r->frame != frame) frames_close(r->frame);
        frame = r->frame;
        // DOTHEROES_PPFX_DELAY_MS=<n> (a test): a slow consumer, n ms a record
        static int delay = -1;
        if (delay < 0) { const char* e = getenv("DOTHEROES_PPFX_DELAY_MS"); delay = e && *e ? atoi(e) : 0; }
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        const uint64_t t0 = now_ns();
        if (r->kind == kDraw) draw(r);
        else if (r->kind == kClear) clear(r);
        stats().cpu_record_ns += now_ns() - t0;
        if (s_b[s_cur].open && (s_b[s_cur].used > kUpload / 2 || s_b[s_cur].sets > kSets / 2)) submit();
        next++;
        q.done.store(next, std::memory_order_release);
        q.cv.notify_all();
    }
}

}  // namespace

uint32_t surface_vkformat(uint32_t color_format, uint32_t* bpp)
{
    rsx_be_format f;   // the engine's eng_surface_format
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: f = RSX_BE_FMT_R16G16B16A16F; break;
    case 0x0C: f = RSX_BE_FMT_R32G32B32A32F; break;
    case 0x0D: f = RSX_BE_FMT_R32F; break;
    default: f = RSX_BE_FMT_R8G8B8A8; break;
    }
    return (uint32_t)be_vkformat(f, bpp);
}

uint32_t be_vkformat_pub(uint32_t be_format, uint32_t* bpp) { return (uint32_t)be_vkformat((rsx_be_format)be_format, bpp); }

bool render_ready() { return s_ready.load() != 0; }

void render_report(std::vector<std::string>& out)
{
    std::lock_guard<std::mutex> g(s_var_lock);
    char line[512];
    for (auto& kv : s_vars) {
        const VarAcc& v = kv.second;
        snprintf(line, sizeof line, "variant %016llx: %llu frames (%zu program variants), %llu with every draw compared; "
                 "draws equal %llu differ %llu; finals equal %llu of %llu | %s", (unsigned long long)kv.first,
                 (unsigned long long)v.frames, v.progs.size(), (unsigned long long)v.cmp_frames,
                 (unsigned long long)v.eq, (unsigned long long)v.diff, (unsigned long long)v.fin_eq,
                 (unsigned long long)v.fin, v.desc.c_str());
        out.push_back(line);
    }
}

int slot_image(uint32_t vkformat, uint32_t w, uint32_t h, uint64_t* image)
{
    if (!render_ready()) return -1;
    std::lock_guard<std::mutex> g(s_slot_lock);
    for (size_t i = 0; i < s_simg.size(); i++) {
        SlotImg& s = s_simg[i];
        if (!s.busy && s.im->fmt == (VkFormat)vkformat && s.im->w == w && s.im->h == h) {
            s.busy = true; *image = (uint64_t)s.im->img; return (int)i;
        }
    }
    if (s_simg.size() >= kMaxSlotImages) return -1;
    uint32_t bpp = 0;
    switch ((VkFormat)vkformat) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: bpp = 16; break;
    default: bpp = 4; break;
    }
    // created here (the walker's thread): vkCreateImage is free-threaded,
    // and it reaches the GENERAL layout through the copy's own barrier below
    Img* o = make_image((VkFormat)vkformat, w, h, 1, bpp, false);
    if (!o) return -1;
    s_simg.push_back({ o, true });
    *image = (uint64_t)o->img;
    return (int)s_simg.size() - 1;
}

int slot_buffer(uint64_t bytes, uint64_t* buffer, bool final_)
{
    if (!render_ready()) return -1;
    std::lock_guard<std::mutex> g(s_slot_lock);
    size_t busy = 0;
    for (const SlotBuf& b : s_sbuf) busy += b.busy;
    if (!final_ && busy + kFinalReserve >= kMaxSlotBuffers) return -1;
    for (size_t i = 0; i < s_sbuf.size(); i++)
        if (!s_sbuf[i].busy && s_sbuf[i].b.n >= bytes) { s_sbuf[i].busy = true; *buffer = (uint64_t)s_sbuf[i].b.b; return (int)i; }
    if (s_sbuf.size() >= kMaxSlotBuffers) return -1;
    SlotBuf s;
    if (!make_buffer(s.b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return -1;
    s.busy = true;
    s_sbuf.push_back(s);
    *buffer = (uint64_t)s.b.b;
    return (int)s_sbuf.size() - 1;
}

int slot_semaphore(uint64_t* semaphore)
{
    if (!render_ready()) return -1;
    std::lock_guard<std::mutex> g(s_slot_lock);
    for (size_t i = 0; i < s_ssem.size(); i++)
        if (!s_ssem[i].busy) { s_ssem[i].busy = true; *semaphore = (uint64_t)s_ssem[i].s; return (int)i; }
    if (s_ssem.size() >= kMaxSlotSems) return -1;
    VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore s = VK_NULL_HANDLE;
    if (F.vkCreateSemaphore(D.device, &ci, nullptr, &s) != VK_SUCCESS) return -1;
    s_ssem.push_back({ s, true });
    *semaphore = (uint64_t)s;
    return (int)s_ssem.size() - 1;
}

void slot_release_image(int slot)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (slot >= 0 && (size_t)slot < s_simg.size()) s_simg[slot].busy = false;
}

void slot_release_buffer(int slot)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (slot >= 0 && (size_t)slot < s_sbuf.size()) s_sbuf[slot].busy = false;
}

void slot_release_semaphore(int slot)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (slot >= 0 && (size_t)slot < s_ssem.size()) s_ssem[slot].busy = false;
}

bool compare_wanted(const Rec* r)
{
    static int every = -1, off = -1;
    if (every < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE_EVERY"); every = e && *e ? atoi(e) : 30; }
    // DOTHEROES_PPFX_COMPARE=0: no comparison at all (the native renderer's own cost)
    if (off < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE"); off = e && *e == '0'; }
    if (off || r->kind != kDraw) return false;
    const bool final_ = r->need[0].format == 0x08;   // the final composite: the A8R8G8B8 display buffer
    return final_ || (every > 0 && r->frame % (uint32_t)every == 0);
}

void render_start()
{
    std::thread(run).detach();
    std::thread(cmp_thread).detach();
}

}  // namespace ppfx
