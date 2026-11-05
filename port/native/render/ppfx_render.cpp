// The native PPFX renderer (docs/NATIVE_R3.md, docs/NATIVE_R3B.md): a thread
// of its own, with a Vulkan queue and command pool of its own on the existing
// renderer's device, rendering PPFX's records (ppfx_recorder.cpp) into images
// of its own.
//
// From a record's explicit description (ppfx_native.h State): its programs go
// through the same translators and the shared SPIR-V cache, with the
// parameters the record's own fields give (the textures' formats, the
// samplers, the surface); the vertex inputs through the toolkit's vertex
// fetch, from the record's snapshot of their bytes; the render state straight
// into the pipeline. No register file is read or written here: where a
// toolkit helper takes a hardware word (the translators' shader control,
// the remap crossbar), it is made right at the call from the record's
// fields.
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
// Synchronisation within the native queue (DOTHEROES_PPFX_BARRIERS): by
// default each image's last write and reads since are tracked and each
// access gets the barrier its hazard needs, in the layout it needs
// (R2's executor, r1_replay.c r2_access); "full" is R3's way -- every image
// in GENERAL and a full barrier between operations -- kept for comparison.
// Across queues: the engine's copies into slots are ordered by the
// semaphore their submission signals, waited on before the first record that
// needs them.
//
// Lifetime. Work is recorded into batches (a command buffer, a fence, a
// descriptor pool, an upload buffer and timestamp queries each), submitted at
// frame ends and when they fill. A batch's completion (its fence) releases
// what it used: slots go back to the bridge, retired images are destroyed,
// comparisons run. Records are released to the recorder (`done`) once
// recorded: nothing here reads a record after that.
//
// Comparison. For the records the walker copied the engine's output of
// (compare_wanted), the native output is read back after the record and
// compared byte for byte once both copies are complete. Every comparison
// asked for ends as exactly one of: equal, differ, skipped (and why), or
// unfinished at exit (render_drain waits for the in-flight ones first).
// DOTHEROES_PPFX_DUMP=<dir>: the first differing outputs, both sides, raw.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ppfx_native.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
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
#include "rsx_vertex_formats.h"
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
int s_full = 0;   // DOTHEROES_PPFX_BARRIERS=full: R3's full barriers, every image in GENERAL

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
bool make_buffer(Buf& o, VkDeviceSize n, VkBufferUsageFlags usage, bool readback = false, bool device = false)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = n; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (F.vkCreateBuffer(D.device, &ci, nullptr, &o.b) != VK_SUCCESS) return false;
    VkMemoryRequirements rq;
    F.vkGetBufferMemoryRequirements(D.device, o.b, &rq);
    uint32_t t = ~0u;
    // the batches' uploads (vertices, constants) in video memory the CPU can
    // write (the default; DOTHEROES_PPFX_UPLOAD=host: system memory, R3's way, 0.45 ms
    // a frame more of GPU time), when the device has such memory
    if (device)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (readback)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t == ~0u)
        t = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = rq.size; ai.memoryTypeIndex = t;
    if (t == ~0u || F.vkAllocateMemory(D.device, &ai, nullptr, &o.m) != VK_SUCCESS) return false;
    if (device) {   // which memory the uploads got, for the report
        static int said = 0;
        if (!said++) report_add((s_mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                    ? "render: uploads in device-local memory the CPU writes"
                                    : "render: uploads in system memory (no device-local host-visible memory)");
    }
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
    // the hazard tracking (the native queue's view of it): its layout, the
    // stage and access of its last write, the stages that read it since
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags wstage = 0, rstages = 0;
    VkAccessFlags waccess = 0;
    bool external = false;                             // a slot: the engine's queue writes it, a semaphore orders it
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

// ---- synchronisation ---------------------------------------------------------------

// R3's way (DOTHEROES_PPFX_BARRIERS=full): a full barrier between operations
// (the engine's vk_cmd_after_everything).
void barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    F.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    stats().n_barriers++;
    stats().n_barriers_full++;
}

// One access to an image, with the barrier its hazard needs: read after
// write, write after read or write, a layout change (R2's r2_access). In
// full mode: the full barrier, and every image stays in GENERAL.
enum Use { kAttach, kSampled, kCopySrc, kCopyDst };
void access(VkCommandBuffer cb, Img* o, Use use)
{
    if (s_full) {
        if (o->layout != VK_IMAGE_LAYOUT_GENERAL) {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = o->img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, o->levels, 0, 1 };
            F.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &b);
            o->layout = VK_IMAGE_LAYOUT_GENERAL;
            stats().n_barriers++;
        }
        barrier_all(cb);
        return;
    }
    VkImageLayout want; VkPipelineStageFlags stage; VkAccessFlags acc; bool write;
    switch (use) {
    case kAttach: want = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                  acc = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; write = true; break;
    case kSampled: want = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                   acc = VK_ACCESS_SHADER_READ_BIT; write = false; break;
    case kCopySrc: want = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                   acc = VK_ACCESS_TRANSFER_READ_BIT; write = false; break;
    default: want = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
             acc = VK_ACCESS_TRANSFER_WRITE_BIT; write = true; break;
    }
    // a slot stays in GENERAL, as the engine's copy left it; the semaphore
    // has ordered the native queue after that copy
    if (o->external) want = VK_IMAGE_LAYOUT_GENERAL;
    VkPipelineStageFlags src = 0;
    VkAccessFlags srca = 0;
    bool need = false;
    if (o->layout != want) {                 // a transition: after every earlier access
        src |= o->wstage | o->rstages; srca |= o->waccess; need = true;
    } else if (write) {                      // WAW, WAR
        if (o->wstage || o->rstages) { src |= o->wstage | o->rstages; srca |= o->waccess; need = true; }
    } else if (o->wstage && !(o->rstages & stage)) {   // RAW, once per reading stage
        src |= o->wstage; srca |= o->waccess; need = true;
    }
    if (need) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = srca; b.dstAccessMask = acc;
        b.oldLayout = o->layout; b.newLayout = want;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = o->img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, o->levels, 0, 1 };
        F.vkCmdPipelineBarrier(cb, src ? src : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stage, 0, 0, nullptr, 0, nullptr, 1, &b);
        stats().n_barriers++;
    }
    if (write) { o->wstage = stage; o->waccess = acc; o->rstages = 0; }
    else if (need && o->layout != want) o->rstages = stage;
    else o->rstages |= stage;
    o->layout = want;
}

// The layout an image is sampled in.
VkImageLayout sampled_layout() { return s_full ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
VkImageLayout layout_of(const Img* o) { return o->layout; }

// ---- the bridge's slots ----------------------------------------------------------------
// Staging images (handoffs), buffers (the engine's outputs for comparison)
// and binary semaphores. The walker takes one (slot_*), the record names it,
// and the batch that used it gives it back once complete.

struct SlotImg { Img* im; bool busy; };
struct SlotBuf { Buf b; bool busy; int prio; };
struct SlotSem { VkSemaphore s; bool busy; };
std::mutex s_slot_lock;
std::vector<SlotImg> s_simg;
std::vector<SlotBuf> s_sbuf;
std::vector<SlotSem> s_ssem;
const size_t kMaxSlotImages = 128, kMaxSlotBuffers = 512, kMaxSlotSems = 64;
// Buffers kept back for the comparisons that must run: final composites (one
// a frame) and the frames after a structure change (an area's first frames).
const size_t kFinalReserve = 32, kChangeReserve = 160;
const VkDeviceSize kSlotBufferBytes = 768ull << 20;   // the engine-side reference buffers, at most
const VkDeviceSize kReadbackPoolBytes = 256ull << 20; // free native readback buffers kept for reuse

// ---- batches --------------------------------------------------------------------------

// What a stretch of a batch's GPU time went to: the time from one timestamp
// to the next goes to the operation recorded between them (its barriers
// included).
enum Sect : uint8_t { kSectDraw = 0, kSectCopy = 1, kSectReadback = 2, kSectOther = 3 };
const uint32_t kQueries = 2048;

struct Batch {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    VkQueryPool qp = VK_NULL_HANDLE;
    Buf up;
    VkDeviceSize used = 0;
    bool open = false, pending = false;
    uint32_t frame = 0, records = 0, sets = 0, nq = 0;
    std::vector<uint8_t> sect;                   // per timestamp after the first: its section
    std::vector<int> sems;                       // semaphore slots to wait on ...
    std::vector<long long> ords;                 // ... and their signals' ordinals
    std::vector<std::function<void()>> done;     // after completion
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

// A timestamp: the GPU time since the one before goes to `s`.
void mark(Batch& b, Sect s)
{
    if (b.nq >= kQueries) return;
    F.vkCmdWriteTimestamp(b.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b.qp, b.nq++);
    b.sect.push_back(s);
}

void retire(Batch& b)
{
    if (!b.pending) return;
    F.vkWaitForFences(D.device, 1, &b.fence, VK_TRUE, UINT64_MAX);
    if (b.qp && b.nq >= 2) {
        std::vector<uint64_t> ts(b.nq);
        if (F.vkGetQueryPoolResults(D.device, b.qp, 0, b.nq, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS) {
            Stats& st = stats();
            st.gpu_ns += (uint64_t)((double)(ts[b.nq - 1] - ts[0]) * s_ts_period);
            for (uint32_t i = 1; i < b.nq; i++) {
                const uint64_t d = (uint64_t)((double)(ts[i] - ts[i - 1]) * s_ts_period);
                switch (b.sect[i - 1]) {
                case kSectDraw: st.gpu_draw_ns += d; break;
                case kSectCopy: st.gpu_copy_ns += d; break;
                case kSectReadback: st.gpu_readback_ns += d; break;
                default: st.gpu_other_ns += d; break;
                }
            }
        }
    }
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
    F.vkCmdResetQueryPool(b.cb, b.qp, 0, kQueries);
    b.nq = 0; b.sect.clear();
    F.vkCmdWriteTimestamp(b.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, b.qp, b.nq++);
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
    // the readback copies visible to the host for the comparisons
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    F.vkCmdPipelineBarrier(b.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    mark(b, kSectOther);
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

// A texture's view swizzle: its CellGcmTexture.remap crossbar, as the asset
// helper reads it for the texture's format.
VkComponentMapping remap(uint32_t crossbar, uint32_t fmt)
{
    uint8_t sel[4];
    rsx_texture_component_remap(crossbar, fmt & 0x9Fu, sel);
    return { remap_sel(sel[1]), remap_sel(sel[2]), remap_sel(sel[3]), remap_sel(sel[0]) };
}

VkSamplerAddressMode gcm_wrap(uint32_t w)
{
    switch (w & 0xFu) {   // CELL_GCM_TEXTURE_WRAP, _MIRROR, _CLAMP_TO_EDGE ... (the backend's vk_gcm_wrap)
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

std::map<uint64_t, VkSampler> s_samplers;

// A sampler from cellGcmSetTextureFilter's filters and cellGcmSetTextureControl's
// levels of detail and cellGcmSetTextureAddress's wrap modes (the engine's
// eng_decode_sampler and the backend's vk_eng_sampler).
VkSampler sampler(const Sampler& s)
{
    const uint32_t minf = s.min_filter, magf = s.mag_filter;
    const int min_linear = minf == 2 || minf == 4 || minf == 6, mag_linear = magf == 2;
    const int mip_present = minf >= 3, mip_linear = minf == 5 || minf == 6;
    float min_lod = (float)s.min_lod / 256.0f;
    float max_lod = mip_present ? (float)s.max_lod / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    if (!mip_present) max_lod = min_lod;
    uint32_t lo, hi;
    memcpy(&lo, &min_lod, 4); memcpy(&hi, &max_lod, 4);
    const uint64_t key = (uint64_t)min_linear | (uint64_t)mag_linear << 1 | (uint64_t)mip_linear << 2 |
                         (uint64_t)mip_present << 3 | (uint64_t)(s.wrap_s & 0xF) << 4 | (uint64_t)(s.wrap_t & 0xF) << 8 |
                         (uint64_t)(s.wrap_r & 0xF) << 12 | (uint64_t)(lo >> 16) << 16 | (uint64_t)(hi >> 16) << 32;
    auto it = s_samplers.find(key);
    if (it != s_samplers.end()) return it->second;
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = gcm_wrap(s.wrap_s); ci.addressModeV = gcm_wrap(s.wrap_t); ci.addressModeW = gcm_wrap(s.wrap_r);
    ci.minLod = min_lod; ci.maxLod = max_lod;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler v = VK_NULL_HANDLE;
    F.vkCreateSampler(D.device, &ci, nullptr, &v);
    return s_samplers[key] = v;
}

// The translators' per-unit texture parameters, from the record's texture
// and sampler (what rsx_texture_gamma_mask, rsx_texture_expand_mask and
// rsx_fp_shadow_desc_from_texture compute, on the explicit fields).
uint8_t gamma_mask(const Texture& t, const Sampler& s)
{
    switch (t.format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    uint8_t gamma = s.gamma & 15u;
    if (!gamma) return 0;
    uint32_t crossbar = t.remap & 0xFFFFu;
    if (!crossbar) crossbar = 0xAAE4u;
    uint8_t sel[4]; rsx_texture_component_remap(crossbar, t.format, sel);
    static const uint8_t rgba_lane[4] = { 3, 0, 1, 2 };
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t source = (crossbar >> (2u * i)) & 3u;
        if (sel[i] < RSX_REMAP_ZERO && (s.filter_signed >> source & 1u)) gamma &= (uint8_t)~(1u << rgba_lane[i]);
    }
    return gamma;
}

uint8_t expand_mask(const Texture& t, const Sampler& s)
{
    switch (t.format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x98:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    if (!(s.unsigned_remap & 1u)) return 0;   // CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL
    uint32_t crossbar = t.remap & 0xFFFFu;
    if (!crossbar) crossbar = 0xAAE4u;
    uint8_t sel[4]; rsx_texture_component_remap(crossbar, t.format, sel);
    static const uint8_t rgba_lane[4] = { 3, 0, 1, 2 };
    uint8_t mask = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t source = (crossbar >> (2u * i)) & 3u;
        if (sel[i] < RSX_REMAP_ZERO && !(s.filter_signed >> source & 1u)) mask |= (uint8_t)(1u << rgba_lane[i]);
    }
    if (mask && ((t.format & 0x9Fu) == 0x94u || (t.format & 0x9Fu) == 0x95u)) mask |= 0x10u;
    return mask;
}

void shadow_desc(rsx_fp_shadow_desc* out, const Texture& t, const Sampler& s)
{
    memset(out, 0, sizeof *out);
    const uint32_t base = t.format & 0x9Fu, func = s.zfunc & 15u;
    if (!s.enable || (base != 0x90 && base != 0x91 && base != 0x92 && base != 0x93) || func == 0 || func == 7) return;
    const uint32_t minf = s.min_filter, magf = s.mag_filter;
    const int minlin = minf == 2 || minf == 4 || minf == 6;
    const int maglin = magf == 2;
    out->enabled = (base == 0x91 || base == 0x93 || func > 7 || t.dimension != 2 || t.cubemap || t.mipmap > 1 ||
                    minf < 1 || minf > 6 || magf < 1 || magf > 2 || minlin != maglin) ? 2 : 1;
    out->func = (uint8_t)func;
    out->linear = (uint8_t)minlin;
    out->unnormalized = (uint8_t)((t.format & 0x40u) != 0);
}

std::map<VkFormat, VkRenderPass> s_rps;

VkRenderPass render_pass(VkFormat f)
{
    auto it = s_rps.find(f);
    if (it != s_rps.end()) return it->second;
    const VkImageLayout l = s_full ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription a = {};
    a.format = f; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = l; a.finalLayout = l;
    VkAttachmentReference ar = { 0, l };
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

Img* target_image(uint32_t loc, uint32_t off, VkFormat fmt, uint32_t bpp, uint32_t w, uint32_t h)
{
    Img*& t = s_targets[akey(loc, off)];
    if (t && t->fmt == fmt && t->w >= w && t->h >= h) return t;
    // new, or other than this record needs: a new image (resource reuse); the
    // old one goes once nothing in flight can use it
    if (t) { s_retired.push_back(t); stats().n_retired++; }
    t = make_image(fmt, w, h, 1, bpp, true);
    return t;
}

// ---- programs and pipelines -----------------------------------------------------------------

struct Pipe { VkPipeline p; uint32_t nconst; };
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

// The fragment translator's control word, made from the program's
// CgBinaryFragmentProgram fields (registerCount, outputFromH0, depthReplace,
// pixelKill) and the bind's txp: the translator's parameter, as libgcm's
// cellGcmSetFragmentProgram makes it.
uint32_t fp_control(const FragmentProgram& f)
{
    return (uint32_t)(f.reg_count < 2 ? 2 : f.reg_count) << 24 | (uint32_t)f.txp << 15 | 0x400u |
           (f.output_from_h0 ? 0 : 0x40u) | (f.depth_replace ? 0xEu : 0) | (f.pixel_kill ? 0x80u : 0);
}

// The colour write mask, from cellGcmSetColorMask's (or asp's) CELL_GCM_COLOR_MASK bits.
VkColorComponentFlags write_mask(uint32_t m)
{
    VkColorComponentFlags wm = 0;
    if (m >> 16 & 0xFF) wm |= VK_COLOR_COMPONENT_R_BIT;
    if (m >> 8 & 0xFF) wm |= VK_COLOR_COMPONENT_G_BIT;
    if (m & 0xFF) wm |= VK_COLOR_COMPONENT_B_BIT;
    if (m >> 24 & 0xFF) wm |= VK_COLOR_COMPONENT_A_BIT;
    return wm;
}

// The pipeline for this record: its programs as the engine makes them, with
// the record's texture, sampler and surface parameters, and its render state.
// 0 when it cannot be made.
const Pipe* pipeline(const Rec* r, const rsx_vertex_layout_plan& layout, VkFormat rt, rsx_fp_constant_block* consts)
{
    const State& st = r->st;
    const uint8_t* vp = r->vp->bytes.data();
    const uint32_t vp_bytes = (uint32_t)r->vp->bytes.size();
    const uint8_t* fp = r->fp->bytes.data();
    const uint32_t fp_size = (uint32_t)r->fp->bytes.size();
    const uint32_t fp_ctrl = fp_control(st.fp);
    const uint32_t srgb = rsx_fp_srgb_output_enabled(st.fp.gamma, fp_ctrl, st.surface.color_format & 0x1Fu);
    const uint32_t sampled = rsx_fp_texture_units(fp, fp_size);
    uint32_t cube = 0, vol = 0, shadow_mask = 0, unnorm_mask = 0;
    rsx_fp_shadow_desc shadow[16] = {};
    uint8_t gamma[16] = {}, expand[16] = {};
    uint32_t unnorm[16][3] = {};
    for (uint32_t u = 0; u < 16; u++) {
        const Texture& t = st.tex[u];
        const Sampler& s = st.samp[u];
        if (!s.enable) continue;
        if (t.cubemap && t.width && t.width == t.height) cube |= 1u << u;
        if (!t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height) vol |= 1u << u;
        gamma[u] = gamma_mask(t, s);
        expand[u] = expand_mask(t, s);
        if (sampled >> u & 1) {
            shadow_desc(&shadow[u], t, s);
            if (shadow[u].enabled) shadow_mask |= 1u << u;
        }
        if ((t.format & RSX_TEX_FMT_UNNORM) && t.width && t.height) {
            unnorm_mask |= 1u << u;
            unnorm[u][0] = t.width; unnorm[u][1] = t.height; unnorm[u][2] = t.depth;
        }
    }
    const uint32_t gamma_units = rsx_fp_texture_gamma_units(fp, fp_size, gamma);
    for (uint32_t u = 0; u < 16; u++) if (!(gamma_units >> u & 1)) gamma[u] = 0;
    const uint32_t expand_units = rsx_fp_texture_expand_units(fp, fp_size, expand);
    for (uint32_t u = 0; u < 16; u++) if (!(expand_units >> u & 1)) expand[u] = 0;
    unnorm_mask &= sampled & ~shadow_mask;
    for (uint32_t u = 0; u < 16; u++) if (!(unnorm_mask >> u & 1)) unnorm[u][0] = unnorm[u][1] = unnorm[u][2] = 0;
    consts->count = 0;
    if (rsx_fp_collect_constants(fp, fp_size, consts) < 0) return nullptr;
    // vertex textures, branch bits, user clip planes: never set by PPFX (the
    // explicit state has none); a single-sample surface: no position grid
    const uint32_t vtex = 0, bb = 0, ucp = 0;
    const uint32_t wpos[2] = { 1u, 1u };
    const Raster& ra = st.raster;
    // identity: everything the translation and the pipeline are made from
    uint64_t k = fnv(vp, vp_bytes);
    k = rsx_fp_structural_hash(fp, fp_size, k);
    const uint32_t ints[] = { fp_ctrl & 0x4Eu, srgb, cube, vol, shadow_mask, unnorm_mask, gamma_units, expand_units,
                              layout.mask, layout.stride, (uint32_t)rt, ra.color_mask, ra.front_face };
    k = fnv(ints, sizeof ints, k);
    k = fnv(gamma, sizeof gamma, k); k = fnv(expand, sizeof expand, k);
    k = fnv(unnorm, sizeof unnorm, k); k = fnv(shadow, sizeof shadow, k);
    auto it = s_pipes.find(k);
    if (it != s_pipes.end()) return it->second.p ? &it->second : nullptr;
    Pipe& P = s_pipes[k];
    P.p = VK_NULL_HANDLE;
    ProgParams pp = { vp, vp_bytes, fp, fp_size, fp_ctrl, cube, vol, vtex, layout.mask, bb, ucp, srgb,
                      0u, 0u, unnorm_mask, shadow_mask, gamma_units, expand_units,
                      { wpos[0], wpos[1] }, 0, shadow, gamma, expand, (const uint32_t (*)[3])unnorm };
    std::string vs, ps;
    uint32_t nconst = 0;
    if (!programs_text(pp, vs, ps, &nconst) || nconst != consts->count) {
        report_add("render: a record's programs did not translate");
        return nullptr;
    }
    VkShaderModule vsm = spirv_module(vs, RSX_SHADER_STAGE_VERTEX), fsm = spirv_module(ps, RSX_SHADER_STAGE_FRAGMENT);
    if (!vsm || !fsm) return nullptr;
    VkPipelineShaderStageCreateInfo stg[2] = {
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
    ras.cullMode = VK_CULL_MODE_NONE;   // culling off (checked by the caller)
    ras.frontFace = ra.front_face == 0x0901u ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    ras.depthBiasEnable = VK_TRUE; ras.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE; cba.colorWriteMask = write_mask(ra.color_mask);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    const VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 5; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gci.stageCount = 2; gci.pStages = stg; gci.pVertexInputState = &vi; gci.pInputAssemblyState = &ia;
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

// The toolkit's vertex fetch plan, from the record's vertex inputs (what
// rsx_vertex_fetch_plan_init fills from a register file).
void fetch_plan(rsx_vertex_fetch_plan* plan, const Rec* r, const rsx_vertex_layout_plan& layout)
{
    memset(plan, 0, sizeof *plan);
    plan->layout = layout;
    plan->base_offset = r->st.vdata.base_offset;
    plan->divider_mask = r->st.vdata.divider_op;
    plan->guest_ptr = snap_ptr;
    plan->guest_user = (void*)r;
    for (uint32_t a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        const VertexInput& in = r->st.vin[a];
        rsx_vertex_fetch_attr& f = plan->attr[a];
        f.desc.type = in.type & 7u; f.desc.size = in.size; f.desc.stride = in.stride;
        f.desc.frequency = in.frequency; f.desc.offset = in.offset & 0x7FFFFFFFu; f.desc.location = in.location;
        f.elem_size = rsx_vertex_attrib_size(f.desc.type, f.desc.size);
        f.stride = f.desc.stride ? f.desc.stride : f.elem_size;
        memcpy(f.default_value, r->st.vdata.current[a], 16);
        if (a == 3 && f.default_value[0] == 0.0f && f.default_value[1] == 0.0f && f.default_value[2] == 0.0f &&
            f.default_value[3] == 1.0f)
            f.default_value[0] = f.default_value[1] = f.default_value[2] = 1.0f;
    }
}

// A static texture from the bytes the recorder took (the engine's
// eng_texture_upload, one face), by content.
Img* static_texture(Batch& b, const Rec* r, uint32_t unit, const Texture& t)
{
    const VtxRange* snap = nullptr;
    for (uint32_t k = 0; k < r->ntex; k++) if (r->tex[k].location == unit) snap = &r->tex[k];
    if (!snap || t.cubemap || t.dimension == 3) return nullptr;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const uint32_t nlv = rsx_texture_mip_chain(t.format, t.width, t.height, t.mipmap, t.pitch, lv);
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
    access(b.cb, o, kCopyDst);
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
        F.vkCmdCopyBufferToImage(b.cb, b.up.b, o->img, layout_of(o), 1, &c);
    }
    mark(b, kSectCopy);
    o->written = 1;
    stats().n_static_textures++;
    if (s_static.size() > 256) { for (auto& kv : s_static) s_retired.push_back(kv.second); s_static.clear(); }
    return s_static[h] = o;
}

VkImageView view_of(Img* o, uint32_t crossbar, uint32_t fmt)
{
    const uint64_t k = (uint64_t)crossbar << 32 | fmt;
    auto it = o->views.find(k);
    if (it != o->views.end()) return it->second;
    return o->views[k] = make_view(o->img, o->fmt, o->levels, remap(crossbar, fmt));
}

// The handoffs of a record: the engine's copies, from their slots into the
// native images of their addresses.
void apply_handoffs(Batch& b, const Rec* r)
{
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
        }
        access(b.cb, src, kCopySrc);
        access(b.cb, dst, kCopyDst);
        VkImageCopy c = {};
        c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.extent = { src->w, src->h, 1 };
        F.vkCmdCopyImage(b.cb, src->img, layout_of(src), dst->img, layout_of(dst), 1, &c);
        mark(b, kSectCopy);
        stats().n_handoff_copies++;
        dst->written = 1;
        b.done.push_back([s] { std::lock_guard<std::mutex> g(s_slot_lock); s_simg[s].busy = false; });
    }
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

std::mutex s_inh_lock;
std::set<uint32_t> s_inherited;   // frames the cross-check flagged (bounded: the latest 4096)

bool frame_is_inherited(uint32_t frame)
{
    std::lock_guard<std::mutex> g(s_inh_lock);
    return s_inherited.count(frame) != 0;
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
                int final_; int change; VkFormat f; };
std::mutex s_cmp_lock;
std::condition_variable s_cmp_cv;
std::vector<CmpJob> s_cmp_q;
std::atomic<int> s_cmp_running{0};
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

void release_ref(int ref)
{
    std::lock_guard<std::mutex> g(s_slot_lock);
    if (ref >= 0 && (size_t)ref < s_sbuf.size()) s_sbuf[ref].busy = false;
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
    if (!same) st.cmp_skip_extent++;
    else {
        if (j.change) st.cmp_change_done++;
        frame_result(j.frame, equal, j.final_);
        if (equal) { st.cmp_equal++; if (j.final_) st.cmp_finals_equal++; }
        else {
            st.cmp_differ++;
            if (frame_is_inherited(j.frame)) st.cmp_differ_inherited++;
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
    release_ref(j.ref);
    {
        std::lock_guard<std::mutex> g(s_cmp_lock);
        VkDeviceSize pooled = 0;
        for (auto& kv : s_rb_pool) pooled += kv.first;
        if (pooled + j.mine.n <= kReadbackPoolBytes) s_rb_pool.emplace(j.mine.n, j.mine);
        else { F.vkDestroyBuffer(D.device, j.mine.b, nullptr); F.vkFreeMemory(D.device, j.mine.m, nullptr); }
    }
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
            s_cmp_running = 1;
        }
        s_cmp_cv.notify_all();
        cmp_run(j);
        s_cmp_running = 0;
        s_cmp_cv.notify_all();
    }
}

// A record whose output the walker copied: its native output read back
// after it, compared once both copies are complete. Every record the
// comparison asked for ends counted (wanted = equal + differ + skipped +
// unfinished).
void compare_after(Batch& b, const Rec* r, Img* t, uint32_t w, uint32_t h)
{
    const int ref = r->walk.ref;
    if (ref < 0) return;
    CmpJob j;
    j.bytes = (VkDeviceSize)w * h * t->bpp;
    j.mine = rb_take(j.bytes);
    if (!j.mine.b) { stats().cmp_skip_no_readback++; release_ref(ref); return; }
    access(b.cb, t, kCopySrc);
    VkBufferImageCopy c = {};
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { w, h, 1 };
    F.vkCmdCopyImageToBuffer(b.cb, t->img, layout_of(t), j.mine.b, 1, &c);
    mark(b, kSectReadback);
    // what the comparison needs after the record is gone
    j.ref = ref; j.w = w; j.h = h; j.bpp = t->bpp; j.ref_w = r->walk.ref_w; j.ref_h = r->walk.ref_h;
    j.frame = r->frame; j.off = r->need[0].offset; j.seq = r->seq; j.final_ = r->need[0].format == 0x08; j.f = t->fmt;
    j.change = r->walk.cmp_why == kCmpChange;
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
    // a comparison it was copied for cannot run
    if (r->walk.ref >= 0) { stats().cmp_skip_not_drawn++; release_ref(r->walk.ref); }
    static int said = 0;
    if (said++ < 20) {
        char line[200];
        snprintf(line, sizeof line, "render: record %llu (frame %u) not rendered: %s", (unsigned long long)r->seq,
                 r->frame, why);
        report_add(line);
    }
}

// The engine's eng_scissor_rect, on the record's scissor.
void scissor_rect(const Scissor& s, uint32_t w, uint32_t h, uint32_t out[4])
{
    uint32_t sx = 0, sy = 0, sw = w, sh = h;
    const uint32_t gx = s.x, gw = s.w, gy = s.y, gh = s.h;
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
    const State& st = r->st;
    const Surface& sf = st.surface;
    if (sf.color_target != 1 || sf.antialias) { unsupported(r, "MRT or antialiased target"); return; }
    const Raster& ra = st.raster;
    if (ra.blend || ra.depth_test || ra.stencil_test || ra.alpha_test || ra.cull) {
        unsupported(r, "render state beyond PPFX's (blend, depth, stencil, alpha test, cull)");
        return;
    }
    uint32_t bpp = 0;
    const VkFormat rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
    const uint32_t W = sf.width, H = sf.height;
    if (!W || !H) { unsupported(r, "no clip"); return; }
    apply_handoffs(b, r);
    Img* t = target_image(sf.color_location[0] ? 1u : 0u, sf.color_offset[0], rt, bpp, W, H);
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
    fetch_plan(&plan, r, layout);
    rsx_vertex_fetch_plan_prepare(&plan, refs.data(), n);
    std::vector<uint8_t> verts((size_t)n * layout.stride);
    if (!rsx_vertex_fetch_many(&plan, refs.data(), n, verts.data())) { unsupported(r, "vertex fetch"); return; }
    const uint32_t n_draw = indexed ? rsx_draw_engine_topology_index_count(r->prim, n, nullptr, 0) : n - n % 3;
    std::vector<uint32_t> idx(n_draw ? n_draw : 1);
    if (indexed) rsx_draw_engine_write_topology_indices(r->prim, n, nullptr, 0, nullptr, idx.data());
    else for (uint32_t i = 0; i < n_draw; i++) idx[i] = i;
    // the pipeline
    rsx_fp_constant_block fc;
    const Pipe* P = pipeline(r, layout, rt, &fc);
    if (!P) { unsupported(r, "pipeline"); return; }
    // constants: the vertex program's and the viewport epilogue; the fragment
    // program's buffered constants and the alpha/window slot
    const Viewport& vp = st.viewport;
    const float Wf = (float)W, Hf = (float)H;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.offset[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wf * 0.5f);
        xf[1] = -(vp.scale[1] / (Hf * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.offset[0] - Wf * 0.5f) / (Wf * 0.5f);
        xf[5] = -((vp.offset[1] - Hf * 0.5f) / (Hf * 0.5f));
        xf[6] = vp.offset[2];
    }
    uint8_t vcb[512 * 16 + 32];
    memcpy(vcb, r->vconst, 512 * 16);
    memcpy(vcb + 512 * 16, xf, sizeof xf);
    const uint32_t nslots = fc.count ? fc.count : 1u;
    std::vector<uint8_t> pcb((nslots + 1) * 16, 0);
    if (fc.count) memcpy(pcb.data(), fc.values, fc.count * 16u);
    {
        // alpha test off (checked above): no reference; the window: its
        // height and origin as cellGcmSetSurface sets them (bottom left,
        // pixel centres at half)
        float* alpha = (float*)(pcb.data() + nslots * 16);
        alpha[0] = 0.0f;
        alpha[1] = (float)(sf.height & 0xFFFu);
        alpha[2] = 0.0f;
        alpha[3] = 0.0f;
    }
    // textures
    VkDescriptorImageInfo ii[16], si[16];
    const uint32_t sampled = rsx_fp_texture_units(r->fp->bytes.data(), (uint32_t)r->fp->bytes.size());
    Img* srcs[16] = {};
    for (uint32_t u = 0; u < 16; u++) {
        if (!(sampled >> u & 1) || !st.samp[u].enable) continue;
        const Texture& tx = st.tex[u];
        Img* src = nullptr;
        auto it = s_targets.find(akey(tx.location ? 1u : 0u, tx.offset));
        if (it != s_targets.end() && it->second->written) src = it->second;
        if (src && src == t) {
            // its own target: a copy taken just before the draw
            Img*& snap = s_snap[std::make_tuple(t->fmt, t->w, t->h)];
            if (!snap) snap = make_image(t->fmt, t->w, t->h, 1, t->bpp, false);
            if (!snap) { unsupported(r, "snapshot image"); return; }
            access(b.cb, t, kCopySrc);
            access(b.cb, snap, kCopyDst);
            VkImageCopy c = {};
            c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.extent = { t->w, t->h, 1 };
            F.vkCmdCopyImage(b.cb, t->img, layout_of(t), snap->img, layout_of(snap), 1, &c);
            mark(b, kSectCopy);
            stats().n_snapshot_copies++;
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
        srcs[u] = src;
    }
    // every sampled image in the layout it is read in, after its writes
    for (uint32_t u = 0; u < 16; u++) if (srcs[u]) access(b.cb, srcs[u], kSampled);
    for (uint32_t u = 0; u < 16; u++) {
        ii[u] = { VK_NULL_HANDLE, s_dummy->view, sampled_layout() };
        si[u] = { s_dummy_smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        if (!srcs[u]) continue;
        ii[u].imageView = view_of(srcs[u], st.tex[u].remap & 0xFFFFu, st.tex[u].format);
        si[u].sampler = sampler(st.samp[u]);
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
    // negative control): before one draw in 5000, the target is copied twice
    // into a scratch image with no barrier between the copies -- a write
    // after write the validation layer must report
    static int hazard = -1;
    if (hazard < 0) { const char* e = getenv("DOTHEROES_PPFX_HAZARD_TEST"); hazard = e && *e == '1'; }
    if (hazard && stats().n_draws % 5000 == 4999) {
        static Img* scratch = nullptr;
        if (!scratch || scratch->fmt != t->fmt || scratch->w < t->w || scratch->h < t->h)
            scratch = make_image(t->fmt, t->w, t->h, 1, t->bpp, false);
        if (scratch) {
            access(b.cb, t, kCopySrc);
            access(b.cb, scratch, kCopyDst);
            VkImageCopy c = {};
            c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.extent = { t->w, t->h, 1 };
            F.vkCmdCopyImage(b.cb, t->img, layout_of(t), scratch->img, layout_of(scratch), 1, &c);
            F.vkCmdCopyImage(b.cb, t->img, layout_of(t), scratch->img, layout_of(scratch), 1, &c);
            report_add("render: hazard test: two copies into one image without a barrier");
        }
    }
    access(b.cb, t, kAttach);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = render_pass(rt); rbi.framebuffer = framebuffer(t);
    rbi.renderArea = { { 0, 0 }, { t->w, t->h } };
    F.vkCmdBeginRenderPass(b.cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    F.vkCmdBindPipeline(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, P->p);
    VkViewport v = { 0.0f, 0.0f, Wf, Hf, 0.0f, 1.0f };
    F.vkCmdSetViewport(b.cb, 0, 1, &v);
    uint32_t sc[4];
    scissor_rect(st.scissor, t->w, t->h, sc);
    VkRect2D s = { { (int32_t)sc[0], (int32_t)sc[1] }, { sc[2], sc[3] } };
    F.vkCmdSetScissor(b.cb, 0, 1, &s);
    F.vkCmdSetStencilReference(b.cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);   // stencil off (checked above)
    F.vkCmdSetDepthBias(b.cb, 0.0f, 0.0f, 0.0f);
    const float bc[4] = { 0, 0, 0, 0 };   // blend is off (checked above): the constant is never read
    F.vkCmdSetBlendConstants(b.cb, bc);
    F.vkCmdBindDescriptorSets(b.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl, 0, 1, &set, 0, nullptr);
    F.vkCmdBindVertexBuffers(b.cb, 0, 1, &b.up.b, &o_v);
    F.vkCmdBindIndexBuffer(b.cb, b.up.b, o_i, VK_INDEX_TYPE_UINT32);
    F.vkCmdDrawIndexed(b.cb, n_draw, 1, 0, 0, 0);
    F.vkCmdEndRenderPass(b.cb);
    mark(b, kSectDraw);
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
    const Surface& sf = r->st.surface;
    uint32_t bpp = 0;
    const VkFormat rt = (VkFormat)surface_vkformat(sf.color_format, &bpp);
    if (!sf.width || !sf.height || sf.color_target != 1) { unsupported(r, "clear target"); return; }
    apply_handoffs(b, r);
    Img* t = target_image(sf.color_location[0] ? 1u : 0u, sf.color_offset[0], rt, bpp, sf.width, sf.height);
    if (!t) { unsupported(r, "clear target image"); return; }
    const uint32_t c = r->st.clear_color;   // cellGcmSetClearColor's ARGB
    VkClearColorValue cv;
    cv.float32[0] = (float)(c >> 16 & 0xFF) / 255.0f; cv.float32[1] = (float)(c >> 8 & 0xFF) / 255.0f;
    cv.float32[2] = (float)(c & 0xFF) / 255.0f; cv.float32[3] = (float)(c >> 24 & 0xFF) / 255.0f;
    // the engine's eng_clear_rect
    const uint32_t cw = sf.width < t->w ? sf.width : t->w, ch = sf.height < t->h ? sf.height : t->h;
    uint32_t rc[4];
    scissor_rect(r->st.scissor, cw, ch, rc);
    if (!rc[2] || !rc[3]) return;
    if (rc[0] == 0 && rc[1] == 0 && rc[2] >= cw && rc[3] >= ch) {
        access(b.cb, t, kCopyDst);
        VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        F.vkCmdClearColorImage(b.cb, t->img, layout_of(t), &cv, 1, &rg);
    } else {
        access(b.cb, t, kAttach);
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
    mark(b, kSectDraw);
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
        const char* e = getenv("DOTHEROES_PPFX_BARRIERS");
        s_full = e && !strcmp(e, "full");
        char line[240];
        snprintf(line, sizeof line, "render: on %s queue of family %u (%s); barriers %s", D.shared_queue ? "the backend's" : "a second",
                 D.queue_family, props.deviceName, s_full ? "full (every image in GENERAL)" : "per image (tracked hazards)");
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
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kQueries;
        F.vkCreateQueryPool(D.device, &qi, nullptr, &b.qp);
        static int dev_up = -1;
        if (dev_up < 0) { const char* e = getenv("DOTHEROES_PPFX_UPLOAD"); dev_up = !(e && !strcmp(e, "host")); }
        if (!make_buffer(b.up, kUpload, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         false, dev_up != 0))
            return false;
    }
    // the dummy every unbound unit samples (as the engine's: 1x1 magenta)
    {
        Batch& b = batch(0);
        s_dummy = make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 4, false);
        access(b.cb, s_dummy, kCopyDst);
        const uint8_t magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
        const VkDeviceSize at = upload(b, magenta, 4, 16);
        VkBufferImageCopy c = {};
        c.bufferOffset = at; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; c.imageExtent = { 1, 1, 1 };
        F.vkCmdCopyBufferToImage(b.cb, b.up.b, s_dummy->img, layout_of(s_dummy), 1, &c);
        access(b.cb, s_dummy, kSampled);
        submit();
        Sampler ds = {};
        ds.min_filter = 2; ds.mag_filter = 2; ds.wrap_s = ds.wrap_t = ds.wrap_r = 3;
        s_dummy_smp = sampler(ds);
    }
    const char* d = getenv("DOTHEROES_PPFX_DUMP");
    snprintf(s_dump, sizeof s_dump, "%s", d && *d ? d : "");
#ifdef _WIN32
    if (s_dump[0]) CreateDirectoryA(s_dump, NULL);
#endif
    s_ready = 1;
    return true;
}

std::atomic<int> s_draining{0};
std::atomic<int> s_idle{0};   // the thread has submitted everything walked and waits

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
            if (s_draining) { for (Batch& b : s_b) retire(b); s_idle = 1; }
            std::unique_lock<std::mutex> lk(q.m);
            q.cv.wait_for(lk, std::chrono::milliseconds(2),
                          [&] { return next < q.walked.load(std::memory_order_acquire); });
            stats().wait_walker_us += (now_ns() - t0) / 1000;
            continue;
        }
        s_idle = 0;
        Rec* r = q.at(next);
        if (r->frame != frame && s_b[s_cur].open) submit();   // a frame's work in a batch of its own
        if (r->frame != frame) {
            stats().n_frames++;
            frames_close(r->frame);
            // DOTHEROES_PPFX_REALLOC_EVERY=<n> (a test of image lifetime):
            // every n frames every native image is retired -- destroyed once
            // the batches in flight are done -- and made again at its next use
            static int every = -1;
            if (every < 0) { const char* e = getenv("DOTHEROES_PPFX_REALLOC_EVERY"); every = e && *e ? atoi(e) : 0; }
            if (every > 0 && r->frame % (uint32_t)every == 0 && !s_targets.empty()) {
                if (s_b[s_cur].open) submit();
                for (auto& kv : s_targets) { s_retired.push_back(kv.second); stats().n_retired++; }
                s_targets.clear();
                for (auto& kv : s_snap) if (kv.second) { s_retired.push_back(kv.second); stats().n_retired++; }
                s_snap.clear();
            }
        }
        frame = r->frame;
        // DOTHEROES_PPFX_DELAY_MS=<n> (a test): a slow consumer, n ms a record
        static int delay = -1;
        if (delay < 0) { const char* e = getenv("DOTHEROES_PPFX_DELAY_MS"); delay = e && *e ? atoi(e) : 0; }
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        const uint64_t t0 = now_ns();
        if (r->kind == kDraw) draw(r);
        else if (r->kind == kClear) clear(r);
        stats().cpu_record_ns += now_ns() - t0;
        if (s_b[s_cur].open && (s_b[s_cur].used > kUpload / 2 || s_b[s_cur].sets > kSets / 2 ||
                                s_b[s_cur].nq > kQueries - 64)) submit();
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

void frame_flag_inherited(uint32_t frame)
{
    std::lock_guard<std::mutex> g(s_inh_lock);
    s_inherited.insert(frame);
    while (s_inherited.size() > 4096) s_inherited.erase(s_inherited.begin());
}

// At exit: the native thread submits everything walked and the comparisons
// queued run, within the time given; what is still in flight then is
// counted as unfinished (wanted = equal + differ + skipped + unfinished).
void render_drain(uint32_t timeout_ms)
{
    if (!render_ready()) return;
    const uint64_t t0 = now_ns(), limit = (uint64_t)timeout_ms * 1000000ull;
    s_draining = 1;
    Queue& q = queue();
    while (now_ns() - t0 < limit) {
        const bool walked_done = q.done.load() >= q.walked.load() && s_idle.load();
        bool cmp_done;
        { std::lock_guard<std::mutex> g(s_cmp_lock); cmp_done = s_cmp_q.empty() && !s_cmp_running; }
        if (walked_done && cmp_done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Stats& s = stats();
    const uint64_t ended = s.cmp_equal + s.cmp_differ + s.cmp_skip_no_ref + s.cmp_skip_no_readback +
                           s.cmp_skip_extent + s.cmp_skip_not_drawn;
    s.cmp_unfinished = s.cmp_wanted > ended ? s.cmp_wanted - ended : 0;
}

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
    // frames not closed yet (their comparisons in flight, or the run's last)
    for (auto& kv : s_frames) {
        const FrameAcc& f = kv.second;
        if (!f.eq && !f.diff && !f.pending) continue;
        snprintf(line, sizeof line, "open frame %u: %u draws, compared equal %u differ %u, pending %d",
                 kv.first, f.draws, f.eq, f.diff, f.pending);
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
    // created here (the walker's thread): vkCreateImage is free-threaded; the
    // engine's copy takes it to GENERAL with its own barrier, and it stays
    // there (the native side reads it in GENERAL, ordered by the semaphore)
    Img* o = make_image((VkFormat)vkformat, w, h, 1, bpp, false);
    if (!o) return -1;
    o->external = true;
    o->layout = VK_IMAGE_LAYOUT_GENERAL;
    s_simg.push_back({ o, true });
    *image = (uint64_t)o->img;
    return (int)s_simg.size() - 1;
}

int slot_buffer(uint64_t bytes, uint64_t* buffer, int prio)
{
    if (!render_ready()) return -1;
    std::lock_guard<std::mutex> g(s_slot_lock);
    size_t busy = 0;
    for (const SlotBuf& b : s_sbuf) busy += b.busy;
    // the reserves: a sampled comparison leaves room for the change frames'
    // and the finals', a change frame's for the finals'
    const size_t keep = prio >= 2 ? 0 : prio == 1 ? kFinalReserve : kFinalReserve + kChangeReserve;
    if (busy + keep >= kMaxSlotBuffers) return -1;
    int best = -1;
    for (size_t i = 0; i < s_sbuf.size(); i++)
        if (!s_sbuf[i].busy && s_sbuf[i].b.n >= bytes && (best < 0 || s_sbuf[i].b.n < s_sbuf[best].b.n)) best = (int)i;
    if (best >= 0) { s_sbuf[best].busy = true; *buffer = (uint64_t)s_sbuf[best].b.b; return best; }
    if (s_sbuf.size() >= kMaxSlotBuffers) return -1;
    // bounded in bytes too, with the same reserves in bytes
    VkDeviceSize total = 0;
    for (const SlotBuf& b : s_sbuf) total += b.b.n;
    const VkDeviceSize cap = prio >= 2 ? kSlotBufferBytes : prio == 1 ? kSlotBufferBytes * 7 / 8 : kSlotBufferBytes * 5 / 8;
    if (total + bytes > cap) return -1;
    SlotBuf s;
    if (!make_buffer(s.b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return -1;
    s.busy = true; s.prio = prio;
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

// Which records' outputs are compared: every frame's final composite, every
// draw of every DOTHEROES_PPFX_COMPARE_EVERY-th frame (default 30), and (the
// bridge decides) every draw after a structure change.
int compare_wanted(const Rec* r)
{
    static int every = -1, off = -1;
    if (every < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE_EVERY"); every = e && *e ? atoi(e) : 30; }
    // DOTHEROES_PPFX_COMPARE=0: no comparison at all (the native renderer's own cost)
    if (off < 0) { const char* e = getenv("DOTHEROES_PPFX_COMPARE"); off = e && *e == '0'; }
    if (off || r->kind != kDraw) return kCmpNone;
    if (r->need[0].format == 0x08) return kCmpFinal;   // the final composite: the A8R8G8B8 display buffer
    return every > 0 && r->frame % (uint32_t)every == 0 ? kCmpEvery : kCmpNone;
}

void render_start()
{
    std::thread(run).detach();
    std::thread(cmp_thread).detach();
}

}  // namespace ppfx
